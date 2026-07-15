// IrConvolver.cs
// IR畳み込みPoC（Unity C#・Wwise非経由）。AcousticFlowSceneDemo.Status のIRタップを畳み込む。
//   段1: マルチタップ・ディレイ（離散IR）で反射/残響を付ける。
//   段2: 3バンド分割畳み込み。ドライを Low/Mid/High に分け、各タップの3バンドゲインで色付け
//        → 材質(柔らかい面=高域減衰)・距離(空気吸収)が畳み込みに乗る。
//
//   使い方：空GameObjectに AudioSource + このコンポーネント → Play。
//     generateTestSignal=ON で内部クリックを畳み込む（ドライ素材不要・手を叩いて反響を聞く感覚）。
//   段3で空間化(HRTF)、段4で動的クロスフェード。
using UnityEngine;

namespace AcousticFlow
{
    [RequireComponent(typeof(AudioSource))]
    public class IrConvolver : MonoBehaviour
    {
        [Tooltip("ON: 内部クリックを畳み込む（ドライ素材不要）。OFF: このAudioSourceのクリップを畳み込む。")]
        public bool generateTestSignal = true;
        [Tooltip("テストクリックの間隔(秒)。")]
        public float clickIntervalSec = 0.6f;
        [Tooltip("IRの最大長(ms)。これを超える遅延のタップは切り捨て。")]
        public float maxIrMs = 120f;
        [Tooltip("IR(タップ)を再構築する間隔(秒)。")]
        public float irUpdateSec = 0.05f;
        [Tooltip("全体出力ゲイン。")]
        [Range(0f, 4f)] public float outputGain = 0.6f;

        // 3バンド分割のクロスオーバー周波数（低/中/高 の境目）。
        private const float kSplitLowHz = 350f;
        private const float kSplitHighHz = 1400f;

        private struct ConvTap { public int delaySamples; public float gLo, gMid, gHi; }

        // RBJ バイカッド（Direct Form II Transposed）。struct をフィールドに置いて in-place で使う。
        private struct Biquad
        {
            public float b0, b1, b2, a1, a2, z1, z2;
            public void SetLowpass(float fc, float fs) { SetLH(fc, fs, true); }
            public void SetHighpass(float fc, float fs) { SetLH(fc, fs, false); }
            private void SetLH(float fc, float fs, bool low)
            {
                float w0 = 2f * Mathf.PI * fc / fs;
                float cw = Mathf.Cos(w0), sw = Mathf.Sin(w0);
                float alpha = sw / (2f * 0.70710678f);   // Q=1/√2（Butterworth）
                if (low) { b0 = (1f - cw) * 0.5f; b1 = 1f - cw; b2 = (1f - cw) * 0.5f; }
                else { b0 = (1f + cw) * 0.5f; b1 = -(1f + cw); b2 = (1f + cw) * 0.5f; }
                float a0 = 1f + alpha; a1 = -2f * cw; a2 = 1f - alpha;
                b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
                z1 = 0f; z2 = 0f;
            }
            public float Process(float x)
            {
                float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        }

        private ConvTap[] _ir = new ConvTap[0];   // audio-thread が読む（参照swapで受け渡し）
        private float[] _ringLo, _ringMid, _ringHi;  // 3バンドに分けた dry の履歴
        private int _ringMask;
        private int _writePos;
        private int _sampleRate;
        private double _clickTimer;
        private float _irTimer;
        private AudioSource _src;
        private Biquad _lp, _hp;   // Low = LP, High = HP, Mid = dry - Low - High

        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            int need = Mathf.CeilToInt(maxIrMs * 0.001f * _sampleRate) + 1024;
            int ringSize = Mathf.NextPowerOfTwo(need);
            _ringLo = new float[ringSize];
            _ringMid = new float[ringSize];
            _ringHi = new float[ringSize];
            _ringMask = ringSize - 1;

