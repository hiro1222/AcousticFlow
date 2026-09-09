/* Flow/aperture.h ── 扉の開口の解析（段 5）
 *
 * ■ 役割
 *   2 つの問いに**解析**で答える（追記 A「扉の開口は解析、統計はレイ」）。レイの揺れを扉に通さない。
 *   1) discVisibility: 幅を持つ音源（円盤）がリスナーからどれだけ見えるか（0..1）。直接音の「見通し 1/0」を割合に置き換える。
 *      開口（戸口）も遮蔽物（押した箱）も同じ式で扱う（地図 8 章）。
 *   2) openingCoverage: 戸口の矩形を板（扉）がどれだけ覆っているか（0..1）。プローブの開口の吸音率 a と部屋間の混合に使う。
 *
 * ■ 中の仕組み
 *   1) 円盤: 音源の位置を中心に、リスナーへ正対する半径 r の円盤。明るさはガウス（σ = r/2、縁で 2σ）。
 *      ★一様な円盤だと縁が硬く、縁が影に入る瞬間の傾きが急になる。ガウスなら中心が重く、縁は柔らかい。
 *      円盤を水平な走査線（既定 9 本）で切り、各線の上で「影の区間」を求める。
 *        影の区間 = 線上の点 P で、線分 L→P が箱に当たる t の集合。凸な箱の影は線上で 1 つの区間。
 *        粗い標本（16 点）で明暗の切り替わりを見つけ、二分探索（12 回）で境界を 1/65536 の精度に詰める。
 *        箱ごとの区間を合併し、明るい隙間のガウス重み付き長さ（erf で解析）を足す。
 *      ★これが「標本点の階段を解析積分に」（旧 direct-penumbra-scanline）の新コア版。標本は境界を探すためだけに使い、
 *        面積は erf で出すので、扉の縁が動いても値は連続（段差は探索精度の 1/65536）。
 *      点音源（r=0）は線分 1 本の 0/1。見込み角で点に落ちた遠い音源は自然にこちらへ。
 *      影を作った箱ごとの寄与から、遮っている物の透過率 τ_b（寄与の重み付き平均）も返す。遮られた分はこの τ で板を通る。
 *   2) 板の覆い: 板の 8 頂点を戸口の面に法線方向で正射影 → 凸包 → 矩形で切り抜き → 面積比。
 *      正射影なので視点に依らない（部屋の性質）。閉じた扉は 1、90° に開いた扉は 厚み/幅 ≒ 0.03。
 *      板のどこかが面から 10 cm 以内にある物だけ数える（蝶番の縁は必ず面にある。隣の部屋の板は数えない）。
 *
 * ■ 繋がり
 *   受ける: Surfaces（箱。動く物も含む）、リスナーと音源の位置、音源の実効の幅（emitter.effectiveRadius）、
 *           rooms::Aperture（部屋グラフの戸口の矩形）。
 *   渡す:   見通しの割合と τ を distribute へ（直接 = 自由音場 × 割合、透過 = 自由音場 × (1−割合) × τ）。
 *           覆いの割合を world → probe（applyOpenings）へ。
 *
 * ■ 退けた書き方
 *   ・箱の影を透視投影の多角形で出す: 箱がリスナーの後ろや円盤の向こうにまたがると投影が破綻する（無限遠）。
 *     線分の当たりで境界を探すほうが、どんな配置でも同じ式で済む。
 *   ・標本点の多数決（256 点）: 段差は 1/256 で聞こえないが、決めごと「扉は解析」に反する。境界を詰めるのは同じ手間。
 *   ・覆いを透視で（音源から見て）出す: 部屋の性質（RT60）が音源の位置で変わってしまう。正射影で 1 つの答え。
 *
 * ■ 壊れる所
 *   ・走査線の粗い標本より細い影（3 cm の板を真横から）は見落とす。円盤の 1/16 未満なので割合の誤差も 1/16 未満。
 *   ・σ を r にする（縁で 1σ）と縁が重くなり、扉の縁が円盤に入った瞬間の傾きが急になる。r/2 が旧実装の実測どおり滑らか。
 *   ・板の判定の 10 cm を 0 にすると、板が面からわずかに浮いた（clearance）だけで数えなくなり、閉めても RT60 が伸びない。
 */
