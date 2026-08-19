/* Export/scene_api.cpp ── acoustic_scene.h の実装（新アーキ Phase 1）
 *
 * C 境界で受けた POD を Core の acoustic::Scene 呼び出しへ橋渡しする薄い層。
 * ここには計算ロジックを置かない（すべて Core/scene.h 側）。
 */
#include "acoustic_scene.h"

#include <algorithm>
#include <new>
#include <vector>

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

inline AF_Vector3 fromVec3(const Vec3& v) { return AF_Vector3{ v.x, v.y, v.z }; }

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

int AF_MaterialPresetBands(int preset, float* outTransmission,
                           float* outAbsorption, float* outScattering) {
    AcousticMaterial m;
    switch (preset) {
        case 1:  m = AcousticMaterial::concrete();    break;
        case 2:  m = AcousticMaterial::glass();       break;
        case 3:  m = AcousticMaterial::opaque();      break;
        case 4:  m = AcousticMaterial::woodDoor();    break;
        default: m = AcousticMaterial::defaultWall(); break;
    }
    for (int b = 0; b < kNumBands; ++b) {
        if (outTransmission) outTransmission[b] = m.transmission[b];
        if (outAbsorption)   outAbsorption[b]   = m.absorption[b];
        if (outScattering)   outScattering[b]   = m.scattering[b];
    }
    return kNumBands;
}

int AF_SceneAddMaterial(AF_SceneHandle scene,
                        const float* transmission, const float* absorption,
                        const float* scattering, int numBands) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->addMaterial(makeMaterial(transmission, absorption, scattering, numBands));
}

int AF_SceneSetMaterial(AF_SceneHandle scene, int materialId,
                        const float* transmission, const float* absorption,
                        const float* scattering, int numBands) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    return s->setMaterial(materialId,
                          makeMaterial(transmission, absorption, scattering, numBands)) ? 1 : 0;
}

int AF_SceneSetInstanceMaterial(AF_SceneHandle scene, int instanceId, int materialId) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    return s->setInstanceMaterial(instanceId, materialId) ? 1 : 0;
}

int AF_SceneGetInstanceMaterial(AF_SceneHandle scene, int instanceId) {
    Scene* s = asScene(scene);
    return s ? s->instanceMaterial(instanceId) : -1;
}

int AF_SceneAddInstanceBox(AF_SceneHandle scene,
                           AF_Vector3 center, AF_Vector3 halfExtents,
                           AF_Vector3 right, AF_Vector3 up, int materialId) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->addInstance(makeObb(center, halfExtents, right, up), materialId);
}

void AF_SceneSetApertureOpen(AF_SceneHandle scene, float ref, float power,
                             float radius, int samples) {
    Scene* s = asScene(scene);
    if (!s) return;
    s->setApertureOpenRef(ref);
    s->setApertureOpenPower(power);
    s->setApertureOpenRadius(radius);
    s->setApertureOpenSamples(samples);
}

void AF_SceneGetApertureOpen(AF_SceneHandle scene, float* outRef, float* outPower,
                             float* outRadius, int* outSamples) {
    Scene* s = asScene(scene);
    if (outRef)     *outRef     = s ? s->apertureOpenRef() : 0.0f;
    if (outPower)   *outPower   = s ? s->apertureOpenPower() : 0.0f;
    if (outRadius)  *outRadius  = s ? s->apertureOpenRadius() : 0.0f;
    if (outSamples) *outSamples = s ? s->apertureOpenSamples() : 0;
}

float AF_SceneMeasureSlitWidth(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source) {
    Scene* s = asScene(scene);
    if (!s) return 0.0f;
    const Vec3 l = toVec3(listener), src = toVec3(source);
    Scene::DiffractionPath paths[4];
    const int np = s->findDiffractionPaths(l, src, paths, 4);
    if (np <= 0) return 0.0f;
    return paths[0].slitWidth;
}

float AF_SceneMeasureApertureOpenness(AF_SceneHandle scene, AF_Vector3 listener,
                                      AF_Vector3 source) {
    Scene* s = asScene(scene);
    if (!s) return 0.0f;
    // 最有力の開口での開口率を返す。開口点はホストからは取れない（二次音源が返すのは
    // 定位用に投影し直した位置）ので、ここで探索してから測る。
    const Vec3 l = toVec3(listener), src = toVec3(source);
    Scene::DiffractionPath paths[4];
    const int np = s->findDiffractionPaths(l, src, paths, 4);
    if (np <= 0) return 0.0f;
    return s->apertureOpenness(l, src, paths[0].aperture);
}

