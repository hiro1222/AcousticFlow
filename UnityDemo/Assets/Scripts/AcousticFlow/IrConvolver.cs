// IrConvolver.cs
// IR畳み込みPoC（Unity C#・Wwise非経由）。AcousticFlowSceneDemo.Status のIRタップを畳み込む。
//   早期反射 = 離散タップのマルチタップ畳み込み（3バンド分割・到来方向パン・距離減衰・IR切替クロスフェード）。
//   後期残響 = FDN（Feedback Delay Network）。4本の遅延線＋Hadamard帰還＋RT60減衰＋HFダンピングで
//              "本当に密"な尾を安く生成（velvet noiseの粒立ち問題を解消。打楽器でも滑らか）。
//
//   使い方：空GameObjectに AudioSource + このコンポーネント → Play。
//     generateTestSignal=ON で内部クリック/ノイズを畳み込む。OFFで AudioSource のクリップを畳み込む。
using System.Threading;
using UnityEngine;

namespace AcousticFlow
{
    [RequireComponent(typeof(AudioSource))]
    public class IrConvolver : MonoBehaviour
    {
        [Tooltip("ON: 内部テスト信号を畳み込む（ドライ素材不要）。OFF: このAudioSourceのクリップを畳み込む。")]
        public bool generateTestSignal = true;
        [Tooltip("ON: テスト信号を連続ノイズに（途切れ確認用）。OFF: クリック列。")]
        public bool testContinuousNoise = false;
        [Tooltip("テストクリックの間隔(秒)。")]
        public float clickIntervalSec = 0.6f;
        [Tooltip("早期反射IRの最大長(ms)。これを超える遅延のタップは切り捨て。")]
        public float maxIrMs = 120f;
        [Tooltip("IR(タップ)を再構築する間隔(秒)。")]
        public float irUpdateSec = 0.05f;
        [Tooltip("全体出力ゲイン。")]
        [Range(0f, 4f)] public float outputGain = 0.6f;
        [Tooltip("IR切替のクロスフェード長(ms)。壁変化などでIRが変わる時のプチ音を消す。")]
        [Range(2f, 100f)] public float crossfadeMs = 30f;
        [Tooltip("反射/回折タップの音量倍率（直接音=1固定）。0=直接のみ。")]
        [Range(0f, 1f)] public float reflectionLevel = 0.35f;

        [Header("後期残響尾（FDN）")]
        [Tooltip("ON: FDNで合成した後期残響尾を足す（RT60はエコグラム由来）。")]
        public bool enableReverbTail = true;
        [Tooltip("後期尾の音量倍率（wetに乗算）。")]
        [Range(0f, 2f)] public float tailLevel = 0.6f;
        [Tooltip("後期尾の高域ダンピング。0=明るい / 1=暗い（高域が先に減る）。")]
        [Range(0f, 1f)] public float tailDamping = 0.25f;
        [Tooltip("残響に入れる前の低域カット(Hz)。低域モードのボワつき/びりびりを抑える。直接音の低音は残る。0で無効。")]
        [Range(0f, 400f)] public float tailLowCutHz = 150f;
        [Tooltip("残響入力の拡散（allpass）。粒感/金属感を消して滑らかに。0=無効 / 0.5〜0.7が定番。")]
        [Range(0f, 0.8f)] public float tailDiffusion = 0.62f;

        private const float kSplitLowHz = 350f;
        private const float kSplitHighHz = 1400f;
        private const int kFdnN = 16;  // 遅延線数。多いほどモード密度が上がり滑らか（4=メタリック / 8 / 16）。
        private const int kApN = 4;    // 入力ディフュージョンの allpass 段数

        private struct ConvTap { public int delaySamples; public float gLo, gMid, gHi, panL, panR; }

