// VoiceConvolver.cs
// **C++ エンジン側の DSP（段4）で鳴らす**再生コンポーネント。
//
// IrConvolver.cs との違い:
//   IrConvolver  … 畳み込み・HRTF・後期尾を **Unity C# 側**で回す（従来）
//   VoiceConvolver … 同じことを **エンジン(DLL)側**で回す（AF_Voice* API）
//
// 両方をシーンに置いて A/B できるようにしてある。移行が正しければ「同じ音」が出るはずで、
// 違って聞こえたら移行のどこかが失敗している、という判定に使える。
//
// ここがやることは配線だけ:
//   ・AcousticFlowSceneDemo が毎フレーム作るタップ（遅延・6帯域ゲイン・パン）を C 構造体へ詰める
//   ・エコグラムを渡して後期尾を組み直させる
//   ・OnAudioFilterRead で AF_VoiceRender を呼ぶ
// 音の処理そのものは一切書かない。
using System;
using UnityEngine;

namespace AcousticFlow
{
    [RequireComponent(typeof(AudioSource))]
    public class VoiceConvolver : MonoBehaviour
    {
        [Header("対象")]
        [Tooltip("AcousticFlowSceneDemo.Status.Taps[] のどれを鳴らすか。IrConvolver と同じ意味。")]
        public int sourceIndex = 0;

        [Header("音量")]
        [Range(0f, 4f)] public float outputGain = 0.6f;
        [Tooltip("反射/回折タップの音量倍率（直接音は等倍）。IrConvolver の reflectionLevel と同じ。")]
        [Range(0f, 1f)] public float reflectionLevel = 0.35f;
        [Tooltip("後期尾の音量倍率。1.0 が物理どおり。")]
        [Range(0f, 2f)] public float tailLevel = 1.0f;

        [Header("HRTF")]
        public bool enableHrtf = true;
        [Tooltip("頭囲(cm)。ITD は頭のサイズにほぼ比例するので、ここが個人最適化で最も効く。")]
        [Range(48f, 64f)] public float headCircumferenceCm = 57f;
        [Tooltip("空なら合成HRTF（球体頭）。StreamingAssets 内の .afhr 名を入れると実測データを使う。")]
        public string hrtfFileName = "";

        [Header("散乱・尾")]
        [Range(0f, 1f)] public float scatterAmount = 0.5f;
        [Range(0f, 0.8f)] public float scatterDiffusion = 0.62f;
        [Range(0.2f, 3f)] public float tailSeconds = 1.0f;
        [Tooltip("尾を組み直す間隔（フレーム）。重いので数フレームに1回で十分（部屋は緩変）。")]
        public int tailRebuildEveryFrames = 8;

        [Header("診断（読み取り専用）")]
        public int tapCount;
        public int tailPartitions;
        public int tailLatencySamples;
        public float rmsDirect, rmsEarly, rmsScatter, rmsTail, rmsOut;

        private IntPtr _voice = IntPtr.Zero;
        private IntPtr _hrtf = IntPtr.Zero;
        private int _sampleRate;
        private bool _ready;

        // audio thread の作業配列。確保はここでだけ行う。
        private float[] _dry, _outL, _outR;
        private Native.AFVoiceTap[] _taps;
        private float[] _echo;
        private int _frameCounter;

        // メインスレッドが書き、audio thread が読む（値のコピーだけなのでロックしない）。
        private volatile float _wet = 1f, _srcLevel = 1f;

        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            AudioSettings.GetDSPBufferSize(out int bufLen, out _);

            var cfg = new Native.AFVoiceConfig
            {
                sampleRate = _sampleRate,
                maxFrames = Mathf.Max(256, bufLen),
                tailSeconds = tailSeconds,
                tapCrossfadeMs = 30f,
                hrtfCrossfadeMs = 12f,
                hrtfCrossoverHz = 700f,
                tailFirstBlock = 64,
                tailCapBlock = 8192,
            };
            _voice = Native.AF_VoiceCreate(ref cfg);
            if (_voice == IntPtr.Zero) { Debug.LogError("[VoiceConvolver] 音源を作れませんでした"); return; }

            _hrtf = LoadHrtf();
            Native.AF_VoiceSetHrtf(_voice, _hrtf);

