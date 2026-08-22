// WorldSet.cs
// 世界を複数まとめて抱えておき、**1 つだけを「生きている世界」にする**。
//
// ★なぜシーンを切り替えないのか
//   LoadScene(Single) だと暗転が要る。扉をくぐる瞬間は §0 の芯なので、
//   そこに読み込みの引っかかりを置きたくない。
//   隣の世界を先に読み込んでおけば、くぐる動作は**プレイヤーを写像するだけ**になる。
//
// ★同時に生きていいのは 1 つだけ。これは音響側の都合で決まっている。
//   AcousticFlowSceneDemo は OnEnable でエンジンのシーンを作り、
//   OnDisable で全部登録解除して Dispose する。2 つ有効にすると 2 面が立つ。
//   さらに CollectOccluders はシーン全体の BoxCollider / MeshCollider を舐めるので、
//   隣の世界のコライダーが有効なままだと**部屋グラフの格子が 4000m 先まで伸びて破裂**する
//  （上限 400 万ボクセルに収めるためセルが黙って粗くなる）。
//
//   → 生きていない世界は「コライダー無効・音の部品無効・専用レイヤー」にする。
//     見えるのはポータルのカメラごしだけ。エンジンからは存在しないのと同じ。
//
// ★破棄ではなく無効化にしてある。
//   以前は剥がして捨てていたが、それだと**その世界へ移れない**。
//   行き来するなら、生き返らせられる形で眠らせておく必要がある。
//
// ⚠ 切り替えの瞬間、音には継ぎ目が出る（エンジンのシーンが作り直されるので尾も落ちる）。
//   画は繋がるが音は繋がらない。ここを繋ぐのはサウンドシステム側の仕事。
using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.SceneManagement;
using AcousticFlow;

namespace BellGame
{
    // ★誰よりも先に動く。
    //   眠らせる判断は「生成時のまま」の状態を見て決めるので、
    //   他の Awake/OnEnable が走った後だと**壊れた後の状態を正解として覚えてしまう**。
    //   AcousticFlowSceneDemo は OnEnable でエンジンのシーンを作るので、そこも先回りする。
    [DefaultExecutionOrder(-10000)]
    [DisallowMultipleComponent]
    public sealed class WorldSet : MonoBehaviour
    {
        [Header("配置")]
        // ★行き先の世界を、その扉がこちらの扉と**重なる位置**へ動かす。
        //
        //   こうすると二つの空間が扉のところで直接つながる。
        //     ・向こうの地面は扉の裏側、こちらの地面は扉の手前 ── 隙間なく並ぶ
        //     ・くぐるときプレイヤーは**何も動かない**。ただ歩くだけ
        //     ・ポータルのカメラは写像不要（プレイヤーのカメラそのもので合う）
        //     ・切り替わるのは「どちらを描くか」と「どちらが音響的に生きているか」だけ
        //
        //   転移が無くなるので、転移の副作用（判定の帯・解除・クールダウン・往復）が
        //   まるごと消える。直近ずっと戦っていたのは全部その副作用だった。
        //
        // ⚠ 条件: **世界の中身が扉の片側だけに収まっていること。**
        //   両側にはみ出していると、向こうの世界と重なって埋まる。
        // ⚠ 既定は**切**。扉の位置は世界によって違う（発注者の決定）ので、
        //   扉が世界の真ん中にある世界では、重ねた行き先が半分めり込む。
        //   E でくぐる動作を入れた時点で通り道はこちらが決めているので、
        //   写像（転移）でも継ぎ目は見えない ── つなぎ方式に頼る必要が無くなった。
        [Tooltip("行き先の扉をこちらの扉に重ねて、二つの空間を直接つなぐ。扉が端にある世界だけで使える。")]
        public bool stitchWorlds;

        [Tooltip("世界どうしを離す間隔（つながない場合の置き場所）。")]
        public float slotSpacing = 4000f;
        // ★眠っている世界の光を消すか。既定 ON。
        //   OFF にすると以前の「常に点ける」へ完全に戻ります。
        public bool sleepingLightsOff = true;

        [Tooltip("眠っている世界を置くレイヤー名。")]
        public string previewLayerName = "WorldPreview";

        [Header("診断（読み取り専用）")]
        public WorldId active = WorldId.None;
        public string loadedWorlds = "—";
        public bool busy;

        // ★最後に世界を移った時刻。往復を止めるための門。
        //
        //   くぐる判定は WorldTransition が持っているが、あれは世界ごとに居て、
        //   移るたびに有効・無効が入れ替わる。そこに状態を置くと
        //   「いつ解除されるか」が有効化の順番に依存してしまい、実際それで往復が止まらなかった
        //  （抜けた先の到着位置が判定面のちょうど上で、1 秒ごとに往復し続けた）。
        //
        //   WorldSet は一度も無効にならない。門はここに置くのが確実。
        [Tooltip("最後に世界を移った時刻。")]
        public float lastCrossTime = -999f;
        // ★短くした（0.8 → 0.3s）。
        //   長い間隔が要ったのは、手前で切り替えていた頃に到着位置が判定面の上に来て
        //   往復し続けたから。面ちょうどで切り替えるようになったいま、
        //   守りたいのは 1 フレームの震え（押し出し・当たり）だけなので、短くてよい。
        //   長いままだと、扉をまたいで行き来するのが不自由になる。
        [Tooltip("移った直後、この秒数は次を受け付けない。震え止め。")]
        [Range(0f, 3f)] public float crossCooldown = 0.3f;


        public bool CrossReady => Time.time - lastCrossTime >= crossCooldown;
        public float CrossWait => Mathf.Max(0f, crossCooldown - (Time.time - lastCrossTime));

        public int PreviewLayer { get; private set; } = -1;
        public static WorldSet Current { get; private set; }

        /// 1 つの世界。眠っていても壊さない。
        public sealed class World
        {
            public WorldId id;
            public GameObject root;          // 中身をぶら下げる親
            public Transform door;           // その世界の扉（写像の基準）
            public Transform listener;       // その世界のリスナー
            public bool live;                // いま生きているか
            public string sceneName;         // 追加読み込みしたシーン（ホームは空）
            // ★「元がどうだったか」を必ず覚える。起こすときに全部 true にしてはいけない ──
            //   VoiceConvolver(C++) と IrConvolver(C#) は排他で、生成時は片方が false。
            //   両方 true にすると互いに相手を降ろし合い、音もカメラも壊れる。
            public readonly List<Collider> colliders = new List<Collider>();
            public readonly List<bool> colliderWas = new List<bool>();
            public readonly List<Behaviour> parts = new List<Behaviour>();
            public readonly List<bool> partWas = new List<bool>();
            public readonly List<Light> lights = new List<Light>();
            public readonly List<Camera> cameras = new List<Camera>();
            public readonly List<AudioListener> ears = new List<AudioListener>();

            // ★その世界の音源。プレイヤーの装置（デモ・カメラ・耳）は世界に属さないので、
            //   世界が持つのは「鳴る物」だけ。移ったらこれを付け替える。
            public Transform sourceMain;
            public Transform[] sourceExtra;

            // ★扉が「その世界のどこに立つか」。根っこから見た相対の姿勢。
            //   扉そのものは世界に属さない（1 枚しかない）ので、
            //   世界が持つのは**目印**だけ。世界をこの目印で扉へ合わせる。
            public Matrix4x4 doorLocal = Matrix4x4.identity;
            public bool hasDoorLocal;
        }

        private readonly Dictionary<WorldId, World> _worlds = new Dictionary<WorldId, World>();
        private int _nextSlot;

        public World Get(WorldId id) => _worlds.TryGetValue(id, out var w) ? w : null;