void AF_SceneMeasureLeakPoint(AF_SceneHandle scene, AF_Vector3 listener,
                              AF_Vector3 source, AF_Vector3* outPoint) {
    Scene* s = asScene(scene);
    if (!s || !outPoint) return;
    float t[kNumBands]; float frac = 0.0f; Vec3 p = toVec3(source);
    s->computeSoftOcclusion(toVec3(listener), toVec3(source), t, frac, 32, 0.9f, &p);
    *outPoint = fromVec3(p);
}

int AF_SceneComputeSoftOcclusion(AF_SceneHandle scene,
                                 AF_Vector3 listener, AF_Vector3 source,
                                 float* outTrans, int count, float* outOccFrac) {
    Scene* s = asScene(scene);
    if (!s || !outTrans || count <= 0) return 0;
    float t[kNumBands];
    float frac = 0.0f;
    s->computeSoftOcclusion(toVec3(listener), toVec3(source), t, frac);
    const int n = std::min(count, kNumBands);
    for (int b = 0; b < n; ++b) outTrans[b] = t[b];
    if (outOccFrac) *outOccFrac = frac;
    return n;
}

int AF_SceneComputeDiffractionKirchhoff(AF_SceneHandle scene,
                                        AF_Vector3 listener, AF_Vector3 source,
                                        float* outGains, int count,
                                        AF_Vector3* outAperture, float* outPathLength) {
    Scene* s = asScene(scene);
    if (!s || !outGains || count <= 0) return 0;
    float gain[kNumBands];
    Vec3 ap; float pl = 0.0f;
    if (!s->diffractionKirchhoff(toVec3(listener), toVec3(source), gain, ap, pl)) return 0;
    const int n = std::min(count, kNumBands);
    for (int b = 0; b < n; ++b) outGains[b] = gain[b];
    if (outAperture) *outAperture = AF_Vector3{ap.x, ap.y, ap.z};
    if (outPathLength) *outPathLength = pl;
    return 1;
}

void AF_SceneProbeDirectionalEnergy(AF_SceneHandle scene, AF_Vector3 origin,
                                    const AF_Vector3* dirs, int dirCount,
                                    int maxBounces, float* outEnergy) {
    Scene* s = asScene(scene);
    if (!s || !dirs || !outEnergy || dirCount <= 0) return;
    std::vector<Vec3> d(static_cast<size_t>(dirCount));
    for (int i = 0; i < dirCount; ++i) d[static_cast<size_t>(i)] = toVec3(dirs[i]);
    s->probeDirectionalEnergy(toVec3(origin), d.data(), dirCount, maxBounces, outEnergy);
}

int AF_SceneAddMesh(AF_SceneHandle scene,
                    const float* verticesXYZ, int vertexCount,
                    const int* indices, int indexCount,
                    AF_Vector3* outLocalCenter, AF_Vector3* outLocalHalfExtents) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    Vec3 c, h;
    const int id = s->addMesh(verticesXYZ, vertexCount, indices, indexCount, &c, &h);
    if (id < 0) return -1;
    if (outLocalCenter) *outLocalCenter = AF_Vector3{c.x, c.y, c.z};
    if (outLocalHalfExtents) *outLocalHalfExtents = AF_Vector3{h.x, h.y, h.z};
    return id;
}

int AF_SceneGetMeshEdgeCount(AF_SceneHandle scene, int geomId) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->meshEdgeCount(geomId);
}

void AF_SceneRemoveMesh(AF_SceneHandle scene, int geomId) {
    Scene* s = asScene(scene);
    if (!s) return;
    s->removeMesh(geomId);
}

int AF_SceneAddInstanceMesh(AF_SceneHandle scene, int geomId,
                            AF_Vector3 center, AF_Vector3 halfExtents,
                            AF_Vector3 right, AF_Vector3 up, int materialId) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->addInstance(makeObb(center, halfExtents, right, up), materialId, geomId);
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
    // 内部はエネルギー、ホストへ返すタップのゲインは振幅（material.h の規約）。
    for (int b = 0; b < n; ++b) outGains[b] = std::sqrt(gain[b]);
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

