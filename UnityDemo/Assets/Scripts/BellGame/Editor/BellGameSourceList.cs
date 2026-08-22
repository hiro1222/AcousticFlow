// BellGameSourceList.cs
// 登録されている全音源の一覧と、選んだ 1 本の詳細。
//
// ★これが「音のデバッグ台」の本体です。
//   いままでの窓は仕組みごと（遮蔽・回折・開口）に分かれていて、
//   **「この音源が、いま、なぜこう聞こえているか」**を追う場がありませんでした。
//   一覧で当たりを付けて、選んで、中を開く ── その順で辿れるようにします。
//
// ★並べ方を 2 つ持つ理由
//     登録順      … 構成を確かめるとき。並びが変わらないので目が迷わない
//     ラウドネス順 … **いま何が鳴っているか**を知りたいとき。上から順に犯人が来る
//   「音が変」を追うときは後者、「置き忘れ」を探すときは前者です。
//
// ★ラウドネスは**リスナー到達エネルギー**（発注者の指定）。
//   AudioSource の音量ではありません。経路を全部通った後、耳に届く量です。
//   Status.Taps の帯域ゲインを全経路ぶん足して出します。
//
// ★数字は AcousticFlowSceneDemo.Status（公開）から読むだけ。
//   システム側には何も足していません。
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;
using AcousticFlow;
using BellGame;

namespace BellGameEditor
{
    public sealed class BellGameSourceList : EditorWindow
    {
        [MenuItem("BellGame/音の道具/音源リスト")]
        public static void Open()
        {
            var w = GetWindow<BellGameSourceList>("音源リスト");
            w.minSize = new Vector2(420f, 380f);
        }

        private enum SortBy { Registered, Loudness }

        private SortBy _sort = SortBy.Loudness;
        private int _selected = -1;              // 装置に繋がっている並びでの index
        private Vector2 _listScroll, _detailScroll;
        private AcousticFlowSceneDemo _demo;
        private GUIStyle _mono;

        // 一覧に出す 1 行ぶん。毎フレーム作り直すので構造体で。
        private struct Row
        {
            public int index;                    // 装置での index（0 = あなた）
            public string name;
            public float loudness;               // リスナー到達エネルギー（線形）
            public bool spatialized;             // 音響エンジンが経路を解いているか
            public float distance;
        }

        private readonly List<Row> _rows = new();

        private void Update() { if (EditorApplication.isPlaying) Repaint(); }

        private void OnGUI()
        {
            _mono ??= new GUIStyle(EditorStyles.label) { font = EditorStyles.miniFont };

            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に一覧が出ます。", MessageType.Info);
                return;
            }
            if (_demo == null) _demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (_demo == null)
            {
                EditorGUILayout.HelpBox("音響ホスト（AcousticFlowSceneDemo）が見つかりません。",
                                        MessageType.Warning);
                return;
            }

            Gather();
            DrawToolbar();
            DrawSaveBar();
            DrawList();
            DrawDetail();
        }

        // ── 収集 ──────────────────────────────────────────────
        private void Gather()
        {
            _rows.Clear();
            var taps = AcousticFlowSceneDemo.Status.Taps;
            var ear = (_demo.listener != null) ? _demo.listener : _demo.transform;

            var srcs = LiveSources();     // ★作り方は 1 箇所だけ（決めごと #1）

            for (int i = 0; i < srcs.Count; i++)
            {
                var t = srcs[i];
                float loud = 0f;
                if (taps != null && i < taps.Length && taps[i] != null)
                {
                    var ts = taps[i];
                    for (int k = 0; k < ts.Count; k++)
                        for (int b = 0; b < 6; b++) loud += ts.BandGain[k * 6 + b];
                }
                // ★2D / 3D は Unity の spatialBlend では決まりません。
                //   このプロジェクトは全部 spatialBlend=0 で、空間化は畳み込み器の担当です。
                //   ここでは「音響エンジンが経路を解いているか」で分けます。
                bool spatial = t.GetComponent<VoiceConvolver>() != null
                            || t.GetComponent<IrConvolver>() != null;

                _rows.Add(new Row
                {
                    index = i,
                    name = t.name,
                    loudness = loud,
                    spatialized = spatial,
                    distance = (ear != null) ? Vector3.Distance(ear.position, t.position) : 0f,
                });
            }

            if (_sort == SortBy.Loudness)
                _rows.Sort((a, b) => b.loudness.CompareTo(a.loudness));
        }

