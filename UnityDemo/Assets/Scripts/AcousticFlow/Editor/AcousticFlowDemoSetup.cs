// AcousticFlowDemoSetup.cs (Editor 専用)
// メニュー [AcousticFlow > Setup Demo Scene] で、可視化デモのシーンを自動生成する。
// 手作業でのオブジェクト配置やコンポーネント割り当てを省くためのもの。
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public static class AcousticFlowDemoSetup
    {
        [MenuItem("AcousticFlow/Setup Demo Scene")]
        public static void SetupDemoScene()
        {
            // 新規シーン（カメラ＋ライト付き）。
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            // 6つの stem 音源を「リスナー正面」にある程度固めて配置（静的＝動かさない）。
            // リスナーは原点付近で +Z を向くので、+Z 側（前方）の小さなクラスタにまとめる。
            // X(左右)・Y(高さ)・Z(前後)を少しずつ散らして個々の定位を分かるようにする。
            var vocal  = MakeSource("Source_Vocal",  new Vector3(0f, 1.6f, 5f));     // 中央前・高め（リード）
            var guitar = MakeSource("Source_Guitar", new Vector3(-2.5f, 1.2f, 5f));  // 左
            var piano  = MakeSource("Source_Piano",  new Vector3(2.5f, 1.2f, 5f));   // 右
            var bass   = MakeSource("Source_Bass",   new Vector3(-1.5f, 0.6f, 6f));  // 左奥・低め
            var drums  = MakeSource("Source_Drums",  new Vector3(0f, 0.8f, 6.5f));   // 中央奥
            var other  = MakeSource("Source_Other",  new Vector3(1.5f, 1.0f, 6f));   // 右奥

            // リスナー（プレイヤー）。初期座標 (0,1,0)・角度 0（+Z を向く＝音源クラスタの正面）。
            var listener = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            listener.name = "Listener";
            listener.transform.position = new Vector3(0f, 1f, 0f);
            listener.transform.rotation = Quaternion.identity;
            listener.transform.localScale = Vector3.one * 0.5f;

            // 箱だけの部屋を作る（床＋四方の壁＋中央の遮蔽壁）。
            // すべて回転なし＝AABB前提。レイがこの中で跳ね回る。
            var geometry = new System.Collections.Generic.List<Transform>();

            geometry.Add(MakeBox("Floor",     new Vector3(0f, -1.5f, 0f),  new Vector3(40f, 1f, 40f)));
            geometry.Add(MakeBox("Wall_North", new Vector3(0f, 2f, 14f),   new Vector3(40f, 8f, 1f)));
            geometry.Add(MakeBox("Wall_South", new Vector3(0f, 2f, -14f),  new Vector3(40f, 8f, 1f)));
            geometry.Add(MakeBox("Wall_East",  new Vector3(14f, 2f, 0f),   new Vector3(1f, 8f, 40f)));
            geometry.Add(MakeBox("Wall_West",  new Vector3(-14f, 2f, 0f),  new Vector3(1f, 8f, 40f)));

            // デモ制御コンポーネント。
            var controllerGo = new GameObject("AcousticFlowDemo");
            var demo = controllerGo.AddComponent<AcousticFlowDemo>();
            demo.listener = listener.transform;
            demo.source = vocal;
            demo.extraSources = new[] { guitar, piano, bass, drums, other };
            // 各音源のイベント名（Wwise の Events と一致）。並びは source, extraSources… の順。
            demo.sourceEvents = new[] { "Vocal", "Guitar", "Piano", "Bass", "Drums", "Other" };
            demo.walls = geometry.ToArray();
            demo.rayCount = 2048;             // 可視化レイ（gizmo＝エディタのみ。exeには出ない）
            demo.maxBounces = 3;
            demo.occlusionRayCount = 1024;    // 音用レイ（音源数ぶん毎フレーム）
            demo.occlusionBounces = 2;
            demo.mode = AcousticFlowDemo.VisualizationMode.PerceivedPaths;
            demo.sourceDirectivity = AcousticEngine.DirectivityType.Omni;  // 6 stem を均等に聴く
            demo.autoOrbitListener = false;   // 初期座標(0,1,0)に固定（自動周回しない）

            // カメラを真上気味に置いて、レーダー状のレイが見やすいようにする。
            var cam = Camera.main;
            if (cam != null)
            {
                cam.transform.position = new Vector3(0f, 25f, -2f);
                cam.transform.rotation = Quaternion.Euler(80f, 0f, 0f);
            }

            // 保存。
            const string dir = "Assets/Scenes";
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            EditorSceneManager.SaveScene(scene, dir + "/Demo.unity");

            Debug.Log("[AcousticFlow] デモシーンを生成しました。Play を押すと可視化されます。");
        }

        // 名前・位置で音源用の小さな Sphere を作るヘルパ。
        private static Transform MakeSource(string name, Vector3 pos)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
            go.transform.position = pos;
            go.transform.localScale = Vector3.one * 0.5f;
            return go.transform;
        }

        // 名前・中心・サイズ(フルサイズ)で Cube を作るヘルパ。
        private static Transform MakeBox(string name, Vector3 center, Vector3 size)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            go.transform.position = center;
            go.transform.localScale = size;
            return go.transform;
        }
    }
}
