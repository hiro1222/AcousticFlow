// AcousticWorld.cs ── 新コア（Flow）のホスト（段 4）。
//
// ■ 役割
//   場面の箱（BoxCollider）と材質を音響エンジンの「世界」に渡し、毎フレーム リスナーと音源の位置を置いて
//   AF_WorldUpdate を 1 回呼び、配分を各 WorldVoice の Voice に写す。それだけ。
//   音の計算は全部 DLL の中（C# は「音の面の操作」だけ）。
//
// ■ 中の仕組み
//   OnEnable: 世界を作り、BoxCollider を拾って箱を足し（材質は AcousticSurface があればそれ、無ければ既定）、
//             AudioListener の Transform をリスナーに、AudioListener に TailBusRenderer（後期の器と方向バスの持ち主）を付ける。
//   Update:   リスナー → 音源 → AF_WorldUpdate → 各 Voice へ ApplyVoice。
//             FDN の器: 世界が「古い」と言ったら（部屋グラフの作り直し）、TailBusRenderer.ReplaceFdnMix で新しい器を作り、
//             AF_WorldBindFdn と各 Voice の AF_VoiceSetFdnMix を向け直す。
//   Register/Unregister: WorldVoice が自分を登録する（AF_WorldAddEmitter）。
//
// ■ 繋がり
//   受ける: 場面（BoxCollider、AcousticSurface、AudioListener、WorldVoice）。
//   渡す:   DLL（NativeWorld）、WorldVoice（Voice の設定）、TailBusRenderer（器）。
//
// ■ 退けた書き方
//   ・AcousticFlowSceneDemo（旧ホスト 3,000 行）を流用: 旧 API と旧の模型の切り替えが焼き込まれている。新コアは 200 行で足りる。
//   ・Voice ごとに FDN を持つ: 器は 1 つ（設計文書「FDN は部屋ごと。音源数に比例しない」）。
//
// ■ 壊れる所
//   ・AudioListener が無いと世界は作れても音が出ない（リスナー位置が原点のまま）。警告を出す。
//   ・器を差し替えたのに Voice の AF_VoiceSetFdnMix を向け直さないと、Voice は壊された器へ送り続ける（1 秒の猶予の後に落ちる）。
//   ・段 4 では動く箱（扉）を毎フレーム追わない（段 5）。dynamic 印の箱は部屋グラフから外れるだけ。
using System;
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    [DefaultExecutionOrder(-50)]
    public sealed class AcousticWorld : MonoBehaviour
    {
        public static AcousticWorld Instance { get; private set; }

        [Header("場面")]
        [Tooltip("ON: 場面の BoxCollider を全部、音がぶつかる箱として世界に渡す（AcousticSurface があれば材質もそこから）")]
        public bool autoCollectBoxColliders = true;
        [Tooltip("AcousticSurface の無い箱の材質")]
        public AcousticMaterialPreset defaultMaterial = AcousticMaterialPreset.Default;
        [Tooltip("リスナー。空なら AudioListener を探す")]
        public Transform listener;

        [Header("レイ（段 8 で予算に置き換わる）")]
        public int raysPerEmitter = 256;
        public int maxBounces = 40;
        public float headCircumferenceCm = 57f;

        [Header("世界の重み（五成分に 1 つずつ。1 = 物理どおり）")]
        [Range(0f, 4f)] public float weightDirect = 1f;
        [Range(0f, 4f)] public float weightEarly = 1f;
        [Range(0f, 4f)] public float weightLate = 1f;
        [Range(0f, 4f)] public float weightDiffract = 1f;
        [Range(0f, 4f)] public float weightTransmit = 1f;

        [Header("速度の表（秒。0 = 毎フレーム）")]
        public float levelSec = 0f;
        public float colourSec = 0.04f;
        public float statSec = 0.05f;
        public float directionSec = 0.06f;

        public IntPtr Handle => _world;
        public int RoomCount => _world != IntPtr.Zero ? NativeWorld.AF_WorldRoomCount(_world) : 0;
        public float UpdateMs { get; private set; }
        public int VoiceCount => _voices.Count;
        public IReadOnlyList<WorldVoice> Voices => _voices;
        public TailBusRenderer TailHost => _tail;

        private IntPtr _world;
        private readonly List<WorldVoice> _voices = new List<WorldVoice>();
        private TailBusRenderer _tail;
        private int _sampleRate = 48000;
        private int _maxFrames = 1024;
        private readonly float[] _w5 = new float[5];

        private void OnEnable()
        {
            Instance = this;
            _sampleRate = AudioSettings.outputSampleRate;
            AudioSettings.GetDSPBufferSize(out int bufLen, out _);
            _maxFrames = Mathf.Max(256, bufLen);

            _world = NativeWorld.AF_WorldCreate();
            if (_world == IntPtr.Zero) { Debug.LogError("[AcousticWorld] 世界を作れませんでした（DLL）"); enabled = false; return; }

            if (listener == null)
            {
                var al = FindFirstObjectByType<AudioListener>();
                if (al != null) listener = al.transform;
                else Debug.LogWarning("[AcousticWorld] AudioListener が見つかりません。リスナーは原点のままです");
            }
            CollectBoxes();
            NativeWorld.AF_WorldSetRays(_world, raysPerEmitter, maxBounces);
            NativeWorld.AF_WorldSetHeadCm(_world, headCircumferenceCm);
            NativeWorld.AF_WorldBuild(_world);

            // 後期の器と方向バスは AudioListener に付く TailBusRenderer が持つ（OnAudioFilterRead で 1 回回す）。
            if (listener != null)
            {
                _tail = listener.GetComponent<TailBusRenderer>();
                if (_tail == null) _tail = listener.gameObject.AddComponent<TailBusRenderer>();
                _tail.enableSharedTail = false;        // 畳み込みの尾は使わない
                RebindFdn();
            }
            foreach (var v in FindObjectsByType<WorldVoice>(FindObjectsSortMode.None)) Register(v);
            Debug.Log($"[AcousticWorld] 世界: 箱 {_boxCount} / 部屋 {RoomCount} / 音源 {_voices.Count}（レイ {raysPerEmitter} 本）");
        }

        private void OnDisable()
        {
            foreach (var v in _voices) v.Detach();
            _voices.Clear();
            if (_world != IntPtr.Zero) { NativeWorld.AF_WorldDestroy(_world); _world = IntPtr.Zero; }
            if (Instance == this) Instance = null;
        }

        private int _boxCount;
        private void CollectBoxes()
        {
            _boxCount = 0;
            if (!autoCollectBoxColliders) return;
            var cache = new Dictionary<AcousticMaterialPreset, int>();
            foreach (var col in FindObjectsByType<BoxCollider>(FindObjectsSortMode.None))
            {
                if (col == null || !col.enabled) continue;
                if (listener != null && col.transform.IsChildOf(listener)) continue;
                if (col.GetComponentInParent<WorldVoice>() != null) continue;      // 音源の見た目の箱は壁にしない
                int mat;
                var surf = col.GetComponent<AcousticSurface>();
                if (surf != null && surf.mode != AcousticSurfaceMode.Preset)
                {
                    var m = surf.Resolve();
                    mat = NativeWorld.AF_WorldAddMaterial(_world, m.transmission, m.absorption, m.scattering);
                }
                else
                {
                    var preset = surf != null ? surf.material : defaultMaterial;
                    if (!cache.TryGetValue(preset, out mat)) { mat = NativeWorld.AF_WorldAddMaterialPreset(_world, (int)preset); cache[preset] = mat; }
                }
                var t = col.transform;
                Vector3 s = t.lossyScale;
                var center = t.TransformPoint(col.center);
                var half = new Vector3(Mathf.Abs(col.size.x * s.x), Mathf.Abs(col.size.y * s.y), Mathf.Abs(col.size.z * s.z)) * 0.5f;
                bool dynamic = col.GetComponentInParent<Rigidbody>() != null && !col.GetComponentInParent<Rigidbody>().isKinematic;
                NativeWorld.AF_WorldAddBox(_world, new AFVector3(center), new AFVector3(half), new AFVector3(t.right), new AFVector3(t.up), mat, dynamic ? 1 : 0);
                ++_boxCount;
            }
        }

        // ── 音源の登録（WorldVoice が呼ぶ）──
        public void Register(WorldVoice v)
        {
            if (v == null || _world == IntPtr.Zero || _voices.Contains(v)) return;
            int id = NativeWorld.AF_WorldAddEmitter(_world, new AFVector3(v.transform.position), v.radius);
            if (id < 0) return;
            _voices.Add(v);
            v.Attach(id, _tail != null ? _tail.FdnMixHandle : IntPtr.Zero, _tail != null ? _tail.GetOrCreateDirectionBus(_maxFrames) : IntPtr.Zero);
        }
        public void Unregister(WorldVoice v)
        {
            if (v == null || !_voices.Remove(v)) return;
            if (_world != IntPtr.Zero && v.EmitterId >= 0) NativeWorld.AF_WorldRemoveEmitter(_world, v.EmitterId);
            v.Detach();
        }

        private void RebindFdn()
        {
            if (_tail == null || _world == IntPtr.Zero) return;
            var fdn = _tail.ReplaceFdnMix(_maxFrames);      // 古い器は TailBusRenderer が 1 秒後に壊す
            NativeWorld.AF_WorldBindFdn(_world, fdn);
            foreach (var v in _voices) v.SetFdn(fdn);
        }

        private void Update()
        {
            if (_world == IntPtr.Zero) return;
            long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
            if (listener != null)
                NativeWorld.AF_WorldSetListener(_world, new AFVector3(listener.position), new AFVector3(listener.forward), new AFVector3(listener.up));
            _w5[0] = weightDirect; _w5[1] = weightEarly; _w5[2] = weightLate; _w5[3] = weightDiffract; _w5[4] = weightTransmit;
            NativeWorld.AF_WorldSetWeights(_world, _w5);
            NativeWorld.AF_WorldSetResponse(_world, levelSec, colourSec, statSec, directionSec);
            NativeWorld.AF_WorldSetRays(_world, raysPerEmitter, maxBounces);
            foreach (var v in _voices)
                if (v != null && v.EmitterId >= 0)
                    NativeWorld.AF_WorldSetEmitter(_world, v.EmitterId, new AFVector3(v.transform.position), v.radius, v.operated ? 1 : 0, v.loudness);

            NativeWorld.AF_WorldUpdate(_world, Mathf.Max(1e-4f, Time.deltaTime));
            if (NativeWorld.AF_WorldFdnStale(_world) != 0) RebindFdn();

            foreach (var v in _voices)
                if (v != null && v.EmitterId >= 0 && v.VoiceHandle != IntPtr.Zero)
                    NativeWorld.AF_WorldApplyVoice(_world, v.EmitterId, v.VoiceHandle, _sampleRate);
            UpdateMs = (System.Diagnostics.Stopwatch.GetTimestamp() - t0) * 1000f / System.Diagnostics.Stopwatch.Frequency;
        }

        /// 情報タブ用。
        public bool TryGetMixInfo(WorldVoice v, out AFMixInfo info)
        {
            info = default;
            if (_world == IntPtr.Zero || v == null || v.EmitterId < 0) return false;
            return NativeWorld.AF_WorldMixInfo(_world, v.EmitterId, out info) != 0;
        }
        public int ListenerRoom => (_world != IntPtr.Zero && listener != null) ? NativeWorld.AF_WorldRoomAt(_world, new AFVector3(listener.position)) : -1;
    }
}
