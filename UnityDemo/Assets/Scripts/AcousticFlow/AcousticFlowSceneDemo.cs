// AcousticFlowSceneDemo.cs
// 新アーキ(2026-07) Phase1c/5：新コア(AcousticScene)で動く最小デモ（複数音源対応）。
//
//   1. シーンの BoxCollider を occluder としてインスタンス登録（geomId+OBB+matId）。
//   2. 毎フレーム、各インスタンスの transform を更新（動的ジオメトリ）。
//   3. リスナー↔各音源の遮蔽を「反射込み・共有レイ1回」で計算（役割2＝音源数非依存）。
//   4. 6帯域→広帯域(低域加重)の遮蔽量に落として Wwise(既存 static API) の Occlusion を駆動。
//   5. WASD で歩き回れる（一人称）。
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public sealed class AcousticFlowSceneDemo : MonoBehaviour
    {
        [Header("必須オブジェクト")]
        [Tooltip("リスナー（無指定ならメインカメラを使う）。")]
        public Transform listener;
        [Tooltip("主音源（0番）。回折経路の可視化はこの音源を対象にする。")]
        public Transform source;
        [Tooltip("追加音源（1番以降）。source と合わせて全音源になる。")]
        public Transform[] extraSources;
        [Tooltip("各音源の Wwise イベント名（並びは source, extraSources… の順）。")]
        public string[] sourceEvents = { "Vocal", "Guitar", "Piano", "Bass", "Drums", "Other" };

        [Header("occluder")]
        [Tooltip("ON: シーン内の BoxCollider を自動収集して壁にする（listener/source 配下は除外）。")]
        public bool autoCollectBoxColliders = true;
        [Tooltip("手動指定の occluder（autoCollect と併用可）。")]
        public BoxCollider[] extraOccluders;
        [Tooltip("occluder の材質プリセット（全 occluder 共通・最小デモ用）。")]
        public AcousticMaterialPreset occluderMaterial = AcousticMaterialPreset.Concrete;

        [Header("移動 (WASD)")]
        public bool enableMovement = true;
        public float moveSpeed = 4f;
        public float turnSpeed = 120f;
        public float mouseSensitivity = 2.5f;
        public bool firstPersonCamera = true;
        public float eyeHeight = 0f;

        [Header("回折 エッジカタログ (Phase4-B)")]
        [Tooltip("ON: リスナー中心キューブマップでシルエット稜線を拾い回折に使う（無効時は箱コーナー探索）。")]
        public bool useEdgeCatalog = true;
        [Tooltip("キューブマップ面解像度（大きいほど精密・重い。例16〜32）。")]
        public int edgeCatalogRes = 16;
        [Tooltip("カタログ更新間隔（フレーム）。リスナーが動くので数フレーム毎。")]
        public int catalogUpdateEveryFrames = 3;

        [Header("反射 (役割2)")]
        [Tooltip("ON: 壁で反射して回り込む成分も含めて遮蔽を計算（壁裏でも聞こえる）。")]
        public bool useReflections = true;
        [Tooltip("ON: 遮蔽時、音源を『エネルギーが届く方向』へ置き直す（壁越しでも開いてる側から聞こえる）。G キー切替。")]
        public bool useDirectionalSteering = true;
        [Tooltip("ステアの直接項の重み。大=音源方向に定位が張り付く / 小=反射方向へ開く。" +
                 "直接がしっかり届くほど音源方向、遠回り(回折δ)ほど開く。")]
        [Range(0f, 4f)] public float directLocalizeWeight = 1f;
        [Tooltip("この遮蔽量まではステアせず直接音最優先（音源方向）。超えた分だけステアへ切替。" +
                 "0=常にステア / 大きいほど直接優先。")]
        [Range(0f, 1f)] public float steerThreshold = 0.2f;
        [Tooltip("見かけ方向の平滑時間(秒)。臨界減衰(SmoothDamp)でこの時定数で追従。"
                 + "大きいほど滑らか＝経路が切り替わっても速度制限つきでスッと補完（大=もっさり/小=機敏）。")]
        [Range(0.02f, 0.6f)] public float directionSmoothTime = 0.18f;
        [Tooltip("反射計算のリスナーレイ本数（共有＝音源数非依存。例 256〜1024）。")]
        public int reflectionRays = 256;
        [Tooltip("反射の最大回数（例 2〜3）。")]
        public int reflectionBounces = 3;

        [Header("残響 (Phase6)")]
        [Tooltip("ON: エコグラムから RT60/wet を算出して Wwise 残響(RoomVerb)を駆動する。")]
        public bool enableReverb = true;
        [Tooltip("エコグラムの時間ビン数。bins×binMs が窓幅（例 100×10ms=1秒）。")]
        public int echogramBins = 100;
        public float echogramBinMs = 10f;
        [Tooltip("残響用レイ本数（共有）と反射回数。尾を追うので反射は多め。\n"
                 + "バウンス数が足りないと、尾の後半に届くレイが激減して包絡がガタつき、"
                 + "『残響の粒立ち』として聞こえる。必要数の目安は 尾の長さ×音速÷平均自由行程 で、"
                 + "平均自由行程 4V/S が小さい部屋（＝狭い/複雑）ほど多く要る。")]
        public int echogramRays = 512;
        public int echogramBounces = 24;
        [Tooltip("残響の更新間隔（フレーム）。重いので数フレームに1回で十分（部屋は緩変）。")]
        public int reverbUpdateEveryFrames = 4;
        [Tooltip("拡散リバーブ(RoomVerb)の wet 倍率。反響が強すぎるなら下げる（0=残響なし）。")]
        [Range(0f, 2f)] public float reverbWetScale = 0.8f;

        [Header("早期反射 (A: 仮想エミッタ)")]
        [Tooltip("ON: 各音源の主要な初期反射を像源として抽出し、像源位置に『普通の3Dボイス』を立てて"
                 + "音源と同じ音をタップゲインで鳴らす。Wwise コアの3D定位のみ使用（プラグイン不要）。"
                 + "RoomVerb の拡散残響とは別に、壁からの鏡面反射が定位付きで聞こえる。F キー切替。")]
        public bool enableEarlyReflections = true;
        [Tooltip("音源あたりの最大反射タップ数（＝像源＝仮想ボイス数）。多いほど密だがボイスを食う（例 3〜6）。"
                 + "総仮想ボイス数 = 音源数 × これ。")]
        [Range(1, 8)] public int earlyReflectTaps = 4;
        [Tooltip("早期反射抽出のレイ本数（音源ごと）。多いほど角度分解能が上がり、"
                 + "リスナー移動でタップの入れ替わりがカクつきにくい。総コスト = 音源数 × これ。"
                 + "広い部屋（壁が遠い）ほど本数を要する。")]
        public int earlyReflectRays = 512;
        [Tooltip("像源エミッタ音量の追従速度(1/秒)。小さいほどゆっくり。タップが消える瞬間に"
                 + "音量が 0 へスナップして『物っと切れる』のを防ぐため、目標へ滑らかに寄せる。")]
        [Range(1f, 40f)] public float earlyReflectFadeSpeed = 12f;
        [Tooltip("早期反射の反射回数（1〜2で初期反射のみ）。")]
        public int earlyReflectBounces = 2;
        [Tooltip("早期反射の更新間隔（フレーム）。音源ごとにレイを撒くので数フレーム毎。")]
        public int earlyReflectUpdateEveryFrames = 3;
        [Tooltip("反射タップの全体レベル倍率（線形）。大きいほど反射音が大きい。")]
        [Range(0f, 4f)] public float earlyReflectLevelScale = 1f;

        [Header("回折の二次音源 (エッジ=音源)")]
        [Tooltip("ON: 遮蔽時の回折を『エッジ＝二次音源』として、各開口の方向に仮想音源を立てて鳴らす。"
                 + "両側に開口があれば左右2音源として両方から聞こえ、リスナー移動で音量比が滑らかに変わる"
                 + "（1点合成のパッパッ切替を解消）。GTD/ホイヘンスの原理。V キー切替。")]
        public bool enableDiffractionSources = true;
        [Tooltip("音源あたりの二次音源数（＝開口クラスタ数）。2〜3で『両側から』のイメージ。")]
        [Range(1, 5)] public int diffractionSourceCount = 3;
        [Tooltip("回折二次音源の全体レベル倍率（線形）。")]
        [Range(0f, 3f)] public float diffractionLevelScale = 1f;
        [Tooltip("回折二次音源の更新間隔（フレーム）。")]
        public int diffractionUpdateEveryFrames = 2;
        [Tooltip("二次音源レベルの平滑時間(秒)。大きいほど音量比がなめらかに動く。")]
        [Range(0.02f, 0.5f)] public float diffractionLevelSmoothTime = 0.12f;
        [Tooltip("二次音源の位置(方向)の平滑時間(秒)。エッジ切替時の飛びを抑える。")]
        [Range(0.02f, 0.5f)] public float diffractionPosSmoothTime = 0.1f;

        [Header("可視化")]
        [Tooltip("ON: 反響経路（リスナーから撒いた反射レイの跳ね返り）を線で表示。R キー切替。" +
                 "※Game ビューでは上部の Gizmos ボタンを ON にすると見える。")]
        public bool showReflectionPaths = false;
        [Tooltip("表示する反射レイの本数（見やすさ優先で少なめ）。")]
        public int reflectionPathRays = 24;
        [Tooltip("表示する反射の跳ね返り回数。")]
        public int reflectionPathBounces = 3;
        [Tooltip("反射レイの最大到達距離。")]
        public float reflectionPathMaxDist = 40f;
        [Tooltip("ON: 主音源(0)の回折候補の迂回経路を全部線で表示（最短だけ水色で強調）。C キー切替。")]
        public bool showDiffractionCandidates = true;

        [Header("遮蔽チューニング")]
        [Tooltip("遮蔽量が変化する速さ（毎秒）。小さいほど滑らか。")]
        public float occlusionSmoothSpeed = 4f;
        [Tooltip("遮蔽量の上限(0..1)。分断壁で塞ぎたいなら高め、裏で消えすぎるなら下げる。")]
        [Range(0f, 1f)] public float maxOcclusion = 0.92f;
        [Tooltip("遮蔽量全体の強さ倍率。小さいほど遮蔽が緩い。")]
        [Range(0f, 2f)] public float occlusionStrength = 1f;
        [Tooltip("ON: 遮蔽を3バンドEQ(Occ_Low/Mid/High RTPC)で周波数別に鳴らす（要Wwise側EQ配線）。"
                 + "OFF: 従来の単一Occlusionスカラ。Wwise側のEQ/RTPCが未配線のうちはOFFのままにする"
                 + "（ONにするとドライ音量をEQに委ねてOcclusion=0にするため、EQ未配線だと遮蔽が効かなくなる）。")]
        public bool useBandEq = false;
        [Tooltip("3バンドEQの最大減衰(dB)。RTPCの範囲と合わせる（例 -36）。")]
        [Range(-60f, -6f)] public float occlusionEqFloorDb = -36f;
        [Tooltip("空気吸収の強さ倍率。1=物理相当（ゲーム距離だと薄め）。距離のこもりを分かりやすくするなら3〜5へ。0で無効。")]
        [Range(0f, 8f)] public float airAbsorptionScale = 1f;
        [Tooltip("距離減衰の基準距離(m)。この距離でゲイン1、以遠は 1/r（refDist/経路長）で減衰。近距離はクランプ。0で無効。")]
        [Range(0f, 5f)] public float distanceRef = 1.5f;

        [Header("音声(Wwise)")]
        [Tooltip("ON: Wwise を初期化して各音源のイベントを再生する（バンクが無ければ幾何計算のみ）。")]
        public bool enableAudio = true;
        [Tooltip("ON: HRTF（両耳の頭部伝達）で空間化。OFF: パンニング。H キーで切替。")]
        public bool useHrtf = true;
        public string[] banks = { "Init.bnk", "TokyoGeto.bnk" };
        [Tooltip("ON: 全音源を足音SE(footstepEvent)に切替。B キー。トランジェント音は回折/反射の効きが分かりやすい。")]
        public bool useFootstepSE = false;
        [Tooltip("足音SEのイベント名（バンクに含まれる想定）。")]
        public string footstepEvent = "WalkSE_01";
        [Tooltip("Enter で『音源0のみ足音ループ』に切替。足音の再トリガ間隔(秒)＝歩く周期。ワンショットSE前提で使う（footstepEventLoops=OFF時のみ有効）。")]
        public float footstepLoopSeconds = 0.5f;
        [Tooltip("ON: 足音イベントは Wwise 側で Loop 済み → 1回だけ Post して鳴らし続ける（再トリガしない）。"
                 + "OFF: ワンショット素材前提で footstepLoopSeconds 間隔で再トリガしてループ化する。"
                 + "※Loop素材にOFFを使うと再生インスタンスが積み上がって多重再生になる。")]
        public bool footstepEventLoops = true;

        private const ulong ListenerObjId = 2;
        private const ulong SourceObjIdBase = 10;
        private ulong SourceId(int i) => SourceObjIdBase + (ulong)i;
        // 早期反射の仮想エミッタ（像源）ID。音源 s × タップ t で一意。基底は音源IDと衝突しない値。
        private const ulong ReflectObjIdBase = 1000;
        private ulong ReflectId(int s, int t) => ReflectObjIdBase + (ulong)(s * _erTapCap + t);
        // 回折二次音源のエミッタID。音源 s × クラスタ k で一意。
        private const ulong DiffObjIdBase = 5000;
        private int _diffCap;  // 1音源あたりの二次音源スロット数（＝初期化時の diffractionSourceCount）
        private ulong DiffId(int s, int k) => DiffObjIdBase + (ulong)(s * _diffCap + k);

        private AcousticScene _scene;
        private int _materialId;
        private readonly List<BoxCollider> _occluders = new List<BoxCollider>();
        private readonly List<int> _instanceIds = new List<int>();

        private Transform[] _sources;   // source + extraSources
        private Vector3[] _srcPos;      // 音源位置バッファ
        private float[] _occSmoothed;   // 音源ごとの平滑化遮蔽
        private float[] _eqDbSmoothed;  // 音源ごと×3バンド(Low/Mid/High)の平滑化EQゲイン(dB)
        // #3 空気吸収：帯域別 dB/m（125..4k, 20℃相当のざっくり値）。距離×scaleで高域を削る。
        private static readonly float[] _airAbsDbPerM = { 0.0003f, 0.0008f, 0.0017f, 0.003f, 0.0085f, 0.025f };
        private readonly float[] _airTmp = new float[6];
        private float[] _srcSurvival;   // 音源ごとの帯域生存(低域加重平均)。回折二次音源のレベルに使う
        private float[] _bandsPerSource; // 音源ごとの6帯域生存（count*6）
        private float[] _arrivalDir;    // engine出力: エネルギー到来方向（count*3）
        private Vector3[] _apparentDir; // 平滑化した見かけ方向（単位）
        private Vector3[] _apparentVel; // SmoothDamp の速度状態（音源ごと）
        private Vector3[] _srcHome;     // 音源の元配置（Space で復帰）
        private Vector3 _stackPoint;    // 重ねる座標（元配置の重心）
        private bool _stacked;          // true=全音源を1点に重ねる
        private bool _wwiseMuted;       // M: Wwise音源を一括ミュート（IR畳み込みテストで楽曲を止める用）

        // 主音源(0番)の表示用。
        private readonly float[] _bands = new float[AcousticEngine.NumBands];
        private readonly float[] _diffBands = new float[AcousticEngine.NumBands];
        private float _diffDelta = -1f;
        private Vector3 _diffMid;
        // 遮蔽量の帯域加重（低域=大。低音は回り込んで残るため重い）。
        private static readonly float[] _bandWeights = { 3f, 2.5f, 2f, 1.3f, 1f, 0.8f };

        private float[] _echogram;      // 到達時間ビン（広帯域）
        private float[] _echogramBands; // 到達時間ビン×6帯域（実測された尾のIR用）
        private float _reverbWet, _reverbDecay;  // エコグラムから算出（RTPCへ）
        private int _echoCountdown = 1;
        private int _catalogCountdown = 1;

        // 早期反射(A) 用バッファ。音源ごとに像源位置＋帯域ゲインを受け、仮想エミッタへ反映。
        private Vector3[] _erImagePos;  // 像源位置（earlyReflectTaps）
        private float[] _erGain;        // 帯域ゲイン（earlyReflectTaps*6）
        private float[] _erVolSmooth;   // 像源エミッタ音量の平滑状態（音源×タップ枠）
        private int _erCountdown = 1;
        private int _erActiveTaps;      // 直近フレームで鳴っているタップ総数（表示用）
        private int _erTapCap;          // 仮想エミッタ プールの1音源あたり容量（＝初期化時の earlyReflectTaps）
        private bool _erPoolReady;      // 仮想エミッタを登録＆イベント投入済みか

        // 回折二次音源プール。音源ごとにクラスタ方向へ仮想音源を立て、音量比を滑らかに動かす。
        private Vector3[] _diffSrcPos;  // 二次音源位置（diffractionSourceCount）
        private float[] _diffSrcGain;   // 相対ゲイン（diffractionSourceCount）
        private float[] _diffLevel;     // 平滑中のレベル（音源×スロット）
        private float[] _diffLevelVel;  // SmoothDamp 速度（音源×スロット）
        private Vector3[] _diffSlotDir; // 各スロットが追っている方向（トラッキング用, 音源×スロット）
        private Vector3[] _diffSlotPos; // 平滑中の位置（音源×スロット）
        private Vector3[] _diffSlotPosVel; // 位置 SmoothDamp 速度
        private int[] _diffSlotCluster; // 割当作業用（スロット→クラスタindex, 毎フレーム）
        private int _diffCountdown = 1;
        private bool _diffPoolReady;
        private int _diffActive;        // 表示用：鳴っている二次音源総数

        // --- モニター窓向け static フィード（Editor の *MonitorWindow が読む） ---
        public const int OutHistLen = 256;
        public static float[] LatestEchogram { get; private set; }
        public static int EchogramBins { get; private set; }
        public static float EchogramBinMs { get; private set; }
        public static float[] LatestBandGains { get; private set; }  // 主音源の帯域別生存
        public static float[] OutHistoryL { get; private set; }
        public static float[] OutHistoryR { get; private set; }
        public static int OutHistoryHead { get; private set; }
        private static float[] _monBandGains;

        // Status Monitor 窓向けの状態スナップショット。PublishMonitors() で毎フレーム更新する。
        // 配列は既存インスタンス配列を参照共有（＝ゼロGC。LatestBandGains と同じ流儀）。
        public static class Status
        {
            public static bool Valid;
            public static float Fps;
            public static float AcousticMs;
            public static bool UseHrtf;
            public static bool UseSteer;
            public static string[] SourceNames;   // 音源ごとの表示名（EventFor）
            public static float[] Occlusion;      // 音源ごとの遮蔽 0..1（大=遮られてる）
            public static float[] Survival;       // 音源ごとの帯域生存（低域加重）
            public static float DiffDelta;        // 主音源の回折 迂回余剰長δ(m)。-1=迂回路なし
            public static bool UseEdgeCatalog;
            public static int EdgeCatalogCount;
            public static int DiffCandCount;
            public static bool ShowDiffCandidates;
            public static bool EarlyReflEnabled;
            public static int ErActive, ErCap;      // 早期反射 鳴動/上限
            public static bool DiffSrcEnabled;
            public static int DiffActive, DiffCap;  // 回折二次音源 鳴動/上限
            public static float[] BandsTransmit;    // 主音源 6帯域 透過
            public static float[] BandsDiffract;    // 主音源 6帯域 回折
            public static bool UseBandEq;           // 3バンドEQモードか
            public static float[] EqDb;             // 音源ごと×3(Low/Mid/High) の送出EQゲイン(dB)
            // #2 伝搬遅延の検証（主音源のIRタップ）
            public static int TapCount;
            public static float ItdgMs;             // 最初の反射までの相対遅延=ITDG
            public static float[] TapDelayMs;       // タップ遅延(ms, 直接=0)
            public static float[] TapGain;          // 広帯域ゲイン
            public static float[] TapBandGain;      // タップ×6帯域(125/250/500/1k/2k/4kHz) ゲイン（畳み込み用）
            public static float[] TapPanL;          // 段3a: タップの左右パン（等パワー）
            public static float[] TapPanR;
            public static char[] TapType;           // 'D'/'R'/'F'
            // 後期残響尾（ハイブリッド）用：エコグラム由来のRT60/wet
            // 実測された尾のIR生成用。EchogramBands[k*6 + b] = 時間ビンk・帯域b のエネルギー。
            // 古いDLL（帯域別エクスポート無し）では null。
            public static float[] EchogramBands;
            public static float EchogramBinMs;
            public static int EchogramBinCount;
            public static int EchogramVersion;      // 更新のたびに増える。再生成の判定用。
            public static float DistanceRef;        // 距離減衰の基準距離（尾を早期と同じ土俵に乗せる）
            // 残響/直接エネルギーの物理目標比 (r/r_c)²。尾の絶対レベルはこれで決める
            //   （エコグラムの尾/直接比は 2π 結合などで信用できないため、形だけ使い量はこれ）。
            public static float ReverbTargetRatio;
            // 早期↔後期の境目(mixing time)の目安 ≈ √V(ms)。広い部屋ほど遅い。
            //   これより前の反射は方向つき早期タップ、後は拡散尾として扱う。
            public static float MixingTimeMs;
            public static float RtSeconds;          // 残響RT60(秒)
            public static float Wet;                // 残響wet(0..1, tail/total)
            public static float SourceLevel;        // 主音源の直線透過(遮蔽)レベル(0..1)。残響を遮蔽で絞る用
        }
        private string[] _statusNames;  // Status.SourceNames の使い回しバッファ

        private bool _audioReady;
        private string _status = "未初期化";
        private Camera _cam;
        private float _pitch;

        private Vector3[][] _reflPaths; // 反響経路の可視化バッファ
        private int[] _reflPathLens;

        // 回折候補の可視化バッファ（主音源0）。全候補の迂回点＋δ、最短のインデックス。
        private Vector3[] _diffCandPts;
        private float[] _diffCandDelta;
        private int _diffCandCount;
        private int _diffCandMinIdx = -1;

        // #2: 主音源(0)のIRタップ（伝搬遅延の検証用）。delayMs=直接音を0とした相対遅延。
        // これがIRの生材料＝時間軸。ここが正しければ後段(IR組み立て/畳み込み)にそのまま乗る。
        private const float kSpeedOfSound = 343f;
        private int _tapUpdateEveryFrames = 3;
        private int _tapCountdown = 1;
        private float[] _tapDelayMs = new float[64];  // 直接音基準の相対遅延(ms)
        private float[] _tapGain = new float[64];      // 広帯域ゲイン(0..1, プロット/表示用)
        // タップ×6帯域ゲイン（畳み込みの音色）。エンジン出力の6帯域を潰さずそのまま渡す。
        private float[] _tapBandGain = new float[64 * AcousticEngine.NumBands];
        private float[] _tapPanL = new float[64];       // 段3a: 到来方向→左右パン（等パワー）
        private float[] _tapPanR = new float[64];
        private char[] _tapType = new char[64];        // 'D'直接 / 'R'反射 / 'F'回折
        private int _tapCount;
        private float _itdgMs;                         // 最初の反射までの相対遅延=ITDG(広さの手がかり)
        private Vector3[] _tapErPos = new Vector3[8];  // 早期反射 像源バッファ(主音源のタップ計算用)
        private float[] _tapErGain = new float[8 * 6];
        private Vector3[] _tapDiffPos = new Vector3[8];
        private float[] _tapDiffGain = new float[8];

        // パフォーマンス計測
        private readonly System.Diagnostics.Stopwatch _acStopwatch = new System.Diagnostics.Stopwatch();
        private double _acousticMs;   // 音響計算1フレームの所要時間(ms, 平滑化)
        private float _fps;           // 平滑化FPS

        private void OnEnable()
        {
            if (listener == null && Camera.main != null) listener = Camera.main.transform;

            _scene = new AcousticScene();
            if (!_scene.IsValid)
            {
                _status = "Scene 生成失敗（DLL を確認）";
                Debug.LogError("[AcousticFlowScene] AF_SceneCreate が失敗しました。");
                return;
            }

            BuildSources();

            // モニター窓向けの static バッファを用意。
            OutHistoryL = new float[OutHistLen];
            OutHistoryR = new float[OutHistLen];
            OutHistoryHead = 0;
            _monBandGains = new float[AcousticEngine.NumBands];
            LatestBandGains = _monBandGains;

            _materialId = _scene.AddMaterial(AcousticMaterial.FromPreset(occluderMaterial));
            CollectOccluders();
            RegisterInstances();
            SetupCamera();
            if (enableAudio) SetupAudio();

            _status = $"Scene OK / instances={_scene.InstanceCount} / sources={_sources.Length}"
                    + (_audioReady ? " / 再生中" : (enableAudio ? " / 音声なし" : ""));
        }

        // source + extraSources を結合して音源配列とバッファを確定する。
        private void BuildSources()
        {
            var list = new List<Transform>();
            if (source != null) list.Add(source);
            if (extraSources != null)
                foreach (var s in extraSources)
                    if (s != null && s != source && !list.Contains(s)) list.Add(s);
            _sources = list.ToArray();

            int n = Mathf.Max(1, _sources.Length);
            _srcPos = new Vector3[n];
            _occSmoothed = new float[n];
            _eqDbSmoothed = new float[n * 3];
            _srcSurvival = new float[n];
            _bandsPerSource = new float[n * AcousticEngine.NumBands];
            _arrivalDir = new float[n * 3];
            _apparentDir = new Vector3[n];
            _apparentVel = new Vector3[n];

            // 元配置を控え、重ね座標＝元配置の重心を求める（Space で切替）。
            _srcHome = new Vector3[_sources.Length];
            Vector3 sum = Vector3.zero;
            for (int i = 0; i < _sources.Length; i++)
            {
                _srcHome[i] = _sources[i] != null ? _sources[i].position : Vector3.zero;
                sum += _srcHome[i];
            }
            _stackPoint = _sources.Length > 0 ? sum / _sources.Length : Vector3.zero;
            _stacked = false;

            _echogram = new float[Mathf.Max(1, echogramBins)];
            _echogramBands = new float[_echogram.Length * AcousticEngine.NumBands];

            // 早期反射(A) バッファ。1音源分を使い回す（音源ごとに順次計算→仮想エミッタへ反映）。
            _erTapCap = Mathf.Max(1, earlyReflectTaps);
            _erImagePos = new Vector3[_erTapCap];
            _erGain = new float[_erTapCap * AcousticEngine.NumBands];
            // 像源エミッタ音量の平滑状態（音源×タップ枠）。スナップ防止。
            _erVolSmooth = new float[Mathf.Max(1, _sources.Length) * _erTapCap];

            // 回折候補の可視化バッファ（エンジンの合成上限に合わせて64）。
            _diffCandPts = new Vector3[64];
            _diffCandDelta = new float[64];

            // 回折二次音源バッファ。
            _diffCap = Mathf.Max(1, diffractionSourceCount);
            _diffSrcPos = new Vector3[_diffCap];
            _diffSrcGain = new float[_diffCap];
            _diffLevel = new float[n * _diffCap];
            _diffLevelVel = new float[n * _diffCap];
            _diffSlotDir = new Vector3[n * _diffCap];
            _diffSlotPos = new Vector3[n * _diffCap];
            _diffSlotPosVel = new Vector3[n * _diffCap];
            _diffSlotCluster = new int[_diffCap];
        }

        // 音源を「元配置」⇄「1点に重ね」で配置し直す。
        private void ApplySourceLayout()
        {
            if (_sources == null || _srcHome == null) return;
            for (int i = 0; i < _sources.Length; i++)
                if (_sources[i] != null)
                    _sources[i].position = _stacked ? _stackPoint : _srcHome[i];
        }

        private void CollectOccluders()
        {
            _occluders.Clear();
            if (autoCollectBoxColliders)
            {
                foreach (var col in FindObjectsOfType<BoxCollider>())
                {
                    if (col == null || !col.enabled) continue;
                    if (IsExcluded(col.transform)) continue;
                    _occluders.Add(col);
                }
            }
            if (extraOccluders != null)
                foreach (var col in extraOccluders)
                    if (col != null && !_occluders.Contains(col)) _occluders.Add(col);
        }

        // listener / 各音源の階層下（本人含む）は occluder から除外する。
        private bool IsExcluded(Transform t)
        {
            for (Transform p = t; p != null; p = p.parent)
            {
                if (p == listener) return true;
                if (_sources != null)
                    foreach (var s in _sources) if (p == s) return true;
            }
            return false;
        }

        private void RegisterInstances()
        {
            _instanceIds.Clear();
            foreach (var col in _occluders)
            {
                GetObb(col, out Vector3 c, out Vector3 half, out Vector3 right, out Vector3 up);
                _instanceIds.Add(_scene.AddInstanceBox(c, half, right, up, _materialId));
            }
        }

        private static void GetObb(BoxCollider col, out Vector3 center, out Vector3 half,
                                   out Vector3 right, out Vector3 up)
        {
            Transform t = col.transform;
            center = t.TransformPoint(col.center);
            Vector3 s = t.lossyScale;
            half = new Vector3(
                Mathf.Abs(col.size.x * 0.5f * s.x),
                Mathf.Abs(col.size.y * 0.5f * s.y),
                Mathf.Abs(col.size.z * 0.5f * s.z));
            right = t.right;
            up = t.up;
        }

        private void SetupCamera()
        {
            if (!firstPersonCamera) return;
            _cam = Camera.main;
            if (listener != null)
            {
                var rend = listener.GetComponentInChildren<Renderer>();
                if (rend != null) rend.enabled = false;
            }
            _pitch = 0f;
        }

        private string EventFor(int i)
        {
            if (useFootstepSE)
                return string.IsNullOrEmpty(footstepEvent) ? "WalkSE_01" : footstepEvent;
            if (sourceEvents != null && i < sourceEvents.Length && !string.IsNullOrEmpty(sourceEvents[i]))
                return sourceEvents[i];
            return "Vocal";
        }

        // Enter：音源0のみ足音ループに切替（他音源は停止）。ワンショットSEでも footstepLoopSeconds 間隔で
        // 再トリガして歩行ループにする。回折/反射の二次エミッタも一緒に鳴らすので足音が空間を通る。
        private bool _singleFootstep;
        private float _footstepTimer;

        private string FootstepEvt() => string.IsNullOrEmpty(footstepEvent) ? "WalkSE_01" : footstepEvent;

        // 音源 s とそのエミッタ群に、指定イベントを Post（回折/反射も含めて同期）。
        private void PostAllVoices(int s, string evt)
        {
            AcousticEngine.PostEvent(evt, SourceId(s));
            if (_erPoolReady) for (int t = 0; t < _erTapCap; t++) AcousticEngine.PostEvent(evt, ReflectId(s, t));
            if (_diffPoolReady) for (int k = 0; k < _diffCap; k++) AcousticEngine.PostEvent(evt, DiffId(s, k));
        }
        // 音源 s とそのエミッタ群で、指定イベントを Stop。
        private void StopAllVoices(int s, string evt)
        {
            AcousticEngine.ExecuteActionOnEvent(evt, 0, SourceId(s));  // 0 = Stop
            if (_erPoolReady) for (int t = 0; t < _erTapCap; t++) AcousticEngine.ExecuteActionOnEvent(evt, 0, ReflectId(s, t));
            if (_diffPoolReady) for (int k = 0; k < _diffCap; k++) AcousticEngine.ExecuteActionOnEvent(evt, 0, DiffId(s, k));
        }

        private void SetSingleFootstep(bool on)
        {
            if (_singleFootstep == on) return;
            _singleFootstep = on;
            if (!_audioReady) return;
            if (on)
            {
                for (int s = 0; s < _sources.Length; s++) StopAllVoices(s, EventFor(s));  // 背景を全停止
                PostAllVoices(0, FootstepEvt());   // 音源0だけ足音
                _footstepTimer = Mathf.Max(0.1f, footstepLoopSeconds);
            }
            else
            {
                StopAllVoices(0, FootstepEvt());   // 足音停止
                for (int s = 0; s < _sources.Length; s++) PostAllVoices(s, EventFor(s));  // 背景を復帰
            }
        }

        // 全音源＋各エミッタプールの再生イベントを（音楽↔足音SE）切り替える。
        // 現在のイベントを Stop → フラグ反転 → 新イベントを全ゲームオブジェクトへ Post（同期再投入）。
        private void SwitchSourceSound()
        {
            if (!_audioReady) { useFootstepSE = !useFootstepSE; return; }
            for (int s = 0; s < _sources.Length; s++) StopAllVoices(s, EventFor(s));  // 旧を全停止
            useFootstepSE = !useFootstepSE;
            if (useFootstepSE)
            {
                // 足音は音源0の一個だけ（他音源は鳴らさない）。
                PostAllVoices(0, EventFor(0));
            }
            else
            {
                // 音楽は全音源復帰。
                for (int s = 0; s < _sources.Length; s++) PostAllVoices(s, EventFor(s));
            }
        }

        private void SetupAudio()
        {
            if (!AcousticEngine.IsWwiseAvailable) { _status = "Wwise 非搭載ビルド"; return; }
            if (!AcousticEngine.InitAudio()) { _status = "Wwise 初期化失敗"; return; }

            string bankPath = System.IO.Path.Combine(Application.streamingAssetsPath, "WwiseBanks");
            AcousticEngine.SetBankPath(bankPath);
            bool banksOk = true;
            if (banks != null)
                foreach (var b in banks)
                    if (!string.IsNullOrEmpty(b)) banksOk &= AcousticEngine.LoadBank(b);
            if (!banksOk)
            {
                _status = "バンク未検出（StreamingAssets/WwiseBanks を確認）";
                Debug.LogWarning($"[AcousticFlowScene] バンク読込失敗: {bankPath}");
                return;
            }

            AcousticEngine.RegisterGameObject(ListenerObjId, "Listener");
            AcousticEngine.SetDefaultListener(ListenerObjId);
            for (int i = 0; i < _sources.Length; i++)
                AcousticEngine.RegisterGameObject(SourceId(i), "Source" + i);
            UpdateAudioTransforms();

            int playing = 0;
            for (int i = 0; i < _sources.Length; i++)
                if (AcousticEngine.PostEvent(EventFor(i), SourceId(i)) != 0) playing++;
            _audioReady = playing > 0;
            if (!_audioReady)
                Debug.LogWarning("[AcousticFlowScene] PostEvent 失敗。イベント名（sourceEvents）を確認。");

            // 早期反射の仮想エミッタ（像源ボイス）を用意。直接音と同フレームで同期投入する
            // （後からバラバラに鳴らすと拍がズレるため、初期化時にまとめて立てる）。
            if (_audioReady && enableEarlyReflections) SetupReflectionEmitters();
            if (_audioReady && enableDiffractionSources) SetupDiffractionEmitters();

            ApplySpatializationState();  // HRTF/パンニングを反映
        }

        // 回折二次音源のエミッタを登録し、各音源と同じイベントを同フレームで再生（初期はミュート）。
        private void SetupDiffractionEmitters()
        {
            if (!_audioReady || _diffPoolReady || _sources == null) return;
            for (int s = 0; s < _sources.Length; s++)
                for (int k = 0; k < _diffCap; k++)
                {
                    ulong id = DiffId(s, k);
                    AcousticEngine.RegisterGameObject(id, $"Diff{s}_{k}");
                    AcousticEngine.SetGameObjectPosition(id, listener.position, listener.forward, listener.up);
                    AcousticEngine.SetEmitterListenerVolume(id, ListenerObjId, 0f);
                    AcousticEngine.PostEvent(EventFor(s), id);  // 直接と同じ stem を同期再生
                }
            _diffPoolReady = true;
        }

        // 全回折二次音源をミュート（V で OFF にしたとき）。
        private void MuteAllDiffractionSources()
        {
            if (!_diffPoolReady) return;
            for (int s = 0; s < _sources.Length; s++)
                for (int k = 0; k < _diffCap; k++)
                    AcousticEngine.SetEmitterListenerVolume(DiffId(s, k), ListenerObjId, 0f);
            if (_diffLevel != null) System.Array.Clear(_diffLevel, 0, _diffLevel.Length);
            _diffActive = 0;
        }

        // 回折の二次音源を更新：音源ごとに遮蔽時のエッジをクラスタ（＝開口）し、各開口の方向へ
        //   仮想音源を置き、レベル＝相対ゲイン×その音源の生存×スケール。レベルは SmoothDamp で
        //   滑らかに動かすので、左右の開口の音量比がシームレスに変わる（＝パッパッ切替を解消）。
        private void UpdateDiffractionSources()
        {
            if (!_diffPoolReady || _diffSrcPos == null) return;
            _diffActive = 0;
            Vector3 lp = listener.position;
            for (int s = 0; s < _sources.Length; s++)
            {
                int n = _scene.ComputeDiffractionSources(lp, _srcPos[s], _diffSrcPos, _diffSrcGain);
                float survival = (_srcSurvival != null) ? _srcSurvival[s] : 0f;

                // クラスタ→スロットを「前フレーム方向に一番近い順」で安定割当（スロット入れ替わりの飛びを防ぐ）。
                for (int k = 0; k < _diffCap; k++) _diffSlotCluster[k] = -1;
                for (int c = 0; c < n; c++)
                {
                    Vector3 dirC = (_diffSrcPos[c] - lp).normalized;
                    int best = -1; float bestDot = -2f;
                    for (int k = 0; k < _diffCap; k++)
                    {
                        if (_diffSlotCluster[k] >= 0) continue;
                        float d = Vector3.Dot(dirC, _diffSlotDir[s * _diffCap + k]);  // 未使用スロットは0ベクトル→0
                        if (d > bestDot) { bestDot = d; best = k; }
                    }
                    if (best >= 0) _diffSlotCluster[best] = c;
                }

                for (int k = 0; k < _diffCap; k++)
                {
                    ulong id = DiffId(s, k);
                    int li = s * _diffCap + k;
                    int c = _diffSlotCluster[k];
                    float targetLvl = 0f;
                    if (c >= 0)
                    {
                        Vector3 tp = _diffSrcPos[c];
                        _diffSlotDir[li] = (tp - lp).normalized;
                        if (_diffSlotPos[li] == Vector3.zero) _diffSlotPos[li] = tp;  // 初回は snap
                        // 位置(方向＋実距離)を SmoothDamp → 手前↔奥の乗り換えも地続きに。
                        _diffSlotPos[li] = Vector3.SmoothDamp(_diffSlotPos[li], tp, ref _diffSlotPosVel[li],
                                                              Mathf.Max(0.02f, diffractionPosSmoothTime));
                        AcousticEngine.SetGameObjectPosition(id, _diffSlotPos[li], listener.forward, listener.up);
                        targetLvl = Mathf.Clamp(_diffSrcGain[c] * survival * diffractionLevelScale, 0f, 2f);
                        _diffActive++;
                    }
                    // レベルを SmoothDamp（音量比のクロスフェード）。
                    _diffLevel[li] = Mathf.SmoothDamp(_diffLevel[li], targetLvl, ref _diffLevelVel[li],
                                                      Mathf.Max(0.02f, diffractionLevelSmoothTime));
                    AcousticEngine.SetEmitterListenerVolume(id, ListenerObjId, Mathf.Clamp(_diffLevel[li], 0f, 2f));
                }
            }
        }

        // 早期反射の仮想エミッタを登録し、各音源と同じイベントを同フレームで再生（初期はミュート）。
        // 以降は毎フレーム、像源位置と出力音量だけを更新する（拍ズレ防止のため再生し直さない）。
        private void SetupReflectionEmitters()
        {
            if (!_audioReady || _erPoolReady || _sources == null) return;
            for (int s = 0; s < _sources.Length; s++)
            {
                for (int t = 0; t < _erTapCap; t++)
                {
                    ulong id = ReflectId(s, t);
                    AcousticEngine.RegisterGameObject(id, $"Refl{s}_{t}");
                    // 初期位置＝リスナー、出力音量0（次の UpdateEarlyReflections で正しく置き直す）。
                    AcousticEngine.SetGameObjectPosition(id, listener.position, listener.forward, listener.up);
                    AcousticEngine.SetEmitterListenerVolume(id, ListenerObjId, 0f);
                    AcousticEngine.PostEvent(EventFor(s), id);  // 直接と同じ stem を同期再生
                }
            }
            _erPoolReady = true;
        }

        // 全仮想エミッタをミュート（F で OFF にしたとき。ボイスは残すが無音にする）。
        private void MuteAllReflections()
        {
            if (!_erPoolReady) return;
            for (int s = 0; s < _sources.Length; s++)
                for (int t = 0; t < _erTapCap; t++)
                    AcousticEngine.SetEmitterListenerVolume(ReflectId(s, t), ListenerObjId, 0f);
            _erActiveTaps = 0;
        }

        // Wwise の空間化 State を useHrtf に合わせて設定（HRTF↔パンニング）。
        private void ApplySpatializationState()
        {
            if (!AcousticEngine.IsAudioInitialized) return;
            try { AcousticEngine.SetState("Spatialization", useHrtf ? "HRTF" : "Panning"); }
            catch (System.EntryPointNotFoundException) { /* 古いDLLでは無視 */ }
        }

        private void HandleMovement()
        {
            float dt = Time.deltaTime;
            float yaw = 0f;
            if (Input.GetKey(KeyCode.Q)) yaw -= turnSpeed * dt;
            if (Input.GetKey(KeyCode.E)) yaw += turnSpeed * dt;
            if (Input.GetMouseButton(1))
            {
                yaw += Input.GetAxis("Mouse X") * mouseSensitivity;
                _pitch = Mathf.Clamp(_pitch - Input.GetAxis("Mouse Y") * mouseSensitivity, -80f, 80f);
            }
            if (Mathf.Abs(yaw) > 0f) listener.Rotate(0f, yaw, 0f, Space.World);

            float x = Input.GetAxisRaw("Horizontal");
            float z = Input.GetAxisRaw("Vertical");
            if (x != 0f || z != 0f)
            {
                Vector3 fwd = listener.forward; fwd.y = 0f; fwd.Normalize();
                Vector3 right = listener.right; right.y = 0f; right.Normalize();
                Vector3 move = (fwd * z + right * x).normalized;
                float speed = moveSpeed * (Input.GetKey(KeyCode.LeftShift) ? 2.5f : 1f);
                listener.position += move * speed * dt;
            }
        }

        private void Update()
        {
            if (_scene == null || !_scene.IsValid || listener == null || _sources == null) return;
            if (_sources.Length == 0) return;

            // Space：音源を「元配置」⇄「1点に重ね」でトグル。
            if (Input.GetKeyDown(KeyCode.Space)) { _stacked = !_stacked; ApplySourceLayout(); }
            // H：HRTF↔パンニング切替。
            if (Input.GetKeyDown(KeyCode.H)) { useHrtf = !useHrtf; ApplySpatializationState(); }
            // G：方向ステアリング（到来方向で置き直す）ON/OFF。
            if (Input.GetKeyDown(KeyCode.G)) useDirectionalSteering = !useDirectionalSteering;
            // R：反響経路の表示 ON/OFF。
            if (Input.GetKeyDown(KeyCode.R)) showReflectionPaths = !showReflectionPaths;
            // C：回折候補経路の表示 ON/OFF。
            if (Input.GetKeyDown(KeyCode.C)) showDiffractionCandidates = !showDiffractionCandidates;
            // F：早期反射(仮想エミッタ)の ON/OFF。ON=像源ボイスを鳴らす / OFF=全ミュート。
            if (Input.GetKeyDown(KeyCode.F))
            {
                enableEarlyReflections = !enableEarlyReflections;
                if (_audioReady)
                {
                    if (enableEarlyReflections)
                    {
                        if (!_erPoolReady) SetupReflectionEmitters();  // 初回ONで遅延生成
                        _erCountdown = 1;  // 次フレームで即更新
                    }
                    else MuteAllReflections();
                }
            }
            // B：全音源を 音楽 ↔ 足音SE(WalkSE_01) に切替。
            if (Input.GetKeyDown(KeyCode.B)) SwitchSourceSound();
            // M：Wwise音源を一括ミュート/復帰（IR畳み込みテストで楽曲を止めてクリックを聞く用）。
            //   ミュート=全ボイスStop、復帰=再Post（拍は頭出しに戻る）。IrConvolver(Unity)は無関係に鳴り続ける。
            if (Input.GetKeyDown(KeyCode.M))
            {
                _wwiseMuted = !_wwiseMuted;
                if (_audioReady)
                    for (int s = 0; s < _sources.Length; s++)
                        if (_wwiseMuted) StopAllVoices(s, EventFor(s));
                        else PostAllVoices(s, EventFor(s));
            }
            // Enter：音源0のみ足音ループ に切替。
            if (Input.GetKeyDown(KeyCode.Return) || Input.GetKeyDown(KeyCode.KeypadEnter))
                SetSingleFootstep(!_singleFootstep);
            // V：回折の二次音源（エッジ＝音源）の ON/OFF。
            if (Input.GetKeyDown(KeyCode.V))
            {
                enableDiffractionSources = !enableDiffractionSources;
                if (_audioReady)
                {
                    if (enableDiffractionSources)
                    {
                        if (!_diffPoolReady) SetupDiffractionEmitters();
                        _diffCountdown = 1;
                    }
                    else MuteAllDiffractionSources();
                }
            }

            _fps = Mathf.Lerp(_fps, 1f / Mathf.Max(1e-4f, Time.unscaledDeltaTime), 0.1f);

            if (enableMovement) HandleMovement();

            // 1) 動的ジオメトリ更新（動いた分だけ）。
            for (int i = 0; i < _occluders.Count; i++)
            {
                var col = _occluders[i];
                if (col == null || _instanceIds[i] < 0) continue;
                GetObb(col, out Vector3 c, out Vector3 half, out Vector3 right, out Vector3 up);
                _scene.UpdateInstance(_instanceIds[i], c, half, right, up);
            }

            // 1.5) エッジカタログを低レートで構築（リスナー中心・全音源共有）→ 回折が使う。
            if (useEdgeCatalog)
            {
                if (--_catalogCountdown <= 0)
                {
                    _catalogCountdown = Mathf.Max(1, catalogUpdateEveryFrames);
                    _scene.BuildEdgeCatalog(listener.position, edgeCatalogRes, 40f);
                }
            }
            else { _scene.ClearEdgeCatalog(); }

            // 2) 音源位置バッファ。
            for (int i = 0; i < _sources.Length; i++)
                _srcPos[i] = _sources[i] != null ? _sources[i].position : listener.position;

            // 3) 帯域別生存を音源ごとに求める（ここから音響計算の時間計測）。
            _acStopwatch.Restart();
            if (useReflections)
            {
                // 共有レイ1回で全音源へ（役割2＝音源数非依存）。_arrivalDir に到来方向も受ける。
                _scene.OcclusionReflectedMulti(listener.position, _srcPos, _sources.Length,
                                               null, _bandsPerSource, _arrivalDir,
                                               directLocalizeWeight, reflectionRays, reflectionBounces);
            }
            else
            {
                // 反射なし＝音源ごとに直接（透過⊕回折）。到来方向は真方向。
                for (int i = 0; i < _sources.Length; i++)
                {
                    _scene.ComputeTransmissionBands(_srcPos[i], listener.position, _bands);
                    _scene.ComputeDiffractionBands(_srcPos[i], listener.position, _diffBands);
                    for (int b = 0; b < AcousticEngine.NumBands; b++)
                        _bandsPerSource[i * AcousticEngine.NumBands + b] = Mathf.Max(_bands[b], _diffBands[b]);
                    Vector3 td = (_srcPos[i] - listener.position).normalized;
                    _arrivalDir[i * 3] = td.x; _arrivalDir[i * 3 + 1] = td.y; _arrivalDir[i * 3 + 2] = td.z;
                }
            }

            // 見かけ方向：遮蔽量でゲート。クリア=直接最優先(音源方向)、遮蔽が混じった分だけステアへ。
            int nbDir = AcousticEngine.NumBands;
            for (int i = 0; i < _sources.Length; i++)
            {
                // このソースの遮蔽量（低域加重）。
                float wsum = 0f, gsum = 0f;
                for (int b = 0; b < nbDir; b++)
                {
                    float w = _bandWeights[b];
                    gsum += w * _bandsPerSource[i * nbDir + b];
                    wsum += w;
                }
                float occ = Mathf.Clamp01(1f - (wsum > 0f ? gsum / wsum : 0f));
                // 閾値まではステア0（直接優先）、超えた分を 0..1 に再マップ。
                float steer = (steerThreshold < 1f)
                    ? Mathf.Clamp01((occ - steerThreshold) / (1f - steerThreshold)) : 0f;

                Vector3 trueDir = (_srcPos[i] - listener.position).normalized;
                Vector3 engDir = new Vector3(_arrivalDir[i * 3], _arrivalDir[i * 3 + 1], _arrivalDir[i * 3 + 2]);
                if (engDir.sqrMagnitude < 1e-8f) engDir = trueDir;
                engDir.Normalize();
                // クリア→音源方向 / 遮蔽→エネルギー到来方向。
                Vector3 target = Vector3.Slerp(trueDir, engDir, steer);

                // 臨界減衰の SmoothDamp で追従。経路が切り替わって target が飛んでも、速度制限つきで
                // S字に補完される（指数追従の「最初だけ速いスウッシュ」が出ない）。方向なので後で正規化。
                if (_apparentDir[i].sqrMagnitude < 1e-8f) { _apparentDir[i] = target; _apparentVel[i] = Vector3.zero; }
                else
                {
                    _apparentDir[i] = Vector3.SmoothDamp(_apparentDir[i], target, ref _apparentVel[i],
                                                         Mathf.Max(0.01f, directionSmoothTime));
                    if (_apparentDir[i].sqrMagnitude > 1e-8f) _apparentDir[i].Normalize();
                }
            }

            // 4) 反射込みの遮蔽（反射が効くと音源が明るく残る）→ Occlusion で駆動。
            //    直接が塞がれても反射が帯域を持ち上げる＝方向性のある反射音として音源が聞こえる。
            float dt = Time.deltaTime;
            int nb = AcousticEngine.NumBands;
            for (int i = 0; i < _sources.Length; i++)
            {
                float wsum = 0f, gsum = 0f;
                for (int b = 0; b < nb; b++)
                {
                    float w = _bandWeights[b];
                    gsum += w * _bandsPerSource[i * nb + b];  // 反射込みの帯域生存
                    wsum += w;
                }
                float avgGain = (wsum > 0f) ? gsum / wsum : 0f;
                _srcSurvival[i] = avgGain;  // 回折二次音源のレベル基準
                float target = Mathf.Clamp(Mathf.Clamp01(1f - avgGain) * occlusionStrength, 0f, maxOcclusion);
                _occSmoothed[i] = Mathf.MoveTowards(_occSmoothed[i], target, occlusionSmoothSpeed * dt);  // 表示/後方互換用に常時更新

                // 3バンドEQゲイン(dB)は常時計算しておく（Status Monitorのプレビュー用＋EQモードの送出用）。
                //   6帯域生存を Low/Mid/High に畳み、生存→dB。生存1.0→0dB(素通り) / 0.1→-20dB。
                if (nb >= 6)
                {
                    int bb = i * nb, e = i * 3;
                    // #3 空気吸収：直接距離ぶんの帯域別減衰を生存に乗算（遠いほど高域が削れる）。
                    AirAbsorptionBands(Vector3.Distance(listener.position, _srcPos[i]), _airTmp);
                    float lo = 0.5f * (_bandsPerSource[bb + 0] * _airTmp[0] + _bandsPerSource[bb + 1] * _airTmp[1]);
                    float mi = 0.5f * (_bandsPerSource[bb + 2] * _airTmp[2] + _bandsPerSource[bb + 3] * _airTmp[3]);
                    float hi = 0.5f * (_bandsPerSource[bb + 4] * _airTmp[4] + _bandsPerSource[bb + 5] * _airTmp[5]);
                    _eqDbSmoothed[e + 0] = Mathf.MoveTowards(_eqDbSmoothed[e + 0],
                        Mathf.Clamp(20f * Mathf.Log10(Mathf.Max(lo, 1e-3f)), occlusionEqFloorDb, 0f), 48f * dt);
                    _eqDbSmoothed[e + 1] = Mathf.MoveTowards(_eqDbSmoothed[e + 1],
                        Mathf.Clamp(20f * Mathf.Log10(Mathf.Max(mi, 1e-3f)), occlusionEqFloorDb, 0f), 48f * dt);
                    _eqDbSmoothed[e + 2] = Mathf.MoveTowards(_eqDbSmoothed[e + 2],
                        Mathf.Clamp(20f * Mathf.Log10(Mathf.Max(hi, 1e-3f)), occlusionEqFloorDb, 0f), 48f * dt);
                }

                if (_audioReady)
                {
                    if (useBandEq && nb >= 6)
                    {
                        // 周波数別の遮蔽を3バンドEQ(音源ごとRTPC)で鳴らす。ドライ音量もEQが担うので
                        // Occlusion は無効化（二重減衰回避）。※EQ未配線のうちは useBandEq=OFF のままにする。
                        int e = i * 3;
                        AcousticEngine.SetRTPCValueOnObject("Occ_Low", _eqDbSmoothed[e + 0], SourceId(i));
                        AcousticEngine.SetRTPCValueOnObject("Occ_Mid", _eqDbSmoothed[e + 1], SourceId(i));
                        AcousticEngine.SetRTPCValueOnObject("Occ_High", _eqDbSmoothed[e + 2], SourceId(i));
                        AcousticEngine.SetObstructionOcclusion(SourceId(i), ListenerObjId, 0f, 0f);
                    }
                    else
                    {
                        AcousticEngine.SetObstructionOcclusion(SourceId(i), ListenerObjId, 0f, _occSmoothed[i]);
                    }
                }
            }

            // 4.5) 早期反射(A)：音源ごとに主要な初期反射を像源として抽出し Wwise Reflect へ。
            //      拡散残響(RoomVerb)と別に、壁からの鏡面反射が『方向つきの反射音』として鳴る。
            if (enableEarlyReflections && _audioReady)
            {
                if (--_erCountdown <= 0)
                {
                    _erCountdown = Mathf.Max(1, earlyReflectUpdateEveryFrames);
                    UpdateEarlyReflections(listener.position);
                }
            }

            // 4.6) 回折の二次音源（エッジ＝音源）：遮蔽時、開口の方向へ仮想音源を立て音量比を滑らかに。
            if (enableDiffractionSources && _audioReady && _diffPoolReady)
            {
                if (--_diffCountdown <= 0)
                {
                    _diffCountdown = Mathf.Max(1, diffractionUpdateEveryFrames);
                    UpdateDiffractionSources();
                }
            }

            // 4.7) 足音ループ：ワンショット素材のときだけ一定間隔で再トリガして歩行ループ化する。
            //   footstepEventLoops=ON（Wwise側でLoop済み）のときは再トリガ禁止。1回のPostで鳴り続けるので、
            //   再トリガすると15秒ループの新インスタンスが積み上がって多重再生になる（このバグの原因だった）。
            if (_singleFootstep && _audioReady && !footstepEventLoops)
            {
                _footstepTimer -= Time.deltaTime;
                if (_footstepTimer <= 0f)
                {
                    _footstepTimer = Mathf.Max(0.1f, footstepLoopSeconds);
                    PostAllVoices(0, FootstepEvt());  // 音源0＋その回折/反射エミッタを再生（空間を通る足音）
                }
            }

            // 5) 主音源(0番)の透過/回折/回折経路を表示用に取得。
            _scene.ComputeTransmissionBands(_srcPos[0], listener.position, _bands);
            _scene.ComputeDiffractionBands(_srcPos[0], listener.position, _diffBands);
            _diffDelta = _scene.DiffractionPath(_srcPos[0], listener.position, out _diffMid);

            // 回折候補（主音源0）を全部取得し、最短δのインデックスを求める（可視化用）。
            if (showDiffractionCandidates)
            {
                _diffCandCount = _scene.DiffractionCandidates(_srcPos[0], listener.position,
                                                              _diffCandPts, _diffCandDelta);
                _diffCandMinIdx = -1;
                float md = float.MaxValue;
                for (int i = 0; i < _diffCandCount; i++)
                    if (_diffCandDelta[i] < md) { md = _diffCandDelta[i]; _diffCandMinIdx = i; }
            }
            else _diffCandCount = 0;

            // #2: 主音源のIRタップを低レートで再構築（伝搬遅延の検証。_bandsはこのフレームで更新済み）。
            if (--_tapCountdown <= 0)
            {
                _tapCountdown = Mathf.Max(1, _tapUpdateEveryFrames);
                BuildMainSourceTaps();
            }

            // 6) 残響（低レートでエコグラム→RT60/wet→Wwise RoomVerb を RTPC 駆動）。
            //    ※ Wwise 非依存の畳み込み経路もエコグラムを使うので、_audioReady では止めない。
            if (enableReverb && _echogram != null)
            {
                if (--_echoCountdown <= 0)
                {
                    _echoCountdown = Mathf.Max(1, reverbUpdateEveryFrames);
                    // まず帯域別を試す（実測された尾のIR用）。古いDLLなら広帯域版へフォールバック。
                    bool haveBands = _scene.ComputeEchogramBands(
                        listener.position, _srcPos, _sources.Length,
                        _echogramBands, _echogram.Length, echogramBinMs * 0.001f, 343f,
                        echogramRays, echogramBounces, distanceRef);
                    if (haveBands)
                    {
                        // 広帯域エコグラム（RT60/wet 用）は帯域平均として導出する。
                        for (int k = 0; k < _echogram.Length; k++)
                        {
                            float e = 0f;
                            for (int b = 0; b < nb; b++) e += _echogramBands[k * nb + b];
                            _echogram[k] = e / nb;
                        }
                        Status.EchogramBands = _echogramBands;
                    }
                    else
                    {
                        _scene.ComputeEchogram(listener.position, _srcPos, _sources.Length,
                            _echogram, _echogram.Length, echogramBinMs * 0.001f, 343f,
                            echogramRays, echogramBounces);
                        Status.EchogramBands = null;
                    }
                    Status.EchogramBinMs = echogramBinMs;
                    Status.DistanceRef = distanceRef;
                    Status.EchogramBinCount = _echogram.Length;
                    Status.EchogramVersion++;
                    UpdateReverbFromEchogram();
                    UpdateReverbTargetRatio();
                    // Reverb Monitor 窓へ。
                    LatestEchogram = _echogram;
                    EchogramBins = _echogram.Length;
                    EchogramBinMs = echogramBinMs;
                }
            }

            _acStopwatch.Stop();
            _acousticMs = _acousticMs * 0.9 + _acStopwatch.Elapsed.TotalMilliseconds * 0.1;

            if (showReflectionPaths) TraceReflectionPaths();

            PublishMonitors();
            if (_audioReady) { UpdateAudioTransforms(); AcousticEngine.RenderAudio(); }
        }

        // モニター窓向け：主音源の帯域別生存＋出力(マスターL/R)波形を static へ流す。
        private void PublishMonitors()
        {
            if (_monBandGains != null && _bandsPerSource != null)
            {
                for (int b = 0; b < AcousticEngine.NumBands; b++) _monBandGains[b] = _bandsPerSource[b];
                LatestBandGains = _monBandGains;
            }
            if (_audioReady && OutHistoryL != null)
            {
                AcousticEngine.GetOutputLevels(out float l, out float r);
                OutHistoryL[OutHistoryHead] = l;
                OutHistoryR[OutHistoryHead] = r;
                OutHistoryHead = (OutHistoryHead + 1) % OutHistLen;
            }

            // Status Monitor 窓向けスナップショット（HUD と同じ数値を別タブで見られるように）。
            Status.Valid = _scene != null && _scene.IsValid;
            Status.Fps = _fps;
            Status.AcousticMs = (float)_acousticMs;
            Status.UseHrtf = useHrtf;
            Status.UseSteer = useDirectionalSteering;
            if (_sources != null)
            {
                if (_statusNames == null || _statusNames.Length != _sources.Length)
                    _statusNames = new string[_sources.Length];
                for (int i = 0; i < _sources.Length; i++) _statusNames[i] = EventFor(i);
                Status.SourceNames = _statusNames;
            }
            Status.Occlusion = _occSmoothed;
            Status.Survival = _srcSurvival;
            Status.DiffDelta = _diffDelta;
            Status.UseEdgeCatalog = useEdgeCatalog;
            Status.EdgeCatalogCount = (_scene != null && _scene.IsValid) ? _scene.EdgeCatalogCount : 0;
            Status.DiffCandCount = _diffCandCount;
            Status.ShowDiffCandidates = showDiffractionCandidates;
            Status.EarlyReflEnabled = enableEarlyReflections;
            Status.ErActive = _erActiveTaps;
            Status.ErCap = (_sources != null) ? _sources.Length * _erTapCap : 0;
            Status.DiffSrcEnabled = enableDiffractionSources;
            Status.DiffActive = _diffActive;
            Status.DiffCap = (_sources != null) ? _sources.Length * _diffCap : 0;
            Status.BandsTransmit = _bands;
            Status.BandsDiffract = _diffBands;
            Status.UseBandEq = useBandEq;
            Status.EqDb = _eqDbSmoothed;
            Status.TapCount = _tapCount;
            Status.ItdgMs = _itdgMs;
            Status.TapDelayMs = _tapDelayMs;
            Status.TapGain = _tapGain;
            Status.TapBandGain = _tapBandGain;
            Status.TapPanL = _tapPanL;
            Status.TapPanR = _tapPanR;
            Status.TapType = _tapType;
            Status.RtSeconds = _reverbDecay;
            Status.Wet = _reverbWet;
            Status.SourceLevel = Mean6(_bands, 0);   // 主音源の直線透過(遮蔽)＝残響を遮蔽で絞る用
        }

        // #2: 主音源(0)の全経路を「タップ」に束ねる（直接/反射/回折）。IRの生材料＝時間軸。
        //   各タップ = {相対遅延(ms, 直接=0), 広帯域ゲイン, 種別}。
        //   遅延 = (経路長 − 直接距離) ÷ 音速。まずは数値で妥当性を検証（小部屋=数ms / ホール=数十ms）。
        private void BuildMainSourceTaps()
        {
            _tapCount = 0;
            _itdgMs = 0f;
            if (_scene == null || !_scene.IsValid || _srcPos == null || _srcPos.Length == 0) return;
            Vector3 lp = listener.position;
            Vector3 sp = _srcPos[0];
            float directDist = Vector3.Distance(lp, sp);
            float toMs = 1000f / kSpeedOfSound;  // 距離(m) → ms（÷c ×1000）

            int n = 0;
            // 直接タップ（基準 0ms）。6帯域透過×空気吸収 を low/mid/high にまとめる。
            WriteTapBands(n++, _bands, 0, directDist, sp, 'D', 0f);

            // 反射タップ（像源位置から経路長→遅延、6帯域ゲイン×空気吸収→3バンド）。
            int er = _scene.ComputeEarlyReflections(lp, sp, _tapErPos, _tapErGain, earlyReflectRays, earlyReflectBounces);
            for (int t = 0; t < er && n < _tapDelayMs.Length; t++)
            {
                float pl = Vector3.Distance(lp, _tapErPos[t]);  // 全経路長
                float rel = (pl - directDist) * toMs;
                if (rel < 0f) rel = 0f;
                WriteTapBands(n++, _tapErGain, t * 6, pl, _tapErPos[t], 'R', rel);
            }
            // 回折タップ（遮蔽時のみ。ゲインはスカラ=v1で3バンド一律、6帯域化は後段。空気吸収は広帯域で乗算）。
            int df = _scene.ComputeDiffractionSources(lp, sp, _tapDiffPos, _tapDiffGain);
            for (int t = 0; t < df && n < _tapDelayMs.Length; t++)
            {
                float pl = Vector3.Distance(lp, _tapDiffPos[t]);
                float rel = (pl - directDist) * toMs;
                if (rel < 0f) rel = 0f;
                AirAbsorptionBands(pl, _airTmp);
                WriteTapFlat(n++, _tapDiffGain[t] * Mean6(_airTmp, 0), pl, _tapDiffPos[t], 'F', rel);
            }
            _tapCount = n;

            // ITDG = 直接以外の最小遅延（＝広さの主要な手がかり）。
            float best = float.MaxValue;
            for (int i = 0; i < n; i++)
                if (_tapType[i] != 'D' && _tapDelayMs[i] < best) best = _tapDelayMs[i];
            _itdgMs = (best == float.MaxValue) ? 0f : best;
        }

        // 6帯域の単純平均（広帯域ゲイン表示用）。
        private static float Mean6(float[] g, int off)
        {
            float s = 0f;
            for (int b = 0; b < 6; b++) s += g[off + b];
            return s / 6f;
        }

        // #3 空気吸収：経路長ぶんの帯域別ゲイン(0..1)を outGain に書く。gain = 10^(-α·len·scale/20)。
        //   低域はほぼ1、高域ほど距離で小さくなる。scale=0 で全帯域1（無効）。
        private void AirAbsorptionBands(float pathLen, float[] outGain)
        {
            for (int b = 0; b < 6; b++)
            {
                float dB = _airAbsDbPerM[b] * pathLen * airAbsorptionScale;
                outGain[b] = Mathf.Pow(10f, -dB / 20f);
            }
        }

        // 6帯域(gOff..)×空気吸収(pathLen) を low/mid/high にまとめてタップnに書く（段2）＋到来方向パン（段3a）＋距離減衰（③）。
        private void WriteTapBands(int n, float[] g, int gOff, float pathLen, Vector3 arrival, char type, float delayMs)
        {
            AirAbsorptionBands(pathLen, _airTmp);
            float da = DistAtten(pathLen);   // ③ 絶対距離減衰（1/r）
            int nb = AcousticEngine.NumBands;
            int o = n * nb;
            float sum = 0f;
            for (int b = 0; b < nb; b++)
            {
                float v = g[gOff + b] * _airTmp[b] * da;
                _tapBandGain[o + b] = v;
                sum += v;
            }
            _tapGain[n] = sum / nb;                 // 広帯域（プロット/表示用）
            ComputePan(arrival, out _tapPanL[n], out _tapPanR[n]);
            _tapType[n] = type; _tapDelayMs[n] = delayMs;
        }

        // スカラゲインを3バンド一律で書く（回折タップ用・v1）＋到来方向パン（段3a）＋距離減衰（③）。
        private void WriteTapFlat(int n, float g, float pathLen, Vector3 arrival, char type, float delayMs)
        {
            g *= DistAtten(pathLen);   // ③ 絶対距離減衰（1/r）
            int nb = AcousticEngine.NumBands;
            int o = n * nb;
            for (int b = 0; b < nb; b++) _tapBandGain[o + b] = g;
            _tapGain[n] = g;
            ComputePan(arrival, out _tapPanL[n], out _tapPanR[n]);
            _tapType[n] = type; _tapDelayMs[n] = delayMs;
        }

        // ③ 絶対距離減衰（1/r・振幅）。distanceRef でゲイン1、以遠は refDist/pathLen。0で無効(=1)。
        private float DistAtten(float pathLen)
        {
            if (distanceRef <= 0f) return 1f;
            return distanceRef / Mathf.Max(pathLen, distanceRef);
        }

        // 到来点→リスナー左右軸への投影→等パワーパン（段3a：簡易ステレオ。前後・上下は中央＝HRTFは段3b）。
        private void ComputePan(Vector3 arrival, out float panL, out float panR)
        {
            Vector3 dir = arrival - listener.position;
            float x = (dir.sqrMagnitude > 1e-8f) ? Vector3.Dot(dir.normalized, listener.right) : 0f;  // -1..1
            float t = (x + 1f) * 0.5f;
            panL = Mathf.Sqrt(1f - t);
            panR = Mathf.Sqrt(t);
        }

        // 早期反射(A)：各音源の主要な初期反射を像源として抽出し、仮想エミッタ（像源位置の3Dボイス）へ反映。
        //   Core が像源位置(= listener + 到来方向×経路長)と6帯域ゲインを返す。
        //   像源ボイスの出力音量 = 帯域ゲインを低域加重で1つに畳んだ値 × 全体スケール。
        //   遅延は付けない（=ゼロ遅延の方向つきコピー）。初期反射は直接音と融合して空間の"広がり/音色"を作る
        //   成分なので、方向つきの同期コピーで空間印象を与えるのは妥当な近似。
        private void UpdateEarlyReflections(Vector3 listenerPos)
        {
            if (!_erPoolReady || _erImagePos == null || _erGain == null) return;
            int nb = AcousticEngine.NumBands;
            _erActiveTaps = 0;

            // 平滑係数：この更新までの経過時間ベース（更新間隔に依らず一定の追従時間）。
            float dt = Time.deltaTime * Mathf.Max(1, earlyReflectUpdateEveryFrames);
            float k = 1f - Mathf.Exp(-earlyReflectFadeSpeed * dt);

            for (int s = 0; s < _sources.Length; s++)
            {
                int n = _scene.ComputeEarlyReflections(listenerPos, _srcPos[s],
                                                       _erImagePos, _erGain,
                                                       earlyReflectRays, earlyReflectBounces);
                for (int t = 0; t < _erTapCap; t++)
                {
                    ulong id = ReflectId(s, t);
                    // 目標音量（タップが無い枠は 0）。存在する枠だけ像源位置を更新する。
                    float target = 0f;
                    if (t < n)
                    {
                        AcousticEngine.SetGameObjectPosition(id, _erImagePos[t], listener.forward, listener.up);
                        float wsum = 0f, gsum = 0f;
                        for (int b = 0; b < nb; b++)
                        {
                            float w = _bandWeights[b];
                            gsum += w * _erGain[t * nb + b];
                            wsum += w;
                        }
                        target = Mathf.Clamp((wsum > 0f ? gsum / wsum : 0f) * earlyReflectLevelScale, 0f, 2f);
                        _erActiveTaps++;
                    }
                    // 目標へ滑らかに寄せる（0 へ落ちる枠は前回位置のままフェードアウト＝スナップしない）。
                    int si = s * _erTapCap + t;
                    _erVolSmooth[si] = Mathf.Lerp(_erVolSmooth[si], target, k);
                    AcousticEngine.SetEmitterListenerVolume(id, ListenerObjId, _erVolSmooth[si]);
                }
            }
        }

        // エコグラムから wet量（反射割合）と RT60（尾の長さ）を算出し、Wwise 残響へ RTPC 送出。
        //   ReverbWet   : 0..100（反射割合×100） / ReverbDecay : 秒（尾の長さ）
        private void UpdateReverbFromEchogram()
        {
            int bins = _echogram.Length;
            float peak = 0f, total = 0f;
            for (int k = 0; k < bins; k++) { float e = _echogram[k]; if (e > peak) peak = e; total += e; }
            if (peak <= 0f) return;

            float tail = Mathf.Max(0f, total - peak);
            float wet = Mathf.Clamp01(tail / Mathf.Max(total, 1e-6f));
            float floor = peak * 0.001f;   // -60dB
            int tailBin = 0;
            for (int k = 0; k < bins; k++) if (_echogram[k] > floor) tailBin = k;
            float rt60 = (tailBin + 1) * echogramBinMs * 0.001f;

            _reverbWet = Mathf.Lerp(_reverbWet, wet, 0.2f);
            _reverbDecay = Mathf.Lerp(_reverbDecay, rt60, 0.2f);
            // RTPC は Wwise が生きているときだけ。畳み込み経路は Status 経由で読む（Wwise非依存）。
            if (!_audioReady) return;
            AcousticEngine.SetRTPCValue("ReverbWet", _reverbWet * 100f * reverbWetScale);
            AcousticEngine.SetRTPCValue("ReverbDecay", _reverbDecay);
        }

        // 尾の絶対レベルの土台＝残響/直接エネルギーの物理目標比 (r/r_c)² を出す。
        //
        // なぜ物理式か:
        //   エコグラムの「尾/直接」比は、反射側にだけ立体角積分(2π)が掛かった非対称な量で、
        //   本当の残響/直接比ではない（部屋ごとに桁でズレる原因だった）。
        //   拡散音場の古典的結果 残響/直接 = (r/r_c)²、臨界距離 r_c = 0.057√(V/RT60) は、
        //   部屋の広さ V と残響 RT60 だけで正しい比を与える。エコグラムは尾の“形”に専念させ、
        //   “量”はこの式で決める。→ tailLevel=1.0 が全部屋で物理どおりになり、部屋間で一貫する。
        //
        // V は occluder(壁)の AABB から推定する。閉じた部屋なら外形箱＝ほぼ V。
        //   ※ 開けた地面だけのシーンでは過大評価になるが、そこは RT60 が小さく残響自体が僅少。
        private void UpdateReverbTargetRatio()
        {
            if (_srcPos == null || _srcPos.Length == 0 || listener == null) return;

            // 部屋の体積を occluder の合成 AABB から推定。
            bool has = false;
            Bounds b = default;
            foreach (var col in _occluders)
            {
                if (col == null) continue;
                if (!has) { b = col.bounds; has = true; }
                else b.Encapsulate(col.bounds);
            }
            if (!has) { Status.ReverbTargetRatio = 0f; return; }
            Vector3 sz = b.size;
            float vol = Mathf.Max(1f, sz.x * sz.y * sz.z);

            float r = Mathf.Max(0.1f, Vector3.Distance(listener.position, _srcPos[0]));
            float rt = Mathf.Max(0.05f, _reverbDecay);
            // 臨界距離（メートル法, RT60[s], V[m³]）。
            float rc = 0.057f * Mathf.Sqrt(vol / rt);
            float t = (r * r) / Mathf.Max(rc * rc, 1e-4f);
            // 暴走防止のクランプ（極端な V/RT60 推定の保険）。
            Status.ReverbTargetRatio = Mathf.Clamp(t, 0f, 50f);

            // mixing time ≈ √V(ms)（Polack の目安）。広い部屋ほど反射が拡散に溶けるのが遅い＝
            // 早期タップとして扱える時間が長い。特大部屋で遠壁の反射が早期窓から漏れるのを防ぐ。
            Status.MixingTimeMs = Mathf.Clamp(Mathf.Sqrt(vol), 5f, 500f);
        }

        // 反響経路：リスナーから反射レイを撒き、跳ね返り経路をバッファに貯める（Gizmoが描く）。
        private void TraceReflectionPaths()
        {
            int rays = Mathf.Max(1, reflectionPathRays);
            int cap = Mathf.Max(2, reflectionPathBounces + 2);
            if (_reflPaths == null || _reflPaths.Length != rays || _reflPaths[0].Length != cap)
            {
                _reflPaths = new Vector3[rays][];
                _reflPathLens = new int[rays];
                for (int i = 0; i < rays; i++) _reflPaths[i] = new Vector3[cap];
            }
            for (int i = 0; i < rays; i++)
            {
                Vector3 dir = FibonacciSphereDir(i, rays);
                _reflPathLens[i] = _scene.TraceReflectionPath(
                    listener.position, dir, reflectionPathMaxDist, reflectionPathBounces, _reflPaths[i]);
            }
        }

        private static Vector3 FibonacciSphereDir(int i, int n)
        {
            float k = i + 0.5f;
            float phi = Mathf.Acos(1f - 2f * k / n);
            float theta = Mathf.PI * (1f + Mathf.Sqrt(5f)) * k;
            float s = Mathf.Sin(phi);
            return new Vector3(s * Mathf.Cos(theta), Mathf.Cos(phi), s * Mathf.Sin(theta));
        }

        private void LateUpdate()
        {
            if (!firstPersonCamera || _cam == null || listener == null) return;
            if (_cam.transform == listener) return;
            _cam.transform.position = listener.position + Vector3.up * eyeHeight;
            _cam.transform.rotation = Quaternion.Euler(_pitch, listener.eulerAngles.y, 0f);
        }

        private void UpdateAudioTransforms()
        {
            AcousticEngine.SetGameObjectPosition(ListenerObjId, listener.position, listener.forward, listener.up);
            if (_sources == null) return;
            for (int i = 0; i < _sources.Length; i++)
            {
                if (_sources[i] == null) continue;
                Vector3 pos = _sources[i].position;
                // 回折二次音源ON時は直接音を真位置に固定（回り込みの定位はエッジ音源が担う）。
                // 方向ステアリング：真位置と同じ距離を保ちつつ、向きを「エネルギーが届く方向」に置き直す。
                if (!enableDiffractionSources && useDirectionalSteering && _apparentDir != null && _apparentDir[i].sqrMagnitude > 1e-8f)
                {
                    float dist = Vector3.Distance(_sources[i].position, listener.position);
                    pos = listener.position + _apparentDir[i] * dist;
                }
                AcousticEngine.SetGameObjectPosition(SourceId(i), pos, _sources[i].forward, _sources[i].up);
            }
        }

        private void OnDisable()
        {
            if (_audioReady && AcousticEngine.IsAudioInitialized)
            {
                if (_sources != null)
                    for (int i = 0; i < _sources.Length; i++)
                        AcousticEngine.UnregisterGameObject(SourceId(i));
                // 早期反射の仮想エミッタも解除。
                if (_erPoolReady && _sources != null)
                    for (int s = 0; s < _sources.Length; s++)
                        for (int t = 0; t < _erTapCap; t++)
                            AcousticEngine.UnregisterGameObject(ReflectId(s, t));
                // 回折二次音源も解除。
                if (_diffPoolReady && _sources != null)
                    for (int s = 0; s < _sources.Length; s++)
                        for (int k = 0; k < _diffCap; k++)
                            AcousticEngine.UnregisterGameObject(DiffId(s, k));
                AcousticEngine.UnregisterGameObject(ListenerObjId);
                AcousticEngine.ShutdownAudio();
            }
            _erPoolReady = false;
            _diffPoolReady = false;
            _audioReady = false;
            _scene?.Dispose();
            _scene = null;
        }

        // 可視化：各音源の直接線（緑=素通り/赤=遮蔽）＋主音源の回折経路（青の折れ線）。
        private void OnDrawGizmos()
        {
            if (listener == null) return;

            // 反響経路（反射レイの跳ね返り）。バウンスが進むほど薄い黄色。
            if (showReflectionPaths && _reflPaths != null && _reflPathLens != null)
            {
                for (int i = 0; i < _reflPaths.Length; i++)
                {
                    int len = _reflPathLens[i];
                    for (int p = 0; p + 1 < len; p++)
                    {
                        float a = 1f - (float)p / Mathf.Max(1, reflectionPathBounces);
                        Gizmos.color = new Color(1f, 0.82f, 0.2f, Mathf.Clamp01(a * 0.9f));
                        Gizmos.DrawLine(_reflPaths[i][p], _reflPaths[i][p + 1]);
                    }
                }
            }
            if (_sources != null && _occSmoothed != null)
            {
                for (int i = 0; i < _sources.Length; i++)
                {
                    if (_sources[i] == null) continue;
                    Gizmos.color = Color.Lerp(Color.green, Color.red, _occSmoothed[i]);
                    Gizmos.DrawLine(_sources[i].position, listener.position);
                }
            }
            else if (source != null)
            {
                Gizmos.color = Color.green;
                Gizmos.DrawLine(source.position, listener.position);
            }

            Vector3 src0 = (_sources != null && _sources.Length > 0 && _sources[0] != null)
                ? _sources[0].position : (source != null ? source.position : listener.position);

            // 回折候補（主音源0）：全候補を source→P→listener で描画。最短δだけ水色で強調、他は薄い橙。
            if (showDiffractionCandidates && _diffCandCount > 0)
            {
                for (int i = 0; i < _diffCandCount; i++)
                {
                    bool isMin = (i == _diffCandMinIdx);
                    Gizmos.color = isMin ? new Color(0.1f, 0.9f, 1f, 1f)     // 最短＝水色
                                         : new Color(1f, 0.6f, 0.15f, 0.35f); // 他＝薄い橙
                    Gizmos.DrawLine(src0, _diffCandPts[i]);
                    Gizmos.DrawLine(_diffCandPts[i], listener.position);
                    Gizmos.DrawSphere(_diffCandPts[i], isMin ? 0.16f : 0.08f);
                }
            }
            else if (_diffDelta >= 0f)  // 候補表示OFF時は従来の単一最短のみ（青）。
            {
                Gizmos.color = new Color(0.2f, 0.6f, 1f);
                Gizmos.DrawLine(src0, _diffMid);
                Gizmos.DrawLine(_diffMid, listener.position);
                Gizmos.DrawSphere(_diffMid, 0.15f);
            }
        }

        private void OnGUI()
        {
            var style = new GUIStyle(GUI.skin.label) { fontSize = 13 };
            GUILayout.BeginArea(new Rect(10, 10, 480, 300), GUI.skin.box);
            GUILayout.Label($"[新コア Scene] {_status}", style);
            GUILayout.Label($"FPS: {_fps:F0}    音響計算: {_acousticMs:F2} ms/frame    " +
                            $"空間化: {(useHrtf ? "HRTF" : "パン")} (H)    " +
                            $"方向ステア: {(useDirectionalSteering ? "ON" : "OFF")} (G)", style);
            if (enableMovement)
                GUILayout.Label("操作: WASD / 右ドラッグ / QE / Shift / Space:重ね / H:HRTF / G:ステア / R:反響経路 / C:回折候補 / F:早期反射 / V:回折二次音源 / B:足音SE / Enter:足音ループ(音源0) / M:楽曲ミュート", style);
            GUILayout.Label($"音: {(_singleFootstep ? $"足音ループ・音源0のみ ({footstepEvent})" : (useFootstepSE ? $"足音SE・音源0のみ ({footstepEvent})" : "音楽ステム"))}  (B:足音切替 / Enter:足音ループ)", style);
            GUILayout.Label($"音源配置: {(_stacked ? "重ね(1点)" : "展開")}", style);
            if (enableReverb)
            {
                float wetP = _reverbWet * 100f;                 // 物理wet%（echogram: tail/total）
                float sendP = wetP * reverbWetScale;            // 送出 RTPC 値（0..100）
                GUILayout.Label(
                    $"直接:反響(物理) = {100f - wetP:F0}:{wetP:F0}   " +
                    $"送出RTPC {sendP:F0}(×{reverbWetScale:F2})   RT {_reverbDecay:F2}s", style);
            }

            if (_sources != null && _occSmoothed != null)
            {
                string occ = "遮蔽/音源: ";
                for (int i = 0; i < _sources.Length; i++)
                    occ += $"{EventFor(i)}:{_occSmoothed[i]:F2}  ";
                GUILayout.Label(occ, style);
            }
            GUILayout.Label(_diffDelta >= 0f
                ? $"主音源の回折: 迂回路あり δ={_diffDelta:F2} m（青=経路補完）"
                : "主音源の回折: なし", style);
            if (useEdgeCatalog && _scene != null && _scene.IsValid)
                GUILayout.Label($"エッジカタログ: {_scene.EdgeCatalogCount} 稜線 (res {edgeCatalogRes})", style);
            if (showDiffractionCandidates)
                GUILayout.Label($"回折候補(主音源): {_diffCandCount} 本合成 (水色=最短) (C)", style);
            GUILayout.Label(enableEarlyReflections
                ? $"早期反射(仮想エミッタ): ON  鳴動 {_erActiveTaps} 本/上限 {_sources?.Length * _erTapCap} (F)"
                : "早期反射(仮想エミッタ): OFF (F)", style);
            GUILayout.Label(enableDiffractionSources
                ? $"回折二次音源(エッジ=音源): ON  鳴動 {_diffActive} 本/上限 {_sources?.Length * _diffCap} (V)"
                : "回折二次音源(エッジ=音源): OFF (V)", style);

            if (_scene != null && _scene.IsValid)
            {
                GUILayout.Label(BandLine("主音源 透過", _bands), style);
                GUILayout.Label(BandLine("主音源 回折", _diffBands), style);
            }
            GUILayout.EndArea();
        }

        private static string BandLine(string label, float[] g)
        {
            string s = label + " ";
            for (int b = 0; b < g.Length; b++)
                s += $"{AcousticEngine.BandFreqs[b]}:{g[b]:F2} ";
            return s;
        }
    }
}
