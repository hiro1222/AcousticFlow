// BellGameDoorLab.cs (Editor 専用)
// 扉の仕組みを**段階で**確かめるための最小シーン。
//
// ★なぜ要るのか
//   本編での検証は「4 ステージ生成 → 束ねる → 再生 → ベルを探す → 7 → 5 → 扉に正対 → 歩く」で、
//   1 回試すのに 2 分・関わる部品が 6 つあった。
//   どこが壊れているか毎回分からず、推測で往復することになった。
//
//   ここは **10 秒で 1 回試せる**。含まれない物は本当に含まれていないので、
//   動かなければ原因はその段の中にしかない。
//
// ★前の段が通ってから次を作ること。
//   1 が通らないうちに 2 を作ったのが今回の失敗そのもの。
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using AcousticFlow;

namespace BellGame.EditorTools
{
    public static class BellGameDoorLab
    {
        private const float W = 1.1f, H = 2.2f, T = 0.25f;

        // ── 段 1：扉だけ ─────────────────────────────────────────
        // 確かめること:
        //   ・5/6 で開く、離した角度で止まる
        //   ・掴んで押し引きできる
        //   ・**角度で音が変わる**（扉の奥の音が、隙間が広がるほど高域まで通る）
        // 含まないもの: ベル / 世界の入れ替え / ポータル描画
        [MenuItem("BellGame/検証/段1 扉だけ → 崩壊都市（RT60 1.5s）")]
        public static void Stage1ToTemple() => Stage1DoorOnly(WorldId.Temple);

        [MenuItem("BellGame/検証/段1 扉だけ → 洞窟（RT60 5.1s）")]
        public static void Stage1ToCave() => Stage1DoorOnly(WorldId.Cave);

        // ★扉の奥に**本物の部屋**を建てる。見えなくてよい。
        //
        //   以前は行き先の残響をクリップへ焼いていたが、それは
        //   **非ベイクが売りのエンジンで、ベイクする**という本末転倒だった。
        //   部屋を建てれば、部屋グラフが実行時に見つけ、Sabine が体積と材質から
        //   RT60 を導き、ポータルが開口として結合する ── 全部エンジンの仕事になる。
        //
        //   寸法は Sabine（RT60 = 0.161V/(αS)）から逆算した:
        //     崩壊都市 12×10×6 / α0.15  → V/S=1.43 → 1.53s（狙い 1.5s）
        //     洞窟     10×8×5  / α0.038 → V/S=1.18 → 4.98s（狙い 5.1s）
        //   **箱の大きさと材質を変えるだけで世界の響きが変わる**。ここが見せ場。
        //
        // ★部屋になるには**天井が要る**（`2e63c46`：壁だけでは足りない）。
        //   だから床・壁・天井を閉じて、開口は扉の枠だけにする。
        private static void Stage1DoorOnly(WorldId dest)
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            // 地面は小さくてよい。格子が太らないほど部屋の割れ方が安定する。
            var ground = Box("Ground", new Vector3(0f, -0.5f, 0f), new Vector3(24f, 1f, 30f));

            // ★プレイヤーは扉の正面 4m に、**扉のほうを向いて**立たせる。
            //   本編では出発点が扉に背を向けていて、後ろ歩きで入ることになっていた。
            var listener = MakeListener(new Vector3(0f, 1.6f, 4f), lookAt: new Vector3(0f, 1.6f, 0f));

            var door = MakeDoor(Vector3.zero, listener);

            // ★扉の奥に、行き先の世界の**部屋**を建てる（見えない）。
            //   残響はここからエンジンが導く。焼かない。
            var room = BuildBeyondRoom(dest);

            // 音源は部屋の**奥のほう**に置く。開口のすぐ手前だと直接音が支配して、
            //   部屋の尾が聞こえない（臨界距離 rc の内側になる）。
            var beyond = MakeSource("Beyond", new Vector3(0f, 1.4f, -(room.depth * 0.6f)), 1);
            SetTestSignal(beyond, door);

            // 呼びかけ用の音源はここでは使わないが、デモが src[0] を要求するので置く。
            var dummy = MakeSource("Src0", listener.position + new Vector3(0f, 0f, 1.5f), 0);
            var follow = dummy.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = new Vector3(0f, 0f, 1.5f);

            var demoGo = new GameObject("AcousticFlowSceneDemo");
            var demo = demoGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = dummy;
            demo.extraSources = new[] { beyond };
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = AcousticMaterialPreset.Default;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;
            demo.useCppDsp = true;
            demo.apertureContrast = 1.5f;   // 4.0 だと半開きが実質無音（実測）
            // ★残響を切らない。扉の奥に部屋を建てたので、**導かせる相手が居る**。
            //   これが「サウンドシステムの利点を使う」ということ。
            demo.enableReverb = true;
            demo.roomSeedRadius = 0.6f;
            demo.roomCellSize = 0.25f;
            demo.roomBlendRadius = 2.0f;

            var wb = new GameObject("WorldBounds").AddComponent<WorldBounds>();
            wb.listener = listener;
            wb.ground = ground.GetComponent<BoxCollider>();

            new GameObject("Hud").AddComponent<BellGameHud>();

            // 扉の周りだけ手前の世界を薄くする（演出。物理ではない）。
            var hush = new GameObject("DoorHush").AddComponent<DoorHush>();
            hush.listener = listener;
            hush.door = door.transform;

            // 屋外側でフォールバックの尾が立たないように押さえる。
            new GameObject("OutdoorTailGate").AddComponent<OutdoorTailGate>();

            // 行き先。開口の向こうがこの世界の部屋になっている。
            door.destination = dest;

            WorldSky.Apply(WorldId.Grassland);
            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
                if (l.type == LightType.Directional) WorldSky.ApplySun(WorldId.Grassland, l);

            Save(scene, "Lab_Door_" + dest + ".unity",
                 $"段1: 扉だけ → {BellVoices.DisplayName(dest)}。5/6 で開閉、左ドラッグで掴む。"
                 + "扉の奥に見えない部屋があり、RT60 はエンジンが実行時に導く。"
                 + "角度を変えながら、通る帯域と尾の乗り方が連続に変わるかを聴く。");
        }

        // ── 音の検証：扉の奥の世界 ──────────────────────────────
        //
        // ★確かめること（発注者の指定 ①②③）
        //   ① 繋がっていない扉は**ただの扉**。奥から何も鳴らない
        //   ② 繋いだ瞬間、扉の奥に**その世界の音**が出現する（行き先を変えれば音も変わる）
        //   ③ 扉を開くほど取り込まれる（§4.3 のフレネル帯域積分だけがやっている）
        //
        // ★**音だけ**の場所。ポータル描画も世界の入れ替えも入れない。
        //   絵と 25ms が混ざると「いま変わったのは扉のせいか」が判断できなくなる。
        //   含まれない物は本当に含まれていないので、変化があれば扉が原因だと言い切れる。
        //
        // ★奥の部屋は 1 つだけ（崩壊都市の寸法・RT60≒1.5s）。
        //   世界ごとの残響の違いは**段1**が受け持つ（崩壊都市 1.5s と洞窟 5.1s の対比）。
        //   ここで両方やろうとすると、聞いているのが声なのか残響なのか分からなくなる。
        [MenuItem("BellGame/検証/音 扉の奥の世界（①②③）")]
        public static void AudioBeyondDoor()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            var ground = Box("Ground", new Vector3(0f, -0.5f, 0f), new Vector3(24f, 1f, 30f));

            // 扉の正面 4m に、扉のほうを向いて立たせる。
            var listener = MakeListener(new Vector3(0f, 1.6f, 4f), lookAt: new Vector3(0f, 1.6f, 0f));
            var door = MakeDoor(Vector3.zero, listener);

            // 扉の奥の部屋。見えなくてよい ── 閉じた扉の奥は見えない。
            //   残響はここからエンジンが実行時に導く。焼かない。
            var room = BuildBeyondRoom(WorldId.Temple);

            // ★代役を退場させる。奥に本物の部屋を建てたので、MakeDoor の見えない箱
            //   （α≈0.99）が入れ子で残ると、開口の内側が死んだ殻で覆われる。
            //   音源（z=-6.0）は箱（z≥-4.0）の**外**なので、殻ごしにしか届かなくなっていた。
            RemoveAperturePanel(door, "奥に本物の部屋を建てたので代役は要らない");

            // 音源は部屋の**奥のほう**。開口のすぐ手前だと直接音が支配して部屋の尾が聞こえない。
            var beyond = MakeSource("Beyond", new Vector3(0f, 1.4f, -(room.depth * 0.6f)), 1);
            SetTestSignal(beyond, door);          // 既定は世界の声。F5 で広帯域へ
            beyond.SetParent(door.transform, true);   // ★扉の持ち物。扉の下に置く

            // デモが src[0] を要求するので置くだけ。鳴らさない。
            var dummy = MakeSource("Src0", listener.position + new Vector3(0f, 0f, 1.5f), 0);
            var follow = dummy.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = new Vector3(0f, 0f, 1.5f);

            var demoGo = new GameObject("AcousticFlowSceneDemo");
            var demo = demoGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = dummy;
            demo.extraSources = new[] { beyond };
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = AcousticMaterialPreset.Default;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;
            demo.useCppDsp = true;
            demo.apertureContrast = 1.5f;   // 4.0 だと半開きが実質無音（実測）
            demo.transmissionGainDb = 0f;   // 既定。透過は「壁の向こう感」、方向は回折の担当
            demo.enableReverb = true;       // 奥に部屋を建てたので、導かせる相手が居る
            demo.roomSeedRadius = 0.6f;
            demo.roomCellSize = 0.25f;
            demo.roomBlendRadius = 2.0f;

            var wb = new GameObject("WorldBounds").AddComponent<WorldBounds>();
            wb.listener = listener;
            wb.ground = ground.GetComponent<BoxCollider>();

            // 扉の周りだけ手前の世界を薄くする（演出。物理ではない）。
            var hush = new GameObject("DoorHush").AddComponent<DoorHush>();
            hush.listener = listener;
            hush.door = door.transform;

            // 屋外側でフォールバックの尾が立たないように押さえる。
            new GameObject("OutdoorTailGate").AddComponent<OutdoorTailGate>();

            // 操作台。Tab で行き先を一巡（**なし**から始まる）。
            var labGo = new GameObject("DoorAudioLab");
            var lab = labGo.AddComponent<DoorAudioLab>();
            lab.worldDoor = door;
            lab.beyond = beyond.GetComponent<BeyondAmbience>();

            WorldSky.Apply(WorldId.Grassland);
            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
                if (l.type == LightType.Directional) WorldSky.ApplySun(WorldId.Grassland, l);