        /// その世界の太陽（最初の平行光）。
        ///
        /// ★要る理由: `Skybox/Procedural` の太陽は**マテリアルではなくシーンの光**から来る
        ///   （`RenderSettings.sun`、無ければ一番明るい平行光）。
        ///   だから空だけ差し替えても、**照らしているのは現世の太陽**になる。
        ///   崩壊都市の空（大気 1.35 ＝ 強い橙の地平）を草原の明るい太陽で照らすと
        ///   黄色い帯が出る ── 「緑から赤に行くときだけ空が変」の正体がこれ。
        public Light SunOf(WorldId id)
        {
            var w = Get(id);
            if (w == null) return null;
            foreach (var l in w.lights)
                if (l != null && l.type == LightType.Directional) return l;
            return null;
        }
        public bool IsLoaded(WorldId id) => _worlds.ContainsKey(id);

        public static string SceneNameOf(WorldId id)
        {
            switch (id)
            {
                case WorldId.Grassland: return "Stage1_Grassland";
                case WorldId.Temple: return "Stage2_Ruins";
                case WorldId.Cave: return "Stage3_Cave";
                case WorldId.Snow: return "Stage4_Snow";
                case WorldId.Sea: return "Stage5_Sea";
                case WorldId.White: return "Stage0_Layout";
                default: return null;
            }
        }

        // ★跨がずに「デモの入れ直し」だけを起こす切り分け（F4）。
        //
        //   実測: 跨ぐと**毎回、2 フレーム後に 24〜34ms のフレーム**が来る。
        //   位置は一度も戻っていない（押し戻しは無罪）。
        //   Activate 自体は 0.8〜1.1ms なので、**同期に走っている処理ではない**。
        //
        //   Activate がやることは大きく 3 つ ── レイヤーの入れ替え／空の差し替え／
        //   デモの入れ直し（音源を登録し直すため）。どれが遅れて重くなっているかは
        //   推測で決めない。入れ直しだけを単独で起こして、同じ形が出るかを見る。
        //
        //     出る   → 犯人は入れ直し（エンジンのシーン再構築）。連絡板へ
        //     出ない → 犯人はレイヤーか空。こちらで直せる
        private void Update()
        {
            // ★行き先が変わったら、扉の向こうの世界を置き直す。
            //   行き先を決めるのはベル（BellCallResponse）や操作卓で、
            //   どちらも `WorldDoor.destination` を書くだけです。
            //   置き直す役はここ 1 箇所に置きます（決めごと #1：答えは 1 つ）。
            var wdNow = (RigDoor != null) ? RigDoor.GetComponent<WorldDoor>() : null;
            if (wdNow != null && wdNow.destination != _placedDest) PlaceSleeping();

            if (!Input.GetKeyDown(KeyCode.F4) || _rigDemo == null) return;

            var sw = System.Diagnostics.Stopwatch.StartNew();
            _rigDemo.enabled = false;
            _rigDemo.enabled = true;
            sw.Stop();

            Debug.Log($"[BellGame] 切り分け(F4): デモの入れ直しだけを実行 "
                      + $"= {sw.Elapsed.TotalMilliseconds:F1} ms（跨いでいない）");
            StartCoroutine(WatchFrames("F4 入れ直し"));
        }

        /// 何かをした後の 4 フレームの長さを出す。重いフレームがどこに来るかを見る。
        public IEnumerator WatchFrames(string what)
        {
            for (int f = 0; f < 4; f++)
            {
                yield return new WaitForEndOfFrame();
                float dt = Time.deltaTime;
                Debug.Log($"[BellGame] {what} の後 {f + 1} フレーム目: {dt * 1000f:F1}ms"
                          + (dt > 1f / 55f ? "（★重い）" : ""));
            }
        }

        private void Awake()
        {
            Current = this;
            PreviewLayer = LayerMask.NameToLayer(previewLayerName);
            if (PreviewLayer < 0)
                Debug.LogError("[BellGame] レイヤー '" + previewLayerName + "' が無い。"
                               + "メニュー BellGame/セットアップ/1 を実行してほしい。");

            // ★Start ではなく Awake。他の部品が動き出す前に眠らせる。
            if (!AdoptPlaced()) AdoptHome();
        }

        [Header("全部入りシーンのとき")]
        [Tooltip("最初に立つ世界。World_<id> の根っこが並んでいるシーンで使う。")]
        public WorldId startWorld = WorldId.Grassland;

        // ★全部入りシーン：World_<id> の根っこが最初から並んでいる場合。
        //
        //   こちらが本線。追加読み込みは「読んだ瞬間に相手の OnEnable が走る」ので、
        //   起動を止める・剥がす・起こし直す、という層がまるごと要った。
        //   最初から置いてあれば、やることは**眠らせる／起こす**だけで済む。
        private bool AdoptPlaced()
        {
            var scene = SceneManager.GetActiveScene();
            bool any = false;

            foreach (var go in scene.GetRootGameObjects())
            {
                if (!go.name.StartsWith("World_")) continue;
                var name = go.name.Substring("World_".Length);
                if (!System.Enum.TryParse<WorldId>(name, out var id)) continue;
                if (_worlds.ContainsKey(id)) continue;

                var w = Collect(id, go, null);
                _worlds[id] = w;
                any = true;
            }
            if (!any) return false;

            // 立つ世界を決める。指定が読み込まれていなければ手近な 1 つ。
            var first = startWorld;
            if (!_worlds.ContainsKey(first))
                foreach (var kv in _worlds) { first = kv.Key; break; }


            // ★立つ世界は触らない。
            //   一度眠らせて起こすと、AcousticFlowSceneDemo が OnDisable で
            //   エンジンのシーンを壊し、OnEnable で作り直す ── 起動のたびに無駄な往復になる。
            //   生成時点で「立つ世界だけ起きている」状態で保存してあるので、そのままでよい。
            foreach (var kv in _worlds)
                if (kv.Key != first) SetLive(kv.Value, false);

            var live = _worlds[first];
            live.live = true;
            EnsureVisible(live);
            AdoptRig(live);
            SwitchEyes(live);

            // ★眠っている世界を**起動時に 1 回**、扉の向こうへ置く。
            //   ここで置いておけば、以後は「起きているほうを入れ替える」だけで済みます。
            //   置くのは装置の扉が決まった後（AdoptRig の後）でないと、合わせる先がありません。
            //
            //   ⚠ 生きている世界は置きません。造形レーンが置いた座標のままです ──
            //     プレイヤーはそこに立っているので、動かしたら足元が抜けます。
            PlaceSleeping();

            active = first;
            ApplyLiveSky(first);      // 起動時も空を合わせる（移った後だけ合わせても片手落ち）
            _nextSlot = _worlds.Count;
            Report();
            DumpDiagnostics();
            return true;
        }

        // ★いま開いているシーンを 1 つめの世界として引き取る。
        //   根っこをまとめて親の下へ入れ、以降は他の世界と同じ扱いにする。
        private void AdoptHome()
        {
            var bell = FindFirstObjectByType<BellCallResponse>();
            var id = (bell != null) ? bell.world : WorldId.Grassland;

            var root = new GameObject("World_" + id);
            var scene = SceneManager.GetActiveScene();
            foreach (var go in scene.GetRootGameObjects())
            {
                if (go == root) continue;
                if (go == gameObject) continue;                    // 自分は動かさない
                if (go.transform.parent != null) continue;
                go.transform.SetParent(root.transform, true);
            }

            var w = Collect(id, root, null);
            w.live = true;
            active = id;
            _worlds[id] = w;
            _nextSlot = 1;
            Report();
        }

