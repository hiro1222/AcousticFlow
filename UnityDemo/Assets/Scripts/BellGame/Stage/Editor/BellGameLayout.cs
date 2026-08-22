// BellGameLayout.cs (Editor 専用)
// メニュー: [BellGame > ステージ配置 > ...]
//
// ★これは「世界観・ステージ構成」レーンの入口。**仮**の置き場として作った。
//
//   BellGameStages.cs はステージの形とギミックの結線が同居していて、
//   ゲームシステムレーンが常時触っている。同じファイルを 2 レーンで触ると壊すので、
//   **形と、その地点で聞こえる音**だけをこちらに置く。
//
//   ここが作るもの        … 地面 / 地形 / 草 / 木 / 材質(α と TL) / 立ち位置の目印
//   ここが作らないもの    … ベル・扉・プレイヤー・音響ホスト（ギミック側）
//
//   したがって**このシーンは鳴らない**（歩くだけはできる ── LayoutWalker）。
//   絵と寸法を目で確かめるためのもの。ギミックを載せる段になったら、
//   BellGameStages 側の結線を呼ぶか、あちらへ座標を渡す。
//
// ★コライダーの方針（音響に直結するので、ここが一番大事）
//   音響ホストが拾うのは BoxCollider と MeshCollider だけ。したがって
//   **コライダーを付けた物だけが聞こえる。**
//
//   | 物 | コライダー | 理由 |
//   |---|---|---|
//   | 地面（音響用の箱） | あり | 早期反射の主役 |
//   | 扉の丘 | あり(Mesh) | プレイヤーが登る。付けないと坂が足をすり抜ける |
//   | 木の幹 | あり(Box) | チュートリアルで「物が音を遮る」を教える唯一の教材 |
//   | 縁の丘 | **なし** | 付けると外接 147×147m ＝ 格子 588×30×588 = 1,037 万で上限 400 万を超え、
//   |  |  | **セルが 0.375m 以上へ降格**する（C7 と同じ罠）。
//   |  |  | プレイヤーは境界に押し戻されて丘には届かない |
//   | 草・木の葉・見た目の地面 | **なし** | 葉は音をほとんど遮らない。樹冠に箱を付けると巨大な遮蔽物になる |
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using AcousticFlow;

namespace BellGame.EditorTools
{
    public static class BellGameLayout
    {
        // ── ステージ 0：白い部屋（プロローグ）─────────────────────────
        // 発注者の決定（案 B・2026-08-21）:
        //   一番最初は真っ白な何もない部屋と扉から始まり、
        //   **最初から持っているベルを鳴らすと草原へ入る。**
        //
        // ★**閉じた部屋にしてある。**「無限の白い虚空」にはしない。
        //   草原は `部屋 0・開口 0・尾なし`（WORLD_VISUAL §3-1）なので、
        //   ここも無響にすると**二つが同じ音になり、冒頭が何も教えない。**
        //   天井があれば部屋になる（`2e63c46`）ので、壁と天井を立てて響かせる。
        //   目が「閉じている」と言い、耳も「閉じている」と言う ── それが揃っていることが要。
        //
        //   これで教える順番が一段ずつ増える:
        //     白い部屋 = 閉じた場所は響く
        //     → 草原   = 開けた場所は返らない
        //     → 崩壊都市 = 返す建物と返さない建物がある
        //
        // ★内寸 8×8×4.2m = 269m³。RT60 と α はゲームシステムレーンの領分（連絡板 M11）。
        //   7×7×3.2m から広げたのは、**扉の全高 3.3m が入らなくなったから**。
        [MenuItem("BellGame/ステージ配置/0 白い部屋 (Prologue)")]
        public static void Stage0WhiteRoom()
        {
            if (!EnsureNotPlaying()) return;
            var scene = NewScene();
            SetupAirWhite();

            // 部屋そのもの。**歩けるようにコライダーを付ける**（本編では箱が音響を持つ）。
            Place("Assets/Models/Stage0_WhiteRoom.obj", "WhiteRoom", Vector3.zero, true);

            // ── 扉。他の世界と同じ 1 種類 ────────────────────────
            //   ★壁に嵌めず、部屋の中に**独立して立たせる。**
            //     「どこにも属さない扉が、属さない場所に立っている」が、この作品の扉の絵。
            //     壁の穴にすると、ただの出口になる。
            var doorPos = new Vector3(0f, 0f, 2.0f);
            var face = Quaternion.Euler(0f, 180f, 0f);   // プレイヤー側（-Z）を向かせる
            var door = Place("Assets/Models/Prop_Door.obj", "Door_Frame", doorPos, face, true);
            PlaceDoorSlab(doorPos + new Vector3(0f, 1.5f, 0f), face, door != null ? door.transform : null);

            // ベルは**最初から持っている**ので、床には置かない。
            //   立ち位置の目印だけ。
            var startPos = new Vector3(0f, 1.6f, -1.6f);
            Marker("Mark_Start", startPos, new Color(0.9f, 0.3f, 0.3f));
            MakeWalkerRect(startPos, Vector3.zero, new Vector2(3.7f, 3.7f));

            Save(scene, "Stage0_Layout.unity",
                 "プロローグの白い部屋。内寸 8×8×4.2m = 269m³ の**閉じた**部屋で、"
                 + "中央に扉が独立して立つ。ベルは最初から持っている前提なので床には置いていない。"
                 + "★閉じているのは意図 ── 草原(無響)との対比で「閉じた場所は響く」を教える。");
        }

        /// 白い部屋の空気。**霧は入れない**（部屋の中なので奥行きは要らない）。
        ///   ★環境光を白 1 色にしない。3 色とも白だと壁・床・天井の境目が消えて、
        ///     箱の中に居ることが目で分からなくなる（耳は響いていると言っているのに）。
        private static void SetupAirWhite()
        {
            RenderSettings.fog = false;
            RenderSettings.ambientMode = AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = new Color(0.93f, 0.94f, 0.95f);
            RenderSettings.ambientEquatorColor = new Color(0.84f, 0.85f, 0.86f);
            RenderSettings.ambientGroundColor = new Color(0.70f, 0.71f, 0.73f);
            RenderSettings.ambientIntensity = 1f;

            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
            {
                if (l.type != LightType.Directional) continue;
                // 影を薄く。白い部屋に濃い影が落ちると、途端に「部屋」になりすぎる。
                l.transform.rotation = Quaternion.Euler(52f, -28f, 0f);
                l.color = Color.white;
                l.intensity = 0.55f;
                l.shadows = LightShadows.Soft;
                l.shadowStrength = 0.30f;
            }
            // ★空。地平の色に**霧と同じ値**を渡す（別々に書くとズレる）。
            RenderSettings.skybox = SkyMaterial("Sky_White", new Color(0.93f, 0.94f, 0.95f),
                                                RenderSettings.fogColor, SunDirection(), 0f);
            DynamicGI.UpdateEnvironment();
        }