            _lp.SetLowpass(kSplitLowHz, _sampleRate);
            _hp.SetHighpass(kSplitHighHz, _sampleRate);

            _src = GetComponent<AudioSource>();
            if (_src.clip == null)
                _src.clip = AudioClip.Create("ir_silence", _sampleRate, 1, _sampleRate, false);
            _src.loop = true;
            _src.spatialBlend = 0f;   // 2D（空間化はIR側。段2まではモノ）
            _src.playOnAwake = false;
            _src.Play();
        }

        private void Update()
        {
            _irTimer += Time.deltaTime;
            if (_irTimer >= irUpdateSec) { _irTimer = 0f; RebuildIr(); }
        }

        // 主音源のタップ（Status）を {遅延サンプル・3バンドゲイン} 配列に変換して参照swap。
        private void RebuildIr()
        {
            var d = AcousticFlowSceneDemo.Status.TapDelayMs;
            var bg = AcousticFlowSceneDemo.Status.TapBandGain;
            var bb = AcousticFlowSceneDemo.Status.TapGain;   // フォールバック（広帯域）
            int tc = AcousticFlowSceneDemo.Status.TapCount;
            if (d == null || tc <= 0) { _ir = new ConvTap[0]; return; }

            int maxDelay = Mathf.CeilToInt(maxIrMs * 0.001f * _sampleRate);
            var ir = new ConvTap[tc];
            int n = 0;
            for (int i = 0; i < tc && i < d.Length; i++)
            {
                int ds = Mathf.RoundToInt(d[i] * 0.001f * _sampleRate);
                if (ds < 0) ds = 0;
                if (ds > maxDelay) continue;
                ir[n].delaySamples = ds;
                if (bg != null && i * 3 + 2 < bg.Length)
                {
                    ir[n].gLo = bg[i * 3 + 0]; ir[n].gMid = bg[i * 3 + 1]; ir[n].gHi = bg[i * 3 + 2];
                }
                else
                {
                    float g = (bb != null && i < bb.Length) ? bb[i] : 0f;   // 3バンド無ければ広帯域一律
                    ir[n].gLo = g; ir[n].gMid = g; ir[n].gHi = g;
                }
                n++;
            }
            if (n != ir.Length) System.Array.Resize(ref ir, n);
            _ir = ir;   // 参照swap（段2までクロスフェード無し＝切替でプチる場合あり→段4で解消）
        }

        // audio-thread：3バンド分割 → バンドごとマルチタップ・ディレイ → 合算。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            var ir = _ir;
            int frames = data.Length / channels;
            double clickPeriod = clickIntervalSec * _sampleRate;

            for (int f = 0; f < frames; f++)
            {
                float dry;
                if (generateTestSignal)
                {
                    _clickTimer += 1.0;
                    if (_clickTimer >= clickPeriod) { _clickTimer -= clickPeriod; dry = 1f; }
                    else dry = 0f;
                }
                else
                {
                    float s = 0f;
                    for (int c = 0; c < channels; c++) s += data[f * channels + c];
                    dry = s / channels;
                }

                // 3バンドに分割（low+mid+high = dry を厳密に保つ）。
                float lo = _lp.Process(dry);
                float hi = _hp.Process(dry);
                float mid = dry - lo - hi;

                int wi = _writePos & _ringMask;
                _ringLo[wi] = lo; _ringMid[wi] = mid; _ringHi[wi] = hi;

                float outv = 0f;
                if (ir != null)
                    for (int k = 0; k < ir.Length; k++)
                    {
                        int rp = (_writePos - ir[k].delaySamples) & _ringMask;
                        outv += ir[k].gLo * _ringLo[rp] + ir[k].gMid * _ringMid[rp] + ir[k].gHi * _ringHi[rp];
                    }
                outv *= outputGain;

                for (int c = 0; c < channels; c++) data[f * channels + c] = outv;  // 段2までモノ→全ch同値
                _writePos++;
            }
        }
    }
}