        /// その世界を読み込んでおく（眠った状態で）。すでにあれば何もしない。
        public IEnumerator Ensure(WorldId id)
        {
            if (id == WorldId.None || _worlds.ContainsKey(id)) yield break;

            string scene = SceneNameOf(id);
            if (string.IsNullOrEmpty(scene) || !Application.CanStreamedLevelBeLoaded(scene))
            {
                Debug.LogWarning($"[BellGame] シーン '{scene}' が Build Settings に無い。"
                                 + "メニュー BellGame/セットアップ/2 を実行してほしい。");
                yield break;
            }

            busy = true;
            var op = SceneManager.LoadSceneAsync(scene, LoadSceneMode.Additive);
            while (!op.isDone) yield return null;

            var loaded = SceneManager.GetSceneByName(scene);
            if (!loaded.IsValid()) { busy = false; yield break; }

            // ★まず全部止める。読み込んだ瞬間に相手の OnEnable が走っており、
            //   向こうの AcousticFlowSceneDemo が既にエンジンのシーンを作っている。
            //   SetActive(false) で OnDisable まで通して片付けさせる。
            foreach (var go in loaded.GetRootGameObjects()) go.SetActive(false);

            var root = new GameObject("World_" + id);
            foreach (var go in loaded.GetRootGameObjects())
                go.transform.SetParent(root.transform, true);
            root.transform.position = new Vector3(slotSpacing * _nextSlot++, 0f, 0f);

            var w = Collect(id, root, scene);
            SetLive(w, false);

            foreach (Transform t in root.transform) t.gameObject.SetActive(true);
            SetLive(w, false);       // 起こし直した部品をもう一度眠らせる

            _worlds[id] = w;

            // ★後から読み込んだ世界も、音源の目印を隠す。
            //   起動時に一度だけ隠していたので、追加読み込みの世界は素通りで、
            //   起きた瞬間に球が現れていた。
            HideMarkers();

            busy = false;
            Report();
        }

        // ★扉は 1 枚しかない。装置と同じで、世界に属さない。
        //
        //   世界ごとに扉を持たせていたせいで、移るたびに**別の扉へ乗り換えて**いた。
        //   設定は「扉は世界に属さない。同じ 1 枚を両側から見ている」なので、
        //   実装がそれに反していた。そこから角度の同期・行き先の同期・
        //   ポータルが 2 つ動く・解除の状態が消えるコンポーネントに載る、が全部出ていた。
        //
        //   いまは扉を 1 枚だけ持ち、**世界のほうを扉へ合わせて動かす**。
        //   扉は一度も動かないので、くぐってもプレイヤーは同じ扉のところに居る。
        public Transform RigDoor { get; private set; }

        // ★プレイヤーが扉のどちら側に立っているか（+1 = 扉の forward 側、−1 = 裏側）。
        //
        //   これが要る理由：**扉は一度も動かない**ので、くぐるとプレイヤーは
        //   扉の反対側に出る。次からは「向こう側」の意味が入れ替わる。
        //
        //   これを持たずに「行き先は常に裏、生きている世界は常に手前」としていたのが
        //   **「くぐるとステージが切り替わって、見ていたのと反対向きに出る」**の正体。
        //   覗いている間は行き先を裏（180 度回して）置き、くぐった瞬間に
        //   手前（回さない）へ置き直していたので、渡りきったところで世界が 180 度飛んでいた。
        //
        //   ★不変条件: 覗いているときの置き方と、くぐった後の置き方は**同じでなければならない**。
        //     LiveFlip / PreviewFlip がその一致を保証する
        //     ── 側が入れ替わると PreviewFlip の値がそのまま LiveFlip になる。
        public int PlayerSide { get; private set; } = 1;

        /// 生きている世界の置き方（プレイヤーの居る側へ中身を向ける）。
        public bool LiveFlip => PlayerSide < 0;

        /// 覗いている世界の置き方（扉の反対側へ中身を向ける）。
        public bool PreviewFlip => PlayerSide > 0;

        /// くぐりきったところで呼ぶ。以後、扉の「向こう」の意味が入れ替わる。
        public void FlipPlayerSide() => PlayerSide = -PlayerSide;

        // ★行き先の世界は**扉に重ねて置く**。
        //
        //   一度「離して置き、くぐる瞬間に運ぶ」形にしたが、それは
        //   **渡る瞬間をこちらが決められる**ことに依存していた
        //   （開口が画面を覆っているフレームでしか運べない）。
        //   発注者が求めているのは**渡り方の自由**なので、重ねる形に戻す。
        //   二つの空間が扉のところで直接つながっていれば、どこからどう跨いでも成立する。
        //   入れ替わるのは「どちらを描くか」「どちらが音響的に生きているか」だけで、
        //   **プレイヤーは一切動かない**。
        //
        //   代償は Scene で二つの世界が交差すること。これは受け入れる。
        //   ただし**開口の外へ漏れることは許さない** ── 眠っている世界は専用レイヤーに置き、
        //   本編カメラのカリングマスクから外してある。
        [Tooltip("重ねるのをやめて、離した場所のまま描く（漏れの切り分け用）。")]
        public bool debugParkInsteadOfPlace;

        // ★世界を置く答えは PlaceWorld ひとつ。
        //   仮置き（ParkWorld）を併存させていたが、置き方の答えが 2 つあると
        //   どちらが最後に動かしたかで結果が変わる。生成時の座標のままにして、
        //   扉に合わせるのは PlaceWorld だけにする。

        // 180 度回す ── くぐるとは「扉の手前で −Z へ進むことが、向こうで +Z へ進むこと」。
        private static readonly Matrix4x4 Flip = Matrix4x4.Rotate(Quaternion.Euler(0f, 180f, 0f));

        /// 世界を、その目印が扉と重なる位置へ置く。
        ///   flipped=true で 180 度回す ── 扉の**向こう側**に置きたいとき。
        public void PlaceWorld(World w, bool flipped)
        {
            if (w == null || w.root == null || RigDoor == null || !w.hasDoorLocal) return;
            if (debugParkInsteadOfPlace) return;

            Matrix4x4 wantRoot = RigDoor.localToWorldMatrix
                               * (flipped ? Flip : Matrix4x4.identity)
                               * w.doorLocal.inverse;

            var pos = (Vector3)wantRoot.GetColumn(3);
            var rot = wantRoot.rotation;
            if ((w.root.transform.position - pos).sqrMagnitude < 1e-6f
                && Quaternion.Angle(w.root.transform.rotation, rot) < 0.01f) return;

            // ★太陽は世界と一緒に回さない。
            //
            //   行き先は扉に合わせて 180 度回して置く。平行光は世界の子なので、
            //   何もしないと**太陽まで 180 度回り、影の向きが真逆になる**。
            //   実際そうなっていた（発注者の報告）。
            //
            //   太陽は「その場所にある物」ではなく「空の向き」なので、
            //   世界をどこへ置き直しても向きは変わらない。
            //   ★向きの出どころは触らない（生成時に決めた値をそのまま保つ）。
            //     ここで WorldSky から引き直すと、ラボのように意図して
            //     揃えてある場合にそれを踏み潰す。
            var sunWas = new Quaternion[w.lights.Count];
            for (int i = 0; i < w.lights.Count; i++)
                if (w.lights[i] != null) sunWas[i] = w.lights[i].transform.rotation;

            w.root.transform.SetPositionAndRotation(pos, rot);

            for (int i = 0; i < w.lights.Count; i++)
            {
                var l = w.lights[i];
                // 点光源・スポットは場所に属するので、世界と一緒に動いてよい。
                if (l != null && l.type == LightType.Directional)
                    l.transform.rotation = sunWas[i];
            }

            // 位置から導いている量を導き直す（範囲は世界の位置に依存する）。
            foreach (var b in w.root.GetComponentsInChildren<WorldBounds>(true)) b.Derive();

            Debug.Log($"[BellGame] {BellVoices.DisplayName(w.id)}を扉へ合わせた"
                      + (flipped ? "（向こう側・180度）" : "（手前側・回さない）")
                      + $" ／ 立っている側={PlayerSide:+0;-0} "
                      + $"生={(w.live ? "生" : "眠")} 呼び出し元={(w.live ? "★生きている世界を動かした" : "正常")}");
        }