int AF_SceneComputeDiffractionBandsUtd(AF_SceneHandle scene,
                                       AF_Vector3 from, AF_Vector3 to,
                                       float* outGains, int count) {
    Scene* s = asScene(scene);
    if (!s || !outGains || count <= 0) return 0;
    const Vec3 f = toVec3(from), t = toVec3(to);
    float gain[kNumBands];
    s->diffractionUtd(f, t, s->isOccluded(f, t), gain);
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

int AF_SceneDiffractionCandidates(AF_SceneHandle scene, AF_Vector3 from, AF_Vector3 to,
                                  AF_Vector3* outPoints, float* outDeltas, int maxCount) {
    Scene* s = asScene(scene);
    if (!s || !outPoints || !outDeltas || maxCount <= 0) return 0;
    std::vector<Vec3> pts(static_cast<size_t>(maxCount));
    const int n = s->diffractionCandidates(toVec3(from), toVec3(to), pts.data(), outDeltas, maxCount);
    for (int i = 0; i < n; ++i) {
        outPoints[i].x = pts[i].x;
        outPoints[i].y = pts[i].y;
        outPoints[i].z = pts[i].z;
    }
    return n;
}

int AF_SceneComputeDiffractionSources(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source,
                                      AF_Vector3* outPos, float* outGain, int maxN) {
    Scene* s = asScene(scene);
    if (!s || !outPos || !outGain || maxN <= 0) return 0;
    std::vector<Vec3> pos(static_cast<size_t>(maxN));
    const int n = s->computeDiffractionSources(toVec3(listener), toVec3(source),
                                               pos.data(), outGain, maxN);
    for (int i = 0; i < n; ++i) {
        outPos[i].x = pos[i].x;
        outPos[i].y = pos[i].y;
        outPos[i].z = pos[i].z;
    }
    return n;
}

float AF_SceneOcclusionReflected(AF_SceneHandle scene, AF_Vector3 source, AF_Vector3 listener,
                                 float* outBands6, int numRays, int maxBounces) {
    Scene* s = asScene(scene);
    if (!s) return 0.0f;
    return s->occlusionReflected(toVec3(source), toVec3(listener), outBands6, numRays, maxBounces);
}

void AF_SceneOcclusionReflectedMulti(AF_SceneHandle scene, AF_Vector3 listener,
                                     const AF_Vector3* sources, int count,
                                     float* outOcc, float* outBands, float* outDir,
                                     float directWeight, int numRays, int maxBounces) {
    Scene* s = asScene(scene);
    if (!s || !sources || count <= 0) return;
    // 境界の AF_Vector3 配列を Core の Vec3 へ詰め替える。
    std::vector<Vec3> src(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) src[i] = toVec3(sources[i]);
    s->occlusionReflectedMulti(toVec3(listener), src.data(), count,
                               outOcc, outBands, outDir, directWeight, numRays, maxBounces);
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

void AF_SceneComputeEchogramBands(AF_SceneHandle scene, AF_Vector3 listener,
                                  const AF_Vector3* sources, int count,
                                  float* outBins, int numBins,
                                  float binSeconds, float speedOfSound,
                                  int numRays, int maxBounces, float distanceRef) {
    Scene* s = asScene(scene);
    if (!s || !sources || count <= 0 || !outBins || numBins <= 0) return;
    std::vector<Vec3> src(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) src[i] = toVec3(sources[i]);
    // ★computeEchogramBands は音源ごとの面（[音源][ビン][帯域]）を書くようになった。
    //   この単発クエリの契約は「全音源まとめた 1 本」なので、内部で音源ぶん確保してから
    //   足し込む。呼び手のバッファは numBins*6 のままでよい（そのまま渡すと溢れる）。
    const std::size_t per = static_cast<std::size_t>(numBins) * kNumBands;
    std::vector<float> tmp(per * static_cast<std::size_t>(count), 0.0f);
    s->computeEchogramBands(toVec3(listener), src.data(), count, tmp.data(), numBins,
                            binSeconds, speedOfSound, numRays, maxBounces, distanceRef);
    for (std::size_t i = 0; i < per; ++i) outBins[i] = 0.0f;
    for (int j = 0; j < count; ++j)
        for (std::size_t i = 0; i < per; ++i)
            outBins[i] += tmp[static_cast<std::size_t>(j) * per + i];
}

int AF_SceneTraceReflectionPath(AF_SceneHandle scene, AF_Vector3 origin, AF_Vector3 dir,
                                float maxDist, int maxBounces,
                                AF_Vector3* outPoints, int maxPoints) {
    Scene* s = asScene(scene);
    if (!s || !outPoints || maxPoints < 2) return 0;
    std::vector<Vec3> buf(static_cast<size_t>(maxPoints));
    const int n = s->traceReflectionPath(toVec3(origin), toVec3(dir), maxDist, maxBounces,
                                         buf.data(), maxPoints);
    for (int i = 0; i < n; ++i) {
        outPoints[i].x = buf[i].x;
        outPoints[i].y = buf[i].y;
        outPoints[i].z = buf[i].z;
    }
    return n;
}

void AF_SceneBuildEdgeCatalog(AF_SceneHandle scene, AF_Vector3 listener, int res, float maxDist) {
    Scene* s = asScene(scene);
    if (s) s->buildEdgeCatalog(toVec3(listener), res, maxDist);
}

int AF_SceneEdgeCatalogCount(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? s->edgeCatalogCount() : 0;
}

void AF_SceneClearEdgeCatalog(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    if (s) s->clearEdgeCatalog();
}

int AF_SceneComputeEarlyReflections(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source,
                                    AF_Vector3* outImagePos, float* outGain,
                                    int maxTaps, int numRays, int maxBounces) {
    Scene* s = asScene(scene);
    if (!s || !outImagePos || !outGain || maxTaps <= 0) return 0;
    std::vector<Vec3> pos(static_cast<size_t>(maxTaps));
    const int n = s->computeEarlyReflections(toVec3(listener), toVec3(source),
                                             pos.data(), outGain, maxTaps, numRays, maxBounces);
    for (int i = 0; i < n; ++i) {
        outImagePos[i].x = pos[i].x;
        outImagePos[i].y = pos[i].y;
        outImagePos[i].z = pos[i].z;
    }
    return n;
}

int AF_SceneInstanceCount(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? s->instanceCount() : 0;
}

// --- リスナー / 音源の保持（段1）---

void AF_SceneSetListener(AF_SceneHandle scene, AF_Vector3 pos) {
    Scene* s = asScene(scene);
    if (s) s->setListener(toVec3(pos));
}

void AF_SceneSetSource(AF_SceneHandle scene, unsigned long long id, AF_Vector3 pos) {
    Scene* s = asScene(scene);
    if (s) s->setSource(id, toVec3(pos));
}

void AF_SceneRemoveSource(AF_SceneHandle scene, unsigned long long id) {
    Scene* s = asScene(scene);
    if (s) s->removeSource(id);
}

void AF_SceneClearSources(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    if (s) s->clearSources();
}

int AF_SceneSourceCount(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? s->sourceCount() : 0;
}

// --- バッチ更新（段2）---

void AF_SceneSetUpdateConfig(AF_SceneHandle scene, const AF_UpdateConfig* cfg) {
    Scene* s = asScene(scene);
    if (!s || !cfg) return;
    Scene::UpdateConfig c;
    c.role1EveryN = cfg->role1EveryN;
    c.role2EveryN = cfg->role2EveryN;
    c.earlyEveryN = cfg->earlyEveryN;
    c.diffSrcEveryN = cfg->diffSrcEveryN;
    c.catalogEveryN = cfg->catalogEveryN;
    c.reflectionRays = cfg->reflectionRays;
    c.reflectionBounces = cfg->reflectionBounces;
    c.directWeight = cfg->directWeight;
    c.useReflections = cfg->useReflections != 0;
    c.useEdgeCatalog = cfg->useEdgeCatalog != 0;
    c.edgeCatalogRes = cfg->edgeCatalogRes;
    c.edgeCatalogMaxDist = cfg->edgeCatalogMaxDist;
    c.enableReverb = cfg->enableReverb != 0;
    c.echogramBins = cfg->echogramBins;
    c.echogramBinSeconds = cfg->echogramBinSeconds;
    c.echogramRays = cfg->echogramRays;
    c.echogramBounces = cfg->echogramBounces;
    c.speedOfSound = cfg->speedOfSound;
    c.distanceRef = cfg->distanceRef;
    c.enableEarlyReflections = cfg->enableEarlyReflections != 0;
    c.earlyTaps = cfg->earlyTaps;
    c.earlyRays = cfg->earlyRays;
    c.earlyBounces = cfg->earlyBounces;
    c.enableDiffractionSources = cfg->enableDiffractionSources != 0;
    c.diffSources = cfg->diffSources;
    s->setUpdateConfig(c);
}

void AF_SceneUpdate(AF_SceneHandle scene, float dt) {
    Scene* s = asScene(scene);
    if (s) s->update(dt);
}

int AF_SceneSourceIndex(AF_SceneHandle scene, unsigned long long id) {
    Scene* s = asScene(scene);
    return s ? s->sourceIndexOf(id) : -1;
}

void AF_SceneGetSourceOcclusion(AF_SceneHandle scene, int index, float* out6) {
    Scene* s = asScene(scene);
    if (s) s->getSourceOcclusion(index, out6);
}

float AF_SceneGetSourceOcclusionScalar(AF_SceneHandle scene, int index) {
    Scene* s = asScene(scene);
    return s ? s->getSourceOcclusionScalar(index) : 0.0f;
}

void AF_SceneGetSourceArrivalDir(AF_SceneHandle scene, int index, float* out3) {
    Scene* s = asScene(scene);
    if (s) s->getSourceArrivalDir(index, out3);
}

int AF_SceneGetEarlyReflections(AF_SceneHandle scene, int index,
                                AF_Vector3* outPos, float* outGain6, int maxTaps) {
    Scene* s = asScene(scene);
    if (!s || !outPos || !outGain6 || maxTaps <= 0) return 0;
    std::vector<Vec3> tmp(static_cast<size_t>(maxTaps));
    const int n = s->getEarlyReflections(index, tmp.data(), outGain6, maxTaps);
    for (int i = 0; i < n; ++i) outPos[i] = fromVec3(tmp[static_cast<size_t>(i)]);
    return n;
}

int AF_SceneGetDiffractionSources(AF_SceneHandle scene, int index,
                                  AF_Vector3* outPos, float* outGain, int maxSrc) {
    Scene* s = asScene(scene);
    if (!s || !outPos || !outGain || maxSrc <= 0) return 0;
    std::vector<Vec3> tmp(static_cast<size_t>(maxSrc));
    const int n = s->getDiffractionSources(index, tmp.data(), outGain, maxSrc);
    for (int i = 0; i < n; ++i) outPos[i] = fromVec3(tmp[static_cast<size_t>(i)]);
    return n;
}

int AF_SceneGetEchogramBands(AF_SceneHandle scene, int index, float* outBins, int numBins) {
    Scene* s = asScene(scene);
    return s ? s->getEchogramBands(index, outBins, numBins) : 0;
}

}  // extern "C"

void AF_SceneSetApertureSpread(AF_SceneHandle scene, int points) {
    Scene* s = asScene(scene);
    if (s) s->setApertureSpread(points);
}

int AF_SceneGetApertureSpread(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? s->apertureSpread() : 0;
}

int AF_SceneComputeDiffractionSourceBands(AF_SceneHandle scene,
                                          AF_Vector3 listener, AF_Vector3 source,
                                          AF_Vector3* outPos, float* outGain,
                                          float* outBand6, int maxSrc) {
    Scene* s = asScene(scene);
    if (!s || !outPos || !outGain || maxSrc <= 0) return 0;
    std::vector<Vec3> pos(static_cast<size_t>(maxSrc));
    const int n = s->computeDiffractionSourceBands(toVec3(listener), toVec3(source),
                                                   pos.data(), outGain, outBand6, maxSrc);
    for (int i = 0; i < n; ++i) outPos[i] = fromVec3(pos[static_cast<size_t>(i)]);
    return n;
}

int AF_SceneAddPortal(AF_SceneHandle scene, AF_Vector3 center,
                      AF_Vector3 axisU, AF_Vector3 axisV, float halfU, float halfV) {
    Scene* s = asScene(scene);
    if (!s) return -1;
    return s->addPortal(toVec3(center), toVec3(axisU), toVec3(axisV), halfU, halfV);
}

void AF_SceneUpdatePortal(AF_SceneHandle scene, int id, AF_Vector3 center,
                          AF_Vector3 axisU, AF_Vector3 axisV, float halfU, float halfV) {
    Scene* s = asScene(scene);
    if (s) s->updatePortal(id, toVec3(center), toVec3(axisU), toVec3(axisV), halfU, halfV);
}

// ★ホストは毎フレーム押してくる。値が変わったときだけ作り直すこと
//   （rebuildAutoPortals は部屋グラフ全体の開口を舐めるので、毎フレームは無駄）。
void AF_SceneSetAutoPortals(AF_SceneHandle scene, int enable) {
    Scene* s = asScene(scene);
    if (!s || s->autoPortals() == (enable != 0)) return;
    s->setAutoPortals(enable != 0);
    s->rebuildAutoPortals();          // 切り替えた瞬間に反映する（次の update を待たない）
}

void AF_SceneSetAutoPortalMinArea(AF_SceneHandle scene, float m2) {
    Scene* s = asScene(scene);
    if (!s || s->autoPortalMinArea() == m2) return;
    s->setAutoPortalMinArea(m2);
    s->rebuildAutoPortals();
}

int AF_SceneGetPortalCounts(AF_SceneHandle scene, int* outAuto, int* outManual) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    if (outAuto)   *outAuto   = s->autoPortalCount();
    if (outManual) *outManual = s->manualPortalCount();
    return s->portalCount();
}

