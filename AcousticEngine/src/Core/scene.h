/* Core/scene.h  ── 新アーキ(2026-07) Phase 0：インスタンス方式の音響シーン
 *
 * 旧 AcousticWorld の boxes_/meshes_ 分離を廃し、次世代設計の「インスタンス」に統一する。
 *   インスタンス = geomId(形状) + transform(OBB) + materialId(材質)
 * これは TLAS/BLAS の土台。Phase 0 では全インスタンスを OBB プリミティブとして線形走査し、
 * BVH-of-OBB(TLAS) は Phase 3 で差し込む（走査ループだけ差し替えれば済むよう API を分離）。
 *
 * 設計方針（[[dynamic-ray-architecture]] に準拠）:
 *   - 静的音響空間を作らない。ジオメトリは毎フレーム更新可能な物性のみ。
 *   - 材質は materials_ テーブルに一元化し、インスタンスは matId で参照（焼かない）。
 *   - Wwise にも C API にも依存しない純粋計算層（STL 自由）。
 *
 * Phase 0 が提供するクエリ:
 *   - raycastClosest : 最近傍ヒット（レイキャストの土台）
 *   - isOccluded     : 2点間の見通し（二値）
 *   - computeTransmission : 直線上の壁の帯域別透過ゲイン(6帯域) ← 役割1の素
 */
#ifndef ACOUSTICFLOW_CORE_SCENE_H
#define ACOUSTICFLOW_CORE_SCENE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/utd.h"
#include "Core/vec3.h"

namespace acoustic {

// ── レイサンプリング補助（役割2の反射トレース用）。旧 acoustic_world.cpp から移植。
namespace scene_detail {

constexpr float kPiF = 3.14159265358979f;

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// フィボナッチ球：一様に近い方向分散。
inline Vec3 fibonacciSphereDir(int i, int n) {
    const float k = static_cast<float>(i) + 0.5f;
    const float phi = std::acos(1.0f - 2.0f * k / static_cast<float>(n));
    const float theta = kPiF * (1.0f + std::sqrt(5.0f)) * k;
    const float s = std::sin(phi);
    return Vec3(s * std::cos(theta), std::cos(phi), s * std::sin(theta));
}

// 軽量ハッシュ乱数（PCG系）。
inline float hashRand01(uint32_t& state) {
    state = state * 747796405u + 2891336453u;
    uint32_t w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    w = (w >> 22) ^ w;
    return static_cast<float>(w) * (1.0f / 4294967296.0f);
}

// 法線 n まわりの余弦重み半球サンプル（ランバート拡散）。
inline Vec3 cosineHemisphere(const Vec3& n, uint32_t& rng) {
    const float u1 = hashRand01(rng);
    const float u2 = hashRand01(rng);
    const float r = std::sqrt(u1);
    const float th = 2.0f * kPiF * u2;
    const float x = r * std::cos(th);
    const float y = r * std::sin(th);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    const Vec3 t = (std::fabs(n.x) > 0.9f) ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 b1 = normalized(cross(n, t));
    const Vec3 b2 = cross(n, b1);
    return normalized(b1 * x + b2 * y + n * z);
}

// scattering(0..1) で反射方向を鏡面⇄拡散に確率的に振り分ける。
inline Vec3 scatteredDir(const Vec3& d, const Vec3& n, float s, uint32_t& rng) {
    if (s > 0.0f && hashRand01(rng) < s) return cosineHemisphere(n, rng);  // 拡散
    return reflect(d, n);                                                  // 鏡面
}

inline float scatteringMean(const AcousticMaterial& m) {
    float s = 0.0f;
    for (int b = 0; b < kNumBands; ++b) s += m.scattering[b];
    return s / static_cast<float>(kNumBands);
}

// 【回折(Phase 4)】迂回余剰長 δ(m) から帯域別回折ゲイン(0..1)を出す。
//   ITU-R P.526 ナイフエッジ回折損 J(ν)（ν=Fresnel-Kirchhoff パラメータ）。
//   Maekawa より連続で標準的。影境界(δ=0)で ~6dB=0.5、低域ほど回り込む（ν 小＝損小）。
inline void knifeEdgeGain(float delta, float outGain[kNumBands]) {
    const float kSpeed = 343.0f;
    const float bandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
    const float dd = delta > 0.0f ? delta : 0.0f;
    for (int b = 0; b < kNumBands; ++b) {
        const float lambda = kSpeed / bandFreq[b];
        const float nu = 2.0f * std::sqrt(dd / lambda);  // ν = 2√(δ/λ)（影側 δ≥0）
        const float t = nu - 0.1f;
        float J = 6.9f + 20.0f * std::log10(std::sqrt(t * t + 1.0f) + t);  // dB 損
        if (J < 0.0f) J = 0.0f;
        outGain[b] = std::pow(10.0f, -J / 20.0f);
    }
}

}  // namespace scene_detail

// シーン内の1つの占有物。Phase 0 では形状=OBB 固定（geomId は BLAS 導入時に使う予約）。
struct Instance {
    Obb obb;              // ワールド空間の有向境界ボックス（transform 相当）
    int materialId = 0;   // materials_ テーブルへの参照
    int geomId = 0;       // 将来の BLAS(メッシュ)識別用。Phase 0 では未使用(0=単位ボックス)
    bool active = true;   // false のとき全走査からスキップ（LOD ストリーミング用）
};

// レイ走査の結果。
struct SceneHit {
    bool  hit = false;
    float t = 0.0f;                    // origin からヒットまでの距離（dir 正規化前提）
    Vec3  point{0.0f, 0.0f, 0.0f};     // ヒット位置（origin + dir*t）
    Vec3  normal{0.0f, 0.0f, 0.0f};    // ワールド法線
    int   instanceId = -1;             // ヒットしたインスタンス
    int   materialId = -1;             // その材質
};

class Scene {
public:
    // --- 構築（登録） ---

    // 材質をテーブルに追加し、その materialId を返す。
    int addMaterial(const AcousticMaterial& m) {
        materials_.push_back(m);
        return static_cast<int>(materials_.size()) - 1;
    }

    // インスタンスを追加し instanceId を返す。materialId は addMaterial の戻り値。
    // 範囲外 materialId は 0 に丸める（材質未登録なら既定壁を1つ入れておくこと）。
    int addInstance(const Obb& obb, int materialId) {
        Instance inst;
        inst.obb = obb;
        inst.materialId = clampMaterialId(materialId);
        instances_.push_back(inst);
        bvhDirty_ = true;
        return static_cast<int>(instances_.size()) - 1;
    }

    // 既存インスタンスの transform を更新する（動いた分だけ＝O(moved) の土台）。
    // 範囲外 id は無視。
    void updateInstanceTransform(int instanceId, const Obb& obb) {
        if (!validInstance(instanceId)) return;
        instances_[instanceId].obb = obb;
        bvhDirty_ = true;
    }

    // インスタンスの有効/無効を切り替える。
    void setInstanceActive(int instanceId, bool active) {
        if (!validInstance(instanceId)) return;
        instances_[instanceId].active = active;
        bvhDirty_ = true;
    }

    // 全インスタンスを消す（材質テーブルは保持）。毎フレーム作り直す用途。
    void clearInstances() { instances_.clear(); bvhDirty_ = true; }

    // 材質もインスタンスも全消し。
    void clearAll() { instances_.clear(); materials_.clear(); bvhDirty_ = true; }

    // --- クエリ ---

