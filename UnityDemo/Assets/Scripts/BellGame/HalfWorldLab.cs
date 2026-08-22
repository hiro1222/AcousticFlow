// HalfWorldLab.cs
// 「扉の奥半分を、世界ごと丸ごと入れ替えられるか」だけを見る操作台。検証シーン専用。
//
// ★これは**構造チェック**であって、遊びの検証ではない。
//   聞きたいのは「気持ちいいか」ではなく「成立するか」── つまり、
//
//     ・奥半分に**実体のある世界**を置いて、まるごと差し替えられるか
//     ・差し替えると、そこに**住んでいる音源**も一緒に入れ替わるか
//     ・繋がっていない状態では、奥に**何も無い**（消音ではなく不在）
//
// ★輪は 3 つだけ。A世界（赤）→ B世界（緑）→ なし。
//   5 つの世界のローテーションは**この場では外してある**（発注者の指示）。
//   変数が減るほど、変化の原因が言い切れる。
//
// ★①「繋がっていない扉はただの扉」を、**音量ではなく不在**で作る。
//   なし＝両方の半分が眠っている＝エンジンに音源が 1 本も登録されていない。
//   絞って黙らせているのではなく、鳴らす相手が居ない。ここが本番と同じ形。
//
// ★A世界は左右に別の環境音を置いてある（鳥＝右／川＝左）。
//   扉が閉じている間、音は蝶番側からは来ない ── 回り込むのは板の**自由端**で、
//   この扉は右吊りなので隙間は**左**。だから閉扉時は
//
//       川（左＝隙間の側） … そのまま左に居るはず
//       鳥（右＝蝶番の側） … 左へ引かれるはず
//
//   2 本同時に鳴らしてあるので、片方だけが引かれるならその場で分かる。
//   これは不具合ではなく、隙間が其処にあるという事実がそのまま出ている。
//
// ★奥半分の入れ替えは、エンジンのシーンを組み直す（掛かりは実測してログに出す）。
//   AcousticFlowSceneDemo.OnEnable は音源だけでなく遮蔽物とポータルも集め直すので、
//   世界を替えるなら省けない。省くと**前の世界の壁を持ったまま**になる。
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class HalfWorldLab : MonoBehaviour
    {
        public enum Half { None = 0, A = 1, B = 2 }

        // ★音源 1 本ぶんの指定。**環境音の生成もここが持つ。**
        //
        //   もとは AmbientSource（部品）に任せていたが、二重に壊れた:
        //     1. AmbientSource.Awake と IrConvolver.Awake は**同じ実行順＝順序が未定義**。
        //        IrConvolver は「クリップが空なら ir_silence を入れる」（IrConvolver.cs:500）ので、
        //        先に走られた音源は一生 ir_silence を鳴らす。
        //     2. その AmbientSource 自体がシーン上で参照を失っていた
        //        （"The referenced script (Unknown) ... is missing!"）。部品が消えると
        //        Awake も走らず、GetComponent も null ── 修理コードごと素通りする。
        //
        //   同じ音を 2 人が持つと、どちらが黙ったのか分からない（決めごと #1 と同じ形）。
        //   **持ち主を 1 人にする。**HalfWorldLab が世界を起こすときに自分で入れる。
        [System.Serializable]
        public sealed class LabSource
        {
            public Transform t;
            [Tooltip("ON: 合成の環境音を鳴らす。OFF: AudioSource のクリップをそのまま鳴らす。")]
            public bool ambient;
            public AmbientKind kind = AmbientKind.Stream;
            public int seed = 1;
            [Range(0f, 1f)] public float volume = 1f;
            [Tooltip("単発物（鳥・水滴）の間隔。")]
            public float minInterval = 1.0f, maxInterval = 3.0f;

            [System.NonSerialized] public AudioClip made;   // 生成した環境音
            [System.NonSerialized] public float next;       // 次に単発を鳴らす時刻

            public bool IsLoop => kind == AmbientKind.Wind || kind == AmbientKind.Stream
                               || kind == AmbientKind.Falls;
        }

        [Header("配線")]
        public AcousticFlowSceneDemo demo;
        public WorldDoor worldDoor;
        public Transform listener;

        [Header("奥半分（同じ場所に重ねてある。生きるのは常に片方だけ）")]
        public GameObject halfA;
        public GameObject halfB;
        [Tooltip("A世界の音源。鳥＝向かって右、川＝向かって左。")]
        public LabSource[] worldA;
        [Tooltip("B世界の音源。")]
        public LabSource[] worldB;
        [Tooltip("デモが index 0 を要求するので置いてある、鳴らさない音源。")]
        public Transform src0;

        [Header("操作")]
        public KeyCode cycleKey = KeyCode.Tab;

        [Header("診断（読み取り専用）")]
        public Half live = Half.None;
        [Tooltip("直近の入れ替えで、同期部分に掛かった時間(ms)。")]
        public float lastSwapMs;
        [Tooltip("扉の吊り。指定は 蝶番=右 / 隙間=左。")]
        public string doorHanding = "—";

        private static readonly Transform[] Empty = new Transform[0];
        private string _lines = "—";

        // ★数字は**別ウィンドウ**（BellGame/モニター）へ出す。
        //   Game ビューに敷き詰めると、確かめたい絵そのものが文字で隠れる。
        //   実際、扉ごしの見え方を見ている最中に扉が文字の下だった。
        public static string Report = "";
        public static bool DumpRequested;      // モニターのボタンから叩かれる

        // ★くぐった後は「閉まって、接続も切れる」（発注者の決定）。
        //
        //   扉の面をまたいだ瞬間に、扉を閉じ、行き先を消す。
        //   聞こえ方はここで**役目が入れ替わる**のが要点:
        //       くぐる前 … 奥の音が、閉じた扉ごしにくぐもって漏れてくる
        //       くぐった後 … その音が**自分のまわりの音**になる（遮る物が無い）
        //   同じ音源が、経路だけで別物に聞こえる。演出は何も足していない。
        //
        // ⚠ この場では**くぐった先からの再接続は作っていません。**
        //   奥から扉を繋ぎ直すには、世界のほうを動かして扉に合わせる必要があり、
        //   それは WorldSet の仕事（本番はそちらが持っている）。
        //   ここで確かめたいのは「跨いだときに音が正しく引き継がれるか」と
        //   「そのとき何 ms 持っていかれるか」の 2 つなので、そこまでで足りる。
        [Header("転移")]
        [Tooltip("+1 = 手前側（扉の +Z 側） / -1 = 奥側。跨ぐと反転する。")]
        public int playerSide = 1;
        [Tooltip("1 フレームでこれ以上飛んだら、跨いだのではなく瞬間移動とみなす(m)。")]
        [Range(0.2f, 5f)] public float maxStepPerFrame = 1.5f;
        [Tooltip("直近の転移で、組み直しに掛かった時間(ms)。")]
        public float lastCrossMs;

        // ★本番と同じ機構にする（歩いて跨ぐ方式から変更）。
        //
        //   本番の扉は「繋がっていると歩いて跨げない・E でくぐる」です（DoorThreshold）。
        //   ここで測ったものを本番へ持っていくのが目的なので、**機構を揃えないと意味が無い。**
        //   繋いだら worldDoor.destination を立てる → DoorThreshold のカプセルが塞ぐ →
        //   E でくぐる、という本番と同じ流れにしました。
        //
        //   繋がっていない扉は素通りできます（＝ただの扉）。そこも本番と同じ。
        // ★自由な渡り方。**既定は OFF ＝ いまの E の仕組みのまま。**
        //
        //   置き換えではなく**増やしただけ**です（発注者の指示）。
        //   OFF に戻せば、敷居が塞いで E でしかくぐれない、いまの挙動へ完全に戻ります。
        //
        //   止めていた理由は「跨ぐと 25ms のフレームが出る」でしたが、
        //   世界を動かさない方式に変えてから**転移は 1ms 前後**になりました。
        //   前提が変わったので、試せる状態にしてあります。
        //
        // ⚠ ON にすると本番の機構（DoorThreshold で塞いで E）と挙動が変わります。
        //   本番へ持っていく判断は、聴いて・見て決めてください。
        [Tooltip("ON: 歩いて跨ぐだけで転移する。OFF（既定）: 敷居が塞いで E でくぐる。")]
        public bool freeCross;
        [Tooltip("くぐるキー。本番と同じ E。")]
        public KeyCode crossKey = KeyCode.E;
        [Tooltip("扉からこの距離まで寄るとくぐれる(m)。")]
        [Range(0.5f, 5f)] public float crossReach = 2.0f;
        // ★くぐる動きは **WorldTransition.EnterRoutine と同じ形**にしてある。
        //   別の動きを書くと「くぐるとはどう見えることか」に答えが二つできる（決めごと #1）。
        //   数値も既定を合わせてある（1.3 秒 / 2.0m）。
        [Tooltip("くぐり切るまでの秒数。WorldTransition と同じ既定。")]
        [Range(0.4f, 3f)] public float enterSeconds = 1.3f;
        [Tooltip("くぐった後、扉の面からどれだけ先に立つか(m)。")]
        [Range(0.5f, 4f)] public float enterDepth = 2.0f;

        // A/B を扉の「行き先」に割り当てる。DoorThreshold は None かどうかしか見ないので、
        //   どの WorldId でもよい。表示名が出るように既存の 2 つを借りている。
        private static WorldId IdOf(Half h)
            => (h == Half.A) ? WorldId.Grassland : (h == Half.B) ? WorldId.Temple : WorldId.None;

        [Header("画面表示")]
        [Tooltip("Game 画面に出す量。既定は一行だけ。全部はモニターウィンドウで見る。")]
        public bool verboseOnScreen;

        private void Start()
        {
            if (demo == null) demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (worldDoor == null) worldDoor = FindFirstObjectByType<WorldDoor>();
            if (listener == null && demo != null) listener = demo.listener;

            CheckHanding();

            // ★「なし」から始める。無いことを先に聞かせないと、有ることの意味が決まらない。
            live = Half.None;
            Apply(Half.None, quiet: true);
        }

        // ★吊り元は「絶対固定」と決まった（発注者の指示）。
        //   決まりごとは**検査**にしておく ── 看板だけ書いて黙って動かれるのが一番困る。
        //   PhysicsDoor は板のローカル -X を蝶番にするので、そこが右に来ていれば正しい。
        private void CheckHanding()
        {
            var pd = (worldDoor != null) ? worldDoor.door : null;
            var box = (pd != null) ? pd.GetComponent<BoxCollider>() : null;
            if (box == null || listener == null) { doorHanding = "扉が読めない"; return; }

            var hinge = pd.transform.TransformPoint(new Vector3(-box.size.x * 0.5f, 0f, 0f));
            var free = pd.transform.TransformPoint(new Vector3(box.size.x * 0.5f, 0f, 0f));

            // ★「耳から見てどちら側か」で判定してはいけない。
            //   蝶番と自由端は 0.55m しか離れていないのに 4m 先から見るので、
            //   横成分は 0.14 しか出ず、どんな閾値でも「正面」に丸まる。
            //   （最初そう書いて「吊りが指定と違う」と誤報した。式のほうが間違っていた）
            //   見るべきは**蝶番から自由端へ向かう向き**。距離に依らない。
            float lateral = Vector3.Dot((free - hinge).normalized, listener.right);
            bool rightHung = lateral < 0f;      // 自由端が左 ＝ 蝶番が右
            doorHanding = rightHung ? "蝶番=右 / 隙間=左" : "蝶番=左 / 隙間=右";

            if (rightHung)
                Debug.Log($"[BellGame] 扉の吊り: {doorHanding} ── 指定どおり（右吊り・左開口）"
                          + $"  横成分 {lateral:F2}");
            else
                Debug.LogWarning($"[BellGame] ★扉の吊りが指定と違う: {doorHanding}"
                                 + $"（指定は 蝶番=右 / 隙間=左）横成分 {lateral:F2}。"
                                 + "左右の聞こえ方の話が全部ずれる。", this);
        }

        private void Update()
        {
            if (Input.GetKeyDown(cycleKey))
            {
                // ★奥に居るあいだは繋ぎ替えさせない。
                //   奥から扉を繋ぎ直すには世界のほうを動かす必要があり（WorldSet の仕事）、
                //   ここには無い。**無い物を有るように見せない。**
                if (playerSide < 0)
                    Debug.LogWarning("[BellGame] 奥側に居るので繋ぎ替えられません。"
                        + "戻ってから Tab を押してください（奥からの再接続は WorldSet の担当で、"
                        + "この検証シーンには入れていません）", this);
                else Apply((Half)(((int)live + 1) % 3), quiet: false);
            }

            // ★「逆側に定位がある」を、耳だけで追わずに**経路を切って**特定する。
            //   閉じた扉ごしに鳴っているのは 4 つの経路の重ね合わせで、
            //   どれが逆側の像を作っているかは合計を聴いても分からない。
            if (Input.GetKeyDown(KeyCode.F6)) SoloToggle('D');
            if (Input.GetKeyDown(KeyCode.F7)) SoloToggle('R');
            if (Input.GetKeyDown(KeyCode.F8)) SoloToggle('F');
            if (Input.GetKeyDown(KeyCode.F9)) SoloToggle('T');
            if (Input.GetKeyDown(KeyCode.F10)) { IrConvolver.Solo.Reset(); Debug.Log("[BellGame] 経路: 全部戻した"); }

            // ★透過と回折の取り分を**耳で決められる**ようにする。
            //   閉じた扉の音は 2 人で作られている:
            //     透過（板を抜ける）… 向きは**音源のほう**。役目は「壁の向こう感」
            //     回折（隙間を回る）… 向きは**隙間のほう**。役目は「どこから聞こえるか」
            //   どちらを厚くするかは物理では決まらない（決めごと #3）。聴いて決める場所。
            if (demo != null)
            {
                if (Input.GetKeyDown(KeyCode.LeftBracket)) NudgeTrans(-3f);
                if (Input.GetKeyDown(KeyCode.RightBracket)) NudgeTrans(+3f);
                if (Input.GetKeyDown(KeyCode.Comma)) NudgeDiff(-3f);
                if (Input.GetKeyDown(KeyCode.Period)) NudgeDiff(+3f);

                // ★こもり。**壁の奥で鳴っている感じ**はここが作る。
                //   音量ではなく音色なので、落差を縮めても「隔てられている」が消えない。
                if (Input.GetKeyDown(KeyCode.F11)) NudgeMuffle(-3f);
                if (Input.GetKeyDown(KeyCode.F12)) NudgeMuffle(+3f);
            }

            // ★【決定的な一手】回折タップを HRTF に載せるのをやめてみる。
            //
            //   これが「開けるまで定位が逆」の本命です。遮蔽されている間、
            //   エンジンは**いちばん強い回折タップ**を HRTF に載せます（B1）。
            //   その向きは音源ではなく**回り込んだ場所**なので、定位が扉に引きずられる。
            //   切れば、定位を運ぶのは音源を向いている直接（透過）タップだけになる。
            //
            //   ・切って直れば → 逆さの出所は回折→HRTF の経路
            //   ・切っても逆なら → もっと奥（C++ の HRTF そのもの）
            //   どちらでも一歩前に進む。1 ビットで済む実験は先にやる。
            // ★定常音の種類を差し替える。**聴き比べを作り直しなしでやる。**
            //   どれが「壁の奥で鳴っている同じ音」に聞こえるかは、耳でしか決まらない
            //   （決めごと #3）。滝＝全帯域 / 川＝高域だけ / 風＝低域だけ、と性格が違う。
            if (Input.GetKeyDown(KeyCode.F4)) CycleLoopKind();

            // ★定位のステア。OFF＝常に音源の方位（＝反転の回避策であり、指定の実現）。
            //   ON に戻すと反転が再現するので、サウンドレーンに見せるときはこれで。
            if (demo != null && Input.GetKeyDown(KeyCode.F3))
            {
                demo.useDirectionalSteering = !demo.useDirectionalSteering;
                Debug.Log($"[BellGame] 定位のステア = {(demo.useDirectionalSteering ? "ON（遮蔽時は見かけの方向＝いまは反転する）" : "OFF（常に音源の方位）")}", this);
            }

            if (demo != null && Input.GetKeyDown(KeyCode.F2))
            {
                demo.diffractionHrtf = !demo.diffractionHrtf;
                Debug.Log($"[BellGame] 回折をHRTFに載せる = {(demo.diffractionHrtf ? "ON" : "OFF")}"
                          + " ── OFF なら定位を運ぶのは音源を向いた透過タップだけ", this);
            }

            // ★いまの状態をコンソールへ吐く。
            //   入れ替えた直後のログでは Status.Taps がまだ空で「まだ読めない」しか出ない。
            //   数字が要るのは**落ち着いた後**なので、好きなときに押せる鍵にしておく。
            if (Input.GetKeyDown(KeyCode.F1)) { Peek(); Dump("F1"); }

            // 入れ替えの 0.5 秒後にも一度だけ吐く（押し忘れても数字が残るように）。
            if (_dumpAt > 0f && Time.time >= _dumpAt)
            { _dumpAt = 0f; Peek(); Dump("入れ替えの 0.5 秒後"); }

            TickOneShots();
            // ★E でくぐる（本番と同じ）。繋がった扉は DoorThreshold が塞いでいるので、
            //   歩いては抜けられない。通れない理由は黙らずに言う。
            if (Input.GetKeyDown(crossKey))
            {
                if (CanCross(out string why)) Cross();
                else Debug.Log($"[BellGame] くぐれません: {why}", this);
            }

            if (Input.GetKeyDown(KeyCode.Backslash)) verboseOnScreen = !verboseOnScreen;
            if (DumpRequested) { DumpRequested = false; Peek(); Dump("モニターのボタン"); }

            if (Time.frameCount % 15 == 0) { Peek(); BuildReport(); }
        }

        // ★「なんか小さい」「なんか逆」を数字にするための覗き窓。
        //   音量そのものはエンジンの中なので、ここでは**幾何のほうだけ**出す ──
        //   どれだけ離れていて、どちら側で、あいだに何が挟まっているか。
        private void Peek()
        {
            var srcs = LiveSources();
            if (srcs.Length == 0) { _lines = "    （奥に音源は無い）"; return; }

            var basis = (Camera.main != null) ? Camera.main.transform
                      : (listener != null ? listener : transform);
            Vector3 ear = basis.position;

            var sb = new System.Text.StringBuilder();
            foreach (var s in srcs)
            {
                if (s == null) continue;
                string blocked = Physics.Linecast(ear, s.position, out var hit, ~0,
                                                  QueryTriggerInteraction.Ignore)
                               ? hit.collider.name : "直線が通っている";
                sb.AppendLine($"    {s.name,-12} 幾何={SideOf(basis, ear, s.position)}  "
                              + $"{Vector3.Distance(ear, s.position):F1}m  {Level(s)}  ／ {blocked}");
                sb.AppendLine("      " + EngineDir(System.Array.IndexOf(srcs, s) + 1));
            }
            AudioListener.GetOutputData(_buf, 0);
            sb.AppendLine($"    耳に届いている合計 = {Rms(_buf)}");

            // ★物差しを疑う。
            //   エンジンは `listener.InverseTransformDirection`（＝listener.right）で
            //   左右を決めている。こちらの表示は Camera.main.right で決めている。
            //   この 2 つが向かい合っていたら、**エンジンは正しくて表示が逆**になる。
            //   「エンジンが反転している」と言う前に、ここを潰しておく。
            if (listener != null && Camera.main != null)
            {
                var lr = listener.right; var cr = Camera.main.transform.right;
                float agree = Vector3.Dot(lr, cr);
                sb.AppendLine($"    物差し: listener.right=({lr.x:+0.00;-0.00},{lr.z:+0.00;-0.00}) "
                    + $"camera.right=({cr.x:+0.00;-0.00},{cr.z:+0.00;-0.00}) "
                    + $"内積={agree:+0.00;-0.00} {(agree > 0.9f ? "一致" : agree < -0.9f ? "★正反対" : "★ずれている")}");

                foreach (var s in srcs)
                {
                    if (s == null) continue;
                    var d = (s.position - ear); d.y = 0f; d.Normalize();
                    sb.AppendLine($"      {s.name,-12} listener基準 {Vector3.Dot(d, lr):+0.00;-0.00}"
                        + $"   camera基準 {Vector3.Dot(d, cr):+0.00;-0.00}"
                        + "   ← エンジンの値と同じ符号になっているのはどちらか");
                }
            }
            _lines = sb.ToString().TrimEnd();
        }

        // ★エンジン自身が持っている到来方向を読む。
        //
        //   幾何（こちらの計算）とエンジンの向きを**並べて出す**のが要点。
        //     どちらも同じで、耳だけが逆   → 反転しているのは C++ の HRTF より奥
        //     エンジンの向きが既に逆       → 反転しているのはタップの選び方／到来点
        //   耳の印象だけで追うと、この 2 つは一生区別が付かない。
        //
        //   DirLocal はリスナー座標系（+X = right）なので、x>0 なら右。
        private static string EngineDir(int hostIndex)
        {
            var taps = AcousticFlowSceneDemo.Status.Taps;
            if (taps == null) return "エンジン: Taps がまだ空（入れ直した直後は毎回こう出る）";
            if (hostIndex < 0 || hostIndex >= taps.Length || taps[hostIndex] == null)
                return $"エンジン: index {hostIndex} が範囲外（Taps は {taps.Length} 本）";

            var ts = taps[hostIndex];
            string s = $"エンジン: 直接={Side(ts.DirectDirLocal.x)}({ts.DirectDirLocal.x:+0.00;-0.00})";

            if (ts.HrtfTapIndex >= 0)
            {
                s += $"  HRTFに載せた回折#{ts.HrtfTapIndex}="
                   + $"{Side(ts.HrtfTapDirLocal.x)}({ts.HrtfTapDirLocal.x:+0.00;-0.00})";
                if (ts.PanL != null && ts.PanR != null && ts.HrtfTapIndex < ts.PanL.Length)
                    s += $"  pan L{ts.PanL[ts.HrtfTapIndex]:F2}/R{ts.PanR[ts.HrtfTapIndex]:F2}";
            }
            else s += "  回折はHRTFに載せていない";

            return s;
        }

        private static string Side(float x) => (x > 0.05f) ? "右" : (x < -0.05f) ? "左" : "中";

        private readonly float[] _buf = new float[256];

        // ★「何も聞こえない」を 3 つに割る。
        //     止まっている        … AudioSource が鳴っていない（世界を起こし損ねた）
        //     鳴っているが -∞     … 鳴ってはいるが、エンジンが落としきっている
        //     -50dB など数字が出る … 鳴っていて通っている。小さいだけ
        //   どれなのかで直す場所が全く違うので、耳で当てにいかない。
        private string Level(Transform s)
        {
            var a = s.GetComponent<AudioSource>();
            if (a == null) return "[AudioSource 無し]";
            if (a.clip == null) return "[クリップ無し]";
            if (!a.isPlaying) return "[★止まっている]";
            a.GetOutputData(_buf, 0);
            return "[" + Rms(_buf) + " " + a.clip.name + "]";
        }

        private static string Rms(float[] b)
        {
            float sum = 0f;
            for (int i = 0; i < b.Length; i++) sum += b[i] * b[i];
            float r = Mathf.Sqrt(sum / b.Length);
            return (r > 1e-7f) ? $"{20f * Mathf.Log10(r):F0}dB" : "★無音";
        }

        /// 耳から見て左右どちら側か。エンジンのパンと同じ式（right 軸への投影）で出す。
        private static string SideOf(Transform basis, Vector3 ear, Vector3 p)
        {
            var d = p - ear; d.y = 0f;
            if (d.sqrMagnitude < 1e-6f) return "正面";
            float x = Vector3.Dot(d.normalized, basis.right);
            return (x > 0.15f) ? "右  " : (x < -0.15f) ? "左  " : "正面";
        }

        private static readonly LabSource[] NoSpecs = new LabSource[0];

        public LabSource[] LiveSpecs()
            => (live == Half.A) ? (worldA ?? NoSpecs)
             : (live == Half.B) ? (worldB ?? NoSpecs) : NoSpecs;

        private Transform[] _liveArr = Empty;

        private Transform[] LiveSources() => _liveArr;

        // ★世界を起こしたら、音は**こちらが**入れる。順序にも部品の生死にも賭けない。
        private void EnsureAudio()
        {
            int sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;
            foreach (var s in LiveSpecs())
            {
                if (s == null || s.t == null) continue;
                var a = s.t.GetComponent<AudioSource>();
                if (a == null) continue;

                if (!s.ambient)
                {
                    if (a.clip != null && !a.isPlaying) { a.loop = true; a.Play(); }
                    continue;
                }

                if (s.made == null) s.made = AmbientVoices.Make(s.kind, s.seed, sr);

                if (s.IsLoop)
                {
                    // 風・川は鳴りっぱなし。ir_silence を掴んでいたらここで取り返す。
                    if (a.clip != s.made) { a.Stop(); a.clip = s.made; }
                    a.loop = true;
                    a.volume = s.volume;
                    if (!a.isPlaying) a.Play();
                }
                else
                {
                    // 鳥・水滴は単発を重ねる。鳴り止むと畳み込みの尾まで切れるので、
                    //   土台には無音のループを回しておく（AmbientSource と同じ作法）。
                    if (a.clip == null || a.clip.length > 0.2f)
                    {
                        var sil = AudioClip.Create("LabSilence", sr / 10, 1, sr, false);
                        sil.SetData(new float[sr / 10], 0);
                        a.Stop(); a.clip = sil;
                    }
                    a.loop = true;
                    a.volume = 1f;
                    if (!a.isPlaying) a.Play();
                    s.next = Time.time + 0.4f;
                }
            }
        }

        // 定常音の輪。滝（全帯域）→ 川（高域寄り）→ 風（低域寄り）。
        private static readonly AmbientKind[] LoopRing =
            { AmbientKind.Falls, AmbientKind.Stream, AmbientKind.Wind };

        /// 音源 1 本の音を差し替える。
        ///
        /// ★順序を聴くときは、**両側を同じ音にする**のが筋です。
        ///   鳥（単発の囀り）と滝（定常）では「どちらが先に抜けてきたか」を比べられません
        ///  ── 比べるものが違うので、耳は「別の音がした」としか言えない。
        ///   扉の開き角シーンも左右で同じステムを使っています。
        public void SetKind(LabSource s, AmbientKind kind)
        {
            if (s == null || s.t == null) return;
            int sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;

            s.ambient = true;
            s.kind = kind;
            s.made = AmbientVoices.Make(kind, s.seed, sr);

            var a = s.t.GetComponent<AudioSource>();
            if (a == null) return;

            if (s.IsLoop)
            {
                a.Stop(); a.clip = s.made; a.loop = true; a.volume = s.volume; a.Play();
            }
            else
            {
                // 単発物は無音ループを土台にして重ねる（鳴り止むと畳み込みの尾まで切れる）。
                var sil = AudioClip.Create("LabSilence", sr / 10, 1, sr, false);
                sil.SetData(new float[sr / 10], 0);
                a.Stop(); a.clip = sil; a.loop = true; a.volume = 1f; a.Play();
                s.next = Time.time + 0.4f;
            }
            Debug.Log($"[BellGame] {s.t.name} の音 → {kind}", this);
        }

        /// 音源 1 本に**音声ファイル**を鳴らさせる（合成の環境音をやめる）。
        ///
        /// ★合成音より聴き分けやすいので、順序や音色を判断するときはこちら。
        ///   ⚠ ただし**曲は帯域が偏り、途中で切れます。**
        ///     「隙間が 5cm から 15cm になった差」が、曲の展開と紛れることがあります。
        ///     迷ったら滝（全帯域・定常）に戻して確かめてください。
        ///
        /// ⚠ 市販楽曲は配布物に入れられません（仕様書 §7.6・§9）。検証専用です。
        public void SetClip(LabSource s, AudioClip clip)
        {
            if (s == null || s.t == null || clip == null) return;
            s.ambient = false;
            s.made = null;

            var a = s.t.GetComponent<AudioSource>();
            if (a == null) return;
            a.Stop();
            a.clip = clip;
            a.loop = true;
            a.volume = s.volume;
            a.time = 0f;
            a.Play();
            Debug.Log($"[BellGame] {s.t.name} の音 → {clip.name}", this);
        }

        /// 生きている世界の音源を**全部同じ音声ファイル**にする。
        ///   ★位置の違いだけを残すため、頭出しも揃えます
        ///     （ずれていると「先に抜けてきた」のか「先に鳴った」のか区別できません）。
        public void SetAllClips(AudioClip clip)
        {
            foreach (var s in LiveSpecs()) SetClip(s, clip);

            // 揃えて鳴らし直す。1 本ずつ Play すると数 ms ずれます。
            double at = AudioSettings.dspTime + 0.05;
            foreach (var s in LiveSpecs())
            {
                if (s == null || s.t == null) continue;
                var a = s.t.GetComponent<AudioSource>();
                if (a == null) continue;
                a.Stop(); a.time = 0f; a.PlayScheduled(at);
            }
        }

        /// 生きている世界の音源を**全部同じ音**にする。順序を聴くときの下ごしらえ。
        public void SetAllKinds(AmbientKind kind)
        {
            foreach (var s in LiveSpecs()) SetKind(s, kind);
        }

        public void CycleLoopKind()
        {
            int sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;
            foreach (var s in LiveSpecs())
            {
                if (s == null || s.t == null || !s.ambient || !s.IsLoop) continue;
                int at = System.Array.IndexOf(LoopRing, s.kind);
                s.kind = LoopRing[(at + 1) % LoopRing.Length];
                s.made = AmbientVoices.Make(s.kind, s.seed, sr);   // 作り直す

                var a = s.t.GetComponent<AudioSource>();
                if (a == null) continue;
                a.Stop(); a.clip = s.made; a.loop = true; a.volume = s.volume; a.Play();
                Debug.Log($"[BellGame] {s.t.name} の定常音 → {s.kind}"
                          + (s.kind == AmbientKind.Falls ? "（6帯域すべてに芯がある）"
                           : s.kind == AmbientKind.Stream ? "（高域寄り。こもらせると消える）"
                           : "（低域寄り。こもらせても変わらない）"), this);
            }
        }

        // 単発物を鳴らす。生きている世界のぶんだけ。
        private void TickOneShots()
        {
            foreach (var s in LiveSpecs())
            {
                if (s == null || s.t == null || !s.ambient || s.IsLoop || s.made == null) continue;
                if (Time.time < s.next) continue;
                s.next = Time.time + Random.Range(s.minInterval, Mathf.Max(s.minInterval, s.maxInterval));
                var a = s.t.GetComponent<AudioSource>();
                if (a != null) a.PlayOneShot(s.made, s.volume);
            }
        }

        private GameObject LiveHalf()
            => (live == Half.A) ? halfA : (live == Half.B) ? halfB : null;

        public void Apply(Half next, bool quiet)
        {
            live = next;

            // 1) 奥半分を差し替える。**先に世界を建ててから**エンジンに集めさせる。
            if (halfA != null) halfA.SetActive(next == Half.A);
            if (halfB != null) halfB.SetActive(next == Half.B);

            // ★扉の「行き先」を立てる／降ろす。これで DoorThreshold が塞ぐ／通す。
            //   繋がった扉は歩いて跨げなくなり、E でくぐる ── 本番と同じ。
            if (worldDoor != null) worldDoor.destination = IdOf(next);

            var specs = LiveSpecs();
            var list = new System.Collections.Generic.List<Transform>(specs.Length);
            foreach (var s in specs) if (s != null && s.t != null) list.Add(s.t);
            _liveArr = list.ToArray();
            var srcs = _liveArr;

            // 2) 装置を繋ぎ替える。index 0 はあなた、1 番以降がその世界の音。
            if (demo != null)
            {
                demo.source = src0;
                demo.extraSources = srcs;
            }

            // 3) エンジンのシーンを組み直す。掛かりを測る（見立てではなく数字で）。
            float pitch = PitchNow();
            var sw = System.Diagnostics.Stopwatch.StartNew();
            if (demo != null) { demo.enabled = false; demo.enabled = true; }
            sw.Stop();
            lastSwapMs = (float)sw.Elapsed.TotalMilliseconds;
            RestorePitch(pitch);

            // 4) その世界の音を入れ直す。
            //    エンジンを入れ直した**後**にやる ── OnEnable の途中に触っても消される。
            //    クリップの持ち主はここだけなので、順序にも部品の生死にも賭けなくていい。
            EnsureAudio();

            Peek();
            _dumpAt = Time.time + 0.5f;     // 落ち着いた頃にもう一度、数字が入った状態で吐く
            if (!quiet) LogSwap(next, srcs);
        }

        private float _dumpAt;

        public void Dump(string why)
        {
            float ang = (worldDoor != null && worldDoor.door != null) ? worldDoor.door.AngleDeg : 0f;
            Debug.Log($"[BellGame] ── 状態（{why}）──\n"
                + $"  奥半分={live}  扉={ang:F0}°  吊り={doorHanding}  経路={PathState()}\n"
                + $"  透過{(demo != null ? demo.transmissionGainDb : 0f):F0}dB / "
                + $"回折{(demo != null ? demo.diffractionGainDb : 0f):F0}dB / "
                + $"こもり{(demo != null ? demo.transmissionHighCutDb : 0f):F0}dB / "
                + $"回折HRTF={(demo != null && demo.diffractionHrtf ? "ON" : "OFF")}\n"
                + _lines, this);
        }

        private void LogSwap(Half next, Transform[] srcs)
        {
            Debug.Log(next == Half.None
                ? $"[BellGame] 奥半分: なし ── 扉の奥に世界が**存在しない**。"
                  + $"音源はエンジンに 0 本（絞っているのではない）。組み直し {lastSwapMs:F1}ms"
                : $"[BellGame] 奥半分: {next}世界 ── 音源 {srcs.Length} 本、"
                  + $"組み直し {lastSwapMs:F1}ms\n{_lines}", this);
        }

        private void NudgeTrans(float d)
        {
            demo.transmissionGainDb = Mathf.Clamp(demo.transmissionGainDb + d, -24f, 24f);
            demo.enabled = false; demo.enabled = true;
            Debug.Log($"[BellGame] 透過 {demo.transmissionGainDb:F0}dB / 回折 {demo.diffractionGainDb:F0}dB"
                      + " ── 透過は音源の向き、回折は隙間の向き");
        }

        private void NudgeDiff(float d)
        {
            demo.diffractionGainDb = Mathf.Clamp(demo.diffractionGainDb + d, -24f, 24f);
            demo.enabled = false; demo.enabled = true;
            Debug.Log($"[BellGame] 透過 {demo.transmissionGainDb:F0}dB / 回折 {demo.diffractionGainDb:F0}dB"
                      + " ── 透過は音源の向き、回折は隙間の向き");
        }

        private void NudgeMuffle(float d)
        {
            demo.transmissionHighCutDb = Mathf.Clamp(demo.transmissionHighCutDb + d, 0f, 24f);
            demo.enabled = false; demo.enabled = true;
            Debug.Log($"[BellGame] 透過のこもり {demo.transmissionHighCutDb:F0}dB"
                      + $"（傾き {demo.transmissionTilt:F1}）── 壁の奥で鳴っている感じの担当");
        }

        // ★跨いだかどうかは **LateUpdate** で見る。
        //   移動を処理し終わった後の位置でないと、1 フレームぶん古い所を見ることになる。
        private float _prevZ;
        private bool _zPrimed;

        private void LateUpdate()
        {
            if (worldDoor == null) return;
            var cam = Camera.main;
            Vector3 eye = (cam != null) ? cam.transform.position
                        : (listener != null ? listener.position : transform.position);

            float z = worldDoor.transform.InverseTransformPoint(eye).z;
            if (!_zPrimed) { _prevZ = z; _zPrimed = true; return; }

            // ★敷居は freeCross のときだけ通す。**既定は塞いだまま＝いまの E の仕組み。**
            var th = worldDoor.GetComponent<DoorThreshold>();
            if (th != null) th.SetPassable(freeCross);

            // 符号が変わった＝面を跨いだ。飛んだ量が大きすぎるときは無視する
            //   （E でくぐったときの瞬間移動を「歩いて跨いだ」と読み違えないため）。
            if ((z < 0f) != (_prevZ < 0f) && Mathf.Abs(z - _prevZ) < maxStepPerFrame)
            {
                if (freeCross && live != Half.None && !_crossing)
                {
                    // 歩いて跨いだだけで転移する。運ぶ動きは要らない（もう向こうに居る）。
                    CrossFinish();
                }
                else
                {
                    // 繋がっていない扉は**ただの扉**。跨いでも側が変わるだけ。
                    playerSide = -playerSide;
                    Debug.Log($"[BellGame] 扉を歩いて抜けた（繋がっていない扉）→ "
                              + $"{(playerSide > 0 ? "手前側" : "奥側")}", this);
                    BuildReport();
                }
            }
            _prevZ = z;
        }

        /// E でくぐれるか。近くに居て、扉が繋がっていることが条件（本番と同じ）。
        public bool CanCross(out string why)
        {
            why = "";
            if (worldDoor == null) { why = "扉が無い"; return false; }
            if (live == Half.None) { why = "扉が繋がっていない（Tab で繋ぐ）"; return false; }
            var eye = (Camera.main != null) ? Camera.main.transform.position
                    : (listener != null ? listener.position : transform.position);
            float d = Vector3.Distance(eye, worldDoor.transform.position);
            if (d > crossReach) { why = $"扉から遠い（{d:F1}m / {crossReach:F1}m まで）"; return false; }

            // ★「何度以上開いていること」ではなく「**体が通れる幅が空いていること**」。
            //   角度で線を引くと、扉の幅を変えた瞬間に嘘になる（決めごと #2）。
            //   蝶番で開いた板の残り投影は W·cosθ なので、空いた幅は W(1-cosθ)。
            //   そこにプレイヤーの太さ（WorldBounds が持っている値）が入るかを見る。
            var pd = worldDoor.door;
            if (pd != null)
            {
                var box = pd.GetComponent<BoxCollider>();
                float w = (box != null) ? box.size.x : 1.1f;
                if (_bounds == null) _bounds = FindFirstObjectByType<WorldBounds>();
                float body = (_bounds != null) ? _bounds.bodyRadius * 2f : 0.7f;
                float gap = w * (1f - Mathf.Cos(pd.AngleDeg * Mathf.Deg2Rad));
                if (gap < body)
                { why = $"扉の隙間が体より狭い（{gap:F2}m / {body:F2}m 要る・いま {pd.AngleDeg:F0}°）"; return false; }
            }
            return true;
        }

        private WorldBounds _bounds;

        private bool _crossing;

        public void Cross()
        {
            if (!_crossing) StartCoroutine(EnterRoutine());
        }

        /// くぐる動き。**WorldTransition.EnterRoutine と同じ組み立て。**
        ///   ・動けなくする（通り道を保証できないため）
        ///   ・敷居を通す（押し戻しに邪魔をさせない）
        ///   ・開口の**中心**を通す（端をかすめると周辺が見えて入れ替わりが露見する）
        ///   ・**面を越えた瞬間**に入れ替える（そこでは開口が視界を占めていて継ぎ目が見えない）
        private System.Collections.IEnumerator EnterRoutine()
        {
            _crossing = true;
            var t = worldDoor.transform;
            var th = worldDoor.GetComponent<DoorThreshold>();
            if (th != null) th.SetPassable(true);

            bool hadMovement = demo != null && demo.enableMovement;
            if (demo != null) demo.enableMovement = false;

            // 進む向きは立っている側で決まる。扉は動かないので、次は逆向きにくぐる。
            int side = (t.InverseTransformPoint(listener.position).z > 0f) ? 1 : -1;
            Vector3 through = -side * t.forward;

            Vector3 from = listener.position;
            Quaternion fromRot = listener.rotation;
            Vector3 mid = t.position; mid.y = from.y;
            Vector3 to = mid + through * enterDepth;
            Quaternion toRot = Quaternion.LookRotation(through, Vector3.up);

            bool swapped = false;
            float k = 0f;
            while (k < 1f)
            {
                k += Time.deltaTime / Mathf.Max(0.05f, enterSeconds);
                float e = Mathf.SmoothStep(0f, 1f, Mathf.Clamp01(k));
                var p = Vector3.Lerp(from, to, e);
                // 向きは少し早めに向こうを向かせる（本番と同じ 1.6 倍）。
                listener.SetPositionAndRotation(p, Quaternion.Slerp(fromRot, toRot, Mathf.Clamp01(e * 1.6f)));

                if (!swapped && t.InverseTransformPoint(p).z * side <= 0f)
                { swapped = true; CrossFinish(); }

                yield return null;
            }

            // 面を越えないまま終わった場合の受け皿（フレーム落ち・極端に短い秒数）。
            if (!swapped) CrossFinish();

            listener.SetPositionAndRotation(to, toRot);
            _zPrimed = false;                    // この移動を「歩いて跨いだ」と読ませない
            if (th != null) th.SetPassable(false);
            if (demo != null) demo.enableMovement = hadMovement;
            _crossing = false;
        }

        private void CrossFinish()
        {
            var wasLive = live;
            playerSide = -playerSide;

            // ⚠ live は落としません。**切れるのは「扉の接続」であって、世界ではない。**
            //   いま自分がその世界の中に立っているので、音源も部屋も生きたままが正しい。
            //   接続が切れたことは destination=None（扉が塞がなくなる）と
            //   playerSide（奥側では Tab を止める）で表しています。
            //   ここで live を None にすると、立っている世界の音を自分で消すことになります。

            // 1) 閉まって、接続も切れる（発注者の決定）。
            if (worldDoor != null)
            {
                if (worldDoor.door != null) worldDoor.door.ForceAngle(0f);
                worldDoor.destination = WorldId.None;
            }

            // 2) 装置を組み直す。音源は変えない ── **同じ音源が、経路だけで別物になる**のを聴く。
            float pitch = PitchNow();
            var sw = System.Diagnostics.Stopwatch.StartNew();
            if (demo != null) { demo.enabled = false; demo.enabled = true; }
            sw.Stop();
            lastCrossMs = (float)sw.Elapsed.TotalMilliseconds;
            RestorePitch(pitch);
            EnsureAudio();

            Peek(); BuildReport();
            _dumpAt = Time.time + 0.5f;

            Debug.Log($"[BellGame] 扉を跨いだ → {(playerSide > 0 ? "手前側" : "奥側")}へ。"
                + $"扉は閉じ、行き先は消えた。組み直し {lastCrossMs:F1}ms\n"
                + $"  {wasLive}世界の音が、扉ごしの漏れから**まわりの音**に変わったか聴いてください。\n"
                + _lines, this);
        }

        private static void SoloToggle(char t)
        {
            switch (t)
            {
                case 'D': IrConvolver.Solo.PassDirect = !IrConvolver.Solo.PassDirect; break;
                case 'R': IrConvolver.Solo.PassReflect = !IrConvolver.Solo.PassReflect; break;
                case 'F': IrConvolver.Solo.PassDiffract = !IrConvolver.Solo.PassDiffract; break;
                default: IrConvolver.Solo.PassTail = !IrConvolver.Solo.PassTail; break;
            }
            Debug.Log("[BellGame] 経路: " + PathState());
        }

        private static string On(bool b) => b ? "○" : "×";

        private static string PathState()
            => $"直接{On(IrConvolver.Solo.PassDirect)} 反射{On(IrConvolver.Solo.PassReflect)} "
             + $"回折{On(IrConvolver.Solo.PassDiffract)} 尾{On(IrConvolver.Solo.PassTail)}";

        // ★視線の上下を守る。demo は初期化で _pitch を 0 にするので、入れ直すと水平に戻る。
        //   private なので控えて書き戻す。**その場しのぎ**（WorldSet と同じ理由・同じ形）。
        private static System.Reflection.FieldInfo _pitchField;

        private float PitchNow()
        {
            var cam = Camera.main;
            return (cam != null) ? cam.transform.eulerAngles.x : 0f;
        }

        private void RestorePitch(float pitch)
        {
            if (demo == null) return;
            if (_pitchField == null)
                _pitchField = typeof(AcousticFlowSceneDemo).GetField("_pitch",
                    System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
            if (_pitchField == null || _pitchField.FieldType != typeof(float)) return;

            float p = pitch > 180f ? pitch - 360f : pitch;
            _pitchField.SetValue(demo, Mathf.Clamp(p, -80f, 80f));
        }

        /// 別ウィンドウ（BellGame/モニター）に出す全文を作る。画面には出さない。
        private void BuildReport()
        {
            float ang = (worldDoor != null && worldDoor.door != null) ? worldDoor.door.AngleDeg : 0f;
            var sb = new System.Text.StringBuilder();
            sb.AppendLine($"奥半分 = {(live == Half.None ? "なし（ただの扉）" : live + "世界")}"
                          + $"   扉 {ang:F0}°／最大 {(worldDoor != null && worldDoor.door != null ? worldDoor.door.maxAngle : 0f):F0}°"
                          + $"   吊り {doorHanding}");
            sb.AppendLine($"居る側 {(playerSide > 0 ? "手前" : "奥（くぐった後）")}"
                          + $"   組み直し {lastSwapMs:F1} ms"
                          + $"   転移 {lastCrossMs:F1} ms"
                          + $"   DSP {(demo != null && demo.useCppDsp ? "C++" : "C#")}");
            sb.AppendLine();
            sb.AppendLine("音源:");
            sb.AppendLine(_lines);
            sb.AppendLine();
            sb.AppendLine($"透過 {(demo != null ? demo.transmissionGainDb : 0f):F0}dB   "
                          + $"回折 {(demo != null ? demo.diffractionGainDb : 0f):F0}dB   "
                          + $"こもり {(demo != null ? demo.transmissionHighCutDb : 0f):F0}dB   "
                          + $"経路 {PathState()}");
            sb.AppendLine($"ステア {(demo != null && demo.useDirectionalSteering ? "ON（いまは反転する）" : "OFF（常に音源の方位）")}"
                          + $"   回折HRTF {(demo != null && demo.diffractionHrtf ? "ON" : "OFF")}");
            sb.AppendLine();
            sb.AppendLine(CanCross(out string why) ? "★E でくぐれます"
                                                  : $"E でくぐる: {why}");
            sb.AppendLine("Tab 世界を一巡  5/6 扉  F1 コンソールへ  F2 回折HRTF  F3 ステア");
            sb.AppendLine("F4 定常音（滝→川→風）  F6..F9 経路を切る  F10 戻す");
            sb.AppendLine("[ ] 透過   , . 回折   F11/F12 こもり   \\ 画面表示を増減");
            sb.AppendLine();
            sb.AppendLine("狙い: 定位は扉で変えない。閉じていても透過が音源の向きを見せ、");
            sb.AppendLine("      開くにつれて入ってくる量と音色だけが変わる。");
            Report = sb.ToString();
        }

        private void OnGUI()
        {
            float ang = (worldDoor != null && worldDoor.door != null) ? worldDoor.door.AngleDeg : 0f;

            // ★既定は**一行だけ**。残りは別ウィンドウ（BellGame/モニター）。
            //   確かめたい絵を文字で隠さないため。\ で増減できる。
            if (!verboseOnScreen)
            {
                GUI.Label(new Rect(12, 12, 900, 20),
                    $"{(live == Half.None ? "なし" : live + "世界")}   扉 {ang:F0}°   "
                    + PerfMeter.OneLine() + "   （詳細は BellGame/モニター）");
                return;
            }

            var sb = new System.Text.StringBuilder();
            sb.AppendLine("── 構造チェック：奥半分を丸ごと入れ替える ──────────────");
            sb.AppendLine($"  いまの奥半分 = {(live == Half.None ? "なし（ただの扉）" : live + "世界")}"
                          + $"   扉の開き = {ang:F0}°   扉の吊り = {doorHanding}");
            sb.AppendLine($"  直近の組み直し = {lastSwapMs:F1} ms（同期部分。数フレーム後の山は別）"
                          + $"   DSP = {(demo != null && demo.useCppDsp ? "C++ (VoiceConvolver)" : "C# (IrConvolver)")}");
            sb.AppendLine("  音源:");
            sb.AppendLine(_lines);
            sb.AppendLine();
            sb.AppendLine("  Tab  なし → A世界（赤・鳥が右／川が左） → B世界（緑・楽曲が左）");
            sb.AppendLine("  5/6  扉を開く／閉じる（押している間）");
            sb.AppendLine($"  F6/F7/F8/F9 経路を切る（直接/反射/回折/尾）  F10 戻す   いま = {PathState()}");
            sb.AppendLine($"  [ ]  透過 {(demo != null ? demo.transmissionGainDb : 0f):F0}dB（音源の向き）"
                          + $"　, .  回折 {(demo != null ? demo.diffractionGainDb : 0f):F0}dB（隙間の向き）"
                          + $"　F11/F12 こもり {(demo != null ? demo.transmissionHighCutDb : 0f):F0}dB（壁の奥で鳴っている感じ）");
            sb.AppendLine();
            sb.AppendLine("  F1   いまの状態をコンソールへ吐く（そのまま貼れる）");
            sb.AppendLine("  F4   定常音を差し替え（滝=全帯域 → 川=高域だけ → 風=低域だけ）");
            sb.AppendLine($"  F3   定位のステア = {(demo != null && demo.useDirectionalSteering ? "ON（遮蔽時は見かけの方向＝いまは反転する）" : "OFF（常に音源の方位）")}");
            sb.AppendLine($"  F2   回折をHRTFに載せる = {(demo != null && demo.diffractionHrtf ? "ON" : "OFF")}"
                          + "  ← 定位が扉に引きずられるかどうかの本命");
            sb.AppendLine();
            sb.AppendLine("  ★狙い（発注者の指定）: **定位は扉で変えない。**");
            sb.AppendLine("    閉じていても透過が音源の向きを見せ、開くにつれて入ってくる量と");
            sb.AppendLine("    音色だけが変わる。定位が動くのは狙いではない。");
            sb.AppendLine();
            sb.AppendLine("  ★逆さを追うとき: 上の「幾何=」と「エンジン:」を見比べる。");
            sb.AppendLine("    両方同じで耳だけ逆 → 反転は C++ の HRTF より奥");
            sb.AppendLine("    エンジンが既に逆   → 反転はタップの選び方／到来点");

            GUI.Label(new Rect(12, 12, 860, 300), sb.ToString());
        }
    }
}