            _dry = new float[cfg.maxFrames];
            _outL = new float[cfg.maxFrames];
            _outR = new float[cfg.maxFrames];
            _taps = new Native.AFVoiceTap[AcousticFlowSceneDemo.SourceTaps.MaxTaps];
            _echo = new float[256 * AcousticEngine.NumBands];
            _ready = true;
        }

        private IntPtr LoadHrtf()
        {
            if (!string.IsNullOrEmpty(hrtfFileName))
            {
                string path = System.IO.Path.Combine(Application.streamingAssetsPath, hrtfFileName);
                if (System.IO.File.Exists(path))
                {
                    IntPtr h = Native.AF_HrtfLoadFile(path);
                    if (h != IntPtr.Zero)
                    {
                        Debug.Log($"[VoiceConvolver] HRTF 読み込み: {hrtfFileName} "
                                  + $"({Native.AF_HrtfDirectionCount(h)} 方向)");
                        return h;
                    }
                    Debug.LogWarning($"[VoiceConvolver] HRTF 読めず、合成へ falls back: {path}");
                }
            }
            return Native.AF_HrtfCreateSynthetic(_sampleRate);
        }

        private void OnDestroy()
        {
            _ready = false;
            if (_voice != IntPtr.Zero) { Native.AF_VoiceDestroy(_voice); _voice = IntPtr.Zero; }
            if (_hrtf != IntPtr.Zero) { Native.AF_HrtfDestroy(_hrtf); _hrtf = IntPtr.Zero; }
        }

        private AcousticFlowSceneDemo.SourceTaps MyTaps()
        {
            var all = AcousticFlowSceneDemo.Status.Taps;
            if (all == null || sourceIndex < 0 || sourceIndex >= all.Length) return null;
            return all[sourceIndex];
        }

        // メインスレッド：タップと尾をエンジンへ渡す。
        private void Update()
        {
            if (!_ready) return;
            var ts = MyTaps();
            if (ts == null) return;

            Native.AF_VoiceSetOutputGain(_voice, outputGain);
            Native.AF_VoiceSetTailLevel(_voice, tailLevel);
            Native.AF_VoiceSetScatterDiffusion(_voice, scatterDiffusion);
            Native.AF_VoiceSetHrtfEnabled(_voice, enableHrtf ? 1 : 0);

            // 到来方向（遮蔽時は回折で回り込む方向になる）。
            Vector3 d = ts.DirectDirLocal;
            Native.AF_VoiceSetDirection(_voice, new AFVector3(d), headCircumferenceCm);

            // 尾の掛かり方。開けた場所は wet が小さく、壁裏では srcLevel が小さくなる。
            _wet = Mathf.Clamp01(AcousticFlowSceneDemo.Status.Wet);
            _srcLevel = (ts.SourceLevel > 0f) ? ts.SourceLevel : 1f;
            Native.AF_VoiceSetTailEnvelope(_voice, _wet, _srcLevel);

            // ── タップを詰める ──
            //   index 0 は直接音。以降は反射/回折で、音量を reflectionLevel で絞る。
            //   散乱率は遅延とともに上げる（高次反射ほど拡散へ溶ける）。正規化には
            //   **頭打ち前の真の mixing time** を使うこと（畳み込みの都合で下限を掛けた値だと
            //   狭い部屋で実際の4倍以上に膨らみ、散乱が効かなくなる）。
            float mix = AcousticFlowSceneDemo.Status.MixingTimeMs;
            if (mix <= 0f) mix = 25f;
            int n = Mathf.Min(ts.Count, _taps.Length);
            int nb = AcousticEngine.NumBands;
            for (int i = 0; i < n; i++)
            {
                int o = i * nb;
                float rl = (i == 0) ? 1f : reflectionLevel;
                var t = new Native.AFVoiceTap
                {
                    delaySamples = Mathf.Max(0, Mathf.RoundToInt(ts.DelayMs[i] * 0.001f * _sampleRate)),
                    g0 = ts.BandGain[o + 0] * rl, g1 = ts.BandGain[o + 1] * rl,
                    g2 = ts.BandGain[o + 2] * rl, g3 = ts.BandGain[o + 3] * rl,
                    g4 = ts.BandGain[o + 4] * rl, g5 = ts.BandGain[o + 5] * rl,
                    panL = ts.PanL[i], panR = ts.PanR[i],
                };
                if (i == 0) { t.gSpec = 1f; t.gDiff = 0f; }   // 直接音は滲ませない
                else
                    Native.AF_VoiceScatterSplit(
                        ts.DelayMs[i], mix, scatterAmount, 1f, out t.gSpec, out t.gDiff);
                _taps[i] = t;
            }
            Native.AF_VoiceSetTaps(_voice, _taps, n);
            tapCount = n;

            // ── 後期尾。重いので数フレームに1回 ──
            if (++_frameCounter >= Mathf.Max(1, tailRebuildEveryFrames))
            {
                _frameCounter = 0;
                var scene = AcousticFlowSceneDemo.SharedScene;
                if (scene != null && scene.IsValid)
                {
                    int bins = scene.GetEchogramBands(_echo, _echo.Length / nb);
                    if (bins > 0)
                    {
                        // 直接音の広帯域ゲイン＝絶対レベルの基準。
                        float dg = 0f;
                        for (int b = 0; b < nb; b++) dg += ts.BandGain[b];
                        dg /= nb;
                        Native.AF_VoiceRebuildTail(
                            _voice, _echo, bins, AcousticFlowSceneDemo.Status.EchogramBinMs,
                            Mathf.Max(10f, AcousticFlowSceneDemo.Status.MixingTimeMs), 8f,
                            30f, 0f, 0.6f, dg,
                            AcousticFlowSceneDemo.Status.ReverbTargetRatio, null, 0);
                        tailPartitions = Native.AF_VoiceTailPartitions(_voice);
                        tailLatencySamples = Native.AF_VoiceTailLatency(_voice);
                    }
                }
            }
        }

