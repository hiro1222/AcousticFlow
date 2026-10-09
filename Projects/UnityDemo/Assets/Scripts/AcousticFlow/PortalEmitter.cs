// ★旧コア（Demo / Test_* の場面）。新コア（Flow_* の場面 ＝ AcousticWorld 系）では使っていない。
//   2026-09-24 の棚卸しで印を付けた。試聴で新コアへの乗り換えが決まったら、旧コアの C++ ごと消す（段 10）。
//   それまでは聞き比べの基準として残す。──代わり: 新コアでは戸口の線音源が engine の中にある（lateThrough=2）
// PortalEmitter.cs — 扉の定点。**隣の空間の響きを、戸口の位置から鳴らす。**
//
// ★何のためにあるか
//   これまで後期残響（尾）は部屋ごとの共有バスで畳んで、リスナーの周囲から 2ch で出していた。
//   向きは探針で作る左右の耳のバランスだけ。だから「洞窟の前に立っているのに、洞窟の響きが
//   まわりから聞こえる」（S1「響きには向きがある」に反する）。閉じた扉ごしの音も同じで、
//   前提「閉じた扉ごしの音は扉から来る」が成り立たなかった。
//   基準の実測（scene_regression [診断] 洞窟の口）: 場は口を指しているのに（正面 2 m で
//   平均方向が口から 6.6°）左右バランスは 0.86 で向きが消え、6 m 離れても尾の量は落ちない。
//
// ★どう鳴らすか
//   部屋のバスの出力のモノラル（AF_TailBusLastMono）を、この物体の位置（戸口）に置いた
//   別のボイスの入力にする。ボイスは直接音タップ 1 本だけで、
//     ゲイン = 口の結合率（帯域別、AF_ScenePortalDiffuseCoupling） × 口からの距離減衰 × 口→リスナーの遮蔽
//     方向   = リスナーから見た戸口の向き（HRTF）
//   を毎フレーム主スレッドで入れ直す。入力は 1 ブロック前のバスの出力（順序の都合。
//   尾の立ち上がり 25 ms より短いので聞こえない）。
//
// ★二重に鳴らさないために
//   部屋のバスの直接の足し込みは「リスナーのまわりのその部屋の占め方 w」で薄め、
//   この定点は (1 − w) を持つ。部屋の中では w=1 で従来どおり、外では w=0 で定点だけ。
//   戸口の真ん中では半々。どちらも同じバスのモノラルなので、レベルは足して 1 になる。
//   （AcousticFlowSceneDemo.UpdatePortalTail が w と feedGain を入れる。）
//
// ★この定点自身は尾を送らない（自分の部屋のバスへ戻すと帰還ループになる）。
//   隣の部屋の残響が更にこの部屋を鳴らす分（再残響）は、いまは持たない。
using System;
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public class PortalEmitter : MonoBehaviour
    {
        [Header("何を運ぶか（AcousticFlowSceneDemo が入れる）")]
        public int portalId = -1;        // どの口か
        public int room = -1;            // どの部屋の尾を運ぶか（部屋番号）
        public int busShape = -1;        // その部屋のバスの鍵（尾の形の index）
        [Tooltip("(1 − その部屋の占め方) × 全体レベル。主スレッドが毎フレーム入れる。")]
        [Range(0f, 4f)] public float feedGain = 0f;
        [Tooltip("口の結合率（帯域別 0..1）。開いていれば 1 に近く、閉じた扉なら板の透過。")]
        public float[] coupling6 = { 1f, 1f, 1f, 1f, 1f, 1f };
        [Header("計器")]
        public float rmsOut;

        // 主スレッドが入れ、オーディオスレッドが読む。IntPtr/float の代入は原子的。
        //   （TailBusRenderer の辞書をオーディオスレッドから引かないための写し。）
        [NonSerialized] public IntPtr busHandle = IntPtr.Zero;

        private IntPtr _voice = IntPtr.Zero;
        private IntPtr _hrtf = IntPtr.Zero;
        private float[] _in, _l, _r;
        private int _sampleRate = 48000;
        private int _maxFrames = 1024;
        private volatile bool _ready;
        private readonly Native.AFVoiceTap[] _tap = new Native.AFVoiceTap[1];
        private float _headCm = 57f;

        /// 主スレッドから 1 回。ボイスと HRTF を用意し、無音クリップの AudioSource で
        /// OnAudioFilterRead を回す（Unity はクリップが鳴っていないとフィルタを呼ばない）。
        public void Init(string hrtfFileName)
        {
            if (_ready) return;
            if (!Native.CheckAbi()) return;
            _sampleRate = AudioSettings.outputSampleRate;
            AudioSettings.GetDSPBufferSize(out int bufLen, out _);
            _maxFrames = Mathf.Max(256, bufLen);
            var cfg = new Native.AFVoiceConfig
            {
                sampleRate = _sampleRate,
                maxFrames = _maxFrames,
                tailSeconds = 0.1f,          // 尾は持たない（入力が既に尾）
                tapCrossfadeMs = 30f,
                hrtfCrossfadeMs = 12f,
                hrtfCrossoverHz = 700f,
                tailFirstBlock = 64,
                tailCapBlock = 8192,
            };
            _voice = Native.AF_VoiceCreate(ref cfg);
            if (_voice == IntPtr.Zero) { Debug.LogError("[PortalEmitter] ボイスを作れませんでした"); return; }
            _hrtf = IntPtr.Zero;
            if (!string.IsNullOrEmpty(hrtfFileName))
            {
                string path = System.IO.Path.Combine(Application.streamingAssetsPath, hrtfFileName);
                if (System.IO.File.Exists(path)) _hrtf = Native.AF_HrtfLoadFile(path);
            }
            if (_hrtf == IntPtr.Zero) _hrtf = Native.AF_HrtfCreateSynthetic(_sampleRate);
            Native.AF_VoiceSetHrtf(_voice, _hrtf);
            Native.AF_VoiceSetHrtfEnabled(_voice, 1);
            Native.AF_VoiceSetTailLevel(_voice, 0f);      // 自分では尾を作らない
            _in = new float[_maxFrames];
            _l = new float[_maxFrames];
            _r = new float[_maxFrames];

            var src = GetComponent<AudioSource>();
            if (src == null) src = gameObject.AddComponent<AudioSource>();
            src.clip = AudioClip.Create("AF_PortalSilence", _sampleRate, 1, _sampleRate, false);
            src.loop = true;
            src.playOnAwake = false;
            src.spatialBlend = 0f;        // 空間化はエンジン（HRTF）がやる
            src.volume = 1f;
            src.Play();
            _ready = true;
        }

        /// 直接音タップ 1 本と向きを入れ直す（主スレッド）。
        ///   gain6   : 結合率 × 距離減衰 × 遮蔽（帯域別）
        ///   dirLocal: リスナー座標系での戸口の向き
        public void SetTap(float[] gain6, Vector3 dirLocal, float headCm)
        {
            if (!_ready || _voice == IntPtr.Zero || gain6 == null || gain6.Length < 6) return;
            _headCm = headCm;
            float xr = Mathf.Clamp01((dirLocal.x + 1f) * 0.5f);
            _tap[0] = new Native.AFVoiceTap
            {
                delaySamples = 0,
                g0 = gain6[0], g1 = gain6[1], g2 = gain6[2], g3 = gain6[3], g4 = gain6[4], g5 = gain6[5],
                panL = Mathf.Sqrt(1f - xr), panR = Mathf.Sqrt(xr),
                gSpec = 1f, gDiff = 0f,
                hrtfWeight = 0f,
                dirX = dirLocal.x, dirY = dirLocal.y, dirZ = dirLocal.z,
            };
            Native.AF_VoiceSetTaps(_voice, _tap, 1);
            Native.AF_VoiceSetDirection(_voice, new AFVector3(dirLocal), _headCm);
        }

        /// バスを壊す前に呼ぶ（TailBusRenderer.OnDisable）。以後は無音を出す。
        public void Detach()
        {
            _ready = false;
            busHandle = IntPtr.Zero;
        }

        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (!_ready || _voice == IntPtr.Zero || channels < 1) { Array.Clear(data, 0, data.Length); return; }
            int frames = data.Length / channels;
            if (frames > _in.Length) { Array.Clear(data, 0, data.Length); return; }
            IntPtr bus = busHandle;
            float g = feedGain;
            if (bus == IntPtr.Zero || g <= 0f) { Array.Clear(data, 0, data.Length); rmsOut = 0f; return; }
            // 1 ブロック前のバスの尾（モノラル）。無ければ 0 で埋まって返る。
            Native.AF_TailBusLastMono(bus, _in, frames);
            for (int i = 0; i < frames; i++) _in[i] *= g;
            var m = new Native.AFVoiceMetering();
            Native.AF_VoiceRender(_voice, _in, frames, _l, _r, ref m);
            rmsOut = m.rmsOut;
            for (int f = 0; f < frames; f++)
            {
                int b = f * channels;
                if (channels >= 2)
                {
                    data[b] = _l[f]; data[b + 1] = _r[f];
                    for (int c = 2; c < channels; c++) data[b + c] = 0.5f * (_l[f] + _r[f]);
                }
                else data[b] = 0.5f * (_l[f] + _r[f]);
            }
        }

        private void OnDestroy()
        {
            _ready = false;
            if (_voice != IntPtr.Zero) { Native.AF_VoiceDestroy(_voice); _voice = IntPtr.Zero; }
            if (_hrtf != IntPtr.Zero) { Native.AF_HrtfDestroy(_hrtf); _hrtf = IntPtr.Zero; }
        }
    }
}
