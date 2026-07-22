// IrConvolver.cs
// IR畳み込みPoC（Unity C#・Wwise非経由）。AcousticFlowSceneDemo.Status のIRタップを畳み込む。
//   早期反射 = 離散タップのマルチタップ畳み込み（6帯域分割・散乱スメア・到来方向パン・距離減衰・IR切替クロスフェード）。
//   後期残響 = tailMode で選ぶ。
//     Measured … 実測エコグラム(帯域別)をそのまま尾のIRにして畳み込む（本命）。
//                「フルIRでの計測」というシステムのコンセプトどおり、尾も合成ではなく計測。
//     Fdn      … RT60スカラから合成する従来方式（比較用に残置）。
//                16本の遅延線＋Hadamard帰還＋HFダンピング。
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
        public enum TailMode { Off, Fdn, Measured }

        /// 畳み込み後の実出力を覗くための窓（Output Scope が読む）。
        /// Wwise の出力レベル(AcousticEngine_GetOutputLevels)は畳み込み経路を通らないので
        /// 何も映らない。ここは audio thread が書いた「実際に鳴っている波形」そのもの。
        ///
        /// audio thread が書き、Editor が読む。ロックはしない（表示用なので多少のズレは無害）。
        public static class Scope
        {
            public const int BufferLength = 32768;   // 48kHz で約 0.68 秒
            public static readonly float[] L = new float[BufferLength];
            public static readonly float[] R = new float[BufferLength];
            public static volatile int WritePos;
            public static volatile int TriggerPos = -1;   // テストクリックを出した位置（同期表示用）
            public static volatile int SampleRate;

            // 段ごとの RMS（どこが鳴っていて、どこが鳴っていないかの切り分け用）。
            public static volatile float RmsDirect;    // 直接タップ
            public static volatile float RmsEarly;     // 早期反射の鏡面成分
            public static volatile float RmsScatter;   // 散乱スメアの拡散成分
            public static volatile float RmsTail;      // 後期尾（Measured / Fdn どちらでも）
            public static volatile float RmsOut;       // 最終出力
            public static volatile int TailPartitions; // 実測尾のパーティション数（0=尾IR未設定）
            public static volatile float TailToDirectRatio; // エコグラム実測の 尾/直接 エネルギー比
            public static volatile float SplitMs;      // 早期↔後期の境目(ms)。部屋の大きさで動く
            public static volatile int ActiveParts;    // 実際に計算しているパーティション数
        }

        [Tooltip("ON: 内部テスト信号を畳み込む（ドライ素材不要）。OFF: このAudioSourceのクリップを畳み込む。")]
        public bool generateTestSignal = true;
        [Tooltip("ON: テスト信号を連続ノイズに（途切れ確認用）。OFF: クリック列。")]
        public bool testContinuousNoise = false;
        [Tooltip("テストクリックの間隔(秒)。")]
        public float clickIntervalSec = 0.6f;
        [Tooltip("早期反射IRの最大長(ms)＝方向つき早期タップを保持できる上限。"
                 + "広い部屋は最初の反射が遅く届く（壁が遠い）ので、ここが短いと早期反射が"
                 + "丸ごと尾に落ちて消える。mixing time(√V) がここまで伸びられる。")]
        public float maxIrMs = 400f;
        [Tooltip("IR(タップ)を再構築する間隔(秒)。")]
        public float irUpdateSec = 0.05f;
        [Tooltip("全体出力ゲイン。")]
        [Range(0f, 4f)] public float outputGain = 0.6f;
        [Tooltip("IR切替のクロスフェード長(ms)。壁変化などでIRが変わる時のプチ音を消す。")]
        [Range(2f, 100f)] public float crossfadeMs = 30f;
        [Tooltip("反射/回折タップの音量倍率（直接音=1固定）。0=直接のみ。"
                 + "散乱スメア(enableScatter)が効いていれば櫛が立たないので、1.0 に近づけられる。")]
        [Range(0f, 1f)] public float reflectionLevel = 0.35f;

        [Header("反射の散乱スメア")]
        [Tooltip("ON: 反射タップを『鏡面デルタ + 拡散バースト』に分ける。"
                 + "粗い面の反射は時間方向にも滲むので、デルタのままだと持続音に櫛(コムフィルタ)が立つ。")]
        public bool enableScatter = true;
        [Tooltip("散乱率 s(0..1)。0=完全鏡面(従来どおり全部デルタ) / 1=完全拡散。"
                 + "本来は材質の scattering[6] をタップ単位でエンジンから受け取るべき値。"
                 + "まず耳で効果を確かめるための暫定グローバル値。")]
        [Range(0f, 1f)] public float scatterAmount = 0.5f;
        [Tooltip("拡散バーストの撹拌の強さ(allpass係数)。0=滲まない / 0.5〜0.7が定番。")]
        [Range(0f, 0.8f)] public float scatterDiffusion = 0.62f;

        [Header("後期残響尾")]
        [Tooltip("後期尾の作り方。Measured=実測エコグラムをそのままIRにして畳み込む（本命）。"
                 + "Fdn=RT60スカラから合成する従来方式（比較用）。Off=尾なし。")]
        public TailMode tailMode = TailMode.Measured;
        [Tooltip("実測尾の長さ(秒)。エコグラム窓(ビン数×ビン長)を超える分は無音になる。")]
        [Range(0.2f, 3f)] public float measuredTailSec = 1.0f;
        [Tooltip("実測尾の畳み込みブロック長(サンプル)。大きいほど軽いが、"
                 + "尾の最早開始(minSplitMs)より長い遅延になると尾が遅れて聞こえる。")]
        public int measuredBlockSize = 2048;
        // 早期↔後期の境目は Status.MixingTimeMs(≈√V) を使い、下は minSplitMs・上は maxIrMs で挟む。
        [Tooltip("境目の下限(ms)。畳み込みのブロック遅延をここに隠すので、"
                 + "小さくするほどブロックが小さくなり負荷が上がる。")]
        [Range(10f, 120f)] public float minSplitMs = 25f;
        [Tooltip("尾の包絡の平滑幅(ms)。0=平滑なし。"
                 + "レイが有限本数なのでエコグラムには平均自由行程ごとの塊が残り、"
                 + "そのままだと『なめらかな尾』でなく『山彦』に聞こえる。減衰カーブ自体は保たれる。")]
        [Range(0f, 200f)] public float tailSmoothMs = 30f;
        [Tooltip("平滑幅を時刻に比例して広げる割合。後期ほどレイの寄与が減って推定の分散が大きく、"
                 + "固定幅だと後半だけガタついて『残響の粒立ち』として聞こえる。"
                 + "広い部屋ほど効く。0 で固定幅（従来）。")]
        // ※ 平滑は「測定をぼかす」対処なので、広さの手がかり（尾の立ち上がりの形など）も一緒に消す。
        //    粒立ちの根本原因はレイのバウンス数不足なので、まず echogramBounces を上げること。
        //    ここは既定 0＝無効。上げるのは最後の手段。
        [Range(0f, 0.6f)] public float tailSmoothGrowth = 0f;
        [Tooltip("包絡を更新のたびに新しい測定へ寄せる割合。小さいほど時間平均が強い。"
                 + "時間平均は『分散を下げる』と『IR切替の段差を減らす』の両方に効くので、"
                 + "空間平滑(上2つ)を弱めたままで粒立ちを抑えられる＝減衰カーブの情報が残る。"
                 + "1.0 で平均なし。小さすぎると部屋の変化への追従が遅れる。")]
        [Range(0.05f, 1f)] public float tailEnvSmoothing = 0.6f;

        [Tooltip("ON: FDNで合成した後期残響尾を足す（RT60はエコグラム由来）。tailMode=Fdn のとき使う。")]
        public bool enableReverbTail = true;
        [Tooltip("後期尾の音量倍率。Measured では絶対レベルを物理式 (r/r_c)² から決めるので"
                 + " 1.0 が物理どおり（部屋間で一貫）。乾かしたい/湿らせたいときだけ全部屋一律に振る。"
                 + " Fdn では従来どおり wet に乗算する係数。")]
        [Range(0f, 2f)] public float tailLevel = 1.0f;
        [Tooltip("後期尾の高域ダンピング。0=明るい / 1=暗い（高域が先に減る）。")]
        [Range(0f, 1f)] public float tailDamping = 0.25f;
        [Tooltip("残響に入れる前の低域カット(Hz)。低域モードのボワつき/びりびりを抑える。直接音の低音は残る。0で無効。")]
        [Range(0f, 400f)] public float tailLowCutHz = 150f;
        [Tooltip("残響入力の拡散（allpass）。粒感/金属感を消して滑らかに。0=無効 / 0.5〜0.7が定番。")]
        [Range(0f, 0.8f)] public float tailDiffusion = 0.62f;

        // エンジン出力の6帯域(125/250/500/1k/2k/4kHz)に対応する分割。
        // クロスオーバー周波数は隣接帯域の幾何平均（オクターブバンドの境目）。
        private const int kNumBands = 6;
        private static readonly float[] kCrossHz = { 177f, 354f, 707f, 1414f, 2828f };
        private const int kFdnN = 16;  // 遅延線数。多いほどモード密度が上がり滑らか（4=メタリック / 8 / 16）。
        private const int kApN = 4;    // 入力ディフュージョンの allpass 段数

        // g0..g5 = 6帯域それぞれのゲイン（配列にすると毎IR構築で確保が要るため展開して持つ）。
        // gSpec/gDiff = 散乱率 s の鏡面/拡散への振り分け係数（√(1-s) / √s）。
        // audio thread で毎サンプル Sqrt を呼ばないよう、IR 構築時に畳んでおく。
        private struct ConvTap
        {
            public int delaySamples;
            public float g0, g1, g2, g3, g4, g5;
            public float panL, panR, gSpec, gDiff;
        }

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
        private float[][] _ring;          // [帯域][時間] の入力履歴
        private int _ringMask;
        private int _writePos;
        private Biquad[] _split;          // 直列クロスオーバー用の lowpass 群（kNumBands-1 個）
        private float[] _bandTmp;         // 1サンプル分の帯域分割結果

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

        // 早期反射の散乱スメア用 allpass。FDN 入力用(_ap*)とは別物：
        //   こちらは「反射タップの拡散成分」を入力とし、後期尾より短い時定数で滲ませる。
        //   L/R で長さの違う2系統を通し、拡散音場が中央に張り付かないようにする。
        private float[][] _scatBufL, _scatBufR;
        private int[] _scatLenL, _scatLenR, _scatPosL, _scatPosR;

        // 実測エコグラム由来の後期尾（partitioned convolution）
        private PartitionedConvolver _tailConv;
        private ReverbTailIr _tailIr;
        private int _tailEchoVersion = -1;          // 最後に取り込んだエコグラムの版
        private volatile float _tailGain;           // D/R比から決めた尾の絶対ゲイン
        private float _splitMs = 120f;              // 早期↔後期の境目（RT60から毎回決める）
        private float[] _tailInMono;                // 畳み込み入力（モノラル dry）
        private float[][] _tailOut;                 // 畳み込み出力 [ch][frames]
        private int _tailScratchFrames;

        // Awake が終わるまで audio thread を走らせないためのフラグ。
        //   OnAudioFilterRead は audio thread から呼ばれるので、Awake より先に来ることがある
        //   （AudioSource の playOnAwake がシーン側で有効なら特に）。バッファ確保前に触ると
        //   NullReference で落ちるため、準備できるまでは無音を返す。
        private volatile bool _ready;

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
            _ring = new float[kNumBands][];
            for (int b = 0; b < kNumBands; b++) _ring[b] = new float[ringSize];
            _ringMask = ringSize - 1;
            _xfadeLen = Mathf.Max(1, Mathf.RoundToInt(crossfadeMs * 0.001f * _sampleRate));

            // 直列クロスオーバー：低い方から順に「LPで抜き出して残りから引く」を繰り返す。
            // 各段の帯域を足すと必ず元信号に戻る（＝全帯域同ゲインなら完全再構成）。
            _split = new Biquad[kNumBands - 1];
            for (int i = 0; i < _split.Length; i++) _split[i].SetLowpass(kCrossHz[i], _sampleRate);
            _bandTmp = new float[kNumBands];

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

            // 散乱スメア：早期反射用なので FDN 入力用より短い（数ms〜十数ms）。
            // L/R で互いに素っぽい別の長さにして、拡散成分を左右で相関させない。
            float[] scatMsL = { 3.7f, 6.1f, 9.7f };
            float[] scatMsR = { 4.3f, 7.3f, 11.3f };
            AllocAllpass(scatMsL, out _scatBufL, out _scatLenL, out _scatPosL);
            AllocAllpass(scatMsR, out _scatBufR, out _scatLenR, out _scatPosR);

            // 実測尾：ブロック長は「尾の立ち上がりより短い」ことが条件（固有遅延を尾の中に隠す）。
            int tailLen = Mathf.CeilToInt(measuredTailSec * _sampleRate);
            int blk = Mathf.NextPowerOfTwo(Mathf.Max(64, measuredBlockSize));
            // 遅延を隠せるのは「尾が最も早く始まるとき」＝ minSplitMs まで。
            int maxBlk = Mathf.NextPowerOfTwo(Mathf.Max(64, Mathf.FloorToInt(minSplitMs * 0.001f * _sampleRate))) / 2;
            if (blk > maxBlk)
            {
                Debug.LogWarning($"[IrConvolver] measuredBlockSize={blk} は尾の最早開始({minSplitMs}ms)より" +
                                 $"長い遅延になるため {maxBlk} に下げました。");
                blk = Mathf.Max(64, maxBlk);
            }
            int parts = Mathf.Max(1, Mathf.CeilToInt((float)tailLen / blk));
            _tailConv = new PartitionedConvolver(blk, parts, 2);
            _tailIr = new ReverbTailIr(_sampleRate, _tailConv.TailSamples, 2);

            // ここまでで audio thread が触る配列は全て確保済み。以降 OnAudioFilterRead を通す。
            _ready = true;

            _src = GetComponent<AudioSource>();
            if (_src.clip == null)
                _src.clip = AudioClip.Create("ir_silence", _sampleRate, 1, _sampleRate, false);
            _src.loop = true;
            _src.spatialBlend = 0f;   // 2D（空間化はIR側）
            _src.playOnAwake = false;
            _src.Play();
        }

        // ms 配列から allpass の遅延バッファ一式を確保する。
        private void AllocAllpass(float[] ms, out float[][] buf, out int[] len, out int[] pos)
        {
            buf = new float[ms.Length][];
            len = new int[ms.Length];
            pos = new int[ms.Length];
            for (int i = 0; i < ms.Length; i++)
            {
                len[i] = Mathf.Max(1, Mathf.RoundToInt(ms[i] * 0.001f * _sampleRate));
                buf[i] = new float[len[i]];
            }
        }

        // Schroeder allpass 1段。位相だけ撹拌して振幅特性は平坦（＝音色を変えずに滲ませる）。
        private static float Allpass(float x, float[][] buf, int[] len, int[] pos, float g)
        {
            for (int i = 0; i < buf.Length; i++)
            {
                float bo = buf[i][pos[i]];
                float di = x + g * bo;
                buf[i][pos[i]] = di;
                x = bo - g * di;
                if (++pos[i] >= len[i]) pos[i] = 0;
            }
            return x;
        }

        private void Update()
        {
            _irTimer += Time.deltaTime;
            if (_irTimer >= irUpdateSec)
            {
                _irTimer = 0f;
                // 早期↔後期の境目を実測RT60から決める。ブロック遅延を隠すため下限は minSplitMs。
                //   この値が動くと「早期タップの打ち切り」と「尾の開始」が同時にずれる。
                //   早期側はクロスフェードされるが尾側はブロック境界でハードスワップなので、
                //   急に動くと音が段差として聞こえる。ゆっくり追従させる。
                //   境目は mixing time ≈ √V（部屋の広さで決まる物理量）。RT60×係数ではなく
                //   これを使うことで、特大部屋で遠壁の反射(100〜300ms)が早期窓から漏れなくなる。
                //   早期窓の上限は ring バッファ長 maxIrMs なので、そこで頭打ちにする。
                float mix = AcousticFlowSceneDemo.Status.MixingTimeMs;
                if (mix <= 0f) mix = 120f;   // 未計算時のフォールバック
                float targetSplit = Mathf.Clamp(mix, minSplitMs, maxIrMs);
                _splitMs = Mathf.Lerp(_splitMs, targetSplit, 0.15f);
                RebuildIr();
                UpdateFdnParams();
                RebuildTailIr();
            }
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

        // 実測エコグラム（帯域別）から後期尾の IR を作り直す（main thread）。
        // エコグラムが更新されたときだけ作る。尾は緩変なので毎回作る必要はない。
        private void RebuildTailIr()
        {
            if (tailMode != TailMode.Measured || _tailConv == null || _tailIr == null) return;
            var bands = AcousticFlowSceneDemo.Status.EchogramBands;
            if (bands == null) return;   // 古いDLL＝帯域別が取れない。Fdn へフォールバックすべき状態。
            int ver = AcousticFlowSceneDemo.Status.EchogramVersion;
            if (ver == _tailEchoVersion) return;
            _tailEchoVersion = ver;

            float ratio = _tailIr.Build(bands,
                                        AcousticFlowSceneDemo.Status.EchogramBinCount,
                                        AcousticFlowSceneDemo.Status.EchogramBinMs,
                                        _splitMs,
                                        20f,
                                        tailSmoothMs,
                                        tailSmoothGrowth,
                                        tailEnvSmoothing);
            if (ratio <= 0f) { _tailGain = 0f; return; }   // 尾のエネルギーが無い＝尾なし

            // 尾の絶対レベルは「残響/直接の物理目標比 (r/r_c)²」から決める。
            //   ・エコグラムの尾/直接比(ratio)は 2π 結合で信用できないので“量”には使わない
            //     （形＝時間包絡と帯域減衰にだけ使う）。
            //   ・直接音タップの広帯域ゲイン dg = 距離減衰・透過を含む絶対レベルの基準。
            //   尾IRはエネルギー1に正規化済みなので、tailGain = dg × √target で
            //   出力の 尾/直接 パワー比がちょうど target になる。
            var bg = AcousticFlowSceneDemo.Status.TapBandGain;
            float dg = 0f;
            if (bg != null && bg.Length >= kNumBands)
            {
                for (int b = 0; b < kNumBands; b++) dg += bg[b] * bg[b];
                dg = Mathf.Sqrt(dg / kNumBands);
            }
            float target = AcousticFlowSceneDemo.Status.ReverbTargetRatio;
            _tailGain = ReverbTailIr.CalibrateGain(dg, target);
            Scope.TailToDirectRatio = target;   // スコープには使った目標比を出す
            _tailConv.SetIr(_tailIr.Ir);
        }

        // 早期反射のタップ（Status）を {遅延サンプル・6帯域ゲイン・パン} に変換して参照swap。
        private void RebuildIr()
        {
            var d = AcousticFlowSceneDemo.Status.TapDelayMs;
            var bg = AcousticFlowSceneDemo.Status.TapBandGain;
            var bb = AcousticFlowSceneDemo.Status.TapGain;
            var pl = AcousticFlowSceneDemo.Status.TapPanL;
            var pr = AcousticFlowSceneDemo.Status.TapPanR;
            int tc = AcousticFlowSceneDemo.Status.TapCount;
            if (d == null || tc <= 0) return;

            // 打ち切りは「早期↔後期の境目」。ここから先は尾の畳み込みが担当するので、
            // 早期タップが越境すると二重計上になる。
            int maxDelay = Mathf.CeilToInt(Mathf.Min(_splitMs, maxIrMs) * 0.001f * _sampleRate);
            var ir = new ConvTap[tc];
            int n = 0;
            for (int i = 0; i < tc && i < d.Length; i++)
            {
                int ds = Mathf.RoundToInt(d[i] * 0.001f * _sampleRate);
                if (ds < 0) ds = 0;
                if (ds > maxDelay) continue;
                ir[n].delaySamples = ds;
                float rl = (i == 0) ? 1f : reflectionLevel;      // 反射/回折(i>0)は音量を絞る
                if (bg != null && i * kNumBands + (kNumBands - 1) < bg.Length)
                {
                    int o = i * kNumBands;
                    ir[n].g0 = bg[o + 0] * rl; ir[n].g1 = bg[o + 1] * rl; ir[n].g2 = bg[o + 2] * rl;
                    ir[n].g3 = bg[o + 3] * rl; ir[n].g4 = bg[o + 4] * rl; ir[n].g5 = bg[o + 5] * rl;
                }
                else
                {
                    float g = ((bb != null && i < bb.Length) ? bb[i] : 0f) * rl;
                    ir[n].g0 = g; ir[n].g1 = g; ir[n].g2 = g;
                    ir[n].g3 = g; ir[n].g4 = g; ir[n].g5 = g;
                }
                ir[n].panL = (pl != null && i < pl.Length) ? pl[i] : 0.70710678f;
                ir[n].panR = (pr != null && i < pr.Length) ? pr[i] : 0.70710678f;
                // 直接音は滲ませない。反射/回折だけ散乱成分を持つ。
                // ※本来はタップごとに当たった面の材質 scattering[6] を使うべき値。
                float sc = (i == 0 || !enableScatter) ? 0f : Mathf.Clamp01(scatterAmount);
                ir[n].gSpec = Mathf.Sqrt(1f - sc);
                ir[n].gDiff = Mathf.Sqrt(sc);
                n++;
            }
            if (n != ir.Length) System.Array.Resize(ref ir, n);
            _pendingIr = ir;
        }

        // audio thread の作業配列を確保する。DSPバッファ長は起動後に変わりうるので毎回見る
        // （通常は初回だけ確保され、以降は素通り）。
        private void EnsureTailScratch(int frames)
        {
            if (_tailScratchFrames >= frames) return;
            _tailInMono = new float[frames];
            _tailOut = new float[2][];
            _tailOut[0] = new float[frames];
            _tailOut[1] = new float[frames];
            _tailScratchFrames = frames;
        }

        // audio-thread：早期反射(マルチタップ畳み込み) ＋ 後期尾(実測IR畳み込み or FDN) → ステレオ出力。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            // Awake 前に audio thread が来ることがある。準備前は無音を返す（掴んだ素材を素通しさせない）。
            if (!_ready) { System.Array.Clear(data, 0, data.Length); return; }

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
            bool useFdn = (tailMode == TailMode.Fdn) && enableReverbTail;
            float fdnWet = useFdn ? tailLevel * wet * srcLv : 0f;

            EnsureTailScratch(frames);

            // スコープ用の集計（このブロック分の二乗和）。
            float sumDirect = 0f, sumEarly = 0f, sumScatter = 0f, sumTail = 0f, sumOut = 0f;
            int scopeBase = Scope.WritePos;

            // ① dry を1ブロック分先に取り出す。
            //    後期尾の畳み込みはブロック単位で回すので、サンプルごとの処理より前に dry が要る。
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
                        if (_clickTimer >= clickPeriod)
                        {
                            _clickTimer -= clickPeriod; dry = 1f;
                            // スコープをクリックに同期させるための目印。
                            Scope.TriggerPos = (Scope.WritePos + f) % Scope.BufferLength;
                        }
                        else dry = 0f;
                    }
                }
                else
                {
                    float s = 0f;
                    for (int c = 0; c < channels; c++) s += data[f * channels + c];
                    dry = s / channels;
                }
                _tailInMono[f] = dry;
            }

            // ② 実測尾を畳み込む（周波数領域・ブロック単位）。
            //    尾IRはエネルギー1に正規化済みで、絶対レベルは _tailGain（実測 D/R 比由来）が持つ。
            //    tailLevel は好みの微調整（1.0 が物理どおり）。
            System.Array.Clear(_tailOut[0], 0, frames);
            System.Array.Clear(_tailOut[1], 0, frames);
            if (tailMode == TailMode.Measured && _tailConv != null && _tailConv.HasIr)
                _tailConv.ProcessAdd(_tailInMono, 0, frames, _tailOut, 0, _tailGain * tailLevel);

            for (int f = 0; f < frames; f++)
            {
                float dry = _tailInMono[f];

                // 6帯域に分割 → 履歴へ（早期反射用）。
                // 「LPで下を抜き、残りを次段へ」の直列。最後の残りが最高域。
                int wi = _writePos & _ringMask;
                float rest = dry;
                for (int b = 0; b < kNumBands - 1; b++)
                {
                    float lo = _split[b].Process(rest);
                    _bandTmp[b] = lo;
                    rest -= lo;
                }
                _bandTmp[kNumBands - 1] = rest;
                for (int b = 0; b < kNumBands; b++) _ring[b][wi] = _bandTmp[b];

                // 早期反射の畳み込み（クロスフェード）。
                ConvOne(_ir, _writePos, out float aL, out float aR, out float aD,
                        out float aDirL, out float aDirR);
                float outL, outR, scatSend, dirL, dirR;
                if (_fading)
                {
                    ConvOne(_irNext, _writePos, out float bL, out float bR, out float bD,
                            out float bDirL, out float bDirR);
                    float t = (float)_xfadePos / _xfadeLen;
                    outL = aL * (1f - t) + bL * t;
                    outR = aR * (1f - t) + bR * t;
                    scatSend = aD * (1f - t) + bD * t;
                    dirL = aDirL * (1f - t) + bDirL * t;
                    dirR = aDirR * (1f - t) + bDirR * t;
                    if (++_xfadePos >= _xfadeLen) { _fading = false; _ir = _irNext; }
                }
                else { outL = aL; outR = aR; scatSend = aD; dirL = aDirL; dirR = aDirR; }

                // 計測：直接音と、早期反射の鏡面成分（直接音を除く。散乱・尾を足す前）。
                sumDirect += (dirL * dirL + dirR * dirR) * 0.5f;
                float eL = outL - dirL, eR = outR - dirR;
                sumEarly += (eL * eL + eR * eR) * 0.5f;

                // 散乱スメア：拡散成分を allpass で撹拌して時間方向に滲ませる。
                // 拡散器は固定ネットワークなので、IR が切り替わっても状態は連続でよい
                // （送り込む信号側を上でクロスフェード済み）。
                {
                    float g = scatterDiffusion;
                    float scL = Allpass(scatSend, _scatBufL, _scatLenL, _scatPosL, g);
                    float scR = Allpass(scatSend, _scatBufR, _scatLenR, _scatPosR, g);
                    sumScatter += (scL * scL + scR * scR) * 0.5f;
                    outL += scL; outR += scR;
                }

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
                    float fL = wL * _oNorm * fdnWet, fR = wR * _oNorm * fdnWet;
                    sumTail += (fL * fL + fR * fR) * 0.5f;
                    outL += fL; outR += fR;
                }

                // 実測尾を加算。
                float tL = _tailOut[0][f], tR = _tailOut[1][f];
                sumTail += (tL * tL + tR * tR) * 0.5f;
                outL += tL; outR += tR;

                outL *= outputGain; outR *= outputGain;
                sumOut += (outL * outL + outR * outR) * 0.5f;

                // スコープ用の波形リングへ。
                int sp2 = (scopeBase + f) % Scope.BufferLength;
                Scope.L[sp2] = outL; Scope.R[sp2] = outR;
                int baseI = f * channels;
                if (channels >= 2)
                {
                    data[baseI] = outL; data[baseI + 1] = outR;
                    for (int c = 2; c < channels; c++) data[baseI + c] = (outL + outR) * 0.5f;
                }
                else data[baseI] = (outL + outR) * 0.5f;
                _writePos++;
            }

            // スコープへ書き出す。RMS は表示が暴れないよう軽く平滑する。
            float inv = 1f / Mathf.Max(1, frames);
            Scope.WritePos = (scopeBase + frames) % Scope.BufferLength;
            Scope.SampleRate = _sampleRate;
            bool tailLive = tailMode == TailMode.Measured && _tailConv != null && _tailConv.HasIr;
            Scope.TailPartitions = tailLive ? _tailConv.NumPartitions : 0;
            Scope.ActiveParts = tailLive ? _tailConv.ActiveParts : 0;
            Scope.SplitMs = _splitMs;
            const float k = 0.3f;
            Scope.RmsDirect = Mathf.Lerp(Scope.RmsDirect, Mathf.Sqrt(sumDirect * inv), k);
            Scope.RmsEarly = Mathf.Lerp(Scope.RmsEarly, Mathf.Sqrt(sumEarly * inv), k);
            Scope.RmsScatter = Mathf.Lerp(Scope.RmsScatter, Mathf.Sqrt(sumScatter * inv), k);
            Scope.RmsTail = Mathf.Lerp(Scope.RmsTail, Mathf.Sqrt(sumTail * inv), k);
            Scope.RmsOut = Mathf.Lerp(Scope.RmsOut, Mathf.Sqrt(sumOut * inv), k);
        }

        // 1つのIRを畳んで L/R と「拡散送り」を返す（6帯域×タップ×パン）。
        // クロスフェード中は新旧2回呼ぶ。
        //
        // 各タップを鏡面と拡散にエネルギー分割する:
        //   鏡面 = √(1-s) … 従来どおり方向つきのデルタ
        //   拡散 = √s     … 方向を失った成分。まとめて拡散器へ送り、時間方向に滲ませる
        // 振幅ではなく√で分けるのは、両者が無相関でパワーが加算されるため（(1-s)+s=1 で総量保存）。
        // dirL/dirR は「タップ0（直接音）だけの寄与」。L/R には含まれたまま返す。
        // 計測で 直接音 と 早期反射 を分けるために使う（混ざっていると内訳が読めない）。
        private void ConvOne(ConvTap[] ir, int wp, out float L, out float R, out float diffuse,
                             out float dirL, out float dirR)
        {
            float l = 0f, r = 0f, d = 0f;
            dirL = 0f; dirR = 0f;
            if (ir != null)
            {
                float[] r0 = _ring[0], r1 = _ring[1], r2 = _ring[2];
                float[] r3 = _ring[3], r4 = _ring[4], r5 = _ring[5];
                for (int k = 0; k < ir.Length; k++)
                {
                    int rp = (wp - ir[k].delaySamples) & _ringMask;
                    float tv = ir[k].g0 * r0[rp] + ir[k].g1 * r1[rp] + ir[k].g2 * r2[rp]
                             + ir[k].g3 * r3[rp] + ir[k].g4 * r4[rp] + ir[k].g5 * r5[rp];
                    d += tv * ir[k].gDiff;
                    float sp = tv * ir[k].gSpec;
                    float cl = sp * ir[k].panL, cr = sp * ir[k].panR;
                    if (k == 0) { dirL = cl; dirR = cr; }   // タップ0は必ず直接音（RebuildIr の並び順）
                    l += cl; r += cr;
                }
            }
            L = l; R = r; diffuse = d;
        }
    }
}
