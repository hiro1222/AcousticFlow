// BellGameMonitor.cs
// 検証中の数字を**別ウィンドウ**に出す。Game ビューの上に重ねない。
//
// ★なぜ画面から追い出すか
//   ゲーム画面に文字を敷き詰めると、**見ている絵が検証の邪魔をする**。
//   扉ごしの見え方を確かめている最中に、その扉が文字で隠れている状態だった。
//   EditorWindow なら別モニターへ投げられるし、ドッキングも自由。
//
// ★画面側は一行だけ残す。
//   全部消すと、ウィンドウを開き忘れたときに何も分からなくなる。
//   「いま何世界か・扉が何度か・60 出ているか」だけは絵の隅にあってよい。
using UnityEditor;
using UnityEngine;
using BellGame;

namespace BellGameEditor
{
    public sealed class BellGameMonitor : EditorWindow
    {
        [MenuItem("BellGame/モニター（別ウィンドウ）")]
        public static void Open()
        {
            var w = GetWindow<BellGameMonitor>("BellGame モニター");
            w.minSize = new Vector2(420f, 260f);
            w.Show();
        }

        private Vector2 _scroll, _srcScroll;
        private GUIStyle _mono;
        private AcousticFlow.AcousticFlowSceneDemo _demo;
        private bool _showSources = true;

        // ── 音源一覧 ──────────────────────────────────────────
        //
        // ★どのシーンでも出る。HalfWorldLab に頼らない。
        //   数字は AcousticFlowSceneDemo.Status（公開されている）から読むだけなので、
        //   システム側には何も足していません。
        //
        // ★**タップ数**を出すのが肝です。
        //   音源 1 本の費用はだいたいタップ数で決まります（経路の数）。
        //   合計 ms だけ見ていても「どの音源が重いか」は分からない ──
        //   1 本だけ 40 タップ持っている音源が居れば、そこが全部です。
        private void DrawSources()
        {
            _showSources = EditorGUILayout.Foldout(_showSources, "音源一覧", true);
            if (!_showSources) return;

            if (_demo == null)
                _demo = Object.FindFirstObjectByType<AcousticFlow.AcousticFlowSceneDemo>();

            var taps = AcousticFlow.AcousticFlowSceneDemo.Status.Taps;
            var occ = AcousticFlow.AcousticFlowSceneDemo.Status.Occlusion;
            var names = AcousticFlow.AcousticFlowSceneDemo.Status.SourceNames;

            if (_demo == null || taps == null)
            {
                EditorGUILayout.HelpBox("音響ホスト（AcousticFlowSceneDemo）が見つかりません。",
                                        MessageType.Info);
                return;
            }

            // 装置に繋がっている順に並べる。0 番＝あなた、1 番以降＝その世界の音。
            var list = new System.Collections.Generic.List<Transform>();
            if (_demo.source != null) list.Add(_demo.source);
            if (_demo.extraSources != null)
                foreach (var s in _demo.extraSources) if (s != null) list.Add(s);

            var ear = (_demo.listener != null) ? _demo.listener : _demo.transform;
            int totalTaps = 0;
            for (int i = 0; i < taps.Length; i++) if (taps[i] != null) totalTaps += taps[i].Count;

            EditorGUILayout.LabelField(
                $"{list.Count} 本 ／ タップ合計 {totalTaps} ／ 音響 {PerfMeter.AcousticMs:F2} ms"
                + (list.Count > 0 ? $"（1 本あたり {PerfMeter.AcousticMs / list.Count:F2} ms）" : ""),
                _mono);
            EditorGUILayout.LabelField("  #  名前            距離   左右   遮蔽   タップ  生存", _mono);

            _srcScroll = EditorGUILayout.BeginScrollView(_srcScroll, GUILayout.MaxHeight(220f));
            for (int i = 0; i < list.Count; i++)
            {
                var t = list[i];
                float d = Vector3.Distance(ear.position, t.position);

                string side = "—";
                var ts = (i < taps.Length) ? taps[i] : null;
                if (ts != null)
                {
                    float x = ts.DirectDirLocal.x;
                    side = (x > 0.05f) ? "右" : (x < -0.05f) ? "左" : "中";
                }

                float o = (occ != null && i < occ.Length) ? occ[i] : 0f;
                int nt = (ts != null) ? ts.Count : 0;
                float sv = (ts != null) ? ts.SourceLevel : 0f;
                string nm = (names != null && i < names.Length && !string.IsNullOrEmpty(names[i]))
                          ? names[i] : t.name;
                if (nm.Length > 14) nm = nm.Substring(0, 14);

                // ★タップが飛び抜けている音源は目で拾えるようにする。犯人はたいてい 1 本。
                var style = (nt > 24) ? new GUIStyle(_mono) { normal = { textColor = Color.yellow } }
                                      : _mono;
                EditorGUILayout.LabelField(
                    $"  {i,2}  {nm,-14} {d,5:F1}m  {side}    {o,4:F2}   {nt,4}   {sv,4:F2}", style);
            }
            EditorGUILayout.EndScrollView();

            EditorGUILayout.LabelField("  遮蔽 1=完全に遮られている ／ 生存 = 反射込みで残っている量", _mono);
            EditorGUILayout.LabelField("  ★黄色 = タップが多い（24超）。1 本の費用はほぼタップ数で決まります。", _mono);
        }