        /// 行き先の世界を、その扉がこちらの扉と重なる位置へ動かす。
        ///
        /// ★180 度回すのは「くぐる」ものだから ── 手前で −Z へ進むことが、
        ///   向こうで +Z へ進むことに対応する。転移していたときの写像と同じ考え方で、
        ///   違うのは**プレイヤーではなく世界のほうを動かす**点。
        public void Align(WorldId destId, Transform hereDoor)
        {
            if (!stitchWorlds || hereDoor == null) return;
            var w = Get(destId);
            if (w == null || w.root == null || w.door == null) return;
            if (w.live) return;                       // 生きている世界は動かさない

            // 扉が根っこから見てどこに在るか（動かしても変わらない関係）。
            Matrix4x4 rootToDoor = w.root.transform.worldToLocalMatrix * w.door.localToWorldMatrix;

            // 置きたい扉の姿勢＝こちらの扉を 180 度回したもの。
            Matrix4x4 flip = Matrix4x4.Rotate(Quaternion.Euler(0f, 180f, 0f));
            Matrix4x4 wantDoor = hereDoor.localToWorldMatrix * flip;

            // そこから根っこの姿勢を逆算する。
            Matrix4x4 wantRoot = wantDoor * rootToDoor.inverse;

            var pos = (Vector3)wantRoot.GetColumn(3);
            var rot = wantRoot.rotation;
            if ((w.root.transform.position - pos).sqrMagnitude < 1e-6f
                && Quaternion.Angle(w.root.transform.rotation, rot) < 0.01f) return;

            w.root.transform.SetPositionAndRotation(pos, rot);

            // ★動かしたら、その世界の**位置から導いている量**を導き直す。
            //   WorldBounds は Start で一度しか導かないので、動かすと
            //   範囲の中心が古い座標のまま残る。抜けた瞬間にその古い範囲へ
            //   押し戻されて、**見た目には「飛んだ」ように見える**。
            foreach (var b in w.root.GetComponentsInChildren<WorldBounds>(true)) b.Derive();

            Debug.Log($"[BellGame] {BellVoices.DisplayName(destId)}を扉に合わせて置いた"
                      + $"（根っこ {pos}）。ここから先は**歩くだけ**で繋がる。");
        }

        /// 使っていない世界を捨てる。生きている世界と、その扉の行き先は残す。
        public IEnumerator Trim(WorldId keepA, WorldId keepB)
        {
            var drop = new List<WorldId>();
            foreach (var kv in _worlds)
                if (kv.Key != keepA && kv.Key != keepB) drop.Add(kv.Key);

            foreach (var id in drop)
            {
                var w = _worlds[id];
                // ★最初からシーンに置いてある世界は捨てない。眠らせたままにする。
                //   捨てるとシーンから消えてしまい、二度と戻れない。
                if (string.IsNullOrEmpty(w.sceneName)) { SetLive(w, false); continue; }

                _worlds.Remove(id);
                if (w.root != null) Destroy(w.root);
                if (!string.IsNullOrEmpty(w.sceneName)
                    && SceneManager.GetSceneByName(w.sceneName).isLoaded)
                {
                    var un = SceneManager.UnloadSceneAsync(w.sceneName);
                    while (un != null && !un.isDone) yield return null;
                }
            }
            if (drop.Count > 0) Report();
        }

        /// 生きている世界を入れ替える。**古い方を先に落としてから**新しい方を起こす。
        ///   順番が逆だと、一瞬エンジンのシーンが 2 つ立つ。
        public void Activate(WorldId id)
        {
            var next = Get(id);
            if (next == null) { Debug.LogWarning($"[BellGame] {id} は読み込まれていない。"); return; }

            // ★① 目と耳を**先に**付け替える。
            //   これが後だと、次の世界の AcousticFlowSceneDemo が OnEnable で
            //   Camera.main を掴む瞬間に有効なカメラが 0 個で、null を掴む。
            //   以後その世界ではカメラが一切動かない ＝「移った先で動けない」になる。
            EnsureVisible(next);

            // ★② 古い世界を落とす。新しい方を起こす前に（2 面立てないため）。
            foreach (var kv in _worlds)
                if (kv.Key != id && kv.Value.live) SetLive(kv.Value, false);

            // ★③ 起こして装置を繋ぎ替える。**入る世界は動かさない。**
            //
            // ⚠ ここで PlaceWorld(next, ...) を呼んでいたのを外しました。
            //
            //   跨ぐ動きはプレイヤーを扉の向こう（＝覗いていた世界が在る場所）へ運びます。
            //   その**後**に行き先を置き直すと、置き方が 1mm でも違えば
            //   世界だけが動いてプレイヤーが取り残されます。実際にそうなっていた:
            //
            //       くぐる前  音源 0.0 / 5.1 / 10.2 / 11.4m …
            //       くぐった後 音源 4.1 / 390.7 / 392.3 / 397.2m …（400m 先に置き去り）
            //
            //   「同じ置き方なら動かないはず」という前提で呼んでいましたが、
            //   **呼ばなければ前提そのものが要りません。**
            //
            //   ★新しい規則: **いま立っている世界は絶対に動かさない。**
            //     動かしてよいのは眠っている世界だけ（見えていないので、いつ動いてもよい）。
            //     草が置き去りになる・ベルが消える・向きが裏返るも、全部これで消えます。
            SetLive(next, true);
            RepointRig(next);

            // ★④ 装置の目と耳が生きていることを確かめる。
            //   ここを省いていたせいで、移った瞬間に有効なカメラが 0 個になり暗転した。
            SwitchEyes(next);

            active = id;

            // ★④' 眠っている世界を、いまの立ち位置に合わせて置き直す。
            //   跨いだことで「扉のどちら側が向こう側か」が入れ替わったので、
            //   次に覗く世界はもう一方の側に立っていないといけない。
            //   眠っている＝見えていないので、ここで動かしても誰も気づきません。
            PlaceSleeping();

            ApplyLiveSky(id);
            Report();
            DumpDiagnostics();
        }

        /// 眠っている世界を、いまの立ち位置から見て「扉の向こう」へ置く。
        ///   ★生きている世界には**触らない**。そこにプレイヤーが立っているので。
        ///
        /// ★世界が 3 つになって壊れました。
        ///   ここは**眠っている世界を全部**扉の向こうへ置いていました。
        ///   眠りが 1 つのうちは正しく見えていましたが、2 つになると
        ///   **2 つの世界が同じ場所に重なって置かれます** ── 発注者の
        ///   「二つともの世界とつなげてる」がこれです。
        ///
        /// ★扉が繋がる先は 1 つだけ（`WorldDoor.destination`）。
        ///   置くのもその 1 つだけで、残りは遠くへ退けます。
        ///   行き先が None なら、どの世界も扉へは来ません（＝何処にも繋がっていない）。
        private void PlaceSleeping()
        {
            var wd = (RigDoor != null) ? RigDoor.GetComponent<WorldDoor>() : null;
            _placedDest = (wd != null) ? wd.destination : WorldId.None;

            int parked = 0;
            foreach (var kv in _worlds)
            {
                var w = kv.Value;
                if (w == null || w.live) continue;

                if (kv.Key == _placedDest) PlaceWorld(w, flipped: PreviewFlip);
                else ParkAway(w, ++parked);
            }
        }

