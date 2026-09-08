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
        [Tooltip("尾 IR を差し替えるときのクロスフェード(ms)。0 で即差し替え（A/B 用）。\n"
                 + "⚠ 0 にすると、周波数領域遅延線に溜まった過去の入力が新しい IR で"
                 + "畳み直されて +1.97dB のふくらみが 0.35 秒続きます（実測）。\n"
                 + "混ぜ方はエンジンの中（遅延線は共有、IR スペクトルだけ 2 世代）。")]
        [Range(0f, 200f)] public float tailCrossfadeMs = 50f;

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
        [Tooltip("早期↔後期の境目の下限(ms)。境目は mixing time ≈ √V で決まるが、\n"
                 + "畳み込みのブロック遅延を隠すために下限を掛ける。\n"
                 + "※C# 経路(IrConvolver)の minSplitMs と同じ値にすること。片方だけ動かすと\n"
                 + "  A/B したとき尾の始まりがずれて別の音に聞こえる。")]
        [Range(1f, 60f)] public float minSplitMs = 3f;
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
        private float _splitMs = 120f;   // 早期↔後期の境目。急に動かさない（下の解説）
        private int _tailEchoVersion = -1;   // 最後に取り込んだエコグラムの版
        private int _tailShapeIndex = -1;    // 最後に尾の形を引いた代表音源の index
        private IntPtr _tailBus = IntPtr.Zero;   // 預けている共有バス（Zero=自前で畳む）
        private int _tailBusFrames = 0;
        private float _lastOutSample = 0f;   // 段差の計器。ブロックの継ぎ目を見るため持ち越す
        private bool _tailWasVirtual = false;   // 直前がバーチャル段だったか（戻すため）

        // 尾の畳み込みを共有バスへ預ける。**メインスレッドからだけ呼ぶこと。**
        //   ⚠ バスへ預けると、この音源の rmsTail は 0 になる（計器はバス側の rms()）。
        private void AttachTailBus(int shapeIndex, AcousticFlowSceneDemo.SourceTaps ts)
        {
            var renderer = TailBusHost;
            if (renderer == null || !renderer.enableSharedTail) { DetachTailBus(); return; }
            var bus = renderer.GetOrCreateBus(shapeIndex, tailSeconds, _tailBusFrames);
            if (bus == IntPtr.Zero) { DetachTailBus(); return; }
            // 代表＝尾の形の出どころが自分。その 1 本だけが IR をバスへ入れる。
            bool isOwner = (shapeIndex == ts.EngineIndex);
            _tailBus = bus;
            if (_voice != IntPtr.Zero) Native.AF_VoiceSetTailBus(_voice, bus, isOwner ? 1 : 0);
        }

        /// 共有バスから外して自前の畳み込みに戻す。バスを壊す前に必ず呼ぶこと。
        // ── 方向バス ──
        //   反射・回折のタップをリスナーのレーンへ預ける。ホストのバスは実行時に生える（Start で付く）ので、
        //   付くまで毎フレーム試す。付いた後の SetTaps からレーン行きになる。
        private IntPtr _dirBus = IntPtr.Zero;
        private void AttachDirectionBus()
        {
            var host = TailBusHost;
            if (host == null || !host.enableDirectionBus || _voice == IntPtr.Zero) return;
            var bus = host.GetOrCreateDirectionBus(_tailBusFrames);
            if (bus == IntPtr.Zero) return;
            if (Native.AF_DirectionBusHasHrtf(bus) == 0 && _hrtf != IntPtr.Zero)
                Native.AF_DirectionBusSetHrtf(bus, _hrtf, headCircumferenceCm);
            _dirBus = bus;
            Native.AF_VoiceSetDirectionBus(_voice, bus);
        }
        public void DetachDirectionBus()
        {
            if (_voice != IntPtr.Zero && _dirBus != IntPtr.Zero) Native.AF_VoiceSetDirectionBus(_voice, IntPtr.Zero);
            _dirBus = IntPtr.Zero;
        }

        public void DetachTailBus()
        {
            if (_voice != IntPtr.Zero && _tailBus != IntPtr.Zero)
                Native.AF_VoiceSetTailBus(_voice, IntPtr.Zero, 0);
            _tailBus = IntPtr.Zero;
            _tailShapeIndex = -1;   // 次のフレームで組み直させる（自前の器には IR が無い）
            _tailEchoVersion = -1;
        }

        private TailBusRenderer _tailBusHost;
        private TailBusRenderer TailBusHost
        {
            get
            {
                if (_tailBusHost == null)
                    _tailBusHost = FindFirstObjectByType<TailBusRenderer>();
                return _tailBusHost;
            }
        }
        private float[] _lastEarGain;        // 最後に焼き込んだ左右バランス

        // 尾の左右バランスが意味のある量だけ変わったか。エコグラムより速く動くので、
        // 版が同じでもこれが変われば作り直す必要がある。閾値未満は無視して差し替えを減らす。
        private bool EarGainChanged(float[] ear)
        {
            if (ear == null) { bool had = _lastEarGain != null; _lastEarGain = null; return had; }
            if (_lastEarGain == null || _lastEarGain.Length != ear.Length)
            {
                _lastEarGain = (float[])ear.Clone();
                return true;
            }
            for (int i = 0; i < ear.Length; i++)
            {
                if (Mathf.Abs(ear[i] - _lastEarGain[i]) > 0.02f)
                {
                    System.Array.Copy(ear, _lastEarGain, ear.Length);
                    return true;
                }
            }
            return false;
        }

        // ★C# 経路(IrConvolver)と同時に有効にしない。
        //   両方が OnAudioFilterRead を返すと、同じ音源が二重に鳴る（実際に踏んだ）。
        //   AcousticFlowSceneDemo.SetDspPath は起動時と Y キーでしか呼ばれないので、
        //   インスペクタでチェックを手で入れると排他が効かない。どこから有効にされても
        //   不変条件（鳴らすのは常に片方だけ）が保たれるよう、ここで相手を降ろす。
        //   ※ 同じ GameObject とは限らないので探索で拾う（OnEnable は滅多に呼ばれない）。
        private void OnEnable()
        {
            int downed = 0;
            foreach (var ir in FindObjectsByType<IrConvolver>(FindObjectsSortMode.None))
                if (ir.enabled) { ir.enabled = false; downed++; }
            if (downed > 0)
                Debug.Log($"[VoiceConvolver] C++ 経路を有効にしたので C# 経路(IrConvolver) {downed} 個を"
                          + "降ろしました（両方鳴らすと二重になります。Y キーで切り替え）");
        }

        private void Awake()
        {
            // 配置されている DLL が C# と同じ版か確かめる。違えば鳴らさない。
            //   AF_VoiceTap の長さが食い違うと、例外も出さずにタップの中身が化ける。
            if (!Native.CheckAbi()) return;

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

            // 尾 IR のクロスフェード。古い DLL には無い口なので握りつぶす（既定の挙動のまま）。
            try { Native.AF_VoiceSetTailCrossfadeMs(_voice, tailCrossfadeMs); }
            catch (System.EntryPointNotFoundException) { }

            _hrtf = LoadHrtf();
            Native.AF_VoiceSetHrtf(_voice, _hrtf);

            _tailBusFrames = cfg.maxFrames;
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
            DetachDirectionBus();
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
            if (_dirBus == IntPtr.Zero) AttachDirectionBus();
            if (!_ready) return;
            var ts = MyTaps();
            if (ts == null) return;

            Native.AF_VoiceSetOutputGain(_voice, outputGain);
            // 【道具A】尾のゲート。IrConvolver 側と同じ意味にそろえる。
            Native.AF_VoiceSetTailLevel(_voice, IrConvolver.Solo.PassTail ? tailLevel : 0f);
            Native.AF_VoiceSetScatterDiffusion(_voice, scatterDiffusion);
            Native.AF_VoiceSetHrtfEnabled(_voice, enableHrtf ? 1 : 0);

            // 到来方向（遮蔽時は回折で回り込む方向になる）。
            Vector3 d = ts.DirectDirLocal;
            Native.AF_VoiceSetDirection(_voice, new AFVector3(d), headCircumferenceCm);

            // 【B1】回折バスの到来方向＝開口の方向。直接音とは別に持つ。
            //   直接音は壁の向こうの音源を指したまま透過まで落ち、実エネルギーは
            //   開口を回り込んだ成分が運ぶ。その成分に定位の手がかりを載せる。
            if (ts.HrtfTapIndex >= 0)
                Native.AF_VoiceSetDiffractionDirection(
                    _voice, new AFVector3(ts.HrtfTapDirLocal), headCircumferenceCm);

            // 尾の掛かり方。開けた場所は wet が小さく、壁裏では srcLevel が小さくなる。
            _wet = Mathf.Clamp01(AcousticFlowSceneDemo.Status.Wet);
            _srcLevel = (ts.SourceLevel > 0f) ? ts.SourceLevel : 1f;
            Native.AF_VoiceSetTailEnvelope(_voice, _wet, _srcLevel);

            // ── タップを詰める ──
            //   index 0 は直接音。以降は反射/回折で、音量を reflectionLevel で絞る。
            //   散乱率は遅延とともに上げる（高次反射ほど拡散へ溶ける）。正規化には
            //   **頭打ち前の真の mixing time** を使うこと（畳み込みの都合で下限を掛けた値だと
            //   狭い部屋で実際の4倍以上に膨らみ、散乱が効かなくなる）。
            // mixing time と尾の比は、DLL の組み立てなら音源ごと（#4）。無ければホストの全体値。
            float mix = (ts.MixingTimeMs > 0f) ? ts.MixingTimeMs : AcousticFlowSceneDemo.Status.MixingTimeMs;
            if (mix <= 0f) mix = 25f;
            int n = Mathf.Min(ts.Count, _taps.Length);
            int nb = AcousticEngine.NumBands;
            // 【道具A】経路単位のソロ／ミュート。既定（素通し）のときは判定ごと飛ばす。
            //   落としたぶんは詰めて渡すので、エンジンには「そのタップは無かった」ように見える。
            bool soloOn = !IrConvolver.Solo.IsDefault;
            int w = 0;
            for (int i = 0; i < n; i++)
            {
                if (soloOn && ts.Type != null && i < ts.Type.Length
                    && !IrConvolver.Solo.AllowsType(ts.Type[i])) continue;
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
                // 【B1】選ばれた回折タップだけ HRTF バスへ。切り替えの連続性は
                //   エンジン側のタップ補間（30ms）が受け持つので、ここは 0/1 でよい。
                t.hrtfWeight = (i == ts.HrtfTapIndex) ? 1f : 0f;
                // 到来方向。反射/回折タップの軽量な両耳化（ITD＋帯域別ILD）に使う。
                //   直接音(index 0)はフル HRTF が担当なので渡さない。
                if (i > 0) {
                    Vector3 dl = ts.DirLocal[i];
                    t.dirX = dl.x; t.dirY = dl.y; t.dirZ = dl.z;
                }
                _taps[w++] = t;
            }
            Native.AF_VoiceSetTaps(_voice, _taps, w);
            tapCount = w;

            // ── 後期尾。重いので数フレームに1回 ──
            //
            // ★エコグラムの中身が変わったときだけ作り直す。
            //   尾の IR はブロック境界で**ハードスワップ**される（クロスフェードしない）。
            //   分割畳み込みは過去の入力ブロックを保持しているので、IR を差し替えると
            //   「もう出ている尾」が別の IR で畳み直される。中身が同じでも毎回差し替えると
            //   60fps で毎秒 7.5 回それが起き、移動中に尾が揺れて二重に聞こえる。
            //   C# 経路(IrConvolver)には最初からこのガードがあったが、こちらには無かった。
            //   エンジンはエコグラムを内部レートでしか作り直さないので、版が同じなら
            //   差し替える理由が無い。
            if (++_frameCounter >= Mathf.Max(1, tailRebuildEveryFrames))
            {
                _frameCounter = 0;
                var scene = AcousticFlowSceneDemo.SharedScene;

                // ★バーチャル段は尾も鳴らさない。
                //   段はシーン側（何を解くか）の話だが、**オーディオスレッドの費用は別の天井**。
                //   実測: 音源 1 本の音声スレッド負荷 4.27% のうち**尾が 1.95%（46%）**。
                //   ここを止めないと「解かないのに一番高い部分は鳴り続ける」ことになる。
                //   ⚠ 可聴限界より下でしか選ばれない段なので、止めても聞こえ方に出ない。
                bool isVirtual = (scene != null && scene.IsValid && ts.EngineIndex >= 0
                                  && scene.GetSourceTierEffective(ts.EngineIndex) == 2);
                if (isVirtual)
                {
                    if (_tailBus != IntPtr.Zero) DetachTailBus();
                    Native.AF_VoiceSetTailLevel(_voice, 0f);
                    _tailEchoVersion = -1;   // 復帰したら組み直させる
                    _tailWasVirtual = true;
                }
                else if (_tailWasVirtual)
                {
                    Native.AF_VoiceSetTailLevel(_voice, tailLevel);   // 戻す
                    _tailWasVirtual = false;
                }
                int echoVer = isVirtual ? _tailEchoVersion
                                        : AcousticFlowSceneDemo.Status.EchogramVersion;
                var earNow = AcousticFlowSceneDemo.Status.TailEarBandGain;
                bool earChanged = EarGainChanged(earNow);
                // 代表音源（尾の形の出どころ）が変わったら、エコグラム版が同じでも組み直す。
                //   これを見ないと、部屋をまたいだ瞬間に古い部屋の形が残る。
                int shapeNow = ts.TailShapeIndex >= 0 ? ts.TailShapeIndex : ts.EngineIndex;
                bool shapeChanged = (shapeNow != _tailShapeIndex);
                if (scene != null && scene.IsValid
                    && (echoVer != _tailEchoVersion || earChanged || shapeChanged))
                {
                    _tailEchoVersion = echoVer;
                    // ★尾の形が変わったら、預ける共有バスも張り替える（形＝バスの鍵）。
                    //   IR が違う音源を同じバスへ入れると、片方の部屋の響きがもう片方に付く。
                    //   代表（TailShapeIndex == 自分の EngineIndex）だけが IR をバスへ入れる。
                    //   0 本だとバスに IR が入らず尾が丸ごと鳴らないので、ここを間違えないこと。
                    if (shapeChanged || _tailBus == System.IntPtr.Zero) AttachTailBus(shapeNow, ts);
                    _tailShapeIndex = shapeNow;
                    // ★自分が担当する音源のエコグラムを引く。以前は全音源の和しか無かったので、
                    //   別の部屋の音源も同じ尾で鳴っていた。
                    // ★尾の**形**は同じ部屋の代表音源から引く（TailShapeIndex）。
                    //   量（下の dg）は自分のものを使うので、部屋の響きは共有しつつ
                    //   「どれだけ送るか」は音源ごとに残る。実測の分解に基づく:
                    //     形（200→600ms の傾き） 同室 1.3dB 以内 / 隣室 2.6〜3.9dB ずれ
                    //     量（オフセット）        同室でも 3〜4dB 開く
                    int shapeIdx = ts.TailShapeIndex >= 0 ? ts.TailShapeIndex : ts.EngineIndex;
                    int bins = scene.GetEchogramBands(shapeIdx, _echo, _echo.Length / nb);
                    if (bins > 0)
                    {
                        // 尾の絶対レベルの基準になる直接音ゲイン。
                        //
                        // ★遮蔽を割り戻して「遮られていなければどれだけ届くか」にする。
                        //   ts.BandGain = 生存(遮蔽) × 空気吸収 × 距離減衰 で、生存が入っている。
                        //   これをそのまま渡すと
                        //     尾 = (生存 × 空気 × 1/r) × √target × tailSrcLevel(= 生存)
                        //   となって**生存が 2 回掛かる**。柱の陰に入っただけで尾が二乗ぶん
                        //   落ちて、残響ごと消えてしまう（実測: 生存 0.775 で -4.4dB、
                        //   隣の部屋なら -20dB 以上）。
                        //   物理的には、部屋の残響音場は音源が部屋へ注いだパワーで決まるので、
                        //   リスナーの見通しが柱で切れても残響は残る（むしろ相対的に増える）。
                        //   遮蔽は tailSrcLevel の 1 箇所だけで掛ける。
                        //   ※ tailSrcLevel は反射込みの生存なので、同じ部屋の柱では大きく
                        //     下がらず、別の部屋なら下がる ── 「部屋にどれだけ注げているか」の
                        //     近似として機能する（将来は部屋グラフの開口面積で置き換えたい）。
                        // ★遮蔽を含まない自由音場の直接レベル（距離減衰＋空気吸収だけ）。
                        //   直接音タップの BandGain を使うと、柱の陰では透過ぶんまで落ちて
                        //   -31dB になり（実測）、尾まで一緒に消える。SourceLevel で割り戻す
                        //   だけでは戻らない（割る値は 0.66 なのに落ちているのは 31dB）。
                        //   遮蔽は下流の tailSrcLevel の 1 箇所だけで効かせる。
                        float dg = ts.FreeFieldDirect;
                        // ★尾の開始は平滑して動かす。ここが動くと「早期タップの打ち切り」と
                        //   「尾の開始」が同時にずれ、尾側はブロック境界でハードスワップなので
                        //   段差として聞こえる。以前は max(10, mixingTime) を毎回そのまま
                        //   渡していた（C# 経路は平滑していたので、そこも食い違っていた）。
                        float mixT = (ts.MixingTimeMs > 0f) ? ts.MixingTimeMs : AcousticFlowSceneDemo.Status.MixingTimeMs;
                        if (mixT <= 0f) mixT = 120f;
                        // ★尾の比は音源ごと（不具合 #4）。以前は音源 0 の距離だけで全音源の比を決めていた。
                        float tailRatio = (ts.TailRatio >= 0f) ? ts.TailRatio : AcousticFlowSceneDemo.Status.ReverbTargetRatio;
                        _splitMs = Mathf.Lerp(_splitMs, Mathf.Max(minSplitMs, mixT), 0.15f);

                        // ★耳ごとの帯域ゲイン（後期残響の左右バランス）を渡す。
                        //   これを null にしていたので C++ 経路の尾は**方向づけなしの均一**で、
                        //   C# 経路（渡している）と比べて定位が弱く聞こえていた。
                        var ear = AcousticFlowSceneDemo.Status.TailEarBandGain;
                        Native.AF_VoiceRebuildTail(
                            _voice, _echo, bins, AcousticFlowSceneDemo.Status.EchogramBinMs,
                            _splitMs, 20f,
                            30f, 0f, 0.6f, dg,
                            tailRatio,
                            ear, ear != null ? ear.Length : 0);
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

            // 【道具10】このブロックの実費。IrConvolver 側と同じやり方で測る
            //   （C++ 経路の音源が予算メータに出てこないと、実機の内訳が見えない）。
            long meterT0 = System.Diagnostics.Stopwatch.GetTimestamp();

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

            // 【道具 ぷつぷつの切り分け】波形の段差を測る。ブロックをまたぐので前ブロックの
            //   最後のサンプルを持ち越す（持ち越さないと、いちばん出やすい継ぎ目を見逃す）。
            //   ★音源 0 だけが書く。全音源が同じ静的枠へ書くと最後に書いた者勝ちになり、
            //     どの音源の段差を見ているのか分からなくなる。
            if (sourceIndex == 0)
            {
                float maxStep = 0f, sumSq = 0f, prev = _lastOutSample;
                for (int f = 0; f < frames; f++)
                {
                    float v = _outL[f];
                    float d = v - prev; if (d < 0f) d = -d;
                    if (d > maxStep) maxStep = d;
                    sumSq += v * v;
                    prev = v;
                }
                _lastOutSample = prev;
                float blkRms = Mathf.Sqrt(sumSq / Mathf.Max(1, frames));
                // 無音のブロックは比が暴れるので数えない（0 割りも避ける）。
                if (blkRms > 1e-5f)
                {
                    float ratio = maxStep / blkRms;
                    IrConvolver.Scope.StepRatio = ratio;
                    float calm = IrConvolver.Scope.StepRatioCalm;
                    if (calm <= 0f) calm = ratio;
                    // 平常値はゆっくり追う（跳ねを平常に取り込まないよう、上がるときだけ鈍く）。
                    float a = (ratio > calm) ? 0.002f : 0.02f;
                    IrConvolver.Scope.StepRatioCalm = calm + (ratio - calm) * a;
                    if (ratio > calm * 4f) IrConvolver.Scope.StepSpikes++;
                }
                IrConvolver.Scope.BlockDurMs = _sampleRate > 0
                    ? frames * 1000f / _sampleRate : 0f;
            }

            if (sourceIndex >= 0 && sourceIndex < IrConvolver.Scope.MaxMeteredSources)
            {
                float ms = (float)((System.Diagnostics.Stopwatch.GetTimestamp() - meterT0) * 1000.0
                                   / System.Diagnostics.Stopwatch.Frequency);
                IrConvolver.Scope.BlockMs[sourceIndex] =
                    Mathf.Lerp(IrConvolver.Scope.BlockMs[sourceIndex], ms, k);
                IrConvolver.Scope.BlockFrames[sourceIndex] = frames;
                if (sourceIndex > IrConvolver.Scope.MeteredMax) IrConvolver.Scope.MeteredMax = sourceIndex;
                // 【道具A】音源ごとの出力。★消す前の量を出す（比べるために要る）。
                IrConvolver.Scope.OutRms[sourceIndex] =
                    Mathf.Lerp(IrConvolver.Scope.OutRms[sourceIndex], m.rmsOut, k);
            }

            // 【道具A】音源ごとのミュート／ソロ。★計算は止めない
            //   （止めるとエンジン側の遅延線が進まず、解除で音が飛ぶ＝連続性の事故になる）。
            if (!IrConvolver.Solo.AllowsSource(sourceIndex)) Array.Clear(data, 0, data.Length);
        }
    }
}
