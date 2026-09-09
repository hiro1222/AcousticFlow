/* Flow/energy_trace.h ── レイトレース（段 2。段 5 で直接音を aperture に渡した）
 *
 * ■ 役割
 *   音源から出た音のエネルギーが、面で 反射／透過／吸収 に分かれながら、リスナーにどれだけ・いつ届くかを
 *   帯域別に集計する。設計文書 Ⅵ「レイトレース（BVH）── 素材の透過率・反射率でエネルギー分配 → 帯域別エネルギー総量」。
 *   出すのは 3 つ: 自由音場の直接音（決定的。見通しの割合は aperture が別に出す）、反射の初期・後期（レイで統計的）。
 *
 * ■ 中の仕組み
 *   単位は「音源の出力を 1 としたときの、リスナーの点に届く強さ（1/m²）」。直接音なら 1/(4πd²)。
 *   1) 自由音場の直接 freeDirect6 = 1/(4πd²) × 空気吸収。**レイを飛ばさない**。見通し（割合と τ）は段 5 の aperture が
 *      解析で出し、distribute が 直接 = free × 割合、透過 = free × (1−割合) × τ にする（追記 A）。
 *      直線が横切った壁の枚数 directCrossings は情報として残す。
 *   2) 反射は N 本のレイ。各当たり点で
 *          吸収 a·e を帳簿へ、残り (r+t)·e を運ぶ。反射か透過かは r:t の確率で片方だけ選ぶ（期待値は不偏）。
 *      反射方向は材質の散乱率で 鏡面⇄拡散 を振り分ける（旧 scene.h の scatteredDir と同じ）。
 *   3) **次イベント推定（NEE）**: 当たり点ごとにリスナーへ影レイを 1 本引き、拡散の放射（ランバート）として
 *          届く強さ = e_side · cosθ / (π d²) · τ_途中 · 空気
 *      を足す。e_side はリスナーが面の表側なら r·e、裏側なら t·e。到達時刻 (経路長 + d)/c が mixingSec 未満なら
 *      初期、以上なら後期。★レイがリスナーの球に当たるのを待たない（当たらない＝分散が大きい）。
 *      「diffuse rain」と呼ばれる標準の推定法。
 *   4) 打ち切り: 帯域の最大エネルギーが出発の 1e-4（−40 dB）を切るか、maxBounces に達したら残りを帳簿の
 *      remainder へ。保存則  emitted = absorbed + remainder + escaped  が帳簿で成り立つ（検査）。
 *   5) **乱数の種は音源ごとに固定**。同じ幾何・同じ位置なら毎フレーム同じレイが飛び、結果がビット一致する。
 *      → 静止していれば揺れない（追記 D）。動けば相関した標本が滑らかに追う。
 *
 * ■ 繋がり
 *   受ける: Surfaces、MaterialTable、Emitter の位置、Listener の位置、TraceParams（本数は budget が決める）。
 *   渡す:   TraceResult（freeDirect6 / early6 / late6 と帳簿）を distribute へ。
 *
 * ■ 退けた書き方
 *   ・リスナーの球にレイが当たったら集計（検出球）: 当たる本数が少なく、256 本では初期反射がほぼ 0 か 1 本。
 *   ・直接音もレイで出す: 決定的に出せる物を統計で出す理由が無い。揺れの源を増やすだけ。
 *   ・直接音の τ をここで直線から出す（段 4 までの形）: 幅を持つ音源が半分だけ板に隠れるとき、中心の直線は見通しでも
 *     隠れた半分は板を通る。τ は「遮っている物」の物で、aperture が影ごとに出すのが筋。段 5 でこちらから外した。
 *   ・反射と透過の両方にレイを分岐させる: 本数が指数で増える。片方を確率で選び重みで補うのが標準。
 *
 * ■ 壊れる所
 *   ・影レイの原点を当たり点ちょうどに置くと自分の面に当たって全部遮られる（kEps で法線側へ押す）。
 *   ・cosθ を絶対値にすると、面の裏側のリスナーに反射エネルギーが届く（壁を透ける）。表裏で e_side を分ける。
 *   ・NEE の 1/π を忘れると反射が π 倍（+5 dB）。
 *   ・種を毎フレーム変えると静止で揺れる。
 *   ・帳簿は double。4096 本 × 40 回のヒットで float では 2e-4 ずれて保存則の検査が落ちた（段 2）。
 */