        /// 行き先でない世界を遠くへ退ける。
        ///   ★消すのではなく退けるだけ ── 読み込み直しの費用を払いたくないのと、
        ///     行き先を選び直した瞬間に戻せるようにするため。
        ///   ★扉から測ります。生きている世界がどこに在っても離れることが保証されるので。
        private void ParkAway(World w, int n)
        {
            if (w == null || w.root == null || RigDoor == null) return;
            Vector3 to = RigDoor.position + new Vector3(slotSpacing * n, 0f, 0f);
            if ((w.root.transform.position - to).sqrMagnitude < 1e-6f) return;

            w.root.transform.position = to;
            foreach (var b in w.root.GetComponentsInChildren<WorldBounds>(true)) b.Derive();
        }

        /// いま扉へ置いてある世界。行き先が変わったら置き直す合図に使う。
        private WorldId _placedDest = WorldId.None;

        // ★いま居る世界の空・霧・環境光に切り替える。
        //
        //   ここが抜けていたので、**世界を移っても空が最初の世界のまま**だった。
        //   扉ごしは差し替えて描いていたので正しく、立っている世界だけ間違っていた ──
        //   だから「緑から赤に行くときだけ空の色がおかしい」に見えていた
        //  （草原の青のままなので、崩壊都市に立つと食い違う）。
        //
        //   ★空を決めるのは「いまどの世界に居るか」で、それを知っているのはここだけ。
        //     生成時に一度呼ぶ形だと、移った後の答えを持てない。
        //
        //   太陽も一緒に指す。Skybox/Procedural の太陽は**マテリアルではなく
        //   シーンの光**（RenderSettings.sun）から来るので、指さないと
        //   別の世界の太陽で空が焼かれる。
        private readonly Dictionary<WorldId, Material> _skyCache = new Dictionary<WorldId, Material>();

        private void ApplyLiveSky(WorldId id)
        {
            if (!_skyCache.TryGetValue(id, out var sky) || sky == null)
            {
                sky = WorldSky.MakeSkybox(id);
                _skyCache[id] = sky;
            }
            WorldSky.Apply(id, sky);

            var sun = SunOf(id);
            if (sun != null) RenderSettings.sun = sun;

            Debug.Log($"[BellGame] 空を{BellVoices.DisplayName(id)}のものにした"
                      + (sun != null ? $"（太陽 {sun.transform.eulerAngles.y:F0}°）" : "（太陽が無い）"));
        }

        // ── 中身 ──────────────────────────────────────────────

        private World Collect(WorldId id, GameObject root, string sceneName)
        {
            var w = new World { id = id, root = root, sceneName = sceneName };

            var door = root.GetComponentInChildren<WorldDoor>(true);
            if (door != null)
            {
                w.door = door.transform;
                // 扉が根っこから見てどこに立つか。世界を動かしても変わらない関係。
                w.doorLocal = root.transform.worldToLocalMatrix * door.transform.localToWorldMatrix;
                w.hasDoorLocal = true;
            }

            var demo = root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
            if (demo != null)
            {
                if (demo.listener != null) w.listener = demo.listener;
                // 音源だけ控える。デモそのものは（最初の 1 つを除いて）使わない。
                w.sourceMain = demo.source;
                w.sourceExtra = demo.extraSources;
            }

            // ★いま有効かどうかで選り分けない。
            //   全部入りシーンでは、生成時点で既に眠らせてある世界がある。
            //   「有効な物だけ覚える」にすると、起こすときに何も戻らない。
            foreach (var c in root.GetComponentsInChildren<Collider>(true))
                if (c != null) { w.colliders.Add(c); w.colliderWas.Add(c.enabled); }

            foreach (var m in root.GetComponentsInChildren<Behaviour>(true))
            {
                if (m == null) continue;
                if (m is Light l) { w.lights.Add(l); continue; }
                if (m is Camera cam) { w.cameras.Add(cam); continue; }
                if (m is AudioListener ear) { w.ears.Add(ear); continue; }
                if (IsWorldPart(m)) { w.parts.Add(m); w.partWas.Add(m.enabled); }
            }
            return w;
        }

        // 世界に属する部品＝眠らせる対象。
        //   ★音を出す物とエンジンに触る物は必ず入れること。1 つでも残ると 2 面が立つ。
        private static bool IsWorldPart(Behaviour m)
        {
            switch (m.GetType().Name)
            {
                case "AcousticFlowSceneDemo":
                case "VoiceConvolver":
                case "IrConvolver":
                case "NonUniformConvolver":
                case "PartitionedConvolver":
                case "HrtfProcessor":
                case "ResizableRoom":
                case "SwingDoor":
                case "BellCallResponse":
                case "BeyondBell":
                case "BeyondAmbience":
                case "AmbientSource":
                case "PlayerBells":
                case "WorldDoor":
                case "PhysicsDoor":
                case "OutdoorTailGate":
                case "PinPosition":
                case "FollowTransform":
                case "FloatSpin":
                case "WorldBounds":
                case "WorldPortalStencil":
                case "WorldTransition":
                case "WorldSkyLibrary":
                    return true;
                // ★Camera と AudioListener は**ここに入れない**。
                //   一括処理に混ぜると引き継ぎに漏れが出て「No cameras rendering」になる。
                //   どちらもシーンに 1 つだけ有効、という強い決まりがあるので明示的に扱う。
                default:
                    return m is AudioBehaviour;
            }
        }

        private void SetLive(World w, bool live)
        {
            if (w == null || w.root == null) return;
            w.live = live;

            for (int i = 0; i < w.colliders.Count; i++)
                if (w.colliders[i] != null) w.colliders[i].enabled = live && w.colliderWas[i];
            for (int i = 0; i < w.parts.Count; i++)
                if (w.parts[i] != null) w.parts[i].enabled = live && w.partWas[i];

            // 眠っている世界は専用レイヤー。本編カメラは描かず、ポータルだけが見る。
            // ★PreviewLayer が 0 だと Default と衝突して全部消える。0 は使わせない。
            if (PreviewLayer > 0)
            {
                int layer = live ? 0 : PreviewLayer;   // 0 = Default
                SetLayer(w.root.transform, layer);
                int bit = 1 << PreviewLayer;
                foreach (var l in w.lights)
                    if (l != null) l.cullingMask = live ? ~bit : bit;
            }

            // ★遊べる範囲も一緒に眠らせる。
            //
            //   SetLive は Collider と Renderer しか止めていなかったので、
            //   眠っている世界の WorldBounds が**押し戻しだけ続けていた**。
            //   本番 0⇔1 で実際にこうなっていた:
            //       白い部屋 X[393,407] Z[-9,9] ／ 草原 X[370,430] Z[-5,61]
            //   草原へくぐった後も白い部屋の範囲が効くので、**14×18m の箱に閉じ込められる**。
            //
            //   見えない・ぶつからない世界が、体だけ押してくるのは筋が通らない。
            //   範囲は「いま立っている世界」のものだけが効けばよい。
            foreach (var b in w.root.GetComponentsInChildren<WorldBounds>(true))
                if (b != null) b.enabled = live;

            // ★眠っている世界の光。
            //
            //   ここは長く `l.enabled = true;   // 光は常に必要` でした。
            //   理由は「ポータルが向こうの世界を描くときに要るから」で、それは正しい。
            //   ただし**要るのはポータルが描く一瞬だけ**です。
            //   点けっぱなしだと、影付きの平行光が世界の数だけシャドウマップを描きます
            //  （いまは 3 つ）。
            //
            //   ポータル（WorldPortalStencil）が Render() の前後で点け消しするので、
            //   ここでは消しておきます。**行き先でない世界の光は誰も要りません。**
            //
            //   ⚠ 元に戻せます ── `sleepingLightsOff = false` で以前の
            //     「常に点ける」に完全に戻ります。
            foreach (var l in w.lights)
                if (l != null) l.enabled = live || !sleepingLightsOff;
        }

