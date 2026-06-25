/* Core/acoustic_world.cpp
 * AcousticWorld の実装。
 */
#include "Core/acoustic_world.h"

#include <algorithm>  // std::max
#include <cmath>      // std::sin, std::cos, std::acos, std::sqrt

namespace acoustic {

namespace {

inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// フィボナッチ球：i 番目の方向（球面上にほぼ均等な n 方向）。
// Unity 側の可視化レイと同じ分布。音用の一括レイ散布に使う。
Vec3 fibonacciSphereDir(int i, int n) {
    const float kPi = 3.14159265358979f;
    const float k = static_cast<float>(i) + 0.5f;
    const float phi = std::acos(1.0f - 2.0f * k / static_cast<float>(n));
    const float theta = kPi * (1.0f + std::sqrt(5.0f)) * k;
    const float s = std::sin(phi);
    return Vec3(s * std::cos(theta), std::cos(phi), s * std::sin(theta));
}

}  // namespace

void AcousticWorld::addBox(const Vec3& center, const Vec3& halfExtents) {
    addBox(center, halfExtents, AcousticMaterial::defaultWall());
}

void AcousticWorld::addBox(const Vec3& center, const Vec3& halfExtents,
                           const AcousticMaterial& material) {
    occluders_.push_back(Occluder{
        Aabb::fromCenterHalfExtents(center, halfExtents), material});
}

void AcousticWorld::clearGeometry() {
    occluders_.clear();
}

bool AcousticWorld::isOccluded(const Vec3& from, const Vec3& to) const {
    // どれか1つでも線分を遮る箱があれば遮蔽とみなす。
    // 今は素朴な全件走査。障害物が増えたら BVH で高速化する（夏休みの工程）。
    for (const Occluder& o : occluders_) {
        if (segmentIntersectsAabb(from, to, o.box)) {
            return true;
        }
    }
    return false;
}

void AcousticWorld::computeTransmission(const Vec3& from, const Vec3& to,
                                        float outGain[kNumBands]) const {
    // 何も無ければ全帯域素通り(1.0)。
    for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;

    // 直線上にある壁ごとに、その材質の透過率を帯域別に掛けていく。
    // （厚みは未考慮＝壁1枚で係数1回。厚み対応は今後 tmin/tmax で。）
    for (const Occluder& o : occluders_) {
        if (segmentIntersectsAabb(from, to, o.box)) {
            for (int b = 0; b < kNumBands; ++b) {
                outGain[b] *= o.material.transmission[b];
            }
        }
    }
}

float AcousticWorld::occlusionScalar(const Vec3& from, const Vec3& to) const {
    float gain[kNumBands];
    computeTransmission(from, to, gain);

    float mean = 0.0f;
    for (int b = 0; b < kNumBands; ++b) mean += gain[b];
    mean /= kNumBands;

    // 生き残り平均が小さいほど遮蔽が強い。occlusion = 1 - 透過。
    float occ = 1.0f - mean;
    if (occ < 0.0f) occ = 0.0f;
    if (occ > 1.0f) occ = 1.0f;
    return occ;
}

int AcousticWorld::computeFaceReachability(const Vec3& center, const Vec3& normal,
                                           const Vec3& rightAxis, float halfW, float halfH,
                                           int shape, const Vec3& listener,
                                           float* outGrid, int cols, int rows) const {
    if (outGrid == nullptr || cols <= 0 || rows <= 0) return 0;

    // 面のローカル基底を作る。right は法線成分を抜いて面内に正規化し、
    // up は normal×right で直交させる（入力の right が多少ずれても面内に収まる）。
    const Vec3 n = normalized(normal);
    Vec3 right = rightAxis - n * dot(rightAxis, n);
    right = normalized(right);
    const Vec3 up = normalized(cross(n, right));

    for (int r = 0; r < rows; ++r) {
        // v: 上(+1) → 下(-1)。タブのヒートマップの行方向と一致させる。
        const float v = (rows == 1) ? 0.0f
                                    : 1.0f - 2.0f * (static_cast<float>(r) + 0.5f) / rows;
        for (int c = 0; c < cols; ++c) {
            const float u = (cols == 1) ? 0.0f
                                        : -1.0f + 2.0f * (static_cast<float>(c) + 0.5f) / cols;
            const int idx = r * cols + c;

            // 円形の面は単位円の外を「面の外」として -1 で塗る（タブ側で透明扱い）。
            if (shape == 1 && (u * u + v * v) > 1.0f) {
                outGrid[idx] = -1.0f;
                continue;
            }

            // 面上の世界座標 → リスナーへの直接透過（帯域別）の広帯域平均。
            const Vec3 p = center + right * (u * halfW) + up * (v * halfH);
            float gain[kNumBands];
            computeTransmission(p, listener, gain);
            float mean = 0.0f;
            for (int b = 0; b < kNumBands; ++b) mean += gain[b];
            outGrid[idx] = mean / kNumBands;  // 1=届く / 0=遮断
        }
    }
    return cols * rows;
}

float AcousticWorld::occlusionScalarRayIntegrated(const Vec3& source, const Vec3& listener,
                                                  int numRays, int maxBounces) const {
    const float kEps = 1e-3f;
    const float refDist = std::max(length(source - listener), 1e-3f);

    // 1) 直接経路：直線で壁を透過した生存率（既存と同じ）。これが土台。
    float total[kNumBands];
    computeTransmission(listener, source, total);

    // 2) 反射で回り込む成分を、リスナー起点レイ＋next-event推定で足す。
    //    各レイは壁で反射しながらエネルギーを失い、各バウンス点で音源へ「つなぐ」。
    if (numRays > 0 && maxBounces > 0 && !occluders_.empty()) {
        float reflected[kNumBands] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        // レイの打ち切り距離（部屋を覆う程度。直接距離基準で適当に確保）。
        const float maxDist = refDist * 8.0f + 50.0f;

        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            float carry[kNumBands] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // 各帯域の残存エネルギー
            float totalLen = 0.0f;
            float remaining = maxDist;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const RayHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.distance;
                remaining -= hit.distance;
                if (remaining <= kEps) break;

                const AcousticMaterial& mat = occluders_[hit.boxIndex].material;

                // next-event：このバウンス点から音源へ直接つなぐ。
                // 面から少し浮かせて自己ヒットを防ぐ。途中に別の壁があれば透過で減衰。
                const Vec3 q = hit.point + hit.normal * 0.02f;
                float seg[kNumBands];
                computeTransmission(q, source, seg);

                // 経路長（リスナー→…→q→音源）。直接経路長を基準にした相対の逆2乗で減衰。
                // 絶対距離の減衰は Wwise 側が別途やるので、ここは「遠回りほど弱い」を表す相対量。
                const float pathLen = totalLen + length(source - q);
                float atten = refDist / pathLen;
                atten *= atten;
                if (atten > 1.0f) atten = 1.0f;

                for (int b = 0; b < kNumBands; ++b) {
                    // 反射率 = 1 - 吸収 - 透過（負はクランプ）。
                    const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    reflected[b] += carry[b] * refl * seg[b] * atten;
                    carry[b] *= refl;  // 継続レイは反射のたびにエネルギーを失う
                }

                // 鏡面反射して次のバウンスへ。
                d = reflect(d, hit.normal);
                o = q;
            }
        }