        // ── 上の帯 ────────────────────────────────────────────
        private void DrawToolbar()
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                EditorGUILayout.LabelField($"{_rows.Count} 本", EditorStyles.miniLabel,
                                           GUILayout.Width(48f));
                GUILayout.FlexibleSpace();
                EditorGUILayout.LabelField("並べ方", EditorStyles.miniLabel, GUILayout.Width(40f));
                if (GUILayout.Toggle(_sort == SortBy.Registered, "登録順",
                                     EditorStyles.toolbarButton, GUILayout.Width(60f)))
                    _sort = SortBy.Registered;
                if (GUILayout.Toggle(_sort == SortBy.Loudness, "ラウドネス順",
                                     EditorStyles.toolbarButton, GUILayout.Width(88f)))
                    _sort = SortBy.Loudness;
            }
        }

        // ── 保存・読み込み ────────────────────────────────────
        //
        // ★詳細エディタで触った値は**再生を止めると消えます**。
        //   耳で合わせた状態へ戻れるように、CSV へ落とせるようにしました。
        //   設定（往復する）と測定（書き出すだけ）を**分けてあります** ── 理由は
        //   BellGameSourceSettings.cs の頭に書いています。
        private void DrawSaveBar()
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                var srcs = LiveSources();
                using (new EditorGUI.DisabledScope(srcs.Count == 0))
                {
                    if (GUILayout.Button("設定を保存", EditorStyles.toolbarButton, GUILayout.Width(80f)))
                        Debug.Log("[BellGame] " + BellGameSourceSettings.Save(srcs));

                    if (GUILayout.Button("設定を読み込む", EditorStyles.toolbarButton, GUILayout.Width(96f)))
                        Debug.Log("[BellGame] " + BellGameSourceSettings.Load(srcs));

                    GUILayout.FlexibleSpace();

                    if (GUILayout.Button("測定を Excel へ", EditorStyles.toolbarButton, GUILayout.Width(104f)))
                        Debug.Log("[BellGame] " + BellGameSourceSettings.Export(srcs));
                }
            }
        }

        /// 装置に繋がっている音源を、**一覧と同じ並び**で返す。
        ///   ★並びが違うと、測定の書き出しが別の音源の行に乗ります。
        private List<Transform> LiveSources()
        {
            var srcs = new List<Transform>();
            if (_demo == null) return srcs;
            if (_demo.source != null) srcs.Add(_demo.source);
            if (_demo.extraSources != null)
                foreach (var s in _demo.extraSources) if (s != null) srcs.Add(s);
            return srcs;
        }

        // ── 一覧 ──────────────────────────────────────────────
        //
        // ★選択中は**地の色に薄く白を混ぜる**（発注者の指定）。
        //   枠線や矢印だと、行が増えたときに目が探しに行く必要があります。
        //   面で持ち上げれば、視線を動かさずに「いまここ」が分かります。
        private void DrawList()
        {
            float max = 0.0001f;
            foreach (var r in _rows) if (r.loudness > max) max = r.loudness;

            _listScroll = EditorGUILayout.BeginScrollView(_listScroll, GUILayout.MinHeight(150f));
            foreach (var r in _rows)
            {
                var rect = EditorGUILayout.GetControlRect(false, 20f);
                bool sel = r.index == _selected;

                if (sel) EditorGUI.DrawRect(rect, new Color(1f, 1f, 1f, 0.10f));
                else if ((r.index & 1) == 1) EditorGUI.DrawRect(rect, new Color(1f, 1f, 1f, 0.02f));

                // ラウドネスの帯。数字より先に「どれが大きいか」が目に入る。
                var bar = new Rect(rect.x + rect.width * 0.55f, rect.y + 5f,
                                   (rect.width * 0.28f) * Mathf.Clamp01(r.loudness / max), 10f);
                EditorGUI.DrawRect(bar, sel ? new Color(0.55f, 0.78f, 0.95f, 0.85f)
                                            : new Color(0.42f, 0.58f, 0.72f, 0.65f));

                string db = (r.loudness > 1e-6f)
                          ? $"{20f * Mathf.Log10(r.loudness):F0}dB" : "  —  ";
                GUI.Label(new Rect(rect.x + 4f, rect.y, rect.width * 0.52f, rect.height),
                          $"{r.index,2}  {r.name}", _mono);
                GUI.Label(new Rect(rect.x + rect.width * 0.84f, rect.y, rect.width * 0.16f, rect.height),
                          $"{db}  {(r.spatialized ? "3D" : "2D")}", _mono);

                if (Event.current.type == EventType.MouseDown && rect.Contains(Event.current.mousePosition))
                { _selected = r.index; Event.current.Use(); Repaint(); }
            }
            EditorGUILayout.EndScrollView();
        }

        // ── 詳細 ──────────────────────────────────────────────
        private void DrawDetail()
        {
            if (_selected < 0) { EditorGUILayout.HelpBox("行をクリックすると詳細が出ます。", MessageType.None); return; }

            var srcs = LiveSources();     // ★作り方は 1 箇所だけ（決めごと #1）
            if (_selected >= srcs.Count) { _selected = -1; return; }

            var t = srcs[_selected];
            var taps = AcousticFlowSceneDemo.Status.Taps;
            var ts = (taps != null && _selected < taps.Length) ? taps[_selected] : null;

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField($"■ {t.name}", EditorStyles.boldLabel);

            _detailScroll = EditorGUILayout.BeginScrollView(_detailScroll);

            // 触れる設定。⚠ 再生を止めると消えます（保存は下の注記）。
            var a = t.GetComponent<AudioSource>();
            if (a != null)
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    EditorGUILayout.LabelField("音量", GUILayout.Width(60f));
                    a.volume = EditorGUILayout.Slider(a.volume, 0f, 1f);
                    a.mute = EditorGUILayout.ToggleLeft("消音", a.mute, GUILayout.Width(50f));
                }
                EditorGUILayout.LabelField($"　クリップ {(a.clip != null ? a.clip.name : "—")}"
                    + $"　{(a.isPlaying ? "再生中" : "停止")}", _mono);

            // ── 段 ────────────────────────────────────────────
            //
            // ★費用で決めず、耳で決めること。
            //   ここで切り替えると**その場で音が変わります**。
            //   「軽くなったか」ではなく「変わったと気づくか」で判断してください。
            DrawTier(t);
            }

            if (ts == null || ts.Count <= 0)
            {
                EditorGUILayout.HelpBox("この音源はまだ経路が解かれていません。", MessageType.None);
                EditorGUILayout.EndScrollView();
                return;
            }

            var occ = AcousticFlowSceneDemo.Status.Occlusion;
            float o = (occ != null && _selected < occ.Length) ? occ[_selected] : 0f;
            EditorGUILayout.LabelField(
                $"遮蔽 {o:F2}　生存 {ts.SourceLevel:F2}　自由音場 {ts.FreeFieldDirect:F3}"
                + $"　経路 {ts.Count} 本", _mono);

            // 直接音の 6 帯域 ＝ 透過込みで残っている量。
            EditorGUILayout.Space(2f);
            EditorGUILayout.LabelField("直接音の 6 帯域（1=素通り / 0=遮断）", EditorStyles.miniBoldLabel);
            DrawBands(ts.BandGain, 0);

            // 回折。何本立っていて、いちばん早いのがどれか。
            int nF = 0, best = -1; float bestMs = float.MaxValue;
            for (int k = 1; k < ts.Count && ts.Type != null && k < ts.Type.Length; k++)
            {
                if (ts.Type[k] != 'F') continue;
                nF++;
                if (ts.DelayMs[k] < bestMs) { bestMs = ts.DelayMs[k]; best = k; }
            }
            EditorGUILayout.Space(2f);
            EditorGUILayout.LabelField(
                $"回折 {nF} 本" + (best >= 0 ? $"　最短 {bestMs:F1}ms" : "")
                + (ts.HrtfTapIndex >= 0 ? $"　HRTF に載せているのは #{ts.HrtfTapIndex}" : ""),
                EditorStyles.miniBoldLabel);
            if (best >= 0) DrawBands(ts.BandGain, best * 6);

            // 経路の一覧。どこから何が来ているかを全部出す。
            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("経路（種別 / 遅延 / 左右 / 6 帯域）", EditorStyles.miniBoldLabel);
            for (int k = 0; k < ts.Count; k++)
            {
                char ty = (ts.Type != null && k < ts.Type.Length) ? ts.Type[k] : '?';
                string label = ty == 'D' ? "直接" : ty == 'F' ? "回折" : ty == 'R' ? "反射" : "?";
                float pan = (ts.PanR != null && k < ts.PanR.Length)
                          ? ts.PanR[k] - ts.PanL[k] : 0f;
                EditorGUILayout.LabelField(
                    string.Format("  #{0,-2} {1}  {2,6:F1}ms  {3}", k, label, ts.DelayMs[k],
                                  pan > 0.08f ? "右" : pan < -0.08f ? "左" : "中"), _mono);
            }

            EditorGUILayout.Space(4f);
            EditorGUILayout.HelpBox(
                "⚠ ここで触った値は再生を止めると消えます。保存はまだ入れていません。\n"
                + "保存するなら、名前ごとの設定を資産に書き出して起動時に当てる形になります"
                + "（再生中の変更は Unity が捨てるため、その形しかありません）。",
                MessageType.None);

            EditorGUILayout.EndScrollView();
        }

        /// 6 帯域を横棒で。数字だけだと傾きが読み取れないので、形を先に見せる。

        // ── 段の選択 ──────────────────────────────────────────
        //
        // ★札が無い音源は「厳密」です（エンジンの既定）。
        //   簡易やバーチャルを選んだ瞬間に札を貼ります ── 何もしていない音源に
        //   札が生えると、後で読む人が「意図して厳密にした」と読み違えるので。
        private void DrawTier(Transform t)
        {
            // ★係が居ないと、段を選んでも**エンジンには何も届きません**。
            //   黙って効かないのがいちばん困るので、ここで言います。
            if (Object.FindFirstObjectByType<SourceTierApplier>() == null)
            {
                EditorGUILayout.HelpBox("段を渡す係がこのシーンに居ません。"
                    + "選んでもエンジンには届きません。", MessageType.Warning);
                if (GUILayout.Button("段の係を置く"))
                {
                    var go = new GameObject("SourceTierApplier");
                    go.AddComponent<SourceTierApplier>();
                    Undo.RegisterCreatedObjectUndo(go, "段の係を置く");
                }
                return;
            }

            var tag = t.GetComponent<SourceTierTag>();
            var cur = (tag != null) ? tag.tier : SourceTierTag.Tier.Exact;

            EditorGUILayout.Space(2f);
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("段", GUILayout.Width(60f));

                var next = cur;
                if (GUILayout.Toggle(cur == SourceTierTag.Tier.Exact, "厳密",
                                     EditorStyles.miniButtonLeft)) next = SourceTierTag.Tier.Exact;
                if (GUILayout.Toggle(cur == SourceTierTag.Tier.Simple, "簡易",
                                     EditorStyles.miniButtonMid)) next = SourceTierTag.Tier.Simple;
                if (GUILayout.Toggle(cur == SourceTierTag.Tier.Virtual, "バーチャル",
                                     EditorStyles.miniButtonRight)) next = SourceTierTag.Tier.Virtual;

                if (next != cur)
                {
                    if (tag == null) tag = Undo.AddComponent<SourceTierTag>(t.gameObject);
                    Undo.RecordObject(tag, "段を変える");
                    tag.tier = next;
                    Reapply();
                }
            }

            if (tag != null && tag.tier == SourceTierTag.Tier.Virtual)
            {
                float r = EditorGUILayout.Slider("可聴半径(m)", tag.audibleRadius, 0f, 200f);
                if (!Mathf.Approximately(r, tag.audibleRadius))
                {
                    Undo.RecordObject(tag, "可聴半径を変える");
                    tag.audibleRadius = r;
                    Reapply();
                }
                EditorGUILayout.LabelField("　★響く部屋では思ったより遠くまで聞こえます。"
                    + "小さくしすぎると**聞こえるはずの音が消えます**。", EditorStyles.miniLabel);
            }

            // ★指定と、エンジンが実際に使っている段は違うことがあります
            //  （バーチャルは半径の外に出たときだけ効くので、近づけば厳密に戻る）。
            //   食い違いが見えないと「バーチャルにしたのに軽くならない」を追えません。
            if (tag != null && !string.IsNullOrEmpty(tag.effective) && tag.effective != "—")
                EditorGUILayout.LabelField($"　いまエンジンが使っている段: {tag.effective}", _mono);

            if (cur == SourceTierTag.Tier.Simple)
                EditorGUILayout.HelpBox("簡易では回り込みの**方向**が出ません"
                    + "（壁の向こうの音が壁の中から鳴る）。近接感も落ちます。",
                    MessageType.None);
        }

        private void Reapply()
        {
            foreach (var ap in Object.FindObjectsByType<SourceTierApplier>(
                         FindObjectsInactive.Include, FindObjectsSortMode.None))
                ap.Reapply();
        }
        private void DrawBands(float[] g, int offset)
        {
            if (g == null || offset + 6 > g.Length) return;
            string[] names = { "125", "250", "500", " 1k", " 2k", " 4k" };
            for (int b = 0; b < 6; b++)
            {
                var rect = EditorGUILayout.GetControlRect(false, 13f);
                float v = Mathf.Clamp01(g[offset + b]);
                EditorGUI.DrawRect(new Rect(rect.x + 34f, rect.y + 2f,
                                            (rect.width - 90f) * v, 9f),
                                   new Color(0.45f, 0.72f, 0.52f, 0.8f));
                GUI.Label(new Rect(rect.x, rect.y, 34f, rect.height), names[b], _mono);
                GUI.Label(new Rect(rect.xMax - 52f, rect.y, 52f, rect.height),
                          g[offset + b].ToString("F3"), _mono);
            }
        }
    }
}
