// AcousticFlowDemo.cs
// AcousticEngine(ネイティブ) を Unity 上で可視化するデモ。
//   遮蔽線:  リスナー<->音源 が見通せれば緑 / 遮蔽なら赤
//   レイ:    リスナーから3D全方向に散布し、壁で反射させて描画
//
// 表示モード（数字キーで切替）:
//   [1] PerceivedPaths … 音源に「届く」経路だけ表示。届かないレイは隠す。
//                        直接音が遮蔽されていれば直接線は赤。
//                        → 壁裏でも反射で音が回り込む経路が見える。
//   [2] Directivity    … 音源の指向特性を forward 軸まわりの回転体で描く。
//                        正面ほど膨らみ、背面/側面ほどしぼむ＝聞こえ方の形。
//
// レイは可視化と遮蔽エネルギー計算で共用（同一の散布・本数）。基準 8192 本。
using UnityEngine;

namespace AcousticFlow
{
    public class AcousticFlowDemo : MonoBehaviour
    {
        public enum VisualizationMode
        {
            PerceivedPaths = 0,
            Directivity = 1,
        }

        [Header("シーン参照")]
        public Transform listener;
        public Transform source;
        public Transform[] walls;

        [Header("複数音源")]
        [Tooltip("追加の音源（source が0番、ここが1番以降）。")]
        public Transform[] extraSources;
        [Tooltip("各音源が鳴らす Wwise イベント名（並び順=source, extraSources…）。空/未設定は Play_Test。")]
        public string[] sourceEvents;
        [Tooltip("モニター（Band/Path/指向性）に表示する音源の番号。Tab キーで巡回。")]
        public int selectedSource = 0;

        [Header("空間化（HRTF / パンニング）")]
        [Tooltip("H キーで切替。Wwise の State グループ Spatialization を HRTF/Panning に切り替える。")]
        public bool useHrtf = false;

        [Header("リスナーの自動周回（中央の音源を周回）")]
        [Tooltip("ON のときリスナーが中央の音源の周りを自動で回る。WASD/視点操作で OFF。")]
        public bool autoOrbitListener = true;
        public float orbitRadius = 8f;
        public float orbitSpeed = 40f;   // 度/秒

        [Header("音源の手動操作")]
        [Tooltip("矢印キー=水平移動 / PageUp=上昇 / PageDown=下降。動かすと自動周回はオフ。")]
        public float moveSpeed = 6f;

        [Header("リスナー（プレイヤー）操作")]
        [Tooltip("WASD=向いてる方向に水平移動 / Space=上昇 / LeftShift=下降。")]
        public float listenerMoveSpeed = 6f;
        [Tooltip("右クリック押しながらマウスで見回す。値は感度。")]
        public float lookSpeed = 2.5f;

        [Header("レイ（3D散布＋反射）— 可視化と遮蔽エネルギー計算で共用")]
        [Tooltip("可視化レイと遮蔽エネルギー計算レイは同一。基準 8192 本。多いほど安定だが重い。")]
        public int rayCount = 8192;
        public int maxBounces = 3;
        public float rayMaxDistance = 30f;

        [Header("表示モード（1/2 キーで切替）")]
        public VisualizationMode mode = VisualizationMode.PerceivedPaths;

        [Header("音源指向性")]
        [Tooltip("音源の主放射方向は source.forward。タイプで正面/側面/背面の聞こえ方が変わる。")]
        public AcousticEngine.DirectivityType sourceDirectivity = AcousticEngine.DirectivityType.Cardioid;
        [Tooltip("指向性バルーン（モード2）の大きさ（メートル）。")]
        public float directivityGizmoScale = 3f;
        private float _directivityGain = 1f;     // 向きによる音量(0..1)
        private float _directivityLowpass = 0f;  // 背面/側面のこもり(0..1)
        // 残響のみモード（Rキー）：直接音(ドライ出力)をミュートし、反射=リバーブだけ聴く。
        // 出力バス音量を0にするとドライが消え、リバーブのaux送りは別経路なので残る。
        private bool _soloReverb = false;
        // --- Wwise配線の切り分けテスト（一時）。締切後に撤去予定 ---
        // Oキー：obstruction を強制1.0 → こもれば LPF 経路が生きてる証拠。
        private bool _testObstruction = false;
        // Tキー：ReverbWet=100 / Decay=1.5s 固定 → 残響が出れば RoomVerb 経路が生きてる証拠。
        private bool _testReverbFull = false;
        // Pキー：全音源を一時停止（ボイスを止める）。リバーブの尾は残るので、止めた瞬間に
        // 「どれだけ響きが残るか」＝残響の減衰を耳で確かめられる。
        private bool _audioPaused = false;

        // 帯域別EQ：6帯域の壁透過を 低/中/高 の3バンドに集約し、各音源の実音EQ Gain(dB)を
        // ゲームオブジェクト単位RTPCで駆動する。これで「壁の向こうは低音だけ回り込む」が
        // スカラLPFでなく本物の帯域別音色変化として鳴る。Wwise側に3バンドEQ＋RTPC対応付けが要る。
        public bool bandEq = true;            // 帯域別EQを実音に当てる
        public float bandEqFloorDb = -24f;    // 各バンドの最大カット量(dB)。-inf/過剰カット回避の下限
        // 注: 帯域ゲイン受けの _bandGains は後方（Band Monitor 用）で宣言済み。重複宣言しない。
        private float[] _bandSmoothedDb;      // 音源×3バンドの平滑化dB（zipperノイズ回避）
        private const string RtpcEqLow = "EqLow";
        private const string RtpcEqMid = "EqMid";
        private const string RtpcEqHigh = "EqHigh";

