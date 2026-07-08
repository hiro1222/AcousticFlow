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
        return static_cast<int>(instances_.size()) - 1;
    }

    // 既存インスタンスの transform を更新する（動いた分だけ＝O(moved) の土台）。
    // 範囲外 id は無視。
    void updateInstanceTransform(int instanceId, const Obb& obb) {
        if (!validInstance(instanceId)) return;
        instances_[instanceId].obb = obb;
    }

    // インスタンスの有効/無効を切り替える。
    void setInstanceActive(int instanceId, bool active) {
        if (!validInstance(instanceId)) return;
        instances_[instanceId].active = active;
    }

    // 全インスタンスを消す（材質テーブルは保持）。毎フレーム作り直す用途。
    void clearInstances() { instances_.clear(); }

    // 材質もインスタンスも全消し。
    void clearAll() { instances_.clear(); materials_.clear(); }

    // --- クエリ ---

    // origin から dir 方向へ最近傍ヒットを返す。dir は内部で正規化する。
    SceneHit raycastClosest(const Vec3& origin, const Vec3& dir, float maxDist) const {
        SceneHit best;
        const Vec3 d = normalized(dir);
        float closest = maxDist;
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
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
        return best;
    }

    // 2点間が何かに遮られているか（二値）。1本でも当たれば遮蔽。
    bool isOccluded(const Vec3& from, const Vec3& to) const {
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
            if (segmentIntersectsObb(from, to, inst.obb)) return true;
        }
        return false;
    }

    // 直線 from->to が通る壁の帯域別透過ゲイン(0..1)を outGain に書く。
    //   壁なし → 全帯域 1.0 / 壁を通るほど（材質次第で高域が）小さくなる。
    // 複数の壁は帯域ごとに透過率を掛け合わせる（＝役割1「材質ベースのこもり」の素）。
    void computeTransmission(const Vec3& from, const Vec3& to,
                             float outGain[kNumBands]) const {
        for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
            if (!segmentIntersectsObb(from, to, inst.obb)) continue;
            const AcousticMaterial& m = materialOf(inst.materialId);
            for (int b = 0; b < kNumBands; ++b) outGain[b] *= m.transmission[b];
        }
    }

    // 【回折(Phase 1.5 暫定・可視化用)】直接 from->to が遮蔽されているとき、箱(OBB)の
    // 稜線を回る「最短迂回」の余剰経路長 δ(= 迂回長 − 直線長, m)を返し、最良の迂回点を
    // outPoint に書く。遮蔽なし or 迂回路なしは -1（outPoint 不定）。
    //   ※ これは本実装 Phase 4（リスナー中心エッジカタログ + UTD）で置き換える前味。
    //     単一回折・箱のみ・稜線を数点サンプルする近似（[[dynamic-ray-architecture]]）。
    float diffractionDetour(const Vec3& from, const Vec3& to, Vec3& outPoint) const {
        if (!isOccluded(from, to)) return -1.0f;
        const float direct = std::max(length(to - from), 1e-4f);
        const float margin = 0.15f;  // 箱を少し膨らませた稜線上を候補に（視線を通しやすく）
        float best = -1.0f;
        Vec3 bestP{0.0f, 0.0f, 0.0f};

        auto tryPoint = [&](const Vec3& P) {
            if (isOccluded(from, P) || isOccluded(P, to)) return;  // 両区間見通せる点だけ
            float d = length(P - from) + length(to - P) - direct;
            if (d < 0.0f) d = 0.0f;
            if (best < 0.0f || d < best) { best = d; bestP = P; }
        };

        // 箱(OBB)の 12 稜線を数点サンプルして回り込み点候補にする（角のみだと薄い壁で失敗）。
        const float ts[3] = {-0.6f, 0.0f, 0.6f};
        const int fixed2[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
            const Obb& b = inst.obb;
            const float h[3] = {b.halfExtents.x + margin, b.halfExtents.y + margin,
                                b.halfExtents.z + margin};
            auto pointAt = [&](const float a[3]) {
                return b.center + b.axisX * (h[0] * a[0]) + b.axisY * (h[1] * a[1]) +
                       b.axisZ * (h[2] * a[2]);
            };
            for (int freeAxis = 0; freeAxis < 3; ++freeAxis) {
                const int o1 = (freeAxis + 1) % 3;
                const int o2 = (freeAxis + 2) % 3;
                for (const auto& sg : fixed2) {
                    for (float t : ts) {
                        float a[3];
                        a[freeAxis] = t;
                        a[o1] = static_cast<float>(sg[0]);
                        a[o2] = static_cast<float>(sg[1]);
                        tryPoint(pointAt(a));
                    }
                }
            }
        }
        if (best >= 0.0f) outPoint = bestP;
        return best;
    }

    // 【回折(Phase 1.5)】from->to の帯域別回折ゲイン(0..1)。
    //   遮蔽なし → 全帯域 1.0 / 迂回路あり → Maekawa（低域ほど回り込む）/ 迂回路なし → 0。
    void computeDiffraction(const Vec3& from, const Vec3& to, float outGain[kNumBands]) const {
        if (!isOccluded(from, to)) {
            for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;
            return;
        }
        Vec3 p;
        const float delta = diffractionDetour(from, to, p);
        if (delta < 0.0f) {
            for (int b = 0; b < kNumBands; ++b) outGain[b] = 0.0f;
            return;
        }
        // Maekawa: フレネル数 N=2δ/λ、減衰(dB)≈10 log10(3+20N)。低域ほど N 小＝よく回り込む。
        const float kSpeed = 343.0f;
        const float bandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
        for (int b = 0; b < kNumBands; ++b) {
            const float lambda = kSpeed / bandFreq[b];
            const float N = 2.0f * delta / lambda;
            const float x = 3.0f + 20.0f * std::max(N, 0.0f);
            const float attDb = 10.0f * std::log10(x);
            outGain[b] = std::pow(10.0f, -attDb / 10.0f);
        }
    }

    // 【役割1(Phase 2)：ソフト遮蔽】直接音を「音源周りの複数サンプル」で測り、遮られた割合を
    // 滑らかに出す（＝半影）。二値の見通し判定と違い、壁の縁をまたぐとき段差でなく連続に変わる。
    //   outGain[b] = 直接透過(ソフト) と 回折フロア(遮蔽割合でフェードイン) の大きい方。
    //   numSamples : 音源周りのサンプル数（例 8） / sourceRadius : サンプル半径(m, 例 0.4)
    // 段差の主因（直接 1.0→材質透過 と 回折 1.0→Maekawa の一斉切替）を連続化する。
    // ※ 影境界の厳密な連続化は Phase 4（UTD 遷移関数）で。ここはその手前の緩和。
    void computeDirectSoft(const Vec3& listener, const Vec3& source, float outGain[kNumBands],
                           int numSamples, float sourceRadius) const {
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

        // 回折フロア（中心が遮蔽のときの Maekawa）を遮蔽割合でフェードイン。
        float dif[kNumBands];
        computeDiffraction(listener, source, dif);  // 中心非遮蔽なら全1.0
        const bool centerOcc = isOccluded(listener, source);
        for (int b = 0; b < kNumBands; ++b) {
            const float soft = transAccum[b] * inv;                       // 滑らかな直接透過
            const float diffFloor = centerOcc ? dif[b] * occFrac : 0.0f;  // 影で徐々に立つ回折
            outGain[b] = clamp01(std::max(soft, diffFloor));
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
        computeDirectSoft(listener, source, total, 8, 0.4f);

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
    void occlusionReflectedMulti(const Vec3& listener, const Vec3* sources, int count,
                                 float* outOcc, float* outBands,
                                 int numRays, int maxBounces) const {
        using namespace scene_detail;
        if (!sources || count <= 0) return;
        const float kEps = 1e-3f;
        std::vector<float> total(static_cast<size_t>(count) * kNumBands);
        std::vector<float> reflected(static_cast<size_t>(count) * kNumBands, 0.0f);
        std::vector<float> refDist(static_cast<size_t>(count));

        // 1) 直接（音源ごと）＝ソフト遮蔽（半影で連続化）。
        for (int j = 0; j < count; ++j) {
            computeDirectSoft(listener, sources[j], &total[j * kNumBands], 8, 0.4f);
            refDist[j] = std::max(length(sources[j] - listener), 1e-3f);
        }

        // 2) 反射（リスナーレイは1回だけ＝音源数非依存）。
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
        using namespace scene_detail;
        if (!outBins || numBins <= 0 || !sources || count <= 0) return;
        for (int k = 0; k < numBins; ++k) outBins[k] = 0.0f;

        const float kEps = 1e-3f;
        const float invC = (speedOfSound > 1e-3f) ? 1.0f / speedOfSound : 0.0f;
        const float invBin = (binSeconds > 1e-6f) ? 1.0f / binSeconds : 0.0f;

        auto addBin = [&](float dist, float energy) {
            if (energy <= 0.0f) return;
            const int k = static_cast<int>(dist * invC * invBin);
            if (k < 0 || k >= numBins) return;
            outBins[k] += energy;
        };

        // 直接音（直線の透過・広帯域平均）。
        for (int j = 0; j < count; ++j) {
            float g[kNumBands];
            computeTransmission(listener, sources[j], g);
            float mean = 0.0f;
            for (int b = 0; b < kNumBands; ++b) mean += g[b];
            addBin(length(sources[j] - listener), mean / kNumBands);
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
                        const float pathLen = totalLen + length(sources[j] - q);
                        float e = 0.0f;
                        for (int b = 0; b < kNumBands; ++b) e += carry[b] * refl[b] * seg[b];
                        addBin(pathLen, (e / kNumBands) * inv);
                    }
                    for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
        }
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

    std::vector<AcousticMaterial> materials_;  // 材質テーブル（インスタンスが matId で参照）
    std::vector<Instance> instances_;          // 占有物（毎フレーム更新可能）
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_SCENE_H