        // ── ステージ 1：草原 ─────────────────────────────────────────
        // WORLD_SETTING §3「基準」／発注者の決定「草原はチュートリアル。探すステージではない」。
        //
        // ★案 B（発注者の決定・2026-08-21）で並びが変わった。
        //   **扉（南）→ プレイヤー → 丘の上のベル（中央）** の一直線。
        //   歩く順がそのままチュートリアルの台本になる:
        //     白い部屋で鳴らす→扉が開く … 草原へ出る→鳴らしても**返ってこない**
        //     … 丘のベルを拾う … 扉へ戻って新しい音色を鳴らす→次の世界へ
        //
        // ★Z 軸上に素直に並べる。プレイヤーは開始時に +Z を向くので、そのまま丘のベルに正対し、
        //   振り返れば出てきた扉が見える。探索距離は要らない（プレイヤー→ベルは 24m）。
        [MenuItem("BellGame/ステージ配置/1 草原 (Grassland)")]
        public static void Stage1Grassland()
        {
            if (!EnsureNotPlaying()) return;
            var scene = NewScene();
            SetupAir();

            // ── 立ち位置（この 3 点が配置の骨） ──────────────────
            // ★世界は円。中心に扉、半径 30m が遊べる範囲、その外は地面が暗くなる。
            const float border = 30f;                         // 生成器の GrassBorderRadius() と同じ値
            // ★Z 軸上に並べて正対させる。斜めに敷くと門も視線も傾くので、素直に軸へ乗せた。
            //   MakeListener 相当の既定の向きが +Z なので、開始時にそのまま扉を向く。
            //   ★案 B（発注者の決定）で扉とベルを入れ替えた。
            //     扉から現れ → 丘の上のベルを取り → 扉へ戻って鳴らす。
            //     この形は崩壊都市以降と同じで、白い部屋から数えて 4 回とも同じになる。
            var doorPos = new Vector3(0f, 0f, -20f);          // 南寄り（発注者の指示で -26 → -20）
            var bellPos = new Vector3(0f, 2.3f, 0f);          // 中央・丘の頂上（平場 0.7m）から 1.6m 浮く
            var startPos = new Vector3(0f, 1.6f, -18f);       // 扉の 2m 手前。振り返れば扉が見える

            // ── 音響用の地面 ────────────────────────────────────
            // 円を包む正方の箱。64×64m で半径 30m の円が収まる。
            // 格子 256×24×256 = 157 万 ── 上限 400 万に対して余裕があり、セル 0.25m を保てる。
            var ground = MakeBox("Ground", new Vector3(0f, -0.5f, 0f), new Vector3(64f, 1f, 64f));
            // 草案 §4.3 の草地 α ≈ 0.30。Default(平均 0.208) の 1.45 倍。
            Adjust(ground, AcousticMaterialPreset.Default, 1.45f);
            Dress(ground, "Grass_Ground");

            // 見た目だけの地面。3cm 下げて Z ファイティングを避ける。コライダーは外す。
            var vis = MakeBox("Ground_Visual", new Vector3(0f, -0.53f, 0f), new Vector3(220f, 1f, 220f));
            Object.DestroyImmediate(vis.GetComponent<Collider>());
            Dress(vis, "Grass_Ground");

            // ── 地形 ────────────────────────────────────────────
            // 扉の丘だけコライダーを付ける（上の表のとおり）。
            //   ★丘と川の土手を 1 枚にまとめた（`Stage1_Field.obj`）。歩く面なのでコライダーを付ける。
            var field0 = Place("Assets/Models/Stage1_Field.obj", "Terrain_Field", Vector3.zero, meshCollider: true);
            if (field0 != null) Adjust(field0.transform, AcousticMaterialPreset.Default, 1.45f);
            Place("Assets/Models/Stage1_Hills.obj", "Terrain_Hills", Vector3.zero, meshCollider: false);

            // ── 草 ──────────────────────────────────────────────
            // 下草は GPU インスタンシング（`InstancedGrassField`）。リポジトリに残る
            // ファイルは 0 バイトで、密度は数字ひとつで変わる。**音響には触らない**
            // ── コライダーを 1 つも作らないので `CollectOccluders` の対象外。
            var grass = new GameObject("Grass_Instanced");
            var field = grass.AddComponent<InstancedGrassField>();
            // ★丘の上まで伸ばす。ボーダー(30m)で切ると、そこから先が禿げた丘になる。
            //   外側は密度を落とし、色も暗い地面に合わせる（行けない場所だと分かるように）。
            field.radius = 36f;
            field.density = 110f;
            field.outerDensity = 0.40f;
            // ★葉の長さ。**ここまで 0.35 → 0.70 → 0.48 と動いている。**
            //   既定任せにすると値が見えず、また往復するので配置側に書く。
            //   短くすると地面が透けやすくなる ── 気になったら密度で埋める
            //   （ただし密度は本数に直に効くので、上げると重くなる）。
            field.bladeHeight = 0.48f;
            field.material = GrassMaterial();
            // ★土手には生やさない。**生成器の IsBank と同じ `RiverOut`(9.5m)。**
            //   定数をそのまま参照して、二度とズレないようにする ── 数字を直接書くと、
            //   片方だけ変わって「草は消えているのに地面は草色」の帯が出る。
            //   9.5m は**土手が終わって野原に戻る距離**なので、
            //   草は地面が平らに戻る所からちょうど生え始める。
            field.riverClear = GrasslandTerrain.RiverOut;
            // ★門の石段の上には生やさない。インスタンシングは置いてある物を知らないので、
            //   footprint を渡す。土台を広げたので矩形も追従させる。
            //   石の門をやめて扉 1 枚になったので、除外はぐっと小さくてよい。
            //   （x 中心, z 中心, x 半幅, z 半幅）。全幅 1.92m の枠のぶんだけ。
            field.excludeRects = new[] { new Vector4(0f, -20f, 1.2f, 0.5f) };
            // ★設定し終えたら**明示的に組み直す。**
            //   AddComponent の時点（riverClear=0）で一度組まれるので、
            //   後から設定した値が LateUpdate まで効かない。生成直後に見ると
            //   「土手に草が生えたまま」に見える。
            field.Rebuild();
            // ★除外が効いているかを**数字で**出す。効いていれば川の帯のぶんだけ減る。
            //   半径 36m・密度 110 で理論値は約 448,000 本。
            Debug.Log($"[BellGame] 草 {field.instanceCount:N0} 本（riverClear {field.riverClear}m。除外なしなら約 448,000 本）");
            grass.AddComponent<LayoutStats>();   // FPS と草の本数を画面に出す（切り分け用）

            // ★背の高い穂の層は入れない（発注者の判断・2026-08-20）。
            //   `InstancedGrassField.seedHead` と穂のメッシュは残してあるので、
            //   要るときは背の高い層をもう 1 つ足すだけで戻せる。

            // ★実体メッシュの草木（株・穂・花）は全部やめた。
            //   揺れないものが揺れる草に混じると浮くうえ、株 1,853 個で 44,500 三角形あった。
            //   足すならインスタンシングの層として足す（生成器の BuildGrassland に撒き方が残っている）。

            // ── 木 2 本 ─────────────────────────────────────────
            // 幹と枝と小枝だけメッシュ。葉は InstancedCanopy が持つ。
            // 幹だけが聞こえる。葉には付けない（葉は音をほとんど遮らないし、
            // 樹冠に箱を付けると直径 12m の遮蔽物になる）。
            //
            // ★1 本目 (14.5, -25) は**出発点のすぐ右手**（発注者の指示）。
            //   扉から出た瞬間、右に木・その向こうに川、という並びになる。
            //   川の中心線までは 11.5m あるので、土手の帯 7.5m には掛からない。
            // ★2 本目は左の中景。1 本だと右に寄りすぎて絵が傾く。
            //   少し小さくして種を変える ── 同じ形が 2 つ並ぶと作り物に見える。
            PlaceTree("Tree_Near", new Vector3(14.5f, 0f, -25f), 1.00f, 4120260);
            //   ⚠ (23,-4) は**川の中心線から 2.75m**（水面の半幅は約 2.65m）。
            //     木が水際に半分立つ形になる。狙いと違うなら z か x を教えてほしい。
            PlaceTree("Tree_Far", new Vector3(23f, 0f, -4f), 0.82f, 908311);

            void PlaceTree(string name, Vector3 pos, float scale, int seed)
            {
                var tree = Place("Assets/Models/Prop_Tree.obj", name, pos, meshCollider: false);
                if (tree == null) return;
                tree.transform.localScale = Vector3.one * scale;

                // 樹冠は GPU インスタンシング（葉 1 枚ずつ）。塊で作ると団子に見えるうえ揺らせない。
                var canopy = tree.AddComponent<InstancedCanopy>();
                canopy.center = new Vector3(0f, 7.4f, 0f);
                canopy.extents = new Vector3(15f, 4.2f, 15f);   // 横に広く・薄く。枝張り 14m に乗せる
                canopy.leafCount = Mathf.RoundToInt(12000 * scale);
                canopy.leafSize = 0.34f;
                canopy.lobes = 7;
                canopy.lobeSpread = 0.55f;
                canopy.seed = seed;
                canopy.material = InstancedMaterial("Leaf_Instanced", wholeSway: true);

                var trunk = new GameObject("Tree_Trunk_Acoustic");
                trunk.transform.SetParent(tree.transform, false);
                trunk.transform.localPosition = new Vector3(0f, 3.25f, 0f);
                var bc = trunk.AddComponent<BoxCollider>();
                bc.size = new Vector3(1.5f, 6.5f, 1.5f);
                Adjust(trunk.transform, AcousticMaterialPreset.Default, 1f);
            }

            // ── 左手奥の川 ──────────────────────────────────────
            //   ★2026-08-21 戻した（発注者の判断「掘るんなら遮るものにはならない」）。
            //     前回見えなかった原因は「平らだった」ことではなく**草に隠れた**こと。
            //     効いているのは深さより**土手の草を剥いで緑を切る明るい帯**のほう。
            //     土手 0.55m → 1.00m、除外の帯と土手の材質の帯を 7.5m でぴったり揃えた。
            //   ★水面はコライダー無し。渡れてしまうと「浅瀬」の意味が変わる。
            Place("Assets/Models/Stage1_River.obj", "River", Vector3.zero, meshCollider: false);

            // ── 扉。**全世界で共通の 1 枚。**南の端に立つ ────────────
            //   ★案 B（発注者の決定）で役割が変わった。扉は「奥にある目的地」ではなく
            //     **白い部屋から出てきた場所であり、次の音色を鳴らして次へ行く場所**。
            //     だから背中にあるのが正しい。中央の丘にはベルが載る。
            //
            //   ★石の門（`Prop_Shrine_Grassland.obj` 7,970 三角形）は**使わない。**
            //     発注者の指示で扉の意匠を全世界で 1 種類に統一したため。
            //     OBJ と生成器は残してある（消すのは番号が確定してから）。
            //
            //   ★コライダーは付ける。枠は細いが、付けなければ「見える物が聞こえない」嘘になる。
            //     全幅 1.35m しかないので、草原に足す遮蔽物としては石の門よりずっと軽い。
            //   ★扉の正面は −Z 向き。プレイヤーは扉の北側（+Z 側）に立つので、
            //     こちらを向かせるため 180 度回す。
            var faceListener = Quaternion.Euler(0f, 180f, 0f);
            var door = Place("Assets/Models/Prop_Door.obj", "Door_Frame",
                             doorPos, faceListener, meshCollider: true);
            if (door != null)
            {
                foreach (var mf in door.GetComponentsInChildren<MeshFilter>())
                    Adjust(mf.transform, AcousticMaterialPreset.Default, 1f);

                // 板は物理で回る側なので、ここでは見た目の目安として扉の子に置くだけ。
                // 開口 3.0m の半分 ＝ 1.5m。
                PlaceDoorSlab(doorPos + new Vector3(0f, 1.5f, 0f), faceListener, door.transform);
            }

            // ── 目印（ギミックはここでは作らない） ──────────────
            // 見た目の物だけ置いて、構図を目で確かめられるようにする。
            // ★ふわふわ浮いて回る。本編の `PlaceFloatingBell` と同じ `FloatSpin` を使う
            //   ── 配置シーンだけ付け忘れていて、本編と見え方が違っていた。
            //   ★実寸は 0.15×0.30m と小さいので 1.6 倍にする（0.24×0.48m）。
            //     丘の上に 1 つだけ置く物なので、小さいと遠くから見つからない。
            var refBell = Place("Assets/Models/Prop_Bell.obj", "Ref_Bell", bellPos, meshCollider: false);
            if (refBell != null)
            {
                refBell.transform.localScale = Vector3.one * 1.6f;
                var fs = refBell.AddComponent<FloatSpin>();
                fs.degreesPerSecond = 12f;   // ゆっくり。速いと「拾う物」ではなく「装置」に見える
                fs.bobAmplitude = 0.09f;     // 上下 9cm。丘の上なので大きめでよい
                fs.bobSeconds = 5.5f;        // 呼吸くらいの周期
                fs.tiltDegrees = 7f;
            }
            MakeWalker(startPos, border);

            // 川を外したので、環境音の目印も置かない。

            // ⚠ 円の押し戻しは、この配置シーンでは LayoutWalker が持っている。
            //   **本編にはまだ無い** ── WorldBounds は箱（center + extents）だけで半径を持たず、
            //   そのままだと見た目は円・止まるのは箱で食い違う（対角線でまだ緑が見えるのに止まる）。
            //   ゲームシステム側に「WorldBounds に円形モード（半径 30m）」を依頼すること。
            Debug.LogWarning($"[BellGame] 円の押し戻しは配置シーン限定（LayoutWalker）。"
                             + $"本編用に WorldBounds へ半径 {border}m の円形モードが要る");

            Save(scene, "Stage1_Layout.unity",
                 $"草原の配置（仮）。円形・半径 {border}m。"
                 + $"プレイヤー {startPos} → ベル {bellPos} → 扉 {doorPos} の一直線。"
                 + "ギミックと音響ホストは載せていないので**鳴らない**が、"
                 + "LayoutWalker で歩ける（WASD / 右ドラッグ視線 / Shift 2.5倍速）。");
        }

