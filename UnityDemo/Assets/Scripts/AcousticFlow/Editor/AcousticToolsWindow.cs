/* AcousticToolsWindow.cs
 * 音響まわりの道具を 1 枚の窓にタブで束ねたもの。
 *
 *   メニュー: AcousticFlow > ツール (履歴・予算)
 *
 * なぜ 1 枚に束ねるか:
 *   監視窓が既に 5 枚あり（Status / Band / Output / Reverb / OutputScope）、
 *   そこへ道具を 1 つずつ足していくと「どの窓に何があるか」を探す代金のほうが高くつく。
 *   道具を足すたびにタブが 1 つ増える形にしておく。
 *
 * いま入っているタブ:
 *   履歴 … 道具2。毎フレームの状態を巻き戻して見る（記録は AcousticHistory）
 *   予算 … 道具10。audio thread の実費と音源数の余裕
 *   音源 … 道具A。ソロ／ミュートと寄与度順の一覧
 *   情報 … 2026-09-09 追加。いま何で鳴っているか（模型の切り替え・尾・部屋・音の出どころ）。
 *          ★画面の HUD は箱が 480x300 で固定なので、下に足した行から順に**切れて見えなくなる**
 *            （尾の切り替えを足しても表示が変わらず「効いていない」と読めてしまった）。
 *            聞き比べの判断はこのタブで行う。ここは窓なので伸びるし、切り替えも押せる。
 */
