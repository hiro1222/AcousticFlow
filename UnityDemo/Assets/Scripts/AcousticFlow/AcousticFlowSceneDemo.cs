// AcousticFlowSceneDemo.cs
// 新アーキ(2026-07) Phase1c/5：新コア(AcousticScene)で動く最小デモ（複数音源対応）。
//
//   1. シーンの BoxCollider を occluder としてインスタンス登録（geomId+OBB+matId）。
//   2. 毎フレーム、各インスタンスの transform を更新（動的ジオメトリ）。
//   3. リスナー↔各音源の遮蔽を「反射込み・共有レイ1回」で計算（役割2＝音源数非依存）。
//   4. 6帯域の生存を IR のタップ（直接/反射/回折/尾）に落として畳み込み器へ渡す。
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
        // 旧 Wwise 用のイベント名。★2026-09-03 に再生経路を消したので、もう何も読まない。
        //   BellGame 側の編集スクリプト（BellGameStages.cs）が書き込むので、型だけ残している（連絡板に掲示）。
        [HideInInspector] public string[] sourceEvents;

        [Header("occluder")]
        [Tooltip("ON: シーン内の BoxCollider と MeshCollider を自動収集して壁にする"
                 + "（listener/source 配下は除外）。\n"
                 + "MeshCollider は実形状で扱われるので、穴の空いた壁のように箱で表せない"
                 + "形もそのまま occluder にできる。")]
        public bool autoCollectBoxColliders = true;   // 名前は歴史的。MeshCollider も拾う
        [Tooltip("手動指定の occluder（autoCollect と併用可）。Box / Mesh どちらでもよい。")]
        public Collider[] extraOccluders;
        [Tooltip("部屋の材質（AcousticSurface の無い occluder 全部の既定）。部屋の響きのプリセットはここ: "
                 + "Concrete = 今の後反響 / WoodRoom = 板張りの部屋 / Stone = 石積み / Cave = 岩肌 / Snow = 新雪 / Default = 内壁。 "
                 + "Play 中に変えてよい（L キーで巡る）。反射・尾・Sabine の RT60 が一緒に変わる。")]
        public AcousticMaterialPreset occluderMaterial = AcousticMaterialPreset.Concrete;

        [Header("移動 (WASD)")]
        public bool enableMovement = true;
        public float moveSpeed = 4f;
        public float turnSpeed = 120f;
        public float mouseSensitivity = 2.5f;
        public bool firstPersonCamera = true;
        public float eyeHeight = 0f;

        [Header("回折 エッジカタログ (Phase4-B)")]
        [Tooltip("ON: リスナー中心キューブマップでシルエット稜線を拾い回折に使う（無効時は箱コーナー探索）。\n\n"
                 + "⚠ 2026-08-23 に「重複だから消せる」と判断して削除を試みたが、**く字の角で回折が死んだ**\n"
                 + "（空振り 5 → 33 箇所／到来方向が音源の方角へ張り付いた）。切っただけの実験では\n"
                 + "差が出なかったのに、消すと出た。理由は未解明。**消さないこと。**")]
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
        [Tooltip("反射の安全上限回数。実際はレイが運ぶエネルギーが床（−20 dB）を割った所で先に止まる（エンジンの reflectionRayExhausted）。")]
        public int reflectionBounces = 6;

        [Header("残響 (Phase6)")]
        [Tooltip("ON: エコグラムから RT60/wet を算出し、尾の IR と部屋のバスに渡す。")]
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

        [Header("回折の鳴らし方")]
        [Tooltip("ON: 回折タップに周波数依存の減衰を掛けず、**開口までの総経路長による距離減衰だけ**にする。\n\n"
                 + "回折が運ぶべき情報を『どこから抜けてくるか（定位）』に絞る立場。\n"
                 + "こもり（LPF）は壁を抜けてくる透過音が担う ── 材質の6帯域透過が\n"
                 + "そのまま低域寄りの性格を持つので、役割を分けられる。\n\n"
                 + "根拠: 定位の手がかりは 700Hz 以下では ITD しか無い。回折成分を強く\n"
                 + "ローパスすると『どこから来ているか』が分からなくなり、定位させたい成分の\n"
                 + "高域を削るという自己矛盾になる。広帯域のまま鳴らすのが定位の条件。\n\n"
                 + "OFF: 前川の式による6帯域減衰を掛ける（物理寄り）。A/B比較用。")]
        public bool diffractionDistanceOnly = true;

        [Tooltip("回折タップだけの距離減衰の基準距離(m)。0 で『遮蔽チューニング → Distance Ref』と同じ。\n\n"
                 + "diffractionDistanceOnly が ON のとき、回折が持つ情報は『開口の方向』と\n"
                 + "『開口までの総経路長』だけになる。つまり**この距離減衰が回折の性格そのもの**。\n"
                 + "直接音と別に調整できるようにしてある。\n\n"
                 + "大きくすると近くで頭打ちになる区間が伸びる＝回り込みが近距離で大きく聞こえる。")]
        [Range(0f, 12f)] public float diffractionDistanceRef = 0f;

        [Tooltip("回折タップの距離減衰の急峻さ。1 = 物理どおりの 1/r。\n\n"
                 + "  1 より小さい … 遠くでも落ちにくい（壁の向こうの気配を残したいとき）\n"
                 + "  1 より大きい … 早く落ちる（回り込みを近距離だけの現象にしたいとき）\n"
                 + "  0            … 距離で減衰しない\n\n"
                 + "基準距離より近い側は 1 で頭打ちなので、ここを動かしても変わらない。\n"
                 + "効くのは基準距離より遠い側だけ。")]
        [Range(0f, 2.5f)] public float diffractionDistancePower = 1f;

        [Header("タップの平滑化（歩いたときの段差を潰す）")]
        [Tooltip("遮蔽割合と透過の帯域ゲインを、この時定数(秒)で追従させる。0 で無効。\n\n"
                 + "幾何は連続でも、歩けば値は動く。実測で、戸口の影境界を跨ぐ 0.5m の間に\n"
                 + "合計レベルが 5.9dB 動く（Unity の設定・物理の基準もほぼ同じ量なので\n"
                 + "**幾何が悪いのではない**）。既存のゲーム音響が例外なく持っている平滑化を\n"
                 + "ここでも掛けて、段差を段差として鳴らさないようにする。\n\n"
                 + "対数（dB）で補間する。振幅で補間すると立ち上がりだけ速く聞こえるため。\n"
                 + "大きくすると滑らかになるが、扉が開いた瞬間の反応が鈍る。")]
        [Range(0f, 0.5f)] public float tapSmoothTime = 0.08f;

        [Header("聞こえ方の調整（LPF は透過音が主役）")]
        [Tooltip("壁を抜けてくる透過音の『こもり』の強さ。材質の6帯域カーブの**形はそのまま**に、\n"
                 + "125Hz を基準にして傾きだけを強める／弱める。\n\n"
                 + "  1   … 材質そのまま\n"
                 + "  >1  … 高域がさらに落ちる（こもる）\n"
                 + "  <1  … 平らに近づく（抜ける）\n\n"
                 + "回折タップは平坦のままなので、こもりの担当はここに集まる。\n"
                 + "『どこから聞こえるか』は回折、『壁の向こう感』は透過、と役割を分ける設計。")]
        [Range(0.2f, 3f)] public float transmissionTilt = 1f;

        [Tooltip("透過音のレベル(dB)。**『隣の部屋に移っただけで変わりすぎ』を下げる主役はここ。**\n\n"
                 + "実測: 戸口を抜ける往復で合計レベルが 16.2dB 動く（物理の基準も約15dB なので\n"
                 + "幾何は正しい）。平滑化は歩く速さの変化には効かない ── 0.5m 歩くのに 0.33秒\n"
                 + "かかるので、0.08秒の時定数では追いついてしまうため。\n\n"
                 + "ここを上げると壁の向こうの音が持ち上がり、部屋を移ったときの落差が縮む。\n"
                 + "『壁の向こう感』は音量ではなく LPF（上の2つ）が担う、という役割分担にできる。\n"
                 + "材質どうしの差（コンクリ vs 木の扉）は一律に上がるだけなので保たれる。")]
        [Range(-24f, 24f)] public float transmissionGainDb = 0f;

        [Tooltip("回折タップのレベル(dB)。**『隣の部屋に移っただけで変わりすぎ』に一番効くのはここ。**\n\n"
                 + "遮蔽中に鳴っているのは透過音ではなく回折音の方（実測: 透過 -56dB に対し\n"
                 + "回折 -22dB）。だから透過ゲインをいくら上げても合計はほとんど動かない\n"
                 + "（+24dB で振れ幅 16.1→14.0dB だけ）。ここを上げると遮蔽側が持ち上がり、\n"
                 + "戸口を跨ぐ落差がそのまま縮む。\n\n"
                 + "回折タップは平坦なので、上げても『こもり』は増えない ── 音量差を詰めつつ\n"
                 + "壁の向こう感は透過音の LPF が担う、という役割分担にできる。\n\n"
                 + "実測（戸口を跨ぐ往復の振れ幅／最大隣接差）:\n"
                 + "   +0dB  16.2dB / 5.8dB      +12dB   5.1dB / 4.9dB\n"
                 + "   +6dB  10.2dB / 2.9dB ←既定 +18dB  10.1dB / 9.7dB\n"
                 + "上げすぎると遮蔽側が見通し側より明るくなって逆に段差が増える。\n"
                 + "0 にすれば従来どおり。")]
        [Range(-24f, 24f)] public float diffractionGainDb = 6f;

        [Tooltip("透過音に足すローパスの量(dB)。125Hz で 0dB、4kHz でこの値ぶん落とす\n"
                 + "（間の帯域は対数周波数で線形に配分）。材質に依らず一律に効くので、\n"
                 + "『材質はこのままで、もう少しこもらせたい』というときはこちら。")]
        [Range(0f, 24f)] public float transmissionHighCutDb = 0f;

        [Tooltip("回折タップに足すローパスの量(dB)。既定 0（＝平坦）。\n\n"
                 + "diffractionDistanceOnly の方針では回折は平坦のままが筋（定位の手がかりを\n"
                 + "削らないため）。それでも少し丸めたいときのための逃げ道として置いてある。\n"
                 + "上げすぎると『どこから来ているか』が分からなくなる。")]
        [Range(0f, 24f)] public float diffractionHighCutDb = 0f;

        [Tooltip("【B1】いちばん強い回折タップを HRTF に載せる。\n\n"
                 + "遮蔽されると直接音タップは材質の透過まで落ちるので、実際に耳へ届いている\n"
                 + "エネルギーは開口を回り込んだ回折タップが運んでいる。HRTF が直接音にしか\n"
                 + "掛かっていないと、**聞こえている音のほうに定位の手がかりが無い**\n"
                 + "（ITD なし・前後の区別なし・上下なし）。\n\n"
                 + "実測(kemar): 等パワーパンの ITD 0us → HRTF で -1000us（右60°）。\n"
                 + "  前後の差 パンのみ -180dB（＝完全に同一）→ HRTF -1.2dB。\n"
                 + "コスト: 遮蔽されている間だけ +0.117ms/block（1音源 2.80%→3.89%）。\n"
                 + "  見通せていれば回折タップが無いので 0。")]
        public bool diffractionHrtf = true;

        [Tooltip("開口を乗り換えるときのヒステリシス(dB)。\n\n"
                 + "強さが拮抗した 2 つの開口の間で毎フレーム選択が入れ替わると、\n"
                 + "HRTF の方向が左右に振れて像が暴れる。今載せている開口をこの dB ぶん\n"
                 + "上回るまで乗り換えない。0 で無効（＝毎フレーム最強を選ぶ）。")]
        [Range(0f, 12f)] public float diffractionHrtfMarginDb = 2f;

        [Header("後期残響の方向づけ（方向プローブ）※既定オフ・下記参照")]
        [Tooltip("リスナー位置から全方向へレイを撒き、『どちらから残響が返るか』を測って"
                 + "尾の左右バランスに反映する。\n\n"
                 + "【既定オフの理由】測定は正しく効いているが、鳴らし方が非対称を運べない。\n"
                 + "  廊下での実測(125Hz): 前/後 6.43/3.55（1.8倍）, 右/左 6.18/5.07（1.2倍）\n"
                 + "  → 場には明確な方向がある。しかし L/R バランスは左右軸しか表現できず、\n"
                 + "     最終的に 0.9dB しか動かないので聞こえない。\n"
                 + "  ちゃんと鳴らすには尾IRにHRTFを焼き込む必要があるが、拡散音場の前後定位は\n"
                 + "  完璧に鳴らしても知覚されにくい（前後判別は耳介のスペクトル手がかり依存）。\n"
                 + "  空間の印象を作る『両耳間の無相関』『早期反射の方向』は既に入っている。\n\n"
                 + "測定そのものは安価で正確なので、将来『どれだけ囲まれているか』の指標として"
                 + "転用しうる（残響の量は方向と違って明確に知覚される）。詳細は DEV_LOG I章。\n\n"
                 + "★2026-08-21: 既定を ON に戻した。上の『測定は正しく効いている』は\n"
                 + "  **半分だけ正しかった**。プローブに『反射点から音源が見えるか』の項が無く、\n"
                 + "  測っていたのは『その向きへレイを飛ばすとエネルギーがどれだけ生き残るか』。\n"
                 + "  閉じた部屋で材質が同じなら向きに依らないので、く字廊下の角の向こう\n"
                 + "  （音は角の口からしか来ない）でも **6軸すべて 0.250・最大差 0.1dB** だった。\n"
                 + "  早期反射側と同じ seg 項を足して直した実測（同じ位置）:\n"
                 + "    前 0.556 / 後 0.101 / 右 0.249 / 左 0.148 → 最大差 **14.8dB**\n"
                 + "  左右軸だけしか運べない制約は残るので、効き目は控えめ（左右で 4.5dB 程度）。\n"
                 + "  前後の構造を運ぶには尾を方向バスへ分ける必要がある（未着手）。")]
        public bool enableDirectionalTail = true;
        [Tooltip("プローブが撒くレイの本数。方向分布は滑らかなので少なくてよい。")]
        [Range(8, 256)] public int probeRays = 64;
        [Tooltip("プローブのバウンス数。残響の『返り』を捉える深さ。")]
        [Range(2, 24)] public int probeBounces = 10;
        [Tooltip("プローブの更新間隔（フレーム）。")]
        public int probeUpdateEveryFrames = 8;
        [Tooltip("左右バランスの効き。0=方向づけなし / 1=測定どおり / >1=誇張。\n"
                 + "実際の左右差は控えめなので、聞き取りやすさのために少し上げてよい。")]
        [Range(0f, 3f)] public float directionalTailStrength = 1.0f;
        [Tooltip("左右バランスの追従の速さ(秒)。急に変わるとリバーブが揺れて不自然になる。")]
        [Range(0.05f, 2f)] public float directionalTailSmoothSec = 0.35f;
        [Tooltip("拡散リバーブ(RoomVerb)の wet 倍率。反響が強すぎるなら下げる（0=残響なし）。")]
        [Range(0f, 2f)] public float reverbWetScale = 0.8f;
        [Tooltip("残響/直接の物理エネルギー比を鳴らす前に圧縮する指数。\n"
                 + "1.0=物理そのまま（部屋を変えたとき残響量が過剰に振れる）\n"
                 + "0.5=平方根で圧縮（推奨。大小関係は残しつつ穏やかに）\n"
                 + "0=常に一定（部屋差は減衰時間だけで出す。DEV_LOG C-2 の極）\n"
                 + "人が感じる残響の多さはエネルギー比そのものではない（先行音効果で"
                 + "直接音が重く聞こえる）ため、物理値を知覚側へ寄せる補正。")]
        [Range(0f, 1f)] public float reverbRatioExponent = 0.5f;
        [Tooltip("残響/直接比のやわらかい上限。50 までは素通しで、そこから上はこの値へ漸近する。\n"
                 + "以前は 50 で硬くクランプしていたが、上限に達した瞬間から\n"
                 + "どれだけ離れても値が動かなくなる（＝そこで反応が消える）。\n"
                 + "50 以下にすると従来どおりの硬いクランプ。")]
        [Range(50f, 400f)] public float reverbRatioCeiling = 150f;

        [Header("部屋（幾何から自動検出）")]
        [Tooltip("残響の体積と減衰時間を混ぜる半径(m)。\n"
                 + "★部屋番号で切り替えず『まわりの何割がどの部屋か』で混ぜている。\n"
                 + "  切り替えにすると、プレイヤーが必ず通る戸口のど真ん中に段差が乗る。\n"
                 + "この半径が『何メートルかけて隣の部屋の響きへ入れ替わるか』そのもの。\n"
                 + "実測の傾き（部屋をまたぐ 18dB の変化に対して、0.1m あたり）:\n"
                 + "  0.5m→2.67dB / 1.0m→1.58dB / 2.0m→0.92dB / 3.0m→0.69dB")]
        [Range(0.3f, 4f)] public float roomBlendRadius = 2.0f;
        [Tooltip("部屋検出のボクセル一辺(m)。細かいほど狭い戸口を見分けられるがコストが増える。\n"
                 + "0.25m だと幅 0.9m の戸口がぎりぎり（半ボクセルぶんの甘さがある）。")]
        [Range(0.1f, 0.5f)] public float roomCellSize = 0.25f;
        [Tooltip("部屋を戸口で割る半径(m)。幅がこの 2 倍に満たないくびれで部屋が分かれる。\n"
                 + "★戸口の半幅以下だと部屋が戸口から外へ漏れて 0 個になる（実測: 0.6 は戸口 1.0 m まで、0.8 は 1.4 m まで、1.0 でも 2.0 m は漏れる）。\n"
                 + "いちばん広い戸口の半幅 ＋ 1 セルより大きく、部屋の狭い所の半分より小さく。0 個ならコンソールに警告が出る。\n"
                 + "曲がり角・柱・腰高の仕切りでは分かれない（くびれていないため）。")]
        [Range(0f, 2f)] public float roomSeedRadius = 0.6f;
        [Tooltip("外へ出るときの残響の量を「まわりの空間のうち部屋の中である割合」で縮める（#5）。\n"
                 + "OFF だと V/RT60 の混ぜだけになり、球が最後の部屋を離れる 2 m 外で残響が一気に消える（段）。A/B 用。")]
        public bool reverbShareFade = true;

        [Tooltip("【C1】部屋グラフの開口からポータルを自動生成する。\n\n"
                 + "ポータルは §4.3 のフレネル帯域積分の**積分範囲**で、"
                 + "これが無いと戸口はただの幾何の隙間として一般の回折で処理される\n"
                 + "＝『扉がどれだけ開いているかを音色で伝える』が丸ごと動かない。\n"
                 + "置き忘れても静かに劣化するだけなので、自動で生やす。\n\n"
                 + "手置きの AcousticPortal は消えない（そちらが優先されるわけではなく、"
                 + "両方が候補になる）。枠の内側ぴったりに置きたい所だけ手で置けばよい。\n\n"
                 + "実測: 自動生成と手置き（実寸）の差は 0.01dB。")]
        public bool autoPortals = true;

        [Tooltip("音源ごとの段（遮蔽・回折／早期反射／回折二次音源）を何コアで回すか。1 = 直列（既定）。\n\n"
                 + "★並列にしても音は 1 ビットも変わらない。割っているのは「音源ごとに自分の枠にしか"
                 + "書かない」段だけで、各音源の計算順も丸めも変わらないため"
                 + "（回帰テストで 616 個の値を厳密比較して確認済み）。\n\n"
                 + "実測（Test_Full と同じ規模・扉が毎フレーム動く）:\n"
                 + "  1 コア  平均 8.01ms  最悪 15.92ms\n"
                 + "  2 コア  平均 4.43ms  最悪  8.78ms（1.81 倍）\n"
                 + "  4 コア  平均 2.77ms  最悪  7.02ms（2.89 倍）  ← 既定\n"
                 + "  8 コア  平均 2.52ms  最悪  6.86ms（3.18 倍）\n"
                 + "4→8 でほとんど伸びないのは、残る直列（エッジカタログ・部屋・BVH 構築）が"
                 + "頭打ちを作るため。**コアを増やすほど良い、ではありません。**\n\n"
                 + "★エンジン側の既定は 1（直列）です。ここが 4 なのはホスト側の判断で、"
                 + "回帰テストを直列に保って厳密差分を使えるようにするためにエンジンは 1 のままにしてあります。\n\n"
                 + "⚠ Unity は既に全コアでジョブを回している。増やしすぎると食い合って、"
                 + "音響は速くなったのに他が遅くなる。上げたら必ず**全体の**フレーム時間で確かめること。\n"
                 + "⚠ 診断カウンタはスレッドごとになる。診断を採るときは 1 に戻すこと。")]
        [Range(1, 16)] public int workerThreads = 4;

        [Tooltip("後期残響の尾を**音源で共有して 1 回だけ畳む**（段階②）。\n\n"
                 + "★音は変わりません。畳み込みが線形なので、同じ IR なら"
                 + "「音源ごとのレベルを掛けてから足して 1 回畳む」で厳密に等しくなります"
                 + "（回帰テストで -123.7dB 下まで確認）。音源ごとの尾の量は保たれます。\n\n"
                 + "実測: 音源 8 本で 1 ブロック 2.345 → 0.404 ms（**5.81 倍**）。"
                 + "畳み込みの分割は 104 → 13。音源数が多いほど効きます。\n\n"
                 + "★AudioListener に TailBusRenderer を付けて、そこで 1 回畳みます。"
                 + "音源のフィルタが呼ばれる順は保証されませんが、リスナーのフィルタは"
                 + "全音源のミックス後に走るので、**遅延を足さずに**順序が保証されます。\n\n"
                 + "⚠ 尾の形は部屋ごとに共有済み（段階①）。その形の index をバスの鍵にします。"
                 + "違う部屋を同じバスへ入れると、片方の部屋の響きがもう片方に付きます。")]
        public bool sharedTailBus = false;
        [Tooltip("方向バス: 反射・回折のタップをリスナー座標で固定した N 方向へ振り、方向×耳の固定 HRIR で畳む（全音源で共有）。"
                 + "音声スレッドの費用が O(タップ) → O(方向) になり音源数に依らない。ITD はタップごと。OFF で従来の軽量両耳化（A/B 用）。")]
        public bool directionBus = true;
        [Tooltip("方向バスの本数（水平の環）。8 = 45° 刻み（費用 3.4%）。12 = 30°（5.4%）。両耳相関は 8 で基準と 0.02 以内。")]
        [Range(4, 16)] public int directionLanes = 8;
        [Header("段の予算と順位（主スレッドの上限）")]
        [Tooltip("厳密（回折の合成・面の線・二次音源まで解く）の本数の上限。0 = 無制限。1 本 0.3 ms の桁。"
                 + "超えたぶんは可聴性（音量 × 1/r × 前フレームの生存 × 重要度）の低い順に落ちる。"
                 + "重要度と固定は音源に AcousticSourcePriority を付けて決める。")]
        public int tierBudgetExact = 6;
        [Tooltip("簡易（遮蔽の音量と帯域カーブだけ。回り込みの方向は出ない）の本数の上限。0 = 無制限。1 本 0.05 ms の桁。"
                 + "ここからも漏れた音源は「保持」＝最後の答えを保ち、毎フレーム 1 本ずつ簡易で探り直す（素通しにはしない）。")]
        public int tierBudgetSimple = 10;
        [Header("タップの組み立て（DLL 側）")]
        [Tooltip("ON: タップ（1/r・空気吸収・パン・尾の比・平滑・HRTF の回折タップの選び方）は DLL が組み立て、ホストは写すだけ。"
                 + "直接タップの透過は役割1 の半影（窓の走査線積分）＝生存と同じ模型になる。尾の比は音源ごと（#4）。\n"
                 + "OFF で従来の自前の組み立て（AF_SceneComputeSoftOcclusion の円盤 32 点。A/B 用。採用が固まったら消す）。")]
        public bool tapsFromEngine = true;
        [Header("更新の非同期化（主スレッドの上限）")]
        [Tooltip("ON: 音響の更新は DLL のワーカースレッドが解き、主スレッドの Update は数 µs で帰る。"
                 + "答えは 1 フレーム前の入力に対する物（出力の平滑 0.35 s より十分短い）。設定は待ち行列で次の着手に効き、"
                 + "幾何の問い合わせ（ComputeSoftOcclusion など）は解く段と並走する。OFF で従来の同期（A/B 用）。")]
        public bool asyncUpdate = true;
        [Header("扉の定点（隣の空間の響きを戸口の位置から鳴らす）")]
        [Tooltip("ON: 隣の部屋（洞窟の中など）の尾は、その部屋のバスをそのまま足すのではなく、"
                 + "戸口の位置に置いた定点から HRTF で鳴らす。口の結合率（閉じた扉なら板の透過）と"
                 + "口からの距離で減る。部屋の中では従来どおり。\n\n"
                 + "★sharedTailBus が ON のときだけ効く（バスのモノラルを使うため）。\n"
                 + "★外へ開く口（洞窟の口）も自動ポータルになる。回折の経路には使わないので他の音は変わらない。\n"
                 + "★戻せる切り替え。OFF で従来どおり。")]
        public bool portalTail = false;
        [Tooltip("定点の全体レベル（1.0 で戸口の真ん中でバスの直接の足し込みと同じ大きさ）。")]
        [Range(0f, 4f)] public float portalTailLevel = 1f;
        [Tooltip("口の結合率（半球の見通し、360 本）を測り直す間隔（フレーム）。扉が回っている間の追従。")]
        [Range(1, 30)] public int portalTailCouplingEveryFrames = 5;
        [Tooltip("定点のボイスが使う HRTF（StreamingAssets 内のファイル名）。無ければ合成。")]
        public string portalTailHrtfFile = "kemar.afhr";
        [Tooltip("これ未満の断面積(m2)の開口はポータルにしない。\n"
                 + "格子の量子化ノイズ（セル 1〜2 個の隙間）でありもしない戸口が並ぶのを防ぐ。")]
        [Range(0f, 4f)] public float autoPortalMinArea = 0.25f;
        [Tooltip("ポータルの支配が及ぶ距離(m)。矩形と経路の距離がこれを超えたら、"
                 + "回折は一般の稜線探索に完全に戻る。あいだは滑らかに混ざる。\n\n"
                 + "★以前は『シーンに 1 枚でもポータルがあれば回折はポータルが全部決める』"
                 + "だった。自動生成すると全シーンがその状態になり、部屋の中央の柱の"
                 + "回り込みまでポータルが答えてしまう（実測 -3.3dB）。\n"
                 + "実測: 戸口の正面から横へ 0.1m 刻みで歩いて最大隣接差 1.77dB（崖なし）。")]
        [Range(0.2f, 4f)] public float portalGovernRange = 1.0f;

        [Tooltip("ON: 各音源の早期反射を IR の反射タップ(R)として畳み込む。F キー切替。"
                 + "模型は earlyReflectModel で選ぶ（既定は面の線）。尾のエコグラムからは 1 次反射を外して二重に鳴らさない。")]
        public bool enableEarlyReflections = true;
        [Tooltip("早期反射の模型。0 = 像源をレイで拾う（旧。像が見えた/消えたの二値でタップが跳ぶ）"
                 + "1 = 面ごとの線音源（2026-09-03 決定。鏡面点にピークを持つ下位タップの線。低域は点、高域は面いっぱいに広がる）")]
        [Range(0, 1)] public int earlyReflectModel = 1;
        [Tooltip("直接経路の半影の作り方。2 = 窓の走査線積分（2026-09-05 既定。影の区間を解析的に積むので扉の隙間で跳ばない）"
                 + "／1 = 帯域ごとのフレネル半径の環に標本点（09-02。4 点の環が階段になる）／0 = 旧（音源まわり 8 点）。"
                 + "同じビルドで聞き比べるための切り替え（Play 中に動かしてよい）。採用が固まったら 0・1 側ごと消す。")]
        [Range(0, 2)] public int directPenumbraMode = 2;
        [Tooltip("面 1 枚あたりの下位タップ数（面の線の分割数）。3〜5。")]
        [Range(1, 8)] public int earlyFaceSubTaps = 5;
        [Tooltip("ON: 起動時に静的な面のリストとセルごとの見通しを焼く（3 層構造の焼く層）。実行時は 焼いた見通し × 動いた物の遮蔽。OFF なら毎回生で解く（A/B 用。答えは同じ）。")]
        public bool bakeStaticFaces = true;
        [Tooltip("焼くセルの一辺(m)。小さいほど正確で重い。1 m で 15 m 角の部屋 675 セル。")]
        [Range(0.5f, 2f)] public float bakeCellSize = 1f;
        [Tooltip("音響的に動く物（扉・門・車両）。この配下のコライダは焼く層に入れず、最初から実行時の遮蔽として扱う。"
                 + "SwingDoor が付いた物と Rigidbody 付きは自動で動く物になる。タグを忘れても、動いた瞬間に焼き直す（1 回だけ重い）。")]
        public Transform[] dynamicOccluders;
        [Tooltip("面の焼きのファイル（StreamingAssets からの相対パス。空なら毎回メモリの中で焼く）。"
                 + "ファイルがあれば読んで焼きを飛ばす。無ければ焼いてから（エディタでだけ）書き出す。実体の並びが違えば読まずに焼き直す。")]
        public string bakeFile = "";
        [Tooltip("像源レイ模型（earlyReflectModel=0）のときの反射タップ数。面の線では使わない（面数 × 下位タップ数、上限 48）。")]
        [Range(1, 8)] public int earlyReflectTaps = 4;
        [Tooltip("早期反射抽出のレイ本数（音源ごと）。多いほど角度分解能が上がり、"
                 + "リスナー移動でタップの入れ替わりがカクつきにくい。総コスト = 音源数 × これ。"
                 + "広い部屋（壁が遠い）ほど本数を要する。")]
        public int earlyReflectRays = 512;
        [Tooltip("早期反射の反射回数（1〜2で初期反射のみ）。")]
        public int earlyReflectBounces = 2;
        [Tooltip("早期反射の更新間隔（フレーム）。音源ごとにレイを撒くので数フレーム毎。")]
        public int earlyReflectUpdateEveryFrames = 3;

        [Header("回折の二次音源 (エッジ=音源)")]
        [Tooltip("ON: 遮蔽時の回折を『エッジ＝二次音源』として開口ごとに分け、IR の回折タップ(F)として畳み込む。"
                 + "両側に開口があれば左右から聞こえ、リスナー移動で音量比が滑らかに変わる。GTD/ホイヘンスの原理。V キー切替。")]
        public bool enableDiffractionSources = true;
        [Tooltip("音源あたりの二次音源数（＝開口クラスタ数）。2〜3で『両側から』のイメージ。")]
        [Range(1, 5)] public int diffractionSourceCount = 3;
        [Tooltip("回折二次音源の全体レベル倍率（線形）。")]
        [Range(0f, 3f)] public float diffractionLevelScale = 1f;
        [Tooltip("回折二次音源の更新間隔（フレーム）。\n\n"
                 + "⚠ これが間引くのは『二次音源をどこに置くか』の置き直しだけで、"
                 + "**回折そのものは間引きません**。回折の計算は下の occlusionUpdateEveryFrames "
                 + "（エンジンの role1）の側にあります。名前で誤読されたので明記します。\n"
                 + "実測: 1→16 まで振っても平均は 11.3% しか減りません（4→8 は動きません）。")]
        public int diffractionUpdateEveryFrames = 2;

        [Tooltip("★遮蔽・回折の更新間隔（フレーム）。エンジンの role1。\n\n"
                 + "**音源ごとの費用の床はここです。**既定 1 ＝ 毎フレーム、全音源ぶん走ります。"
                 + "遮蔽された音源はここで回折の合成（diffractionComposite）が走るので、"
                 + "見通せる音源より数倍高くつきます。\n\n"
                 + "実測（扉が動く 6 音源）: 1→4 で 7.72→5.73ms（-26%）。そこから先は飽和します"
                 + "（8 で 5.34 / 16 で 5.28）。\n\n"
                 + "⚠ これは『いつ解くか』を削る手です。上げると遮蔽の追従がフレーム数ぶん遅れ、"
                 + "**扉が動いている最中の音色の変化が階段状になります。**"
                 + "数字だけで決めず、扉を掃きながら耳で確かめてください。")]
        [Range(1, 8)] public int occlusionUpdateEveryFrames = 1;

        [Header("材質")]
        [Tooltip("ON: シーン中の AcousticSurface が指す材質を毎フレーム送り直す。\n"
                 + "Play 中に扉の材質を切り替えて聞き比べたいときに使う（毎フレーム材質表を\n"
                 + "引き直すので少し重い。詰め終わったら OFF）。\n\n"
                 + "材質の 125Hz/4kHz の遮音量:\n"
                 + "  Concrete 26/54dB ・ Default 25/46 ・ Glass 20/35 ・ WoodDoor 15/27")]
        public bool liveMaterialUpdate = false;

        [Header("開口の扱い")]
        [Tooltip("ON: 開口を通る成分を『透過の一部』として扱う（合成透過率 τ=τ壁(1−f)+f の f の項）。\n\n"
                 + "開口をまっすぐ通る音は、曲がりもせず壁も抜けないので**何も払わない**。\n"
                 + "これを回折の一部として前川の δ 減衰を払わせていたのが、扉の効きを潰していた本体。\n"
                 + "実測: 音源が戸口に正対する配置で 開−閉 が 3.0dB → 22.3dB。\n"
                 + "開口を通る成分は開口の方向から届くので、直接音ではなく開口のタップへ入る\n"
                 + "＝定位は壊れない。")]
        public bool apertureIsTransmission = true;

        [Header("開口のコントラスト")]
        [Tooltip("開口率の**幅**を開く指数。1.0=素通し（物理そのまま）。\n\n"
                 + "開き具合の**形**は物理から出ている（隙間が 1−cosθ で増えるクレッシェンド、\n"
                 + "高域ほど大きく開く）。ただし**量**が足りず、実測で扉の全掃引が 1.3dB しかない。\n"
                 + "現実の合成透過率は同条件で 125Hz が 11dB、4kHz が 26dB 動く。\n\n"
                 + "ここは形を保ったまま幅だけを開く演出用のつまみ。\n"
                 + "『実測は参照であって目標ではない』の適用先。\n"
                 + "開口という一般の量への写像なので、扉を特別視しない。\n\n"
                 + "★2026-08-20: 既定を 4 → 1 に戻した。4 だと 10°の扉が**完全に無音**になり\n"
                 + "（実測 10° 0.0000 / 45° 0.0060 / 90° 0.9573）、看板チェック\n"
                 + "「少し開けたら少し聞こえる」が落ちる。1.0 なら 0.0204 / 0.2183 / 0.6851。\n"
                 + "C++ 既定も 1.0。ここを動かすと両方を揃えること（設計 §5-13）。\n"
                 + "量が足りない件は、コントラストではなく §5-13 のレベル設計側で解く。")]
        [Range(1f, 12f)] public float apertureContrast = 1f;

        [Tooltip("開口の**音色**の広がりを開く指数。1.0=素通し（既定＝物理そのまま）。\n\n"
                 + "apertureContrast が『開閉で**音量**がどれだけ動くか』なのに対し、\n"
                 + "こちらは『開閉で**音色**がどれだけ動くか』。帯域平均を保つので音量に効かない。\n"
                 + "  f'[b] = m·(f[b]/m)^k  （m = 帯域平均。掛けたあと平均を戻す）\n\n"
                 + "実測（半開き 35°の扉）: 音量は 0.1656 で不変のまま、4k/125 の傾きが\n"
                 + "  k=0.5 → −3.15dB / k=1.0 → −6.31dB / k=1.8 → −11.35dB / k=2.5 → −15.77dB\n\n"
                 + "★これはエンジンであって作品ではないので、どれくらい芝居がかって聞こえるかは\n"
                 + "  作品側が決める。既定を 1.0 にしてあるのはそのため（決めごと #3：形は物理から\n"
                 + "  採り、絶対値は演出で決める）。ホラー寄りに濃くするなら 1.5〜2.5 あたり。\n"
                 + "全帯域が同じ値のときは動かないので、『完全に閉じている／全開』は k で変わらない。")]
        [Range(0.2f, 4f)] public float apertureTimbre = 1f;

        [Tooltip("回折を BTM（有限楔の稜線積分）で出す。\n\n"
                 + "★検証途上。BTM 単体は性質チェック 7 件中 6 件を通っている\n"
                 + "（相反性は誤差ゼロ、深い影で前川と 1.1倍一致、影境界で総和が連続）が、\n"
                 + "シーンへ繋ぐと扉の掃引が反転する。渡している稜線集合の側の問題。\n"
                 + "解決するまで OFF のまま。")]
        public bool useBtmDiffraction = false;

        [Header("DSP 経路 (段4)")]
        [Tooltip("ON: 畳み込み・HRTF・後期尾を **C++ エンジン側**(VoiceConvolver)で回す。\n"
                 + "OFF: **Unity C# 側**(IrConvolver)で回す。Y キー切替。\n\n"
                 + "同じタップ束(SourceTaps.BandGain)を食わせているので、両者は同じ音が出るはず。\n"
                 + "つまり聞こえ方の調整（透過/回折のゲインと LPF、平滑化）はどちらでも同じに効く。\n\n"
                 + "★**既定は ON（C++ 経路）**。エンジンが本体で、C# 側は比較用という位置づけ。\n"
                 + "  以前は『C++ 経路が耳で未検証』を理由に OFF を既定にしていたが、\n"
                 + "  残響が減って聞こえる件（直接音の二重計上）を直して確認済みなので入れ替えた。\n"
                 + "  比較したいときだけ Y で C# 側へ落とす。\n"
                 + "  なお扉のタップ補間は C++ 側にしか無いので、\n"
                 + "  『開けた瞬間ガタっと変わる』の確認は ON 側でしかできない。\n"
                 + "  VoiceConvolver が載っていない古いシーンでは自動で C# 側に落ちて警告が出る。")]
        public bool useCppDsp = true;

        [Header("可視化")]
        [Tooltip("OFF: 画面左上のデバッグ表示（HUD）を出さない。" +
                 "※ゲームシステム側の「UI 無しの完成版」で、文字も数字も出さずに扉とベルだけで" +
                 "一周できるかを見るための切り口。既定 ON なので既存シーンの見え方は変わらない。")]
        public bool showHud = true;
        [Tooltip("HUD を実行中に切り替えるキー。既定 None ＝ キーを奪わない。" +
                 "※キーはゲームシステム側と共有の資源なので、こちらから勝手に取らない。" +
                 "収録中に出し入れしたいときだけシーンで割り当てる。")]
        public KeyCode hudToggleKey = KeyCode.None;
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
        [Tooltip("3バンドの遮蔽量(dB)の下限。モニター表示（Status.EqDb）用。")]
        [Range(-60f, -6f)] public float occlusionEqFloorDb = -36f;
        [Tooltip("空気吸収の強さ倍率。1=物理相当（ゲーム距離だと薄め）。距離のこもりを分かりやすくするなら3〜5へ。0で無効。")]
        [Range(0f, 8f)] public float airAbsorptionScale = 1f;
        [Tooltip("距離減衰の基準距離(m)。この距離でゲイン1、以遠は 1/r（refDist/経路長）で減衰。近距離はクランプ。0で無効。")]
        [Range(0f, 5f)] public float distanceRef = 1.5f;

        [Header("音声")]
        [Tooltip("ON: 各音源の AudioSource を鳴らす（clip が無い音源は幾何計算のみ）。空間化は畳み込み器が担う。")]
        public bool enableAudio = true;
        [Tooltip("ON: HRTF（両耳の頭部伝達）で空間化。OFF: パンニング。H キーで切替。")]
        public bool useHrtf = true;
        [Tooltip("B キーで全音源のクリップをこれに差し替える（足音などの過渡音は回折/反射の効きが分かりやすい）。"
                 + "未設定なら B は何もしない。")]
        public AudioClip altClip;
        [Tooltip("ON: 最初から altClip に差し替えた状態で鳴らす。B キーで切替。")]
        public bool useAltClip = false;

        // 音源 i のエンジン側 ID（AF_SceneSetSource の鍵）。連番でよいが 0/1 を避けて 10 から。
        private const ulong SourceObjIdBase = 10;
        private ulong SourceId(int i) => SourceObjIdBase + (ulong)i;

        private AcousticScene _scene;
        private int _materialId;
        private AcousticMaterialPreset _appliedOccluderMaterial;   // 部屋の材質の切り替え（L）を見るための「今エンジンにある物」
        // 部屋の材質として意味のある物だけを巡る（Glass / Opaque / WoodDoor は面や検証用）。
        private static readonly AcousticMaterialPreset[] kRoomPresets = {
            AcousticMaterialPreset.Concrete, AcousticMaterialPreset.WoodRoom, AcousticMaterialPreset.Stone,
            AcousticMaterialPreset.Cave, AcousticMaterialPreset.Snow, AcousticMaterialPreset.Default };
        private static AcousticMaterialPreset NextRoomPreset(AcousticMaterialPreset cur)
        {
            int i = System.Array.IndexOf(kRoomPresets, cur);   // 無ければ -1 → 0（Concrete）から
            return kRoomPresets[(i + 1) % kRoomPresets.Length];
        }
        // 材質の中身→materialId。同じ値の壁が何枚あってもテーブルは1つで済ませる。
        //   ★キーをプリセットの enum にしてはいけない（任意材質が同じ ID に潰れる）。
        private readonly System.Collections.Generic.Dictionary<string, int> _materialIdByContent
            = new System.Collections.Generic.Dictionary<string, int>();
        // occluder は箱かメッシュ。geomId < 0 が箱で、その場合 local* は使わない。
        //   メッシュはローカルAABBが単位箱になるよう正規化されて登録されるので、
        //   ホスト側は「そのローカルAABB＋Transform」から毎フレーム OBB を組み直す。
        private struct Occluder
        {
            public Collider col;          // BoxCollider または MeshCollider
            public int instanceId;
            public int geomId;            // -1 = 箱
            public Vector3 localCenter;   // メッシュのみ
            public Vector3 localHalf;     // メッシュのみ
        }
        private readonly List<Occluder> _occluders = new List<Occluder>();
        private readonly List<AcousticPortal> _portals = new List<AcousticPortal>();
        private SwingDoor[] _swingDoors;   // HUD に開き角を出すため（初回に一度だけ探す）
        private readonly float[] _portalFrac = new float[AcousticEngine.NumBands];
        // 同じ Mesh アセットは 1 回だけアップロードする（形状は共有し、配置だけ増やす）。
        private readonly Dictionary<int, int> _meshGeomCache = new Dictionary<int, int>();
        private readonly Dictionary<int, Vector3> _meshLocalCenter = new Dictionary<int, Vector3>();
        private readonly Dictionary<int, Vector3> _meshLocalHalf = new Dictionary<int, Vector3>();

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

        // 主音源(0番)の表示用。
        private readonly float[] _bands = new float[AcousticEngine.NumBands];
        private readonly float[] _diffBands = new float[AcousticEngine.NumBands];
        // 直接タップ用のバッファ。見通せていれば回折込みの総合値、遮蔽時は透過のみ。
        private float[] _directBands;
        // 回折タップ用のバッファ。回り込む総量を開口ごとの重みで割ったもの。
        private float[] _diffTapBands;
        // ソフト遮蔽の透過（振幅）。直接タップはこれを使う（単一レイだと掠める位置で跳ぶ）。
        private float[] _softTrans;
        // 副音源(1以降)の透過/回折を引くための一時バッファ（主音源は _bands/_diffBands を流用）。
        private float[] _srcTransmit, _srcDiffract;
        // タップ経路の平滑化。音源ごとに「遮蔽割合」と「透過の6帯域」を持ち越す。
        //   （2026-09-03 以前は Wwise エミッタ経路にだけ平滑化があり、タップ経路には無かった。）
        private float[] _tapOccSm;          // 音源ごと
        private float[] _tapTransSm;        // 音源ごと × 6帯域
        private bool[]  _tapSmInit;         // 初回はスナップする（無音から立ち上げない）
        private float   _diffGainLin = 1f;  // diffractionGainDb の線形値（毎フレーム更新）

        // ── 方向プローブ（後期残響の方向分布）──
        private Vector3[] _probeDirs;        // 球面上の等分布方向（ワールド固定）
        private float[] _probeEnergy;        // dirCount*6
        private float[] _tailEarBandGain;    // [0..5]=左 / [6..11]=右。平均1に正規化
        private float[] _probeAxis;          // 診断用: 6軸へ投影した分布
        private int _probeCountdown = 1;
        private float _diffDelta = -1f;
        private Vector3 _diffMid;
        // 遮蔽量の帯域加重（低域=大。低音は回り込んで残るため重い）。
        private static readonly float[] _bandWeights = { 3f, 2.5f, 2f, 1.3f, 1f, 0.8f };

        private float[] _echogram;      // 到達時間ビン（広帯域）
        private float[] _echogramBands; // 到達時間ビン×6帯域（実測された尾のIR用）
        private float _reverbWet, _reverbDecay;  // エコグラムから算出（RTPCへ）
        private float[] _roomRt60;      // 部屋の帯域別 RT60（幾何と材質から。使い回し）
        private int[] _roomIdsUi; private float[] _roomWUi;   // 画面表示用（使い回し）
        // ※ 各役割の更新レートはエンジンが持つ（段2）。ホストはカウンタを持たない。

        // --- モニター窓向け static フィード（Editor の *MonitorWindow が読む） ---
        public const int OutHistLen = 256;
        public static float[] LatestEchogram { get; private set; }
        public static int EchogramBins { get; private set; }
        public static float EchogramBinMs { get; private set; }
        /// エンジンの Scene。VoiceConvolver（C++ 側 DSP）がエコグラムを取りに来る。
        /// 1シーンに1つの想定。破棄時に null へ戻す。
        public static AcousticScene SharedScene { get; private set; }
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
            public static float RoomShare = 1f;   // まわりの空間のうち部屋の中である割合（残響の量の重み）
            public static bool UseHrtf;
            public static bool UseSteer;
            public static string[] SourceNames;   // 音源ごとの表示名（クリップ名）
            public static float[] Occlusion;      // 音源ごとの遮蔽 0..1（大=遮られてる）
            public static float[] Survival;       // 音源ごとの帯域生存（低域加重）
            public static float DiffDelta;        // 主音源の回折 迂回余剰長δ(m)。-1=迂回路なし
            public static int DiffCandCount;
            // エッジカタログ。★Editor の StatusMonitorWindow が参照しているのに
            //   ここに無く、**Editor アセンブリがコンパイルできない状態**だった
            //   （tools/check_csharp.ps1 で検出）。
            public static bool UseEdgeCatalog;
            public static int EdgeCatalogCount;
            public static bool ShowDiffCandidates;
            public static bool EarlyReflEnabled;
            public static int ErActive, ErCap;      // 早期反射 鳴動/上限
            public static bool DiffSrcEnabled;
            public static int DiffActive, DiffCap;  // 回折二次音源 鳴動/上限
            public static bool DiffDistanceOnly;    // 回折のゲインが距離減衰だけか（周波数依存なし）
            public static float[] BandsTransmit;    // 主音源 6帯域 透過
            public static float[] BandsDiffract;    // 主音源 6帯域 回折
            public static bool UseBandEq;           // 3バンドEQモードか
            public static float[] EqDb;             // 音源ごと×3(Low/Mid/High) の送出EQゲイン(dB)
            // 音源ごとのタップ束。IrConvolver が sourceIndex で 1 つ選んで読む。
            public static SourceTaps[] Taps;

            // 以下は主音源(0)のエイリアス。モニタウィンドウ用に残してある。
            public static int TapCount;
            public static float ItdgMs;             // 最初の反射までの相対遅延=ITDG
            public static float[] TapDelayMs;       // タップ遅延(ms, 直接=0)
            public static float[] TapGain;          // 広帯域ゲイン
            public static float[] TapBandGain;      // タップ×6帯域(125/250/500/1k/2k/4kHz) ゲイン（畳み込み用）
            public static float[] TapPanL;          // 段3a: タップの左右パン（等パワー）
            public static float[] TapPanR;
            public static char[] TapType;           // 'D'/'R'/'F'
            // 直接音の到来方向（リスナー座標系: +x=右, +y=上, +z=前）。HRTF の方向選択に使う。
            // 遮蔽時は回折で回り込む方向になるので、単純な音源方向ではなく実測の到来方向を渡す。
            public static Vector3 DirectDirLocal = Vector3.forward;
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
            // 圧縮前の生の物理比（診断用。ReverbTargetRatio はこれを知覚圧縮したもの）。
            public static float ReverbPhysicalRatio;
            // 早期↔後期の境目(mixing time)の目安 ≈ √V(ms)。広い部屋ほど遅い。
            //   これより前の反射は方向つき早期タップ、後は拡散尾として扱う。
            public static float MixingTimeMs;
            public static float RtSeconds;          // 残響RT60(秒)
            public static float Wet;                // 残響wet(0..1, tail/total)
            // 部屋グラフから取った実効値（0 なら部屋が取れず外形箱にフォールバックした）。
            //   RoomVolume: まわりの何割がどの部屋かで混ぜた体積(m3)
            //   RoomRt60  : 部屋ごとの Sabine を同じ割合で混ぜた残響時間(s, 500Hz帯)
            public static float RoomVolume;
            public static float RoomRt60;
            public static float SourceLevel;        // 主音源の直線透過(遮蔽)レベル(0..1)。残響を遮蔽で絞る用

            // 後期残響の左右バランス（耳ごと×6帯域）。平均が 1 になるよう正規化してある
            //   ＝全体の音量は変えず、どちらから響いてくるかだけを変える。
            //   [0..5]=左耳 / [6..11]=右耳。null なら方向づけなし（従来どおり均一）。
            public static float[] TailEarBandGain;
            public static float TailDirBalance;     // 診断用: 低域の右/左 比（1=均等）

            // 診断用: プローブが測った方向分布そのもの（リスナー座標系の6方向・125Hz）。
            //   前後/上下の非対称は L/R バランスでは表現できないので、生の分布を見る必要がある。
            //   [0]=右 [1]=左 [2]=上 [3]=下 [4]=前 [5]=後
            public static float[] ProbeAxisEnergy;
        }

        /// <summary>
        /// 1 音源ぶんの IR タップ束。IrConvolver はこれを 1 つ見て畳み込む。
        ///
        /// 音源ごとに持つ理由: 遅延も到来方向も遮蔽も音源ごとに違うので、共有できない。
        /// 空間音響エンジンが 1 音源しかレンダリングできないのはシステムとして欠落であり、
        /// ホラーのように環境音・足音・物音が同時に鳴る用途では前提条件になる。
        ///
        /// 後期残響（尾）は音源ごとに畳み込む。拡散音場自体はリスナー位置でほぼ決まるが、
        /// 送出量は音源ごとの遮蔽・距離で変わるため。音源数が増えたら共有バス化を検討する
        /// （業界の定石。ただし現時点では最適化より完成を優先する）。
        /// </summary>
        public sealed class SourceTaps
        {
            public const int MaxTaps = 64;

            public int Count;
            public float ItdgMs;                    // 最初の反射までの相対遅延=ITDG
            public float SourceLevel;               // 反射込みの生存(0..1)。残響の送出量に使う
            // エンジン内の音源 index（AF_SceneSourceIndex の戻り）。-1 は未登録。
            //   畳み込み器が「自分の音源のエコグラム」を引くのに要る。
            //   ホスト側の並び順とは別物なので、ここに控えて渡す。
            public int EngineIndex = -1;
            // 【尾の形を引く index】自分と**同じ部屋にいる音源のうち先頭**のエンジン index。
            //   尾は 2 つの要素でできていて、実測で分解できた:
            //     形（傾き＝RT60）… 部屋で決まる。同室 3 音源の 200→600ms の傾きは
            //                        -17.3 / -18.6 / -18.0 dB と **1.3dB 以内**で揃い、
            //                        隣室は -14.7 dB で **2.6〜3.9dB** ずれた
            //     量（オフセット） … 音源ごとの到達の幾何。同室でも 3〜4dB 開く
            //   なので**形は部屋で共有し、量（dg）は音源ごとに残す**。
            //   ★これは段①（一貫性）。畳み込みは依然として音源ごとなのでコストは減らない。
            //     コスト削減は段②（部屋ごとの尾バスへ送って 1 回だけ畳む）で入る。
            public int TailShapeIndex = -1;
            // 【自由音場の直接レベル】距離減衰と空気吸収だけ。遮蔽を**含まない**。
            //   尾の絶対レベルの校正基準（tailGain = これ × √target）に使う。
            //   ★BandGain[0..5]（直接音タップ）を使ってはいけない。あれは遮蔽込みなので、
            //     柱の陰に入ると透過ぶんまで落ちる（実測 -31dB）。それを基準にすると
            //     尾まで一緒に -31dB 落ちて、部屋の残響が消える。
            //     部屋の残響音場は音源が部屋へ注いだパワーで決まり、リスナーの見通しが
            //     柱で切れても消えない。遮蔽は SourceLevel の 1 箇所だけで効かせる。
            public float FreeFieldDirect = 1f;
            public Vector3 DirectDirLocal = Vector3.forward;   // HRTF 用の到来方向
            // DLL の組み立てから来る音源ごとの尾の量（-1 = 無い＝ホストの全体値を使う）。
            public float TailRatio = -1f;
            public float TailRatioPhysical = 0f;
            public float MixingTimeMs = -1f;
            public float RoomShare = 1f;

            // 【B1】HRTF に載せる回折タップ。-1 = 無し（＝見通せている／回折が無い）。
            //   遮蔽されると直接音タップは材質の透過まで落ちるので、実際に耳へ届く
            //   エネルギーは回折タップが運ぶ。そちらに ITD と前後の手がかりが無いと、
            //   「音の方へ進むと穴に着く」が成立しない。
            //   ★1 本だけなのはコストのため（HRTF 1 本で +0.117ms/block）。
            //     いちばん強い開口に載せれば、体験上いちばん効くところが埋まる。
            public int HrtfTapIndex = -1;
            public Vector3 HrtfTapDirLocal = Vector3.forward;
            // 乗り換えのヒステリシス用。前フレームに載せていた開口の位置（世界座標）。
            public Vector3 HrtfPrevArrival;
            public bool HrtfHasPrev;

            public readonly float[] DelayMs = new float[MaxTaps];
            public readonly float[] Gain = new float[MaxTaps];
            public readonly float[] BandGain = new float[MaxTaps * AcousticEngine.NumBands];
            public readonly float[] PanL = new float[MaxTaps];
            public readonly float[] PanR = new float[MaxTaps];
            public readonly char[] Type = new char[MaxTaps];    // 'D'直接 / 'R'反射 / 'F'回折
            public readonly Vector3[] Arrival = new Vector3[MaxTaps];   // 到来点（世界座標）
            // 到来方向（リスナー座標系）。反射タップの軽量な両耳化（ITD＋帯域別ILD）に使う。
            //   ★フル HRTF を通すのは直接音と最強の回折タップだけ。多数の反射に HRIR を
            //     畳み込むと重い（HRTF 1 本 +0.102ms/block に対し、こちらは 7 本で +0.043ms）。
            public readonly Vector3[] DirLocal = new Vector3[MaxTaps];
        }

        private SourceTaps[] _taps;     // 音源ごと（_sources と同じ長さ）
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
        // タップ本体は音源ごとに SourceTaps が持つ（_taps / Status.Taps）。
        private Vector3[] _tapErPos = new Vector3[48];  // 早期反射 像源バッファ（面の線: 面 ~10 枚 × 5 本）
        private float[] _tapErGain = new float[48 * 6];
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
            SharedScene = _scene;   // VoiceConvolver がエコグラムを取りに来る
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
            _appliedOccluderMaterial = occluderMaterial;
            CollectOccluders();
            RegisterInstances();
            CollectPortals();
            BakeStaticFaces();
            SetupCamera();
            // DSP 経路（C# の IrConvolver ⇔ C++ の VoiceConvolver）。両方シーンに載せて片方だけ enabled にする。
            //   実機の HUD で「音声なし」と出ていたのがその証拠（_audioReady == false）。
            SetDspPath(useCppDsp);

            // 尾の共有バス（段階②）。AudioListener に居ないと意味が無い ──
            //   リスナーのフィルタが全音源のミックス後に走ることを使って、順序を保証している。
            //   シーンに手を入れずに実行時に付ける（シーンは他レーンとも共有しているので）。
            {
                var lis = FindFirstObjectByType<AudioListener>();
                if (lis != null)
                {
                    var host = lis.GetComponent<TailBusRenderer>();
                    // ★尾の共有バスを使わなくても付ける。方向バス（反射・回折タップの束ね）もこの部品が持つため。
                    if (host == null) host = lis.gameObject.AddComponent<TailBusRenderer>();
                    if (host != null) host.enableSharedTail = sharedTailBus;
                    if (host != null) { host.enableDirectionBus = directionBus; host.directionLanes = directionLanes; }
                }
                else if (sharedTailBus)
                {
                    Debug.LogWarning("[AcousticFlowScene] AudioListener が見つからないので"
                                     + "尾の共有バスを使えません（音源ごとに畳みます）。");
                }
            }

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
            _tapOccSm = new float[n];
            _tapTransSm = new float[n * AcousticEngine.NumBands];
            _tapSmInit = new bool[n];
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

            // 回折候補の可視化バッファ（エンジンの合成上限に合わせて64）。
            _diffCandPts = new Vector3[64];
            _diffCandDelta = new float[64];
        }

        /// 音源リストを反映し直す。**エンジンのシーンは作り直さない。**
        ///
        /// ホストが `extraSources` を差し替えたときに呼ぶ。以前はこれを行うために
        /// コンポーネントごと `enabled = false → true` するしかなく、そのたびに
        /// `OnDisable` が C++ のシーンを Dispose → `OnEnable` が全コライダーを
        /// 登録し直していた。**跨いだ 3 フレーム目に毎回 24〜34ms** が出ていたのはこれで、
        /// 重いのは部屋グラフの再ボクセル化（C# の同期部分は 1ms 未満）。
        /// 幾何に触らなければ格子は dirty にならないので、その支払いが丸ごと消える。
        ///
        /// 幾何そのものが入れ替わったとき（世界のコライダーが変わった等）は
        /// 従来どおり作り直しが要る。そちらとは意図的に分けてある。
        public void RefreshSources()
        {
            if (_scene == null || !_scene.IsValid) return;

            // ★重ねた状態のまま作り直してはいけない。
            //   BuildSources は「元配置」(_srcHome) を現在位置から取り直すので、
            //   重ねている最中に呼ぶと元の位置が重ね座標で上書きされ、**永久に失われる**。
            //   先に元配置へ戻してから作り直す。
            if (_stacked) { _stacked = false; ApplySourceLayout(); }

            // 古い音源の音声を畳む（差し替えたクリップを戻す）。
            TeardownSourceAudio();

            // C++ 側の登録も空にする。音源が減ったとき古い id が残ったままになり、
            // 鳴らない音源のために遮蔽・回折・エコグラムを計算し続ける。
            _scene.ClearSources();

            BuildSources();

            // ★平滑化の状態（_tapOccSm / _tapTransSm 等）は
            //   BuildSources の再確保でゼロに戻る。音源の顔ぶれが変わる瞬間なので
            //   許容しているが、同じ音源が残る場合はそこだけ立ち上がりが速くなる。
            if (enableAudio) SetupSourceAudio();
        }

        // 音源の音声を畳む（RefreshSources / OnDisable 共用）。クリップを元に戻し、ミュートを解く。
        private void TeardownSourceAudio()
        {
            if (_srcAudio == null) return;
            for (int i = 0; i < _srcAudio.Length; i++)
            {
                var a = _srcAudio[i];
                if (a == null) continue;
                if (_srcClipOrig[i] != null && a.clip != _srcClipOrig[i]) { a.clip = _srcClipOrig[i]; a.Play(); }
                a.mute = false;
            }
            _srcAudio = null;
            _srcClipOrig = null;
        }

        // 音源の顔ぶれが変わったあと、AudioSource を拾い直して鳴らし直す。
        private void SetupSourceAudio() => SetupAudio();

        // 音源を「元配置」⇄「1点に重ね」で配置し直す。
        private void ApplySourceLayout()
        {
            if (_sources == null || _srcHome == null) return;
            for (int i = 0; i < _sources.Length; i++)
                if (_sources[i] != null)
                    _sources[i].position = _stacked ? _stackPoint : _srcHome[i];
        }

        // Inspector の設定をエンジンのバッチ更新設定へ写す（段2）。
        // 更新レートもここで渡すので、ホスト側のカウンタは持たない。
        private AcousticUpdateConfig BuildUpdateConfig()
        {
            var c = new AcousticUpdateConfig
            {
                role1EveryN = Mathf.Max(1, occlusionUpdateEveryFrames),
                role2EveryN = Mathf.Max(1, reverbUpdateEveryFrames),
                earlyEveryN = Mathf.Max(1, earlyReflectUpdateEveryFrames),
                diffSrcEveryN = Mathf.Max(1, diffractionUpdateEveryFrames),
                catalogEveryN = Mathf.Max(1, catalogUpdateEveryFrames),

                reflectionRays = reflectionRays,
                reflectionBounces = reflectionBounces,
                directWeight = directLocalizeWeight,
                useReflections = useReflections ? 1 : 0,

                useEdgeCatalog = useEdgeCatalog ? 1 : 0,
                edgeCatalogRes = edgeCatalogRes,
                edgeCatalogMaxDist = 40f,


                enableReverb = enableReverb ? 1 : 0,
                echogramBins = Mathf.Max(1, echogramBins),
                echogramBinSeconds = echogramBinMs * 0.001f,
                echogramRays = echogramRays,
                echogramBounces = echogramBounces,
                speedOfSound = kSpeedOfSound,
                distanceRef = distanceRef,

                enableEarlyReflections = enableEarlyReflections ? 1 : 0,
                // タップ本数は IR 畳み込み側の要求（_tapErPos.Length）と inspector 値の max を要求する
                //（受け取り側は自分のバッファ長で頭打ちになるので、余分は無視される）。
                earlyTaps = Mathf.Max(Mathf.Max(1, earlyReflectTaps), _tapErPos.Length),
                earlyRays = earlyReflectRays,
                earlyBounces = earlyReflectBounces,
                earlyModel = earlyReflectModel,
                earlyFaceSubTaps = Mathf.Clamp(earlyFaceSubTaps, 1, 8),
                echogramSkipFirstOrder = enableEarlyReflections ? 1 : 0,
                enableDiffractionSources = enableDiffractionSources ? 1 : 0,
                diffSources = Mathf.Max(Mathf.Max(1, diffractionSourceCount), _tapDiffPos.Length),
            };
            return c;
        }

        // シーンに置かれた AcousticPortal を集めてエンジンへ登録する。
        //   ホストが持つのは**トポロジだけ**（ここが開口である、という事実）。
        //   開き具合は渡さない ── エンジンが毎フレーム実形状から測る。
        // 焼く層（第 1 段）: 静的な面の見通しを焼く。幾何が揃った後（RegisterInstances / CollectPortals の後）に呼ぶ。
        //   焼くのは構造で、答えではない。動いた物は moved で焼きから外れ、実行時に生で扱われる。
        private int _bakeCells, _bakeFaces;
        private float _bakeMs;
        private int _bakeDynamic;
        private void BakeStaticFaces()
        {
            if (_scene == null || !_scene.IsValid) return;
            if (!bakeStaticFaces) { _scene.ClearFaceBake(); _bakeCells = 0; _bakeFaces = 0; _bakeMs = 0f; return; }
            // 動く物のタグ（焼く前に付ける）。
            int dyn = 0;
            for (int i = 0; i < _occluders.Count; i++)
            {
                var o = _occluders[i];
                if (o.col == null || o.instanceId < 0) continue;
                bool d = o.col.GetComponentInParent<SwingDoor>() != null || o.col.attachedRigidbody != null;
                if (!d && dynamicOccluders != null)
                    foreach (var t in dynamicOccluders)
                        if (t != null && o.col.transform.IsChildOf(t)) { d = true; break; }
                if (d) { _scene.SetInstanceDynamic(o.instanceId, true); dyn++; }
            }
            _bakeDynamic = dyn;
            // ファイルがあれば読む（焼く層の「制作時に焼く」の最小形）。
            string bakePath = string.IsNullOrEmpty(bakeFile) ? null : System.IO.Path.Combine(Application.streamingAssetsPath, bakeFile);
            if (bakePath != null && System.IO.File.Exists(bakePath))
            {
                var swf = System.Diagnostics.Stopwatch.StartNew();
                if (_scene.FaceBakeImport(System.IO.File.ReadAllBytes(bakePath)))
                {
                    _bakeFaces = _scene.FaceBakeFaceCount();
                    _bakeCells = -1;   // ファイル由来（セル数は表示しない）
                    _bakeMs = (float)swf.Elapsed.TotalMilliseconds;
                    Debug.Log($"[AcousticFlowScene] 面の焼きをファイルから読んだ: {bakeFile} / 面 {_bakeFaces} / {_bakeMs:F0} ms");
                    return;
                }
                Debug.LogWarning($"[AcousticFlowScene] 焼きのファイルが今の実体と合わないので焼き直します: {bakeFile}");
            }
            var sw = System.Diagnostics.Stopwatch.StartNew();
            _bakeCells = _scene.BakeStaticFaces(bakeCellSize, Mathf.Clamp(earlyFaceSubTaps, 1, 8));
            _bakeFaces = _scene.FaceBakeFaceCount();
            _bakeMs = (float)sw.Elapsed.TotalMilliseconds;
            Debug.Log($"[AcousticFlowScene] 面の焼き: 面 {_bakeFaces} / セル {_bakeCells} / {_bakeMs:F0} ms ／ 動く物 {_bakeDynamic}");
            if (bakePath != null && Application.isEditor)
            {
                var bytes = _scene.FaceBakeExport();
                if (bytes != null)
                {
                    System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(bakePath));
                    System.IO.File.WriteAllBytes(bakePath, bytes);
                    Debug.Log($"[AcousticFlowScene] 面の焼きを書き出した: {bakeFile} ({bytes.Length} バイト)");
                }
            }
        }

        private void CollectPortals()
        {
            _portals.Clear();
            var found = FindObjectsByType<AcousticPortal>(
                FindObjectsInactive.Include, FindObjectsSortMode.None);
            foreach (var p in found)
            {
                if (p == null || !p.active) continue;
                p.engineId = _scene.AddPortal(p.transform.position, p.AxisU, p.AxisV,
                                              p.HalfU, p.HalfV);
                if (p.engineId >= 0) _portals.Add(p);
            }
            if (_portals.Count > 0)
                Debug.Log($"[AcousticFlowScene] ポータル {_portals.Count} 個を登録しました。");
        }

        // 扉ごと動く戸口もあるので、配置は毎フレーム送り直す（安い）。
        private void UpdatePortals()
        {
            for (int i = 0; i < _portals.Count; i++)
            {
                var p = _portals[i];
                if (p == null || p.engineId < 0) continue;
                _scene.UpdatePortal(p.engineId, p.transform.position, p.AxisU, p.AxisV,
                                    p.HalfU, p.HalfV);
            }
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
                    AddOccluder(col);
                }
                // MeshCollider も同じ扱いで拾う。箱で表せない形（穴の空いた壁など）はこちら。
                foreach (var col in FindObjectsOfType<MeshCollider>())
                {
                    if (col == null || !col.enabled || col.sharedMesh == null) continue;
                    if (IsExcluded(col.transform)) continue;
                    AddOccluder(col);
                }
            }
            if (extraOccluders != null)
                foreach (var col in extraOccluders)
                    if (col != null) AddOccluder(col);
        }

        private void AddOccluder(Collider col)
        {
            foreach (var o in _occluders) if (o.col == col) return;   // 重複登録を防ぐ
            _occluders.Add(new Occluder { col = col, instanceId = -1, geomId = -1 });
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

        // このオクルーダーの材質 ID を解決する（段5: 材質の個別化）。
        //   優先順位は AcousticSurface（明示）> グローバル既定。
        //   材質は「見た目」からは決まらないので、必要な面にだけ AcousticSurface を付ける。
        //   プリセットごとに1つだけ登録して使い回す（同じ材質の壁が何枚あってもテーブルは増えない）。
        private int ResolveMaterialId(Occluder o)
        {
            if (o.col == null) return _materialId;
            var surf = o.col.GetComponent<AcousticSurface>();
            if (surf == null) surf = o.col.GetComponentInParent<AcousticSurface>();
            if (surf == null) return _materialId;

            // ★キーは**中身**にする。以前はプリセットの enum をキーにしていたので、
            //   任意材質（AcousticSurfaceMode.Adjusted / Custom）を入れると
            //   「形の出どころが同じプリセット」というだけで別の材質が同じ ID に潰れ、
            //   最初に登録した方の音で全部鳴っていた。
            //   中身をキーにすれば、同じ値の壁は今までどおり 1 つを共有する。
            var mat = surf.Resolve();
            string key = mat.ContentKey();
            if (_materialIdByContent.TryGetValue(key, out int id)) return id;
            id = _scene.AddMaterial(mat);
            _materialIdByContent[key] = id;
            return id;
        }

        private void RegisterInstances()
        {
            _meshGeomCache.Clear();
            _meshLocalCenter.Clear();
            _meshLocalHalf.Clear();
            _materialIdByContent.Clear();

            for (int i = 0; i < _occluders.Count; i++)
            {
                var o = _occluders[i];
                var mc = o.col as MeshCollider;
                if (mc != null && mc.sharedMesh != null)
                {
                    // 形状は Mesh アセット単位で 1 回だけ登録し、配置だけ増やす（インスタンシング）。
                    int key = mc.sharedMesh.GetInstanceID();
                    if (!_meshGeomCache.TryGetValue(key, out int geom))
                    {
                        geom = _scene.AddMesh(mc.sharedMesh, out Vector3 lc, out Vector3 lh);
                        _meshGeomCache[key] = geom;
                        _meshLocalCenter[key] = lc;
                        _meshLocalHalf[key] = lh;
                    }
                    if (geom >= 0)
                    {
                        o.geomId = geom;
                        o.localCenter = _meshLocalCenter[key];
                        o.localHalf = _meshLocalHalf[key];
                        GetObb(o, out Vector3 c, out Vector3 half, out Vector3 r, out Vector3 u);
                        o.instanceId = _scene.AddInstanceMesh(geom, c, half, r, u, ResolveMaterialId(o));
                        _occluders[i] = o;
                        continue;
                    }
                    // 登録に失敗したら境界ボックスで代用する（音が消えるよりまし）。
                }

                GetObb(o, out Vector3 bc, out Vector3 bhalf, out Vector3 br, out Vector3 bu);
                o.instanceId = _scene.AddInstanceBox(bc, bhalf, br, bu, ResolveMaterialId(o));
                _occluders[i] = o;
            }
        }

        // occluder の現在の transform から OBB を組む。
        //   箱   … BoxCollider の center/size × lossyScale
        //   メッシュ … 登録時のローカルAABB × lossyScale（正規化ローカル[-1,1]^3 → ワールドの写像）
        private static void GetObb(Occluder o, out Vector3 center, out Vector3 half,
                                   out Vector3 right, out Vector3 up)
        {
            Transform t = o.col.transform;
            Vector3 s = t.lossyScale;
            if (o.geomId >= 0)
            {
                center = t.TransformPoint(o.localCenter);
                half = new Vector3(o.localHalf.x * Mathf.Abs(s.x),
                                   o.localHalf.y * Mathf.Abs(s.y),
                                   o.localHalf.z * Mathf.Abs(s.z));
            }
            else if (o.col is BoxCollider bc)
            {
                center = t.TransformPoint(bc.center);
                half = new Vector3(Mathf.Abs(bc.size.x * 0.5f * s.x),
                                   Mathf.Abs(bc.size.y * 0.5f * s.y),
                                   Mathf.Abs(bc.size.z * 0.5f * s.z));
            }
            else
            {
                // メッシュ登録に失敗した場合などのフォールバック。ワールド境界で代用する。
                Bounds b = o.col.bounds;
                center = b.center;
                half = b.extents;
                right = Vector3.right; up = Vector3.up;
                return;
            }
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

        // ── 音声（Unity の AudioSource）──────────────────────────────────────────
        //   ★2026-09-03: Wwise 風の再生経路（PostEvent/RTPC/像源エミッタ/回折エミッタ）を全部消した。
        //     DLL 側は既定でスタブ（AF_USE_WWISE=OFF）だったので、あの道は何も鳴らしていなかった。
        //     鳴っているのは各音源の AudioSource のクリップ → IrConvolver / VoiceConvolver の畳み込みだけ。
        //     道を 2 本持っていると「1〜4 のソロが効かない＝別の何かが鳴っている」の切り分けができない。
        private AudioSource[] _srcAudio;    // 音源ごとの AudioSource（無い音源は null）
        private AcousticSourcePriority[] _srcPriority;   // 音源ごとの順位のつまみ（無い音源は null ＝ 重要度 1・固定なし）
        private AudioClip[]   _srcClipOrig; // B で元に戻すためのクリップ
        private bool _muted;                // M: 音源を一括ミュート
        private float[] _outTmp;            // 出力レベル計測用（AudioListener.GetOutputData）

        // 表示名。クリップ名 → 無ければ Transform 名。
        private string SourceName(int i)
        {
            if (_srcAudio != null && i < _srcAudio.Length && _srcAudio[i] != null && _srcAudio[i].clip != null)
                return _srcAudio[i].clip.name;
            return (_sources != null && i < _sources.Length && _sources[i] != null) ? _sources[i].name : $"Source{i}";
        }

        private void SetupAudio()
        {
            if (_sources == null) return;
            _srcAudio = new AudioSource[_sources.Length];
            _srcPriority = new AcousticSourcePriority[_sources.Length];
            _srcClipOrig = new AudioClip[_sources.Length];
            int playing = 0;
            for (int i = 0; i < _sources.Length; i++)
            {
                var a = (_sources[i] != null) ? _sources[i].GetComponent<AudioSource>() : null;
                _srcAudio[i] = a;
                _srcPriority[i] = (_sources[i] != null) ? _sources[i].GetComponent<AcousticSourcePriority>() : null;
                if (a == null) continue;
                _srcClipOrig[i] = a.clip;
                // 空間化は畳み込み器がやる。Unity 側の 3D 減衰が重なると距離の数字が合わなくなるので 2D に固定。
                a.spatialBlend = 0f;
                a.mute = _muted;
                if (useAltClip && altClip != null) a.clip = altClip;
                if (a.clip == null) continue;
                if (!a.isPlaying) a.Play();
                playing++;
            }
            _audioReady = playing > 0;
            if (!_audioReady)
                Debug.LogWarning("[AcousticFlowScene] 鳴らせる AudioSource が音源にありません（clip 未設定）。幾何計算だけ動きます。");
            ApplyHrtfFlag();
        }

        // B：全音源のクリップを altClip ⇄ 元 に差し替える（再生位置は頭に戻る）。
        private void SwitchSourceSound()
        {
            useAltClip = !useAltClip;
            if (_srcAudio == null) return;
            for (int i = 0; i < _srcAudio.Length; i++)
            {
                var a = _srcAudio[i];
                if (a == null) continue;
                var want = (useAltClip && altClip != null) ? altClip : _srcClipOrig[i];
                if (want == null || a.clip == want) continue;
                a.clip = want;
                a.Play();
            }
        }

        private void SetMuted(bool on)
        {
            _muted = on;
            if (_srcAudio == null) return;
            foreach (var a in _srcAudio) if (a != null) a.mute = on;
        }

        // H：畳み込み器の HRTF を useHrtf に合わせる。両経路とも同じ旗を持っているので両方に配る
        //   （以前は Wwise の State にしか送っていなかったので、畳み込み経路では H が何もしていなかった）。
        private void ApplyHrtfFlag()
        {
            foreach (var c in FindObjectsByType<IrConvolver>(FindObjectsInactive.Include, FindObjectsSortMode.None))
                c.enableHrtf = useHrtf;
            foreach (var c in FindObjectsByType<VoiceConvolver>(FindObjectsInactive.Include, FindObjectsSortMode.None))
                c.enableHrtf = useHrtf;
        }

        private static float Rms(float[] x)
        {
            double s = 0.0;
            for (int i = 0; i < x.Length; i++) s += x[i] * x[i];
            return (float)System.Math.Sqrt(s / System.Math.Max(1, x.Length));
        }

        // ── DSP 経路の切替（C# の IrConvolver ⇔ C++ の VoiceConvolver）──
        //   同じ GameObject に両方載せておき、**片方だけ enabled にする**。
        //   Unity は無効な MonoBehaviour の OnAudioFilterRead を呼ばないので、
        //   これだけで経路が入れ替わる（AudioSource は共有のまま＝再生位置がズレない）。
        private void SetDspPath(bool cpp)
        {
            useCppDsp = cpp;
            // ★FindObjectsInactive.Include が要る。切替の相手は**今まさに無効な方**なので、
            //   既定（Exclude）だと有効な側しか拾えず、一度切ったら戻せなくなる。
            var irs = FindObjectsByType<IrConvolver>(
                FindObjectsInactive.Include, FindObjectsSortMode.None);
            var voices = FindObjectsByType<VoiceConvolver>(
                FindObjectsInactive.Include, FindObjectsSortMode.None);
            int nIr = irs.Length, nVoice = voices.Length;
            foreach (var c in irs) c.enabled = !cpp;
            foreach (var c in voices) c.enabled = cpp;

            if (cpp && nVoice == 0)
            {
                // シーンが古い（VoiceConvolver が載っていない）。黙って C# 経路のままにすると
                // 「切り替えたのに音が変わらない」で悩むので、はっきり言う。
                useCppDsp = false;
                foreach (var c in irs) c.enabled = true;
                Debug.LogWarning("[AcousticFlowScene] VoiceConvolver がシーンにありません。"
                                 + "C# 経路(IrConvolver)のままです。メニュー "
                                 + "AcousticFlow > テストシーン生成 でシーンを作り直してください。");
                return;
            }
            Debug.Log($"[AcousticFlowScene] DSP 経路: {(useCppDsp ? "C++ (VoiceConvolver)" : "C# (IrConvolver)")}"
                      + $"  IrConvolver {nIr} / VoiceConvolver {nVoice}");
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
            if (Input.GetKeyDown(KeyCode.H)) { useHrtf = !useHrtf; ApplyHrtfFlag(); }
            // G：方向ステアリング（到来方向で置き直す）ON/OFF。
            if (Input.GetKeyDown(KeyCode.G)) useDirectionalSteering = !useDirectionalSteering;
            // R：反響経路の表示 ON/OFF。
            if (Input.GetKeyDown(KeyCode.R)) showReflectionPaths = !showReflectionPaths;
            // C：回折候補経路の表示 ON/OFF。
            if (Input.GetKeyDown(KeyCode.C)) showDiffractionCandidates = !showDiffractionCandidates;
            // F：早期反射（像源→IR の R タップ）の ON/OFF。エンジン側の抽出ごと止まる（設定は毎フレーム渡している）。
            if (Input.GetKeyDown(KeyCode.F)) enableEarlyReflections = !enableEarlyReflections;
            // B：全音源を 音楽 ↔ 足音SE(WalkSE_01) に切替。
            if (Input.GetKeyDown(KeyCode.B)) SwitchSourceSound();
            // P：扉の定点 ON/OFF（聴き比べ用。sharedTailBus が ON のときだけ効く）。
            if (Input.GetKeyDown(KeyCode.P)) portalTail = !portalTail;
            // 1〜4：成分のソロ（直接音／反射／回折／後期尾）。
            //   ★「何が定位を持っているか」を耳で切り分けるための口。仕組み（IrConvolver.Solo）は
            //     前からあったのに**キーが無く、実行中に切り替えられなかった**。
            //     2026-09-03「閉扉で壁の奥の定位が反転する」を追うときに、F（早期反射のエミッタ）だけでは
            //     足りず、尾と回折を個別に落とせないと切り分けられないと分かって足した。
            //   ⚠ F と 2 は別物。F はエンジンの像源抽出そのもの、2 は畳み込み器の中の反射タップの通過。
            if (Input.GetKeyDown(KeyCode.Alpha1)) IrConvolver.Solo.PassDirect = !IrConvolver.Solo.PassDirect;
            if (Input.GetKeyDown(KeyCode.Alpha2)) IrConvolver.Solo.PassReflect = !IrConvolver.Solo.PassReflect;
            if (Input.GetKeyDown(KeyCode.Alpha3)) IrConvolver.Solo.PassDiffract = !IrConvolver.Solo.PassDiffract;
            if (Input.GetKeyDown(KeyCode.Alpha4)) IrConvolver.Solo.PassTail = !IrConvolver.Solo.PassTail;
            // M：音源の AudioSource を一括ミュート/復帰（尾やクリックだけ聞く用）。畳み込み器はそのまま走る。
            if (Input.GetKeyDown(KeyCode.M)) SetMuted(!_muted);
            // V：回折二次音源（開口ごとの F タップ）の ON/OFF。設定は毎フレーム渡しているので旗だけ返す。
            if (Input.GetKeyDown(KeyCode.V)) enableDiffractionSources = !enableDiffractionSources;
            // Y：DSP 経路の切替（C# の IrConvolver ⇔ C++ の VoiceConvolver）。
            //   同じタップ束を食わせて鳴らし比べるためのもの。移行が正しければ同じ音が出る。
            //   ★扉の開閉の連続性（タップのパラメータ補間）は C++ 側にしか入っていないので、
            //     「開けた瞬間ガタっと変わる」を聞き分けるときは必ずこちらで確認すること。
            if (Input.GetKeyDown(KeyCode.Y)) SetDspPath(!useCppDsp);
            // L：部屋の材質（AcousticSurface の無い occluder の既定）を巡る。反射・尾・Sabine の RT60 が一緒に変わる。
            if (Input.GetKeyDown(KeyCode.L)) occluderMaterial = NextRoomPreset(occluderMaterial);
            // HUD の出し入れ。既定 None なのでキーは奪わない（シーンで割り当てたときだけ効く）。
            if (hudToggleKey != KeyCode.None && Input.GetKeyDown(hudToggleKey)) showHud = !showHud;

            _fps = Mathf.Lerp(_fps, 1f / Mathf.Max(1e-4f, Time.unscaledDeltaTime), 0.1f);

            if (enableMovement) HandleMovement();

            // 1) 動的ジオメトリ更新（動いた分だけ）。
            //    メッシュも transform を送るだけ。形状の再構築は起きない
            //    （移動・回転・スケール変化＝配置の操作なので）。
            for (int i = 0; i < _occluders.Count; i++)
            {
                var o = _occluders[i];
                if (o.col == null || o.instanceId < 0) continue;
                GetObb(o, out Vector3 c, out Vector3 half, out Vector3 right, out Vector3 up);
                _scene.UpdateInstance(o.instanceId, c, half, right, up);
            }

            // 2) 音源位置バッファ。
            for (int i = 0; i < _sources.Length; i++)
                _srcPos[i] = _sources[i] != null ? _sources[i].position : listener.position;

            // 2-b) リスナー/音源をエンジンに登録し、バッチ更新を1発回す（API移行 段2）。
            //   エッジカタログ / 遮蔽・回折 / 早期反射 / 回折二次音源 / エコグラム を
            //   エンジンが内部レートで実行する。ホストはカウンタを持たない
            //   （＝「どの計算をいつ走らせるか」はエンジンの知識。移植時に書き直さずに済む）。
            //   ID は SourceId(i)（AF_SceneSetSource の鍵）。
            _scene.SetListener(listener.position);
            _scene.SetListenerOrientation(listener.forward, listener.up);
            for (int i = 0; i < _sources.Length; i++)
                _scene.SetSource(SourceId(i), _srcPos[i]);
            if (tapsFromEngine) PushTapParams();
            // 段の予算と順位（主スレッドの上限）。予算と音量はここで押す。順位の規則はエンジンに 1 つだけ。
            //   重要度と固定は AcousticSourcePriority（音源に付ける部品）。無い音源は既定（1・固定なし）のまま。
            _scene.SetTierBudget(tierBudgetExact, tierBudgetSimple);
            for (int i = 0; i < _sources.Length; i++)
            {
                var a = (_srcAudio != null && i < _srcAudio.Length) ? _srcAudio[i] : null;
                _scene.SetSourceLoudness(SourceId(i), a != null ? a.volume : 1f);
                var pr = (_srcPriority != null && i < _srcPriority.Length) ? _srcPriority[i] : null;
                if (pr != null) _scene.SetSourceImportance(SourceId(i), pr.importance, pr.pinned);
            }

            // 3) バッチ更新（ここから音響計算の時間計測）。
            _acStopwatch.Restart();
            _scene.SetUpdateConfig(BuildUpdateConfig());
            // 開口の演出つまみ。Inspector で動かしたら次のフレームから効く。
            UpdatePortals();          // 扉ごと動く戸口もあるので配置を送り直す
            // 部屋の材質の切り替え（L キー／Inspector）。既定の材質を**書き換える**ので、使っている実体が一斉に変わる。
            //   部屋の Sabine（RT60）も DLL の中で作り直される（反射・尾・RT60 が同じ材質。答えは 1 つ）。
            if (occluderMaterial != _appliedOccluderMaterial)
            {
                var rm = AcousticMaterial.FromPreset(occluderMaterial);
                _scene.SetMaterial(_materialId, rm.transmission, rm.absorption, rm.scattering);
                _appliedOccluderMaterial = occluderMaterial;
            }
            if (liveMaterialUpdate)   // Play 中に材質を切り替えて聞き比べたいとき
                for (int i = 0; i < _occluders.Count; i++)
                {
                    var o = _occluders[i];
                    if (o.instanceId >= 0)
                        _scene.SetInstanceMaterial(o.instanceId, ResolveMaterialId(o));
                }
            _scene.SetApertureContrast(apertureContrast);
            _scene.SetApertureTimbre(apertureTimbre);
            _scene.SetApertureIsTransmission(apertureIsTransmission);
            _scene.SetUseBtm(useBtmDiffraction);
            _scene.SetDirectPenumbra(directPenumbraMode);   // 中で丸めるだけ。毎フレーム押してよい
            // 部屋の検出設定。中で値の変化を見ているので、毎フレーム押しても作り直しは起きない。
            _scene.SetRoomCellSize(roomCellSize);
            _scene.SetRoomSeedRadius(roomSeedRadius);
            // 【C1】開口からポータルを自動生成する。中で値の変化を見ている。
            _scene.SetAutoPortalMinArea(autoPortalMinArea);
            _scene.SetPortalGovernRange(portalGovernRange);
            _scene.SetAutoPortals(autoPortals);
            // 【扉の定点】外へ開く口（洞窟の口）も自動ポータルにする。定点が使うときだけ。
            _scene.SetOutsideApertures(portalTail && sharedTailBus && autoPortals);
            // 音源ごとの段を何コアで回すか。中で値の変化を見ているので毎フレーム押してよい。
            _scene.SetWorkerThreads(workerThreads);
            _scene.SetAsync(asyncUpdate);   // 変わらなければ何もしない
            _scene.Update(Time.deltaTime);
            // 格子が上限に当たって粗くなっていたら警告する（中で 1 度だけ出す）。
            //   広い地面を 1 枚置くだけで roomCellSize が黙って無視されるので、
            //   気づけないと「部屋が割れていない」ことに最後まで気づかない。
            _scene.CheckRoomGridDegraded();
            _scene.CheckRoomsFound(_occluders.Count, roomSeedRadius);
            // 部屋が 2 つ以上あるのにポータルが 0 枚なら警告する（中で 1 度だけ）。
            //   フレネル帯域積分が一度も走らない状態＝主題が動いていない状態。
            _scene.CheckPortalsPresent();

            // 3-b) 音源ごとの帯域別生存と到来方向を受け取る。
            for (int i = 0; i < _sources.Length; i++)
            {
                int idx = _scene.SourceIndex(SourceId(i));
                if (idx < 0) continue;
                _scene.GetSourceOcclusion(idx, _bands);
                for (int b = 0; b < AcousticEngine.NumBands; b++)
                    _bandsPerSource[i * AcousticEngine.NumBands + b] = _bands[b];
                Vector3 ad = _scene.GetSourceArrivalDir(idx);
                _arrivalDir[i * 3] = ad.x; _arrivalDir[i * 3 + 1] = ad.y; _arrivalDir[i * 3 + 2] = ad.z;
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
                BuildAllSourceTaps();
                UpdateDirectionalTail();
            }

            // 6) 残響（低レートでエコグラム→RT60/wet。畳み込み器が Status 経由で読む）。
            if (enableReverb && _echogram != null)
            {
                // 段2: 計算はバッチ更新（内部レート）で済んでいるので、結果を受け取るだけ。
                //   エンジン側が再計算していないフレームでは前回と同じ内容が返るので、
                //   中身が変わったときだけ後段（IR再生成など）へ知らせる。
                // ここは部屋全体の響き（RT60/wet の算出用）なので全音源の和(-1)。
                // 音源ごとの尾は各畳み込み器が自分の EngineIndex で引く。
                if (_scene.GetEchogramBands(-1, _echogramBands, _echogram.Length) > 0)
                {
                    // 広帯域エコグラム（RT60/wet 用）は帯域平均として導出する。
                    bool changed = false;
                    for (int k = 0; k < _echogram.Length; k++)
                    {
                        float e = 0f;
                        for (int b = 0; b < nb; b++) e += _echogramBands[k * nb + b];
                        e /= nb;
                        if (!changed && !Mathf.Approximately(_echogram[k], e)) changed = true;
                        _echogram[k] = e;
                    }
                    Status.EchogramBands = _echogramBands;
                    Status.EchogramBinMs = echogramBinMs;
                    Status.DistanceRef = distanceRef;
                    Status.EchogramBinCount = _echogram.Length;
                    if (changed) Status.EchogramVersion++;
                    // ★順番が逆になった。wet は残響/直接の物理比 t から出すようになったので、
                    //   先に比を確定させる（旧: wet を出してから比を出していた）。
                    UpdateReverbTargetRatio();
                    UpdateReverbFromEchogram();
                    UpdatePortalTail();
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
        }

        // モニター窓向け：主音源の帯域別生存＋出力(マスターL/R)波形を static へ流す。
        private void PublishMonitors()
        {
            if (_monBandGains != null && _bandsPerSource != null)
            {
                for (int b = 0; b < AcousticEngine.NumBands; b++) _monBandGains[b] = _bandsPerSource[b];
                LatestBandGains = _monBandGains;
            }
            if (OutHistoryL != null)
            {
                // 出力レベルは Unity のリスナー出力（＝マスター相当）から採る。
                //   旧: Wwise のマスターバス RMS。畳み込み器の音は Unity 側を通るので、こちらが実際に聞こえている量。
                if (_outTmp == null) _outTmp = new float[256];
                AudioListener.GetOutputData(_outTmp, 0);
                OutHistoryL[OutHistoryHead] = Rms(_outTmp);
                AudioListener.GetOutputData(_outTmp, 1);
                OutHistoryR[OutHistoryHead] = Rms(_outTmp);
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
                for (int i = 0; i < _sources.Length; i++) _statusNames[i] = SourceName(i);
                Status.SourceNames = _statusNames;
            }
            Status.Occlusion = _occSmoothed;
            Status.Survival = _srcSurvival;
            Status.DiffDelta = _diffDelta;
            Status.DiffCandCount = _diffCandCount;
            Status.UseEdgeCatalog = useEdgeCatalog;
            Status.EdgeCatalogCount = (_scene != null) ? _scene.EdgeCatalogCount() : 0;
            Status.ShowDiffCandidates = showDiffractionCandidates;
            Status.EarlyReflEnabled = enableEarlyReflections;
            Status.DiffSrcEnabled = enableDiffractionSources;
            Status.DiffDistanceOnly = diffractionDistanceOnly;
            Status.BandsTransmit = _bands;
            Status.BandsDiffract = _diffBands;
            Status.EqDb = _eqDbSmoothed;
            Status.RtSeconds = _reverbDecay;
            Status.Wet = _reverbWet;
            // タップ・到来方向・遮蔽レベルは音源ごとに BuildAllSourceTaps が書く
            // （Status.Taps）。Status.Tap* と SourceLevel はその音源0のエイリアス。
        }

        // 後期残響の方向分布を測り、尾の左右バランス（耳ごと×6帯域）に落とす。
        //
        //   後期残響は拡散音場なので「いつ届くか」はエコーグラムが持てばよく、足りないのは
        //   「どちらから届くか」だけ。それは音源に依存せずリスナー位置だけで決まるので、
        //   音源が何個あってもこの計算は 1 回で済む。
        //
        //   尾IRはチャンネルごとに独立ノイズを持つ（＝L/R が無相関で広がる）ので、
        //   耳ごとの帯域ゲインを掛けるだけで方向がつく。畳み込みのコストは増えない。
        //
        //   平均が 1 になるよう正規化する。全体の音量は変えず、左右の比だけを動かす
        //   ── 音量まで動くと「部屋差は減衰時間で出す」という C-2 の結論を壊すため。
        private void UpdateDirectionalTail()
        {
            if (!enableDirectionalTail || _scene == null || !_scene.IsValid || listener == null)
            {
                Status.TailEarBandGain = null;
                return;
            }
            if (--_probeCountdown > 0) return;
            _probeCountdown = Mathf.Max(1, probeUpdateEveryFrames);

            int nd = Mathf.Clamp(probeRays, 8, 256);
            int nb = AcousticEngine.NumBands;
            if (_probeDirs == null || _probeDirs.Length != nd) _probeDirs = FibonacciSphere(nd);
            if (_probeEnergy == null || _probeEnergy.Length < nd * nb) _probeEnergy = new float[nd * nb];
            if (_tailEarBandGain == null)
            {
                _tailEarBandGain = new float[nb * 2];
                for (int i = 0; i < _tailEarBandGain.Length; i++) _tailEarBandGain[i] = 1f;
            }

            _scene.ProbeDirectionalEnergy(listener.position, _probeDirs, nd,
                                          Mathf.Clamp(probeBounces, 2, 24), _probeEnergy);

            // 各方向を「左耳寄り／右耳寄り」に重み付けして帯域ごとに集める。
            //   カーディオイド 0.5+0.5*cos は、真横で 1/0、正面と後方で 0.5/0.5。
            //   ＝正中面の音は左右均等に配られ、横方向だけが偏る。
            Vector3 earAxis = listener.right;   // +X が右耳側
            for (int b = 0; b < nb; b++)
            {
                float sumL = 0f, sumR = 0f;
                for (int i = 0; i < nd; i++)
                {
                    float e = _probeEnergy[i * nb + b];
                    if (e <= 0f) continue;
                    float c = Vector3.Dot(_probeDirs[i], earAxis);   // +1=右 / -1=左
                    sumR += e * (0.5f + 0.5f * c);
                    sumL += e * (0.5f - 0.5f * c);
                }
                float mean = 0.5f * (sumL + sumR);
                float gl = 1f, gr = 1f;
                if (mean > 1e-6f)
                {
                    // 平均1に正規化 → 強度で誇張／減衰 → 極端な値を抑える。
                    gl = Mathf.Lerp(1f, sumL / mean, directionalTailStrength);
                    gr = Mathf.Lerp(1f, sumR / mean, directionalTailStrength);
                    gl = Mathf.Clamp(gl, 0.25f, 2f);
                    gr = Mathf.Clamp(gr, 0.25f, 2f);
                }
                // 急に変わるとリバーブが揺れるので追従を鈍らせる。
                float k = 1f - Mathf.Exp(-Time.deltaTime * Mathf.Max(1, probeUpdateEveryFrames)
                                          / Mathf.Max(0.05f, directionalTailSmoothSec));
                _tailEarBandGain[b] = Mathf.Lerp(_tailEarBandGain[b], gl, k);
                _tailEarBandGain[nb + b] = Mathf.Lerp(_tailEarBandGain[nb + b], gr, k);
            }

            Status.TailEarBandGain = _tailEarBandGain;
            Status.TailDirBalance = (_tailEarBandGain[0] > 1e-4f)
                ? _tailEarBandGain[nb] / _tailEarBandGain[0] : 1f;

            // 診断: 場そのものに構造があるかを見る。リスナー座標系の6軸へ投影（125Hz）。
            //   L/R バランスが 1.00/1.00 でも、前後や上下に差があれば「場は測れているが
            //   鳴らし方が捉えていない」と分かる。逆に全軸が同じなら場自体が平坦。
            if (_probeAxis == null) _probeAxis = new float[6];
            Vector3[] axes = {
                listener.right, -listener.right, Vector3.up, Vector3.down,
                listener.forward, -listener.forward,
            };
            for (int a = 0; a < 6; a++)
            {
                float sum = 0f, wsum = 0f;
                for (int i = 0; i < nd; i++)
                {
                    float w = Vector3.Dot(_probeDirs[i], axes[a]);
                    if (w <= 0f) continue;                 // その軸の半球だけ、cos 重みで
                    sum += _probeEnergy[i * nb] * w;
                    wsum += w;
                }
                _probeAxis[a] = (wsum > 1e-6f) ? sum / wsum : 0f;
            }
            Status.ProbeAxisEnergy = _probeAxis;
        }

        // 球面上のほぼ等分布な方向（フィボナッチ格子）。エンジン側と同じ考え方。
        private static Vector3[] FibonacciSphere(int n)
        {
            var d = new Vector3[n];
            const float ga = 2.39996323f;   // 黄金角
            for (int i = 0; i < n; i++)
            {
                float y = 1f - 2f * (i + 0.5f) / n;
                float r = Mathf.Sqrt(Mathf.Max(0f, 1f - y * y));
                float th = ga * i;
                d[i] = new Vector3(Mathf.Cos(th) * r, y, Mathf.Sin(th) * r);
            }
            return d;
        }

        // 全音源ぶんのタップを組む。IR の生材料＝時間軸。
        //   各タップ = {相対遅延(ms, 直接=0), 6帯域ゲイン, 到来方向のパン, 種別}。
        //   遅延 = (経路長 − 直接距離) ÷ 音速。
        private void BuildAllSourceTaps()
        {
            if (_scene == null || !_scene.IsValid || _srcPos == null || _srcPos.Length == 0) return;
            if (_taps == null || _taps.Length != _srcPos.Length)
            {
                _taps = new SourceTaps[_srcPos.Length];
                for (int i = 0; i < _taps.Length; i++) _taps[i] = new SourceTaps();
                Status.Taps = _taps;
            }
            for (int i = 0; i < _srcPos.Length; i++)
            {
                // DLL の組み立て（既定）。口が無い古い DLL、または OFF なら従来の自前の組み立て。
                if (tapsFromEngine && _scene.TapBuilderAvailable && FillTapsFromEngine(i, _taps[i])) continue;
                BuildTapsForSource(i, _taps[i]);
            }
            if (tapsFromEngine && _taps.Length > 0 && _taps[0].TailRatio >= 0f)
            {
                // 表示は音源 0 の値（従来どおり）。畳み込み器は音源ごとの値を読む。
                Status.ReverbTargetRatio = _taps[0].TailRatio;
                Status.ReverbPhysicalRatio = _taps[0].TailRatioPhysical;
                Status.MixingTimeMs = _taps[0].MixingTimeMs;
                Status.RoomShare = _taps[0].RoomShare;
            }

            // 主音源(0)はモニタウィンドウ用にエイリアスしておく。
            var t0 = _taps[0];
            Status.TapCount = t0.Count;
            Status.ItdgMs = t0.ItdgMs;
            Status.TapDelayMs = t0.DelayMs;
            Status.TapGain = t0.Gain;
            Status.TapBandGain = t0.BandGain;
            Status.TapPanL = t0.PanL;
            Status.TapPanR = t0.PanR;
            Status.TapType = t0.Type;
            Status.DirectDirLocal = t0.DirectDirLocal;
            Status.SourceLevel = t0.SourceLevel;
        }

        // #2: 音源 si の全経路を「タップ」に束ねる（直接/反射/回折）。
        // DLL の組み立ての規則の値（つまみ）を押す。規則そのものは DLL に 1 つ（tap_builder.h）。
        private void PushTapParams()
        {
            var p = new Native.AFTapParams
            {
                distanceRef = distanceRef,
                diffractionDistanceRef = diffractionDistanceRef,
                diffractionDistancePower = diffractionDistancePower,
                airAbsorptionScale = airAbsorptionScale,
                transmissionTilt = transmissionTilt,
                transmissionHighCutDb = transmissionHighCutDb,
                transmissionGainDb = transmissionGainDb,
                diffractionGainDb = diffractionGainDb,
                diffractionDistanceOnly = diffractionDistanceOnly ? 1 : 0,
                diffractionHighCutDb = diffractionHighCutDb,
                tapSmoothTime = tapSmoothTime,
                directionSmoothTime = directionSmoothTime,
                steerThreshold = steerThreshold,
                diffractionHrtf = diffractionHrtf ? 1 : 0,
                diffractionHrtfMarginDb = diffractionHrtfMarginDb,
                reverbRatioExponent = reverbRatioExponent,
                reverbRatioCeiling = reverbRatioCeiling,
                roomBlendRadius = roomBlendRadius,
                reverbShareFade = reverbShareFade ? 1 : 0,
                fallbackRt60 = Mathf.Max(0.05f, _reverbDecay),
                maxTaps = SourceTaps.MaxTaps,
                diffractionTapReserve = 8,
            };
            _scene.SetTapParams(ref p);
        }

        // DLL が組み立てたタップを SourceTaps へ写す（計算はしない）。写せなければ false。
        private bool FillTapsFromEngine(int si, SourceTaps ts)
        {
            int idx = _scene.SourceIndex(SourceId(si));
            if (idx < 0) return false;
            if (!_scene.GetVoiceProgram(idx, out var pr)) return false;
            int nb = AcousticEngine.NumBands;
            int n = Mathf.Min(pr.count, SourceTaps.MaxTaps);
            for (int i = 0; i < n; i++)
            {
                var t = pr.taps[i];
                int o = i * nb;
                ts.BandGain[o + 0] = t.g0; ts.BandGain[o + 1] = t.g1; ts.BandGain[o + 2] = t.g2;
                ts.BandGain[o + 3] = t.g3; ts.BandGain[o + 4] = t.g4; ts.BandGain[o + 5] = t.g5;
                ts.Gain[i] = (t.g0 + t.g1 + t.g2 + t.g3 + t.g4 + t.g5) / 6f;
                ts.DelayMs[i] = t.delayMs;
                ts.PanL[i] = t.panL; ts.PanR[i] = t.panR;
                ts.Type[i] = (t.type == 0) ? 'D' : (t.type == 1) ? 'R' : 'F';
                ts.Arrival[i] = new Vector3(t.arrX, t.arrY, t.arrZ);
                ts.DirLocal[i] = new Vector3(t.dirX, t.dirY, t.dirZ);
            }
            ts.Count = n;
            ts.EngineIndex = idx;
            ts.TailShapeIndex = pr.tailShapeIndex >= 0 ? pr.tailShapeIndex : idx;
            if (ts.TailShapeIndex == idx && _srcPos != null && si < _srcPos.Length)
                _shapeRoom[idx] = _scene.RoomAt(_srcPos[si]);
            ts.ItdgMs = pr.itdgMs;
            ts.SourceLevel = pr.sourceLevel;
            ts.FreeFieldDirect = pr.freeFieldDirect;
            ts.DirectDirLocal = new Vector3(pr.directDirX, pr.directDirY, pr.directDirZ);
            ts.HrtfTapIndex = pr.hrtfTapIndex;
            ts.HrtfTapDirLocal = new Vector3(pr.hrtfDirX, pr.hrtfDirY, pr.hrtfDirZ);
            ts.HrtfHasPrev = pr.hrtfTapIndex >= 0;
            ts.TailRatio = pr.tailRatio;
            ts.TailRatioPhysical = pr.tailRatioPhysical;
            ts.MixingTimeMs = pr.mixingTimeMs;
            ts.RoomShare = pr.roomShare;
            return true;
        }

        private void BuildTapsForSource(int si, SourceTaps ts)
        {
            ts.TailRatio = -1f; ts.MixingTimeMs = -1f;   // 自前の組み立て: 尾の量はホストの全体値
            ts.Count = 0;
            ts.ItdgMs = 0f;
            Vector3 lp = listener.position;
            Vector3 sp = _srcPos[si];
            float directDist = Vector3.Distance(lp, sp);
            float toMs = 1000f / kSpeedOfSound;  // 距離(m) → ms（÷c ×1000）

            int n = 0;
            int nb6 = AcousticEngine.NumBands;
            _diffGainLin = (Mathf.Abs(diffractionGainDb) > 0.01f)
                         ? Mathf.Pow(10f, diffractionGainDb / 20f) : 1f;
            if (_directBands == null) _directBands = new float[nb6];
            if (_diffTapBands == null) _diffTapBands = new float[nb6];
            if (_srcTransmit == null) _srcTransmit = new float[nb6];
            if (_srcDiffract == null) _srcDiffract = new float[nb6];

            // 透過と回折はこの音源ぶんを引く。主音源だけ Update で取った値を流用する
            //   （表示用に _bands/_diffBands へ入っているので二度引かない）。
            float[] tr, di;
            if (si == 0) { tr = _bands; di = _diffBands; }
            else
            {
                _scene.ComputeTransmissionBands(sp, lp, _srcTransmit);
                _scene.ComputeDiffractionBands(sp, lp, _srcDiffract);
                tr = _srcTransmit; di = _srcDiffract;
            }

            // 段3: 早期反射/回折二次音源はバッチ更新で計算済み。ここでは結果を受け取るだけ。
            int idx = _scene.SourceIndex(SourceId(si));
            // 畳み込み器が「自分の音源のエコグラム」を引けるよう控えておく。
            ts.EngineIndex = idx;

            // 【尾の形は部屋で共有】★規則はエンジン側に 1 つだけ置いてある。
            //   以前はここでも「同じ部屋のいちばん若い index」を自前で解いていたが、
            //   **同じ規則が 2 箇所にある**と、片方だけ変えたときに
            //   「エンジンが計算した代表」と「ホストが読む代表」がずれる。
            //   （代表の選び方が「若い順」なのは、位置で選ぶと音源が動くたびに
            //     代表が入れ替わって尾の形が乗り換わり、段差になるため。）
            ts.TailShapeIndex = _scene.GetTailShapeIndex(idx);
            if (ts.TailShapeIndex < 0) ts.TailShapeIndex = idx;
            // 【扉の定点】この音源が部屋の代表なら「尾の形の index → 部屋番号」を控える。
            //   バスの鍵は形の index なので、定点が「どの部屋のバスか」を引くのに要る。
            if (ts.TailShapeIndex == idx && _srcPos != null && si < _srcPos.Length)
                _shapeRoom[idx] = _scene.RoomAt(_srcPos[si]);

            // 回折の開口は直接タップの決定に要る（下記）ので先に取る。
            int df = _scene.GetDiffractionSources(idx, _tapDiffPos, _tapDiffGain);

            // ★直接タップ（基準 0ms）＝ **ソフト遮蔽の透過**。分岐なし。
            //
            //   以前は「見通せているか(lit)」で意味を切り替えていた:
            //     lit  → 回折ゲイン（境界で 0.56）
            //     !lit → 透過（壁材なら 0.12）＋ F タップが開口に出現
            //   境界を跨いだ瞬間に 13dB 落ち、同時に音が音源方向から開口方向へワープしていた。
            //   これが「遮蔽ON/OFFの差が激しすぎる」の主因。
            //
            //   ソフト遮蔽は音源まわりの円盤をサンプルするので、掠める位置では
            //   「一部だけ遮られる」状態がそのまま数値になる（見通し 1.0 → 境界 約0.5 → 影 材質の透過）。
            //   連続なので分岐が要らない。
            if (_softTrans == null) _softTrans = new float[nb6];
            float occFrac;
            if (!_scene.ComputeSoftOcclusion(lp, sp, _softTrans, out occFrac))
            {
                for (int b = 0; b < nb6; b++) _softTrans[b] = tr[b];
                occFrac = 0f;
            }

            // ★聞こえ方の調整: 透過音のこもりはここで作る（LPF の主役）。
            //   材質の6帯域カーブの**形**は残したまま、125Hz を基準に傾きだけ動かす。
            //   形ごと差し替えないのは、材質の違い（コンクリ／木の扉）が消えてしまうため。
            ApplyTilt(_softTrans, nb6, transmissionTilt, transmissionHighCutDb);
            if (Mathf.Abs(transmissionGainDb) > 0.01f)
            {
                float tg = Mathf.Pow(10f, transmissionGainDb / 20f);
                for (int b = 0; b < nb6; b++) _softTrans[b] = Mathf.Min(_softTrans[b] * tg, 1f);
            }

            // ★平滑化。幾何は連続でも歩けば値は動くので、時間方向で均す。
            //   既存のゲーム音響が例外なく持っている処理で、ここには無かった。
            //   dB（対数）で補間する ── 振幅で補間すると、大きい側へは速く小さい側へは
            //   遅く動くので「立ち上がりだけ速い」不自然さが出る。
            {
                float dt = Time.deltaTime;
                float a = (tapSmoothTime > 1e-4f && dt > 0f)
                        ? 1f - Mathf.Exp(-dt / tapSmoothTime) : 1f;
                int bo2 = si * nb6;
                if (_tapSmInit != null && si < _tapSmInit.Length)
                {
                    if (!_tapSmInit[si])
                    {
                        _tapSmInit[si] = true; _tapOccSm[si] = occFrac;
                        for (int b = 0; b < nb6; b++) _tapTransSm[bo2 + b] = _softTrans[b];
                    }
                    else
                    {
                        _tapOccSm[si] += (occFrac - _tapOccSm[si]) * a;
                        for (int b = 0; b < nb6; b++)
                            _tapTransSm[bo2 + b] = SmoothLog(_tapTransSm[bo2 + b], _softTrans[b], a);
                    }
                    occFrac = _tapOccSm[si];
                    for (int b = 0; b < nb6; b++) _softTrans[b] = _tapTransSm[bo2 + b];
                }
            }
            WriteTapBands(ts, n++, _softTrans, 0, directDist, sp, 'D', 0f);

            // 反射タップ（像源位置から経路長→遅延、6帯域ゲイン×空気吸収）。
            int er = _scene.GetEarlyReflections(idx, _tapErPos, _tapErGain);
            for (int t = 0; t < er && n < SourceTaps.MaxTaps - 8; t++)   // 回折タップの枠（8）を残す。面の線は最大 48 本
            {
                float pl = Vector3.Distance(lp, _tapErPos[t]);  // 全経路長
                float rel = (pl - directDist) * toMs;
                if (rel < 0f) rel = 0f;
                WriteTapBands(ts, n++, _tapErGain, t * 6, pl, _tapErPos[t], 'R', rel);
            }
            // 回折タップ。開口の方向から、開口までの経路長ぶん遅れて鳴る。
            //   ★ゲインは6帯域。以前はスカラ(WriteTapFlat)だったので、回折の周波数依存
            //     ——低域ほど回り込む＝高域から落ちる——が音に乗っていなかった。
            //     遮蔽感の正体はこのハイ落ちなので、エンジンが計算した情報を捨てていたことになる。
            //   配分: エンジンの6帯域回折ゲイン(_diffBands = 回り込む総量) を、
            //     開口ごとの相対重み(_tapDiffGain, 合計1に正規化済み) で割り振る。
            //   ★さらに**遮蔽割合(occFrac)で重み付け**する。
            //     見通せているとき occFrac=0 なので回折タップは 0 になり、直接タップとの
            //     二重計上が起きない。境界では 0.5 前後で滑らかに入れ替わる。
            //     これで「見通せているか」の分岐なしに配分できる。
            //   ★diffractionDistanceOnly なら周波数依存の減衰を掛けない。
            //     残るのは「開口の方向」「開口までの総経路長ぶんの遅延と距離減衰」だけ。
            //     距離減衰と空気吸収は WriteTapBands が pl から掛けるので、ここでは
            //     開口間の配分(_tapDiffGain)と、直接音との切り替え(occFrac)だけを渡す。
            for (int t = 0; t < df && n < SourceTaps.MaxTaps; t++)
            {
                if (occFrac <= 1e-4f) break;                 // 見通せている＝回折の出番なし
                float pl = Vector3.Distance(lp, _tapDiffPos[t]);
                float rel = (pl - directDist) * toMs;
                if (rel < 0f) rel = 0f;
                float share = _tapDiffGain[t] * occFrac * _diffGainLin;
                for (int b = 0; b < nb6; b++)
                    _diffTapBands[b] = diffractionDistanceOnly ? share : di[b] * share;
                // 既定 0 = 平坦のまま。回折は定位担当なので削らないのが筋だが、
                //   少し丸めたいときのための逃げ道（上げすぎると定位が消える）。
                if (diffractionHighCutDb > 0.01f)
                    ApplyTilt(_diffTapBands, nb6, 1f, diffractionHighCutDb);
                WriteTapBands(ts, n++, _diffTapBands, 0, pl, _tapDiffPos[t], 'F', rel);
            }
            ts.Count = n;

            // ITDG = 直接以外の最小遅延（＝広さの主要な手がかり）。
            float best = float.MaxValue;
            for (int i = 0; i < n; i++)
                if (ts.Type[i] != 'D' && ts.DelayMs[i] < best) best = ts.DelayMs[i];
            ts.ItdgMs = (best == float.MaxValue) ? 0f : best;

            // 残響の送出量。「その音源が部屋にエネルギーを注げているか」＝反射込みの生存。
            //   直線透過だけで測ると、衝立の裏に回っただけで残響が -35dB 消える（DEV_LOG D章）。
            int bo = si * nb6;
            ts.SourceLevel = (_bandsPerSource != null && bo + nb6 <= _bandsPerSource.Length)
                ? Mean6(_bandsPerSource, bo) : Mean6(tr, 0);

            // HRTF 用の到来方向。実測の見かけ方向（遮蔽時は回り込む向き）をリスナー座標系へ。
            if (listener != null && _apparentDir != null && si < _apparentDir.Length)
            {
                Vector3 w = _apparentDir[si];
                if (w.sqrMagnitude < 1e-8f) w = sp - lp;
                if (w.sqrMagnitude > 1e-8f)
                    ts.DirectDirLocal = listener.InverseTransformDirection(w.normalized);
            }

            SelectDiffractionHrtfTap(ts, n);
        }

        // 【B1】HRTF に載せる回折タップを 1 本選ぶ。
        //
        //   ★毎フレーム単純に「いちばん強い 1 本」を選ぶと、強さが拮抗した 2 つの開口の
        //     間で選択が振動する。そのたびに HRTF の方向が飛んで像が左右に暴れるので、
        //     いま載せている開口を marginDb ぶん上回るまで乗り換えない。
        //   ★index ではなく**到来点の位置**で「同じ開口か」を判定する。タップの並び順は
        //     フレーム間で保証されていないので、index で覚えると別の開口に化ける
        //     （エンジン側のタップ対応付けが遅延で行っているのと同じ理由）。
        private void SelectDiffractionHrtfTap(SourceTaps ts, int count)
        {
            ts.HrtfTapIndex = -1;
            if (!diffractionHrtf || listener == null) { ts.HrtfHasPrev = false; return; }

            int best = -1;      float bestG = 0f;
            int keep = -1;      float keepG = 0f;
            const float kSameApertureDist = 1.5f;   // これ以内なら「同じ開口」とみなす
            float nearest = kSameApertureDist * kSameApertureDist;
            for (int i = 0; i < count; i++)
            {
                if (ts.Type[i] != 'F') continue;
                float g = ts.Gain[i];
                if (g <= 0f) continue;
                if (g > bestG) { bestG = g; best = i; }
                if (ts.HrtfHasPrev)
                {
                    float d2 = (ts.Arrival[i] - ts.HrtfPrevArrival).sqrMagnitude;
                    if (d2 < nearest) { nearest = d2; keep = i; keepG = g; }
                }
            }
            if (best < 0) { ts.HrtfHasPrev = false; return; }

            int pick = best;
            if (keep >= 0 && keep != best && diffractionHrtfMarginDb > 0f)
            {
                // 乗り換えるのは、新しい候補が今の開口を margin ぶん上回ったときだけ。
                float marginLin = Mathf.Pow(10f, diffractionHrtfMarginDb / 20f);
                if (bestG < keepG * marginLin) pick = keep;
            }

            ts.HrtfTapIndex = pick;
            ts.HrtfPrevArrival = ts.Arrival[pick];
            ts.HrtfHasPrev = true;
            Vector3 w = ts.Arrival[pick] - listener.position;
            if (w.sqrMagnitude > 1e-8f)
                ts.HrtfTapDirLocal = listener.InverseTransformDirection(w.normalized);
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

        // 6帯域(gOff..)×空気吸収(pathLen)×距離減衰 をタップnに書く＋到来方向のパン。
        // 6帯域カーブの傾きを調整する。**形は残して傾きだけ**動かす。
        //   tilt: 125Hz を基準にした指数。1=そのまま / >1=高域がさらに落ちる / <1=平ら寄り
        //   cutDb: 一律に足すローパス量。125Hz で 0dB、最上帯域で cutDb ぶん落とす。
        //   材質のカーブごと差し替えないのは、コンクリと木の扉の違いが消えてしまうため。
        private static void ApplyTilt(float[] g, int nb, float tilt, float cutDb)
        {
            if (g == null || nb <= 1) return;
            bool doTilt = Mathf.Abs(tilt - 1f) > 1e-3f;
            bool doCut = cutDb > 1e-3f;
            if (!doTilt && !doCut) return;
            float lo = Mathf.Max(g[0], 1e-6f);
            for (int b = 0; b < nb; b++)
            {
                float v = g[b];
                if (doTilt)
                {
                    // 125Hz に対する比を累乗する。b=0 では比が 1 なので値が動かない。
                    float rel = Mathf.Clamp(v / lo, 0f, 1f);
                    v = lo * Mathf.Pow(rel, tilt);
                }
                if (doCut)
                {
                    float f = (nb > 1) ? (float)b / (nb - 1) : 0f;   // 帯域は1オクターブ刻み＝対数で等間隔
                    v *= Mathf.Pow(10f, -(cutDb * f) / 20f);
                }
                g[b] = v;
            }
        }

        // dB（対数）での一次追従。振幅で補間すると大きい側へだけ速く動いて不自然になる。
        private static float SmoothLog(float cur, float target, float a)
        {
            const float floorAmp = 1e-5f;    // -100dB。無音を -inf にしないための床
            float c = Mathf.Log(Mathf.Max(cur, floorAmp));
            float t = Mathf.Log(Mathf.Max(target, floorAmp));
            float v = Mathf.Exp(c + (t - c) * a);
            return (v <= floorAmp * 1.01f && target <= 0f) ? 0f : v;
        }

        private void WriteTapBands(SourceTaps ts, int n, float[] g, int gOff, float pathLen,
                                   Vector3 arrival, char type, float delayMs)
        {
            AirAbsorptionBands(pathLen, _airTmp);
            float da = DistAtten(pathLen, type);   // ③ 絶対距離減衰（1/r）
            int nb = AcousticEngine.NumBands;
            int o = n * nb;
            float sum = 0f;
            for (int b = 0; b < nb; b++)
            {
                float v = g[gOff + b] * _airTmp[b] * da;
                ts.BandGain[o + b] = v;
                sum += v;
            }
            // 直接音タップのときだけ、遮蔽を含まない自由音場レベルを控えておく。
            //   尾の絶対レベルはこれを基準にする（BandGain だと遮蔽込みで、柱の陰で
            //   残響まで消えてしまう）。target がエネルギー比なので二乗平均平方根で束ねる。
            if (type == 'D')
            {
                float e = 0f;
                for (int b = 0; b < nb; b++) e += (_airTmp[b] * da) * (_airTmp[b] * da);
                ts.FreeFieldDirect = Mathf.Sqrt(e / nb);
            }
            ts.Gain[n] = sum / nb;                  // 広帯域（プロット/表示用）
            ComputePan(arrival, out ts.PanL[n], out ts.PanR[n]);
            ts.Type[n] = type; ts.DelayMs[n] = delayMs;
            ts.Arrival[n] = arrival;                // B1: HRTF に載せる開口を選ぶのに要る
            if (listener != null) {
                Vector3 wd = arrival - listener.position;
                ts.DirLocal[n] = (wd.sqrMagnitude > 1e-8f)
                               ? listener.InverseTransformDirection(wd.normalized)
                               : Vector3.forward;
            }
        }


        // ③ 絶対距離減衰（1/r・振幅）。distanceRef でゲイン1、以遠は refDist/pathLen。0で無効(=1)。
        //
        //   回折タップ('F')だけは別の基準距離・別の急峻さを持てる。
        //   diffractionDistanceOnly が ON のとき回折に残る情報は「開口の方向」と
        //   「開口までの総経路長」だけなので、この減衰が回折の性格そのものになる。
        //   直接音の距離感を保ったまま回り込みの遠達性だけ触れるようにしてある。
        private float DistAtten(float pathLen, char type)
        {
            float refDist = distanceRef;
            float power = 1f;
            if (type == 'F')
            {
                if (diffractionDistanceRef > 0f) refDist = diffractionDistanceRef;
                power = diffractionDistancePower;
            }
            if (refDist <= 0f) return 1f;
            float a = refDist / Mathf.Max(pathLen, refDist);
            // Pow は毎タップ毎フレーム走るので、既定値のときは呼ばない。
            return (power == 1f) ? a : Mathf.Pow(a, power);
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

        // wet量（反射割合）と RT60（尾の長さ）を算出し、Status へ出す（畳み込み器が読む）。
        //
        // ★wet の出し方を変えた。旧: (総和-ピーク)/総和。
        //   これは比なので、ピーク（＝直接音）が壁で消えると分母だけが落ちて 1.0 へ寄る。
        //   実測すると、直接音が遮蔽されていても(透過 0.0074)開けていても(1.0)
        //   全区間 0.94〜0.96 に張り付いて、位置の情報を持っていなかった。
        //   新: 残響/直接の物理比 t（部屋の体積と残響時間から出したもの）から
        //   wet = t/(1+t) に変換する。t は 0〜∞ なので wet は 0〜1 に単調・連続で写り、
        //   飽和しない（t=1 すなわち臨界距離でちょうど 0.5）。
        //
        // RT60 も部屋グラフ（Sabine）を優先する。エコグラム由来は窓が
        // bins×binMs（既定 1 秒）しかなく、吸音率が 17.6 倍違う 2 部屋を
        // 0.50s と 0.51s としか区別できていなかった（実測）。
        private void UpdateReverbFromEchogram()
        {
            int bins = _echogram.Length;
            float peak = 0f, total = 0f;
            for (int k = 0; k < bins; k++) { float e = _echogram[k]; if (e > peak) peak = e; total += e; }
            if (peak <= 0f) return;

            float floor = peak * 0.001f;   // -60dB
            int tailBin = 0;
            for (int k = 0; k < bins; k++) if (_echogram[k] > floor) tailBin = k;
            float rt60 = (tailBin + 1) * echogramBinMs * 0.001f;
            // 部屋が取れているならそちらを使う（幾何と材質から。窓の長さに縛られない）。
            if (Status.RoomRt60 > 0f) rt60 = Status.RoomRt60;

            // 物理比 → wet。比が取れていないときだけ従来の割合に落とす。
            float wet;
            if (Status.ReverbTargetRatio > 0f)
            {
                float t = Status.ReverbTargetRatio;
                wet = t / (1f + t);
            }
            else
            {
                float tailE = Mathf.Max(0f, total - peak);
                wet = Mathf.Clamp01(tailE / Mathf.Max(total, 1e-6f));
            }

            _reverbWet = Mathf.Lerp(_reverbWet, wet, 0.2f);
            _reverbDecay = Mathf.Lerp(_reverbDecay, rt60, 0.2f);
            Status.Wet = _reverbWet;
            Status.RtSeconds = _reverbDecay;
        }

        // ── 扉の定点（PortalEmitter）の管理 ──────────────────────────────────────
        //   部屋ごとのバスの「直接に足す割合 w」と、口ごとの定点の (1−w)・結合率・距離・遮蔽・向きを
        //   入れ直す。エコグラムの更新と同じ周期で呼ばれる（毎フレームではない）。
        //   ★部屋番号で切り替えず「まわりの何割がどの部屋か」で混ぜる（UpdateReverbTargetRatio と同じ理由）。
        private readonly Dictionary<int, int> _shapeRoom = new Dictionary<int, int>();   // 形の index → 部屋
        private readonly Dictionary<long, PortalEmitter> _portalEmitters = new Dictionary<long, PortalEmitter>();
        private TailBusRenderer _portalHost;
        private readonly int[] _ptRooms = new int[8];
        private readonly float[] _ptW = new float[8];
        private readonly float[] _ptTrans = new float[AcousticEngine.NumBands];
        private readonly float[] _ptGain = new float[AcousticEngine.NumBands];
        private int _ptFrame;

        private float PortalRoomWeight(int room, int n, float outsideW)
        {
            if (room < 0) return outsideW;
            for (int i = 0; i < n; i++) if (_ptRooms[i] == room) return _ptW[i];
            return 0f;
        }

        private void UpdatePortalTail()
        {
            bool on = portalTail && sharedTailBus && autoPortals && _audioReady
                      && _scene != null && _scene.IsValid && listener != null;
            if (_portalHost == null) _portalHost = FindFirstObjectByType<TailBusRenderer>();
            if (!on || _portalHost == null)
            {
                // 切ったら従来どおり: バスは全部足し、定点は黙る。
                foreach (var kv in _shapeRoom) _portalHost?.SetBusWeight(kv.Key, 1f);
                foreach (var kv in _portalEmitters) if (kv.Value != null) kv.Value.feedGain = 0f;
                return;
            }
            _ptFrame++;
            // リスナーのまわりの部屋の占め方（空間版。外の世界も分母）。外の世界の分 = 1 − 合計。
            //   ★部屋どうしの割合（GetRoomWeights）を使うと、外へ開く戸口の 1.8 m 手前で 0→1 と跳ぶ。
            int n = _scene.GetRoomShare(listener.position, roomBlendRadius, _ptRooms, _ptW);
            if (n <= 0)
            {
                // 占め方が取れない（部屋グラフが無い・古い DLL）。部屋の中にいるならその部屋を 1 にして、
                // 少なくとも従来どおり鳴らす（全部 0 にすると部屋の中で尾が消える）。
                int here = _scene.RoomAt(listener.position);
                if (here >= 0) { _ptRooms[0] = here; _ptW[0] = 1f; n = 1; }
            }
            float sum = 0f;
            for (int i = 0; i < n; i++) sum += _ptW[i];
            float outsideW = Mathf.Clamp01(1f - sum);
            // バスごとの直接の足し込み w。
            foreach (var kv in _shapeRoom)
                _portalHost.SetBusWeight(kv.Key, PortalRoomWeight(kv.Value, n, outsideW));
            // 口ごとの定点。
            _scene.GetPortalCounts(out int nAuto, out int nManual);
            int total = nAuto + nManual;
            Vector3 lp = listener.position;
            var alive = new HashSet<long>();
            for (int id = 0; id < total; id++)
            {
                if (!_scene.GetPortal(id, out Vector3 c, out Vector3 u, out Vector3 v, out float hu, out float hv)) continue;
                if (!_scene.GetPortalRooms(id, out int ra, out int rb, out bool toOutside)) continue;
                Vector3 nrm = Vector3.Cross(u, v).normalized;
                float side = Mathf.Sign(Vector3.Dot(lp - c, nrm));
                if (side == 0f) side = 1f;
                // 口に面した部屋それぞれについて、その部屋の尾をこの口から鳴らす定点を持つ。
                for (int k = 0; k < 2; k++)
                {
                    int room = (k == 0) ? ra : rb;
                    if (room < 0) continue;                      // 外の世界には尾のバスが無い
                    int shape = -1;
                    foreach (var kv in _shapeRoom) if (kv.Value == room) { shape = kv.Key; break; }
                    if (shape < 0) continue;                     // その部屋に音源が無い＝尾が無い
                    long key = ((long)id << 32) | (uint)room;
                    alive.Add(key);
                    if (!_portalEmitters.TryGetValue(key, out PortalEmitter em) || em == null)
                    {
                        var go = new GameObject($"AF_PortalEmitter_p{id}_r{room}");
                        go.transform.SetParent(transform, false);
                        em = go.AddComponent<PortalEmitter>();
                        em.portalId = id; em.room = room;
                        em.Init(portalTailHrtfFile);
                        _portalEmitters[key] = em;
                    }
                    em.busShape = shape;
                    em.busHandle = _portalHost.BusHandle(shape);
                    // 定点は口の面からリスナー側へ少し出す（閉じた板の中に置かない）。
                    em.transform.position = c + nrm * (side * 0.15f);
                    // (1 − その部屋の占め方) × 全体レベル。部屋の中では 0、外では 1。
                    float w = PortalRoomWeight(room, n, outsideW);
                    em.feedGain = (1f - w) * portalTailLevel;
                    // 口の結合率は周期を落として測る（半球の見通し 360 本）。
                    if ((_ptFrame % Mathf.Max(1, portalTailCouplingEveryFrames)) == 0 || em.coupling6[0] <= 0f)
                        _scene.PortalDiffuseCoupling(id, em.coupling6);
                    // 距離減衰（口の 1 m 以内は頭打ち＝戸口の真ん中でバスと同じ大きさ）と、口→リスナーの遮蔽。
                    Vector3 ep = em.transform.position;
                    float dist = Vector3.Distance(lp, ep);
                    float att = 1f / Mathf.Max(dist, 1f);
                    float occ;
                    if (!_scene.ComputeSoftOcclusion(lp, ep, _ptTrans, out occ))
                        for (int b = 0; b < _ptTrans.Length; b++) _ptTrans[b] = 1f;
                    for (int b = 0; b < _ptGain.Length; b++)
                        _ptGain[b] = em.coupling6[Mathf.Min(b, 5)] * att * _ptTrans[b];
                    Vector3 dirLocal = listener.InverseTransformDirection((ep - lp).normalized);
                    em.SetTap(_ptGain, dirLocal, 57f);
                }
            }
            // 消えた口の定点は黙らせる。
            foreach (var kv in _portalEmitters)
                if (kv.Value != null && !alive.Contains(kv.Key)) kv.Value.feedGain = 0f;
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
        // V と RT60 は **部屋グラフ**から取る（旧: occluder 全部の合成 AABB とエコグラム）。
        //
        // なぜ変えたか（実測、AcousticEngineTest の [診断] 戸口をまたいで歩いたときの残響送出）:
        //   ・旧 V は occluder の合成 AABB ＝ レベル全体の外形箱で、部屋を一切見ていなかった。
        //     2部屋(365/364m³)のシーンで全域 862m³。狭い部屋でも広間でも同じ値になる。
        //   ・旧 RT60 はエコグラムが「ピーク-60dB を超える最後のビン」＝離散インデックス。
        //     しかも窓が 100ビン×10ms=1秒しかないので、吸音率が 17.6 倍違う 2 部屋を
        //     0.50s と 0.51s（＝1.0倍）としか区別できていなかった。部屋を測れていない。
        //   ・新は部屋ごとの Sabine（RT60 = 0.161V/ΣSα）。同じ 2 部屋が 4.34s と 0.25s。
        //     値は幾何で決まる定数なので、リスナーが動いても値そのものは動かない。
        //
        // ★不連続にしないための肝: 部屋番号で切り替えず、「まわりの何割がどの部屋か」で
        //   混ぜる。切り替えにすると、プレイヤーが必ず通る戸口のど真ん中に段差が乗る。
        //   混ぜる半径 = 何メートルかけて入れ替わるか。実測の傾き（部屋をまたぐ 18dB の
        //   変化に対して）: 0.5m→2.67dB/0.1m / 1.0m→1.58 / 2.0m→0.92 / 3.0m→0.69。
        private void UpdateReverbTargetRatio()
        {
            if (_srcPos == null || _srcPos.Length == 0 || listener == null) return;

            float vol = _scene != null ? _scene.RoomVolumeAt(listener.position, roomBlendRadius) : 0f;
            float rt;
            if (vol > 1f)
            {
                // 部屋が見つかった。体積も残響時間も幾何と材質から。
                if (_roomRt60 == null) _roomRt60 = new float[6];
                _scene.GetRt60At(listener.position, roomBlendRadius, _roomRt60);
                rt = Mathf.Max(0.05f, _roomRt60[2]);       // 500Hz 帯を代表に
                Status.RoomVolume = vol;
                Status.RoomRt60 = rt;
            }
            else
            {
                // 部屋が取れない（屋外・囲われていない）。従来どおり外形箱で当てる。
                bool has = false;
                Bounds b = default;
                foreach (var o in _occluders)
                {
                    if (o.col == null) continue;
                    if (!has) { b = o.col.bounds; has = true; }
                    else b.Encapsulate(o.col.bounds);
                }
                if (!has) { Status.ReverbTargetRatio = 0f; return; }
                Vector3 sz = b.size;
                vol = Mathf.Max(1f, sz.x * sz.y * sz.z);
                rt = Mathf.Max(0.05f, _reverbDecay);
                Status.RoomVolume = 0f;
                Status.RoomRt60 = 0f;
            }

            float r = Mathf.Max(0.1f, Vector3.Distance(listener.position, _srcPos[0]));
            // 臨界距離（メートル法, RT60[s], V[m³]）。
            float rc = 0.057f * Mathf.Sqrt(vol / rt);
            float t = (r * r) / Mathf.Max(rc * rc, 1e-4f);

            // ── 知覚圧縮 ──
            // 物理エネルギー比をそのまま鳴らすと、部屋を変えたとき残響の量が過剰に振れる。
            // 人が「残響が多い」と感じる量はエネルギー比そのものではないため:
            //   ・先行音効果(precedence)で、直接音とその直後は融合して直接音側に強く重み付けされる
            //     → エネルギー的に残響が勝つ小部屋でも、耳は直接音を主として聞く（実際より乾く）
            //   ・そもそも部屋の広さの手がかりは主に減衰時間で、音量比ではない（DEV_LOG C-2）
            // そこで比を指数 exponent で圧縮する。1.0=物理そのまま / 0=常に一定（C-2の極）。
            // r=r_c（比=1）を不動点にしているので、圧縮しても「臨界距離で直接=残響」は保たれ、
            // そこから離れたときの振れ幅だけが穏やかになる。
            Status.ReverbPhysicalRatio = t;                 // 圧縮前（診断用）
            t = Mathf.Pow(t, reverbRatioExponent);

            // ── 上限 ──
            // ★以前は Clamp(t, 0, 50) だった。クランプは「上限に達した瞬間から、
            //   どれだけ離れても値が動かなくなる」＝そこで導関数が 0 に落ちる不連続で、
            //   遠くにいるあいだ張り付いたままになる。ソフトニー（比が上がるほど
            //   効きを緩める）に置き換える。50 で 50 のまま、そこから上は圧縮されて
            //   ceiling に漸近するので、どこまで離れても反応は残る。
            // ★外へ出るときの量（#5、2026-09-05）: まわりの空間のうち部屋の中である割合で縮める。
            //   V と RT60 を空間版で混ぜても rc が動かず量は連続にならない。量だけをこの割合で付ける。
            //   戸口の 1.3 m 手前で 0.98、戸口で 0.64、0.7 m 外で 0.15、2 m 外で 0（混ぜ半径 2 m）。
            float share = 1f;
            if (reverbShareFade && _scene != null && vol > 1f)
                share = Mathf.Clamp01(_scene.RoomShareTotalAt(listener.position, roomBlendRadius));
            Status.RoomShare = share;
            Status.ReverbTargetRatio = SoftCeiling(t, 50f, reverbRatioCeiling) * share;

            // mixing time ≈ √V(ms)（Polack の目安）。広い部屋ほど反射が拡散に溶けるのが遅い＝
            // 早期タップとして扱える時間が長い。特大部屋で遠壁の反射が早期窓から漏れるのを防ぐ。
            Status.MixingTimeMs = Mathf.Clamp(Mathf.Sqrt(vol), 5f, 500f);
        }

        // 上限のやわらかい当て方。knee までは素通し、そこから上は ceiling へ漸近する。
        //   x <= knee            : x
        //   x  > knee            : knee + (ceiling-knee) * (1 - exp(-(x-knee)/(ceiling-knee)))
        // 導関数が knee で 1 から連続に落ちていくので、上限付近で「張り付いて動かない」
        // 状態にならない。ceiling <= knee なら従来どおりの硬いクランプ。
        private static float SoftCeiling(float x, float knee, float ceiling)
        {
            if (x <= knee) return x;
            float span = ceiling - knee;
            if (span <= 0f) return knee;
            return knee + span * (1f - Mathf.Exp(-(x - knee) / span));
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

        private void OnDisable()
        {
            TeardownSourceAudio();
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
            // ★切る口。ここより下は何も描かない。
            //   デバッグ表示が残っていると「UI 無しで一周できるか」の判断そのものができない。
            if (!showHud) return;

            var style = new GUIStyle(GUI.skin.label) { fontSize = 13 };
            GUILayout.BeginArea(new Rect(10, 10, 480, 300), GUI.skin.box);
            GUILayout.Label($"[新コア Scene] {_status}", style);
            GUILayout.Label($"FPS: {_fps:F0}    音響計算: {_acousticMs:F2} ms/frame    " +
                            $"空間化: {(useHrtf ? "HRTF" : "パン")} (H)    " +
                            $"方向ステア: {(useDirectionalSteering ? "ON" : "OFF")} (G)", style);
            if (enableMovement)
                GUILayout.Label("操作: WASD / 右ドラッグ / QE / Shift / Space:重ね / H:HRTF / G:ステア / R:反響経路 / C:回折候補"
                                + " / F:早期反射(Rタップ) / V:回折二次音源(Fタップ) / Y:DSP経路(C#/C++) / B:クリップ差し替え"
                                + " / M:音源ミュート / P:扉の定点 / 1-4:成分ソロ", style);
            GUILayout.Label($"音: {(useAltClip && altClip != null ? $"差し替え ({altClip.name})" : "各音源のクリップ")}"
                            + (_muted ? "  ミュート中 (M)" : ""), style);
            GUILayout.Label($"音源配置: {(_stacked ? "重ね(1点)" : "展開")}", style);
            GUILayout.Label($"成分ソロ: 1直接 {(IrConvolver.Solo.PassDirect ? "ON " : "OFF")}"
                            + $" / 2反射 {(IrConvolver.Solo.PassReflect ? "ON " : "OFF")}"
                            + $" / 3回折 {(IrConvolver.Solo.PassDiffract ? "ON " : "OFF")}"
                            + $" / 4尾 {(IrConvolver.Solo.PassTail ? "ON " : "OFF")}"
                            + $"   (F=早期反射 {(enableEarlyReflections ? "ON" : "OFF")}・模型 {(earlyReflectModel == 1 ? "面の線" : "像源レイ")})", style);
            {
                // 扉の定点の状態。効いていない理由があればそれも出す（黙って効かないのがいちばん困る）。
                float peak = 0f; int live = 0;
                foreach (var kv in _portalEmitters)
                {
                    if (kv.Value == null) continue;
                    if (kv.Value.feedGain > 0f) live++;
                    if (kv.Value.rmsOut > peak) peak = kv.Value.rmsOut;
                }
                string why = !portalTail ? "" :
                             !sharedTailBus ? "（sharedTailBus が OFF なので効いていません）" :
                             !autoPortals ? "（autoPortals が OFF なので効いていません）" : "";
                GUILayout.Label($"扉の定点(P): {(portalTail ? "ON" : "OFF")}{why}   定点 {_portalEmitters.Count} 本"
                                + $"（鳴っている {live} 本、最大 {(peak > 1e-6f ? 20f * Mathf.Log10(peak) : -99f):F1} dB）"
                                + $"  レベル {portalTailLevel:F2}", style);
            }
            if (enableReverb)
            {
                float wetP = _reverbWet * 100f;                 // 物理wet%（t/(1+t)）
                GUILayout.Label($"直接:反響(物理) = {100f - wetP:F0}:{wetP:F0}   RT {_reverbDecay:F2}s", style);
                GUILayout.Label($"部屋の材質(L): {occluderMaterial}" + (_roomRt60 != null
                    ? $"   Sabine RT60 125/500/4k = {_roomRt60[0]:F2}/{_roomRt60[2]:F2}/{_roomRt60[5]:F2} s" : ""), style);
                // 部屋（幾何から自動検出）。部屋番号は表示だけで、音は割合で混ぜている。
                if (_scene != null)
                {
                    int nr = _scene.RoomCount;
                    if (nr > 0)
                    {
                        if (_roomIdsUi == null) { _roomIdsUi = new int[4]; _roomWUi = new float[4]; }
                        int n = _scene.GetRoomWeights(listener.position, roomBlendRadius,
                                                      _roomIdsUi, _roomWUi);
                        string mix = "";
                        for (int i = 0; i < n && i < 3; i++)
                            mix += $"{(i > 0 ? " + " : "")}部屋{_roomIdsUi[i]} {_roomWUi[i] * 100f:F0}%";
                        if (n == 0) mix = "部屋の外";
                        GUILayout.Label(
                            $"部屋: {nr}個 / 開口 {_scene.ApertureCount}箇所   居場所 = {mix}   " +
                            $"実効 V {Status.RoomVolume:F0}m3  RT60 {Status.RoomRt60:F2}s", style);
                    }
                }
            }

            if (_sources != null && _occSmoothed != null)
            {
                string occ = "遮蔽/音源: ";
                for (int i = 0; i < _sources.Length; i++)
                    occ += $"{SourceName(i)}:{_occSmoothed[i]:F2}  ";
                GUILayout.Label(occ, style);
            }
            GUILayout.Label(_diffDelta >= 0f
                ? $"主音源の回折: 迂回路あり δ={_diffDelta:F2} m（青=経路補完）"
                : "主音源の回折: なし", style);
            if (showDiffractionCandidates)
                GUILayout.Label($"回折候補(主音源): {_diffCandCount} 本合成 (水色=最短) (C)", style);
            GUILayout.Label($"直接の半影: {(directPenumbraMode == 2 ? "窓の走査線" : directPenumbraMode == 1 ? "環の標本点" : "旧(8 点)")} (Inspector directPenumbraMode)   "
                            + $"タップの組み立て: {(tapsFromEngine && _scene != null && _scene.TapBuilderAvailable ? "DLL" : "ホスト")} (tapsFromEngine)", style);
            GUILayout.Label($"早期反射(IR の R タップ・{(earlyReflectModel == 1 ? "面の線" : "像源レイ")}): {(enableEarlyReflections ? "ON" : "OFF")} (F)   "
                            + $"回折二次音源(IR の F タップ): {(enableDiffractionSources ? "ON" : "OFF")} (V)", style);
            {
                var tb = _portalHost != null ? _portalHost : FindFirstObjectByType<TailBusRenderer>();
                if (tb != null)
                    GUILayout.Label($"方向バス: {(tb.enableDirectionBus && tb.DirectionBusHandle != System.IntPtr.Zero ? $"{tb.directionLanes} 方向 × 2 耳（RMS {20f * Mathf.Log10(Mathf.Max(tb.DirectionBusRms, 1e-6f)):F1} dB）" : "OFF（タップごとの両耳化）")}", style);
            }
            if (_sources != null && _scene != null && _scene.IsValid)
            {
                // 段の内訳。探り（保持を 1 本だけ簡易で解き直す）は簡易に数えるので、簡易は上限 + 1 まで出る。
                int ne = 0, ns = 0, nv = 0, nh = 0;
                for (int i = 0; i < _sources.Length; i++)
                {
                    switch (_scene.GetSourceTierEffective(_scene.SourceIndex(SourceId(i))))
                    {
                        case 0: ne++; break;
                        case 1: ns++; break;
                        case 2: nv++; break;
                        case 3: nh++; break;
                    }
                }
                string capE = tierBudgetExact > 0 ? tierBudgetExact.ToString() : "∞";
                string capS = tierBudgetSimple > 0 ? tierBudgetSimple.ToString() : "∞";
                GUILayout.Label($"段の予算: 厳密 {ne}/{capE}・簡易 {ns}/{capS}（探り込み）・保持 {nh}・仮想 {nv}", style);
            }
            if (_scene != null && _scene.IsValid)
            {
                // 更新の非同期化。主スレッドの数字は Update ＋ 結果の読み出し（_acousticMs）、ワーカーの数字は解いた時間。
                _scene.GetUpdateStats(out float cms, out int lag, out int skipped, out int queued);
                GUILayout.Label(_scene.IsAsync
                    ? $"更新: 非同期  ワーカー {cms:F2} ms／答えの古さ {lag} フレーム／追いつかず {skipped}／待ち行列 {queued}   主スレッド {_acousticMs:F2} ms"
                    : $"更新: 同期   主スレッド {_acousticMs:F2} ms", style);
            }
            GUILayout.Label($"焼き(面の見通し): {(bakeStaticFaces ? $"面 {_bakeFaces} / セル {_bakeCells} / {_bakeMs:F0} ms ／ 動く物 {_bakeDynamic}" : "OFF（生で解く）")}", style);
            // 扉の開き角。音を判断する前提なので数字でも出す（見た目だけだと追えない）。
            if (_swingDoors == null)
                _swingDoors = FindObjectsByType<SwingDoor>(
                    FindObjectsInactive.Include, FindObjectsSortMode.None);
            for (int i = 0; i < _swingDoors.Length; i++)
                if (_swingDoors[i] != null)
                    GUILayout.Label($"扉 {_swingDoors[i].name}: {_swingDoors[i].angleDeg:F1}° (5/6キー)",
                                    style);

            // ポータルの開き具合。主音源に対して測って出す（見て分かるように）。
            if (_portals.Count > 0 && _scene != null && _scene.IsValid)
            {
                var p0 = _portals[0];
                if (p0 != null && p0.engineId >= 0
                    && _scene.MeasurePortal(p0.engineId, listener.position, _srcPos[0],
                                            _portalFrac, out Vector3 pc))
                {
                    GUILayout.Label(
                        $"ポータル {_portals.Count}個  開口率 125Hz {_portalFrac[0]:F3} / "
                        + $"1k {_portalFrac[3]:F3} / 4k {_portalFrac[5]:F3}", style);
                    GUILayout.Label($"　開口の重心: ({pc.x:F2}, {pc.y:F2}, {pc.z:F2}) ＝ここへ定位する",
                                    style);
                }
            }
            GUILayout.Label(useBtmDiffraction
                ? $"回折の出し方: BTM(検証途上)  開口コントラスト {apertureContrast:F1}"
                : $"回折の出し方: 前川＋開口積分  開口コントラスト {apertureContrast:F1}", style);
            // どちらの DSP で鳴っているか。扉の連続性は C++ 側にしか入っていないので必ず出す。
            GUILayout.Label(useCppDsp
                ? "DSP 経路: C++ (VoiceConvolver) ─ タップ補間あり (Y)"
                : "DSP 経路: C# (IrConvolver) ─ タップ補間なし (Y)", style);

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
