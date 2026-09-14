/* Flow/image_lattice.h ── 虚像の網: 虚像どうしを結んだ格子で、見かけの音源の点を受け、網の目の角の虚像へ配る（2026-09-14）
 *
 * ■ 役割
 *   World::earlyModel = 3 の受取面。レイが耳へ届けた受け取り 1 件ごとに「来た向き × 道のり」の点（見かけの音源の位置）を作り、
 *   虚像を結んだ網のどの目に入ったかで、角の虚像へ量を配る。鏡面の反射の点は虚像にぴったり重なるので全部その虚像へ、
 *   散乱した分は目の中の位置に比例して周りの虚像へ分かれる。発注者と決めた形（「ISM をつないで、その面を面として扱う」の A 案）。
 *
 * ■ 中の仕組み
 *   1) 網を作る（build、音源ごと・毎フレーム）。見えるかどうかは問わない（見えない虚像も網の角に残す）。
 *      ・軸: 音源と耳が同じ側にある面ごとに、1 次の虚像の向き a = −法線。同じ向きの面は 1 本の軸に束ねる（cos > kAxisCos）。
 *      ・軸の上の点: 1 次の虚像（音源から 2·side(S, f)）と、同じ軸に乗る 2 次の虚像（平行な 2 枚の面の往復）。音源からの距離の順。
 *        結び方は「経路が反射 1 回だけ違う」: 音源 → 1 次 → 2 次 と、軸の上で隣り合う。
 *   2) 点を網で受ける（shares）。r = P − S。
 *      ・軸ごとに x = r·a。x ≤ 0 の軸は「0 段目（音源の側）」。x > 0 なら、並んだ点の間で折れ線の山の重み（隣の 2 点に 1 − t と t）。
 *        最後の点より先は、ひとつ前の間隔の長さで 0 へ落とし、落ちた分は「網の外」。
 *      ・軸どうしは掛け算。箱の部屋なら x・y・z の 3 本で、目は角が虚像の箱、重みは 8 つの角への三重線形と同じになる。
 *        掛け算なので、軸が何本でも重みの和は 1。
 *      ・角の虚像は、軸ごとに選んだ点の面を合わせた経路で決まる:
 *          面 0 枚 → 音源の角（0 次。方向なしの 1 本へ）
 *          面 1 枚 → その 1 次の虚像
 *          面 2 枚 → その 2 次の虚像（順の違う 2 通りがあれば可視率で分ける）
 *          面 3 枚以上・網の外・ImageSet に無い（見えない・上限で落ちた） → 残り（面のタップ）
 *      ・虚像へ渡すのは 重み × 可視率。2 通りあるときは 重み × v_k / max(1, Σv)。渡さなかった分は残り。
 *
 * ■ 繋がり
 *   受ける: ISM の面（World::faces_）、その音源の ImageSet（可視率と遅れ）、音源と耳の位置。
 *   渡す:   receiver.h の buildFaceTaps が、受け取りごとに shares を呼んで虚像の器・音源の角の器・面の器へ足す。
 *
 * ■ 退けた書き方
 *   ・壁の面を受取面にして虚像へ量を分ける（earlyModel 2）: 受けているのは壁で、虚像の側に受取面が無い。発注者の指摘で作り直した。
 *   ・一番近い虚像へ全部渡す: 目の境で配り先が 1 フレームで入れ替わり、段になる。折れ線の山で繋ぐ。
 *   ・見えている虚像だけで網を作る: 虚像が窓の縁で出入りするたびに網の形が変わり、周りの虚像の取り分が跳ぶ。
 *     網は見え方に依らずに作り、見え方は角の可視率で掛ける。
 *   ・耳から見た向きだけで三角形に結ぶ: 同じ向きで遠さの違う虚像（1 次と、平行な壁を往復した 2 次）を分けられない。
 *
 * ■ 壊れる所
 *   ・軸は面の向きで束ねる。箱が少しだけ回った場面では向きの近い軸が 2 本立ち、両方の軸で x > 0 になって存在しない面の組へ配り、
 *     その分が残りへ落ちる。効く軸は kMaxActive 本までで、超えたら弱い軸を捨てる（そこで重みが跳ぶ）。
 *   ・3 次の虚像（imageOrder = 3）は網に入れていない。3 次の虚像へは配らず、その目の分は残りになる。
 *   ・ImageSet の上限（48 本）で落ちた虚像は、見えていても残りへ行く。
 */