        // RBJ バイカッド（Direct Form II Transposed）。フィールドに置いて in-place で使う。
        private struct Biquad
        {
            public float b0, b1, b2, a1, a2, z1, z2;
            public void SetLowpass(float fc, float fs) { SetLH(fc, fs, true); }
            public void SetHighpass(float fc, float fs) { SetLH(fc, fs, false); }
            private void SetLH(float fc, float fs, bool low)
            {
                float w0 = 2f * Mathf.PI * fc / fs;
                float cw = Mathf.Cos(w0), sw = Mathf.Sin(w0);
                float alpha = sw / (2f * 0.70710678f);
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

        // 早期反射（クロスフェード付きマルチタップ畳み込み）
        private ConvTap[] _ir = new ConvTap[0];
        private ConvTap[] _irNext = new ConvTap[0];
        private ConvTap[] _pendingIr;
        private bool _fading;
        private int _xfadePos, _xfadeLen;
        private float[] _ringLo, _ringMid, _ringHi;
        private int _ringMask;
        private int _writePos;
        private Biquad _lp, _hp;

        // FDN 後期尾
        private float[][] _fdnBuf;
        private int[] _fdnLen;
        private int[] _fdnPos;
        private float[] _fdnLp;      // 各線のHFダンピング(one-pole LP)状態
        private float[] _fdnGain;    // 各線の帰還減衰(RT60由来)
        private float _dampCoef = 0.5f;
        private float _fdnInGain = 0.5f;  // 入力正規化：長RT60でも定常レベル一定（違いは"長さ"に）
        private float[] _fdnY, _fdnF;     // audio-thread の作業配列（読み出し / 帰還）
        private float _oNorm = 0.25f;     // 1/√N（FWHT/出力の正規化）
        private float _fdnHpState;        // 残響入力の低域カット(one-pole HP)状態
        private float _hpCoef = 0.02f;    // 低域カットのカットオフ係数
        private float[][] _apBuf;         // 入力ディフュージョン allpass の遅延バッファ
        private int[] _apLen, _apPos;

        private int _sampleRate;
        private double _clickTimer;
        private uint _noiseState = 2463534242u;
        private float _irTimer;
        private AudioSource _src;

        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            int need = Mathf.CeilToInt(maxIrMs * 0.001f * _sampleRate) + 1024;
            int ringSize = Mathf.NextPowerOfTwo(need);
            _ringLo = new float[ringSize];
            _ringMid = new float[ringSize];
            _ringHi = new float[ringSize];
            _ringMask = ringSize - 1;
            _xfadeLen = Mathf.Max(1, Mathf.RoundToInt(crossfadeMs * 0.001f * _sampleRate));

            _lp.SetLowpass(kSplitLowHz, _sampleRate);
            _hp.SetHighpass(kSplitHighHz, _sampleRate);

            // FDN：16本の遅延線（互いに素っぽい長さ＝共鳴/色付き防止・広く分散）。
            float[] fdnMs = { 15.3f, 18.1f, 21.7f, 25.3f, 29.1f, 33.7f, 37.9f, 42.3f,
                              46.7f, 51.1f, 55.9f, 60.3f, 65.1f, 69.7f, 73.9f, 78.3f };
            _oNorm = 1f / Mathf.Sqrt(kFdnN);
            _fdnBuf = new float[kFdnN][];
            _fdnLen = new int[kFdnN];
            _fdnPos = new int[kFdnN];
            _fdnLp = new float[kFdnN];
            _fdnGain = new float[kFdnN];
            _fdnY = new float[kFdnN];
            _fdnF = new float[kFdnN];
            for (int i = 0; i < kFdnN; i++)
            {
                _fdnLen[i] = Mathf.Max(1, Mathf.RoundToInt(fdnMs[i] * 0.001f * _sampleRate));
                _fdnBuf[i] = new float[_fdnLen[i]];
                _fdnGain[i] = 0.85f;
            }
            UpdateFdnParams();

            // 入力ディフュージョン：短い allpass を直列（位相を撹拌して即座に密＝粒感/金属感を消す）。
            float[] apMs = { 7.3f, 9.9f, 12.7f, 15.1f };
            _apBuf = new float[kApN][];
            _apLen = new int[kApN];
            _apPos = new int[kApN];
            for (int i = 0; i < kApN; i++)
            {
                _apLen[i] = Mathf.Max(1, Mathf.RoundToInt(apMs[i] * 0.001f * _sampleRate));
                _apBuf[i] = new float[_apLen[i]];
            }

            _src = GetComponent<AudioSource>();
            if (_src.clip == null)
                _src.clip = AudioClip.Create("ir_silence", _sampleRate, 1, _sampleRate, false);
            _src.loop = true;
            _src.spatialBlend = 0f;   // 2D（空間化はIR側）
            _src.playOnAwake = false;
            _src.Play();
        }

        private void Update()
        {
            _irTimer += Time.deltaTime;
            if (_irTimer >= irUpdateSec) { _irTimer = 0f; RebuildIr(); UpdateFdnParams(); }
        }

