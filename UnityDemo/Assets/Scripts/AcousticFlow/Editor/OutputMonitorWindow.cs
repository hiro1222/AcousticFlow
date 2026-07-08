/* OutputMonitorWindow.cs
 * リスナーに届いている実際の出力（マスターバス）の左右レベルを、時間方向に
 * スクロールする波形（エンベロープ）として可視化するデバッグ用ウィンドウ。
 *
 *   メニュー: AcousticFlow > Output Monitor で開く。
 *
 * 値の出どころ: AcousticFlowDemo が毎フレーム AcousticEngine.GetOutputLevels で
 *   マスターバスの L/R RMS（線形）を取り、リングバッファ
 *   AcousticFlowSceneDemo.OutHistoryL/R に積んでいる（OutHistoryHead が最古位置）。
 *   左右の振幅が時間で動く＝音の鳴り・定位の偏りが波形として見える。
 *   ※ Windows Sonic の「前」のバス値なので HRTF の頭部回り込みは映らない。
 */
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class OutputMonitorWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/Output Monitor")]
        public static void Open()
        {
            var w = GetWindow<OutputMonitorWindow>("Output Monitor");
            w.minSize = new Vector2(360f, 220f);
            w.Show();
        }

        private void OnEnable() { EditorApplication.update += Repaint; }
        private void OnDisable() { EditorApplication.update -= Repaint; }

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox(
                    "Play 中に、リスナーに届く出力(マスターバス)の左右レベルを波形表示します。\nシーンを再生してください。",
                    MessageType.Info);
                return;
            }

            float[] hl = AcousticFlowSceneDemo.OutHistoryL;
            float[] hr = AcousticFlowSceneDemo.OutHistoryR;
            if (hl == null || hr == null || hl.Length == 0)
            {
                EditorGUILayout.HelpBox("データ待ち…（再生中か確認）", MessageType.Warning);
                return;
            }
            int head = AcousticFlowSceneDemo.OutHistoryHead;
            int len = hl.Length;

            // 直近値（メータリングが効いているかの確認にもなる）。
            int last = (head - 1 + len) % len;
            EditorGUILayout.LabelField(
                $"出力レベル(線形)  L={hl[last]:F3}  R={hr[last]:F3}", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("左=上レーン / 右=下レーン。横軸=時間（右が最新）。", EditorStyles.miniLabel);
            EditorGUILayout.Space(4f);

            Rect area = GUILayoutUtility.GetRect(len, 140f,
                GUILayout.ExpandWidth(true), GUILayout.ExpandHeight(true));
            EditorGUI.DrawRect(area, new Color(0.08f, 0.08f, 0.08f));

            float laneH = area.height * 0.5f;
            DrawLane(new Rect(area.x, area.y, area.width, laneH), hl, head, len,
                     new Color(0.30f, 0.70f, 1.0f));   // L: 青
            DrawLane(new Rect(area.x, area.y + laneH, area.width, laneH), hr, head, len,
                     new Color(1.0f, 0.55f, 0.30f));   // R: 橙

            // レーン境界線。
            EditorGUI.DrawRect(new Rect(area.x, area.y + laneH - 1f, area.width, 1f),
                               new Color(1f, 1f, 1f, 0.15f));

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField(
                "※ マスターバスのRMS（Windows Sonic前）。HRTFの頭部回り込みは含まない。",
                EditorStyles.miniLabel);
        }

        // 1レーンぶんの波形（中央線から上下対称のエンベロープ）を描く。
        private static void DrawLane(Rect lane, float[] hist, int head, int len, Color col)
        {
            float midY = lane.y + lane.height * 0.5f;
            float colW = lane.width / len;
            const float gain = 4.0f;  // 線形RMSは小さめなので見やすく増幅
            for (int x = 0; x < len; x++)
            {
                int i = (head + x) % len;                 // 最古→最新を左→右に
                float a = Mathf.Clamp01(hist[i] * gain);
                float h = a * (lane.height * 0.48f);
                var r = new Rect(lane.x + x * colW, midY - h, Mathf.Max(1f, colW), h * 2f);
                EditorGUI.DrawRect(r, col);
            }
        }
    }
}