        // ───────────────────────────────────────────────────────────
        // 部品
        // ───────────────────────────────────────────────────────────

        // ── ステージ 2：無人の現代都市 ───────────────────────────────
        // WORLD_VISUAL.md §3-2「**屋根の有無が遠目に読めること。これが最重要。**」
        //   崩壊都市から作り替えたが、**この芯は捨てていない** ──
        //   屋根のある建物は響き、壁だけで上が抜けた区画は響かない。
        //   エンジンの規則（`2e63c46`：部屋には天井が要る）はそのまま。
        //
        // ★これは**見た目を確かめるためのシーン**で、音響は入っていない。
        //   本編は `BellGame > Stages > 2 崩壊都市` が作る `Stage2_Ruins.unity`。
        //   歩けるように見た目メッシュへ MeshCollider を付けているが、
        //   本編では付けない（`CollectOccluders` が拾って音響が二重になる）。
        //
        // ★★座標は**ブロックアウトと一致していない。連絡板 M12 の提案値。**
        //   |物|現行|ここ（提案）|
        //   |---|---|---|
        //   | Ground | (0,-0.5,-1.5) 50×61 | (0,-0.5,**4**) **70×80** |
        //   | ExitDoor | (0,0,-32) | **(-29,0,-30)** / Y 回転 **-135 度** |
        //   | listener | (0,1.6,-26) | **(-27.6,1.6,-28.6)** |
        //   | bellPos | (8,1.2,14) | **(-21.19,1.2,34)**（ドームの中心）|
        //   | farDistance | 42f | **66f**（出発点→ベル 62.9m）|
        //   | 旧 BldgA〜D / 瓦礫 / 鉄骨 | 箱あり | **全部外す** |
        [MenuItem("BellGame/ステージ配置/2 崩壊都市 (Ruins)")]
        public static void Stage2Ruins()
        {
            if (!EnsureNotPlaying()) return;
            var scene = NewScene();
            SetupAirRuins();

            var ground = MakeBox("Ground", new Vector3(0f, -0.5f, 4f), new Vector3(70f, 1f, 80f));
            Dress(ground, "City_Ground");

            // ── 道路。**ステージの外**を西と南に L 字で回る ──────────
            //   ★中を通さない。街区の内側に立っている、という場所が決まるうえ、
            //     道が見えるのに行けないので「世界は続いているが、行けるのはここまで」が
            //     柵を見せずに伝わる（§7.2）。コライダーは付けない。
            Place("Assets/Models/City_Roads.obj", "Visual_Roads", Vector3.zero, false);

            // ── 街区。**発注者がシーンビューで調整した値をそのまま焼いてある**（2026-08-22）──
            //   公園を 1.7 倍にしたときに被った棟を動かし、ドームを公園の中央へ寄せた形。
            //   B1・B2 を南へ 6.5m ／ C1 を東へ 8m・南へ 4m ／ C2 を南へ 4m ／ ドームを東へ 2.81m。
            //
            //   ★これらは推定値ではなく、`Stage2_Layout.unity` から読み出した実測値。
            //     図から座標を推定して 2 度往復させた反省で、以降はこの形にする。
            //
            //   ★塊 8 棟は内部を持たない。部屋になるのは B1（囮）とドームの 2 つだけ。
            Place("Assets/Models/City_A1.obj", "Visual_A1", new Vector3(-26f, 0f, -14f), true);   // 上が空
            Place("Assets/Models/City_A2.obj", "Visual_A2", new Vector3(-4.5f, 0f, -14f), true);
            Place("Assets/Models/City_A3.obj", "Visual_A3", new Vector3(15.5f, 0f, -14f), true);
            Place("Assets/Models/City_A4.obj", "Visual_A4", new Vector3(31f, 0f, -14f), true);
            Place("Assets/Models/City_B1.obj", "Visual_B1", new Vector3(-27.5f, 0f, 7.5f), true); // 屋根あり・囮
            Place("Assets/Models/City_B2.obj", "Visual_B2", new Vector3(-7.5f, 0f, 7.5f), true);
            Place("Assets/Models/City_B3.obj", "Visual_B3", new Vector3(13.5f, 0f, 14f), true);
            Place("Assets/Models/City_B4.obj", "Visual_B4", new Vector3(30.5f, 0f, 14f), true);
            Place("Assets/Models/City_C1.obj", "Visual_C1", new Vector3(4f, 0f, 34f), true);
            Place("Assets/Models/City_C2.obj", "Visual_C2", new Vector3(23.5f, 0f, 34f), true);

            // ── 公園とドーム ────────────────────────────────
            //   ★公園 756 m²（x -35..-7 / z 17..44）。B1・B2 の北面から **2.4m 空いている。**
            //   ★ドームの開口は**南（-Z）に 1 つだけ**。§4.3 の崩しをここが引き継ぐ ──
            //     外に漏れる音はベルからではなく開口から来るので、
            //     南から聞こえるのに**公園への入口は東の通りだけ**、という嘘が成立する。
            Place("Assets/Models/City_Park.obj", "Visual_Park", Vector3.zero, false);
            Place("Assets/Models/City_Dome.obj", "Visual_Dome", new Vector3(-21.19f, 0f, 34f), true);

            // ── 遠景のビル群。**触れない・音響に入らない** ────────────
            //   道路（x ±58 / z ±62）の**外**に置くこと。中に入れると足元が道路の板に埋まる。
            Place("Assets/Models/City_Skyline.obj", "Visual_Skyline", Vector3.zero, false);

            // ── 扉。**1 枚だけ。**南西の角で、ここが出発点 ──────────
            //   ★発注者がシーンビューで置いた値。向きは -135 度 ＝ 正面が北東 45 度。
            //     角に立つ扉なので、街区の対角へ真っ直ぐ向く。
            var doorPos = new Vector3(-29f, 0f, -30f);
            const float DoorYaw = -135f;
            var doorFace = Quaternion.Euler(0f, DoorYaw, 0f);
            // 扉が向いている方向（正面はローカル -Z なので、そこへ回転を掛ける）。
            Vector3 doorOut = doorFace * Vector3.back;
            var rDoor = Place("Assets/Models/Prop_Door.obj", "Visual_Doorway", doorPos, doorFace, true);
            PlaceDoorSlab(doorPos + new Vector3(0f, 1.5f, 0f), doorFace,
                          rDoor != null ? rDoor.transform : null);

            // ── 目印（本編ではベルが入る所）──────────────────────
            //   ★ベルの在り処が**見える**造形は置かない（連絡板の決めごと）。
            //     ここは配置を確かめるためだけの球なので、確認が済んだら消す。
            Marker("Mark_Bell", new Vector3(-21.19f, 1.2f, 34f), new Color(0.95f, 0.82f, 0.35f));

            // 出発点は扉の 2m 前。振り返れば出てきた扉が見える。
            var startPos = doorPos + doorOut * 2f + new Vector3(0f, 1.6f, 0f);
            Marker("Mark_Start", startPos, new Color(0.9f, 0.3f, 0.3f));
            //   ★道はステージの外なので、歩ける矩形は縁の内側で止める。
            //     「見えるが行けない」は柵ではなく境界の抵抗で伝える（§7.2）。
            MakeWalkerRect(startPos, new Vector3(0f, 0f, 4f), new Vector2(32f, 37f));

            Save(scene, "Stage2_Layout.unity",
                 "無人の現代都市。扉は1枚だけで、南西の角(-29,-30)＝出発点。道はステージの外。"
                 + "屋根あり(B1)は響き、上が空のA1は響かない。ベルは北西の公園のドームの中。"
                 + "★座標はブロックアウトと**一致していない**（連絡板 M12 の提案値）。音響は入っていない。");
        }

