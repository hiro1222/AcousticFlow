// BellGameWorldAll.cs (Editor 専用)
// 4 つのステージを 1 つのシーンへ束ねる。メニュー: [BellGame > Stages > ★全部入りシーンを作る]
//
// ★なぜ全部入りにするのか
//   扉をくぐる瞬間に暗転も引っかかりも置きたくない（§0 の芯）。
//   行き先が最初から置いてあれば、くぐる動作は**写像だけ**で済む。
//
//   追加読み込みでも同じことはできるが、あちらは
//     ・読み込んだ瞬間に相手の OnEnable が走ってエンジンのシーンが立つ
//     ・止める → 剥がす → 起こし直す、という層が要る
//     ・親を先に動かすと中身が付いてこない
//   と、事故の種が多い。**最初から置いてあれば、眠らせる／起こすだけ**になる。
//
// ★ステージの作り方は変えない。
//   各ステージは今までどおり単独のシーンとして作り、ここが**束ねるだけ**にしてある。
//   1 つの世界を直したいときは、そのステージだけ作り直して、ここをもう一度回す。
//   束ねる側にステージの中身を書くと、答えが 2 箇所になる（決めごと #1）。
//
// ★保存するのは「生成そのままの状態」。ここで眠らせない。
//   眠らせるのは実行時に WorldSet がやる（誰よりも先に動くようにしてある）。
//   生成時に無効化すると、WorldSet が「元は無効だった」と覚えてしまい、
//   起こしても何も戻らなくなる ── VoiceConvolver(C++) と IrConvolver(C#) が
//   排他だという前提も、そこで壊れる。
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace BellGame.EditorTools
{
    public static class BellGameWorldAll
    {
        public const string OutputPath = "Assets/Scenes/World_All.unity";

        // 世界どうしを離す間隔。WorldSet.slotSpacing と揃えること。
        private const float Spacing = 4000f;

        private static readonly (WorldId id, string path)[] Stages =
        {
            (WorldId.Grassland, "Assets/Scenes/Stage1_Grassland.unity"),
            (WorldId.Temple,    "Assets/Scenes/Stage2_Ruins.unity"),
            (WorldId.Cave,      "Assets/Scenes/Stage3_Cave.unity"),
            (WorldId.Snow,      "Assets/Scenes/Stage4_Snow.unity"),
        };

        // ★これ 1 つで済むようにしてある。
        //   4 ステージを作り直す → 束ねる、を毎回手で 5 回叩いていたが、
        //   順番を間違える余地しか無い（束ねるのは必ず後）。手順は 1 つにする。
        [MenuItem("BellGame/Stages/★これ1つ：全部作り直して束ねる")]
        public static void RebuildAll()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            BellGameStages.Stage1Grassland();
            BellGameStages.Stage2Ruins();
            BellGameStages.Stage3Cave();
            BellGameStages.Stage4Snow();
            Build();

            Debug.Log("[BellGame] 全部作り直して束ねた。"
                      + $"{OutputPath} を開いて再生してほしい。");
        }

        [MenuItem("BellGame/Stages/束ねるだけ (World_All)")]
        public static void Build()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;

            int previewLayer = LayerMask.NameToLayer(BellGamePortalSetup.PreviewLayerName);
            if (previewLayer <= 0)
            {
                Debug.LogError("[BellGame] レイヤー '" + BellGamePortalSetup.PreviewLayerName
                               + "' が無い。先に BellGame/セットアップ/1 を実行してほしい。");
                return;
            }

            var dst = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var roots = new List<(WorldId id, GameObject root)>();
            int slot = 0;

            foreach (var (id, path) in Stages)
            {
                if (!File.Exists(path))
                {
                    Debug.LogWarning($"[BellGame] {path} が無いので飛ばす。"
                                     + "先に BellGame/Stages/… でそのステージを作ってほしい。");
                    continue;
                }

                var src = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);

                // その世界の根っこを 1 つ用意して、中身を全部ぶら下げる。
                var root = new GameObject("World_" + id);
                SceneManager.MoveGameObjectToScene(root, src);
                foreach (var go in src.GetRootGameObjects())
                {
                    if (go == root) continue;
                    if (go.transform.parent != null) continue;
                    go.transform.SetParent(root.transform, true);
                }

                // ★中身をぶら下げてから動かす。逆にすると中身が付いてこない。
                root.transform.position = new Vector3(Spacing * slot++, 0f, 0f);

                EditorSceneManager.MergeScenes(src, dst);
                roots.Add((id, root));
            }

            if (roots.Count == 0)
            {
                Debug.LogError("[BellGame] 束ねられるステージが 1 つも無かった。");
                return;
            }

            // ★束ねる側は WorldSet を 1 つだけ持つ。世界の外に置くこと
            //   （世界の一部だと、眠らせるときに自分ごと止まる）。
            var setGo = new GameObject("WorldSet");
            var set = setGo.AddComponent<WorldSet>();
            set.slotSpacing = Spacing;
            set.startWorld = roots[0].id;

            // ★眠らせるのは実行時に WorldSet がやる。ここでは触らない。
            //   生成時に無効化してしまうと、WorldSet が「元は無効だった」と覚えてしまい、
            //   起こしても何も戻らなくなる。保存するのは**生成そのままの状態**。

            // 立つ世界の空気を全体設定へ入れておく（RenderSettings はシーンに 1 つ）。
            WorldSky.Apply(set.startWorld);

            Directory.CreateDirectory("Assets/Scenes");
            EditorSceneManager.SaveScene(dst, OutputPath);

            var names = new List<string>();
            foreach (var (id, _) in roots) names.Add(BellVoices.DisplayName(id));
            Debug.Log($"[BellGame] {OutputPath} を作った ── {string.Join(" / ", names)}"
                      + $"（{Spacing}m 間隔・最初に立つのは {BellVoices.DisplayName(set.startWorld)}）\n"
                      + "  ステージを直したら、そのステージを作り直してからここをもう一度回すこと。");
        }
    }
}