        [Header("音源面（area source）— 経路可視化")]
        [Tooltip("音源面の形。Point=点 / Disk=円 / Rect=矩形。面は source.forward を法線、source.right を横軸にする。")]
        public AcousticEngine.SourceShape sourceShape = AcousticEngine.SourceShape.Disk;
        [Tooltip("円形(Disk)のときの半径（メートル）。")]
        public float sourceRadius = 1.5f;
        [Tooltip("矩形(Rect)のときの半幅(x)・半高(y)（メートル）。")]
        public Vector2 sourceHalfSize = new Vector2(1.5f, 1f);
        [Tooltip("経路モニター（別タブ）のグリッド解像度。")]
        public int faceGridCols = 24;
        public int faceGridRows = 24;

        // 音源面の到達量グリッド(0..1, -1=面外)。Source Path Monitor（別タブ）が読む。
        private float[] _faceGrid;
        public static float[] LatestFaceReachability { get; private set; }
        public static int FaceGridCols { get; private set; }
        public static int FaceGridRows { get; private set; }
        public static AcousticEngine.SourceShape LatestFaceShape { get; private set; }
        public static string FaceStatus { get; private set; } = "未計算";  // 別タブの状態表示用

        [Header("残響モニター（エコグラム）")]
        [Tooltip("残響可視化用のレイ本数。")]
        public int echogramRays = 1024;
        [Tooltip("残響の反射回数。多いほど尾が長く出る。")]
        public int echogramBounces = 4;
        [Tooltip("時間ビン数。")]
        public int echogramBins = 100;
        [Tooltip("1ビンの長さ(ms)。bins×binMs が窓幅（例 100×10ms=1秒）。")]
        public float echogramBinMs = 10f;
        private float[] _echogram;
        private readonly Vector3[] _echoSrc = new Vector3[1];
        private int _echoFrameCounter;
        private float _reverbWet, _reverbDecay;   // エコグラムから算出した残響パラメータ（RTPCへ）
        // エコグラム（到達時間ビンの広帯域エネルギー）。Reverb Monitor（別タブ）が読む。
        public static float[] LatestEchogram { get; private set; }
        public static int EchogramBins { get; private set; }
        public static float EchogramBinMs { get; private set; }

        [Header("遮蔽の計算（音用レイ＝可視化とは別予算）")]
        [Tooltip("音用の遮蔽レイ本数。音源数ぶん毎フレーム飛ばすので、増やすと重い。可視化レイ(rayCount)とは別。")]
        public int occlusionRayCount = 1024;
        [Tooltip("音用の遮蔽レイの反射回数。")]
        public int occlusionBounces = 2;

        [Header("遮蔽のスムージング")]
        [Tooltip("遮蔽値が 0↔1 に変化する速さ（毎秒）。小さいほどゆっくり滑らかに切替。")]
        public float occlusionSmoothSpeed = 3f;

        private AcousticEngine _engine;
        private bool _occluded;
        private float _occlusionTarget;  // 材質ベースの遮蔽量(0..1)

        // パフォーマンス計測（「応答速度＝内面」可視化用）。
        private readonly System.Diagnostics.Stopwatch _acStopwatch = new System.Diagnostics.Stopwatch();
        private double _acousticMs;  // 音響計算1フレームの所要時間(ms)
        private float _fps;          // 平滑化した FPS
        private float _outL, _outR;  // 出力(マスターバス)の左右レベル(線形)
        // 出力L/Rの時間履歴（波形表示用）。Output Monitor タブが読む。
        public const int OutHistLen = 256;
        private float[] _outHistL, _outHistR;
        private int _outHistIdx;
        public static float[] OutHistoryL { get; private set; }
        public static float[] OutHistoryR { get; private set; }
        public static int OutHistoryHead { get; private set; }  // 次に書く位置(=最古)

        // 帯域別の透過ゲイン(0..1)。Band Monitor（別タブ）が読む。
        private readonly float[] _bandGains = new float[AcousticEngine.NumBands];
        public static float[] LatestBandGains { get; private set; }   // Editor監視用（再生中のみ非null）
        private float _orbitAngle;
        private Vector3[] _path;   // 1本のレイの反射経路（使い回し）

        // Wwise ゲームオブジェクトID。リスナー=2 固定、音源は 10 から連番。
        private const ulong ListenerObjId = 2;
        private const ulong SourceObjIdBase = 10;
        private Transform[] _sources;   // [0]=source, 以降 extraSources（再生中に確定）
        private float[] _occSmoothed;   // 音源ごとの遮蔽スムージング値
        private Vector3[] _srcPos;      // 共有パスへ渡す音源位置バッファ
        private float[] _occTargets;    // 共有パスが返す音源ごとの遮蔽目標値
        private ulong SourceId(int i) => SourceObjIdBase + (ulong)i;
        private bool _audioReady;   // バンク読込＋再生開始まで成功したか
        private string _audioStatus = "未初期化";

        private void OnEnable()
        {
            try
            {
                _engine = new AcousticEngine();
            }
            catch (System.Exception e)
            {
                Debug.LogError($"[AcousticFlow] エンジン生成に失敗: {e.Message}");
                _engine = null;
            }

            if (_engine != null && AcousticEngine.IsWwiseAvailable)
            {
                AcousticEngine.InitAudio();
                SetupAudio();
            }
        }

