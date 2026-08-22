// BellGameHud.cs
// いま何が起きているかを、画面に出す。
//
// ★なぜ要るのか
//   「扉が開かない」「移動できない」を Console のログと推測で往復してしまった。
//   角度・入力・座標・状態は**見れば分かる**ものなので、見れば分かる形にする。
//   決めごと #7（症状 → 実測 → 原因）の「実測」を、毎回コードを足さずに取れるようにする。
//
// ⚠ 開発用。出荷前に外すか、キーで隠せるようにすること。
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class BellGameHud : MonoBehaviour
    {
        [Tooltip("表示の入り切り。")]
        public KeyCode toggleKey = KeyCode.F1;
        public bool show = true;

        private GUIStyle _style;

        private void Update()
        {
            if (Input.GetKeyDown(toggleKey)) show = !show;
        }

        private void OnGUI()
        {
            if (!show) return;
            if (_style == null)
                _style = new GUIStyle(GUI.skin.label) { fontSize = 13, richText = false };

            var set = WorldSet.Current;
            var door = FindFirstObjectByType<PhysicsDoor>();
            var wd = FindFirstObjectByType<WorldDoor>();
            var demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            var bells = FindFirstObjectByType<PlayerBells>();
            var move = FindFirstObjectByType<WorldTransition>();

            var sb = new System.Text.StringBuilder(512);

            sb.Append("いる世界: ").Append(set != null ? BellVoices.DisplayName(set.active) : "—")
              .Append("   持っている断片: ")
              .Append(bells != null ? bells.held.Count.ToString() : "—").Append('\n');

            // ★扉。角度が唯一の答えなので、それと入力を並べて出す。
            if (door != null)
            {
                bool o = Input.GetKey(door.openKey) || Input.GetKey(KeyCode.Keypad5);
                bool c = Input.GetKey(door.closeKey) || Input.GetKey(KeyCode.Keypad6);
                sb.Append("扉: ").Append(door.angleDeg.ToString("F1")).Append("°")
                  .Append("  [5]").Append(o ? "押" : "・")
                  .Append(" [6]").Append(c ? "押" : "・")
                  .Append("  掴み=").Append(door.grabbed ? "有" : "無")
                  .Append("（").Append(door.grabStatus).Append("）\n");
            }
            else sb.Append("扉: 見つからない\n");

            if (wd != null)
                sb.Append("行き先: ").Append(BellVoices.DisplayName(wd.destination))
                  .Append("   扉のそば=").Append(wd.playerIsNear ? "はい" : "いいえ").Append('\n');

            if (demo != null && demo.listener != null)
            {
                var p = demo.listener.position;
                sb.Append("自分: (").Append(p.x.ToString("F1")).Append(", ")
                  .Append(p.z.ToString("F1")).Append(")");
                var cam = Camera.main;
                if (cam != null)
                {
                    var q = cam.transform.position;
                    sb.Append("   カメラ: (").Append(q.x.ToString("F1")).Append(", ")
                      .Append(q.z.ToString("F1")).Append(")");
                    // ★自分とカメラがずれていたら、カメラが追従していない
                    //   ＝「動いているのに画が動かない」の正体。
                    if ((p - q).magnitude > 2f) sb.Append("  ★離れている");
                }
                sb.Append('\n');
            }

            if (move != null)
                sb.Append("くぐり: ").Append(move.status)
                  .Append("   開口までの奥行き=").Append(move.localZ.ToString("F2")).Append("m\n");

            sb.Append("\n7=断片取得＋扉を接続 / 5=開く 6=閉じる（押している間）/ Space=鳴らす / F1=この表示");

            var text = sb.ToString();
            var size = _style.CalcSize(new GUIContent(text));
            var r = new Rect(8, 8, Mathf.Max(560f, size.x + 16f), 150f);
            GUI.Box(r, GUIContent.none);
            GUI.Label(new Rect(r.x + 8, r.y + 6, r.width - 16, r.height - 12), text, _style);
        }
    }
}
