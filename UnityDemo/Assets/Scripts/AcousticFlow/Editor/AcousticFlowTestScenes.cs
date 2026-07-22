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
        private static void MakeRoom(string prefix, Vector3 innerSize, float thick)
        {
            var go = new GameObject(prefix + "_Room");
            var room = go.AddComponent<ResizableRoom>();
            room.innerSize = innerSize;
            room.thickness = thick;
            room.ApplyNow();   // 壁を生成して保存対象にする（Awake を待たない）
        }

        // 本編と同じ6ステム音源を srcPos に全部重ねて配置し、デモ制御を付ける（座標かぶりOK）。
        private static void AddDemo(Transform listener, Vector3 srcPos, AcousticMaterialPreset material)
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
        }

        // IR畳み込みのテスト機。全シーン統一：TokyoGeto.wav を畳み込み・tailLevel=0.3。
        //   Play→M で Wwise 楽曲をミュートして、畳み込み音だけ聴く。
        private static void AddConvolver(Vector3 pos)
        {
            var go = new GameObject("IrConvolverTest");
            go.transform.position = pos;   // スピーカ(音源)と同じ位置に置く
            var src = go.AddComponent<AudioSource>();
            var clip = AssetDatabase.LoadAssetAtPath<AudioClip>("Assets/Audio/TokyoGeto.wav");
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