#ifndef ACOUSTICFLOW_FLOW_APERTURE_H
#define ACOUSTICFLOW_FLOW_APERTURE_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/room_graph.h"
#include "Core/vec3.h"
#include "Flow/surfaces.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

struct Visibility {
    float visible = 1.0f;                 // 見える割合 0..1
    float shadowTau6[kNumBands] = {1, 1, 1, 1, 1, 1};   // 遮っている物の透過率（寄与の重み付き平均）。遮る物が無ければ 1
    int   shadowers = 0;                  // 影を落とした箱の数
};

namespace detail {
inline double gaussCdf(double x, double sigma) { return 0.5 * (1.0 + std::erf(x / (sigma * 1.41421356237))); }
struct Interval { double a, b; };
/// 線分 L→P(x) の明暗の境界を二分探索で詰める。lit(x0)=lit0、lit(x1)=!lit0 が前提。
inline double bisect(const Surfaces& surf, int box, const Vec3& L, const Vec3& O, const Vec3& U, double x0, double x1, bool lit0) {
    for (int k = 0; k < 12; ++k) {
        const double xm = 0.5 * (x0 + x1);
        const bool litM = !segmentIntersectsObb(L, O + U * static_cast<float>(xm), surf.at(box).obb);
        if (litM == lit0) x0 = xm; else x1 = xm;
    }
    return 0.5 * (x0 + x1);
}
}  // namespace detail