#ifndef ACOUSTICFLOW_FLOW_IMAGE_LATTICE_H
#define ACOUSTICFLOW_FLOW_IMAGE_LATTICE_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Core/vec3.h"
#include "Flow/image_sources.h"

namespace acoustic {
namespace flow {

/// 配り先 1 件。image は ImageSet の番号、または ImageLattice::kNear（音源の角）/ kRest（残り）。
struct LatticeShare {
    int   image = -2;
    float w = 0.0f;
};

class ImageLattice {
public:
    static constexpr int   kNear = -1;
    static constexpr int   kRest = -2;
    static constexpr int   kMaxActive = 4;          // 1 つの点に効く軸の上限（箱の部屋は 3）
    static constexpr int   kMaxShares = 40;         // 2^4 の組 × 2 通り ＋ 音源の角 ＋ 残り
    static constexpr float kAxisCos = 0.999f;       // 同じ軸とみなす向きの近さ（2.6°）

    /// 網を作る。images は shares の間、生きていること。
    void build(const std::vector<Face>& faces, const ImageSet& images, const Vec3& source, const Vec3& listener) {
        images_ = &images;
        source_ = source;
        nFaces_ = static_cast<int>(faces.size());
        axes_.clear(); points_.clear(); tmp_.clear();
        map1_.assign(static_cast<std::size_t>(nFaces_), -1);
        map2_.assign(static_cast<std::size_t>(nFaces_) * static_cast<std::size_t>(nFaces_), -1);
        for (int i = 0; i < images.count; ++i) {
            const ImageSource& s = images.img[i];
            if (s.order == 1 && s.face[0] >= 0 && s.face[0] < nFaces_) map1_[static_cast<std::size_t>(s.face[0])] = i;
            else if (s.order == 2 && s.face[0] >= 0 && s.face[0] < nFaces_ && s.face[1] >= 0 && s.face[1] < nFaces_)
                map2_[static_cast<std::size_t>(s.face[0]) * static_cast<std::size_t>(nFaces_) + static_cast<std::size_t>(s.face[1])] = i;
        }
        auto axisOf = [&](const Vec3& dir) {
            for (std::size_t k = 0; k < axes_.size(); ++k) if (dot(axes_[k].a, dir) > kAxisCos) return static_cast<int>(k);
            axes_.push_back(Axis{dir, 0, 0});
            return static_cast<int>(axes_.size()) - 1;
        };
        // 1 次: 音源と耳が同じ側の面（buildImages と同じ候補。見えるかどうかは問わない）
        for (int f = 0; f < nFaces_; ++f) {
            const Face& fc = faces[static_cast<std::size_t>(f)];
            const float sS = detail::side(source, fc), sL = detail::side(listener, fc);
            if (sS <= 0.0f || sL <= 0.0f) continue;
            tmp_.push_back(Tmp{axisOf(fc.normal * -1.0f), 2.0f * sS, f, -1});
        }
        // 2 次のうち、同じ軸に乗る物（平行な 2 枚の面の往復）。音源の側の面 f1 → 耳の側の面 f0
        for (int f1 = 0; f1 < nFaces_; ++f1) {
            const Face& a = faces[static_cast<std::size_t>(f1)];
            if (detail::side(source, a) <= 0.0f || detail::side(listener, a) <= 0.0f) continue;
            const Vec3 S1 = detail::mirror(source, a);
            for (int f0 = 0; f0 < nFaces_; ++f0) {
                if (f0 == f1) continue;
                const Face& b = faces[static_cast<std::size_t>(f0)];
                if (std::fabs(dot(a.normal, b.normal)) < kAxisCos) continue;
                if (a.box == b.box && dot(a.normal, b.normal) < -0.99f) continue;      // 同じ箱の表裏（buildImages と同じ決まり）
                if (detail::side(S1, b) <= 0.0f || detail::side(listener, b) <= 0.0f) continue;
                const Vec3 d = detail::mirror(S1, b) - source;
                const float len = length(d);
                if (len < 1e-4f) continue;
                tmp_.push_back(Tmp{axisOf(d * (1.0f / len)), len, f0, f1});
            }
        }
        std::sort(tmp_.begin(), tmp_.end(), [](const Tmp& x, const Tmp& y) { return (x.axis != y.axis) ? x.axis < y.axis : x.proj < y.proj; });
        for (std::size_t i = 0; i < tmp_.size();) {
            const int ax = tmp_[i].axis;
            axes_[static_cast<std::size_t>(ax)].first = static_cast<int>(points_.size());
            for (; i < tmp_.size() && tmp_[i].axis == ax; ++i) {
                const Tmp& t = tmp_[i];
                if (!points_.empty() && static_cast<int>(points_.size()) > axes_[static_cast<std::size_t>(ax)].first &&
                    std::fabs(points_.back().proj - t.proj) < 1e-4f && points_.back().n < 2) {
                    Point& p = points_.back();                                   // 同じ位置の 2 つ目の経路
                    p.f0[p.n] = t.f0; p.f1[p.n] = t.f1; ++p.n;
                    continue;
                }
                Point p; p.proj = t.proj; p.n = 1; p.f0[0] = t.f0; p.f1[0] = t.f1;
                points_.push_back(p);
            }
            axes_[static_cast<std::size_t>(ax)].count = static_cast<int>(points_.size()) - axes_[static_cast<std::size_t>(ax)].first;
        }
    }

