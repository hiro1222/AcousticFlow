/* Flow/trace_scene.h ── レイが見る場面を「平らな配列」にした物
 *
 * ■ 役割
 *   レイトレースが必要とする物だけを、**ポインタも仮想関数も持たない配列**にまとめる。
 *   目的は GPU。コンピュートシェーダへ渡せるのは配列だけなので、CPU 側の基準実装も
 *   同じ配列を見るようにしておく。そうすれば CPU 版と GPU 版を同じ入力で突き合わせられる。
 *
 * ■ 中の仕組み
 *   1) 箱: Surfaces と**同じ添字**のまま持つ（無効な物も席を残す）。
 *      ★詰めて並べ直すと skip（自分の面を除く）の番号が合わなくなる。席は残して印で外す。
 *   2) 材質: 帯域ごとの 反射・透過・吸収 を**前もって割って**持つ（splitAt を毎回呼ばない）。
 *      GPU で毎回 clamp を回すより、表を引くほうが素直。値は splitAt と同じ。
 *   3) 木: SurfaceBvh のノードと添字をそのまま写す。
 *   4) 交差判定は Surfaces と同じ関数（rayIntersectsObb）を呼ぶ。**当て方は変えない**。
 *      変えると「木にしたから答えが変わった」のか「平らにしたから変わった」のか分からなくなる。
 *
 * ■ 繋がり
 *   受ける: Surfaces と MaterialTable（1 フレームに 1 回、World が作る）。
 *   渡す:   energy_trace の traceRay がこれだけを見る。
 *
 * ■ 退けた書き方
 *   ・Surfaces を毎回そのまま渡す: メソッド越しなので GPU へ持っていけない。
 *   ・無効な箱を詰めて並べ直す: skip の番号がずれる（上記）。
 *   ・材質を添字で引かずに箱ごとに複製する: 箱が増えると表が太る。材質は数個なので添字で引く。
 *
 * ■ 壊れる所
 *   ・箱を動かしたのに作り直さないと、木の包みも材質の表も古いまま。World が毎フレーム作る。
 *   ・active の印を見落とすと、消した箱に当たる。nearest と横切りの両方で見る。
 */
#ifndef ACOUSTICFLOW_FLOW_TRACE_SCENE_H
#define ACOUSTICFLOW_FLOW_TRACE_SCENE_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Core/aabb.h"
#include "Core/vec3.h"
#include "Flow/surface_bvh.h"
#include "Flow/surfaces.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

/// レイが見る場面（平らな配列だけ）。
struct TraceScene {
    // 箱（Surfaces と同じ添字。無効な物も席を残す）
    std::vector<Obb>          obb;
    std::vector<std::uint8_t> active;
    std::vector<std::int32_t> material;
    // 材質の表（帯域ごとに前もって割った物）。[材質 * kNumBands + 帯域]
    std::vector<SurfaceSplit> split;
    std::vector<float>        scatter1k;      // 材質ごと。向きを決めるのに使う 1 kHz の散乱率
    // 木（SurfaceBvh の写し）
    std::vector<SurfaceBvh::Node> node;
    std::vector<std::int32_t>     item;       // 葉 → obb の添字

    int boxCount() const { return static_cast<int>(obb.size()); }
    bool hasTree() const { return !node.empty(); }
};

/// Surfaces と MaterialTable から作る。1 フレームに 1 回。
inline void buildTraceScene(const Surfaces& surf, const MaterialTable& mats, TraceScene& out) {
    const int n = surf.count();
    out.obb.resize(static_cast<std::size_t>(n));
    out.active.resize(static_cast<std::size_t>(n));
    out.material.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Surface& s = surf.at(i);
        out.obb[static_cast<std::size_t>(i)] = s.obb;
        out.active[static_cast<std::size_t>(i)] = s.active ? 1u : 0u;
        // ★材質の番号はここで丸める。MaterialTable::get は範囲外を 0 番に丸めるが、
        //   配列を直に引く GPU 側にはその親切が無い。作る所で揃えておく。
        out.material[static_cast<std::size_t>(i)] = (s.material >= 0 && s.material < mats.count()) ? s.material : 0;
    }
    const int nm = mats.count();
    out.split.resize(static_cast<std::size_t>(nm) * kNumBands);
    out.scatter1k.resize(static_cast<std::size_t>(nm));
    for (int m = 0; m < nm; ++m) {
        const AcousticMaterial& mm = mats.get(m);
        for (int b = 0; b < kNumBands; ++b)
            out.split[static_cast<std::size_t>(m) * kNumBands + b] = splitAt(mm, b);
        out.scatter1k[static_cast<std::size_t>(m)] = std::min(1.0f, std::max(0.0f, mm.scattering[3]));
    }
    // 木は Surfaces が持っている物をそのまま写す（World が rebuildBvh 済み）
    surf.bvh().exportFlat(out.node, out.item);
}

