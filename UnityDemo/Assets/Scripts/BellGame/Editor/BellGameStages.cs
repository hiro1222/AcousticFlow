// BellGameStages.cs (Editor 専用)
// ステージのブロックアウトを生成する。メニュー: [BellGame > Stages > ...]
//
// 各ステージの役割（音響的な性格）:
//   1 草原 … 基準。遮蔽も残響も無い生の音。直接音 + HRTF だけ
//   2 神殿 … 反響と残響。RT60 約1.4s / 臨界距離 約2.6m
//   3 洞窟 … 回折の方向性。食い違いの開口で直線が通らない。RT60 約5.1s / rc 約0.83m
//   4 雪山 … 乾いた音。早期反射も尾も無く、直接音と稜線の回折だけ
//   （海は保留。潮汐案を採るなら部屋グラフの差分更新がそのまま企画になる）
//
// ★サウンドシステムには一切触らない。
//   既存の AcousticFlow/ 配下は 1 行も変更していない。ここがやるのは
//   「箱を並べて、既存のホスト（AcousticFlowSceneDemo）と畳み込み器を載せる」だけ。
//
// ★見た目は作らない。草案 §8 フェーズ1 のとおりグレーボックスのみ。
//   音のゲームなので最後まで箱で検証できる。
//
// 材質について（重要な制約）:
//   草案 §4.3 は草原の地面に α≈0.3、廃ビル群に α≈0.10〜0.15 を指定しているが、
//   エンジンのプリセットは Default(平均α 0.208) / Concrete(0.038) / Glass(0.058) /
//   WoodDoor(0.088) / Opaque(0.208) の 5 つしかない（material.cpp 実測値）。
//   新しい吸音率を足すにはエンジン側の変更が要るので、既存プリセットで代用している。
//     草原 / 神殿 / 雪山 : Default（α0.208）
//     洞窟               : Concrete（α0.038）＝裸岩。ここだけは残響優位が設計どおり
//     雪山の雪（α0.6〜0.9 相当）: **プリセットが無い**ので早期反射を切って代用している
//   ★Concrete は臨界距離が 1m を切るので、洞窟以外では選ばないこと（仕様書 §7.1）。
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using AcousticFlow;