    /// 見かけの音源の点 P を網で受け、配り先と重みを out に書く（Σw = 1）。返り値は件数。
    int shares(const Vec3& P, LatticeShare* out) const {
        const Vec3 r = P - source_;
        Act act[kMaxActive];
        int na = 0;
        for (std::size_t k = 0; k < axes_.size(); ++k) {
            const Axis& ax = axes_[k];
            if (ax.count <= 0) continue;
            const float x = dot(r, ax.a);
            if (!(x > 0.0f)) continue;
            Act A;
            int j = 0;
            while (j < ax.count && points_[static_cast<std::size_t>(ax.first + j)].proj <= x) ++j;
            if (j < ax.count) {
                const float p1 = points_[static_cast<std::size_t>(ax.first + j)].proj;
                const float p0 = (j == 0) ? 0.0f : points_[static_cast<std::size_t>(ax.first + j - 1)].proj;
                const float t = (p1 > p0 + 1e-6f) ? std::min(1.0f, std::max(0.0f, (x - p0) / (p1 - p0))) : 1.0f;
                A.lo = (j == 0) ? kLevel0 : ax.first + j - 1; A.hi = ax.first + j;
                A.wLo = 1.0f - t; A.wHi = t;
            } else {
                const int last = ax.first + ax.count - 1;
                const float pl = points_[static_cast<std::size_t>(last)].proj;
                const float pp = (ax.count >= 2) ? points_[static_cast<std::size_t>(last - 1)].proj : 0.0f;
                const float t = std::min(1.0f, std::max(0.0f, (x - pl) / std::max(1e-3f, pl - pp)));
                A.lo = last; A.hi = kOutside;
                A.wLo = 1.0f - t; A.wHi = t;
            }
            A.strength = (A.lo == kLevel0) ? A.wHi : 1.0f;
            if (!(A.strength > 0.0f)) continue;
            if (na < kMaxActive) { act[na++] = A; continue; }
            int weakest = 0;
            for (int q = 1; q < na; ++q) if (act[q].strength < act[weakest].strength) weakest = q;
            if (A.strength > act[weakest].strength) act[weakest] = A;
        }
        int n = 0;
        double nearW = 0.0, restW = 0.0;
        const int combos = 1 << na;
        for (int mask = 0; mask < combos; ++mask) {
            double W = 1.0;
            bool outside = false;
            const Point* pts[kMaxActive];
            int np = 0;
            for (int i = 0; i < na; ++i) {
                const bool hi = ((mask >> i) & 1) != 0;
                const int st = hi ? act[i].hi : act[i].lo;
                W *= hi ? act[i].wHi : act[i].wLo;
                if (st == kOutside) outside = true;
                else if (st >= 0) pts[np++] = &points_[static_cast<std::size_t>(st)];
            }
            if (!(W > 0.0)) continue;
            if (outside || np > 2) { restW += W; continue; }
            if (np == 0) { nearW += W; continue; }
            int idx[2] = {-1, -1};
            int ni = 0;
            if (np == 1) {
                for (int c = 0; c < pts[0]->n && ni < 2; ++c) idx[ni++] = lookup(pts[0]->f0[c], pts[0]->f1[c]);
            } else {
                const int fa = single(*pts[0]), fb = single(*pts[1]);
                if (fa < 0 || fb < 0) { restW += W; continue; }      // 2 次の点 ＋ 1 次の点 ＝ 3 次
                idx[ni++] = lookup(fa, fb);
                idx[ni++] = lookup(fb, fa);
            }
            double sumV = 0.0;
            for (int c = 0; c < ni; ++c) sumV += validity(idx[c]);
            const double scale = W / std::max(1.0, sumV);
            double given = 0.0;
            for (int c = 0; c < ni; ++c) {
                const double v = validity(idx[c]);
                if (!(v > 0.0)) continue;
                const double w = v * scale;
                given += w;
                add(out, n, idx[c], w);
            }
            restW += std::max(0.0, W - given);
        }
        if (nearW > 0.0) add(out, n, kNear, nearW);
        if (restW > 0.0) add(out, n, kRest, restW);
        return n;
    }