int AF_SceneGetPortal(AF_SceneHandle scene, int id, AF_Vector3* outCenter,
                      AF_Vector3* outAxisU, AF_Vector3* outAxisV,
                      float* outHalfU, float* outHalfV) {
    Scene* s = asScene(scene);
    if (!s || id < 0 || id >= s->portalCount()) return 0;
    const auto& p = s->portal(id);
    if (outCenter) *outCenter = fromVec3(p.center);
    if (outAxisU)  *outAxisU  = fromVec3(p.axisU);
    if (outAxisV)  *outAxisV  = fromVec3(p.axisV);
    if (outHalfU)  *outHalfU  = p.halfU;
    if (outHalfV)  *outHalfV  = p.halfV;
    return 1;
}

/* 【計測用】回折の可視判定のゲートを個別に切る。0 = 全部有効（本番）。 */
/* 【(B)】稜線からポータルを生成してフレネル積分する。既定 0（従来の前川＋開口積分）。
 * 1 点で回折を表すのをやめる ── 隙間の幅も角の死角も、矩形の中で影が落ちることで
 * 自動的に解ける。ナイフエッジ回折の厳密解はもともとこの形の積分。 */
/* 【計測用】直近の開口積分で矩形に写った遮蔽物の枚数。-1 は未実行。 */
int AF_SceneDebugPortalPolys(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? s->dbgPortalPolys() : -1;
}

