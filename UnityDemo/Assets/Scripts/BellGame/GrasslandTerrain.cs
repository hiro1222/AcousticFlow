// GrasslandTerrain.cs
// 草原の地形の高さを式で返す。**草をこの上に生やすために要る。**
//
// ★`tools/StageModelGen` の BuildGrassMound / BuildGrassHills と**同じ式**。
//   片方だけ変えると草が地面に埋まる／浮くので、必ず両方直すこと。
//   共有すべき数字はここに全部集めてある（下の定数）。
//
// なぜコライダーへのレイキャストにしないか:
//   縁の丘には**わざとコライダーを付けていない**（付けると外接 147×147m ＝
//   格子 1,037 万で、セルが 0.375m 以上へ降格する）。レイが当たらないので、
//   高さは式から取るしかない。扉の丘だけは Mesh コライダーがあるが、
//   2 通りの取り方を持つほうが危ない。
//
// ノイズは生成器の `Noise.Fbm` をそのまま移してある。近似で済ませると、
// 稜線の斜面で最大 0.55m ずれて草が浮く。
using UnityEngine;

namespace BellGame
{
    public static class GrasslandTerrain
    {
        // ── 生成器と共有する数字 ──────────────────────────
        public const float Border = 30f;      // 遊べる範囲の半径。ここまでが明るい地面
        public const float MoundR = 10f;      // 扉の丘の裾
        public const float MoundFlat = 3f;    // 扉が立つ平場の半径
        public const float MoundH = 0.7f;     // 平場の高さ
        public const float Apron = 3f;        // ボーダーの外の平らな暗い帯
        public const float Rise = 9f;         // 稜線までの距離
        public const float Crest = 3.6f;      // 稜線の高さ
        public const float Roll = 10f;        // 稜線の外の転がり
        public const float Outer = 4.2f;      // 転がり切った高さ

        /// 地形の高さ。中心 (0,0) が扉。
        public static float Height(float x, float z)
        {
            float r = Mathf.Sqrt(x * x + z * z);

            // 扉の丘
            float mound = 0f;
            if (r < MoundR)
            {
                float t = Mathf.Clamp01((MoundR - r) / (MoundR - MoundFlat));
                mound = MoundH * t * t * (3f - 2f * t);
            }

            // 縁の丘
            float rim = 0f;
            float d = r - Border - Apron;
            if (d > 0f)
            {
                if (d <= Rise)
                {
                    float t = d / Rise;
                    rim = Crest * t * t * (3f - 2f * t);
                }
                else
                {
                    float t = Mathf.Clamp01((d - Rise) / Roll);
                    rim = Crest + (Outer - Crest) * t * t * (3f - 2f * t);
                }
            }

            // ★丘と川は**足さずに max**。生成器の BuildGrassField と同じ扱い。
            float rd = RiverDistance(x, z);
            float y = Mathf.Max(mound + rim, RiverBerm(rd));
            if (y <= 0.02f) return 0f;

            float amp = Mathf.Min(0.55f, y * 0.16f);
            if (mound > 0f) amp *= Mathf.Clamp01((r - MoundFlat) / 2f);   // 平場はノイズを切る
            amp *= Mathf.Clamp01((rd - RiverBed) / 1.5f);                 // 河床もノイズを切る
            float fade = Mathf.Clamp01(y / 0.6f);
            return y + Fbm(x, z, 0.035f) * amp * fade;
        }


        // ── 川の土手 ─────────────────────────────────────────
        /// ★生成器の `RiverBerm` と**同じ式**。片方だけ変えると草が土手に浮く／水面から生える。
        ///
        ///   掘り下げてはいけない（音響の地面の天面が y=0 なので、掘ると水の上を歩く）。
        ///   だから**土手を上げて相対的に窪ませる**。河床 0.10m ＞ 0。
        public const float RiverBed = 3.2f;     // 河床（水の下）の半幅
        public const float RiverTop = 6.4f;     // 土手の天端まで
        public const float RiverOut = 9.5f;     // 天端から野原へ戻り切るまで
        public const float RiverBedY = 0.10f, RiverCrestY = 1.15f;

