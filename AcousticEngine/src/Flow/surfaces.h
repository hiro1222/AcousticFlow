/* Flow/surfaces.h ── 面の一覧（段 2。地図に無かった薄い 1 枚）
 *
 * ■ 役割
 *   世界の「音がぶつかる物」の一覧。箱（Obb）＋材質番号＋動く印。
 *   レイの最近ヒットと、線分がどの箱を横切るか（透過の掛け合わせ）の 2 つの問いに答える。
 *   旧 scene.h では Instance として 8,000 行の中に埋まっていた。幾何の問いはここだけで受ける。
 *
 * ■ 中の仕組み
 *   1) nearest: 全部の箱に rayIntersectsObb（Core/aabb.h、スラブ法）を当てて最小の t を取る。
 *      箱の数は場面で数十なので総当たり（BVH は三角形メッシュ用に Core/bvh.h が残っている。段 10 の後で繋ぐ）。
 *   2) transmittance: 線分 p0→p1 が横切る箱ごとに、その材質の透過率 τ_b を掛け合わせる。
 *      「壁越し」の量はこれ 1 つ。何枚横切ったかも返す（0 枚なら見通し＝直接音、1 枚以上なら透過音）。
 *      skip で「いま当たっている箱」を除ける（当たり点から出る影レイが自分自身に当たらないため）。
 *   3) dynamic: 動く物の印。部屋グラフと焼きから外す（地図 8 章）。ここでは印を持つだけ。
 *
 * ■ 繋がり
 *   受ける: ホストが箱を足す（世界が中継）。
 *   渡す:   energy_trace（最近ヒット・透過）、aperture（段 5、開口の縁）、image_sources（段 7、面）。
 *
 * ■ 退けた書き方
 *   ・最初から BVH を組む: 箱が数十のうちは総当たりのほうが速く、読める。三角形メッシュが来たら bvh.h。
 *   ・透過を「1 枚でも横切ったら 0」にする: 透過音が消える。設計文書 Ⅸ ④「透過音はレイ方式」は τ の積。
 *
 * ■ 壊れる所
 *   ・影レイの原点を当たり点ちょうどにすると、同じ箱に t≈0 で再ヒットして常に遮られる。
 *     skip で自分を除け、原点を法線側へ 1 mm 押す（kEps）。
 *   ・箱の中から出るレイ: rayIntersectsObb は箱の中の原点でも出口の面で当たる。
 *     transmittance はそれを「横切り」に数えるので、音源が箱の中にあると τ が掛かる（閉じ込めの籠り。意図どおり）。
 */
#ifndef ACOUSTICFLOW_FLOW_SURFACES_H
#define ACOUSTICFLOW_FLOW_SURFACES_H

#include <algorithm>
#include <vector>
#include "Core/aabb.h"
#include "Flow/surface_bvh.h"
#include "Core/material.h"
#include "Core/vec3.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

constexpr float kEps = 1e-3f;   // 1 mm。当たり点から離す距離

struct Surface {
    Obb  obb;
    int  material = 0;
    bool dynamic = false;
    bool active = true;
};

struct SurfaceHit {
    bool  hit = false;
    float t = 0.0f;
    Vec3  point{0, 0, 0};
    Vec3  normal{0, 0, 0};       // 外向き
    int   index = -1;            // どの箱か
};

class Surfaces {
public:
    int add(const Obb& obb, int material, bool dynamic = false) {
        Surface s; s.obb = obb; s.material = material; s.dynamic = dynamic;
        list_.push_back(s);
        return static_cast<int>(list_.size()) - 1;
    }
    int count() const { return static_cast<int>(list_.size()); }
    Surface& at(int i) { return list_[static_cast<std::size_t>(i)]; }
    const Surface& at(int i) const { return list_[static_cast<std::size_t>(i)]; }

    /// 最近ヒット。skip の箱は無視する。
    /// 木を作り直す。動く箱があるので**毎フレーム**呼ぶ（World::update の頭）。
    ///   ★呼ばなければ総当たりのまま。木があれば木を歩く。答えは同じ（検査で担保）。
    void rebuildBvh() {
        bvh_.build(count(),
                   [this](int i) { return obbAabb(list_[static_cast<std::size_t>(i)].obb); },
                   [this](int i) { return list_[static_cast<std::size_t>(i)].active; });
    }
    void clearBvh() { bvh_.clear(); }
    const SurfaceBvh& bvh() const { return bvh_; }

    SurfaceHit nearest(const Vec3& origin, const Vec3& dir, float maxDist, int skip = -1) const {
        SurfaceHit best; best.t = maxDist;
        auto test = [&](int i) {
            if (i == skip || !list_[static_cast<std::size_t>(i)].active) return;
            float t; Vec3 n;
            if (rayIntersectsObb(origin, dir, list_[static_cast<std::size_t>(i)].obb, best.t, t, n) && t < best.t && t > 0.0f) {
                best.hit = true; best.t = t; best.normal = n; best.index = i;
            }
        };
        if (!bvh_.empty()) bvh_.traverseRay(origin, dir, best.t, test);
        else for (int i = 0; i < count(); ++i) test(i);
        if (best.hit) best.point = origin + dir * best.t;
        return best;
    }

    /// 線分 p0→p1 の帯域別の透過率（横切った箱の τ の積）。crossings に横切った枚数。
    void transmittance(const Vec3& p0, const Vec3& p1, int skip, const MaterialTable& mats,
                       float out6[kNumBands], int* crossings = nullptr) const {
        for (int b = 0; b < kNumBands; ++b) out6[b] = 1.0f;
        int n = 0;
        const Vec3 d = p1 - p0;
        const float len = length(d);
        if (len <= kEps) { if (crossings) *crossings = 0; return; }
        const Vec3 dir = d * (1.0f / len);
        // ★横切った箱を**添字の昇順**に並べてから掛ける。積は順で末尾が変わるので、
        //   木で拾った順のまま掛けると総当たりと一致しなくなる。
        int hitIdx[32]; int nh = 0;
        auto test = [&](int i) {
            if (i == skip || !list_[static_cast<std::size_t>(i)].active) return;
            float t; Vec3 nrm;
            if (!rayIntersectsObb(p0, dir, list_[static_cast<std::size_t>(i)].obb, len, t, nrm)) return;
            if (t <= 0.0f || t >= len) return;
            if (nh < 32) hitIdx[nh++] = i;
        };
        if (!bvh_.empty()) bvh_.traverseRay(p0, dir, len, test);
        else for (int i = 0; i < count(); ++i) test(i);
        std::sort(hitIdx, hitIdx + nh);
        for (int k = 0; k < nh; ++k) {
            const AcousticMaterial& m = mats.get(list_[static_cast<std::size_t>(hitIdx[k])].material);
            for (int b = 0; b < kNumBands; ++b) out6[b] *= splitAt(m, b).transmit;
            ++n;
        }
        if (crossings) *crossings = n;
    }

private:
    std::vector<Surface> list_;
    SurfaceBvh bvh_;
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_SURFACES_H