        // レイ本数で平均し、直接成分に足し込む（0..1にクランプ）。
        const float inv = 1.0f / static_cast<float>(numRays);
        for (int b = 0; b < kNumBands; ++b) {
            total[b] = clamp01(total[b] + reflected[b] * inv);
        }
    }

    // 広帯域平均 → occlusion = 1 - 生存。
    float mean = 0.0f;
    for (int b = 0; b < kNumBands; ++b) mean += total[b];
    mean /= kNumBands;
    return clamp01(1.0f - mean);
}

void AcousticWorld::occlusionScalarMultiSource(const Vec3& listener,
                                               const Vec3* sources, int count,
                                               float* outOcc, int numRays,
                                               int maxBounces) const {
    if (sources == nullptr || outOcc == nullptr || count <= 0) return;
    const float kEps = 1e-3f;

    // 音源ごと×帯域の生存エネルギー（直接＋反射）。count 可変なので vector（境界外なのでOK）。
    std::vector<float> total(static_cast<size_t>(count) * kNumBands);
    std::vector<float> reflected(static_cast<size_t>(count) * kNumBands, 0.0f);
    std::vector<float> refDist(static_cast<size_t>(count));

    // 1) 直接経路（音源ごと）。
    for (int j = 0; j < count; ++j) {
        float dir[kNumBands];
        computeTransmission(listener, sources[j], dir);
        for (int b = 0; b < kNumBands; ++b) total[j * kNumBands + b] = dir[b];
        refDist[j] = std::max(length(sources[j] - listener), 1e-3f);
    }

    // 2) 反射成分。リスナーレイは1回だけ撒く（raycast＝音源数に依存しない）。
    if (numRays > 0 && maxBounces > 0 && !occluders_.empty()) {
        float maxRef = 1e-3f;
        for (int j = 0; j < count; ++j) maxRef = std::max(maxRef, refDist[j]);
        const float maxDist = maxRef * 8.0f + 50.0f;

        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            float carry[kNumBands] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            float totalLen = 0.0f;
            float remaining = maxDist;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const RayHit hit = raycastClosest(o, d, remaining);  // ← 共有（音源数に無関係）
                if (!hit.hit) break;
                totalLen += hit.distance;
                remaining -= hit.distance;
                if (remaining <= kEps) break;

                const AcousticMaterial& mat = occluders_[hit.boxIndex].material;
                const Vec3 q = hit.point + hit.normal * 0.02f;

                // 反射率（帯域別・音源非依存）。
                float refl[kNumBands];
                for (int b = 0; b < kNumBands; ++b)
                    refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);

                // 各音源へ next-event（ここだけ音源数ぶん）。
                for (int j = 0; j < count; ++j) {
                    float seg[kNumBands];
                    computeTransmission(q, sources[j], seg);
                    const float pathLen = totalLen + length(sources[j] - q);
                    float atten = refDist[j] / pathLen;
                    atten *= atten;
                    if (atten > 1.0f) atten = 1.0f;
                    for (int b = 0; b < kNumBands; ++b)
                        reflected[j * kNumBands + b] += carry[b] * refl[b] * seg[b] * atten;
                }

                // carry 更新（音源非依存）＋鏡面反射で次へ。
                for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];
                d = reflect(d, hit.normal);
                o = q;
            }
        }

        const float inv = 1.0f / static_cast<float>(numRays);
        for (int j = 0; j < count; ++j)
            for (int b = 0; b < kNumBands; ++b)
                total[j * kNumBands + b] =
                    clamp01(total[j * kNumBands + b] + reflected[j * kNumBands + b] * inv);
    }

    // 3) 音源ごとに広帯域平均 → occlusion = 1 - 生存。
    for (int j = 0; j < count; ++j) {
        float mean = 0.0f;
        for (int b = 0; b < kNumBands; ++b) mean += total[j * kNumBands + b];
        mean /= kNumBands;
        outOcc[j] = clamp01(1.0f - mean);
    }
}

