// AcousticFlowTestScenes.cs (Editor 専用)
// メニュー [AcousticFlow > Test Scenes > ...] で、現象ごとの検証用シーンを自動生成する。
//   回折 / 透過 / 小部屋 / 広い部屋。各シーンに AcousticFlowSceneDemo（6音源・タップ算出）＋
//   IrConvolver（クリックで反射パターンを聞く）を入れる。Play → M で楽曲ミュート →
//   ヘッドホン＋Status Monitor（IRタップのプロット）で確認する。
//   ※音源は本編と同じ6ステム。座標は全部同じ場所に重ねる（検証用なので分散不要）。
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
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);   // 6音源を重ねる
            AddConvolver(srcPos);
            Save(scene, "Test_Diffraction.unity",
                "回折: 衝立の裏(+Z)に6音源。左右(A/D)に動くと縁を回り込む回折が変わる。Status Monitorで水色(回折)タップを確認。");
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
            AddDemo(listener, srcPos, AcousticMaterialPreset.Default);   // 6音源を重ねる（Default=石膏ボード相当）
            AddConvolver(srcPos);
            Save(scene, "Test_Transmission.unity",
                "透過: 密閉ボックス内の6音源。開口が無いので透過だけ(低域寄りにこもる)。occluderMaterialをConcrete/Glassに変えて比較。");
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
            AddConvolver(srcPos);
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
            AddConvolver(srcPos);
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
            AddConvolver(srcPos);
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

            // 音源2つ。扉が振れる向きに対して左右に置き、聞こえ始める順序の違いを出す。
            //   音源0（右・+X 側）… 隙間が開く方向。早い角度からダイレクトになる
            //   音源1（左・-X 側）… 振れた扉の板が覆う方向。最後まで透過のまま
            //   ※戸口が幅1mなので、直線が戸口を通るのは音源の X が概ね ±1m 以内のとき。
            //     それを超えると壁側で遮られるため、左右とも ±0.8m に置く。
            var srcRight = new Vector3(0.8f, 1.6f, 3f);
            var srcLeft = new Vector3(-0.8f, 1.6f, 3f);
            var srcs = AddDemo(listener, srcRight, AcousticMaterialPreset.Concrete);
            if (srcs.Length > 1) srcs[1].position = srcLeft;   // 音源1だけ左へ

            AddConvolver(srcRight, 0, name: "IrConvolver_Right");
            AddConvolver(srcLeft, 1, name: "IrConvolver_Left");

            Save(scene, "Test_SwingDoor.unity",
                "扉の開き角: 5/6 キーで扉を開閉（Inspector の SwingDoor.angleDeg でも可）。" +
                "右(+X)の音源が先に抜けてきて、左(-X)は扉の板に覆われて最後まで透過のまま。" +
                "開閉の2状態ではなく、途中の角度すべてで連続に変わることを確認する。");
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
            AddConvolver(srcPos);

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
            AddConvolver(srcPos);
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

        private static Transform MakeSource(string name, Vector3 pos)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.4f;
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
        private static Transform[] AddDemo(Transform listener, Vector3 srcPos, AcousticMaterialPreset material)
        {
            string[] events = { "Vocal", "Guitar", "Piano", "Bass", "Drums", "Other" };
            var srcs = new Transform[events.Length];
            for (int i = 0; i < events.Length; i++)
                srcs[i] = MakeSource("Source_" + events[i], srcPos);

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
        //   音源は足音（過渡音）。持続音の楽曲より、早期反射のパターンや HRTF による
        //   定位が聞き取りやすい（DEV_LOG E-3「テスト信号を目的で使い分ける」）。
        //   楽曲で確かめたいとき（コムフィルタ・粒感）は TokyoGeto.wav に差し替える。
        private const string kTestClipPath = "Assets/Audio/Footstep_Asphalt.mp3";

        // 音源 sourceIndex ぶんの畳み込み器。遅延も到来方向も遮蔽も音源ごとに違うので、
        // 鳴らしたい音源 1 つにつき 1 つ置く。clip を渡さなければ既定の足音を使う。
        private static IrConvolver AddConvolver(Vector3 pos, int sourceIndex = 0,
                                                string clipPath = null, string name = null)
        {
            var go = new GameObject(name ?? ("IrConvolver_" + sourceIndex));
            go.transform.position = pos;   // スピーカ(音源)と同じ位置に置く
            var src = go.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(clipPath ?? kTestClipPath);
            if (clip == null) clip = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (clip != null) src.clip = clip;

            var conv = go.AddComponent<IrConvolver>();
            conv.sourceIndex = sourceIndex;
            conv.tailLevel = 0.3f;                       // 全シーン統一
            conv.generateTestSignal = (clip == null);    // clipがあれば実音源を畳み込み / 無ければテスト信号
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
