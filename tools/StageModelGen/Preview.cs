// Preview.cs
// 生成したメッシュを SVG で描いて目視確認するためのもの。
// 画家のアルゴリズム（奥から順に塗る）＋平行光の平坦シェーディング。
// 形が壊れていないかを見るだけの用途で、見た目の品質は問わない。
using System.Globalization;
using System.Text;

namespace StageModelGen;

public static class Preview
{
    public static void WriteSvg(IReadOnlyList<(V3[] poly, string group)> faces,
                                string path, int width = 1100, int height = 760,
                                float yawDeg = 34f, float pitchDeg = 21f)
    {
        float yaw = yawDeg * MathF.PI / 180f, pitch = pitchDeg * MathF.PI / 180f;
        float cy = MathF.Cos(yaw), sy = MathF.Sin(yaw);
        float cp = MathF.Cos(pitch), sp = MathF.Sin(pitch);

        (float x, float y, float d) Project(V3 p)
        {
            float x1 = p.X * cy - p.Z * sy;
            float z1 = p.X * sy + p.Z * cy;
            float y2 = p.Y * cp - z1 * sp;
            float d = p.Y * sp + z1 * cp;
            return (x1, y2, d);
        }

        var light = new V3(-0.42f, 0.80f, -0.43f).Normalized;

        // 群ごとの基準色。
        var baseColor = new Dictionary<string, (int r, int g, int b)>
        {
            ["Walls"] = (168, 160, 146),
            ["Plinth"] = (150, 143, 130),
            ["Cornice"] = (150, 143, 130),
            ["Columns"] = (196, 188, 172),
            ["Roof"] = (122, 114, 106),
        };

        var items = new List<(float depth, string svg)>(faces.Count);
        float minX = float.MaxValue, maxX = float.MinValue, minY = float.MaxValue, maxY = float.MinValue;
        var projected = new List<((float x, float y)[] pts, float depth, string group, float shade)>(faces.Count);

        foreach ((V3[] poly, string group) in faces)
        {
            if (poly.Length < 3) continue;
            V3 n = V3.Cross(poly[1] - poly[0], poly[2] - poly[0]).Normalized;
            // 裏向きは描かない（面の巻き方向の確認も兼ねる）。
            var (_, _, dz) = Project(poly[0]);
            float facing = n.X * -sy * cp + n.Y * sp + n.Z * cy * cp;
            if (facing > 0f) continue;

            float shade = MathF.Max(0.18f, n.X * light.X + n.Y * light.Y + n.Z * light.Z);
            var pts = new (float x, float y)[poly.Length];
            float depth = 0f;
            for (int i = 0; i < poly.Length; i++)
            {
                var (px, py, pd) = Project(poly[i]);
                pts[i] = (px, py);
                depth += pd;
                if (px < minX) minX = px;
                if (px > maxX) maxX = px;
                if (py < minY) minY = py;
                if (py > maxY) maxY = py;
            }
            projected.Add((pts, depth / poly.Length, group, shade));
        }

        float spanX = MathF.Max(1e-3f, maxX - minX), spanY = MathF.Max(1e-3f, maxY - minY);
        float scale = MathF.Min((width - 60) / spanX, (height - 60) / spanY);
        float offX = (width - spanX * scale) * 0.5f - minX * scale;
        float offY = (height - spanY * scale) * 0.5f + maxY * scale;

        projected.Sort((a, b) => a.depth.CompareTo(b.depth));

        var ci = CultureInfo.InvariantCulture;
        var sb = new StringBuilder();
        sb.AppendLine($"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{width}\" height=\"{height}\" viewBox=\"0 0 {width} {height}\">");
        sb.AppendLine("<rect width=\"100%\" height=\"100%\" fill=\"#1b1d20\"/>");
        foreach (var (pts, _, group, shade) in projected)
        {
            var (r, g, b) = baseColor.TryGetValue(group, out var c) ? c : (170, 170, 170);
            int rr = (int)MathF.Min(255, r * shade), gg = (int)MathF.Min(255, g * shade), bb = (int)MathF.Min(255, b * shade);
            sb.Append("<polygon points=\"");
            for (int i = 0; i < pts.Length; i++)
            {
                float sx = pts[i].x * scale + offX;
                float sy2 = offY - pts[i].y * scale;
                sb.Append(sx.ToString("0.##", ci)).Append(',').Append(sy2.ToString("0.##", ci));
                if (i < pts.Length - 1) sb.Append(' ');
            }
            sb.AppendLine($"\" fill=\"rgb({rr},{gg},{bb})\"/>");
        }
        sb.AppendLine("</svg>");
        File.WriteAllText(path, sb.ToString());
    }
}