        // バンクを読み込み、音源/リスナーを登録し、テストイベントを再生する。
        private void SetupAudio()
        {
            if (!AcousticEngine.IsAudioInitialized)
            {
                _audioStatus = "Wwise初期化に失敗";
                return;
            }

            // StreamingAssets/WwiseBanks をバンクの基準フォルダにする。
            string bankPath = System.IO.Path.Combine(Application.streamingAssetsPath, "WwiseBanks");
            AcousticEngine.SetBankPath(bankPath);

            bool banksOk = AcousticEngine.LoadBank("Init.bnk") && AcousticEngine.LoadBank("TokyoGeto.bnk");
            if (!banksOk)
            {
                _audioStatus = "バンク未検出（WwiseBanksにInit.bnk/TokyoGeto.bnkを置く）";
                Debug.LogWarning($"[AcousticFlow] バンク読込失敗: {bankPath} に Init.bnk / TokyoGeto.bnk が必要です。");
                return;
            }

            AcousticEngine.RegisterGameObject(ListenerObjId, "Listener");
            AcousticEngine.SetDefaultListener(ListenerObjId);

            BuildSources();
            for (int i = 0; i < _sources.Length; i++)
                AcousticEngine.RegisterGameObject(SourceId(i), "Source" + i);
            UpdateAudioTransforms();

            int playingCount = 0;
            for (int i = 0; i < _sources.Length; i++)
                if (AcousticEngine.PostEvent(EventFor(i), SourceId(i)) != 0) playingCount++;

            _audioReady = playingCount > 0;
            _audioStatus = _audioReady
                ? $"再生中 (Play_Test) ×{playingCount}/{_sources.Length}"
                : "PostEvent失敗（イベント名を確認）";
            if (!_audioReady)
                Debug.LogWarning("[AcousticFlow] PostEvent 失敗。イベント名 Play_Test を確認してください。");

            // 初期の空間化 State（HRTF/パンニング）を反映。
            ApplySpatializationState();
        }

        // i 番目の音源が鳴らすイベント名。未指定は Play_Test にフォールバック。
        // sourceEvents の並びは _sources（=source, extraSources…）と一致させること。
        private string EventFor(int i)
        {
            if (sourceEvents != null && i < sourceEvents.Length && !string.IsNullOrEmpty(sourceEvents[i]))
                return sourceEvents[i];
            return "Play_Test";
        }

        // 全音源を一時停止／再開する（Pキー）。ボイスを止めてもリバーブのバスは
        // 尾を鳴らし続けるので、止めた瞬間に残響の減衰（RT60相当）を耳で確認できる。
        private void ToggleAudioPause()
        {
            if (!_audioReady || _sources == null || !AcousticEngine.IsAudioInitialized) return;
            _audioPaused = !_audioPaused;
            int action = _audioPaused ? AcousticEngine.ActionPause : AcousticEngine.ActionResume;
            for (int i = 0; i < _sources.Length; i++)
                AcousticEngine.ExecuteActionOnEvent(EventFor(i), action, SourceId(i));
        }

        // source（0番）＋ extraSources を結合して音源配列を確定する。
        private void BuildSources()
        {
            var list = new System.Collections.Generic.List<Transform>();
            if (source != null) list.Add(source);
            if (extraSources != null)
                foreach (var s in extraSources)
                    if (s != null && s != source) list.Add(s);
            _sources = list.ToArray();
            _occSmoothed = new float[_sources.Length];
        }

        // Wwise の空間化 State を現在の useHrtf に合わせて設定する。
        private void ApplySpatializationState()
        {
            if (!AcousticEngine.IsAudioInitialized) return;
            // SetState 未搭載の古い DLL でも音声セットアップを巻き添えで止めない。
            try { AcousticEngine.SetState("Spatialization", useHrtf ? "HRTF" : "Panning"); }
            catch (System.EntryPointNotFoundException) { /* DLL再ビルドで有効化される */ }
        }

        // Wwise 側の音源・リスナーの位置と向きを Unity の Transform から更新する。
        private void UpdateAudioTransforms()
        {
            AcousticEngine.SetGameObjectPosition(ListenerObjId,
                listener.position, listener.forward, listener.up);
            if (_sources == null) return;
            for (int i = 0; i < _sources.Length; i++)
            {
                var s = _sources[i];
                if (s == null) continue;
                AcousticEngine.SetGameObjectPosition(SourceId(i), s.position, s.forward, s.up);
            }
        }

        private void OnDisable()
        {
            if (AcousticEngine.IsAudioInitialized)
            {
                if (_sources != null)
                    for (int i = 0; i < _sources.Length; i++)
                        AcousticEngine.UnregisterGameObject(SourceId(i));
                AcousticEngine.UnregisterGameObject(ListenerObjId);
                AcousticEngine.ShutdownAudio();
            }
            _audioReady = false;

            _engine?.Dispose();
            _engine = null;
        }