/// 音源の円盤（半径 r、リスナーに正対、ガウス σ=r/2）がリスナー L から見える割合。
inline Visibility discVisibility(const Surfaces& surf, const MaterialTable& mats, const Vec3& L, const Vec3& S, float r,
                                 int lines = 9, int coarse = 16) {
    Visibility out;
    const Vec3 toL = L - S;
    const float dist = length(toL);
    if (dist <= kEps) return out;
    // 円盤の候補になる箱: リスナーと円盤を含む箱の範囲に重なる物だけ（他は影を落とせない）
    std::vector<int> cand;
    {
        Aabb reach = Aabb::fromCenterHalfExtents(L, Vec3(0, 0, 0));
        const Vec3 lo(std::min(L.x, S.x) - r, std::min(L.y, S.y) - r, std::min(L.z, S.z) - r);
        const Vec3 hi(std::max(L.x, S.x) + r, std::max(L.y, S.y) + r, std::max(L.z, S.z) + r);
        reach.min = lo; reach.max = hi;
        for (int i = 0; i < surf.count(); ++i) {
            const Surface& s = surf.at(i);
            if (!s.active) continue;
            const Aabb bb = rooms::obbBounds(s.obb);
            if (bb.max.x < reach.min.x || bb.min.x > reach.max.x || bb.max.y < reach.min.y || bb.min.y > reach.max.y ||
                bb.max.z < reach.min.z || bb.min.z > reach.max.z) continue;
            cand.push_back(i);
        }
    }
    if (cand.empty()) return out;
    // 点音源
    if (r <= 1e-4f) {
        for (int i : cand) {
            if (segmentIntersectsObb(L, S, surf.at(i).obb)) {
                out.visible = 0.0f; ++out.shadowers;
                const AcousticMaterial& m = mats.get(surf.at(i).material);
                for (int b = 0; b < kNumBands; ++b) out.shadowTau6[b] *= splitAt(m, b).transmit;   // 複数なら積
            }
        }
        return out;
    }
    // 円盤の基底
    const Vec3 n = toL * (1.0f / dist);
    const Vec3 t = (std::fabs(n.y) < 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 U = normalized(cross(t, n));       // 水平
    const Vec3 V = cross(n, U);                    // 垂直
    const double sigma = r * 0.5;
    double num = 0.0, den = 0.0;
    std::vector<double> shadowWeight(cand.size(), 0.0);
    std::vector<detail::Interval> ivs;
    for (int k = 0; k < lines; ++k) {
        const double y = -r + (k + 0.5) * (2.0 * r / lines);
        const double halfW = std::sqrt(std::max(0.0, static_cast<double>(r) * r - y * y));
        if (halfW <= 1e-6) continue;
        const double wy = std::exp(-y * y / (2.0 * sigma * sigma));
        const Vec3 O = S + V * static_cast<float>(y);
        const double total = detail::gaussCdf(halfW, sigma) - detail::gaussCdf(-halfW, sigma);
        ivs.clear();
        for (std::size_t ci = 0; ci < cand.size(); ++ci) {
            const int i = cand[ci];
            bool lit[32]; double xs[32];
            const int np = std::min(coarse, 31) + 1;
            bool anyShadow = false;
            for (int j = 0; j < np; ++j) {
                xs[j] = -halfW + (2.0 * halfW) * j / (np - 1);
                lit[j] = !segmentIntersectsObb(L, O + U * static_cast<float>(xs[j]), surf.at(i).obb);
                if (!lit[j]) anyShadow = true;
            }
            if (!anyShadow) continue;
            // 影の区間を組み立てる（明→暗で開き、暗→明で閉じる。境界は二分探索）
            double a = lit[0] ? 0.0 : -halfW; bool open = !lit[0];
            for (int j = 0; j + 1 < np; ++j) {
                if (lit[j] == lit[j + 1]) continue;
                const double xb = detail::bisect(surf, i, L, O, U, xs[j], xs[j + 1], lit[j]);
                if (lit[j]) { a = xb; open = true; }
                else { ivs.push_back({a, xb}); shadowWeight[ci] += (detail::gaussCdf(xb, sigma) - detail::gaussCdf(a, sigma)) * wy; open = false; }
            }
            if (open) { ivs.push_back({a, halfW}); shadowWeight[ci] += (detail::gaussCdf(halfW, sigma) - detail::gaussCdf(a, sigma)) * wy; }
        }
        // 合併して明るい隙間を足す
        double litW = total;
        if (!ivs.empty()) {
            std::sort(ivs.begin(), ivs.end(), [](const detail::Interval& p, const detail::Interval& q) { return p.a < q.a; });
            double ca = ivs[0].a, cb = ivs[0].b, shadowW = 0.0;
            for (std::size_t j = 1; j < ivs.size(); ++j) {
                if (ivs[j].a <= cb) { cb = std::max(cb, ivs[j].b); continue; }
                shadowW += detail::gaussCdf(cb, sigma) - detail::gaussCdf(ca, sigma);
                ca = ivs[j].a; cb = ivs[j].b;
            }
            shadowW += detail::gaussCdf(cb, sigma) - detail::gaussCdf(ca, sigma);
            litW = std::max(0.0, total - shadowW);
        }
        num += wy * litW; den += wy * total;
    }
    out.visible = (den > 0.0) ? static_cast<float>(std::min(1.0, std::max(0.0, num / den))) : 1.0f;
    // 遮っている物の τ（寄与の重み付き平均）
    double wsum = 0.0; double tau[kNumBands] = {};
    for (std::size_t ci = 0; ci < cand.size(); ++ci) {
        if (shadowWeight[ci] <= 0.0) continue;
        ++out.shadowers; wsum += shadowWeight[ci];
        const AcousticMaterial& m = mats.get(surf.at(cand[ci]).material);
        for (int b = 0; b < kNumBands; ++b) tau[b] += shadowWeight[ci] * splitAt(m, b).transmit;
    }
    if (wsum > 0.0) for (int b = 0; b < kNumBands; ++b) out.shadowTau6[b] = static_cast<float>(tau[b] / wsum);
    return out;
}

// ── 板の覆い（正射影 → 凸包 → 矩形で切り抜き）──
namespace detail {
struct P2 { double x, y; };
inline double cross2(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
inline std::vector<P2> convexHull(std::vector<P2> p) {
    std::sort(p.begin(), p.end(), [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    const std::size_t n = p.size();
    if (n < 3) return p;
    std::vector<P2> h(2 * n);
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) { while (k >= 2 && cross2(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
    for (std::size_t i = n - 1, t = k + 1; i > 0; --i) { while (k >= t && cross2(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k; h[k++] = p[i - 1]; }
    h.resize(k - 1);
    return h;
}
/// 半平面 (inside(p) が真の側) で多角形を切る（Sutherland–Hodgman の 1 辺）。
template <class F, class G>
inline std::vector<P2> clipHalf(const std::vector<P2>& poly, F inside, G intersect) {
    std::vector<P2> out;
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const P2& cur = poly[i]; const P2& prev = poly[(i + n - 1) % n];
        const bool ci = inside(cur), pi = inside(prev);
        if (ci) { if (!pi) out.push_back(intersect(prev, cur)); out.push_back(cur); }
        else if (pi) out.push_back(intersect(prev, cur));
    }
    return out;
}
inline double polyArea(const std::vector<P2>& p) {
    double a = 0.0; const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) { const P2& u = p[i]; const P2& v = p[(i + 1) % n]; a += u.x * v.y - v.x * u.y; }
    return 0.5 * std::fabs(a);
}
}  // namespace detail

/// 戸口の矩形を板が覆う割合（0..1）。板のどこかが面から nearM 以内にある物だけ数える。
inline float openingCoverage(const rooms::Aperture& ap, const Obb& leaf, float nearM = 0.1f) {
    using detail::P2;
    const Vec3 n = normalized(ap.normal);
    std::vector<P2> pts; pts.reserve(8);
    float nearest = 1e9f;
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f, sz = (i & 4) ? 1.0f : -1.0f;
        const Vec3 c = leaf.center + leaf.axisX * (leaf.halfExtents.x * sx) + leaf.axisY * (leaf.halfExtents.y * sy) + leaf.axisZ * (leaf.halfExtents.z * sz);
        const Vec3 d = c - ap.rectCenter;
        nearest = std::min(nearest, std::fabs(dot(d, n)));
        pts.push_back({static_cast<double>(dot(d, ap.axisU)), static_cast<double>(dot(d, ap.axisV))});
    }
    if (nearest > nearM) return 0.0f;
    std::vector<P2> poly = detail::convexHull(pts);
    if (poly.size() < 3) return 0.0f;
    const double hu = ap.halfU, hv = ap.halfV;
    auto lerp = [](const P2& a, const P2& b, double t) { return P2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; };
    poly = detail::clipHalf(poly, [&](const P2& p) { return p.x >= -hu; }, [&](const P2& a, const P2& b) { return lerp(a, b, (-hu - a.x) / (b.x - a.x)); });
    if (poly.size() < 3) return 0.0f;
    poly = detail::clipHalf(poly, [&](const P2& p) { return p.x <= hu; }, [&](const P2& a, const P2& b) { return lerp(a, b, (hu - a.x) / (b.x - a.x)); });
    if (poly.size() < 3) return 0.0f;
    poly = detail::clipHalf(poly, [&](const P2& p) { return p.y >= -hv; }, [&](const P2& a, const P2& b) { return lerp(a, b, (-hv - a.y) / (b.y - a.y)); });
    if (poly.size() < 3) return 0.0f;
    poly = detail::clipHalf(poly, [&](const P2& p) { return p.y <= hv; }, [&](const P2& a, const P2& b) { return lerp(a, b, (hv - a.y) / (b.y - a.y)); });
    if (poly.size() < 3) return 0.0f;
    const double area = detail::polyArea(poly), rect = 4.0 * hu * hv;
    return (rect > 1e-9) ? static_cast<float>(std::min(1.0, std::max(0.0, area / rect))) : 0.0f;
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_APERTURE_H