        /// 崩壊都市の空気。**曇り。**影を弱くして、面の明度差で見せる（WORLD_VISUAL §3-2）。
        ///   霧は「中」＝ 18m から 80m。草原（28→130m）より濃い ──
        ///   遠くの建物が霞んで、**輪郭だけが残る**。屋根の有無を輪郭で読ませたいので、
        ///   むしろ霞んでいるほうが効く。
        private static void SetupAirRuins()
        {
            RenderSettings.fog = true;
            RenderSettings.fogMode = FogMode.Linear;
            RenderSettings.fogColor = new Color(0.714f, 0.765f, 0.804f);   // #B6C3CD 霧・空気遠近
            RenderSettings.fogStartDistance = 18f;
            RenderSettings.fogEndDistance = 80f;

            RenderSettings.ambientMode = AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = new Color(0.714f, 0.765f, 0.804f);      // #B6C3CD
            RenderSettings.ambientEquatorColor = new Color(0.506f, 0.584f, 0.647f);  // #8195A5
            RenderSettings.ambientGroundColor = new Color(0.212f, 0.263f, 0.310f);   // #36434F
            RenderSettings.ambientIntensity = 1f;

            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
            {
                if (l.type != LightType.Directional) continue;
                // ★曇りなので影は弱く、光は高めから。斜光にすると晴れて見える。
                l.transform.rotation = Quaternion.Euler(58f, 20f, 0f);
                l.color = new Color(0.88f, 0.91f, 0.95f);
                l.intensity = 0.72f;
                l.shadows = LightShadows.Soft;
                l.shadowStrength = 0.45f;
            }
            // ★空。地平の色に**霧と同じ値**を渡す（別々に書くとズレる）。
            RenderSettings.skybox = SkyMaterial("Sky_Ruins", new Color(0.42f, 0.49f, 0.58f),
                                                RenderSettings.fogColor, SunDirection(), 0.85f);
            DynamicGI.UpdateEnvironment();
        }

        // ── ステージ 3：雪山（洞窟を棄却したので 3 番目）─────────────
        // WORLD_VISUAL.md §3-4「★**輪郭が溶けること。**白い面に白い雪で、形が読みにくい。
        //   **稜線だけが読める** ── 音で頼れるものが稜線の回折だけ、というのと同じ構図」
        //
        // ★この世界だけ**絵の要請が逆**。他の世界では「シルエットを読ませる」ためにやってきた
        //   ことを、ここでは逆に使う。雪原に細かい起伏を刻むと形が読めてしまうので、
        //   **長い波長のうねりだけ**にして、面としては情報を持たせない。
        //
        // ★読ませるのは 3 つだけ:
        //     稜線 ── 空を背にした縁。雪庇を付けると線が締まる
        //     露岩 ── 一面の白の中で**唯一の暗い物**。距離の基準
        //     樹氷 ── 縦の線。大きさの手がかり
        //
        // ★寸法は `BellGameStages.Stage4Snow` のブロックアウトと一致させてある。
        //   ⚠ 洞窟を落としたので**番号がずれる**。連絡板 M13 で出してある。
        [MenuItem("BellGame/ステージ配置/3 雪山 (Snow)")]
        public static void Stage3Snow()
        {
            if (!EnsureNotPlaying()) return;
            var scene = NewScene();
            SetupAirSnow();

            var ground = MakeBox("Ground", new Vector3(0f, -0.5f, 0f), new Vector3(64f, 1f, 80f));
            Dress(ground, "Snow_Ground");

            // 雪原。歩く面なのでコライダーを付ける（本編では箱が音響を持つ）。
            Place("Assets/Models/Snow_Field.obj", "Visual_Field", Vector3.zero, true);

            // ── 稜線 3 本。ブロックアウトの箱と同じ位置 ────────────
            //   Ridge_1 (-10,-12) 長さ 40（X 方向）／ Ridge_2 (14,6) 長さ 30（Z 方向）
            //   Ridge_3 (-4,24) 長さ 26（X 方向）
            //   ★生成器は長さを Z 方向に取っている。X に伸びる稜線は Y へ 90 度回す。
            var alongX = Quaternion.Euler(0f, 90f, 0f);
            Place("Assets/Models/Snow_Ridge1.obj", "Visual_Ridge1",
                  new Vector3(-10f, 0f, -12f), alongX, true);
            Place("Assets/Models/Snow_Ridge2.obj", "Visual_Ridge2",
                  new Vector3(14f, 0f, 6f), true);
            Place("Assets/Models/Snow_Ridge3.obj", "Visual_Ridge3",
                  new Vector3(-4f, 0f, 24f), alongX, true);

            // 遠景の山脈。**触れない・音響に入らない。**
            Place("Assets/Models/Snow_Range.obj", "Visual_Range", Vector3.zero, false);

            // ── 扉。**1 枚だけ。**出発点でもある ────────────────
            //   ブロックアウトの listener は (0,1.6,-34)。扉はその 2m 後ろ。
            var doorPos = new Vector3(0f, 0f, -36f);
            var doorFace = Quaternion.Euler(0f, 180f, 0f);   // 正面(-Z)を +Z 側のプレイヤーへ
            var door = Place("Assets/Models/Prop_Door.obj", "Visual_Doorway", doorPos, doorFace, true);
            PlaceDoorSlab(doorPos + new Vector3(0f, 1.5f, 0f), doorFace,
                          door != null ? door.transform : null);

            // ── 目印 ────────────────────────────────────────
            //   ★ベルの在り処が**見える**造形は置かない。確認が済んだら消す。
            Marker("Mark_Bell", new Vector3(20f, 1.2f, 14f), new Color(0.95f, 0.82f, 0.35f));

            var startPos = new Vector3(0f, 1.6f, -34f);
            Marker("Mark_Start", startPos, new Color(0.9f, 0.3f, 0.3f));
            MakeWalkerRect(startPos, Vector3.zero, new Vector2(30f, 38f));

            Save(scene, "Stage3_Snow.unity",
                 "雪山。★輪郭が溶ける世界 ── 雪原は形を教えず、読めるのは稜線・露岩・樹氷だけ。"
                 + "夜だが真っ暗にしない（雪の照り返し）。霧は高（14→70m）。"
                 + "稜線 3 本はブロックアウトの箱と同じ位置。音響は入っていない。");
        }

        /// 雪山の空気。**夜。ただし真っ暗にしない**（WORLD_SETTING §6-4）。
        ///
        ///   ★暗さで不安を作らない、が全世界共通の決めごと。
        ///     月と雪の照り返しで形は読める明るさに保つ。
        ///   ★霧は**高**（14→70m）。他のどの世界より濃い ──
        ///     「手がかりを奪われる」世界なので、視界も奪ってよい。
        ///   ★環境光の地面色を**明るく**する。雪は下から光を返すので、
        ///     ここを暗くすると雪山ではなく夜の荒野になる。
        private static void SetupAirSnow()
        {
            RenderSettings.fog = true;
            RenderSettings.fogMode = FogMode.Linear;
            RenderSettings.fogColor = new Color(0.855f, 0.890f, 0.965f);   // #DAE3F6
            RenderSettings.fogStartDistance = 14f;
            RenderSettings.fogEndDistance = 70f;

            RenderSettings.ambientMode = AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = new Color(0.255f, 0.286f, 0.373f);      // #41495F 夜空
            RenderSettings.ambientEquatorColor = new Color(0.608f, 0.659f, 0.776f);  // #9BA8C6
            // ★地面からの返りを一番明るくする。雪山はここが要。
            RenderSettings.ambientGroundColor = new Color(0.740f, 0.780f, 0.870f);
            RenderSettings.ambientIntensity = 1f;

            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
            {
                if (l.type != LightType.Directional) continue;
                // 月。低い角度から青白く。影は落とすが薄く ──
                // 濃い影が出ると「輪郭が溶ける」が壊れる。
                l.transform.rotation = Quaternion.Euler(24f, 145f, 0f);
                l.color = new Color(0.78f, 0.84f, 1.00f);
                l.intensity = 0.55f;
                l.shadows = LightShadows.Soft;
                l.shadowStrength = 0.22f;
            }
            // ★空。地平の色に**霧と同じ値**を渡す（別々に書くとズレる）。
            //   雲は薄く ── 夜空に厚い雲を出すと、稜線のシルエットが読めなくなる。
            RenderSettings.skybox = SkyMaterial("Sky_Snow", new Color(0.13f, 0.16f, 0.26f),
                                                RenderSettings.fogColor, SunDirection(), 0.25f);
            DynamicGI.UpdateEnvironment();
        }