    // origin から dir 方向へ最近傍ヒットを返す。dir は内部で正規化する。BVH-of-OBB で加速。
    SceneHit raycastClosest(const Vec3& origin, const Vec3& dir, float maxDist) const {
        ensureBvh();
        SceneHit best;
        const Vec3 d = normalized(dir);
        float closest = maxDist;
        if (bvhNodes_.empty()) return best;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            float tn;
            Vec3 nn;
            if (!rayIntersectsAabb(origin, d, node.bounds, closest, tn, nn)) continue;
            if (node.count > 0) {  // 葉
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    const Instance& inst = instances_[i];
                    float t;
                    Vec3 n;
                    if (rayIntersectsObb(origin, d, inst.obb, closest, t, n) && t < closest) {
                        closest = t;
                        best.hit = true;
                        best.t = t;
                        best.point = origin + d * t;
                        best.normal = n;
                        best.instanceId = i;
                        best.materialId = inst.materialId;
                    }
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
        return best;
    }

    // 2点間が何かに遮られているか（二値）。1本でも当たれば遮蔽。BVH で加速。
    bool isOccluded(const Vec3& from, const Vec3& to) const {
        ensureBvh();
        if (bvhNodes_.empty()) return false;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            if (!segmentIntersectsAabb(from, to, node.bounds)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    if (segmentIntersectsObb(from, to, instances_[i].obb)) return true;
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
        return false;
    }

    // isOccluded と同じだが、指定インスタンスを無視する。回折中の「当の箱」を除外して
    // 「他の障害物だけ」で P 区間の見通しを見るのに使う（掠める点→音源が当の箱の裏に
    // 再突入して自分で自分を遮る、を防ぐ）。except<0 は isOccluded と同じ。
    bool isOccludedExcept(const Vec3& from, const Vec3& to, int except) const {
        ensureBvh();
        if (bvhNodes_.empty()) return false;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            if (!segmentIntersectsAabb(from, to, node.bounds)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    if (i == except) continue;
                    if (segmentIntersectsObb(from, to, instances_[i].obb)) return true;
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
        return false;
    }

    // 直線 from->to が通る壁の帯域別透過ゲイン(0..1)を outGain に書く。BVH で加速。
    //   壁なし → 全帯域 1.0 / 壁を通るほど（材質次第で高域が）小さくなる。
    void computeTransmission(const Vec3& from, const Vec3& to,
                             float outGain[kNumBands]) const {
        for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;
        ensureBvh();
        if (bvhNodes_.empty()) return;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            if (!segmentIntersectsAabb(from, to, node.bounds)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    const Instance& inst = instances_[i];
                    if (!segmentIntersectsObb(from, to, inst.obb)) continue;
                    const AcousticMaterial& m = materialOf(inst.materialId);
                    for (int b = 0; b < kNumBands; ++b) outGain[b] *= m.transmission[b];
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
    }

    // ── B: キューブマップ エッジカタログ（Phase 4-B, [[dynamic-ray-architecture]] 項1） ──
    // 回折に効くシルエット稜線。UTD のウェッジ幾何込み。
    struct DiffEdge {
        Vec3 p0, p1;      // 稜線の端点（ワールド）
        Vec3 edgeDir;     // 単位エッジ方向
        Vec3 refTangent;  // 0面接線（⊥edge, 外向き。UTD用）
        float n;          // ウェッジ指数（箱=1.5）
        int instance;     // この稜線が属するインスタンス（遮蔽判定の自己除外用。-1=不明）
    };

    // リスナー中心にキューブマップ(6面×res²)でレイを撒き、隣接セルの深度不連続を
    // シルエット稜線として拾ってカタログ化する。1回撒けば全音源で共有できる（リスナー係留）。
    // res=面解像度（例16〜32）、maxDist=レイ到達距離。毎フレーム or 低レートで呼ぶ。
    void buildEdgeCatalog(const Vec3& listener, int res, float maxDist) const {
        edgeCatalog_.clear();
        if (instanceCount() == 0 || res < 2) return;
        const int F = 6, R = res;
        std::vector<float> depth(static_cast<size_t>(F) * R * R, 1e30f);
        std::vector<int> inst(static_cast<size_t>(F) * R * R, -1);
        std::vector<Vec3> hp(static_cast<size_t>(F) * R * R);
        for (int f = 0; f < F; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const float u = (i + 0.5f) / R * 2.0f - 1.0f;
                    const float v = (j + 0.5f) / R * 2.0f - 1.0f;
                    const SceneHit h = raycastClosest(listener, cubeTexelDir(f, u, v), maxDist);
                    const int idx = (f * R + j) * R + i;
                    if (h.hit) { depth[idx] = h.t; inst[idx] = h.instanceId; hp[idx] = h.point; }
                }
        // 隣接セル（右・下、同一面内）で深度が飛ぶ＝シルエット。近い側のヒットを稜線点に。
        auto consider = [&](int a, int b) {
            int nearIdx = -1;
            if (inst[a] >= 0 && inst[b] < 0) nearIdx = a;
            else if (inst[a] < 0 && inst[b] >= 0) nearIdx = b;
            else if (inst[a] >= 0 && inst[b] >= 0) {
                const float dmin = std::min(depth[a], depth[b]);
                if (std::fabs(depth[a] - depth[b]) > 0.15f * dmin)  // 相対15%以上の飛び
                    nearIdx = (depth[a] < depth[b]) ? a : b;
            }
            if (nearIdx >= 0) addCatalogEdge(inst[nearIdx], hp[nearIdx]);
        };
        for (int f = 0; f < F; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const int idx = (f * R + j) * R + i;
                    if (i + 1 < R) consider(idx, (f * R + j) * R + (i + 1));
                    if (j + 1 < R) consider(idx, (f * R + (j + 1)) * R + i);
                }
    }

    int edgeCatalogCount() const { return static_cast<int>(edgeCatalog_.size()); }
    void clearEdgeCatalog() const { edgeCatalog_.clear(); }

    // 【回折の候補列挙（共有）】遮蔽時、回り込み候補エッジを列挙し、各エッジで「掠める点 P
    // （from→P→to が最短になる点＝三分探索）」と余剰経路 δ を、両区間見通せるものだけ
    //   fn(P, delta, edgeDir, refT) で渡す。エッジカタログがあればそれを、無ければ箱稜線を使う
    //   （カタログで有効候補が 0 なら箱へフォールバック）。単一最短(diffractionDetour)も
    //   多重合成(diffractionComposite)もこれを土台にする。
    template <class Fn>
    void forEachDiffractionCandidate(const Vec3& from, const Vec3& to, Fn&& fn) const {
        const float direct = std::max(length(to - from), 1e-4f);
        const float margin = 0.15f;  // 箱を少し膨らませた稜線上を候補に（視線を通しやすく）

        // 稜線 A-B 上で from→P→to が最短になる点（＝直線が掠める角）＋端点から、両区間見通せる
        // 最短を1つ選んで fn に渡す。g(t)=|from-P|+|P-to| は単峰なので三分探索で最小点を得る。
        // exceptInst = この稜線が属する箱（P区間の遮蔽判定から自己除外する。掠める点→音源が
        // 当の箱の裏に再突入して自分で自分を遮る＝候補消滅・中央死角、を防ぐ）。
        auto tryEdge = [&](const Vec3& A, const Vec3& B, const Vec3& edgeDir, const Vec3& refT,
                           int exceptInst) -> bool {
            auto Pf = [&](float t) { return A + (B - A) * t; };
            auto g  = [&](float t) { const Vec3 p = Pf(t); return length(p - from) + length(to - p); };
            float lo = 0.0f, hi = 1.0f;
            for (int it = 0; it < 20; ++it) {
                const float m1 = lo + (hi - lo) * (1.0f / 3.0f);
                const float m2 = hi - (hi - lo) * (1.0f / 3.0f);
                if (g(m1) < g(m2)) hi = m2; else lo = m1;
            }
            const Vec3 cands[3] = {Pf(0.5f * (lo + hi)), A, B};  // 掠める角＋端点
            bool any = false; float bd = 0.0f; Vec3 bp{0, 0, 0};
            for (const Vec3& P : cands) {
                // 当の箱を除外して「他の障害物だけ」で両区間を見通せる点だけ採用。
                if (isOccludedExcept(from, P, exceptInst) || isOccludedExcept(P, to, exceptInst)) continue;
                float d = length(P - from) + length(to - P) - direct;
                if (d < 0.0f) d = 0.0f;
                if (!any || d < bd) { any = true; bd = d; bp = P; }
            }
            if (any) fn(bp, bd, edgeDir, refT);
            return any;
        };

        // ★回折は「直接経路を実際に塞いでいる箱（ブロッカー）」の稜線だけを回る。部屋の床/天井/壁など
        //   直接線と交差しない箱の稜線まで候補にすると、無関係な遠い方向へ候補が飛んで合成を濁す。
        //   直接 from→to の線分が OBB と交差する箱＝ブロッカー、だけを対象にする。
        auto isBlocker = [&](int inst) {
            return inst >= 0 && inst < instanceCount() && instances_[inst].active &&
                   segmentIntersectsObb(from, to, instances_[inst].obb);
        };

        // エッジカタログ（B: キューブマップ由来のシルエット稜線）と箱の実稜線の両方を候補にする（和集合）。
        // カタログだけだと片側しか拾えないことがある（→両側から鳴らない・エッジ入替で方向が飛ぶ）ので、
        // 箱の 12 稜線も必ず加えて両側を確実に取る。ただし双方ともブロッカーの稜線に限る。
        if (!edgeCatalog_.empty())
            for (const DiffEdge& e : edgeCatalog_)
                if (isBlocker(e.instance))
                    tryEdge(e.p0, e.p1, e.edgeDir, e.refTangent, e.instance);

        // 箱(OBB)の 12 稜線それぞれで掠める角を探索（角のみだと薄い壁で失敗するので稜線全体）。
        const int fixed2[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
            const Obb& b = inst.obb;
            if (!segmentIntersectsObb(from, to, b)) continue;  // ブロッカーだけ回折対象
            const Vec3 ax[3] = {b.axisX, b.axisY, b.axisZ};
            const float h[3] = {b.halfExtents.x + margin, b.halfExtents.y + margin,
                                b.halfExtents.z + margin};
            auto pointAt = [&](const float a[3]) {
                return b.center + ax[0] * (h[0] * a[0]) + ax[1] * (h[1] * a[1]) + ax[2] * (h[2] * a[2]);
            };
            for (int freeAxis = 0; freeAxis < 3; ++freeAxis) {
                const int o1 = (freeAxis + 1) % 3;
                const int o2 = (freeAxis + 2) % 3;
                for (const auto& sg : fixed2) {
                    // 稜線方向＝freeAxis 軸。0面(o1面)接線＝エッジから外側へ（−o2·sg1）。
                    const Vec3 edgeDir = ax[freeAxis];
                    const Vec3 refT = ax[o2] * (-static_cast<float>(sg[1]));
                    float aA[3], aB[3];
                    aA[freeAxis] = -1.0f; aA[o1] = static_cast<float>(sg[0]); aA[o2] = static_cast<float>(sg[1]);
                    aB[freeAxis] = +1.0f; aB[o1] = static_cast<float>(sg[0]); aB[o2] = static_cast<float>(sg[1]);
                    tryEdge(pointAt(aA), pointAt(aB), edgeDir, refT, i);
                }
            }
        }
    }

    // 【回折(Phase 1.5 暫定・可視化用)】直接 from->to が遮蔽されているとき、稜線を回る「最短迂回」の
    // 余剰経路長 δ(= 迂回長 − 直線長, m)を返し、最良の迂回点を outPoint に書く。遮蔽なし or 迂回路なしは
    // -1（outPoint 不定）。outEdgeDir/outRefTangent : null でなければ最良稜線の「エッジ方向」「0面接線」を書く。
    float diffractionDetour(const Vec3& from, const Vec3& to, Vec3& outPoint,
                            Vec3* outEdgeDir = nullptr, Vec3* outRefTangent = nullptr) const {
        if (!isOccluded(from, to)) return -1.0f;
        float best = -1.0f;
        Vec3 bestP{0, 0, 0}, bestEdge{1, 0, 0}, bestRefT{0, 1, 0};
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3& e, const Vec3& r) {
            if (best < 0.0f || d < best) { best = d; bestP = P; bestEdge = e; bestRefT = r; }
        });
        if (best >= 0.0f) {
            outPoint = bestP;
            if (outEdgeDir) *outEdgeDir = bestEdge;
            if (outRefTangent) *outRefTangent = bestRefT;
        }
        return best;
    }

    // 【回折の多重エッジ合成】遮蔽時、複数の回り込みエッジを合成した「届く方向」を outDir に書く。
    // 単一最短エッジだけだと (1)エッジが入れ替わった瞬間に方向が飛ぶ (2)両側空いてても片側からしか
    // 鳴らない。各エッジの掠める点の方向を、迂回の短さ w=exp(-(δ-δmin)/scale) で加重合成＝到来方向の
    // インテンシティ重心。→ 滑らかに切替わり、複数の開口があれば両方から届く。
    // 戻り値 = 最短δ（距離減衰/重み用、-1=迂回路なし）。outDir は迂回路なしのとき未書換。
    float diffractionComposite(const Vec3& from, const Vec3& to, Vec3& outDir) const {
        if (!isOccluded(from, to)) return -1.0f;
        constexpr int kMaxCand = 64;  // 同時に合成する回り込み経路の上限（数十本まで）
        Vec3 dirs[kMaxCand];
        float deltas[kMaxCand];
        int nc = 0;
        float dmin = -1.0f;
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3&, const Vec3&) {
            if (nc < kMaxCand) { dirs[nc] = normalized(P - from); deltas[nc] = d; ++nc; }
            if (dmin < 0.0f || d < dmin) dmin = d;
        });
        if (nc == 0) return -1.0f;
        // 全候補を同等の重みで合成（最短偏重をやめる）。どのエッジも定位に等しく効くので、
        // エッジが1本入れ替わっても方向の変化が概ね 1/本数 に収まり、切替のガクつきが減る。
        Vec3 acc{0, 0, 0};
        for (int i = 0; i < nc; ++i) acc = acc + dirs[i];
        if (length(acc) < 1e-6f) acc = to - from;
        outDir = normalized(acc);
        return dmin;
    }