        // ★プレイヤーの装置（リスナー・カメラ・耳・音響デモ）は**世界に属さない**。
        //
        //   世界ごとに持たせていたせいで、扉をくぐるたびに
        //     ・カメラが入れ替わる（掴み損ねると Camera.main が null になる）
        //     ・向こうのデモは視線の上下を 0 から始める ＝ **ピッチが水平に飛ぶ**
        //   が起きていた。「一瞬景色が切り替わる」の、避けられるほうの正体がこれ。
        //
        //   1 組だけ持って持ち歩けば、姿勢も内部状態もそのまま繋がる。
        //   世界が持つのは「鳴る物」だけで、移ったらデモの音源を差し替える。
        [Header("音源の目印")]
        // ★音源は球で見えているが、これはデバッグ用の目印。
        //   世界のベルが見えているのは**設計違反**でもある
        //  （「ベルの位置を教える飾りは置かない」＝音で探すゲームなので）。
        [Tooltip("音源の球を描かない。位置は音だけで伝える。")]
        public bool hideSourceMarkers = true;

        public Transform Rig { get; private set; }
        private AcousticFlowSceneDemo _rigDemo;
        private Camera _rigCam;
        private AudioListener _rigEar;

        // ★プレイヤーのベルも装置の一部。**世界には属さない。**
        //   あなたのベルなので、世界を移っても同じ 1 つが付いてくる。
        //   世界ごとに別の球があると、前に居たり居なかったりする（実際そうなっていた）。
        //   エンジンの index 0（demo.source）＝あなた、1 番以降＝その世界、という分け方に揃う。
        private Transform _rigSource;

        private void AdoptRig(World first)
        {
            _rigDemo = first.root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
            if (_rigDemo == null) return;

            Rig = _rigDemo.listener;
            _rigCam = (first.cameras.Count > 0) ? first.cameras[0] : Camera.main;
            _rigEar = (first.ears.Count > 0) ? first.ears[0] : null;
            _rigSource = first.sourceMain;              // ★あなたのベル

            // ★扉も装置。1 枚だけ持ち、世界の外へ出す。
            //   世界ごとに持たせていたせいで、移るたびに別の扉へ乗り換えていた。
            var d0 = first.root.GetComponentInChildren<WorldDoor>(true);
            if (d0 != null)
            {
                // 扉の一式（枠・板・ポータル・敷居・設え）をまとめて外へ。
                var doorRoot = (d0.transform.parent != null && d0.transform.parent != first.root.transform)
                    ? d0.transform.parent : d0.transform;
                RigDoor = d0.transform;
                doorRoot.SetParent(null, true);
            }

            // ★扉を見ている仕掛けも装置。扉が 1 枚なら、それを見る目も 1 つ。
            //
            //   これを忘れていたのが、移った先で
            //   「ポータル: 扉が繋がっていない（worldDoor が空）」が出る正体。
            //
            //   WorldPortalView / WorldTransition / DoorHush は世界ごとに置いてあるので、
            //   移ると**行き先の世界の複製**が目を覚ます。その複製の worldDoor は
            //   （その世界の扉は捨てたので）空 ── 向こうが映らず、E でも戻れない。
            //
            //   扉の一式の下に居る物（敷居・吸音壁）は既に一緒に出ているので触らない。
            // 旧 WorldPortalView は削除済み（ステンシル版へ移行）。
            AdoptDoorRig<WorldPortalStencil>(first);
            AdoptDoorRig<WorldTransition>(first);
            AdoptDoorRig<DoorHush>(first);

            // 装置は世界の外へ出す。世界ごと眠らせるときに巻き込まれないように。
            if (Rig != null) Rig.SetParent(null, true);
            if (_rigCam != null) _rigCam.transform.SetParent(null, true);
            if (_rigSource != null) _rigSource.SetParent(null, true);
            if (_rigDemo != null) _rigDemo.transform.SetParent(null, true);

            // ★他の世界の装置は捨てる。残すと Camera.main が揺れ、耳が 2 つになり、
            //   ベルの球が世界ごとに前に居たり居なかったりする。
            //   捨てても困らない ── その世界の音源（1 番以降）は Collect で控えてある。
            foreach (var kv in _worlds)
            {
                if (kv.Value == first) continue;
                var d = kv.Value.root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
                if (d != null) Destroy(d.gameObject);
                foreach (var c in kv.Value.cameras) if (c != null) Destroy(c.gameObject);
                foreach (var e in kv.Value.ears) if (e != null) Destroy(e);
                if (kv.Value.listener != null) Destroy(kv.Value.listener.gameObject);
                if (kv.Value.sourceMain != null) Destroy(kv.Value.sourceMain.gameObject);
                // ★その世界の扉も捨てる。扉は 1 枚しかない。
                //   立つ位置は doorLocal に控えてあるので、世界を合わせるのに困らない。
                var dw = kv.Value.root.GetComponentInChildren<WorldDoor>(true);
                if (dw != null)
                {
                    var dr = (dw.transform.parent != null && dw.transform.parent != kv.Value.root.transform)
                        ? dw.transform.parent : dw.transform;
                    Destroy(dr.gameObject);
                }
                kv.Value.door = null;
                kv.Value.sourceMain = null;
                kv.Value.cameras.Clear();
                kv.Value.ears.Clear();
                kv.Value.listener = null;
            }

            // ★装置を「世界の持ち物リスト」から外す。
            //
            //   これを忘れていたのが**暗転の正体**。
            //   装置は最初の世界の cameras / ears / parts に載ったままで、
            //   そこから移ると「古い世界を眠らせる」処理が**装置のカメラを無効化**し、
            //   新しい世界のリストは空なので誰も有効化しない ── 有効なカメラ 0 個。
            //
            //   親から外してあるので、「もう根っこの下に居ない物」を落とせばよい。
            //   「世界に属さない」を宣言したなら、リストの上でも属していてはいけない。
            DropDetached(first);
            HideMarkers();

        }

        // 扉に付き従う仕掛けを 1 つだけ残して世界の外へ出し、他の世界の複製は捨てる。
        private void AdoptDoorRig<T>(World first) where T : Component
        {
            var keep = first.root.GetComponentInChildren<T>(true);
            if (keep != null && (RigDoor == null || !keep.transform.IsChildOf(RigDoor.root)))
                keep.transform.SetParent(null, true);

            foreach (var kv in _worlds)
            {
                if (kv.Value == first || kv.Value.root == null) continue;
                foreach (var c in kv.Value.root.GetComponentsInChildren<T>(true))
                    if (c != null) Destroy(c.gameObject);
            }
        }

        // 入れ直しで水平に戻された視線の上下を書き戻す。
        //   見つからなければ黙って諦める（相手が直したらそうなる）。
        private static System.Reflection.FieldInfo _pitchField;
        private static bool _pitchWarned;

        private void RestorePitch(float pitch)
        {
            if (_rigDemo == null) return;
            if (_pitchField == null)
            {
                _pitchField = typeof(AcousticFlowSceneDemo).GetField("_pitch",
                    System.Reflection.BindingFlags.Instance
                    | System.Reflection.BindingFlags.NonPublic);
            }
            if (_pitchField == null || _pitchField.FieldType != typeof(float))
            {
                if (!_pitchWarned)
                {
                    _pitchWarned = true;
                    Debug.Log("[BellGame] デモの視線の上下を書き戻せなかった"
                              + "（相手側で残るようになったなら、この処理は消してよい）");
                }
                return;
            }

            // 0..360 で来るので -180..180 へ畳んでから渡す（デモは ±80 で刻む）。
            float p = Mathf.Repeat(pitch + 180f, 360f) - 180f;
            _pitchField.SetValue(_rigDemo, Mathf.Clamp(p, -80f, 80f));
        }

