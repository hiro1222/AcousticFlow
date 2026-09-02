/* AcousticHistory.cs
 * 【道具2 音の履歴タイムライン】の記録側。
 *
 * 解いている問題:
 *   音の不具合は「聞こえた瞬間には手が止まっていない」。
 *   気づいて Pause を押した時にはもう過ぎていて、同じ状況を作り直すのに何往復もかかる。
 *   毎フレームの状態を輪バッファに積んでおけば、後から巻き戻して見られる。
 *
 * ★窓（AcousticToolsWindow）を開いていなくても回る。
 *   [InitializeOnLoad] で EditorApplication.update に繋いであるため。
 *   「不具合が起きてから窓を開ける」では間に合わないので、こうした。
 *
 * コスト:
 *   1 フレームあたり float を 50 個ほど写すだけ。配列は起動時に確保して使い回すので GC は出ない。
 *   実行時（ビルド）側のコードには一切触っていない ── これは Editor 専用。
 *
 * 値の出どころ:
 *   AcousticFlowSceneDemo.Status（PublishMonitors() が毎フレーム更新）
 *   IrConvolver.Scope（audio thread が書く段ごとの RMS）
 *   ★Status の配列は参照共有（ゼロGC）なので、参照を持つと次のフレームで上書きされる。
 *     必ず値を写すこと。
 */