        // 再生中は毎フレーム描き直す。止まっているときは静かにしておく。
        private void Update() { if (EditorApplication.isPlaying) Repaint(); }

        private void OnGUI()
        {
            _mono ??= new GUIStyle(EditorStyles.label)
            { font = EditorStyles.miniFont, richText = false, wordWrap = false };

            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に数字が出ます。", MessageType.Info);
                return;
            }

            // ── 予算 ──────────────────────────────────────────
            EditorGUILayout.LabelField("演算の残り", EditorStyles.boldLabel);
            if (!PerfMeter.WorkMeasurable)
            {
                EditorGUILayout.HelpBox(PerfMeter.Note, MessageType.Warning);
                EditorGUILayout.LabelField($"{PerfMeter.Fps:F0} fps   フレーム間隔 {PerfMeter.FrameMs:F1} ms"
                                           + $"   音響 {PerfMeter.AcousticMs:F2} ms", _mono);
            }
            else
            {
                float used = Mathf.Clamp01(PerfMeter.WorkMs / Mathf.Max(0.01f, PerfMeter.BudgetMs));
                var r = EditorGUILayout.GetControlRect(false, 18f);
                EditorGUI.ProgressBar(r, used,
                    $"実働 {PerfMeter.WorkMs:F1} / {PerfMeter.BudgetMs:F1} ms   "
                    + $"余り {PerfMeter.FreeMs:F1} ms ({PerfMeter.FreePct:F0}%)");

                EditorGUILayout.LabelField(
                    $"{PerfMeter.Fps:F0} fps   GPU {PerfMeter.GpuMs:F1} ms   "
                    + $"うち音響 {PerfMeter.AcousticMs:F2} ms", _mono);

                // ★山のほうが大事。平均で足りていても、山で落ちるなら落ちる。
                float wf = PerfMeter.WorstFreeMs;
                EditorGUILayout.LabelField(
                    $"直近3秒の山: 実働 {PerfMeter.WorstWorkMs:F1} ms   余り {wf:F1} ms"
                    + (wf < 0f ? "   ★予算を超えている（ここで落ちる）" : ""), _mono);
            }

            EditorGUILayout.Space(6f);
            DrawSources();
            EditorGUILayout.Space(6f);

            // ── ラボの状態 ────────────────────────────────────
            EditorGUILayout.LabelField("扉の検証", EditorStyles.boldLabel);
            _scroll = EditorGUILayout.BeginScrollView(_scroll);
            EditorGUILayout.LabelField(
                string.IsNullOrEmpty(HalfWorldLab.Report) ? "（HalfWorldLab が居ません）"
                                                          : HalfWorldLab.Report, _mono);
            EditorGUILayout.EndScrollView();

            EditorGUILayout.Space(4f);
            if (GUILayout.Button("いまの状態をコンソールへ（F1 と同じ）"))
                HalfWorldLab.DumpRequested = true;
        }
    }
}
