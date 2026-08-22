// PlayerBells.cs
// プレイヤーが持っているベル。
//
// 2 種類ある:
//   ・**呼びかけ用のベル**（常に 1 つ・声は固定）
//       草案 §4.5 の src[0]。世界を探すために鳴らすもの。
//       声を世界ごとに変えない ── 変えると「どの音が返ってきたか」ではなく
//       「どの音で呼んだか」で場所を当てられてしまう余地が出る。
//   ・**集めたベル**（世界ごとに固有の声）
//       扉の行き先を選ぶ鍵。鳴らす対象ではなく、扉の前で選ぶためのもの。
//
// 集めるほど扉の行き先が増える＝ほかの世界へ繋がる、という進行。
using System.Collections.Generic;
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class PlayerBells : MonoBehaviour
    {
        [Tooltip("集めたベル。ここに入っている世界へ扉を繋げられる。")]
        public List<WorldId> held = new List<WorldId>();

        [Tooltip("いま選んでいるベル（held の添字）。")]
        public int selected;

        [Tooltip("持ち替えるキー。")]
        public KeyCode cycleKey = KeyCode.Tab;

        [Header("診断（読み取り専用）")]
        public string selectedName = "—";

        public WorldId Selected =>
            (held != null && held.Count > 0 && selected >= 0 && selected < held.Count)
                ? held[selected] : WorldId.None;

        // ★世界をまたいで持ち越す。
        //   扉をくぐるとシーンごと読み直すので、集めた断片はここで拾い直す。
        //   持ち越さないと、移った先で自分のベルが芯 2 本に戻ってしまい、
        //   「集めるほど音が豊かになる」が世界をまたいだ瞬間に消える。
        private void Awake() => Sync();

        /// GameProgress から持ち物を映し直す。世界を移った直後にも呼ぶ。
        public void Sync()
        {
            if (held == null) held = new List<WorldId>();
            held.Clear();
            held.AddRange(GameProgress.Held);
            selected = Mathf.Clamp(selected, 0, Mathf.Max(0, held.Count - 1));
        }

        /// 新しいベルを手に入れる。すでに持っていれば false。
        public bool Collect(WorldId id)
        {
            if (id == WorldId.None) return false;
            if (held == null) held = new List<WorldId>();
            if (held.Contains(id)) return false;
            held.Add(id);
            if (!GameProgress.Held.Contains(id)) GameProgress.Held.Add(id);
            Debug.Log($"[BellGame] {BellVoices.DisplayName(id)}のベルを手に入れた"
                      + $"（所持 {held.Count} 個）");
            return true;
        }

        private void Update()
        {
            if (held != null && held.Count > 1 && Input.GetKeyDown(cycleKey))
            {
                selected = (selected + 1) % held.Count;
                Debug.Log($"[BellGame] 持ち替え: {BellVoices.DisplayName(Selected)}");
            }
            selectedName = BellVoices.DisplayName(Selected);
        }
    }
}
