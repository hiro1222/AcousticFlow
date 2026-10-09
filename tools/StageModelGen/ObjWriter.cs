// ObjWriter.cs
// OBJ / MTL を書き出すだけの小さな器。
//
// 面ごとに法線を出す（フラットシェーディング）。建築のブロックアウトなので
// スムーズシェーディングは要らず、柱の溝もエッジが立っていたほうが陰影が出る。
// UV は面の主軸で平面投影し、1 ワールドメートル = 1 タイルにしてある。
using System.Globalization;
using System.Text;

namespace StageModelGen;

public readonly record struct V3(float X, float Y, float Z)
{
    public static V3 operator +(V3 a, V3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static V3 operator -(V3 a, V3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static V3 operator *(V3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);

    public static V3 Cross(V3 a, V3 b) => new(
        a.Y * b.Z - a.Z * b.Y,
        a.Z * b.X - a.X * b.Z,
        a.X * b.Y - a.Y * b.X);

    public float Length => MathF.Sqrt(X * X + Y * Y + Z * Z);

    public V3 Normalized
    {
        get { float l = Length; return l < 1e-9f ? new V3(0, 1, 0) : new V3(X / l, Y / l, Z / l); }
    }
}

public sealed class ObjMesh
{
    private readonly List<V3> _v = new();
    private readonly List<V3> _vn = new();
    private readonly List<(float u, float v)> _vt = new();
    private readonly List<string> _lines = new();
    private string _group = "";
    private string _material = "";
    private bool _uv = true;

    // 同じ値を 2 度書かないための索引。草原のように面数が多いメッシュだと、
    // 座標を面ごとに書き出すと OBJ が 13MB を超える（両面に出す葉で全頂点が二重になる）。
    // 1mm（書き出す桁と同じ）で丸めて突き合わせる ── 見た目のメッシュにその精度は要らないし、
    // Unity の取り込みも weldVertices で結局まとめる。
    private readonly Dictionary<(int, int, int), int> _vIndex = new();
    private readonly Dictionary<(int, int, int), int> _vnIndex = new();
    private readonly Dictionary<(int, int), int> _vtIndex = new();

    private static (int, int, int) Key(V3 p, float q)
        => ((int)MathF.Round(p.X * q), (int)MathF.Round(p.Y * q), (int)MathF.Round(p.Z * q));

    private int VertexOf(V3 p)
    {
        var k = Key(p, 1000f);
        if (_vIndex.TryGetValue(k, out int i)) return i;
        _v.Add(p);
        return _vIndex[k] = _v.Count;
    }

    private int NormalOf(V3 n)
    {
        var k = Key(n, 10000f);
        if (_vnIndex.TryGetValue(k, out int i)) return i;
        _vn.Add(n);
        return _vnIndex[k] = _vn.Count;
    }

    private int UvOf(float u, float v)
    {
        var k = ((int)MathF.Round(u * 1000f), (int)MathF.Round(v * 1000f));
        if (_vtIndex.TryGetValue(k, out int i)) return i;
        _vt.Add((u, v));
        return _vtIndex[k] = _vt.Count;
    }

    public int TriangleCount { get; private set; }

    /// プレビュー描画用に面をそのまま控えておく（OBJ の中身には影響しない）。
    public List<(V3[] poly, string group)> Faces { get; } = new();

    /// uv:false にすると UV を書かない（`f v//vn` 形式）。
    ///
    /// テクスチャを持たない材質（色だけの草・花など）では UV が完全に無駄になる。
    /// しかも 1m=1タイルの平面投影は、幅 2cm の葉に貼ると画像の 2% を拾うだけで
    /// **どのみち意味のある UV にならない**。草原のように面数が多いメッシュだと
    /// vt の塊と f の桁で 1/3 近くを食うので、そこだけ落とす。
    public void Group(string name, string material, bool uv = true)
    {
        _group = name;
        _material = material;
        _uv = uv;
        _lines.Add($"g {name}");
        _lines.Add($"usemtl {material}");
    }

    /// 多角形を 1 面として足す（凸であること）。法線は最初の 3 点から求める。
    public void AddPolygon(params V3[] p) => AddPolygon(p, null);

    /// UV を明示して足す。uv は p と同じ長さ。
    ///
    /// 自動の平面投影は**面の主軸**に貼るので、曲がった川に使うと UV が世界軸を向く。
    /// テクスチャをスクロールさせても流れが川筋に沿わないので、そこだけ明示する。
    public void AddPolygon(V3[] p, (float u, float v)[] uv)
    {
        if (p.Length < 3) return;
        Faces.Add(((V3[])p.Clone(), _group));
        // ★Unity の OBJ 読み込みは右手系→左手系の変換で **X を反転する。**
        //   .meta に切る設定は無く、読み込みの仕様として必ず掛かる。
        //   打ち消さないと、**メッシュは左に描かれるのに、C# 側（草の除外・歩行の床・
        //   音響の箱）は右で計算する**という食い違いになる。
        //   実際それで、川の位置を 3 回取り違えた。
        //
        //   ここで X を反転し、**巻き方向も逆にする**（鏡像は面の裏表が入れ替わるため）。
        //   法線は反転後の点から計算するので、自動的に正しくなる。
        //   `Faces` には**反転前**を入れてある ── プレビューの SVG は Unity 座標で見たい。
        var mirrored = new V3[p.Length];
        for (int k = 0; k < p.Length; k++)
        {
            V3 s = p[p.Length - 1 - k];
            mirrored[k] = new V3(-s.X, s.Y, s.Z);
        }
        if (uv != null)
        {
            var mu = new (float u, float v)[uv.Length];
            for (int k = 0; k < uv.Length; k++) mu[k] = uv[uv.Length - 1 - k];
            uv = mu;
        }
        p = mirrored;

        V3 n = V3.Cross(p[1] - p[0], p[2] - p[0]).Normalized;

        // UV は法線の主軸で平面投影。1m = 1 タイル。
        float ax = MathF.Abs(n.X), ay = MathF.Abs(n.Y), az = MathF.Abs(n.Z);
        int axis = (ay >= ax && ay >= az) ? 1 : (ax >= az ? 0 : 2);

        int vnIndex = NormalOf(n);

        var sb = new StringBuilder("f");
        for (int k = 0; k < p.Length; k++)
        {
            V3 q = p[k];
            int vi = VertexOf(q);
            if (!_uv && uv == null)
            {
                sb.Append(CultureInfo.InvariantCulture, $" {vi}//{vnIndex}");
                continue;
            }
            (float u, float v) t = uv != null ? uv[k] : axis switch
            {
                1 => (q.X, q.Z),   // 上下面
                0 => (q.Z, q.Y),   // 左右面
                _ => (q.X, q.Y),   // 前後面
            };
            sb.Append(CultureInfo.InvariantCulture, $" {vi}/{UvOf(t.u, t.v)}/{vnIndex}");
        }
        _lines.Add(sb.ToString());
        TriangleCount += p.Length - 2;
    }

    /// 軸平行の箱。min/max はワールド座標。
    public void AddBox(V3 min, V3 max)
    {
        V3 a = new(min.X, min.Y, min.Z), b = new(max.X, min.Y, min.Z);
        V3 c = new(max.X, min.Y, max.Z), d = new(min.X, min.Y, max.Z);
        V3 e = new(min.X, max.Y, min.Z), f = new(max.X, max.Y, min.Z);
        V3 g = new(max.X, max.Y, max.Z), h = new(min.X, max.Y, max.Z);
        // 巻き方向は「外向き法線になる順」。cross(p1-p0, p2-p0) で確認済み。
        AddPolygon(a, b, c, d);   // 下（-Y）
        AddPolygon(e, h, g, f);   // 上（+Y）
        AddPolygon(a, e, f, b);   // -Z
        AddPolygon(d, c, g, h);   // +Z
        AddPolygon(a, d, h, e);   // -X
        AddPolygon(b, f, g, c);   // +X
    }

    public void Write(string objPath, string mtlName)
    {
        var sb = new StringBuilder();
        sb.AppendLine("# AcousticFlow / BellGame  ステージの見た目用メッシュ");
        sb.AppendLine("# 手続き生成（tools/StageModelGen）。音響には使わない ──");
        sb.AppendLine("# コライダーを付けずに置くこと。付けると音響へ二重登録される。");
        sb.AppendLine($"mtllib {mtlName}");
        var ci = CultureInfo.InvariantCulture;
        foreach (V3 p in _v) sb.AppendLine($"v {p.X.ToString("0.###", ci)} {p.Y.ToString("0.###", ci)} {p.Z.ToString("0.###", ci)}");
        foreach (var t in _vt) sb.AppendLine($"vt {t.u.ToString("0.###", ci)} {t.v.ToString("0.###", ci)}");
        foreach (V3 n in _vn) sb.AppendLine($"vn {n.X.ToString("0.####", ci)} {n.Y.ToString("0.####", ci)} {n.Z.ToString("0.####", ci)}");
        foreach (string l in _lines) sb.AppendLine(l);
        File.WriteAllText(objPath, sb.ToString());
    }
}
