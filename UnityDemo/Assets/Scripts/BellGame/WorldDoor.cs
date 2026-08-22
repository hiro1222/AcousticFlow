// WorldDoor.cs
// 世界へ繋がる扉。最初から在り、開け閉めできる。
//
// 進行:
//   1. 扉を**閉める**
//   2. 集めたベルの中から行き先を選び、扉の前で鳴らす
//   3. 扉の行き先がその世界になる
//   ベルを集めるほど行き先が増える＝ほかの世界へ繋がる。
//
// ★音響的には「扉を特別視しない」（仕様書 §5 決めごと #4）。
//   この板は BoxCollider を持つただの障害物で、AcousticFlowSceneDemo が
//   毎フレーム transform をエンジンへ流すだけで機能する。
//   角度から音のパラメータを作る処理はここには**書かない** ── 幾何を渡せば音は出る。
//   開き具合が音色に出るのは §4.3 のフレネル帯域積分がやっていることで、
//   ゲーム側が真似すると二重計上になる。
//
// ⚠ いまの配置では扉の両側がどちらも屋外なので、音響的な効きは小さい。
//   `2e63c46` のとおり屋外は部屋にならず、開口が立たないため。
//   扉の向こうに閉じた空間（次の世界）を置いて初めて、§4.3 が本領を出す。
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class WorldDoor : MonoBehaviour
    {
        [Header("配線")]
        [Tooltip("物理の扉。開け閉めは PhysicsDoor が持つ（掴んで押し引きする）。")]
        public PhysicsDoor door;
        public Transform listener;
        public PlayerBells bells;

        [Header("操作")]
        [Tooltip("この距離まで寄ると行き先を選べる。")]
        [Range(1f, 8f)] public float interactDistance = 3.5f;
        [Tooltip("これ以下の角度なら『閉まっている』とみなす(度)。")]
        [Range(0.5f, 10f)] public float closedAngleDeg = 2.5f;

        [Header("行き先")]
        [Tooltip("いま扉が繋がっている世界。None なら何処にも繋がっていない。")]
        public WorldId destination = WorldId.None;

        [Header("診断（読み取り専用）")]
        public bool playerIsNear;
        public bool isClosed = true;
        public string destinationName = "—";

        /// 扉が閉じているか。行き先を変えられるのは閉じているときだけ。
        /// ★角度は**物理から読むだけ**。ここから駆動はしない（掴んで動かすのが操作）。
        public bool IsClosed => door == null || door.AngleDeg <= closedAngleDeg;

        private void Reset()
        {
            door = GetComponentInChildren<PhysicsDoor>();
        }

        private void Update()
        {
            playerIsNear = listener != null &&
                           Vector3.Distance(listener.position, transform.position) <= interactDistance;
            isClosed = IsClosed;
            destinationName = BellVoices.DisplayName(destination);
        }

        /// ベルが鳴らされたときに BellCallResponse から呼ばれる。
        /// 扉が閉じていて、近くにいて、そのベルを持っているときだけ行き先が変わる。
        public bool TrySetDestination(WorldId id)
        {
            if (id == WorldId.None) return false;
            if (!playerIsNear) return false;
            if (!IsClosed) return false;
            if (bells != null && bells.held != null && !bells.held.Contains(id)) return false;
            if (destination == id) return false;

            destination = id;
            Debug.Log($"[BellGame] 扉の行き先を {BellVoices.DisplayName(id)} にした");
            return true;
        }
    }
}