        private void Update()
        {
            if (_engine == null || !_engine.IsValid) return;
            if (listener == null || source == null) return;

            _fps = Mathf.Lerp(_fps, 1f / Mathf.Max(1e-4f, Time.unscaledDeltaTime), 0.1f);

            // モード切替・音源選択（Tab）・空間化切替（H）。
            if (Input.GetKeyDown(KeyCode.Alpha1)) mode = VisualizationMode.PerceivedPaths;
            if (Input.GetKeyDown(KeyCode.Alpha2)) mode = VisualizationMode.Directivity;
            if (Input.GetKeyDown(KeyCode.Tab)) CycleSelectedSource();
            if (Input.GetKeyDown(KeyCode.H)) { useHrtf = !useHrtf; ApplySpatializationState(); }
            if (Input.GetKeyDown(KeyCode.R)) _soloReverb = !_soloReverb;  // 残響のみモード
            if (Input.GetKeyDown(KeyCode.O)) _testObstruction = !_testObstruction;  // 切り分け: LPF経路
            if (Input.GetKeyDown(KeyCode.T)) _testReverbFull = !_testReverbFull;    // 切り分け: 残響経路
            if (Input.GetKeyDown(KeyCode.P)) ToggleAudioPause();                    // 音停止→残響の尾を聴く

            Transform sel = SelectedSource();   // モニター対象の音源

            // リスナー（プレイヤー）操作。WASD移動＋右クリックで見回す。
            HandleListenerInput();

            // 音源の手動操作（矢印キー）。選択中の音源を動かす。
            HandleSourceInput(sel);

            // リスナーを選択中の音源の周りで自動周回させ、定位/遮蔽/指向性の変化を見せる。
            if (autoOrbitListener)
            {
                _orbitAngle += orbitSpeed * Time.deltaTime;
                float rad = _orbitAngle * Mathf.Deg2Rad;
                listener.position = sel.position +
                    new Vector3(Mathf.Cos(rad), 0f, Mathf.Sin(rad)) * orbitRadius;
                // 常に音源の方を向く（左右の定位が分かりやすいように）。
                Vector3 look = sel.position - listener.position; look.y = 0f;
                if (look.sqrMagnitude > 1e-6f)
                    listener.rotation = Quaternion.LookRotation(look.normalized, Vector3.up);
            }

            RebuildGeometry();

            _acStopwatch.Restart();   // ここから音響計算の所要時間を計測

            // --- 共有パス：リスナーレイ1回で全音源の遮蔽を計算（音源数で raycast が増えない）---
            Transform[] srcs = CurrentSources();
            int n = srcs.Length;
            int selIdx = (n > 0) ? Mathf.Clamp(SelectedIndex(), 0, n - 1) : 0;
            if (_srcPos == null || _srcPos.Length < n) _srcPos = new Vector3[n];
            if (_occTargets == null || _occTargets.Length < n) _occTargets = new float[n];
            for (int i = 0; i < n; i++) _srcPos[i] = srcs[i].position;
            if (n > 0)
                _engine.ComputeOcclusionMultiSource(
                    listener.position, _srcPos, n, _occTargets, occlusionRayCount, occlusionBounces);

            // --- 選択中の音源：モニター用の値（線色・帯域・指向性表示・面ヒートマップ）---
            _occluded = _engine.IsOccluded(listener.position, sel.position);             // 視覚の線色用(二値)
            _occlusionTarget = (n > 0) ? _occTargets[selIdx] : 0f;                       // 共有パスの結果を表示
            _engine.ComputeTransmissionBands(listener.position, sel.position, _bandGains);
            LatestBandGains = _bandGains;
            _directivityGain = AcousticEngine.ComputeDirectivity(
                sel.position, sel.forward, listener.position, sourceDirectivity, out _directivityLowpass);
            UpdateFaceReachability(sel);

            // 残響エコグラム（モニター用）。毎フレームは重いので 4 フレームに1回だけ更新。
            if ((++_echoFrameCounter & 3) == 0) UpdateEchogram(sel);

            // --- Wwise へ反映：全音源（遮蔽は共有パスの結果、指向性は音源ごとに）---
            if (AcousticEngine.IsAudioInitialized)
            {
                if (_audioReady && _sources != null)
                {
                    UpdateAudioTransforms();
                    float dt = occlusionSmoothSpeed * Time.deltaTime;
                    for (int i = 0; i < _sources.Length; i++)
                    {
                        var s = _sources[i];
                        if (s == null) continue;
                        float occTarget = (i < _occTargets.Length) ? _occTargets[i] : 0f;
                        _occSmoothed[i] = Mathf.MoveTowards(_occSmoothed[i], occTarget, dt);
                        // 指向性（向きによる音量＋背面のこもり）は音源ごとの純幾何（軽い）。
                        float gain = AcousticEngine.ComputeDirectivity(
                            s.position, s.forward, listener.position, sourceDirectivity, out float lp);
                        // occlusion=壁の遮蔽 / obstruction=指向性のこもり（別ノブ）。
                        // 切り分けテスト中は obstruction を 1.0 に張る（LPF経路の生死確認）。
                        float obstruction = _testObstruction ? 1.0f : lp;
                        AcousticEngine.SetObstructionOcclusion(SourceId(i), ListenerObjId, obstruction, _occSmoothed[i]);
                        // 残響のみモードはドライ出力を0に（リバーブ送りは別経路で残る）。
                        AcousticEngine.SetEmitterListenerVolume(SourceId(i), ListenerObjId,
                                                                _soloReverb ? 0f : gain);
                    }
                }
                // 切り分けテスト：残響を強制ON（geometry算出値を上書き）。出れば RoomVerb 経路は生きてる。
                if (_testReverbFull)
                {
                    // 切り分けは"極端"に振る：wet最大＋decay最大(大聖堂級)で、ONにした瞬間
                    // 音楽全体が巨大ホールに化けるのが聴き逃せないレベルにする。
                    AcousticEngine.SetRTPCValue("ReverbWet", 100f);
                    AcousticEngine.SetRTPCValue("ReverbDecay", 3.0f);
                }
                // イベント処理を進めるため毎フレーム必須。
                AcousticEngine.RenderAudio();
            }

            // 出力(マスターバス)の左右レベルを取得し、波形履歴に積む（Output Monitor タブ用）。
            AcousticEngine.GetOutputLevels(out _outL, out _outR);
            if (_outHistL == null)
            {
                _outHistL = new float[OutHistLen];
                _outHistR = new float[OutHistLen];
                OutHistoryL = _outHistL;
                OutHistoryR = _outHistR;
            }
            _outHistL[_outHistIdx] = _outL;
            _outHistR[_outHistIdx] = _outR;
            _outHistIdx = (_outHistIdx + 1) % OutHistLen;
            OutHistoryHead = _outHistIdx;

            _acStopwatch.Stop();
            _acousticMs = _acStopwatch.Elapsed.TotalMilliseconds;
        }

