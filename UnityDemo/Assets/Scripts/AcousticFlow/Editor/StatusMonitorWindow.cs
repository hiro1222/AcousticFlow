/* StatusMonitorWindow.cs
 * ゲーム画面(OnGUI)の HUD に収まりきらないエンジン状態を、独立タブでまとめて表示する
 * デバッグ用ウィンドウ。特に回折デバッグ向けに「主音源の回折δ」「回折二次音源の鳴動数」を
 * 強調し、下部に『次に何を見るべきか』のヒントを出す。
 *
 *   メニュー: AcousticFlow > Status Monitor で開く。Scene/Game の隣にドッキング可。
 *   値の出どころ: AcousticFlowSceneDemo.Status（PublishMonitors() が毎フレーム更新）。
 */
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class StatusMonitorWindow : EditorWindow
    {
        private Vector2 _scroll;

        [MenuItem("AcousticFlow/Status Monitor")]
        public static void Open()
        {
            var w = GetWindow<StatusMonitorWindow>("Status Monitor");
            w.minSize = new Vector2(300f, 320f);
            w.Show();
        }

        private void OnEnable() { EditorApplication.update += Repaint; }
        private void OnDisable() { EditorApplication.update -= Repaint; }

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("Play 中にエンジンの状態を表示します。\nシーンを再生してください。",
                                        MessageType.Info);
                return;
            }
            if (!AcousticFlowSceneDemo.Status.Valid)
            {
                EditorGUILayout.HelpBox("データ待ち…（AcousticFlowDemo が動いているか確認）",
                                        MessageType.Warning);
                return;
            }

            _scroll = EditorGUILayout.BeginScrollView(_scroll);

            // --- 概況 ---
            EditorGUILayout.LabelField(
                $"FPS {AcousticFlowSceneDemo.Status.Fps:F0}    " +
                $"音響計算 {AcousticFlowSceneDemo.Status.AcousticMs:F2} ms/f    " +
                $"{(AcousticFlowSceneDemo.Status.UseHrtf ? "HRTF" : "パン")}(H)    " +
                $"ステア {(AcousticFlowSceneDemo.Status.UseSteer ? "ON" : "OFF")}(G)",
                EditorStyles.miniBoldLabel);

            // --- 遮蔽（音源ごと） ---
            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("遮蔽（音源ごと。赤=遮られてる / 緑=素通り）", EditorStyles.boldLabel);
            var occ = AcousticFlowSceneDemo.Status.Occlusion;
            var surv = AcousticFlowSceneDemo.Status.Survival;
            var names = AcousticFlowSceneDemo.Status.SourceNames;
            if (occ != null && names != null)
            {
                int n = Mathf.Min(occ.Length, names.Length);
                for (int i = 0; i < n; i++)
                {
                    float o = Mathf.Clamp01(occ[i]);
                    // 遮蔽が大きいほど赤・バーが伸びる。数値は遮蔽値。
                    DrawBar(names[i], o, o.ToString("F2"),
                            new Color(0.25f, 0.80f, 0.35f), new Color(0.85f, 0.25f, 0.20f));
                    if (surv != null && i < surv.Length)
                        EditorGUILayout.LabelField("   生存(帯域)", surv[i].ToString("F2"),
                                                   EditorStyles.miniLabel);
                }
            }

            // --- 回折（デバッグの主役） ---
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("回折", EditorStyles.boldLabel);
            float dd = AcousticFlowSceneDemo.Status.DiffDelta;
            EditorGUILayout.LabelField("主音源の回折",
                dd >= 0f ? $"迂回路あり  δ={dd:F2} m" : "なし（迂回路が見つからない）");

            int dAct = AcousticFlowSceneDemo.Status.DiffActive;
            int dCap = AcousticFlowSceneDemo.Status.DiffCap;
            bool dOn = AcousticFlowSceneDemo.Status.DiffSrcEnabled;
            DrawCountRow("回折二次音源 (V)", dOn, dAct, dCap);

            int eAct = AcousticFlowSceneDemo.Status.ErActive;
            int eCap = AcousticFlowSceneDemo.Status.ErCap;
            bool eOn = AcousticFlowSceneDemo.Status.EarlyReflEnabled;
            DrawCountRow("早期反射 (F)", eOn, eAct, eCap);

            EditorGUILayout.LabelField("エッジカタログ",
                AcousticFlowSceneDemo.Status.UseEdgeCatalog
                    ? $"{AcousticFlowSceneDemo.Status.EdgeCatalogCount} 稜線" : "OFF");
            if (AcousticFlowSceneDemo.Status.ShowDiffCandidates)
                EditorGUILayout.LabelField("回折候補(主音源)",
                    $"{AcousticFlowSceneDemo.Status.DiffCandCount} 本 (C)");

            // --- 帯域（主音源） ---
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("主音源 帯域ゲイン (1=素通り / 0=遮断)", EditorStyles.boldLabel);
            DrawBands("透過", AcousticFlowSceneDemo.Status.BandsTransmit);
            DrawBands("回折", AcousticFlowSceneDemo.Status.BandsDiffract);

            // --- 次に何を見るべきかのヒント（回折が鳴らない切り分け） ---
            EditorGUILayout.Space(8f);
            DrawHint(dOn, dAct, dd);

            EditorGUILayout.EndScrollView();
        }

        // 「鳴動 N/上限」行。N>0 は緑、0 は灰で強調。
        private static void DrawCountRow(string label, bool on, int active, int cap)
        {
            EditorGUILayout.BeginHorizontal();
            EditorGUILayout.LabelField(label, GUILayout.Width(150f));
            if (!on) { EditorGUILayout.LabelField("OFF"); EditorGUILayout.EndHorizontal(); return; }
            var prev = GUI.color;
            GUI.color = active > 0 ? new Color(0.5f, 1f, 0.6f) : new Color(0.7f, 0.7f, 0.7f);
            EditorGUILayout.LabelField($"ON   鳴動 {active} 本 / 上限 {cap}");
            GUI.color = prev;
            EditorGUILayout.EndHorizontal();
        }

        // 1本のバー行: 「label [####____] valueText」。左→右で leftColor→rightColor に寄せる。
        private static void DrawBar(string label, float t, string valueText, Color leftColor, Color rightColor)
        {
            EditorGUILayout.BeginHorizontal();
            EditorGUILayout.LabelField(label, GUILayout.Width(120f));
            Rect r = GUILayoutUtility.GetRect(100f, 16f, GUILayout.ExpandWidth(true));
            EditorGUI.DrawRect(r, new Color(0.18f, 0.18f, 0.18f));
            Rect fill = new Rect(r.x, r.y, r.width * Mathf.Clamp01(t), r.height);
            EditorGUI.DrawRect(fill, Color.Lerp(leftColor, rightColor, Mathf.Clamp01(t)));
            EditorGUILayout.LabelField(valueText, GUILayout.Width(40f));
            EditorGUILayout.EndHorizontal();
            EditorGUILayout.Space(2f);
        }

        private static void DrawBands(string label, float[] g)
        {
            if (g == null) return;
            int n = Mathf.Min(g.Length, AcousticEngine.BandFreqs.Length);
            for (int b = 0; b < n; b++)
            {
                int f = AcousticEngine.BandFreqs[b];
                string fl = (f >= 1000 ? (f / 1000) + "k" : f.ToString()) + "Hz " + label;
                float v = Mathf.Clamp01(g[b]);
                DrawBar(fl, v, v.ToString("F2"),
                        new Color(0.85f, 0.25f, 0.20f), new Color(0.25f, 0.80f, 0.35f));
            }
        }

        // 回折が鳴らないときの切り分けヒント。
        private static void DrawHint(bool diffOn, int diffActive, float delta)
        {
            if (!diffOn)
            {
                EditorGUILayout.HelpBox("回折二次音源は OFF。V キーで ON にできます。", MessageType.Info);
                return;
            }
            if (diffActive > 0)
            {
                EditorGUILayout.HelpBox(
                    $"回折二次音源が {diffActive} 本 鳴動中。小さくて聞こえないなら " +
                    "Inspector の diffractionLevelScale を上げる。", MessageType.Info);
            }
            else if (delta < 0f)
            {
                EditorGUILayout.HelpBox(
                    "鳴動 0 本＋迂回路なし＝この位置では回折が発火していない。\n" +
                    "→ そもそも遮蔽が成立しているか（上の遮蔽バーが赤いか）、壁が回折対象になっているかを確認。\n" +
                    "レベルを上げても意味がない段階。", MessageType.Warning);
            }
            else
            {
                EditorGUILayout.HelpBox(
                    "迂回路はあるのに鳴動 0 本＝エッジのクラスタ化で棄却されている（UTD重み/自己除外）。\n" +
                    "エンジン側の閾値を見る必要あり。", MessageType.Warning);
            }
        }
    }
}
