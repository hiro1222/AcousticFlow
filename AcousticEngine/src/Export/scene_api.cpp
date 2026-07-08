/* Export/scene_api.cpp ── acoustic_scene.h の実装（新アーキ Phase 1）
 *
 * C 境界で受けた POD を Core の acoustic::Scene 呼び出しへ橋渡しする薄い層。
 * ここには計算ロジックを置かない（すべて Core/scene.h 側）。
 */
#include "acoustic_scene.h"

#include <algorithm>
#include <new>

#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/scene.h"
#include "Core/vec3.h"

using acoustic::AcousticMaterial;
using acoustic::kNumBands;
using acoustic::Obb;
using acoustic::Scene;
using acoustic::SceneHit;
using acoustic::Vec3;

namespace {

inline Vec3 toVec3(const AF_Vector3& v) { return Vec3(v.x, v.y, v.z); }

inline Scene* asScene(AF_SceneHandle h) { return static_cast<Scene*>(h); }

// 半サイズと right/up から OBB を作る。right/up が退化していれば軸並行にフォールバック。
Obb makeObb(const AF_Vector3& center, const AF_Vector3& half,
            const AF_Vector3& right, const AF_Vector3& up) {
    const Vec3 c = toVec3(center);
    const Vec3 he = toVec3(half);
    const Vec3 r = toVec3(right);
    const Vec3 u = toVec3(up);
    const float kEps = 1e-8f;
    if (dot(r, r) < kEps || dot(u, u) < kEps) {
        return Obb::axisAligned(c, he);
    }
    return Obb::oriented(c, he, r, u);
}

// C 側の帯域配列から AcousticMaterial を組む。null の配列は既定壁のまま。
AcousticMaterial makeMaterial(const float* transmission, const float* absorption,
                              const float* scattering, int numBands) {
    AcousticMaterial m = AcousticMaterial::defaultWall();
    const int n = std::min(numBands, kNumBands);
    for (int b = 0; b < n; ++b) {
        if (transmission) m.transmission[b] = transmission[b];
        if (absorption)   m.absorption[b]   = absorption[b];
        if (scattering)   m.scattering[b]   = scattering[b];
    }
    return m;
}

}  // namespace

extern "C" {

AF_SceneHandle AF_SceneCreate(void) { return new (std::nothrow) Scene(); }

void AF_SceneDestroy(AF_SceneHandle scene) { delete asScene(scene); }

int AF_SceneAddMaterial(AF_SceneHandle scene,
                        const float* transmission, const float* absorption,
                        const float* scattering, int numBands) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->addMaterial(makeMaterial(transmission, absorption, scattering, numBands));
}

int AF_SceneAddInstanceBox(AF_SceneHandle scene,
                           AF_Vector3 center, AF_Vector3 halfExtents,
                           AF_Vector3 right, AF_Vector3 up, int materialId) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->addInstance(makeObb(center, halfExtents, right, up), materialId);
}

void AF_SceneUpdateInstance(AF_SceneHandle scene, int instanceId,
                            AF_Vector3 center, AF_Vector3 halfExtents,
                            AF_Vector3 right, AF_Vector3 up) {
    Scene* s = asScene(scene);
    if (!s) return;
    s->updateInstanceTransform(instanceId, makeObb(center, halfExtents, right, up));
}

void AF_SceneSetInstanceActive(AF_SceneHandle scene, int instanceId, int active) {
    Scene* s = asScene(scene);
    if (!s) return;
    s->setInstanceActive(instanceId, active != 0);
}

void AF_SceneClearInstances(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    if (s) s->clearInstances();
}

int AF_SceneComputeTransmissionBands(AF_SceneHandle scene,
                                     AF_Vector3 from, AF_Vector3 to,
                                     float* outGains, int count) {
    Scene* s = asScene(scene);
    if (!s || !outGains || count <= 0) return 0;
    float gain[kNumBands];
    s->computeTransmission(toVec3(from), toVec3(to), gain);
    const int n = std::min(count, kNumBands);
    for (int b = 0; b < n; ++b) outGains[b] = gain[b];
    return n;
}

int AF_SceneIsOccluded(AF_SceneHandle scene, AF_Vector3 from, AF_Vector3 to) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    return s->isOccluded(toVec3(from), toVec3(to)) ? 1 : 0;
}

float AF_SceneRaycast(AF_SceneHandle scene, AF_Vector3 origin, AF_Vector3 dir,
                      float maxDist) {
    Scene* s = asScene(scene);
    if (!s) return -1.0f;
    const SceneHit hit = s->raycastClosest(toVec3(origin), toVec3(dir), maxDist);
    return hit.hit ? hit.t : -1.0f;
}

int AF_SceneComputeDiffractionBands(AF_SceneHandle scene,
                                    AF_Vector3 from, AF_Vector3 to,
                                    float* outGains, int count) {
    Scene* s = asScene(scene);
    if (!s || !outGains || count <= 0) return 0;
    float gain[kNumBands];
    s->computeDiffraction(toVec3(from), toVec3(to), gain);
    const int n = std::min(count, kNumBands);
    for (int b = 0; b < n; ++b) outGains[b] = gain[b];
    return n;
}

float AF_SceneDiffractionPath(AF_SceneHandle scene, AF_Vector3 from, AF_Vector3 to,
                              AF_Vector3* outMidPoint) {
    Scene* s = asScene(scene);
    if (!s) return -1.0f;
    Vec3 p{0.0f, 0.0f, 0.0f};
    const float delta = s->diffractionDetour(toVec3(from), toVec3(to), p);
    if (delta >= 0.0f && outMidPoint) {
        outMidPoint->x = p.x;
        outMidPoint->y = p.y;
        outMidPoint->z = p.z;
    }
    return delta;
}

float AF_SceneOcclusionReflected(AF_SceneHandle scene, AF_Vector3 source, AF_Vector3 listener,
                                 float* outBands6, int numRays, int maxBounces) {
    Scene* s = asScene(scene);
    if (!s) return 0.0f;
    return s->occlusionReflected(toVec3(source), toVec3(listener), outBands6, numRays, maxBounces);
}

void AF_SceneOcclusionReflectedMulti(AF_SceneHandle scene, AF_Vector3 listener,
                                     const AF_Vector3* sources, int count,
                                     float* outOcc, float* outBands,
                                     int numRays, int maxBounces) {
    Scene* s = asScene(scene);
    if (!s || !sources || count <= 0) return;
    // 境界の AF_Vector3 配列を Core の Vec3 へ詰め替える。
    std::vector<Vec3> src(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) src[i] = toVec3(sources[i]);
    s->occlusionReflectedMulti(toVec3(listener), src.data(), count,
                               outOcc, outBands, numRays, maxBounces);
}

void AF_SceneComputeEchogram(AF_SceneHandle scene, AF_Vector3 listener,
                             const AF_Vector3* sources, int count,
                             float* outBins, int numBins,
                             float binSeconds, float speedOfSound,
                             int numRays, int maxBounces) {
    Scene* s = asScene(scene);
    if (!s || !sources || count <= 0) return;
    std::vector<Vec3> src(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) src[i] = toVec3(sources[i]);
    s->computeEchogram(toVec3(listener), src.data(), count,
                       outBins, numBins, binSeconds, speedOfSound, numRays, maxBounces);
}

int AF_SceneInstanceCount(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? s->instanceCount() : 0;
}

}  // extern "C"
