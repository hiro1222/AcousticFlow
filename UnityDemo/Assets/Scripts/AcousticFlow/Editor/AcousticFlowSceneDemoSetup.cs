// AcousticFlowSceneDemoSetup.cs (Editor 専用)
// メニュー [AcousticFlow > Setup Scene Demo (new core)] で、新コア(AcousticScene)用の
// デモシーンを自動生成する。
//   囲まれた部屋（床＋天井＋四方壁）＋中央の衝立（回折エッジ）＋衝立の奥に6ステム音源。
//   → 反射で回り込む・衝立で回折する・壁で分断すると塞がる、を6音源で体感できる。
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public static class AcousticFlowSceneDemoSetup
    {
        [MenuItem("AcousticFlow/Setup Scene Demo (new core)")]
        public static void SetupSceneDemo()
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            // リスナー（プレイヤー）。衝立の手前(-Z)。奥のバンドの方(+Z)を向く。
            var listener = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            listener.name = "Listener";
            listener.transform.position = new Vector3(0f, 1.5f, -5f);
            listener.transform.rotation = Quaternion.identity;
            listener.transform.localScale = Vector3.one * 0.5f;

            // 6ステム音源を「衝立の奥(+Z)」のクラスタに配置。直接線は衝立に塞がれ、反射/回折で回り込む。
            var vocal  = MakeSource("Source_Vocal",  new Vector3(0f, 1.7f, 4f));
            var guitar = MakeSource("Source_Guitar", new Vector3(-2.5f, 1.3f, 4f));
            var piano  = MakeSource("Source_Piano",  new Vector3(2.5f, 1.3f, 4f));
            var bass   = MakeSource("Source_Bass",   new Vector3(-1.5f, 0.8f, 5f));
            var drums  = MakeSource("Source_Drums",  new Vector3(0f, 0.9f, 5.5f));
            var other  = MakeSource("Source_Other",  new Vector3(1.5f, 1.0f, 5f));

            // 囲まれた部屋（床＋天井＋四方壁）。反射がこの中で回り込む。
            new List<Transform>
            {
                MakeBox("Floor",      new Vector3(0f, -0.5f, 0f), new Vector3(20f, 1f, 20f)),
                MakeBox("Ceiling",    new Vector3(0f, 5.5f, 0f),  new Vector3(20f, 1f, 20f)),
                MakeBox("Wall_East",  new Vector3(9f, 2.5f, 0f),  new Vector3(0.5f, 5f, 18f)),
                MakeBox("Wall_West",  new Vector3(-9f, 2.5f, 0f), new Vector3(0.5f, 5f, 18f)),
                MakeBox("Wall_North", new Vector3(0f, 2.5f, 9f),  new Vector3(18f, 5f, 0.5f)),
                MakeBox("Wall_South", new Vector3(0f, 2.5f, -9f), new Vector3(18f, 5f, 0.5f)),
            };

            // 中央の衝立（回折エッジを作る有限の壁）。listener と band の直接線を塞ぐ。
            var screen = MakeBox("Screen", new Vector3(0f, 1.5f, 0f), new Vector3(5f, 4f, 0.4f));
            Tint(screen, new Color(0.85f, 0.55f, 0.35f));

            // デモ制御コンポーネント。
            var controllerGo = new GameObject("AcousticFlowSceneDemo");
            var demo = controllerGo.AddComponent<AcousticFlowSceneDemo>();
            demo.listener = listener.transform;
            demo.source = vocal;
            demo.extraSources = new[] { guitar, piano, bass, drums, other };
            demo.sourceEvents = new[] { "Vocal", "Guitar", "Piano", "Bass", "Drums", "Other" };
            demo.autoCollectBoxColliders = true;              // 床/天井/壁/衝立を自動収集
            demo.occluderMaterial = AcousticMaterialPreset.Concrete;
            demo.firstPersonCamera = true;                    // WASD で歩き回る一人称

            const string dir = "Assets/Scenes";
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            EditorSceneManager.SaveScene(scene, dir + "/Demo.unity");

            Debug.Log("[AcousticFlow] 新コアのデモシーン(Demo.unity)を生成しました" +
                      "（囲まれた部屋＋衝立＋6音源）。Play で WASD 移動＋6ステム再生。" +
                      "衝立の裏でも反射/回折で回り込みます。");
        }

        private static Transform MakeSource(string name, Vector3 pos)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.name = name;
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

        private static void Tint(Transform t, Color c)
        {
            var r = t.GetComponent<Renderer>();
            if (r == null) return;
            var shader = Shader.Find("Standard");
            if (shader == null) return;
            r.sharedMaterial = new Material(shader) { color = c };
        }
    }
}