            Save(scene, "Lab_Audio.unity",
                 "音の検証: ①なし＝無音 ②繋ぐと向こうの世界の音 ③開くほど取り込む。"
               + "Tab で行き先を一巡（なしから始まる）／5・6 で開閉／F5 で信号を切替。"
               + "ポータル描画も世界の入れ替えも入っていない ── 変化があれば扉が原因。");
        }

        // ── 構造チェック：奥半分を丸ごと入れ替える ───────────────────
        //
        // ★確かめたいこと（発注者の指定）
        //   扉の奥半分に**実体のある世界を半分だけ**置いて、Tab で丸ごと差し替える。
        //     A世界 … 赤。扉に向かって**奥の右**にロクデナシ
        //     B世界 … 緑。扉に向かって**奥の左**に TokyoGeto
        //     なし  … 奥に世界が**存在しない**（＝ただの扉）
        //   世界のローテーション（草原・崩壊都市…）はこの場では外してある。
        //
        // ★A と B は**形をそろえてある**。
        //   違うのは色・音源の位置・曲の 3 つだけ。寸法や材質まで変えると、
        //   聞こえ方が変わったときに「入れ替わったから」なのか「部屋が違うから」なのか
        //   分からなくなる。ここは**入れ替えが成立するか**を見る場所なので、
        //   変える物は少ないほどいい。響きの違いを足すのは、成立を確かめた後。
        //
        // ★色は**世界の内側**に塗ってある（扉の周りの面は素の灰色）。
        //   閉じた扉の手前で色が分かってしまうと、「扉ごしに向こうを知る」という
        //   この作品の芯そのものが確かめられなくなる。奥を知る手段は扉だけにしておく。
        //
        // ★含めていない物: ポータル描画 / ベル / DoorHush（演出）。
        //   変化があれば、原因は入れ替えか扉のどちらかだと言い切れる状態にしてある。
        [MenuItem("BellGame/検証中/奥半分を丸ごと入れ替える（A赤 / B緑 / なし）")]
        public static void TrialHalfWorlds()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            // 手前側の地面。奥（z<0）は世界のほうが床を持っているので敷かない。
            var ground = Box("Ground", new Vector3(0f, -0.5f, 7.5f), new Vector3(24f, 1f, 15f));

            // 戸口の手前 3m。★奥の音源（-3m）と対称にしてある ── 扉の開き角シーンと同じ形。
            var listener = MakeListener(new Vector3(0f, 1.6f, 3f), lookAt: new Vector3(0f, 1.6f, 0f));
            var door = MakeDoor(Vector3.zero, listener);

            // ★手前には**何も建てません。**
            //
            //   一度ここに本物の部屋（8×8×4.2m）を建てましたが、取り消しました。
            //   本番でプレイヤーが立つのは草原です ── **野原の真ん中に部屋は建てられません。**
            //   囲いは「透明で・体は素通りで・扉の面より奥だけ」に置く
            //   DoorAperturePanel が持ちます（発注者の指示）。
            //   そうすれば**いま居る世界の音には一切触らず**、繋いだ世界の音にだけ効きます。

            // ★扉を見ている人から見て、奥は -Z、右は -X。
            //   （-Z を向き上が +Y のとき、右手側は -X になる）
            //   主張ではなく座標で置く。HalfWorldLab が実行時に読み上げて突き合わせる。
            //
            // ★扉は**右吊り**（蝶番が右・隙間が左）で固定。発注者の指示。
            //   PhysicsDoor は板のローカル -X を蝶番にするので、world x=-0.55 が蝶番。
            //   -X はプレイヤーから見て右 ＝ 右蝶番・左開口。いまの MakeDoor のままで合う。
            //   HalfWorldLab が起動時に**検査**して、崩れていれば黙らずに言う。

            // ── A世界（赤）：環境音で試す。左右に別の音を置いて、
            //    閉じた扉が**隙間の側（左）**へ寄せてしまうかを聞き分ける。
            //      鳥  … 向かって右（-X）＝ **蝶番の側**。閉じている間は左へ引かれるはず
            //      川  … 向かって左（+X）＝ **隙間の側**。閉じていても正しい側に居るはず
            //    同時に鳴らすので、片方だけが引かれるなら**その場で分かる**。
            // ★置き方は**扉の開き角シーンに合わせてあります**（発注者の指示）。
            //
            //   あちらの注記がそのまま条件です:
            //     「戸口が幅1mなので、直線が戸口を通るのは音源の X が概ね ±1m 以内のとき。
            //       それを超えると壁側で遮られるため、左右とも ±0.8m に置く」
            //
            //   ここは戸口が 1.1m なので ±0.85m。**前は ±3.5m ＝ 4 倍外**に置いていて、
            //   扉を全開にしても壁の陰のままでした（角度を掃いても 125Hz:4kHz の比が
            //   4〜5 倍から動かなかったのがその証拠）。§4.3 ではなく置き場所の問題でした。
            //
            // ★左右に分けるのは**聞こえ始める順序**を作るため。
            //     +X（プレイヤーから見て左）… 自由端の側。隙間が開く方向。**先に抜けてくる**
            //     -X（同 右）              … 蝶番の側。振れた板が覆う。**最後まで透過のまま**
            //   開閉の 2 状態ではなく、途中の角度すべてで連続に変わることを聴く場です。
            var halfA = BuildHalfRoom("Half_A", new Color(0.72f, 0.18f, 0.16f));
            var birdA = Ambient(halfA, "Bird_Hinge", AmbientKind.Bird, -0.85f, 1, seed: 21);
            // ★左は**滝**にした（川から差し替え。発注者の「音色が変わりすぎる」を受けて）。
            //   川（Stream）は一次ハイパスで低域を捨てているので高域しか無く、
            //   こもらせると鈍るのではなく消える ＝ 別の音になる。
            //   滝（Falls）は低・中・高すべてに芯があるので、
            //   閉じていても「同じ音がくぐもっている」と分かり、開けると晴れる。
            //   F4 で 滝→川→風 と切り替えて聴き比べられる。
            var streamA = Ambient(halfA, "Falls_Gap", AmbientKind.Falls, 0.85f, 2, seed: 22);
            // 定常音なので、単発の鳥と同じ音量だと場を占領する（発注者の指示で下げる）。
            streamA.volume = 0.5f;

            // ★見通し線の**内側**に 1 本。左右の 2 本と同じ表で比べるため。
            //
            //   リスナー z=+4 から幅 1.1m の戸口ごしに見えるのは、奥行き 6.5m で ±1.44m。
            //   左右の音源（x=±3.5）は**その外**なので、扉を全開にしても壁の陰のままです。
            //   実測でも 125Hz:4kHz の比が 25°〜140° で 4〜5 倍のまま動きませんでした
            //  （＝「開くほど高域まで通る」になっていない）。
            //
            //   正面の 1 本は開けるにつれて**見通せる**ので、こちらだけ比が 1 に近づくはず。
            //   同じ表に並ぶので、置き場所の差だとひと目で分かります。
            var centerA = Ambient(halfA, "Falls_Center", AmbientKind.Falls, 0f, 3, seed: 23);
            centerA.volume = 0.5f;

            // ── B世界（緑）：楽曲のまま残す。
            //    環境音は単発と定常が混ざるので、「入れ替わった」だけを確かめたいときの
            //    基準として、続きの分かる音が 1 つ要る。
            var clipB = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (clipB == null)
                Debug.LogWarning("[BellGame] TokyoGeto.wav が見つからない。B世界が無音になる。");

            // ★負荷の**傾き**を測るための音源。音は出ないが費用は本物。
            //
            //   本番（12 本）は重すぎて回せないので、軽いこの場で
            //   「1 本増やすと何 ms 増えるか」を測ります。傾きが分かれば
            //   本番で何本持つかは計算で出ます ── 重い場面で当てずっぽうに削るより速い。
            //
            //   ★置き場所は**囲いの内側**（半径 12m）。外に出すと扉ごしに聞こえない音源になり、
            //     測っているものが変わります。
            //   ★扉を閉じれば遮蔽され、開ければ見通せます。**同じ場で両方の傾きが採れます**
            //     ── 実測では遮蔽の有無で 1 本あたり 10 倍違いました。
            // ★世界に載せるのは**この 3 本だけ**。
            //
            //   負荷用の 10 本を混ぜていたので、音源一覧も角度掃きの表も 13 行になって
            //   **順序を読み取れなくなっていました**。測るための音が、聴くための場を潰していた。
            //   負荷用は下で作りますが世界には載せず、LoadTestRig が必要なときに足します。
            var loadA = new System.Collections.Generic.List<HalfWorldLab.LabSource> { birdA, streamA, centerA };
            float lz = -(HD * 0.5f);
            for (int i = 0; i < kLoadSources; i++)
            {
                float a = Mathf.PI * 2f * i / kLoadSources;
                var s = MakeSource($"Load_{i:00}",
                    new Vector3(Mathf.Cos(a) * 5f, 1.4f, lz + Mathf.Sin(a) * 3.5f), 4 + i);
                s.SetParent(halfA.transform, true);
                // ★worldA には入れません。LoadTestRig が名前（Load_*）で拾って、
                //   測るときだけ装置へ足します。既定は 0 本 ＝ 聴く場は 3 本のまま。
            }

            var halfB = BuildHalfRoom("Half_B", new Color(0.18f, 0.62f, 0.24f));
            var musicB = AddSourceTo(halfB, "Music_Gap", 0.85f, 1);
            var ab = musicB.GetComponent<AudioSource>();
            ab.clip = clipB; ab.loop = true; ab.playOnAwake = false;

            // ★見えない囲いを**残します**（前は外していました）。
            //
            //   奥の世界から壁と天井を外したので、開口を成立させるのはこれだけです。
            //   半径 12m ＝「扉の周りどこまでを向こうの世界とみなすか」。
            //   音源は扉から 6.5m なので、**きちんと内側に入ります**
            //  （外に出ると、囲いが音源とリスナーのあいだの殻になって前の不具合が再発します）。
            var panel = door.GetComponentInParent<Transform>().root
                            .GetComponentInChildren<DoorAperturePanel>(true);
            if (panel != null)
            {
                panel.radius = 12f;
                panel.height = 8f;
                panel.apertureWidth = W;
                panel.apertureHeight = H;
            }

            // ★壁は一切通さない。扉の板だけが通す（発注者の指定）。
            //   奥の世界には床と柱しか無いので、実質は柱と地面の設定です。
            SealWalls(halfA, door);
            SealWalls(halfB, door);

            // 保存時点では両方眠らせておく ── シーンを開いた直後は「なし」から。
            halfA.SetActive(false);
            halfB.SetActive(false);

            var dummy = MakeSource("Src0", listener.position + new Vector3(0f, 0f, 1.5f), 0);
            var follow = dummy.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = new Vector3(0f, 0f, 1.5f);

            var demoGo = new GameObject("AcousticFlowSceneDemo");
            var demo = demoGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = dummy;
            demo.extraSources = System.Array.Empty<Transform>();
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = AcousticMaterialPreset.Default;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;
            demo.useCppDsp = true;
            demo.apertureContrast = 1.5f;

            // ★透過は**既定（0dB）に戻してある**。
            //
            //   一度 -12dB まで下げた（「扉の透過音を下げて」の指示）が、あれは
            //   死んだ殻がまだ入っていた頃の判断で、前提が変わった。いまの役割分担は:
            //
            //     透過（板を抜ける）… 向きは**音源のほう**。役目は「壁の向こう感」
            //     回折（隙間を回る）… 向きは**隙間のほう**。役目は「どこから聞こえるか」
            //
            //   透過を絞ると「奥に何か居る」ごと薄くなる。方向の話は回折の担当なので、
            //   方向が気に入らないときに透過をいじるのは筋が違う。
            //   どちらを厚くするかは物理では決まらない（決めごと #3）ので、
            //   実行中に [ ] と , . で振れるようにしてある。**耳で決めてください。**
            //
            // ★立場を反転させました（発注者の指示・2026-08-21）。
            //
            //   前: 「**向こう**が部屋で、その音がこちらへ漏れてくる」
            //   今: 「**こちら**が部屋で、外の音が開口から入ってくる」
            //
            //   §4.3 はまさに後者のために書かれています
            //  （「戸口 … 格子の大半が壁で、開いたセルだけが隙間」）。
            //   立場が変わると、担当も入れ替わります:
            //
            //     壁    … 通さない。部屋の中に居るのだから、壁は塞いでいるのが正しい
            //     開口  … **唯一の入口**。ここが音の量も向きも決める
            //     残響  … **こちらの部屋**のもの。入ってきた音がこの部屋を鳴らす
            //
            // ★透過を下げます（+12 → -6dB）。
            //   壁から入ってくると「部屋に居る」感じが消えます。
            //   壁越しに残るぶんは、こもりが強いままなので「向こうに何か在る」だけ伝わります。
            // ★全体のつまみは中立へ戻します。壁と扉の差は**材質側**で付けたので
            //   （SealWalls）、ここで一律に動かすと、せっかく付けた差まで一緒に動きます。
            demo.transmissionGainDb = 0f;

            // ★壁の向こう感。**音量ではなく音色**で作る。
            //
            //   「こもって聞こえないから壁の奥で鳴っているように聞こえない」への答えが
            //   この 2 つ。透過音の高域を落とすと「隔てられている」が出る。
            //     transmissionTilt      … 材質の 6 帯域カーブの**形はそのまま**に傾きだけ強める
            //     transmissionHighCutDb … 材質に依らず一律に、125Hz 0dB → 4kHz でこの値ぶん落とす
            //
            //   ★ここが**③の主役**になる。
            //     扉を開けると開口から高域が入ってくるので、こもりが晴れていく。
            //     音量ではなく**音色**で「開いた」が分かる ── 落差を縮めても③が消えない。
            //   ⚠ 回折タップは平坦のままなので、こもりの担当は透過に集まる（エンジンの設計）。
            demo.transmissionTilt = 1.8f;
            demo.transmissionHighCutDb = 9f;

            // ★B1（いちばん強い回折タップを HRTF に載せる）を**入れます**。
            //
            //   前は切っていました。「定位を扉で動かしたくない」からで、
            //   B1 は遮蔽中の定位を**回り込んだ場所**へ持っていくためです。
            //
            //   立場を反転させたので、判断も反転します ──
            //   **部屋の中に居て外の音を聞いているなら、音は開口から来るのが正しい。**
            //   B1 はその開口に ITD と前後の手がかりを載せる機能なので、
            //   いまはむしろ主役です。F2 で切って比べられます。
            demo.diffractionHrtf = true;

            // ★定位のステアを切る。**これが「定位が逆」の回避策であり、指定の実現でもある。**
            //
            //   遮蔽されると AcousticFlowSceneDemo は定位を「見かけの方向」へ寄せる:
            //       target = Slerp(trueDir, engDir, steer)            （:1562 付近）
            //       trueDir = (音源 - リスナー)          ← 正しい方位
            //       engDir  = AF_SceneGetSourceArrivalDir ← エンジンが返す到来方向
            //   扉を閉じると steer≈1 になり、定位は engDir だけで決まる。
            //
            //   実測（Lab_Half・扉 0°・物差しは listener/camera で完全に一致）:
            //       Bird_Right  幾何=右  →  DirectDirLocal.x = -0.35
            //       Stream_Left 幾何=左  →  DirectDirLocal.x = +0.35
            //   大きさが同じで符号だけ逆 ＝ **engDir が方位の反転になっている。**
            //   開けた瞬間に正しい位置へ飛ぶのは、steer が 0 に落ちて trueDir に戻るから。
            //
            //   ★切ると steer=0 固定 ＝ 常に trueDir ＝ **定位は扉で動かず、常に音源を向く。**
            //     発注者の指定（TestSwing の見え方）そのものなので、回避策と狙いが一致する。
            //
            // ⚠ ただしこれは**症状を避けているだけ**で、engDir の反転は残っています。
            //   エンジン側の話なのでこちらでは直しません（連絡板でサウンドレーンへ報告済み）。
            // ⚠ また「音を頼りに探す」場面では、遮蔽時に開口のほうを向いてほしいはずなので、
            //   そちらは反転が直ってからステアを戻して考え直す必要があります。
            demo.useDirectionalSteering = false;

            // ★回折は 0dB へ（既定は +6dB）。
            //
            //   回折タップのパンは**到来点＝開口**を向いています。
            //   立場を反転させたので、これは**直すべき癖ではなく、欲しい振る舞い**になりました。
            //   部屋の中に居て外の音を聞いているのだから、音は開口から来るのが正しい。
            //
            // ★開口を主役にするので +3dB（既定 +6 の半分）。
            //   上げすぎると遮蔽側が見通し側より明るくなって、逆に段差が増えます
            //  （エンジンの実測表: +18dB で振れ幅が 10.1dB に増えている）。
            demo.diffractionGainDb = 3f;

            demo.enableReverb = true;
            demo.roomSeedRadius = 0.6f;
            demo.roomCellSize = 0.25f;
            demo.roomBlendRadius = 2.0f;

            var wb = new GameObject("WorldBounds").AddComponent<WorldBounds>();
            wb.listener = listener;
            wb.ground = ground.GetComponent<BoxCollider>();

            new GameObject("OutdoorTailGate").AddComponent<OutdoorTailGate>();

            // 60fps に張り付かせて、**まだ何 ms 余っているか**を出す。
            //   本番で落ちるかどうかは「いま何 fps か」ではなく「山でいくら足りないか」で決まる。
            new GameObject("PerfMeter").AddComponent<PerfMeter>().targetFps = 45;
            new GameObject("SourceTierApplier").AddComponent<SourceTierApplier>();

            // 負荷の傾きを測る操作台。マイナス/イコールで音源を 1 本ずつ増減できる。
            new GameObject("LoadTestRig").AddComponent<LoadTestRig>();

            var labGo = new GameObject("HalfWorldLab");
            var lab = labGo.AddComponent<HalfWorldLab>();
            lab.demo = demo;
            lab.worldDoor = door;
            lab.listener = listener;
            lab.halfA = halfA; lab.halfB = halfB;
            lab.worldA = loadA.ToArray();
            lab.worldB = new[] { new HalfWorldLab.LabSource { t = musicB, ambient = false } };
            lab.src0 = dummy;

            WorldSky.Apply(WorldId.Grassland);
            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
                if (l.type == LightType.Directional) WorldSky.ApplySun(WorldId.Grassland, l);

            Save(scene, "Lab_Half.unity",
                 "構造チェック: 扉の奥半分を丸ごと入れ替える。Tab で なし → A世界（赤・鳥が右／"
               + "滝が左） → B世界（緑・楽曲が左）。5・6 で扉の開閉、歩いて抜けると転移（閉まって"
               + "接続が切れる）。扉は右吊り・140°まで開く。定位は扉で動かさない設定（ステア OFF）。"
               + "数字は BellGame/モニター（別ウィンドウ）へ。画面は一行だけ（\\ で増減）。"
               + "「なし」は消音ではなく**音源が存在しない**状態。");
        }

        // 奥半分の世界を 1 つ建てる。
        //
        // ★BuildBeyondRoom（音だけの部屋）と違い、こちらは**見える**。
        //   発注者の指定が「実際の世界を半分だけ置く」なので、Renderer を残す。
        //   ただし色を塗るのは内側だけ ── 扉の周りの面は素のまま。
        //
        // ★天井が要る（§4.4 は天井の無い空間を部屋と認めない）。
        //   囲うと当然暗いので、中に点光源を 1 つ置いて色が読めるようにしてある。
        //   ベルの在り処を教える灯りは置かない決まりだが、ここにベルは居ない。
        private const float HW = 12f, HD = 10f, HH = 6f;   // 奥半分の部屋の内寸

        // ★手前側の部屋。**プレイヤーはこの中に立つ。**
        //
        //   立場を反転させた（こちらが部屋・外の音が開口から入ってくる）ので、
        //   手前が野原のままだと新しい枠組みを試せません。
        //   §4.3 が本領を出すのは「部屋の中に居て、開口ごしに外を聞く」形なので、
        //   囲いが要ります。
        //
        //   内寸は**白い部屋と同じ 8×8×4.2m**（連絡板 M11）。
        //   ここで出た数字がそのまま本番へ移せるように、寸法を合わせてあります。
        private const float NW = 8f, ND = 8f, NH = 4.2f;

        private static GameObject BuildNearRoom(Color tint)
        {
            const float t = 0.4f;
            float cz = ND * 0.5f;                 // 扉(z=0)の手前側
            var root = new GameObject("NearRoom");

            void Wall(string n, Vector3 c, Vector3 s)
            {
                var b = Box(n, c, s);
                Tint(b, tint);
                b.gameObject.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.Default;
                b.SetParent(root.transform, true);
            }

            Wall("Floor", new Vector3(0f, -t * 0.5f, cz), new Vector3(NW, t, ND));
            Wall("Ceil", new Vector3(0f, NH + t * 0.5f, cz), new Vector3(NW, t, ND));  // ★天井が要る
            Wall("Back", new Vector3(0f, NH * 0.5f, cz + ND * 0.5f), new Vector3(NW, NH, t));
            Wall("Left", new Vector3(-NW * 0.5f, NH * 0.5f, cz), new Vector3(t, NH, ND));
            Wall("Right", new Vector3(NW * 0.5f, NH * 0.5f, cz), new Vector3(t, NH, ND));

            // 扉の面。開口のぶんだけ穴を空ける ── **この部屋の唯一の出口**。
            float side = (NW - W) * 0.5f;
            Wall("FrontL", new Vector3(-(W * 0.5f + side * 0.5f), NH * 0.5f, 0f),
                 new Vector3(side, NH, t));
            Wall("FrontR", new Vector3(W * 0.5f + side * 0.5f, NH * 0.5f, 0f),
                 new Vector3(side, NH, t));
            Wall("FrontTop", new Vector3(0f, H + (NH - H) * 0.5f, 0f),
                 new Vector3(W, NH - H, t));

            var lamp = new GameObject("Lamp").AddComponent<Light>();
            lamp.type = LightType.Point;
            lamp.transform.position = new Vector3(0f, NH * 0.75f, cz);
            lamp.range = ND * 1.5f;
            lamp.intensity = 1.3f;
            lamp.transform.SetParent(root.transform, true);

            Debug.Log($"[BellGame] 手前に部屋を建てた: 内寸 {NW}×{ND}×{NH}m"
                      + $"（白い部屋と同じ）。開口は扉だけ。", root);
            return root;
        }

        private static GameObject BuildHalfRoom(string name, Color tint)
        {
            const float t = 0.4f;
            float w = HW, d = HD, h = HH;
            float cz = -(d * 0.5f);

            var root = new GameObject(name);

            void Wall(string n, Vector3 c, Vector3 s, bool inside)
            {
                var b = Box(n, c, s);
                if (inside) Tint(b, tint);                       // 世界の色は内側だけ
                b.gameObject.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.Default;
                b.SetParent(root.transform, true);
            }

            // ★壁も天井も建てません。**囲いは DoorAperturePanel（透明・体は素通り）が持ちます。**
            //
            //   本番の向こう側は草原です。**野原の真ん中に本物の部屋は建てられません。**
            //   だから「向こうの世界」は地面と音源だけにして、開口を成立させる囲いは
            //   見えない箱に任せる ── そのほうが本番と同じ形になります。
            //
            //   ここで本物の壁を建てると、見えない箱と**二重の殻**になって、
            //   開けても小さくなる・くぐっても小さくなる、が再発します。
            Wall("Floor", new Vector3(0f, -t * 0.5f, cz), new Vector3(w, t, d), true);

            // 色と奥行きの手がかりだけ、低い柱で置く（音はほとんど遮りません）。
            for (int i = 0; i < 4; i++)
            {
                float a = Mathf.PI * (0.25f + 0.5f * i);
                var p = new Vector3(Mathf.Cos(a) * w * 0.32f, 1.1f, cz + Mathf.Sin(a) * d * 0.32f);
                var m = Box($"Mark_{i}", p, new Vector3(0.5f, 2.2f, 0.5f));
                Tint(m, tint * 1.4f);
                m.SetParent(root.transform, true);
            }

            var lamp = new GameObject("Lamp").AddComponent<Light>();
            lamp.type = LightType.Point;
            lamp.transform.position = new Vector3(0f, h * 0.7f, cz);
            lamp.range = d * 1.4f;
            lamp.intensity = 1.6f;
            lamp.color = Color.Lerp(tint, Color.white, 0.55f);
            lamp.transform.SetParent(root.transform, true);

            return root;
        }

        // 奥半分に音源を 1 本足す。
        //
        // ★x は**世界座標**。扉を見ている人から見て右が -X、左が +X。
        //   （-Z を向き上が +Y のとき右手側は -X。ここを間違うと左右が入れ替わる）
        //
        // ★奥行きは開口のすぐ手前ではなく**奥のほう**に置く。
        //   手前だと直接音が支配して、部屋の尾も左右の差も聞き取れない。
        /// 扉から奥へ 3m。**扉の開き角シーンと同じ距離**にしてある。
        ///   遠くへ置くと、開口を通る直線の許容幅が狭まって（相似）、
        ///   どの音源も見通せなくなります。近くに置くのが条件です。
        private const float SrcZ = -3f;

        private static Transform AddSourceTo(GameObject half, string name, float x, int index)
        {
            var s = MakeSource(name, new Vector3(x, 1.6f, SrcZ), index);
            s.SetParent(half.transform, true);
            return s;
        }

        /// 合成の環境音を鳴らす音源の**指定**を作る。
        ///
        /// ★AmbientSource（部品）は付けない。
        ///   付けると IrConvolver.Awake（クリップが空なら ir_silence を入れる）と
        ///   Awake の順序を争って負けることがあり、実際に川が一生無音になった。
        ///   さらにその部品自体がシーン上で参照を失うこともある。
        ///   **クリップの持ち主は HalfWorldLab 1 人**にして、争いごと自体を無くす。
        private static HalfWorldLab.LabSource Ambient(GameObject half, string name, AmbientKind kind,
                                                      float x, int index, int seed)
        {
            var s = AddSourceTo(half, name, x, index);

            // ★合成の環境音は楽曲より**ずっと小さい**（Stream は内部で 0.55 倍、
            //   さらにハイパスで振幅が落ちている）。ここは目一杯にしておく。
            return new HalfWorldLab.LabSource
            {
                t = s, ambient = true, kind = kind, seed = seed, volume = 1.0f,
                // 鳥は既定 2〜7 秒間隔。検証では「鳴っていないのか間が空いているのか」が
                //   区別できなくなるので、この場だけ詰める。
                minInterval = 1.0f, maxInterval = 3.0f,
            };
        }

        // ── 段 2：扉＋2 つの世界 ────────────────────────────────
        // 確かめること: **くぐると世界が入れ替わる**（描画なし）
        // 含まないもの: ポータル描画 / ベル
        [MenuItem("BellGame/検証/段2 扉＋2つの世界（くぐる）")]
        public static void Stage2TwoWorlds()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var a = BuildMiniWorld(WorldId.Grassland, Vector3.zero, new Color(0.40f, 0.55f, 0.30f));
            var b = BuildMiniWorld(WorldId.Temple, new Vector3(400f, 0f, 0f), new Color(0.45f, 0.44f, 0.42f));

            // 互いを行き先にしておく。往復できるかをそのまま試せる。
            a.door.destination = WorldId.Temple;
            b.door.destination = WorldId.Grassland;

            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = 400f;
            set.startWorld = WorldId.Grassland;

            new GameObject("Hud").AddComponent<BellGameHud>();

            WorldSky.Apply(WorldId.Grassland);

            Save(scene, "Lab_Cross.unity",
                 "段2: 世界 2 つ。5 で開けて扉の正面から歩けば入れ替わる。振り返れば戻れる。"
                 + "ポータル描画は入れていない（見えないのが正しい）。");
        }

        // ── 段 3：扉に向こうの景色を映す ────────────────────────
        // 確かめること: **開口に向こうの世界が見えるか**
        //
        // ★段 2 と同じ世界の作りに、ポータル描画を足しただけ。
        //   移動が自然に効いているところへ、描画だけを重ねる。
        //   壊れたらポータル描画しか疑うところが無い。
        //
        // ★まず板が出るかを確かめる（debugSolidColor）。
        //   前回は「テクスチャが空」と「板が出ていない」を同時に疑っていて、
        //   値が全部正しいのに描かれない、で止まった。切り分けから入る。
        [MenuItem("BellGame/検証/段3-A 板が出るか（べた塗り）")]
        public static void Stage3Solid() => Stage3Portal(solid: true);

        [MenuItem("BellGame/検証/段3-B 向こうの景色を映す")]
        public static void Stage3Real() => Stage3Portal(solid: false);

        private static void Stage3Portal(bool solid)
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            int previewLayer = LayerMask.NameToLayer(BellGamePortalSetup.PreviewLayerName);
            if (previewLayer <= 0)
            {
                Debug.LogError("[BellGame] レイヤー '" + BellGamePortalSetup.PreviewLayerName
                               + "' が無い。先に BellGame/セットアップ/1 を実行してほしい。");
                return;
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var a = BuildMiniWorld(WorldId.Grassland, Vector3.zero, new Color(0.40f, 0.55f, 0.30f));
            var b = BuildMiniWorld(WorldId.Temple, new Vector3(400f, 0f, 0f), new Color(0.75f, 0.35f, 0.25f));
            a.door.destination = WorldId.Temple;
            b.door.destination = WorldId.Grassland;

            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = 400f;
            set.startWorld = WorldId.Grassland;

            // ★ポータル描画は**両方の世界に**付ける。
            //   向こうへ移った後、振り返って戻る側でも見えないと片道になる。
            AddPortalView(a, set, solid);
            AddPortalView(b, set, solid);

            new GameObject("Hud").AddComponent<BellGameHud>();
            WorldSky.Apply(WorldId.Grassland);

            Save(scene, solid ? "Lab_Portal_Solid.unity" : "Lab_Portal.unity",
                 solid
                 ? "段3-A: 板をべた塗り。開口にマゼンタが見えれば板は出ている（原因はテクスチャ側）。"
                 : "段3-B: 開口に向こうの世界を映す。左上の小窓はポータルカメラの生の絵。");
        }

        // 切り抜き方式のポータル。板を貼らず、開口をステンシルで抜いて中にだけ描く。
        private static void AddPortalStencil(Mini w, WorldSet set)
        {
            var go = new GameObject("WorldPortalStencil");
            go.transform.SetPositionAndRotation(w.door.transform.position, w.door.transform.rotation);
            var view = go.AddComponent<WorldPortalStencil>();
            view.worldDoor = w.door;
            view.worlds = set;
            view.drawDistance = 40f;
            view.debugShowRawTexture = true;      // 検証中なので数字は出したまま
            go.transform.SetParent(w.root.transform, true);
        }

        // ★旧 WorldPortalView は削除しました。ステンシル版に一本化（決めごと #1）。
        //   段3-A（べた塗り）は、ステンシル版の debugSolidMask が同じ役をします
        //   ── 切り抜きの形だけを見たいときに、中身を単色で塗る debug 表示です。
        private static void AddPortalView(Mini w, WorldSet set, bool solid)
        {
            AddPortalStencil(w, set);
            var view = w.root.GetComponentInChildren<WorldPortalStencil>(true);
            if (view != null) view.debugSolidMask = solid;
        }

        // ── 中身 ──────────────────────────────────────────────

        // ── 段4：色だけ変わるか ─────────────────────────────────
        //
        // ★継ぎ目を見つけるための検査。発注者の指定：
        //     「扉を真ん中において、景色の中で色だけ変わるようにして」
        //
        //   行き先の世界は扉のところで**180 度回して**重ねる。
        //   だから、扉のまわりが 180 度回転対称な世界を 2 つ作り、
        //   **色以外をまったく同じ**にすれば、幾何は完全に一致する。
        //
        //   → 跨いだとき、正しければ**色が変わるだけ**で何も動かない。
        //     ずれ・回り込み・裏返り・置き直しは、動いた瞬間に見える。
        //     ここまでの不具合は全部「何かが動いていた」ので、この検査で捕まる。
        //
        //   ★扉は世界の**真ん中**に立つ。中身が扉の両側にあるので、
        //     「中身は扉の片側に寄せる」という普段の制約をわざと破っている。
        //     対称なら重なっても一致するので、この検査に限っては成立する。
        // ── 検証中：開口の印が near 平面に潰れる件 ─────────────────
        //
        // ★A 戸口方式 / B 厚み方式は**どちらも廃止**した。
        //
        //   実測: 扉ローカル z=0.05m ／ near=0.05m のとき、幾何どおりなら印は
        //   横 −76°〜+87°（画角 ±45.7°）＝画面全体を覆うはずが、画面の 3 割だった。
        //   矩形がカメラの目の前にあり、横を向いているぶんの端しか near を越えないため。
        //
        //   A（戸口の中では全画面）はその穴を埋めたが、跨いだ後も全画面が続き
        //   **出てきたばかりの世界が全画面に出た**。範囲を詰めても段は残る。
        //   B（厚みのある箱）は潰れないが、箱の中で遮蔽が緩む。
        //
        //   ★根は「**矩形を描いて印にする**」こと。目が矩形に入ると必ず破れる。
        //     いまは線分と矩形の交差を直に見ているので、その前提ごと消えている。
        //     場合分けも閾値も無く、真横に立てば画面が扉の面で割れる。
        [MenuItem("BellGame/検証中/扉の切り抜き（線分と矩形の交差）")]
        public static void TrialRayMask() => Stage4Tint("Trial_RayMask.unity",
            "検証中: 印は『目からその画素へ引いた線分が扉の矩形を横切るか』だけで決まる。"
          + "場合分けも閾値も無い。見るところ: 歩いて跨ぐ間に段が出ないか／"
          + "面に重なって真横を向くと画面が扉の面で割れるか／"
          + "扉板を奥へ開いたとき板が塗り潰されないか。");

        [MenuItem("BellGame/検証/段4 色だけ変わるか（扉は真ん中）")]
        public static void Stage4TintDefault()
            => Stage4Tint("Lab_Tint.unity",
                 "段4: 扉は真ん中。二つの世界は**色以外まったく同じ**（180 度回転対称）。"
               + "正しければ跨いでも色が変わるだけで何も動かない。動いたらそこが継ぎ目。");

        private static void Stage4Tint(string file, string note)
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            int previewLayer = LayerMask.NameToLayer(BellGamePortalSetup.PreviewLayerName);
            if (previewLayer <= 0)
            {
                Debug.LogError("[BellGame] レイヤー '" + BellGamePortalSetup.PreviewLayerName
                               + "' が無い。先に BellGame/セットアップ/1 を実行してほしい。");
                return;
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var a = BuildTintWorld(WorldId.Grassland, Vector3.zero, new Color(0.38f, 0.62f, 0.34f));
            var b = BuildTintWorld(WorldId.Temple, new Vector3(400f, 0f, 0f), new Color(0.72f, 0.36f, 0.30f));
            a.door.destination = WorldId.Temple;
            b.door.destination = WorldId.Grassland;

            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = 400f;
            set.startWorld = WorldId.Grassland;

            // ★切り抜き方式。板を貼らないので、漏れも解像度の変化も原理的に起きない。
            AddPortalStencil(a, set);
            AddPortalStencil(b, set);

            new GameObject("Hud").AddComponent<BellGameHud>();
            WorldSky.Apply(WorldId.Grassland);

            Save(scene, file, note);
        }

        // 扉のまわりが 180 度回転対称なミニ世界。
        //   180 度回すと (x,z) は (-x,-z) へ移るので、目印は必ず対で置く。
        private static Mini BuildTintWorld(WorldId id, Vector3 origin, Color tint)
        {
            var root = new GameObject("World_" + id);

            // 地面は扉を中心にした正方形。端が対称でないと縁で気づいてしまう。
            var ground = Box("Ground", origin + new Vector3(0f, -0.5f, 0f), new Vector3(30f, 1f, 30f));
            Tint(ground, tint);
            ground.SetParent(root.transform, true);

            // ★目印は (x,z) と (-x,-z) の対で置く。高さも同じにする。
            //   対でないと「向こう側は形が違う」が先に見えてしまい、
            //   色だけ変わるかどうかの検査にならない。
            var spots = new[]
            {
                new Vector3(-6f, 0f,  9f), new Vector3( 5f, 0f,  6f),
                new Vector3(-9f, 0f,  3f), new Vector3( 8f, 0f, 11f),
            };
            var heights = new[] { 3.0f, 5.0f, 4.0f, 6.0f };

            for (int i = 0; i < spots.Length; i++)
            {
                float h = heights[i];
                for (int s = 0; s < 2; s++)
                {
                    var p = (s == 0) ? spots[i] : new Vector3(-spots[i].x, 0f, -spots[i].z);
                    var m = Box($"Mark_{i}{(s == 0 ? "" : "b")}",
                                origin + new Vector3(p.x, h * 0.5f, p.z),
                                new Vector3(0.6f, h, 0.6f));
                    Tint(m, tint * 1.45f);
                    m.SetParent(root.transform, true);
                }
            }

            var listener = MakeListener(origin + new Vector3(0f, 1.6f, 4f),
                                        lookAt: origin + new Vector3(0f, 1.6f, 0f));
            listener.SetParent(root.transform, true);

            var door = MakeDoor(origin, listener);
            door.transform.parent.SetParent(root.transform, true);

            var beyond = MakeSource("Beyond", origin + new Vector3(0f, 1.4f, -3f), 1);
            SetTestSignal(beyond, door);
            beyond.SetParent(door.transform, true);   // ★扉の持ち物。扉と一緒に装置側へ来る

            var dummy = MakeSource("Src0", listener.position + new Vector3(0f, 0f, 1.5f), 0);
            var follow = dummy.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = new Vector3(0f, 0f, 1.5f);
            dummy.SetParent(root.transform, true);

            var demoGo = new GameObject("AcousticFlowSceneDemo");
            var demo = demoGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = dummy;
            demo.extraSources = new[] { beyond };
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = AcousticMaterialPreset.Default;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;
            demo.useCppDsp = true;
            demo.apertureContrast = 1.5f;
            demo.enableReverb = false;
            demoGo.transform.SetParent(root.transform, true);

            var camGo = new GameObject("Main Camera");
            camGo.tag = "MainCamera";
            camGo.AddComponent<Camera>();
            camGo.AddComponent<AudioListener>();
            camGo.transform.SetParent(root.transform, true);

            var lightGo = new GameObject("Directional Light");
            var light = lightGo.AddComponent<Light>();
            WorldSky.ApplySun(id, light);

            // ★このラボでは**太陽を共通にする。**
            //
            //   本編は世界ごとに太陽を持つ（草原 52°/−35°・強度1.15、
            //   崩壊都市 38°/+15°・強度0.72 ── 方位が 50° 違う）。
            //   それは設計どおりだが、**この検査の前提を壊す**。
            //   段4 は「色以外まったく同じ」で、動いたら継ぎ目だと判断する場所なのに、
            //   影が 50° 振れると「バグで動いた」のか「別の世界だから動いた」のか
            //   区別が付かなくなる。検査にならない。
            //
            //   → 向きと強度を揃え、違いを**色と空だけ**に絞る。
            light.transform.rotation = Quaternion.Euler(45f, -30f, 0f);
            light.intensity = 1.0f;
            lightGo.transform.SetParent(root.transform, true);

            var wbGo = new GameObject("WorldBounds");
            var wb = wbGo.AddComponent<WorldBounds>();
            wb.listener = listener;
            wb.ground = ground.GetComponent<BoxCollider>();
            wbGo.transform.SetParent(root.transform, true);

            var hushGo = new GameObject("DoorHush");
            var hush = hushGo.AddComponent<DoorHush>();
            hush.listener = listener;
            hush.door = door.transform;
            hushGo.transform.SetParent(root.transform, true);

            var moveGo = new GameObject("WorldTransition");
            moveGo.transform.SetPositionAndRotation(door.transform.position, door.transform.rotation);
            var move = moveGo.AddComponent<WorldTransition>();
            move.worldDoor = door;
            move.aperture = moveGo.transform;
            move.listener = listener;
            moveGo.transform.SetParent(root.transform, true);

            return new Mini { root = root, door = door };
        }

        private struct Mini { public GameObject root; public WorldDoor door; }

        private static Mini BuildMiniWorld(WorldId id, Vector3 origin, Color tint)
        {
            var root = new GameObject("World_" + id);

            // 地面は扉の手前へ寄せる（扉が端に立つ形）。
            var ground = Box("Ground", origin + new Vector3(0f, -0.5f, 11f), new Vector3(24f, 1f, 26f));
            Tint(ground, tint);
            ground.SetParent(root.transform, true);

            // 目印。どちらの世界に居るか一目で分かるように高さを変えた柱を置く。
            //   ★扉の**手前側(+Z)** に置く。本編と同じ並び ──
            //     扉は世界の端に立ち、世界は扉の手前に広がる。
            //     以前は -Z に置いていて、ポータルが世界の裏側を映していた。
            for (int i = 0; i < 3; i++)
            {
                var m = Box("Mark_" + i, origin + new Vector3(-6f + i * 6f, 1.5f + i, 9f),
                            new Vector3(0.6f, 3f + i * 2f, 0.6f));
                Tint(m, tint * 1.4f);
                m.SetParent(root.transform, true);
            }

            var listener = MakeListener(origin + new Vector3(0f, 1.6f, 4f),
                                        lookAt: origin + new Vector3(0f, 1.6f, 0f));
            listener.SetParent(root.transform, true);

            var door = MakeDoor(origin, listener);
            door.transform.parent.SetParent(root.transform, true);

            // 扉の奥の音。ミニ世界には向こうの部屋を建てていないので、位置だけ。
            var beyond = MakeSource("Beyond", origin + new Vector3(0f, 1.4f, -3f), 1);
            SetTestSignal(beyond, door);
            beyond.SetParent(door.transform, true);   // ★扉の持ち物。扉と一緒に装置側へ来る

            var dummy = MakeSource("Src0", listener.position + new Vector3(0f, 0f, 1.5f), 0);
            var follow = dummy.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = new Vector3(0f, 0f, 1.5f);
            dummy.SetParent(root.transform, true);

            var demoGo = new GameObject("AcousticFlowSceneDemo");
            var demo = demoGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = dummy;
            demo.extraSources = new[] { beyond };
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = AcousticMaterialPreset.Default;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;
            demo.useCppDsp = true;
            demo.apertureContrast = 1.5f;   // 4.0 だと半開きが実質無音（実測）
            demo.enableReverb = false;
            demoGo.transform.SetParent(root.transform, true);

            var camGo = new GameObject("Main Camera");
            camGo.tag = "MainCamera";
            camGo.AddComponent<Camera>();
            camGo.AddComponent<AudioListener>();
            camGo.transform.SetParent(root.transform, true);

            var lightGo = new GameObject("Directional Light");
            var light = lightGo.AddComponent<Light>();
            WorldSky.ApplySun(id, light);
            lightGo.transform.SetParent(root.transform, true);

            var wbGo = new GameObject("WorldBounds");
            var wb = wbGo.AddComponent<WorldBounds>();
            wb.listener = listener;
            wb.ground = ground.GetComponent<BoxCollider>();
            wbGo.transform.SetParent(root.transform, true);

            // 扉から離れても映るように、ラボでは描画距離を広く取る。
            //   本編の既定 24m は「常時 1 枚ぶん余計にシーンを描く」コストの歯止め。

            var hushGo = new GameObject("DoorHush");
            var hush = hushGo.AddComponent<DoorHush>();
            hush.listener = listener;
            hush.door = door.transform;
            hushGo.transform.SetParent(root.transform, true);

            // くぐり。ポータル描画は付けない。
            var moveGo = new GameObject("WorldTransition");
            moveGo.transform.SetPositionAndRotation(door.transform.position, door.transform.rotation);
            var move = moveGo.AddComponent<WorldTransition>();
            move.worldDoor = door;
            move.aperture = moveGo.transform;
            move.listener = listener;
            moveGo.transform.SetParent(root.transform, true);

            return new Mini { root = root, door = door };
        }

        // 扉の奥で鳴らすテスト信号。
        //
        // ★**持続する広帯域**でないと、開き具合の音色変化が読めない。
        //   合成の環境音（葉擦れ・風）は帯域が偏っていて途切れもあるので、
        //   隙間が 5cm から 15cm になったときの差が「変化」ではなく
        //   「別の音になった」に聞こえてしまう。
        //   DEV_LOG E-3「テスト信号を目的で使い分ける」。デモの既定が楽曲なのも同じ理由。
        //
        //   ここは**§4.3 のフレネル帯域積分が連続かどうかを聴く場所**なので、
        //   信号は素直な方を選ぶ。ゲーム本編の音づくりとは別の話。
        //
        // ⚠ 市販楽曲。**配布物には入れられない**（仕様書 §7.6・§9）。`*.wav` はコミットしない。
        //   検証シーン（Lab_*.unity）専用で、本編のステージ生成では使っていない。
        private const string kTestClip =
            "Assets/Audio/ロクデナシ「ブリザード」 Rokudenashi - Blizzard【Official Music Video】"
            + " - Rokudenashi (128k).wav";

        // 扉の奥の音を仕込む。
        //
        // ★既定は**世界の声**（BeyondAmbience）。
        //   「繋がっていない扉の奥には音源が無い」「繋いだら向こうの世界の音が出現する」
        //   「開くほど取り込まれる」── この 3 つは BeyondAmbience が既に満たしている。
        //   音量も帯域も扉の角度で触らないので、開き具合は §4.3 の仕事のまま。
        //
        // ★テスト信号（持続する広帯域）は**差し替え候補として渡すだけ**。F5 で切り替わる。
        //   合成の環境音は帯域が偏っていて途切れるので、隙間の連続性は読み取れない。
        //   目的が違うので両方要る ── どちらか一方を既定にして、もう一方を捨てない。
        private static void SetTestSignal(Transform src, WorldDoor door)
        {
            var beyond = src.gameObject.GetComponent<BeyondAmbience>();
            if (beyond == null) beyond = src.gameObject.AddComponent<BeyondAmbience>();
            beyond.worldDoor = door;

            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(kTestClip);
            if (clip == null)
                Debug.LogWarning("[BellGame] 検証用の楽曲が見つからない。"
                                 + "F5 の切り替え先が無いので、隙間の連続性は読み取りにくい。");
            beyond.testSignal = clip;

            // ★IrConvolver がテスト信号を作らないようにしておく。
            //   こちらのクリップと二重に鳴ると、何を聴いているのか分からなくなる。
            var ir = src.GetComponent<IrConvolver>();
            if (ir != null) ir.generateTestSignal = false;
        }

        private struct Room { public float w, depth, h; }

        // 扉の奥の部屋。**見た目は要らない**（閉じた扉の奥は見えない）ので、
        //   Renderer は切って音響だけ残す。
        private static Room BuildBeyondRoom(WorldId dest)
        {
            // Sabine から逆算した寸法と材質。狙いは WORLD_SETTING §3 の実測値。
            float w, d, h, mul;
            AcousticMaterialPreset mat;
            switch (dest)
            {
                case WorldId.Cave:                       // 狙い 5.1s
                    w = 10f; d = 8f; h = 5f;
                    mat = AcousticMaterialPreset.Concrete; mul = 1f;      // α0.038
                    break;
                default:                                 // 崩壊都市：狙い 1.5s
                    w = 12f; d = 10f; h = 6f;
                    mat = AcousticMaterialPreset.Default; mul = 0.72f;    // α0.208×0.72≒0.15
                    break;
            }

            const float t = 0.4f;
            var root = new GameObject("BeyondRoom_" + dest);
            // 扉（z=0）の奥＝ -Z 側。開口は扉の枠だけにする。
            float cz = -(d * 0.5f);

            void Wall(string name, Vector3 c, Vector3 s)
            {
                var b = Box(name, c, s);
                var r = b.GetComponent<Renderer>();
                if (r != null) Object.DestroyImmediate(r);       // 見せない
                var surf = b.gameObject.AddComponent<AcousticSurface>();
                surf.material = mat;
                if (mul != 1f)
                {
                    surf.mode = AcousticSurfaceMode.Adjusted;
                    surf.absorptionScale = mul;
                }
                b.SetParent(root.transform, true);
            }

            Wall("Floor", new Vector3(0f, -t * 0.5f, cz), new Vector3(w, t, d));
            Wall("Ceil", new Vector3(0f, h + t * 0.5f, cz), new Vector3(w, t, d));   // ★天井が要る
            Wall("Back", new Vector3(0f, h * 0.5f, cz - d * 0.5f), new Vector3(w, h, t));
            Wall("Left", new Vector3(-w * 0.5f, h * 0.5f, cz), new Vector3(t, h, d));
            Wall("Right", new Vector3(w * 0.5f, h * 0.5f, cz), new Vector3(t, h, d));

            // 手前の面。扉の枠のぶんだけ穴を空ける（左右の袖と、まぐさの上）。
            const float dw = 1.1f, dh = 2.2f;
            float side = (w - dw) * 0.5f;
            Wall("FrontL", new Vector3(-(dw * 0.5f + side * 0.5f), h * 0.5f, 0f),
                 new Vector3(side, h, t));
            Wall("FrontR", new Vector3(dw * 0.5f + side * 0.5f, h * 0.5f, 0f),
                 new Vector3(side, h, t));
            Wall("FrontTop", new Vector3(0f, dh + (h - dh) * 0.5f, 0f),
                 new Vector3(dw, h - dh, t));

            Debug.Log($"[BellGame] 扉の奥に部屋を建てた: {w}×{d}×{h}m / {mat}"
                      + (mul != 1f ? $"×{mul}" : "")
                      + $"\n  → RT60 は**エンジンが実行時に導く**（Sabine）。焼いていない。");
            return new Room { w = w, depth = d, h = h };
        }

        // ── 検証中：本番の草原 ⇔ ラボの箱世界 ─────────────────────
        //
        // ★確かめたいこと（発注者の指定）
        //   1. **モデルやデザインが変わっても扉が成立するか**
        //      ラボは箱 14 個で作ってきた。本番は地形・木・門・川が入る。
        //      置き方（doorLocal で扉に合わせる）が本物の中身で崩れないか。
        //   2. **シェーダで生やした草が扉ごしに見えるか**
        //      草はポータルのカメラにも描かれないといけない。
        //      片方のカメラにしか渡っていなければ、扉の中だけ草が消える。
        //
        // ★片方をわざとラボの箱世界にしてある。
        //   両方本番だと「どちらが原因か」が分からない。
        //   箱の側は既に通っているので、崩れたら本番側の何かが原因だと言い切れる。
        // ── 検証中：本番 0 ⇔ 1（白い部屋 ⇔ 草原）─────────────────
        //
        // ★これは**ゲームの冒頭そのもの**です。
        //   ラボの箱は一切使いません。両側とも造形レーンが作った本番のレイアウトで、
        //   仕掛けだけこちらで足します（レイアウトのシーンには扉も装置も入っていない）。
        //
        // ★確かめたいこと
        //   1. 本番の物量で**転移が成立するか**（Lab_Half で通した機構がそのまま効くか）
        //   2. 跨いだときの重いフレームが**本番でどれだけ伸びるか**（モニターの「山の余り」）
        //   3. 白い部屋（閉空間）⇔ 草原（屋外）で、扉の開き具合が音に出るか
        //      ── ラボでは両側とも箱だった。**閉と開が非対称なのは本番が初めて。**
        //
        // ⚠ 白い部屋に **WorldId が無い**ので、`Temple`（崩壊都市）の枠を借りています。
        //   WorldSet は根っこの名前 `World_<WorldId>` で世界を見つける作りなので、
        //   枠が無いと登録できません。借りているせいで**空と世界の声が崩壊都市のもの**に
        //   なります。白い部屋に id を足すかどうかは設計の話なので、勝手にはやりません
        //  （連絡板で聞いています）。
        // ── 検証中：本番 0 → 1 → 2 の通し ───────────────────────
        //
        // ★確かめたいこと（発注者の指定）
        //   **3 つのステージを通しで回して、崩れずに動くか。**
        //   2 つでは出ない問題が 3 つ目で出ます ── 眠っている世界が 2 つになるので、
        //   置き直しも、レイヤーも、装置の繋ぎ替えも、初めて「複数」を相手にします。
        //
        // ★**音源は置きません**（発注者の指定）。
        //   通るかどうかだけを見る場です。音源が居ると、崩れたときに
        //   「構造の問題か音の問題か」が混ざります。
        //   装置が要求する index 0 の 1 本だけ、鳴らさずに置いてあります。
        //
        // ★行き先は操作卓から選びます。ベルを集める進行を通さずに、
        //   どの世界へでも繋げられるようにしてあります（検証の場なので）。
        [MenuItem("BellGame/検証中/本番 0 → 1 → 2 の通し（音源なし）")]
        public static void TrialStage012()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            int previewLayer = LayerMask.NameToLayer(BellGamePortalSetup.PreviewLayerName);
            if (previewLayer <= 0)
            {
                Debug.LogError("[BellGame] レイヤー '" + BellGamePortalSetup.PreviewLayerName
                               + "' が無い。先に BellGame/セットアップ/1 を実行してほしい。");
                return;
            }

            // ★新しいシーンを作る**前に**、材料が揃っているか確かめる。
            //
            //   以前ここは NewScene してから読み込みに失敗して return していました。
            //   その結果、**開いていたシーンが無題の壊れかけに置き換わったまま残り**、
            //   「作ったつもりのシーンが無い」という分かりにくい壊れ方をしました。
            //   失敗するなら、何も壊さないうちに失敗させます。
            string[] need = {
                "Assets/Scenes/Stage0_Layout.unity",
                "Assets/Scenes/Stage1_Layout.unity",
                "Assets/Scenes/Stage2_Layout.unity",
            };
            foreach (var p in need)
            {
                if (System.IO.File.Exists(p)) continue;
                Debug.LogError("[BellGame] " + p + " が無い。先にステージを作ってほしい。"
                               + "（今のシーンには手を付けていません）");
                return;
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var w0 = ImportStage("Assets/Scenes/Stage0_Layout.unity", WorldId.White,
                                 Vector3.zero, withSources: false);
            if (w0.root == null) return;
            var w1 = ImportStage("Assets/Scenes/Stage1_Layout.unity", WorldId.Grassland,
                                 new Vector3(400f, 0f, 0f), withSources: false);
            if (w1.root == null) return;
            var w2 = ImportStage("Assets/Scenes/Stage2_Layout.unity", WorldId.Temple,
                                 new Vector3(800f, 0f, 0f), withSources: false);
            if (w2.root == null) return;

            // ★音を**完全に**外す（発注者の指定）。
            //
            //   音響ホスト（AcousticFlowSceneDemo）自体は残します。
            //   `WorldSet.AdoptRig` は**デモが無いと即 return** し、扉が世界の外へ出ないため
            //   `RigDoor` が空のまま ── 世界の入れ替えが成立しなくなります。
            //   残すのはホストだけで、鳴る物は 1 つも置きません。
            StripAudio(w0.root); StripAudio(w1.root); StripAudio(w2.root);

            // ★カメラを 1 台に絞る。**これが「カメラが動かない」の心当たり**です。
            //
            //   EquipRig は世界ごとに Main Camera を作るので、この場では 3 台居ます。
            //   `Camera.main` は「タグが MainCamera で**有効な最初の 1 台**」を返すので、
            //   起動の瞬間に 3 台とも有効だと、**眠る側のカメラを掴むことがあります。**
            //   掴んだカメラは映っていないので、動かしても画が変わりません。
            //   立つ世界のぶんだけ有効にしておけば、迷いようがありません。
            FocusCameras(w0, w1, w2);

            // 行き先の初期値。操作卓から変えられます。
            w0.door.destination = WorldId.Grassland;
            w1.door.destination = WorldId.Temple;
            w2.door.destination = WorldId.Grassland;

            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = 400f;
            set.startWorld = WorldId.White;      // 白い部屋から始まる（ゲームの冒頭）

            AddPortalStencil(w0, set);
            AddPortalStencil(w1, set);
            AddPortalStencil(w2, set);

            if (Object.FindFirstObjectByType<BellGameHud>() == null)
                new GameObject("Hud").AddComponent<BellGameHud>();
            new GameObject("PerfMeter").AddComponent<PerfMeter>().targetFps = 45;
            new GameObject("SourceTierApplier").AddComponent<SourceTierApplier>();

            WorldSky.Apply(WorldId.White);

            Save(scene, "Trial_Stage012.unity",
                 "検証中: 本番 0（白い部屋）→ 1（草原）→ 2（崩壊都市）の通し。**音源なし**。"
               + "行き先は BellGame/操作卓 から選べます。"
               + "見るところ ①3 つでも置き直しが崩れないか ②眠っている世界が 2 つでも"
               + "レイヤーと装置の繋ぎ替えが正しいか ③くぐって戻れるか。");
        }

        [MenuItem("BellGame/検証中/本番 0 ⇔ 1（白い部屋 ⇔ 草原）")]
        public static void TrialStage01()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            int previewLayer = LayerMask.NameToLayer(BellGamePortalSetup.PreviewLayerName);
            if (previewLayer <= 0)
            {
                Debug.LogError("[BellGame] レイヤー '" + BellGamePortalSetup.PreviewLayerName
                               + "' が無い。先に BellGame/セットアップ/1 を実行してほしい。");
                return;
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            // 草原＝Stage1。白い部屋＝Stage0（id は Temple を借りている）。
            var w1 = ImportStage("Assets/Scenes/Stage1_Layout.unity", WorldId.Grassland, Vector3.zero);
            if (w1.root == null) return;
            var w0 = ImportStage("Assets/Scenes/Stage0_Layout.unity", WorldId.White,
                                 new Vector3(400f, 0f, 0f));
            if (w0.root == null) return;

            // 扉は互いを向く。片方だけ繋いでも WorldSet が置き方を決められない。
            w1.door.destination = WorldId.White;
            w0.door.destination = WorldId.Grassland;

            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = 400f;
            set.startWorld = WorldId.White;       // ★白い部屋から始まる（ゲームの冒頭）

            AddPortalStencil(w1, set);
            AddPortalStencil(w0, set);

            if (Object.FindFirstObjectByType<BellGameHud>() == null)
                new GameObject("Hud").AddComponent<BellGameHud>();

            // 60fps に張り付かせて、残りコストを出す。**ここが今回の主目的。**
            new GameObject("PerfMeter").AddComponent<PerfMeter>().targetFps = 45;
            new GameObject("SourceTierApplier").AddComponent<SourceTierApplier>();

            // ★全部の音源を音量 0 に押さえる（費用はそのまま）。F8 で戻せる。
            //   BeyondAmbience も AmbientSource も volume を書き戻すので、
            //   一度落とすだけでは戻ってしまう。押さえ続ける役を 1 つ置く。
            new GameObject("LoadTestSilence").AddComponent<LoadTestSilence>();

            // ★「重い」をその場で二分する操作台。音源の本数と、後付けの当たり判定を切れる。
            new GameObject("LoadTestRig").AddComponent<LoadTestRig>();

            WorldSky.Apply(WorldId.White);

            Save(scene, "Trial_Stage01.unity",
                 "検証中: 本番 0（白い部屋）⇔ 1（草原）。両側とも本番のレイアウトで、"
               + "扉と装置だけこちらで足してある。白い部屋から始まる。"
               + "見るところ ①本番の物量で転移が成立するか ②跨いだときの山が"
               + "どれだけ伸びるか（BellGame/モニター の『山の余り』）"
               + "③閉空間⇔屋外で扉の開き具合が音に出るか。"
               + "⚠白い部屋は WorldId が無いので Temple の枠を借りている（空と声が崩壊都市になる）。");
        }

                /// 本番のレイアウトを 1 つ取り込んで、扉と装置を足して世界にする。
        ///   ★WorldSet は根っこの名前 `World_<WorldId>` で世界を見つける（WorldSet.AdoptPlaced）。
        ///     名前を外すと、World が 1 つも見つからず黙って何も起きない。
        /// 扉一式を**構造で**探す。名前では探さない。
        ///
        /// ★名前で探して 3 回壊れました。
        ///     `Door_Shrine` → `Door_Frame` に改名されて落ちた
        ///     `Stage2_Layout` だけ `Visual_Doorway` で落ちた
        ///   造形レーンが名前を変えるのは自然なことで、**こちらが名前に依存しているのが誤り**です。
        ///
        /// ★動く板（`Ref_DoorSlab`）は 3 つのステージすべてが持っています。
        ///   そこから親を辿り、**世界の根っこの 1 つ下**までのぼれば扉一式です。
        ///   世界に扉は 1 枚なので、これで一意に決まります。
        private static Transform FindDoorAssembly(Transform worldRoot)
        {
            var slab = FindByName(worldRoot, "Ref_DoorSlab") ?? FindByName(worldRoot, "DoorPanel");
            if (slab == null) return null;

            var t = slab;
            while (t.parent != null && t.parent != worldRoot) t = t.parent;
            return t;
        }

        /// 鳴る物を全部外す。音響ホストは残す（WorldSet が扉を外へ出すのに要る）。
        private static void StripAudio(GameObject root)
        {
            int n = 0;
            // ★配列を先に取ってから**オブジェクトごと**消すので、
            //   消した親の下にもう 1 本 AudioSource が居ると、その要素は無効参照になります。
            //   `s.gameObject` で MissingReferenceException が飛び、
            //   **この後（カメラを絞る／WorldSet を置く／保存）が丸ごと実行されません**でした。
            //   ＝「3 世界は入ったのに Untitled のまま」の正体。
            foreach (var s in root.GetComponentsInChildren<AudioSource>(true))
            {
                if (s == null) continue;        // 親ごと消えた後の抜け殻
                Object.DestroyImmediate(s.gameObject); n++;
            }

            // 音を出す側の残りも同じ理屈で落とす。鳴る物を 1 つも残さないため。
            foreach (var a in root.GetComponentsInChildren<AmbientSource>(true))
            { if (a != null) Object.DestroyImmediate(a); }

            var demo = root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
            if (demo != null)
            {
                // ★音源を **0 本にはできません**。
                //
                //   AcousticFlowSceneDemo.Update の頭に
                //       if (_sources.Length == 0) return;
                //   があり、**移動（HandleMovement）もその return の先**に居ます。
                //   全部外すと WASD もマウスも死にます ── 発注者の「WASD が聞かない」。
                //
                //   システム側は触らない誓約なので、こちら側で
                //   **黙った音源を 1 本だけ**置きます。耳のすぐ横に置くので
                //   遮蔽の計算もほぼ発生せず、費用の邪魔になりません。
                var q = new GameObject("Silent_Anchor");
                // ★耳の**子**にする。世界の子にすると置いた場所に残り、
                //   歩いて離れれば壁ごしになって遮蔽が掛かります
                //  （実測: 遮蔽 0.80 / 帯域 0.16〜0.21 ＝ しっかり計算されていた）。
                //   耳に付けておけば常に距離 1m・遮蔽なしで、費用がほぼ立ちません。
                //
                //   装置（デモ・耳）は WorldSet が世界の外へ出すので、
                //   これも一緒に出て**世界を移っても付いて来ます**。
                var host = (demo.listener != null) ? demo.listener : root.transform;
                q.transform.SetParent(host, false);
                q.transform.localPosition = Vector3.up * 1f;

                var src = q.AddComponent<AudioSource>();
                src.playOnAwake = false;
                src.mute = true;
                src.volume = 0f;
                src.spatialBlend = 1f;

                demo.source = q.transform;
                demo.extraSources = System.Array.Empty<Transform>();
                demo.enableReverb = false;      // 鳴る物が無いので導かせる相手も居ない
            }
            Debug.Log($"[BellGame] {root.name}: 音源を {n} 本外した"
                      + "（ホストと、移動を生かすための黙った 1 本だけ残す）。", root);
        }

        /// 立つ世界のカメラだけを有効にする。`Camera.main` の取り違えを起こさないため。
        private static void FocusCameras(Mini live, params Mini[] sleepers)
        {
            foreach (var s in sleepers)
            {
                if (s.root == null) continue;
                foreach (var c in s.root.GetComponentsInChildren<Camera>(true)) c.enabled = false;
                foreach (var e in s.root.GetComponentsInChildren<AudioListener>(true)) e.enabled = false;
            }
            if (live.root == null) return;
            foreach (var c in live.root.GetComponentsInChildren<Camera>(true)) c.enabled = true;
            Debug.Log($"[BellGame] カメラを {live.root.name} の 1 台に絞った"
                      + "（3 台とも有効だと Camera.main が眠る側を掴むことがある）", live.root);
        }

        private static Mini ImportStage(string path, WorldId id, Vector3 offset, bool withSources = true)
        {
            var empty = default(Mini);
            if (!File.Exists(path))
            {
                Debug.LogError($"[BellGame] {path} が無い。造形レーンのレイアウトが要る。");
                return empty;
            }

            var root = new GameObject("World_" + id);
            root.transform.position = offset;
            var loaded = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            foreach (var go in loaded.GetRootGameObjects())
                go.transform.SetParent(root.transform, true);
            // ★閉じる前に、**シーンが持っているマテリアルを複製する。**
            //
            //   資産になっていないマテリアル（`new Material(shader)` を CreateAsset せずに
            //   直接 Renderer へ入れた物。BellGameLayout.cs:642 がそれ）は
            //   **そのシーンの持ち物**です。元のシーンを閉じると一緒に消えるので、
            //   参照だけが残って**ピンク**になります。
            //
            //   資産になっている物はそのまま使います ── 複製するとシーンが太るうえ、
            //   造形レーンが資産を直しても反映されなくなるので。
            RescueSceneMaterials(root);

            EditorSceneManager.CloseScene(loaded, true);
            if (offset != Vector3.zero)
                foreach (Transform t in root.transform) t.position += offset;

            // 装置として重複する物は捨てる。WorldSet は世界の外に 1 つだけ。
            foreach (var old in root.GetComponentsInChildren<WorldSet>(true))
                Object.DestroyImmediate(old.gameObject);

            // 見て回るための道具は外す。立っていた場所は出発点として控える。
            Vector3 standAt = root.transform.position;
            var walker = root.GetComponentInChildren<LayoutWalker>(true);
            if (walker != null)
            {
                standAt = walker.transform.position;
                // ★順番が要る。LayoutWalker は CharacterController を [RequireComponent] で
                //   要求しているので、**先に CharacterController を消そうとすると拒否される**
                //  （"Can't remove CharacterController because LayoutWalker depends on it"）。
                //   依存している側から外す。
                var cc = walker.GetComponent<CharacterController>();
                Object.DestroyImmediate(walker);
                if (cc != null) Object.DestroyImmediate(cc);
            }
            else standAt = ContentBounds(root).center;

            foreach (var c in root.GetComponentsInChildren<Camera>(true))
                Object.DestroyImmediate(c.gameObject);
            foreach (var e in root.GetComponentsInChildren<AudioListener>(true))
                Object.DestroyImmediate(e);

            // ★扉のモデルは既にある。仕掛けだけ足す（箱で作り直さない）。
            //   造形レーンが `Door_Shrine` から `Door_Frame` に作り直しているので、
            //   両方の名前を見る。片方に決め打つと、また名前が変わったときに黙って壊れる。
            var frame = FindDoorAssembly(root.transform);
            if (frame == null)
            {
                Debug.LogError($"[BellGame] {path} に扉が見つからない（Ref_DoorSlab も DoorPanel も無い）。"
                               + "扉の位置が決まらない。", root);
                return empty;
            }
            Debug.Log($"[BellGame] 扉一式を見つけた: {frame.name}（構造から。名前では探していません）", frame);

            var listener = MakeListener(standAt, lookAt: frame.position + Vector3.up * 1.6f);
            listener.SetParent(root.transform, true);

            var door = EquipRealDoor(frame, listener);
            if (door == null) return empty;

            // ★扉は**自分の世界の中身のほう**を向く。
            //
            //   WorldSet はこの向き（doorLocal）を基準に、行き先の世界を扉へ重ねます。
            //   規約が世界どうしで食い違うと、片方が 180° 裏返って置かれる ──
            //   「草原のモデルの向きが逆」がこれです。
            //
            //   規約は既に通っているラボ世界に合わせました。BuildMiniWorld は
            //   地面を扉の +Z 側（origin+11）に敷き、扉の forward も +Z ＝
            //   **forward が自分の世界の中身を向いている**。本番の扉は造形レーンが
            //   モデルの都合で置いているので、ここで向きだけ揃えます。
            //
            //   ⚠ 見た目のモデル（Door_Frame）は回しません。回すと枠ごと裏返ります。
            //     揃えるのは WorldDoor（写像の基準）だけ。
            var wdT = door.transform;
            Vector3 toContent = ContentBounds(root).center - wdT.position;
            toContent.y = 0f;
            if (toContent.sqrMagnitude > 1e-4f && Vector3.Dot(wdT.forward, toContent) < 0f)
            {
                wdT.rotation = Quaternion.AngleAxis(180f, Vector3.up) * wdT.rotation;
                Debug.Log($"[BellGame] {id} の扉の向きを 180° 揃えた"
                          + "（forward が自分の世界の中身を向くように）", wdT);
            }

            // ★代役を退場させる。両側とも本物の地形・部屋があるので、
            //   4×4×3m の吸音箱（α≈0.99）が入れ子で残ると開口の内側を殻で覆う。
            //   ここが Lab_Audio / Lab_Half で「開けたら小さくなる」を起こしていた原因。
            RemoveAperturePanel(door, "両側とも本番の地形なので代役は要らない");

            EquipRig(root, listener, door);

            // ★音源なしの通し検証（withSources = false）。
            //   装置は index 0 を要求するので `Src0` は残し、扉の奥の音（Beyond）だけ外します。
            //   通るかどうかだけを見る場なので、音が居ると
            //   崩れたときに「構造の問題か音の問題か」が混ざります。
            if (!withSources)
            {
                var beyond = FindByName(root.transform, "Beyond");
                if (beyond != null) Object.DestroyImmediate(beyond.gameObject);
                var d0 = root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
                if (d0 != null) d0.extraSources = System.Array.Empty<Transform>();
            }

            // ★白い部屋は閉空間。残響を切ると、この検証の見どころが半分消える。
            //   （EquipRig の既定は反響オフ ── ラボの箱世界に合わせた値だった）
            var demo = root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
            if (demo != null)
            {
                demo.enableReverb = true;
                demo.roomSeedRadius = 0.6f;
                demo.roomCellSize = 0.25f;
                demo.roomBlendRadius = 2.0f;
                // 定位を扉で動かさない（発注者の決定 ＋ ステアの反転を避ける）。
                // ★立場の反転（こちらが部屋・外の音が入ってくる）に合わせた設定。
                //   開口を主役にするので B1 は入れる。ステアは engDir の反転が
                //   直るまで切ったまま（回折タップのパンで開口へ寄るので実害は無い）。
                demo.useDirectionalSteering = false;
                demo.diffractionHrtf = true;

                // ★音の配分を Lab_Half と同じにする。
                //   ここが揃っていないと、ラボで出した数字を本番へ持っていけません
                //  （同じ設定で測っていないものを比べても意味が無い）。
                //     透過 0dB   … 壁と扉の差は**材質側**で付けてある（SealWalls）
                //     回折 +3dB  … 開口が唯一の入口
                //     こもり 9dB … 壁の奥で鳴っている感じは音色で作る
                demo.transmissionGainDb = 0f;
                demo.transmissionTilt = 1.8f;
                demo.transmissionHighCutDb = 9f;
                demo.diffractionGainDb = 3f;
                demo.apertureContrast = 1.5f;
            }

            if (withSources) AddLoadSources(root, listener, door, kLoadSources);
            EquipColliders(root);
            SealWalls(root, door);

            // くぐる仕掛け。レイアウトには入っていないので足す。
            if (root.GetComponentInChildren<WorldTransition>(true) == null)
            {
                var moveGo = new GameObject("WorldTransition");
                moveGo.transform.SetPositionAndRotation(door.transform.position,
                                                        door.transform.rotation);
                var move = moveGo.AddComponent<WorldTransition>();
                move.worldDoor = door;
                move.aperture = moveGo.transform;
                move.listener = listener;
                moveGo.transform.SetParent(root.transform, true);
            }

            Debug.Log($"[BellGame] {id} を取り込んだ: {path}\n"
                      + $"  出発点 {standAt}  扉 {frame.name} {frame.position}");
            return new Mini { root = root, door = door };
        }

        [MenuItem("BellGame/検証中/本番の草原 ⇔ ラボの箱世界")]
        public static void TrialRealGrass()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            // ★本番はこちら（発注者の指定）。造形レーンが作っているレイアウト。
            //   GPU インスタンスの草 1 面と葉 3 本が入っている ── そこが今回の本題。
            //   ⚠ レイアウト用なので**扉も装置も入っていない**。両方こちらで足す。
            const string src = "Assets/Scenes/Stage1_Layout.unity";
            if (!File.Exists(src))
            {
                Debug.LogError($"[BellGame] {src} が無い。造形レーンのレイアウトが要る。");
                return;
            }

            int previewLayer = LayerMask.NameToLayer(BellGamePortalSetup.PreviewLayerName);
            if (previewLayer <= 0)
            {
                Debug.LogError("[BellGame] レイヤー '" + BellGamePortalSetup.PreviewLayerName
                               + "' が無い。先に BellGame/セットアップ/1 を実行してほしい。");
                return;
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            // ── 本番の草原を丸ごと取り込む ──────────────────
            var root = new GameObject("World_Grassland");
            var loaded = EditorSceneManager.OpenScene(src, OpenSceneMode.Additive);
            foreach (var go in loaded.GetRootGameObjects())
                go.transform.SetParent(root.transform, true);
            EditorSceneManager.CloseScene(loaded, true);

            // ★取り込んだ物のうち、**装置として重複する物**は捨てる。
            //   WorldSet は世界の外に 1 つだけ。旧方式のポータルは新方式に差し替える。
            foreach (var old in root.GetComponentsInChildren<WorldSet>(true))
                Object.DestroyImmediate(old.gameObject);

            // ★見て回るための道具（LayoutWalker）は外す。
            //   本編のプレイヤーではないので、残すとデモの移動と取り合いになる。
            //   立っていた場所は**扉を置く目印**として先に控える。
            Vector3 standAt = Vector3.zero, lookDir = Vector3.forward;
            var walker = root.GetComponentInChildren<LayoutWalker>(true);
            if (walker != null)
            {
                standAt = walker.transform.position;
                lookDir = walker.transform.forward;
                var cc = walker.GetComponent<CharacterController>();
                if (cc != null) Object.DestroyImmediate(cc);
                Object.DestroyImmediate(walker);
            }
            else
            {
                // 目印が無ければ、描画物の真ん中に立たせる。
                var b = ContentBounds(root);
                standAt = new Vector3(b.center.x, b.center.y, b.center.z);
            }

            // 取り込んだ側のカメラと耳は捨てる。装置はこちらで 1 組だけ作る。
            foreach (var c in root.GetComponentsInChildren<Camera>(true))
                Object.DestroyImmediate(c.gameObject);
            foreach (var e in root.GetComponentsInChildren<AudioListener>(true))
                Object.DestroyImmediate(e);

            // ── 扉の**モデルは既にある**（Door_Shrine）。仕掛けだけ足す ──
            //
            // ★箱で作り直さない。造形レーンが立てた石の門がそのまま扉になる。
            //   ここが今回の検証の芯 ── 「モデルが変わっても成立するか」。
            var shrine = FindByName(root.transform, "Door_Shrine");
            if (shrine == null)
            {
                Debug.LogError("[BellGame] Door_Shrine が見つからない。扉の位置が決まらない。");
                return;
            }

            var listener = MakeListener(standAt + Vector3.up * 0.0f,
                                        lookAt: shrine.position + Vector3.up * 1.6f);
            listener.SetParent(root.transform, true);

            var door = EquipRealDoor(shrine, listener);
            if (door == null) return;

            EquipRig(root, listener, door);

            // ── 相手はラボの箱世界。崩れたときに本番側だと言い切るため ──
            var lab = BuildTintWorld(WorldId.Temple, new Vector3(400f, 0f, 0f),
                                     new Color(0.72f, 0.36f, 0.30f));

            door.destination = WorldId.Temple;
            lab.door.destination = WorldId.Grassland;

            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = 400f;
            set.startWorld = WorldId.Grassland;

            var real = new Mini { root = root, door = door };
            AddPortalStencil(real, set);
            AddPortalStencil(lab, set);

            // 本番側にはくぐる仕掛けが無いので足す（ラボ側は BuildTintWorld が付けている）。
            if (root.GetComponentInChildren<WorldTransition>(true) == null)
            {
                var moveGo = new GameObject("WorldTransition");
                moveGo.transform.SetPositionAndRotation(door.transform.position,
                                                        door.transform.rotation);
                var move = moveGo.AddComponent<WorldTransition>();
                move.worldDoor = door;
                move.aperture = moveGo.transform;
                move.listener = door.listener;
                moveGo.transform.SetParent(root.transform, true);
            }

            if (Object.FindFirstObjectByType<BellGameHud>() == null)
                new GameObject("Hud").AddComponent<BellGameHud>();

            WorldSky.Apply(WorldId.Grassland);

            Save(scene, "Trial_RealGrass.unity",
                 "検証中: 本番の草原（シェーダで生やした草・本物の地形）⇔ ラボの箱世界。"
               + "見るところ ①モデルが本物でも扉が成立するか ②**扉ごしに草が生えているか** "
               + "③跨いだときの重いフレームが本番の物量でどれだけ伸びるか（F4 で測れる）。"
               + "片方を箱にしてあるので、崩れたら本番側が原因だと言い切れる。");
        }

        // 取り込んだ世界に、装置一式（デモ・カメラ・音源・範囲）を足す。
        //   レイアウトのシーンには入っていないので、ここで組む。
        private static void EquipRig(GameObject root, Transform listener, WorldDoor door)
        {
            // 扉の奥の音。★扉の下にぶら下げる ── 扉の持ち物なので、扉と一緒に運ばれる。
            var beyond = MakeSource("Beyond", door.transform.position
                                    - door.transform.forward * 3f + Vector3.up * 0.3f, 1);
            SetTestSignal(beyond, door);
            beyond.SetParent(door.transform, true);

            var dummy = MakeSource("Src0", listener.position + listener.forward * 1.5f, 0);
            var follow = dummy.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = new Vector3(0f, 0f, 1.5f);
            dummy.SetParent(root.transform, true);

            var demoGo = new GameObject("AcousticFlowSceneDemo");
            var demo = demoGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = dummy;
            demo.extraSources = new[] { beyond };
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = AcousticMaterialPreset.Default;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;
            demo.useCppDsp = true;
            demo.apertureContrast = 1.5f;
            demo.enableReverb = false;
            demoGo.transform.SetParent(root.transform, true);

            var camGo = new GameObject("Main Camera");
            camGo.tag = "MainCamera";
            camGo.AddComponent<Camera>();
            camGo.AddComponent<AudioListener>();
            camGo.transform.SetParent(root.transform, true);

            // 遊べる範囲。地面は**いちばん大きい当たり判定**から拾う
            //   （本番の地面の名前を決め打ちしない）。
            var wbGo = new GameObject("WorldBounds");
            var wb = wbGo.AddComponent<WorldBounds>();
            wb.listener = listener;
            wb.ground = LargestCollider(root);
            wbGo.transform.SetParent(root.transform, true);

            var hushGo = new GameObject("DoorHush");
            var hush = hushGo.AddComponent<DoorHush>();
            hush.listener = listener;
            hush.door = door.transform;
            hushGo.transform.SetParent(root.transform, true);
        }

        /// ★レイアウトのモデルに当たり判定を足す。
        ///
        ///   レイアウトのシーンは**見た目だけ**で作られていて、コライダーがありません。
        ///   その状態だとモデルは
        ///     ・体をすり抜ける（WorldBounds の押し出しは Box/Mesh しか見ない）
        ///     ・音も遮らない（CollectOccluders も Box/Mesh しか拾わない）
        ///   ＝ **物理にも音響にも存在していない**。壁を抜けて歩けたのはこれです。
        ///
        /// ★足すのは箱だけにします。MeshCollider を全部に付けると、
        ///   部屋グラフの格子が膨れて重くなるうえ、木の葉 1 枚まで音を遮り始めます。
        ///   箱なら「其処に物が在る」は伝わり、費用も読めます。
        ///
        /// ★除くもの（足すと嘘になる物）
        ///     草・葉・雨 … GPU で描いているだけ。遮る実体は無い
        ///     音源の目印 … 中に入れてしまうので、ぶつかると動けない
        ///     扉の動く板 … 既に EquipRealDoor が付けている（二重に付けない）
        ///     ごく小さい物 … 金具や小枝。数が多く、効果が無い割に格子を汚す
        private static void EquipColliders(GameObject root)
        {
            int added = 0, skipped = 0, tooBig = 0;

            foreach (var r in root.GetComponentsInChildren<MeshRenderer>(true))
            {
                var t = r.transform;
                if (t.GetComponentInParent<SourceMarker>() != null) { skipped++; continue; }
                if (t.GetComponentInParent<InstancedGrassField>() != null) { skipped++; continue; }
                if (t.GetComponentInParent<InstancedCanopy>() != null) { skipped++; continue; }
                if (t.GetComponentInParent<RainVolume>() != null) { skipped++; continue; }
                if (t.GetComponentInParent<PhysicsDoor>() != null) { skipped++; continue; }
                if (t.GetComponent<Collider>() != null) { skipped++; continue; }

                // ごく小さい物は数ばかり増えて効かない。0.25m 未満は置いていく。
                var size = r.bounds.size;
                if (Mathf.Max(size.x, Mathf.Max(size.y, size.z)) < 0.25f) { skipped++; continue; }

                // ★大きい物に**境界箱**を付けてはいけない。
                //
                //   地形・地面・川は凹んだ面なので、境界箱にすると
                //   **中身が全部詰まった巨大な塊**になる。プレイヤーも音源も
                //   その塊の内側に立つことになり、遮蔽の解き方が破綻する
                //  （実際に音響 172.88ms／山 377.9ms＝4fps になった）。
                //   地形は元から地面の当たり判定を持っているので、そちらに任せる。
                if (Mathf.Max(size.x, size.z) > 8f) { tooBig++; continue; }

                var box = t.gameObject.AddComponent<BoxCollider>();
                var lb = r.localBounds;
                box.center = lb.center;
                box.size = lb.size;
                t.gameObject.AddComponent<AddedCollider>();   // 切り分け用の名札（K で入切）
                added++;
            }

            Debug.Log($"[BellGame] {root.name} に当たり判定を {added} 個足した"
                      + $"（{skipped} 個は対象外／{tooBig} 個は大きすぎるので置いた）。\n"
                      + "  ★これで体も音も同じ物にぶつかります。草・葉・雨・音源の目印・扉の板は除いてあります。\n"
                      + "  ★地形のような大きい物は境界箱にすると中身の詰まった塊になるので触りません。\n"
                      + "  ⚠ 足したぶんは**そのまま音の遮蔽物**にもなります。K でまとめて切って比べられます。", root);
        }

        /// 取り込んだ物が使っている「資産になっていないマテリアル」を複製して、
        /// 元のシーンを閉じても消えないようにする。
        ///   ★複製するのは資産**でない**物だけ。資産はそのまま参照を残します。
        private static void RescueSceneMaterials(GameObject root)
        {
            int n = 0;

            Material Keep(Material m)
            {
                if (m == null) return null;
                if (!string.IsNullOrEmpty(AssetDatabase.GetAssetPath(m))) return m;   // 資産はそのまま
                n++;
                return new Material(m) { name = m.name };
            }

            foreach (var r in root.GetComponentsInChildren<Renderer>(true))
            {
                var mats = r.sharedMaterials;
                bool changed = false;
                for (int i = 0; i < mats.Length; i++)
                {
                    var keep = Keep(mats[i]);
                    if (keep != mats[i]) { mats[i] = keep; changed = true; }
                }
                if (changed) r.sharedMaterials = mats;
            }

            // GPU インスタンシングと雨は Renderer を持たず、部品がマテリアルを抱えています。
            foreach (var g in root.GetComponentsInChildren<InstancedGrassField>(true))
                g.material = Keep(g.material);
            foreach (var c in root.GetComponentsInChildren<InstancedCanopy>(true))
                c.material = Keep(c.material);
            foreach (var rn in root.GetComponentsInChildren<RainVolume>(true))
            { rn.nearMaterial = Keep(rn.nearMaterial); rn.farMaterial = Keep(rn.farMaterial); }

            if (n > 0)
                Debug.Log($"[BellGame] {root.name}: シーンが持っていたマテリアル {n} 個を複製した。\n"
                    + "  ★これをしないと、取り込み元のシーンを閉じた瞬間に消えてピンクになります。", root);
        }

        /// ★壁は音を一切通さない。扉の板だけが通す（発注者の指定）。
        ///
        /// ★`transmissionGainDb` では届きません。あれは**全部の透過を一律に**動かすつまみで、
        ///   壁だけ黙らせて扉だけ通す、ができない。面ごとの値が要ります。
        ///
        ///   エンジンは面ごとに `transmissionLossOffsetDb`（−30〜+30dB）を持っているので、
        ///   壁に +30dB 足して実質ゼロにし、扉の板には −6dB で少し多めに通させます。
        ///
        /// ★**吸音率は触りません。**
        ///   `mode = Adjusted` は「プリセットの形は残したまま量だけ動かす」入口なので、
        ///   透過だけ動かして反射の性格はそのまま残せます。
        ///   ここを一緒に動かすと部屋の RT60 が変わって、
        ///   「囲まれている感じ」まで消えます（白い部屋の存在理由がそれです）。
        ///
        /// ★これで扉が**部屋の弱点**になります。
        ///   `WoodDoor` のプリセットの説明そのもの ──「木の扉（壁より弱い＝部屋の弱点。
        ///   閉めていても向こうが聞こえる）」。回り込みは開口が担い、
        ///   板を抜けてくるぶんは扉だけが持つ。壁からは何も来ない。
        private static void SealWalls(GameObject root, WorldDoor door)
        {
            var slab = (door != null && door.door != null) ? door.door.transform : null;
            int walls = 0;

            foreach (var c in root.GetComponentsInChildren<Collider>(true))
            {
                if (c == null) continue;
                // 音響専用の当たり判定（吸音壁など）は元の設定を尊重する。
                if (c.GetComponent<AcousticOnly>() != null) continue;

                var t = c.transform;
                if (slab != null && (t == slab || t.IsChildOf(slab))) continue;   // 扉の板は別扱い

                var s = t.GetComponent<AcousticSurface>();
                if (s == null) s = t.gameObject.AddComponent<AcousticSurface>();
                if (s.mode == AcousticSurfaceMode.Preset) s.mode = AcousticSurfaceMode.Adjusted;
                s.transmissionLossOffsetDb = 30f;     // +30dB ＝ エネルギー 1/1000。実質通さない
                walls++;
            }

            if (slab != null)
            {
                var ds = slab.GetComponent<AcousticSurface>();
                if (ds == null) ds = slab.gameObject.AddComponent<AcousticSurface>();
                ds.material = AcousticMaterialPreset.WoodDoor;
                ds.mode = AcousticSurfaceMode.Adjusted;
                ds.transmissionLossOffsetDb = -6f;    // 割と通す
            }

            Debug.Log($"[BellGame] {root.name}: 壁 {walls} 面を実質不透過にし（+30dB）、"
                      + $"扉の板だけ通すようにした（WoodDoor −6dB）。\n"
                      + "  ★吸音率は触っていないので、部屋の響き（RT60）はそのままです。\n"
                      + "  ★これで扉が**部屋の弱点**になります。回り込みは開口、"
                      + "板を抜けるぶんは扉だけ。壁からは何も来ません。", root);
        }

        /// 負荷検証用に置く音源の数（1 世界あたり）。
        private const int kLoadSources = 10;

        /// ★負荷検証：世界ごとに音源を並べ、**音量 0 で鳴らす**。
        ///
        ///   狙いは「音源が増えたときに何 ms 持っていかれるか」を、
        ///   耳を邪魔されずに測ること。音量 0 でも
        ///     ・エンジンは毎フレーム遮蔽・回折・経路を解く（ここが主な費用）
        ///     ・畳み込み器は回り続ける（DSP の費用もそのまま）
        ///   ので、**費用は本物のまま音だけ消える**。
        ///
        ///   ⚠ 「登録だけして無効にすれば安いのでは」という案が前に出ていたが、
        ///     それだと測りたい費用そのものが消えてしまう。無効化ではなく音量 0。
        ///
        ///   置き方は扉を中心にした輪。片側に固めると、向いた方向で費用が変わって
        ///   読みにくくなる（遮蔽の解き方は方向で変わる）。
        private static void AddLoadSources(GameObject root, Transform listener, WorldDoor door, int count)
        {
            if (count <= 0) return;

            var demo = root.GetComponentInChildren<AcousticFlowSceneDemo>(true);
            if (demo == null) return;

            // ★輪の中心は**扉**。世界の中身の重心ではない。
            //
            //   重心にすると草原では半径 77m・高さ 0.5m になっていた（地形と丘まで
            //   含めた箱の真ん中）。それだと音源が遊ぶ場所の外に並び、しかも地面すれすれ。
            //   測りたいのは「**プレイヤーの周りに音源が何本あると重いか**」なので、
            //   立つ場所（＝扉の前）を中心にしないと、世界ごとに条件が変わってしまう。
            // ★輪は**その世界に収まる大きさ**にする。
            //
            //   10m 固定にしていたら、内寸 8m の白い部屋では全部が壁の中に埋まり、
            //   10 本とも遮蔽 0.92 になった。実測はこうなった:
            //       白い部屋（遮蔽 0.92）… 1 本あたり 17.86 ms
            //       草原  （遮蔽 0.00）… 1 本あたり  1.65 ms
            //   遮られた音源は回折の経路探索が走るので桁が変わる。
            //   **壁に埋めた音源で測ると、いつも最悪値しか出ません。**
            //   世界の広さに合わせておけば、その世界の普通の費用が読めます。
            var b = ContentBounds(root);
            float fit = Mathf.Min(b.extents.x, b.extents.z) * 0.55f;
            float radius = Mathf.Clamp(fit, 2.5f, 10f);
            float y = listener.position.y;
            Vector3 hub = (door != null) ? door.transform.position : listener.position;

            var list = new System.Collections.Generic.List<Transform>();
            if (demo.extraSources != null) list.AddRange(demo.extraSources);

            for (int i = 0; i < count; i++)
            {
                float a = Mathf.PI * 2f * i / count;
                var p = new Vector3(hub.x + Mathf.Cos(a) * radius, y,
                                    hub.z + Mathf.Sin(a) * radius);
                // index は 1 番以降（0 はあなた）。並びがそのままエンジンの音源番号になる。
                var s = MakeSource($"Load_{i:00}", p, list.Count);
                var au = s.GetComponent<AudioSource>();
                au.volume = 0f;          // ★鳴らさない。費用だけ残す
                s.SetParent(root.transform, true);
                list.Add(s);
            }

            demo.extraSources = list.ToArray();
            Debug.Log($"[BellGame] 負荷検証の音源を {count} 本置いた（音量 0）。"
                      + $"この世界の音源は計 {list.Count} 本 ＋ あなた 1 本。\n"
                      + $"  扉を中心にした半径 {radius:F1}m の輪／高さ {y:F1}m。"
                      + $"BellGame/モニター の『実働』と『山の余り』で費用を読んでください。", root);
        }

        /// 子まで含めた見た目の境界を、世界座標で返す。入れ物でも寸法が取れる。
        private static Bounds WorldBoundsOfChildren(Transform t)
        {
            var rs = t.GetComponentsInChildren<Renderer>(true);
            if (rs.Length == 0) return new Bounds(t.position, Vector3.zero);
            var b = rs[0].bounds;
            for (int i = 1; i < rs.Length; i++) b.Encapsulate(rs[i].bounds);
            return b;
        }

        /// 子まで含めた見た目の境界を、その Transform のローカル座標で返す。
        ///   入れ物（自分に Renderer が無い）でも正しい寸法が取れる。
        private static Bounds LocalBoundsOfChildren(Transform t)
        {
            var rs = t.GetComponentsInChildren<Renderer>(true);
            if (rs.Length == 0) return new Bounds(Vector3.zero, Vector3.one);

            var b = new Bounds();
            bool first = true;
            foreach (var r in rs)
            {
                var wb = r.bounds;
                var c = t.InverseTransformPoint(wb.center);
                var e = t.InverseTransformVector(wb.extents);
                e = new Vector3(Mathf.Abs(e.x), Mathf.Abs(e.y), Mathf.Abs(e.z));
                var one = new Bounds(c, e * 2f);
                if (first) { b = one; first = false; } else b.Encapsulate(one);
            }
            return b;
        }

        // いちばん大きい当たり判定＝地面、と見なす。名前で決め打ちしないため。
        private static Collider LargestCollider(GameObject root)
        {
            Collider best = null; float bestArea = 0f;
            foreach (var c in root.GetComponentsInChildren<Collider>(true))
            {
                if (c == null) continue;
                var e = c.bounds.extents;
                float area = e.x * e.z;
                if (area > bestArea) { bestArea = area; best = c; }
            }
            return best;
        }

        // 描画物ぜんぶを含む箱。目印が無いときの立ち位置に使う。
        private static Bounds ContentBounds(GameObject root)
        {
            bool any = false; var b = new Bounds();
            foreach (var r in root.GetComponentsInChildren<Renderer>(true))
            {
                if (r == null) continue;
                if (!any) { b = r.bounds; any = true; } else b.Encapsulate(r.bounds);
            }
            return any ? b : new Bounds(Vector3.zero, Vector3.one);
        }

        // 名前で 1 つ探す（部分一致。造形側が接尾辞を付けても拾えるように）。
        private static Transform FindByName(Transform root, string name)
        {
            foreach (var t in root.GetComponentsInChildren<Transform>(true))
                if (t.name == name) return t;
            foreach (var t in root.GetComponentsInChildren<Transform>(true))
                if (t.name.StartsWith(name)) return t;
            return null;
        }

        // ★既にある扉のモデルに、仕掛けだけ足す。
        //
        //   箱で作り直さない ── 造形レーンが立てた石の門をそのまま扉にする。
        //   「モデルやデザインが変わっても成立するか」が今回の検証の芯なので、
        //   こちらの都合で置き換えたら検証にならない。
        //
        //   足すもの: 板の開閉（PhysicsDoor）／開口（AcousticPortal）／
        //             吸音壁（DoorAperturePanel）／扉の役目（WorldDoor）／敷居。
        private static WorldDoor EquipRealDoor(Transform shrine, Transform listener)
        {
            // ★動く部分は **Ref_DoorSlab**（発注者の指定：その中身がドアの動く部分）。
            //   DoorPanel だけを回すと、框（DoorStile）も金具（DoorBrass）も置き去りになり、
            //   板だけが枠から抜けていく。**回す単位を間違えると、絵も隙間も両方壊れる。**
            var panel = FindByName(shrine, "Ref_DoorSlab") ?? FindByName(shrine, "DoorPanel");
            if (panel == null)
            {
                Debug.LogError($"[BellGame] {shrine.name} の中に Ref_DoorSlab（または DoorPanel）が無い。"
                    + "開く板が要る（開閉と §4.3 の隙間がそこで決まる）。", shrine);
                return null;
            }

            var pd = panel.GetComponent<PhysicsDoor>();
            if (pd == null) pd = panel.gameObject.AddComponent<PhysicsDoor>();
            pd.maxAngle = 140f;   // もっと奥まで開く（発注者の指示。PhysicsDoor の上限）
            if (panel.GetComponent<AcousticSurface>() == null)
                panel.gameObject.AddComponent<AcousticSurface>().material =
                    AcousticMaterialPreset.WoodDoor;

            // ★当たり判定は**子まで含めた見た目**から作る。
            //   Ref_DoorSlab は入れ物なので、自分の Renderer は持っていないことがある。
            //   自分だけ見て作ると 1×1×1 の箱になり、PhysicsDoor の蝶番（板の -X 端）が
            //   まるで違う場所に立つ ── 扉が変な軸で回る。
            var slabBox = panel.GetComponent<BoxCollider>();
            if (slabBox == null) slabBox = panel.gameObject.AddComponent<BoxCollider>();
            var lb = LocalBoundsOfChildren(panel);
            slabBox.center = lb.center;
            slabBox.size = lb.size;

            // ★開口の寸法は**板の見た目から測る**。定数に落とさない。
            //
            // ⚠ ここは一度壊した。`panel.GetComponent<Renderer>()` で取っていたが、
            //   Ref_DoorSlab は入れ物で自分の Renderer を持たないので null になり、
            //   ラボの定数 1.1×2.2 に落ちていた。本番の扉は 1.4×3.0。
            //   その結果、**音響ポータルも敷居のカプセルも扉の中心の高さも**
            //   古い数字で作られていた ── 絵は本番、音は 1.1×2.2 という状態。
            //   子まで含めて測れば、造形レーンが寸法を変えても勝手に追いつく。
            var wb = WorldBoundsOfChildren(panel);
            float h = (wb.size.y > 0.01f) ? wb.size.y : H;
            float w = Mathf.Max(wb.size.x, wb.size.z);
            if (w < 0.01f) w = W;
            Vector3 center = new Vector3(shrine.position.x, wb.center.y, shrine.position.z);

            // ★開口のポータル。これが無いと §4.3 が一度も走らない。
            if (FindByName(shrine, "Portal") == null)
            {
                var pgo = new GameObject("Portal");
                pgo.transform.SetPositionAndRotation(center, shrine.rotation);
                var portal = pgo.AddComponent<AcousticPortal>();
                portal.width = w;
                portal.height = h;
                pgo.transform.SetParent(shrine, true);
            }

            // 扉の横の吸音壁（見えない・α≈0.99）。開口を成立させる。
            if (shrine.GetComponentInChildren<DoorAperturePanel>(true) == null)
            {
                var wallGo = new GameObject("Aperture");
                wallGo.transform.SetPositionAndRotation(shrine.position, shrine.rotation);
                wallGo.AddComponent<DoorAperturePanel>();
                wallGo.transform.SetParent(shrine, true);
            }

            var go = new GameObject("WorldDoor");
            go.transform.SetPositionAndRotation(center, shrine.rotation);
            var wd = go.AddComponent<WorldDoor>();
            wd.door = pd;
            wd.listener = listener;
            // ★敷居のカプセルも測った寸法で作る。既定は 1.1×2.2（ラボの値）なので、
            //   放っておくと本番の扉に**ラボの大きさの見えない栓**が立つ。
            var sill = go.AddComponent<DoorThreshold>();
            sill.worldDoor = wd;
            sill.apertureWidth = w;
            sill.apertureHeight = h;

            go.transform.SetParent(shrine, true);

            Debug.Log($"[BellGame] 本番の扉に仕掛けを付けた: {shrine.name} / 動く部分 {panel.name} / "
                      + $"開口 {w:F2} x {h:F2}m / 中心 {center}\n"
                      + $"  ★寸法は板の見た目から測った値です。1.10 x 2.20 と出ていたら"
                      + $"測れていない（ラボの定数に落ちている）ので言ってください。");
            return wd;
        }

        private static WorldDoor MakeDoor(Vector3 basePos, Transform listener)
        {
            var frame = new GameObject("Door");
            frame.transform.position = basePos;

            Box("PostL", basePos + new Vector3(-(W * 0.5f + T * 0.5f), H * 0.5f, 0f),
                new Vector3(T, H, T)).SetParent(frame.transform, true);
            Box("PostR", basePos + new Vector3(W * 0.5f + T * 0.5f, H * 0.5f, 0f),
                new Vector3(T, H, T)).SetParent(frame.transform, true);
            Box("Lintel", basePos + new Vector3(0f, H + T * 0.5f, 0f),
                new Vector3(W + T * 2f, T, T)).SetParent(frame.transform, true);

            // 板。transform はスケールせず、BoxCollider の size で寸法を持つ。
            var slab = new GameObject("Slab");
            slab.transform.position = basePos + new Vector3(0f, H * 0.5f, 0f);
            var box = slab.AddComponent<BoxCollider>();
            box.size = new Vector3(W, H, 0.08f);
            var vis = GameObject.CreatePrimitive(PrimitiveType.Cube);
            vis.name = "Slab_Visual";
            vis.transform.SetParent(slab.transform, false);
            vis.transform.localScale = new Vector3(W, H, 0.08f);
            Object.DestroyImmediate(vis.GetComponent<Collider>());
            Tint(vis.transform, new Color(0.55f, 0.38f, 0.24f));
            var pd = slab.AddComponent<PhysicsDoor>();
            pd.maxAngle = 140f;   // もっと奥まで開く（発注者の指示。PhysicsDoor の上限）
            slab.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.WoodDoor;
            slab.transform.SetParent(frame.transform, true);

            // 扉の横の吸音壁（見えない・α≈0.99）。開口を成立させる。
            var wallGo = new GameObject("Aperture");
            wallGo.transform.position = basePos;
            wallGo.AddComponent<DoorAperturePanel>();
            wallGo.transform.SetParent(frame.transform, true);

            // ★開口のポータル。これが無いと §4.3 が一度も走らない。
            var pgo = new GameObject("Portal");
            pgo.transform.position = basePos + new Vector3(0f, H * 0.5f, 0f);
            var portal = pgo.AddComponent<AcousticPortal>();
            portal.width = W;
            portal.height = H;
            pgo.transform.SetParent(frame.transform, true);

            var go = new GameObject("WorldDoor");
            go.transform.position = basePos + new Vector3(0f, H * 0.5f, 0f);
            var wd = go.AddComponent<WorldDoor>();
            wd.door = pd;
            wd.listener = listener;

            // 繋がっている扉は歩いて跨げない（E でくぐる）。音響は塞がない。
            var sill = go.AddComponent<DoorThreshold>();
            sill.worldDoor = wd;

            go.transform.SetParent(frame.transform, true);
            return wd;
        }

        // 扉の奥に**本物の部屋を建てた場合**に、MakeDoor が付ける見えない箱を外す。
        //
        // ★DoorAperturePanel は「野原に枠だけで立っている扉」の代役です。
        //   向こう側に何も無いと開口が成立しないので、4×4×3m の閉じた箱を
        //   **実行時に**建てて「閉空間 ⇄ 屋外」に持ち上げている。
        //
        //   ところが扉の奥に本物の部屋を建てると、その箱は
        //   **本物の部屋の内側に入れ子で残る**。しかも材質は α≈0.99（ほぼ完全吸音）で、
        //   開口のすぐ内側を覆う殻になる。
        //
        //     ・箱は |x|≤2.0 / z −4.0〜0 の範囲だけ。部屋の音源はその**外**にある
        //     ・扉を開けると、繋がる先は世界ではなく**この死んだ殻**
        //     ・扉をくぐると、入るのは世界ではなく**この死んだ殻の中**
        //
        //   「開けたら小さくなる」「近寄ったら小さくなる」はこれで説明が付く。
        //   代役は本物が来たら退場する ── それだけの話です。
        //
        // ⚠ Start() で建つので、保存したシーンにも Scene ビューにも現れない。
        //   目で見て気づけない類の物なので、ここに書き残しておく。
        private static void RemoveAperturePanel(WorldDoor door, string why)
        {
            var root = door.transform.root;
            int n = 0;
            foreach (var p in root.GetComponentsInChildren<DoorAperturePanel>(true))
            { Object.DestroyImmediate(p.gameObject); n++; }

            if (n > 0)
                Debug.Log($"[BellGame] 扉の奥の見えない箱（DoorAperturePanel）を {n} 個外した ── {why}");
        }

        private static Transform MakeListener(Vector3 pos, Vector3 lookAt)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = "Listener";
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.5f;
            var d = lookAt - pos; d.y = 0f;
            if (d.sqrMagnitude > 1e-4f) go.transform.rotation = Quaternion.LookRotation(d, Vector3.up);
            return go.transform;
        }

        private static Transform MakeSource(string name, Vector3 pos, int index)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.4f;

            var src = go.AddComponent<AudioSource>();
            src.playOnAwake = false;
            src.loop = true;
            src.spatialBlend = 0f;

            var conv = go.AddComponent<IrConvolver>();
            conv.sourceIndex = index;
            conv.tailLevel = 0.3f;
            conv.generateTestSignal = false;

            var voice = go.AddComponent<VoiceConvolver>();
            voice.sourceIndex = index;
            voice.tailLevel = 0.3f;
            voice.enabled = false;      // 既定は demo.useCppDsp が決める（排他）

            go.AddComponent<PinPosition>();
            go.AddComponent<SourceMarker>();   // 検証用の目印。本物のモデルと区別するための名札
            return go.transform;
        }

        private static Transform Box(string name, Vector3 center, Vector3 size)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            go.transform.position = center;
            go.transform.localScale = size;
            return go.transform;
        }

        private static void Tint(Transform t, Color c)
        {
            var r = t.GetComponent<Renderer>();
            if (r == null) return;
            var sh = Shader.Find("Standard");
            if (sh != null) r.sharedMaterial = new Material(sh) { color = c };
        }

        private static void Save(UnityEngine.SceneManagement.Scene scene, string file, string note)
        {
            const string dir = "Assets/Scenes";
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            EditorSceneManager.SaveScene(scene, dir + "/" + file);
            Debug.Log("[BellGame] 検証シーン: " + file + " — " + note);
        }
    }
}