void AF_SceneSetEdgePortals(AF_SceneHandle scene, int enable) {
    Scene* s = asScene(scene);
    if (s) s->setEdgePortals(enable != 0);
}

/* 矩形の半幅をフレネル半径の何倍にするか（既定 1.0）。 */
void AF_SceneSetEdgePortalSpan(AF_SceneHandle scene, float k) {
    Scene* s = asScene(scene);
    if (s) s->setEdgePortalSpan(k);
}

void AF_SceneSetDiffractionGateMask(AF_SceneHandle scene, int mask) {
    Scene* s = asScene(scene);
    if (s) s->setDiffractionGateMask(mask);
}

void AF_SceneSetPortalGovernRange(AF_SceneHandle scene, float meters) {
    Scene* s = asScene(scene);
    if (s) s->setPortalGovernRange(meters);
}

int AF_SceneMeasurePortal(AF_SceneHandle scene, int id,
                          AF_Vector3 listener, AF_Vector3 source,
                          float* outFrac6, AF_Vector3* outPoint) {
    Scene* s = asScene(scene);
    if (!s || !outFrac6 || id < 0 || id >= s->portalCount()) return 0;
    Vec3 p(0, 0, 0);
    const bool ok = s->portalOpenBands(s->portal(id), toVec3(listener), toVec3(source),
                                       outFrac6, &p);
    if (outPoint) *outPoint = fromVec3(p);
    return ok ? 1 : 0;
}

