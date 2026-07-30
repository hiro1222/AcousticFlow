/* KemarImporter.cs (Editor 専用)
 * MIT KEMAR (Gardner & Martin, MIT Media Lab 1994) の compact データを .afhr へ変換する。
 *
 * データ形式（hrtfdoc.txt より）:
 *   ・16bit 符号付き整数
 *   ・ビッグエンディアン（"most significant byte stored in the low address"＝Motorola 68000）
 *   ・compact は 1 ファイルに 128 点のステレオペア、(左, 右) インターリーブ
 *   ・ファイル名 HEEeAAAa.dat （EE=仰角[度], AAA=方位角[度]）
 *   ・仰角 -40〜+90、合計 710 方向
 *
 * SOFA(HDF5) ではなく生バイナリなので、Python を介さず C# で完結する。
 *
 * クレジット（データセットのライセンス条件）:
 *   "provided free with no restrictions on use, provided the authors are cited"
 *   → Bill Gardner and Keith Martin, MIT Media Lab (1994)
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public static class KemarImporter
    {
        private const int kKemarRate = 44100;
        private const int kIrLen = 128;

        private struct Entry
        {
            public float az, el;      // 度
            public float[] l, r;
        }

        // ファイル名 H-10e005a.dat → 仰角 -10 / 方位角 5
        private static readonly Regex kName =
            new Regex(@"^H(-?\d+)e(\d+)a\.dat$", RegexOptions.IgnoreCase);

        /// compact フォルダ（.dat を含む階層）を読み、targetRate にリサンプルして .afhr を書く。
        ///
        /// HrtfSet を経由せず生の HRIR のまま書くのが要点。HrtfSet は読み込み時に ITD を
        /// 分離して波形を 0 に揃えるので、そこを通すと ITD が失われる（AfhrWriter 冒頭参照）。
        public static bool ImportToFile(string rootDir, int targetRate, string outPath, out string log)
        {
            var sb = new StringBuilder();
            log = "";
            if (!Directory.Exists(rootDir)) { log = $"フォルダが見つかりません: {rootDir}"; return false; }

            var files = Directory.GetFiles(rootDir, "*.dat", SearchOption.AllDirectories);
            if (files.Length == 0) { log = $".dat が1つもありません: {rootDir}"; return false; }

            var list = new List<Entry>(files.Length);
            int skipped = 0;
            foreach (var f in files)
            {
                var m = kName.Match(Path.GetFileName(f));
                if (!m.Success) { skipped++; continue; }
                if (!ReadDat(f, out float[] l, out float[] r)) { skipped++; continue; }
                list.Add(new Entry
                {
                    el = int.Parse(m.Groups[1].Value),
                    az = int.Parse(m.Groups[2].Value),
                    l = l,
                    r = r,
                });
            }
            if (list.Count == 0) { log = "読み込めた .dat がありません（ファイル名の規則が違う可能性）"; return false; }
            sb.AppendLine($"読み込み: {list.Count} 方向（スキップ {skipped}）");

            // ── 方位角の向きを自動判定する ──
            // MIT と AcousticFlow で方位角の正方向が同じとは限らない。仕様書を読み違えるより、
            // 「右にあるはずの音源で右耳が早いか」を実データで確かめる方が確実。
            //   AcousticFlow: az +90 = 右。右の音源なら右耳が先に鳴る（onsetR < onsetL）。
            bool flip = NeedAzimuthFlip(list, sb);
            if (flip)
                for (int i = 0; i < list.Count; i++)
                {
                    var e = list[i];
                    e.az = -e.az;
                    list[i] = e;
                }

            // ── 半球しか無い場合は左右対称で補完する ──
            // compact は KEMAR の左右対称性を使って片側だけを収録していることがある。
            var span = AzimuthSpan(list);
            sb.AppendLine($"方位角の範囲: {span.min:F0}° 〜 {span.max:F0}°");
            if (span.max - span.min < 300f)
            {
                int before = list.Count;
                MirrorHalfSphere(list);
                sb.AppendLine($"左右対称で補完: {before} → {list.Count} 方向");
            }

            // ── リサンプル ──
            float[][][] hrir = new float[list.Count][][];
            int outLen = kIrLen;
            if (targetRate != kKemarRate)
            {
                outLen = Mathf.Max(8, Mathf.RoundToInt(kIrLen * (float)targetRate / kKemarRate));
                sb.AppendLine($"リサンプル: {kKemarRate} → {targetRate} Hz（{kIrLen} → {outLen} タップ）");
            }
            for (int i = 0; i < list.Count; i++)
            {
                hrir[i] = new float[2][];
                hrir[i][0] = (targetRate == kKemarRate) ? list[i].l : Resample(list[i].l, kKemarRate, targetRate, outLen);
                hrir[i][1] = (targetRate == kKemarRate) ? list[i].r : Resample(list[i].r, kKemarRate, targetRate, outLen);
            }

            // ── 正規化（全体のピークを 1 に）──
            float peak = 0f;
            foreach (var h in hrir) { foreach (var ch in h) foreach (var v in ch) peak = Mathf.Max(peak, Mathf.Abs(v)); }
            if (peak > 1e-9f)
            {
                float inv = 1f / peak;
                foreach (var h in hrir) foreach (var ch in h) for (int k = 0; k < ch.Length; k++) ch[k] *= inv;
                sb.AppendLine($"正規化: ピーク {peak:F4} → 1.0");
            }

            var az = new float[list.Count];
            var el = new float[list.Count];
            for (int i = 0; i < list.Count; i++) { az[i] = list[i].az; el[i] = list[i].el; }

            bool ok = AfhrWriter.Write(outPath, targetRate, outLen, az, el, hrir);
            sb.AppendLine(ok ? $"書き出し: {outPath}" : "書き出しに失敗しました。");
            log = sb.ToString();
            return ok;
        }

        // 16bit BE ステレオインターリーブ 128点を読む。
        private static bool ReadDat(string path, out float[] l, out float[] r)
        {
            l = null; r = null;
            try
            {
                byte[] raw = File.ReadAllBytes(path);
                int need = kIrLen * 2 * 2;   // 128点 × 2ch × 2byte
                if (raw.Length < need) return false;
                l = new float[kIrLen];
                r = new float[kIrLen];
                const float inv = 1f / 32768f;
                for (int k = 0; k < kIrLen; k++)
                {
                    int o = k * 4;
                    l[k] = BE16(raw, o) * inv;
                    r[k] = BE16(raw, o + 2) * inv;
                }
                return true;
            }
            catch { return false; }
        }

        // ビッグエンディアン 16bit 符号付き。
        private static short BE16(byte[] b, int off) => (short)((b[off] << 8) | b[off + 1]);

        // 立ち上がり位置（ピークの15%を最初に超えた位置）。
        private static int Onset(float[] h)
        {
            float peak = 0f;
            for (int i = 0; i < h.Length; i++) peak = Mathf.Max(peak, Mathf.Abs(h[i]));
            if (peak <= 1e-9f) return 0;
            float th = peak * 0.15f;
            for (int i = 0; i < h.Length; i++) if (Mathf.Abs(h[i]) >= th) return i;
            return 0;
        }

        // 「方位角 +90 付近の音源で、右耳が早いか」を調べる。早くなければ符号が逆。
        private static bool NeedAzimuthFlip(List<Entry> list, StringBuilder sb)
        {
            int best = -1;
            float bestScore = float.MaxValue;
            for (int i = 0; i < list.Count; i++)
            {
                // 水平面かつ方位角 90 に近いものを選ぶ。
                float score = Mathf.Abs(list[i].el) * 4f + Mathf.Abs(Mathf.Abs(list[i].az) - 90f);
                if (list[i].az < 0f) continue;   // 元データの正側だけで判定する
                if (score < bestScore) { bestScore = score; best = i; }
            }
            if (best < 0) { sb.AppendLine("方位角の向き: 判定できず（そのまま採用）"); return false; }

            int oL = Onset(list[best].l), oR = Onset(list[best].r);
            bool rightEarlier = oR < oL;
            sb.AppendLine($"方位角の向き判定: az={list[best].az:F0}° el={list[best].el:F0}° "
                          + $"onsetL={oL} onsetR={oR} → "
                          + (rightEarlier ? "+が右（そのまま）" : "+が左（符号を反転）"));
            return !rightEarlier;
        }

        private static (float min, float max) AzimuthSpan(List<Entry> list)
        {
            float mn = float.MaxValue, mx = float.MinValue;
            foreach (var e in list) { mn = Mathf.Min(mn, e.az); mx = Mathf.Max(mx, e.az); }
            return (mn, mx);
        }

        // 片側しか無いデータを左右対称で補完する（L/R を入れ替えて方位角を反転）。
        private static void MirrorHalfSphere(List<Entry> list)
        {
            int n = list.Count;
            for (int i = 0; i < n; i++)
            {
                float mirroredAz = -list[i].az;
                // 0 と 180 は鏡像が自分自身なので複製しない。
                if (Mathf.Abs(list[i].az) < 0.5f || Mathf.Abs(Mathf.Abs(list[i].az) - 180f) < 0.5f) continue;
                list.Add(new Entry
                {
                    az = mirroredAz,
                    el = list[i].el,
                    l = list[i].r,   // 左右入れ替え
                    r = list[i].l,
                });
            }
        }

        // Lanczos リサンプル。HRIR は短いので素直な実装で十分（変換は1回だけ）。
        //   線形補間だと高域が鈍って定位の手がかり（耳介由来のノッチ）が潰れるため、
        //   帯域制限つきの補間を使う。
        private static float[] Resample(float[] src, int srcRate, int dstRate, int outLen)
        {
            const int kA = 8;   // Lanczos の半径
            var dst = new float[outLen];
            double ratio = (double)srcRate / dstRate;
            for (int i = 0; i < outLen; i++)
            {
                double pos = i * ratio;
                int c = (int)Math.Floor(pos);
                double sum = 0.0, wsum = 0.0;
                for (int k = c - kA + 1; k <= c + kA; k++)
                {
                    if (k < 0 || k >= src.Length) continue;
                    double w = Lanczos(pos - k, kA);
                    sum += src[k] * w;
                    wsum += w;
                }
                dst[i] = (float)((wsum > 1e-12) ? sum / wsum : 0.0);
            }
            return dst;
        }

        private static double Lanczos(double x, int a)
        {
            if (Math.Abs(x) < 1e-12) return 1.0;
            if (Math.Abs(x) >= a) return 0.0;
            double px = Math.PI * x;
            return a * Math.Sin(px) * Math.Sin(px / a) / (px * px);
        }
    }
}