        /// エンジンに登録する音源＝「その世界の音」＋「扉の音」。
        ///
        /// 扉の音（接続世界の漏れ）は装置側の持ち物なので、世界が変わっても外さない。
        /// 世界の側からは、根っこの下にもう居ない物と破棄済みの物を落とす。
        private Transform[] BuildExtraSources(World w)
        {
            var seen = new HashSet<Transform>();
            var list = new List<Transform>();

            if (w != null && w.root != null && w.sourceExtra != null)
                foreach (var t in w.sourceExtra)
                    if (t != null && t.IsChildOf(w.root.transform) && seen.Add(t)) list.Add(t);

            int doorCount = 0;
            if (RigDoor != null)
                foreach (var a in RigDoor.root.GetComponentsInChildren<AudioSource>(true))
                    if (a != null && seen.Add(a.transform)) { list.Add(a.transform); doorCount++; }

            Debug.Log($"[BellGame] 音源: {BellVoices.DisplayName(w != null ? w.id : WorldId.None)}"
                      + $"の音 {list.Count - doorCount} 本 ＋ 扉の音 {doorCount} 本");
            return list.ToArray();
        }

        // 根っこの下から出た物を、その世界の持ち物リストから落とす。
        private static void DropDetached(World w)
        {
            if (w == null || w.root == null) return;
            var root = w.root.transform;

            bool Gone(Component c) => c == null || !c.transform.IsChildOf(root);

            for (int i = w.parts.Count - 1; i >= 0; i--)
                if (Gone(w.parts[i])) { w.parts.RemoveAt(i); w.partWas.RemoveAt(i); }
            for (int i = w.colliders.Count - 1; i >= 0; i--)
                if (Gone(w.colliders[i])) { w.colliders.RemoveAt(i); w.colliderWas.RemoveAt(i); }
            w.cameras.RemoveAll(c => Gone(c));
            w.ears.RemoveAll(e => Gone(e));
            w.lights.RemoveAll(l => Gone(l));
        }

        // 音源の球とリスナーの球を描かない。位置は音だけで伝える。
        private void HideMarkers()
        {
            if (!hideSourceMarkers) return;
            int n = 0;

            // ★隠すのは**検証用の目印だけ**。本物のモデルは隠さない。
            //
            //   もとは「IrConvolver を持つ物の Renderer を全部切る」だったので、
            //   本番のベルのモデルまで消えていた（音を出す物は全部目印扱いになる）。
            //   目印か本物かは見た目では分からないので、**目印の側に名札を付けた**
            //  （SourceMarker。MakeSource が付ける）。名札が無い物は本物とみなす。
            //
            //   「音だけで探す」は守りたいが、それは**位置を教える印を出さない**という
            //   意味であって、其処に在る物を消すことではない。見つけたベルは見えてよい。
            void Hide(GameObject go)
            {
                if (go == null) return;
                if (go.GetComponentInParent<SourceMarker>() == null) return;
                foreach (var r in go.GetComponentsInChildren<Renderer>(true))
                    if (r != null && r.enabled) { r.enabled = false; n++; }
            }

            if (Rig != null) Hide(Rig.gameObject);
            if (_rigSource != null) Hide(_rigSource.gameObject);

            // ★扉に付いている音源（接続世界の漏れ）も隠す。扉は装置側なので世界の走査に入らない。
            if (RigDoor != null)
                foreach (var a in RigDoor.root.GetComponentsInChildren<AudioSource>(true))
                    Hide(a.gameObject);

            foreach (var kv in _worlds)
            {
                var w = kv.Value;
                if (w.root == null) continue;

                // ★**音源は全部隠す。**
                //
                //   以前は IrConvolver を持つ物だけ隠していた。
                //   Beyond（扉の奥の音）のような素の音源は対象外で、
                //   眠っている間はレイヤーで見えないだけ ──
                //   **起きた瞬間に球が現れる**（発注者の報告そのもの）。
                //   「装飾や光でベルの位置を明かさない」に正面から反していた。
                //
                //   隠す基準は「音として在る物」。見た目で位置を教えない。
                if (w.sourceMain != null) Hide(w.sourceMain.gameObject);
                if (w.sourceExtra != null)
                    foreach (var s in w.sourceExtra)
                        if (s != null) Hide(s.gameObject);

                foreach (var ir in w.root.GetComponentsInChildren<IrConvolver>(true))
                    Hide(ir.gameObject);
            }

            if (n > 0) Debug.Log($"[BellGame] 音源の目印を {n} 個隠した（音だけで探す）。");
        }
        /// 装置を行き先の世界へ繋ぎ替える。姿勢と内部状態はそのまま残る。
        private void RepointRig(World next)
        {
            if (_rigDemo == null) return;

            // ★index 0 は**あなたのベル**で、世界を移っても同じ 1 つ。
            //   1 番以降がその世界の音（環境音・世界のベル・扉の奥）。
            _rigDemo.source = _rigSource;

            // ★扉に付いている音源は、世界が変わっても**同じ 1 本**。
            //
            //   ここを世界の配列で丸ごと置き換えていたのが
            //   「くぐった先では扉の奥から何も漏れてこない」の正体。
            //   扉は装置側に 1 枚しか無いので、
            //     ・行き先の世界の配列に入っているのは**破棄済みの自分の扉の音源**
            //     ・いま生きている扉の音源は、どの世界の配列にも入っていない
            //   となり、接続世界の音（BeyondAmbience）がエンジンに登録されない。
            //
            //   AdoptDoorRig と同じ形の抜け ── 扉の持ち物は扉と一緒に装置側へ来る。
            // ⚠ ここで「音源が同じなら入れ直さない」をやってはいけない。**一度やって取り消した。**
            //
            //   跨ぐたびに 25ms のフレームが出るので、入れ直しを省こうとした。
            //   だが `AcousticFlowSceneDemo.OnEnable` がやり直すのは音源だけではない：
            //
            //       BuildSources();          音源
            //       AddMaterial();           材質
            //       CollectOccluders();      **遮蔽物** ← 世界が変われば必ず変わる
            //       RegisterInstances();
            //       CollectPortals();        **ポータル** ← 同上
            //
            //   省くと**エンジンは前の世界の壁を持ったまま**になる。
            //   ラボは 2 つの世界が 180 度対称でほぼ同じ形なので気づけないが、
            //   本番では音が前の世界のものになる。
            //   **25ms を節約する代わりに音を壊していた。**
            //
            //   25ms を本当に消すには、作り直しではなく差し替えができる形が要る
            //  （連絡板でサウンドシステムレーンへ依頼済み）。それまでは払う。
            _rigDemo.extraSources = BuildExtraSources(next);

            // ★入れ直してエンジンのシーンを組み直す。
            //   ピッチなどのフィールドは OnDisable/OnEnable では消えないので、
            //   視線は繋がったまま音源だけが入れ替わる。
            // ★視線の上下を守る。
            //
            //   AcousticFlowSceneDemo は初期化で `_pitch = 0f` にし、毎フレーム
            //   `カメラの回転 = Euler(_pitch, リスナーのヨー, 0)` を書く。
            //   だから入れ直すと**上下だけ水平に戻る** ── 跨いだ瞬間に
            //   「カメラの向きが修正される」の正体がこれ。
            //   ヨーはリスナーの transform に載っているので生き残る。
            //
            //   `_pitch` は private なので、控えて書き戻す。**その場しのぎ**です ──
            //   連絡板でサウンドシステムレーンに「入れ直しても上下を残す」形を頼んである。
            //   向こうが直ったらここは消す。
            float pitch = (_rigCam != null) ? _rigCam.transform.eulerAngles.x : 0f;

            _rigDemo.enabled = false;
            _rigDemo.enabled = true;

            RestorePitch(pitch);

            RepointWorldRefs(next);
            RestartAudio(next);
        }

