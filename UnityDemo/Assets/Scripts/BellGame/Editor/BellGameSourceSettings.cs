// BellGameSourceSettings.cs
// 音源リストで触った設定を CSV に保存し、読み直す。
//
// ★なぜ要るか
//   音源リストの詳細エディタは**再生中の実物**を触ります。
//   再生を止めると全部消えます ── 耳で合わせた値がその場限りになるので、
//   「良かった状態」へ二度と戻れません。
//
// ★なぜ CSV か（発注者の指定：「エクセルとか扱うんならそれで全然いい」）
//   ScriptableObject にすると Unity の中でしか読めません。
//   CSV なら Excel で並べ替えも比較もでき、差分も git で読めます。
//   **人が読める形で残る**ことのほうが、型の安全より価値があります。
//
// ★2 種類あります。混ぜません。
//     ① 設定（往復する）   … 音量・消音・2D/3D。読み込むと実物に戻ります
//     ② 測定（書き出すだけ）… 遮蔽・帯域・経路数。読み込む先がありません
//   1 つのファイルに混ぜると「読み込んだら測定値まで書き戻すのか？」という
//   答えのない問いが生まれます（決めごと #1：一つの問いに二つの答えを作らない）。
//
// ★文字コードは **UTF-8 BOM 付き**。
//   BOM が無いと Excel が日本語を化けさせます（Shift_JIS だと思い込むため）。
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using AcousticFlow;
using BellGame;
using UnityEditor;
using UnityEngine;

namespace BellGameEditor
{
    public static class BellGameSourceSettings
    {
        public const string SettingsPath = "Assets/BellGame/SourceSettings.csv";

        // ── ① 設定（往復する）──────────────────────────────
        //
        // 列は**触れるものだけ**。測定値を混ぜないこと。

        public static string Save(IList<Transform> sources)
        {
            if (sources == null || sources.Count == 0) return "音源が居ません";

            var sb = new StringBuilder();
            sb.AppendLine("名前,音量,消音,空間,段,可聴半径");

            var seen = new HashSet<string>();
            var dup = new List<string>();

            foreach (var t in sources)
            {
                if (t == null) continue;
                var a = t.GetComponent<AudioSource>();
                if (a == null) continue;

                if (!seen.Add(t.name)) dup.Add(t.name);

                sb.Append(Csv(t.name)).Append(',')
                  .Append(a.volume.ToString("F4", CultureInfo.InvariantCulture)).Append(',')
                  .Append(a.mute ? "1" : "0").Append(',')
                  .Append(a.spatialBlend >= 0.5f ? "3D" : "2D").Append(',')
                  .Append(TierName(t)).Append(',')
                  .Append(RadiusOf(t).ToString("F1", CultureInfo.InvariantCulture))
                  .AppendLine();
            }

            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(SettingsPath));
            System.IO.File.WriteAllText(SettingsPath, sb.ToString(), new UTF8Encoding(true));
            AssetDatabase.Refresh();

            string msg = $"{seen.Count} 本ぶんを {SettingsPath} に保存しました";

            // ★名前が重なっていると、読み込みで**どちらに当たるか決まりません**。
            //   黙って片方だけ当てるより、名前を直してもらうほうが早い。
            if (dup.Count > 0)
                msg += $"\n  ⚠ 名前が重なっています: {string.Join(", ", dup)}"
                     + "\n    読み込みは同じ名前すべてに同じ値を当てます。分けたいなら名前を変えてください。";

            return msg;
        }

        public static string Load(IList<Transform> sources)
        {
            if (!System.IO.File.Exists(SettingsPath))
                return $"{SettingsPath} がありません（先に保存してください）";

            var table = new Dictionary<string, Row>();
            var lines = System.IO.File.ReadAllLines(SettingsPath, Encoding.UTF8);

            for (int i = 1; i < lines.Length; i++)          // 1 行目は見出し
            {
                var c = SplitCsv(lines[i]);
                if (c.Count < 3) continue;
                if (!float.TryParse(c[1], NumberStyles.Float, CultureInfo.InvariantCulture, out float v))
                    continue;

                var r = new Row { vol = Mathf.Clamp01(v), mute = c[2].Trim() == "1" };

                // 段の列は**後から足したので、無くても読めるようにする**。
                //   古い表を「壊れている」と扱うと、耳で合わせた音量まで捨てることになります。
                r.hasTier = false;
                if (c.Count >= 5) r.hasTier = TryTier(c[4], out r.tier);
                if (c.Count >= 6)
                    float.TryParse(c[5], NumberStyles.Float, CultureInfo.InvariantCulture, out r.radius);

                table[c[0]] = r;
            }

            int hit = 0, tiered = 0;
            var missed = new List<string>();

            foreach (var t in sources)
            {
                if (t == null) continue;
                var a = t.GetComponent<AudioSource>();
                if (a == null) continue;

                if (!table.TryGetValue(t.name, out var s)) { missed.Add(t.name); continue; }
                a.volume = s.vol;
                a.mute = s.mute;
                hit++;

                if (!s.hasTier) continue;

                // ★札が無ければ**付けます**。Excel 側で段を決められるようにするため。
                //   ただし「厳密・半径 0」＝既定と同じ行では付けません ──
                //   何もしていない音源に札が生えると、後で読む人が
                //   「誰かが意図して厳密にした」と読み違えます。
                var tag = t.GetComponent<SourceTierTag>();
                if (tag == null)
                {
                    if (s.tier == SourceTierTag.Tier.Exact && s.radius <= 0f) continue;
                    tag = Undo.AddComponent<SourceTierTag>(t.gameObject);
                }
                tag.tier = s.tier;
                tag.audibleRadius = s.radius;
                tiered++;
            }

            // 段を触ったなら掛け直させる。掛け直さないと表と実物がズレます。
            if (tiered > 0)
                foreach (var ap in Object.FindObjectsByType<SourceTierApplier>(
                             FindObjectsInactive.Include, FindObjectsSortMode.None))
                    ap.Reapply();

            string msg = $"{hit} 本に当てました（表には {table.Count} 行"
                       + (tiered > 0 ? $" / うち段を当てたのが {tiered} 本" : "") + "）";
            if (missed.Count > 0)
                msg += $"\n  ・表に無かった音源: {string.Join(", ", missed)}";
            return msg;
        }

