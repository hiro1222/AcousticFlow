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

            // --- 後期残響(FDN)を決めてる値 ---
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("後期残響(FDN)を決めてる値", EditorStyles.boldLabel);
            float rt = AcousticFlowSceneDemo.Status.RtSeconds;
            float wet = AcousticFlowSceneDemo.Status.Wet;
            float slv = AcousticFlowSceneDemo.Status.SourceLevel;
            EditorGUILayout.LabelField("  RT60(尾の長さ)", $"{rt:F2} s");
            EditorGUILayout.LabelField("  Wet(反響割合)", $"{wet:F2}");
            EditorGUILayout.LabelField("  遮蔽レベル(SourceLevel)", $"{slv:F2}  （1=素通り / 小=遮蔽）");
            EditorGUILayout.LabelField("  → 残響の効き ≈ Wet×遮蔽", $"{wet * slv:F3}  （小さいほど残響ほぼ無し）");
            EditorGUILayout.LabelField("  ※開けた場所は Wet≈0 か 遮蔽小 で残響が消えるべき", EditorStyles.miniLabel);

            // --- 帯域（主音源） ---
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("主音源 帯域ゲイン (1=素通り / 0=遮断)", EditorStyles.boldLabel);
            DrawBands("透過", AcousticFlowSceneDemo.Status.BandsTransmit);
            DrawBands("回折", AcousticFlowSceneDemo.Status.BandsDiffract);

            // --- 3バンドEQ 送出(dB)：Wwise の Occ_Low/Mid/High RTPC に送る値 ---
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField(
                AcousticFlowSceneDemo.Status.UseBandEq
                    ? "3バンドEQ 送出(dB)  [ON]" : "3バンドEQ 送出(dB)  [OFF: useBandEq を ON に]",
                EditorStyles.boldLabel);
            var eq = AcousticFlowSceneDemo.Status.EqDb;
            var enames = AcousticFlowSceneDemo.Status.SourceNames;
            if (eq != null && enames != null)
            {
                int ns = Mathf.Min(enames.Length, eq.Length / 3);
                for (int i = 0; i < ns; i++)
                    EditorGUILayout.LabelField(enames[i],
                        $"Low {eq[i * 3 + 0]:F1}   Mid {eq[i * 3 + 1]:F1}   High {eq[i * 3 + 2]:F1}  dB");
            }

            // --- #2: IRタップ / 伝搬遅延（主音源） ---
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("IRタップ / 伝搬遅延（主音源）", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("ITDG(最初の反射まで)",
                $"{AcousticFlowSceneDemo.Status.ItdgMs:F1} ms   （大=広い / 小=狭い）");
            var td = AcousticFlowSceneDemo.Status.TapDelayMs;
            var tg = AcousticFlowSceneDemo.Status.TapGain;
            var tt = AcousticFlowSceneDemo.Status.TapType;
            int tc = AcousticFlowSceneDemo.Status.TapCount;
            DrawIrPlot(td, tg, tt, tc, 120f);   // 反射パターン＝時間軸に各タップを縦線で（インパルス応答）
            EditorGUILayout.LabelField("  白=直接 / 緑=反射 / 水色=回折  （X=時間 0〜120ms, Y=ゲイン）",
                EditorStyles.miniLabel);
            if (td != null && tg != null && tt != null)
            {
                int show = Mathf.Min(tc, Mathf.Min(td.Length, Mathf.Min(tg.Length, tt.Length)));
                for (int i = 0; i < show; i++)
                {
                    string label = tt[i] == 'D' ? "直接" : (tt[i] == 'R' ? "反射" : "回折");
                    EditorGUILayout.LabelField($"  {label}", $"{td[i]:F1} ms    gain {tg[i]:F2}");
                }
            }
            EditorGUILayout.LabelField("※このタップ列を IrConvolver が畳み込んで鳴らす（段1）",
                EditorStyles.miniLabel);

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

        // 反射パターン＝IR（インパルス応答）のプロット。X=時間(0..maxMs)、縦線=各タップ、高さ=ゲイン、色=種別。
        //   直接が左端(0ms)、反射が右に散る。狭い部屋=左に密集 / 広い空間=右まで広がる。
        private static void DrawIrPlot(float[] delayMs, float[] gain, char[] type, int count, float maxMs)
        {
            Rect r = GUILayoutUtility.GetRect(200f, 90f, GUILayout.ExpandWidth(true));
            EditorGUI.DrawRect(r, new Color(0.11f, 0.11f, 0.12f));                 // 背景
            float baseY = r.yMax - 4f;
            EditorGUI.DrawRect(new Rect(r.x, baseY, r.width, 1f), new Color(0.35f, 0.35f, 0.35f));  // 基線
            // 目盛（0 / 中間 / max ms）
            for (int k = 0; k <= 2; k++)
            {
                float gx = r.x + r.width * (k / 2f);
                EditorGUI.DrawRect(new Rect(gx, r.y, 1f, r.height), new Color(0.2f, 0.2f, 0.22f));
            }
            if (delayMs == null || gain == null || type == null || count <= 0 || maxMs <= 0f) return;

            float h = r.height - 8f;
            int m = Mathf.Min(count, Mathf.Min(delayMs.Length, Mathf.Min(gain.Length, type.Length)));
            for (int i = 0; i < m; i++)
            {
                float x = r.x + Mathf.Clamp01(delayMs[i] / maxMs) * r.width;
                float barH = Mathf.Clamp01(gain[i]) * h;
                Color c = type[i] == 'D' ? Color.white
                        : (type[i] == 'R' ? new Color(0.30f, 0.85f, 0.40f) : new Color(0.30f, 0.80f, 0.95f));
                EditorGUI.DrawRect(new Rect(x, baseY - barH, 2f, barH), c);
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