using System.IO;
using UnityEditor;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public class AcousticToolsWindow : EditorWindow
    {
        private enum Tab { Info, History, Budget, Sources }
        private static readonly string[] kTabNames = { "情報", "履歴", "予算", "音源" };

        private Tab _tab = Tab.Info;
        private Vector2 _scroll;

        // --- 履歴タブの状態 ---
        private bool _follow = true;          // 最新に追従するか
        private int _cursor;                  // 見ているフレーム（0 = 一番古い）
        private Vector3[] _pts = new Vector3[0];

        // --- 予算タブの状態 ---
        // 36 は実測（1 スレッドが飽和するまで。SYSTEM_BRIEF §6）。推定値ではない。
        private int _budget = 36;

        [MenuItem("AcousticFlow/ツール (履歴・予算)")]
        public static void Open()
        {
            var w = GetWindow<AcousticToolsWindow>("AF ツール");
            w.minSize = new Vector2(420f, 420f);
            w.Show();
        }

        private void OnEnable()
        {
            EditorApplication.update += Repaint;
            SceneView.duringSceneGui += OnSceneGui;
        }

        private void OnDisable()
        {
            EditorApplication.update -= Repaint;
            SceneView.duringSceneGui -= OnSceneGui;
            // 窓を閉じたらソロは必ず解除する。切ったまま忘れると「音が出ない」で悩む。
            IrConvolver.Solo.Reset();
        }

        private void OnGUI()
        {
            _tab = (Tab)GUILayout.Toolbar((int)_tab, kTabNames);
            EditorGUILayout.Space(4f);

            if (_tab == Tab.Info) DrawInfo();
            else if (_tab == Tab.History) DrawHistory();
            else if (_tab == Tab.Budget) DrawBudget();
            else DrawSources();
        }

        // =====================================================================
        // 情報タブ ── いま何で鳴っているか（読むだけでなく、ここから切り替える）
        // =====================================================================
        private Vector2 _infoScroll;

        private static string OnOff(bool b) { return b ? "ON" : "OFF"; }
        private static string Db(float lin) { return (lin > 1e-6f) ? (20f * Mathf.Log10(lin)).ToString("F1") + " dB" : "無音"; }

        private void DrawInfo()
        {
            var demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo == null) { EditorGUILayout.HelpBox("AcousticFlowSceneDemo が場面にありません。", MessageType.Warning); return; }
            bool playing = EditorApplication.isPlaying;
            _infoScroll = EditorGUILayout.BeginScrollView(_infoScroll);

            // ── 後期の尾（聞き比べの主役）──
            EditorGUILayout.LabelField("後期の尾", EditorStyles.boldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("模型", GUILayout.Width(60f));
                int now = Mathf.Clamp(demo.tailModel, 0, 1);
                int next = GUILayout.Toolbar(now, new[] { "0 畳み込み（IR）", "1 FDN（部屋ごと）" }, GUILayout.Width(260f));
                if (next != now) { Undo.RecordObject(demo, "tailModel"); demo.tailModel = next; EditorUtility.SetDirty(demo); }
                GUILayout.Label("(K キーでも切替)", EditorStyles.miniLabel);
            }
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("画面の HUD", GUILayout.Width(60f));
                bool d = GUILayout.Toggle(demo.hudDetail, "一覧も画面に出す（既定 OFF＝2 行）", "Button", GUILayout.Width(260f));
                if (d != demo.hudDetail) { Undo.RecordObject(demo, "hudDetail"); demo.hudDetail = d; EditorUtility.SetDirty(demo); }
                GUILayout.Label("(ビルドで見るとき用)", EditorStyles.miniLabel);
            }
            var tb = Object.FindFirstObjectByType<TailBusRenderer>();
            if (!playing)
            {
                EditorGUILayout.HelpBox("再生中に、尾の器と音の出どころを表示します。", MessageType.Info);
            }
            else if (demo.tailModel == 1)
            {
                string why = AcousticFlowSceneDemo.Status.TailFdnWhy;
                bool live = tb != null && tb.FdnMixHandle != System.IntPtr.Zero;
                if (!live)
                    EditorGUILayout.HelpBox("FDN の器がありません: " + (string.IsNullOrEmpty(why) ? "作成待ち" : why), MessageType.Warning);
                else
                {
                    EditorGUILayout.LabelField("  部屋の FDN", tb.FdnRoomCount + " 本   出力 " + Db(tb.FdnRms));
                    EditorGUILayout.LabelField("  生きた RT60 (500Hz)", AcousticFlowSceneDemo.Status.TailFdnRt60.ToString("F2") + " s"
                        + "    口の素通し " + AcousticFlowSceneDemo.Status.TailFdnOpen.ToString("F2"));
                    EditorGUILayout.LabelField("  向き", tb.DirectionBusHandle != System.IntPtr.Zero
                        ? "方向バス（戸口の向き・自室は一様）" : "L/R の 2 本（方向バスが OFF）");
                }
            }
            else
            {
                EditorGUILayout.LabelField("  尾の共有バス", OnOff(tb != null && tb.enableSharedTail)
                    + "   （音源ごとの尾の出力は「音源」タブ、または下の一覧の 出力 を見る）");
            }

            // ── 音の出どころ（M を押しても消えない、の切り分け）──
            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("音の出どころ", EditorStyles.boldLabel);
            if (!playing) { EditorGUILayout.EndScrollView(); return; }
            var all = Object.FindObjectsByType<AudioSource>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
            if (all.Length == 0) EditorGUILayout.HelpBox("鳴っている AudioSource がありません。", MessageType.Warning);
            int muted = 0, playingN = 0;
            foreach (var a in all) { if (a.mute) muted++; if (a.isPlaying) playingN++; }
            EditorGUILayout.LabelField("  AudioSource", all.Length + " 本（再生中 " + playingN + "・ミュート " + muted + "）");
            // ★ミュートが効かないときの正体はたいてい「デモが知らない AudioSource が鳴っている」。
            //   デモの M キーは Inspector の sources に並んだ物しか触らないので、一覧で見分ける。
            foreach (var a in all)
            {
                var vc = a.GetComponent<VoiceConvolver>();
                var ic = a.GetComponent<IrConvolver>();
                string dsp = (vc != null) ? "C++ 畳み込み器" : (ic != null) ? "C# 畳み込み器" : "素通し（畳み込み器なし）";
                string outRms = (vc != null) ? "  出力 " + Db(vc.rmsOut) : "";
                using (new EditorGUILayout.HorizontalScope())
                {
                    GUILayout.Label((a.mute ? "[消] " : a.isPlaying ? "[鳴] " : "[停] ") + a.gameObject.name, GUILayout.Width(190f));
                    GUILayout.Label(dsp + "   音量 " + a.volume.ToString("F2") + outRms, EditorStyles.miniLabel);
                    if (GUILayout.Button(a.mute ? "解除" : "消す", EditorStyles.miniButton, GUILayout.Width(44f))) a.mute = !a.mute;
                }
            }

            // ── 診断の一覧 ──
            //   ★画面の HUD と**同じ関数**を呼ぶ（AcousticFlowSceneDemo.DrawDiagnosticsGui）。
            //     ここで書き写すと、片方だけ直して食い違う。窓は伸びるので、全部ここで見られる。
            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("診断（画面の HUD と同じ中身）", EditorStyles.boldLabel);
            if (_diagStyle == null) _diagStyle = new GUIStyle(GUI.skin.label) { fontSize = 12, wordWrap = true };
            demo.DrawDiagnosticsGui(_diagStyle);
            EditorGUILayout.EndScrollView();
        }
        private GUIStyle _diagStyle;

        // =====================================================================
        // 履歴タブ（道具2）
        // =====================================================================
        private void DrawHistory()
        {
            int n = AcousticHistory.Count;

            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                _follow = GUILayout.Toggle(_follow, "最新に追従", EditorStyles.toolbarButton, GUILayout.Width(80f));
                if (GUILayout.Button("いまに印", EditorStyles.toolbarButton, GUILayout.Width(64f)))
                    AcousticHistory.MarkLatest(AcousticHistory.MarkManual);
                if (GUILayout.Button("クリア", EditorStyles.toolbarButton, GUILayout.Width(52f)))
                { AcousticHistory.Clear(); _cursor = 0; }
                if (GUILayout.Button("CSV", EditorStyles.toolbarButton, GUILayout.Width(44f)))
                {
                    string p = EditorUtility.SaveFilePanel("履歴を書き出す", "", "acoustic_history.csv", "csv");
                    if (!string.IsNullOrEmpty(p))
                    {
                        AcousticHistory.ExportCsv(p);
                        Debug.Log("[AFツール] 履歴を書き出した: " + p + "（" + n + " フレーム）");
                    }
                }
                GUILayout.FlexibleSpace();
                GUILayout.Label(n + " / " + AcousticHistory.Capacity + " フレーム", EditorStyles.miniLabel);
            }

            // 跳びの検出しきい値。ここは道具1（不連続ウォッチャ）の芽。
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("跳びと見なす差", GUILayout.Width(84f));
                AcousticHistory.JumpDb = EditorGUILayout.FloatField(AcousticHistory.JumpDb, GUILayout.Width(40f));
                GUILayout.Label("dB / 遮蔽", GUILayout.Width(50f));
                AcousticHistory.JumpOcc = EditorGUILayout.FloatField(AcousticHistory.JumpOcc, GUILayout.Width(40f));
                GUILayout.FlexibleSpace();
                var st = new GUIStyle(EditorStyles.miniLabel);
                if (AcousticHistory.JumpCount > 0) st.normal.textColor = new Color(1f, 0.45f, 0.35f);
                GUILayout.Label("検出 " + AcousticHistory.JumpCount + " 件", st);
            }

            if (n <= 0)
            {
                EditorGUILayout.HelpBox(
                    "まだ履歴がありません。\n再生すると自動で溜まります（窓を開いていなくても記録しています）。",
                    MessageType.Info);
                return;
            }

            if (_follow) _cursor = n - 1;
            _cursor = Mathf.Clamp(_cursor, 0, n - 1);

            Rect graph = GUILayoutUtility.GetRect(10f, 10f, GUILayout.ExpandWidth(true), GUILayout.Height(132f));
            DrawGraph(graph, n);

            // 掃引バー。触ったら追従を切る（見たい所で止まってほしいので）。
            int newCursor = Mathf.RoundToInt(GUILayout.HorizontalSlider(_cursor, 0f, n - 1));
            if (newCursor != _cursor) { _cursor = newCursor; _follow = false; }

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("◀", GUILayout.Width(28f))) { _cursor = Mathf.Max(0, _cursor - 1); _follow = false; }
                if (GUILayout.Button("▶", GUILayout.Width(28f))) { _cursor = Mathf.Min(n - 1, _cursor + 1); _follow = false; }
                if (GUILayout.Button("◀ 前の印", GUILayout.Width(70f))) { _cursor = FindMark(_cursor, -1, n); _follow = false; }
                if (GUILayout.Button("次の印 ▶", GUILayout.Width(70f))) { _cursor = FindMark(_cursor, +1, n); _follow = false; }
                GUILayout.FlexibleSpace();
                float t0 = AcousticHistory.Time[AcousticHistory.Index(0)];
                float t = AcousticHistory.Time[AcousticHistory.Index(_cursor)];
                GUILayout.Label("t = " + (t - t0).ToString("F2") + " s"
                                + "   （最新から " + (n - 1 - _cursor) + " フレーム前）", EditorStyles.miniLabel);
            }

            EditorGUILayout.Space(4f);
            _scroll = EditorGUILayout.BeginScrollView(_scroll);
            DrawFrameDetail(AcousticHistory.Index(_cursor));
            EditorGUILayout.EndScrollView();
        }

        private int FindMark(int from, int dir, int n)
        {
            for (int i = from + dir; i >= 0 && i < n; i += dir)
                if (AcousticHistory.Mark[AcousticHistory.Index(i)] != AcousticHistory.MarkNone) return i;
            return from;
        }

        /// <summary>出力レベル(dB)の推移。跳びと印は縦線で出す。</summary>
        private void DrawGraph(Rect r, int n)
        {
            EditorGUI.DrawRect(r, new Color(0.13f, 0.13f, 0.15f));

            const float dbMin = -80f, dbMax = 0f;
            // 目盛り（20dB ごと）
            for (float db = dbMin; db <= dbMax; db += 20f)
            {
                float y = r.y + r.height * (1f - (db - dbMin) / (dbMax - dbMin));
                EditorGUI.DrawRect(new Rect(r.x, y, r.width, 1f), new Color(1f, 1f, 1f, 0.07f));
                GUI.Label(new Rect(r.x + 2f, y - 7f, 40f, 14f), db.ToString("F0"), EditorStyles.miniLabel);
            }

            // 印（縦線）。先に描いて線の下に敷く。
            for (int i = 0; i < n; i++)
            {
                int k = AcousticHistory.Index(i);
                int m = AcousticHistory.Mark[k];
                if (m == AcousticHistory.MarkNone) continue;
                float x = r.x + r.width * (n <= 1 ? 0f : (float)i / (n - 1));
                Color c = (m == AcousticHistory.MarkJump)
                    ? new Color(1f, 0.35f, 0.25f, 0.75f) : new Color(0.3f, 0.85f, 1f, 0.75f);
                EditorGUI.DrawRect(new Rect(x, r.y, 1f, r.height), c);
            }

            if (Event.current.type == EventType.Repaint)
            {
                Handles.BeginGUI();
                Series(r, n, AcousticHistory.RmsTail, new Color(0.35f, 0.6f, 1f), dbMin, dbMax);
                Series(r, n, AcousticHistory.RmsEarly, new Color(1f, 0.85f, 0.3f), dbMin, dbMax);
                Series(r, n, AcousticHistory.RmsDirect, new Color(0.4f, 1f, 0.5f), dbMin, dbMax);
                Series(r, n, AcousticHistory.RmsOut, Color.white, dbMin, dbMax);
                Handles.EndGUI();
            }

            // いま見ているフレーム
            float cx = r.x + r.width * (n <= 1 ? 0f : (float)_cursor / (n - 1));
            EditorGUI.DrawRect(new Rect(cx, r.y, 1f, r.height), new Color(1f, 1f, 1f, 0.85f));

            GUI.Label(new Rect(r.xMax - 210f, r.y + 2f, 208f, 14f),
                      "出力 / 直接 / 早期 / 尾   (dB)", EditorStyles.miniLabel);
        }

        private void Series(Rect r, int n, float[] lin, Color c, float dbMin, float dbMax)
        {
            int w = Mathf.Max(2, Mathf.Min((int)r.width, n));
            if (_pts.Length != w) _pts = new Vector3[w];

            for (int p = 0; p < w; p++)
            {
                int i = (w <= 1) ? 0 : Mathf.RoundToInt((float)p / (w - 1) * (n - 1));
                float db = AcousticHistory.ToDb(lin[AcousticHistory.Index(i)]);
                float y = r.y + r.height * (1f - Mathf.InverseLerp(dbMin, dbMax, db));
                _pts[p] = new Vector3(r.x + r.width * ((w <= 1) ? 0f : (float)p / (w - 1)), y, 0f);
            }
            Handles.color = c;
            Handles.DrawAAPolyLine(1.6f, _pts);
        }

        private void DrawFrameDetail(int k)
        {
            int mark = AcousticHistory.Mark[k];
            if (mark == AcousticHistory.MarkJump)
                EditorGUILayout.HelpBox("★このフレームで跳びを検出しています。\n"
                    + "設計の第一制約は連続性なので、ここは原因を見る価値があります。", MessageType.Warning);

            EditorGUILayout.LabelField(
                "FPS " + AcousticHistory.Fps[k].ToString("F0")
                + "    音響計算 " + AcousticHistory.AcousticMs[k].ToString("F2") + " ms/f"
                + "    音源 " + AcousticHistory.SrcCount[k], EditorStyles.miniBoldLabel);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("出力の段別", EditorStyles.boldLabel);
            DbBar("出力", AcousticHistory.RmsOut[k]);
            DbBar("直接", AcousticHistory.RmsDirect[k]);
            DbBar("早期", AcousticHistory.RmsEarly[k]);
            DbBar("散乱", AcousticHistory.RmsScatter[k]);
            DbBar("尾", AcousticHistory.RmsTail[k]);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("部屋と残響", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("  RT60 " + AcousticHistory.RoomRt60[k].ToString("F2") + " s"
                + "    体積 " + AcousticHistory.RoomVolume[k].ToString("F0") + " m3"
                + "    wet " + AcousticHistory.Wet[k].ToString("F2")
                + "    mixing " + AcousticHistory.MixingMs[k].ToString("F0") + " ms", EditorStyles.miniLabel);
            EditorGUILayout.LabelField("  回折δ "
                + (AcousticHistory.DiffDelta[k] < 0f ? "（迂回路なし）"
                                                     : AcousticHistory.DiffDelta[k].ToString("F2") + " m")
                + "    直線透過 " + AcousticHistory.SourceLevel[k].ToString("F3")
                + "    タップ " + AcousticHistory.TapCount[k]
                + "（反射 " + AcousticHistory.ErActive[k] + " / 回折 " + AcousticHistory.DiffActive[k] + "）",
                EditorStyles.miniLabel);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("主音源の 6 帯域（125 / 250 / 500 / 1k / 2k / 4k Hz）", EditorStyles.boldLabel);
            int bb = k * AcousticHistory.NumBands;
            BandRow("透過", AcousticHistory.BandsTx, bb);
            BandRow("回折", AcousticHistory.BandsDf, bb);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("音源ごとの遮蔽（1 = 完全に遮られている）", EditorStyles.boldLabel);
            int ob = k * AcousticHistory.MaxSources;
            int ns = Mathf.Min(AcousticHistory.SrcCount[k], AcousticHistory.MaxSources);
            for (int i = 0; i < ns; i++)
            {
                string name = (AcousticHistory.SourceNames != null && i < AcousticHistory.SourceNames.Length)
                    ? AcousticHistory.SourceNames[i] : ("音源" + i);
                Bar01(name, AcousticHistory.Occ[ob + i],
                      Color.Lerp(new Color(0.35f, 0.9f, 0.45f), new Color(1f, 0.4f, 0.35f),
                                 AcousticHistory.Occ[ob + i]),
                      "生存 " + AcousticHistory.Surv[ob + i].ToString("F2"));
            }
        }

        private void DbBar(string label, float lin)
        {
            float db = AcousticHistory.ToDb(lin);
            Bar01(label, Mathf.InverseLerp(-80f, 0f, db), new Color(0.55f, 0.75f, 1f), db.ToString("F1") + " dB");
        }

        private void BandRow(string label, float[] arr, int baseIdx)
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("  " + label, GUILayout.Width(40f));
                for (int b = 0; b < AcousticHistory.NumBands; b++)
                {
                    float v = arr[baseIdx + b];
                    Rect r = GUILayoutUtility.GetRect(10f, 16f, GUILayout.ExpandWidth(true));
                    EditorGUI.DrawRect(r, new Color(0.16f, 0.16f, 0.18f));
                    EditorGUI.DrawRect(new Rect(r.x, r.yMax - r.height * Mathf.Clamp01(v),
                                                r.width - 2f, r.height * Mathf.Clamp01(v)),
                                       new Color(0.5f, 0.8f, 1f, 0.85f));
                    GUI.Label(r, " " + v.ToString("F2"), EditorStyles.miniLabel);
                }
            }
        }

        private void Bar01(string label, float v01, Color c, string right)
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("  " + label, GUILayout.Width(150f));
                Rect r = GUILayoutUtility.GetRect(10f, 14f, GUILayout.ExpandWidth(true));
                EditorGUI.DrawRect(r, new Color(0.16f, 0.16f, 0.18f));
                EditorGUI.DrawRect(new Rect(r.x, r.y, r.width * Mathf.Clamp01(v01), r.height), c);
                GUILayout.Label(right, EditorStyles.miniLabel, GUILayout.Width(78f));
            }
        }

        // =====================================================================
        // 予算タブ（道具10）
        // =====================================================================
        private void DrawBudget()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に audio thread の実費を表示します。", MessageType.Info);
                return;
            }

            int len, num;
            AudioSettings.GetDSPBufferSize(out len, out num);
            int sr = IrConvolver.Scope.SampleRate > 0 ? IrConvolver.Scope.SampleRate : AudioSettings.outputSampleRate;
            float periodMs = (sr > 0) ? (len * 1000f / sr) : 0f;

            EditorGUILayout.LabelField("DSP ブロック " + len + " サンプル × " + num
                + "    " + sr + " Hz    周期 " + periodMs.ToString("F2") + " ms", EditorStyles.miniBoldLabel);

            // 実測の合計。推定（0.292ms × 本数）ではない ──
            // コストは「遮蔽された音源数」で効くので、本数からの推定は実際と数倍ずれる。
            float total = 0f;
            int active = 0;
            int maxIdx = Mathf.Min(IrConvolver.Scope.MeteredMax, IrConvolver.Scope.MaxMeteredSources - 1);
            for (int i = 0; i <= maxIdx; i++)
            {
                if (IrConvolver.Scope.BlockMs[i] <= 0f) continue;
                total += IrConvolver.Scope.BlockMs[i];
                active++;
            }

            float use = (periodMs > 0f) ? total / periodMs : 0f;
            Color barCol = (use < 0.5f) ? new Color(0.35f, 0.9f, 0.45f)
                         : (use < 0.8f) ? new Color(1f, 0.85f, 0.3f)
                                        : new Color(1f, 0.4f, 0.35f);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("audio thread の占有", EditorStyles.boldLabel);
            Bar01("実測 " + total.ToString("F3") + " ms/block", Mathf.Clamp01(use), barCol,
                  (use * 100f).ToString("F1") + " %");

            EditorGUILayout.Space(4f);
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("音源数の上限（実測）", GUILayout.Width(150f));
                _budget = EditorGUILayout.IntField(_budget, GUILayout.Width(48f));
                GUILayout.Label("本", GUILayout.Width(24f));
                GUILayout.FlexibleSpace();
            }
            int nSrc = (AcousticFlowSceneDemo.Status.SourceNames != null)
                ? AcousticFlowSceneDemo.Status.SourceNames.Length : 0;
            Bar01("いま鳴っている音源 " + nSrc + " / " + _budget,
                  (_budget > 0) ? Mathf.Clamp01((float)nSrc / _budget) : 0f,
                  (nSrc < _budget * 0.8f) ? new Color(0.35f, 0.9f, 0.45f) : new Color(1f, 0.4f, 0.35f),
                  active + " 本計測中");

            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("音源ごとの実費", EditorStyles.boldLabel);
            float worst = 0.0001f;
            for (int i = 0; i <= maxIdx; i++) worst = Mathf.Max(worst, IrConvolver.Scope.BlockMs[i]);
            for (int i = 0; i <= maxIdx; i++)
            {
                float ms = IrConvolver.Scope.BlockMs[i];
                if (ms <= 0f) continue;
                string name = (AcousticFlowSceneDemo.Status.SourceNames != null
                               && i < AcousticFlowSceneDemo.Status.SourceNames.Length)
                    ? AcousticFlowSceneDemo.Status.SourceNames[i] : ("音源" + i);
                float occ = (AcousticFlowSceneDemo.Status.Occlusion != null
                             && i < AcousticFlowSceneDemo.Status.Occlusion.Length)
                    ? AcousticFlowSceneDemo.Status.Occlusion[i] : 0f;
                Bar01(name, ms / worst, new Color(0.55f, 0.75f, 1f),
                      ms.ToString("F3") + " ms  遮" + occ.ToString("F2"));
            }

            EditorGUILayout.Space(6f);
            EditorGUILayout.HelpBox(
                "上限 36 本は実測（1 スレッドが飽和するまで。SYSTEM_BRIEF §6）。\n"
                + "★合計は推定ではなく実測です。コストは「遮蔽された音源数」で効くので、\n"
                + "　本数 × 0.292ms の推定は実際と数倍ずれます。",
                MessageType.None);
        }

        // =====================================================================
        // 音源タブ（道具A ソロ／ミュート・寄与度順の一覧）
        // =====================================================================
        private enum SortBy { OutRms, PathGain }
        private static readonly string[] kSortNames = { "実出力", "経路ゲイン" };

        private SortBy _sortBy = SortBy.OutRms;
        private int _selSource;
        private Vector2 _srcScroll;
        private bool _drawPaths = true;
        private readonly int[] _order = new int[IrConvolver.Solo.MaxSources];
        private readonly float[] _metric = new float[IrConvolver.Solo.MaxSources];

        private void DrawSources()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に、いま鳴っている音源を寄与度順に並べます。", MessageType.Info);
                return;
            }
            var taps = AcousticFlowSceneDemo.Status.Taps;
            string[] names = AcousticFlowSceneDemo.Status.SourceNames;
            int n = (names != null) ? names.Length : 0;
            if (n <= 0 || taps == null) { EditorGUILayout.HelpBox("音源がまだありません。", MessageType.Warning); return; }
            n = Mathf.Min(n, IrConvolver.Solo.MaxSources);

            // --- 並べ替えの基準 ---
            // ★2 つの意味がある。混ぜると読めなくなるので明示的に切り替える。
            //   実出力     … 実際に耳へ届いている量。マスキングの切り分けはこちら
            //   経路ゲイン … 届きやすさ。音源が鳴っていなくても出る（経路の良し悪しを見る用）
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                GUILayout.Label("並べ替え", EditorStyles.miniLabel, GUILayout.Width(50f));
                _sortBy = (SortBy)GUILayout.Toolbar((int)_sortBy, kSortNames,
                                                    EditorStyles.toolbarButton, GUILayout.Width(140f));
                GUILayout.FlexibleSpace();
                _drawPaths = GUILayout.Toggle(_drawPaths, "Scene に経路", EditorStyles.toolbarButton, GUILayout.Width(84f));
                if (GUILayout.Button("すべて解除", EditorStyles.toolbarButton, GUILayout.Width(70f)))
                { IrConvolver.Solo.Reset(); SceneView.RepaintAll(); }
            }

            // --- 経路単位のゲート ---
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("経路", GUILayout.Width(34f));
                IrConvolver.Solo.PassDirect = GUILayout.Toggle(IrConvolver.Solo.PassDirect, "直接 D", "Button", GUILayout.Width(60f));
                IrConvolver.Solo.PassReflect = GUILayout.Toggle(IrConvolver.Solo.PassReflect, "反射 R", "Button", GUILayout.Width(60f));
                IrConvolver.Solo.PassDiffract = GUILayout.Toggle(IrConvolver.Solo.PassDiffract, "回折 F", "Button", GUILayout.Width(60f));
                IrConvolver.Solo.PassTail = GUILayout.Toggle(IrConvolver.Solo.PassTail, "尾", "Button", GUILayout.Width(44f));
                GUILayout.FlexibleSpace();
            }
            if (!IrConvolver.Solo.IsDefault)
                EditorGUILayout.HelpBox("いま経路を切っています。窓を閉じれば自動で戻ります。", MessageType.Warning);

            // --- 寄与度を測って並べる ---
            float worst = 1e-6f;
            for (int i = 0; i < n; i++)
            {
                _order[i] = i;
                _metric[i] = (_sortBy == SortBy.OutRms) ? IrConvolver.Scope.OutRms[i] : PathGain(taps, i);
                worst = Mathf.Max(worst, _metric[i]);
            }
            for (int a = 0; a < n - 1; a++)          // 音源は多くて数十本なので単純な選択ソートで足りる
                for (int b = a + 1; b < n; b++)
                    if (_metric[_order[b]] > _metric[_order[a]])
                    { int t = _order[a]; _order[a] = _order[b]; _order[b] = t; }

            EditorGUILayout.Space(4f);
            _srcScroll = EditorGUILayout.BeginScrollView(_srcScroll, GUILayout.Height(180f));
            for (int r = 0; r < n; r++)
            {
                int i = _order[r];
                bool sel = (i == _selSource);
                using (new EditorGUILayout.HorizontalScope(sel ? EditorStyles.helpBox : GUIStyle.none))
                {
                    bool solo = (IrConvolver.Solo.Only == i);
                    bool newSolo = GUILayout.Toggle(solo, "S", "Button", GUILayout.Width(22f));
                    if (newSolo != solo) { IrConvolver.Solo.Only = newSolo ? i : -1; SceneView.RepaintAll(); }

                    IrConvolver.Solo.Mute[i] = GUILayout.Toggle(IrConvolver.Solo.Mute[i], "M", "Button", GUILayout.Width(22f));

                    string nm = (names[i] != null) ? names[i] : ("音源" + i);
                    if (GUILayout.Button(nm, EditorStyles.label, GUILayout.Width(120f)))
                    { _selSource = i; SceneView.RepaintAll(); }

                    Rect br = GUILayoutUtility.GetRect(10f, 14f, GUILayout.ExpandWidth(true));
                    EditorGUI.DrawRect(br, new Color(0.16f, 0.16f, 0.18f));
                    float v = Mathf.Clamp01(_metric[i] / worst);
                    EditorGUI.DrawRect(new Rect(br.x, br.y, br.width * v, br.height),
                                       IrConvolver.Solo.AllowsSource(i) ? new Color(0.55f, 0.75f, 1f)
                                                                        : new Color(0.4f, 0.4f, 0.45f));
                    GUILayout.Label(AcousticHistory.ToDb(_metric[i]).ToString("F1") + " dB",
                                    EditorStyles.miniLabel, GUILayout.Width(56f));
                    GUILayout.Label("×" + ((i < taps.Length && taps[i] != null) ? taps[i].Count : 0),
                                    EditorStyles.miniLabel, GUILayout.Width(32f));
                }
            }
            EditorGUILayout.EndScrollView();

            // --- 選んだ音源のタップ内訳 ---
            EditorGUILayout.Space(4f);
            _selSource = Mathf.Clamp(_selSource, 0, n - 1);
            var ts = (_selSource < taps.Length) ? taps[_selSource] : null;
            if (ts == null) { EditorGUILayout.HelpBox("この音源のタップがまだありません。", MessageType.Info); return; }

            EditorGUILayout.LabelField(
                ((names[_selSource] != null) ? names[_selSource] : ("音源" + _selSource))
                + " のタップ内訳（" + ts.Count + " 本）"
                + "   ITDG " + ts.ItdgMs.ToString("F1") + " ms", EditorStyles.boldLabel);

            _scroll = EditorGUILayout.BeginScrollView(_scroll);
            for (int i = 0; i < ts.Count && i < AcousticFlowSceneDemo.SourceTaps.MaxTaps; i++)
            {
                char ty = (ts.Type != null && i < ts.Type.Length) ? ts.Type[i] : 'D';
                bool passed = IrConvolver.Solo.AllowsType(ty);
                using (new EditorGUILayout.HorizontalScope())
                {
                    var st = new GUIStyle(EditorStyles.miniLabel);
                    if (!passed) st.normal.textColor = new Color(0.5f, 0.5f, 0.55f);
                    GUILayout.Label(TypeLabel(ty), st, GUILayout.Width(52f));
                    GUILayout.Label(ts.DelayMs[i].ToString("F1") + " ms", st, GUILayout.Width(58f));
                    GUILayout.Label(AcousticHistory.ToDb(ts.Gain[i]).ToString("F1") + " dB", st, GUILayout.Width(58f));
                    for (int b = 0; b < AcousticHistory.NumBands; b++)
                    {
                        Rect r = GUILayoutUtility.GetRect(6f, 13f, GUILayout.ExpandWidth(true));
                        EditorGUI.DrawRect(r, new Color(0.16f, 0.16f, 0.18f));
                        int o = i * AcousticHistory.NumBands + b;
                        float g = (ts.BandGain != null && o < ts.BandGain.Length) ? Mathf.Clamp01(ts.BandGain[o]) : 0f;
                        EditorGUI.DrawRect(new Rect(r.x, r.yMax - r.height * g, r.width - 2f, r.height * g),
                                           passed ? new Color(0.5f, 0.8f, 1f, 0.85f) : new Color(0.45f, 0.45f, 0.5f, 0.6f));
                    }
                }
            }
            EditorGUILayout.EndScrollView();
        }

        private static string TypeLabel(char t)
        {
            if (t == 'R') return "反射 R";
            if (t == 'F') return "回折 F";
            return "直接 D";
        }

        private static float PathGain(AcousticFlowSceneDemo.SourceTaps[] taps, int i)
        {
            if (i >= taps.Length || taps[i] == null) return 0f;
            var ts = taps[i];
            float e = 0f;
            for (int k = 0; k < ts.Count && k < ts.Gain.Length; k++) e += ts.Gain[k] * ts.Gain[k];
            return Mathf.Sqrt(e);
        }

        /// <summary>選んだ音源の経路を Scene ビューへ描く（種別で色分け）。実行時コードは触らない。</summary>
        private void OnSceneGui(SceneView sv)
        {
            if (_tab != Tab.Sources || !_drawPaths || !EditorApplication.isPlaying) return;
            var taps = AcousticFlowSceneDemo.Status.Taps;
            if (taps == null || _selSource >= taps.Length || taps[_selSource] == null) return;

            var demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo == null || demo.listener == null) return;
            Vector3 ear = demo.listener.position;

            var ts = taps[_selSource];
            for (int i = 0; i < ts.Count && i < AcousticFlowSceneDemo.SourceTaps.MaxTaps; i++)
            {
                char ty = (ts.Type != null && i < ts.Type.Length) ? ts.Type[i] : 'D';
                if (!IrConvolver.Solo.AllowsType(ty)) continue;
                Vector3 p = (ts.Arrival != null && i < ts.Arrival.Length) ? ts.Arrival[i] : ear;

                // 直接=緑 / 反射=黄 / 回折=水色。強いタップほど濃く太く。
                float a = Mathf.Clamp01(Mathf.InverseLerp(-60f, 0f, AcousticHistory.ToDb(ts.Gain[i])));
                Handles.color = (ty == 'R') ? new Color(1f, 0.85f, 0.3f, 0.25f + 0.75f * a)
                              : (ty == 'F') ? new Color(0.2f, 0.9f, 1f, 0.25f + 0.75f * a)
                                            : new Color(0.4f, 1f, 0.5f, 0.35f + 0.65f * a);
                Handles.DrawAAPolyLine(1f + 3f * a, p, ear);
                Handles.SphereHandleCap(0, p, Quaternion.identity, 0.10f + 0.25f * a, EventType.Repaint);
            }
        }
    }
}
