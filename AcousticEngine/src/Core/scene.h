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
#include "Core/aperture_fresnel.h"
#include "Core/btm.h"
#include "Core/kirchhoff.h"
#include "Core/maekawa.h"
#include "Core/material.h"
#include "Core/mesh_geom.h"
#include "Core/utd.h"
#include "Core/vec3.h"

namespace acoustic {

// ── レイサンプリング補助（役割2の反射トレース用）。旧 acoustic_world.cpp から移植。
namespace scene_detail {

constexpr float kPiF = 3.14159265358979f;

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// 環境変数 AF_DIFF_DEBUG で回折の内訳を stderr に出す（調査用。既定は無効）。
//   候補がどの判定で落ちたか、開口率が幾らか、といった値は外から見えないので、
//   これが無いと「音が変わらない」の原因を推測で追うことになる。
inline bool diffDebug() {
    static const bool on = (std::getenv("AF_DIFF_DEBUG") != nullptr);
    return on;
}

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

// シーン内の1つの占有物。形状は geomId で選ぶ（-1=箱 / >=0=メッシュ）。
struct Instance {
    Obb obb;              // ワールド空間の有向境界ボックス（transform 相当）
    int materialId = 0;   // materials_ テーブルへの参照
    // 形状(BLAS)の指定。-1 = 箱（obb そのもの）／>=0 = meshes_ の添字。
    //   メッシュの場合、obb は「変換行列」と「ワールド境界ボックス」を兼ねる。
    //   メッシュは正規化ローカル空間（AABB=[-1,1]^3）で持つので、obb がそのまま
    //   local→world の写像になる（mesh_geom.h 参照）。
    int geomId = -1;
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

    // 既存の材質の中身を書き換える。その材質を使っているインスタンスが一斉に変わる。
    //   材質は materials_ を毎回引き直して使う（キャッシュも前計算も無い）ので、
    //   ここを書き換えれば次のクエリから効く。BVH は形状だけなので再構築も不要。
    bool setMaterial(int id, const AcousticMaterial& m) {
        if (id < 0 || id >= materialCount()) return false;
        materials_[static_cast<std::size_t>(id)] = m;
        return true;
    }

    // インスタンスの材質を付け替える。「この扉だけ木」のような使い方。
    bool setInstanceMaterial(int instanceId, int materialId) {
        if (!validInstance(instanceId)) return false;
        if (materialId < 0 || materialId >= materialCount()) return false;
        instances_[static_cast<std::size_t>(instanceId)].materialId = materialId;
        return true;
    }

    int instanceMaterial(int instanceId) const {
        return validInstance(instanceId)
             ? instances_[static_cast<std::size_t>(instanceId)].materialId : -1;
    }

    // インスタンスを追加し instanceId を返す。materialId は addMaterial の戻り値。
    // 範囲外 materialId は 0 に丸める（材質未登録なら既定壁を1つ入れておくこと）。
    // geomId: -1 = 箱（obb そのもの）／>=0 = addMesh の戻り値。
    int addInstance(const Obb& obb, int materialId, int geomId = -1) {
        Instance inst;
        inst.obb = obb;
        inst.materialId = clampMaterialId(materialId);
        inst.geomId = validMesh(geomId) ? geomId : -1;
        instances_.push_back(inst);
        bvhDirty_ = true;
        return static_cast<int>(instances_.size()) - 1;
    }

    // ── 形状(BLAS) ────────────────────────────────────────────────
    // 三角形メッシュを登録し geomId を返す。失敗は -1。
    //   実行時に呼べる。破壊やプロシージャル生成で形状が増えるのは配置の操作であり、
    //   構築が要るのはこの 1 個ぶんだけ（レベル全体の再計算は決して起きない）。
    //   outLocalCenter / outLocalHalfExtents には、正規化に使ったローカル AABB を返す。
    //   ホストはこれを使ってインスタンスの OBB（＝変換）を作ること。
    int addMesh(const float* verticesXYZ, int vertexCount, const int* indices, int indexCount,
                Vec3* outLocalCenter = nullptr, Vec3* outLocalHalfExtents = nullptr) {
        // 空きスロットを再利用する。詰め直すと既存の geomId が壊れるので絶対にしない。
        int slot = -1;
        for (size_t i = 0; i < meshes_.size(); ++i)
            if (!meshes_[i].used) { slot = static_cast<int>(i); break; }
        if (slot < 0) { meshes_.emplace_back(); slot = static_cast<int>(meshes_.size()) - 1; }

        if (!meshes_[static_cast<size_t>(slot)].build(verticesXYZ, vertexCount, indices, indexCount)) {
            meshes_[static_cast<size_t>(slot)].clear();
            return -1;
        }
        if (outLocalCenter) *outLocalCenter = meshes_[static_cast<size_t>(slot)].localCenter;
        if (outLocalHalfExtents) *outLocalHalfExtents = meshes_[static_cast<size_t>(slot)].localHalfExtents;
        return slot;
    }

    // 形状を解放する。参照していたインスタンスは箱扱いに落ちる（境界ボックスとして残る）。
    void removeMesh(int geomId) {
        if (!validMesh(geomId)) return;
        meshes_[static_cast<size_t>(geomId)].clear();
        for (Instance& inst : instances_)
            if (inst.geomId == geomId) inst.geomId = -1;
        bvhDirty_ = true;
    }

    // 抽出された回折稜線の本数（診断用）。テッセレーションに依存しないことの確認に使う。
    int meshEdgeCount(int geomId) const {
        if (!validMesh(geomId)) return -1;
        return static_cast<int>(meshes_[static_cast<size_t>(geomId)].edges.size());
    }

    int meshCount() const { return static_cast<int>(meshes_.size()); }
    bool validMesh(int geomId) const {
        return geomId >= 0 && geomId < static_cast<int>(meshes_.size())
            && meshes_[static_cast<size_t>(geomId)].used;
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

    // 材質もインスタンスも形状も全消し。
    void clearAll() {
        instances_.clear();
        materials_.clear();
        meshes_.clear();
        bvhDirty_ = true;
    }

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
                    if (instanceRaycast(inst, origin, d, closest, t, n) && t < closest) {
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

    // ── インスタンス単位の幾何判定（箱／メッシュを吸収する）───────────────
    //   TLAS の葉ではこの2つだけを呼ぶ。形状の種類はここに閉じ込める。

    // レイ最近ヒット。maxT/outT はワールド距離。
    //   メッシュはローカルへ移してから BVH に問う。**方向を正規化しない**ので
    //   ローカル側の t がワールド距離と一致し、他インスタンスとの比較がそのまま通る。
    bool instanceRaycast(const Instance& inst, const Vec3& origin, const Vec3& dir,
                         float maxT, float& outT, Vec3& outNormal) const {
        if (inst.geomId < 0) return rayIntersectsObb(origin, dir, inst.obb, maxT, outT, outNormal);
        if (inst.geomId >= static_cast<int>(meshes_.size())) return false;
        const MeshGeometry& g = meshes_[static_cast<size_t>(inst.geomId)];
        if (!g.used) return false;
        // 先にワールド境界ボックスで弾く（obb はメッシュの境界も兼ねている）。
        float bt; Vec3 bn;
        if (!rayIntersectsObb(origin, dir, inst.obb, maxT, bt, bn)) return false;
        const Vec3 lo = meshWorldToLocalPoint(origin, inst.obb);
        const Vec3 ld = meshWorldToLocalDir(dir, inst.obb);
        Vec3 ln;
        if (!g.bvh.raycast(lo, ld, maxT, outT, ln)) return false;
        outNormal = meshLocalNormalToWorld(ln, inst.obb);
        return true;
    }

    // 線分がこのインスタンスに遮られるか。
    bool instanceOccludes(const Instance& inst, const Vec3& from, const Vec3& to) const {
        if (inst.geomId < 0) return segmentIntersectsObb(from, to, inst.obb);
        if (inst.geomId >= static_cast<int>(meshes_.size())) return false;
        const MeshGeometry& g = meshes_[static_cast<size_t>(inst.geomId)];
        if (!g.used) return false;
        if (!segmentIntersectsObb(from, to, inst.obb)) return false;   // 境界ボックスで先に棄却
        return g.bvh.occludes(meshWorldToLocalPoint(from, inst.obb),
                              meshWorldToLocalPoint(to, inst.obb));
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
                    if (instanceOccludes(instances_[i], from, to)) return true;
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
                    if (instanceOccludes(instances_[i], from, to)) return true;
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
                    if (!instanceOccludes(inst, from, to)) continue;
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
    // blockerMargin: ブロッカー判定を膨らませる量(m)。0 なら「実際に直接線を塞ぐ箱」だけ。
    //   影境界の外側（照らされた領域）でも回折場を求めたい場合に > 0 を渡す。
    //   そこでは直接線が箱の脇をかすめて通るだけなので、膨らませないと候補が0本になる。
    //   遠いエッジを拾っても UTD の総合ゲインは 1.0 に収束するので、多めでも害はない。
    // requireBothEnds: false なら「from から見通せる」だけを条件にする（to 側は問わない）。
    //   2次回折の 1 段目で使う。真の2次回折では、どの稜線も from と to の**両方**からは
    //   見通せない ── それがまさに「2次でしか届かない」ということなので、両方を要求すると
    //   候補が 0 本になり、再帰に入る前にループが空になる。
    void forEachDiffractionCandidate(const Vec3& from, const Vec3& to, Fn&& fn,
                                     float blockerMargin = 0.0f,
                                     bool requireBothEnds = true) const {
        const float direct = std::max(length(to - from), 1e-4f);
        // ★稜線は実形状の上に置く。以前は 0.15m 膨らませていたが、それだと UTD が見る影境界が
        //   実際の影境界からずれる（この距離だけ回折点が動くので）。UTD は「直接音が消える点」と
        //   「回折場が符号反転する点」が一致することで連続になる仕組みなので、両者がずれると
        //   その区間だけ値が壊れる（実測で 3.8dB の段差が出ていた区間と一致）。
        //   一方で 0 にもできない。回折点を通る経路は箱の面を「掠める」ので、面に沿って走る。
        //   稜線が表面ぴったりだと、その経路が自分の箱と接触したと判定されてしまう
        //   （接触は回折点の近傍ではなく経路全体に分布するので、端点を引くだけでは避けられない）。
        //   → 面から確実に離れる最小限だけ外へ出す。前川の式は δ のみに依存するので、
        //     この程度のずれでは連続性は壊れない（UTD は影境界の位置がずれて壊れていた）。
        const float margin = 0.02f;   // 2cm

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
            // 回折点 P から from / to の両方を見通せるか。
            //
            //   ★当の箱(exceptInst)は「除外」ではなく「貫通長で判定」する。
            //     以前は当の箱を判定から丸ごと外していた。厚みのある壁の稜線を回る経路は、
            //     幾何的に必ずその壁の厚みぶんを貫くので、単純に「交差＝無効」にすると
            //     正しい回折経路まで消えてしまうからである。
            //     しかし丸ごと除外は乱暴すぎた。大きな壁では「その壁を何メートルも突き抜けて
            //     遠い稜線に達する経路」まで見通せると判定され、音が通らない場所が開口として
            //     認識される（実機で、壁の裏の見当違いな方向に回折経路が生えていた）。
            //     → 両者を分けるのは「通るか否か」ではなく「どれだけ通るか」。
            //       箱のいちばん薄い方向を通り抜けるぶんまでは「回り込み」として許し、
            //       それを超える貫通は「壁を突っ切っている」として棄却する。
            //
            //   ★分けるのは貫通の「量」ではなく「場所」。
            //     量で見ると詰む: 許容を厚みぶん(thinnest+0.05)にすると**薄い板が透明**になり、
            //     厚み 6cm の扉を突き抜ける経路が合法になって扉が音響的に存在しなくなる。
            //     かといって固定の小さい値にすると、厚い衝立の角を回る正当な経路が消える
            //     （実測: 6cm 固定で衝立の回折経路と開口側の定位が壊れた）。
            //     両者は同じ貫通長でも**貫通する場所**が違う:
            //       角を掠める正当な経路 … 稜線のすぐ近くだけを貫く（掠めるので当然）
            //       板を突き抜ける偽の経路 … 面の真ん中を、板の幅いっぱいに貫く
            //     掠めが伸びる距離は箱の厚みの程度なので、そこを基準に近傍を決める。
            const Obb& selfObb = instances_[exceptInst].obb;
            const float thinnest = 2.0f * std::min(selfObb.halfExtents.x,
                                        std::min(selfObb.halfExtents.y, selfObb.halfExtents.z));
            const float edgeNear = thinnest * 2.0f + 0.05f;
            const Vec3 dirTravel = normalized(to - from);
            // ★「掠める」と「通り抜ける」を**幾何で**分ける。閾値を置かない。
            //     掠める    … 物体の外を回る。中心面を横切らないか、横切っても断面の外
            //     通り抜ける… 断面の**中**で中心面を横切る
            //   位置や長さの閾値では区別できない（閉じた扉の幻は壁の角を 4.5cm 掠め、
            //   衝立の正当な経路は 14cm 掠める。長さで切ると後者が死ぬ ── 実際に死んだ）。
            //   中心面は板の**最も薄い軸**に垂直な面。板を貫くとはそこを断面内で越えること。
            const Vec3 selfAxes[3] = {selfObb.axisX, selfObb.axisY, selfObb.axisZ};
            const float selfHalf[3] = {selfObb.halfExtents.x, selfObb.halfExtents.y,
                                       selfObb.halfExtents.z};
            int thinAxis = 0;
            for (int k = 1; k < 3; ++k) if (selfHalf[k] < selfHalf[thinAxis]) thinAxis = k;
            // ★メッシュには使えない。OBB は境界箱でしかなく、その「中心面」は
            //   実形状と対応しない（戸口の空いたメッシュ壁なら、穴の真ん中を通る
            //   正当な経路まで「板を貫いた」と判定されて回折が全部消える）。
            //   メッシュは実形状で遮蔽判定されるので、この補助判定は要らない。
            const bool selfIsMesh = instances_[exceptInst].geomId >= 0;
            // ★★ 幻の経路は「1点で回折する」模型では原理的に分離できない ★★
            //   閉扉でリスナーを斜めに置くと、戸口の**奥側**の縁へ壁を貫通して届く
            //   偽の経路が候補になる（P(-0.58,1.60,0.17)、壁の中を 27.7cm 進む）。
            //   これを消そうとして4通り試し、全部**正当な経路まで殺した**:
            //     ・貫通の長さで切る        8cm で衝立の回り込み(14cm)が死ぬ
            //     ・断面の内側で中心面を横切ったら棄却  3cm で4件死に、5cm で幻が復活
            //     ・自分の実体の除外を稜線近傍だけに    6cm で掠めが死に、45cm では
            //                                          壁の手前で切れて検査にならない
            //     ・margin だけ縮めた「芯」を横切ったら棄却  5件死ぬ
            //
            //   最後の実測で理由が判明した。厚い衝立(0.4m)を上から回り込む**正当な**経路は、
            //   上面に沿って掠めるので必ず実体の中を通る:
            //     回折点 P(0, 4.02, -0.22) → 音源(0,1.6,4) の線は中心面 z=0 で y=3.894。
            //     衝立の上端は y=4.0 なので、**中を通っている**。
            //   幻も実体の中を通る。**同じ性質なので幾何量では分けられない。**
            //
            //   分離には経路が **2回曲がる** ことが要る（近い縁 → 遠い縁）。
            //   厚い障害物の回り込みは本来そういう形で、1点近似が限界を作っている。
            //
            //   → そこで 1 次には「**芯を横切ったら棄却**」を課す。
            //     芯 = 実体を margin だけ縮めたもの。margin の殻は掠めの許容なので、
            //     そこを通るのは正当、芯を横切るのは通り抜け。
            //     これで幻も厚い衝立の1点近似も同時に消える。
            //     厚い衝立の正当な回り込みは **2 次**（近い縁→遠い縁）が肩代わりする。
            //   殻の厚さは **margin 固定では足りない**。掠めが実体に食い込む深さは
            //   障害物の厚みに比例して伸びるので（厚 200mm を超えると 2cm の殻を
            //   突き抜けて候補が 0 になった。実測）、厚みの割合でも確保する。
            const float shell = std::max(margin, thinnest * 0.5f);
            const Obb coreObb = [&] {
                Obb o = selfObb;
                o.halfExtents = Vec3(std::max(o.halfExtents.x - shell, 1e-3f),
                                     std::max(o.halfExtents.y - shell, 1e-3f),
                                     std::max(o.halfExtents.z - shell, 1e-3f));
                return o;
            }();
            auto crossesCore = [&](const Vec3& a, const Vec3& c) {
                if (selfIsMesh) return false;   // 境界箱の芯は実形状と対応しない
                float t0, t1;
                return segmentObbPenetrationSpan(a, c, coreObb, t0, t1);
            };
            // 貫通区間が回折点 p の近傍に収まっているか＋板を貫いていないか。
            // ★許容を**二値にしない**。返すのは 0〜1 の重み。
            //   掠めの深さ（連続量）に閾値 edgeNear を置いて可否を決めていたので、
            //   リスナーが少し動いて閾値を跨いだ瞬間に候補が生まれたり消えたりしていた。
            //   実測（回折だけシーン、開口の手前側の縦稜線 x=2.02）:
            //     リスナー x=-7.0  掠め 0.645m ≥ 0.65 → 全域不可視（候補ゼロ）
            //     リスナー x=-6.0  掠め 0.594m < 0.65 → 採用（満額）
            //   1m 動いただけで支配経路が入れ替わり、経路長が 2.65m 跳んでいた。
            //   実機で聞こえていた「歩くと回折が消える／音が気持ち悪い」と同じ仕組み。
            //   深いほど 0 へ滑らかに落とせば、生まれる瞬間の重みが 0 なので跳ばない。
            //   計算量は前と同じ（同じ距離を測って、比較の代わりに写像するだけ）。
            auto penNearWeight = [&](const Vec3& a, const Vec3& c, const Vec3& p) -> float {
                if (crossesCore(a, c)) return 0.0f;   // 芯を横切った＝掠めではない
                float t0, t1;
                if (!segmentObbPenetrationSpan(a, c, selfObb, t0, t1)) return 1.0f;  // 貫通なし
                const Vec3 d = c - a;
                const float far = std::max(length(a + d * t0 - p), length(a + d * t1 - p));
                // ★試したが**1点も変わらなかった**もの: 許容を入射角で伸ばす
                //   （厚み t を角度 θ で横切れば面に沿う走りは t/sinθ、という理屈で
                //    edgeNear を thinnest*2/sinθ+0.05 にする）。
                //   「離れると稜線が探索できなくなるのでは」を疑って入れたが、実測では
                //   固定値と完全に同一の結果だった（横 18m まで到達、格子も同じ、回帰も同じ）。
                if (far >= edgeNear) return 0.0f;
                const float x = 1.0f - far / edgeNear;
                return x * x * (3.0f - 2.0f * x);     // smoothstep（端で傾きも 0）
            };
            auto vis = [&](float t) {
                const Vec3 p = Pf(t);
                // ★膨らませた点が**別の実体の中**なら、そこは開口ではない。
                //   稜線は面から 2cm 外へ出してある（面すれすれの経路を拾うために必要）。
                //   そのため、扉の自由端が戸口の枠に密着していても 2cm ぶんの隙間が
                //   開いて見え、閉じた扉に幻の経路が生えていた
                //   （実測: 閉扉で δ が 0.05m ＝ 往復 2cm ぶんしか無かった）。
                //   衝立の影境界では膨らませた点は空中なので、この判定に掛からない
                //   （連続性 0.033 を壊さない）。
                if (pointInsideOther(p, exceptInst, margin)) return false;
                // ★試して駄目だったもの: 「手前側の対になる縁が塞がれているなら奥へは
                //   届いていない」（中心面で鏡映した点の可視性で判定）。
                //   実測では幻が消えず、扉 10° の開き始めが 0 になる副作用だけ出た。
                // ★その角の脇を**まっすぐ通り抜けられるか**を1本のレイで聞く。
                //   閉じた扉と、衝立の正当な回り込みは、貫通の量でも位置でも区別できない
                //   （実測: 閉扉の幻は壁の角を 4.5cm 掠め、衝立の正当な経路は 14cm 掠める。
                //    掠めの許容は厚い壁のために必要なので、閾値ではどちらかが必ず壊れる）。
                //   違うのは**角の向こうに抜けられるかどうか**だけ。扉が塞いでいれば抜けられない。
                //   ★1本ではなく複数本に散らして、**1本でも通れば候補**とする。
                //     1本だと「通る／通らない」の二値なので、扉が開き始めた瞬間に
                //     ゲインが 0 からいきなり立ち上がる（実測: 5°で0 → 10°で0.192）。
                //     耳には「ぐわっと大きくなる」ではなく「カチッと切り替わる」と聞こえる。
                //     通った本数の割合は開き具合とともに連続に増えるので、それをゲインに使う
                //     （passThroughFraction。ソフト遮蔽と同じ考え方）。
                //
                // ★★★ ここが「幻の経路」問題の答え（長かった経緯を残す）★★★
                //
                //   【幻がどう成立していたか】閉扉・リスナーを横へ振った配置で座標を追った:
                //     ① margin の押し出しは**角では 2 方向へ同時に効く**。縦稜線の点は
                //        x にも z にも 2cm 押されるので、P(-0.58,1.60,+0.17) は
                //        「戸口の中へ 2cm・壁の奥面から 2cm」の対角に立つ。
                //        ＝ リスナーから見て**壁の向こう側**に回折点ができる。
                //     ② そこへ届く線は必然的に壁の中を通る（z=-0.15 を x=-0.69 で横切る）。
                //     ③ 扉の z 帯では線は x=-0.648〜-0.638 にいる。扉は x>=-0.6 なので
                //        **扉の脇の壁の中**を抜けていて、扉には当たらない。
                //     ④ 貫かれている袖壁は isOccludedExcept の除外対象（稜線が属する箱）
                //        なので、その判定には**見えない**。
                //     ⑤ crossesCore は箱を 0.15m 縮めた芯で見る。芯の内縁 x=-0.75 に対し
                //        横切りは x=-0.638 ＝ 帯の中なので、すり抜ける。
                //
                //   【なぜ距離では分けられなかったか】芯を締めて死んだ経路を特定した:
                //     厚い衝立(0.4m)を**上から回り込む正当な**経路は、回折点 (0,4.02,±0.22)
                //     から音源への線が中心面 z=0 を y=3.89（縁から 0.11m 内側）で横切る。
                //     厚みを渡るので必ずそうなる。
                //     対して幻の横切りは縁から **0.038m**。**幻の方が縁に近い**。
                //     閾値で切るなら「近い方を落とす」必要があり、原理的に不可能だった。
                //     8 回の失敗はこれが理由。「量でも位置でも分けられない」は正しかった。
                //
                //   【本当の違い】**縁の向こうが実際に開いているか**だけだった。
                //     正当（衝立の上）… 縁の向こうは空
                //     幻（閉扉）      … 縁の向こうは戸口だが、扉が塞いでいる
                //   これを直接聞く。P を挟んで箱の厚みぶん跨ぐ線分が塞がっていれば、
                //   そこに通り道は無い。上の例なら (-0.58,1.60,0.00) が扉の内部なので落ちる。
                //
                //   跨ぐ軸は**経路の向きに最も沿う箱の軸**を採る。
                //     ・箱の「最も薄い軸」版は立方体で軸が恣意的に決まる（3 軸同値）。
                //     ・稜線の 2 面から外向きに出す版は幻を落とせなかった（0.088→0.183 と悪化）。
                //     ・経路基準なら板・立方体・柱を**分類せずに**済み、意味も素直:
                //       「経路が越えようとしている面を、その場所で越えられるか」。
                //     形を分類する閾値を持ち込まないので、分類の境目で飛ぶこともない。
                //
                //   【従来の 3 本（進行方向・入り・出り）を置き換えた理由】
                //     距離が **厚みに比例**（厚み+margin×2）していて、隙間の幅を知らない。
                //     隙間が壁の厚みより狭いと必ず反対側の枠に当たるので、
                //     厚い壁の狭い隙間を「貫通」と同一視していた。実測:
                //       壁の半厚 0.05m → 0.06m の隙間から鳴る
                //       壁の半厚 0.15m → 0.35m から（0.25m 以下は候補 0 本）
                //       壁の半厚 0.30m → 1.00m から（1m の戸口すら塞がっている判定）
                //     厚い壁ほど広い隙間を要求する、という現実と逆の挙動だった。
                //
                //   メッシュには使わない。境界箱の軸は実形状と対応しないので、
                //   跨ぎ線分が形状と無関係な所を通る（実測で回折が全部消えた）。
                if (!selfIsMesh) {
                    int ax = 0; float bestA = -1.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float a = std::fabs(dot(selfAxes[k], dirTravel));
                        if (a > bestA) { bestA = a; ax = k; }
                    }
                    const float half = selfHalf[ax] + 2.0f * margin;
                    const Vec3 nAx = selfAxes[ax];
                    if (isOccluded(p - nAx * half, p + nAx * half)) return false;
                } else {
                    if (isOccluded(p, p + dirTravel * (thinnest + 2.0f * margin))) return false;
                    const float reach = thinnest + 2.0f * margin;
                    const Vec3 din = normalized(p - from);
                    if (isOccluded(p, p + din * reach)) return false;
                    if (requireBothEnds) {
                        const Vec3 dout = normalized(to - p);
                        if (isOccluded(p, p + dout * reach)) return false;
                    }
                    // ★メッシュ側にはまだ幻が残りうる（上の跨ぎ判定が使えないため）。
                    //   箱側で効いた「縁の向こうが開いているか」に相当するものを
                    //   実形状で作るのが筋。未着手。
                }
                if (isOccludedExcept(from, p, exceptInst)) return false;   // 他の障害物
                // 可視判定そのものは「重みが 0 より大きいか」。重みの値は下で採る。
                if (penNearWeight(from, p, p) <= 0.0f) return false;
                if (!requireBothEnds) return true;
                if (isOccludedExcept(p, to, exceptInst)) return false;
                return penNearWeight(p, to, p) > 0.0f;
            };

            // ★回折点は「可視範囲に制約した g の最小点」。
            //   以前は {最適点, 端点A, 端点B} の3点しか試さず、最適点が見通せないと P が端点へ
            //   スナップしていた。端点は動かないので δ が変化せず踊り場になり、最適点が見通せた
            //   瞬間に P が端から中央へワープして段差が出る（実測で 3.8dB の跳び）。
            //   可視境界は幾何とともに連続に動くので、そこを二分で求めれば P も連続に動く。
            const float tStar = 0.5f * (lo + hi);
            float bestT = -1.0f;
            if (vis(tStar)) {
                bestT = tStar;
            } else {
                // 最適点から左右へ粗くスキャンし、最初に可視へ変わる区間を二分で詰める。
                // g は単峰なので、可視領域内の最小は最適点に最も近い可視境界にある。
                constexpr int kScan = 16, kBisect = 12;
                for (int side = 0; side < 2; ++side) {
                    const float span = side ? (1.0f - tStar) : tStar;
                    if (span <= 1e-5f) continue;
                    float prev = tStar;
                    for (int k = 1; k <= kScan; ++k) {
                        const float t = side ? (tStar + span * (k / float(kScan)))
                                             : (tStar - span * (k / float(kScan)));
                        if (!vis(t)) { prev = t; continue; }
                        float bad = prev, good = t;            // bad:不可視, good:可視
                        for (int it = 0; it < kBisect; ++it) {
                            const float m = 0.5f * (bad + good);
                            if (vis(m)) good = m; else bad = m;
                        }
                        if (bestT < 0.0f || g(good) < g(bestT)) bestT = good;
                        break;
                    }
                }
            }
            if (bestT < 0.0f) return false;                    // 稜線全体が見通せない
            const Vec3 bp = Pf(bestT);
            // 掠めの深さから来る重み（0〜1）。生まれる瞬間は 0 なので候補の出入りで跳ばない。
            //   ★試して**割に合わなかった**別案: 稜線の可視割合を 8 点サンプルして重みにする。
            //     実測で 経路長の最大隣接差 2.65m → 2.44m しか改善せず、
            //     1フレーム 7.0ms → 8.1ms（+15%）。可視判定はレイを何本も撃つので高い。
            //     主因は「見え始め」ではなく掠め判定の閾値そのものだった。
            const float edgeW = requireBothEnds
                ? std::min(penNearWeight(from, bp, bp), penNearWeight(bp, to, bp))
                : penNearWeight(from, bp, bp);
            if (edgeW <= 0.0f) return false;
            float bd = length(bp - from) + length(to - bp) - direct;
            if (bd < 0.0f) bd = 0.0f;
            // ★端点 A,B も渡す。BTM は**有限の稜線**であることを使うので、
            //   方向だけでは足りない（そこが前川との本質的な違い）。
            //   edgeW と exceptInst も渡す。前者はクラスタの重みに、後者は
            //   「同じ開口か」の判定に使う。
            fn(bp, bd, edgeDir, refT, A, B, edgeW, exceptInst);
            return true;
        };

        // ★回折は「直接経路を実際に塞いでいる箱（ブロッカー）」の稜線だけを回る。部屋の床/天井/壁など
        //   直接線と交差しない箱の稜線まで候補にすると、無関係な遠い方向へ候補が飛んで合成を濁す。
        //   直接 from→to の線分が OBB と交差する箱＝ブロッカー、だけを対象にする。
        auto isBlocker = [&](int inst) {
            if (inst < 0 || inst >= instanceCount() || !instances_[inst].active) return false;
            if (blockerMargin <= 0.0f) return segmentIntersectsObb(from, to, instances_[inst].obb);
            // 膨らませた OBB で判定する（半径方向に margin だけ拡大）。
            Obb fat = instances_[inst].obb;
            fat.halfExtents = fat.halfExtents + Vec3(blockerMargin, blockerMargin, blockerMargin);
            return segmentIntersectsObb(from, to, fat);
        };

        // ★開口の縁は、直接線を塞いでいる箱に属するとは限らない。
        //
        //   仕切りが「左の壁＋右の壁」の 2 枚でできていると（Unity の Test_DiffractionGap が
        //   まさにそれ）、直接線を塞ぐのは片方だけで、開口の**反対側の縁**を持つ方は
        //   ブロッカーにならない。つまり開口の縁が片方しか候補に入らない。
        //   残った片方も仕切りの角を掠めるので貫通判定で落ちることがあり、そうなると
        //   **候補が全滅して無音になる**（実測: Test_DiffractionGap と同形状で
        //   リスナー x∈[-1.5,0.5] の 4 点が候補 0 本。歩くと回折音が丸ごと切れる）。
        //   そのとき、リスナーから反対側の縁への線は開口の中を素通りしていた
        //   ── 候補に入ってさえいれば通っていた、というだけの話。
        //
        //   ブロッカーと**同じ平面にある箱**は同じ仕切りの一部なので、同じ開口を囲っている。
        //   扉・戸口・開口を名指ししない一般の規則なので、authoring は要らないし
        //   手続き生成で壁が分割されても効く。
        //   候補が増えても、下の可視判定を通らなければ経路にはならない。
        constexpr int kMaxBlockerPlanes = 8;
        Vec3  bpN[kMaxBlockerPlanes]; Vec3 bpC[kMaxBlockerPlanes]; int nbp = 0;
        auto thinNormalOf = [&](const Obb& b) {
            const float hx = b.halfExtents.x, hy = b.halfExtents.y, hz = b.halfExtents.z;
            Vec3 n = b.axisZ;
            if (hx <= hy && hx <= hz) n = b.axisX;
            else if (hy <= hx && hy <= hz) n = b.axisY;
            return (length(n) > 1e-4f) ? normalized(n) : Vec3(0, 0, 1);
        };
        for (int i = 0; i < instanceCount(); ++i) {
            if (nbp >= kMaxBlockerPlanes) break;
            if (!instances_[i].active || instances_[i].geomId >= 0) continue;
            if (!isBlocker(i)) continue;
            const Obb& b = instances_[i].obb;
            const Vec3 n = thinNormalOf(b);
            bool dup = false;
            for (int k = 0; k < nbp; ++k)
                if (std::fabs(dot(n, bpN[k])) > 0.999f
                    && std::fabs(dot(n, b.center - bpC[k])) < 0.05f) { dup = true; break; }
            if (!dup) { bpN[nbp] = n; bpC[nbp] = b.center; ++nbp; }
        }
        // ブロッカー、またはブロッカーと同一平面の箱。
        //   メッシュは実形状で遮蔽判定されるので広げない（境界箱の平面は形状と対応しない）。
        auto isEdgeSource = [&](int inst) {
            if (isBlocker(inst)) return true;
            if (inst < 0 || inst >= instanceCount()) return false;
            const Instance& in2 = instances_[inst];
            if (!in2.active || in2.geomId >= 0) return false;
            const Vec3 n = thinNormalOf(in2.obb);
            for (int k = 0; k < nbp; ++k)
                if (std::fabs(dot(n, bpN[k])) > 0.999f
                    && std::fabs(dot(n, in2.obb.center - bpC[k])) < 0.05f) return true;
            return false;
        };

        // エッジカタログ（B: キューブマップ由来のシルエット稜線）と箱の実稜線の両方を候補にする（和集合）。
        // カタログだけだと片側しか拾えないことがある（→両側から鳴らない・エッジ入替で方向が飛ぶ）ので、
        // 箱の 12 稜線も必ず加えて両側を確実に取る。ただし双方ともブロッカーの稜線に限る。
        if (!edgeCatalog_.empty())
            for (const DiffEdge& e : edgeCatalog_)
                if (isEdgeSource(e.instance))
                    tryEdge(e.p0, e.p1, e.edgeDir, e.refTangent, e.instance);

        // 箱(OBB)の 12 稜線それぞれで掠める角を探索（角のみだと薄い壁で失敗するので稜線全体）。
        //   メッシュインスタンスは箱の 12 稜線ではなく、**形状から抽出した回折稜線**を使う。
        //   OBB の稜線は「メッシュの稜線」ではないので、そのまま回すと遮蔽判定（実形状）と
        //   回折判定（境界ボックス）が矛盾した幾何を見ることになる ── 壁と戸口が 1 メッシュなら、
        //   遮蔽は「戸口を通る」、回折は「壁の外周を回れ」と言う（設計 §5-5）。
        const int fixed2[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
            const Obb& b = inst.obb;
            // ブロッカー＋同一平面の箱が回折対象（margin>0 なら近傍も含む）
            if (!isEdgeSource(i)) continue;

            if (inst.geomId >= 0 && inst.geomId < static_cast<int>(meshes_.size())) {
                const MeshGeometry& g = meshes_[static_cast<size_t>(inst.geomId)];
                if (!g.used) continue;
                for (const DiffractionEdgeLocal& e : g.edges) {
                    const Vec3 A = meshLocalToWorldPoint(e.a, b);
                    const Vec3 B = meshLocalToWorldPoint(e.b, b);
                    const Vec3 dir = B - A;
                    if (length(dir) < 1e-5f) continue;
                    const Vec3 ed = normalized(dir);
                    // refT は UTD 用（比較実装のみが使う）。稜線に直交する適当な接線でよい。
                    Vec3 t0 = cross(ed, Vec3(0, 1, 0));
                    if (length(t0) < 1e-3f) t0 = cross(ed, Vec3(1, 0, 0));
                    tryEdge(A, B, ed, normalized(t0), i);
                }
                continue;
            }

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
    //   blockerMargin > 0 なら、遮蔽されていなくても近傍のエッジを探す（影境界の外側で
    //   回折場を求めるため）。既定 0 では従来どおり「遮蔽時のみ」。
    float diffractionDetour(const Vec3& from, const Vec3& to, Vec3& outPoint,
                            Vec3* outEdgeDir = nullptr, Vec3* outRefTangent = nullptr,
                            float blockerMargin = 0.0f) const {
        if (blockerMargin <= 0.0f && !isOccluded(from, to)) return -1.0f;
        float best = -1.0f;
        Vec3 bestP{0, 0, 0}, bestEdge{1, 0, 0}, bestRefT{0, 1, 0};
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3& e, const Vec3& r, const Vec3&, const Vec3&, float, int) {
            if (best < 0.0f || d < best) { best = d; bestP = P; bestEdge = e; bestRefT = r; }
        }, blockerMargin);
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
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
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
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
            if (n < maxCount) { outP[n] = P; outDelta[n] = d; ++n; }
        });
        return n;
    }

