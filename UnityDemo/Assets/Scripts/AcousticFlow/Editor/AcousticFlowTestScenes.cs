// AcousticFlowTestScenes.cs (Editor 専用)
// メニュー [AcousticFlow > Test Scenes > ...] で、現象ごとの検証用シーンを自動生成する。
//   回折 / 透過 / 小部屋 / 広い部屋。各シーンに AcousticFlowSceneDemo（6音源・タップ算出）＋
//   IrConvolver（クリックで反射パターンを聞く）を入れる。Play → M で楽曲ミュート →
//   ヘッドホン＋Status Monitor（IRタップのプロット）で確認する。
//   ※音源は本編と同じ6ステム。座標は全部同じ場所に重ねる（検証用なので分散不要）。
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
        // 影境界を跨ぐ瞬間に段差が出ないか（回折ゲインが 1.0 と UTD 値で飛ばないか）を確認する。
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

        private static void AddConvolver(Vector3 pos)
        {
            var go = new GameObject("IrConvolverTest");
            go.transform.position = pos;   // スピーカ(音源)と同じ位置に置く
            var src = go.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(kTestClipPath);
            if (clip == null) clip = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
            if (clip != null) src.clip = clip;

            var conv = go.AddComponent<IrConvolver>();
            conv.tailLevel = 0.3f;                       // 全シーン統一
            conv.generateTestSignal = (clip == null);    // clipがあれば実音源を畳み込み / 無ければテスト信号
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
