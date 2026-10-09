// Noise.cs
// 依存なしの値ノイズ（fBm）。岩壁の凹凸と雪原の起伏に使う。
//
// 決定的であること。乱数だと生成のたびに形が変わり、
// 「見た目を直したら音が変わったように聞こえる」錯覚のもとになる
// （実際には音響は箱が持っているので変わらないが、切り分けが面倒になる）。
namespace StageModelGen;

public static class Noise
{
    private static float Hash(int x, int y, int z)
    {
        int h = x * 374761393 + y * 668265263 + z * 1274126177;
        h = (h ^ (h >> 13)) * 1274126177;
        h ^= h >> 16;
        return (h & 0x7fffffff) / (float)0x7fffffff * 2f - 1f;   // -1..1
    }

    private static float Smooth(float t) => t * t * (3f - 2f * t);

    /// 3D 値ノイズ。-1..1。
    public static float Value(float x, float y, float z)
    {
        int xi = (int)MathF.Floor(x), yi = (int)MathF.Floor(y), zi = (int)MathF.Floor(z);
        float fx = Smooth(x - xi), fy = Smooth(y - yi), fz = Smooth(z - zi);

        float Lerp(float a, float b, float t) => a + (b - a) * t;

        float c000 = Hash(xi, yi, zi), c100 = Hash(xi + 1, yi, zi);
        float c010 = Hash(xi, yi + 1, zi), c110 = Hash(xi + 1, yi + 1, zi);
        float c001 = Hash(xi, yi, zi + 1), c101 = Hash(xi + 1, yi, zi + 1);
        float c011 = Hash(xi, yi + 1, zi + 1), c111 = Hash(xi + 1, yi + 1, zi + 1);

        float x00 = Lerp(c000, c100, fx), x10 = Lerp(c010, c110, fx);
        float x01 = Lerp(c001, c101, fx), x11 = Lerp(c011, c111, fx);
        float y0 = Lerp(x00, x10, fy), y1 = Lerp(x01, x11, fy);
        return Lerp(y0, y1, fz);
    }

    /// オクターブを重ねた fBm。-1..1 程度。
    public static float Fbm(V3 p, float freq, int octaves = 3, float gain = 0.5f, float lacunarity = 2.1f)
    {
        float sum = 0f, amp = 1f, norm = 0f, f = freq;
        for (int i = 0; i < octaves; i++)
        {
            sum += Value(p.X * f, p.Y * f, p.Z * f) * amp;
            norm += amp;
            amp *= gain;
            f *= lacunarity;
        }
        return norm > 0f ? sum / norm : 0f;
    }
}

/// 決定的な擬似乱数（xorshift32）。散らばりの「ばらつき」に使う。
///
/// Noise と同じ理由で System.Random を使わない ── 種を固定しても実装依存で
/// 値が変わりうるし、呼ぶ順に依存する。ここは自前で閉じておく。
public struct Rng
{
    private uint _s;

    public Rng(int seed) { _s = (uint)seed * 2654435761u + 1013904223u; if (_s == 0) _s = 1; }

    /// 0..1
    public float Next()
    {
        _s ^= _s << 13; _s ^= _s >> 17; _s ^= _s << 5;
        return (_s & 0xffffffu) / (float)0x1000000;
    }

    public float Range(float a, float b) => a + (b - a) * Next();
}