        private struct Row
        {
            public float vol;
            public bool mute;
            public bool hasTier;
            public SourceTierTag.Tier tier;
            public float radius;
        }

        // ── 段の読み書き ──────────────────────────────────────
        //
        // ★日本語で書きます。Excel で開いたときに、そのまま読めて選べるように。
        //   英語の Exact/Simple/Virtual だと、表を触る人が意味を辞書引きすることになります。

        private static string TierName(Transform t)
        {
            var tag = t.GetComponent<SourceTierTag>();
            if (tag == null) return "厳密";
            switch (tag.tier)
            {
                case SourceTierTag.Tier.Simple: return "簡易";
                case SourceTierTag.Tier.Virtual: return "バーチャル";
                default: return "厳密";
            }
        }

        private static float RadiusOf(Transform t)
        {
            var tag = t.GetComponent<SourceTierTag>();
            return (tag != null) ? tag.audibleRadius : 0f;
        }

        private static bool TryTier(string s, out SourceTierTag.Tier tier)
        {
            switch (s.Trim())
            {
                case "厳密": tier = SourceTierTag.Tier.Exact; return true;
                case "簡易": tier = SourceTierTag.Tier.Simple; return true;
                case "バーチャル": tier = SourceTierTag.Tier.Virtual; return true;
                default: tier = SourceTierTag.Tier.Exact; return false;
            }
        }

        // ── ② 測定（書き出すだけ）────────────────────────────
        //
        // ★読み込む口は**作りません**。遮蔽や帯域はエンジンが解いた答えで、
        //   書き戻す先がありません。書き戻せる形にすると
        //   「表を直せば音が変わる」と誤解されます。

        public static string Export(IList<Transform> sources)
        {
            if (!AcousticFlowSceneDemo.Status.Valid)
                return "測定がまだありません（再生中に押してください）";

            var taps = AcousticFlowSceneDemo.Status.Taps;
            var occ = AcousticFlowSceneDemo.Status.Occlusion;

            var sb = new StringBuilder();
            sb.AppendLine("名前,遮蔽,生存,自由音場,経路数,"
                        + "125Hz,250Hz,500Hz,1kHz,2kHz,4kHz,こもり倍率");

            for (int i = 0; i < sources.Count; i++)
            {
                var t = sources[i];
                if (t == null) continue;

                var ts = (taps != null && i < taps.Length) ? taps[i] : null;
                float o = (occ != null && i < occ.Length) ? occ[i] : 0f;

                sb.Append(Csv(t.name)).Append(',')
                  .Append(F(o)).Append(',')
                  .Append(F(ts != null ? ts.SourceLevel : 0f)).Append(',')
                  .Append(F(ts != null ? ts.FreeFieldDirect : 0f)).Append(',')
                  .Append(ts != null ? ts.Count : 0).Append(',');

                for (int b = 0; b < 6; b++)
                {
                    float g = (ts != null && ts.BandGain != null && b < ts.BandGain.Length)
                            ? ts.BandGain[b] : 0f;
                    sb.Append(F(g)).Append(',');
                }

                // ★こもり倍率 ＝ 125Hz / 4kHz。**扉の掃きで見ている量と同じ定義**にする。
                //   別の定義をここで作ると、掃きの表と突き合わせられなくなります。
                float lo = (ts != null && ts.BandGain != null && ts.BandGain.Length > 5) ? ts.BandGain[0] : 0f;
                float hi = (ts != null && ts.BandGain != null && ts.BandGain.Length > 5) ? ts.BandGain[5] : 0f;
                sb.Append(hi > 1e-6f ? F(lo / hi) : "—").AppendLine();
            }

            string path = $"Assets/BellGame/SourceReport_{System.DateTime.Now:MMdd_HHmm}.csv";
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path));
            System.IO.File.WriteAllText(path, sb.ToString(), new UTF8Encoding(true));
            AssetDatabase.Refresh();
            return $"{path} に書き出しました（Excel でそのまま開けます）";
        }

        // ── CSV の細かいところ ────────────────────────────────

        private static string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);

        // 名前にカンマや引用符が入っても壊れないようにする。
        private static string Csv(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            if (s.IndexOfAny(new[] { ',', '"', '\n' }) < 0) return s;
            return "\"" + s.Replace("\"", "\"\"") + "\"";
        }

        private static List<string> SplitCsv(string line)
        {
            var outp = new List<string>();
            if (string.IsNullOrEmpty(line)) return outp;

            var cur = new StringBuilder();
            bool q = false;

            for (int i = 0; i < line.Length; i++)
            {
                char ch = line[i];
                if (q)
                {
                    if (ch != '"') { cur.Append(ch); continue; }
                    if (i + 1 < line.Length && line[i + 1] == '"') { cur.Append('"'); i++; continue; }
                    q = false;
                }
                else if (ch == '"') q = true;
                else if (ch == ',') { outp.Add(cur.ToString()); cur.Clear(); }
                else cur.Append(ch);
            }
            outp.Add(cur.ToString());
            return outp;
        }
    }
}