        // 選択中の音源を巡回（Tab）。再生中は _sources、未再生は source/extraSources の数で。
        private void CycleSelectedSource()
        {
            int n = (_sources != null && _sources.Length > 0) ? _sources.Length
                                                              : Mathf.Max(1, CurrentSources().Length);
            selectedSource = (selectedSource + 1) % n;
        }

        // モニター対象の音源。再生中は _sources から、未再生は source にフォールバック。
        private Transform SelectedSource()
        {
            if (_sources != null && _sources.Length > 0)
                return _sources[SelectedIndex()];
            return source;
        }

        // 選択中音源の _sources 内インデックス（範囲クランプ）。
        private int SelectedIndex()
        {
            if (_sources != null && _sources.Length > 0)
                return Mathf.Clamp(selectedSource, 0, _sources.Length - 1);
            return 0;
        }

        // 現在の音源一覧。再生中は確定済み _sources、未再生は source/extraSources から組む。
        private Transform[] CurrentSources()
        {
            if (_sources != null && _sources.Length > 0) return _sources;
            var list = new System.Collections.Generic.List<Transform>();
            if (source != null) list.Add(source);
            if (extraSources != null)
                foreach (var s in extraSources)
                    if (s != null && s != source) list.Add(s);
            return list.ToArray();
        }

        // 矢印キー=水平 / PageUp=上昇 / PageDown=下降 で「選択中の」音源を動かす。
        // （WASD はリスナー側に譲ったので、音源は矢印キーに分離している）
        private void HandleSourceInput(Transform target)
        {
            if (target == null) return;
            Vector3 move = Vector3.zero;
            if (Input.GetKey(KeyCode.UpArrow)) move.z += 1f;
            if (Input.GetKey(KeyCode.DownArrow)) move.z -= 1f;
            if (Input.GetKey(KeyCode.RightArrow)) move.x += 1f;
            if (Input.GetKey(KeyCode.LeftArrow)) move.x -= 1f;
            if (Input.GetKey(KeyCode.PageUp)) move.y += 1f;
            if (Input.GetKey(KeyCode.PageDown)) move.y -= 1f;

            if (move != Vector3.zero)
                target.position += move.normalized * moveSpeed * Time.deltaTime;
        }

        // リスナー（プレイヤー）を動かす。
        //   右クリック押しながらマウス: 見回す（ヨー＝水平 / ピッチ＝垂直）
        //   WASD: リスナーの向き基準で水平移動 / Space=上昇 / LeftShift=下降
        // 向きが変わると左右の定位も変わるので、見回しを入れている。
        private void HandleListenerInput()
        {
            // --- 向き（右クリック中のみマウスで回す）---
            if (Input.GetMouseButton(1))
            {
                float yaw = Input.GetAxis("Mouse X") * lookSpeed;
                float pitch = -Input.GetAxis("Mouse Y") * lookSpeed;
                listener.Rotate(Vector3.up, yaw, Space.World);     // 水平回転（ワールドY軸）
                listener.Rotate(listener.right, pitch, Space.World); // 上下回転（自分の右軸）
                autoOrbitListener = false;  // 手動で見回したら自動周回オフ
            }

            // --- 移動（リスナーの向きを水平に投影した基準で）---
            Vector3 fwd = listener.forward; fwd.y = 0f; fwd = fwd.normalized;
            // fwd を水平に -90度ヨー回転したのが右方向（Unity 左手系）。
            Vector3 right = new Vector3(fwd.z, 0f, -fwd.x);

            Vector3 move = Vector3.zero;
            if (Input.GetKey(KeyCode.W)) move += fwd;
            if (Input.GetKey(KeyCode.S)) move -= fwd;
            if (Input.GetKey(KeyCode.D)) move += right;
            if (Input.GetKey(KeyCode.A)) move -= right;
            if (Input.GetKey(KeyCode.Space)) move += Vector3.up;
            if (Input.GetKey(KeyCode.LeftShift)) move += Vector3.down;

            if (move != Vector3.zero)
            {
                listener.position += move.normalized * listenerMoveSpeed * Time.deltaTime;
                autoOrbitListener = false;  // 手動で動いたら自動周回オフ
            }
        }