    int axisCount() const { return static_cast<int>(axes_.size()); }
    int pointCount() const { return static_cast<int>(points_.size()); }

private:
    static constexpr int kLevel0 = -1;
    static constexpr int kOutside = -2;
    struct Axis { Vec3 a; int first; int count; };
    struct Point { float proj = 0.0f; int n = 0; int f0[2] = {-1, -1}; int f1[2] = {-1, -1}; };   // f1 < 0 なら 1 次
    struct Tmp { int axis; float proj; int f0; int f1; };
    struct Act { int lo = kLevel0, hi = kLevel0; float wLo = 1.0f, wHi = 0.0f, strength = 0.0f; };

    int lookup(int f0, int f1) const {
        if (f0 < 0 || f0 >= nFaces_) return -1;
        if (f1 < 0) return map1_[static_cast<std::size_t>(f0)];
        if (f1 >= nFaces_) return -1;
        return map2_[static_cast<std::size_t>(f0) * static_cast<std::size_t>(nFaces_) + static_cast<std::size_t>(f1)];
    }
    /// 1 次の経路だけを持つ点なら、その面。2 次の点なら −1。
    static int single(const Point& p) {
        for (int c = 0; c < p.n; ++c) if (p.f1[c] < 0) return p.f0[c];
        return -1;
    }
    double validity(int idx) const {
        return (idx >= 0 && images_ && idx < images_->count) ? std::min(1.0, std::max(0.0, static_cast<double>(images_->img[idx].validity))) : 0.0;
    }
    static void add(LatticeShare* out, int& n, int image, double w) {
        for (int i = 0; i < n; ++i) if (out[i].image == image) { out[i].w += static_cast<float>(w); return; }
        if (n < kMaxShares) { out[n].image = image; out[n].w = static_cast<float>(w); ++n; }
    }

    const ImageSet*    images_ = nullptr;
    Vec3               source_{0, 0, 0};
    int                nFaces_ = 0;
    std::vector<Axis>  axes_;
    std::vector<Point> points_;
    std::vector<Tmp>   tmp_;
    std::vector<int>   map1_, map2_;
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_IMAGE_LATTICE_H
