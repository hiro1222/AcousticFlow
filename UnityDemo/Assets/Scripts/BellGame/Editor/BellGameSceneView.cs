// BellGameSceneView.cs
// 再生中、**Scene ビューでだけ**眠っている世界を隠す。
//
// ★なぜ要るか
//   行き先の世界は扉のところに**重ねて**置いてある（それが「歩いて跨げる」の条件）。
//   Game ビューではレイヤーで分かれているので問題は無いが、
//   Scene ビューは全部のレイヤーを描くので、二つの世界の地面と柱が交差して見える。
//   位置を確かめたい・当たりを見たい、というときにこれが一番邪魔になる。
//
// ★SceneVisibilityManager を使う。
//   これは **Scene ビュー限定**の表示制御で、Game ビューにも
//   カメラのカリングマスクにも一切影響しない。
//   だから「見え方を確かめるために隠す」ことが、確かめたい対象を変えてしまわない。
//
//   ⚠ レイヤーやアクティブ状態で隠してはいけない。
//     それをやると音響（コライダーを舐める）と描画の両方が変わり、
//     **見ているものが調べたいものでなくなる**。
//
// ★生きている世界が入れ替わったら追随する。
//   扉を跨ぐと生き死にが入れ替わるので、一度隠して終わりにはできない。
using UnityEditor;
using UnityEngine;

namespace BellGame.EditorTools
{
    [InitializeOnLoad]
    public static class BellGameSceneView
    {
        private const string kMenu = "BellGame/Scene表示/生きている世界だけ映す";
        private const string kPref = "BellGame.SceneView.LiveOnly";

        private static WorldId _lastActive = WorldId.None;
        private static bool _wasPlaying;

        static BellGameSceneView()
        {
            EditorApplication.update += Tick;
        }

        private static bool Enabled
        {
            get => EditorPrefs.GetBool(kPref, true);
            set => EditorPrefs.SetBool(kPref, value);
        }

        [MenuItem(kMenu)]
        private static void Toggle()
        {
            Enabled = !Enabled;
            if (!Enabled) ShowAll();
            _lastActive = WorldId.None;       // 次の Tick で必ず組み直す
        }

        [MenuItem(kMenu, true)]
        private static bool ToggleValidate()
        {
            Menu.SetChecked(kMenu, Enabled);
            return true;
        }

        private static void Tick()
        {
            bool playing = Application.isPlaying;

            // 再生を抜けたら必ず全部戻す。隠したまま残すと、
            // 次に開いたとき「作った物が消えている」と誤解する。
            if (_wasPlaying && !playing) { ShowAll(); _lastActive = WorldId.None; }
            _wasPlaying = playing;

            if (!playing || !Enabled) return;

            var set = WorldSet.Current;
            if (set == null) return;
            if (set.active == _lastActive) return;      // 変わったときだけ組み直す
            _lastActive = set.active;

            Apply(set.active);
        }

        private static void Apply(WorldId live)
        {
            var vis = SceneVisibilityManager.instance;
            foreach (var go in Roots())
            {
                var name = go.name.Substring("World_".Length);
                if (!System.Enum.TryParse<WorldId>(name, out var id)) continue;

                if (id == live) vis.Show(go, true);
                else vis.Hide(go, true);
            }
        }

        private static void ShowAll()
        {
            var vis = SceneVisibilityManager.instance;
            foreach (var go in Roots()) vis.Show(go, true);
        }

        // World_<id> の根っこ。装置（扉・カメラ・リスナー）は根っこから外してあるので、
        // ここには入らない ── **扉は常に見える**。位置合わせを見るのに要るので、それでよい。
        private static System.Collections.Generic.IEnumerable<GameObject> Roots()
        {
            var scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene();
            if (!scene.IsValid()) yield break;
            foreach (var go in scene.GetRootGameObjects())
                if (go != null && go.name.StartsWith("World_")) yield return go;
        }
    }
}