        // 音源面を cols×rows に分割し、各点→リスナーの直接到達量グリッドを計算して
        // 静的公開する（別タブ Source Path Monitor が読む）。面の向きは選択音源 src の
        // forward(法線)・right(横軸) に従う。
        private void UpdateFaceReachability(Transform src)
        {
            int cols = Mathf.Clamp(faceGridCols, 1, 128);
            int rows = Mathf.Clamp(faceGridRows, 1, 128);
            int need = cols * rows;
            if (_faceGrid == null || _faceGrid.Length < need) _faceGrid = new float[need];

            float halfW, halfH;
            if (sourceShape == AcousticEngine.SourceShape.Disk)
                halfW = halfH = Mathf.Max(0f, sourceRadius);
            else if (sourceShape == AcousticEngine.SourceShape.Rect)
            { halfW = Mathf.Max(0f, sourceHalfSize.x); halfH = Mathf.Max(0f, sourceHalfSize.y); }
            else
                halfW = halfH = 0f;  // 点音源（面の広がり無し）

            // ネイティブ呼び出しはDLL未更新だと EntryPointNotFound を投げ得る。
            // ここで握り潰して、後続（Wwise反映など）を巻き添えで止めない。失敗理由はタブへ。
            try
            {
                _engine.ComputeFaceReachability(
                    src.position, src.forward, src.right,
                    halfW, halfH, sourceShape, listener.position, _faceGrid, cols, rows);

                LatestFaceReachability = _faceGrid;
                FaceGridCols = cols;
                FaceGridRows = rows;
                LatestFaceShape = sourceShape;
                FaceStatus = "OK";
            }
            catch (System.EntryPointNotFoundException)
            {
                FaceStatus = "ネイティブDLLが古い：ComputeFaceReachability 未搭載。再ビルド＋再デプロイが必要。";
            }
            catch (System.DllNotFoundException)
            {
                FaceStatus = "AcousticEngine.dll が見つからない（Plugins/x86_64 を確認）。";
            }
            catch (System.Exception e)
            {
                FaceStatus = "計算で例外: " + e.Message;
            }
        }

        // 選択音源→リスナーのエコグラム（到達時間ビン）を計算して静的公開する（別タブ用）。
        private void UpdateEchogram(Transform src)
        {
            if (src == null) return;
            int bins = Mathf.Clamp(echogramBins, 1, 512);
            if (_echogram == null || _echogram.Length < bins) _echogram = new float[bins];
            _echoSrc[0] = src.position;
            try
            {
                _engine.ComputeEchogram(listener.position, _echoSrc, 1,
                    echogramRays, echogramBounces, _echogram, bins, echogramBinMs * 0.001f, 343f);
                LatestEchogram = _echogram;
                EchogramBins = bins;
                EchogramBinMs = echogramBinMs;
                UpdateReverbFromEchogram(bins);   // 残響パラメータ算出 → RTPC
            }
            catch (System.EntryPointNotFoundException) { /* 古いDLL：再ビルドで有効化 */ }
            catch (System.Exception) { }
        }

        // エコグラムから「wet量（反射の割合）」と「残響時間 RT60（尾の長さ）」を算出し、
        // RTPC で Wwise のリバーブ（RoomVerb）に当てる。これで部屋の形・材質に応じて
        // 響きが物理的に変わる（geometry 駆動）。RTPC 範囲は Wwise 側で対応付ける想定：
        //   ReverbWet   : 0..100（反射割合×100。送り量/wet に紐付け）
        //   ReverbDecay : 秒（尾の長さ。RoomVerb の Decay Time に紐付け）
        private void UpdateReverbFromEchogram(int bins)
        {
            float peak = 0f, total = 0f;
            for (int k = 0; k < bins; k++) { float e = _echogram[k]; if (e > peak) peak = e; total += e; }
            if (peak <= 0f) return;

            float tail = Mathf.Max(0f, total - peak);                 // 反射成分（直接音ピークを除く）
            float wet = Mathf.Clamp01(tail / Mathf.Max(total, 1e-6f)); // 反射の割合 0..1
            float floor = peak * 0.001f;                               // -60dB
            int tailBin = 0;
            for (int k = 0; k < bins; k++) if (_echogram[k] > floor) tailBin = k;
            float rt60 = (tailBin + 1) * echogramBinMs * 0.001f;       // 尾の長さ(秒)

            // 時間平滑化（ジッタ防止）。
            _reverbWet = Mathf.Lerp(_reverbWet, wet, 0.2f);
            _reverbDecay = Mathf.Lerp(_reverbDecay, rt60, 0.2f);

            AcousticEngine.SetRTPCValue("ReverbWet", _reverbWet * 100f);
            AcousticEngine.SetRTPCValue("ReverbDecay", _reverbDecay);
        }

        private void RebuildGeometry()
        {
            _engine.ClearGeometry();
            if (walls == null) return;
            foreach (var w in walls)
            {
                if (w == null) continue;
                Vector3 half = w.lossyScale * 0.5f;  // Cube は 1x1x1 メッシュ
                _engine.AddBox(w.position, half);
            }
        }

        private void OnDrawGizmos()
        {
            bool live = Application.isPlaying && _engine != null && _engine.IsValid;
            if (listener == null) return;
            Transform sel = SelectedSource();
            if (sel == null) return;

            // --- 全音源への遮蔽線：緑=見通せる / 赤=遮蔽。選択中は球を大きく ---
            foreach (var s in CurrentSources())
            {
                if (s == null) continue;
                bool occ = live && _engine.IsOccluded(listener.position, s.position);
                Gizmos.color = occ ? Color.red : Color.green;
                Gizmos.DrawLine(listener.position, s.position);
                Gizmos.DrawSphere(s.position, s == sel ? 0.28f : 0.16f);
            }

            // 選択中の音源の面枠（円/矩形）。面音源の物理的な広がりを示す。
            DrawSourceFace(sel);

            // 指向性モード：選択音源の指向特性を回転体で描く（再生不要・幾何のみ）。
            if (mode == VisualizationMode.Directivity)
            {
                DrawDirectivityLobe(sel);
                return;
            }

            if (!live) return;

            int cap = Mathf.Max(2, maxBounces + 2);
            if (_path == null || _path.Length < cap) _path = new Vector3[cap];

            DrawPerceivedPaths(sel);
        }

