// DoorThreshold.cs
// 行き先が繋がっている扉は、**歩いて跨げない**。E でくぐる。
//
// ★なぜ塞ぐのか
//   繋がっている扉を歩いて跨ぐと、開口の外が一斉に入れ替わる瞬間を
//   プレイヤー任せにすることになる（狙って隠せない）。
//   さらに跨いだ先は「行き先の世界が在るはずの空間」で、まだ切り替わっていないので
//   **何も無い場所に立つ**ことになる。壊れた見え方になる。
//
//   E を通せば、通り道をこちらで決められる ── 開口が画面を埋めたフレームで
//   入れ替えられるので、継ぎ目が見えない。
//
// ★繋がっていない扉は素通りできる。
//   行き先が無い扉は**ただの扉**（発注者の指示）。向こうに世界が無いので、
//   跨いでも何も起きない。塞ぐ理由が無い。
//
// ★⚠ 普通のコライダーで塞ぐと**音響まで塞がる**。
//   AcousticFlowSceneDemo.CollectOccluders は BoxCollider と MeshCollider を
//   シーン全体から拾ってオクルーダーにする。開口に板を置いた瞬間、
//   **扉を開けても閉じたままの音**になり、§4.3 が死ぬ。
//
//   → カプセルで作る。エンジンは Box と Mesh しか拾わないので、素通りする。
//     プレイヤーの押し出し（WorldBounds）だけが PlayerBlocker を見て拾う。
//
//   これは「聞こえる物にだけぶつかる」という約束の**意図的な例外**。
//   繋がった扉は物理的な壁ではなく、**操作で越える敷居**だから。
using UnityEngine;

namespace BellGame
{
    /// 体だけを止める。音響には存在しない（WorldBounds が見る目印）。
    public sealed class PlayerBlocker : MonoBehaviour { }

    [DisallowMultipleComponent]
    public sealed class DoorThreshold : MonoBehaviour
    {
        [Header("配線")]
        [Tooltip("この扉の行き先を追う。")]
        public WorldDoor worldDoor;

        [Header("開口（MakeDoorFrame と揃える）")]
        public float apertureWidth = 1.1f;
        public float apertureHeight = 2.2f;

        [Header("診断（読み取り専用）")]
        [Tooltip("いま塞がっているか。")]
        public bool blocking;

        private CapsuleCollider _cap;
        private bool _forcedPassable;
        private WorldTransition _free;

        private void Start()
        {
            var go = new GameObject("Threshold");
            go.transform.SetParent(transform, false);
            go.transform.localPosition = Vector3.zero;
            go.transform.localRotation = Quaternion.identity;

            // ★カプセル。Box でも Mesh でもないので CollectOccluders は拾わない。
            _cap = go.AddComponent<CapsuleCollider>();
            _cap.direction = 1;                          // Y 軸
            _cap.radius = apertureWidth * 0.5f;
            _cap.height = apertureHeight;
            _cap.enabled = false;

            go.AddComponent<PlayerBlocker>();
        }

        private void Update()
        {
            if (_cap == null) return;

            // ★自由に跨ぐ設定のときは塞がない（発注者の決定：渡り方は自由に）。
            //   塞ぐ理由は「E でしか通れないようにする」ことだけだったので、
            //   E をやめた時点で理由が消える。判断の出どころは WorldTransition ひとつ
            //  （ここに別の真偽値を置くと、同じ問いに答えが二つになる）。
            if (_free == null) _free = FindFirstObjectByType<WorldTransition>();
            bool freeCross = _free != null && _free.autoCross;

            bool want = !freeCross
                        && !_forcedPassable
                        && worldDoor != null
                        && worldDoor.destination != WorldId.None;
            if (_cap.enabled != want) _cap.enabled = want;
            blocking = want;
        }

        /// くぐる動作のあいだだけ通す。押し戻しに邪魔をさせない。
        public void SetPassable(bool passable)
        {
            _forcedPassable = passable;
            if (_cap != null && passable) _cap.enabled = false;
        }
    }
}
