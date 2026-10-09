// CaptureBrowser.cs — 録れた .afcap を開いて調べる**中身**。見た目は持たない。
//
// ★なぜランタイム側に置くのか
//   `Native`（P/Invoke の宣言）は **internal**。Unity は `Editor/` を別アセンブリに
//   するので、Editor 窓からは触れない（最初これで CS0122 が 33 件出た）。
//   `Native` を public に広げるより、中身をここへ置いて両方から使うほうが素直。
//
// ★見せ方は 2 つ、中身は 1 つ
//   ・`CapturePanel`（F11・ゲーム内）……ビルドでも動く
//   ・`Editor/CaptureWindow`……ドッキングできる。**再生中でなくても使える**
//   写経すると必ずずれるので、計算と DLL 呼び出しはここだけ。
//
// ⚠ 走査（型紙を当てる）は C# では**やらない**。DLL の中の関数を呼ぶ。
//   回帰テストが呼ぶのと同じ関数でないと「Unity では印が出るのに検査は通る」が起きる。
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace AcousticFlow
{
    /// 走査で付いた印（連続したものはまとめてある）。
    /// ★P/Invoke の構造体をそのまま公開しない（Native が internal なので公開できない）。
    ///   ここで素の型へ写しておくと、ABI の並びが変わっても呼び手が壊れない。
    public struct CaptureRunInfo
    {
        public ulong SourceId;
        public int FirstFrame, LastFrame, Count;
        public float FirstSeconds, LastSeconds, Worst;
        public string TemplateName, What;

        /// 続いた長さ（秒）。★1 フレームの瞬きか、続いているかで意味が違う。
        public float Seconds { get { return Mathf.Max(0f, LastSeconds - FirstSeconds); } }
    }

    public struct CaptureSummary
    {
        public int Frames, SampleRate, Preroll, Postroll, WorkerThreads, MarkedFrame;
        public int BoxCount, MeshCount, MaterialCount, PcmFrames, SourceCount;
        public uint DllHash;
        public string SceneName;

        public float PcmSeconds
        {
            get { return SampleRate > 0 ? PcmFrames / (float)SampleRate : 0f; }
        }
    }

    public class CaptureBrowser : System.IDisposable
    {
        private System.IntPtr _cap = System.IntPtr.Zero;
        private readonly List<string> _files = new List<string>();

        public IList<string> Files { get { return _files; } }
        public int Selected { get; private set; } = -1;
        public bool IsOpen { get { return _cap != System.IntPtr.Zero; } }
        public CaptureSummary Info { get; private set; }
        public CaptureRunInfo[] Runs { get; private set; } = new CaptureRunInfo[0];
        public int RawMarkCount { get; private set; }
        public string Note { get; private set; } = "";

        /// 保存先。ビルドでも書ける場所（Assets 直下は製品ビルドに無い）。
        public static string CaptureDir
        {
            get { return Path.Combine(Application.persistentDataPath, "AcousticFlowCaptures"); }
        }

        public void RefreshFiles()
        {
            _files.Clear();
            try
            {
                if (Directory.Exists(CaptureDir))
                {
                    var f = Directory.GetFiles(CaptureDir, "*.afcap");
                    System.Array.Sort(f);
                    System.Array.Reverse(f);          // 新しい順
                    _files.AddRange(f);
                }
            }
            catch (System.Exception e) { Note = "一覧を読めません: " + e.Message; }
        }

        public void Close()
        {
            if (_cap != System.IntPtr.Zero) { Native.AF_CaptureClose(_cap); _cap = System.IntPtr.Zero; }
            Runs = new CaptureRunInfo[0];
            RawMarkCount = 0;
            Selected = -1;
        }

        public void Dispose() { Close(); }

        public bool Open(int index)
        {
            Close();
            if (index < 0 || index >= _files.Count) return false;
            Selected = index;
            _cap = Native.AF_CaptureOpen(_files[index]);
            if (_cap == System.IntPtr.Zero) { Note = "開けません: " + _files[index]; Selected = -1; return false; }

            Native.AF_CaptureInfo raw;
            Native.AF_CaptureGetInfo(_cap, out raw);
            var buf = new byte[128];
            int n = Native.AF_CaptureGetSceneName(_cap, buf, buf.Length);
            Info = new CaptureSummary
            {
                Frames = raw.frames, SampleRate = raw.sampleRate,
                Preroll = raw.preroll, Postroll = raw.postroll,
                WorkerThreads = raw.workerThreads, MarkedFrame = raw.markedFrame,
                BoxCount = raw.boxCount, MeshCount = raw.meshCount,
                MaterialCount = raw.materialCount, PcmFrames = raw.pcmFrames,
                SourceCount = raw.sourceCount, DllHash = raw.dllHash,
                SceneName = n > 0 ? System.Text.Encoding.UTF8.GetString(buf, 0, n) : "",
            };

            // ★走査は DLL の中。回帰テストが呼ぶのと同じ関数。
            RawMarkCount = Native.AF_CaptureScan(_cap, null, 0);
            int total = Native.AF_CaptureScanRuns(_cap, null, 0);
            if (total > 0)
            {
                var runs = new Native.AF_CaptureRun[total];
                Native.AF_CaptureScanRuns(_cap, runs, total);
                Runs = new CaptureRunInfo[total];
                for (int i = 0; i < total; ++i)
                    Runs[i] = new CaptureRunInfo
                    {
                        SourceId = runs[i].sourceId,
                        FirstFrame = runs[i].firstFrame, LastFrame = runs[i].lastFrame,
                        Count = runs[i].count,
                        FirstSeconds = runs[i].firstSeconds, LastSeconds = runs[i].lastSeconds,
                        Worst = runs[i].worst,
                        TemplateName = Native.Utf8(runs[i].templateName),
                        What = Native.Utf8(runs[i].what),
                    };
            }
            else Runs = new CaptureRunInfo[0];

            Note = string.Format("印 {0} 件（まとめて {1} か所）", RawMarkCount, Runs.Length);
            return true;
        }

        /// マスターPCM を取り出す（interleaved stereo）。取れたフレーム数を返す。
        /// ★「実際に何が聞こえたか」の物証。**エンジンを直しても、これは変わらない。**
        public int ReadPcm(out float[] pcm)
        {
            pcm = null;
            if (!IsOpen || Info.PcmFrames <= 0) return 0;
            var buf = new float[Info.PcmFrames * 2];
            int got = Native.AF_CaptureGetPcm(_cap, buf, Info.PcmFrames);
            if (got <= 0) { Note = "PCM が入っていません"; return 0; }
            pcm = buf;
            return got;
        }

        /// 印から C++ の回帰テストを吐く。書けたパスを返す（失敗で null）。
        /// ⚠ 番号は**生の印**の番号（まとめる前）。まとまりの先頭を使うこと。
        public string EmitCase(int rawMarkIndex, string sceneNameForFile)
        {
            if (!IsOpen) return null;
            if (Info.MeshCount > 0)
            {
                Note = "メッシュを含む場面は C++ の検査に吐けません（形が大きすぎるため）";
                return null;
            }
            var buf = new byte[1 << 20];
            string name = string.Format("testGenerated_{0}_{1}", SafeName(sceneNameForFile), rawMarkIndex);
            int n = Native.AF_CaptureEmitCase(_cap, rawMarkIndex, name, buf, buf.Length);
            if (n <= 0) { Note = "吐けませんでした"; return null; }
            // ⚠ 戻り値は**元の長さ**なのでバッファより大きいことがある。NUL まで読む。
            string code = Native.Utf8(buf);
            if (n > buf.Length - 1) Note = "⚠ 途中で切れました（" + n + " 文字）。";
            string outPath = Path.ChangeExtension(_files[Selected], null) + "_" + name + ".cpp.txt";
            try
            {
                File.WriteAllText(outPath, code, new System.Text.UTF8Encoding(false));
                Note = "吐きました: " + outPath;
                return outPath;
            }
            catch (System.Exception e) { Note = "書けません: " + e.Message; return null; }
        }

        /// まとまりの先頭が、生の印の何番目かを引く。
        /// ★吐き出しは生の番号で指定するため。
        public int RawIndexOfRun(int runIndex)
        {
            if (runIndex < 0 || runIndex >= Runs.Length || !IsOpen) return -1;
            int total = Native.AF_CaptureScan(_cap, null, 0);
            if (total <= 0) return -1;
            var marks = new Native.AF_CaptureMark[total];
            Native.AF_CaptureScan(_cap, marks, total);
            for (int i = 0; i < total; ++i)
                if (marks[i].sourceId == Runs[runIndex].SourceId
                    && marks[i].frame == Runs[runIndex].FirstFrame
                    && Native.Utf8(marks[i].templateName) == Runs[runIndex].TemplateName)
                    return i;
            return -1;
        }

        /// マスターPCM を WAV で書き出す。書けたパスを返す（失敗で null）。
        /// ★Editor 窓は再生中でなくても開けるが、そのとき AudioSource は使えない。
        ///   WAV にしておけば、どの編集ソフトでも開けるし AfAnalyzeWav でも測れる。
        public string ExportWav()
        {
            float[] pcm;
            int frames = ReadPcm(out pcm);
            if (frames <= 0) return null;
            string outPath = Path.ChangeExtension(_files[Selected], null) + ".wav";
            try
            {
                using (var fs = new FileStream(outPath, FileMode.Create, FileAccess.Write))
                using (var w = new BinaryWriter(fs))
                {
                    const int ch = 2, bits = 16;
                    int dataBytes = frames * ch * (bits / 8);
                    w.Write(new char[] { 'R', 'I', 'F', 'F' });
                    w.Write(36 + dataBytes);
                    w.Write(new char[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
                    w.Write(16); w.Write((short)1); w.Write((short)ch);
                    w.Write(Info.SampleRate);
                    w.Write(Info.SampleRate * ch * (bits / 8));
                    w.Write((short)(ch * (bits / 8))); w.Write((short)bits);
                    w.Write(new char[] { 'd', 'a', 't', 'a' });
                    w.Write(dataBytes);
                    for (int i = 0; i < frames * ch; ++i)
                        w.Write((short)Mathf.Clamp(Mathf.RoundToInt(pcm[i] * 32767f), -32768, 32767));
                }
                Note = "WAV を書きました: " + outPath;
                return outPath;
            }
            catch (System.Exception e) { Note = "WAV を書けません: " + e.Message; return null; }
        }

        private static string SafeName(string s)
        {
            if (string.IsNullOrEmpty(s)) return "Capture";
            var sb = new System.Text.StringBuilder();
            foreach (char c in s) sb.Append(char.IsLetterOrDigit(c) || c == '_' ? c : '_');
            return sb.ToString();
        }
    }
}