        // 音源の指向特性を「forward 軸まわりの回転体（バルーン）」として描く。
        // 各方向のゲインはエンジンの ComputeDirectivity をそのまま使うので、
        // 実際に聞こえる指向性と描画が必ず一致する。
        // 回転対称（ゲインは forward との角度θだけに依存）なので、θごとに1回だけ
        // ゲインを計算し、それを forward まわりに回して曲面を作る＝軽い。
        private void DrawDirectivityLobe(Transform src)
        {
            Vector3 c = src.position;
            Vector3 fwd = src.forward.normalized;
            if (fwd == Vector3.zero) fwd = Vector3.forward;

            // forward に直交する2軸 u, v（回転の基底）。
            Vector3 u = Vector3.Cross(fwd, Vector3.up);
            if (u.sqrMagnitude < 1e-6f) u = Vector3.Cross(fwd, Vector3.right);
            u.Normalize();
            Vector3 v = Vector3.Cross(fwd, u).normalized;

            const int RINGS = 18;  // θ方向（0=正面 .. π=背面）の分割数
            const int SEG = 24;    // forward まわりの回転分割数
            float scale = Mathf.Max(0.01f, directivityGizmoScale);

            // θごとのゲインを1回だけ取得（仮想リスナーを各方向に置いて計算）。
            var g = new float[RINGS + 1];
            for (int i = 0; i <= RINGS; i++)
            {
                float theta = Mathf.PI * i / RINGS;
                Vector3 dir = Mathf.Cos(theta) * fwd + Mathf.Sin(theta) * u;
                g[i] = AcousticEngine.ComputeDirectivity(
                    c, fwd, c + dir, sourceDirectivity, out _);
            }

            // 回転体上の点。i=θ, j=回転角φ。半径 = ゲイン×scale。
            Vector3 Point(int i, int j)
            {
                float theta = Mathf.PI * i / RINGS;
                float phi = 2f * Mathf.PI * j / SEG;
                Vector3 dir = Mathf.Cos(theta) * fwd
                            + Mathf.Sin(theta) * (Mathf.Cos(phi) * u + Mathf.Sin(phi) * v);
                return c + dir * (g[i] * scale);
            }

            // ワイヤーフレーム（リング＝同θ / メリディアン＝同φ）。
            var lo = new Color(0.10f, 0.20f, 0.45f, 0.5f);  // 弱い向き：暗い青
            var hi = new Color(0.20f, 1.00f, 0.60f, 0.9f);  // 強い向き：明るい緑
            for (int i = 0; i < RINGS; i++)
            {
                Gizmos.color = Color.Lerp(lo, hi, 0.5f * (g[i] + g[i + 1]));
                for (int j = 0; j < SEG; j++)
                {
                    Vector3 a = Point(i, j);
                    Gizmos.DrawLine(a, Point(i, (j + 1) % SEG));  // リング
                    Gizmos.DrawLine(a, Point(i + 1, j));          // メリディアン
                }
            }

            // 主放射方向の矢印（黄）。長さは正面ゲインに比例。
            Gizmos.color = Color.yellow;
            DrawArrow(c, fwd, scale * Mathf.Max(0.4f, g[0]));
        }

        // 音源面（円/矩形）の枠を描く。法線=src.forward、横軸=src.right。
        // 点音源のときは描かない（広がりが無いため）。
        private void DrawSourceFace(Transform src)
        {
            if (sourceShape == AcousticEngine.SourceShape.Point) return;

            Vector3 c = src.position;
            Vector3 n = src.forward.normalized;
            Vector3 right = src.right.normalized;
            Vector3 up = Vector3.Cross(n, right).normalized;

            Gizmos.color = new Color(1f, 0.85f, 0.2f, 0.9f);  // 面の枠：黄
            if (sourceShape == AcousticEngine.SourceShape.Disk)
            {
                float r = Mathf.Max(0f, sourceRadius);
                const int SEG = 32;
                Vector3 prev = c + right * r;
                for (int i = 1; i <= SEG; i++)
                {
                    float a = 2f * Mathf.PI * i / SEG;
                    Vector3 p = c + (right * Mathf.Cos(a) + up * Mathf.Sin(a)) * r;
                    Gizmos.DrawLine(prev, p);
                    prev = p;
                }
            }
            else  // Rect
            {
                float hw = Mathf.Max(0f, sourceHalfSize.x);
                float hh = Mathf.Max(0f, sourceHalfSize.y);
                Vector3 p0 = c + right * hw + up * hh;
                Vector3 p1 = c + right * hw - up * hh;
                Vector3 p2 = c - right * hw - up * hh;
                Vector3 p3 = c - right * hw + up * hh;
                Gizmos.DrawLine(p0, p1); Gizmos.DrawLine(p1, p2);
                Gizmos.DrawLine(p2, p3); Gizmos.DrawLine(p3, p0);
            }
        }

        // 簡易な矢印（線＋矢じり）。
        private static void DrawArrow(Vector3 origin, Vector3 dir, float len)
        {
            Vector3 tip = origin + dir * len;
            Gizmos.DrawLine(origin, tip);

            Vector3 right = Vector3.Cross(dir, Vector3.up);
            if (right.sqrMagnitude < 1e-6f) right = Vector3.Cross(dir, Vector3.right);
            right.Normalize();
            Vector3 up = Vector3.Cross(dir, right).normalized;

            float h = len * 0.15f;
            Gizmos.DrawLine(tip, tip - dir * h + right * h * 0.6f);
            Gizmos.DrawLine(tip, tip - dir * h - right * h * 0.6f);
            Gizmos.DrawLine(tip, tip - dir * h + up * h * 0.6f);
            Gizmos.DrawLine(tip, tip - dir * h - up * h * 0.6f);
        }

