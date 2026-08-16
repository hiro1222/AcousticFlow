// aperture_fresnel.h — 開口を通るエネルギーを「フレネルゾーンのうちどれだけ開いているか」で出す。
//
// なぜこれが要るか:
//   幾何音響は「レイ」と「稜線」しか知らず、**開口面積という概念を持たない**。
//   前川の式は半無限スクリーンの実験式で、エネルギー保存から導かれたものではないので、
//   「隙間をどれだけのエネルギーが通るか」を稜線探索から絞り出そうとしても出てこない
//   （実際6回失敗した）。ここは波動側の量として別に持つ。
//
// 何を計算するか:
//   遮蔽物の面の上で、リスナー↔音源を結ぶ線の周りに**フレネルゾーンの円**を置き、
//   その円のうち塞がっていない面積の割合を帯域ごとに返す。
//
//   ゾーン半径 r = √(λ d₁ d₂ / (d₁+d₂)) は**波長に依存する**。
//     L,S が各4m なら 125Hz で 2.34m、4kHz で 0.41m。
//   だから同じ 7cm の隙間でも、4kHz はゾーンの一部を占めるので通り、
//   125Hz はゾーンのごく一部しか占めないのでほとんど通らない。
//   ＝「**狭い隙間は高域だけ通す／開くと音色が開く**」が構成から出てくる。
//   現実の扉の開閉を収録して測った傾向（開−閉の差が 125Hz +2.6dB / 1kHz +10.3dB）と合う。
//
// なぜ連続なのか（これが導入の主目的）:
//   ・面積は多角形の頂点座標の連続関数。扉が回れば断面の頂点が連続に動くので面積も連続
//   ・**x 方向は標本化しない**。各行で遮蔽物の断面が作る区間を解析的に求めて引くので、
//     6.5cm の隙間でも 1mm の隙間でも取りこぼさない
//     （標本化していた前の実装は、粗い刻みより細い隙間を見落として崖を作っていた）
//   ・レイを1本も撃たない。純粋な2次元幾何なので安い
//
// 適用の限界:
//   キルヒホッフ近似は開口が波長より十分大きいときに精度が出る。125Hz(λ=2.7m)に対して
//   1.2m の戸口は「大きい」とは言えないので、低域では誤差が出る。ただし目的は絶対精度では
//   なく「開き具合が連続に音へ出ること」なので、形が合っていればよい（絶対値は演出で決める）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "Core/aabb.h"
#include "Core/vec3.h"

namespace acoustic {
namespace fresnel {

// 平面と OBB の断面（凸多角形）。平面座標 (u,v) で最大 6 頂点。頂点数を返す。
//   箱の12稜線と平面の交点を集めて、重心まわりの角度で並べる。単純だが凸性が保証される。
inline int obbPlaneSection(const Obb& b, const Vec3& planePt, const Vec3& n,
                           const Vec3& u, const Vec3& v, float outU[8], float outV[8]) {
    // 箱の8頂点。
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sy = (i & 2) ? 1.0f : -1.0f;
        const float sz = (i & 4) ? 1.0f : -1.0f;
        c[i] = b.center + b.axisX * (b.halfExtents.x * sx)
                        + b.axisY * (b.halfExtents.y * sy)
                        + b.axisZ * (b.halfExtents.z * sz);
    }
    // 12稜線（頂点番号の組）。
    static const int kEdge[12][2] = {
        {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7}
    };
    float px[12], py[12];
    int n2 = 0;
    for (int e = 0; e < 12; ++e) {
        const Vec3& a = c[kEdge[e][0]];
        const Vec3& d = c[kEdge[e][1]];
        const float da = dot(a - planePt, n);
        const float db = dot(d - planePt, n);
        if ((da > 0.0f) == (db > 0.0f)) continue;      // 平面を跨がない
        const float den = da - db;
        if (std::fabs(den) < 1e-9f) continue;
        const float t = da / den;
        const Vec3 p = a + (d - a) * t;
        if (n2 < 12) {
            px[n2] = dot(p - planePt, u);
            py[n2] = dot(p - planePt, v);
            ++n2;
        }
    }
    if (n2 < 3) return 0;

    // 重心まわりの角度で並べ替える（凸多角形になる）。
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < n2; ++i) { cx += px[i]; cy += py[i]; }
    cx /= n2; cy /= n2;
    int idx[12];
    for (int i = 0; i < n2; ++i) idx[i] = i;
    std::sort(idx, idx + n2, [&](int a, int b2) {
        return std::atan2(py[a] - cy, px[a] - cx) < std::atan2(py[b2] - cy, px[b2] - cx);
    });
    const int outN = std::min(n2, 8);
    for (int i = 0; i < outN; ++i) { outU[i] = px[idx[i]]; outV[i] = py[idx[i]]; }
    return outN;
}

// 三角形と平面の交線。交わらなければ false。交われば線分の両端を平面座標で返す。
//   メッシュの断面はこれを集めて作る。境界箱で代用してはいけない ── 戸口の空いた
//   メッシュ壁を「隙間のない一枚板」として扱ってしまい、回折が 1/20 に潰れる（実測）。
inline bool trianglePlaneSegment(const Vec3& a, const Vec3& b, const Vec3& c,
                                 const Vec3& planePt, const Vec3& n,
                                 const Vec3& u, const Vec3& v,
                                 float outU[2], float outV[2]) {
    const Vec3 p[3] = {a, b, c};
    const float d[3] = {dot(a - planePt, n), dot(b - planePt, n), dot(c - planePt, n)};
    int k = 0;
    for (int i = 0; i < 3 && k < 2; ++i) {
        const int j = (i + 1) % 3;
        if ((d[i] > 0.0f) == (d[j] > 0.0f)) continue;
        const float den = d[i] - d[j];
        if (std::fabs(den) < 1e-9f) continue;
        const Vec3 q = p[i] + (p[j] - p[i]) * (d[i] / den);
        outU[k] = dot(q - planePt, u);
        outV[k] = dot(q - planePt, v);
        ++k;
    }
    return k == 2;
}

// 凸多角形を y=const で切ったときの x 区間。交わらなければ false。
//   ★ここが解像度の下限を消している所。x 方向を標本化せず、辺との交点から厳密に出す。
inline bool polygonSpanAtY(const float* px, const float* py, int n, float y,
                           float& outLo, float& outHi) {
    float lo = 1e30f, hi = -1e30f;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float y0 = py[i], y1 = py[j];
        if ((y0 > y) == (y1 > y)) continue;            // この辺は y を跨がない
        const float t = (y - y0) / (y1 - y0);
        const float x = px[i] + (px[j] - px[i]) * t;
        lo = std::min(lo, x); hi = std::max(hi, x);
        any = true;
    }
    if (!any || hi <= lo) return false;
    outLo = lo; outHi = hi;
    return true;
}

}  // namespace fresnel
}  // namespace acoustic
