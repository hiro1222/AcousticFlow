/* AfhrWriter.cs (Editor 専用)
 * .afhr 形式の書き出し。
 *
 * 重要な前提: .afhr は「ITD が波形に焼き込まれた生の HRIR」を格納する。
 *   実測データ(SOFA/KEMAR)がそうなっているのに合わせた形式で、
 *   読み込み側(HrtfSet)が立ち上がりを検出して ITD を分離する。
 *
 *   したがって書き出す側は、ITD を含んだ状態で渡さなければならない。
 *   HrtfSet が保持している HRIR は既に ITD を抜いて 0 に揃えてあるので、
 *   それをそのまま書くと読み直したとき ITD=0 になり定位が消える。
 *   → 合成HRTFのように ITD を別に持つデータは、書き出し時に遅延として付け直す。
 */
using System.IO;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public static class AfhrWriter
    {
        private const uint kMagic = 0x52484641;   // 'A','F','H','R'
        private const int kVersion = 1;

        /// 生の HRIR（ITD 込み）をそのまま書く。実測データの取り込み用。
        ///   hrir[dir][ear][sample]
        public static bool Write(string path, int sampleRate, int irLength,
                                 float[] az, float[] el, float[][][] hrir)
        {
            if (az == null || el == null || hrir == null || irLength <= 0) return false;
            int n = Mathf.Min(az.Length, Mathf.Min(el.Length, hrir.Length));
            if (n <= 0) return false;

            try
            {
                using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write))
                using (var bw = new BinaryWriter(fs))
                {
                    bw.Write(kMagic);
                    bw.Write(kVersion);
                    bw.Write(sampleRate);
                    bw.Write(n);
                    bw.Write(irLength);
                    for (int i = 0; i < n; i++) { bw.Write(az[i]); bw.Write(el[i]); }
                    for (int i = 0; i < n; i++)
                    {
                        WriteChannel(bw, hrir[i] != null ? hrir[i][0] : null, irLength);
                        WriteChannel(bw, hrir[i] != null && hrir[i].Length > 1 ? hrir[i][1] : null, irLength);
                    }
                }
                return true;
            }
            catch (System.Exception e)
            {
                Debug.LogError($"[AfhrWriter] 書き出し失敗: {e.Message}");
                return false;
            }
        }

        /// ITD を別に持つ HrtfSet（合成HRTFなど）を、ITD を遅延として付け直して書く。
        ///   こうしないと読み直したとき ITD が失われる（このファイル冒頭の注意参照）。
        public static bool WriteWithItd(string path, HrtfSet set, int extraTailSamples = 48)
        {
            if (set == null || !set.IsValid) return false;

            int n = set.DirectionCount;
            // ITD 分の遅延で末尾が切れないよう、少し長めに書く。
            int outLen = set.IrLength + Mathf.Max(0, extraTailSamples);

            var az = new float[n];
            var el = new float[n];
            var hrir = new float[n][][];

            for (int i = 0; i < n; i++)
            {
                set.GetAngles(i, out az[i], out el[i]);
                float itd = set.GetItdSeconds(i);
                // 正 = 右耳が遅い。負の遅延は作れないので遅い側だけを遅らせる。
                int dl = (itd < 0f) ? Mathf.RoundToInt(-itd * set.SampleRate) : 0;
                int dr = (itd > 0f) ? Mathf.RoundToInt(itd * set.SampleRate) : 0;
                hrir[i] = new[]
                {
                    Delay(set.GetHrir(i, 0), dl, outLen),
                    Delay(set.GetHrir(i, 1), dr, outLen),
                };
            }
            return Write(path, set.SampleRate, outLen, az, el, hrir);
        }

        private static float[] Delay(float[] src, int delaySamples, int outLen)
        {
            var dst = new float[outLen];
            if (src == null) return dst;
            for (int k = 0; k < src.Length; k++)
            {
                int d = k + delaySamples;
                if (d >= 0 && d < outLen) dst[d] = src[k];
            }
            return dst;
        }

        private static void WriteChannel(BinaryWriter bw, float[] h, int irLength)
        {
            for (int k = 0; k < irLength; k++) bw.Write((h != null && k < h.Length) ? h[k] : 0f);
        }
    }
}