void AcousticWorld::computeEchogram(const Vec3& listener, const Vec3* sources, int count,
                                    int numRays, int maxBounces,
                                    float* outBins, int numBins,
                                    float binSeconds, float speedOfSound) const {
    if (outBins == nullptr || numBins <= 0 || sources == nullptr || count <= 0) return;
    for (int k = 0; k < numBins; ++k) outBins[k] = 0.0f;

    const float kEps = 1e-3f;
    const float invC = (speedOfSound > 1e-3f) ? 1.0f / speedOfSound : 0.0f;
    const float invBin = (binSeconds > 1e-6f) ? 1.0f / binSeconds : 0.0f;

    // 距離→到達時間→ビンに energy を積む小ヘルパ。窓外は捨てる。
    auto addBin = [&](float dist, float energy) {
        if (energy <= 0.0f) return;
        const int k = static_cast<int>(dist * invC * invBin);
        if (k < 0 || k >= numBins) return;
        outBins[k] += energy;
    };

    // 直接音（直線の透過。広帯域平均）。
    for (int j = 0; j < count; ++j) {
        float g[kNumBands];
        computeTransmission(listener, sources[j], g);
        float mean = 0.0f;
        for (int b = 0; b < kNumBands; ++b) mean += g[b];
        addBin(length(sources[j] - listener), mean / kNumBands);
    }

    // 反射（リスナーレイ1回＝共有 / 各バウンス点から全音源へ next-event）。
    if (numRays > 0 && maxBounces > 0 && !occluders_.empty()) {
        float maxRef = 1e-3f;
        for (int j = 0; j < count; ++j)
            maxRef = std::max(maxRef, std::max(length(sources[j] - listener), 1e-3f));
        const float maxDist = maxRef * 8.0f + 50.0f;
        // 反射場は「球面全方向から来る到達」を積分したもの。N本のレイは球面(4π sr)を
        // 均一サンプルするので、各レイは 4π/N ステラジアンを代表する。
        // 直接音は1方向の1到達なので係数1。反射は立体角で積分する必要があり、
        // ここを単なる 1/N（平均ラジアンス）にすると反射が約1桁過小評価され、
        // 残響が乾き・拡散場が立たず片耳collapseも残る（不具合の根本原因）。
        // リスナーは前方の入射場を主に受けるので半球 2π を採用。強すぎれば下げて調整可。
        const float kDiffuseCoupling = 6.2831853f;  // 2π。反射場を立体角で積分する係数
        const float inv = kDiffuseCoupling / static_cast<float>(numRays);

        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            float carry[kNumBands] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            float totalLen = 0.0f;
            float remaining = maxDist;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const RayHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.distance;
                remaining -= hit.distance;
                if (remaining <= kEps) break;

                const AcousticMaterial& mat = occluders_[hit.boxIndex].material;
                const Vec3 q = hit.point + hit.normal * 0.02f;
                float refl[kNumBands];
                for (int b = 0; b < kNumBands; ++b)
                    refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);

                for (int j = 0; j < count; ++j) {
                    float seg[kNumBands];
                    computeTransmission(q, sources[j], seg);
                    // 到達距離（音源→…→q→リスナー、可逆なので listener 起点長と同じ）。
                    const float pathLen = totalLen + length(sources[j] - q);
                    const float refDist = std::max(length(sources[j] - listener), 1e-3f);
                    float atten = refDist / pathLen;
                    atten *= atten;
                    if (atten > 1.0f) atten = 1.0f;
                    float e = 0.0f;
                    for (int b = 0; b < kNumBands; ++b) e += carry[b] * refl[b] * seg[b] * atten;
                    addBin(pathLen, (e / kNumBands) * inv);  // 到達時間ビンへ。レイ本数で平均
                }

                for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];
                d = reflect(d, hit.normal);
                o = q;
            }
        }
    }
}