void AF_SceneSetApertureIsTransmission(AF_SceneHandle scene, int on) {
    Scene* s = asScene(scene);
    if (s) s->setApertureIsTransmission(on);
}

int AF_SceneRoomCount(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? static_cast<int>(s->roomGraph().rooms.size()) : 0;
}

int AF_SceneRoomAt(AF_SceneHandle scene, AF_Vector3 p) {
    Scene* s = asScene(scene);
    return s ? s->roomAt(toVec3(p)) : -1;
}

int AF_SceneRoomInfo(AF_SceneHandle scene, int room, float* outVolume,
                     AF_Vector3* outCentroid, AF_Vector3* outMin, AF_Vector3* outMax) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    const auto& rr = s->roomGraph();
    if (room < 0 || room >= static_cast<int>(rr.rooms.size())) return 0;
    const auto& rm = rr.rooms[static_cast<std::size_t>(room)];
    const float c = rr.grid.cell;
    if (outVolume) *outVolume = static_cast<float>(rm.voxels) * c * c * c;
    if (outCentroid) *outCentroid = fromVec3(rm.centroid);
    if (outMin) *outMin = fromVec3(rm.boundsMin);
    if (outMax) *outMax = fromVec3(rm.boundsMax);
    return 1;
}

void AF_SceneSetRoomCellSize(AF_SceneHandle scene, float meters) {
    Scene* s = asScene(scene);
    if (s) s->setRoomCellSize(meters);
}