    // 【隙間の幅】回折点 P における「稜線を横切る向きの自由な差し渡し」(m)。
    //
    //   ★測る軸を cross(稜線の向き, 進行方向) に取るのが要点。
    //     過去に3回、円盤や「経路の軸に垂直な4方向」で測って失敗している。
    //     それらは**稜線に沿った方向**まで含んでしまうので、扉の隙間ではなく
    //     部屋の差し渡し（実測 3.0m）を測ってしまい、開き具合と無関係な値になる。
    //     稜線に垂直・かつ進行方向に垂直な軸なら、**隙間を横切る向きだけ**を見られる。
    //
    //   この軸なら「開口」と「自由な縁」も分かれる:
    //     扉の隙間 … 両側とも塞がっている → 差し渡しが小さい（＝通りにくい）
    //     衝立の縁 … 片側が無限に開いている → 差し渡しが大きい（＝制限しない）
    //   円盤で測ると衝立の縁でも半分が影に入って誤って絞られる（実測で影境界が壊れた）。
    //
    //   前川の式は δ しか見ないので、1.8cm の隙間でも 1.8m の開口でも、迂回量が同じなら
    //   同じ値を返す（設計 §6-2）。その欠けをここで埋める。
    //   ★軸は2本要る。**稜線に垂直な向き**と**稜線に沿った向き**の両方を測って最小を採る。
    //     どちらが隙間を横切るかは、候補がどの稜線に乗ったかで変わるため:
    //       扉の自由端（垂直な稜線）… 垂直な軸＝横方向 が隙間を横切る
    //       まぐさ（水平な稜線）    … 沿った軸＝横方向 が隙間を横切る
    //     片方だけだと、まぐさに乗った候補が「戸口の高さ 2.4m」を測ってしまい、
    //     1.8cm の隙間なのに素通りと判定される（実測でそうなった）。
    //     衝立の自由な縁はどちらの軸も片側が開いているので、min を採っても大きいまま。
    float slitWidthAt(const Vec3& P, const Vec3& edgeDir, const Vec3& travelDir) const {
        constexpr float kMaxSpan = 4.0f;   // これ以上広ければ開口として律速しない
        auto freeDist = [&](const Vec3& d) {
            const SceneHit hit = raycastClosest(P, d, kMaxSpan);
            return hit.hit ? hit.t : kMaxSpan;
        };
        auto span = [&](const Vec3& d) {
            return freeDist(d) + freeDist(Vec3(-d.x, -d.y, -d.z));
        };
        float narrowest = kMaxSpan * 2.0f;
        Vec3 w = cross(edgeDir, travelDir);
        if (length(w) > 1e-4f) narrowest = std::min(narrowest, span(normalized(w)));
        if (length(edgeDir) > 1e-4f) narrowest = std::min(narrowest, span(normalized(edgeDir)));
        return narrowest;
    }

    // 隙間の幅 → 広帯域ゲイン(0..1)。狭いほど通らない。
    //   基準幅は「これより広ければ素通り」の目安。物理的には波長の半分あたりが境目
    //   （500Hz で 0.34m）。スカラで返すのは、回折が運ぶ情報を定位に絞る方針のため。
    float slitWidthGain(float width) const {
        if (slitWidthRef_ <= 1e-4f) return 1.0f;         // 0 で無効化
        const float r = width / slitWidthRef_;
        if (r >= 1.0f) return 1.0f;
        if (r <= 0.0f) return 0.0f;
        if (slitWidthPower_ == 1.0f) return r;
        return std::pow(r, slitWidthPower_);
    }

    // 【開口の実効幅】遮蔽物の面に格子を張り、「音が抜けられるセル」を数えて幅(m)を出す。
    //
    //   ★これは回折点に**一切依存しない**。過去6回の失敗は全部「回折点まわりを測る」
    //     という前提を共有していて、回折点と狭窄部が別の場所にあるせいで破綻していた
    //     （扉が斜めに開くと、扉の縁は壁の面より 21cm 手前へ飛び出す）。
    //     壁そのものを測れば、狭窄部がどこにあっても関係ない。
    //
    //   面の前後に厚みを持たせて判定するのが要点。面上だけで見ると、壁の面より
    //   10cm 奥に立っている扉を取りこぼす（6回目がこれで外した）。
    //
    //   開口と自由な縁の区別も自然につく:
    //     戸口   … 格子の大半が壁で、開いたセルだけが隙間 → 実効幅が小さい
    //     衝立の縁 … 衝立の外側のセルは全部抜けられる → 実効幅が大きい（律速しない）
    //
    //   実効幅の出し方: 開いた面積 ÷ 開いた領域の長い方の差し渡し。
    //     高さ H・幅 w の縦スリットなら (wH)/H = w が出る。大きな開口でも短い方の辺が出る。
    //   seed は「開いている場所の当たり」を与えるヒント（回折点を渡す想定）。
    //     一様走査だけでは cm 単位の隙間を見つけられない（半径1.5mを7分割してもセルは
    //     21cm、20°の扉の隙間は7cm）。**測る場所は面のまま**で、探し始める場所だけ
    //     教える。過去の失敗は「回折点で測っていた」ことで、出発点にするのは別の話。
    // 【開口のエネルギー・帯域別】フレネルゾーンのうちどれだけ開いているかを帯域ごとに返す。
    //   詳しい理屈と、なぜこれで連続性が取れるかは Core/aperture_fresnel.h の冒頭を参照。
    //   戻り値: 遮るものが無ければ false（＝制限しない）。
    // ======================================================== ポータル（開口の矩形）
    //
    // なぜ導入するか:
    //   今日ここまで、開口の測り方を4通り試して全部失敗した。原因は毎回同じで
    //   **「面のうちどこまでを積分するか」に正解が無い**ことだった。
    //     直線と平面の交点に窓 → 音源が正面に無いと窓が壁の中に沈む
    //     稜線の開口点に窓     → 点が跳ぶ（実測 3m）
    //     開いている領域の重心 → 部屋の外まで「開いている」と数える（閉扉で 0.517）
    //     複数の面で最小       → 斜めの薄い板が断面に現れない
    //   ポータルはこの問いの答えそのもの。**積分範囲＝ポータルの矩形**。
    //   ホストが「ここが戸口」と置いた時点で境界が決まる。
    //   （エリアのトポロジはホスト、音響的な状態はエンジンが毎フレーム幾何から測る、
    //     という前に決めた境界に沿う。開き具合を authoring しないのが要点。）
    //
    // 何が変わるか（実測との対比）:
    //   従来は閉扉でも開口率 0.328 あった（窓が部屋の外まで広がっていたため）。
    //   ポータルなら扉が矩形を覆いきれば 0.0、何も塞がなければ 1.0 ── フルレンジになる。
    //   経路が「無から有限のゲインで生まれる」ことが構造的に起きない。
    struct Portal {
        Vec3 center{0, 0, 0};
        Vec3 axisU{1, 0, 0};   // 矩形の横方向（単位）
        Vec3 axisV{0, 1, 0};   // 矩形の縦方向（単位）
        float halfU = 0.6f;
        float halfV = 1.2f;
        bool active = true;
    };

    int addPortal(const Vec3& center, const Vec3& axisU, const Vec3& axisV,
                  float halfU, float halfV) {
        Portal p;
        p.center = center;
        p.axisU = normalized(axisU);
        p.axisV = normalized(axisV);
        p.halfU = std::max(halfU, 1e-3f);
        p.halfV = std::max(halfV, 1e-3f);
        portals_.push_back(p);
        return static_cast<int>(portals_.size()) - 1;
    }
    void updatePortal(int id, const Vec3& center, const Vec3& axisU, const Vec3& axisV,
                      float halfU, float halfV) {
        if (id < 0 || id >= static_cast<int>(portals_.size())) return;
        Portal& p = portals_[static_cast<std::size_t>(id)];
        p.center = center; p.axisU = normalized(axisU); p.axisV = normalized(axisV);
        p.halfU = std::max(halfU, 1e-3f); p.halfV = std::max(halfV, 1e-3f);
    }
    int portalCount() const { return static_cast<int>(portals_.size()); }
    void clearPortals() { portals_.clear(); }