        // ── マテリアルの作り直し ───────────────────────────────────
        // ★`ToonMaterial` は**既にある資産を上書きしない。**
        //   インスペクタで触った値（輪郭線の太さなど）を消さないためだが、
        //   代わりに**コード側の色表を直しても既存の .mat には反映されない。**
        //   このメニューが、既存の資産へ色表と個別の設定を**入れ直す。**
        //
        // ★★資産を**消さないこと。**
        //   以前ここは `DeleteAsset` で消して作り直していた。**guid が変わるので、
        //   参照しているシーンが黙って壊れる** ── 実際 `Stage1_Layout` の 12 個中 9 個が
        //   解決しなくなり、扉以外が全部ピンクになった（造形／演出レーンの報告・2026-08-22）。
        //   壊れても**次に誰かが開くまで気づけない**のが厄介なところ。
        //   だから消さずに、同じ資産へ値を入れ直す形にしてある。
        [MenuItem("BellGame/ステージ配置/マテリアルを作り直す")]
        public static void ResetToonMaterials()
        {
            if (!Directory.Exists(ToonRoot))
            {
                Debug.Log("[BellGame] " + ToonRoot + " は無い。次のステージ生成で作られる");
                return;
            }
            var shader = ToonShader();
            int n = 0, miss = 0;
            foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { ToonRoot }))
            {
                string path = AssetDatabase.GUIDToAssetPath(guid);
                var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
                if (mat == null) continue;
                string name = Path.GetFileNameWithoutExtension(path);
                if (shader != null && mat.shader != shader) mat.shader = shader;
                if (name.EndsWith("_Instanced")) { miss++; continue; }   // 草・葉は別の作り
                ApplyToonSettings(mat, name);
                EditorUtility.SetDirty(mat);
                n++;
            }
            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
            Debug.Log($"[BellGame] トゥーンのマテリアル {n} 個を色表から入れ直した"
                      + (miss > 0 ? $"（インスタンシング用 {miss} 個は対象外）" : ""));
        }

        /// 歩いて確かめるための操作を置く。**本編のプレイヤーではない**
        /// （本編の移動・当たり・境界はゲームシステム側が持つ）。
        ///
        ///   既定のカメラをこの下へ移して目にする。`Camera.main` のままなので、
        ///   草と葉のインスタンシングもこのカメラを基準に間引かれる。
        /// 矩形で囲う版。白い部屋のような**四角い箱の中**で使う。
        ///   ★LayoutWalker は `radius <= 0` のときだけ halfExtents を見る作りなので、
        ///     円を 0 にしてから矩形を入れる（両方入れると円が勝つ）。
        ///   ⚠ ゲームシステムレーンが**コンパイルを通すために足しました**
        ///     （`MakeWalkerRect` の呼び出しだけがあって、本体がありませんでした）。
        ///     意図と違っていたら直してください。
        private static void MakeWalkerRect(Vector3 startPos, Vector3 center, Vector2 halfExtents)
        {
            var w = MakeWalker(startPos, 0f);
            if (w == null) return;
            w.center = center;
            w.halfExtents = halfExtents;
        }

        private static LayoutWalker MakeWalker(Vector3 startPos, float border)
        {
            var go = new GameObject("LayoutWalker");
            go.transform.position = new Vector3(startPos.x, startPos.y, startPos.z);

            var cc = go.AddComponent<CharacterController>();
            cc.height = 1.7f;
            cc.radius = 0.35f;
            cc.center = new Vector3(0f, 0.85f, 0f);
            cc.slopeLimit = 55f;
            cc.stepOffset = 0.4f;          // 門の石段（蹴上 0.18m）を登れるように

            var cam = Camera.main;
            if (cam == null)
            {
                var camGo = new GameObject("Main Camera");
                camGo.tag = "MainCamera";
                cam = camGo.AddComponent<Camera>();
                camGo.AddComponent<AudioListener>();
            }
            cam.transform.SetParent(go.transform, worldPositionStays: false);
            cam.transform.localPosition = new Vector3(0f, 1.6f, 0f);
            cam.transform.localRotation = Quaternion.identity;
            cam.nearClipPlane = 0.05f;
            cam.farClipPlane = 400f;

            var w = go.AddComponent<LayoutWalker>();
            w.eye = cam.transform;
            w.radius = border;
            return w;
        }

        /// 空気（霧と環境光）。**色の話の半分はここで決まる。**
        ///
        ///   ★霧は怖がらせるためではなく**奥行き**のため（WORLD_SETTING §6-4）。
        ///     距離で空の色へ寄せると、同じ緑でも手前と奥で違って見える ──
        ///     ベタ 1 色の平原がのっぺり見える原因の半分はこれが無いこと。
        ///   ★環境光を 3 色（空・地平・地面）にする。単色だと影側が濁った灰になり、
        ///     セル調の「影も色を持っている」感じが出ない。
        ///   ★影を真っ黒にしない ── 夜の世界も真っ暗にしない、という決定と同じ考え方。
        private static void SetupAir()
        {
            RenderSettings.fog = true;
            RenderSettings.fogMode = FogMode.Linear;
            // ★霧は導出値 field/mist #C8DBE2（palette.json）。**空の色と一致させること。**
            //   ずれると地平で継ぎ目が見える。スカイボックスを差し替えたらここも合わせる。
            RenderSettings.fogColor = new Color(0.784f, 0.859f, 0.886f);   // #C8DBE2
            RenderSettings.fogStartDistance = 28f;
            RenderSettings.fogEndDistance = 130f;

            RenderSettings.ambientMode = AmbientMode.Trilight;
            // 環境光も導出値から。空＝mist、地平＝light、地面＝deep（草原の列）。
            RenderSettings.ambientSkyColor = new Color(0.784f, 0.859f, 0.886f);      // #C8DBE2
            RenderSettings.ambientEquatorColor = new Color(0.557f, 0.671f, 0.714f);  // #8EABB6
            RenderSettings.ambientGroundColor = new Color(0.231f, 0.306f, 0.341f);   // #3B4E57
            RenderSettings.ambientIntensity = 1f;

            // 既定の Directional Light を昼の斜光へ。参考絵の光の向きに寄せる。
            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
            {
                if (l.type != LightType.Directional) continue;
                l.transform.rotation = Quaternion.Euler(38f, -35f, 0f);
                l.color = new Color(1f, 0.97f, 0.88f);
                l.intensity = 1.15f;
                l.shadows = LightShadows.Soft;
            }
            // ★空。地平の色に**霧と同じ値**を渡す（別々に書くとズレる）。
            RenderSettings.skybox = SkyMaterial("Sky_Grassland", new Color(0.29f, 0.52f, 0.78f),
                                                RenderSettings.fogColor, SunDirection(), 0.55f);
            DynamicGI.UpdateEnvironment();
        }

        /// 再生中は新しいシーンを作れない（`EditorSceneManager.NewScene` が例外を投げる）。
        /// 例外で落ちると原因が分かりにくいので、先に止めて理由を出す。
        private static bool EnsureNotPlaying()
        {
            if (!EditorApplication.isPlayingOrWillChangePlaymode) return true;
            Debug.LogWarning("[BellGame] 再生中はステージ配置を作れません。"
                             + "Play を止めてからもう一度実行してください。");
            return false;
        }

        private static UnityEngine.SceneManagement.Scene NewScene()
            => EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

        private static Transform MakeBox(string name, Vector3 center, Vector3 size)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            go.transform.position = center;
            go.transform.localScale = size;
            return go.transform;
        }

        /// 立ち位置の目印。**コライダーは外す**（付けると音響に本物の障害物として入る）。
        private static Transform Marker(string name, Vector3 pos, Color c)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.6f;
            Object.DestroyImmediate(go.GetComponent<Collider>());
            var shader = ToonShader();
            if (shader != null) go.GetComponent<Renderer>().sharedMaterial = new Material(shader) { color = c };
            return go.transform;
        }

        private static void Adjust(Transform t, AcousticMaterialPreset preset, float absorptionScale)
        {
            var s = t.gameObject.GetComponent<AcousticSurface>();
            if (s == null) s = t.gameObject.AddComponent<AcousticSurface>();
            s.mode = AcousticSurfaceMode.Adjusted;
            s.material = preset;
            s.absorptionScale = absorptionScale;
        }

        // ── 材質 ───────────────────────────────────────────────────
        // ★このステージのマテリアルは**全部 `BellGame/Toon` 1 枚**で作る。
        //   `BellGameDressing` が作るのは Standard（PBR）のフォトスキャン材質で、
        //   アニメ調とは陰影の付き方が違う。混ぜると草だけセル調・地面は写実、になる。
        //   ステージ 1 が先にアニメ調へ行くので、ここは自前で持つ。
        //   （他のステージが追いつくとき、この表をまるごと dressing 側へ移せばよい）
        private const string ToonRoot = "Assets/Materials/BellGameToon";

        private static readonly Dictionary<string, Color> kPalette = new()
        {
            ["Grass_Ground"]      = new Color(0.43f, 0.61f, 0.27f),   // 明るい黄緑。参考絵の草原
            ["Grass_Ground_Dark"] = new Color(0.30f, 0.40f, 0.27f),   // ボーダーの外。霧で青ざめる前提で黒くしすぎない
            ["Grass_Blade"]       = new Color(0.44f, 0.60f, 0.26f),
            ["Grass_Blade2"]      = new Color(0.28f, 0.42f, 0.19f),
            ["Grass_Dry"]         = new Color(0.66f, 0.60f, 0.34f),
            ["Grass_Flower"]      = new Color(0.94f, 0.92f, 0.78f),
            ["Grass_Shrub"]       = new Color(0.22f, 0.34f, 0.16f),
            ["Grass_Rock"]        = new Color(0.64f, 0.66f, 0.64f),
            ["Shrine_Stone"]      = new Color(0.60f, 0.64f, 0.68f),   // 淡い青白い石。明るすぎると白飛びする
            ["Shrine_Column"]     = new Color(0.68f, 0.71f, 0.74f),   // 柱。壁より少し明るく＝溶けない
            ["Shrine_Step"]       = new Color(0.53f, 0.56f, 0.58f),   // 石段・崩れた石。踏まれて汚れた側
            ["Shrine_Deep"]       = new Color(0.44f, 0.48f, 0.53f),   // アーチの奥。影が溜まる所
            ["Door_Aqua"]         = new Color(0.62f, 0.84f, 0.90f),   // 水色。先端で白へ
            ["Wood_Beam"]         = new Color(0.36f, 0.29f, 0.22f),
            ["Wood_Door"]         = new Color(0.44f, 0.32f, 0.20f),
            ["Tree_Leaf"]         = new Color(0.30f, 0.46f, 0.20f),
            ["River_Water"]       = new Color(0.42f, 0.70f, 0.80f),
            ["River_Bank"]        = new Color(0.46f, 0.43f, 0.38f),   // 濡れた砂利。★緑とは明度ではなく**色相**で離す
            ["Bell_Bronze"]       = new Color(0.72f, 0.60f, 0.34f),

            // ── 扉（**全世界で共通の 1 種類**）と白い部屋 ──
            ["Door_Wood"]         = new Color(0.48f, 0.36f, 0.24f),   // 板と框。木の茶
            ["Door_WoodDark"]     = new Color(0.38f, 0.28f, 0.19f),   // 枠とケーシング。板より一段暗く
            ["Brass_Old"]         = new Color(0.66f, 0.55f, 0.29f),   // 取っ手・蝶番
            ["Brick_Old"]         = new Color(0.47f, 0.30f, 0.25f),   // 足元の崩れた煉瓦
            ["Ivy_Leaf"]          = new Color(0.40f, 0.50f, 0.34f),
            ["White_Floor"]       = new Color(0.80f, 0.81f, 0.82f),
            ["White_Wall"]        = new Color(0.87f, 0.88f, 0.88f),
            ["White_Ceil"]        = new Color(0.91f, 0.92f, 0.93f),
            ["White_Trim"]        = new Color(0.72f, 0.74f, 0.76f),

            // ── 崩壊都市。WORLD_VISUAL.md §2 の導出パレット。**手で選ばない・足さない** ──
            //   影 #36434F / 中間 #56697A / ハイライト #8195A5 / 霧 #B6C3CD / 植生 #6E7F63
            ["City_Concrete"]     = new Color(0.49f, 0.55f, 0.61f),   // 躯体の枠。中間とハイライトの間
            ["City_Deep"]         = new Color(0.34f, 0.41f, 0.48f),   // 窪みの底と屋根裏。これが窓に見える
            ["City_Metal"]        = new Color(0.21f, 0.26f, 0.31f),   // 鉄筋・鉄骨。影の色 #36434F
            ["City_Rubble"]       = new Color(0.37f, 0.45f, 0.50f),
            ["Park_Grass"]        = new Color(0.40f, 0.50f, 0.36f),   // 公園の芝。都市の青灰の中で唯一の緑

            // ── 雪山。★純白にしない（WORLD_VISUAL: 白は #DAE3F6 基調）──
            ["Snow_Ground"]       = new Color(0.855f, 0.890f, 0.965f),   // 雪原
            ["Snow_Ridge"]        = new Color(0.820f, 0.860f, 0.945f),   // 稜線。雪原よりわずかに沈める
            ["Snow_Rock"]         = new Color(0.240f, 0.255f, 0.300f),   // 露岩。**一面の白で唯一の暗い物**
            ["Snow_Tree"]         = new Color(0.800f, 0.840f, 0.920f),   // 樹氷
            ["Snow_Far"]          = new Color(0.250f, 0.280f, 0.380f),   // 遠景の山脈
            ["City_Ground"]       = new Color(0.30f, 0.35f, 0.39f),   // 割れた舗装。建物より暗く沈める
            ["City_Tower"]        = new Color(0.46f, 0.52f, 0.58f),   // 上層（音響の箱より上の飾り）。窓はシェーダ
            ["City_Far"]          = new Color(0.21f, 0.26f, 0.31f),   // 遠景のビル群。霧に沈むシルエット
            ["City_Road"]         = new Color(0.18f, 0.22f, 0.25f),   // 車道。地面より暗く沈める
            ["City_Walk"]         = new Color(0.29f, 0.34f, 0.38f),   // 歩道
            ["City_Line"]         = new Color(0.72f, 0.76f, 0.79f),   // 横断歩道の白線
            ["City_LineYellow"]   = new Color(0.56f, 0.50f, 0.30f),   // 中央線。**唯一の暖色**。褪せた塗料に留める
            ["Wood_Worn"]         = new Color(0.40f, 0.34f, 0.27f),   // くたびれた板戸（草原以外の三世界で共通）
            ["Iron_Rust"]         = new Color(0.26f, 0.24f, 0.22f),   // 帯金具・蝶番
        };

        // ── 空 ─────────────────────────────────────────────────────
        // ★配置シーンはこれまで**スカイボックスを何も入れていなかった**（Unity 既定のまま）。
        //   本編は `WorldSky` が写真の HDRI を入れているが、そちらは
        //   地上のセル調と質感が食い違うので、ここは手続き生成の空を使う。
        //
        // ★地平の色は**必ず霧の色と同じ値を渡す。**
        //   ずれると、遠景が霧で空へ溶け切る手前に色の段ができる。
        //   だから引数で受け取る ── 2 箇所に書くと必ずズレる。
        /// 扉の板を置く。**見た目だけでなく当たり判定も付ける。**
        ///   ★配置シーンにはギミック（`PhysicsDoor`）が無いので、板は動かない。
        ///     それでもコライダーが無いと**閉じた扉をすり抜けられる**ので、歩いて
        ///     確かめるシーンとして成立しない。本編では物理の板が同じ役をする。
        ///   ★MeshCollider ではなく BoxCollider。板は 6 枚の鏡板で凹凸があり、
        ///     メッシュのままだと当たり判定が無駄に細かい。寸法は開口と同じ。
        private static GameObject PlaceDoorSlab(Vector3 pos, Quaternion rot, Transform parent)
        {
            var slab = Place("Assets/Models/Prop_DoorSlab.obj", "Ref_DoorSlab", pos, rot, false);
            if (slab == null) return null;
            var bc = slab.AddComponent<BoxCollider>();
            bc.size = new Vector3(1.4f, 3.0f, 0.08f);   // 開口 1.4×3.0m（M8 の契約値）
            if (parent != null) slab.transform.SetParent(parent, worldPositionStays: true);
            return slab;
        }

        /// 雨の筒 1 枚ぶんのマテリアル。近い筒は太く速く、遠い筒は細く遅く。
        private static Material RainMaterial(string name, float columns, float speed, float len)
        {
            const string dir = "Assets/Materials/BellGameToon";
            string path = dir + "/" + name + ".mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            var sh = Shader.Find("BellGame/Rain");
            if (sh == null) { Debug.LogError("[BellGame] シェーダ BellGame/Rain が見つからない"); return mat; }
            if (mat != null) { if (mat.shader != sh) mat.shader = sh; return mat; }
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            mat = new Material(sh);
            mat.SetFloat("_Columns", columns);
            mat.SetFloat("_Speed", speed);
            mat.SetFloat("_Length", len);
            // 崩壊都市の霧の色に寄せる。白い筋だと曇り空から浮く。
            mat.SetColor("_Color", new Color(0.71f, 0.76f, 0.80f));
            AssetDatabase.CreateAsset(mat, path);
            AssetDatabase.SaveAssets();
            return mat;
        }

        private static Material SkyMaterial(string name, Color zenith, Color horizon,
                                            Vector3 sunDir, float clouds)
        {
            const string dir = "Assets/Materials/BellGameToon";
            string path = dir + "/" + name + ".mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            var sh = Shader.Find("BellGame/ToonSky");
            if (sh == null)
            {
                Debug.LogError("[BellGame] シェーダ BellGame/ToonSky が見つからない — "
                               + "Assets/Shaders/BellToonSky.shader を確認");
                return mat;
            }
            if (mat != null) { if (mat.shader != sh) mat.shader = sh; return mat; }
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);

            mat = new Material(sh);
            mat.SetColor("_ZenithColor", zenith);
            mat.SetColor("_HorizonColor", horizon);
            // 地平より下は霧より少し暗く。実際はほぼ見えない保険。
            mat.SetColor("_GroundColor", horizon * 0.62f);
            mat.SetVector("_SunDir", sunDir.normalized);
            mat.SetFloat("_CloudAmount", clouds);
            AssetDatabase.CreateAsset(mat, path);
            AssetDatabase.SaveAssets();
            return mat;
        }

        /// 太陽の向き。`SetupAir*` が入れた Directional Light から取る
        /// （角度を 2 箇所に書かないため）。
        private static Vector3 SunDirection()
        {
            foreach (var l in Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
                if (l.type == LightType.Directional) return -l.transform.forward;
            return new Vector3(0.4f, 0.55f, -0.73f);
        }

        private static Shader ToonShader()
        {
            var sh = Shader.Find("BellGame/Toon");
            if (sh == null)
                Debug.LogError("[BellGame] シェーダ BellGame/Toon が見つからない — "
                               + "Assets/Shaders/BellToon.shader を確認");
            return sh;
        }

        /// 名前で引くトゥーンマテリアル。無ければ作って資産として置く
        /// （インスペクタで段数・リム・色を触れるようにするため）。
        private static Material ToonMaterial(string name)
        {
            string path = ToonRoot + "/" + name + ".mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            var shader = ToonShader();
            if (shader == null) return mat;
            if (mat != null)
            {
                if (mat.shader != shader) mat.shader = shader;   // 旧シェーダで作った資産を拾い直す
                return mat;
            }
            if (!Directory.Exists(ToonRoot)) Directory.CreateDirectory(ToonRoot);
            mat = new Material(shader);
            ApplyToonSettings(mat, name);
            AssetDatabase.CreateAsset(mat, path);
            AssetDatabase.SaveAssets();
            return mat;
        }

        /// 1 枚のマテリアルに色表と個別の設定を入れる。**新規にも既存にも同じものを掛ける。**
        ///
        ///   ★冒頭で機能を全部 0 に戻してから掛けること。
        ///     戻さないと、前の版で入れた値（雨の濡れ・ひび割れなど）が
        ///     名前の分岐に入らないまま**残り続ける**。
        private static void ApplyToonSettings(Material mat, string name)
        {
            // 既定へ戻す。ここに載っていない機能を後で足したら、ここにも足すこと。
            foreach (var f in new[] { "_PatchAmount", "_HeightAmount", "_WaterAmount",
                                      "_CrackAmount", "_WindowAmount", "_WetAmount",
                                      "_PanelAmount", "_TipBlend", "_TipFromUv", "_OutlineWidth" })
                if (mat.HasProperty(f)) mat.SetFloat(f, 0f);
            if (mat.HasProperty("_Steps")) mat.SetFloat("_Steps", 3f);
            if (mat.HasProperty("_ShadeFloor")) mat.SetFloat("_ShadeFloor", 0.45f);
            if (mat.HasProperty("_RimAmount")) mat.SetFloat("_RimAmount", 0.25f);

            // ★色表に無い名前は黙って灰色にしない。**気づけないまま白い物が残る**
            //   （`Grass_Ground` を .mtl に書き忘れて、扉の丘が白一色になっていた）。
            if (!kPalette.TryGetValue(name, out var c))
            {
                c = new Color(1f, 0f, 1f);   // わざと目立つ色にする
                Debug.LogWarning($"[BellGame] 色表に '{name}' が無い — マゼンタで出す。"
                                 + "BellGameLayout.kPalette に足すこと");
            }
            // ⚠ ここに `mat = new Material(shader);` が紛れ込んでいて、コンパイルが
            //   止まっていました（`shader` はこの関数のスコープに無い）。外しました。
            //   この関数は**渡されたマテリアルに設定を掛ける**役なので、
            //   ここで新しく作ると呼び出し側の資産に何も反映されません。
            //   直上のコメントも二重になっていたので 1 つにしました。
            //   （ゲームシステムレーン 2026-08-22）
            mat.color = c;
            if (name == "Door_Aqua")
            {
                // 設定画の「水色 → 白 でグラデーション」。
                // ★_TipFromUv = 1。頂点 Y から取ると _BladeHeight(0.35m) で割られて、
                //   高さ 2.2m の板は下から 0.35m で振り切って上が一色になる（実際そうなっていた）。
                mat.SetColor("_TipColor", Color.white);
                mat.SetFloat("_TipBlend", 1f);
                mat.SetFloat("_TipFromUv", 1f);
            }
            else if (name == "River_Bank")
            {
                // ★土手が草と同じような緑に見えていた。原因は**明度だけで離していた**こと。
                //   セル調は段が付くので、明るい緑と明るい砂利は同じ段に落ちて溶ける。
                //   色相で離す（緑 → 黄土）。彩度は落としたまま。
                mat.SetColor("_ColorB", new Color(0.34f, 0.32f, 0.29f));
                mat.SetFloat("_PatchAmount", 0.60f);
                mat.SetFloat("_PatchScale", 0.55f);   // 周期 約 1.8m の砂利の斑
                mat.SetFloat("_Steps", 3f);
                mat.SetFloat("_RimAmount", 0f);
                // ★水際は濡れて暗く、天端は乾いて明るい。
                //   `_HeightTo` は `_HeightFrom` より**大きくないと効かない**
                //   （シェーダが max(差, 0.001) で割るので、逆にすると段差になる）。
                //   なので**基本色を濡れた側**にして、高いところへ乾いた色を混ぜる。
                mat.SetColor("_HeightTint", new Color(0.74f, 0.70f, 0.59f));
                mat.SetFloat("_HeightAmount", 0.80f);
                mat.SetFloat("_HeightFrom", 0.35f);   // 水際のすぐ上から
                mat.SetFloat("_HeightTo", 1.15f);     // 土手の天端
            }
            else if (name == "River_Water")
            {
                // ★流れはシェーダが作る。テクスチャは使わない。
                //   UV の v が川筋に沿って伸びているので、曲がっても流れが川下を向く。
                mat.SetFloat("_WaterAmount", 0.55f);
                // ★流れの速さ 0.30 → 0.85（発注者の指示）。
                //   合わせて模様を細かくする ── 粗いまま速くすると、
                //   大きな塊がスライドして見えて「流れ」ではなく「ずれ」になる。
                mat.SetFloat("_WaterSpeed", 0.85f);
                mat.SetFloat("_WaterScale", 6.5f);
                mat.SetColor("_FoamColor", new Color(0.86f, 0.94f, 0.97f));
                mat.SetFloat("_Steps", 4f);        // 水は段を細かく（面が滑らかなので）
                mat.SetFloat("_RimAmount", 0.30f); // 水際が光る
            }
            else if (name == "Shrine_Stone" || name == "Shrine_Column" || name == "Shrine_Step")
            {
                // ★石もベタ 1 色にしない。白い面が大きいと、セル調では一気に飛ぶ。
                //   細かい斑（周期 約 4m）で汚れとムラを入れる。
                mat.SetColor("_ColorB", new Color(0.42f, 0.47f, 0.50f));
                mat.SetFloat("_PatchAmount", 0.40f);
                mat.SetFloat("_PatchScale", 0.25f);
            }
            else if (name == "Grass_Ground")
            {
                // ★ベタ 1 色にしない。広い面が一色だと奥行きが死ぬ。
                //   長波長の斑で明るい緑と黄緑を混ぜ、高いところ（扉の丘）を少し明るく。
                mat.SetColor("_ColorB", new Color(0.55f, 0.70f, 0.33f));
                mat.SetFloat("_PatchAmount", 0.55f);
                mat.SetFloat("_PatchScale", 0.035f);
                mat.SetColor("_HeightTint", new Color(0.58f, 0.72f, 0.36f));
                mat.SetFloat("_HeightAmount", 0.35f);
                mat.SetFloat("_HeightFrom", 0f);
                mat.SetFloat("_HeightTo", 1.2f);
            }
            else if (name == "City_Concrete" || name == "City_Deep" || name == "City_Rubble")
            {
                // ★コンクリの面は広いので、ベタ 1 色だと段ボールに見える。
                //   周期 約 3m の斑で汚れと雨だれのムラを入れる。
                //   曇りで陰影が弱いぶん、明度差はここで作るしかない。
                mat.SetColor("_ColorB", new Color(0.27f, 0.33f, 0.39f));
                mat.SetFloat("_PatchAmount", 0.45f);
                mat.SetFloat("_PatchScale", 0.33f);
                // 足元ほど汚れて暗い（雨で洗われるのは上）。
                mat.SetColor("_HeightTint", new Color(0.58f, 0.63f, 0.68f));
                mat.SetFloat("_HeightAmount", 0.30f);
                mat.SetFloat("_HeightFrom", 0f);
                mat.SetFloat("_HeightTo", 6.5f);
                // 躯体の水平面（庇・割れた床・瓦礫の天端）も濡らす。地面より控えめに。
                mat.SetFloat("_WetAmount", 0f);    // 同上（雨を戻すなら 0.35）
                mat.SetColor("_WetColor", new Color(0.13f, 0.17f, 0.21f));
                mat.SetFloat("_RippleScale", 1.4f);
                mat.SetFloat("_RippleSpeed", 1.6f);
                mat.SetFloat("_RippleStrength", 0.10f);
            }
            else if (name == "Door_Wood" || name == "Door_WoodDark")
            {
                // 木目。細かい**縦**の縞にしたいが、ムラはワールド XZ で拾うので
                // 縦縞にはならない ── 代わりに細かい斑で「塗りではない」感じだけ出す。
                mat.SetColor("_ColorB", name == "Door_Wood"
                             ? new Color(0.36f, 0.26f, 0.17f) : new Color(0.28f, 0.20f, 0.14f));
                mat.SetFloat("_PatchAmount", 0.42f);
                mat.SetFloat("_PatchScale", 2.4f);
                // ★扉は全世界で唯一「同じ物」なので、少し光らせて主役に見せる。
                mat.SetFloat("_RimAmount", 0.22f);
                mat.SetFloat("_Steps", 3f);
            }
            else if (name == "City_Tower")
            {
                // ★上層の窓は**シェーダが塗る**。ジオメトリでは作らない。
                //   ここは音響の箱より上のただの飾りなので、三角形を使うのは丸損。
                //   ワールド座標の格子なので、箱に貼っても引き伸ばされない。
                mat.SetFloat("_WindowAmount", 1f);
                mat.SetVector("_WindowCell", new Vector4(2.6f, 3.2f, 0f, 0.9f));
                mat.SetVector("_WindowFill", new Vector4(0.62f, 0.52f, 0f, 0f));
                mat.SetColor("_WindowColor", new Color(0.11f, 0.14f, 0.17f));
                mat.SetColor("_ColorB", new Color(0.29f, 0.35f, 0.41f));
                mat.SetFloat("_PatchAmount", 0.35f);
                mat.SetFloat("_PatchScale", 0.28f);
                // 上へ行くほど霧の色へ寄せる。高い所は空気が厚い＝空気遠近。
                mat.SetColor("_HeightTint", new Color(0.64f, 0.70f, 0.75f));
                mat.SetFloat("_HeightAmount", 0.40f);
                mat.SetFloat("_HeightFrom", 4f);
                mat.SetFloat("_HeightTo", 24f);
            }
            else if (name == "City_Far")
            {
                // 遠景。霧が 80m で終わるので、ここは**シルエットの濃さ**だけの仕事。
                //   段を減らして平坦に潰す ── 面の陰影が出ると近くに見えてしまう。
                mat.SetFloat("_Steps", 2f);
                mat.SetFloat("_ShadeFloor", 0.85f);
                mat.SetFloat("_RimAmount", 0f);
                mat.SetColor("_HeightTint", new Color(0.55f, 0.61f, 0.67f));
                mat.SetFloat("_HeightAmount", 0.45f);
                mat.SetFloat("_HeightFrom", 0f);
                mat.SetFloat("_HeightTo", 34f);
            }
            else if (name == "City_Ground" || name == "City_Walk")
            {
                // ★普通のコンクリ。**パネル形式**（発注者の指示）。
                //   写真の質感は狙わない ── 欲しいのは「打ち放しの板が敷いてある」という
                //   読みで、それを作るのは細かい肌ではなく**目地の線と板ごとの明度差**。
                //   セル調の他の面と揃うのはこちら。
                mat.SetFloat("_PanelAmount", 0.85f);
                mat.SetFloat("_PanelSize", name == "City_Ground" ? 2.4f : 1.2f);  // 歩道は小割り
                mat.SetFloat("_PanelWidth", 0.035f);
                mat.SetColor("_PanelColor", new Color(0.16f, 0.19f, 0.22f));
                // ★ひび割れは切る。「荒れていない都市」なので、割れていると廃墟に戻る。
                mat.SetFloat("_CrackAmount", 0f);
                // 斑も弱く。パネルの明度差が主役なので、雲状のムラが乗ると濁る。
                mat.SetColor("_ColorB", new Color(0.34f, 0.38f, 0.41f));
                mat.SetFloat("_PatchAmount", 0.22f);
                mat.SetFloat("_PatchScale", 0.09f);
                mat.SetFloat("_Steps", 2f);
                mat.SetFloat("_ShadeFloor", 0.76f);
                mat.SetFloat("_RimAmount", 0f);
            }
            else if (name == "City_Road")
            {
                // ★ひび割れはシェーダが描く。参考写真のアスファルトを**イラスト調に**寄せる。
                //   写真の細かい網目をそのまま真似ると、セル調の他の面から浮く。
                //   線を太く・数を減らし、割れていない区画を残す。
                // 車道はアスファルト。ひび割れは残す（舗装は割れるもの）。
                mat.SetFloat("_CrackAmount", 0.7f);
                mat.SetFloat("_CrackScale", 0.30f);
                mat.SetFloat("_CrackWidth", 0.024f);
                mat.SetColor("_CrackColor", new Color(0.07f, 0.09f, 0.11f));
                // 染みとムラ。★段を減らして平坦に潰す ──
                //   路面に陰影の段が出ると、平らな道が波打って見える。
                mat.SetColor("_ColorB", new Color(0.13f, 0.17f, 0.20f));
                mat.SetFloat("_PatchAmount", 0.55f);
                mat.SetFloat("_PatchScale", 0.22f);
                mat.SetFloat("_Steps", 2f);
                mat.SetFloat("_ShadeFloor", 0.74f);
                // ★雨で濡れる。**雨は筋だけでは降って見えない** ── 人が読むのは
                //   空中の粒より地面の変化のほう（色が濃くなり、輪が広がる）。
                mat.SetFloat("_WetAmount", 0f);    // 雨を取り下げたので乾かす（雨を戻すなら 0.55）
                mat.SetColor("_WetColor", new Color(0.09f, 0.12f, 0.15f));
                mat.SetFloat("_RippleScale", 0.9f);
                mat.SetFloat("_RippleSpeed", 1.4f);
                mat.SetFloat("_RippleStrength", 0.14f);
                mat.SetFloat("_RimAmount", 0f);
            }
            else if (name == "City_Line" || name == "City_LineYellow")
            {
                // 白線・中央線。塗料なので陰影を持たせない（平らに乗っているだけ）。
                mat.SetFloat("_Steps", 2f);
                mat.SetFloat("_ShadeFloor", 0.88f);
                mat.SetFloat("_RimAmount", 0f);
                // 剥げ。ムラで下地の色を透かす。
                mat.SetColor("_ColorB", new Color(0.20f, 0.24f, 0.27f));
                mat.SetFloat("_PatchAmount", 0.45f);
                mat.SetFloat("_PatchScale", 0.9f);
            }
            else if (name == "Wood_Worn")
            {
                // 板戸。木目のかわりに細かい縦のムラ。★暖色はここと中央線だけ。
                mat.SetColor("_ColorB", new Color(0.28f, 0.23f, 0.18f));
                mat.SetFloat("_PatchAmount", 0.5f);
                mat.SetFloat("_PatchScale", 1.6f);
                mat.SetFloat("_RimAmount", 0.18f);
            }
            else if (name == "Grass_Ground_Dark")
            {
                // 縁の丘。稜線へ向かって青緑へ寄せると、遠景として奥に引っ込む。
                mat.SetColor("_ColorB", new Color(0.24f, 0.34f, 0.25f));
                mat.SetFloat("_PatchAmount", 0.6f);
                mat.SetFloat("_PatchScale", 0.03f);
                mat.SetColor("_HeightTint", new Color(0.42f, 0.55f, 0.55f));
                mat.SetFloat("_HeightAmount", 0.55f);
                mat.SetFloat("_HeightFrom", 0.3f);
                mat.SetFloat("_HeightTo", 4.2f);
            }
        }

        private static Material GrassMaterial() => InstancedMaterial("Grass_Instanced", wholeSway: false);

        /// インスタンシング用のマテリアル。同じ `BellGame/Toon` を、
        /// 裏面あり・風あり・遠方で縮む設定にしたもの。
        ///   wholeSway: 草は根元を固定して揺らす / 葉は房ごと動かす
        private static Material InstancedMaterial(string name, bool wholeSway)
        {
            string path = ToonRoot + "/" + name + ".mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            var shader = ToonShader();
            if (shader == null) return mat;
            if (mat != null)
            {
                if (mat.shader != shader) mat.shader = shader;
                mat.enableInstancing = true;
                return mat;
            }
            if (!Directory.Exists(ToonRoot)) Directory.CreateDirectory(ToonRoot);

            mat = new Material(shader) { enableInstancing = true };
            mat.SetFloat("_Cull", 0f);                       // 両面。片面だと回り込むと消える
            mat.SetFloat("_WholeSway", wholeSway ? 1f : 0f);
            mat.SetFloat("_WindAmp", wholeSway ? 0.055f : 0.10f);
            mat.SetFloat("_TipBlend", 0.55f);
            mat.SetColor("_TipColor", wholeSway ? new Color(0.52f, 0.68f, 0.30f)
                                                : new Color(0.62f, 0.74f, 0.36f));
            if (!wholeSway)
            {
                // 描画距離の手前で縮める。間引きは CPU 側（farKeep）が持つので、
                // ここは「最後の数 m で消える」ぶんだけ。
                bool stalk = name.StartsWith("Stalk");
                mat.SetFloat("_FadeStart", stalk ? 34f : 24f);
                mat.SetFloat("_FadeEnd", stalk ? 40f : 30f);
                if (stalk) mat.SetFloat("_WindAmp", 0.16f);   // 背が高いぶん大きく揺れる
            }
            AssetDatabase.CreateAsset(mat, path);
            AssetDatabase.SaveAssets();
            return mat;
        }

        private static void Dress(Transform t, string materialName)
        {
            var r = t.GetComponent<Renderer>();
            var mat = ToonMaterial(materialName);
            if (r != null && mat != null) r.sharedMaterial = mat;
        }

        /// 生成した OBJ を置く。
        ///   ★既定でコライダーを全部剥がす。meshCollider:true のときだけ付け直す。
        ///   .mtl の材質名（Grass_Ground など）はトゥーンのマテリアルへ差し替える。
        private static GameObject Place(string objPath, string name, Vector3 pos, bool meshCollider)
            => Place(objPath, name, pos, Quaternion.identity, meshCollider);

        private static GameObject Place(string objPath, string name, Vector3 pos,
                                        Quaternion rot, bool meshCollider)
        {
            var src = AssetDatabase.LoadAssetAtPath<GameObject>(objPath);
            if (src == null)
            {
                Debug.LogWarning($"[BellGame] 見た目メッシュが無い: {objPath} — 飛ばす"
                                 + "（tools/StageModelGen を走らせると生成される）");
                return null;
            }
            var go = Object.Instantiate(src);
            go.name = name;
            go.transform.SetPositionAndRotation(pos, rot);

            foreach (var c in go.GetComponentsInChildren<Collider>()) Object.DestroyImmediate(c);

            if (meshCollider)
                foreach (var mf in go.GetComponentsInChildren<MeshFilter>())
                {
                    var mc = mf.gameObject.AddComponent<MeshCollider>();
                    mc.sharedMesh = mf.sharedMesh;
                }

            foreach (var r in go.GetComponentsInChildren<Renderer>())
            {
                var mats = r.sharedMaterials;
                for (int i = 0; i < mats.Length; i++)
                {
                    if (mats[i] == null) continue;
                    var m = ToonMaterial(mats[i].name.Replace(" (Instance)", "").Trim());
                    if (m != null) mats[i] = m;
                }
                r.sharedMaterials = mats;
            }
            return go;
        }

        private static void Save(UnityEngine.SceneManagement.Scene scene, string fileName, string note)
        {
            const string dir = "Assets/Scenes";
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            EditorSceneManager.SaveScene(scene, dir + "/" + fileName);
            Debug.Log("[BellGame] ステージ配置: " + fileName + " — " + note);
        }
    }
}