int AF_SceneRoomGridDegraded(AF_SceneHandle scene, float* outRequested, float* outActual,
                             double* outVoxels, double* outMaxVoxels) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    const auto& g = s->roomGraph().grid;
    const float req = s->roomCellRequested();
    const double nv = static_cast<double>(g.nx) * g.ny * g.nz;
    if (outRequested)  *outRequested  = req;
    if (outActual)     *outActual     = g.cell;
    if (outVoxels)     *outVoxels     = nv;
    if (outMaxVoxels)  *outMaxVoxels  = static_cast<double>(s->roomMaxVoxels());
    // 1.5 倍ずつしか粗くならないので、少しでも大きければ降格している。
    return (g.cell > req * 1.001f) ? 1 : 0;
}

void AF_SceneRoomGridDims(AF_SceneHandle scene, int* nx, int* ny, int* nz, float* cell) {
    Scene* s = asScene(scene);
    if (!s) return;
    const auto& g = s->roomGraph().grid;
    if (nx) *nx = g.nx;
    if (ny) *ny = g.ny;
    if (nz) *nz = g.nz;
    if (cell) *cell = g.cell;
}

void AF_SceneRoomBuildTimes(AF_SceneHandle scene, float* alloc, float* fill,
                            float* dist, float* label, float* merge, float* grow,
                            int* dirtyBricks, int* totalBricks) {
    Scene* s = asScene(scene);
    if (!s) return;
    const auto& rr = s->roomGraph();
    if (alloc)       *alloc       = static_cast<float>(rr.msAlloc);
    if (fill)        *fill        = static_cast<float>(rr.msFill);
    if (dist)        *dist        = static_cast<float>(rr.msDist);
    if (label)       *label       = static_cast<float>(rr.msLabel);
    if (merge)       *merge       = static_cast<float>(rr.msMerge);
    if (grow)        *grow        = static_cast<float>(rr.msGrow);
    if (dirtyBricks) *dirtyBricks = rr.dirtyBricks;
    if (totalBricks) *totalBricks = rr.totalBricks;
}

void AF_SceneSetRoomBrick(AF_SceneHandle scene, int voxels) {
    Scene* s = asScene(scene);
    if (s) s->setRoomBrick(voxels);
}

void AF_SceneSetRoomSeedRadius(AF_SceneHandle scene, float meters) {
    Scene* s = asScene(scene);
    if (s) s->setRoomSeedRadius(meters);
}

void AF_SceneSetDiffractionFlat(AF_SceneHandle scene, int flat) {
    Scene* s = asScene(scene);
    if (s) s->setDiffractionFlat(flat != 0);
}