        // 選択音源に「届く」反射経路だけ描く。届かないレイは隠す。
        // 反射点から音源が見通せれば、そこに音が回り込んで届くとみなす。
        private void DrawPerceivedPaths(Transform srcT)
        {
            Vector3 origin = listener.position;
            Vector3 src = srcT.position;
            var reachColor = new Color(0.2f, 1f, 0.3f, 0.8f);  // 届く: 緑

            for (int i = 0; i < rayCount; i++)
            {
                Vector3 dir = FibonacciSphereDir(i, rayCount);
                int n = _engine.TraceReflectionPath(origin, dir, rayMaxDistance, maxBounces, _path);

                // 反射点(内部の点)から音源が見通せる最初の点を探す。
                int deliver = -1;
                for (int p = 1; p <= n - 2; p++)
                {
                    // 壁面から音源方向へ少し浮かせて自己遮蔽を避ける。
                    Vector3 from = _path[p] + (src - _path[p]).normalized * 0.02f;
                    if (!_engine.IsOccluded(from, src))
                    {
                        deliver = p;
                        break;
                    }
                }

                if (deliver < 0) continue;  // どこからも届かない → 非表示

                // リスナー → 反射点 までの経路。
                Gizmos.color = reachColor;
                for (int seg = 0; seg < deliver; seg++)
                    Gizmos.DrawLine(_path[seg], _path[seg + 1]);
                // 反射点 → 音源（届く区間）。
                Gizmos.DrawLine(_path[deliver], src);

                // 反射点の印。
                Gizmos.color = Color.yellow;
                for (int p = 1; p <= deliver; p++)
                    Gizmos.DrawCube(_path[p], Vector3.one * 0.1f);
            }
        }

        // フィボナッチ球: i 番目の方向（球面上にほぼ均等な N 方向）。
        private static Vector3 FibonacciSphereDir(int i, int n)
        {
            float k = i + 0.5f;
            float phi = Mathf.Acos(1f - 2f * k / n);
            float theta = Mathf.PI * (1f + Mathf.Sqrt(5f)) * k;
            float s = Mathf.Sin(phi);
            return new Vector3(s * Mathf.Cos(theta), Mathf.Cos(phi), s * Mathf.Sin(theta));
        }


        private void OnGUI()
        {
            var style = new GUIStyle(GUI.skin.label) { fontSize = 16 };
            string wwise = AcousticEngine.IsWwiseAvailable
                ? (AcousticEngine.IsAudioInitialized ? "初期化済" : "利用可") : "なし";
            string modeName = mode switch
            {
                VisualizationMode.PerceivedPaths => "1: 届く経路のみ",
                _ => "2: 指向性",
            };

            int srcCount = CurrentSources().Length;
            // 共有パス：raycast(反射トレース)は occlusionRayCount 本＝音源数に依存しない。
            // next-event(音源への接続)だけが ×音源数。これがスケールの肝。
            long raycastRays = occlusionRayCount;
            long nextEvents = (long)occlusionRayCount * srcCount;

            GUILayout.BeginArea(new Rect(12, 12, 680, 390));
            GUILayout.Label($"AcousticFlow  DLL v={AcousticEngine.NativeVersion}", style);
            GUILayout.Label($"Wwise: {wwise}", style);
            GUILayout.Label($"音声: {_audioStatus}", style);
            GUILayout.Label($"性能: {_fps:F0} FPS  音響計算 {_acousticMs:F2} ms/frame", style);
            GUILayout.Label($"共有レイ: raycast {raycastRays:N0}（音源非依存）/ 接続 {nextEvents:N0}", style);
            GUILayout.Label($"残響(geometry駆動): wet {_reverbWet * 100f:F0}%  RT {_reverbDecay:F2}s"
                            + (_soloReverb ? "  ★残響のみ(直接音ミュート)" : "  ［Rで残響のみ］"), style);
            GUILayout.Label($"音源数: {srcCount}  選択: #{selectedSource}（Tabで巡回）", style);
            GUILayout.Label($"空間化: {(useHrtf ? "HRTF" : "パンニング")}（Hで切替）", style);
            GUILayout.Label($"遮蔽(選択音源/直接): {(_occluded ? "YES（赤）" : "NO（緑）")}  量={_occlusionTarget:F2}", style);
            GUILayout.Label($"指向性[{sourceDirectivity}]  ゲイン={_directivityGain:F2}  こもり={_directivityLowpass:F2}", style);
            GUILayout.Label($"切り分け: O=obstruction強制{(_testObstruction ? "ON★" : "off")}  T=残響強制{(_testReverbFull ? "ON★" : "off")}  P=音停止{(_audioPaused ? "★(残響の尾)" : "")}", style);
            GUILayout.Label($"表示モード [{modeName}]   ※ 1/2 キーで切替", style);
            GUILayout.Label($"レイ数: {rayCount}  反射: {maxBounces}回", style);
            GUILayout.Label("操作: リスナー=WASD+右ドラッグ / 選択音源=矢印+PgUp/PgDn / Tab=音源選択 / H=HRTF / R=残響のみ", style);
            GUILayout.EndArea();
        }
    }
}