    // ポータルがどれだけ開いているかを帯域別に測る。
    //   outFrac6 : 帯域ごとの「通る割合」(0..1)。矩形が完全に塞がれれば 0、素通しなら 1。
    //   outPoint : 開いている部分の重み付き重心（ワールド）。**定位に使う**。
    //              エネルギーと方向が同じ積分から出るので、両者がずれて飛ぶことがない。
    //
    //   帯域差はフレネルゾーンの大きさから出る。高域はゾーンが小さいので隙間に収まって
    //   よく通り、低域はゾーンが大きいので塞がれた部分に掛かる ＝「開くと明るくなる」。
    //   ゾーンの中心は直線と矩形面の交点（連続に動く）。**矩形で切られるので暴走しない** ──
    //   ここが従来との決定的な違いで、中心の置き方に答えが無い問題が消える。
    bool portalOpenBands(const Portal& pt, const Vec3& listener, const Vec3& source,
                         float* outFrac6, Vec3* outPoint) const {
        for (int b = 0; b < kNumBands; ++b) outFrac6[b] = 1.0f;
        if (!pt.active) return false;

        Vec3 n = normalized(cross(pt.axisU, pt.axisV));
        // 法線はリスナー側へ向ける。「奥にある物体」を判定するのに要る。
        if (dot(n, listener - pt.center) < 0.0f) n = Vec3(-n.x, -n.y, -n.z);
        const Vec3 u = pt.axisU, v = pt.axisV;

        // ゾーンの中心＝直線と矩形面の交点（矩形の外に出たら端へ寄せる）。
        const Vec3 ls = source - listener;
        const float den = dot(n, ls);
        Vec3 hitP = pt.center;
        if (std::fabs(den) > 1e-6f) {
            const float t = dot(n, pt.center - listener) / den;
            hitP = listener + ls * t;
        }
        float cu = dot(hitP - pt.center, u);
        float cv = dot(hitP - pt.center, v);
        cu = std::min(std::max(cu, -pt.halfU), pt.halfU);
        cv = std::min(std::max(cv, -pt.halfV), pt.halfV);

        const Vec3 zoneCenter = pt.center + u * cu + v * cv;
        const float d1 = std::max(length(zoneCenter - listener), 1e-3f);
        const float d2 = std::max(length(source - zoneCenter), 1e-3f);

        // 遮るものを**リスナーから見た影**として矩形面へ落とす（凸多角形の集まり）。
        //   断面ではなく影なのは、斜めに立った薄い板が断面にほぼ現れないため（実測で確認）。
        struct Poly { float u[8], v[8]; int n; };
        static thread_local std::vector<Poly> polys;
        polys.clear();
        const float planeD = dot(n, pt.center - listener);
        auto project = [&](const Vec3& q, float& ou, float& ov) -> bool {
            const Vec3 dq = q - listener;
            const float dd = dot(n, dq);
            if (std::fabs(dd) < 1e-6f) return false;
            const float t = planeD / dd;
            if (t <= 0.0f) return false;
            const Vec3 p = listener + dq * t;
            ou = dot(p - pt.center, u);
            ov = dot(p - pt.center, v);
            return true;
        };
        auto hullInto = [&](const float* px, const float* py, int m, Poly& out) {
            int idx[8];
            for (int i = 0; i < m; ++i) idx[i] = i;
            std::sort(idx, idx + m, [&](int a, int b2) {
                return (px[a] != px[b2]) ? (px[a] < px[b2]) : (py[a] < py[b2]);
            });
            auto cr2 = [&](int o, int a, int b2) {
                return (px[a] - px[o]) * (py[b2] - py[o]) - (py[a] - py[o]) * (px[b2] - px[o]);
            };
            int st[18]; int k = 0;
            for (int i = 0; i < m; ++i) {
                while (k >= 2 && cr2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            const int lower = k + 1;
            for (int i = m - 2; i >= 0; --i) {
                while (k >= lower && cr2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            out.n = std::min(k - 1, 8);
            for (int i = 0; i < out.n; ++i) { out.u[i] = px[st[i]]; out.v[i] = py[st[i]]; }
        };
        // ★ポータル面より**奥**にある物体は、ポータルを塞いでいない。
        //   リスナーから戸口越しに見える奥の部屋の床や天井は、レイがポータルを
        //   通り抜けた**先**で当たるもの。これを塞いでいると数えると、全開でも
        //   開口率が上がらない（実測で 90° でも 0.343 止まりだった原因がこれ）。
        //   全開の扉も、戸口の外へ振れ切れば「奥の部屋の物体」になる ── それで正しい。
        //   開口が開いているかどうかと、その先に何があるかは別の話。
        auto beyondPortal = [&](const Obb& ob) {
            const float d = dot(ob.center - pt.center, n);
            const float ext = std::fabs(dot(ob.axisX, n)) * ob.halfExtents.x
                            + std::fabs(dot(ob.axisY, n)) * ob.halfExtents.y
                            + std::fabs(dot(ob.axisZ, n)) * ob.halfExtents.z;
            return (d + ext) < -0.01f;      // 完全に奥側
        };
        for (const Instance& inst : instances_) {
            if (!inst.active || beyondPortal(inst.obb)) continue;
            if (inst.geomId >= 0 && inst.geomId < static_cast<int>(meshes_.size())
                && meshes_[static_cast<std::size_t>(inst.geomId)].used) {
                const MeshGeometry& g = meshes_[static_cast<std::size_t>(inst.geomId)];
                const int nt = g.bvh.triangleCount();
                for (int ti = 0; ti < nt && polys.size() < 2048; ++ti) {
                    const Triangle& lt = g.bvh.triangle(ti);
                    const Vec3 tv[3] = {meshLocalToWorldPoint(lt.v0, inst.obb),
                                        meshLocalToWorldPoint(lt.v1, inst.obb),
                                        meshLocalToWorldPoint(lt.v2, inst.obb)};
                    Poly pg; bool ok = true;
                    for (int i = 0; i < 3 && ok; ++i) ok = project(tv[i], pg.u[i], pg.v[i]);
                    if (!ok) continue;
                    pg.n = 3;
                    polys.push_back(pg);
                }
            } else {
                const Obb& ob = inst.obb;
                float px[8], py[8]; bool ok = true;
                for (int i = 0; i < 8 && ok; ++i) {
                    const float sx = (i & 1) ? 1.0f : -1.0f;
                    const float sy = (i & 2) ? 1.0f : -1.0f;
                    const float sz = (i & 4) ? 1.0f : -1.0f;
                    ok = project(ob.center + ob.axisX * (ob.halfExtents.x * sx)
                                           + ob.axisY * (ob.halfExtents.y * sy)
                                           + ob.axisZ * (ob.halfExtents.z * sz), px[i], py[i]);
                }
                if (!ok) continue;
                Poly pg; hullInto(px, py, 8, pg);
                if (pg.n >= 3) polys.push_back(pg);
            }
        }

        // 行ごとに「影の和集合」の補集合＝開いている区間を出し、帯域ごとの核で積む。
        constexpr float kBandHz[kNumBands] = {125, 250, 500, 1000, 2000, 4000};
        constexpr int kRows = 32, kCols = 12;
        struct Span { float lo, hi; };
        static thread_local std::vector<Span> spans, openRow;
        static thread_local std::vector<int> begin_;
        openRow.clear();
        begin_.assign(kRows + 1, 0);
        const float rowH = 2.0f * pt.halfV / kRows;
        double area = 0.0, ccu = 0.0, ccv = 0.0;
        for (int j = 0; j < kRows; ++j) {
            begin_[j] = static_cast<int>(openRow.size());
            const float y = -pt.halfV + rowH * (j + 0.5f);
            spans.clear();
            for (const Poly& pg : polys) {
                float lo, hi;
                if (fresnel::polygonSpanAtY(pg.u, pg.v, pg.n, y, lo, hi))
                    spans.push_back(Span{lo, hi});
            }
            std::sort(spans.begin(), spans.end(),
                      [](const Span& a, const Span& b) { return a.lo < b.lo; });
            float cursor = -pt.halfU;
            auto emit = [&](float lo, float hi) {
                if (hi <= lo) return;
                openRow.push_back(Span{lo, hi});
                const double w = static_cast<double>(hi - lo) * rowH;
                area += w; ccu += w * 0.5 * (lo + hi); ccv += w * y;
            };
            for (const Span& sp : spans) {
                const float lo = std::min(std::max(sp.lo, -pt.halfU), pt.halfU);
                const float hi = std::min(std::max(sp.hi, -pt.halfU), pt.halfU);
                if (lo > cursor) emit(cursor, lo);
                cursor = std::max(cursor, hi);
                if (cursor >= pt.halfU) break;
            }
            if (cursor < pt.halfU) emit(cursor, pt.halfU);
        }
        begin_[kRows] = static_cast<int>(openRow.size());

        if (area <= 1e-9) {                       // 完全に塞がれている
            for (int b = 0; b < kNumBands; ++b) outFrac6[b] = 0.0f;
            if (outPoint) *outPoint = pt.center;
            return true;
        }
        if (outPoint)
            *outPoint = pt.center + u * static_cast<float>(ccu / area)
                                  + v * static_cast<float>(ccv / area);

        for (int b = 0; b < kNumBands; ++b) {
            const float lambda = 343.0f / kBandHz[b];
            const float r1 = std::max(std::sqrt(lambda * d1 * d2 / (d1 + d2)), 1e-3f);
            const float invR2 = 1.0f / (r1 * r1);
            auto kern = [&](float pu, float pv) {
                const float du = pu - cu, dv = pv - cv;
                return 1.0 / (1.0 + static_cast<double>(du * du + dv * dv) * invR2);
            };
            auto integ = [&](float lo, float hi, float pv) {
                if (hi <= lo) return 0.0;
                const float st = (hi - lo) / kCols;
                double acc = 0.0;
                for (int c = 0; c < kCols; ++c) acc += kern(lo + st * (c + 0.5f), pv);
                return acc * st;
            };
            double numer = 0.0, denom = 0.0;
            for (int j = 0; j < kRows; ++j) {
                const float y = -pt.halfV + rowH * (j + 0.5f);
                denom += integ(-pt.halfU, pt.halfU, y);      // 矩形全体＝素通しのとき
                for (int i = begin_[j]; i < begin_[j + 1]; ++i)
                    numer += integ(openRow[static_cast<std::size_t>(i)].lo,
                                   openRow[static_cast<std::size_t>(i)].hi, y);
            }
            outFrac6[b] = (denom > 1e-12)
                        ? static_cast<float>(std::min(1.0, numer / denom)) : 1.0f;
        }
        return true;
    }

    const Portal& portal(int i) const { return portals_[static_cast<std::size_t>(i)]; }

    // ポータル**経由で実際に届く量**。開口率（どれだけ開いているか）に、
    // 位置関係で決まる2つの係数を掛ける。開口率そのものは幾何の量なので汚さない。
    //
    //   ① 開口の指向性 … 戸口は正面へ強く放射し、斜めでは弱い（cos 則）。
    //      両側（リスナー側・音源側）に掛かる。
    //   ② 遠回りの広がり損失 … リスナー→開口→音源は直線より長い。その比で減る。
    //
    // なぜ要るか（実測）:
    //   「直線が矩形を横切るか」の二値判定を外して崖（8.8dB）は消えたが、
    //   代わりに**位置による変化まで平らになった**（音源を戸口から 1.6m 外しても
    //   0.978 のまま）。開口率はゾーンの中心を矩形内へ丸めるので、
    //   線がどれだけ外れても「矩形の端から見た開き具合」しか見ていない。
    //   外れた距離を効かせるのがこの2係数。どちらも閾値を持たないので崖は戻らない。
    bool portalCoupling(const Portal& pt, const Vec3& listener, const Vec3& source,
                        float* outBands6, Vec3* outPoint) const {
        Vec3 cp(0, 0, 0);
        if (!portalOpenBands(pt, listener, source, outBands6, &cp)) return false;
        if (outPoint) *outPoint = cp;

        Vec3 n = normalized(cross(pt.axisU, pt.axisV));
        const Vec3 toL = listener - cp;
        const Vec3 toS = source - cp;
        const float dl = std::max(length(toL), 1e-3f);
        const float ds = std::max(length(toS), 1e-3f);
        // 開口の指向性。法線から離れるほど弱い。裏側へは出さない（0 で切る）。
        const float cl2 = std::fabs(dot(toL, n)) / dl;
        const float cs2 = std::fabs(dot(toS, n)) / ds;
        // 遠回りの広がり損失（振幅比）。まっすぐ通れば 1。
        const float direct = std::max(length(source - listener), 1e-3f);
        const float detour = std::min(direct / (dl + ds), 1.0f);

        const float k = scene_detail::clamp01(cl2) * scene_detail::clamp01(cs2) * detour;
        for (int b = 0; b < kNumBands; ++b) outBands6[b] *= k;
        return true;
    }

    // この経路を支配するポータル。線分が矩形を通るものを返す（無ければ nullptr）。
    //   ★「ポータルがあるならポータルが唯一の答え」を1箇所で判定するための入口。
    //     同じ問いに旧経路（前川＋開口積分）とポータルの2つが答えを持つと、
    //     片方が古くなって矛盾する（実測: 閉じた部屋なのに回折が -39dB の床を作り、
    //     壁の裏だけ材質に関係なく平坦になっていた）。
    const Portal* governingPortal(const Vec3& listener, const Vec3& source) const {
        if (portals_.empty()) return nullptr;
        const Vec3 d = source - listener;
        const float dist = length(d);
        if (dist < 1e-4f) return nullptr;
        const Vec3 dir = d * (1.0f / dist);
        for (const Portal& pt : portals_) {
            if (!pt.active) continue;
            const Vec3 pn = normalized(cross(pt.axisU, pt.axisV));
            const float dn = dot(pn, dir);
            if (std::fabs(dn) < 1e-4f) continue;
            const float t = dot(pn, pt.center - listener) / dn;
            if (t <= 0.0f || t >= dist) continue;
            const Vec3 hit = listener + dir * t;
            if (std::fabs(dot(hit - pt.center, pt.axisU)) > pt.halfU) continue;
            if (std::fabs(dot(hit - pt.center, pt.axisV)) > pt.halfV) continue;
            return &pt;
        }
        return nullptr;
    }

    // atPoint を渡すと、フレネルゾーンの中心を**その点**に置く。
    //
    // ★なぜ要るか（実測で潰した失敗）:
    //   中心を「リスナーと音源を結ぶ直線と平面の交点」に置くと、音源が戸口の正面に
    //   無いときその直線は**壁の中**を通る。するとゾーンは戸口ではなく壁の上に乗り、
    //   測っているのが「扉の開き具合」ではなく「戸口とゾーンの面積比」という
    //   固定値になる。実測（扉 0°→90°）:
    //       125Hz 0.041 → 0.183(10°) → 0.203(90°)   ＝ 10°で頭打ち
    //       4kHz  0.000 → 0.000      → 0.000        ＝ 一度も開かない
    //   幾何では隙間は 10°で1.8cm、90°で1.2m ＝ 67倍開くのに、音では2.3倍しか動かない。
    //   ＝「どのくらい開いたか」を伝えるというコンセプトそのものを外していた。
    //
    //   回折では音が実際に通る場所は開口なので、そこを中心にする。
    //   すると高域（ゾーンが小さい）は開口に収まって 1.0 まで上がり、
    //   「開くと明るくなる」が構成から出る。
    //
    //   nullptr のときは従来どおり直線との交点。直接音の判定はこちらのまま
    //   （影境界の崖を消した修正がそこに乗っているので壊さない）。
    bool apertureFresnelBands(const Vec3& listener, const Vec3& source,
                              float outFrac[kNumBands],
                              const Vec3* atPoint = nullptr) const {
        for (int b = 0; b < kNumBands; ++b) outFrac[b] = 1.0f;

        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return false;
        axis = axis * (1.0f / dist);
        // ★測る面は**1枚では足りない**。経路が横切る面を拾い、それぞれで測って
        //   **最も狭い所（最小）**を採る。
        //   1枚だけだと「最初にレイが当たった壁」で面が決まるので、リスナーが動いて
        //   当たり先が入れ替わった瞬間に値が飛ぶ。千鳥配置の戸口で実測:
        //     x=-1.15  当たり=z=4の壁  開口率 0.275
        //     x=-1.10  当たり=z=0の壁  開口率 0.543   ← 5cm で 2倍
        //   経路は**両方の戸口を通っている**のに片方しか測っていなかった、というだけの話。
        //   最小を採れば、面が増えてもその値が大きければ結果は変わらないので跳ばない。
        //
        //   ★所属の判定は「線分が**実体に当たる**か」にしてはいけない。それだと戸口を
        //     すり抜けた瞬間にその壁が候補から落ちるが、落ちる直前の値は 1 ではない
        //     （実測: x=0.90 で 0.159 だった面が x=0.95 で消え、全体が 0.026 → 0.101 に跳んだ）。
        //     判定は「経路がその**面を横切る**か」にする。戸口を通り抜けても面は横切って
        //     いるので落ちない。逆に床・天井は水平な経路と交わらないので入らない
        //     （入れてしまうと部屋の床で全部が潰れる）。
        //   ★レイを進めながら撃ち直す方式も不可。壁の内部から撃つと距離ほぼ 0 の当たりが
        //     返り、1cm ずつしか進めずに厚さ 30cm の壁を抜けられない（実測で 12 歩とも同じ壁）。
        //   ★同じ面を経路方向へずらして複数枚、は**1ビットも変わらなかった**
        //     （±1m を7枚でも同じ値）。要るのは面のオフセットではなく**別の壁**だった。
        constexpr int kMaxPlanes = 4;
        int   planeInst[kMaxPlanes] = {};
        float planeRank[kMaxPlanes] = {};   // 交点が実体にどれだけ近いか(m)。小さいほど効く
        Vec3  planeNrm[kMaxPlanes];
        int nPlanes = 0;
        {
            const int ni = static_cast<int>(instances_.size());
            for (int i = 0; i < ni; ++i) {
                const Instance& inst = instances_[static_cast<std::size_t>(i)];
                if (!inst.active) continue;
                const Obb& ob = inst.obb;
                const float hx = ob.halfExtents.x, hy2 = ob.halfExtents.y, hz = ob.halfExtents.z;
                Vec3 nn = ob.axisZ;                      // 板の法線＝いちばん薄い軸
                if (hx <= hy2 && hx <= hz) nn = ob.axisX;
                else if (hy2 <= hx && hy2 <= hz) nn = ob.axisY;
                if (length(nn) < 1e-4f) continue;
                nn = normalized(nn);
                const float den = dot(nn, source - listener);
                if (std::fabs(den) < 1e-3f * dist) continue;        // 面に平行＝横切らない
                const float tt = dot(nn, ob.center - listener) / den;
                if (tt < 0.02f || tt > 0.98f) continue;             // 交点が線分の外
                // 交点が実体からどれだけ離れているか（0 なら壁の中＝完全に塞いでいる）。
                const Vec3 xp = listener + (source - listener) * tt;
                const Vec3 lp = obbToLocalPoint(xp, ob);
                const float ex = std::max(std::fabs(lp.x) - hx, 0.0f);
                const float ey = std::max(std::fabs(lp.y) - hy2, 0.0f);
                const float ez = std::max(std::fabs(lp.z) - hz, 0.0f);
                const float rank = std::sqrt(ex * ex + ey * ey + ez * ez);

                // 同じ平面のインスタンスはまとめる（戸口の左右の壁など）。影の投影は
                //   どのみち全インスタンスを見るので、同じ平面を二度測る意味がない。
                int same = -1;
                for (int k = 0; k < nPlanes; ++k) {
                    if (std::fabs(dot(planeNrm[k], nn)) < 0.999f) continue;
                    const Vec3 oc = instances_[static_cast<std::size_t>(planeInst[k])].obb.center;
                    if (std::fabs(dot(nn, ob.center - oc)) < 1e-3f) { same = k; break; }
                }
                if (same >= 0) {
                    if (rank < planeRank[same]) { planeInst[same] = i; planeRank[same] = rank; }
                    continue;
                }
                int slot = nPlanes;
                if (nPlanes >= kMaxPlanes) {
                    int worst = 0;
                    for (int k = 1; k < kMaxPlanes; ++k)
                        if (planeRank[k] > planeRank[worst]) worst = k;
                    if (planeRank[worst] <= rank) continue;
                    slot = worst;
                } else ++nPlanes;
                planeInst[slot] = i;
                planeRank[slot] = rank;
                planeNrm[slot] = nn;
            }
        }
        if (nPlanes == 0) {
            // ★当たらなくてもゲートを切ってはいけない。切ると影境界の外側で音量が跳ぶ
            //   （実測: 0.285 → 0.563）。すぐ脇に壁があるなら、フレネルゾーンはまだ
            //   その壁に半分隠されている ── 縁を通り過ぎるにつれて開口率が 1 へ**連続に**
            //   近づくのが正しい。だから「経路に最も近い遮蔽物」を選んで測り続ける。
            //   経路が本当に開けていれば、ゾーンはほぼ開いていて自動的に 1 になる。
            float best = 1e30f;
            int bestId = -1;
            const int ni = static_cast<int>(instances_.size());
            for (int i = 0; i < ni; ++i) {
                const Instance& inst = instances_[static_cast<std::size_t>(i)];
                if (!inst.active) continue;
                // 線分と OBB 中心の距離（近さの目安。厳密でなくてよい）。
                const Vec3 d0 = inst.obb.center - listener;
                const float t2 = std::min(std::max(dot(d0, axis), 0.0f), dist);
                const float dd = length(d0 - axis * t2);
                if (dd < best) { best = dd; bestId = i; }
            }
            if (bestId < 0) return false;
            planeInst[nPlanes++] = bestId;
        }

        const float directLen = std::max(length(source - listener), 1e-4f);

        for (int b = 0; b < kNumBands; ++b) outFrac[b] = 1.0f;

        for (int pl = 0; pl < nPlanes; ++pl) {
        const Obb& planeObb = instances_[static_cast<std::size_t>(planeInst[pl])].obb;

        // 面の向きは**壁自身の形**から決める。OBB のいちばん薄い軸＝板の法線。
        //   レイが当たった面の法線を使ってはいけない。すれすれの入射だとレイは正面ではなく
        //   「厚み側の面」に当たり、その法線は板に**垂直**なので、壁を縦に切った
        //   ほぼ空っぽの断面を見てしまう（実測: 影境界の手前で開口率が 0.485 → 0.947 に跳んだ）。
        Vec3 nrm;
        {
            const float hx = planeObb.halfExtents.x, hy2 = planeObb.halfExtents.y,
                        hz = planeObb.halfExtents.z;
            Vec3 thin = planeObb.axisZ;
            if (hx <= hy2 && hx <= hz) thin = planeObb.axisX;
            else if (hy2 <= hx && hy2 <= hz) thin = planeObb.axisY;
            if (dot(thin, axis) > 0.0f) thin = Vec3(-thin.x, -thin.y, -thin.z);
            nrm = thin;
        }
        if (length(nrm) < 1e-4f) nrm = axis;
        nrm = normalized(nrm);
        Vec3 u = cross(nrm, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(nrm, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(nrm, u));

        // 面内の位置は「直線が当たった点」、**深さは遮蔽物の中心**に置く。
        //   表面ちょうどに平面を置くと断面が退化する ── 面上の稜線・三角形は両端とも
        //   平面上にあるので符号が変わらず、交点が1つも拾えない。厚みの真ん中なら確実に切れる。
        //   ★平面は**無限に延長**して、直線との交点を中心にする。
        //     レイが掠めた点をそのまま使うと、すれすれの入射で交点が板の縁に張り付き、
        //     リスナーが動いても中心が動かない＝開口率が頭打ちになる
        //     （実測: 影境界の手前で 0.522 のまま平らになり、その先で 1.0 へ飛んだ）。
        const Vec3 ic = planeObb.center;
        Vec3 centerP;
        if (atPoint) {
            centerP = *atPoint - nrm * dot(*atPoint - ic, nrm);
        } else {
            const float den = dot(nrm, source - listener);
            centerP = (std::fabs(den) > 1e-6f)
                    ? listener + (source - listener) * (dot(nrm, ic - listener) / den)
                    : ic;
        }
        const float d1 = std::max(length(centerP - listener), 1e-3f);
        const float d2 = std::max(length(source - centerP), 1e-3f);

        // ★遮蔽物を「面で切った断面」ではなく「**リスナーから見た影**」として面に落とす。
        //
        //   断面では斜めに立った薄い物体を見られない。45°の扉はどの面で切っても
        //   厚み3cmの筋にしかならず、面積をほとんど塞がない（実測: 面を7枚に増やしても
        //   値が1ビットも変わらなかった）。
        //   ところが実際に音を止めているのは断面積ではなく**遮っている見かけの広さ**で、
        //   45°の扉は投影すると 1.2·cos45° = 85cm を塞ぐ。
        //   建築音響の合成透過率が使う開口面積もこの投影で、扉の隙間が 1−cos θ で
        //   増える（前半ゆっくり・後半で急に開く＝クレッシェンド）のはここから出る。
        //
        //   投影はリスナーからの**透視投影**。「窓のうちこの物体がどれだけ塞いでいるか」
        //   そのものなので、レイを撒いて数えるのと同じことを解析的にやっていることになる。
        //   扉・戸口・開き角は参照しない。箱でも三角形でも同じ式で影になる。
        //
        //   影は**凸多角形**として持つ。凸なら行ごとの区間が必ず1本に決まるので、
        //   物体をまたいで偶奇規則を使う必要が消える（和集合を直接取れる）。
        //   以前は偶奇を全物体まとめて掛けていて、断面が接する所で内外が反転していた。
        struct Poly { float u[8], v[8]; int n; };
        constexpr int kMaxPoly = 2048;
        static thread_local std::vector<Poly> polys;
        polys.clear();

        // 点をリスナーから面へ透視投影する。面より手前/後ろに回り込む点は捨てる。
        const float planeD = dot(nrm, centerP - listener);
        auto project = [&](const Vec3& q, float& ou, float& ov) -> bool {
            const Vec3 dq = q - listener;
            const float den = dot(nrm, dq);
            if (std::fabs(den) < 1e-6f) return false;
            const float t = planeD / den;
            if (t <= 0.0f) return false;
            const Vec3 p = listener + dq * t;
            ou = dot(p - centerP, u);
            ov = dot(p - centerP, v);
            return true;
        };
        // 投影した点群の凸包（単調チェイン）。影の輪郭。
        auto hullInto = [&](const float* px, const float* py, int m, Poly& out) {
            int idx[8];
            for (int i = 0; i < m; ++i) idx[i] = i;
            std::sort(idx, idx + m, [&](int a, int b2) {
                return (px[a] != px[b2]) ? (px[a] < px[b2]) : (py[a] < py[b2]);
            });
            auto cross2 = [&](int o, int a, int b2) {
                return (px[a] - px[o]) * (py[b2] - py[o]) - (py[a] - py[o]) * (px[b2] - px[o]);
            };
            int st[18]; int k = 0;
            for (int i = 0; i < m; ++i) {
                while (k >= 2 && cross2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            const int lower = k + 1;
            for (int i = m - 2; i >= 0; --i) {
                while (k >= lower && cross2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            out.n = std::min(k - 1, 8);
            for (int i = 0; i < out.n; ++i) { out.u[i] = px[st[i]]; out.v[i] = py[st[i]]; }
        };

        for (const Instance& inst : instances_) {
            if (!inst.active || static_cast<int>(polys.size()) >= kMaxPoly) continue;
            if (inst.geomId >= 0 && inst.geomId < static_cast<int>(meshes_.size())
                && meshes_[static_cast<std::size_t>(inst.geomId)].used) {
                // メッシュ：三角形ごとに影を落とす。三角形は凸なのでそのまま使える。
                //   ★境界箱で代用してはいけない。戸口の空いた壁を一枚板として扱ってしまい、
                //     回折が 1/20 に潰れる（実測）。
                const MeshGeometry& g = meshes_[static_cast<std::size_t>(inst.geomId)];
                const int nt = g.bvh.triangleCount();
                for (int ti = 0; ti < nt && static_cast<int>(polys.size()) < kMaxPoly; ++ti) {
                    const Triangle& lt = g.bvh.triangle(ti);
                    const Vec3 tv[3] = {meshLocalToWorldPoint(lt.v0, inst.obb),
                                        meshLocalToWorldPoint(lt.v1, inst.obb),
                                        meshLocalToWorldPoint(lt.v2, inst.obb)};
                    Poly pg; pg.n = 0;
                    bool ok = true;
                    for (int i = 0; i < 3 && ok; ++i)
                        ok = project(tv[i], pg.u[i], pg.v[i]);
                    if (!ok) continue;
                    pg.n = 3;
                    polys.push_back(pg);
                }
            } else {
                // 箱：8頂点を投影して凸包を取る（＝箱の影の輪郭）。
                const Obb& ob = inst.obb;
                float px[8], py[8];
                bool ok = true;
                for (int i = 0; i < 8 && ok; ++i) {
                    const float sx = (i & 1) ? 1.0f : -1.0f;
                    const float sy = (i & 2) ? 1.0f : -1.0f;
                    const float sz = (i & 4) ? 1.0f : -1.0f;
                    const Vec3 q = ob.center + ob.axisX * (ob.halfExtents.x * sx)
                                             + ob.axisY * (ob.halfExtents.y * sy)
                                             + ob.axisZ * (ob.halfExtents.z * sz);
                    ok = project(q, px[i], py[i]);
                }
                if (!ok) continue;
                Poly pg; pg.n = 0;
                hullInto(px, py, 8, pg);
                if (pg.n >= 3) polys.push_back(pg);
            }
        }
        if (polys.empty()) continue;      // この面には影が無い＝この面は絞らない

        // ================= 開いている所を一度だけ解き、その上で帯域を回す =================
        //
        // ★「開口を見つけて測る」ことを**していない**。開口という対象はここに存在しない。
        //   面の上を一様に舐めて、点ごとに「影に入っているか」だけを見る。
        //   扉・戸口・蝶番・開き角は一度も参照しない。扉は影を落とす物体の一つで、
        //   木箱でも瓦礫でも同じ式が同じように走る。
        //
        // 帯域ごとの違いをどこから出すか（ここが何度も間違えた所）:
        //   波動として正しいのは「**開口が波長に対して大きいほどよく通る**」。
        //   だから開くと高域から先に素通りになる ＝「開くと明るくなる」。
        //   ところが窓を**直線と平面の交点**に置くと、高域ほど窓が小さくなり、
        //   小さくなるほど戸口から離れて開口を見失う。符号が逆転して、
        //   開いても高域が伸びなくなる（実測: 全開でも 4kHz 0.007）。
        //
        //   かといって窓を稜線由来の開口点に置くと跳ぶ（実測 3m）。
        //   そこで**開いている領域そのものの重心**に置く。扉が回れば開いている所は
        //   連続に動くので、その重心も連続に動く。離散的な探索をしていないので飛ばない。
        //
        // 手順:
        //   A) 大きい固定の窓で、行ごとの「開いている区間」を一度だけ出す（幾何は1回）
        //   B) その面積重心を出す（連続に動く点）
        //   C) 帯域ごとに、重心を中心とする波長サイズの核で開いている所を積む
        //      核 k(p) = 1/(1 + (|p−重心| / r1_b)²)、r1_b は第1フレネル半径
        //      開口が r1_b より大きければ核はほぼ開口に収まる → 1 に近づく（明るい）
        constexpr float kBandHz[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
        constexpr int kRows = 32;      // v 方向の行数
        constexpr int kCols = 12;      // 各区間の中で核を積む点数（区間の端は厳密）
        constexpr float kWindow = 3.0f;
        struct Span { float lo, hi; };
        static thread_local std::vector<Span> spans;    // その行の影（作業用）
        static thread_local std::vector<Span> openRow;  // 行ごとの開いている区間（連結して保持）
        static thread_local std::vector<int> openBegin; // 行 j の開始位置

        // 重心に核を置く方式を試すためのスイッチ。既定は OFF（下の記録を参照）。
        const bool useCentroidKernel = false;

        // ★★ 実測で分かったこと（この方式は未完成。既定では使っていない）★★
        //   窓を低域の大きさに固定して重心へ核を置くと、**部屋の外側が「開いている」と
        //   数えられる**。仕切りの平面は部屋の外まで伸びていて、そこには何も無いので
        //   遮られていない扱いになり、重心がそちらへ引っ張られる。
        //   結果、閉扉でも 0.517 が出て、扉の全掃引で 4% しか動かなかった。
        //   ＝ 以前の δ 重みは「減衰」の顔をして**領域を絞る役目も担っていた**。
        //   直すには δ とは別に、面のうち意味のある範囲を決める仕組みが要る。
        //   （部屋という概念はコアに持たない方針なので、そこの設計から）
        const float lamLo = 343.0f / kBandHz[0];
        const float r1lo = std::sqrt(lamLo * d1 * d2 / (d1 + d2));
        if (r1lo < 1e-4f) continue;
        const float R = r1lo * kWindow;
        const float rowH = 2.0f * R / kRows;

        // ── A) 開いている区間を行ごとに出す（幾何は帯域に依らないので1回だけ）──
        openRow.clear();
        openBegin.assign(kRows + 1, 0);
        double area = 0.0, cu = 0.0, cv = 0.0;
        for (int j = 0; j < kRows; ++j) {
            openBegin[j] = static_cast<int>(openRow.size());
            const float y = -R + rowH * (j + 0.5f);

            // 影（凸多角形）ごとに、この行を横切る区間を1本ずつ取る。
            //   凸なので区間は必ず1本＝端は解析的に決まる。u 方向は探索していないので
            //   どんなに細い隙間も取りこぼさない。影は重なってよい（和集合を取る）。
            spans.clear();
            for (const Poly& pg : polys) {
                float lo, hi;
                if (fresnel::polygonSpanAtY(pg.u, pg.v, pg.n, y, lo, hi))
                    spans.push_back(Span{lo, hi});
            }
            std::sort(spans.begin(), spans.end(),
                      [](const Span& a, const Span& b) { return a.lo < b.lo; });

            // 影の**補集合**＝開いている区間。
            float cursor = -R;
            auto emit = [&](float lo, float hi) {
                if (hi <= lo) return;
                openRow.push_back(Span{lo, hi});
                const double w = static_cast<double>(hi - lo) * rowH;
                area += w; cu += w * 0.5 * (lo + hi); cv += w * y;
            };
            for (const Span& sp : spans) {
                const float lo = std::min(std::max(sp.lo, -R), R);
                const float hi = std::min(std::max(sp.hi, -R), R);
                if (lo > cursor) emit(cursor, lo);
                cursor = std::max(cursor, hi);
                if (cursor >= R) break;
            }
            if (cursor < R) emit(cursor, R);
        }
        openBegin[kRows] = static_cast<int>(openRow.size());
        if (area <= 1e-9) {                 // どこも開いていない
            for (int b = 0; b < kNumBands; ++b) outFrac[b] = 0.0f;
            continue;
        }

        // ── B) 開いている領域の面積重心。連続に動く点。──
        const float ku = static_cast<float>(cu / area);
        const float kv = static_cast<float>(cv / area);

        // ── C) 帯域ごとに、重心まわりの波長サイズの核で積む ──
        for (int b = 0; b < kNumBands; ++b) {
            const float lambda = 343.0f / kBandHz[b];
            const float r1 = std::sqrt(lambda * d1 * d2 / (d1 + d2));
            if (r1 < 1e-4f) continue;
            const float invR2 = 1.0f / (r1 * r1);
            const float invLam2 = 2.0f / lambda;

            // 既定は δ（遠回り）による重み。これは減衰であると同時に、
            //   **面のうち意味のある範囲を絞る役目**も担っている（外すと部屋の外まで
            //   「開いている」と数えてしまう）。役割が二つ乗っているのが今の弱点。
            auto kern = [&](float pu, float pv) -> double {
                if (useCentroidKernel) {
                    const float du = pu - ku, dv = pv - kv;
                    return 1.0 / (1.0 + static_cast<double>(du * du + dv * dv) * invR2);
                }
                const Vec3 p = centerP + u * pu + v * pv;
                const float dl = length(p - listener) + length(source - p) - directLen;
                const float nz = std::max(dl, 0.0f) * invLam2 * deltaWeight_;
                return 1.0 / (1.0 + static_cast<double>(nz) * nz);
            };
            auto integrate = [&](float lo, float hi, float pv) -> double {
                if (hi <= lo) return 0.0;
                const float step = (hi - lo) / kCols;
                double acc = 0.0;
                for (int c = 0; c < kCols; ++c) acc += kern(lo + step * (c + 0.5f), pv);
                return acc * step;
            };

            double numer = 0.0, denom = 0.0;
            for (int j = 0; j < kRows; ++j) {
                const float y = -R + rowH * (j + 0.5f);
                denom += integrate(-R, R, y);
                for (int i = openBegin[j]; i < openBegin[j + 1]; ++i)
                    numer += integrate(openRow[static_cast<std::size_t>(i)].lo,
                                       openRow[static_cast<std::size_t>(i)].hi, y);
            }
            const float here = (denom > 1e-12)
                             ? static_cast<float>(std::min(1.0, numer / denom))
                             : 1.0f;
            outFrac[b] = std::min(outFrac[b], here);
        }
        }   // 面のループ
        return true;
    }

    // 【開口のエネルギー】遮蔽物の面に開いている**面積**(m²)と、その実効幅(m)を返す。
    //
    //   ★これは幾何ではなく**波動側の量**。キルヒホッフの開口積分の離散近似にあたる。
    //     幾何音響は「レイ」と「稜線」しか知らず、**開口面積という概念を持たない**。
    //     前川の式は半無限スクリーンの実験式で、エネルギー保存から導かれたものではない。
    //     だから「隙間をどれだけのエネルギーが通るか」を稜線探索から絞り出そうとするのは
    //     モデルの適用域の外で、実際6回失敗した。役割を分けるのが正しい:
    //       方向（定位）… 幾何（稜線探索）。ここは幾何の本領
    //       エネルギー   … 開口面積。ここは波動側
    //
    //   ★連続性のために「離散的な探索の上に測定を乗せない」。
    //     以前は「開いた点を1つ見つけて、そこからの差し渡し」を測っていた。
    //     見つかる点が変われば値が飛ぶので、まさに消したかった段差を自分で作っていた
    //     （実測: 10°で0.012 → 15°で4.000 → 25°で0.377）。
    //     行ごとに「抜けられる区間の長さ」を二分探索で求めて足し上げれば、
    //     扉が動くと**区間の端が連続に動く** so 面積も連続に変わる。
    //
    //   測る面と貫通方向は**壁の法線**で決める。経路の向きで決めると壁を斜めに切った面に
    //   なり、走査点も貫通レイも壁の外へずれる（実測で 3m 級の誤った値が出た）。
    //   中心は「直線が面と交わる点」。同一平面の壁なら、当たるインスタンスが
    //   左右の壁の間で入れ替わっても平面は変わらないので連続。
    float apertureOpenArea(const Vec3& listener, const Vec3& source, float* outWidth) const {
        if (outWidth) *outWidth = 0.0f;
        const int rows = gridSamples_;
        if (rows <= 1) return 0.0f;

        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return 0.0f;
        axis = axis * (1.0f / dist);
        const SceneHit hit = raycastClosest(listener, axis, dist);
        if (!hit.hit) return -1.0f;              // 遮るものが無い＝制限しない

        Vec3 nrm = hit.normal;
        if (length(nrm) < 1e-4f) nrm = axis;
        nrm = normalized(nrm);
        Vec3 u = cross(nrm, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(nrm, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(nrm, u));

        const float R = gridRadius_;
        const float depth = gridDepth_;
        const Vec3 origin = hit.point;
        auto passes = [&](float x, float y) {
            const Vec3 p = origin + u * x + v * y;
            return !isOccluded(p - nrm * depth, p + nrm * depth);
        };

        // 行ごとに「抜けられる区間の長さ」を積む。端は二分探索で詰めるので、
        // 扉が動くと長さが連続に変わる（セルが1つずつ入れ替わるのではない）。
        constexpr int kCoarse = 16;      // 1行あたりの粗い刻み
        constexpr int kBisect = 6;
        const float step = 2.0f * R / kCoarse;
        const float rowH = 2.0f * R / static_cast<float>(rows - 1);

        double area = 0.0;
        float openMinV = 1e30f, openMaxV = -1e30f;
        // 1行ぶんの「抜けられる長さ」。steps を増やすと細い隙間まで拾える。
        auto scanRow = [&](float y, int steps) {
            const float st = 2.0f * R / static_cast<float>(steps);
            double rowLen = 0.0;
            bool prevOpen = passes(-R, y);
            float runStart = -R;
            for (int i = 1; i <= steps; ++i) {
                const float x = -R + st * i;
                const bool nowOpen = passes(x, y);
                if (nowOpen == prevOpen) continue;
                // 境界を二分で詰める。ここが連続性の要 ── 端が連続に動くので
                // 扉が動くと長さも連続に変わる（セルが1つずつ入れ替わるのではない）。
                float lo = x - st, hi = x;
                for (int it = 0; it < kBisect; ++it) {
                    const float mid = 0.5f * (lo + hi);
                    if (passes(mid, y) == prevOpen) lo = mid; else hi = mid;
                }
                const float edge = 0.5f * (lo + hi);
                if (prevOpen) rowLen += edge - runStart; else runStart = edge;
                prevOpen = nowOpen;
            }
            if (prevOpen) rowLen += R - runStart;
            return rowLen;
        };

        for (int j = 0; j < rows; ++j) {
            const float y = -R + rowH * j;
            double rowLen = scanRow(y, kCoarse);
            // ★空振りした行だけ細かく引き直す。
            //   粗い刻み(18cm)より細い隙間は取りこぼす。取りこぼすと「開口なし＝制限しない」
            //   に落ちて音量が跳ね上がるので、扉が開き始める角度で崖になる
            //   （実測: 20°で隣接差 0.21）。空振り時だけ払えばコストは普段かからない。
            if (rowLen <= 1e-6) rowLen = scanRow(y, kCoarse * 4);
            if (rowLen > 1e-4) { openMinV = std::min(openMinV, y); openMaxV = std::max(openMaxV, y); }
            area += rowLen * rowH;
        }
        // ★見つからなくても 0 を返してはいけない。**この測定は狭めるだけの役目**で、
        //   経路が在るかどうかは候補探索が既に決めている。標本した範囲に開口が無いのは
        //   「別の回り方をしている」（衝立の縁を回るなど、開口が半径の外にある）の意味。
        //   0 を返すと衝立の回折を殺す（実測で2件のテストが落ちた）。
        if (area <= 1e-6) return -1.0f;          // 制限しない

        // 実効幅 = 面積 ÷ 縦の広がり。高さH・幅wの縦スリットなら (wH)/H = w。
        if (outWidth) {
            const float spanV = std::max(openMaxV - openMinV + rowH, 1e-3f);
            *outWidth = static_cast<float>(area) / spanV;
        }
        return static_cast<float>(area);
    }

    float apertureWidthByGrid(const Vec3& listener, const Vec3& source,
                              const Vec3* seed = nullptr) const {
        constexpr float kMaxWidth = 4.0f;      // これ以上は「開口として律速しない」
        const int n = gridSamples_;
        if (n <= 1) return kMaxWidth;

        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return kMaxWidth;
        axis = axis * (1.0f / dist);

        const SceneHit hit = raycastClosest(listener, axis, dist);
        if (!hit.hit) return kMaxWidth;        // 遮るものが無い

        // ★面と貫通方向は**壁の法線**で決める。経路の向きで決めてはいけない。
        //   経路は壁に対して斜めなので、その垂直面は壁を斜めに切った面になり、
        //   走査点も貫通レイも壁の外へずれていく（実測で 3m 級の値が出ていた）。
        //   測りたいのは「壁に開いた穴」なので、壁の面内で見て、壁の厚みを貫く。
        Vec3 nrm = hit.normal;
        if (length(nrm) < 1e-4f) nrm = axis;
        nrm = normalized(nrm);
        Vec3 u = cross(nrm, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(nrm, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(nrm, u));

        const float R = gridRadius_;
        const float depth = gridDepth_;
        // 面の手前から奥まで、まっすぐ抜けられるか。厚みを持たせるのが要点で、
        // 面上だけ見ると壁より奥に立っている扉を取りこぼす。
        // 測る面は遮蔽物の面。原点はヒントを**その面へ落とした点**に置く。
        //   直線が当たった点を原点にすると、開口がそこから 1.7m 離れていて
        //   測定半径(1.5m)の外へ出てしまう（実測で細い隙間を全部取りこぼしていた）。
        //   面の位置（軸方向）は遮蔽物のままで、面内の中心だけ開口へ寄せる。
        Vec3 origin = hit.point;
        if (seed) origin = *seed - nrm * dot(*seed - hit.point, nrm);

        auto passes = [&](float x, float y) {
            const Vec3 p = origin + u * x + v * y;
            return !isOccluded(p - nrm * depth, p + nrm * depth);
        };

        // ① 「抜けられる点」を1つ見つける。まずヒントの近傍を細かく、無ければ全体を粗く。
        float ox = 0.0f, oy = 0.0f;
        bool found = false;
        if (seed) {
            const float sx = 0.0f, sy = 0.0f;   // 原点をヒントに置いてあるので中心から
            // ヒントを通る**十字**を細かく走る。面全体を細かく張ると高くつくが、
            // 隙間は必ず稜線に接しているので、稜線の当たりを通る線上を見れば掛かる。
            //   ±25cm を 1.5cm 刻み。20°の扉の隙間(7cm)も、10°(1.8cm)も拾える。
            constexpr int kFine = 33;
            constexpr float kFineR = 0.25f;
            for (int i = 0; i < kFine && !found; ++i) {
                const float t2 = -kFineR + 2.0f * kFineR * i / (kFine - 1);
                if (std::fabs(sx + t2) <= R && std::fabs(sy) <= R && passes(sx + t2, sy)) {
                    ox = sx + t2; oy = sy; found = true;
                }
            }
            for (int i = 0; i < kFine && !found; ++i) {
                const float t2 = -kFineR + 2.0f * kFineR * i / (kFine - 1);
                if (std::fabs(sx) <= R && std::fabs(sy + t2) <= R && passes(sx, sy + t2)) {
                    ox = sx; oy = sy + t2; found = true;
                }
            }
        }
        for (int j = 0; j < n && !found; ++j) {
            const float y = -R + 2.0f * R * j / static_cast<float>(n - 1);
            for (int i = 0; i < n && !found; ++i) {
                const float x = -R + 2.0f * R * i / static_cast<float>(n - 1);
                if (passes(x, y)) { ox = x; oy = y; found = true; }
            }
        }
        // ★見つからなければ「制限しない」。0 を返してはいけない。
        //   経路が在るかどうかは候補探索が既に決めている。ここは**狭めるだけ**の役目で、
        //   近くに開口が見つからないのは「別の回り方をしている」（衝立の縁など）の意味。
        //   ここで 0 を返すと衝立の回折を殺す（実測でそうなった）。
        if (!found) return kMaxWidth;

        // ② そこから二分探索で境界を詰める。格子の刻みでは cm 単位の隙間を解像できない
        //    （半径1.5m を7分割するとセルが50cm。20°の扉の隙間は7cm しかない）。
        auto edgeDist = [&](float dx, float dy) {
            float lo = 0.0f, hi = 0.0f;
            const float step = 2.0f * R / static_cast<float>(n - 1);
            // 粗く外へ進んで、塞がる所を見つける。
            for (hi = step; hi <= 2.0f * R; hi += step) {
                if (!passes(ox + dx * hi, oy + dy * hi)) break;
                lo = hi;
            }
            if (hi > 2.0f * R) return 2.0f * R;     // 端まで開いている
            for (int it = 0; it < 8; ++it) {        // 二分で境界へ寄せる
                const float mid = 0.5f * (lo + hi);
                if (passes(ox + dx * mid, oy + dy * mid)) lo = mid; else hi = mid;
            }
            return lo;
        };
        const float spanU = edgeDist(1.0f, 0.0f) + edgeDist(-1.0f, 0.0f);
        const float spanV = edgeDist(0.0f, 1.0f) + edgeDist(0.0f, -1.0f);
        return std::min(std::min(spanU, spanV), kMaxWidth);
    }

    void setApertureGrid(int samples, float radius, float depth) {
        gridSamples_ = std::min(std::max(samples, 0), 15);
        gridRadius_ = radius;
        gridDepth_ = depth;
    }
    int apertureGridSamples() const { return gridSamples_; }

    // 隙間の幅 → **帯域ごと**のゲイン。「開くと音色が開く」を作る本体。
    //
    //   実測（現実の扉の開閉を収録・解析）:
    //     開−閉の差は 125Hz +2.6dB / 500Hz +7.9dB / 1kHz +10.3dB / 4kHz +8.8dB
    //   低域はほとんど変わらず、中高域だけが大きく増える。理由は役割分担で説明できる:
    //     低域は閉じていても**壁を抜けてくる**（質量則）ので、開けても増える余地が少ない
    //     中高域は壁で止まっているので、開いたぶんだけ丸ごと増える
    //   つまり扉を開けたときの「ぐわっと広がる」は音量ではなく**音色が開くこと**だった。
    //
    //   モデルは「隙間はハイパス」。隅の周波数は幅で決まる: fc ≒ c / (2w)。
    //     w=1.8cm(10°) → fc 9.5kHz … 高域だけが少し漏れる
    //     w=1.2m(全開) → fc 143Hz  … ほぼ素通し
    //   幅が広がるにつれ隅が下がり、低い帯域が順に入ってくる。
    //
    //   ★これは以前捨てた「回折の LPF」とは**逆向き**なので混同しないこと。
    //     捨てたのは前川の式の帯域依存＝「回り込むと高域が減る」で、定位の手がかりを
    //     自ら削る自己矛盾だった。こちらは「開くと高域が入る」で、開き具合を音色で
    //     伝える ── コンセプトの中心そのもの。
    void slitWidthBandGain(float width, float outGain[kNumBands]) const {
        if (slitWidthRef_ <= 1e-4f || width <= 0.0f) {
            for (int b = 0; b < kNumBands; ++b) outGain[b] = (width > 0.0f) ? 1.0f : 0.0f;
            return;
        }
        constexpr float kBandHz[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
        const float fc = 343.0f / (2.0f * std::max(width, 1e-4f));
        for (int b = 0; b < kNumBands; ++b) {
            // 隅より上は素通し、下は 6dB/oct（slitBandSlope_ で調整可）で落ちる。
            float g = kBandHz[b] / fc;
            if (g > 1.0f) g = 1.0f;
            outGain[b] = (slitBandSlope_ == 1.0f) ? g : std::pow(g, slitBandSlope_);
        }
    }

    /// 開口を「透過の一部」として扱う（合成透過率 τ_eff = τ_壁(1−f) + f の f の項）。
    ///   ON: 開口タップは前川の δ 減衰を払わず、f をそのまま持つ。壁の透過は (1−f) で減る。
    ///   OFF: 従来（前川 × 開口率）。
    void setApertureIsTransmission(int on) { apertureIsTransmission_ = (on != 0); }
    int apertureIsTransmission() const { return apertureIsTransmission_ ? 1 : 0; }

    /// 開口率の**幅**を開く指数。形は変えずコントラストだけを上げる。1.0=素通し。
    ///   物理から出るのは形で、量は演出で決める（実測は参照であって目標ではない）。
    void setApertureContrast(float p) { apertureContrast_ = (p > 0.05f) ? p : 0.05f; }
    float apertureContrast() const { return apertureContrast_; }

    /// BTM（有限楔の稜線積分）で回折の帯域ゲインを出す。既定 OFF（前川＋開口積分）。
    ///   ON にすると前川の δ 減衰も開口率も使わず、BTM の値がそのまま帯域ゲインになる。
    void setUseBtm(int on) { useBtm_ = (on != 0); }
    int useBtm() const { return useBtm_ ? 1 : 0; }
    /// 楔の開き角(rad)。既定 1.5π＝箱の凸稜線。薄い衝立なら 2π。
    void setBtmWedgeAngle(float rad) { btmWedgeAngle_ = (rad > 0.1f) ? rad : 4.712389f; }

    /// 開口の積分で δ をどれだけ効かせるか（1=そのまま / 0=効かせない）。deltaWeight_ 参照。
    void setApertureDeltaWeight(float w) { deltaWeight_ = std::max(w, 0.0f); }
    float apertureDeltaWeight() const { return deltaWeight_; }

    void setSlitWidth(float ref, float power) { slitWidthRef_ = ref; slitWidthPower_ = power; }
    void setSlitBandSlope(float s) { slitBandSlope_ = s; }
    float slitBandSlope() const { return slitBandSlope_; }
    float slitWidthRef() const { return slitWidthRef_; }
    float slitWidthPower() const { return slitWidthPower_; }

    // 点が self 以外の箱の内部にあるか。膨らませた稜線候補が壁に埋まっていないかの判定。
    //   メッシュは見ない。メッシュの obb は境界箱なので、中空の部屋でも内部を埋めてしまう。
    //   tol は稜線を膨らませた量。膨らませたぶんだけ箱も太らせて判定しないと、
    //   扉の自由端と戸口の枠がぴったり接している状態で、候補点が扉の**数 mm 外**に
    //   落ちて判定をすり抜ける（実測: 閉扉で 6mm 外に落ちて幻の経路が残った）。
    bool pointInsideOther(const Vec3& P, int selfInst, float tol) const {
        const int n = static_cast<int>(instances_.size());
        for (int i = 0; i < n; ++i) {
            if (i == selfInst) continue;
            const Instance& inst = instances_[i];
            if (!inst.active || inst.geomId >= 0) continue;
            const Vec3 l = obbToLocalPoint(P, inst.obb);
            if (std::fabs(l.x) <= inst.obb.halfExtents.x + tol &&
                std::fabs(l.y) <= inst.obb.halfExtents.y + tol &&
                std::fabs(l.z) <= inst.obb.halfExtents.z + tol) return true;
        }
        return false;
    }

    // 【開口の開き具合】開口まわりの断面のうち、音が実際に通れる割合(0..1)を数える。
    //
    //   ★これが回折の**音量**を決める量。稜線探索は**定位**だけを担当する。
    //
    //   前川の式は δ（迂回の余剰長）だけの関数で、開口がどれだけ開いているかを見ていない。
    //   扉が回っても回折経路が回る戸口の枠は動かないので δ が変わらず、
    //   「扉がどのくらい開いたか」が音に出なかった（実測: 0°→90° で総量 1.000 のまま）。
    //
    //   以前は「開口点まわりの自由な差し渡し(m)」を測っていたが、これは行き止まりだった。
    //   開口点は戸口の枠の上にあり、枠は扉が回っても動かない。そこの自由空間は
    //   「戸口の幅」か「部屋の差し渡し」であって、扉の隙間ではない
    //   （実測: 開き角と無関係に 2.2〜4.0m とばらつくだけだった）。
    //
    //   動くのは板の方なので、**断面のどれだけが塞がっていないか**を直接数える。
    //
    //   断面は**遮蔽物を抜けた先**に置く。遮蔽物の手前に置くと、板より前で判定して
    //     しまい扉の開閉が一切出ない（実測: 閉扉でも 0.583 と出た）。
    //
    //   中心と半径は「塞がれた直線」と「開口点」の両方を含むように取る。
    //     開口点だけを中心にすると円盤の半分が壁の中に入る（実測: 0/12 か 1/12）。
    //     直線の当たり所だけを中心にすると、衝立のように**縁が遠い**障害物で
    //     回り込む先が円盤の外へ出てしまい、正当な回折まで 0 になる。
    //     両端を含む円盤なら、戸口でも衝立でも「音が squeeze する領域」を覆える。
    //
    //   判定は「**リスナーからその点が見えるか**」だけ。音源まで見通せるかは問わない。
    //     問うてしまうと、2 段目の戸口が別に在る配置（千鳥）で常に 0 になり、
    //     正当な 2 次回折が全部黙る。ここで測りたいのは「手前の開口がどれだけ開いているか」。
    //
    //   各サンプルは独立なので、扉が回れば「見えるサンプル数」が滑らかに変わる
    //   ＝連続性が構造的に保証される。
    float apertureOpenness(const Vec3& listener, const Vec3& source,
                           const Vec3& aperture) const {
        const int n = apertureOpenSamples_;
        if (n <= 0) return 1.0f;                       // 0 で無効化（常に素通し）
        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return 1.0f;
        axis = axis * (1.0f / dist);

        const SceneHit hit = raycastClosest(listener, axis, dist);
        if (!hit.hit) return 1.0f;                     // 遮るものが無い

        Vec3 u = cross(axis, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(axis, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(axis, u));

        // 断面上での「塞がれた点」と「開口点」の位置（軸方向の成分は捨てる）。
        const Vec3 dh = hit.point - listener;
        const Vec3 da = aperture - listener;
        const float hu = dot(dh, u), hv = dot(dh, v);
        const float au = dot(da, u), av = dot(da, v);
        const float cu = 0.5f * (hu + au), cv = 0.5f * (hv + av);
        // 両端を含む半径。開口が遠いほど広く見る（衝立のように縁が遠い場合）。
        const float half = 0.5f * std::sqrt((au - hu) * (au - hu) + (av - hv) * (av - hv));
        const float radius = std::max(apertureOpenRadius_, half + apertureOpenRadius_ * 0.5f);

        // 遮蔽物を抜けた先へ送る。手前だと板の前で判定してしまう。
        const Vec3 base = hit.point + axis * kOpenClearance
                        + u * (cu - dot(dh, u)) + v * (cv - dot(dh, v));

        // フィボナッチ円盤（決定的）。乱数だとフレームごとにパターンが変わって
        // 割合がちらつく ── computeSoftOcclusion と同じ理由で固定配置にする。
        constexpr float kGolden = 2.39996323f;
        int open = 0;
        for (int i = 0; i < n; ++i) {
            const float rr = radius * std::sqrt((i + 0.5f) / n);
            const float aa = kGolden * i;
            const Vec3 p = base + u * (rr * std::cos(aa)) + v * (rr * std::sin(aa));
            if (!isOccluded(listener, p)) ++open;
        }
        return static_cast<float>(open) / static_cast<float>(n);
    }
    // 断面を遮蔽物のどれだけ先に置くか(m)。板の厚みを確実に越える距離。
    static constexpr float kOpenClearance = 0.5f;

    // 開口率 → 広帯域ゲイン(0..1)。
    //   apertureOpenRef_ は「これだけ開いていれば素通しとみなす」割合。
    //     円盤は戸口の周りの壁も含むので、全開の戸口でも割合は 1 にならない。
    //     戸口 1.2m を半径 1m の円盤で見ると 3 割前後。既定はその見当。
    //   apertureOpenPower_ はカーブ。1.0 = 割合に比例（開き始めから素直に増える）。
    //   スカラで返すのは、回折が運ぶ情報を定位に絞る方針のため（LPF は透過が担当）。
    float apertureOpenGain(float frac) const {
        if (apertureOpenRef_ <= 1e-4f) return 1.0f;    // 0 で無効化
        const float r = frac / apertureOpenRef_;
        if (r >= 1.0f) return 1.0f;
        if (r <= 0.0f) return 0.0f;
        if (apertureOpenPower_ == 1.0f) return r;
        if (apertureOpenPower_ <= 0.0f) return 1.0f;
        return std::pow(r, apertureOpenPower_);
    }

    void setApertureOpenRef(float f) { apertureOpenRef_ = f; }
    float apertureOpenRef() const { return apertureOpenRef_; }
    void setApertureOpenPower(float p) { apertureOpenPower_ = p; }
    float apertureOpenPower() const { return apertureOpenPower_; }
    void setApertureOpenRadius(float m) { apertureOpenRadius_ = m; }
    float apertureOpenRadius() const { return apertureOpenRadius_; }
    void setApertureOpenSamples(int n) { apertureOpenSamples_ = n; }
    int apertureOpenSamples() const { return apertureOpenSamples_; }

    // ================================================================ 回折（キルヒホッフ）
    // 開口の「大きさ」を測り、フレネル・キルヒホッフの解析解に渡す。
    //
    //   前川の式は δ（迂回の余剰長）だけの関数なので、**開口の幅に反応できない**。
    //   扉が回っても回折経路が回る戸口の枠は動かないため δ が変わらず、
    //   「扉がどのくらい開いたか」が piecewise constant になっていた（実測: 0〜6°で完全に平坦、
    //   6.5°で 0.234 跳ぶ）。作品のコンセプトの中核がモデルの構造で表現できていない状態だった。
    //
    //   ここでは元の法則に戻る。音源と受音点の間に面を置き、**その面のうち塞がれていない範囲**を
    //   実測して、矩形開口の解析解（kirchhoff.h）に渡す。開口の幅・扉の開き具合・周波数依存が
    //   すべて同じ式から出る。閾値も場合分けも要らない。
    //
    //   戻り値 = 開口が見つかれば true。outAperture に開口中心（定位に使う）。
    bool diffractionKirchhoff(const Vec3& listener, const Vec3& source,
                              float outGain[kNumBands], Vec3& outAperture,
                              float& outPathLength) const {
        const Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return false;
        const Vec3 n = axis * (1.0f / dist);

        // ── 面の位置 ──
        //   遮蔽されているなら最初に当たった所（そこが音を止めている面）。
        //   遮蔽が無いなら中点（第1フレネルゾーンが最も広く、遮蔽の影響が最も大きい位置）。
        float d1 = dist * 0.5f;
        const SceneHit hit = raycastClosest(listener, n, dist);
        if (hit.hit) d1 = hit.t;
        d1 = std::max(std::min(d1, dist - 0.05f), 0.05f);
        const float d2 = dist - d1;
        const Vec3 planeOrigin = listener + n * d1;

        // 面上の直交基底。
        Vec3 u = cross(n, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(n, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(n, u));

        // 面上の点が「開いている」か＝音源と受音点の双方から見通せるか。
        auto openAt = [&](float su, float sv) {
            const Vec3 P = planeOrigin + u * su + v * sv;
            return !isOccluded(listener, P) && !isOccluded(P, source);
        };

        // 探索範囲は第1フレネルゾーンの数倍（最低域基準）。そこから外は寄与が打ち消し合う。
        const float D = (d1 * d2) / std::max(d1 + d2, 1e-6f);
        const float lambdaLow = kirchhoff::kSpeed / kirchhoff::kBandFreq[0];
        const float searchR = 3.0f * std::sqrt(lambdaLow * D);

        // ── 開口の種を集める ──
        //   ★等間隔に探すと小さな隙間を見逃す。扉が 6° 開いたときの隙間は 6mm しかなく、
        //     フレネルゾーン数メートルを 6mm 刻みで探すのは非現実的（1万点規模）。
        //     そこで**稜線探索を種に使う** ── 稜線は開口の縁そのものなので、その近傍を見れば
        //     どんなに細い隙間でも捉えられる。「稜線がどこかを教え、積分がどれだけかを決める」。
        constexpr int kMaxSeed = 24;
        float seedU[kMaxSeed], seedV[kMaxSeed];
        int nseed = 0;
        auto pushSeed = [&](float a, float b) {
            if (nseed < kMaxSeed) { seedU[nseed] = a; seedV[nseed] = b; ++nseed; }
        };
        pushSeed(0.0f, 0.0f);                       // 軸上（見通せているならここが開口）
        forEachDiffractionCandidate(listener, source,
            [&](const Vec3& P, float, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
                const Vec3 rel = P - planeOrigin;
                pushSeed(dot(rel, u), dot(rel, v));  // 稜線上の点を面へ射影
            }, 4.0f);

        // 近すぎる種はまとめる（同じ開口を何度も測らない）。
        for (int i = 0; i < nseed; ++i)
            for (int j = i + 1; j < nseed; ) {
                const float du2 = seedU[i] - seedU[j], dv2 = seedV[i] - seedV[j];
                if (du2 * du2 + dv2 * dv2 < 0.04f) {   // 20cm 以内は同じ開口とみなす
                    seedU[j] = seedU[nseed - 1]; seedV[j] = seedV[nseed - 1]; --nseed;
                } else ++j;
            }

        // 稜線上の点は境界そのものなので、少しずらして開いている側を探す。
        //   ★**最初に開いた種で決めてはいけない**。順序依存になり、扉の隙間ではなく
        //     壁の外側や上を拾ってしまう（実際にそうなった）。全部の種で開口を測り、
        //     いちばんよく通る開口を採る。
        auto seedOpen = [&](float bu, float bv, float& ou, float& ov) {
            const float nudge[3] = {0.0f, 0.02f, 0.10f};
            const float dirU[5] = {0.0f, 1.0f, -1.0f, 0.0f, 0.0f};
            const float dirV[5] = {0.0f, 0.0f, 0.0f, 1.0f, -1.0f};
            for (int e = 0; e < 3; ++e)
                for (int d = 0; d < 5; ++d) {
                    const float cu = bu + dirU[d] * nudge[e];
                    const float cv = bv + dirV[d] * nudge[e];
                    if (openAt(cu, cv)) { ou = cu; ov = cv; return true; }
                }
            return false;
        };

        // ── 開口の広がりを測る ──
        //   種から4方向へ進み、塞がれる位置を二分で詰める。開口の縁が連続に動くので、
        //   扉が回れば範囲も連続に動く ── ここが「どのくらい開いたか」の実体。
        //   ★刻んで進み、**最初に塞がれた所**で止める。
        //     「端が開いていれば全開」と早期判定してはいけない ── 開いた領域は非凸で、
        //     壁の上から下へ進むと壁を貫通して反対側の開いた領域に出てしまう。
        //     それを全開と誤判定すると、閉じた扉でもゲインが 1.0 になる（実際にそうなった）。
        auto extent = [&](float su, float sv, float du, float dv) {
            constexpr int kStep = 16;
            float lastOpen = 0.0f;
            for (int i = 1; i <= kStep; ++i) {
                const float r = searchR * (i / float(kStep));
                if (!openAt(su + du * r, sv + dv * r)) {
                    // lastOpen(開) と r(閉) の間に縁がある。二分で詰める。
                    float lo = lastOpen, hi = r;
                    for (int j = 0; j < 10; ++j) {
                        const float m = 0.5f * (lo + hi);
                        if (openAt(su + du * m, sv + dv * m)) lo = m; else hi = m;
                    }
                    return lo;
                }
                lastOpen = r;
            }
            return searchR;   // 探索範囲の端まで開いている
        };

        // 種ごとに開口を測り、いちばんよく通るものを採る。
        //   ※複数開口の複素合成は初版では行わない（支配開口の近似）。
        bool any = false;
        float bestScore = -1.0f;
        for (int s = 0; s < nseed; ++s) {
            float su, sv;
            if (!seedOpen(seedU[s], seedV[s], su, sv)) continue;
            const float u2 = su + extent(su, sv, 1, 0), u1 = su - extent(su, sv, -1, 0);
            const float v2 = sv + extent(su, sv, 0, 1), v1 = sv - extent(su, sv, 0, -1);
            float g[kNumBands];
            kirchhoff::apertureGain(u1, u2, v1, v2, d1, d2, g);
            // 低域加重で「どれだけ通るか」を1スカラに（回折は低域が回り込む）。
            const float wgt[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
            float gs = 0.0f, ws = 0.0f;
            for (int b = 0; b < kNumBands; ++b) { gs += wgt[b] * g[b]; ws += wgt[b]; }
            const float score = gs / ws;
            if (score > bestScore) {
                bestScore = score;
                for (int b = 0; b < kNumBands; ++b) outGain[b] = g[b];
                const float cu = 0.5f * (u1 + u2), cv = 0.5f * (v1 + v2);
                outAperture = planeOrigin + u * cu + v * cv;
                outPathLength = length(outAperture - listener) + length(source - outAperture);
                any = true;
            }
        }
        return any;
    }

    // ================================================================ 回折（一本化）
    // docs/DIFFRACTION_DESIGN.md §2。**回折の探索はここ 1 箇所だけ**。
    //
    //   以前は「ゲイン用」と「方向用」が別々に稜線を歩き、別々の重み・別々の候補条件を
    //   持っていた。そのせいで「ゲインは回折を見ているのに開口は 0 本」という不整合が起き、
    //   見通しが開通した瞬間に定位が消えていた（設計 §7-1 の実測）。
    //
    //   遮蔽時   : 開口ごとの経路。回り込んで届く成分を、開口の方向から鳴らす。
    //   見通し時 : 1 本だけ返す。aperture = 音源位置、gain = 開口によるフレネル補正。
    //              直接音は音源方向から来るので、方向は音源そのもの。補正だけが意味を持つ。
    struct DiffractionPath {
        Vec3  aperture{0, 0, 0};    // 音が抜けてくる点（定位に使う）
        float pathLength = 0.0f;    // listener → aperture → source の実長（遅延・距離減衰）
        // 符号付きの迂回余剰長（影で正・境界で 0・照射側で負）。**ゲインはこれから出す。**
        //   aperture はクラスタの重心なので、経路長から δ を逆算してはいけない
        //   ── 重心はクラスタの構成が変わると跳ぶので、ゲインが不連続になる（実測で
        //   扉の掃引が 4kHz で 0.474→0.132→0.495 とジグザグした）。
        //   δ 自体は候補の min なので連続。
        float delta = 0.0f;
        // 開口の開き具合による広帯域の抑制(0..1)。閉まっているほど小さい。
        //   gain には既に掛けてある。距離減衰だけで鳴らす側（回折の周波数依存を捨てる設定）は
        //   これを別途掛ける必要があるので、単体でも取れるようにしてある。
        float openGain = 1.0f;
        float slitWidth = 0.0f;   // 実測の隙間幅(m)。診断用（ゲインは openGain に入っている）
        // 隙間の幅による帯域ごとの通りやすさ。開くほど低い帯域が入ってくる。
        float openBand[kNumBands] = {1, 1, 1, 1, 1, 1};
        // 開口の**広がり**を表す実在の点（ホイヘンス）。開口を点1つで鳴らすと戸口が
        // ピンポイントに聞こえる ── 現実の開口は面全体が二次音源として光る。
        // spread[0] は aperture と同じ。以降は開口の端の方へ散らした点。
        static constexpr int kMaxSpread = 3;
        Vec3 spread[kMaxSpread];
        int nSpread = 1;
        float gain[kNumBands] = {0, 0, 0, 0, 0, 0};
    };

    //   order: 探索する回折の次数。1=1次のみ / 2=1次で届かなければ開口を新しい始点に再帰。
    //     L 字の廊下や、壁が消えてできた新しい通路が 2 次に当たる。
    //     1 次で届けば 2 次は探索しないので、開けた場所ではコストが増えない。
    int findDiffractionPaths(const Vec3& listener, const Vec3& source,
                             DiffractionPath* out, int maxPaths, int order = 2) const {
        if (!out || maxPaths <= 0) return 0;
        const bool occ = isOccluded(listener, source);
        const float directDist = std::max(length(source - listener), 1e-4f);

        // ★ブロッカーは「直接線が実際に交差する箱」だけにする（膨張なし）。
        //
        //   一時 margin=4m を常用したが、**閉じた部屋では床・天井・側壁まで
        //   ブロッカーに入ってしまい**、開口が1つしかないのに候補が3本立った。
        //   偽の候補どうしで支配クラスタが入れ替わり、経路長が 13.8m ⇄ 19.8m と
        //   6m も飛んでいた（距離減衰だけで鳴らすと、そのまま音量と遅延の飛びになる）。
        //   コストも 1.4ms → 10.6ms/frame に悪化した。
        //
        //   一方 margin=0 にすると、見通し側で候補が消えてフレネル補正が効かなくなり、
        //   影境界のゲインが 0.033 → 0.438 に悪化した。
        //   補正が効く範囲は δ < 0.1λ 程度（125Hz で 0.28m）なので、**1m あれば足りる**。
        //   閉じた部屋でも 1m の膨張なら床・天井・側壁は直接線と交差しない。
        constexpr float margin = 1.0f;

        // 開口＝方向クラスタ。掠める点はクラスタ内の重み付き重心にして連続にスライドさせる
        // （手前稜線↔奥稜線の乗り換えで重心が滑らかに移る＝飛ばない）。
        //
        //   重みは**上限で頭打ちにしない**前川値（maekawa::apertureWeight）。
        //   出力ゲインには 24dB の上限があるが、重みに上限をかけると遠い開口が全部同点になり、
        //   支配開口が薄まって**定位がぼやける**。本作で優先するのは遮蔽量の精度ではなく
        //   回折点への定位なので、優劣がはっきり付く方を採る。
        constexpr int kMaxCl = 24;
        // 開口点は「重心に最も近い**実在の候補点**」にする（重心そのものは使わない）。
        //
        //   重心 pAcc/w はどの稜線の上にも、どの隙間の中にも無い架空の点で、扉の左右の
        //   稜線が 1 クラスタになると板の中や板を通り過ぎた先の空間へ出てしまう。
        //   すると開口幅が扉の隙間ではなく部屋の差し渡しを測り（実測: 閉扉で 3.03m）、
        //   逆に板の中に落ちれば幅 0 として開いている扉まで黙らせる（実測: 15〜65° が 0）。
        //   かといって δ 最小の候補を採ると外れ値を拾う（実測: 開口が y=42 や x=-20 を指した）。
        //   → 重心の安定性はそのままに、点だけ実在のものへ寄せる。
        //   候補は数個あれば足りる（同じクラスタ内は方向 25° 以内なので密）。
        constexpr int kClPts = 8;
        struct Cl {
            Vec3 pAcc; Vec3 dir; float w; float minDelta;
            Vec3 pts[kClPts]; int npts;
            // 各候補点が乗っていた稜線の向き。開口点を選んだ後、**その点の稜線**で
            // 隙間幅を測るために要る。クラスタ内で max/min を採ると、別の稜線が
            // 測った部屋の差し渡しに上書きされて隙間が見えなくなる（実測でそうなった）。
            Vec3 edges[kClPts];
            // BTM 用。稜線の**端点**と 0 面接線。BTM は有限であることを使うので
            // 方向だけでは足りない（前川との本質的な違いがここ）。
            Vec3 eA[kClPts], eB[kClPts], eRefT[kClPts];
            int inst;   // 稜線が属する箱。「同じ開口か」を判定するのに使う
            // 開口の**幾何的な中心**用の蓄積。重みは掠めの深さ(edgeW)だけにする。
            //   pAcc 側は apertureWeight(δ) 重みなのでリスナー側の縁へ強く寄るが、
            //   こちらはほぼ平ら＝開口の真ん中を指す。
            //   ★単純平均にしてはいけない。新しい点が満額で入って中心が飛ぶ
            //     （実測: 経路長の隣接差が 1.38m → 1.76m に悪化した）。
            //     edgeW は生まれる瞬間 0 なので、重みに使えば連続に入る。
            Vec3 gAcc; float gw;
        };
        Cl cl[kMaxCl];
        int ncl = 0;
        const float cosThresh = 0.90f;   // ~25°以内は同じ開口とみなす（設計 §6-1）
        float globalMinDelta = -1.0f;

        // 2 つのクラスタが**同じ開口**を囲っているか。扉や戸口を名指ししない一般の判定。
        //   ① 稜線の乗る箱が同一平面（同じ仕切りの一部。板の最も薄い軸を法線とする）
        //   ② 代表点どうしの間に遮るものが無い（＝その間が開口そのもの）
        //   閉じた扉があれば ② が成立しないので、まとめられない ── これは正しい。
        //   その場合そもそも通り道が無いので、タップも立たない。
        auto sameApertureCluster = [&](const Cl& a, const Cl& b) {
            if (a.inst < 0 || b.inst < 0
                || a.inst >= instanceCount() || b.inst >= instanceCount()) return false;
            const Obb& oa = instances_[static_cast<std::size_t>(a.inst)].obb;
            const Obb& ob = instances_[static_cast<std::size_t>(b.inst)].obb;
            auto thinN = [](const Obb& o) {
                const float hx = o.halfExtents.x, hy = o.halfExtents.y, hz = o.halfExtents.z;
                Vec3 nn = o.axisZ;
                if (hx <= hy && hx <= hz) nn = o.axisX;
                else if (hy <= hx && hy <= hz) nn = o.axisY;
                return (length(nn) > 1e-4f) ? normalized(nn) : Vec3(0, 0, 1);
            };
            const Vec3 na = thinN(oa), nb = thinN(ob);
            if (std::fabs(dot(na, nb)) < 0.999f) return false;                   // ① 向きが違う
            if (std::fabs(dot(na, oa.center - ob.center)) > 0.05f) return false; // ① 別の平面
            if (a.npts <= 0 || b.npts <= 0) return false;
            return !isOccluded(a.pts[0], b.pts[0]);                              // ② 間が開いている
        };

        auto gather = [&](bool bothEnds) {
            ncl = 0;
            globalMinDelta = -1.0f;
            const Vec3 travelDir = normalized(source - listener);
            forEachDiffractionCandidate(listener, source,
                [&](const Vec3& P, float d, const Vec3& edgeDir, const Vec3& refT,
                    const Vec3& eA, const Vec3& eB, float edgeW, int inst) {
                    if (globalMinDelta < 0.0f || d < globalMinDelta) globalMinDelta = d;
                    const float w = maekawa::apertureWeight(occ ? d : -d);
                    if (w < 1e-6f) return;
                    const float sw = slitWidthAt(P, edgeDir, travelDir);
                    const Vec3 dir = normalized(P - listener);
                    int best = -1; float bestDot = cosThresh;
                    for (int i = 0; i < ncl; ++i) {
                        const float dt = dot(dir, cl[i].dir);
                        if (dt > bestDot) { bestDot = dt; best = i; }
                    }
                    if (best >= 0) {
                        cl[best].pAcc = cl[best].pAcc + P * w;
                        cl[best].gAcc = cl[best].gAcc + P * edgeW;
                        cl[best].gw += edgeW;
                        cl[best].dir = normalized(cl[best].dir * cl[best].w + dir * w);
                        cl[best].w += w;
                        if (d < cl[best].minDelta) cl[best].minDelta = d;
                        if (cl[best].npts < kClPts) {
                            const int k = cl[best].npts;
                            cl[best].edges[k] = edgeDir;
                            cl[best].eA[k] = eA; cl[best].eB[k] = eB; cl[best].eRefT[k] = refT;
                            cl[best].pts[cl[best].npts++] = P;
                        }
                    } else if (ncl < kMaxCl) {
                        cl[ncl].pAcc = P * w;
                        cl[ncl].gAcc = P * edgeW; cl[ncl].gw = edgeW;
                        cl[ncl].dir = dir; cl[ncl].w = w; cl[ncl].minDelta = d;
                        cl[ncl].pts[0] = P; cl[ncl].edges[0] = edgeDir;
                        cl[ncl].eA[0] = eA; cl[ncl].eB[0] = eB; cl[ncl].eRefT[0] = refT;
                        cl[ncl].npts = 1;
                        cl[ncl].inst = inst;
                        ++ncl;
                    }
                }, margin, bothEnds);

            // ★**同じ開口を囲う稜線どうし**は 1 本にまとめる。
            //
            //   方向 25° で分けているので、戸口を横から覗くと左右の縁が別クラスタになる。
            //   すると同じ乾いた信号のコピーが**違う遅延で 2 本**鳴る。回折タップは平坦
            //   （diffractionDistanceOnly）なので全帯域で同相に効き、櫛形フィルタになる。
            //   実測（Test_DiffractionGap と同形状）:
            //     x=2.0 で経路差 3.18m → 第1ノッチ 54Hz（以降 162/270/378Hz …）
            //     x=5.0 で経路差 0.90m → 第1ノッチ 192Hz
            //   低域から低中域にノッチが並ぶので「ドラムが削れて低音が壊れる」と聞こえる。
            //   ★「開口を複数点へ散らして埋める」は**逆効果**だった（実測: 等間隔なので
            //     周期的なノッチが深くなり、3点で -27.4dB、5点で -28.3dB）。まとめる方が正しい。
            for (int i = 0; i < ncl; ++i) {
                for (int j = ncl - 1; j > i; --j) {
                    if (!sameApertureCluster(cl[i], cl[j])) continue;
                    // j を i へ畳む。重みつきの量は足し、点は入るだけ取り込む。
                    cl[i].pAcc = cl[i].pAcc + cl[j].pAcc;
                    cl[i].gAcc = cl[i].gAcc + cl[j].gAcc;
                    cl[i].gw += cl[j].gw;
                    cl[i].dir = normalized(cl[i].dir * cl[i].w + cl[j].dir * cl[j].w);
                    cl[i].w += cl[j].w;
                    cl[i].minDelta = std::min(cl[i].minDelta, cl[j].minDelta);
                    for (int k = 0; k < cl[j].npts && cl[i].npts < kClPts; ++k) {
                        const int m = cl[i].npts;
                        cl[i].pts[m] = cl[j].pts[k];     cl[i].edges[m] = cl[j].edges[k];
                        cl[i].eA[m] = cl[j].eA[k];       cl[i].eB[m] = cl[j].eB[k];
                        cl[i].eRefT[m] = cl[j].eRefT[k];
                        ++cl[i].npts;
                    }
                    cl[j] = cl[ncl - 1];
                    --ncl;
                }
            }
        };

        // まずは 1 次として探す（回折点が listener と source の両方から見通せるもの）。
        gather(true);

        // ★1 本も無いときだけ、2 次を試す。
        //   このとき「source からも見通せる」条件を外す ── 真の 2 次回折では、どの稜線も
        //   両方からは見通せない（それがまさに 2 次でしか届かないということ）ので、
        //   条件を付けたままだと候補が 0 本になり、再帰に入る前にループが空になる。
        //   一方、1 次が見つかっているときに緩めてはいけない。source 側の貫通判定が外れ、
        //   壁を突き抜ける経路が開口として混ざる（実測で「開口側を指す」テストが落ちた）。
        bool secondOrder = false;
        if (ncl == 0 && order > 1 && occ) {
            gather(false);
            secondOrder = true;
        }

        if (ncl == 0) return 0;   // 回折の相手が無い。呼び出し側で「遮蔽なら0/見通しなら1.0」を決める

        // 見通しているときは、回折は「直接経路への補正」であって別経路ではない。
        //   別経路として複数返してエネルギー加算すると 1.0 を超えて過大計上になる（設計 §5-6）。
        //   方向も音源そのものなので、1 本にまとめて返す。
        if (!occ) {
            out[0] = DiffractionPath{};
            out[0].aperture = source;
            out[0].pathLength = directDist;
            out[0].delta = -globalMinDelta;
            maekawa::gainBands(out[0].delta, out[0].gain);
            // ★見通せている側にも同じ開口の制限を掛ける。
            //   掛けないと、影境界を跨いだ瞬間にゲートが消えて音量が跳ぶ
            //   （実測: 遮蔽側 0.285 → 見通し側 0.563）。物理的にも、境界のすぐ外では
            //   フレネルゾーンはまだ半分壁に隠れているので、制限が続くのが正しい。
            //   遠ざかれば開口率は 1 に近づき、自然に制限が消える。
            float fr[kNumBands];
            if (useFresnelAperture_ && apertureFresnelBands(listener, source, fr)) {
                for (int b = 0; b < kNumBands; ++b) {
                    out[0].gain[b] *= fr[b];
                    out[0].openBand[b] = fr[b];
                }
                static const float w6[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
                float gs = 0.0f, ws = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { gs += w6[b] * fr[b]; ws += w6[b]; }
                const float raw = (ws > 0.0f) ? gs / ws : 1.0f;
                // ★遮蔽側と**同じ写像**を通す。ここを生のままにしていたせいで、
                //   影境界の両側で別の量を計算していた（実測で 0.562 → 0.334 の段差）。
                //   コメントは元から「見通せている側にも同じ開口の制限を掛ける」と
                //   書いてあり、意図は正しかった。実装が片側だけ通していなかった。
                out[0].openGain = raw * apertureContrastMap(raw);
            }
            return 1;
        }

        // ★ポータルがあるなら、稜線探索ではなく**ポータルから経路を作る**。
        //   探索が無いので、探索由来の不連続（経路が生まれる/消える）が起きない。
        //   方向（開いている部分の重心）とエネルギー（開口率）が同じ積分から出るので、
        //   両者がずれて飛ぶこともない。ここが今日ずっと戦っていた問題の解。
        //   ポータルが1枚も反応しなければ、下の稜線探索へ落ちる（穴を空けない）。
        int portalPaths = 0;          // ポータルが埋めた本数。稜線探索はこの続きから書く
        if (!portals_.empty()) {
            int np2 = 0;
            for (const Portal& pt : portals_) {
                if (np2 >= maxPaths) break;
                if (!pt.active) continue;
                float f[kNumBands]; Vec3 cp(0, 0, 0);
                if (!portalCoupling(pt, listener, source, f, &cp)) continue;
                // 低域加重の広帯域スカラ（他の経路と同じ畳み方）。
                static const float w6[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
                float gs = 0.0f, ws = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { gs += w6[b] * f[b]; ws += w6[b]; }
                const float bb = (ws > 0.0f) ? gs / ws : 0.0f;
                if (bb < 1e-5f) continue;          // 完全に塞がっている＝鳴らさない
                DiffractionPath& p = out[np2];
                p.aperture = cp;
                p.spread[0] = cp; p.nSpread = 1;
                p.pathLength = length(cp - listener) + length(source - cp);
                p.delta = std::max(p.pathLength - directDist, 0.0f);
                p.openGain = bb;
                p.slitWidth = 0.0f;
                const float cmul = apertureContrastMap(bb);
                for (int b = 0; b < kNumBands; ++b) {
                    // ★前川の δ 減衰は掛けない。開口を素通りする音は曲がっていない。
                    //   掛けると二重になる（実測で 1kHz に 4倍の差が出た）。
                    p.gain[b] = f[b] * cmul;
                    p.openBand[b] = f[b];
                }
                ++np2;
            }
            portalPaths = np2;
        }

        // ★ポータルは**自分が覆う開口だけ**を担当する。覆っていない開口は稜線探索が担当。
        //
        //   以前は「ポータルが 1 枚でもあれば稜線探索を丸ごと迂回」していた。
        //   理由は「閉じたポータルのとき旧経路が走って幻を鳴らす」ことの回避だったが、
        //   副作用が大きすぎた。実測（開口 2 つの仕切りで、片方にだけポータルを置く）:
        //     ポータル無し          回折125Hz 0.3162
        //     関係ない開口にポータル 回折125Hz 0.0021   ← **43dB 減**
        //   リスナーの目の前にある開口が担当から外れ、遠いポータルだけが答えを出していた。
        //   ホストがポータルを 1 枚置いた瞬間に他の開口が全部黙る、という挙動になる。
        //
        //   元の懸念（閉じたポータルの所で幻が出る）は、**覆っている開口のクラスタを
        //   落とす**ことで保たれる ── 閉じた扉の戸口はポータルが覆っているので、
        //   そこのクラスタは稜線探索側から除かれる。丸ごと迂回するより範囲が狭い。
        //   さらに幻そのものは跨ぎ判定で消えているので、当時より条件も良い。
        //   判定は「開口点が矩形の中にあるか」ではなく「**その経路がポータルを通るか**」。
        //   点で見ると、扉が大きく開いたとき扉パネル自体の縁が矩形の外に出て
        //   覆い判定を外れ、別タップが立つ（実測: 81°で合計が 0.386 → 0.619 に跳んだ。
        //   ポータル開口率は 0.7235 → 0.7361 と滑らかなので、経路が増えたことが原因）。
        //   経路で見れば「この音はその戸口を通ってくる」かどうかを直接問える。
        auto segmentCrossesPortal = [&](const Vec3& a, const Vec3& b, const Portal& pt) {
            const Vec3 n2 = normalized(cross(pt.axisU, pt.axisV));
            const Vec3 d = b - a;
            const float den = dot(n2, d);
            if (std::fabs(den) < 1e-6f) return false;
            const float t = dot(n2, pt.center - a) / den;
            if (t < -0.05f || t > 1.05f) return false;
            const Vec3 hit = a + d * t;
            const Vec3 r = hit - pt.center;
            return std::fabs(dot(r, pt.axisU)) <= pt.halfU + 0.25f
                && std::fabs(dot(r, pt.axisV)) <= pt.halfV + 0.25f;
        };
        auto coveredByPortal = [&](const Vec3& p) {
            for (const Portal& pt : portals_) {
                if (!pt.active) continue;
                // 開口点そのものが矩形の中／面の近くにある
                const Vec3 n2 = normalized(cross(pt.axisU, pt.axisV));
                const Vec3 d = p - pt.center;
                if (std::fabs(dot(d, n2)) <= 0.5f
                    && std::fabs(dot(d, pt.axisU)) <= pt.halfU + 0.25f
                    && std::fabs(dot(d, pt.axisV)) <= pt.halfV + 0.25f) return true;
                // または、その開口点を経由する経路がポータルを通る
                if (segmentCrossesPortal(listener, p, pt)) return true;
                if (segmentCrossesPortal(p, source, pt)) return true;
            }
            return false;
        };

        // 遮蔽時は開口ごとに独立した経路。重み上位から maxPaths 本。
        bool used[kMaxCl] = {false};
        int n = portalPaths;
        int taken = 0;
        while (n < maxPaths && taken < ncl) {
            ++taken;
            int bi = -1; float bw = -1.0f;
            for (int i = 0; i < ncl; ++i) if (!used[i] && cl[i].w > bw) { bw = cl[i].w; bi = i; }
            if (bi < 0) break;
            used[bi] = true;
            // 重心を出し、そこへ最も近い実在候補へスナップする（上の Cl のコメント参照）。
            // ★鳴らす点は「**開口の中心とリスナー側の点の中間**」に置く。
            //
            //   重み付き重心 pAcc/w は apertureWeight(δ) 重みなので、迂回の少ない
            //   **リスナー側の縁**へ強く寄る。横から覗くと戸口の端に張り付き、
            //   「開口から鳴っている」という感じが薄れる。
            //   一方、edgeW 重みの重心は開口の幾何的な中心で、リスナーが動いても
            //   ほとんど動かない（＝どこから聞いても真ん中から鳴る）。
            //   その中間を採ると、正面では中心・横から覗くと縁寄り、と連続に移る。
            //   どちらも連続量なので、中間も連続。
            const Vec3 wCentroid = cl[bi].pAcc * (1.0f / std::max(cl[bi].w, 1e-6f));
            const Vec3 geoCenter = (cl[bi].gw > 1e-6f)
                                 ? cl[bi].gAcc * (1.0f / cl[bi].gw) : wCentroid;
            const Vec3 centroid = (geoCenter + wCentroid) * 0.5f;
            // ★この開口をポータルが覆っているなら、担当はポータル。稜線探索側は降りる。
            //   両方が鳴らすと同じ開口が 2 タップになり、平坦なコピーが違う遅延で
            //   足されて櫛形フィルタになる（クラスタ統合で潰したのと同じ現象）。
            if (portalPaths > 0 && coveredByPortal(centroid)) continue;
            Vec3 Pc = centroid;
            Vec3 PcEdge(0, 1, 0);
            Vec3 PcRefT(1, 0, 0);   // 面の接線（⊥稜線・外向き）。2次の送り向きに使う
            {
                float bd = -1.0f;
                for (int k = 0; k < cl[bi].npts; ++k) {
                    const Vec3 v = cl[bi].pts[k] - centroid;
                    const float dd = dot(v, v);
                    if (bd < 0.0f || dd < bd) {
                        bd = dd; Pc = cl[bi].pts[k]; PcEdge = cl[bi].edges[k];
                        PcRefT = cl[bi].eRefT[k];
                    }
                }
            }

            // この開口がどれだけ開いているか。**回折の音量はこれが決める**（稜線探索は定位担当）。
            //   前川の式は δ しか見ないので、1.8cm の隙間でも 1.8m の開口でも同じ値を返す。
            //
            // ★フレネルゾーンは**この開口の上**で測る。経路ごとに開口が違うので、
            //   ループの外へ括り出すことはできない（括ると全開口が同じ値になり、
            //   測っているものが「戸口とゾーンの面積比」という固定値に化ける）。
            //   計算量は開口の本数ぶん増えるが、回折段は実測 0.26ms なので許容範囲。
            // ★窓の中心に Pc（稜線由来の開口点）を渡してはいけない。Pc は跳ぶので
            //   （実測 3m）、窓ごと飛んで値が乱高下する。窓は測定の基準ではなく
            //   積分の範囲なので、**連続に動く点**（直線と平面の交点）に据える。
            float fres[kNumBands];
            const bool haveFres = useFresnelAperture_
                                && apertureFresnelBands(listener, source, fres);
            float openGainHere = 1.0f;
            float slitHere = 0.0f;                 // 診断用。フレネル使用時は測らない

            // ★BTM 経路。前川＋開口積分ではなく、**有限稜線の積分**で帯域ゲインを出す。
            //   開口という量を別に持たないので、「面のうちどこまで見るか」という
            //   （4通り試して全部別の理由で失敗した）問いが発生しない。
            bool btmDone = false;
            float btmBands[kNumBands] = {0, 0, 0, 0, 0, 0};
            if (useBtm_) {
                // ★稜線を1本**選ばない**。この開口に属する稜線すべての寄与を複素で足す。
                //   選ぶ方式だと、角度によって選択が別の稜線へ切り替わった瞬間に値が飛ぶ
                //   （実測: 扉の掃引で 0.0433 → 0.0154 と隣接で 9dB 跳ねた）。
                //   前川の値で均されていたときは目立たなかったものが、BTM にして露出した。
                //   選ぶのをやめれば、選択が切り替わるという事象自体が消える。
                //   戸口を「稜線の集合」として扱うのが BTM 本来の使い方でもある。
                static const double hz[kNumBands] = {125, 250, 500, 1000, 2000, 4000};
                double sre[kNumBands] = {0}, sim[kNumBands] = {0};
                int nOk = 0;
                for (int k = 0; k < cl[bi].npts; ++k) {
                    btm::Edge be;
                    be.a = cl[bi].eA[k];
                    be.b = cl[bi].eB[k];
                    be.faceDir0 = cl[bi].eRefT[k];
                    be.openAngleRad = btmWedgeAngle_;   // 既定 1.5π（箱の凸稜線）
                    if (length(be.b - be.a) < 1e-4f) continue;
                    float g1[kNumBands] = {0, 0, 0, 0, 0, 0};
                    double re1[kNumBands] = {0}, im1[kNumBands] = {0};
                    if (!btm::edgeBands(be, source, listener, hz, kNumBands, 343.0,
                                        g1, nullptr, 96, re1, im1)) continue;
                    for (int b = 0; b < kNumBands; ++b) { sre[b] += re1[b]; sim[b] += im1[b]; }
                    ++nOk;
                }
                if (nOk > 0) {
                    for (int b = 0; b < kNumBands; ++b)
                        btmBands[b] = static_cast<float>(
                            std::sqrt(sre[b] * sre[b] + sim[b] * sim[b]));
                    btmDone = true;
                }
            }
            if (haveFres) {
                // 広帯域スカラは低域加重で畳む（回折が運ぶのは定位と距離、という方針のため）。
                static const float w6[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
                float gs = 0.0f, ws = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { gs += w6[b] * fres[b]; ws += w6[b]; }
                openGainHere = (ws > 0.0f) ? gs / ws : 1.0f;
                // ★コントラスト。開口率の**形**は物理から出ているが、**量**が足りない
                //   （実測で扉の全掃引が 1.3dB。現実の合成透過率は 125Hz で 11dB, 4kHz で 26dB）。
                //   形は残したまま幅だけを開く。開口という一般の量への写像なので、
                //   扉を特別視しない。絶対値は演出で決める、という方針の適用先がここ。
                //   ★単に累乗してはいけない。開口率は 1 未満なので全体が縮むだけで、
                //     実測で全開が 0.0209 → 0.0147（閉と同じ）まで落ちた。
                //     基準 ref を決めて**その周りで**開くこと。ref では値が変わらない。
                //   ★基準 ref で 1.0 になるよう正規化してから累乗する。
                //     「ref のまわりで開く」形（ref*(x/ref)^p）も試したが、開口率が
                //     ref に届かないので全開まで縮み、実測で 0.0209 → 0.0152 になった。
                //     素通しの上限を全開に合わせないと、幅を開いても音が小さくなるだけ。
                if (apertureContrast_ != 1.0f) {
                    const float ref = apertureContrastRef_;   // ここで素通し(1.0)になる開口率
                    const float x = std::max(openGainHere, 1e-4f) / std::max(ref, 1e-4f);
                    openGainHere = std::min(std::pow(x, apertureContrast_), 1.0f);
                }
            } else {
                // フレネルが使えないときだけ、従来の「回折点まわりを稜線2軸で測る」方式
                //   （こちらは扉が斜めだと狭窄部を外す。比較用に残してある）。
                slitHere = slitWidthAt(Pc, PcEdge, normalized(source - listener));
                openGainHere = slitWidthGain(slitHere);
            }

            // ★2次回折。開口から音源が見通せないなら、その開口を新しい始点にしてもう一段探す。
            //   L 字の廊下や、手続き生成された曲がり角がこれに当たる。
            //   **鳴らす位置は 1 段目の開口のまま**（人は「手前の角から聞こえる」と感じるため。設計 ⑥）。
            //   経路長とゲインだけを 2 段目のぶん延長する。
            //   1 次で届けば探索しないので、開けた場所ではコストゼロ。
            float pathLen2 = 0.0f, delta2 = 0.0f;
            bool  have2 = false;          // 2 段目が成立したか（成立時は経路長と δ を差し替える）
            if (secondOrder && isOccluded(Pc, source)) {
                // ★2段目の起点は開口の**向こう側**に置く。
                //   開口の重心は稜線をならした点なので、壁の手前の面に乗ることがある。
                //   そこから音源へ向かうと**すぐ同じ壁に再突入**し、2段目の候補が全部棄却される
                //   （実測で 2 段目のクラスタが 0 本になっていた）。
                //   少し先へ送って壁を抜けた位置から探す。どれだけ送れば抜けるかは壁の厚み次第
                //   なので、いくつか試して最初に経路が見つかった距離を採る。
                //
                //   ★送りは実体を貫きうる。開口の脇の壁面に起点が乗るので、
                //     正当な 2 次回折（千鳥配置の戸口）でも壁の厚みぶんは貫く。
                //     だから「一切貫くな」にはできない ── 実際、次の3案はどれも
                //     正当な 2 次回折まで殺した:
                //       (a) 送りの途中が遮蔽されていたら棄却
                //       (b) 送りを 5cm 刻みまで細かくして (a)
                //       (c) 直接線を塞ぐ障害物だけ貫通禁止
                //     効いたのは下の「素通しなら貫通を許さない」判定（ループ内）。
                DiffractionPath sub[4];
                int ns = 0;
                const Vec3 dirS = normalized(source - Pc);
                // ★送る向きは音源方向だけでは足りない。
                //   **厚い障害物**を回り込む場合、近い縁から音源方向へ送ると
                //   板の中へ突っ込むので、2段目の候補が見つからない
                //   （1次に「芯を横切ったら棄却」を課した途端、厚い衝立の回り込みが
                //     全部消えたのがこれ。2次が肩代わりできていなかった）。
                //   厚みを回るには**板に沿って遠い縁の側へ**送る必要がある。
                //   稜線の向き PcEdge と板の面内方向の両方を候補にする。
                //   どれで抜けられるかは形状次第なので、順に試して最初に通ったものを採る。
                // ★厚みを渡る向き。音源方向 dirS から**面の外向き成分を抜く**。
                //   衝立の上端では dirS が斜め下を向くので、そのまま送ると板の中へ入る
                //   （実測: 上端 y=4.02 から 0.25 送ると y=3.896 で衝立の中）。
                //   外向き（PcRefT）の成分を抜けば、同じ高さのまま厚みを渡れる。
                //   そこから遠い縁が見え、2 段目が成立する。
                const Vec3 across = [&] {
                    Vec3 t = dirS - PcRefT * dot(dirS, PcRefT);
                    t = t - PcEdge * dot(t, PcEdge);          // 稜線方向にも進まない
                    return (length(t) > 1e-4f) ? normalized(t) : dirS;
                }();
                struct Push { Vec3 dir; float dist; };
                const Push pushes[8] = {
                    {dirS, 0.0f},  {dirS, 0.25f}, {dirS, 0.75f}, {dirS, 1.5f},
                    {across, 0.15f}, {across, 0.35f}, {across, 0.7f}, {across, 1.4f},
                };
                // ★「最初に成功した送り」ではなく「**総経路長が最短の送り**」を採る。
                //   最初に成功したものを採ると、送り 0（＝Pc そのもの）が先頭にあるせいで
                //   ゴミ経路で確定してしまう。Pc は膨らませた箱の上に乗るので**壁の手前**に
                //   出ることがあり（実測 z=-0.26、壁は z∈[-0.15,0.15]）、そこから 2 段目の
                //   戸口へ引いた線が 1 段目の戸口を数 cm 外して壁に当たる。結果、2 段目は
                //   遠回りの経路しか見つけられない:
                //     Pc(-0.89) → 送り0 → 2段目 8.17m  → 合計 10.92m（正しい）
                //     Pc(-0.11) → 送り0 → 2段目16.57m → 合計 19.39m（遠回り）
                //   Pc は戸口の左右どちらの縁にも乗りうるので、リスナーが 5cm 動いただけで
                //   10.92 ⇄ 19.39 と入れ替わり、到来方向が 70° 飛んでいた。
                //   最短路なら、支配経路として物理的に正しいうえ、リスナーの微小移動に対して
                //   連続に動く（切り替わっても長さが近いので方向も近い）。
                DiffractionPath bestSub{};
                float bestTotal = 1e30f;
                Vec3 bestStart = Pc;
                int usedPush = -1, pushIdx = -1;
                for (const Push& pu : pushes) {
                    ++pushIdx;
                    const float e = pu.dist;
                    const Vec3 st = Pc + pu.dir * e;
                    const int nsHere = findDiffractionPaths(st, source, sub, 4, order - 1);
                    if (nsHere <= 0) continue;
                    // ★送った先から音源が**素通しに見える**なら、それは 2 段目ではなく
                    //   「開口を抜けた」ということ。ならば送りの途中に遮蔽が無いはずで、
                    //   在るなら実体を貫いている＝そこに通り道は無い。
                    //   これを見ないと閉じた扉をすり抜ける（実測 125Hz 0.551）。
                    if (e > 0.0f && !isOccluded(st, source) && isOccluded(Pc, st)) continue;
                    int k0 = 0;                      // 2段目は最も通る（＝最短の）ものを1本だけ
                    for (int k = 1; k < nsHere; ++k)
                        if (maekawa::apertureWeight(sub[k].pathLength) >
                            maekawa::apertureWeight(sub[k0].pathLength)) k0 = k;
                    const float total = e + sub[k0].pathLength;
                    if (total < bestTotal) {
                        bestTotal = total; bestSub = sub[k0];
                        bestStart = st; usedPush = pushIdx;
                    }
                }
                if (usedPush < 0) continue;         // その開口の先は行き止まり。経路として採らない
                const Vec3 start = bestStart;
                sub[0] = bestSub;
                const int bs = 0;
                (void)ns;
                // ★2 段目は**幾何だけ**を持ち帰る（どこから聞こえるか・どれだけ遠いか）。
                //   ゲインは下の 1 次と**同じ式**で作る。以前はここで
                //     前川(δ1) × sub.gain(= 前川(δ2)×開口率2) × 開口率1
                //   と 4 つ掛けていたが、開口率は apertureFresnelBands が
                //   **リスナー視点で全インスタンスの影を 1 枚の面へ投影**した量なので、
                //   1 回の積分に 1 段目・2 段目の遮蔽が既に両方入っている。
                //   つまり同じ量を二重に払い、さらに前川まで二重に払っていた。
                //   実害は 1次⇄2次 の境界に出る。千鳥配置の戸口を斜めに覗くと、
                //   「2 枚目の縁がリスナーから直接見えるか」という**二値判定**で扱いが入れ替わり、
                //   同じ物理経路なのに値が飛ぶ（実測: x=-1.60 で 0.0893 → x=-1.55 で 0.0252、3.5倍）。
                //   合計 δ で前川を 1 回・開口率を 1 回にすれば、境界では δ1→0 なので
                //   合計 δ が 1 次の δ に一致し、**両側が同じ値**になって段差が消える。
                pathLen2 = length(centroid - listener) + length(start - Pc) + sub[bs].pathLength;
                delta2   = cl[bi].minDelta + std::max(sub[bs].delta, 0.0f);
                have2    = true;
            }

            // ★**定位は重心、幾何はスナップ点**と役割を分ける。
            //   Pc は「重心にいちばん近い実在の候補点」だが、戸口の左右の稜線は
            //   どちらも候補なので、リスナーが動くとどちらが近いかが入れ替わり、
            //   開口点が戸口の幅ぶん飛ぶ（実測: 5cm 動いただけで x=-0.14 ⇄ -0.89、
            //   到来方向が 15° 振れた）。人が聞くのは方向なので、ここは飛ばせない。
            //   一方 Pc（実在の点）は落とせない。2 次の起点や幅の測定は実体の上に
            //   乗っている必要があり、重心は板の中や向こう側へ出ることがある。
            out[n].aperture = centroid;
            // 開口の広がりを表す点を拾う（ホイヘンス。1つだと戸口が点に聞こえる）。
            //   Pc に加えて「Pc から最も遠い候補」と「その両者から最も遠い候補」を採り、
            //   開口の端まで張るようにする。候補は全部**実在の稜線上の点**なので、
            //   開口の外へはみ出すことはない。
            out[n].spread[0] = Pc;
            out[n].nSpread = 1;
            for (int pass = 0; pass < 2 && out[n].nSpread < DiffractionPath::kMaxSpread; ++pass) {
                int far = -1; float farD = 0.0f;
                for (int k = 0; k < cl[bi].npts; ++k) {
                    float d = 1e30f;
                    for (int q = 0; q < out[n].nSpread; ++q)
                        d = std::min(d, length(cl[bi].pts[k] - out[n].spread[q]));
                    if (d > farD) { farD = d; far = k; }
                }
                if (far < 0 || farD < 0.05f) break;   // 5cm 未満しか離れていないなら散らす意味がない
                out[n].spread[out[n].nSpread++] = cl[bi].pts[far];
            }
            // ★経路長も**重心**で測る。定位を重心にしたのと同じ理由。
            //   Pc で測ると、開口の左右の縁が両方候補になっているとき支配点がその間で
            //   入れ替わり、経路長が飛ぶ（実測: 縁を両方拾えるようにした途端 3.94m の跳び）。
            //   経路長は遅延と距離減衰の両方を決めるので、飛べばそのまま音の飛びになる。
            //   2 段目が成立したなら、経路長と δ を差し替える（ゲインの作り方は 1 次と同じ）。
            out[n].pathLength = have2 ? pathLen2
                                      : length(centroid - listener) + length(source - centroid);
            out[n].delta = have2 ? delta2 : cl[bi].minDelta;
            maekawa::gainBands(out[n].delta, out[n].gain);
            // 開口がどれだけ開いているかで抑える。**回折の音量はここが決める**。
            //   前川の式は δ しか見ないので、これが無いと扉を開けても音が変わらない
            //   （実測: Test_Full の形状で 0°→90° 振っても総量 1.000 のままだった）。
            //   稜線探索が幻の経路を返しても、開口率が 0 ならここで 0 になる。
            out[n].openGain = openGainHere;
            out[n].slitWidth = slitHere;
            // 隙間の幅による**帯域ごと**の通りやすさ。狭いほど高域だけが通る。
            //   「開くと音色が開く」はここで作られる（slitWidthBandGain のコメント参照）。
            if (haveFres) for (int b = 0; b < kNumBands; ++b) out[n].openBand[b] = fres[b];
            else slitWidthBandGain(slitHere, out[n].openBand);
            if (btmDone && !have2) {
                // ★BTM は「前川の δ 減衰 × 開口率」を**まとめて置き換える**。
                //   両方掛けると二重になる（そこが今までの詰まりだった）。
                for (int b = 0; b < kNumBands; ++b) {
                    out[n].gain[b] = btmBands[b];
                    out[n].openBand[b] = 1.0f;
                }
                out[n].openGain = 1.0f;
            } else if (haveFres && apertureIsTransmission_) {
                // ★開口を通る分は**前川の δ 減衰を払わない**。
                //   f（フレネル/影の積分）はキルヒホッフ側の量で、遠回りの効果は
                //   その中の重みに既に入っている。ここへ前川を掛けると二重になる
                //   （実測: 二重掛けを外すと 1kHz で 4倍差が出た）。
                //   合成透過率の割合形 τ_eff = τ_壁(1−f) + f の、**f の項**がこれ。
                //   開口を通る成分は開口の方向から届くので、このタップへ入れる。
                //   音源方向の直接音へ足すと定位が壊れる。
                for (int b = 0; b < kNumBands; ++b)
                    out[n].gain[b] = fres[b] * apertureContrastMap(fres[b]);
            } else {
                for (int b = 0; b < kNumBands; ++b) out[n].gain[b] *= out[n].openGain;
            }
            ++n;
        }
        return n;
    }

    // 【回折を二次音源として鳴らす（GTD/ホイヘンス）】遮蔽時、回り込みエッジを「エッジ＝二次音源」として
    // 最大 maxN 個の仮想音源（方向つき）に束ねて返す。近い方向のエッジはクラスタ統合するので、両側に
    // 開口があれば左右2音源…のように分かれ、リスナー移動で各ゲインが滑らかに変わる（＝1点合成の飛びを排除）。
    //   outPos[k]  : 二次音源のワールド位置（= listener + 方向 × 音源距離。Wwise がこの方向へ定位）
    //   outGain[k] : 相対ゲイン（全クラスタ合計で正規化, 短い迂回ほど大）。総和 ≤ 1
    // 戻り値 = 書き込んだ音源数。遮蔽なし/迂回なしは 0。
    //   ※ findDiffractionPaths への薄いラッパ。探索は 1 箇所に一本化してある。
    //     見通し時に返る「直接経路への補正」は二次音源ではないので、ここでは 0 本にする
    //     （方向は音源そのものなので、呼び出し側は直接音として鳴らせばよい）。
    /// 上と同じだが、開口ごとに**6帯域のゲイン**を返す（outBand は maxN*6 要素）。
    ///   隙間が狭いほど高域だけが通るので、開き具合が音色に出る。
    ///   outBand が null なら computeDiffractionSources と同じ。
    int computeDiffractionSourceBands(const Vec3& listener, const Vec3& source,
                                      Vec3* outPos, float* outGain, float* outBand,
                                      int maxN) const {
        return computeDiffractionSources(listener, source, outPos, outGain, maxN, outBand);
    }

    int computeDiffractionSources(const Vec3& listener, const Vec3& source,
                                  Vec3* outPos, float* outGain, int maxN,
                                  float* outBand = nullptr) const {
        if (!outPos || !outGain || maxN <= 0) return 0;
        if (!isOccluded(listener, source)) return 0;

        constexpr int kMaxOut = 24;
        DiffractionPath paths[kMaxOut];
        const int np = findDiffractionPaths(listener, source, paths, std::min(maxN, kMaxOut));
        if (np <= 0) return 0;

        // 低域加重で 6 帯域を 1 スカラへ（回折は低域が回り込むので低域を重く見る）。
        auto bbGain = [](const float g[kNumBands]) {
            const float w[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
            float gs = 0.0f, ws = 0.0f;
            for (int b = 0; b < kNumBands; ++b) { gs += w[b] * g[b]; ws += w[b]; }
            return ws > 0.0f ? gs / ws : 0.0f;
        };
        // 重みは頭打ちしない前川値（定位をぼやけさせないため。findDiffractionPaths と同じ理由）。
        const float directDist = std::max(length(source - listener), 1e-4f);
        float sum = 0.0f;
        for (int i = 0; i < np; ++i) sum += maekawa::apertureWeight(paths[i].pathLength - directDist);
        int n = 0;
        for (int i = 0; i < np && n < maxN; ++i) {
            const float w = maekawa::apertureWeight(paths[i].pathLength - directDist);
            // ★返すのは**絶対量**。「配分比 × その開口をどれだけ通るか」。
            //   以前は配分比だけを返していた。配分比は合計 1 に正規化されるので、開口が
            //   1 つなら常に 1.0 ── **扉を開けても音量が変わらなかった**（実測: 0°→90° で
            //   総量 1.000 のまま）。「どこへ配るか」は返せていたが「どれだけ通るか」を
            //   捨てていた。
            //
            //   通りやすさは δ（開口を回り込むぶんの余剰距離）から出す。扉が壁として効けば
            //   δ は扉について動くので、開き具合がそのまま音量になる。
            //   ★低域加重で 1 スカラに畳んで掛ける（帯域別に掛けない）。
            //     帯域別に掛けるのは LPF であり、こもりは透過が担当という役割分担を壊す。
            //     回折が運ぶのは「開口への定位」と「回り込むぶんの距離」の 2 つだけ。
            const float thru = bbGain(paths[i].gain);
            const float g = ((sum > 1e-6f) ? w / sum : 0.0f) * thru;

            // ★開口を**面**として鳴らす（ホイヘンス）。
            //   点1つだと戸口がピンポイントに聞こえて「直線的」になる。開口の広がりに
            //   沿って数点へ散らすと、点ごとに経路長と方向がわずかに違うので、
            //   遅延とパンがばらけて「戸口サイズの面」として聞こえる。
            //   定位は失わない（全点が同じ開口の上にある）ので、設計の優先順位
            //   「遮蔽量の精度より開口への定位」を崩さずに幅だけが加わる。
            //   ゲインは 1/k。低域（遅延差が波長より小さい）では同相で足されて元の量に戻り、
            //   高域では散らばって小さくなる ── 面音源の周波数特性としてそれで正しい。
            const int k = std::min(apertureSpread_, paths[i].nSpread);
            const float inv = (k > 1) ? 1.0f / static_cast<float>(k) : 1.0f;
            const int nq = (k > 1) ? k : 1;
            for (int q = 0; q < nq && n < maxN; ++q) {
                const Vec3 sp = (k > 1) ? paths[i].spread[q] : paths[i].aperture;
                const Vec3 dir = normalized(sp - listener);
                // 経路長も点ごとに測り直す。ここが違うから遅延がばらける。
                const float plen = (k > 1) ? (length(sp - listener) + length(source - sp))
                                           : paths[i].pathLength;
                outPos[n] = listener + dir * std::max(plen, 0.5f);
                outGain[n] = g * inv;
                if (outBand)
                    for (int b = 0; b < kNumBands; ++b)
                        outBand[n * kNumBands + b] = g * inv * paths[i].openBand[b];
                ++n;
            }
        }
        return n;
    }

    /// 開口を何点に散らすか（1=点のまま／2〜3=面として鳴らす）。既定 1。
    void setApertureSpread(int n) { apertureSpread_ = std::min(std::max(n, 1), DiffractionPath::kMaxSpread); }
    int apertureSpread() const { return apertureSpread_; }

    // 【回折(Phase 1.5)】from->to の帯域別回折ゲイン(0..1)。
    //   遮蔽なし → 全帯域 1.0 / 迂回路あり → Maekawa（低域ほど回り込む）/ 迂回路なし → 0。
    // 影境界を跨いでも連続になるよう、照らされた領域でも回折場を計算して直接音と合成する。
    //
    //   以前は isOccluded の二値判定で「非遮蔽 → 全帯域 1.0 / 遮蔽 → UTD値」と切り替えていた。
    //   UTD は影境界で約 0.5(-6dB) を返すので、境界を跨いだ瞬間に 0.5 ⇄ 1.0 の段差
    //   （実測で最大 12.5dB）が出ていた。
    //   物理的には照らされた領域にも回折場は存在し、直接音と足すと境界で連続になる
    //   ── それが UTD が GTD を「一様化」した目的そのもの（utd.h の utdTotalGain 参照）。
    void computeDiffraction(const Vec3& from, const Vec3& to, float outGain[kNumBands]) const {
        diffractionContinuous(from, to, isOccluded(from, to), outGain);
    }

    // 回折ゲイン（影境界で連続）。occ は呼び出し側が既に持っている遮蔽判定を渡す
    // （computeDirectSoft は同じ判定を使い回すため、二重に raycast しない）。
    //
    // モデルは前川の式（Core/maekawa.h）。符号付き δ だけで決まる:
    //     影の側 δ>0 / 影境界 δ=0（両側 5dB）/ 照らされた側 δ<0
    //
    //   ★連続性が構造的に保証される理由:
    //     δ は「全候補稜線の最小値」を取っても連続（min は連続関数を保つ。どの稜線が
    //     最小かが入れ替わっても、値そのものは飛ばない）。したがってゲインも飛ばない。
    //     影境界では直線が稜線を掠めるので δ→0 になり、符号だけが反転して滑らかに繋がる。
    //
    //   UTD(Core/utd.h) を音声経路に使わない理由は maekawa.h の冒頭に記載。
    //   要点だけ: UTD は回折点の 3D 幾何（φ',φ,n,β0）に依存するので、稜線が入れ替わると
    //   幾何が不連続に変わって飛ぶ。重み付き平均・複素和・エネルギー加算をいずれも試したが
    //   解消できず、最良でも 3.8dB、最悪 17.6dB の段差が残った。
    //   UTD は diffractionUtd() として比較・検証用に残してある。
    //   ゲインは**最小 δ から**出す。
    //     設計 §5-6 では開口ごとのエネルギー加算としていたが、実測すると連続性が悪化した
    //     （影境界 0.033 → 0.093 / ドア 0.191 → 0.234）。理由は、加算の前提である
    //     「同じ物理経路を重複して数えない」がクラスタリングの離散性で満たせないため。
    //     クラスタが分裂・統合する瞬間に和が飛ぶ。
    //     min は連続なので飛ばない。本作の優先順位（遮蔽量の精度より定位）からも、
    //     ゲインは連続でありさえすればよい。**方向は開口ごとに出るので情報は失われない。**
    void diffractionContinuous(const Vec3& from, const Vec3& to, bool occ,
                               float outGain[kNumBands]) const {
        constexpr int kMaxOut = 8;
        DiffractionPath paths[kMaxOut];
        const int np = findDiffractionPaths(from, to, paths, kMaxOut);
        if (np <= 0) {
            // 回折の相手が無い。遮蔽なら完全に届かない、非遮蔽なら素通り。
            const float g = occ ? 0.0f : 1.0f;
            for (int b = 0; b < kNumBands; ++b) outGain[b] = g;
            return;
        }
        // 経路ごとに maekawa(δ) × 開口幅ゲイン を出し、その最大を採る。
        //
        //   ・δ から作り直すこと自体は正しい（上記の連続性の理由）。
        //     ただし**開口幅ゲインを掛け直さないと、閉じた扉のわずかな隙間から素通りする**
        //     （実測: 3cm の隙間でも 0.56 のままだった）。幅は δ に現れないため。
        //   ・経路の gain[] をそのまま使ってはいけない。2 次回折の gain[] は段ごとの積
        //     （g1×g2）で作ってあり、maekawa(δ1+δ2) より小さい。1 次と 2 次が入れ替わる
        //     瞬間に段差が出る（実測: 扉 6.5° で 0.191）。
        //   ・幅ゲートが全て 1 のときは max_i maekawa(δ_i) = maekawa(min_i δ_i) となり、
        //     従来の「最小 δ」と厳密に一致する（maekawa は δ の単調減少関数）。
        for (int b = 0; b < kNumBands; ++b) outGain[b] = 0.0f;
        for (int i = 0; i < np; ++i) {
            float gi[kNumBands];
            maekawa::gainBands(paths[i].delta, gi);
            for (int b = 0; b < kNumBands; ++b)
                outGain[b] = std::max(outGain[b], gi[b] * paths[i].openGain);
        }
    }

    // 全候補稜線を回る迂回の余剰長 δ(m, 非負) の最小値。候補が無ければ false。
    //   照らされた領域では直接線を塞ぐ箱が無いので、候補探索を近傍まで広げる必要がある。
    //   マージンは前川の式が効く範囲より広ければよい。効かなくなるのは N<-0.2、
    //   すなわち δ > 0.1λ。最低域 125Hz の λ=2.7m でも δ 0.28m 程度なので 4m あれば十分。
    bool minDetour(const Vec3& from, const Vec3& to, bool occ, float& outDelta) const {
        const float margin = occ ? 0.0f : 4.0f;
        bool any = false;
        float best = 0.0f;
        forEachDiffractionCandidate(from, to,
            [&](const Vec3&, float d, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
                if (!any || d < best) { any = true; best = d; }
            }, margin);
        outDelta = best;
        return any;
    }

    // UTD 版の回折ゲイン（比較・検証用。音声経路では使わない）。
    // 厳密解だが影境界で不連続になるため採用していない ── 回帰テストで両者を並べて出す。
    void diffractionUtd(const Vec3& from, const Vec3& to, bool occ,
                        float outGain[kNumBands]) const {
        const float margin = occ ? 0.0f : 8.0f;
        constexpr int kMaxEdges = 8;
        struct Cand { Vec3 P; Vec3 edgeDir; Vec3 refT; float delta; };
        Cand cands[kMaxEdges];
        int nc = 0;
        float bestDelta = -1.0f;
        forEachDiffractionCandidate(from, to,
            [&](const Vec3& P, float d, const Vec3& edgeDir, const Vec3& refT, const Vec3&, const Vec3&, float, int) {
                if (bestDelta < 0.0f || d < bestDelta) bestDelta = d;
                if (nc < kMaxEdges) cands[nc++] = Cand{P, edgeDir, refT, d};
            }, margin);
        if (nc == 0) {
            const float g = occ ? 0.0f : 1.0f;
            for (int b = 0; b < kNumBands; ++b) outGain[b] = g;
            return;
        }
        float direct[kNumBands];
        for (int b = 0; b < kNumBands; ++b) direct[b] = 1.0f;
        float acc[kNumBands] = {0, 0, 0, 0, 0, 0};
        float wSum = 0.0f;
        const float kSigma = 0.15f;   // δ による軟らかい重み付け（稜線の乗り換えを均す試み）
        for (int i = 0; i < nc; ++i) {
            const float w = std::exp(-(cands[i].delta - bestDelta) / kSigma);
            if (w < 1e-3f) continue;
            float g[kNumBands];
            utd::utdWedgeTotal(to, cands[i].P, from, cands[i].edgeDir, cands[i].refT,
                               1.5f, !occ, direct, g);
            for (int b = 0; b < kNumBands; ++b) acc[b] += w * g[b];
            wSum += w;
        }
        const float inv = (wSum > 1e-6f) ? 1.0f / wSum : 0.0f;
        for (int b = 0; b < kNumBands; ++b) outGain[b] = acc[b] * inv;
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

        // 回折：影境界で連続になる版を使う（diffractionContinuous 参照）。
        //   以前はここで「非遮蔽→全帯域1.0 / 遮蔽→UTD値」と二値で切り替えており、
        //   影境界を跨ぐ瞬間に約12dBの段差が出ていた。occFrac で緩和を試みていたが、
        //   dif 自体が跳ぶので効いていなかった。
        //   ※音声経路は useReflections=ON のとき occlusionReflectedMulti 経由で
        //     ここを通る。computeDiffraction を直しただけでは音に効かない。
        const bool centerOcc = isOccluded(listener, source);
        float dif[kNumBands];
        // ★ポータルがこの経路を支配しているなら、回り込み量は**ポータルが唯一の答え**。
        //   旧経路（前川ベース）を併用すると、閉じた部屋で経路が無いのに
        //   回折が −39dB の床を作り、そこから上は材質が効かなくなる
        //   （実測: 壁の裏だけ 500Hz 以上が扉と同じ 0.0115 に張り付いていた）。
        //   ポータルが閉じている(f=0)なら回り込みも 0 でなければ辻褄が合わない。
        if (!portals_.empty()) {
            // ポータルを置いたシーンでは、開口経由の音はポータルだけが決める。
            //   ★「直線が矩形を横切るか」で使う／使わないを切り替えてはいけない。
            //     開口率そのものは連続なのに、**使い方が二値**になって崖が出る
            //     （実測: 音源を横へ 5cm 動かしただけで 8.8dB 落ち、その後
            //      +6.6 / -2.4 / -3.6 dB と暴れた。開口率は 0.9656 で平らなまま）。
            //     耳には「急に音が小さくなる」と聞こえる。
            //   常に全ポータルを測り、最も通るものを採る。開口率はゾーンの中心を
            //   矩形内へ丸めているので、線が外れても連続に落ちる。
            for (int b = 0; b < kNumBands; ++b) dif[b] = 0.0f;
            for (const Portal& pt : portals_) {
                if (!pt.active) continue;
                float pf[kNumBands]; Vec3 cp(0, 0, 0);
                if (!portalCoupling(pt, listener, source, pf, &cp)) continue;
                for (int b = 0; b < kNumBands; ++b) dif[b] = std::max(dif[b], pf[b]);
            }
        } else {
            diffractionContinuous(listener, source, centerOcc, dif);
        }

        // 迂回余剰長 δ（ステアの重み付けに使う）。非遮蔽=0 / 迂回路なし=大。
        float detourDelta = 0.0f;
        if (centerOcc) {
            Vec3 dp;
            const float delta = diffractionDetour(listener, source, dp);
            detourDelta = (delta < 0.0f) ? 1e9f : delta;
        }

        for (int b = 0; b < kNumBands; ++b) {
            // 透過はエネルギー、回折(前川)は振幅。**そのまま比べてはいけない**ので
            //   透過側を振幅へ揃える（material.h の単位規約）。
            //   揃えないと壁越しの成分が二乗ぶん小さく評価され、実測で 9dB 過小だった。
            const float soft = std::sqrt(transAccum[b] * inv);   // 滑らかな直接透過（振幅）
            // 壁を抜ける成分と回り込む成分の大きい方を採る。
            //   回折側は既に「照らされていれば直接音込み」の総合値なので、
            //   occFrac のような後付けのフェードは要らない。
            outGain[b] = clamp01(std::max(soft, dif[b]));
        }
        if (outDetourDelta) *outDetourDelta = detourDelta;
    }

    // 【ソフト遮蔽の素材】直接経路の透過(振幅)と「どれだけ遮られているか(0..1)」を別々に返す。
    //
    //   ★これがあると `occ` の二値分岐を全部消せる。
    //     以前は「見通せているか」で直接タップの意味を切り替えていた:
    //       lit  → 直接タップ = 回折ゲイン（境界で 0.56）
    //       !lit → 直接タップ = 透過（壁材なら 0.12）＋ F タップが開口に出現
    //     境界を跨いだ瞬間に 13dB 落ち、同時に音が音源方向から開口方向へワープしていた。
    //
    //   音源まわりの円盤をサンプルするので、掠める位置では「一部だけ遮られる」状態が
    //   そのまま数値になる。透過も遮蔽割合も連続に動くので、分岐なしで書ける:
    //       直接タップ = softTrans          （見通しで 1.0、境界で約 0.5、影で材質の透過）
    //       F タップ   = 回折ゲイン × occFrac（見通しで 0 になるので二重計上しない）
    //   sourceRadius が**遷移の幅**を決める。物理的には遷移幅はフレネルゾーン（低域ほど広い、
    //   数メートル規模）だが、ここは帯域共通の1つの円盤で近似している。広げるほど滑らかに
    //   なる代わりに影の縁がぼやけるので、耳で決めるチューニング値。
    // outLeakPoint … **どこから最も多く漏れているか**（透過で重み付けた、遮蔽面上の重心）。
    //   閉じた扉の向こうで鳴っている音は、現実には扉の板が再放射して届く。
    //   だから部屋のどこにいても「扉から聞こえる」。ところが今の定位は
    //   遮蔽の深さ δ でブレンドしていて、閉扉は δ が 0.023 と小さいため
    //   **ほぼ音源方向のまま**になる（壁の中から鳴っているように聞こえる）。
    //
    //   ★「直線と壁の交点」を使っても意味がない。交点は直線上にあるので方向が変わらない。
    //     要るのは面全体のうち**最も通す場所**で、それは透過の重み付き重心。
    //     音源まわりに散らした標本のレイが当たる位置を、その標本の透過量で
    //     重み付けて平均すれば出る。壁より弱い扉があれば重心は扉へ寄る。
    //     物体が扉かどうかは見ない ── 弱い所へ寄る、というだけ。
    void computeSoftOcclusion(const Vec3& listener, const Vec3& source,
                              float outTrans[kNumBands], float& outOccFrac,
                              int numSamples = 32, float sourceRadius = 0.9f,
                              Vec3* outLeakPoint = nullptr) const {
        using namespace scene_detail;
        Vec3 dir = source - listener;
        const float dist = length(dir);
        if (dist < 1e-4f) {
            for (int b = 0; b < kNumBands; ++b) outTrans[b] = 1.0f;
            outOccFrac = 0.0f;
            return;
        }
        dir = dir * (1.0f / dist);
        const Vec3 t = (std::fabs(dir.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 u = normalized(cross(dir, t));
        const Vec3 v = cross(dir, u);

        const int N = numSamples < 1 ? 1 : numSamples;
        float acc[kNumBands] = {0, 0, 0, 0, 0, 0};
        float blockedAcc[kNumBands] = {0, 0, 0, 0, 0, 0};
        int blockedN = 0;
        float occWeighted = 0.0f;
        Vec3 leakAcc(0, 0, 0);
        float leakW = 0.0f;

        // ★サンプル配置は**決定的**にする（フィボナッチ円盤）。
        //   乱数で撒くと、リスナーが少し動くだけでパターンが変わり、遮蔽割合がガタつく
        //   （実測で 0.511 → 0.371 → 0.794 と非単調になった）。
        //   位置に依存しない固定配置なら、変化するのは幾何だけなので滑らかに動く。
        constexpr float kGolden = 2.39996323f;
        for (int i = 0; i < N; ++i) {
            const float rr = sourceRadius * std::sqrt((i + 0.5f) / static_cast<float>(N));
            const float aa = kGolden * static_cast<float>(i);
            const Vec3 p = source + u * (rr * std::cos(aa)) + v * (rr * std::sin(aa));
            float g[kNumBands];
            computeTransmission(listener, p, g);      // エネルギー
            for (int b = 0; b < kNumBands; ++b) acc[b] += g[b];
            // 漏れの重心。遮られている標本について、当たった位置を透過量で重み付ける。
            if (outLeakPoint && g[0] < 0.5f) {
                const Vec3 sd = p - listener;
                const float sl = length(sd);
                if (sl > 1e-4f) {
                    const SceneHit hh = raycastClosest(listener, sd * (1.0f / sl), sl);
                    if (hh.hit) {
                        const float w = g[0];        // 低域の透過量で重み付け（漏れの主成分）
                        leakAcc = leakAcc + hh.point * w;
                        leakW += w;
                    }
                }
            }
            // 「塞がれている側」の材質。**実際に遮られている標本だけ**を平均する。
            //   ★最小値を採ってはいけない。最小は「最もよく遮る材質」＝壁を拾うので、
            //     戸口を覆っているのが扉(TL15)でも壁(TL34)の値になり、
            //     閉じた扉が壁として鳴る（実測で直接音が 19dB 沈んだ）。
            //     覆っているのが何かは標本が示しているので、それを素直に使う。
            if (g[0] < 0.5f) {
                for (int b = 0; b < kNumBands; ++b) blockedAcc[b] += g[b];
                ++blockedN;
            }
            // 遮蔽割合も「何本当たったか」の二値カウントではなく、**低域の減衰量**で測る。
            //   二値だと 1/N 刻みの階段になる。減衰量なら部分的な遮蔽が連続に出る。
            occWeighted += 1.0f - std::sqrt(g[0]);
        }
        const float inv = 1.0f / static_cast<float>(N);
        // エネルギーで平均してから振幅へ（material.h の単位規約）。
        for (int b = 0; b < kNumBands; ++b) outTrans[b] = std::sqrt(acc[b] * inv);
        outOccFrac = clamp01(occWeighted * inv);
        if (outLeakPoint)
            *outLeakPoint = (leakW > 1e-9f) ? leakAcc * (1.0f / leakW) : source;

        // ★経路がポータルを横切るなら、**開口率で置き換える**。
        //   円盤の標本化は「抜けたか塞がれたか」の二値を N 個数えるので、
        //   遮蔽側(0.003)と素通し側(1.0)の比が 316倍あると 1/N の粒度が巨大な段になる。
        //   実測: 扉 20°→30° で 32点中3点が抜けただけで **+14.9dB** 跳んだ。
        //   ポータルの開口率 f は解析的で連続なので、これを「抜けた割合」に使う。
        //     τ² = f + (1−f)·τ_塞がれた²
        //   τ_塞がれた は標本の**最小値**を採る（材質そのものなので角度で動かない＝段が出ない）。
        //   ★「線分が矩形を通るか」で使う／使わないを切り替えてはいけない。
        //     開口率そのものは連続なのに**使い方が二値**になって崖が出る
        //     （実測: 音源を横へ 5cm 動かしただけで 8.8dB 落ち、その後 +6.6 / -2.4 dB と
        //      暴れた。開口率は 0.9656 で平らなまま）。耳には「急に小さくなる」と聞こえる。
        //     全ポータルを測って最も通るものを採る。開口率はゾーンの中心を矩形内へ
        //     丸めているので、線が外れても連続に落ちる。
        if (!portals_.empty()) {
            float best[kNumBands] = {0, 0, 0, 0, 0, 0};
            bool anyPortal = false;
            for (const Portal& pt : portals_) {
                if (!pt.active) continue;
                float f[kNumBands]; Vec3 cp(0, 0, 0);
                if (!portalCoupling(pt, listener, source, f, &cp)) continue;
                for (int b = 0; b < kNumBands; ++b) best[b] = std::max(best[b], f[b]);
                anyPortal = true;
            }
            if (anyPortal) {
                const float binv = (blockedN > 0) ? 1.0f / static_cast<float>(blockedN) : 0.0f;
                for (int b = 0; b < kNumBands; ++b) {
                    // 遮られている標本が1つも無ければ素通し扱い（塞ぐものが無い）。
                    const float blocked = (blockedN > 0) ? clamp01(blockedAcc[b] * binv) : 1.0f;
                    outTrans[b] = std::sqrt(clamp01(best[b] + (1.0f - best[b]) * blocked));
                }
                outOccFrac = clamp01(1.0f - outTrans[0]);
            }
        }
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
            // total は振幅（computeDirectSoft の出力）、reflected はエネルギー。
            //   直接と反射は無相関な別経路なので**エネルギーで足してから振幅へ戻す**。
            //   単位を揃えずに足すと、反射ぶんが二乗の分だけ過大に効く。
            for (int b = 0; b < kNumBands; ++b) {
                const float e = total[b] * total[b] + reflected[b] * inv;
                total[b] = clamp01(std::sqrt(e));
            }
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
                // ★★ 未完成: 壁を抜けてくる音を「面が再放射する」形にしたい ★★
                //   音源方向のまま鳴らすと壁の中から聞こえ、歩いても
                //   「扉から漏れている」感じにならない（耳で確認済み）。
                //   漏れの重心（computeSoftOcclusion の outLeakPoint）を定位に使う案を
                //   実装したが、**重心が動かなかった**（実測: 扉の材質を変えても
                //   重心は x=-1.45 のまま）。
                //   理由: 重心は「音源のまわりに散らした標本」のレイが当たる位置を集める。
                //   リスナーが戸口から外れていると、標本のレイは全部**壁**に当たり、
                //   扉に届く標本が 1 つも無い。弱点が別の場所にある場合を拾えない。
                //   → 面上の全点を再放射源として積分する（音響ラジオシティ）か、
                //     弱点を宣言してポータル同様に扱うか。前者は重く、後者は authoring。
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
            // total は振幅、reflected はエネルギー。無相関な別経路なので
            //   エネルギーで足してから振幅へ戻す（material.h の単位規約）。
            for (int j = 0; j < count; ++j)
                for (int b = 0; b < kNumBands; ++b) {
                    const float amp = total[j * kNumBands + b];
                    const float e = amp * amp + reflected[j * kNumBands + b] * inv;
                    total[j * kNumBands + b] = clamp01(std::sqrt(e));
                }
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

    // 【方向プローブ】origin から各方向へレイを飛ばし、「その方向からどれだけ残響が返るか」を
    // 帯域別に返す。outEnergy は dirCount*kNumBands 要素。
    //
    //   後期残響は拡散なので時間構造はエコーグラムが持てばよく、**方向分布**だけが足りない。
    //   音源に依存しない量なので音源ごとに計算する必要がなく、リスナー位置だけで決まる
    //   ＝ 音源数が増えてもコストが増えない（業界が後期残響を共有バスにしているのと同じ理屈）。
    //
    //   量の定義: 各方向へ撃ったレイが、反射のたびに残るエネルギー carry を積算したもの。
    //     ・生きた部屋へ向かう方向 … 何度も反射して積算が大きい
    //     ・吸音の強い面／開けた空へ向かう方向 … すぐ尽きる、または何にも当たらず 0
    //   絶対値ではなく方向間の相対分布として使う（呼び出し側で正規化する）。
    void probeDirectionalEnergy(const Vec3& origin, const Vec3* dirs, int dirCount,
                                int maxBounces, float* outEnergy) const {
        using namespace scene_detail;
        if (!dirs || !outEnergy || dirCount <= 0) return;
        for (int k = 0; k < dirCount * kNumBands; ++k) outEnergy[k] = 0.0f;
        if (maxBounces <= 0 || instanceCount() == 0) return;

        const float kEps = 1e-3f;
        const float kMaxDist = 500.0f;   // これ以上先の反射は残響として意味を持たない
        for (int i = 0; i < dirCount; ++i) {
            Vec3 o = origin;
            Vec3 d = dirs[i];
            const float dl = length(d);
            if (dl < 1e-6f) continue;
            d = d * (1.0f / dl);
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 9781u;
            float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
            float remaining = kMaxDist;
            float* dst = outEnergy + static_cast<size_t>(i) * kNumBands;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const SceneHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;             // 何にも当たらない＝開けている＝残響を返さない
                remaining -= hit.t;
                if (remaining <= kEps) break;
                const AcousticMaterial& mat = materialOf(hit.materialId);
                for (int b = 0; b < kNumBands; ++b) {
                    carry[b] *= clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    dst[b] += carry[b];
                }
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                o = hit.point + hit.normal * 0.02f;
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
            // 内部はエネルギー（refl = 1-α-τ も透過も）、タップのゲインは振幅で返す
            //   （material.h の単位規約）。ここを素通しにすると反射が二乗ぶん小さくなり、
            //   高次ほど誤差が積み上がって尾が痩せる。
            for (int b = 0; b < kNumBands; ++b)
                outGain[n * kNumBands + b] = std::sqrt(t.g[b]);
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

    // ========================================================================
    // バッチ更新（API移行 段2: docs/API_MIGRATION_PLAN.md）
    //
    // SPEC §4 のフレーム内パイプラインを 1 関数にまとめる。
    //   役割1（中頻度）: 音源ごとの遮蔽・回折・透過・到来方向
    //   役割2（低頻度）: エコグラム（残響）・早期反射・回折二次音源
    //
    // これまでホストが「どれをいつ呼ぶか」を _erCountdown 等のカウンタで管理していたが、
    // それはエンジンの知識であってホストに置くべきものではない（移植のたびに書き直しになる）。
    // ここで内部レートとして持つ。
    //
    // 結果は results_ に置き、getSource*/getEarly* 等で読む。
    // 段5でワーカースレッド化するとき、この関数ごとワーカーへ移してダブルバッファ化する。
    // ========================================================================

    struct UpdateConfig {
        // 役割ごとの更新間隔（フレーム）。1=毎フレーム。
        int role1EveryN = 1;      // 遮蔽・回折
        int role2EveryN = 4;      // 残響（重いので低レート）
        int earlyEveryN = 3;      // 早期反射
        int diffSrcEveryN = 2;    // 回折二次音源
        int catalogEveryN = 3;    // エッジカタログ

        // 役割1
        int reflectionRays = 256;
        int reflectionBounces = 3;
        float directWeight = 1.0f;
        bool useReflections = true;

        // エッジカタログ
        bool useEdgeCatalog = true;
        int edgeCatalogRes = 16;
        float edgeCatalogMaxDist = 40.0f;

        // 役割2: エコグラム
        bool enableReverb = true;
        int echogramBins = 100;
        float echogramBinSeconds = 0.01f;
        int echogramRays = 512;
        int echogramBounces = 24;
        float speedOfSound = 343.0f;
        float distanceRef = 0.0f;   // 0=広がり損失なし

        // 役割2: 早期反射 / 回折二次音源
        bool enableEarlyReflections = true;
        int earlyTaps = 4;
        int earlyRays = 512;
        int earlyBounces = 2;
        bool enableDiffractionSources = true;
        int diffSources = 3;
    };

    // 段の**位相**をずらすことは試したが、測ったら効かなかった（最悪 18.65 → 19.35ms）。
    //   山の正体は「重い段が重なること」ではなく、**エコグラムが単独で予算を超えること**
    //   （512本×24反射を 1 フレームで撃つので、走る回のフレームだけ約 14ms かかる）。
    //   並べ直しても 14ms の塊は消えないので、位相ずらしは入れていない。
    void setUpdateConfig(const UpdateConfig& c) { cfg_ = c; }
    const UpdateConfig& updateConfig() const { return cfg_; }

    // 毎フレーム 1 発。内部レートに従って各役割を実行し、結果を results_ に置く。
    void update(float /*dt*/) {
        const int n = sourceCount();
        results_.resize(n, cfg_);
        if (n == 0) return;

        // 音源位置を配列へ（既存の multi 系 API がポインタ配列を取るため）。
        srcScratch_.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) srcScratch_[static_cast<size_t>(i)] = sources_[static_cast<size_t>(i)].pos;

        // a) エッジカタログ（リスナー中心・全音源共有）。回折が使う。
        if (cfg_.useEdgeCatalog) {
            if (--catalogCountdown_ <= 0) {
                catalogCountdown_ = (cfg_.catalogEveryN > 0) ? cfg_.catalogEveryN : 1;
                buildEdgeCatalog(listenerPos_, cfg_.edgeCatalogRes, cfg_.edgeCatalogMaxDist);
            }
        } else {
            clearEdgeCatalog();
        }

        // b) 役割1: 遮蔽・回折・透過・到来方向（音源ごと）。
        if (--role1Countdown_ <= 0) {
            role1Countdown_ = (cfg_.role1EveryN > 0) ? cfg_.role1EveryN : 1;
            runRole1(n);
        }

        // c) 役割2: 早期反射（音源ごと・低レート）。
        if (cfg_.enableEarlyReflections) {
            if (--earlyCountdown_ <= 0) {
                earlyCountdown_ = (cfg_.earlyEveryN > 0) ? cfg_.earlyEveryN : 1;
                runEarlyReflections(n);
            }
        }

        // d) 役割2: 回折二次音源（音源ごと・低レート）。
        if (cfg_.enableDiffractionSources) {
            if (--diffSrcCountdown_ <= 0) {
                diffSrcCountdown_ = (cfg_.diffSrcEveryN > 0) ? cfg_.diffSrcEveryN : 1;
                runDiffractionSources(n);
            }
        }

        // e) 役割2: エコグラム（全音源まとめて・最低レート）。
        if (cfg_.enableReverb) {
            if (--role2Countdown_ <= 0) {
                role2Countdown_ = (cfg_.role2EveryN > 0) ? cfg_.role2EveryN : 1;
                runEchogram(n);
            }
        }
    }

    // --- 結果取得（前回 update ぶん）---
    // index は sourceIndexOf(id) で引く。範囲外は何もしない（呼び手のバッファは不変）。

    void getSourceOcclusion(int index, float* out6) const {
        if (!out6 || !validResult(index)) return;
        const float* src = &results_.bands[static_cast<size_t>(index) * kNumBands];
        for (int b = 0; b < kNumBands; ++b) out6[b] = src[b];
    }

    float getSourceOcclusionScalar(int index) const {
        return validResult(index) ? results_.occ[static_cast<size_t>(index)] : 0.0f;
    }

    void getSourceArrivalDir(int index, float* out3) const {
        if (!out3 || !validResult(index)) return;
        const float* d = &results_.dir[static_cast<size_t>(index) * 3];
        out3[0] = d[0]; out3[1] = d[1]; out3[2] = d[2];
    }

    // 早期反射タップ。書き込んだ本数を返す。
    int getEarlyReflections(int index, Vec3* outPos, float* outGain6, int maxTaps) const {
        if (!outPos || !outGain6 || maxTaps <= 0 || !validResult(index)) return 0;
        const int cap = std::min(maxTaps, results_.earlyCap);
        const int n = std::min(cap, results_.earlyCount[static_cast<size_t>(index)]);
        const size_t base = static_cast<size_t>(index) * results_.earlyCap;
        for (int t = 0; t < n; ++t) {
            outPos[t] = results_.earlyPos[base + static_cast<size_t>(t)];
            const float* g = &results_.earlyGain[(base + static_cast<size_t>(t)) * kNumBands];
            for (int b = 0; b < kNumBands; ++b) outGain6[t * kNumBands + b] = g[b];
        }
        return n;
    }

    // 回折二次音源。書き込んだ本数を返す。
    int getDiffractionSources(int index, Vec3* outPos, float* outGain, int maxSrc) const {
        if (!outPos || !outGain || maxSrc <= 0 || !validResult(index)) return 0;
        const int cap = std::min(maxSrc, results_.diffCap);
        const int n = std::min(cap, results_.diffCount[static_cast<size_t>(index)]);
        const size_t base = static_cast<size_t>(index) * results_.diffCap;
        for (int t = 0; t < n; ++t) {
            outPos[t] = results_.diffPos[base + static_cast<size_t>(t)];
            outGain[t] = results_.diffGain[base + static_cast<size_t>(t)];
        }
        return n;
    }

    // 帯域別エコグラム。numBins*kNumBands 要素を書く。書けたビン数を返す。
    int getEchogramBands(float* outBins, int numBins) const {
        if (!outBins || numBins <= 0 || results_.echogramBins <= 0) return 0;
        const int n = std::min(numBins, results_.echogramBins);
        for (int i = 0; i < n * kNumBands; ++i) outBins[i] = results_.echogram[static_cast<size_t>(i)];
        return n;
    }

    // --- 参照 ---
    int instanceCount() const { return static_cast<int>(instances_.size()); }
    int materialCount() const { return static_cast<int>(materials_.size()); }

private:
    // update() が置く結果。段5でこれをダブルバッファ化する。
    struct Results {
        int count = 0;
        std::vector<float> occ;     // [count] 遮蔽スカラ
        std::vector<float> bands;   // [count*6] 帯域別生存
        std::vector<float> dir;     // [count*3] 到来方向

        int earlyCap = 0;
        std::vector<Vec3> earlyPos;    // [count*earlyCap]
        std::vector<float> earlyGain;  // [count*earlyCap*6]
        std::vector<int> earlyCount;   // [count]

        int diffCap = 0;
        std::vector<Vec3> diffPos;     // [count*diffCap]
        std::vector<float> diffGain;   // [count*diffCap]
        std::vector<int> diffCount;    // [count]

        int echogramBins = 0;
        std::vector<float> echogram;   // [echogramBins*6]

        void resize(int n, const UpdateConfig& c) {
            const int eCap = (c.earlyTaps > 0) ? c.earlyTaps : 1;
            const int dCap = (c.diffSources > 0) ? c.diffSources : 1;
            if (count == n && earlyCap == eCap && diffCap == dCap &&
                echogramBins == c.echogramBins) return;
            count = n;
            earlyCap = eCap;
            diffCap = dCap;
            echogramBins = c.echogramBins;
            occ.assign(static_cast<size_t>(n), 0.0f);
            bands.assign(static_cast<size_t>(n) * kNumBands, 1.0f);
            dir.assign(static_cast<size_t>(n) * 3, 0.0f);
            earlyPos.assign(static_cast<size_t>(n) * eCap, Vec3(0, 0, 0));
            earlyGain.assign(static_cast<size_t>(n) * eCap * kNumBands, 0.0f);
            earlyCount.assign(static_cast<size_t>(n), 0);
            diffPos.assign(static_cast<size_t>(n) * dCap, Vec3(0, 0, 0));
            diffGain.assign(static_cast<size_t>(n) * dCap, 0.0f);
            diffCount.assign(static_cast<size_t>(n), 0);
            echogram.assign(static_cast<size_t>(std::max(0, c.echogramBins)) * kNumBands, 0.0f);
        }
    };

    bool validResult(int index) const {
        return index >= 0 && index < results_.count;
    }

    void runRole1(int n) {
        if (cfg_.useReflections) {
            occlusionReflectedMulti(listenerPos_, srcScratch_.data(), n,
                                    results_.occ.data(), results_.bands.data(),
                                    results_.dir.data(), cfg_.directWeight,
                                    cfg_.reflectionRays, cfg_.reflectionBounces);
        } else {
            // 反射を使わない場合は直接経路のみ（透過⊕回折）。
            //   ★computeDirectSoft と**同じ規約**を通す。
            //     以前はここで computeTransmission（エネルギー）と
            //     computeDiffraction（振幅）を直接 max で比べていた。単位が違うので
            //     比較になっておらず、透過が二乗ぶん小さく評価されて回折が常に勝ち、
            //     500Hz 以上が材質に関係なく同じ値（実測 0.01156）に張り付いていた。
            //     ポータルも見ていなかったので、閉じた開口でも回折の床が残っていた。
            for (int i = 0; i < n; ++i) {
                float g[kNumBands];
                computeDirectSoft(listenerPos_, srcScratch_[static_cast<size_t>(i)],
                                  g, 8, 0.4f, nullptr);
                float sum = 0.0f;
                for (int b = 0; b < kNumBands; ++b) {
                    results_.bands[static_cast<size_t>(i) * kNumBands + b] = g[b];
                    sum += g[b];
                }
                results_.occ[static_cast<size_t>(i)] = 1.0f - sum / kNumBands;
            }
        }
    }

    void runEarlyReflections(int n) {
        const int cap = results_.earlyCap;
        for (int i = 0; i < n; ++i) {
            const size_t base = static_cast<size_t>(i) * cap;
            const int got = computeEarlyReflections(
                listenerPos_, srcScratch_[static_cast<size_t>(i)],
                &results_.earlyPos[base], &results_.earlyGain[base * kNumBands],
                cap, cfg_.earlyRays, cfg_.earlyBounces);
            results_.earlyCount[static_cast<size_t>(i)] = got;
        }
    }

    void runDiffractionSources(int n) {
        const int cap = results_.diffCap;
        for (int i = 0; i < n; ++i) {
            const size_t base = static_cast<size_t>(i) * cap;
            const int got = computeDiffractionSources(
                listenerPos_, srcScratch_[static_cast<size_t>(i)],
                &results_.diffPos[base], &results_.diffGain[base], cap);
            results_.diffCount[static_cast<size_t>(i)] = got;
        }
    }

    // ポータルを**仮のリスナー**にして、奥側で聞こえているものをエコグラムへ足す。
    //
    // なぜ要るか（実測）:
    //   扉を閉じると、奥の部屋の材質を変えてもこちらのエコグラムが **1ビットも変わらない**
    //     閉・響く 0.07049 / 閉・吸う 0.07049   ← 完全に同じ
    //     開・響く 128.59  / 開・吸う 73.33     ← 1.75倍動く
    //   閉じているとレイが扉で跳ね返され、奥の部屋を一度も訪れないため。
    //   届いているのは直線の透過だけで、それは残響を持たない。
    //   だから「奥で鳴っている音が漏れてくる」ではなく「壁の向こうの点音源」に聞こえる。
    //
    // 何をするか:
    //   ポータルの位置から**奥側の半球だけ**へレイを撒き、そこで聞こえる量を測る。
    //   それに (1−f)·τ_覆っているもの を掛けて、ポータルの位置から手前へ流す。
    //
    //   ★半球に限るのは、全方位に撒くと手前の部屋も拾い、それを手前へ返してしまうため
    //     （自分の音を自分で拾う＝残響が二重に掛かる）。
    //   ★(1−f) を掛けるのは二重計上を避けるため。開いている分のレイは手前のエコグラムが
    //     既に戸口を通って拾っている。ポータルが足すのは**板を抜けてくる分だけ**。
    //     閉じているときだけ効き、開くにつれて役割がレイ側へ移る。合計は連続。
    void addPortalEchogram(const Portal& pt, const Vec3* sources, int count,
                           float* outBins, int numBins, float binSeconds,
                           float speedOfSound, int numRays, int maxBounces,
                           float distanceRef) const {
        using namespace scene_detail;
        if (!outBins || numBins <= 0 || count <= 0 || numRays <= 0) return;

        // 開口率。開いている分は手前のレイが既に拾っているので、その補集合だけ足す。
        float f[kNumBands]; Vec3 cp(0, 0, 0);
        if (!portalOpenBands(pt, listenerPos_, sources[0], f, &cp)) return;

        // 塞いでいるものの透過。ポータルを法線方向に短く貫いて測る。
        Vec3 nrm = normalized(cross(pt.axisU, pt.axisV));
        if (dot(nrm, listenerPos_ - pt.center) < 0.0f) nrm = Vec3(-nrm.x, -nrm.y, -nrm.z);
        float tau[kNumBands];
        computeTransmission(pt.center + nrm * 0.5f, pt.center - nrm * 0.5f, tau);

        float scale[kNumBands];
        bool any = false;
        for (int b = 0; b < kNumBands; ++b) {
            scale[b] = (1.0f - clamp01(f[b])) * clamp01(tau[b]);
            if (scale[b] > 1e-7f) any = true;
        }
        if (!any) return;                       // 板が完全に遮る／全開で既に拾えている

        // ポータル→リスナーの脚。遅延と広がり損失を後で足す。
        const Vec3 origin = pt.center;
        const float legLen = std::max(length(listenerPos_ - origin), 1e-3f);
        const float invC = (speedOfSound > 1e-3f) ? 1.0f / speedOfSound : 0.0f;
        const float invBin = (binSeconds > 1e-6f) ? 1.0f / binSeconds : 0.0f;
        auto spreadEnergy = [distanceRef](float d) -> float {
            if (distanceRef <= 0.0f) return 1.0f;
            const float a = distanceRef / std::max(d, distanceRef);
            return a * a;
        };
        const float legSpread = spreadEnergy(legLen);

        auto addBin = [&](float distFromSource, const float* e6, float extra) {
            const float total = distFromSource + legLen;      // ポータルまで＋こちらへ
            const int k = static_cast<int>(total * invC * invBin);
            if (k < 0 || k >= numBins) return;
            float* dst = outBins + static_cast<size_t>(k) * kNumBands;
            for (int b = 0; b < kNumBands; ++b) {
                const float v = e6[b] * extra * scale[b] * legSpread;
                if (v > 0.0f) dst[b] += v;
            }
        };

        // 直接（奥の音源 → ポータル）。
        for (int j = 0; j < count; ++j) {
            float g[kNumBands];
            computeTransmission(origin, sources[j], g);
            const float d = length(sources[j] - origin);
            addBin(d, g, spreadEnergy(d));
        }

        // 奥側の半球だけへ撒いて、奥の部屋の反射を拾う。
        const float kEps = 1e-3f;
        float refDist = 1e-3f;
        for (int j = 0; j < count; ++j)
            refDist = std::max(refDist, length(sources[j] - origin));
        const float maxDist = refDist * 8.0f + 50.0f;
        for (int i = 0; i < numRays; ++i) {
            Vec3 d = fibonacciSphereDir(i, numRays);
            if (dot(d, nrm) > 0.0f) d = Vec3(-d.x, -d.y, -d.z);   // 奥側へ折り返す
            Vec3 o = origin - nrm * 0.05f;                        // 板の奥側から出す
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 777u;
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
                for (int j = 0; j < count; ++j) {
                    float seg[kNumBands];
                    computeTransmission(q, sources[j], seg);
                    const float pathLen = totalLen + length(sources[j] - q);
                    float e[kNumBands];
                    for (int b = 0; b < kNumBands; ++b) {
                        const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                        e[b] = carry[b] * refl * seg[b] / static_cast<float>(numRays);
                    }
                    addBin(pathLen, e, spreadEnergy(pathLen));
                }
                for (int b = 0; b < kNumBands; ++b) {
                    const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    carry[b] *= refl;
                }
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                o = q;
            }
        }
    }

    void runEchogram(int n) {
        if (results_.echogramBins <= 0) return;
        computeEchogramBands(listenerPos_, srcScratch_.data(), n,
                             results_.echogram.data(), results_.echogramBins,
                             cfg_.echogramBinSeconds, cfg_.speedOfSound,
                             cfg_.echogramRays, cfg_.echogramBounces, cfg_.distanceRef);
        // 閉じた開口の向こうの響きを足す。レイ数は落としてよい ──
        //   要るのは尾の包絡であって細かい構造ではないので（本体の 1/4）。
        for (const Portal& pt : portals_) {
            if (!pt.active) continue;
            addPortalEchogram(pt, srcScratch_.data(), n,
                              results_.echogram.data(), results_.echogramBins,
                              cfg_.echogramBinSeconds, cfg_.speedOfSound,
                              std::max(cfg_.echogramRays / 4, 32), cfg_.echogramBounces,
                              cfg_.distanceRef);
        }
    }

    UpdateConfig cfg_;
    Results results_;
    std::vector<Vec3> srcScratch_;
    int role1Countdown_ = 1;
    int role2Countdown_ = 1;
    int earlyCountdown_ = 1;
    int diffSrcCountdown_ = 1;
    int catalogCountdown_ = 1;

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
    std::vector<MeshGeometry> meshes_;         // 形状(BLAS)。添字が geomId。詰め直さない
    // 開口幅の基準(m)。これより広い開口は素通り、狭いほど通らない。0 で無効。
    //   物理的な境目は波長の半分あたり（500Hz で 0.34m）。耳で決めるチューニング値。
    // 開口の開き具合（回折の音量を決める）。apertureOpenness/apertureOpenGain 参照。
    float apertureOpenRef_ = 0.30f;      // これだけ開いていれば素通しとみなす割合
    float apertureOpenPower_ = 1.0f;     // カーブ。1.0 = 割合に比例
    float apertureOpenRadius_ = 1.0f;    // 断面を見る円盤の半径(m)
    // 既定 0 ＝無効。扉は「ただの壁」として稜線探索に見せるのが本筋で、
    // 開き具合は壁と扉の間にできる開口部の幾何（稜線の位置と δ）がそのまま表す。
    // 別指標として割合を持つのは二重計上になるため、比較用にだけ残す。
    int   apertureOpenSamples_ = 0;      // 円盤のサンプル数。0 で無効化

    // 隙間の幅ゲート（回折の音量を決める）。slitWidthAt/slitWidthGain 参照。
    // 開口の積分で δ（遠回り）をどれだけ効かせるか。1=そのまま / 0=効かせない。
    //   ★δ 減衰は前川の式として**回折タップに既に掛かっている**。開口の測定でも掛けると
    //     二重になる。役割で言えば「どれだけ開いているか」は開口の仕事、
    //     「遠回りでどれだけ減るか」は幾何の仕事なので、本来ここは効かせなくてよいはず。
    //     切り分けを実測するために外から変えられるようにしてある。
    float deltaWeight_ = 1.0f;

    // 開口率 x（0〜1）に掛ける倍率。形を変えず**傾きだけ**を変える。
    //
    //   y = x^p / (x^p + (1-x)^p)      p = apertureContrast_
    //
    //   ★このS字は端点を保つ: y(0)=0, y(1)=1, y(0.5)=0.5、p=1 で恒等。
    //     p を上げると中央付近が急になり、開口の差が音量差として開く。
    //
    //   ★以前は `min((x/ref)^p, 1)` だった。2つ壊れていた:
    //     ① **クランプが幾何の情報を捨てる**。ref=0.278 に対し扉は 40° で既に 0.269 なので
    //        ほぼ全域が飽和側。実測で開口率は 54°→90° に 0.416 → 0.851 と上がり続けるのに、
    //        出力は 54° で 1.0 に張り付いて 90° まで一切変化しなかった。
    //     ② 遮蔽側だけこれを通し、見通し側は生の開口率のままだった。
    //        contrast=1 では両側 0.5 前後で偶然そろっていたが、実機の 4 では
    //        片側だけ飽和して影境界に 0.228（-4.5dB）の段差が出ていた。
    //     S字なら上限に張り付かないので ① が消え、両側に同じものを掛ければ ② が消える。
    //
    //   ★単純な累乗 x^p が駄目だった理由も解消している。開口率は実際には 1 に届かない
    //     （全開でも 0.85 程度）ので、x^p だと上端まで縮んで全体が暗くなる
    //     （実測で全開が 0.0209 → 0.0152）。S字は x が 1 に近づくと y も 1 に近づくので、
    //     上端が沈まない（x=0.85, p=4 で y=0.999）。
    float apertureContrastMap(float x) const {
        if (apertureContrast_ == 1.0f) return 1.0f;
        const float t = scene_detail::clamp01(x);
        const float a = std::pow(std::max(t, 1e-6f), apertureContrast_);
        const float b = std::pow(std::max(1.0f - t, 1e-6f), apertureContrast_);
        const float y = a / (a + b);
        return y / std::max(x, 1e-4f);
    }

    bool  apertureIsTransmission_ = false; // 開口を透過の一部として扱う（既定 OFF）
    // ★既定は**ホスト(Unity)が実際に押している値**に揃える。
    //   以前は 1.0（素通し）で、Unity は 4.0 を毎フレーム押していた。
    //   つまり回帰テストが**出荷しない設定**を守っていて、実機の挙動を誰も検査して
    //   いなかった。実際それが原因で「開口の幅が音量に効かない」と誤診している
    //   （contrast=1 で測っていた。隙間 0.06→2.00m の効きは 1 で +4.7dB、4 で +18.6dB）。
    // 写像を S 字に変えたので、同じ 4.0 だと低角側が急すぎる（扉 45° で -46dB＝無音）。
    //   実測で振った結果 2.0 を採る:
    //     p=1.5  20°で 0.0146（閉じかけでも漏れる）
    //     p=2.0  20°で 0.0036 / 45°で 0.0648 / 90°で 0.8136  ← 採用
    //     p=3〜4 45°で -40dB 以下（半開きが無音）
    float apertureContrast_ = 2.0f;        // 開口率→音量の傾き（1=恒等）
    // ここで素通し(1.0)になる開口率。**全開のときの実測値**に合わせる。
    //   低域加重で畳んだ広帯域スカラは、全開の戸口で 0.278（帯域別 125Hz 0.408〜4kHz 0.094）。
    //   ここを大きく取ると全開でも 1.0 に届かず、幅を開いたつもりが全体が縮む
    //   （0.40 にしていたときは実測で 0.0209 → 0.0155 まで落ちた）。
    float apertureContrastRef_ = 0.278f;

    std::vector<Portal> portals_;          // ホストが置く開口の矩形（トポロジはホストの担当）
    bool  useBtm_ = false;                 // BTM 経路（既定 OFF）
    float btmWedgeAngle_ = 4.712389f;      // 1.5π＝箱の凸稜線

    float slitWidthRef_ = 0.35f;         // これより広ければ素通り(m)。500Hz の半波長あたり
    float slitWidthPower_ = 1.0f;        // カーブ。1.0 = 幅に比例
    // 隙間のハイパスの傾き。1.0 = 6dB/oct。小さくすると音色の変化が穏やかになる
    // （実測に合わせるのではなく、聞かせたい音に合わせて決めてよい値）。
    float slitBandSlope_ = 1.0f;
    // 開口のエネルギーをフレネルゾーンで出すか（false で従来の幅ゲート）。
    bool  useFresnelAperture_ = true;
    // 開口の実効幅を格子で測る設定（apertureWidthByGrid）。0 で無効＝従来の稜線2軸に戻す。
    int   gridSamples_ = 7;      // 片辺のセル数（7 → 49 セル）
    float gridRadius_ = 1.5f;    // 面上で見る半径(m)。戸口を覆う大きさに
    float gridDepth_ = 0.5f;     // 面の前後どれだけ見るか(m)。板が奥にいても捕まえる
    // 開口を面として鳴らす点数。1 で従来どおり（点1つ）。
    int   apertureSpread_ = 1;
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