        // RT60(エコグラム由来)から FDN の帰還減衰とダンピング係数を更新（main thread）。
        private void UpdateFdnParams()
        {
            float rt = Mathf.Max(AcousticFlowSceneDemo.Status.RtSeconds, 0.05f);  // 0除算だけ回避。開けた場所は小RT60=ほぼ無響（尊重）
            float meanG = 0f;
            for (int i = 0; i < kFdnN; i++)
            {
                float g = Mathf.Pow(10f, -3f * _fdnLen[i] / (rt * _sampleRate));  // RT60秒で-60dB
                _fdnGain[i] = Mathf.Min(g, 0.995f);                                // 発振防止クランプ
                meanG += _fdnGain[i];
            }
            meanG /= kFdnN;
            _fdnInGain = Mathf.Sqrt(Mathf.Max(1f - meanG * meanG, 1e-3f));         // 入力正規化(定常レベル一定)
            _dampCoef = Mathf.Clamp(1f - tailDamping * 0.9f, 0.05f, 1f);           // 1=明るい / 小=暗い
            _hpCoef = Mathf.Clamp01(2f * Mathf.PI * tailLowCutHz / _sampleRate);   // 残響入力の低域カット
        }

        // 早期反射のタップ（Status）を {遅延サンプル・3バンドゲイン・パン} に変換して参照swap。
        private void RebuildIr()
        {
            var d = AcousticFlowSceneDemo.Status.TapDelayMs;
            var bg = AcousticFlowSceneDemo.Status.TapBandGain;
            var bb = AcousticFlowSceneDemo.Status.TapGain;
            var pl = AcousticFlowSceneDemo.Status.TapPanL;
            var pr = AcousticFlowSceneDemo.Status.TapPanR;
            int tc = AcousticFlowSceneDemo.Status.TapCount;
            if (d == null || tc <= 0) return;

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
                    float g = (bb != null && i < bb.Length) ? bb[i] : 0f;
                    ir[n].gLo = g; ir[n].gMid = g; ir[n].gHi = g;
                }
                ir[n].panL = (pl != null && i < pl.Length) ? pl[i] : 0.70710678f;
                ir[n].panR = (pr != null && i < pr.Length) ? pr[i] : 0.70710678f;
                float rl = (i == 0) ? 1f : reflectionLevel;      // 反射/回折(i>0)は音量を絞る
                ir[n].gLo *= rl; ir[n].gMid *= rl; ir[n].gHi *= rl;
                n++;
            }
            if (n != ir.Length) System.Array.Resize(ref ir, n);
            _pendingIr = ir;
        }

        // audio-thread：早期反射(マルチタップ畳み込み・クロスフェード) ＋ FDN後期尾 → ステレオ出力。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            var pending = Interlocked.Exchange(ref _pendingIr, null);
            if (pending != null)
            {
                if (_fading) _ir = _irNext;
                _irNext = pending; _xfadePos = 0; _fading = true;
            }

            int frames = data.Length / channels;
            double clickPeriod = clickIntervalSec * _sampleRate;
            float wet = Mathf.Clamp01(AcousticFlowSceneDemo.Status.Wet);   // 開けた場所は wet≈0 = 残響ほぼ無し（尊重）
            float srcLv = AcousticFlowSceneDemo.Status.SourceLevel;        // 遮蔽レベル(0..1)。壁裏では残響も絞る
            if (srcLv <= 0f) srcLv = 1f;
            float fdnWet = enableReverbTail ? tailLevel * wet * srcLv : 0f;

