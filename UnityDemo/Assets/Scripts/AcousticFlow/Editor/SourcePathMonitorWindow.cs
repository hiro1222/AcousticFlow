/* SourcePathMonitorWindow.cs
 * 選択中の音源「面」のどの部分がリスナーに届いているかを、面を正面から見た
 * ヒートマップで可視化するデバッグ用ウィンドウ。
 *
 *   メニュー: AcousticFlow > Source Path Monitor で開く。
 *   Scene/Game の隣にドッキングでき、ゲーム画面（OnGUI）は一切いじらない。
 *
 * 値の出どころ: AcousticFlowDemo が毎フレーム、音源面を cols×rows に切って
 *   各点→リスナーの「直接到達量(0..1)」を計算し、
 *   AcousticFlowDemo.LatestFaceReachability に row-major で公開している。
 *     1=完全に届く（緑） / 0=壁で遮断（赤） / -1=面の外（円の角＝透明）。
 *   壁の縁で面の一部が欠ける「ソフト遮蔽」がそのまま見える。
 */
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class SourcePathMonitorWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/Source Path Monitor")]
        public static void Open()
        {
            var w = GetWindow<SourcePathMonitorWindow>("Source Path Monitor");
            w.minSize = new Vector2(280f, 320f);
            w.Show();
        }

        private void OnEnable()
        {
            // 再生中は毎エディタフレーム再描画して、ヒートマップをリアルタイムに動かす。
            EditorApplication.update += Repaint;
        }

        private void OnDisable()
        {
            EditorApplication.update -= Repaint;
        }

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox(
                    "Play 中に音源面のどこがリスナーに届くかを表示します。\nシーンを再生してください。",
                    MessageType.Info);
                return;
            }

            float[] grid = AcousticFlowDemo.LatestFaceReachability;
            int cols = AcousticFlowDemo.FaceGridCols;
            int rows = AcousticFlowDemo.FaceGridRows;
            if (grid == null || cols <= 0 || rows <= 0 || grid.Length < cols * rows)
            {
                EditorGUILayout.HelpBox(
                    "データ待ち…\n状態: " + AcousticFlowDemo.FaceStatus +
                    "\n（AcousticFlowDemo が動いているか確認）", MessageType.Warning);
                return;
            }

            EditorGUILayout.LabelField(
                $"音源面の到達量  [{AcousticFlowDemo.LatestFaceShape}]  {cols}×{rows}",
                EditorStyles.boldLabel);
            EditorGUILayout.LabelField(
                "緑=届く / 赤=遮断 / 黒=面の外。面を正面から見た図。", EditorStyles.miniLabel);
            EditorGUILayout.Space(4f);

            DrawHeatmap(grid, cols, rows);

            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField(
                "※ 直接経路のみ（壁の透過）。反射の回り込みは含まない。",
                EditorStyles.miniLabel);
        }

        // 面のヒートマップを正方セルで描く。row-major、r=0 が上・c=0 が左。
        private static void DrawHeatmap(float[] grid, int cols, int rows)
        {
            // 利用可能な矩形を確保し、セルを正方に保てる最大サイズを決める。
            Rect area = GUILayoutUtility.GetRect(
                cols, rows, GUILayout.ExpandWidth(true), GUILayout.ExpandHeight(true));

            float cell = Mathf.Floor(Mathf.Min(area.width / cols, area.height / rows));
            if (cell < 1f) cell = 1f;
            float gridW = cell * cols;
            float gridH = cell * rows;
            // 確保領域の中央に寄せる。
            float ox = area.x + (area.width - gridW) * 0.5f;
            float oy = area.y + (area.height - gridH) * 0.5f;

            // 背景（面の外/未塗り）。
            EditorGUI.DrawRect(new Rect(ox, oy, gridW, gridH), new Color(0.10f, 0.10f, 0.10f));

            var lowCol = new Color(0.85f, 0.25f, 0.20f);   // 0=遮断：赤
            var hiCol = new Color(0.25f, 0.80f, 0.35f);    // 1=届く：緑

            for (int r = 0; r < rows; r++)
            {
                for (int c = 0; c < cols; c++)
                {
                    float v = grid[r * cols + c];
                    if (v < 0f) continue;  // 面の外（円の角）は背景のまま

                    Color col = Color.Lerp(lowCol, hiCol, Mathf.Clamp01(v));
                    var cellRect = new Rect(ox + c * cell, oy + r * cell, cell, cell);
                    EditorGUI.DrawRect(cellRect, col);
                }
            }
        }
    }
}
