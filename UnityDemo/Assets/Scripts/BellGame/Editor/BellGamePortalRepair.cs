// BellGamePortalRepair.cs
// 消した WorldPortalView の後始末を、7 シーンまとめて行う。
//
// ★何が起きていたか
//   ステンシル版へ一本化したとき（決めごと #1：答えは 1 つ）に
//   `WorldPortalView.cs` を消しました。ところがシーンの側には
//   そのスクリプトを指す参照が残ります ── Unity では
//   「Missing (Mono Script)」として居座り、**扉ごしに向こうが映りません**。
//
//   guid `1743f6e328fb41d4f93ab7117e987a05` を指したまま残っていたのは 7 つ:
//       Lab_Portal / Lab_Portal_Solid / Stage1_Grassland / Stage2_Ruins
//       Stage3_Cave / Stage4_Snow / World_All
//
// ★なぜ道具にするか
//   1 つずつ開いて「消して・付けて・保存」を 7 回やると、必ずどれかを取りこぼします。
//   取りこぼしても**エラーは出ません**（映らないだけ）ので、気づくのは後です。
//   手数が同じでも、**取りこぼしようがない形**にする価値があります。
//
// ★勝手には走りません。メニューから明示的に実行します。
//   シーンを書き換えるので、走らせる前に保存されているか確かめます。
using System.Collections.Generic;
using BellGame;
using BellGame.EditorTools;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace BellGameEditor
{
    public static class BellGamePortalRepair
    {
        // 直す相手。**生成し直せるものも含めて、ここに全部並べる。**
        //   「どれが壊れているか」を人が覚えている状態にしない。
        private static readonly string[] Targets =
        {
            "Assets/Scenes/Lab_Portal.unity",
            "Assets/Scenes/Lab_Portal_Solid.unity",
            "Assets/Scenes/Stage1_Grassland.unity",
            "Assets/Scenes/Stage2_Ruins.unity",
            "Assets/Scenes/Stage3_Cave.unity",
            "Assets/Scenes/Stage4_Snow.unity",
            "Assets/Scenes/World_All.unity",
        };

        [MenuItem("BellGame/道具/消したポータルの後始末（7 シーン）")]
        public static void RepairAll()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            var report = new List<string>();
            int fixedScenes = 0;

            foreach (var path in Targets)
            {
                if (!System.IO.File.Exists(path))
                {
                    report.Add($"　{Name(path)}: ファイルが無い（飛ばした）");
                    continue;
                }

                var scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Single);

                int removed = 0;
                var empties = new List<GameObject>();

                foreach (var root in scene.GetRootGameObjects())
                {
                    foreach (var t in root.GetComponentsInChildren<Transform>(true))
                    {
                        var go = t.gameObject;
                        int n = GameObjectUtility.GetMonoBehavioursWithMissingScriptCount(go);
                        if (n <= 0) continue;

                        GameObjectUtility.RemoveMonoBehavioursWithMissingScript(go);
                        removed += n;

                        // ★中身が無くなった器は捨てる。
                        //   「WorldPortalView」という名前の空オブジェクトが残ると、
                        //   次に見た人が「付いているのに映らない」と読み違えます。
                        if (go.GetComponents<Component>().Length <= 1 && t.childCount == 0)
                            empties.Add(go);
                    }
                }

                foreach (var go in empties) Object.DestroyImmediate(go);

                // 消しただけでは映りません。ステンシル版を付け直します。
                string attached = EnsureStencil();

                if (removed > 0 || attached != null)
                {
                    EditorSceneManager.MarkSceneDirty(scene);
                    EditorSceneManager.SaveScene(scene);
                    fixedScenes++;
                    report.Add($"　{Name(path)}: 壊れた参照 {removed} 件を外した"
                               + (empties.Count > 0 ? $" / 空の器 {empties.Count} 個を捨てた" : "")
                               + (attached != null ? $" / {attached}" : ""));
                }
                else
                {
                    report.Add($"　{Name(path)}: 直すところは無かった");
                }
            }

            Debug.Log($"[BellGame] ポータルの後始末: {fixedScenes} / {Targets.Length} シーンを書き換えました。\n"
                      + string.Join("\n", report)
                      + "\n  ★扉ごしに向こうが映るか、1 つずつ再生して確かめてください。"
                      + "　映らない場合は BellGame/セットアップ/1 を先に実行してください"
                      + "（レイヤーが無いとステンシルは自分を止めます）。");
        }

        /// ステンシル版が居なければ付ける。居るなら触らない。
        ///   ★戻り値は「何をしたか」。何もしていないときは null。
        private static string EnsureStencil()
        {
            if (Object.FindFirstObjectByType<WorldPortalStencil>() != null) return null;

            var door = Object.FindFirstObjectByType<WorldDoor>();
            if (door == null) return "扉が無いのでポータルは付けなかった";

            var view = BellGamePortalSetup.Attach(door);
            return (view != null) ? "ステンシル版を付け直した" : "ポータルを付けられなかった";
        }

        private static string Name(string path) => System.IO.Path.GetFileNameWithoutExtension(path);
    }
}
