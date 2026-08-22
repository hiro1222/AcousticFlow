// BellGamePortalSetup.cs
// 扉ごしに次の世界を見せるための下ごしらえ。
//
// 3 つとも**一度やれば済む**もの。ステージを作り直しても消えないよう、
// 生成器とは別のメニューに置いてある。
//
//   1. プレビュー専用レイヤーを作る（プロジェクト設定。シーンには残らない）
//   2. ステージを Build Settings に登録する（追加読み込みに必要）
//   3. いま開いているシーンの扉に WorldPortalView / WorldPreview を付ける
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace BellGame.EditorTools
{
    public static class BellGamePortalSetup
    {
        public const string PreviewLayerName = "WorldPreview";

        private static readonly string[] StageScenes =
        {
            "Assets/Scenes/Stage1_Grassland.unity",
            "Assets/Scenes/Stage2_Ruins.unity",
            "Assets/Scenes/Stage3_Cave.unity",
            "Assets/Scenes/Stage4_Snow.unity",
        };

        [MenuItem("BellGame/セットアップ/1. ポータル用レイヤーを作る")]
        public static void EnsureLayer()
        {
            int existing = LayerMask.NameToLayer(PreviewLayerName);
            if (existing >= 0)
            {
                Debug.Log($"[BellGame] レイヤー '{PreviewLayerName}' は既に {existing} 番にある。");
                return;
            }

            var asset = AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/TagManager.asset");
            if (asset == null || asset.Length == 0)
            {
                Debug.LogError("[BellGame] TagManager.asset を開けない。手でレイヤーを足してほしい。");
                return;
            }
            var so = new SerializedObject(asset[0]);
            var layers = so.FindProperty("layers");
            // 0..7 は Unity 予約。8 番以降の空きへ入れる。
            for (int i = 8; i < layers.arraySize; i++)
            {
                var e = layers.GetArrayElementAtIndex(i);
                if (!string.IsNullOrEmpty(e.stringValue)) continue;
                e.stringValue = PreviewLayerName;
                so.ApplyModifiedProperties();
                AssetDatabase.SaveAssets();
                Debug.Log($"[BellGame] レイヤー '{PreviewLayerName}' を {i} 番に作った。");
                return;
            }
            Debug.LogError("[BellGame] 空きレイヤーが無い。手で 1 つ空けてほしい。");
        }

        [MenuItem("BellGame/セットアップ/2. ステージを Build Settings に登録")]
        public static void RegisterScenes()
        {
            var list = new System.Collections.Generic.List<EditorBuildSettingsScene>(
                EditorBuildSettings.scenes);
            int added = 0;
            foreach (var path in StageScenes)
            {
                if (!File.Exists(path)) continue;
                if (list.Exists(s => s.path == path)) continue;
                list.Add(new EditorBuildSettingsScene(path, true));
                added++;
            }
            EditorBuildSettings.scenes = list.ToArray();
            Debug.Log($"[BellGame] Build Settings に {added} 本追加した（登録済みは触っていない）。");
        }

        // ★ステージを作り直さずに、いま開いているシーンへ後付けする用。
        //   BellGameStages 側でも同じ配線をしているので、作り直した場合はこれを実行しなくてよい。
        [MenuItem("BellGame/セットアップ/3. 開いているシーンにポータルを付ける")]
        public static void AttachToOpenScene()
        {
            var door = Object.FindFirstObjectByType<WorldDoor>();
            if (door == null)
            {
                Debug.LogError("[BellGame] このシーンに WorldDoor が無い。");
                return;
            }
            Attach(door);
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            Debug.Log("[BellGame] ポータルを付けた。シーンを保存してほしい。");
        }

        /// 扉 1 枚にポータル一式を付ける。生成器からも呼ぶ。
        // ★ステンシル版へ移行しました（旧 WorldPortalView は削除）。
        //   旧版は「板・レイヤー・材質・RT・カリングマスクが全部正しいのに描かれない」
        //   ところで止まっていて、そのまま既定で無効にして置いてありました。
        //   いまはステンシル版が動いているので、答えを 2 つ残す理由がありません（決めごと #1）。
        public static WorldPortalStencil Attach(WorldDoor door)
        {
            if (door == null) return null;

            var existing = Object.FindFirstObjectByType<WorldPortalStencil>();
            if (existing != null) Object.DestroyImmediate(existing.gameObject);
            var oldSet = Object.FindFirstObjectByType<WorldSet>();
            if (oldSet != null) Object.DestroyImmediate(oldSet.gameObject);

            // ★WorldSet は世界の**外**に置く。自分が世界の一部だと、
            //   眠らせるときに自分ごと止まってしまう。
            var setGo = new GameObject("WorldSet");
            var preview = setGo.AddComponent<WorldSet>();

            var go = new GameObject("WorldPortalStencil");
            // 扉の開口の中心。MakeDoorFrame は WorldDoor 自体をそこへ置いている。
            go.transform.SetPositionAndRotation(door.transform.position, door.transform.rotation);

            // ★ステンシル版は既定で有効。開口の寸法は扉の AcousticPortal から貰います
            //   （§4.3 が見ている開口と、切り抜く矩形を同じ物にしておくため）。
            var view = go.AddComponent<WorldPortalStencil>();
            view.worldDoor = door;
            view.worlds = preview;

            // 扉の周りだけ手前の世界を薄くする（演出。物理ではない）。
            var hush = go.AddComponent<DoorHush>();
            hush.listener = door.listener;
            hush.door = door.transform;

            // いま何が起きているかを画面に出す（開発用）。
            if (Object.FindFirstObjectByType<BellGameHud>() == null)
                preview.gameObject.AddComponent<BellGameHud>();

            // ★くぐって移動する側。開口の面は**ポータルと同じ物**を指す。
            //   別々に持たせると、見えている窓と通れる窓がずれる。
            var old = Object.FindFirstObjectByType<WorldTransition>();
            if (old != null) Object.DestroyImmediate(old);
            var move = go.AddComponent<WorldTransition>();
            move.worldDoor = door;
            move.aperture = go.transform;
            move.listener = door.listener;
            move.worlds = preview;
            return view;
        }
    }
}
