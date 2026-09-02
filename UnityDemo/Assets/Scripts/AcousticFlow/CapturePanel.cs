// CapturePanel.cs ── 録れた .afcap を見る **ゲーム内**パネル（F11）。
//
// ★中身は持たない。`CaptureBrowser`（同じアセンブリ）に 1 つだけ置いてある。
//   Editor 窓版（AcousticFlow / キャプチャ (録音を見る)）も同じものを使う。
//   見せ方が 2 つあるとき写経すると必ずずれる（決めごと #1）。
//
// ★このパネルを残す理由
//   Editor 窓のほうが読みやすいが、**ビルドでは Editor 窓が無い**。
//   実機で録って実機で確かめたいときに要る。
//
// ★走査は C# で書いていない。DLL の中の関数を呼ぶ。
//   回帰テストが呼ぶのと同じ関数でないと「Unity では印が出るのに検査は通る」が起きる。
using System.IO;
using UnityEngine;

namespace AcousticFlow
{
    [AddComponentMenu("AcousticFlow/Capture Panel")]
    public class CapturePanel : MonoBehaviour
    {
        [Tooltip("パネルの開閉キー。\n\n"
                 + "Editor では AcousticFlow / キャプチャ (録音を見る) のほうが読みやすく、"
                 + "再生していなくても見られます。")]
        public KeyCode toggleKey = KeyCode.F11;

        public bool open = false;

        private readonly CaptureBrowser _br = new CaptureBrowser();
        private Vector2 _fileScroll, _runScroll;
        private AudioSource _player;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Attach()
        {
            if (FindFirstObjectByType<CapturePanel>() != null) return;
            var demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo != null) demo.gameObject.AddComponent<CapturePanel>();
        }

        private void OnDisable() { _br.Close(); }

        private void Update()
        {
            if (!Input.GetKeyDown(toggleKey)) return;
            open = !open;
            if (open) _br.RefreshFiles();
        }

        /// マスターPCM を鳴らす（「実際に何が聞こえたか」の物証）。
        /// ⚠ エンジンを直しても、これは変わらない。そこが値打ち。
        private void PlayMasterPcm()
        {
            float[] pcm;
            int got = _br.ReadPcm(out pcm);
            if (got <= 0) return;
            var clip = AudioClip.Create("afcap", got, 2, _br.Info.SampleRate, false);
            clip.SetData(pcm, 0);
            if (_player == null)
            {
                _player = gameObject.AddComponent<AudioSource>();
                _player.spatialBlend = 0f;      // そのまま鳴らす（空間化しない）
                _player.bypassEffects = true;
                _player.bypassListenerEffects = true;
            }
            _player.clip = clip;
            _player.Play();
        }

        private void OnGUI()
        {
            if (!open) return;
            const float kPad = 20f, kMaxW = 720f, kMaxH = 500f;
            float w = Mathf.Min(kMaxW, Screen.width - kPad * 2f);
            float h = Mathf.Min(kMaxH, Screen.height - kPad * 2f);
            var area = new Rect(kPad, kPad, w, h);
            GUI.Box(area, "キャプチャ (" + toggleKey + " で閉じる)");
            GUILayout.BeginArea(new Rect(area.x + 10f, area.y + 24f, area.width - 20f, area.height - 34f));

            GUILayout.BeginHorizontal();
            if (GUILayout.Button("一覧を更新", GUILayout.Width(100f))) _br.RefreshFiles();
            GUILayout.Label(CaptureBrowser.CaptureDir);
            GUILayout.EndHorizontal();

            // ── ファイル一覧 ──
            _fileScroll = GUILayout.BeginScrollView(_fileScroll, GUILayout.Height(100f));
            if (_br.Files.Count == 0)
                GUILayout.Label("まだありません（プレイ中に F10 を押すと落ちます）");
            for (int i = 0; i < _br.Files.Count; ++i)
            {
                bool sel = (_br.Selected == i);
                if (GUILayout.Toggle(sel, Path.GetFileName(_br.Files[i]), "Button") && !sel)
                    _br.Open(i);
            }
            GUILayout.EndScrollView();

            if (_br.IsOpen)
            {
                var info = _br.Info;
                // ★録ったときの場面と今の場面が違えば赤く言う
                //   （「古い物を掴んだまま気づかない」の対策）。
                string liveScene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
                bool match = (info.SceneName == liveScene);
                var old = GUI.color;
                GUI.color = match ? Color.white : new Color(1f, 0.45f, 0.45f);
                GUILayout.Label("場面: " + info.SceneName
                                + (match ? "" : "   ← いま開いている場面（" + liveScene + "）と違います"));
                GUI.color = old;

                GUILayout.Label(string.Format(
                    "{0} フレーム / 前{1:0.0}s 後{2:0.0}s / 並列 {3} / 箱 {4} / 材質 {5} / メッシュ {6} / PCM {7:0.00}s",
                    info.Frames, info.Preroll / 60f, info.Postroll / 60f, info.WorkerThreads,
                    info.BoxCount, info.MaterialCount, info.MeshCount, info.PcmSeconds));
                if (info.WorkerThreads > 1)
                    GUILayout.Label("⚠ 並列で録れています。診断の列は workerThreads=1 でしか信用できません。");
                if (info.MeshCount > 0)
                    GUILayout.Label("⚠ メッシュを含む場面です。C++ の検査には吐けません。");

                GUILayout.BeginHorizontal();
                if (GUILayout.Button("マスターPCM を鳴らす", GUILayout.Width(180f))) PlayMasterPcm();
                if (_player != null && _player.isPlaying && GUILayout.Button("止める", GUILayout.Width(80f)))
                    _player.Stop();
                if (GUILayout.Button("WAV に書き出す", GUILayout.Width(140f))) _br.ExportWav();
                GUILayout.EndHorizontal();

                // ── 印（連続したものはまとめてある）──
                GUILayout.Label(string.Format("印 {0} 件 → まとめて {1} か所",
                                              _br.RawMarkCount, _br.Runs.Length));
                _runScroll = GUILayout.BeginScrollView(_runScroll);
                for (int i = 0; i < _br.Runs.Length; ++i)
                {
                    var r = _br.Runs[i];
                    GUILayout.BeginHorizontal();
                    // ★続いた長さを必ず出す。1 フレームの瞬きとは意味が違う。
                    string span = (r.Count <= 1)
                        ? string.Format("f{0} {1,6:0.00}s", r.FirstFrame, r.FirstSeconds)
                        : string.Format("f{0}..{1} {2:0.00}s 続く", r.FirstFrame, r.LastFrame, r.Seconds);
                    GUILayout.Label(string.Format("{0,-26} 音源 {1,-5} {2,-18} {3} 最大 {4:0.000}",
                                                  span, r.SourceId, r.TemplateName, r.What, r.Worst));
                    if (info.MeshCount == 0 && GUILayout.Button("検査を吐く", GUILayout.Width(90f)))
                    {
                        int raw = _br.RawIndexOfRun(i);
                        if (raw >= 0)
                        {
                            string p = _br.EmitCase(raw, info.SceneName);
                            if (p != null) Debug.Log("[AcousticFlow] 回帰テストを吐きました: " + p);
                        }
                    }
                    GUILayout.EndHorizontal();
                }
                GUILayout.EndScrollView();
            }

            if (!string.IsNullOrEmpty(_br.Note)) GUILayout.Label(_br.Note);
            GUILayout.EndArea();
        }
    }
}