            for (int f = 0; f < frames; f++)
            {
                float dry;
                if (generateTestSignal)
                {
                    if (testContinuousNoise)
                    {
                        _noiseState ^= _noiseState << 13; _noiseState ^= _noiseState >> 17; _noiseState ^= _noiseState << 5;
                        dry = (int)_noiseState * (1f / 2147483648f) * 0.25f;
                    }
                    else
                    {
                        _clickTimer += 1.0;
                        if (_clickTimer >= clickPeriod) { _clickTimer -= clickPeriod; dry = 1f; }
                        else dry = 0f;
                    }
                }
                else
                {
                    float s = 0f;
                    for (int c = 0; c < channels; c++) s += data[f * channels + c];
                    dry = s / channels;
                }

                // 3バンド分割 → 履歴へ（早期反射用）。
                float lo = _lp.Process(dry);
                float hi = _hp.Process(dry);
                float mid = dry - lo - hi;
                int wi = _writePos & _ringMask;
                _ringLo[wi] = lo; _ringMid[wi] = mid; _ringHi[wi] = hi;

                // 早期反射の畳み込み（クロスフェード）。
                ConvOne(_ir, _writePos, out float aL, out float aR);
                float outL, outR;
                if (_fading)
                {
                    ConvOne(_irNext, _writePos, out float bL, out float bR);
                    float t = (float)_xfadePos / _xfadeLen;
                    outL = aL * (1f - t) + bL * t;
                    outR = aR * (1f - t) + bR * t;
                    if (++_xfadePos >= _xfadeLen) { _fading = false; _ir = _irNext; }
                }
                else { outL = aL; outR = aR; }

                // FDN 後期尾。
                if (fdnWet > 0f)
                {
                    // 遅延線を読み出し → ステレオ出力（bit0/bit1で符号を変えた2パターン＝decorrelated・N非依存）。
                    float wL = 0f, wR = 0f;
                    for (int i = 0; i < kFdnN; i++)
                    {
                        float y = _fdnBuf[i][_fdnPos[i]];
                        _fdnY[i] = y;
                        wL += ((i & 1) == 0) ? y : -y;
                        wR += ((i & 2) == 0) ? y : -y;
                    }
                    // 帰還信号＝各線に HFダンピング(one-pole LP) → RT60減衰。
                    for (int i = 0; i < kFdnN; i++)
                        _fdnF[i] = _fdnGain[i] * (_fdnLp[i] += _dampCoef * (_fdnY[i] - _fdnLp[i]));
                    // 高速Walsh-Hadamard変換（8x8ロスレス混合）＝密度＆拡散。
                    for (int len = 1; len < kFdnN; len <<= 1)
                        for (int b = 0; b < kFdnN; b += len << 1)
                            for (int j = b; j < b + len; j++)
                            {
                                float a = _fdnF[j], c = _fdnF[j + len];
                                _fdnF[j] = a + c; _fdnF[j + len] = a - c;
                            }
                    // dry を入れて回す（帰還は 1/√N 正規化）。残響入力は低域カット（HP）＋allpass拡散。
                    _fdnHpState += _hpCoef * (dry - _fdnHpState);   // 一次LP
                    float x = (_hpCoef > 0f) ? dry - _fdnHpState : dry;   // HP = dry - LP（低域除去）
                    for (int a = 0; a < kApN; a++)                  // 入力ディフュージョン（allpass直列）
                    {
                        float bo = _apBuf[a][_apPos[a]];
                        float di = x + tailDiffusion * bo;
                        _apBuf[a][_apPos[a]] = di;
                        x = bo - tailDiffusion * di;
                        if (++_apPos[a] >= _apLen[a]) _apPos[a] = 0;
                    }
                    float din = x * _fdnInGain;
                    for (int i = 0; i < kFdnN; i++)
                    {
                        _fdnBuf[i][_fdnPos[i]] = din + _fdnF[i] * _oNorm;
                        if (++_fdnPos[i] >= _fdnLen[i]) _fdnPos[i] = 0;
                    }
                    outL += wL * _oNorm * fdnWet;
                    outR += wR * _oNorm * fdnWet;
                }

                outL *= outputGain; outR *= outputGain;
                int baseI = f * channels;
                if (channels >= 2)
                {
                    data[baseI] = outL; data[baseI + 1] = outR;
                    for (int c = 2; c < channels; c++) data[baseI + c] = (outL + outR) * 0.5f;
                }
                else data[baseI] = (outL + outR) * 0.5f;
                _writePos++;
            }
        }

        // 1つのIRを畳んで L/R を返す（3バンド×タップ×パン）。クロスフェード中は新旧2回呼ぶ。
        private void ConvOne(ConvTap[] ir, int wp, out float L, out float R)
        {
            float l = 0f, r = 0f;
            if (ir != null)
                for (int k = 0; k < ir.Length; k++)
                {
                    int rp = (wp - ir[k].delaySamples) & _ringMask;
                    float tv = ir[k].gLo * _ringLo[rp] + ir[k].gMid * _ringMid[rp] + ir[k].gHi * _ringHi[rp];
                    l += tv * ir[k].panL; r += tv * ir[k].panR;
                }
            L = l; R = r;
        }
    }
}