/// 最近ヒット。Surfaces::nearest と同じ答えを返す（当て方は同じ関数）。
inline SurfaceHit sceneNearest(const TraceScene& sc, const Vec3& origin, const Vec3& dir, float maxDist, int skip) {
    SurfaceHit best; best.t = maxDist;
    auto test = [&](int i) {
        if (i == skip || !sc.active[static_cast<std::size_t>(i)]) return;
        float t; Vec3 nn;
        if (rayIntersectsObb(origin, dir, sc.obb[static_cast<std::size_t>(i)], best.t, t, nn) && t < best.t && t > 0.0f) {
            best.hit = true; best.t = t; best.normal = nn; best.index = i;
        }
    };
    if (sc.hasTree()) {
        int stack[64]; int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const SurfaceBvh::Node& nd = sc.node[static_cast<std::size_t>(stack[--sp])];
            float t; Vec3 nn;
            if (!rayIntersectsAabb(origin, dir, nd.bounds, best.t, t, nn)) continue;
            if (nd.count > 0) {
                for (int k = 0; k < nd.count; ++k) test(sc.item[static_cast<std::size_t>(nd.first + k)]);
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left; stack[sp++] = nd.right;
            }
        }
    } else {
        for (int i = 0; i < sc.boxCount(); ++i) test(i);
    }
    if (best.hit) best.point = origin + dir * best.t;
    return best;
}

/// 線分が横切った箱の τ の積。Surfaces::transmittance と同じ答え（添字の昇順に掛ける）。
inline void sceneTransmittance(const TraceScene& sc, const Vec3& p0, const Vec3& p1, int skip,
                               float out6[kNumBands], int* crossings) {
    for (int b = 0; b < kNumBands; ++b) out6[b] = 1.0f;
    int n = 0;
    const Vec3 d = p1 - p0;
    const float len = length(d);
    if (len <= kEps) { if (crossings) *crossings = 0; return; }
    const Vec3 dir = d * (1.0f / len);
    int hitIdx[32]; int nh = 0;
    auto test = [&](int i) {
        if (i == skip || !sc.active[static_cast<std::size_t>(i)]) return;
        float t; Vec3 nn;
        if (!rayIntersectsObb(p0, dir, sc.obb[static_cast<std::size_t>(i)], len, t, nn)) return;
        if (t <= 0.0f || t >= len) return;
        if (nh < 32) hitIdx[nh++] = i;
    };
    if (sc.hasTree()) {
        int stack[64]; int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const SurfaceBvh::Node& nd = sc.node[static_cast<std::size_t>(stack[--sp])];
            float t; Vec3 nn;
            if (!rayIntersectsAabb(p0, dir, nd.bounds, len, t, nn)) continue;
            if (nd.count > 0) {
                for (int k = 0; k < nd.count; ++k) test(sc.item[static_cast<std::size_t>(nd.first + k)]);
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left; stack[sp++] = nd.right;
            }
        }
    } else {
        for (int i = 0; i < sc.boxCount(); ++i) test(i);
    }
    std::sort(hitIdx, hitIdx + nh);
    for (int k = 0; k < nh; ++k) {
        const int mi = sc.material[static_cast<std::size_t>(hitIdx[k])];
        const SurfaceSplit* sp6 = &sc.split[static_cast<std::size_t>(mi) * kNumBands];
        for (int b = 0; b < kNumBands; ++b) out6[b] *= sp6[b].transmit;
        ++n;
    }
    if (crossings) *crossings = n;
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_TRACE_SCENE_H