namespace BellGame.EditorTools
{
    public static class BellGameStages
    {
        // ── ステージ 1：草原 ─────────────────────────────────────────
        // 草案 §4.3: 部屋 0 個 / 手がかりは直接音 + HRTF / 崩しなし（基準）
        //
        // 壁を一枚も置かない。§4.2 のとおり屋外には部屋がないので、部屋グラフは 0 個・
        // 開口 0 個になり、後期残響もほぼ立たない。残るのは
        //   直接音 + HRTF / 地面からの早期反射 / 距離減衰と空気吸収
        // だけ。ここでベルに辿り着けることが、以降すべての世界の基準になる。
        [MenuItem("BellGame/Stages/1 草原 (Grassland)")]
        public static void Stage1Grassland()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -20f));

            // 地面だけ。90m 四方あれば端が聞こえてくることはない。
            // ★音響用の地面は**扉のところで切る**（扉 z=-26）。
            //   つなぎ方式では、行き先の世界がこの裏側へ入ってくる。
            //   はみ出していると二つの世界が重なって埋まる。
            var ground = MakeGround(new Vector3(2f, -0.5f, -3f), new Vector3(50f, 1f, 46f));
            Tint(ground, new Color(0.42f, 0.50f, 0.32f));   // 草地のつもり（見た目は検証しない）
            // 草案 §4.3 が指定する草地 α ≈ 0.30。Default(平均 0.208) を 1.45 倍して当てる。
            // 以前はプリセットが 5 択しかなく 0.208 のままだった（当て木を外した箇所）。
            Adjust(ground, AcousticMaterialPreset.Default, 1.45f);

            // 草案 §2「草原の中に、ドアが一枚だけ立っている」。
            // 次の世界への出口。ベルを鳴らすと現れる想定だが、いまは置いておくだけ。
            var bells = MakePlayerBells();
            var exitDoor = MakeDoorFrame("ExitDoor", new Vector3(0f, 0f, -26f), listener, bells);

            // 世界のベル。27m ほど離し、正面から外して置く（HRTF の左右が効くように）。
            var bellPos = new Vector3(9f, 1.2f, 6f);
            var bell = AddBellStage(listener, bellPos, AcousticMaterialPreset.Default, demo =>
            {
                // ★屋外なので後期残響を切る。
                //   部屋が取れないとき、ホストは「全オクルーダーの外接箱の体積」と
                //   「エコグラムの −60dB を超える最後のビン」で尾の量を作る（フォールバック）。
                //   草原だと外接箱＝地面 90×90 を囲む箱で、根拠が幾何に無い尾が立つ。
                //   草案 §4.2 は「屋外には部屋がない。尾も立たない」なので、切るほうが正しい。
                //   残るのは 直接音 + HRTF / 地面からの早期反射 / 回折 ＝ §4.2 の表そのもの。
                demo.enableReverb = false;
            }, exitDoor: exitDoor,
               // ★手触り「静かだが自然を感じる」。ただし草原は**基準の世界**なので控えめに。
               //   ここは直接音と HRTF だけで探させる設計（§3）で、環境音を厚くすると
               //   難易度が音響の出来ではなく音量バランスで決まってしまう。
               //   風（低域・ベルと帯域が被らない）と、間の空く鳥、の 2 本まで。
               //   ★屋外で遮蔽 0.00 なので、実測上いちばん音源を増やして安い世界でもある。
               ambients: new[]
               {
                   (AmbientKind.Wind, new Vector3(-14f, 3.0f, -2f), 0.30f, 11),
                   (AmbientKind.Bird, new Vector3(16f, 5.0f, 18f), 0.32f, 12),
               });
            bell.world = WorldId.Grassland;
            bell.unlocks = WorldId.Temple;   // 草原のベル → 崩壊都市への鍵
            bell.bells = bells;
            bell.worldDoor = exitDoor;
            BellGamePortalSetup.Attach(exitDoor);   // 扉ごしに次の世界を描く
            AddWorldBounds(listener, ground);   // 世界の外へ出さない（保険。閉じるのは造形の仕事）

            DressByName(("Ground", "Grass_Ground"), ("ExitDoor_Slab", "Wood_Door"), ("ExitDoor", "Stone_Wall"));
            SetupWorld(WorldId.Grassland);
            PlaceVisualMesh("Assets/Models/Prop_Door.obj", "Visual_Door",
                            new Vector3(0f, 0f, -26f), "ExitDoor_Post", "ExitDoor_Lintel");
            PlaceFloatingBell(bellPos);

            Save(scene, "Stage1_Grassland.unity",
                 "草原(基準): 部屋0個・開口0個。Space で呼びかけ、RT60≒0 なので応答は最小遅延 0.4s。" +
                 "手がかりは直接音とHRTFだけ。ここで見つけられなければ以降の世界は成立しない。");
        }

        // ── ステージ 2：崩壊都市 ─────────────────────────────────────
        // 役割: **早期反射で距離を読む**世界と、**環境音**。
        //
        // ★この世界だけの仕掛け: **屋根が残っている建物だけが部屋になる。**
        //   `2e63c46` で「格子の外周に届く成分は部屋にしない。壁だけでは足りず**天井が要る**」
        //   と確定している。だから崩れて屋根が落ちた建物は、壁が立っていても部屋にならず
        //   残響も立たない ── **どの建物が生きているかが音で分かる。**
        //   これは演出として足したものではなく、エンジンの規則からそのまま出てくる。
        //   神殿ではこの対比が作れなかった。
        //
        // 狙う数字（草案 §4.3 が廃ビル群に指定する α 0.10〜0.15 を Adjusted で当てる）:
        //   Default(平均α 0.208) × 0.72 → α ≈ 0.15
        //   屋根ありの建物 12×10×6m = 720m³ → RT60 ≈ 1.5s / 臨界距離 ≈ 1.26m
        //   §7.1 の表の「石膏ボード α0.15 → 臨界距離 1.25m」とぴたり合う。
        //
        // 崩し（§4.3）: ベルの建物の開口は **+X 側だけ**。プレイヤーは -Z から来るので、
        //   直線は -Z 壁で塞がれ、到来方向は開口を指す。音の方へ進むと穴に着く。
        [MenuItem("BellGame/Stages/2 崩壊都市 (Ruins)")]
        public static void Stage2Ruins()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -26f));

            // 地面は扉（z=-32）で切る。裏側は行き先の世界の場所。
            var ground = MakeGround(new Vector3(0f, -0.5f, -1.5f), new Vector3(50f, 1f, 61f));
            Tint(ground, new Color(0.36f, 0.35f, 0.33f));

            var bells = MakePlayerBells();
            var exitDoor = MakeDoorFrame("ExitDoor", new Vector3(0f, 0f, -32f), listener, bells);

            const float dw = 1.1f, dh = 2.4f, t = 0.35f;
            const AcousticMaterialPreset wall = AcousticMaterialPreset.Default;

            // ── 屋根が残っている建物（部屋になる・残響が立つ）──
            Building("BldgA", new Vector3(-13f, 0f, -2f), new Vector2(10f, 8f), 5.0f, t, 0, dw, dh, wall);
            // ★ベルの建物。開口は +X 側だけ。
            Building("BldgB_Bell", new Vector3(8f, 0f, 14f), new Vector2(12f, 10f), 6.0f, t, 3, dw, dh, wall);

            // ── 屋根が落ちた建物（壁はあるが部屋にならない・響かない）──
            Building("BldgC_Open", new Vector3(-6f, 0f, 16f), new Vector2(9f, 9f), 4.5f, t, 0, dw, dh, wall,
                     makeRoof: false);
            Building("BldgD_Open", new Vector3(16f, 0f, -8f), new Vector2(8f, 7f), 4.0f, t, 2, dw, dh, wall,
                     makeRoof: false);

            // 崩れた壁と鉄骨。早期反射を増やして「距離が読める」手がかりを作る（§4.3）。
            MakeWall("Rubble_1", new Vector3(-2f, 1.6f, -18f), new Vector3(9f, 3.2f, 0.5f), wall);
            MakeWall("Rubble_2", new Vector3(12f, 1.2f, -20f), new Vector3(0.5f, 2.4f, 7f), wall);
            MakeWall("Rubble_3", new Vector3(-21f, 2.0f, 6f), new Vector3(0.5f, 4.0f, 10f), wall);
            MakeWall("Beam_1", new Vector3(4f, 2.5f, 2f), new Vector3(0.4f, 5.0f, 0.4f), wall);
            MakeWall("Beam_2", new Vector3(-3f, 2.0f, 8f), new Vector3(0.4f, 4.0f, 0.4f), wall);

            // 草案 §4.3 の α 0.10〜0.15。コンクリの躯体が風化して少し吸うようになった想定。
            AdjustByName(0.72f, "Bldg", "Rubble_", "Beam_");

            // ベルは BldgB の中央。開口が +X 側なので、-Z から来ると方向が嘘をつく。
            var bellPos = new Vector3(8f, 1.2f, 14f);
            var bell = AddBellStage(listener, bellPos, wall, demo =>
            {
                demo.roomSeedRadius = 0.6f;   // 人が通る戸口で割れる
                demo.roomCellSize = 0.25f;
                demo.roomBlendRadius = 2.0f;
            }, outdoorTailGate: true, exitDoor: exitDoor,
               // ★環境音は **2 本から**。実測でフレームコストは音源数ではなく
               //   「遮蔽された音源数」で効くと分かっている（神殿 3本/遮蔽0.00 = 3.30ms、
               //   洞窟 3本/遮蔽0.92 = 14.41ms）。建物だらけの世界では環境音が常に陰に入るので、
               //   まず 2 本で測ってから増やす。
               ambients: new[]
               {
                   // 風。開けた場所に置く（遮蔽が軽い＝安い）。屋外なので尾は立たない
                   (AmbientKind.Wind, new Vector3(-4f, 3.0f, -16f), 0.45f, 21),
                   // 水滴。屋根の残った BldgA の中。**閉じた空間なので残響が乗る**。
                   //   同じ世界で「響く音」と「響かない音」が同時に鳴るので、
                   //   音源ごとのエコグラム（Tier 0）がそのまま効く場面になる
                   (AmbientKind.Drip, new Vector3(-13f, 1.5f, -2f), 0.55f, 37),
                   // ★葉擦れ。「自然に還りつつある廃墟」を音だけで出す担当。
                   //   屋根の落ちた BldgC の側、**建物の外**に置く。
                   //   遮蔽の掛かる位置に置くとコストが跳ねる（上の実測）ので開けた所へ。
                   (AmbientKind.Leaves, new Vector3(-6f, 2.2f, 22f), 0.34f, 53),
               });
            bell.farDistance = 42f;           // 出発点からベルまで約 41m
            bell.world = WorldId.Temple;
            bell.unlocks = WorldId.Cave;     // 崩壊都市のベル → 洞窟への鍵
            bell.bells = bells;
            bell.worldDoor = exitDoor;
            BellGamePortalSetup.Attach(exitDoor);   // 扉ごしに次の世界を描く
            AddWorldBounds(listener, ground);   // 世界の外へ出さない（保険。閉じるのは造形の仕事）

            DressByName(("Bldg", "Stone_Wall"), ("Rubble_", "Stone_Wall"), ("Beam_", "Stone_Roof"),
                        ("Ground", "Temple_Ground"),
                        ("ExitDoor_Slab", "Wood_Door"), ("ExitDoor", "Stone_Wall"));
            SetupWorld(WorldId.Temple);
            // 火鉢は手前だけ。**ベルのいる BldgB（z=14 付近）には置かない** ──
            // 灯りで在り処が見えたら、音で探すゲームが成立しない。
            PlaceBraziers(new Vector3(-13f, 0f, -2f), new Vector3(2f, 0f, -12f));
            PlaceVisualMesh("Assets/Models/Prop_Door.obj", "Visual_Door",
                            new Vector3(0f, 0f, -32f), "ExitDoor_Post", "ExitDoor_Lintel");
            PlaceFloatingBell(bellPos);

            Save(scene, "Stage2_Ruins.unity",
                 "崩壊都市(早期反射と環境音): 屋根が残った建物だけが部屋になり残響が立つ。" +
                 "屋根が落ちた建物は壁があっても響かない ── どれが生きているかが音で分かる。" +
                 "ベルは BldgB_Bell の中、開口は +X 側だけなので -Z から来ると到来方向が開口を指す(§4.3)。" +
                 "環境音は風(屋外)と水滴(屋内)の2本。響く音と響かない音が同時に鳴る。" +
                 "α0.15 で RT60 約1.5s・臨界距離 約1.26m。");
        }

        // ── ステージ 3：洞窟 ─────────────────────────────────────────
        // 役割: **回折の方向性**と、残響優位の世界。
        //
        // 草案 §4.3 の「嘘」がいちばん強く出る世界。仕切りごとに開口を左右へずらしてあるので、
        // 直線はどこも通らず、音は開口を 2〜3 回まわり込むしかない。
        // AF_SceneGetSourceArrivalDir が返すのは**いちばん近い開口の方向**になり、
        // 音の方へ歩くとベルではなく穴に着く。演出ではなく、これが物理的に正しい答え。
        //
        // 狙う数字（Sabine 概算・裸岩 Concrete α0.038）:
        //   1 室 24×10×4.5m ≒ 1080m³ → RT60 ≈ 5.1s / 臨界距離 ≈ 0.83m
        //   草案 §4.3 の見積り（RT60 ≈ 6s / rc ≈ 1.0m）とほぼ一致する。
        //   1m 離れれば残響優位＝直接音がほとんど残らない。**ここは設計どおりの難しさ**で、
        //   神殿と違って「定位が立たない」ことがそのまま難易度になる。
        [MenuItem("BellGame/Stages/3 洞窟 (Cave)")]
        public static void Stage3Cave()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -22f));

            // 地面は扉（z=-28）で切る。裏側は行き先の世界の場所。
            var ground = MakeGround(new Vector3(0f, -0.5f, 6f), new Vector3(44f, 1f, 68f));
            Tint(ground, new Color(0.22f, 0.21f, 0.20f));

            var bells = MakePlayerBells();
            var exitDoor = MakeDoorFrame("ExitDoor", new Vector3(0f, 0f, -28f), listener, bells);

            // 開口 0.9m ＝ 人がやっと通る幅。RoomSeedRadius 0.6 の 2 倍(1.2m)を下回るので
            // 確実に部屋が割れる。狭いほど回折の方向性が強く出る。
            const float gw = 0.9f, gh = 2.2f, t = 0.5f;
            const AcousticMaterialPreset rock = AcousticMaterialPreset.Concrete;   // 裸岩

            // 外殻。内寸 24×40m・高さ 4.5m。入口は -Z の中央。
            Building("Cave", new Vector3(0f, 0f, 6f), new Vector2(24f, 40f), 4.5f, t, 0, gw, gh, rock);

            // ★仕切り 3 枚。開口を -8 / +8 / -8 と振って食い違いにする。
            //   入口(x=0) から見て、どの仕切りも直線では通れない。
            PartitionX("Wind1", -6f, -(12f + t), 12f + t, 4.5f, t, -8f, gw, gh, rock);
            PartitionX("Wind2", 4f, -(12f + t), 12f + t, 4.5f, t, 8f, gw, gh, rock);
            PartitionX("Wind3", 14f, -(12f + t), 12f + t, 4.5f, t, -8f, gw, gh, rock);

            // 岩塊。稜線を増やして回折の経路を複雑にする。
            MakeWall("Rock_1", new Vector3(4f, 1.4f, -10f), new Vector3(3f, 2.8f, 2.5f), rock);
            MakeWall("Rock_2", new Vector3(-6f, 1.2f, 0f), new Vector3(2.5f, 2.4f, 3f), rock);
            MakeWall("Rock_3", new Vector3(2f, 1.6f, 10f), new Vector3(3.5f, 3.2f, 2f), rock);

            // ベルは最奥の部屋。入口から仕切り 3 枚ぶん奥。
            var bellPos = new Vector3(8f, 1.2f, 21f);
            var bell = AddBellStage(listener, bellPos, rock, demo =>
            {
                demo.roomSeedRadius = 0.6f;
                demo.roomCellSize = 0.25f;
                demo.roomBlendRadius = 2.0f;

                // ★洞窟だけ重い。実測 14.41ms/frame（神殿は 3.30ms）。
                //   原因は「3 音源すべてが遮蔽 0.92」＝全部が壁の裏で、音源ごとの段
                //   （反射込み遮蔽・早期反射・回折二次音源）が 3 本ぶん全開で回ること。
                //   洞窟の性格を壊さない所だけ削る:
                //     ・早期反射のレイを半分。ここは rc 0.83m の残響優位な世界で、
                //       早期反射は元々主役ではない（§4.3 の設計どおり）
                //     ・残響の更新間隔を伸ばす。RT60 が 5s あるので追従は鈍くてよい
                //   ★エコグラムのレイと反射回数は**減らさない**。長い尾がこの世界の主題で、
                //     そこを削ると難易度そのものが変わる。
                demo.earlyReflectRays = 256;
                demo.reverbUpdateEveryFrames = 6;
            }, outdoorTailGate: true, exitDoor: exitDoor,
               // ★洞窟だけは 1 本しか足さない。
               //   実測 14.41ms / 56fps（音源 3 本・遮蔽 0.92）で、**いちばん高い世界**。
               //   コストは遮蔽された音源数で効くので、ここで増やすと真っ先に落ちる。
               //   水滴は 1 本でも RT60 5.1s の尾が全部乗るので、量は要らない。
               ambients: new[]
               {
                   (AmbientKind.Drip, new Vector3(6f, 2.4f, 26f), 0.40f, 31),
               });
            bell.farDistance = 45f;           // 洞口の外から最奥まで約 44m
            bell.world = WorldId.Cave;
            bell.unlocks = WorldId.Snow;     // 洞窟のベル → 雪山への鍵
            bell.bells = bells;
            bell.worldDoor = exitDoor;
            BellGamePortalSetup.Attach(exitDoor);   // 扉ごしに次の世界を描く
            AddWorldBounds(listener, ground);   // 世界の外へ出さない（保険。閉じるのは造形の仕事）

            DressByName(("Cave_", "Cave_Rock"), ("Wind", "Cave_Rock"), ("Rock_", "Cave_Rock"),
                        ("Ground", "Cave_Ground"), ("ExitDoor_Slab", "Wood_Door"), ("ExitDoor", "Stone_Wall"));
            SetupWorld(WorldId.Cave);
            // 火鉢は手前 3 室だけ。**ベルのいる最奥（z > 14）は暗いままにする。**
            // 洞窟は開口が食い違っているので手前からは見通せず、
            // 灯りがあっても在り処は分からない ── ただし奥まで照らすと台無しになる。
            PlaceBraziers(new Vector3(-9f, 0f, -10f), new Vector3(9f, 0f, -1f),
                          new Vector3(-9f, 0f, 8f));
            // 岩肌は内向きに押し出してあるので箱の内側に収まる。地面(屋外)は隠さない ──
            // 洞内の床だけ 2cm 浮かせて、地面との Z ファイティングを避ける。
            PlaceVisualMesh("Assets/Models/Stage3_Cave.obj", "Visual_Cave",
                            new Vector3(0f, 0.02f, 0f), "Cave_", "Wind", "Rock_");
            PlaceVisualMesh("Assets/Models/Prop_Door.obj", "Visual_Door",
                            new Vector3(0f, 0f, -28f), "ExitDoor_Post", "ExitDoor_Lintel");
            PlaceFloatingBell(bellPos);

            Save(scene, "Stage3_Cave.unity",
                 "洞窟(回折の方向性): 仕切り3枚の開口を -8/+8/-8 と振ってあるので直線はどこも通らない。" +
                 "到来方向は必ず**いちばん近い開口**を指す。音の方へ進むと穴に着く ── " +
                 "これが §4.3 の『嘘』で、演出ではなく物理的に正しい挙動。" +
                 "裸岩(α0.038)で RT60 約5.1s・臨界距離 約0.83m。1m 離れれば残響優位。");
        }

        // ── ステージ 4：雪山 ─────────────────────────────────────────
        // 役割: **乾いた音**。手がかりの喪失。
        //
        // 草案 §4.2 の訂正どおり、屋外なので部屋は 0 個・尾も立たない。
        // そのうえで雪は高域をよく吸うので**地面と斜面からの早期反射が消える**。
        // 残るのは 直接音 + HRTF と、稜線の回折だけ。
        //
        // ⚠ 雪の吸音率(α 0.6〜0.9)に相当するプリセットが無い（最大が Default の 0.208）。
        //   新設には material.cpp ＝ エンジン側の変更が要るので、いまは
        //   **早期反射そのものを切って代用**している。
        //   狙っている結果（早期反射が消え、直接音と回折だけが残る）は §4.2 の表と一致するが、
        //   物理ではなくスイッチで作っているので、材質が入ったら差し替えること。
        [MenuItem("BellGame/Stages/4 雪山 (Snow)")]
        public static void Stage4Snow()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -34f));

            // 地面は扉（z=-40）で切る。裏側は行き先の世界の場所。
            var ground = MakeGround(new Vector3(0f, -0.5f, 0f), new Vector3(64f, 1f, 80f));
            Tint(ground, new Color(0.82f, 0.84f, 0.88f));

            var bells = MakePlayerBells();
            var exitDoor = MakeDoorFrame("ExitDoor", new Vector3(0f, 0f, -40f), listener, bells);

            const AcousticMaterialPreset snow = AcousticMaterialPreset.Default;

            // ★積雪。AcousticSurfaceMode.Custom で 6 帯域を直に入れる。
            //   高域ほど極端に吸うのが雪の性格で、プリセットの倍率では作れない形。
            //   これが効くと、地面と斜面からの早期反射が**高域から順に消える**。
            //   §4.2 の「雪山の正しい難易度理由」がそのまま物理で出る。
            //     以前は雪の材質が作れず enableEarlyReflections=false で代用していた
            //     （早期反射を丸ごと止めるスイッチ）。当て木を外した箇所。
            float[] snowAlpha = { 0.30f, 0.55f, 0.75f, 0.85f, 0.90f, 0.92f };
            float[] snowTl = { 40f, 40f, 40f, 40f, 40f, 40f };   // 下は地面。抜けは考えない
            float[] snowScatter = { 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.80f };
            Custom(ground, snowAlpha, snowTl, snowScatter);

            // 稜線。回折で方向を曲げる唯一の手がかりになるので、
            // ベルとプレイヤーの間に必ず 1 枚は挟まるように置く。雪を被っている。
            var r1 = MakeWall("Ridge_1", new Vector3(-10f, 3.0f, -12f), new Vector3(40f, 6.0f, 1.5f), snow);
            var r2 = MakeWall("Ridge_2", new Vector3(14f, 4.0f, 6f), new Vector3(1.5f, 8.0f, 30f), snow);
            var r3 = MakeWall("Ridge_3", new Vector3(-4f, 2.5f, 24f), new Vector3(26f, 5.0f, 1.5f), snow);
            foreach (var r in new[] { r1, r2, r3 }) Custom(r, snowAlpha, snowTl, snowScatter);

            // 露岩。雪から出ているので吸わない。ここだけ反射が返るのが手がかりになる。
            MakeWall("Rock_1", new Vector3(6f, 1.0f, -20f), new Vector3(3f, 2.0f, 3f),
                     AcousticMaterialPreset.Concrete);
            MakeWall("Rock_2", new Vector3(-14f, 1.2f, 4f), new Vector3(2.5f, 2.4f, 2.5f),
                     AcousticMaterialPreset.Concrete);

            // ベルは Ridge_2 の向こう側。直線は稜線で切れる。
            var bellPos = new Vector3(20f, 1.2f, 14f);
            var bell = AddBellStage(listener, bellPos, snow, demo =>
            {
                // 屋外なので後期残響を切る（草原と同じ理由。フォールバックの尾を使わせない）。
                demo.enableReverb = false;
                // ★早期反射は**切らない**。雪の吸音が高域から順に食うので、
                //   「乾いている」は物理の結果として出る。露岩の近くだけ反射が返る。
            }, exitDoor: exitDoor,
               // 風だけ。雪山は「手がかりの喪失」が主題（§3）なので、
               //   環境音まで豊かにすると喪失感が消える。屋外なので遮蔽は軽い。
               ambients: new[]
               {
                   (AmbientKind.Wind, new Vector3(-10f, 4.0f, 20f), 0.42f, 41),
               });
            bell.farDistance = 55f;           // 出発点からベルまで約 52m
            bell.world = WorldId.Snow;
            bell.unlocks = WorldId.Sea;      // 雪山のベル → 海への鍵
            bell.bells = bells;
            bell.worldDoor = exitDoor;
            BellGamePortalSetup.Attach(exitDoor);   // 扉ごしに次の世界を描く
            AddWorldBounds(listener, ground);   // 世界の外へ出さない（保険。閉じるのは造形の仕事）

            DressByName(("Ridge_", "Snow_Ridge"), ("Rock_", "Snow_Rock"),
                        ("Ground", "Snow_Ground"), ("ExitDoor_Slab", "Wood_Door"), ("ExitDoor", "Stone_Wall"));
            SetupWorld(WorldId.Snow);
            // 雪原は生成メッシュ側が地面を持つので、箱の地面は描画を切る（コライダーは残る）。
            PlaceVisualMesh("Assets/Models/Stage4_Snow.obj", "Visual_Snow",
                            "Ridge_", "Rock_", "Ground");
            PlaceVisualMesh("Assets/Models/Prop_Door.obj", "Visual_Door",
                            new Vector3(0f, 0f, -40f), "ExitDoor_Post", "ExitDoor_Lintel");
            PlaceFloatingBell(bellPos);

            Save(scene, "Stage4_Snow.unity",
                 "雪山(乾いた音): 部屋0個・尾なし。雪 α 0.30〜0.92（高域ほど吸う）を Custom で入れてあるので、" +
                 "早期反射が高域から順に消える。残るのは直接音+HRTFと稜線の回折、それに露岩からの反射だけ。" +
                 "ベルは Ridge_2 の向こうで直線は切れている。近距離まで詰めないと分からない。");
        }

        // ────────────────────────────────────────────────────────────
        // 生成の部品
        // ────────────────────────────────────────────────────────────

        // 内寸 inner(x,z)・高さ h・厚み t の箱状の建物。屋根あり・床なし（地面と二重計上しない）。
        // doorSide の面の中央に幅 dw・高さ dh の戸口を空ける。
        //   doorSide: 0=-Z / 1=+Z / 2=-X / 3=+X
        private static void Building(string prefix, Vector3 center, Vector2 inner,
                                     float h, float t, int doorSide, float dw, float dh,
                                     AcousticMaterialPreset wallMat, int doorSide2 = -1,
                                     bool makePortal = true, bool makeRoof = true)
        {
            float hx = inner.x * 0.5f, hz = inner.y * 0.5f;

            for (int side = 0; side < 4; side++)
            {
                bool alongX = side < 2;                       // -Z/+Z の壁は X 方向に伸びる
                float sign = (side % 2 == 0) ? -1f : 1f;
                Vector3 wc = center + (alongX
                    ? new Vector3(0f, h * 0.5f, sign * (hz + t * 0.5f))
                    : new Vector3(sign * (hx + t * 0.5f), h * 0.5f, 0f));
                Vector3 ws = alongX
                    ? new Vector3(inner.x + t * 2f, h, t)
                    : new Vector3(t, h, inner.y);

                if (side != doorSide && side != doorSide2)
                { MakeWall(prefix + "_W" + side, wc, ws, wallMat); continue; }

                // 戸口のある面は「左の袖・右の袖・まぐさ」に割る。
                float span = alongX ? ws.x : ws.z;
                float seg = (span - dw) * 0.5f;
                if (seg > 0.01f)
                {
                    for (int k = 0; k < 2; k++)
                    {
                        float off = (k == 0 ? -1f : 1f) * (dw + seg) * 0.5f;
                        Vector3 c2 = wc + (alongX ? new Vector3(off, 0f, 0f)
                                                  : new Vector3(0f, 0f, off));
                        Vector3 s2 = alongX ? new Vector3(seg, h, t)
                                            : new Vector3(t, h, seg);
                        MakeWall(prefix + "_W" + side + "_" + k, c2, s2, wallMat);
                    }
                }
                float lintelH = h - dh;
                if (lintelH > 0.01f)
                {
                    Vector3 lc = new Vector3(wc.x, (dh + h) * 0.5f, wc.z);
                    Vector3 ls = alongX ? new Vector3(dw, lintelH, t)
                                        : new Vector3(t, lintelH, dw);
                    MakeWall(prefix + "_W" + side + "_lintel", lc, ls, wallMat);
                }

                // ★開口にポータルを置く。ここが「音が抜けてくる」の当の仕組み。
                //   §4.3: ポータルがあるシーンでは、開口経由の音は**ポータルだけ**が決める
                //   （portalOpenBands）。フレネル半径 r1=√(λd1d2/(d1+d2)) が波長に依るので
                //   帯域ごとに開口率が変わり、低域は狭い隙間でも通り高域は塞がれる。
                //   置かないと戸口はただの幾何の隙間で、一般の回折としてしか扱われない
                //   ＝「抜けてくる」音色にならない。
                // 同じ開口に別のポータルが既にある場合は作らない（二重に測ることになる）。
                if (!makePortal) continue;
                var pgo = new GameObject(prefix + "_Portal");
                pgo.transform.position = new Vector3(wc.x, dh * 0.5f, wc.z);
                // forward が開口の法線。X 方向を向く壁は Y 軸に 90 度回す
                //   （Euler(0,90,0) で forward=+X / right=-Z / up=+Y。矩形なので符号は問わない）。
                pgo.transform.rotation = alongX ? Quaternion.identity
                                                : Quaternion.Euler(0f, 90f, 0f);
                var portal = pgo.AddComponent<AcousticPortal>();
                portal.width = dw;
                portal.height = dh;
            }

            // 屋根。閉じた空間にしないと後期残響が立たない（開口＝完全吸音として抜けてしまう）。
            // ★屋根が無ければ部屋にならない（`2e63c46`：格子の外周に届く成分は部屋にしない）。
            //   崩壊都市では「屋根が残っている建物だけが響く」という対比を、
            //   演出ではなくこの規則から作る。
            if (!makeRoof) return;
            MakeWall(prefix + "_Roof", center + new Vector3(0f, h + t * 0.5f, 0f),
                     new Vector3(inner.x + t * 2f, t, inner.y + t * 2f), wallMat);
        }

        // 材質を明示した壁。AcousticSurface はグローバル既定より優先される
        // （解決順は AcousticSurface > CollArea のエリア素材 > occluderMaterial）。
        private static Transform MakeWall(string name, Vector3 center, Vector3 size,
                                          AcousticMaterialPreset mat)
        {
            var t = MakeBox(name, center, size);
            t.gameObject.AddComponent<AcousticSurface>().material = mat;
            return t;
        }

        // プリセットの「形」は残して量だけ動かす（AcousticSurfaceMode.Adjusted）。
        //   決めごと #3「形は物理から採り、絶対値は演出で決める」の入口。
        //   absorptionScale : 吸音率を何倍にするか
        //   tlOffsetDb      : 透過損失に足す dB（+ で遮る / − で漏れる）
        private static void Adjust(Transform t, AcousticMaterialPreset preset,
                                   float absorptionScale, float tlOffsetDb = 0f)
        {
            var s = t.gameObject.GetComponent<AcousticSurface>();
            if (s == null) s = t.gameObject.AddComponent<AcousticSurface>();
            s.mode = AcousticSurfaceMode.Adjusted;
            s.material = preset;
            s.absorptionScale = absorptionScale;
            s.transmissionLossOffsetDb = tlOffsetDb;
        }

        // 名前の前置きが一致するものをまとめて Adjusted にする。
        //   Building が中で大量の壁を作るので、後から一括で量を動かすための入口。
        private static void AdjustByName(float absorptionScale, params string[] prefixes)
        {
            foreach (var s in Object.FindObjectsByType<AcousticSurface>(FindObjectsSortMode.None))
            {
                foreach (string p in prefixes)
                {
                    if (!s.gameObject.name.StartsWith(p)) continue;
                    s.mode = AcousticSurfaceMode.Adjusted;
                    s.absorptionScale = absorptionScale;
                    break;
                }
            }
        }

        // 6 帯域を直に指定する（AcousticSurfaceMode.Custom）。
        //   形そのものを変えたいとき用。雪のように高域だけ極端に吸う材質は
        //   プリセットの倍率では作れないので、こちらを使う。
        //   帯域は 125 / 250 / 500 / 1k / 2k / 4k Hz。
        private static void Custom(Transform t, float[] alpha, float[] tlDb, float[] scatter)
        {
            var s = t.gameObject.GetComponent<AcousticSurface>();
            if (s == null) s = t.gameObject.AddComponent<AcousticSurface>();
            s.mode = AcousticSurfaceMode.Custom;
            s.customAbsorption = alpha;
            s.customTransmissionLossDb = tlDb;
            s.customScattering = scatter;
        }

        // 世界へ繋がる扉。最初から在り、開け閉めできる。
        //
        // ★板は SwingDoor で蝶番まわりに回る。BoxCollider を持つ普通の障害物なので、
        //   デモが毎フレーム transform をエンジンへ流すだけで音響に乗る（決めごと #4：
        //   扉を特別視しない）。角度から音のパラメータを作る処理はどこにも書かない。
        // ★枠の内側にポータルを置く。§4.3 のフレネル帯域積分は**ポータルがあるときだけ**
        //   効くので、これが無いと「開き具合が音色に出る」が一度も走らない。
        // ★蝶番の Y は扉の中心の高さにすること（SwingDoor の注記）。部屋の高さの半分を
        //   入れると扉が浮き、下に隙間が残って音が漏れる。
        private static WorldDoor MakeDoorFrame(string prefix, Vector3 basePos,
                                               Transform listener, PlayerBells bells)
        {
            const float w = 1.1f, h = 2.2f, t = 0.25f;

            MakeBox(prefix + "_PostL", basePos + new Vector3(-(w * 0.5f + t * 0.5f), h * 0.5f, 0f),
                    new Vector3(t, h, t));
            MakeBox(prefix + "_PostR", basePos + new Vector3(w * 0.5f + t * 0.5f, h * 0.5f, 0f),
                    new Vector3(t, h, t));
            MakeBox(prefix + "_Lintel", basePos + new Vector3(0f, h + t * 0.5f, 0f),
                    new Vector3(w + t * 2f, t, t));

            // ★物理の扉（草案 §4.4 の Visage 方式）。掴んで押し引きする。
            //   キーで 0°⇄85° を往復させるトグルにしないのは、それだと
            //   プレイヤーが**途中の角度に留まらない**から。隙間 5cm と 15cm の音色差は
            //   §4.3 の主張そのものなのに、トグルでは一度も体験されない。
            // ★transform はスケールしない。BoxCollider の size で寸法を持つ。
            //   スケールした transform に子を付けると、見た目のメッシュが潰れるため。
            var slabGo = new GameObject(prefix + "_Slab");
            slabGo.transform.position = basePos + new Vector3(0f, h * 0.5f, 0f);
            var box = slabGo.AddComponent<BoxCollider>();
            box.size = new Vector3(w, h, 0.08f);

            // ★Rigidbody と HingeJoint は置かない。
            //   関節に力を掛ける方式は、力を掛けても動かない／運動学へ落とすと
            //   勝手に閉じる、の往復から抜けられなかった。原因は角度の答えが
            //   3 箇所（関節の解・運動学の積算・表示値）にあったこと。
            //   いまは PhysicsDoor.angleDeg ただ 1 つが答えで、姿勢はそこから焼く。
            //
            //   ★音は何も変わらない。§4.3 が要求するのは
            //     「角度が連続で、途中で止まれること」だけで、
            //     その角度が関節の解であることではない。
            //     エンジンは毎フレーム transform を読む（決めごと #4：扉を特別視しない）。
            var pd = slabGo.AddComponent<PhysicsDoor>();
            pd.maxAngle = 95f;

            slabGo.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.WoodDoor;

            // 板の見た目（框組みの木扉）を子として付ける。コライダーは剥がす。
            var slabSrc = AssetDatabase.LoadAssetAtPath<GameObject>("Assets/Models/Prop_DoorSlab.obj");
            if (slabSrc != null)
            {
                var vis = Object.Instantiate(slabSrc, slabGo.transform);
                vis.name = "Slab_Visual";
                vis.transform.localPosition = Vector3.zero;
                vis.transform.localRotation = Quaternion.identity;
                foreach (var c in vis.GetComponentsInChildren<Collider>()) Object.DestroyImmediate(c);
                foreach (var r in vis.GetComponentsInChildren<Renderer>())
                {
                    var mats = r.sharedMaterials;
                    for (int i = 0; i < mats.Length; i++)
                    {
                        if (mats[i] == null) continue;
                        var mm = BellGameDressing.Load(mats[i].name.Replace(" (Instance)", "").Trim());
                        if (mm != null) mats[i] = mm;
                    }
                    r.sharedMaterials = mats;
                }
            }

            // ★扉の**向こう側**に、見えない吸音の設え（α≈0.99）。
            //
            //   現世にはドアがあるだけ。壁も袖も足さない（発注者の指示）。
            //   接続世界の側が、そのドアを開口として音を取り込む ── という立ち位置。
            //
            //   これが無いと屋外 ⇄ 屋外で「開口」が成立せず、音が脇から素通しで回る。
            //   実際いま、扉を開けても閉めても大差が無い ──
            //   §0 の芯（隙間が狭いほど低域だけ通る）が、最初の扉で一度も動いていない。
            //
            //   吸音率をほぼ 1 にしてあるので**何も返さない**。遮るだけで、鳴らさない。
            //   だから §7.2 の「見えないコライダーは幻の反射を返す」に当たらない。
            var wallGo = new GameObject(prefix + "_Aperture");
            wallGo.transform.position = basePos;
            wallGo.AddComponent<DoorAperturePanel>();

            var pgo = new GameObject(prefix + "_Portal");
            pgo.transform.position = basePos + new Vector3(0f, h * 0.5f, 0f);
            pgo.transform.rotation = Quaternion.identity;   // forward=+Z が開口の法線
            var portal = pgo.AddComponent<AcousticPortal>();
            portal.width = w;
            portal.height = h;

            var go = new GameObject(prefix);
            go.transform.position = basePos + new Vector3(0f, h * 0.5f, 0f);
            var wd = go.AddComponent<WorldDoor>();
            wd.door = pd;
            wd.listener = listener;
            wd.bells = bells;

            // ★繋がっている扉は歩いて跨げない（E でくぐる）。
            //   繋がっていない扉はただの扉なので素通りできる。
            //   カプセルで作ってあるので**音響は塞がない**（Box/Mesh しか拾われない）。
            var sill = go.AddComponent<DoorThreshold>();
            sill.worldDoor = wd;
            return wd;
        }

        // 聴き比べ用の持続音。
        //
        // ⚠ **市販楽曲なので配布物には入れられない**（仕様書 §7.6・§9）。
        //   `*.wav` はコミットしない。聴き取りが済んだら Inspector で外すか、
        //   ここを null 返しにしてベルの合成音へ戻すこと。
        private const string kListeningClip =
            "Assets/Audio/ロクデナシ「ブリザード」 Rokudenashi - Blizzard【Official Music Video】"
            + " - Rokudenashi (128k).wav";

        // ★既定は**使わない**。
        //   持続音は響きの聴き比べには向くが、いまは
        //     ・拾う前は断片、拾った後は自分のベル、という入れ替わり（§0）
        //     ・断片＝倍音 1 本、集めると音が育つ（§2）
        //   が主題なので、鳴りっぱなしの楽曲はそれを丸ごと潰す。
        //   加えて**市販楽曲なので配布物に入れられない**（仕様書 §7.6）。
        //   聴き比べたいときだけ true にすること。
        private const bool kUseListeningClip = false;

        private static AudioClip LoadListeningClip()
        {
            if (!kUseListeningClip) return null;

            var c = AssetDatabase.LoadAssetAtPath<AudioClip>(kListeningClip);
            if (c == null) c = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (c == null)
                Debug.LogWarning("[BellGame] 聴き比べ用の音源が見つかりません。"
                                 + "世界のベルは合成音のままになります。");
            return c;
        }

        // 手持ちのベル入れ。集めたベルが扉の行き先になる。
        private static PlayerBells MakePlayerBells()
            => new GameObject("PlayerBells").AddComponent<PlayerBells>();

        // 音源 2 本（§4.5 の音源構成）とホスト、呼びかけ／応答を載せる。
        //   src[0] プレイヤーのベル（リスナーに追従）
        //   src[1] 世界のベル
        private static BellCallResponse AddBellStage(Transform listener, Vector3 worldBellPos,
                                                     AcousticMaterialPreset material,
                                                     System.Action<AcousticFlowSceneDemo> tweak,
                                                     bool outdoorTailGate = false,
                                                     WorldDoor exitDoor = null,
                                                     (AmbientKind kind, Vector3 pos, float vol, int seed)[] ambients = null)
        {
            // ★リスナーと完全に同一座標に置かない。
            //   残響/直接 = (r/rc)² が 0 になって尾が消える（無指向の点音源に耳を密着させた
            //   状態＝物理的に正しい帰結）。既存の Test_RoomEntry と同じ 1.5m 前方に置く。
            // ★ベルは**手の中**。前方 1.5m から 0.3m へ寄せた（発注者の決定）。
            //
            //   決定は「ベルの音量はずっと同じ。歩き回っていろんな場所で試させたいから」。
            //   鳴らす音が場所で変わると、返ってきた違いが**場所のせいか立ち位置のせいか
            //   分からなくなります。** 鳴らす音は同じ、返る音だけが場所を語る ── それが芯。
            //
            // ⚠ 1.5m のままだと壁際で壊れます。
            //   プレイヤーの太さは 0.35m（WorldBounds.bodyRadius）なので、壁に体を寄せると
            //   **ベルだけが壁の 1.15m 内側**に入り、遮蔽されて自分のベルが黙ります
            //  （壁を +30dB で塞いだ構成では、ほぼ無音）。
            //   体の太さの内側に置けば、どこへ行っても遮る物が挟まりません。
            //
            //   物理的にもこちらが正しい ── ベルは手の中にあって、1.5m 先に浮いていません。
            var followOffset = new Vector3(0f, 0f, 0.3f);
            var player = MakeSource("Bell_Player", listener.position + followOffset, 0,
                                    new Color(0.9f, 0.85f, 0.4f));
            var world = MakeSource("Bell_World", worldBellPos, 1,
                                   new Color(0.9f, 0.45f, 0.35f));

            // ★呼びかけが Space なので、デモ側の「音源を1点に重ねる」トグルと衝突する。
            //   世界のベルは位置を固定して押さえ返す（詳細は PinPosition.cs）。
            //   プレイヤーのベルは FollowTransform が同じ LateUpdate で書き戻すので不要。
            world.gameObject.AddComponent<PinPosition>();

            var follow = player.gameObject.AddComponent<FollowTransform>();
            follow.target = listener;
            follow.offset = followOffset;

            var go = new GameObject("AcousticFlowSceneDemo");
            var demo = go.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = player;
            // 環境音。音源 index は 2 番以降。
            //   ★遮蔽された音源はコストが高い（実測: 神殿 3本/遮蔽0.00 = 3.30ms、
            //     洞窟 3本/遮蔽0.92 = 14.41ms）。増やす前に必ず測ること。
            var extra = new System.Collections.Generic.List<Transform> { world };
            var events = new System.Collections.Generic.List<string> { "PlayerBell", "WorldBell" };
            if (ambients != null)
            {
                for (int i = 0; i < ambients.Length; i++)
                {
                    var a = ambients[i];
                    var asrc = MakeSource("Ambient_" + a.kind, a.pos, 2 + i,
                                          new Color(0.45f, 0.75f, 0.55f));
                    asrc.gameObject.AddComponent<PinPosition>();
                    var amb = asrc.gameObject.AddComponent<AmbientSource>();
                    amb.kind = a.kind;
                    amb.volume = a.vol;
                    amb.seed = a.seed;
                    extra.Add(asrc);
                    events.Add("Ambient" + i);
                }
            }

            // ★扉の奥。次の世界の環境音が 1 本だけ鳴っている（§0 の芯）。
            //   ここに音源を置くだけでよく、部屋は要らない ──
            //   枠の内側に手置きポータルがあるので、§4.3 のフレネル帯域積分が
            //   「隙間が狭いほど低域だけ通る」を経路として計算する。
            //
            //   ⚠ この音源は**必ず遮蔽される側**に置かれる。実測でフレームコストは
            //     「遮蔽された音源数」で効く（洞窟 3本/遮蔽0.92 = 14.41ms）ので、
            //     洞窟のように既に重い世界では、これ 1 本で効く。要実測。
            if (exitDoor != null)
            {
                // ★設えの中に置く。開口の手前だと直接音が支配して、開き具合が効かない。
                var beyondPos = exitDoor.transform.position + new Vector3(0f, -0.2f, -2.5f);
                var beyond = MakeSource("Beyond_Ambience", beyondPos, 2 + (ambients?.Length ?? 0),
                                        new Color(0.70f, 0.62f, 0.85f));
                beyond.gameObject.AddComponent<PinPosition>();
                var ba = beyond.gameObject.AddComponent<BeyondAmbience>();
                ba.worldDoor = exitDoor;

                // ★扉の下にぶら下げる。**扉の持ち物だから。**
                //
                //   世界の根っこに置いていたせいで、扉を装置側へ 1 枚に集約したとき
                //   この音源だけ世界に取り残され、**くぐった先で接続世界が無音**になっていた。
                //   （ログの「扉の音 0 本」がそれ。）
                //   扉と一緒に運ばれるようにしておけば、世界が変わっても鳴り続ける。
                beyond.SetParent(exitDoor.transform, true);

                extra.Add(beyond);
                events.Add("Beyond");
            }
            demo.extraSources = extra.ToArray();
            demo.sourceEvents = new[] { "PlayerBell", "WorldBell" };
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = material;
            demo.firstPersonCamera = true;
            demo.enableMovement = true;
            demo.distanceRef = 4f;    // 既存テストシーンと統一（距離減衰ゆるめ＝直接音を前に）
            demo.useCppDsp = true;    // 扉の連続性の実装は C++ 経路にしかない
            // ★開口率→音量の傾き。4.0 だと半開き 45° が -52.8dB ＝ 実質無音で、
            //   扉は 75° を超えるまでほぼ鳴らない（サウンドレーンの実測）。
            //   1.0 が物理そのまま（45° で -18.7dB）。1.5 は少しだけ誇張した手応え。
            demo.apertureContrast = 1.5f;
            if (tweak != null) tweak(demo);

            var bellGo = new GameObject("BellCallResponse");
            var bell = bellGo.AddComponent<BellCallResponse>();
            bell.listener = listener;
            bell.playerBell = player.GetComponent<AudioSource>();
            bell.worldBell = world.GetComponent<AudioSource>();
            // ★聴き比べ用。世界のベルを持続音にして鳴らしっぱなしにする。
            //   ベル（過渡音）は定位と遅延を確かめるのに向くが、音色の変化は読み取りにくい。
            //   こもり・コムフィルタ・粒感・残響の量を聴くには持続音のほうがよい
            //   （DEV_LOG E-3「テスト信号を目的で使い分ける」）。
            //   外したいときは Inspector で worldBellLoopClip を空にする。
            bell.worldBellLoopClip = LoadListeningClip();

            // 屋外と屋内を行き来する世界だけ。屋外のフォールバック残響を連続に絞る
            //（回避策。理由は OutdoorTailGate.cs に書いてある）。
            if (outdoorTailGate)
                new GameObject("OutdoorTailGate").AddComponent<OutdoorTailGate>();

            return bell;
        }

        // Z=z の位置に X 方向（xMin..xMax）へ伸びる仕切り。openingX を中心に幅 gw・高さ gh の
        // 開口を空け、ポータルを置く。建物の外殻ではなく「部屋を割る内壁」を作るためのもの。
        //
        // 開口の位置を仕切りごとにずらすと**食い違い**になり、直線がどこも通らなくなる。
        // §4.4 の実測では食い違い壁（隙間 1.0m）は部屋 2・開口 1 と判定される。
        // 音は開口を 2 回以上回り込むしかなくなり、到来方向は必ず開口を指す。
        private static void PartitionX(string prefix, float z, float xMin, float xMax,
                                       float h, float t, float openingX, float gw, float gh,
                                       AcousticMaterialPreset mat)
        {
            float gL = openingX - gw * 0.5f, gR = openingX + gw * 0.5f;
            if (gL - xMin > 0.01f)
                MakeWall(prefix + "_L", new Vector3((xMin + gL) * 0.5f, h * 0.5f, z),
                         new Vector3(gL - xMin, h, t), mat);
            if (xMax - gR > 0.01f)
                MakeWall(prefix + "_R", new Vector3((gR + xMax) * 0.5f, h * 0.5f, z),
                         new Vector3(xMax - gR, h, t), mat);
            float lintelH = h - gh;
            if (lintelH > 0.01f)
                MakeWall(prefix + "_lintel", new Vector3(openingX, (gh + h) * 0.5f, z),
                         new Vector3(gw, lintelH, t), mat);

            var pgo = new GameObject(prefix + "_Portal");
            pgo.transform.position = new Vector3(openingX, gh * 0.5f, z);
            pgo.transform.rotation = Quaternion.identity;   // forward=+Z が開口の法線
            var portal = pgo.AddComponent<AcousticPortal>();
            portal.width = gw;
            portal.height = gh;
        }

        // 音源 1 本。既存テストシーンと同じく、音源オブジェクトと畳み込み器を同一 GameObject に置く。
        //   C# 経路(IrConvolver) と C++ 経路(VoiceConvolver) を両方載せ、
        //   AcousticFlowSceneDemo が useCppDsp を見て片方だけ有効にする（実行中は Y キー）。
        private static Transform MakeSource(string name, Vector3 pos, int sourceIndex, Color tint)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.4f;
            Tint(go.transform, tint);

            var src = go.AddComponent<AudioSource>();
            src.playOnAwake = false;   // 実際の再生は BellCallResponse が握る
            src.loop = true;
            src.spatialBlend = 0f;     // 空間化は AcousticFlow 側。Unity の 3D 減衰は使わない

            var conv = go.AddComponent<IrConvolver>();
            conv.sourceIndex = sourceIndex;
            conv.tailLevel = 0.3f;              // 既存テストシーンと統一
            conv.generateTestSignal = false;    // 鳴らすのはベル。テスト信号は要らない

            var voice = go.AddComponent<VoiceConvolver>();
            voice.sourceIndex = sourceIndex;
            voice.tailLevel = 0.3f;
            voice.enabled = false;              // 既定は Demo 側の useCppDsp が決める
            return go.transform;
        }

        // ── 見た目（音響には一切影響しない）─────────────────────────

        // 名前の前置きでマテリアルを割り当てる。マテリアルが未生成なら何もしない
        //（[BellGame > Assets > 1 マテリアルを作る] を先に走らせること）。
        // 規則は先に書いたものが優先されるので、細かいほうを前に置く。
        private static void DressByName(params (string prefix, string material)[] rules)
        {
            foreach (var r in Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None))
            {
                foreach (var (prefix, matName) in rules)
                {
                    if (!r.gameObject.name.StartsWith(prefix)) continue;
                    var m = BellGameDressing.Load(matName);
                    if (m != null) r.sharedMaterial = m;
                    break;
                }
            }
        }

        // 手続き生成した OBJ を置く。
        //   ★コライダーは付けない。音響が拾うのは BoxCollider / MeshCollider だけなので、
        //     付けなければ部屋グラフにもエコグラムのレイにも入らない（仕様書 §7.6 を踏まない）。
        //   覆われるブロックアウトの箱は **Renderer だけ切る**。コライダーは残す＝音響は不変。
        // 世界のベル。ゆっくり回りながら浮かぶ。
        //   位置は**音源そのもの**（モデルの原点が鐘の胴の中心なので、そのまま置ける）。
        //   回転と浮遊は見た目にだけ掛ける ── 音源を動かすと毎フレーム幾何が変わり、
        //   止まっているはずの物のためにタップの組み替えが走る（FloatSpin.cs の注記）。
        private static void PlaceFloatingBell(Vector3 bellPos)
        {
            var go = PlaceVisualMesh("Assets/Models/Prop_Bell.obj", "Visual_Bell",
                                     bellPos, "Bell_World");
            if (go != null) go.AddComponent<FloatSpin>();
        }

        private static GameObject PlaceVisualMesh(string objPath, string name, params string[] hidePrefixes)
            => PlaceVisualMesh(objPath, name, Vector3.zero, hidePrefixes);

        private static GameObject PlaceVisualMesh(string objPath, string name, Vector3 pos,
                                                  params string[] hidePrefixes)
        {
            var src = AssetDatabase.LoadAssetAtPath<GameObject>(objPath);
            if (src == null)
            {
                Debug.LogWarning($"[BellGame] 見た目メッシュが無い: {objPath} — 箱のまま進める"
                                 + "（tools/StageModelGen を走らせると生成される）");
                return null;
            }
            var go = Object.Instantiate(src);
            go.name = name;
            go.transform.position = pos;

            // 念のため。OBJ 取り込みの既定ではコライダーは付かないが、付いていたら外す。
            foreach (var c in go.GetComponentsInChildren<Collider>()) Object.DestroyImmediate(c);

            // .mtl の材質名（Stone_Wall など）を、こちらで組んだマテリアルへ差し替える。
            foreach (var r in go.GetComponentsInChildren<Renderer>())
            {
                var mats = r.sharedMaterials;
                for (int i = 0; i < mats.Length; i++)
                {
                    if (mats[i] == null) continue;
                    var m = BellGameDressing.Load(mats[i].name.Replace(" (Instance)", "").Trim());
                    if (m != null) mats[i] = m;
                }
                r.sharedMaterials = mats;
            }

            // 覆われた箱の描画を止める（コライダーは残す）。
            foreach (var r in Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None))
            {
                if (r.transform.IsChildOf(go.transform)) continue;
                foreach (string p in hidePrefixes)
                    if (r.gameObject.name.StartsWith(p)) { r.enabled = false; break; }
            }

            return go;
        }

        // 太陽（既定シーンの Directional Light）を昼／夜に振る。
        // ★世界の見え方は WorldSky ただ 1 つが持つ（決めごと #1）。
        //   ここで直に色や強さを書かないこと。同じ表を WorldPortalView も読んでいて、
        //   ズレると「扉ごしに見えた世界」と「踏み越えた先」が違って見える。
        private static void SetupWorld(WorldId id)
        {
            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
            {
                if (l.type != LightType.Directional) continue;
                WorldSky.ApplySun(id, l);
            }
            // ★全世界ぶんの空の資産をシーンへ置いておく。
            //   ポータルは実行中に「まだ行っていない世界」の空を描くが、
            //   資産の参照はエディタでしか解決できない（実行時に AssetDatabase は無い）。
            var libGo = GameObject.Find("WorldSkyLibrary") ?? new GameObject("WorldSkyLibrary");
            var lib = libGo.GetComponent<WorldSkyLibrary>() ?? libGo.AddComponent<WorldSkyLibrary>();
            lib.byWorld = new Material[8];
            foreach (WorldId w in System.Enum.GetValues(typeof(WorldId)))
            {
                if (w == WorldId.None) continue;
                var assetName = WorldSky.Of(w).skyAsset;
                if (!string.IsNullOrEmpty(assetName))
                    lib.byWorld[(int)w] = BellGameDressing.Load(assetName);
            }

            // この世界の空。HDRI の複製なので、シーンに焼くため資産として保存する
            //（保存しないと RenderSettings.skybox が壊れた参照になる）。
            var sky = WorldSky.MakeSkybox(id, lib.byWorld[(int)id]);
            if (sky != null)
            {
                const string dir = "Assets/Materials/BellGame/Sky";
                if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
                string path = dir + "/Sky_" + id + ".mat";
                var existing = AssetDatabase.LoadAssetAtPath<Material>(path);
                if (existing != null)
                {
                    existing.shader = sky.shader;
                    existing.CopyPropertiesFromMaterial(sky);
                    sky = existing;
                }
                else
                {
                    AssetDatabase.CreateAsset(sky, path);
                    sky = AssetDatabase.LoadAssetAtPath<Material>(path);
                }
                AssetDatabase.SaveAssets();
            }
            WorldSky.Apply(id, sky);
        }

        // 火鉢を置く。見た目のメッシュ＋点光源。コライダーは付けないので音響には入らない。
        //
        // ★ベルの近くには置かない。
        //   この作品は「音でベルを見つける」ゲームなので、灯りで在り処が見えてしまうと
        //   音響が飾りになる。奥の部屋は暗いままにして、近づいて初めて見えるようにする。
        private static void PlaceBraziers(params Vector3[] positions)
        {
            foreach (var p in positions)
            {
                PlaceVisualMesh("Assets/Models/Prop_Brazier.obj", "Brazier", p);
                var lightGo = new GameObject("Brazier_Light");
                lightGo.transform.position = p + new Vector3(0f, 1.05f, 0f);
                var l = lightGo.AddComponent<Light>();
                l.type = LightType.Point;
                l.color = new Color(1.00f, 0.62f, 0.28f);
                l.intensity = 2.2f;
                l.range = 13f;
                l.shadows = LightShadows.None;   // 影は要らない。数が増えるので軽く
            }
        }

        private static UnityEngine.SceneManagement.Scene NewScene()
            => EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

        private static Transform MakeListener(Vector3 pos)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = "Listener";
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.5f;
            return go.transform;
        }

        // 地面を「音響用」と「見た目用」に分ける。
        //
        // ★部屋グラフの格子は**登録された全ボックスの AABB 全体**を覆う（room_graph.h）。
        //   総ボクセル数が上限 400 万を超えると、収まるまで**セルを 1.5 倍ずつ粗くする**。
        //   つまり広い地面を 1 枚置くだけで、指定した RoomCellSize が黙って無視される。
        //
        //   実測: 90×90m の地面を置いた神殿は 362×40×362 = 524 万ボクセルになり、
        //   **0.25m 指定が 0.375m に落ちていた**。§11 の目安は「0.25m 格子で戸口 0.9m が
        //   ぎりぎり」なので、1.1m の開口では部屋の割れ方が不安定になる。
        //
        //   → 音響用の地面は遊ぶ範囲だけに絞る。見た目はコライダー無しの板で伸ばす
        //     （コライダーが無ければ CollectOccluders が拾わない＝格子にも入らない）。
        // 遊べる範囲を**音響用の地面から導く**。
        //
        // ★数字を別に書かない（決めごと #1）。地面を広げたのに境界が付いてこない、
        //   あるいはその逆、が起きると「地面はあるのに進めない」になる。
        //   地面のコライダーが唯一の答えで、そこから内側へ inset するだけにする。
        //
        // ★境界そのものは**無音**。世界を閉じるのは造形の仕事（丘・岩・稜線・瓦礫）で、
        //   これはその保険。見えない箱を置くと音響的に本物の壁になってしまうので使わない。
        private static WorldBounds AddWorldBounds(Transform listener, Transform ground,
                                                  float inset = 2.0f, float margin = 4.0f)
        {
            var col = ground != null ? ground.GetComponent<BoxCollider>() : null;
            if (col == null)
            {
                Debug.LogWarning("[BellGame] 地面に BoxCollider が無いので境界を作れない。");
                return null;
            }

            var go = new GameObject("WorldBounds");
            var wb = go.AddComponent<WorldBounds>();
            wb.listener = listener;
            // ★数字は焼き込まない。地面のコライダーを渡すだけにして、
            //   範囲は実行時に WorldBounds.Derive() が導く。
            //   焼き込むと、地面を動かしただけで範囲がズレて出発点で動けなくなる。
            wb.ground = col;
            wb.inset = inset;
            wb.softMargin = margin;
            return wb;
        }

        private static Transform MakeGround(Vector3 center, Vector3 acousticSize,
                                            float visualSpan = 220f)
        {
            var g = MakeBox("Ground", center, acousticSize);
            // 見た目だけの地面。3cm 下げて Z ファイティングを避ける。
            var vis = MakeBox("Ground_Visual", center + new Vector3(0f, -0.03f, 0f),
                              new Vector3(visualSpan, acousticSize.y, visualSpan));
            var col = vis.GetComponent<Collider>();
            if (col != null) Object.DestroyImmediate(col);
            return g;
        }

        private static Transform MakeBox(string name, Vector3 center, Vector3 size)
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
            var shader = Shader.Find("Standard");
            if (shader != null) r.sharedMaterial = new Material(shader) { color = c };
        }

        private static void Save(UnityEngine.SceneManagement.Scene scene, string fileName, string note)
        {
            const string dir = "Assets/Scenes";
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            EditorSceneManager.SaveScene(scene, dir + "/" + fileName);
            Debug.Log("[BellGame] ステージ生成: " + fileName + " — " + note);
        }
    }
}
