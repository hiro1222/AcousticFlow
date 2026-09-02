// CaptureWindow.cs — 録れた .afcap を見る **Editor 窓**。
//
//   メニュー: AcousticFlow > キャプチャ (録音を見る)
//
// ★ゲーム内 F11 との違いは**見た目だけ**。中身は `CaptureBrowser`（ランタイム側）に
//   1 つだけ置いてある。写経すると必ずずれる。
//
//   | | ゲーム内 F11 | この窓 |
//   |---|---|---|
//   | 動く場所 | ビルドでも | Editor だけ |
//   | **再生中でなくても** | 使えない | **使える**（昨日のキャプチャを見返せる） |
//   | 画面 | Game ビューを覆う | ドッキング。塞がない |
//   | 音 | AudioSource で鳴らす | **WAV に書き出す** |
//
// ⚠ 録音そのもの（F10 / `CaptureRecorder`）は Editor 窓にできない。
//   `OnAudioFilterRead` は MonoBehaviour のコールバックで、EditorWindow は音を受け取れない。
using UnityEditor;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public class CaptureWindow : EditorWindow
    {
        private readonly CaptureBrowser _br = new CaptureBrowser();
        private Vector2 _fileScroll, _runScroll;
        private bool _openedOnce;

        [MenuItem("AcousticFlow/キャプチャ (録音を見る)")]
        public static void Open()
        {
            var w = GetWindow<CaptureWindow>("キャプチャ");
            w.minSize = new Vector2(560f, 480f);
            w.Show();
        }

        private void OnEnable() { if (!_openedOnce) { _br.RefreshFiles(); _openedOnce = true; } }
        private void OnDisable() { _br.Close(); }

        private void OnGUI()
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                if (GUILayout.Button("一覧を更新", EditorStyles.toolbarButton, GUILayout.Width(80f)))
                    _br.RefreshFiles();
                if (GUILayout.Button("フォルダを開く", EditorStyles.toolbarButton, GUILayout.Width(96f)))
                    EditorUtility.RevealInFinder(CaptureBrowser.CaptureDir);
                GUILayout.FlexibleSpace();
                GUILayout.Label(EditorApplication.isPlaying ? "再生中は F10 で印" : "再生していなくても見られます",
                                EditorStyles.miniLabel);
            }

            // ── ファイル一覧 ──
            EditorGUILayout.LabelField("キャプチャ（新しい順）", EditorStyles.boldLabel);
            _fileScroll = EditorGUILayout.BeginScrollView(_fileScroll, GUILayout.Height(96f));
            if (_br.Files.Count == 0)
                EditorGUILayout.HelpBox("まだありません。再生中に F10 を押すと落ちます。", MessageType.Info);
            for (int i = 0; i < _br.Files.Count; ++i)
            {
                bool sel = (_br.Selected == i);
                if (GUILayout.Toggle(sel, System.IO.Path.GetFileName(_br.Files[i]), "Button") && !sel)
                    _br.Open(i);
            }
            EditorGUILayout.EndScrollView();

            if (!_br.IsOpen)
            {
                if (!string.IsNullOrEmpty(_br.Note)) EditorGUILayout.HelpBox(_br.Note, MessageType.None);
                return;
            }

            // ── ヘッダの照合 ──
            //   ★「古い物を掴んだまま気づかない」がこのプロジェクトの弱点なので、
            //     録ったときの場面と今の場面が違えば**はっきり言う**。
            var info = _br.Info;
            string liveScene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
            EditorGUILayout.Space(4);
            if (info.SceneName != liveScene)
                EditorGUILayout.HelpBox(
                    string.Format("録ったのは「{0}」。いま開いているのは「{1}」です。\n"
                                  + "リプレイや検査の生成をするなら、録ったときと同じ場面を開いてください。",
                                  info.SceneName, liveScene), MessageType.Warning);
            else
                EditorGUILayout.LabelField("場面 " + info.SceneName + "（いまの場面と一致）",
                                           EditorStyles.boldLabel);

            EditorGUILayout.LabelField(string.Format(
                "{0} フレーム / 前 {1:0.0}s 後 {2:0.0}s / 並列 {3} / 箱 {4} / 材質 {5} / メッシュ {6} / PCM {7:0.00}s",
                info.Frames, info.Preroll / 60f, info.Postroll / 60f, info.WorkerThreads,
                info.BoxCount, info.MaterialCount, info.MeshCount, info.PcmSeconds));

            if (info.WorkerThreads > 1)
                EditorGUILayout.HelpBox(
                    "並列で録れています。診断の列は workerThreads = 1 でしか信用できません。\n"
                    + "⚠ ただし直列と並列は音響値がビット一致しないので、"
                    + "「直列で録ったキャプチャ」と「並列で動く本番」は厳密には別物です。",
                    MessageType.Info);
            if (info.MeshCount > 0)
                EditorGUILayout.HelpBox("メッシュを含む場面です。C++ の検査には吐けません。",
                                        MessageType.Info);

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("マスターPCM を WAV に書き出す", GUILayout.Height(22f)))
                {
                    string p = _br.ExportWav();
                    if (p != null) EditorUtility.RevealInFinder(p);
                }
            }

            // ── 印 ──
            EditorGUILayout.Space(4);
            EditorGUILayout.LabelField(
                string.Format("印 {0} 件 → まとめて {1} か所", _br.RawMarkCount, _br.Runs.Length),
                EditorStyles.boldLabel);
            if (_br.Runs.Length == 0)
                EditorGUILayout.HelpBox("型紙に触れたところはありませんでした。", MessageType.Info);

            _runScroll = EditorGUILayout.BeginScrollView(_runScroll);
            for (int i = 0; i < _br.Runs.Length; ++i)
            {
                var r = _br.Runs[i];
                using (new EditorGUILayout.HorizontalScope(EditorStyles.helpBox))
                {
                    // ★続いた長さを必ず出す。1 フレームの瞬きと、何秒も続くのとは意味が違う。
                    string span = (r.Count <= 1)
                        ? string.Format("f{0}  {1:0.00}s", r.FirstFrame, r.FirstSeconds)
                        : string.Format("f{0}..{1}  {2:0.00}s 続く", r.FirstFrame, r.LastFrame, r.Seconds);
                    EditorGUILayout.LabelField(span, GUILayout.Width(150f));
                    EditorGUILayout.LabelField("音源 " + r.SourceId, GUILayout.Width(70f));
                    EditorGUILayout.LabelField(r.TemplateName, GUILayout.Width(150f));
                    EditorGUILayout.LabelField(string.Format("{0} 最大 {1:0.000}", r.What, r.Worst),
                                               GUILayout.Width(140f));
                    using (new EditorGUI.DisabledScope(info.MeshCount > 0))
                        if (GUILayout.Button("検査を吐く", GUILayout.Width(80f)))
                        {
                            int raw = _br.RawIndexOfRun(i);
                            if (raw >= 0)
                            {
                                string p = _br.EmitCase(raw, info.SceneName);
                                if (p != null)
                                {
                                    Debug.Log("[AcousticFlow] 回帰テストを吐きました: " + p);
                                    EditorUtility.RevealInFinder(p);
                                }
                            }
                        }
                }
            }
            EditorGUILayout.EndScrollView();

            if (!string.IsNullOrEmpty(_br.Note)) EditorGUILayout.HelpBox(_br.Note, MessageType.None);
        }
    }
}
