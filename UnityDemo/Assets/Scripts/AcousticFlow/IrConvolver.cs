// IrConvolver.cs
// 段1：IR畳み込みPoC（Unity C#・Wwise非経由）。
//   AcousticFlowSceneDemo.Status のIRタップ（直接/反射/回折 = {相対遅延ms・広帯域ゲイン}）を読み、
//   マルチタップ・ディレイ（＝離散IRの畳み込み）でドライ音に反射・残響を付ける。
//   タップは少数の離散点なので、FFTでなくマルチタップ・ディレイで正確かつ軽い。
//
//   これが「タップ→IR→畳み込み→出力」の最小実証。段2で帯域別、段3で空間化(HRTF)、段4で動的クロスフェード。
//   ミドルウェア(Wwise)を通さず Unityのオーディオで鳴らす＝"ミドルウェア非依存"の実証も兼ねる。
//
//   使い方：空のGameObjectに AudioSource + このコンポーネントを付けて Play。
//     generateTestSignal=ON なら内部クリックを畳み込む（ドライ素材不要）。
//     ※IRは主音源(0)基準。リスナーを動かすと反射パターンが変わる。
using UnityEngine;

namespace AcousticFlow
{
    [RequireComponent(typeof(AudioSource))]
    public class IrConvolver : MonoBehaviour
    {
        [Tooltip("ON: 内部でクリックを生成して畳み込む（手を叩いて部屋の反射を聞く感覚・ドライ素材不要）。OFF: このAudioSourceのクリップを畳み込む。")]
        public bool generateTestSignal = true;
        [Tooltip("テストクリックの間隔(秒)。")]
        public float clickIntervalSec = 0.6f;
        [Tooltip("IRの最大長(ms)。これを超える遅延のタップは切り捨て。")]
        public float maxIrMs = 120f;
        [Tooltip("IR(タップ)を再構築する間隔(秒)。")]
        public float irUpdateSec = 0.05f;
        [Tooltip("全体出力ゲイン。")]
        [Range(0f, 4f)] public float outputGain = 0.6f;

        private struct ConvTap { public int delaySamples; public float gain; }

        private ConvTap[] _ir = new ConvTap[0];  // audio-thread が読む（参照swapで受け渡し）
        private float[] _ring;                    // dry入力の履歴（リングバッファ）
        private int _ringMask;
        private int _writePos;
        private int _sampleRate;
        private double _clickTimer;
        private float _irTimer;
        private AudioSource _src;

        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            int need = Mathf.CeilToInt(maxIrMs * 0.001f * _sampleRate) + 1024;
            int ringSize = Mathf.NextPowerOfTwo(need);
            _ring = new float[ringSize];
            _ringMask = ringSize - 1;

            _src = GetComponent<AudioSource>();
            if (_src.clip == null)
            {
                // 無音の1秒ループを作ってコールバックを回す（テスト信号は内部生成）。
                _src.clip = AudioClip.Create("ir_silence", _sampleRate, 1, _sampleRate, false);
            }
            _src.loop = true;
            _src.spatialBlend = 0f;   // 2D（空間化はIR側。段1はモノ）
            _src.playOnAwake = false;
            _src.Play();
        }

        private void Update()
        {
            _irTimer += Time.deltaTime;
            if (_irTimer >= irUpdateSec)
            {
                _irTimer = 0f;
                RebuildIr();
            }
        }

        // 主音源のタップ（Status）を audio-thread 用の {遅延サンプル・ゲイン} 配列に変換して参照swap。
        private void RebuildIr()
        {
            var d = AcousticFlowSceneDemo.Status.TapDelayMs;
            var g = AcousticFlowSceneDemo.Status.TapGain;
            int tc = AcousticFlowSceneDemo.Status.TapCount;
            if (d == null || g == null || tc <= 0) { _ir = new ConvTap[0]; return; }

            int maxDelay = Mathf.CeilToInt(maxIrMs * 0.001f * _sampleRate);
            var ir = new ConvTap[tc];
            int n = 0;
            for (int i = 0; i < tc && i < d.Length && i < g.Length; i++)
            {
                int ds = Mathf.RoundToInt(d[i] * 0.001f * _sampleRate);
                if (ds < 0) ds = 0;
                if (ds > maxDelay) continue;     // IR長を超えるタップは捨てる
                ir[n].delaySamples = ds;
                ir[n].gain = g[i];
                n++;
            }
            if (n != ir.Length) System.Array.Resize(ref ir, n);
            _ir = ir;   // 参照swap（次コールバックから新IRを見る。段1はクロスフェード無し＝切替でプチる場合あり）
        }

        // audio-thread：マルチタップ・ディレイ（= 離散IRの畳み込み）。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            var ir = _ir;                       // ローカルに固定（swap耐性）
            int frames = data.Length / channels;
            double clickPeriod = clickIntervalSec * _sampleRate;

            for (int f = 0; f < frames; f++)
            {
                // dry入力：テスト信号（インパルス列）or 実クリップ
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

                _ring[_writePos & _ringMask] = dry;

                float outv = 0f;
                if (ir != null)
                    for (int k = 0; k < ir.Length; k++)
                        outv += ir[k].gain * _ring[(_writePos - ir[k].delaySamples) & _ringMask];
                outv *= outputGain;

                for (int c = 0; c < channels; c++) data[f * channels + c] = outv;  // 段1はモノ→全ch同値
                _writePos++;
            }
        }
    }
}
