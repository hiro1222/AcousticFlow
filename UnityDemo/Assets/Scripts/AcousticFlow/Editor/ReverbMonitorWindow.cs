/* ReverbMonitorWindow.cs
 * 選択中の音源 → リスナーに届くエネルギーを「到達時間」で並べたエコグラム
 * （＝インパルス応答の形）を独立タブで可視化するデバッグ用ウィンドウ。
 *
 *   メニュー: AcousticFlow > Reverb Monitor で開く。
 *
 * 値の出どころ: AcousticFlowDemo が数フレームごとに音源面→リスナーのレイ積分を
 *   到達時間ビンに積算し、AcousticFlowDemo.LatestEchogram に公開している。
 *     先頭の大ピーク = 直接音 / その後のまばらな山 = 初期反射 /
 *     指数的に減衰する尾 = 残響。尾が長い = よく響く部屋。
 *   縦軸は dB 表示（ピーク基準 0〜-60dB）で、減衰の様子が直線的に見える。
 */
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class ReverbMonitorWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/Reverb Monitor")]
        public static void Open()
        {
            var w = GetWindow<ReverbMonitorWindow>("Reverb Monitor");
            w.minSize = new Vector2(340f, 240f);
            w.Show();
        }

        private void OnEnable() { EditorApplication.update += Repaint; }
        private void OnDisable() { EditorApplication.update -= Repaint; }

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox(
                    "Play 中に、選択音源→リスナーの到達エネルギーを到達時間順に表示します。\nシーンを再生してください。",
                    MessageType.Info);
                return;
            }

            float[] bins = AcousticFlowDemo.LatestEchogram;
            int n = AcousticFlowDemo.EchogramBins;
            float binMs = AcousticFlowDemo.EchogramBinMs;
            if (bins == null || n <= 0 || bins.Length < n)
            {
                EditorGUILayout.HelpBox("データ待ち…（AcousticFlowDemo が動いているか確認）", MessageType.Warning);
                return;
            }

            // ピーク（直接音）とおおよその尾の長さ（-60dB を下回る最後の時刻）を出す。
            float peak = 0f;
            for (int k = 0; k < n; k++) if (bins[k] > peak) peak = bins[k];
            int tailBin = 0;
            if (peak > 0f)
            {
                float floor = peak * 0.001f;  // -60dB
                for (int k = 0; k < n; k++) if (bins[k] > floor) tailBin = k;
            }
            float windowMs = n * binMs;
            float tailMs = (tailBin + 1) * binMs;

            EditorGUILayout.LabelField("エコグラム（到達時間 → エネルギー / 縦=dB）", EditorStyles.boldLabel);
            EditorGUILayout.LabelField(
                $"窓 {windowMs:F0}ms（1ビン {binMs:F0}ms）  尾の長さ ≈ {tailMs:F0}ms", EditorStyles.miniLabel);
            EditorGUILayout.Space(4f);

            DrawEchogram(bins, n, peak);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField(
                "橙=直接音(最大) / 青=初期反射・残響の尾。尾が長いほどよく響く部屋。",
                EditorStyles.miniLabel);
        }

        // 到達時間ビンを縦棒で描く。高さは dB（ピーク基準 0..-60dB）。
        private static void DrawEchogram(float[] bins, int n, float peak)
        {
            Rect area = GUILayoutUtility.GetRect(n, 140f, GUILayout.ExpandWidth(true), GUILayout.ExpandHeight(true));
            EditorGUI.DrawRect(area, new Color(0.10f, 0.10f, 0.10f));
            if (peak <= 0f) return;

            float barW = area.width / n;
            float baseY = area.yMax;
            var direct = new Color(0.85f, 0.45f, 0.2f);   // 直接音(最大)：橙
            var tail = new Color(0.30f, 0.55f, 0.85f);    // 反射・残響：青

            for (int k = 0; k < n; k++)
            {
                float e = bins[k];
                if (e <= 0f) continue;
                float db = 20f * Mathf.Log10(e / peak);          // ≤0
                float h01 = Mathf.Clamp01((db + 60f) / 60f);     // -60dB→0, 0dB→1
                float h = h01 * area.height;
                bool isPeak = e >= peak;                         // 直接音（最大ビン）
                var r = new Rect(area.x + k * barW, baseY - h, Mathf.Max(1f, barW - 1f), h);
                EditorGUI.DrawRect(r, isPeak ? direct : tail);
            }
        }
    }
}