    // 【可視化】遮蔽時の回折候補（掠める点 P と余剰δ）を最大 maxCount 個 outP/outDelta に書き、
    // 書いた個数を返す。全候補を描画し、最短δのものを呼び出し側で min を取って色分けするデバッグ用。
    int diffractionCandidates(const Vec3& from, const Vec3& to,
                              Vec3* outP, float* outDelta, int maxCount) const {
        if (!outP || !outDelta || maxCount <= 0) return 0;
        if (!isOccluded(from, to)) return 0;
        int n = 0;
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3&, const Vec3&) {
            if (n < maxCount) { outP[n] = P; outDelta[n] = d; ++n; }
        });
        return n;
    }

    // 【回折を二次音源として鳴らす（GTD/ホイヘンス）】遮蔽時、回り込みエッジを「エッジ＝二次音源」として
    // 最大 maxN 個の仮想音源（方向つき）に束ねて返す。近い方向のエッジはクラスタ統合するので、両側に
    // 開口があれば左右2音源…のように分かれ、リスナー移動で各ゲインが滑らかに変わる（＝1点合成の飛びを排除）。
    //   outPos[k]  : 二次音源のワールド位置（= listener + 方向 × 音源距離。Wwise がこの方向へ定位）
    //   outGain[k] : 相対ゲイン（全クラスタ合計で正規化, 短い迂回ほど大）。総和 ≤ 1
    // 戻り値 = 書き込んだ音源数。遮蔽なし/迂回なしは 0。
    int computeDiffractionSources(const Vec3& listener, const Vec3& source,
                                  Vec3* outPos, float* outGain, int maxN) const {
        if (!outPos || !outGain || maxN <= 0) return 0;
        if (!isOccluded(listener, source)) return 0;

        // 低域加重（回折は低域が回り込む）で UTD 6帯域を1スカラに畳む＝各エッジの広帯域ゲイン重み。
        auto bbGain = [](const float g[kNumBands]) {
            const float w[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
            float gs = 0.0f, ws = 0.0f;
            for (int b = 0; b < kNumBands; ++b) { gs += w[b] * g[b]; ws += w[b]; }
            return ws > 0.0f ? gs / ws : 0.0f;
        };

        // 開口＝方向クラスタ。掠める点は「クラスタ内エッジの UTD重み付き重心」で連続にスライドさせる
        // （手前稜線↔奥稜線の乗り換えが、重心が滑らかに移る＝飛ばない）。
        struct Cl { Vec3 pAcc; Vec3 dir; float w; };  // pAcc=Σ w*P（重心用）, dir=平均方向, w=Σ
        Cl cl[24];
        int ncl = 0;
        const float cosThresh = 0.90f;  // ~25°以内は同じ開口として統合
        forEachDiffractionCandidate(listener, source, [&](const Vec3& P, float, const Vec3& edgeDir, const Vec3& refT) {
            float g6[kNumBands];
            utd::utdWedgeGain(source, P, listener, edgeDir, refT, 1.5f, g6);  // このエッジの UTD ゲイン
            const float w = bbGain(g6);
            if (w < 1e-4f) return;
            const Vec3 dir = normalized(P - listener);
            int best = -1; float bestDot = cosThresh;
            for (int i = 0; i < ncl; ++i) { const float dt = dot(dir, cl[i].dir); if (dt > bestDot) { bestDot = dt; best = i; } }
            if (best >= 0) {
                cl[best].pAcc = cl[best].pAcc + P * w;
                cl[best].dir = normalized(cl[best].dir * cl[best].w + dir * w);
                cl[best].w += w;
            } else if (ncl < 24) {
                cl[ncl].pAcc = P * w; cl[ncl].dir = dir; cl[ncl].w = w; ++ncl;
            }
        });
        if (ncl == 0) return 0;

        float sum = 0.0f; for (int i = 0; i < ncl; ++i) sum += cl[i].w;
        bool used[24] = {false};
        int n = 0;
        while (n < maxN && n < ncl) {  // 重み上位から maxN 本を採用
            int bi = -1; float bw = -1.0f;
            for (int i = 0; i < ncl; ++i) if (!used[i] && cl[i].w > bw) { bw = cl[i].w; bi = i; }
            if (bi < 0) break;
            used[bi] = true;
            const Vec3 Pc = cl[bi].pAcc * (1.0f / std::max(cl[bi].w, 1e-6f));  // UTD重み付き重心（連続）
            const Vec3 dir = normalized(Pc - listener);
            const float pathLen = length(Pc - listener) + length(source - Pc);  // 実経路長
            outPos[n] = listener + dir * std::max(pathLen, 0.5f);  // 方向＝重心 / 距離＝実経路長
            outGain[n] = cl[bi].w / std::max(sum, 1e-6f);
            ++n;
        }
        return n;
    }

    // 【回折(Phase 1.5)】from->to の帯域別回折ゲイン(0..1)。
    //   遮蔽なし → 全帯域 1.0 / 迂回路あり → Maekawa（低域ほど回り込む）/ 迂回路なし → 0。
    void computeDiffraction(const Vec3& from, const Vec3& to, float outGain[kNumBands]) const {
        if (!isOccluded(from, to)) {
            for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;
            return;
        }
        Vec3 p, edgeDir, refT;
        const float delta = diffractionDetour(from, to, p, &edgeDir, &refT);
        if (delta < 0.0f) {
            for (int b = 0; b < kNumBands; ++b) outGain[b] = 0.0f;
            return;
        }
        // UTD（複素ウェッジ回折）。from=リスナー/to=音源 の想定 → utd(source,P,listener)。
        utd::utdWedgeGain(to, p, from, edgeDir, refT, 1.5f, outGain);
    }

    // 【役割1(Phase 2)：ソフト遮蔽】直接音を「音源周りの複数サンプル」で測り、遮られた割合を
    // 滑らかに出す（＝半影）。二値の見通し判定と違い、壁の縁をまたぐとき段差でなく連続に変わる。
    //   outGain[b] = 直接透過(ソフト) と 回折フロア(遮蔽割合でフェードイン) の大きい方。
    //   numSamples : 音源周りのサンプル数（例 8） / sourceRadius : サンプル半径(m, 例 0.4)
    // 段差の主因（直接 1.0→材質透過 と 回折 1.0→Maekawa の一斉切替）を連続化する。
    // ※ 影境界の厳密な連続化は Phase 4（UTD 遷移関数）で。ここはその手前の緩和。
    //   outDetourDelta : null でなければ 回折の迂回余剰長 δ(m) を書く（非遮蔽=0 / 完全遮蔽=大）。
    //                    ステアの直接項を「実効経路=直接距離+δ」で重み付けするのに使う。
    void computeDirectSoft(const Vec3& listener, const Vec3& source, float outGain[kNumBands],
                           int numSamples, float sourceRadius, float* outDetourDelta) const {
        using namespace scene_detail;
        Vec3 dir = source - listener;
        const float dist = length(dir);
        if (dist < 1e-4f) { for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f; return; }
        dir = dir * (1.0f / dist);
        // 視線に垂直な基底（音源周りの円盤サンプル用）。
        const Vec3 t = (std::fabs(dir.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 u = normalized(cross(dir, t));
        const Vec3 v = cross(dir, u);

        const int N = numSamples < 1 ? 1 : numSamples;
        float transAccum[kNumBands] = {0, 0, 0, 0, 0, 0};
        int occCount = 0;
        uint32_t rng = static_cast<uint32_t>(dist * 1000.0f) * 2654435761u + 98765u;
        for (int i = 0; i < N; ++i) {
            const float rr = sourceRadius * std::sqrt(hashRand01(rng));
            const float aa = 2.0f * kPiF * hashRand01(rng);
            const Vec3 p = source + u * (rr * std::cos(aa)) + v * (rr * std::sin(aa));
            float g[kNumBands];
            computeTransmission(listener, p, g);
            for (int b = 0; b < kNumBands; ++b) transAccum[b] += g[b];
            if (g[0] < 1.0f) ++occCount;  // このサンプルは壁を通った
        }
        const float inv = 1.0f / static_cast<float>(N);
        const float occFrac = static_cast<float>(occCount) * inv;

        // 回折フロア：中心が遮蔽なら δ から Maekawa、非遮蔽なら 1.0。δ も一緒に得る。
        const bool centerOcc = isOccluded(listener, source);
        float dif[kNumBands];
        float detourDelta = 0.0f;   // 迂回余剰長(m)。非遮蔽=0 / 完全遮蔽(迂回路なし)=大
        if (!centerOcc) {
            for (int b = 0; b < kNumBands; ++b) dif[b] = 1.0f;
        } else {
            Vec3 dp, edgeDir, refT;
            const float delta = diffractionDetour(listener, source, dp, &edgeDir, &refT);
            if (delta < 0.0f) {
                for (int b = 0; b < kNumBands; ++b) dif[b] = 0.0f;  // 完全遮蔽
                detourDelta = 1e9f;
            } else {
                utd::utdWedgeGain(source, dp, listener, edgeDir, refT, 1.5f, dif);  // UTD
                detourDelta = delta;
            }
        }
        for (int b = 0; b < kNumBands; ++b) {
            const float soft = transAccum[b] * inv;                       // 滑らかな直接透過
            const float diffFloor = centerOcc ? dif[b] * occFrac : 0.0f;  // 影で徐々に立つ回折
            outGain[b] = clamp01(std::max(soft, diffFloor));
        }
        if (outDetourDelta) *outDetourDelta = detourDelta;
    }

    // 【役割2(Phase 5)：反射込み遮蔽】リスナー起点で numRays 本のレイを撒き、壁で反射
    // させながら各バウンス点から音源へ next-event でつなぐ。直接(透過⊕回折)＋反射で回り込む
    // 成分を帯域別に積み、遮蔽量(0..1)を返す。反射経路があるので壁裏でも 1.0 に張り付かない。
    //   outBands6 : null でなければ 直接⊕回折⊕反射 の帯域別生存(0..1) を書く。
    //   ※ 相対減衰(遠回りほど弱い)のみ。絶対距離減衰は Wwise 側。旧 acoustic_world から移植。
    float occlusionReflected(const Vec3& source, const Vec3& listener,
                             float* outBands6, int numRays, int maxBounces) const {
        using namespace scene_detail;
        const float kEps = 1e-3f;
        const float refDist = std::max(length(source - listener), 1e-3f);

        // 1) 直接経路＝ソフト遮蔽（透過⊕回折を半影で連続化）。
        float total[kNumBands];
        computeDirectSoft(listener, source, total, 8, 0.4f, nullptr);

        // 2) 反射で回り込む成分（リスナーレイ＋next-event）。
        if (numRays > 0 && maxBounces > 0 && instanceCount() > 0) {
            float reflected[kNumBands] = {0, 0, 0, 0, 0, 0};
            const float maxDist = refDist * 8.0f + 50.0f;
            for (int i = 0; i < numRays; ++i) {
                Vec3 o = listener;
                Vec3 d = fibonacciSphereDir(i, numRays);
                uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
                float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
                float totalLen = 0.0f;
                float remaining = maxDist;
                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    const SceneHit hit = raycastClosest(o, d, remaining);
                    if (!hit.hit) break;
                    totalLen += hit.t;
                    remaining -= hit.t;
                    if (remaining <= kEps) break;
                    const AcousticMaterial& mat = materialOf(hit.materialId);
                    const Vec3 q = hit.point + hit.normal * 0.02f;  // 自己ヒット防止に浮かせる
                    float seg[kNumBands];
                    computeTransmission(q, source, seg);
                    const float pathLen = totalLen + length(source - q);
                    float atten = refDist / pathLen;
                    atten *= atten;
                    if (atten > 1.0f) atten = 1.0f;
                    for (int b = 0; b < kNumBands; ++b) {
                        const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                        reflected[b] += carry[b] * refl * seg[b] * atten;
                        carry[b] *= refl;
                    }
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
            const float inv = 1.0f / static_cast<float>(numRays);
            for (int b = 0; b < kNumBands; ++b) total[b] = clamp01(total[b] + reflected[b] * inv);
        }

        if (outBands6) for (int b = 0; b < kNumBands; ++b) outBands6[b] = total[b];
        float mean = 0.0f;
        for (int b = 0; b < kNumBands; ++b) mean += total[b];
        mean /= kNumBands;
        return clamp01(1.0f - mean);
    }

    // 【役割2・複数音源（リスナーベース共有）】リスナーレイを numRays 本だけ撒き（raycast は
    // 音源数に依存しない＝1回ぶん）、各バウンス点から全音源へ next-event でつなぐ。
    //   outOcc[j]   : 音源 j の遮蔽量(0..1)（null 可）
    //   outBands    : null でなければ j*kNumBands+b に 直接⊕回折⊕反射 の帯域別生存を書く
    // 大量音源でも raycast コストが増えない（next-event のみ音源数ぶん）。旧実装から移植。
    //   outDir : null でなければ j*3+{0,1,2} に「エネルギーが届く支配方向(単位ベクトル)」を書く。
    //            遮蔽時は反射/回折が支配し、音源真方向でなく“回り込んで届く方向”になる。
    //            Wwise 側でこの方向に仮想エミッタを置き直すと「届く方向から聞こえる」になる。
    //   directWeight : ステアの「直接項」の重み係数。大きいほど音源方向へ定位が張り付く。
    //                  直接項は directGain × (直接距離/実効経路)² × directWeight で重み付け。
    //                  実効経路 = 直接距離 + 回折δ（経路補完のぶん加算）＝遠回りほど直接が弱まりステアが開く。
    void occlusionReflectedMulti(const Vec3& listener, const Vec3* sources, int count,
                                 float* outOcc, float* outBands, float* outDir,
                                 float directWeight, int numRays, int maxBounces) const {
        using namespace scene_detail;
        if (!sources || count <= 0) return;
        const float kEps = 1e-3f;
        std::vector<float> total(static_cast<size_t>(count) * kNumBands);
        std::vector<float> reflected(static_cast<size_t>(count) * kNumBands, 0.0f);
        std::vector<float> refDist(static_cast<size_t>(count));
        std::vector<Vec3> dirAccum(static_cast<size_t>(count), Vec3(0.0f, 0.0f, 0.0f));

        // 1) 直接（音源ごと）＝ソフト遮蔽。定位を担う「第一波面」の到来方向を作る：
        //    見通せる → 音源方向 / 遮蔽 → 回り込む角（回折の掠める点）方向。反射は方向に効かせない
        //    （反射は現実でも先行音効果でほぼ定位せず、幅・広がりに化けるため。docs/EARLY_REFLECTIONS.md）。
        for (int j = 0; j < count; ++j) {
            float detourDelta = 0.0f;
            computeDirectSoft(listener, sources[j], &total[j * kNumBands], 8, 0.4f, &detourDelta);
            refDist[j] = std::max(length(sources[j] - listener), 1e-3f);
            float directMean = 0.0f;
            for (int b = 0; b < kNumBands; ++b) directMean += total[j * kNumBands + b];
            directMean /= kNumBands;

            // 第一波面の方向と重み。見通せれば音源方向、塞がれていれば複数エッジ合成の回り込み方向
            //（滑らかに切替わり、複数開口があれば両側から）。実効経路が長いほど定位を弱める。
            Vec3 firstDir = normalized(sources[j] - listener);
            float firstW = directMean;  // 見通せる＝そのまま強い定位
            if (isOccluded(listener, sources[j])) {
                Vec3 cdir;
                const float dd = diffractionComposite(listener, sources[j], cdir);
                if (dd >= 0.0f) {
                    // 遮蔽の“深さ”δで 音源方向→合成回り込み方向 を連続ブレンド。
                    // δ=0（掠める＝遮蔽の境界）で音源方向に一致するので、遮蔽に入る瞬間の飛びが出ない。
                    const Vec3 srcDir = normalized(sources[j] - listener);
                    const float tau = 1.5f;             // ブレンドの深さスケール(m)。小=すぐ回り込み側へ
                    const float t = dd / (dd + tau);    // 0(境界)→1(深い遮蔽)
                    firstDir = normalized(srcDir * (1.0f - t) + cdir * t);
                    const float distFactor = refDist[j] / std::max(refDist[j] + dd, 1e-3f);
                    firstW = directMean * distFactor * distFactor;
                } else {
                    firstW = 0.0f;  // 迂回路なし＝定位ほぼ無し（ホスト側で真方向にフォールバック）
                }
            }
            dirAccum[j] = firstDir * (firstW * directWeight);
        }

        // 2) 反射（リスナーレイは1回だけ＝音源数非依存）。到来方向は初期レイ方向 d0。
        if (numRays > 0 && maxBounces > 0 && instanceCount() > 0) {
            float maxRef = 1e-3f;
            for (int j = 0; j < count; ++j) maxRef = std::max(maxRef, refDist[j]);
            const float maxDist = maxRef * 8.0f + 50.0f;
            for (int i = 0; i < numRays; ++i) {
                Vec3 o = listener;
                Vec3 d = fibonacciSphereDir(i, numRays);
                uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
                float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
                float totalLen = 0.0f;
                float remaining = maxDist;
                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    const SceneHit hit = raycastClosest(o, d, remaining);  // ← 共有
                    if (!hit.hit) break;
                    totalLen += hit.t;
                    remaining -= hit.t;
                    if (remaining <= kEps) break;
                    const AcousticMaterial& mat = materialOf(hit.materialId);
                    const Vec3 q = hit.point + hit.normal * 0.02f;
                    float refl[kNumBands];
                    for (int b = 0; b < kNumBands; ++b)
                        refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    for (int j = 0; j < count; ++j) {  // ここだけ音源数ぶん
                        float seg[kNumBands];
                        computeTransmission(q, sources[j], seg);
                        const float pathLen = totalLen + length(sources[j] - q);
                        float atten = refDist[j] / pathLen;
                        atten *= atten;
                        if (atten > 1.0f) atten = 1.0f;
                        // 反射エネルギーは帯域生存(音量・広がり)には効くが、方向(dirAccum)には
                        // 効かせない（反射は定位を持たせない＝拡散扱い）。
                        for (int b = 0; b < kNumBands; ++b)
                            reflected[j * kNumBands + b] += carry[b] * refl[b] * seg[b] * atten;
                    }
                    for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];  // 継続レイの減衰
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
            const float inv = 1.0f / static_cast<float>(numRays);
            for (int j = 0; j < count; ++j)
                for (int b = 0; b < kNumBands; ++b)
                    total[j * kNumBands + b] =
                        clamp01(total[j * kNumBands + b] + reflected[j * kNumBands + b] * inv);
        }

        // 出力。
        for (int j = 0; j < count; ++j) {
            if (outBands)
                for (int b = 0; b < kNumBands; ++b)
                    outBands[j * kNumBands + b] = total[j * kNumBands + b];
            float mean = 0.0f;
            for (int b = 0; b < kNumBands; ++b) mean += total[j * kNumBands + b];
            mean /= kNumBands;
            if (outOcc) outOcc[j] = clamp01(1.0f - mean);
            if (outDir) {
                Vec3 dv = dirAccum[j];
                if (length(dv) < 1e-6f) dv = sources[j] - listener;  // エネルギー無ければ真方向
                dv = normalized(dv);
                outDir[j * 3 + 0] = dv.x;
                outDir[j * 3 + 1] = dv.y;
                outDir[j * 3 + 2] = dv.z;
            }
        }
    }

    // 【残響(Phase 6)：エコグラム】リスナーに届くエネルギーを到達時間ビンに積む。
    // 直接音＋反射（リスナーレイ1回＝共有・多バウンス、各バウンスから全音源へ next-event）。
    //   outBins[k] : 時間 [k*binSeconds, (k+1)*binSeconds) に届く合計エネルギー（広帯域平均）
    // 直接音の大ピーク→初期反射→指数減衰の尾、という残響の形が出る。RT60/wet 算出の土台。
    // 減衰は吸収 refl^n（carry）が担い、絶対距離の 1/r² は掛けない（相対の“形”。旧実装から移植）。
    void computeEchogram(const Vec3& listener, const Vec3* sources, int count,
                         float* outBins, int numBins, float binSeconds, float speedOfSound,
                         int numRays, int maxBounces) const {
        // 帯域版を計算して広帯域平均に潰す（実装は1本に保つ）。
        if (!outBins || numBins <= 0) return;
        std::vector<float> bands(static_cast<size_t>(numBins) * kNumBands, 0.0f);
        // 旧APIは「相対の形」用途（RT60推定）なので広がり損失なし＝従来どおり。
        computeEchogramBands(listener, sources, count, bands.data(), numBins,
                             binSeconds, speedOfSound, numRays, maxBounces, 0.0f);
        for (int k = 0; k < numBins; ++k) {
            float e = 0.0f;
            for (int b = 0; b < kNumBands; ++b) e += bands[static_cast<size_t>(k) * kNumBands + b];
            outBins[k] = e / kNumBands;
        }
    }

    // 【残響：帯域別エコグラム】上と同じだが、帯域を潰さず outBins[k*kNumBands + b] に書く。
    //   実際の部屋は高域ほど速く減衰する（吸収が高域で大きい）。広帯域平均だとその差が消え、
    //   後期尾の「暗くなっていく」挙動を別途ダンピングで捏造することになる。
    //   IR 畳み込みで“実測された尾”を鳴らすには、この帯域別の減衰カーブが要る。
    // distanceRef: 音源からの広がり損失の基準距離。0 以下で無効（従来どおり損失なし）。
    //   減衰は host 側の早期反射タップと同じ規約 atten = distanceRef / max(d, distanceRef)、
    //   エネルギーにはその2乗を掛ける。
    //
    //   ※ 掛ける相手は「音源→反射点」の区間長であって、リスナーまでの総経路長ではない。
    //     反射点→リスナー側の広がりは、レイ1本が立体角を代表していること自体が担っている。
    //     総経路長で掛けると、時間が経つほど（＝経路が長いほど）一律に減衰が強まり、
    //     尾に本来存在しない 1/t² の減衰が乗る。拡散音場のエネルギー密度は空間的にほぼ一様で、
    //     時間減衰は吸音だけが担うのが正しい。広い部屋の中央ほど経路が長いので、
    //     総経路長で掛けるとそこの反響だけが痩せる。
    void computeEchogramBands(const Vec3& listener, const Vec3* sources, int count,
                              float* outBins, int numBins, float binSeconds, float speedOfSound,
                              int numRays, int maxBounces, float distanceRef) const {
        using namespace scene_detail;
        if (!outBins || numBins <= 0 || !sources || count <= 0) return;
        for (int k = 0; k < numBins * kNumBands; ++k) outBins[k] = 0.0f;

        const float kEps = 1e-3f;
        const float invC = (speedOfSound > 1e-3f) ? 1.0f / speedOfSound : 0.0f;
        const float invBin = (binSeconds > 1e-6f) ? 1.0f / binSeconds : 0.0f;

        // 音源からの距離 d に対する広がり損失（エネルギー）。
        auto spreadEnergy = [distanceRef](float d) -> float {
            if (distanceRef <= 0.0f) return 1.0f;
            const float a = distanceRef / std::max(d, distanceRef);
            return a * a;
        };

        // 帯域別にビンへ積む。energy6 は kNumBands 要素。
        auto addBin = [&](float dist, const float* energy6, float scale) {
            const int k = static_cast<int>(dist * invC * invBin);
            if (k < 0 || k >= numBins) return;
            float* dst = outBins + static_cast<size_t>(k) * kNumBands;
            for (int b = 0; b < kNumBands; ++b) {
                const float e = energy6[b] * scale;
                if (e > 0.0f) dst[b] += e;
            }
        };

        // 直接音（直線の透過）。音源→リスナーの広がり損失を掛ける。
        for (int j = 0; j < count; ++j) {
            float g[kNumBands];
            computeTransmission(listener, sources[j], g);
            const float d = length(sources[j] - listener);
            addBin(d, g, spreadEnergy(d));
        }

        // 反射（共有レイ・尾が窓内に入るまでレイを伸ばす）。
        if (numRays > 0 && maxBounces > 0 && instanceCount() > 0) {
            float maxRef = 1e-3f;
            for (int j = 0; j < count; ++j)
                maxRef = std::max(maxRef, std::max(length(sources[j] - listener), 1e-3f));
            const float windowDist = static_cast<float>(numBins) * binSeconds * speedOfSound;
            const float maxDist = std::max(maxRef * 8.0f + 50.0f, windowDist);
            // 反射場は球面全方向からの入射を積分したもの。立体角係数 2π で結合（拡散場を立てる）。
            const float kDiffuseCoupling = 6.2831853f;
            const float inv = kDiffuseCoupling / static_cast<float>(numRays);

            for (int i = 0; i < numRays; ++i) {
                Vec3 o = listener;
                Vec3 d = fibonacciSphereDir(i, numRays);
                uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
                float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
                float totalLen = 0.0f;
                float remaining = maxDist;

                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    const SceneHit hit = raycastClosest(o, d, remaining);
                    if (!hit.hit) break;
                    totalLen += hit.t;
                    remaining -= hit.t;
                    if (remaining <= kEps) break;
                    const AcousticMaterial& mat = materialOf(hit.materialId);
                    const Vec3 q = hit.point + hit.normal * 0.02f;
                    float refl[kNumBands];
                    for (int b = 0; b < kNumBands; ++b)
                        refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);

                    for (int j = 0; j < count; ++j) {
                        float seg[kNumBands];
                        computeTransmission(q, sources[j], seg);
                        const float srcLeg = length(sources[j] - q);   // 音源→反射点
                        const float pathLen = totalLen + srcLeg;
                        float e[kNumBands];
                        for (int b = 0; b < kNumBands; ++b) e[b] = carry[b] * refl[b] * seg[b];
                        // 広がり損失は「音源→反射点」の区間にだけ掛ける（総経路長ではない）。
                        addBin(pathLen, e, inv * spreadEnergy(srcLeg));
                    }
                    for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
        }
    }

    // 【早期反射タップ(A)】source→…→listener の主要な初期反射を最大 maxTaps 本抽出する。
    //   出力タップ = imageSourcePos（= listener + 到来方向×経路長。定位/距離減衰用）＋6帯域ゲイン。
    //   リスナー起点レイ×next-event で各反射の {到来方向 d0, 経路長, 帯域ゲイン} を集め、
    //   エネルギー強い順に、方向が近いものはまとめて上位を返す。Wwise Reflect の image source
    //   や仮想エミッタで「方向つき反射音」として鳴らす想定。戻り値=書き込んだタップ数。
    int computeEarlyReflections(const Vec3& listener, const Vec3& source,
                                Vec3* outImagePos, float* outGain, int maxTaps,
                                int numRays, int maxBounces) const {
        using namespace scene_detail;
        if (maxTaps <= 0 || !outImagePos || !outGain || instanceCount() == 0) return 0;
        struct Tap { Vec3 dir; float len; float g[kNumBands]; float e; };
        std::vector<Tap> taps;
        const float kEps = 1e-3f;
        const float refDist = std::max(length(source - listener), 1e-3f);
        const float maxDist = refDist * 8.0f + 50.0f;
        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            const Vec3 d0 = d;  // リスナーに届く方向（第1レグ）
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
            float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
            float totalLen = 0.0f;
            float remaining = maxDist;
            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const SceneHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.t;
                remaining -= hit.t;
                if (remaining <= kEps) break;
                const AcousticMaterial& mat = materialOf(hit.materialId);
                const Vec3 q = hit.point + hit.normal * 0.02f;
                float seg[kNumBands];
                computeTransmission(q, source, seg);
                Tap t;
                t.dir = d0;
                t.len = totalLen + length(source - q);
                float e = 0.0f;
                for (int b = 0; b < kNumBands; ++b) {
                    const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    t.g[b] = carry[b] * refl * seg[b];
                    e += t.g[b];
                    carry[b] *= refl;
                }
                t.e = e / kNumBands;
                if (t.e > 1e-4f) taps.push_back(t);
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                o = q;
            }
        }
        // エネルギー降順に、方向が近い(>~25°で同一とみなす)ものはまとめて上位を採る。
        std::sort(taps.begin(), taps.end(), [](const Tap& a, const Tap& b) { return a.e > b.e; });
        Vec3 pickedDir[64];
        int n = 0;
        for (const Tap& t : taps) {
            if (n >= maxTaps || n >= 64) break;
            bool dup = false;
            for (int k = 0; k < n; ++k)
                if (dot(t.dir, pickedDir[k]) > 0.9f) { dup = true; break; }
            if (dup) continue;
            pickedDir[n] = t.dir;
            outImagePos[n] = listener + t.dir * t.len;
            for (int b = 0; b < kNumBands; ++b) outGain[n * kNumBands + b] = t.g[b];
            ++n;
        }
        return n;
    }

    // 【可視化】origin から dir 方向へ鏡面反射で maxBounces 回まで追い、通過点を outPoints に書く。
    //   outPoints[0]=origin、以降=反射点、最後=終端（開放空間での到達点 or 最終反射点）。
    //   返り値=書き込んだ点数。反響経路(reflection path)を Unity で線描画するための土台。
    int traceReflectionPath(const Vec3& origin, const Vec3& dir, float maxDist, int maxBounces,
                            Vec3* outPoints, int maxPoints) const {
        using namespace scene_detail;
        if (!outPoints || maxPoints < 2) return 0;
        Vec3 o = origin;
        Vec3 d = normalized(dir);
        int n = 0;
        outPoints[n++] = o;
        float remaining = maxDist;
        for (int b = 0; b < maxBounces && n < maxPoints; ++b) {
            const SceneHit hit = raycastClosest(o, d, remaining);
            if (!hit.hit) {
                if (n < maxPoints) outPoints[n++] = o + d * remaining;  // 開放空間へ延長
                return n;
            }
            if (n < maxPoints) outPoints[n++] = hit.point;
            remaining -= hit.t;
            if (remaining <= 1e-3f) return n;
            d = reflect(d, hit.normal);
            o = hit.point + hit.normal * 0.02f;  // 自己ヒット防止
        }
        return n;
    }

    // --- リスナー / 音源の保持（API移行 段1: docs/API_MIGRATION_PLAN.md）---
    //
    // これまで listener/source は「クエリのたびに引数で渡す」ものだった。
    // ホストが音源配列を持ち、音源ぶんループするのもホストの仕事になっていたが、
    // これは SPEC §2 の「エンジンが音源を登録で保持し、内部でループする」に反する。
    //
    // ここで保持するようにしておくと、後段の af_Update（1発で全音源を回す）と
    // ワーカースレッド化（入力をスナップショットして投げる）が素直に乗る。
    // 段1では保持するだけで、既存クエリは引数版のまま＝挙動は一切変わらない。
    struct SourceEntry {
        unsigned long long id = 0;
        Vec3 pos{};
        bool active = true;
    };

    void setListener(const Vec3& pos) { listenerPos_ = pos; }
    const Vec3& listenerPos() const { return listenerPos_; }

    // 音源を登録/更新する。同じ id なら位置だけ更新（毎フレーム呼ばれる想定）。
    void setSource(unsigned long long id, const Vec3& pos) {
        for (auto& s : sources_) {
            if (s.id == id) { s.pos = pos; s.active = true; return; }
        }
        SourceEntry e;
        e.id = id;
        e.pos = pos;
        sources_.push_back(e);
    }

    void removeSource(unsigned long long id) {
        for (size_t i = 0; i < sources_.size(); ++i) {
            if (sources_[i].id == id) {
                sources_.erase(sources_.begin() + static_cast<long>(i));
                return;
            }
        }
    }

    void clearSources() { sources_.clear(); }

    int sourceCount() const { return static_cast<int>(sources_.size()); }

    // index でのアクセス（内部ループ用）。範囲外は原点を返す。
    const Vec3& sourcePos(int index) const {
        if (index >= 0 && index < sourceCount()) return sources_[static_cast<size_t>(index)].pos;
        static const Vec3 origin{};
        return origin;
    }

    unsigned long long sourceId(int index) const {
        if (index >= 0 && index < sourceCount()) return sources_[static_cast<size_t>(index)].id;
        return 0;
    }

    // id → index。見つからなければ -1（af_Get* が id で引くときに使う）。
    int sourceIndexOf(unsigned long long id) const {
        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i].id == id) return static_cast<int>(i);
        return -1;
    }

    // --- 参照 ---
    int instanceCount() const { return static_cast<int>(instances_.size()); }
    int materialCount() const { return static_cast<int>(materials_.size()); }