RayHit AcousticWorld::raycastClosest(const Vec3& origin, const Vec3& dir,
                                     float maxDist) const {
    const Vec3 ndir = normalized(dir);

    RayHit best;
    best.hit = false;
    best.distance = maxDist;

    // 全障害物を調べ、最も近いヒットを採用する。
    // best.distance を上限に渡すことで、それより遠いヒットは枝刈りされる。
    for (int i = 0; i < static_cast<int>(occluders_.size()); ++i) {
        float t = 0.0f;
        Vec3 n;
        if (rayIntersectsAabb(origin, ndir, occluders_[i].box, best.distance, t, n)) {
            if (t <= best.distance) {
                best.hit = true;
                best.distance = t;
                best.normal = n;
                best.boxIndex = i;
                best.point = origin + ndir * t;
            }
        }
    }
    return best;
}

int AcousticWorld::traceReflectionPath(const Vec3& origin, const Vec3& dir,
                                       float maxDist, int maxBounces,
                                       Vec3* outPoints, int maxPoints) const {
    if (outPoints == nullptr || maxPoints <= 0) return 0;

    int count = 0;
    outPoints[count++] = origin;       // 始点

    Vec3  o = origin;
    Vec3  d = normalized(dir);
    float remaining = maxDist;          // 残り飛距離の予算
    const float kEps = 1e-3f;           // 面から浮かせる量（自己ヒット防止）

    for (int bounce = 0; bounce <= maxBounces; ++bounce) {
        if (count >= maxPoints) break;

        const RayHit hit = raycastClosest(o, d, remaining);
        if (!hit.hit) {
            // 何にも当たらず開放空間で終端する。
            outPoints[count++] = o + d * remaining;
            break;
        }

        // 壁にヒット。反射点を記録。
        outPoints[count++] = hit.point;
        remaining -= hit.distance;

        // これ以上反射しない / 予算切れ / バッファ満杯 なら終了。
        if (bounce == maxBounces || remaining <= kEps || count >= maxPoints) break;

        // 鏡面反射して次のレイへ。面法線方向に少し浮かせて自己ヒットを防ぐ。
        d = reflect(d, hit.normal);
        o = hit.point + hit.normal * kEps;
    }
    return count;
}

}  // namespace acoustic
