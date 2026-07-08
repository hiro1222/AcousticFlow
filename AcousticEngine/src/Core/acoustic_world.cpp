/* Core/acoustic_world.cpp
 * AcousticWorld の実装。
 */
#include "Core/acoustic_world.h"

#include <algorithm>  // std::max
#include <cmath>      // std::sin, std::cos, std::acos, std::sqrt
#include <cstdint>    // uint32_t

namespace acoustic {

namespace {

constexpr float kPiF = 3.14159265358979f;

inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// フィボナッチ球(一番いい感じの分散）
Vec3 fibonacciSphereDir(int i, int n) {
    const float k = static_cast<float>(i) + 0.5f;
    const float phi = std::acos(1.0f - 2.0f * k / static_cast<float>(n));
    const float theta = kPiF * (1.0f + std::sqrt(5.0f)) * k;
    const float s = std::sin(phi);
    return Vec3(s * std::cos(theta), std::cos(phi), s * std::sin(theta));
}

// 軽量ハッシュ乱数（PCG系）。レイ×バウンスごとに散乱方向を引くのに使う。
inline float hashRand01(uint32_t& state) {
    state = state * 747796405u + 2891336453u;
    uint32_t w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    w = (w >> 22) ^ w;
    return static_cast<float>(w) * (1.0f / 4294967296.0f);
}

// 法線 n まわりの余弦重み半球サンプル（ランバート拡散）。
Vec3 cosineHemisphere(const Vec3& n, uint32_t& rng) {
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

// scattering(0..1) で反射方向を鏡面⇄拡散に振り分ける（当初設計の②ミクロ）。
// 確率的散乱：確率 s で完全拡散（ランバート）、それ以外は鏡面。多数レイで平均すると
// 「反射エネルギーのうち割合 s が拡散」を再現し、鏡面の周期軌道を崩して拡散場を立てる。
// 方向を変えるだけ＝carry の更新(refl^n)は不変（余弦重みサンプルは pdf が cos を打ち消す）。
Vec3 scatteredDir(const Vec3& d, const Vec3& n, float s, uint32_t& rng) {
    if (s > 0.0f && hashRand01(rng) < s) return cosineHemisphere(n, rng);  // 拡散
    return reflect(d, n);                                                  // 鏡面
}

// 材質の scattering の広帯域平均（反射“方向”はブロードバンドで1つ決める）。
inline float scatteringMean(const AcousticMaterial& m) {
    float s = 0.0f;
    for (int b = 0; b < kNumBands; ++b) s += m.scattering[b];
    return s / static_cast<float>(kNumBands);
}

}

void AcousticWorld::addBox(const Vec3& center, const Vec3& halfExtents) {
    addBox(center, halfExtents, AcousticMaterial::defaultWall());
}

void AcousticWorld::addBox(const Vec3& center, const Vec3& halfExtents,
                           const AcousticMaterial& material) {
    boxes_.push_back(BoxOccluder{
        Obb::axisAligned(center, halfExtents), material});
}

void AcousticWorld::addBoxOriented(const Vec3& center, const Vec3& halfExtents,
                                   const Vec3& right, const Vec3& up) {
    addBoxOriented(center, halfExtents, right, up, AcousticMaterial::defaultWall());
}

void AcousticWorld::addBoxOriented(const Vec3& center, const Vec3& halfExtents,
                                   const Vec3& right, const Vec3& up,
                                   const AcousticMaterial& material) {
    boxes_.push_back(BoxOccluder{
        Obb::oriented(center, halfExtents, right, up), material});
}

int AcousticWorld::addMesh(const float* verticesXYZ, int vertexCount,
                           const int* indices, int indexCount,
                           const AcousticMaterial& material) {
    if (verticesXYZ == nullptr || indices == nullptr ||
        vertexCount <= 0 || indexCount < 3) {
        return -1;
    }
    // インデックス列から三角形を組み立てる（範囲外インデックスは捨てる）。
    std::vector<Triangle> tris;
    tris.reserve(static_cast<size_t>(indexCount / 3));
    for (int i = 0; i + 2 < indexCount; i += 3) {
        const int a = indices[i];
        const int b = indices[i + 1];
        const int c = indices[i + 2];
        if (a < 0 || b < 0 || c < 0 ||
            a >= vertexCount || b >= vertexCount || c >= vertexCount) {
            continue;
        }
        Triangle t;
        t.v0 = Vec3(verticesXYZ[a * 3], verticesXYZ[a * 3 + 1], verticesXYZ[a * 3 + 2]);
        t.v1 = Vec3(verticesXYZ[b * 3], verticesXYZ[b * 3 + 1], verticesXYZ[b * 3 + 2]);
        t.v2 = Vec3(verticesXYZ[c * 3], verticesXYZ[c * 3 + 1], verticesXYZ[c * 3 + 2]);
        tris.push_back(t);
    }
    if (tris.empty()) return -1;

    meshes_.push_back(MeshOccluder{});
    MeshOccluder& m = meshes_.back();
    m.material = material;
    m.active = true;
    m.bvh.build(std::move(tris));  // ここで BVH 構築（一度きり）
    return static_cast<int>(meshes_.size()) - 1;  // ID = 追加した位置
}

void AcousticWorld::setMeshActive(int meshId, bool active) {
    if (meshId < 0 || meshId >= static_cast<int>(meshes_.size())) return;
    meshes_[static_cast<size_t>(meshId)].active = active;
}

void AcousticWorld::clearGeometry() {
    boxes_.clear();  // 動的な箱のみ。メッシュは残す。
}

void AcousticWorld::clearMeshes() {
    meshes_.clear();
}

bool AcousticWorld::isOccluded(const Vec3& from, const Vec3& to) const {
    // どれか1つでも線分を遮る障害物があれば遮蔽とみなす。
    // 箱は全件走査（数が少ない前提）、メッシュは各自の BVH で判定。
    for (const BoxOccluder& o : boxes_) {
        if (segmentIntersectsObb(from, to, o.box)) return true;
    }
    for (const MeshOccluder& m : meshes_) {
        if (!m.active) continue;
        if (m.bvh.occludes(from, to)) return true;
    }
    return false;
}

void AcousticWorld::computeTransmission(const Vec3& from, const Vec3& to,
                                        float outGain[kNumBands]) const {
    // 何も無ければ全帯域素通り(1.0)。
    for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;

    // 直線上にある箱ごとに、その材質の透過率を帯域別に掛けていく。
    // （厚みは未考慮＝箱1枚で係数1回。厚み対応は今後 tmin/tmax で。）
    for (const BoxOccluder& o : boxes_) {
        if (segmentIntersectsObb(from, to, o.box)) {
            for (int b = 0; b < kNumBands; ++b) {
                outGain[b] *= o.material.transmission[b];
            }
        }
    }
    // メッシュは「横切った三角形の枚数ぶん」透過を掛ける（閉じたメッシュを貫くと
    // 表裏で2枚＝transmission^2。厚みのある壁を通ったのに近い近似）。
    for (const MeshOccluder& m : meshes_) {
        if (!m.active) continue;
        const int crossings = m.bvh.countCrossings(from, to);
        for (int k = 0; k < crossings; ++k) {
            for (int b = 0; b < kNumBands; ++b) {
                outGain[b] *= m.material.transmission[b];
            }
        }
    }
}

float AcousticWorld::diffractionDetour(const Vec3& from, const Vec3& to) const {
    if (!isOccluded(from, to)) return -1.0f;  // 見通せるなら回折不要
    const float direct = std::max(length(to - from), 1e-4f);
    const float margin = 0.15f;  // 箱を少し膨らませた稜線上を候補にして視線を通しやすく
    float best = -1.0f;

    auto tryPoint = [&](const Vec3& P) {
        // from→P→to の両区間が見通せる点だけ有効な回折経路。
        if (isOccluded(from, P) || isOccluded(P, to)) return;
        float d = length(P - from) + length(to - P) - direct;
        if (d < 0.0f) d = 0.0f;
        if (best < 0.0f || d < best) best = d;
    };

    // Phase1: 箱(OBB)の 12 稜線を数点サンプルして回り込み点候補にする。
    //   角だけだと薄い壁の側面をかすめて失敗するので稜線上を刻む。膨らませた箱の稜線を使い、
    //   視線が実ジオメトリをかすめても通るようにする（薄い衝立の単一回折が対象。厚い壁の
    //   二重回折は将来）。メッシュは未対応（AABB 隅近似は後で）。
    const float ts[3] = {-0.6f, 0.0f, 0.6f};
    const int fixed2[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    for (const BoxOccluder& o : boxes_) {
        const Obb& b = o.box;
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
    return best;  // 有効な稜線点が無ければ -1
}

void AcousticWorld::computeDiffraction(const Vec3& from, const Vec3& to,
                                       float outGain[kNumBands]) const {
    // 遮蔽なし → 直接が素通り（回折ゲイン=1）。
    if (!isOccluded(from, to)) {
        for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;
        return;
    }
    const float delta = diffractionDetour(from, to);
    if (delta < 0.0f) {  // 迂回路が見つからない（完全に囲まれた等）
        for (int b = 0; b < kNumBands; ++b) outGain[b] = 0.0f;
        return;
    }
    // Maekawa 近似：フレネル数 N = 2δ/λ、減衰(dB) ≈ 10 log10(3 + 20N)。
    //   N=0（影の境界）で ~4.8dB（半分）、N が大きいほど（高域ほど）強く減衰。
    const float kSpeed = 343.0f;
    const float bandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
    for (int b = 0; b < kNumBands; ++b) {
        const float lambda = kSpeed / bandFreq[b];
        const float N = 2.0f * delta / lambda;
        float attDb;
        if (N <= -0.2f) {
            attDb = 0.0f;  // 明域（回折不要）
        } else {
            const float x = 3.0f + 20.0f * std::max(N, 0.0f);
            attDb = 10.0f * std::log10(x);
        }
        outGain[b] = std::pow(10.0f, -attDb / 10.0f);  // dB → 線形エネルギー
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

    // 1) 直接経路＝透過（壁を貫く）＋回折（角を回り込む）の並列2経路。
    float total[kNumBands];
    float dif[kNumBands];
    computeTransmission(listener, source, total);
    computeDiffraction(listener, source, dif);  // 非遮蔽なら全帯域1.0
    for (int b = 0; b < kNumBands; ++b) total[b] = clamp01(total[b] + dif[b]);

    // 2) 反射で回り込む成分を、リスナー起点レイ＋next-event推定で足す。
    //    各レイは壁で反射しながらエネルギーを失い、各バウンス点で音源へ「つなぐ」。
    if (numRays > 0 && maxBounces > 0 && (!boxes_.empty() || !meshes_.empty())) {
        float reflected[kNumBands] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        // レイの打ち切り距離（部屋を覆う程度。直接距離基準で適当に確保）。
        const float maxDist = refDist * 8.0f + 50.0f;

        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;  // 散乱方向用の種
            float carry[kNumBands] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // 各帯域の残存エネルギー
            float totalLen = 0.0f;
            float remaining = maxDist;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const RayHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.distance;
                remaining -= hit.distance;
                if (remaining <= kEps) break;

                const AcousticMaterial& mat = *hit.material;

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

                // 反射方向を scattering で鏡面⇄拡散にブレンド（当初設計②ミクロ）。
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
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

    // 1) 直接経路（音源ごと）＝ 透過（壁を貫く）＋ 回折（角を回り込む）の並列2経路。
    //    遮蔽されても回折で低域が生き残るので、壁裏でも音が完全には消えない。
    for (int j = 0; j < count; ++j) {
        float dir[kNumBands];
        float dif[kNumBands];
        computeTransmission(listener, sources[j], dir);
        computeDiffraction(listener, sources[j], dif);  // 非遮蔽なら全帯域1.0
        for (int b = 0; b < kNumBands; ++b)
            total[j * kNumBands + b] = clamp01(dir[b] + dif[b]);
        refDist[j] = std::max(length(sources[j] - listener), 1e-3f);
    }

    // 2) 反射成分。リスナーレイは1回だけ撒く（raycast＝音源数に依存しない）。
    if (numRays > 0 && maxBounces > 0 && (!boxes_.empty() || !meshes_.empty())) {
        float maxRef = 1e-3f;
        for (int j = 0; j < count; ++j) maxRef = std::max(maxRef, refDist[j]);
        const float maxDist = maxRef * 8.0f + 50.0f;

        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;  // 散乱方向用の種
            float carry[kNumBands] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            float totalLen = 0.0f;
            float remaining = maxDist;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const RayHit hit = raycastClosest(o, d, remaining);  // ← 共有（音源数に無関係）
                if (!hit.hit) break;
                totalLen += hit.distance;
                remaining -= hit.distance;
                if (remaining <= kEps) break;

                const AcousticMaterial& mat = *hit.material;
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
                // 反射方向を scattering で鏡面⇄拡散にブレンド（当初設計②ミクロ）。
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
    if (numRays > 0 && maxBounces > 0 && (!boxes_.empty() || !meshes_.empty())) {
        float maxRef = 1e-3f;
        for (int j = 0; j < count; ++j)
            maxRef = std::max(maxRef, std::max(length(sources[j] - listener), 1e-3f));
        // レイ打切り距離は「エコグラム窓の終端に届く距離」まで伸ばす（尾が窓内に入るように）。
        // window秒 = numBins*binSeconds、その距離 = window*speedOfSound。短いと尾が切れて RT60 過小。
        const float windowDist = static_cast<float>(numBins) * binSeconds * speedOfSound;
        const float maxDist = std::max(maxRef * 8.0f + 50.0f, windowDist);
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
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;  // 散乱方向用の種
            float carry[kNumBands] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            float totalLen = 0.0f;
            float remaining = maxDist;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const RayHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.distance;
                remaining -= hit.distance;
                if (remaining <= kEps) break;

                const AcousticMaterial& mat = *hit.material;
                const Vec3 q = hit.point + hit.normal * 0.02f;
                float refl[kNumBands];
                for (int b = 0; b < kNumBands; ++b)
                    refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);

                for (int j = 0; j < count; ++j) {
                    float seg[kNumBands];
                    computeTransmission(q, sources[j], seg);
                    // 到達距離＝到達時間の位置決めに使う（音源→…→q→リスナー）。
                    const float pathLen = totalLen + length(sources[j] - q);
                    // ★残響の減衰は吸収 refl^n（carry）が担う。絶対距離の 1/r² は掛けない。
                    //   エコグラムは減衰の“形”＝相対量で、絶対距離減衰は Wwise 側の責務。
                    //   直接音も 1/r² を掛けていないので、反射だけ全経路 1/r² を掛けると
                    //   後期反射（長経路）が過剰に潰れ RT60 が非物理に短くなる（較正で判明）。
                    float e = 0.0f;
                    for (int b = 0; b < kNumBands; ++b) e += carry[b] * refl[b] * seg[b];
                    addBin(pathLen, (e / kNumBands) * inv);  // 到達時間ビンへ。レイ本数で平均
                }

                for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];
                // 反射方向を scattering で鏡面⇄拡散にブレンド（当初設計②ミクロ）。
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
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

    // 箱を全件、メッシュを各 BVH で調べ、最も近いヒットを採用する。
    // best.distance を上限に渡すことで、それより遠いヒットは枝刈りされる。
    for (int i = 0; i < static_cast<int>(boxes_.size()); ++i) {
        float t = 0.0f;
        Vec3 n;
        if (rayIntersectsObb(origin, ndir, boxes_[i].box, best.distance, t, n)) {
            if (t <= best.distance) {
                best.hit = true;
                best.distance = t;
                best.normal = n;
                best.boxIndex = i;
                best.material = &boxes_[i].material;
                best.point = origin + ndir * t;
            }
        }
    }
    for (const MeshOccluder& m : meshes_) {
        if (!m.active) continue;
        float t = 0.0f;
        Vec3 n;
        if (m.bvh.raycast(origin, ndir, best.distance, t, n)) {
            if (t <= best.distance) {
                best.hit = true;
                best.distance = t;
                best.normal = n;
                best.boxIndex = -1;
                best.material = &m.material;
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