void AF_SceneSetRoomChamferFull(AF_SceneHandle scene, int full) {
    Scene* s = asScene(scene);
    if (s) s->setRoomChamferFull(full != 0);
}

int AF_SceneRoomWeights(AF_SceneHandle scene, AF_Vector3 p, float radius,
                        int* outRooms, float* outWeights, int maxOut) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    return s->roomWeights(toVec3(p), radius, outRooms, outWeights, maxOut);
}

float AF_SceneRoomVolumeAt(AF_SceneHandle scene, AF_Vector3 p, float radius) {
    Scene* s = asScene(scene);
    return s ? s->roomVolumeAt(toVec3(p), radius) : 0.0f;
}

int AF_SceneRt60At(AF_SceneHandle scene, AF_Vector3 p, float radius, float* out, int count) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    return s->rt60At(toVec3(p), radius, out, count);
}

int AF_SceneRoomAcoustics(AF_SceneHandle scene, int room, float* outSurface,
                          float* outOpenArea, float* outAbsorb6, float* outRt60_6) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    const auto& rr = s->roomGraph();
    if (room < 0 || room >= static_cast<int>(rr.rooms.size())) return 0;
    const auto& rm = rr.rooms[static_cast<std::size_t>(room)];
    if (outSurface)  *outSurface  = rm.surface;
    if (outOpenArea) *outOpenArea = rm.openArea;
    for (int b = 0; b < kNumBands; ++b) {
        if (outAbsorb6) outAbsorb6[b] = rm.absorb[b];
        if (outRt60_6)  outRt60_6[b]  = rm.rt60[b];
    }
    return 1;
}

int AF_SceneApertureCount(AF_SceneHandle scene) {
    Scene* s = asScene(scene);
    return s ? static_cast<int>(s->roomGraph().apertures.size()) : 0;
}

int AF_SceneApertureInfo(AF_SceneHandle scene, int index,
                         float* outArea, AF_Vector3* outCenter, AF_Vector3* outNormal,
                         int* outRoomA, int* outRoomB) {
    Scene* s = asScene(scene);
    if (!s) return 0;
    const auto& aps = s->roomGraph().apertures;
    if (index < 0 || index >= static_cast<int>(aps.size())) return 0;
    const auto& a = aps[static_cast<std::size_t>(index)];
    if (outArea)   *outArea   = a.area;
    if (outCenter) *outCenter = fromVec3(a.center);
    if (outNormal) *outNormal = fromVec3(a.normal);
    if (outRoomA)  *outRoomA  = a.roomA;
    if (outRoomB)  *outRoomB  = a.roomB;
    return 1;
}

void AF_SceneSetApertureContrast(AF_SceneHandle scene, float p) {
    Scene* s = asScene(scene);
    if (s) s->setApertureContrast(p);
}

void AF_SceneSetApertureTimbre(AF_SceneHandle scene, float k) {
    Scene* s = asScene(scene);
    if (s) s->setApertureTimbre(k);
}

void AF_SceneSetUseBtm(AF_SceneHandle scene, int on) {
    Scene* s = asScene(scene);
    if (s) s->setUseBtm(on);
}

void AF_SceneSetApertureDeltaWeight(AF_SceneHandle scene, float w) {
    Scene* s = asScene(scene);
    if (s) s->setApertureDeltaWeight(w);
}

void AF_SceneSetSlitBandSlope(AF_SceneHandle scene, float slope) {
    Scene* s = asScene(scene);
    if (s) s->setSlitBandSlope(slope);
}

int AF_SceneMeasureApertureFresnel(AF_SceneHandle scene, AF_Vector3 listener,
                                   AF_Vector3 source, float* outFrac6) {
    Scene* s = asScene(scene);
    if (!s || !outFrac6) return 0;
    const Vec3 L = toVec3(listener), S = toVec3(source);
    float f[kNumBands];
    // エンジン本体と同じ条件で測る（窓の中心＝直線と平面の交点）。
    //   ここで開口点を渡す実験もしたが、開口点は跳ぶので窓ごと飛んで値が乱高下した。
    //   窓は測定の基準ではなく積分の範囲、という設計に合わせて渡さない。
    const bool ok = s->apertureFresnelBands(L, S, f);
    for (int b = 0; b < kNumBands; ++b) outFrac6[b] = f[b];
    return ok ? 1 : 0;
}