private:
    bool validInstance(int id) const { return id >= 0 && id < instanceCount(); }

    int clampMaterialId(int id) const {
        if (materials_.empty()) return 0;
        if (id < 0 || id >= materialCount()) return 0;
        return id;
    }

    // materialId から材質を取る。テーブル空/範囲外なら既定壁を返す（安全側）。
    const AcousticMaterial& materialOf(int id) const {
        if (id >= 0 && id < materialCount()) return materials_[id];
        static const AcousticMaterial fallback = AcousticMaterial::defaultWall();
        return fallback;
    }

    // ── BVH-of-OBB（broad-phase / TLAS）。インスタンス変更で lazy に再構築（[[dynamic-ray-architecture]] 追加ロック②）。──
    struct BvhNode {
        Aabb bounds;
        int leftFirst;  // 内部ノード=左子index / 葉=bvhOrder_ の開始index
        int count;      // 0=内部ノード / >0=葉（インスタンス数）
    };

    // OBB のワールド軸並行境界（各軸へ半サイズを射影して膨らませる）。
    static Aabb obbWorldAabb(const Obb& b) {
        const Vec3 e(
            std::fabs(b.axisX.x) * b.halfExtents.x + std::fabs(b.axisY.x) * b.halfExtents.y + std::fabs(b.axisZ.x) * b.halfExtents.z,
            std::fabs(b.axisX.y) * b.halfExtents.x + std::fabs(b.axisY.y) * b.halfExtents.y + std::fabs(b.axisZ.y) * b.halfExtents.z,
            std::fabs(b.axisX.z) * b.halfExtents.x + std::fabs(b.axisY.z) * b.halfExtents.y + std::fabs(b.axisZ.z) * b.halfExtents.z);
        Aabb a;
        a.min = b.center - e;
        a.max = b.center + e;
        return a;
    }

    void ensureBvh() const {
        if (!bvhDirty_) return;
        buildBvh();
        bvhDirty_ = false;
    }

    void buildBvh() const {
        bvhNodes_.clear();
        bvhOrder_.clear();
        for (int i = 0; i < instanceCount(); ++i)
            if (instances_[i].active) bvhOrder_.push_back(i);
        const int n = static_cast<int>(bvhOrder_.size());
        if (n == 0) return;
        bvhNodes_.reserve(static_cast<size_t>(2 * n));
        bvhNodes_.push_back(BvhNode{});
        buildNode(0, 0, n);
    }

    void buildNode(int nodeIdx, int start, int count) const {
        // ノード境界＝範囲内インスタンスのワールドAABB合併。
        Aabb b = obbWorldAabb(instances_[bvhOrder_[start]].obb);
        for (int k = 1; k < count; ++k) {
            const Aabb a = obbWorldAabb(instances_[bvhOrder_[start + k]].obb);
            b.min = Vec3(std::min(b.min.x, a.min.x), std::min(b.min.y, a.min.y), std::min(b.min.z, a.min.z));
            b.max = Vec3(std::max(b.max.x, a.max.x), std::max(b.max.y, a.max.y), std::max(b.max.z, a.max.z));
        }
        bvhNodes_[nodeIdx].bounds = b;
        if (count <= 2) {  // 葉
            bvhNodes_[nodeIdx].leftFirst = start;
            bvhNodes_[nodeIdx].count = count;
            return;
        }
        // 最長軸で中央値分割。
        const Vec3 ext = b.max - b.min;
        const int axis = (ext.x > ext.y) ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
        const int mid = start + count / 2;
        auto centroidOnAxis = [&](int idx) {
            const Aabb a = obbWorldAabb(instances_[idx].obb);
            const Vec3 c = a.min + a.max;  // 2×重心（比較のみなので係数不要）
            return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
        };
        std::nth_element(bvhOrder_.begin() + start, bvhOrder_.begin() + mid, bvhOrder_.begin() + start + count,
                         [&](int lhs, int rhs) { return centroidOnAxis(lhs) < centroidOnAxis(rhs); });
        const int left = static_cast<int>(bvhNodes_.size());
        bvhNodes_.push_back(BvhNode{});
        bvhNodes_.push_back(BvhNode{});
        bvhNodes_[nodeIdx].leftFirst = left;
        bvhNodes_[nodeIdx].count = 0;
        buildNode(left, start, mid - start);
        buildNode(left + 1, mid, start + count - mid);
    }

    // ── エッジカタログ補助 ──
    // キューブマップのテクセル方向（面f=0..5:+X,-X,+Y,-Y,+Z,-Z、u,v∈[-1,1]）。
    static Vec3 cubeTexelDir(int face, float u, float v) {
        switch (face) {
            case 0:  return normalized(Vec3(1.0f, -v, -u));   // +X
            case 1:  return normalized(Vec3(-1.0f, -v, u));   // -X
            case 2:  return normalized(Vec3(u, 1.0f, v));     // +Y
            case 3:  return normalized(Vec3(u, -1.0f, -v));   // -Y
            case 4:  return normalized(Vec3(u, -v, 1.0f));    // +Z
            default: return normalized(Vec3(-u, -v, -1.0f));  // -Z
        }
    }

    // シルエットのヒット点を、その箱の最近傍稜線に snap してカタログへ（近接重複は除外）。
    void addCatalogEdge(int instanceIdx, const Vec3& hitPoint) const {
        if (instanceIdx < 0 || instanceIdx >= instanceCount()) return;
        const Obb& b = instances_[instanceIdx].obb;
        const Vec3 ax[3] = {b.axisX, b.axisY, b.axisZ};
        const float h[3] = {std::max(b.halfExtents.x, 1e-4f), std::max(b.halfExtents.y, 1e-4f),
                            std::max(b.halfExtents.z, 1e-4f)};
        const Vec3 lp = obbToLocalPoint(hitPoint, b);
        const float lc[3] = {lp.x, lp.y, lp.z};
        // face軸=|lc|/h 最大、side軸=次点、edge軸=残り。
        int faceAxis = 0;
        float mx = std::fabs(lc[0]) / h[0];
        for (int k = 1; k < 3; ++k) { const float r = std::fabs(lc[k]) / h[k]; if (r > mx) { mx = r; faceAxis = k; } }
        const int a1 = (faceAxis + 1) % 3, a2 = (faceAxis + 2) % 3;
        const int sideAxis = (std::fabs(lc[a1]) / h[a1] >= std::fabs(lc[a2]) / h[a2]) ? a1 : a2;
        const int edgeAxis = (sideAxis == a1) ? a2 : a1;
        const float faceSign = lc[faceAxis] >= 0.0f ? 1.0f : -1.0f;
        const float sideSign = lc[sideAxis] >= 0.0f ? 1.0f : -1.0f;
        // 稜線を外向き(2面法線の対角)に少し膨らませ、エッジ→音源が箱に再突入しないように。
        const Vec3 outward = normalized(ax[faceAxis] * faceSign + ax[sideAxis] * sideSign);
        const float margin = 0.15f;
        const Vec3 base = b.center + ax[faceAxis] * (faceSign * h[faceAxis])
                        + ax[sideAxis] * (sideSign * h[sideAxis]) + outward * margin;
        DiffEdge e;
        e.p0 = base - ax[edgeAxis] * h[edgeAxis];
        e.p1 = base + ax[edgeAxis] * h[edgeAxis];
        e.edgeDir = ax[edgeAxis];
        e.refTangent = ax[sideAxis] * (-sideSign);  // 0面(face面)接線＝エッジから外へ
        e.n = 1.5f;
        e.instance = instanceIdx;  // 自己除外用
        const Vec3 mid = (e.p0 + e.p1) * 0.5f;
        for (const DiffEdge& x : edgeCatalog_)
            if (length((x.p0 + x.p1) * 0.5f - mid) < 0.05f) return;  // 同一稜線
        edgeCatalog_.push_back(e);
    }

    std::vector<AcousticMaterial> materials_;  // 材質テーブル（インスタンスが matId で参照）
    std::vector<Instance> instances_;          // 占有物（毎フレーム更新可能）
    Vec3 listenerPos_{};                       // 保持リスナー（段1〜。af_Update が使う）
    std::vector<SourceEntry> sources_;         // 保持音源（同上）

    mutable std::vector<BvhNode> bvhNodes_;    // BVH ノード列（lazy 構築）
    mutable std::vector<int> bvhOrder_;        // アクティブなインスタンス index の並び
    mutable bool bvhDirty_ = true;             // インスタンス変更で立つ再構築フラグ
    mutable std::vector<DiffEdge> edgeCatalog_;  // キューブマップ由来のシルエット稜線
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_SCENE_H
