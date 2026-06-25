/* BandMonitorWindow.cs
 * リスナー→音源の直線が、各周波数帯をどれだけ通すか（帯域別の透過ゲイン）を
 * 独立タブのバーで可視化するデバッグ用ウィンドウ。
 *
 *   メニュー: AcousticFlow > Band Monitor で開く。
 *   Scene/Game の隣にドッキングでき、ゲーム画面（OnGUI）は一切いじらない。
 *
 * 値の出どころ: AcousticFlowDemo が毎フレーム計算して
 *   AcousticFlowDemo.LatestBandGains に公開している 6 帯域の透過ゲイン(0..1)。
 *   1=素通り（バー満タン）/ 0=完全遮断（バー空）。壁を出入りすると動く。
 */
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class BandMonitorWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/Band Monitor")]
        public static void Open()
        {
            var w = GetWindow<BandMonitorWindow>("Band Monitor");
            w.minSize = new Vector2(260f, 220f);
            w.Show();
        }

        private void OnEnable()
        {
            // 再生中は毎エディタフレーム再描画して、バーをリアルタイムに動かす。
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
                    "Play 中にリスナーが聴く帯域を表示します。\nシーンを再生してください。",
                    MessageType.Info);
                return;
            }

            float[] gains = AcousticFlowDemo.LatestBandGains;
            if (gains == null || gains.Length == 0)
            {
                EditorGUILayout.HelpBox(
                    "データ待ち…（AcousticFlowDemo が動いているか確認）", MessageType.Warning);
                return;
            }

            EditorGUILayout.LabelField("帯域別 透過ゲイン (1=素通り / 0=遮断)", EditorStyles.boldLabel);
            EditorGUILayout.Space(4f);

            int n = Mathf.Min(gains.Length, AcousticEngine.BandFreqs.Length);
            for (int b = 0; b < n; b++)
            {
                DrawBandRow(AcousticEngine.BandFreqs[b], Mathf.Clamp01(gains[b]));
            }

            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField(
                "※ これは『壁が各帯域を通す量』。音源自身のスペクトラムではない。",
                EditorStyles.miniLabel);
        }

        // 1 帯域ぶんの行: 「 125Hz [#####____] 0.62」
        private static void DrawBandRow(int freqHz, float gain)
        {
            EditorGUILayout.BeginHorizontal();

            string label = freqHz >= 1000 ? (freqHz / 1000) + "kHz" : freqHz + "Hz";
            EditorGUILayout.LabelField(label, GUILayout.Width(46f));

            Rect r = GUILayoutUtility.GetRect(120f, 16f, GUILayout.ExpandWidth(true));
            EditorGUI.DrawRect(r, new Color(0.18f, 0.18f, 0.18f));   // 背景（空き）
            Rect fill = new Rect(r.x, r.y, r.width * gain, r.height);
            // 通る量が多い=緑、削れている=赤。減衰(1-gain)で色を寄せる。
            Color c = Color.Lerp(new Color(0.85f, 0.25f, 0.20f),
                                 new Color(0.25f, 0.80f, 0.35f), gain);
            EditorGUI.DrawRect(fill, c);

            EditorGUILayout.LabelField(gain.ToString("F2"), GUILayout.Width(36f));
            EditorGUILayout.EndHorizontal();
            EditorGUILayout.Space(2f);
        }
    }
}
