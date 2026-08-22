// BellGameMeshFix.cs
// MeshCollider が使っているメッシュの Read/Write を有効にする。
//
// ★何が起きていたか
//   ログにこう出ていた:
//       Not allowed to access vertices on mesh 'GrassField' (isReadable is false)
//       AcousticScene.AddMesh ← RegisterInstances ← OnEnable
//
//   エンジンは MeshCollider の形をそのまま遮蔽物として取り込みます（AddMesh）。
//   ところが読み込み設定で Read/Write が切れていると、頂点も三角形も**読めません**。
//   例外は出ますが処理は続くので、**その物は音響的に存在しないまま**になります。
//
//   つまり GrassField・RiverBank・DoorCase・DoorFrame は
//   **見えていて、体もぶつかるのに、音だけ素通り**していました。
//   「扉の枠ごしに音が回り込まない」「川岸で音が変わらない」の原因になります。
//
// ★勝手には直しません。読み込み設定はモデルの持ち物（造形レーンの資産）で、
//   Read/Write を入れるとメモリが増えます。**メニューから明示的に**実行する形にしました。
//
// ★⚠ 全部を読めるようにしてはいけません（実測で確かめました）。
//
//   2026-08-22、Assets/Models の .obj **55 件を一括で読めるように**したところ、
//   草原の川岸で `UpdateScene` が **3.36ms → 216.19ms（64 倍）** になりました。
//   原因は `Stage1_Field.obj`（GrassField + RiverBank、2495 三角形）── 
//   **地面がまるごと遮蔽物として登録された**ためです。
//
//   増えるのはメモリだと思っていましたが、本当の代償は**音響の計算量**でした。
//   エンジンは遮蔽物の三角形を相手に遮蔽・回折を解くので、地形を渡すと破綻します。
//
//   ★開けてよいのは「音を遮る物」であって「立っている地面」ではありません。
//     扉まわり（枠・戸当たり・板）は 42〜296 三角形で、開口の縁を作る**主役**です。
//     地形・建物の外殻は BoxCollider の近似で十分で、そちらは既に効いています。
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace BellGameEditor
{
    public static class BellGameMeshFix
    {
        [MenuItem("BellGame/道具/MeshCollider のメッシュを読めるようにする")]
        public static void MakeColliderMeshesReadable()
        {
            var paths = new HashSet<string>();
            var stuck = new List<string>();

            foreach (var mc in Object.FindObjectsByType<MeshCollider>(
                         FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                var m = mc.sharedMesh;
                if (m == null || m.isReadable) continue;

                var path = AssetDatabase.GetAssetPath(m);
                if (string.IsNullOrEmpty(path)) { stuck.Add(m.name); continue; }
                paths.Add(path);
            }

            int done = 0;
            foreach (var p in paths)
            {
                var imp = AssetImporter.GetAtPath(p) as ModelImporter;
                if (imp == null) { stuck.Add(p); continue; }
                if (imp.isReadable) continue;
                imp.isReadable = true;
                imp.SaveAndReimport();
                done++;
            }

            if (done > 0)
                Debug.Log($"[BellGame] メッシュ {done} 件を読めるようにしました。\n"
                    + "  ★これで MeshCollider の形が音響の遮蔽物として取り込まれます。\n"
                    + "  ⚠ シーンを作り直してから測り直してください（登録は OnEnable で走ります）。");
            else if (stuck.Count == 0)
                Debug.Log("[BellGame] 読めないメッシュはありませんでした。");

            if (stuck.Count > 0)
                Debug.LogWarning("[BellGame] 手が届かなかったもの（シーン内で作られたメッシュなど）: "
                    + string.Join(", ", stuck) + "\n"
                    + "  ★作った側で `mesh.UploadMeshData(false)` にするか、"
                    + "MeshCollider ではなく BoxCollider にしてください。");
        }
    }
}
