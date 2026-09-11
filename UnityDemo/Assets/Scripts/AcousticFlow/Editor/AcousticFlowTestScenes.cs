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

            // ★開口にポータルを置く。**部屋グラフの自動生成では拾えない**開口なので手置き。
            //   自動生成は「戸口で部屋が割れた所」に生える。ここは開口が 2m あり、
            //   種半径 0.6m の侵食では括れないので部屋が 1 つのままで、開口が立たない。
            //   ポータルを置くとフレネル帯域積分が担当し、円盤の標本化（1/N の階段）から
            //   解析的で連続な開口率へ変わる。実測（同じ戸口で自動生成の ON/OFF を比較）:
            //     0.1m 刻みの最大段差  合計 4.08 → 1.00 dB / 傾き 0.53 → 0.18 dB
            //   有無を聴き比べたいときは AcousticPortal.active を切る。
            var portalGo = new GameObject("Portal_Gap");
            portalGo.transform.position = new Vector3((gapL + gapR) * 0.5f, height * 0.5f, wallZ);
            portalGo.transform.rotation = Quaternion.identity;   // forward=+Z が開口の法線
            var portal = portalGo.AddComponent<AcousticPortal>();
            portal.width = gapR - gapL;      // 2m
            portal.height = height;          // 床から天井まで（仕切りは全高なので開口も全高）

            // 音源は仕切りの奥・左寄り。開口の正面を外してあるので、必ず回折で回り込む必要がある。
            var srcPos = new Vector3(-3f, 1.6f, 2f);
            AddDemo(listener, srcPos, AcousticMaterialPreset.Concrete);
            Save(scene, "Test_DiffractionGap.unity",
                "回折(開口): 仕切りの右に2mの開口。音源は奥の左側なので直接は必ず遮蔽される。" +
                "A/Dで左右に動くと開口の縁を回り込む回折が変化する。" +
                "影境界を跨ぐとき段差なく連続に変わるかを確認する。" +
                "Portal_Gap の active を切ると、ポータル無し（標本化）との聴き比べになる。");
        }

        // ── く字の廊下：曲がり角だけ。**回折だけで定位が出るか**を確かめる ──
        //
        //   両端にリスナーと音源を置く。**扉も開口も置かない。**
        //   直線では絶対に届かないので、聞こえる音はすべて角の縁を回り込んだもの。
        //
        //   確かめたいのは音量ではなく**方向**:
        //     ・音は「音源の方（壁の向こう）」ではなく「**曲がり角の方**」から聞こえるか
        //     ・角へ歩くとその方向が連続に動くか（＝音の方へ進むと角に着く）
        //     ・角を曲がった瞬間に像が飛ばないか
        //   B1 で回折タップが HRTF を通るようになったので、ここが本題になった。
        //   それ以前は回折が等パワーパンだけで、ITD も前後も無かった。
        //
        //   ★曲がり角は開口ではない。廊下は 3m 幅のまま括れないので部屋は割れず、
        //     ポータルは生えない ＝ 一般の稜線回折が担当する。**そこが狙い。**
        //     戸口／ポータルの検証は Test_DiffractionGap と Test_SwingDoor が持つ。
        [MenuItem("AcousticFlow/Test Scenes/L-Corridor (く字の廊下: 回折だけの定位)")]
        public static void LCorridor()
        {
            var scene = NewScene();
            const float w = 1.5f;      // 廊下の半幅（内寸 3m）
            const float h = 3.0f;      // 天井高
            const float t = 0.3f;      // 壁厚
            const float aEnd = -12f;   // 手前の脚の端（Z）
            const float bEnd = 12f;    // 奥の脚の端（X）

            // 手前の脚: x∈[-w,w], z∈[aEnd, w] ／ 奥の脚: x∈[-w,bEnd], z∈[-w,w]
            //   角は括れていないので部屋は割れない ＝ 開口が立たない ＝ 純粋な回折。
            MakeBox("Wall_A_Left",  new Vector3(-w - t*0.5f, h*0.5f, (aEnd + w)*0.5f),
                    new Vector3(t, h, w - aEnd + t));
            MakeBox("Wall_A_Right", new Vector3( w + t*0.5f, h*0.5f, (aEnd - w)*0.5f),
                    new Vector3(t, h, -w - aEnd));
            MakeBox("Wall_B_Far",   new Vector3((bEnd - w)*0.5f, h*0.5f,  w + t*0.5f),
                    new Vector3(bEnd + w + t, h, t));
            MakeBox("Wall_B_Near",  new Vector3((bEnd + w)*0.5f, h*0.5f, -w - t*0.5f),
                    new Vector3(bEnd - w, h, t));
            MakeBox("Cap_A", new Vector3(0f, h*0.5f, aEnd - t*0.5f), new Vector3(2*w + 2*t, h, t));
            MakeBox("Cap_B", new Vector3(bEnd + t*0.5f, h*0.5f, 0f), new Vector3(t, h, 2*w + 2*t));
            // 床と天井は脚ごとに 2 枚ずつ。角で重なるが問題ない。
            for (int k = 0; k < 2; k++) {
                const float ft = 0.3f;
                float y = (k == 0) ? -ft*0.5f : h + ft*0.5f;
                string s = (k == 0) ? "Floor" : "Ceil";
                MakeBox(s + "_A", new Vector3(0f, y, (aEnd + w)*0.5f),
                        new Vector3(2*w + 2*t, ft, w - aEnd + t));
                MakeBox(s + "_B", new Vector3((bEnd - w)*0.5f, y, 0f),
                        new Vector3(bEnd + w + t, ft, 2*w + 2*t));
            }

            // 両端にリスナーと音源。**あいだに扉も開口も無い。**
            //   直線は必ず角の内側の壁を突くので、届く音はすべて回折。
            var listener = MakeListener(new Vector3(0f, 1.6f, aEnd + 2f));   // 手前の脚の奥
            AddDemo(listener, new Vector3(bEnd - 2f, 1.6f, 0f),              // 奥の脚の奥
                    AcousticMaterialPreset.Concrete);

            Save(scene, "Test_LCorridor.unity",
                "く字の廊下（回折だけの定位）: 両端にリスナーと音源。扉も開口も無い。" +
                "直線は角の内壁で必ず塞がれるので、聞こえるのは角を回り込んだ音だけ。" +
                "確かめるのは音量ではなく**方向** ── 音は音源の方（壁の向こう）ではなく" +
                "『曲がり角の方』から聞こえるか、角へ歩くとその方向が連続に動くか、" +
                "曲がった瞬間に像が飛ばないか。W/A/S/D で歩いて、角に着いたら奥の脚へ曲がる。" +
                "曲がり角は開口ではないのでポータルは生えない（一般の稜線回折が担当）。");
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

            // ★音源は 1 本、戸口の正面（x = 0）から始める。
            //   以前は左右 2 本（Right +0.8 / Left -0.8）で「聞こえ始める順序の差」を
            //   見せていたが、収録では**同時に 2 本鳴っていると何が変わったのか分からない**。
            //   聞き分けたい差は収録卓の 7/8/9（x = -1 / 0 / +1）でテイクを分けて出す。
            //   x = 0 は直線が戸口のど真ん中を通るので、扉が開くと直接音が抜けてくる。
            //
            //   ★2026-09-08: 既定（kTestClipPath）を鳴らすようにした。以前は画面収録の WAV を
            //     名指ししていたが、それは「既定が市販楽曲だから収録に使えない」ための回避だった。
            //     既定を差し替えたので回避は要らない。収録に使うかは権利の確認しだい。
            AddDemo(listener, new Vector3(0f, 1.6f, 3f),
                    AcousticMaterialPreset.Concrete);

            // ★収録卓。手で 5/6 を押すとテイクごとに開く速さが変わるので、
            //   開 3s → 停 2s → 閉 3s を固定で流せるようにしておく。
            //   音源の X も 3 箇所（-1 / 0 / +1）に決め打ちで飛ばせる。
            //   リスナー z=-3 / 音源 z=+3 なので、直線が z=0 を横切る X は音源の X の半分:
            //     x=0 → 0.00（戸口 ±0.5 のど真ん中）/ x=±1 → 0.50（**戸口の縁ちょうど**）。
            //   ±1 は影境界の上なので、跳ねが残っていればここでいちばん出る置き方。
            {
                var rigGo = new GameObject("DoorSweepLab");
                var rig = rigGo.AddComponent<DoorSweepLab>();
                rig.door = door;
                rig.sourceIndex = 0;         // 音源は 1 本だけ
                rig.openAngleDeg = 120f;     // SwingDoor の上限まで開ける
                rig.sourceXs = new[] { -1f, 0f, 1f };
            }

            Save(scene, "Test_SwingDoor.unity",
                "扉の開き角: 音源 1 本・戸口の正面(x=0)・画面収録 WAV。" +
                "T キー／画面のボタンで 開3s(0→120°)→停2s→閉3s を固定で流せる（収録用）。" +
                "7/8/9 で音源の X を -1 / 0 / +1 へ（±1 は横切る点が戸口の縁ちょうど＝影境界の上）。" +
                "5/6 キーで手動の開閉、Inspector の SwingDoor.angleDeg でも可。" +
                "開閉の2状態ではなく、途中の角度すべてで連続に変わることを確認する。");
        }

        // ── 広さ と 隣室 を同じ場面で比べる（比較動画の素材用）──────────────
        // ★何のためにあるか
        //   この作品が音で伝えるものは 2 つある ── 同じ部屋の残響（広さで変わる）と、
        //   隣の部屋のこもり（壁 1 枚と扉の開き角で変わる）。別々のシーンで見せると
        //   条件が揃わないので比べられない。ここは 1 つのシーンに両方を入れて、
        //   **広さ・扉の角度を固定したまま立つ側だけを入れ替える**。
        //
        //   1/2/3 で広さ（片側の部屋 30 / 320 / 4320 m3 ≒ 10 倍刻み）
        //   Enter で 音源と同じ部屋 ⇄ 隣の部屋（戸口からの距離は両側で同じ＝鏡像）
        //   5/6   で扉の開き角（SwingDoor の既定キー）
        //
        // ★ポータルは置いていない
        //   両側とも閉じた部屋なので、戸口は C1 の自動生成で出る（板の表: 屋内⇄屋内 は
        //   自動生成される）。手置きすると広さを変えるたびに追従させる必要が出るので、
        //   ここでは自動に任せる。**屋外に面した開口が無いシーンだから成立する**。
        [MenuItem("AcousticFlow/Test Scenes/Room Compare (広さと隣室の比較)")]
        public static void RoomCompareScene()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -2.1f));

            var labGo = new GameObject("RoomCompareLab");
            var lab = labGo.AddComponent<RoomCompareLab>();
            lab.sizeIndex = 1;          // 中から始める
            lab.inSourceRoom = false;   // 最初は隣の部屋（こもって聞こえる側）
            lab.listener = listener;
            lab.EnsurePieces();

            // 音源は奥の部屋。ApplyNow が正しい奥行きへ置き直すので、ここは仮置き。
            // ★壁は Concrete ではなく Default にしてある（Test_SwingDoor は Concrete）。
            //   Concrete は α = 0.02〜0.07 でほとんど吸わないので、大の部屋で
            //   Sabine の RT60 が 12.8 秒になり、どの広さでも残響に溺れて比べられない。
            //   Default(α = 0.10〜0.40) だと 小 0.55 / 中 1.12 / 大 2.69 秒（500Hz）と
            //   広さ 1 段ごとにほぼ 2 倍になり、階段として聞き取れる。
            //   透過は Default TL 31〜52dB / 扉 WoodDoor 15〜27dB なので、
            //   「漏れているのは扉」という落差は Concrete でなくても保たれる。
            var srcs = AddDemo(listener, new Vector3(0f, 1.6f, 7.3f), AcousticMaterialPreset.Default);
            lab.source = srcs[0];

            var demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();

            // ★扉だけ弱い材質にする。現実の部屋で音が漏れるのは壁ではなく扉。
            //   壁を一様に弱くすると、どこから漏れているか分からなくなる（Test_SwingDoor と同じ理由）。
            if (lab.door != null && lab.door.GetComponent<AcousticSurface>() == null)
                lab.door.gameObject.AddComponent<AcousticSurface>().material =
                    AcousticMaterialPreset.WoodDoor;

            lab.ApplyNow();   // 形と立ち位置を確定させてから保存する（Awake を待たない）

            // ★扉と仕切りの明度差を付ける。何度開いているかは音を判断する前提情報なので、
            //   見て分からないと確認にならない。
            if (lab.door != null) Tint(lab.door.transform, new Color(0.95f, 0.55f, 0.15f));
            foreach (string n in new[] { "Partition_L", "Partition_R", "Partition_Top" })
            {
                Transform p = labGo.transform.Find(n);
                if (p != null) Tint(p, new Color(0.30f, 0.33f, 0.40f));
            }

            Save(scene, "Test_RoomCompare.unity",
                "広さと隣室の比較: 1/2/3 で広さ（片側 30/320/4320 m3）、Enter で居る部屋を切替、" +
                "5/6 で扉の開き角。立ち位置は仕切りに対して鏡像なので戸口からの距離は両側で同じ。" +
                "部屋を跨ぐと音源までの距離が変わるため、音量ではなく HUD の『こもり倍率』で比べること。");
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
            var conv = srcs0[0].GetComponent<VoiceConvolver>();

            // 回折だけを残す。
            conv.reflectionLevel = 1f;      // 'F'(回折)タップは直接音と同じ音量で鳴らす
            conv.tailLevel = 0f;            // 後期残響なし（旧 IrConvolver の enableReverbTail は tailLevel=0 と同義）
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
        // ── 多音源（負荷検証）──────────────────────────────────────────────
        // ★何のためにあるか
        //   「音源数に比例する費用」を減らす手を今日いくつも入れたのに、**確かめる場が
        //   手元に無かった**。他のテストシーンは音源 1〜2 本で、どれも効果が出ない本数。
        //   ゲームレーンのシーンを借りないと検証できない状態は依存として良くない。
        //
        // ★ここで確かめる 3 つ（全部「音源が多いときだけ効く」もの）
        //   ・sharedTailBus         尾の畳み込みを部屋ごとに 1 回へ（音声スレッド）
        //   ・workerThreads         音源ごとの段を複数コアへ（メインスレッド）
        //   ・段（厳密/簡易/バーチャル） 何をどこまで解くか
        //
        // ★部屋を 2 つにしてある
        //   尾の形は部屋ごとなので、バスも部屋ごとに 1 本になる。1 部屋だけだと
        //   「形が違う音源を混ぜたら壊れる」経路を一度も通らず、**壊れていても気づけない**。
        //
        // ★遮蔽された音源を混ぜてある
        //   遮蔽時だけ回折の合成が走る（実測 6 倍高い）。見通せる音源だけだと、
        //   いちばん高い経路を測らずに「軽い」と言うことになる。
        [MenuItem("AcousticFlow/Test Scenes/Many Sources (多音源・負荷)")]
        public static void ManySources()
        {
            var scene = NewScene();
            var listener = MakeListener(new Vector3(0f, 1.6f, -8f));

            // 主室（響かせる。尾が費用の主なので、尾が出ない部屋で測っても意味がない）。
            MakeBox("Floor", new Vector3(0f, -0.5f, 0f), new Vector3(30f, 1f, 40f));
            MakeBox("Ceil", new Vector3(0f, 6.5f, 0f), new Vector3(30f, 1f, 40f));
            MakeBox("W_Left", new Vector3(-12f, 3f, 0f), new Vector3(1f, 7f, 40f));
            MakeBox("W_Right", new Vector3(12f, 3f, 0f), new Vector3(1f, 7f, 40f));
            MakeBox("W_Back", new Vector3(0f, 3f, -14f), new Vector3(30f, 7f, 1f));

            // 仕切り＋戸口（幅 2m）。この向こうが第 2 室 ＝ 尾の形が別になる。
            MakeBox("Div_L", new Vector3(-7f, 3f, 6f), new Vector3(11f, 7f, 0.5f));
            MakeBox("Div_R", new Vector3(7f, 3f, 6f), new Vector3(11f, 7f, 0.5f));
            MakeBox("W_Far", new Vector3(0f, 3f, 14f), new Vector3(30f, 7f, 1f));

            // 主室の中の衝立。ここの裏の音源が「遮蔽された音源」になる。
            var scr = MakeBox("Screen", new Vector3(0f, 3f, -2f), new Vector3(8f, 7f, 0.4f));
            Tint(scr, new Color(0.85f, 0.55f, 0.35f));

            // 音源 16 本。ゲームレーンの実測（声 15 本で DSP CPU 109.5%）に合わせた本数。
            var pos = new List<Vector3>();
            var names = new List<string>();
            for (int i = 0; i < 6; i++) {            // 見通せる（安い）
                pos.Add(new Vector3(-9f + i * 3.6f, 1.6f, -11f));
                names.Add("Open" + i);
            }
            for (int i = 0; i < 6; i++) {            // 衝立の裏（高い＝回折の合成が走る）
                pos.Add(new Vector3(-6f + i * 2.4f, 1.6f, 1f));
                names.Add("Occl" + i);
            }
            for (int i = 0; i < 4; i++) {            // 第 2 室（尾の形が別＝バスが 2 本になる）
                pos.Add(new Vector3(-6f + i * 4f, 1.6f, 10f));
                names.Add("Room2_" + i);
            }

            var srcs = AddDemoAt(listener, pos.ToArray(), AcousticMaterialPreset.Concrete,
                                 names.ToArray());
            Save(scene, "Test_ManySources.unity",
                "多音源（負荷検証）: 見通せる 6 / 衝立の裏 6 / 第2室 4 = 16 本。"
                + "AcousticFlowSceneDemo の sharedTailBus・workerThreads・"
                + "occlusionUpdateEveryFrames を振って、耳と数字の両方で確かめる。"
                + "★音声スレッドの負荷は Unity の Profiler → Audio（DSP CPU）で見ること。"
                + "画面の音響 ms はメインスレッドしか見ていないので 1% も動かない。");
        }

        // 位置を音源ごとに指定できる AddDemo。負荷検証は散らばっていないと意味がない
        // （同じ点に重ねると遮蔽も距離も全部同じになり、1 本を 16 回測るのと変わらない）。
        private static Transform[] AddDemoAt(Transform listener, Vector3[] positions,
                                             AcousticMaterialPreset material, string[] names)
        {
            // ★負荷検証は C++ 経路だけ載せる。
            //   IrConvolver は**無効でも Awake が走る**ので、16 本ぶんの HRTF 読み込みと
            //   18 分割の畳み込み器を確保してしまう。鳴らさない物の起動費用とメモリが
            //   測定に乗ると、何を測っているのか分からなくなる。
            //   （A/B の Y キーは効かなくなる。負荷検証にそれは要らない）
            var srcs = new Transform[positions.Length];
            for (int i = 0; i < positions.Length; i++)
                srcs[i] = AddVoiceOnly(positions[i], i, "Src_" + names[i]);

            var go = new GameObject("AcousticFlowSceneDemo");
            var demo = go.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = srcs[0];
            var extra = new Transform[positions.Length - 1];
            for (int i = 1; i < positions.Length; i++) extra[i - 1] = srcs[i];
            demo.extraSources = extra;
            demo.autoCollectBoxColliders = true;
            demo.occluderMaterial = material;
            demo.firstPersonCamera = true;
            demo.distanceRef = 4f;
            return srcs;
        }

        // C++ 経路だけの音源。負荷検証用（AddConvolver は C# 経路も載せる）。
        private static Transform AddVoiceOnly(Vector3 pos, int sourceIndex, string name)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.4f;
            var src = go.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(kTestClipPath);
            if (clip == null) clip = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (clip != null) src.clip = clip;

            var voice = go.AddComponent<VoiceConvolver>();
            voice.sourceIndex = sourceIndex;
            voice.tailLevel = 0.3f;
            // 16 本が同じ楽曲を鳴らすので、既定 0.6 のままだと確実に割れる。
            voice.outputGain = 0.12f;
            voice.enabled = false;      // Demo 側の useCppDsp が上げる
            return go.transform;
        }

        // clipPath を渡すと、その WAV を鳴らす音源になる（省略時は kTestClipPath）。
        //   ★シーンごとに素材を変えたい場面がある。収録用シーンは市販曲を避けたいし、
        //     定位を見るなら過渡音のほうが良い。既定は変えずに、渡せるようにだけしてある。
        private static Transform[] AddDemo(Transform listener, Vector3 srcPos,
                                           AcousticMaterialPreset material,
                                           string[] names = null,
                                           string clipPath = null)
        {
            string[] events = names ?? new[] { "Main" };
            var srcs = new Transform[events.Length];
            for (int i = 0; i < events.Length; i++)
                srcs[i] = AddConvolver(srcPos, i, clipPath,
                                       name: "AF_Source_" + events[i]).transform;

            var go = new GameObject("AcousticFlowSceneDemo");
            var demo = go.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener;
            demo.source = srcs[0];
            var extra = new Transform[events.Length - 1];
            for (int i = 1; i < events.Length; i++) extra[i - 1] = srcs[i];
            demo.extraSources = extra;
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
        //   ★2026-09-08: 既定を英語名の videoplayback.wav にした（発注者の指示）。
        //     ・それまでの既定（ロクデナシ「ブリザード」）は **Assets/Audio に既に無く、参照が切れていた**。
        //       読めないと下の Tok… へ落ちるので、既定が何なのか実際には決まっていなかった。
        //     ・日本語（全角）のファイル名はパスの取り回しで事故りやすい。英語名に寄せる。
        //   ※音源は Assets/Audio/*.wav ごと .gitignore で追跡外。配布物に入れる前に
        //     権利を確認すること（差し替え可能な効果音にするなら .gitignore に例外を書く）。
        private const string kTestClipPath = "Assets/Audio/videoplayback.wav";
        private const string kTransientClipPath = "Assets/Audio/Footstep_Asphalt.mp3";

        // 画面収録の音声。**収録用シーンはこれを鳴らす。**
        //   kTestClipPath は市販楽曲なので、画面収録した動画をそのまま人に見せられない
        //   （§7.6 の商用曲問題）。こちらは自前の素材なのでその制約が無い。
        private const string kScreenRecClipPath = "Assets/Audio/画面録画-2026-08-24-192052.wav";

        // 音源 sourceIndex ぶんの畳み込み器。遅延も到来方向も遮蔽も音源ごとに違うので、
        // 鳴らしたい音源 1 つにつき 1 つ置く。clip を渡さなければ既定の足音を使う。
        //
        // ★音の計算は**音響エンジン（C++）だけ**が行う（2026-09-09 決定）。
        //   C# が持つのは「音の面の操作」── 何を鳴らすか（AudioSource のクリップ）、どう切り替えるか、
        //   計器の読み出し。信号を作る側には触らない。
        //   それまでは C# 経路(IrConvolver) と C++ 経路(VoiceConvolver) を両方載せて Y キーで切り替えていたが、
        //   (a) 名前に反して IrConvolver は中に C# の FDN も持っていて「どちらの尾の話か」が判らない
        //   (b) 無効な側でも IrConvolver.Awake が走って AudioSource（クリップ・loop・spatialBlend）を握る
        //   (c) 空のクリップに ir_silence（無音）を差し込むので「鳴っているのに無音」が黙って起きる
        //   ── これらが試聴の妨げになった。C++ 経路の採用が固まったので、こちらのレーンからは外す。
        //   ⚠ IrConvolver.cs 自体は消していない（BellGame レーンが構造的に使っているため）。
        private static VoiceConvolver AddConvolver(Vector3 pos, int sourceIndex = 0,
                                                   string clipPath = null, string name = null)
        {
            // ★球にして**見える**ようにする。音源オブジェクトを兼ねているので、
            //   どこで鳴っているかが目で分かることが要る（見えている物と音の一致が狙い）。
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name ?? ("AF_Source_" + sourceIndex);
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.4f;
            var src = go.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(clipPath ?? kTestClipPath);
            if (clip == null) clip = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (clip != null) src.clip = clip;

            if (clip == null)
                Debug.LogWarning($"[AcousticFlow] 音源のクリップが見つかりません（{clipPath ?? kTestClipPath}）。"
                                 + "無音のまま生成します ── AudioSource にクリップを差してください。");

            var voice = go.AddComponent<VoiceConvolver>();
            voice.sourceIndex = sourceIndex;
            voice.tailLevel = 0.3f;                      // 全シーン統一
            return voice;
        }

        private static void Tint(Transform t, Color c)
        {
            var r = t.GetComponent<Renderer>();
            if (r == null) return;
            var shader = Shader.Find("Standard");
            if (shader != null) r.sharedMaterial = new Material(shader) { color = c };
        }

        // ════════════════ 新コア（Flow）の試聴シーン ════════════════
        //   段 4「一周を鳴らす」: 閉じた部屋 7×7×3 m を歩く。扉なし。
        //   部屋の箱は BoxCollider（Cube の既定）。AcousticWorld が拾う。Main Camera（AudioListener 付き）に FlowWalker。
        [MenuItem("AcousticFlow/Flow (新コア)/Room Walk (部屋を歩く)")]
        public static void FlowRoomWalkScene()
        {
            var scene = NewScene();
            const float w = 7f, h = 3f, d = 7f, t = 0.2f;
            MakeBox("Floor",   new Vector3(0, -t * 0.5f, 0),       new Vector3(w + 2 * t, t, d + 2 * t));
            MakeBox("Ceiling", new Vector3(0, h + t * 0.5f, 0),    new Vector3(w + 2 * t, t, d + 2 * t));
            MakeBox("Wall_W",  new Vector3(-w * 0.5f - t * 0.5f, h * 0.5f, 0), new Vector3(t, h, d + 2 * t));
            MakeBox("Wall_E",  new Vector3( w * 0.5f + t * 0.5f, h * 0.5f, 0), new Vector3(t, h, d + 2 * t));
            MakeBox("Wall_S",  new Vector3(0, h * 0.5f, -d * 0.5f - t * 0.5f), new Vector3(w + 2 * t, h, t));
            MakeBox("Wall_N",  new Vector3(0, h * 0.5f,  d * 0.5f + t * 0.5f), new Vector3(w + 2 * t, h, t));
            // 天井は中から見えるように裏返さない（見た目だけ。音は箱の外側の面で当たる）
            var cam = Camera.main;
            if (cam != null)
            {
                cam.transform.position = new Vector3(-1f, 1.6f, 1.5f);
                cam.transform.rotation = Quaternion.LookRotation(new Vector3(1f, 0f, -1f));
                if (cam.GetComponent<AudioListener>() == null) cam.gameObject.AddComponent<AudioListener>();
                cam.gameObject.AddComponent<FlowWalker>();
            }
            var src = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            src.name = "AF_Source_Main";
            src.transform.position = new Vector3(1.5f, 1.6f, -1f);
            src.transform.localScale = Vector3.one * 0.4f;
            Object.DestroyImmediate(src.GetComponent<Collider>());        // 音源の球は壁にしない
            var a = src.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(kTestClipPath);
            if (clip != null) a.clip = clip; else Debug.LogWarning("[AcousticFlow] クリップが見つかりません: " + kTestClipPath);
            src.AddComponent<WorldVoice>();
            Tint(src.transform, new Color(1f, 0.5f, 0.2f));
            var world = new GameObject("AcousticWorld").AddComponent<AcousticWorld>();
            world.defaultMaterial = AcousticMaterialPreset.Default;
            Save(scene, "Flow_RoomWalk.unity",
                 "新コア 段 4。閉じた部屋 7×7×3 m を WASD で歩く（右ドラッグで向く）。AF ツール ▸ 情報 に帳簿が出る。");
        }

        //   段 5「扉を開ける」: 14×3×14 m を仕切って幅 1 m の戸口、木の扉（SwingDoor、5/6 キー）。音源は向こうの部屋。
        //   扉は SwingDoor の下なので AcousticWorld が動く箱として扱う（部屋グラフから外れ、毎フレーム位置を送る）。
        [MenuItem("AcousticFlow/Flow (新コア)/Swing Door (扉を開ける)")]
        public static void FlowSwingDoorScene()
        {
            var scene = NewScene();
            const float half = 7f, h = 3f, t = 0.2f, gapL = -0.5f, gapR = 0.5f;
            MakeBox("Floor",   new Vector3(0, -t * 0.5f, 0),     new Vector3(2 * half + 2 * t, t, 2 * half + 2 * t));
            MakeBox("Ceiling", new Vector3(0, h + t * 0.5f, 0),  new Vector3(2 * half + 2 * t, t, 2 * half + 2 * t));
            MakeBox("Wall_W",  new Vector3(-half - t * 0.5f, h * 0.5f, 0), new Vector3(t, h, 2 * half + 2 * t));
            MakeBox("Wall_E",  new Vector3( half + t * 0.5f, h * 0.5f, 0), new Vector3(t, h, 2 * half + 2 * t));
            MakeBox("Wall_S",  new Vector3(0, h * 0.5f, -half - t * 0.5f), new Vector3(2 * half + 2 * t, h, t));
            MakeBox("Wall_N",  new Vector3(0, h * 0.5f,  half + t * 0.5f), new Vector3(2 * half + 2 * t, h, t));
            float leftW = gapL - (-half), rightW = half - gapR;
            Tint(MakeBox("Partition_L", new Vector3(-half + leftW * 0.5f, h * 0.5f, 0), new Vector3(leftW, h, t)), new Color(0.30f, 0.33f, 0.40f));
            Tint(MakeBox("Partition_R", new Vector3(gapR + rightW * 0.5f, h * 0.5f, 0), new Vector3(rightW, h, t)), new Color(0.30f, 0.33f, 0.40f));
            // 扉。左枠を蝶番に +Z 側へ振れる（Test_SwingDoor と同じ）。木の扉の材質。
            var hinge = new GameObject("Door_Hinge").transform;
            hinge.position = new Vector3(gapL, h * 0.5f, 0);
            var doorGo = GameObject.CreatePrimitive(PrimitiveType.Cube);
            doorGo.name = "Door";
            var door = doorGo.AddComponent<SwingDoor>();
            door.hinge = hinge; door.width = gapR - gapL; door.height = h; door.thickness = 0.06f;
            door.swingTowardPositiveZ = true; door.angleDeg = 0f;
            door.Apply();
            Tint(doorGo.transform, new Color(0.95f, 0.55f, 0.15f));
            doorGo.AddComponent<AcousticSurface>().material = AcousticMaterialPreset.WoodDoor;
            var cam = Camera.main;
            if (cam != null)
            {
                cam.transform.position = new Vector3(0f, 1.6f, -3f);
                cam.transform.rotation = Quaternion.LookRotation(Vector3.forward);
                if (cam.GetComponent<AudioListener>() == null) cam.gameObject.AddComponent<AudioListener>();
                cam.gameObject.AddComponent<FlowWalker>();
            }
            var src = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            src.name = "AF_Source_Main";
            src.transform.position = new Vector3(0f, 1.6f, 3f);
            src.transform.localScale = Vector3.one * 0.4f;
            Object.DestroyImmediate(src.GetComponent<Collider>());
            var a = src.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(kTestClipPath);
            if (clip != null) a.clip = clip; else Debug.LogWarning("[AcousticFlow] クリップが見つかりません: " + kTestClipPath);
            var voice = src.AddComponent<WorldVoice>();
            voice.radius = 0.2f;                       // 幅を持たせる（開口の積分が効く）
            Tint(src.transform, new Color(1f, 0.5f, 0.2f));
            var world = new GameObject("AcousticWorld").AddComponent<AcousticWorld>();
            world.defaultMaterial = AcousticMaterialPreset.Concrete;
            Save(scene, "Flow_SwingDoor.unity",
                 "新コア 段 5。戸口の手前に立ち、5/6 キーで扉を開閉。向こうの部屋の音源が扉の開きで連続に変わる。AF ツール ▸ 情報 に見通しの割合と戸口の素通し。");
        }

        // 開いている場面の壁（BoxCollider）全部に AcousticSurface を付ける（2026-09-12）。
        //   場面を作る MakeBox は Cube を置くだけで、AcousticSurface を持つのは扉だけだった。面ごとに材質を変える入口を作る。
        //   材質は既定（AcousticWorld.defaultMaterial があればそれ）で、付けただけでは音は変わらない。
        //   音源の見た目の箱と、耳（AudioListener）の下の箱は除く。既に付いている面は触らない。再生中でも効く（AcousticWorld が拾う）。
        [MenuItem("AcousticFlow/Flow (新コア)/壁に AcousticSurface を付ける（開いている場面）")]
        public static void AttachSurfacesToWalls()
        {
            var world = Object.FindFirstObjectByType<AcousticWorld>();
            var preset = (world != null) ? world.defaultMaterial : AcousticMaterialPreset.Default;
            var listener = Object.FindFirstObjectByType<AudioListener>();
            int added = 0, skipped = 0;
            foreach (var col in Object.FindObjectsByType<BoxCollider>(FindObjectsSortMode.None))
            {
                if (col == null) continue;
                if (listener != null && col.transform.IsChildOf(listener.transform)) continue;
                if (col.GetComponentInParent<WorldVoice>() != null) continue;
                if (col.GetComponent<AcousticSurface>() != null) { skipped++; continue; }
                var surf = Undo.AddComponent<AcousticSurface>(col.gameObject);
                surf.mode = AcousticSurfaceMode.Preset;
                surf.material = preset;
                EditorUtility.SetDirty(surf);
                added++;
            }
            if (!Application.isPlaying) EditorSceneManager.MarkSceneDirty(UnityEngine.SceneManagement.SceneManager.GetActiveScene());
            Debug.Log("[AcousticFlow] AcousticSurface を " + added + " 面に付けました（既に付いていた " + skipped + " 面は触らず。材質は " + preset + "）");
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
