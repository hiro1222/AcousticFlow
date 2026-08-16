// AcousticFlowTestScenes.cs (Editor 専用)
// メニュー [AcousticFlow > Test Scenes > ...] で、現象ごとの検証用シーンを自動生成する。
//   回折 / 透過 / 小部屋 / 広い部屋。各シーンに AcousticFlowSceneDemo と畳み込み器を入れる。
//   Play → M で楽曲ミュート → ヘッドホン＋Status Monitor（IRタップのプロット）で確認する。
//
//   ★音源オブジェクトと畳み込み器は**同じ GameObject**（IrConvolver_Right など）。
//     以前は楽曲の6ステム（Source_Vocal 〜 Source_Other）を別に作っていたが、
//     畳み込み器は1〜2個しか置かないので、残りは Wwise 無効ビルドでは鳴らないまま
//     ヒエラルキーに並ぶだけだった。鳴る本数だけ作り、名前を役割に合わせてある。
//
//   畳み込み器は C# 経路(IrConvolver) と C++ 経路(VoiceConvolver) を**両方**載せ、
//   AcousticFlowSceneDemo が片方だけ有効にする（実行中に Y キーで切替）。
//   扉の開閉でタップが入れ替わるときの補間は C++ 側にしか無いので、
//   「開けた瞬間ガタっと変わる」の確認は必ず C++ 経路で行うこと。
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public static class AcousticFlowTestScenes
    {
        // ── 回折確認：有限の衝立で直接を塞ぎ、縁を回り込む回折を聞く（材質Concreteで透過を抑制）──
        [MenuItem("AcousticFlow/Test Scenes/Diffraction (回折)")]
        public static void Diffraction()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -4f));
            MakeBox("Floor", new Vector3(0f, -0.5f, 0f), new Vector3(30f, 1f, 30f));
            var barrier = MakeBox("Barrier", new Vector3(0f, 2f, 0f), new Vector3(6f, 4f, 0.4f)); // 有限＝縁で回折
            Tint(barrier, new Color(0.85f, 0.55f, 0.35f));
            var srcPos = new Vector3(0f, 1.6f, 4f);   // 衝立の真裏
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);
            Save(scene, "Test_Diffraction.unity",
                "回折: 衝立の裏(+Z)に音源。左右(A/D)に動くと縁を回り込む回折が変わる。Status Monitorで水色(回折)タップを確認。");
        }

        // ── 透過確認：密閉ボックスに音源→開口なし＝透過だけ（材質Defaultで漏れを聞く）──
        [MenuItem("AcousticFlow/Test Scenes/Transmission (透過)")]
        public static void Transmission()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -3f));
            MakeBox("Ground", new Vector3(0f, -0.5f, 0f), new Vector3(30f, 1f, 30f));
            // 音源を囲む密閉ボックス（6面・開口なし）。listener 側の前面(-Z)越しに透過だけ届く。
            MakeBox("Box_Front", new Vector3(0f, 2f, 2f), new Vector3(4f, 4f, 0.3f));
            MakeBox("Box_Back", new Vector3(0f, 2f, 6f), new Vector3(4f, 4f, 0.3f));
            MakeBox("Box_Left", new Vector3(-2f, 2f, 4f), new Vector3(0.3f, 4f, 4f));
            MakeBox("Box_Right", new Vector3(2f, 2f, 4f), new Vector3(0.3f, 4f, 4f));
            MakeBox("Box_Top", new Vector3(0f, 4f, 4f), new Vector3(4f, 0.3f, 4f));
            MakeBox("Box_Bottom", new Vector3(0f, 0f, 4f), new Vector3(4f, 0.3f, 4f));
            var srcPos = new Vector3(0f, 2f, 4f);   // 密閉箱の中
            AddDemo(listener, srcPos, AcousticMaterialPreset.Default);   // Default=石膏ボード相当
            Save(scene, "Test_Transmission.unity",
                "透過: 密閉ボックス内の音源。開口が無いので透過だけ(低域寄りにこもる)。occluderMaterialをConcrete/Glassに変えて比較。");
        }

        // ── 小部屋：ITDGが小さく反射が密集 ──
        [MenuItem("AcousticFlow/Test Scenes/Small Room (小部屋)")]
        public static void SmallRoom()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -1f));       // 中心付近・音源との距離2m（大部屋と統一）
            MakeRoom("Small", new Vector3(4f, 2.5f, 3f), 0.4f);            // 4×3m・高2.5m
            var srcPos = new Vector3(0f, 1.6f, 1f);   // 中心付近
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);  // 硬い壁＝反射多く残響が分かりやすい
            Save(scene, "Test_SmallRoom.unity",
                "小部屋: 反射がすぐ返る(ITDG小)。Status MonitorのIRプロットが左に密集。" +
                "実行中に [ ] キーで部屋を拡縮できる（1/2/3=小/中/大プリセット）。");
        }

        // ── 広い部屋：ITDGが大きく反射が広がる ──
        [MenuItem("AcousticFlow/Test Scenes/Large Room (広い部屋)")]
        public static void LargeRoom()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -1f));       // 中心付近・音源との距離2m（小部屋と統一）
            MakeRoom("Large", new Vector3(30f, 12f, 24f), 0.6f);          // 30×24m・高12m
            var srcPos = new Vector3(0f, 1.6f, 1f);   // 中心付近
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);
            Save(scene, "Test_LargeRoom.unity",
                "広い部屋: 反射が遅れて返る(ITDG大)。IRプロットが右まで広がる。" +
                "実行中に [ ] キーで部屋を拡縮できる（1/2/3=小/中/大プリセット）。");
        }

        // ── 回折の連続性検証：右側だけ開いた仕切り壁 ──
        // 開口の縁を回り込む回折を、影の中↔外を行き来しながら聴く。
        // 影境界を跨ぐ瞬間に段差が出ないかを確認する。回折は前川の式（符号付き δ のみに依存）
        // なので、影の中↔外で連続に繋がり、高域ほど強く落ちる（＝回折によるローパス）はず。
        [MenuItem("AcousticFlow/Test Scenes/Diffraction Gap (回折・開口)")]
        public static void DiffractionGap()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -2f));   // 手前中央
            var room = MakeRoom("Gap", new Vector3(12f, 4f, 8f), 0.4f);  // 12×8m・高4m
            // 仕切り壁は部屋と連動しないので、拡縮すると位置関係が壊れる。無効にしておく。
            room.enableHotkeys = false;
            room.showGui = false;

            // 中央(Z=0)に仕切り壁。右寄り(+X)に 2m の開口を1か所だけ空ける。
            //   開口の左右に垂直な稜線ができ、そこが回折エッジになる。
            const float wallZ = 0f, thick = 0.3f, height = 4f;
            const float gapL = 3f, gapR = 5f;      // 開口の範囲（X）
            const float roomHalf = 6f;
            float leftW = gapL - (-roomHalf);      // -6 〜 3
            MakeBox("Partition_L", new Vector3(-roomHalf + leftW * 0.5f, height * 0.5f, wallZ),
                    new Vector3(leftW, height, thick));
            float rightW = roomHalf - gapR;        // 5 〜 6
            MakeBox("Partition_R", new Vector3(gapR + rightW * 0.5f, height * 0.5f, wallZ),
                    new Vector3(rightW, height, thick));

            // 音源は仕切りの奥・左寄り。開口の正面を外してあるので、必ず回折で回り込む必要がある。
            var srcPos = new Vector3(-3f, 1.6f, 2f);
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);
            Save(scene, "Test_DiffractionGap.unity",
                "回折(開口): 仕切りの右に2mの開口。音源は奥の左側なので直接は必ず遮蔽される。" +
                "A/Dで左右に動くと開口の縁を回り込む回折が変化する。" +
                "影境界を跨ぐとき段差なく連続に変わるかを確認する。");
        }

        // ── スイングドア：開き角で「直接聞こえる範囲」が連続的に変わる ──
        // docs/DIFFRACTION_DESIGN.md §7 の検証シーン。要件の5性質を一度に確かめられる。
        //
        // 開閉の2状態ではなく、途中のどの角度でも成立していることが要点。
        //   ・隙間が開き始めた方向(+X 側)の音源 … 早い段階からダイレクトに聞こえる
        //   ・振れた扉の板が覆う側(-X 側)の音源 … 扉が振り切るまで透過のまま
        // 事前計算では扱えない領域（角度ごとに焼き直せない）＝作品の主張そのもの。
        //
        // 5/6 キーで開閉。Inspector の SwingDoor.angleDeg を直接動かしてもよい。
        [MenuItem("AcousticFlow/Test Scenes/Swing Door (扉の開き角)")]
        public static void SwingDoorScene()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -3f));   // 戸口の手前

            var room = MakeRoom("Swing", new Vector3(14f, 3f, 14f), 0.4f);
            room.enableHotkeys = false;   // 仕切りと扉が連動しないので拡縮させない
            room.showGui = false;

            // Z=0 の仕切り壁。中央に幅 1m の戸口を空ける（X = -0.5 〜 +0.5）。
            const float wallZ = 0f, thick = 0.2f, height = 3f, half = 7f;
            const float gapL = -0.5f, gapR = 0.5f;
            float leftW = gapL - (-half);
            MakeBox("Partition_L", new Vector3(-half + leftW * 0.5f, height * 0.5f, wallZ),
                    new Vector3(leftW, height, thick));
            float rightW = half - gapR;
            MakeBox("Partition_R", new Vector3(gapR + rightW * 0.5f, height * 0.5f, wallZ),
                    new Vector3(rightW, height, thick));

            // ★戸口そのものをポータルとして置く（扉ではなく**枠の内側**）。
            //   開き具合は渡さない。エンジンが毎フレーム実形状から測るので、
            //   扉が板でどれだけ覆っているかも、音源が戸口の正面にあるかも結果に出る。
            {
                var pgo = new GameObject("Portal_Doorway");
                pgo.transform.position = new Vector3((gapL + gapR) * 0.5f, height * 0.5f, wallZ);
                pgo.transform.rotation = Quaternion.identity;   // forward=+Z が開口の法線
                var portal = pgo.AddComponent<AcousticPortal>();
                portal.width = gapR - gapL;
                portal.height = height;
            }

            // 扉。左枠(X=-0.5)を蝶番にして +Z 側（音源のある奥）へ振れる。
            //   高さは戸口と同じにすること。低いと上に隙間が残り「扉を回り込む」検証にならない。
            var hinge = new GameObject("Door_Hinge").transform;
            hinge.position = new Vector3(gapL, height * 0.5f, wallZ);
            var doorGo = GameObject.CreatePrimitive(PrimitiveType.Cube);
            doorGo.name = "Door";
            var door = doorGo.AddComponent<SwingDoor>();
            door.hinge = hinge;
            door.width = gapR - gapL;
            door.height = height;
            door.thickness = 0.06f;
            door.swingTowardPositiveZ = true;
            door.angleDeg = 0f;
            door.Apply();
            // ★扉と仕切りの明度差をはっきり付ける。扉がどこにあって何度開いているかは
            //   音を判断するための前提情報なので、見て分からないと確認にならない。
            Tint(doorGo.transform, new Color(0.95f, 0.55f, 0.15f));   // 扉＝明るい橙
            Tint(GameObject.Find("Partition_L").transform, new Color(0.30f, 0.33f, 0.40f));
            Tint(GameObject.Find("Partition_R").transform, new Color(0.30f, 0.33f, 0.40f));
            // ★扉だけ弱い材質にする。現実の部屋で音が漏れるのは壁ではなく扉。
            //   壁(Concrete) 125Hz 26dB / 4kHz 54dB に対し、
            //   木の扉は 15dB / 27dB。高域ほど差が大きい。
            //   壁全体を弱くすると均一に漏れて**どこから聞こえるか分からなくなる**が、
            //   扉だけ弱くすれば漏れてくる方向が扉になる ── 閉めていても気配が出る。
            doorGo.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.WoodDoor;

            // 音源2つ。扉が振れる向きに対して左右に置き、聞こえ始める順序の違いを出す。
            //   音源0（右・+X 側）… 隙間が開く方向。早い角度からダイレクトになる
            //   音源1（左・-X 側）… 振れた扉の板が覆う方向。最後まで透過のまま
            //   ※戸口が幅1mなので、直線が戸口を通るのは音源の X が概ね ±1m 以内のとき。
            //     それを超えると壁側で遮られるため、左右とも ±0.8m に置く。
            var srcRight = new Vector3(0.8f, 1.6f, 3f);
            var srcLeft = new Vector3(-0.8f, 1.6f, 3f);
            var srcs = AddDemo(listener, srcRight, AcousticMaterialPreset.Concrete,
                               new[] { "Right", "Left" });
            srcs[1].position = srcLeft;   // 音源1だけ左へ（畳み込み器ごと動く）

            Save(scene, "Test_SwingDoor.unity",
                "扉の開き角: 5/6 キーで扉を開閉（Inspector の SwingDoor.angleDeg でも可）。" +
                "右(+X)の音源が先に抜けてきて、左(-X)は扉の板に覆われて最後まで透過のまま。" +
                "開閉の2状態ではなく、途中の角度すべてで連続に変わることを確認する。");
        }

        // ── 回折だけを聴く：透過・反射・残響を全部落とす ──
        // 壁を完全不透過にし、早期反射と後期残響も切る。
        // **聞こえるのは「開口を回り込んできた音」だけ**になるので、
        // 回折の定位と減衰を、他の要素に紛れずに判断できる。
        //
        // 確かめること:
        //   ・音が開口の方向から聞こえるか（音源の方向ではなく）
        //   ・左右に歩いたとき、その方向が滑らかに動くか
        //   ・開口から遠ざかるほど自然に小さくなるか
        [MenuItem("AcousticFlow/Test Scenes/Diffraction Only (回折だけ)")]
        public static void DiffractionOnly()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -5f));

            // ★完全に閉じた箱を作り、仕切りの開口だけを唯一の通り道にする。
            //   床しか無いと壁の上も端も抜けてしまい、開口が複数できてテストが濁る。
            const float hw = 10f, h = 5f, hd = 10f, t = 0.3f;
            MakeBox("Floor",   new Vector3(0f, -t * 0.5f, 0f),      new Vector3(2 * hw, t, 2 * hd));
            MakeBox("Ceiling", new Vector3(0f, h + t * 0.5f, 0f),   new Vector3(2 * hw, t, 2 * hd));
            MakeBox("Wall_L",  new Vector3(-hw - t * 0.5f, h * 0.5f, 0f), new Vector3(t, h, 2 * hd));
            MakeBox("Wall_R",  new Vector3(hw + t * 0.5f, h * 0.5f, 0f),  new Vector3(t, h, 2 * hd));
            MakeBox("Wall_Back",  new Vector3(0f, h * 0.5f, -hd - t * 0.5f), new Vector3(2 * hw, h, t));
            MakeBox("Wall_Front", new Vector3(0f, h * 0.5f, hd + t * 0.5f), new Vector3(2 * hw, h, t));

            // Z=0 の仕切り。X=+2〜+4 に幅2mの開口を1つだけ（床から天井まで抜けた縦のスリット）。
            //   開口を中央から外してあるので、「音源の方向」と「開口の方向」が明確に別になる。
            const float gapL = 2f, gapR = 4f;
            float leftW = gapL - (-hw);
            MakeBox("Partition_L", new Vector3(-hw + leftW * 0.5f, h * 0.5f, 0f),
                    new Vector3(leftW, h, t));
            float rightW = hw - gapR;
            MakeBox("Partition_R", new Vector3(gapR + rightW * 0.5f, h * 0.5f, 0f),
                    new Vector3(rightW, h, t));

            // 音源は壁の奥・左寄り。開口(X=+3付近)とは反対側なので、
            // 直進では絶対に届かず、必ず開口を回り込むことになる。
            var srcPos = new Vector3(-4f, 1.6f, 5f);
            var srcs0 = AddDemo(listener, srcPos, AcousticMaterialPreset.Opaque);
            var conv = srcs0[0].GetComponent<IrConvolver>();

            // 回折だけを残す。
            conv.reflectionLevel = 1f;      // 'F'(回折)タップは直接音と同じ音量で鳴らす
            conv.tailLevel = 0f;            // 後期残響なし
            conv.enableReverbTail = false;
            var demo = Object.FindObjectOfType<AcousticFlowSceneDemo>();
            if (demo != null)
            {
                demo.enableEarlyReflections = false;   // 早期反射なし
                demo.enableReverb = false;             // 残響なし
                demo.useReflections = false;           // 反射込み遮蔽もなし
            }

            Save(scene, "Test_DiffractionOnly.unity",
                "回折だけ: 閉じた箱を仕切りで2部屋に分け、通り道は X=+2〜+4 の開口ひとつだけ。" +
                "壁は完全不透過、早期反射・残響もなし。聞こえるのは開口を回り込んできた音だけ。" +
                "音源は逆側(X=-4)の奥にあるので直進では届かない。" +
                "A/Dで左右に歩き、(1)音が開口の方向から来るか (2)歩いても方向が滑らかに動くか " +
                "(3)開口から離れるほど自然に小さくなるか を確認する。");
        }

        // ── メッシュ形状の検証：穴の空いた壁（箱では表せない形）──
        // 壁と戸口を「1枚のメッシュ」で作る。境界ボックスで判定していれば戸口も塞がるので、
        // 戸口の正面で音が通ることが、実形状を見ている証拠になる。
        // 左右に歩くと「戸口ごし → 壁ごし」に切り替わる。
        [MenuItem("AcousticFlow/Test Scenes/Mesh Wall (穴の空いた壁)")]
        public static void MeshWall()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -4f));

            var room = MakeRoom("Mesh", new Vector3(16f, 4f, 16f), 0.4f);
            room.enableHotkeys = false;   // 仕切りが連動しないので拡縮させない
            room.showGui = false;

            // Z=0 の仕切り壁。幅16m・高4m・厚み0.3m、中央に 1.2m×2.2m の戸口を抜く。
            //   ★BoxCollider ではなく MeshCollider で入れる。ここが検証の要点。
            var wall = new GameObject("Wall_WithDoorway");
            wall.transform.position = new Vector3(0f, 0f, 0f);
            var mf = wall.AddComponent<MeshFilter>();
            mf.sharedMesh = BuildDoorwayWall(16f, 4f, 0.3f, 1.2f, 2.2f);
            wall.AddComponent<MeshRenderer>();
            var mcol = wall.AddComponent<MeshCollider>();
            mcol.sharedMesh = mf.sharedMesh;
            Tint(wall.transform, new Color(0.75f, 0.75f, 0.8f));

            // 音源は戸口の真正面・壁の向こう。
            var srcPos = new Vector3(0f, 1.6f, 4f);
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);

            Save(scene, "Test_MeshWall.unity",
                "メッシュ形状: 壁と戸口が1枚の MeshCollider。戸口の正面(X=0)では音が素通りし、" +
                "左右へ歩いて壁の裏に入ると遮蔽される。境界ボックスで判定していれば戸口でも" +
                "遮蔽されるので、この差が実形状を見ている証拠になる。" +
                "※メッシュは現時点で回折の対象外（設計 §5-5。回折の作り直しと同時に対応）。");
        }

        // 壁＋戸口のメッシュを作る。厚みのある板を「左・右・まぐさ」の3ブロックで構成する。
        //   戸口は下端まで抜けている（床から高さ doorH まで、幅 doorW）。
        private static Mesh BuildDoorwayWall(float width, float height, float thick,
                                             float doorW, float doorH)
        {
            var verts = new List<Vector3>();
            var tris = new List<int>();

            void AddBox(Vector3 min, Vector3 max)
            {
                int b = verts.Count;
                verts.Add(new Vector3(min.x, min.y, min.z)); // 0
                verts.Add(new Vector3(max.x, min.y, min.z)); // 1
                verts.Add(new Vector3(max.x, max.y, min.z)); // 2
                verts.Add(new Vector3(min.x, max.y, min.z)); // 3
                verts.Add(new Vector3(min.x, min.y, max.z)); // 4
                verts.Add(new Vector3(max.x, min.y, max.z)); // 5
                verts.Add(new Vector3(max.x, max.y, max.z)); // 6
                verts.Add(new Vector3(min.x, max.y, max.z)); // 7
                int[] f = {
                    0,2,1, 0,3,2,   // -Z
                    4,5,6, 4,6,7,   // +Z
                    0,4,7, 0,7,3,   // -X
                    1,2,6, 1,6,5,   // +X
                    3,7,6, 3,6,2,   // +Y
                    0,1,5, 0,5,4,   // -Y
                };
                foreach (int i in f) tris.Add(b + i);
            }

            float hw = width * 0.5f, ht = thick * 0.5f, hd = doorW * 0.5f;
            AddBox(new Vector3(-hw, 0f, -ht), new Vector3(-hd, height, ht));      // 左
            AddBox(new Vector3(hd, 0f, -ht), new Vector3(hw, height, ht));        // 右
            AddBox(new Vector3(-hd, doorH, -ht), new Vector3(hd, height, ht));    // まぐさ

            var m = new Mesh { name = "DoorwayWall" };
            m.SetVertices(verts);
            m.SetTriangles(tris, 0);
            m.RecalculateNormals();
            m.RecalculateBounds();
            return m;
        }

        // ── 残響の検証：外 → 廊下 → 部屋 ──
        // 音響的に性格の違う3つのゾーンを歩いて通る。
        //   外   … 壁なし＝ほぼ無響。反射も残響も立たない
        //   廊下 … 狭くて長い。左右の壁が近く横方向の反射が強い＝独特の詰まった響き
        //   部屋 … 広くて拡散的。滑らかな尾が立つ
        // 音源はリスナーに一定距離で追従するので直接音は変わらない。
        // ＝聞こえ方の違いは全部「空間の応答」だけに由来する。
        [MenuItem("AcousticFlow/Test Scenes/Room Entry (外→廊下→部屋)")]
        public static void RoomEntry()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -12f));   // 外からスタート

            // 外の地面（壁なし＝ほぼ無響）。廊下と部屋の床もこれで兼ねる。
            MakeBox("Ground", new Vector3(0f, -0.5f, 0f), new Vector3(60f, 1f, 60f));

            // ── 廊下：幅2m・長さ8m・高2.5m（Z = -6 〜 +2）──
            //   両側の壁が近いので横方向の反射が短間隔で返る。部屋との差が出る要。
            const float corW = 2f, corH = 2.5f, corT = 0.3f;
            const float corZ0 = -6f, corZ1 = 2f;
            float corLen = corZ1 - corZ0;
            float corMidZ = (corZ0 + corZ1) * 0.5f;
            MakeBox("Corridor_W", new Vector3(-corW * 0.5f - corT * 0.5f, corH * 0.5f, corMidZ),
                    new Vector3(corT, corH, corLen));
            MakeBox("Corridor_E", new Vector3(corW * 0.5f + corT * 0.5f, corH * 0.5f, corMidZ),
                    new Vector3(corT, corH, corLen));
            MakeBox("Corridor_Ceil", new Vector3(0f, corH + corT * 0.5f, corMidZ),
                    new Vector3(corW + corT * 2f, corT, corLen));

            // ── 部屋：内寸 14×12m・高5m（Z = +2 〜 +14）──
            //   南壁(-Z)が廊下の突き当たりに一致し、そこに廊下と同じ幅・高さの開口を空ける。
            const float roomD = 12f;
            float roomCz = corZ1 + roomD * 0.5f;    // 南壁が corZ1 に来るように置く
            var roomGo = new GameObject("Room");
            roomGo.transform.position = new Vector3(0f, 0f, roomCz);
            var room = roomGo.AddComponent<ResizableRoom>();
            room.innerSize = new Vector3(14f, 5f, roomD);
            room.thickness = 0.4f;
            room.makeFloor = false;            // 地面と二重計上しない
            room.makeDoorway = true;           // 廊下から入れるように
            room.doorwayWidth = corW;          // 廊下と同じ幅
            room.doorwayHeight = corH;         // 廊下と同じ高さ（上はまぐさになる）
            room.keepListenerInside = false;   // 外や廊下にいるのに中へワープさせない
            // 廊下は部屋と連動しないので、拡縮すると接続が壊れる。サイズは固定。
            room.enableHotkeys = false;
            // 4キーでドアを開閉（開口を塞いで密閉空間にできる）。拡縮キーとは独立に効く。
            room.enableDoorKey = true;
            room.doorToggleKey = KeyCode.Alpha4;
            room.ApplyNow();

            // 音源はリスナーの 1.5m 前方に追従。
            //   完全に同一座標にすると 残響/直接=(r/r_c)² が 0 になり尾が消えるため
            //   （無指向の点音源に耳を密着させた状態＝物理的に正しい帰結）。
            //   現実に自分の声が響くのは声の指向性によるもので、それは未実装。
            var followOffset = new Vector3(0f, 0f, 1.5f);
            var srcPos = listener.position + followOffset;
            var srcs = AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);
            foreach (var s in srcs)
            {
                var f = s.gameObject.AddComponent<FollowTransform>();
                f.target = listener;
                f.offset = followOffset;
            }
            // 畳み込み機も一緒に追従させる（音源と同じ位置に置く運用のため）。
            var convGo = GameObject.Find("IrConvolverTest");
            if (convGo != null)
            {
                var cf = convGo.AddComponent<FollowTransform>();
                cf.target = listener;
                cf.offset = followOffset;
            }
            Save(scene, "Test_RoomEntry.unity",
                "残響(外→廊下→部屋): 音源がリスナーに1.5mで追従するので直接音は一定。" +
                "W で前進すると 外(ほぼ無響) → 廊下(幅2m・横方向の反射が強い) → 部屋(14×12m・拡散的) " +
                "と響きが3段階で変わる。違いは全部『空間の応答』だけに由来する。" +
                "4キーでドアを開閉でき、閉じると完全密閉になる（開口から抜ける分が消えて残響が伸びる）。" +
                "部屋のサイズは固定（廊下と接続しているため）。");
        }

        // ── 完全対応：いま実装されている要素を全部載せる ──
        // 個々の検証シーンは要素を切り分けるために何かを切っているが、これは逆に**全部入り**。
        // 実際のゲームに近い条件で、要素どうしが噛み合っているかを聴くためのもの。
        //
        //   透過（材質・6帯域）／回折（1次・2次・メッシュ稜線）／早期反射／後期残響（実測IR）
        //   ／HRTF（実測KEMAR）／メッシュ形状／実行時の形状変化（扉）／複数音源
        //
        // 構成: 大部屋 ─[メッシュの戸口＋スイングドア]─ 小部屋 ─[食い違いの2戸口]─ 奥の小部屋
        //   ・大部屋にリスナー。小部屋と奥の部屋にそれぞれ音源
        //   ・奥の音源へは戸口を2回抜ける必要がある＝2次回折
        [MenuItem("AcousticFlow/Test Scenes/Full (全要素)")]
        public static void FullScene()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -8f));

            const float t = 0.3f, h = 4f;
            // 外周（閉じた箱）。X=[-10,10] / Z=[-14,14] / 高さ h。
            const float hw = 10f, hd = 14f;
            MakeBox("Floor",      new Vector3(0f, -t * 0.5f, 0f),    new Vector3(2 * hw, t, 2 * hd));
            MakeBox("Ceiling",    new Vector3(0f, h + t * 0.5f, 0f), new Vector3(2 * hw, t, 2 * hd));
            MakeBox("Wall_L",     new Vector3(-hw - t * 0.5f, h * 0.5f, 0f), new Vector3(t, h, 2 * hd));
            MakeBox("Wall_R",     new Vector3(hw + t * 0.5f, h * 0.5f, 0f),  new Vector3(t, h, 2 * hd));
            MakeBox("Wall_Back",  new Vector3(0f, h * 0.5f, -hd - t * 0.5f), new Vector3(2 * hw, h, t));
            MakeBox("Wall_Front", new Vector3(0f, h * 0.5f, hd + t * 0.5f),  new Vector3(2 * hw, h, t));

            // 戸口と扉は同じ寸法を共有する。別々に書くと扉が戸口を塞ぎきらず、
            //   下に隙間が残ったり上へ突き抜けたりする（実際にそうなった）。
            const float doorW = 1.2f, doorH = 2.4f;

            // 仕切り1（Z=0）＝**メッシュ**の戸口つき壁。箱では表せない形をメッシュで扱えることの実証。
            var wall = new GameObject("Partition1_Mesh");
            var mf = wall.AddComponent<MeshFilter>();
            mf.sharedMesh = BuildDoorwayWall(2 * hw, h, t, doorW, doorH);
            wall.AddComponent<MeshRenderer>();
            wall.AddComponent<MeshCollider>().sharedMesh = mf.sharedMesh;
            Tint(wall.transform, new Color(0.72f, 0.74f, 0.8f));

            // その戸口に**スイングドア**（実行時の形状変化）。5/6 キーで開閉。
            //   蝶番の Y は**扉の中心の高さ**（扉は蝶番を中心に上下へ伸びる）。
            //   部屋の高さの半分を入れると扉が浮く。
            var hinge = new GameObject("Door_Hinge").transform;
            hinge.position = new Vector3(-doorW * 0.5f, doorH * 0.5f, 0f);
            var doorGo = GameObject.CreatePrimitive(PrimitiveType.Cube);
            doorGo.name = "Door";
            var door = doorGo.AddComponent<SwingDoor>();
            door.hinge = hinge;
            door.width = doorW;
            door.height = doorH;
            door.thickness = 0.06f;
            door.swingTowardPositiveZ = true;
            door.angleDeg = 35f;          // 半開きから始める（開き具合の変化を聴きやすい）
            door.Apply();
            // 扉は壁より弱い（段5: 材質の個別化）。壁 TL25dB / 扉 TL15dB の落差が
            // 「閉めていても向こうが聞こえる」という現実の構図を作る。
            doorGo.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.WoodDoor;
            // 扉は明るい橙。仕切り(灰青)との明度差で、開き具合が離れていても分かる。
            Tint(doorGo.transform, new Color(0.95f, 0.55f, 0.15f));

            // 戸口をポータルとして置く（扉ではなく枠の内側）。開き具合は渡さない。
            {
                var pgo = new GameObject("Portal_Doorway");
                pgo.transform.position = new Vector3(0f, doorH * 0.5f, 0f);
                pgo.transform.rotation = Quaternion.identity;
                var portal = pgo.AddComponent<AcousticPortal>();
                portal.width = doorW;
                portal.height = doorH;
            }
            // 奥の戸口（X=[4,5.5]）にもポータル。ここを通ると 2 部屋ぶん先まで届く。
            {
                var pgo = new GameObject("Portal_Doorway_Far");
                pgo.transform.position = new Vector3(4.75f, doorH * 0.5f, 7f);
                pgo.transform.rotation = Quaternion.identity;
                var portal = pgo.AddComponent<AcousticPortal>();
                portal.width = 1.5f;
                portal.height = doorH;
            }

            // 仕切り2（Z=+7）＝食い違いに置いた2戸口のうち奥側。1つ目とずらして 2次回折を作る。
            //   Z=0 の戸口は X=[-0.6,0.6]、こちらは X=[4,5.5]。直線ではどちらも通らない。
            MakeBox("Partition2_L", new Vector3(-3f, h * 0.5f, 7f), new Vector3(14f, h, t));
            MakeBox("Partition2_R", new Vector3(7.75f, h * 0.5f, 7f), new Vector3(4.5f, h, t));

            // 音源2つ。0=小部屋（戸口ごしに1次）／1=奥の部屋（2戸口ごしに2次）。
            var srcNear = new Vector3(-2f, 1.6f, 3.5f);
            var srcFar = new Vector3(4f, 1.6f, 11f);
            var srcs = AddDemo(listener, srcNear, AcousticMaterialPreset.Default,
                               new[] { "Near", "Far" });
            srcs[1].position = srcFar;

            Save(scene, "Test_Full.unity",
                "全要素: 透過/回折(1次・2次・メッシュ稜線)/早期反射/後期残響(実測IR)/HRTF(KEMAR)/" +
                "メッシュ形状/実行時の形状変化/複数音源 を全部載せた総合シーン。" +
                "大部屋 ─[メッシュの戸口＋スイングドア]─ 小部屋 ─[食い違いの戸口]─ 奥の部屋。" +
                "音源0は戸口ごし(1次回折)、音源1は奥の部屋(2次回折)。" +
                "5/6キーで扉を開閉すると、開き具合に応じて音源0の聞こえ方が連続的に変わる。" +
                "W で戸口をくぐると部屋の響きが切り替わる。");
        }

        // ── 共有ヘルパ ──
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

        private static Transform MakeBox(string name, Vector3 center, Vector3 size)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            go.transform.position = center;
            go.transform.localScale = size;
            return go.transform;
        }

        // 内寸 innerSize(幅x, 高y, 奥z) の囲まれた部屋（床y=0, 天井y=高）。
        // 壁6枚は ResizableRoom が持ち、実行中に [ ] キーや Inspector で内寸を変えられる。
        private static ResizableRoom MakeRoom(string prefix, Vector3 innerSize, float thick)
        {
            var go = new GameObject(prefix + "_Room");
            var room = go.AddComponent<ResizableRoom>();
            room.innerSize = innerSize;
            room.thickness = thick;
            room.ApplyNow();   // 壁を生成して保存対象にする（Awake を待たない）
            return room;
        }

        // 本編と同じ6ステム音源を srcPos に全部重ねて配置し、デモ制御を付ける（座標かぶりOK）。
        // 戻り値は生成した音源の Transform 群（追従などを後付けするため）。
        // ★音源オブジェクトと畳み込み器を**1つの GameObject にまとめる**。
        //   以前は Source_Vocal 〜 Source_Other の6個（楽曲のステム）を作り、
        //   その横に IrConvolver_* を別に置いていた。ところが畳み込み器は 1〜2 個しか
        //   置かないので、残りの音源は Wwise 無効ビルドでは**鳴らないまま並ぶだけ**だった。
        //   ヒエラルキーが読めなくなるので、鳴る本数だけ作って名前を役割に合わせる。
        //   音源の位置＝畳み込み器の位置になるので、ズレる余地も消える。
        //
        //   names に渡した数だけ音源を作る（例: {"Right","Left"} → IrConvolver_Right/Left）。
        private static Transform[] AddDemo(Transform listener, Vector3 srcPos,
                                           AcousticMaterialPreset material,
                                           string[] names = null)
        {
            string[] events = names ?? new[] { "Main" };
            var srcs = new Transform[events.Length];
            for (int i = 0; i < events.Length; i++)
                srcs[i] = AddConvolver(srcPos, i, name: "IrConvolver_" + events[i]).transform;

            var go = new GameObject("AcousticFlowSceneDemo");
            var demo = go.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = srcs[0];
            var extra = new Transform[events.Length - 1];
            for (int i = 1; i < events.Length; i++) extra[i - 1] = srcs[i];
            demo.extraSources = extra;
            demo.sourceEvents = events;
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = material;
            demo.firstPersonCamera = true;
            demo.distanceRef = 4f;   // 全シーン統一：距離減衰ゆるめ（直接音を前に）
            return srcs;
        }

        // IR畳み込みのテスト機。
        //   既定は楽曲（持続音）。こもり・コムフィルタ・粒感・残響の量が判断しやすい。
        //   過渡音（足音）だと反射パターンや HRTF の定位は掴みやすいが、
        //   音色の変化は分かりにくい（DEV_LOG E-3「テスト信号を目的で使い分ける」）。
        //   定位や反射の粒を見たいときは Footstep_Asphalt.mp3 に差し替える。
        //
        //   ※この音源は市販楽曲。**ローカルでの検証用**であって、配布物には含められない。
        //     ビルドを配る／リポジトリを公開する段になったら差し替えが要る。
        private const string kTestClipPath =
            "Assets/Audio/ロクデナシ「ブリザード」 Rokudenashi - Blizzard【Official Music Video】 - Rokudenashi (128k).wav";
        private const string kTransientClipPath = "Assets/Audio/Footstep_Asphalt.mp3";

        // 音源 sourceIndex ぶんの畳み込み器。遅延も到来方向も遮蔽も音源ごとに違うので、
        // 鳴らしたい音源 1 つにつき 1 つ置く。clip を渡さなければ既定の足音を使う。
        //
        // ★C# 経路(IrConvolver) と C++ 経路(VoiceConvolver) を**両方**載せる。
        //   同じ AudioSource を共有し、AcousticFlowSceneDemo が片方だけ enabled にする
        //   （Y キーで切替）。AudioSource を分けないのは、鳴らし比べのときに
        //   再生位置がズレて「同じ瞬間の音」を比較できなくなるため。
        private static IrConvolver AddConvolver(Vector3 pos, int sourceIndex = 0,
                                                string clipPath = null, string name = null)
        {
            // ★球にして**見える**ようにする。音源オブジェクトを兼ねているので、
            //   どこで鳴っているかが目で分かることが要る（見えている物と音の一致が狙い）。
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name ?? ("IrConvolver_" + sourceIndex);
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.4f;
            var src = go.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(clipPath ?? kTestClipPath);
            if (clip == null) clip = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (clip != null) src.clip = clip;

            var conv = go.AddComponent<IrConvolver>();
            conv.sourceIndex = sourceIndex;
            conv.tailLevel = 0.3f;                       // 全シーン統一
            conv.generateTestSignal = (clip == null);    // clipがあれば実音源を畳み込み / 無ければテスト信号

            var voice = go.AddComponent<VoiceConvolver>();
            voice.sourceIndex = sourceIndex;
            voice.tailLevel = 0.3f;                      // IrConvolver と揃える（A/B の条件を合わせる）
            voice.enabled = false;                       // 既定は Demo 側の useCppDsp が決める
            return conv;
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
            Debug.Log("[AcousticFlow] テストシーン生成: " + fileName + " — " + note);
        }
    }
}