        // ★眠っていた世界の音を鳴らし直す。
        //
        //   AmbientSource / BeyondAmbience / BellCallResponse は **Awake で一度だけ
        //   Play() を呼ぶ**作りになっている。世界を眠らせると AudioSource が無効になって
        //   再生が止まり、起こしても誰も Play() を呼び直さない ── **移った先が無音**。
        //   「移動後に音が合っていない」の正体がこれ。
        //
        //   鳴らし直すのはループしている物だけ。単発物（水滴・鳥）は
        //   無音ループでフィルタ経路を開けておく作りなので、同じ扱いでよい。
        private static void RestartAudio(World w)
        {
            if (w.root == null) return;
            int n = 0;
            foreach (var a in w.root.GetComponentsInChildren<AudioSource>(true))
            {
                if (a == null || !a.enabled || !a.gameObject.activeInHierarchy) continue;
                if (a.clip == null || !a.loop || a.isPlaying) continue;
                a.Play();
                n++;
            }
            if (n > 0) Debug.Log($"[BellGame] {BellVoices.DisplayName(w.id)}の音を {n} 本鳴らし直した。");
        }

        // ★その世界の部品が指している「リスナー」を、持ち歩く装置へ向け直す。
        //   世界ごとのリスナーは捨てたので、向け直さないと軒並み null で止まる
        //  （境界が効かない・扉が掴めない・くぐり判定が動かない）。
        private void RepointWorldRefs(World w)
        {
            if (Rig == null || w.root == null) return;

            // ★範囲は世界の位置から導く量。生きるたびに導き直す
            //   （Align で動かされているので、Start のときの値は古い）。
            foreach (var b in w.root.GetComponentsInChildren<WorldBounds>(true))
            {
                b.listener = Rig;
                b.Derive();
            }
            foreach (var d in w.root.GetComponentsInChildren<WorldDoor>(true)) d.listener = Rig;
            foreach (var t in w.root.GetComponentsInChildren<WorldTransition>(true)) t.listener = Rig;
            foreach (var h in w.root.GetComponentsInChildren<DoorHush>(true)) h.listener = Rig;
            foreach (var b in w.root.GetComponentsInChildren<BellCallResponse>(true)) b.listener = Rig;
            // 追従は装置のベルだけ。世界側の音源に付いていた FollowTransform は
            // もう対象が居ないので触らない。
            if (_rigSource != null)
                foreach (var f in _rigSource.GetComponentsInChildren<FollowTransform>(true))
                    f.target = Rig;
            foreach (var p in w.root.GetComponentsInChildren<PhysicsDoor>(true))
                p.eye = (_rigCam != null) ? _rigCam.transform : Rig;
            foreach (var v in w.root.GetComponentsInChildren<WorldPortalStencil>(true))
                if (_rigCam != null) v.playerCamera = _rigCam;

            w.listener = Rig;
        }

        // ★目と耳の付け替え。**シーン全体でちょうど 1 組だけ**有効にする。
        //
        //   一括処理（parts）に混ぜていたときは引き継ぎに漏れが出て、
        //   世界を移った瞬間に有効なカメラが 0 個になった（No cameras rendering）。
        //   「1 つだけ」という強い決まりがあるものは、数えて確かめられる形で扱う。
        // ★装置の目と耳を確かめる。**世界のカメラは触らない。**
        //
        //   以前はここで「生きている世界のカメラを有効／他を無効」にしていたが、
        //   装置を持ち歩くようにした時点でそれは間違いになった ──
        //   装置のカメラは最初の世界のリストに載っていたので、そこから移ると
        //   無効化され、新しい世界にはカメラが無いので誰も有効化しない。**暗転**。
        //
        //   いまは装置が唯一の目と耳。ここは「1 組だけ有効」を保つだけにする。
        private void SwitchEyes(World live)
        {
            // 世界側に残っているカメラ・耳は全部落とす（本来は AdoptRig で消えている）。
            foreach (var kv in _worlds)
            {
                foreach (var c in kv.Value.cameras) if (c != null) c.enabled = false;
                foreach (var e in kv.Value.ears) if (e != null) e.enabled = false;
            }

            if (_rigCam != null) _rigCam.enabled = true;
            if (_rigEar != null) _rigEar.enabled = true;

            if (_rigCam == null)
                Debug.LogWarning("[BellGame] 装置のカメラが無い。画が出ない。");

            // ★本編カメラは眠っている世界を描かない。
            //   WorldPortalView.Start に任せると、有効になった次のフレームまで
            //   全部の世界が重なって見える。
            if (PreviewLayer > 0 && _rigCam != null)
                _rigCam.cullingMask &= ~(1 << PreviewLayer);
        }

        // 生きている世界が本編カメラに映る状態にする（レイヤーと光だけ）。
        //   部品の有効/無効には触らない ── そこは生成時の状態が正解。
        private void EnsureVisible(World w)
        {
            if (PreviewLayer <= 0) return;
            SetLayer(w.root.transform, 0);
            int bit = 1 << PreviewLayer;
            foreach (var l in w.lights) if (l != null) l.cullingMask = ~bit;
        }

        // ★「何も見えない」ときに読む 1 行。原因は大抵レイヤーかカリングマスク。
        private void DumpDiagnostics()
        {
            var sb = new System.Text.StringBuilder();
            sb.Append("[BellGame] 全部入り: ");
            foreach (var kv in _worlds)
                sb.Append(BellVoices.DisplayName(kv.Key))
                  .Append(kv.Value.live ? "=生" : "=眠")
                  .Append("(layer ").Append(kv.Value.root.layer)
                  .Append(" 部品").Append(kv.Value.parts.Count).Append(") ");

            sb.Append("\n  立っている側=").Append(PlayerSide > 0 ? "+1(表)" : "-1(裏)")
              .Append(" / くぐるときの回し方=").Append(PreviewFlip ? "180度" : "そのまま");
            sb.Append("\n  置き場所:");
            foreach (var kv in _worlds)
                sb.Append(' ').Append(BellVoices.DisplayName(kv.Key))
                  .Append("=x").Append(kv.Value.root != null
                      ? kv.Value.root.transform.position.x.ToString("F0") : "?");
            if (RigDoor != null) sb.Append(" / 扉=x").Append(RigDoor.position.x.ToString("F0"));
            sb.Append("\n  previewLayer=").Append(PreviewLayer);
            var cam = Camera.main;
            if (cam != null)
                sb.Append(" / 本編カメラ=[").Append(cam.name)
                  .Append("] pos=").Append(cam.transform.position)
                  .Append(" cullingMask=0x").Append(cam.cullingMask.ToString("X8"));
            else
                sb.Append(" / ★本編カメラが無い（Camera.main が null）");

            sb.Append("\n  ★何も見えないときは、生の世界の layer が 0 か、"
                      + "cullingMask に 0x1(Default) が立っているかを見ること。");
            Debug.Log(sb.ToString());
        }

        private static void SetLayer(Transform t, int layer)
        {
            t.gameObject.layer = layer;
            for (int i = 0; i < t.childCount; i++) SetLayer(t.GetChild(i), layer);
        }

        private void Report()
        {
            var sb = new System.Text.StringBuilder();
            foreach (var kv in _worlds)
                sb.Append(kv.Value.live ? "★" : "・").Append(BellVoices.DisplayName(kv.Key)).Append(' ');
            loadedWorlds = sb.ToString();
        }
    }
}