        public static float RiverBerm(float d)
        {
            if (d <= RiverBed) return RiverBedY;
            if (d >= RiverOut) return 0f;
            if (d <= RiverTop)
            {
                float t = (d - RiverBed) / (RiverTop - RiverBed);
                return RiverBedY + (RiverCrestY - RiverBedY) * t * t * (3f - 2f * t);
            }
            float u = (d - RiverTop) / (RiverOut - RiverTop);
            return RiverCrestY * (1f - u * u * (3f - 2f * u));
        }
        // ── 川 ───────────────────────────────────────────────
        // ★生成器の `RiverControl` と**同じ制御点**。片方だけ変えると草が水面から生える。
        //   川は**左（-X）**。2026-08-21 確定。
        //   ★OBJ 側の X 反転は `ObjWriter` で打ち消してあるので、
        //     ここの符号と画面の左右は一致する（-X が左）。
        private static readonly Vector2[] RiverControl =
        {
            new Vector2( -8f,  48f), new Vector2(-12f,  34f), new Vector2(-16f,  18f),
            new Vector2(-19f,   2f), new Vector2(-22f, -12f), new Vector2(-26f, -25f),
            new Vector2(-18.5f, -37.5f), new Vector2(-10f, -48f),
        };

        private static Vector2[] _riverPath;

        private static Vector2[] RiverPath()
        {
            if (_riverPath != null) return _riverPath;
            var pts = new System.Collections.Generic.List<Vector2>();
            var c = RiverControl;
            for (int i = 1; i < c.Length - 2; i++)
                for (int s = 0; s < 16; s++)
                {
                    float t = s / 16f, t2 = t * t, t3 = t2 * t;
                    pts.Add((c[i] * 2f + (c[i + 1] - c[i - 1]) * t
                             + (c[i - 1] * 2f - c[i] * 5f + c[i + 1] * 4f - c[i + 2]) * t2
                             + (c[i] * 3f - c[i - 1] - c[i + 1] * 3f + c[i + 2]) * t3) * 0.5f);
                }
            pts.Add(c[c.Length - 2]);
            return _riverPath = pts.ToArray();
        }

        // ── 川までの距離を速く引くための索引 ──────────────────
        //
        // ★ここが遅いと**起動が数秒止まる。**
        //   草は約 448,000 本（半径 36m × 密度 110）あり、1 本ごとにここを通る。
        //   素朴に全線分（111 本）を舐めると 5,000 万回になる。
        //   z で棚に分けて、その棚の線分だけ見る（1 桁減る）。
        //   さらに x でも粗く弾く（川の帯は 72m の野原のうち 24m しかない）。
        //
        // ★棚の余白 Margin は**知りたい距離の上限**。土手は 9.5m までしか使わないので
        //   12m あれば足りる。それより遠い点は Far を返す ── RiverBerm(Far)=0 で正しい。
        private const float BucketH = 4f, Margin = 12f, Far = 999f;
        private static int[][] _zBuckets;
        private static float[] _bucketMinX, _bucketMaxX;
        private static float _zLo, _zHi;

        private static void BuildIndex()
        {
            var p = RiverPath();
            _zLo = float.MaxValue; _zHi = float.MinValue;
            foreach (var q in p) { if (q.y < _zLo) _zLo = q.y; if (q.y > _zHi) _zHi = q.y; }
            _zLo -= Margin; _zHi += Margin;

            int nb = Mathf.Max(1, Mathf.CeilToInt((_zHi - _zLo) / BucketH));
            var lists = new System.Collections.Generic.List<int>[nb];
            _bucketMinX = new float[nb];
            _bucketMaxX = new float[nb];
            for (int b = 0; b < nb; b++)
            {
                lists[b] = new System.Collections.Generic.List<int>();
                _bucketMinX[b] = float.MaxValue;
                _bucketMaxX[b] = float.MinValue;
            }
            for (int i = 0; i < p.Length - 1; i++)
            {
                float z0 = Mathf.Min(p[i].y, p[i + 1].y) - Margin;
                float z1 = Mathf.Max(p[i].y, p[i + 1].y) + Margin;
                int b0 = Mathf.Clamp(Mathf.FloorToInt((z0 - _zLo) / BucketH), 0, nb - 1);
                int b1 = Mathf.Clamp(Mathf.FloorToInt((z1 - _zLo) / BucketH), 0, nb - 1);
                for (int b = b0; b <= b1; b++)
                {
                    lists[b].Add(i);
                    _bucketMinX[b] = Mathf.Min(_bucketMinX[b], Mathf.Min(p[i].x, p[i + 1].x));
                    _bucketMaxX[b] = Mathf.Max(_bucketMaxX[b], Mathf.Max(p[i].x, p[i + 1].x));
                }
            }
            _zBuckets = new int[nb][];
            for (int b = 0; b < nb; b++) _zBuckets[b] = lists[b].ToArray();
        }