using System.IO;
using UnityEditor;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    [InitializeOnLoad]
    internal static class AcousticHistory
    {
        public const int Capacity = 3600;      // 60fps で 60 秒
        public const int MaxSources = 16;      // これを超えた音源は履歴に載らない（予算メータ側では見える）
        public const int NumBands = 6;

        // 印の種類。タイムライン上に縦線で出る。
        public const int MarkNone = 0;
        public const int MarkManual = 1;       // 手で付けた
        public const int MarkJump = 2;         // 跳びを自動検出した

        // --- 輪バッファ本体 -------------------------------------------------
        // _head は「次に書く位置」。Count は溜まった数（Capacity で頭打ち）。
        public static int Count { get; private set; }
        private static int _head;

        public static readonly float[] Time = new float[Capacity];
        public static readonly float[] Fps = new float[Capacity];
        public static readonly float[] AcousticMs = new float[Capacity];
        public static readonly float[] RoomRt60 = new float[Capacity];
        public static readonly float[] RoomVolume = new float[Capacity];
        public static readonly float[] Wet = new float[Capacity];
        public static readonly float[] MixingMs = new float[Capacity];
        public static readonly float[] DiffDelta = new float[Capacity];
        public static readonly float[] SourceLevel = new float[Capacity];

        public static readonly float[] RmsDirect = new float[Capacity];
        public static readonly float[] RmsEarly = new float[Capacity];
        public static readonly float[] RmsScatter = new float[Capacity];
        public static readonly float[] RmsTail = new float[Capacity];
        public static readonly float[] RmsOut = new float[Capacity];

        public static readonly int[] SrcCount = new int[Capacity];
        public static readonly int[] TapCount = new int[Capacity];
        public static readonly int[] ErActive = new int[Capacity];
        public static readonly int[] DiffActive = new int[Capacity];
        public static readonly int[] Mark = new int[Capacity];

        public static readonly float[] Occ = new float[Capacity * MaxSources];
        public static readonly float[] Surv = new float[Capacity * MaxSources];
        public static readonly float[] BandsTx = new float[Capacity * NumBands];
        public static readonly float[] BandsDf = new float[Capacity * NumBands];

        // 音源名は毎フレーム変わらないので、最後のものだけ持つ。
        public static string[] SourceNames = new string[0];

        // --- 跳びの自動検出 -------------------------------------------------
        // ★これは道具1（不連続ウォッチャ）の芽。
        //   本式のウォッチャは「こちらから振って探す」が、こちらは「実際に起きたものを捕まえる」。
        //   設計の第一制約が連続性なので、跳びは常に見張っていてよい。
        public static float JumpDb = 6f;            // 出力がこの dB 以上 1 フレームで動いたら印
        public static float JumpOcc = 0.35f;        // 遮蔽が 1 フレームでこれ以上動いたら印
        public static int JumpCount { get; private set; }

        private static int _lastFrame = -1;
        private static bool _hasPrev;
        private static float _prevOutDb;
        private static readonly float[] _prevOcc = new float[MaxSources];

        static AcousticHistory()
        {
            EditorApplication.update += Tick;
            EditorApplication.playModeStateChanged += OnPlayModeChanged;
        }

        private static void OnPlayModeChanged(PlayModeStateChange s)
        {
            // 再生に入るたびに捨てる。前回の再生の履歴が混ざると、時刻の連続性が嘘になる。
            if (s == PlayModeStateChange.ExitingEditMode) Clear();
        }

        public static void Clear()
        {
            Count = 0; _head = 0; JumpCount = 0;
            _hasPrev = false; _lastFrame = -1;
        }

        /// <summary>i 番目（0 = 一番古い）の実体位置。</summary>
        public static int Index(int i)
        {
            if (Count < Capacity) return i;
            return (_head + i) % Capacity;
        }

        public static void MarkLatest(int kind)
        {
            if (Count <= 0) return;
            Mark[Index(Count - 1)] = kind;
        }

        private static void Tick()
        {
            if (!EditorApplication.isPlaying) return;
            if (!AcousticFlowSceneDemo.Status.Valid) return;

            // 1 フレーム 1 本。EditorApplication.update は再生中も編集中も呼ばれるので、
            // フレーム番号で間引かないと同じフレームを何本も積む。
            int f = UnityEngine.Time.frameCount;
            if (f == _lastFrame) return;
            _lastFrame = f;

            int w = _head;

            Time[w] = UnityEngine.Time.realtimeSinceStartup;
            Fps[w] = AcousticFlowSceneDemo.Status.Fps;
            AcousticMs[w] = AcousticFlowSceneDemo.Status.AcousticMs;
            RoomRt60[w] = AcousticFlowSceneDemo.Status.RoomRt60;
            RoomVolume[w] = AcousticFlowSceneDemo.Status.RoomVolume;
            Wet[w] = AcousticFlowSceneDemo.Status.Wet;
            MixingMs[w] = AcousticFlowSceneDemo.Status.MixingTimeMs;
            DiffDelta[w] = AcousticFlowSceneDemo.Status.DiffDelta;
            SourceLevel[w] = AcousticFlowSceneDemo.Status.SourceLevel;

            RmsDirect[w] = IrConvolver.Scope.RmsDirect;
            RmsEarly[w] = IrConvolver.Scope.RmsEarly;
            RmsScatter[w] = IrConvolver.Scope.RmsScatter;
            RmsTail[w] = IrConvolver.Scope.RmsTail;
            RmsOut[w] = IrConvolver.Scope.RmsOut;

            TapCount[w] = AcousticFlowSceneDemo.Status.TapCount;
            ErActive[w] = AcousticFlowSceneDemo.Status.ErActive;
            DiffActive[w] = AcousticFlowSceneDemo.Status.DiffActive;
            Mark[w] = MarkNone;

            string[] names = AcousticFlowSceneDemo.Status.SourceNames;
            int n = (names != null) ? names.Length : 0;
            SrcCount[w] = n;
            if (names != null && SourceNames.Length != n) SourceNames = (string[])names.Clone();

            float[] occ = AcousticFlowSceneDemo.Status.Occlusion;
            float[] surv = AcousticFlowSceneDemo.Status.Survival;
            int nRec = Mathf.Min(n, MaxSources);
            int ob = w * MaxSources;
            for (int i = 0; i < MaxSources; i++)
            {
                Occ[ob + i] = (i < nRec && occ != null && i < occ.Length) ? occ[i] : 0f;
                Surv[ob + i] = (i < nRec && surv != null && i < surv.Length) ? surv[i] : 0f;
            }

            float[] tx = AcousticFlowSceneDemo.Status.BandsTransmit;
            float[] df = AcousticFlowSceneDemo.Status.BandsDiffract;
            int bb = w * NumBands;
            for (int b = 0; b < NumBands; b++)
            {
                BandsTx[bb + b] = (tx != null && b < tx.Length) ? tx[b] : 0f;
                BandsDf[bb + b] = (df != null && b < df.Length) ? df[b] : 0f;
            }

            DetectJump(w, nRec);

            _head = (_head + 1) % Capacity;
            if (Count < Capacity) Count++;
        }

        private static void DetectJump(int w, int nRec)
        {
            float outDb = ToDb(RmsOut[w]);
            bool jump = false;

            if (_hasPrev)
            {
                // 無音どうしの差は意味が無いので、片方でも鳴っているときだけ見る。
                if ((outDb > -70f || _prevOutDb > -70f) && Mathf.Abs(outDb - _prevOutDb) >= JumpDb) jump = true;

                int obPrev = w * MaxSources;
                for (int i = 0; i < nRec && !jump; i++)
                    if (Mathf.Abs(Occ[obPrev + i] - _prevOcc[i]) >= JumpOcc) jump = true;
            }

            if (jump) { Mark[w] = MarkJump; JumpCount++; }

            _prevOutDb = outDb;
            int ob = w * MaxSources;
            for (int i = 0; i < MaxSources; i++) _prevOcc[i] = Occ[ob + i];
            _hasPrev = true;
        }

        public static float ToDb(float lin)
        {
            return 20f * Mathf.Log10(Mathf.Max(1e-5f, lin));
        }

        /// <summary>表計算で見たいとき用。1 行 1 フレーム。</summary>
        public static void ExportCsv(string path)
        {
            using (var sw = new StreamWriter(path, false, new System.Text.UTF8Encoding(true)))
            {
                sw.Write("t,fps,acoustic_ms,out_db,direct_db,early_db,scatter_db,tail_db,");
                sw.Write("rt60,room_volume,wet,mixing_ms,diff_delta,src_level,taps,er,diff,mark");
                for (int i = 0; i < MaxSources; i++) sw.Write(",occ" + i);
                for (int b = 0; b < NumBands; b++) sw.Write(",tx" + b);
                for (int b = 0; b < NumBands; b++) sw.Write(",df" + b);
                sw.WriteLine();

                float t0 = (Count > 0) ? Time[Index(0)] : 0f;
                for (int i = 0; i < Count; i++)
                {
                    int k = Index(i);
                    sw.Write((Time[k] - t0).ToString("F4"));
                    sw.Write("," + Fps[k].ToString("F1"));
                    sw.Write("," + AcousticMs[k].ToString("F3"));
                    sw.Write("," + ToDb(RmsOut[k]).ToString("F2"));
                    sw.Write("," + ToDb(RmsDirect[k]).ToString("F2"));
                    sw.Write("," + ToDb(RmsEarly[k]).ToString("F2"));
                    sw.Write("," + ToDb(RmsScatter[k]).ToString("F2"));
                    sw.Write("," + ToDb(RmsTail[k]).ToString("F2"));
                    sw.Write("," + RoomRt60[k].ToString("F3"));
                    sw.Write("," + RoomVolume[k].ToString("F1"));
                    sw.Write("," + Wet[k].ToString("F3"));
                    sw.Write("," + MixingMs[k].ToString("F1"));
                    sw.Write("," + DiffDelta[k].ToString("F3"));
                    sw.Write("," + SourceLevel[k].ToString("F3"));
                    sw.Write("," + TapCount[k] + "," + ErActive[k] + "," + DiffActive[k] + "," + Mark[k]);
                    int ob = k * MaxSources;
                    for (int s = 0; s < MaxSources; s++) sw.Write("," + Occ[ob + s].ToString("F3"));
                    int bb = k * NumBands;
                    for (int b = 0; b < NumBands; b++) sw.Write("," + BandsTx[bb + b].ToString("F4"));
                    for (int b = 0; b < NumBands; b++) sw.Write("," + BandsDf[bb + b].ToString("F4"));
                    sw.WriteLine();
                }
            }
        }
    }
}
