// AcousticWorld.cs ── 新コア（Flow）のホスト（段 4。段 5 で動く箱）。
//
// ■ 役割
//   場面の箱（BoxCollider）と材質を音響エンジンの「世界」に渡し、毎フレーム リスナーと音源の位置を置いて
//   AF_WorldUpdate を 1 回呼び、配分を各 WorldVoice の Voice に写す。それだけ。
//   音の計算は全部 DLL の中（C# は「音の面の操作」だけ）。
//
// ■ 中の仕組み
//   OnEnable: 世界を作り、BoxCollider を拾って箱を足し（材質は AcousticSurface があればそれ、無ければ既定）、
//             AudioListener の Transform をリスナーに、AudioListener に TailBusRenderer（後期の器と方向バスの持ち主）を付ける。
//             動く箱（SwingDoor の下、非キネマティックの Rigidbody、dynamicColliders に入れた物）は dynamic 印で足す。
//   Update:   動く箱の位置 → リスナー → 音源 → AF_WorldUpdate → 各 Voice へ ApplyVoice。
//             FDN の器: 世界が「古い」と言ったら（部屋グラフの作り直し）、TailBusRenderer.ReplaceFdnMix で新しい器を作り、
//             AF_WorldBindFdn と各 Voice の AF_VoiceSetFdnMix を向け直す。
//   Register/Unregister: WorldVoice が自分を登録する（AF_WorldAddEmitter）。
//
// ■ 繋がり
//   受ける: 場面（BoxCollider、AcousticSurface、SwingDoor、AudioListener、WorldVoice）。
//   渡す:   DLL（NativeWorld）、WorldVoice（Voice の設定）、TailBusRenderer（器）。
//
// ■ 退けた書き方
//   ・AcousticFlowSceneDemo（旧ホスト 3,000 行）を流用: 旧 API と旧の模型の切り替えが焼き込まれている。新コアは 200 行で足りる。
//   ・動く箱を毎フレーム全部送る: 静的な箱を動かすと部屋グラフの作り直し（16 ms）が毎フレーム走る。印の付いた物だけ。
//
// ■ 壊れる所
//   ・AudioListener が無いと世界は作れても音が出ない（リスナー位置が原点のまま）。警告を出す。
//   ・器を差し替えたのに Voice の AF_VoiceSetFdnMix を向け直さないと、Voice は壊された器へ送り続ける。
//   ・扉に dynamic 印が付かないと、閉じた扉が部屋グラフに焼かれて戸口が消える（部屋が 2 つに割れたまま開かない）。
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
        [Tooltip("動く箱として扱う Collider（SwingDoor の下と非キネマティックの Rigidbody は自動で動く扱い）")]
        public List<Collider> dynamicColliders = new List<Collider>();

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
        public int ApertureCount => _world != IntPtr.Zero ? NativeWorld.AF_WorldApertureCount(_world) : 0;
        public float ApertureOpenFrac(int i) => _world != IntPtr.Zero ? NativeWorld.AF_WorldApertureOpenFrac(_world, i) : 1f;
        public float UpdateMs { get; private set; }
        public int BoxCount => _boxes.Count;
        public int DynamicCount { get; private set; }
        public IReadOnlyList<WorldVoice> Voices => _voices;
        public TailBusRenderer TailHost => _tail;

        private struct Box { public Collider col; public int id; public bool dynamic; }
        private IntPtr _world;
        private readonly List<Box> _boxes = new List<Box>();
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

            if (listener != null)
            {
                _tail = listener.GetComponent<TailBusRenderer>();
                if (_tail == null) _tail = listener.gameObject.AddComponent<TailBusRenderer>();
                _tail.enableSharedTail = false;        // 畳み込みの尾は使わない
                RebindFdn();
            }
            foreach (var v in FindObjectsByType<WorldVoice>(FindObjectsSortMode.None)) Register(v);
            Debug.Log($"[AcousticWorld] 世界: 箱 {_boxes.Count}（動く物 {DynamicCount}）/ 部屋 {RoomCount} / 戸口 {ApertureCount} / 音源 {_voices.Count}（レイ {raysPerEmitter} 本）");
        }

        private void OnDisable()
        {
            foreach (var v in _voices) v.Detach();
            _voices.Clear();
            _boxes.Clear();
            if (_world != IntPtr.Zero) { NativeWorld.AF_WorldDestroy(_world); _world = IntPtr.Zero; }
            if (Instance == this) Instance = null;
        }

        private bool IsDynamic(Collider col)
        {
            if (dynamicColliders.Contains(col)) return true;
            if (col.GetComponentInParent<SwingDoor>() != null) return true;
            var rb = col.GetComponentInParent<Rigidbody>();
            return rb != null && !rb.isKinematic;
        }

        private static void ObbOf(Collider c, out Vector3 center, out Vector3 half, out Vector3 right, out Vector3 up)
        {
            var box = c as BoxCollider;
            var t = c.transform;
            Vector3 s = t.lossyScale;
            center = t.TransformPoint(box.center);
            half = new Vector3(Mathf.Abs(box.size.x * s.x), Mathf.Abs(box.size.y * s.y), Mathf.Abs(box.size.z * s.z)) * 0.5f;
            right = t.right; up = t.up;
        }

        private void CollectBoxes()
        {
            _boxes.Clear(); DynamicCount = 0;
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
                ObbOf(col, out var center, out var half, out var right, out var up);
                bool dyn = IsDynamic(col);
                int id = NativeWorld.AF_WorldAddBox(_world, new AFVector3(center), new AFVector3(half), new AFVector3(right), new AFVector3(up), mat, dyn ? 1 : 0);
                _boxes.Add(new Box { col = col, id = id, dynamic = dyn });
                if (dyn) DynamicCount++;
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
            // 動く箱（扉）の位置。静的な箱は触らない（触ると部屋グラフが作り直される）。
            foreach (var b in _boxes)
            {
                if (!b.dynamic || b.col == null) continue;
                ObbOf(b.col, out var center, out _, out var right, out var up);
                NativeWorld.AF_WorldSetBoxTransform(_world, b.id, new AFVector3(center), new AFVector3(right), new AFVector3(up));
            }
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