        /// 川の中心線までの水平距離。草を水面に生やさないために使う。
        ///   ★12m より遠いことだけが分かればよい所では Far(999) を返す。
        public static float RiverDistance(float x, float z)
        {
            if (_zBuckets == null) BuildIndex();
            if (z < _zLo || z > _zHi) return Far;
            int b = Mathf.Clamp(Mathf.FloorToInt((z - _zLo) / BucketH), 0, _zBuckets.Length - 1);
            var seg = _zBuckets[b];
            if (seg.Length == 0) return Far;
            if (x < _bucketMinX[b] - Margin || x > _bucketMaxX[b] + Margin) return Far;

            var p = RiverPath();
            var q = new Vector2(x, z);
            float best = Far;
            for (int k = 0; k < seg.Length; k++)
            {
                int i = seg[k];
                Vector2 a = p[i], d = p[i + 1] - a;
                float len2 = d.sqrMagnitude;
                float t = len2 < 1e-6f ? 0f : Mathf.Clamp01(Vector2.Dot(q - a, d) / len2);
                float dist = (a + d * t - q).magnitude;
                if (dist < best) best = dist;
            }
            return best;
        }

        /// 地形の法線。斜面に生えた草を寝かせるのに使う。
        ///
        ///   ★ノイズを含めない**滑らかな形だけ**から求める。
        ///     法線は「草を何度傾けるか」にしか使わないので、細かい凹凸は要らない。
        ///     Height をそのまま 4 回呼ぶと、1 本あたりノイズ評価が 5 回になり、
        ///     60 万本の組み立てが 5 倍遅くなる（読み込み時に数秒固まる）。
        public static Vector3 Normal(float x, float z, float eps = 0.35f)
        {
            float hx = Smooth(x + eps, z) - Smooth(x - eps, z);
            float hz = Smooth(x, z + eps) - Smooth(x, z - eps);
            return new Vector3(-hx, 2f * eps, -hz).normalized;
        }

        /// ノイズ抜きの高さ（丘の形だけ）。法線用。
        public static float Smooth(float x, float z)
        {
            float r = Mathf.Sqrt(x * x + z * z);

            float mound = 0f;
            if (r < MoundR)
            {
                float t = Mathf.Clamp01((MoundR - r) / (MoundR - MoundFlat));
                mound = MoundH * t * t * (3f - 2f * t);
            }

            float rim = 0f;
            float d = r - Border - Apron;
            if (d > 0f)
            {
                if (d <= Rise)
                {
                    float t = d / Rise;
                    rim = Crest * t * t * (3f - 2f * t);
                }
                else
                {
                    float t = Mathf.Clamp01((d - Rise) / Roll);
                    rim = Crest + (Outer - Crest) * t * t * (3f - 2f * t);
                }
            }
            // ★Height と同じく max。斜面の草を寝かせる法線がここから出る。
            return Mathf.Max(mound + rim, RiverBerm(RiverDistance(x, z)));
        }

        // ── 生成器の Noise.cs から移した値ノイズ ──────────
        // ★同じ結果を返すこと。定数を 1 つでも変えると地形とズレる。
        private static float Hash(int x, int y, int z)
        {
            int h = x * 374761393 + y * 668265263 + z * 1274126177;
            h = (h ^ (h >> 13)) * 1274126177;
            h ^= h >> 16;
            return (h & 0x7fffffff) / (float)0x7fffffff * 2f - 1f;
        }

        private static float Value(float x, float y, float z)
        {
            int xi = Mathf.FloorToInt(x), yi = Mathf.FloorToInt(y), zi = Mathf.FloorToInt(z);
            float fx = Smooth(x - xi), fy = Smooth(y - yi), fz = Smooth(z - zi);

            float c000 = Hash(xi, yi, zi), c100 = Hash(xi + 1, yi, zi);
            float c010 = Hash(xi, yi + 1, zi), c110 = Hash(xi + 1, yi + 1, zi);
            float c001 = Hash(xi, yi, zi + 1), c101 = Hash(xi + 1, yi, zi + 1);
            float c011 = Hash(xi, yi + 1, zi + 1), c111 = Hash(xi + 1, yi + 1, zi + 1);

            float x00 = Mathf.Lerp(c000, c100, fx), x10 = Mathf.Lerp(c010, c110, fx);
            float x01 = Mathf.Lerp(c001, c101, fx), x11 = Mathf.Lerp(c011, c111, fx);
            return Mathf.Lerp(Mathf.Lerp(x00, x10, fy), Mathf.Lerp(x01, x11, fy), fz);

            static float Smooth(float t) => t * t * (3f - 2f * t);
        }

        private static float Fbm(float x, float z, float freq, int octaves = 3)
        {
            float sum = 0f, amp = 1f, norm = 0f, f = freq;
            for (int i = 0; i < octaves; i++)
            {
                sum += Value(x * f, 0f, z * f) * amp;
                norm += amp;
                amp *= 0.5f;
                f *= 2.1f;
            }
            return norm > 0f ? sum / norm : 0f;
        }
    }
}
