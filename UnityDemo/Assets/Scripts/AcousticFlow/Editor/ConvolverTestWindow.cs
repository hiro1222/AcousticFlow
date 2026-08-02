/* ConvolverTestWindow.cs (Editor 専用)
 * 畳み込み器の数値検証。
 *
 *   メニュー: AcousticFlow > Convolver Test
 *
 * なぜ必要か:
 *   畳み込みは符号・オフセット・アライメントを1つ間違えても「それらしく鳴る」ので、
 *   耳では正しさを確かめられない。総当たり(直接畳み込み)と数値比較して初めて確定する。
 *   非一様分割は段ごとの遅延合わせが要で、そこがズレると尾の形が崩れる。
 *
 * 検証内容:
 *   1) 一様分割   … 直接畳み込みと一致するか（既存実装の回帰）
 *   2) 非一様分割 … 直接畳み込みと一致するか（今回の実装）
 *   3) 遅延       … 実際の遅延サンプル数が設計どおりか（最小ブロック）
 */
using System.Text;
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class ConvolverTestWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/Convolver Test")]
        public static void Open()
        {
            var w = GetWindow<ConvolverTestWindow>("Convolver Test");
            w.minSize = new Vector2(520f, 320f);
            w.Show();
        }

        private string _report = "";
        private int _irLength = 4096;
        private int _signalLength = 8192;

        private void OnGUI()
        {
            EditorGUILayout.HelpBox(
                "畳み込み器を直接畳み込み(総当たり)と数値比較します。\n" +
                "畳み込みは間違っていても『それらしく』鳴るので、耳ではなく数値で確認します。",
                MessageType.Info);

            _irLength = EditorGUILayout.IntSlider("IR 長(サンプル)", _irLength, 256, 16384);
            _signalLength = EditorGUILayout.IntSlider("信号長(サンプル)", _signalLength, 1024, 32768);

            if (GUILayout.Button("検証を実行")) RunTests();

            if (!string.IsNullOrEmpty(_report))
            {
                EditorGUILayout.Space(8);
                EditorGUILayout.TextArea(_report, GUILayout.ExpandHeight(true));
            }
        }

        private void RunTests()
        {
            var sb = new StringBuilder();
            int irLen = _irLength;
            int sigLen = _signalLength;

            // 再現性のある擬似乱数で IR と入力を作る。
            var rng = new System.Random(12345);
            var ir = new float[2][];
            for (int c = 0; c < 2; c++)
            {
                ir[c] = new float[irLen];
                for (int i = 0; i < irLen; i++)
                {
                    // 指数減衰するノイズ＝残響の尾に近い形。
                    float env = Mathf.Exp(-3f * i / irLen);
                    ir[c][i] = (float)(rng.NextDouble() * 2.0 - 1.0) * env;
                }
            }
            var x = new float[sigLen];
            for (int i = 0; i < sigLen; i++) x[i] = (float)(rng.NextDouble() * 2.0 - 1.0);

            // 期待値: 直接畳み込み（遅延ゼロ）。
            var expect = new float[2][];
            for (int c = 0; c < 2; c++)
            {
                expect[c] = new float[sigLen];
                for (int n = 0; n < sigLen; n++)
                {
                    float s = 0f;
                    int kMax = Mathf.Min(irLen, n + 1);
                    for (int k = 0; k < kMax; k++) s += ir[c][k] * x[n - k];
                    expect[c][n] = s;
                }
            }

            sb.AppendLine($"IR {irLen} サンプル / 信号 {sigLen} サンプル\n");

            // ── 1) 一様分割 ──
            {
                int blk = 1024;
                int parts = Mathf.CeilToInt(irLen / (float)blk);
                var conv = new PartitionedConvolver(blk, parts, 2);
                conv.SetIr(ir);
                var got = RunConvolver(
                    (inp, off, n, outCh) => conv.ProcessAdd(inp, off, n, outCh, 0, 1f), x, sigLen);
                Compare(sb, "一様分割 B=1024", expect, got, blk, sigLen);
            }

            // ── 2) 非一様分割 ──
            {
                var conv = new NonUniformConvolver(irLen, 2, 64);
                conv.SetIr(ir);
                sb.AppendLine($"  スケジュール: {conv.DescribeSchedule()}");
                var got = RunConvolver(
                    (inp, off, n, outCh) => conv.ProcessAdd(inp, off, n, outCh, 0, 1f), x, sigLen);
                Compare(sb, "非一様分割 B0=64", expect, got, conv.Latency, sigLen);
            }

            // ── 3) 非一様分割（最小ブロックを変えても一致するか）──
            {
                var conv = new NonUniformConvolver(irLen, 2, 128);
                conv.SetIr(ir);
                var got = RunConvolver(
                    (inp, off, n, outCh) => conv.ProcessAdd(inp, off, n, outCh, 0, 1f), x, sigLen);
                Compare(sb, "非一様分割 B0=128", expect, got, conv.Latency, sigLen);
            }

            _report = sb.ToString();
        }

        private delegate void ProcessFn(float[] input, int offset, int n, float[][] outCh);

        // オーディオコールバックを模してブロック単位で流す（実行時と同じ経路を通す）。
        private static float[][] RunConvolver(ProcessFn fn, float[] x, int sigLen)
        {
            var outCh = new float[2][];
            for (int c = 0; c < 2; c++) outCh[c] = new float[sigLen];

            const int kCallback = 512;   // Unity の DSP バッファ相当
            var tmp = new float[2][];
            for (int c = 0; c < 2; c++) tmp[c] = new float[kCallback];

            for (int pos = 0; pos < sigLen; pos += kCallback)
            {
                int n = Mathf.Min(kCallback, sigLen - pos);
                for (int c = 0; c < 2; c++) System.Array.Clear(tmp[c], 0, n);
                fn(x, pos, n, tmp);
                for (int c = 0; c < 2; c++)
                    for (int i = 0; i < n; i++) outCh[c][pos + i] = tmp[c][i];
            }
            return outCh;
        }

        // 期待値を latency ぶんずらして比較する。
        private static void Compare(StringBuilder sb, string label,
                                    float[][] expect, float[][] got, int latency, int sigLen)
        {
            // 立ち上がり付近は履歴が足りず一致しないので、十分進んだ範囲だけ見る。
            int start = latency + 2048;
            int end = sigLen;
            if (start >= end) { sb.AppendLine($"  [SKIP] {label}: 比較区間が取れない"); return; }

            float maxErr = 0f, maxRef = 0f;
            for (int c = 0; c < 2; c++)
                for (int n = start; n < end; n++)
                {
                    float e = expect[c][n - latency];
                    float g = got[c][n];
                    maxErr = Mathf.Max(maxErr, Mathf.Abs(g - e));
                    maxRef = Mathf.Max(maxRef, Mathf.Abs(e));
                }
            float rel = (maxRef > 1e-9f) ? maxErr / maxRef : maxErr;
            bool ok = rel < 1e-3f;
            sb.AppendLine($"  [{(ok ? "OK" : "FAIL")}] {label}  遅延={latency}samp  "
                          + $"相対誤差={rel:E2} (最大 {maxErr:E3} / 基準 {maxRef:F3})");

            if (!ok)
            {
                // ズレの正体を掴むため、いくつかの遅延で試して最良を報告する。
                int bestLag = 0;
                float bestRel = float.MaxValue;
                for (int lag = 0; lag < 4096; lag++)
                {
                    float er = 0f, rf = 0f;
                    for (int n = start; n < Mathf.Min(end, start + 2048); n++)
                    {
                        int src = n - lag;
                        if (src < 0 || src >= sigLen) continue;
                        float e = expect[0][src];
                        er = Mathf.Max(er, Mathf.Abs(got[0][n] - e));
                        rf = Mathf.Max(rf, Mathf.Abs(e));
                    }
                    float r = (rf > 1e-9f) ? er / rf : er;
                    if (r < bestRel) { bestRel = r; bestLag = lag; }
                }
                sb.AppendLine($"        → 実際に一致する遅延は {bestLag} samp (相対誤差 {bestRel:E2})");
            }
        }
    }
}