#ifndef ACOUSTICFLOW_FLOW_ENERGY_TRACE_H
#define ACOUSTICFLOW_FLOW_ENERGY_TRACE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Core/material.h"
#include "Core/room_graph.h"      // kAirDbPerM（空気吸収の表はここ 1 か所）
#include "Core/vec3.h"
#include "Flow/surfaces.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

constexpr float kPi = 3.14159265358979f;

// ── 乱数と半球（旧 scene.h から持ち込み。PCG 系のハッシュ乱数と余弦重み半球）──
inline float rand01(std::uint32_t& state) {
    state = state * 747796405u + 2891336453u;
    std::uint32_t w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    w = (w >> 22) ^ w;
    return static_cast<float>(w) * (1.0f / 4294967296.0f);
}
inline Vec3 cosineHemisphere(const Vec3& n, std::uint32_t& rng) {
    const float u1 = rand01(rng), u2 = rand01(rng);
    const float r = std::sqrt(u1), th = 2.0f * kPi * u2;
    const float x = r * std::cos(th), y = r * std::sin(th);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    const Vec3 t = (std::fabs(n.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 b1 = normalized(cross(n, t));
    const Vec3 b2 = cross(n, b1);
    return normalized(b1 * x + b2 * y + n * z);
}
inline Vec3 uniformSphere(std::uint32_t& rng) {
    const float z = 1.0f - 2.0f * rand01(rng);
    const float th = 2.0f * kPi * rand01(rng);
    const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3(s * std::cos(th), z, s * std::sin(th));
}
/// 空気吸収（エネルギー比）。kAirDbPerM は dB/m。
inline float airEnergy(int band, float dist) {
    return std::pow(10.0f, -static_cast<float>(rooms::kAirDbPerM[band]) * dist * 0.1f);
}

struct TraceParams {
    int   rays = 256;               // 予算（budget が音源ごとに決める）
    int   maxBounces = 24;
    float mixingSec = 0.03f;        // 初期／後期の境（到達時刻）。世界が probe から出す（3 × 平均自由行程 / c）
    std::uint32_t seed = 1u;        // 音源ごとに固定
};

struct TraceResult {
    float freeDirect6[kNumBands] = {};  // 自由音場の直接（1/4πd² × 空気）。見通しは aperture が別に出す
    float early6[kNumBands] = {};       // 反射、t < mixing
    float late6[kNumBands] = {};        // 反射、t ≥ mixing
    float firstReflectSec = -1.0f;      // 最初に届いた反射の時刻（尾の開始の代用。ISM が来たら置き換え）
    float directSec = 0.0f;             // 直接音の到達時刻
    float directDist = 0.0f;            // 直接音の距離
    int   directCrossings = 0;          // 直線が横切った壁の枚数（情報）
    // 帳簿（レイ側）。保存則: emitted = absorbed + remainder + escaped
    double emitted6[kNumBands] = {}, absorbed6[kNumBands] = {}, remainder6[kNumBands] = {}, escaped6[kNumBands] = {};
    int   raysTraced = 0, hits = 0, neeVisible = 0;

    float reflected6(int b) const { return early6[b] + late6[b]; }
};

class EnergyTrace {
public:
    TraceResult run(const Surfaces& surf, const MaterialTable& mats,
                    const Vec3& source, const Vec3& listener, const TraceParams& prm) const {
        TraceResult R;
        // ── 1) 自由音場の直接（決定的）──
        {
            const float d = std::max(length(listener - source), kEps);
            float tr[kNumBands]; int crossings = 0;
            surf.transmittance(source, listener, -1, mats, tr, &crossings);   // 枚数だけ使う（τ は aperture の物）
            R.directCrossings = crossings;
            R.directDist = d;
            R.directSec = d / kSpeedOfSound;
            const float geo = 1.0f / (4.0f * kPi * d * d);
            for (int b = 0; b < kNumBands; ++b) R.freeDirect6[b] = geo * airEnergy(b, d);
        }
        // ── 2) 反射（レイ + NEE）──
        const int N = std::max(1, prm.rays);
        const float e0 = 1.0f / static_cast<float>(N);          // 1 本 = 出力の 1/N
        std::uint32_t rng = prm.seed * 2654435761u + 0x9E3779B9u;
        for (int i = 0; i < N; ++i) {
            float e[kNumBands];
            for (int b = 0; b < kNumBands; ++b) { e[b] = e0; R.emitted6[b] += e0; }
            Vec3 pos = source;
            Vec3 dir = uniformSphere(rng);
            float pathLen = 0.0f;
            int skip = -1;
            bool terminated = false;
            ++R.raysTraced;
            for (int bounce = 0; bounce < prm.maxBounces; ++bounce) {
                const SurfaceHit h = surf.nearest(pos, dir, 1e4f, skip);
                if (!h.hit) { for (int b = 0; b < kNumBands; ++b) R.escaped6[b] += e[b]; terminated = true; break; }
                ++R.hits;
                pathLen += h.t;
                const AcousticMaterial& m = mats.get(surf.at(h.index).material);
                float rMean = 0.0f, tMean = 0.0f;
                SurfaceSplit sp[kNumBands];
                for (int b = 0; b < kNumBands; ++b) {
                    sp[b] = splitAt(m, b);
                    R.absorbed6[b] += e[b] * sp[b].absorb;
                    rMean += sp[b].reflect; tMean += sp[b].transmit;
                }
                rMean /= kNumBands; tMean /= kNumBands;
                const Vec3 nFace = (dot(h.normal, dir) < 0.0f) ? h.normal : h.normal * -1.0f;
                // ── NEE: この当たり点からリスナーへ ──
                {
                    const Vec3 toL = listener - h.point;
                    const float d = length(toL);
                    if (d > kEps) {
                        const Vec3 u = toL * (1.0f / d);
                        const float cosF = dot(nFace, u);
                        const bool front = cosF > 0.0f;
                        const float cosT = std::fabs(cosF);
                        const Vec3 org = h.point + (front ? nFace : nFace * -1.0f) * kEps;
                        float tr[kNumBands]; int cr = 0;
                        surf.transmittance(org, listener, h.index, mats, tr, &cr);
                        const float tSec = (pathLen + d) / kSpeedOfSound;
                        const float geo = cosT / (kPi * d * d);
                        bool any = false;
                        for (int b = 0; b < kNumBands; ++b) {
                            const float eSide = e[b] * (front ? sp[b].reflect : sp[b].transmit);
                            const float c = eSide * geo * tr[b] * airEnergy(b, pathLen + d);
                            if (c <= 0.0f) continue;
                            any = true;
                            if (tSec < prm.mixingSec) R.early6[b] += c; else R.late6[b] += c;
                        }
                        if (any) {
                            ++R.neeVisible;
                            if (R.firstReflectSec < 0.0f || tSec < R.firstReflectSec) R.firstReflectSec = tSec;
                        }
                    }
                }
                // ── 続きの経路: 反射か透過か（確率で片方、重みは r+t）──
                const float carry = rMean + tMean;
                if (carry <= 1e-6f) { terminated = true; break; }
                const bool goReflect = rand01(rng) < (rMean / carry);
                for (int b = 0; b < kNumBands; ++b) e[b] *= (sp[b].reflect + sp[b].transmit);
                float eMax = 0.0f; for (int b = 0; b < kNumBands; ++b) eMax = std::max(eMax, e[b]);
                if (eMax < e0 * 1e-4f) { for (int b = 0; b < kNumBands; ++b) R.remainder6[b] += e[b]; terminated = true; break; }
                if (goReflect) {
                    const float s = std::max(0.0f, std::min(1.0f, m.scattering[3]));   // 1 kHz の散乱率で向きを決める
                    dir = (rand01(rng) < s) ? cosineHemisphere(nFace, rng) : reflect(dir, nFace);
                    if (dot(dir, nFace) <= 0.0f) dir = cosineHemisphere(nFace, rng);
                    pos = h.point + nFace * kEps;
                } else {
                    pos = h.point - nFace * kEps;
                }
                skip = h.index;
            }
            if (!terminated) for (int b = 0; b < kNumBands; ++b) R.remainder6[b] += e[b];
        }
        return R;
    }
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_ENERGY_TRACE_H