        // audio thread：入力をモノラルに落として AF_VoiceRender へ。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (!_ready || _voice == IntPtr.Zero) { Array.Clear(data, 0, data.Length); return; }
            int frames = data.Length / channels;
            if (frames > _dry.Length) { Array.Clear(data, 0, data.Length); return; }

            for (int f = 0; f < frames; f++)
            {
                float s = 0f;
                for (int c = 0; c < channels; c++) s += data[f * channels + c];
                _dry[f] = s / channels;
            }

            var m = new Native.AFVoiceMetering();
            Native.AF_VoiceRender(_voice, _dry, frames, _outL, _outR, ref m);
            rmsDirect = m.rmsDirect; rmsEarly = m.rmsEarly; rmsScatter = m.rmsScatter;
            rmsTail = m.rmsTail; rmsOut = m.rmsOut;

            // ★HUD と Output Scope が読む段別メーターは IrConvolver.Scope の static。
            //   ここへも書かないと、C++ 経路へ切り替えた瞬間に数値が固まって
            //   「切り替えたのに何も変わらない」ように見える。**同じ計器で A/B する**ために
            //   平滑係数も IrConvolver と同じ 0.3 に揃える。
            const float k = 0.3f;
            IrConvolver.Scope.SampleRate = _sampleRate;
            IrConvolver.Scope.TailPartitions = tailPartitions;
            IrConvolver.Scope.RmsDirect  = Mathf.Lerp(IrConvolver.Scope.RmsDirect,  m.rmsDirect,  k);
            IrConvolver.Scope.RmsEarly   = Mathf.Lerp(IrConvolver.Scope.RmsEarly,   m.rmsEarly,   k);
            IrConvolver.Scope.RmsScatter = Mathf.Lerp(IrConvolver.Scope.RmsScatter, m.rmsScatter, k);
            IrConvolver.Scope.RmsTail    = Mathf.Lerp(IrConvolver.Scope.RmsTail,    m.rmsTail,    k);
            IrConvolver.Scope.RmsOut     = Mathf.Lerp(IrConvolver.Scope.RmsOut,     m.rmsOut,     k);

            for (int f = 0; f < frames; f++)
            {
                int b = f * channels;
                if (channels >= 2)
                {
                    data[b] = _outL[f]; data[b + 1] = _outR[f];
                    for (int c = 2; c < channels; c++) data[b + c] = (_outL[f] + _outR[f]) * 0.5f;
                }
                else data[b] = (_outL[f] + _outR[f]) * 0.5f;
            }
        }
    }
}
