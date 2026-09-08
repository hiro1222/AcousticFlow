/* Export/scene_api.cpp ── acoustic_scene.h の実装（新アーキ Phase 1）
 *
 * C 境界で受けた POD を Core の acoustic::Scene 呼び出しへ橋渡しする薄い層。
 * ここには計算ロジックを置かない（すべて Core/scene.h 側）。
 */
#include "acoustic_scene.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "Debug/capture_emit.h"   // 破れ → C++ の回帰テスト
#include "Debug/capture_scan.h"   // 録った .afcap に型紙を当てる（回帰テストと同じ関数）
#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/scene.h"
#include "Core/scene_async.h"   // 更新の非同期化（待ち行列・写し・共有ロック）
#include "Core/vec3.h"

using acoustic::AcousticMaterial;
using acoustic::kNumBands;
using acoustic::Obb;
using acoustic::Scene;
using acoustic::SceneBox;
using acoustic::TapParams;
using acoustic::VoiceProgram;
using acoustic::ProgramTap;
using acoustic::SceneHit;
using acoustic::Vec3;

namespace {

inline Vec3 toVec3(const AF_Vector3& v) { return Vec3(v.x, v.y, v.z); }

inline AF_Vector3 fromVec3(const Vec3& v) { return AF_Vector3{ v.x, v.y, v.z }; }

// ── 口の分類（docs/ASYNC_UPDATE.md）──
//   asBox       … 箱（Scene ＋ 非同期の仕組み）
//   SyncGuard   … 稀な同期の口（構築・焼き・書き出し・キャプチャ操作）。仕事を待ち、待ち行列を反映してから排他で入る
//   ReadGuard   … 幾何の問い合わせ。共有ロック（解く段と並走、作り直しの段とは待ち合う）
//   post        … 設定。非同期なら待ち行列へ（着手のとき順に適用）、同期なら即時
//   snapshot    … 結果のゲッタ。非同期なら写し、同期なら Scene 自身
//   同期モードでは全部が素通しで、従来と同じ。
inline SceneBox* asBox(AF_SceneHandle h) { return static_cast<SceneBox*>(h); }
using SyncGuard = SceneBox::SyncGuard;
using ReadGuard = SceneBox::ReadGuard;

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

AF_SceneHandle AF_SceneCreate(void) { return new (std::nothrow) SceneBox(); }

void AF_SceneDestroy(AF_SceneHandle scene) { delete asBox(scene); }   // 仕事を待ってから畳む

int AF_MaterialPresetBands(int preset, float* outTransmission,
                           float* outAbsorption, float* outScattering) {
    AcousticMaterial m;
    switch (preset) {
        case 1:  m = AcousticMaterial::concrete();    break;
        case 2:  m = AcousticMaterial::glass();       break;
        case 3:  m = AcousticMaterial::opaque();      break;
        case 4:  m = AcousticMaterial::woodDoor();    break;
        case 5:  m = AcousticMaterial::woodRoom();    break;
        case 6:  m = AcousticMaterial::stone();       break;
        case 7:  m = AcousticMaterial::cave();        break;
        case 8:  m = AcousticMaterial::snow();        break;
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
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1;
    return s->addMaterial(makeMaterial(transmission, absorption, scattering, numBands));
}

int AF_SceneGetMaterial(AF_SceneHandle scene, int materialId,
                        float* outTransmission, float* outAbsorption,
                        float* outScattering, int count) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || count <= 0) return 0;
    if (materialId < 0 || materialId >= s->materialCount()) return 0;
    const AcousticMaterial& m = s->materialAt(materialId);
    const int n = (count < kNumBands) ? count : kNumBands;
    for (int b = 0; b < n; ++b) {
        if (outTransmission) outTransmission[b] = m.transmission[b];
        if (outAbsorption) outAbsorption[b] = m.absorption[b];
        if (outScattering) outScattering[b] = m.scattering[b];
    }
    return n;
}

int AF_SceneSetMaterial(AF_SceneHandle scene, int materialId,
                        const float* transmission, const float* absorption,
                        const float* scattering, int numBands) {
    SceneBox* b = asBox(scene);
    if (!b) return 0;
    const AcousticMaterial m = makeMaterial(transmission, absorption, scattering, numBands);
    if (!b->async()) return b->scene.setMaterial(materialId, m) ? 1 : 0;
    b->post([materialId, m](Scene& s) { s.setMaterial(materialId, m); });
    return (materialId >= 0 && materialId < b->scene.materialCount()) ? 1 : 0;   // 非同期: 範囲だけ答える
}

int AF_SceneSetInstanceMaterial(AF_SceneHandle scene, int instanceId, int materialId) {
    SceneBox* b = asBox(scene);
    if (!b) return 0;
    if (!b->async()) return b->scene.setInstanceMaterial(instanceId, materialId) ? 1 : 0;
    b->post([instanceId, materialId](Scene& s) { s.setInstanceMaterial(instanceId, materialId); });
    return (instanceId >= 0 && instanceId < b->scene.instanceCount()
            && materialId >= 0 && materialId < b->scene.materialCount()) ? 1 : 0;
}

int AF_SceneGetInstanceMaterial(AF_SceneHandle scene, int instanceId) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->instanceMaterial(instanceId) : -1;
}

int AF_SceneAddInstanceBox(AF_SceneHandle scene,
                           AF_Vector3 center, AF_Vector3 halfExtents,
                           AF_Vector3 right, AF_Vector3 up, int materialId) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1;
    return s->addInstance(makeObb(center, halfExtents, right, up), materialId);
}

void AF_SceneSetApertureOpen(AF_SceneHandle scene, float ref, float power,
                             float radius, int samples) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    b->post([ref, power, radius, samples](Scene& s) {
        s.setApertureOpenRef(ref);
        s.setApertureOpenPower(power);
        s.setApertureOpenRadius(radius);
        s.setApertureOpenSamples(samples);
    });
}

void AF_SceneGetApertureOpen(AF_SceneHandle scene, float* outRef, float* outPower,
                             float* outRadius, int* outSamples) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (outRef)     *outRef     = s ? s->apertureOpenRef() : 0.0f;
    if (outPower)   *outPower   = s ? s->apertureOpenPower() : 0.0f;
    if (outRadius)  *outRadius  = s ? s->apertureOpenRadius() : 0.0f;
    if (outSamples) *outSamples = s ? s->apertureOpenSamples() : 0;
}

float AF_SceneMeasureSlitWidth(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0.0f;
    const Vec3 l = toVec3(listener), src = toVec3(source);
    Scene::DiffractionPath paths[4];
    const int np = s->findDiffractionPaths(l, src, paths, 4);
    if (np <= 0) return 0.0f;
    return paths[0].slitWidth;
}

float AF_SceneMeasureApertureOpenness(AF_SceneHandle scene, AF_Vector3 listener,
                                      AF_Vector3 source) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !outPoint) return;
    float t[kNumBands]; float frac = 0.0f; Vec3 p = toVec3(source);
    s->computeSoftOcclusion(toVec3(listener), toVec3(source), t, frac, 32, 0.9f, &p);
    *outPoint = fromVec3(p);
}

int AF_SceneComputeSoftOcclusion(AF_SceneHandle scene,
                                 AF_Vector3 listener, AF_Vector3 source,
                                 float* outTrans, int count, float* outOccFrac) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !dirs || !outEnergy || dirCount <= 0) return;
    std::vector<Vec3> d(static_cast<size_t>(dirCount));
    for (int i = 0; i < dirCount; ++i) d[static_cast<size_t>(i)] = toVec3(dirs[i]);
    s->probeDirectionalEnergy(toVec3(origin), d.data(), dirCount, maxBounces, outEnergy);
}

int AF_SceneAddMesh(AF_SceneHandle scene,
                    const float* verticesXYZ, int vertexCount,
                    const int* indices, int indexCount,
                    AF_Vector3* outLocalCenter, AF_Vector3* outLocalHalfExtents) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1;
    Vec3 c, h;
    const int id = s->addMesh(verticesXYZ, vertexCount, indices, indexCount, &c, &h);
    if (id < 0) return -1;
    if (outLocalCenter) *outLocalCenter = AF_Vector3{c.x, c.y, c.z};
    if (outLocalHalfExtents) *outLocalHalfExtents = AF_Vector3{h.x, h.y, h.z};
    return id;
}

int AF_SceneGetMeshEdgeCount(AF_SceneHandle scene, int geomId) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1;
    return s->meshEdgeCount(geomId);
}

void AF_SceneRemoveMesh(AF_SceneHandle scene, int geomId) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return;
    s->removeMesh(geomId);
}

int AF_SceneAddInstanceMesh(AF_SceneHandle scene, int geomId,
                            AF_Vector3 center, AF_Vector3 halfExtents,
                            AF_Vector3 right, AF_Vector3 up, int materialId) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1;
    return s->addInstance(makeObb(center, halfExtents, right, up), materialId, geomId);
}

void AF_SceneUpdateInstance(AF_SceneHandle scene, int instanceId,
                            AF_Vector3 center, AF_Vector3 halfExtents,
                            AF_Vector3 right, AF_Vector3 up) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    const Obb o = makeObb(center, halfExtents, right, up);
    b->post([instanceId, o](Scene& s) { s.updateInstanceTransform(instanceId, o); });
}

void AF_SceneSetInstanceActive(AF_SceneHandle scene, int instanceId, int active) {
    SceneBox* b = asBox(scene);
    if (b) b->post([instanceId, active](Scene& s) { s.setInstanceActive(instanceId, active != 0); });
}

void AF_SceneClearInstances(AF_SceneHandle scene) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->clearInstances();
}

int AF_SceneComputeTransmissionBands(AF_SceneHandle scene,
                                     AF_Vector3 from, AF_Vector3 to,
                                     float* outGains, int count) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !outGains || count <= 0) return 0;
    float gain[kNumBands];
    s->computeTransmission(toVec3(from), toVec3(to), gain);
    const int n = std::min(count, kNumBands);
    // 内部はエネルギー、ホストへ返すタップのゲインは振幅（material.h の規約）。
    for (int b = 0; b < n; ++b) outGains[b] = std::sqrt(gain[b]);
    return n;
}

int AF_SceneIsOccluded(AF_SceneHandle scene, AF_Vector3 from, AF_Vector3 to) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    return s->isOccluded(toVec3(from), toVec3(to)) ? 1 : 0;
}

float AF_SceneRaycast(AF_SceneHandle scene, AF_Vector3 origin, AF_Vector3 dir,
                      float maxDist) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1.0f;
    const SceneHit hit = s->raycastClosest(toVec3(origin), toVec3(dir), maxDist);
    return hit.hit ? hit.t : -1.0f;
}

int AF_SceneComputeDiffractionBands(AF_SceneHandle scene,
                                    AF_Vector3 from, AF_Vector3 to,
                                    float* outGains, int count) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0.0f;
    return s->occlusionReflected(toVec3(source), toVec3(listener), outBands6, numRays, maxBounces);
}

void AF_SceneOcclusionReflectedMulti(AF_SceneHandle scene, AF_Vector3 listener,
                                     const AF_Vector3* sources, int count,
                                     float* outOcc, float* outBands, float* outDir,
                                     float directWeight, int numRays, int maxBounces) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->buildEdgeCatalog(toVec3(listener), res, maxDist);
}

int AF_SceneEdgeCatalogCount(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->edgeCatalogCount() : 0;
}

void AF_SceneClearEdgeCatalog(AF_SceneHandle scene) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->clearEdgeCatalog();
}

int AF_SceneComputeEarlyReflections(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source,
                                    AF_Vector3* outImagePos, float* outGain,
                                    int maxTaps, int numRays, int maxBounces) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->instanceCount() : 0;
}

// --- リスナー / 音源の保持（段1）---

void AF_SceneSetListener(AF_SceneHandle scene, AF_Vector3 pos) {
    SceneBox* b = asBox(scene);
    if (b) { const Vec3 p = toVec3(pos); b->post([p](Scene& s) { s.setListener(p); }); }
}

void AF_SceneSetListenerOrientation(AF_SceneHandle scene, AF_Vector3 forward, AF_Vector3 up) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    const Vec3 f = toVec3(forward), u = toVec3(up);
    b->post([f, u](Scene& s) { s.setListenerOrientation(f, u); });
}

void AF_SceneSetTapParams(AF_SceneHandle scene, const AF_TapParams* params) {
    SceneBox* b = asBox(scene);
    if (!b || !params) return;
    TapParams p;
    p.distanceRef = params->distanceRef;
    p.diffractionDistanceRef = params->diffractionDistanceRef;
    p.diffractionDistancePower = params->diffractionDistancePower;
    p.airAbsorptionScale = params->airAbsorptionScale;
    p.transmissionTilt = params->transmissionTilt;
    p.transmissionHighCutDb = params->transmissionHighCutDb;
    p.transmissionGainDb = params->transmissionGainDb;
    p.diffractionGainDb = params->diffractionGainDb;
    p.diffractionDistanceOnly = params->diffractionDistanceOnly != 0;
    p.diffractionHighCutDb = params->diffractionHighCutDb;
    p.tapSmoothTime = params->tapSmoothTime;
    p.directionSmoothTime = params->directionSmoothTime;
    p.steerThreshold = params->steerThreshold;
    p.diffractionHrtf = params->diffractionHrtf != 0;
    p.diffractionHrtfMarginDb = params->diffractionHrtfMarginDb;
    p.reverbRatioExponent = params->reverbRatioExponent;
    p.reverbRatioCeiling = params->reverbRatioCeiling;
    p.roomBlendRadius = params->roomBlendRadius;
    p.reverbShareFade = params->reverbShareFade != 0;
    p.fallbackRt60 = params->fallbackRt60;
    p.maxTaps = params->maxTaps;
    p.diffractionTapReserve = params->diffractionTapReserve;
    b->post([p](Scene& s) { s.setTapParams(p); });
}

int AF_SceneGetVoiceProgram(AF_SceneHandle scene, int index, AF_VoiceProgram* out) {
    SceneBox* b = asBox(scene);
    if (!b || !out) return 0;
    VoiceProgram vp;
    if (!b->scene.voiceProgram(index, vp, b->snapshot())) return 0;
    std::memset(out, 0, sizeof(AF_VoiceProgram));
    out->count = std::min(vp.count, AF_PROGRAM_MAX_TAPS);
    for (int i = 0; i < out->count; ++i) {
        const ProgramTap& t = vp.taps[i];
        AF_ProgramTap& o = out->taps[i];
        o.delayMs = t.delayMs;
        for (int k = 0; k < 6; ++k) o.gain6[k] = t.gain[k];
        o.panL = t.panL; o.panR = t.panR;
        o.dirX = t.dir[0]; o.dirY = t.dir[1]; o.dirZ = t.dir[2];
        o.arrX = t.arr[0]; o.arrY = t.arr[1]; o.arrZ = t.arr[2];
        o.type = t.type; o.hrtfWeight = t.hrtfWeight;
    }
    out->directDirX = vp.directDir[0]; out->directDirY = vp.directDir[1]; out->directDirZ = vp.directDir[2];
    out->hrtfTapIndex = vp.hrtfTapIndex;
    out->hrtfDirX = vp.hrtfDir[0]; out->hrtfDirY = vp.hrtfDir[1]; out->hrtfDirZ = vp.hrtfDir[2];
    out->itdgMs = vp.itdgMs; out->sourceLevel = vp.sourceLevel; out->freeFieldDirect = vp.freeFieldDirect;
    out->tailShapeIndex = vp.tailShapeIndex;
    out->tailRatio = vp.tailRatio; out->tailRatioPhysical = vp.tailRatioPhysical;
    out->mixingTimeMs = vp.mixingTimeMs; out->roomShare = vp.roomShare; out->tier = vp.tier;
    return 1;
}

void AF_SceneSetSource(AF_SceneHandle scene, unsigned long long id, AF_Vector3 pos) {
    SceneBox* b = asBox(scene);
    if (b) { const Vec3 p = toVec3(pos); b->post([id, p](Scene& s) { s.setSource(id, p); }); }
}

void AF_SceneRemoveSource(AF_SceneHandle scene, unsigned long long id) {
    SceneBox* b = asBox(scene);
    if (b) b->post([id](Scene& s) { s.removeSource(id); });
}

void AF_SceneClearSources(AF_SceneHandle scene) {
    SceneBox* b = asBox(scene);
    if (b) b->post([](Scene& s) { s.clearSources(); });
}

int AF_SceneSourceCount(AF_SceneHandle scene) {
    SceneBox* b = asBox(scene);
    if (!b) return 0;
    const Scene::Results* r = b->snapshot();
    return r ? static_cast<int>(r->id.size()) : b->scene.sourceCount();
}

// --- バッチ更新（段2）---

void AF_SceneSetUpdateConfig(AF_SceneHandle scene, const AF_UpdateConfig* cfg) {
    SceneBox* b = asBox(scene);
    if (!b || !cfg) return;
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
    c.earlyModel = cfg->earlyModel;
    c.earlyFaceSubTaps = cfg->earlyFaceSubTaps;
    c.echogramSkipFirstOrder = cfg->echogramSkipFirstOrder != 0;
    b->post([c](Scene& s) { s.setUpdateConfig(c); });
}

void AF_SceneUpdate(AF_SceneHandle scene, float dt) {
    SceneBox* b = asBox(scene);
    if (b) b->update(dt);
}

void AF_SceneSetAsync(AF_SceneHandle scene, int enable) {
    SceneBox* b = asBox(scene);
    if (b) b->setAsync(enable != 0);
}

int AF_SceneIsAsync(AF_SceneHandle scene) {
    SceneBox* b = asBox(scene);
    return (b && b->async()) ? 1 : 0;
}

void AF_SceneAsyncWait(AF_SceneHandle scene) {
    SceneBox* b = asBox(scene);
    if (b) b->wait();
}

void AF_SceneGetUpdateStats(AF_SceneHandle scene, float* outComputeMs, int* outLagFrames,
                            int* outSkippedFrames, int* outQueued) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    b->stats(outComputeMs, outLagFrames, outSkippedFrames, outQueued);
}

int AF_SceneSourceIndex(AF_SceneHandle scene, unsigned long long id) {
    SceneBox* b = asBox(scene);
    if (!b) return -1;
    const Scene::Results* r = b->snapshot();
    const int i = b->scene.sourceIndexOf(id, r);
    if (i >= 0 || !r) return i;
    // 写しにまだ無い id（登録直後）。同期の口で引き直す（仕事を待つのは登録直後の数フレームだけ）。
    SyncGuard g(b);
    return g.get()->sourceIndexOf(id);
}

void AF_SceneGetSourceOcclusion(AF_SceneHandle scene, int index, float* out6) {
    SceneBox* b = asBox(scene);
    if (b) b->scene.getSourceOcclusion(index, out6, b->snapshot());
}

float AF_SceneGetSourceOcclusionScalar(AF_SceneHandle scene, int index) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.getSourceOcclusionScalar(index, b->snapshot()) : 0.0f;
}

void AF_SceneGetSourceArrivalDir(AF_SceneHandle scene, int index, float* out3) {
    SceneBox* b = asBox(scene);
    if (b) b->scene.getSourceArrivalDir(index, out3, b->snapshot());
}

int AF_SceneGetEarlyReflections(AF_SceneHandle scene, int index,
                                AF_Vector3* outPos, float* outGain6, int maxTaps) {
    SceneBox* b = asBox(scene);
    if (!b || !outPos || !outGain6 || maxTaps <= 0) return 0;
    std::vector<Vec3> tmp(static_cast<size_t>(maxTaps));
    const int n = b->scene.getEarlyReflections(index, tmp.data(), outGain6, maxTaps, b->snapshot());
    for (int i = 0; i < n; ++i) outPos[i] = fromVec3(tmp[static_cast<size_t>(i)]);
    return n;
}

int AF_SceneBakeStaticFaces(AF_SceneHandle scene, float cellSize, int subTaps) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->bakeStaticFaces(cellSize, subTaps) : 0;
}

void AF_SceneClearFaceBake(AF_SceneHandle scene) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->clearFaceBake();
}

int AF_SceneFaceBakeFaceCount(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->faceBakeFaceCount() : 0;
}

void AF_SceneSetInstanceDynamic(AF_SceneHandle scene, int instanceId, int dynamic) {
    SceneBox* b = asBox(scene);
    if (b) b->post([instanceId, dynamic](Scene& s) { s.setInstanceDynamic(instanceId, dynamic != 0); });
}

int AF_SceneBvhNodeCount(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->bvhNodeCount() : 0;
}

int AF_SceneBvhOrderCount(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->bvhOrderCount() : 0;
}

int AF_SceneExportBvh(AF_SceneHandle scene, AF_BvhNode* outNodes, int maxNodes, int* outOrder, int maxOrder) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    static_assert(sizeof(AF_BvhNode) == sizeof(Scene::BvhExportNode), "AF_BvhNode と BvhExportNode の並びを揃えること");
    return s->exportBvh(reinterpret_cast<Scene::BvhExportNode*>(outNodes), outNodes ? maxNodes : 0,
                        outOrder, outOrder ? maxOrder : 0);
}

int AF_SceneGetInstance(AF_SceneHandle scene, int instanceId, AF_InstanceDesc* out) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !out) return 0;
    Obb obb; int mat = 0, geom = -1; bool active = false, moved = false, dyn = false;
    if (!s->getInstanceDesc(instanceId, obb, mat, geom, active, moved, dyn)) return 0;
    out->center = fromVec3(obb.center); out->halfExtents = fromVec3(obb.halfExtents);
    out->axisX = fromVec3(obb.axisX); out->axisY = fromVec3(obb.axisY); out->axisZ = fromVec3(obb.axisZ);
    out->materialId = mat; out->geomId = geom;
    out->active = active ? 1 : 0; out->moved = moved ? 1 : 0; out->dynamicTag = dyn ? 1 : 0;
    return 1;
}

int AF_SceneFaceBakeBytes(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->faceBakeBytes() : 0;
}

int AF_SceneFaceBakeExport(AF_SceneHandle scene, void* out, int cap) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->faceBakeExport(out, cap) : 0;
}

int AF_SceneFaceBakeImport(AF_SceneHandle scene, const void* data, int size) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return (s && s->faceBakeImport(data, size)) ? 1 : 0;
}

int AF_SceneGetDiffractionSources(AF_SceneHandle scene, int index,
                                  AF_Vector3* outPos, float* outGain, int maxSrc) {
    SceneBox* b = asBox(scene);
    if (!b || !outPos || !outGain || maxSrc <= 0) return 0;
    std::vector<Vec3> tmp(static_cast<size_t>(maxSrc));
    const int n = b->scene.getDiffractionSources(index, tmp.data(), outGain, maxSrc, b->snapshot());
    for (int i = 0; i < n; ++i) outPos[i] = fromVec3(tmp[static_cast<size_t>(i)]);
    return n;
}

int AF_SceneGetEchogramBands(AF_SceneHandle scene, int index, float* outBins, int numBins) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.getEchogramBands(index, outBins, numBins, b->snapshot()) : 0;
}

}  // extern "C"

void AF_SceneSetApertureSpread(AF_SceneHandle scene, int points) {
    SceneBox* b = asBox(scene);
    if (b) b->post([points](Scene& s) { s.setApertureSpread(points); });
}

int AF_SceneGetApertureSpread(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->apertureSpread() : 0;
}

int AF_SceneComputeDiffractionSourceBands(AF_SceneHandle scene,
                                          AF_Vector3 listener, AF_Vector3 source,
                                          AF_Vector3* outPos, float* outGain,
                                          float* outBand6, int maxSrc) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !outPos || !outGain || maxSrc <= 0) return 0;
    std::vector<Vec3> pos(static_cast<size_t>(maxSrc));
    const int n = s->computeDiffractionSourceBands(toVec3(listener), toVec3(source),
                                                   pos.data(), outGain, outBand6, maxSrc);
    for (int i = 0; i < n; ++i) outPos[i] = fromVec3(pos[static_cast<size_t>(i)]);
    return n;
}

int AF_SceneAddPortal(AF_SceneHandle scene, AF_Vector3 center,
                      AF_Vector3 axisU, AF_Vector3 axisV, float halfU, float halfV) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return -1;
    return s->addPortal(toVec3(center), toVec3(axisU), toVec3(axisV), halfU, halfV);
}

void AF_SceneUpdatePortal(AF_SceneHandle scene, int id, AF_Vector3 center,
                          AF_Vector3 axisU, AF_Vector3 axisV, float halfU, float halfV) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    const Vec3 c = toVec3(center), u = toVec3(axisU), v = toVec3(axisV);
    b->post([id, c, u, v, halfU, halfV](Scene& s) { s.updatePortal(id, c, u, v, halfU, halfV); });
}

// ★ホストは毎フレーム押してくる。値が変わったときだけ作り直すこと
//   （rebuildAutoPortals は部屋グラフ全体の開口を舐めるので、毎フレームは無駄）。
void AF_SceneSetAutoPortals(AF_SceneHandle scene, int enable) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    b->post([enable](Scene& s) {
        if (s.autoPortals() == (enable != 0)) return;
        s.setAutoPortals(enable != 0);
        s.rebuildAutoPortals();          // 切り替えた瞬間に反映する（次の update を待たない）
    });
}

// ★ホストは毎フレーム押してくる。値が変わったときだけプールを作り直すこと
//   （毎フレーム作り直したらスレッドの起こし直しで元も子もない）。setWorkerThreads が中で見ている。
void AF_SceneSetWorkerThreads(AF_SceneHandle scene, int threads) {
    SceneBox* b = asBox(scene);
    if (b) b->post([threads](Scene& s) { s.setWorkerThreads(threads); });   // プールの作り直しは仕事の外で
}

int AF_SceneGetWorkerThreads(AF_SceneHandle scene) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.workerThreads() : 0;   // int の読みだけ
}

int AF_SceneTransmissionCarriers(AF_SceneHandle scene,
                                 AF_Vector3 listener, AF_Vector3 source,
                                 int* outInstance, int* outMaterial,
                                 float* outLossDb, int maxCount) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    return s->transmissionCarriers(toVec3(listener), toVec3(source),
                                   outInstance, outMaterial, outLossDb, maxCount);
}

void AF_SceneSetSourceTier(AF_SceneHandle scene, unsigned long long id, int tier) {
    SceneBox* b = asBox(scene);
    if (b) b->post([id, tier](Scene& s) { s.setSourceTier(id, tier); });
}

void AF_SceneSetSourceAudibleRadius(AF_SceneHandle scene, unsigned long long id, float metres) {
    SceneBox* b = asBox(scene);
    if (b) b->post([id, metres](Scene& s) { s.setSourceAudibleRadius(id, metres); });
}

int AF_SceneGetSourceTierEffective(AF_SceneHandle scene, int index) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.effectiveTier(index, b->snapshot()) : -1;
}

void AF_SceneSetTierBudget(AF_SceneHandle scene, int exactMax, int simpleMax) {
    SceneBox* b = asBox(scene);
    if (b) b->post([exactMax, simpleMax](Scene& s) { s.setTierBudget(exactMax, simpleMax); });
}

void AF_SceneSetSourceLoudness(AF_SceneHandle scene, unsigned long long id, float gainLinear) {
    SceneBox* b = asBox(scene);
    if (b) b->post([id, gainLinear](Scene& s) { s.setSourceLoudness(id, gainLinear); });
}

void AF_SceneSetSourceImportance(AF_SceneHandle scene, unsigned long long id, float importance, int pinned) {
    SceneBox* b = asBox(scene);
    if (b) b->post([id, importance, pinned](Scene& s) { s.setSourceImportance(id, importance, pinned != 0); });
}

float AF_SceneGetSourcePriority(AF_SceneHandle scene, int index) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.sourcePriority(index, b->snapshot()) : -1.0f;
}

int AF_SceneGetTierProbeIndex(AF_SceneHandle scene) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.tierProbeIndex(b->snapshot()) : -1;
}

/* ── キャプチャ（サウンドデバッグツール。docs/SOUND_DEBUG_TOOL.md）───────── */

void AF_SceneCaptureBegin(AF_SceneHandle scene, int prerollFrames, int postrollFrames,
                          int maxSources, int sampleRate, int recordPcm) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return;
    acoustic::dbg::CaptureConfig c;
    if (prerollFrames  > 0) c.prerollFrames  = prerollFrames;
    if (postrollFrames >= 0) c.postrollFrames = postrollFrames;
    if (maxSources > 0) c.maxSources = maxSources;
    if (sampleRate > 0) c.sampleRate = sampleRate;
    c.recordPcm = (recordPcm != 0);
    s->captureBegin(c);
}

int AF_SceneGetTailShapeIndex(AF_SceneHandle scene, int index) {
    SceneBox* b = asBox(scene);
    return b ? b->scene.tailShapeIndex(index, b->snapshot()) : -1;
}

void AF_SceneDebugGetStagePhase(AF_SceneHandle scene, int* out8) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s && out8) s->debugGetStagePhase(out8);
}

void AF_SceneDebugSetStagePhase(AF_SceneHandle scene, const int* in8) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s && in8) s->debugSetStagePhase(in8);
}

void AF_SceneCaptureEnd(AF_SceneHandle scene) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->captureEnd();
}

void AF_SceneCaptureMark(AF_SceneHandle scene) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->captureMark();
}

int AF_SceneCaptureStatus(AF_SceneHandle scene, int* outFramesHeld) {
    SceneBox* b = asBox(scene);
    Scene* s = b ? &b->scene : nullptr;   // 数の読みだけ（毎フレーム呼ばれるので仕事を待たない）
    if (!s) return 0;
    if (outFramesHeld) *outFramesHeld = s->captureFramesHeld();
    /* 0=止まっている 1=録っている 2=前後が揃った（保存できる） */
    return !s->captureActive() ? 0 : (s->captureReady() ? 2 : 1);
}

void AF_SceneCapturePushAudio(AF_SceneHandle scene, const float* interleavedStereo, int frames) {
    SceneBox* b = asBox(scene);
    if (b) b->scene.capturePushAudio(interleavedStereo, frames);   // オーディオスレッド。ロック無し（capture.h）
}

int AF_SceneCaptureWrite(AF_SceneHandle scene, const char* path,
                         const char* sceneName, unsigned int dllHash) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !path) return 0;
    return s->captureWrite(path, sceneName, dllHash) ? 1 : 0;
}

void AF_SceneSetAutoPortalMinArea(AF_SceneHandle scene, float m2) {
    SceneBox* b = asBox(scene);
    if (!b) return;
    b->post([m2](Scene& s) {
        if (s.autoPortalMinArea() == m2) return;
        s.setAutoPortalMinArea(m2);
        s.rebuildAutoPortals();
    });
}

int AF_SceneGetPortalCounts(AF_SceneHandle scene, int* outAuto, int* outManual) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    if (outAuto)   *outAuto   = s->autoPortalCount();
    if (outManual) *outManual = s->manualPortalCount();
    return s->portalCount();
}

int AF_SceneGetPortal(AF_SceneHandle scene, int id, AF_Vector3* outCenter,
                      AF_Vector3* outAxisU, AF_Vector3* outAxisV,
                      float* outHalfU, float* outHalfV) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || id < 0 || id >= s->portalCount()) return 0;
    const auto& p = s->portal(id);
    if (outCenter) *outCenter = fromVec3(p.center);
    if (outAxisU)  *outAxisU  = fromVec3(p.axisU);
    if (outAxisV)  *outAxisV  = fromVec3(p.axisV);
    if (outHalfU)  *outHalfU  = p.halfU;
    if (outHalfV)  *outHalfV  = p.halfV;
    return 1;
}

int AF_SceneGetPortalRooms(AF_SceneHandle scene, int id,
                           int* outRoomA, int* outRoomB, int* outToOutside) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || id < 0 || id >= s->portalCount()) return 0;
    const auto& p = s->portal(id);
    if (outRoomA) *outRoomA = p.roomA;
    if (outRoomB) *outRoomB = p.roomB;
    if (outToOutside) *outToOutside = p.toOutside ? 1 : 0;
    return 1;
}

void AF_SceneSetOutsideApertures(AF_SceneHandle scene, int on) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on](Scene& s) { s.setOutsideApertures(on); });
}

int AF_ScenePortalDiffuseCoupling(AF_SceneHandle scene, int id, float* out6) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !out6) return 0;
    float g[6];
    if (!s->portalDiffuseCoupling(id, g)) return 0;
    for (int b = 0; b < 6; ++b) out6[b] = g[b];
    return 1;
}

/* 【計測用】回折の可視判定のゲートを個別に切る。0 = 全部有効（本番）。 */
/* 【(B)】稜線からポータルを生成してフレネル積分する。既定 0（従来の前川＋開口積分）。
 * 1 点で回折を表すのをやめる ── 隙間の幅も角の死角も、矩形の中で影が落ちることで
 * 自動的に解ける。ナイフエッジ回折の厳密解はもともとこの形の積分。 */
/* 【計測用】直近の開口積分で矩形に写った遮蔽物の枚数。-1 は未実行。 */
void AF_SceneDebugPortalIntegral(AF_SceneHandle scene, double* numer, double* denom, float* limU) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return;
    if (numer) *numer = s->dbgNumer();
    if (denom) *denom = s->dbgDenom();
    if (limU)  *limU  = s->dbgLimU();
}

void AF_SceneSetDiffractionSingleModel(AF_SceneHandle scene, int on) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on](Scene& s) { s.setDiffractionSingleModel(on); });
}

void AF_SceneDebugDiffractionCounts(AF_SceneHandle scene, int* raw, int* cut, int* clusters) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (s) s->diffractionSearchCounts(raw, cut, clusters);
}

void AF_SceneSetInsideOtherContinuous(AF_SceneHandle scene, int on, float scale) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on, scale](Scene& s) { s.setInsideOtherContinuous(on, scale); });
}

void AF_SceneSetKeepDoubleOpen(AF_SceneHandle scene, int on) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on](Scene& s) { s.setKeepDoubleOpen(on); });
}

int AF_SceneDebugDiffractionPath(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source,
                                 float* out17, int which) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    return s->debugDiffractionPath(toVec3(listener), toVec3(source), out17, which);
}

int AF_SceneDebugPortalPolys(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->dbgPortalPolys() : -1;
}

void AF_SceneSetEdgePortals(AF_SceneHandle scene, int enable) {
    SceneBox* b = asBox(scene);
    if (b) b->post([enable](Scene& s) { s.setEdgePortals(enable != 0); });
}

/* 矩形の半幅をフレネル半径の何倍にするか（既定 1.0）。 */
void AF_SceneSetEdgePortalSpan(AF_SceneHandle scene, float k) {
    SceneBox* b = asBox(scene);
    if (b) b->post([k](Scene& s) { s.setEdgePortalSpan(k); });
}

void AF_SceneSetDiffractionGateMask(AF_SceneHandle scene, int mask) {
    SceneBox* b = asBox(scene);
    if (b) b->post([mask](Scene& s) { s.setDiffractionGateMask(mask); });
}

void AF_SceneSetPortalGovernRange(AF_SceneHandle scene, float meters) {
    SceneBox* b = asBox(scene);
    if (b) b->post([meters](Scene& s) { s.setPortalGovernRange(meters); });
}

int AF_SceneMeasurePortal(AF_SceneHandle scene, int id,
                          AF_Vector3 listener, AF_Vector3 source,
                          float* outFrac6, AF_Vector3* outPoint) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !outFrac6 || id < 0 || id >= s->portalCount()) return 0;
    Vec3 p(0, 0, 0);
    const bool ok = s->portalOpenBands(s->portal(id), toVec3(listener), toVec3(source),
                                       outFrac6, &p);
    if (outPoint) *outPoint = fromVec3(p);
    return ok ? 1 : 0;
}

void AF_SceneSetApertureIsTransmission(AF_SceneHandle scene, int on) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on](Scene& s) { s.setApertureIsTransmission(on); });
}

int AF_SceneRoomCount(AF_SceneHandle scene) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? static_cast<int>(s->roomGraph().rooms.size()) : 0;
}

int AF_SceneRoomAt(AF_SceneHandle scene, AF_Vector3 p) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->roomAt(toVec3(p)) : -1;
}

int AF_SceneRoomInfo(AF_SceneHandle scene, int room, float* outVolume,
                     AF_Vector3* outCentroid, AF_Vector3* outMin, AF_Vector3* outMax) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SceneBox* b = asBox(scene);
    if (b) b->post([meters](Scene& s) { s.setRoomCellSize(meters); });
}

int AF_SceneRoomGridDegraded(AF_SceneHandle scene, float* outRequested, float* outActual,
                             double* outVoxels, double* outMaxVoxels) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SceneBox* b = asBox(scene);
    if (b) b->post([voxels](Scene& s) { s.setRoomBrick(voxels); });
}

void AF_SceneSetRoomSeedRadius(AF_SceneHandle scene, float meters) {
    SceneBox* b = asBox(scene);
    if (b) b->post([meters](Scene& s) { s.setRoomSeedRadius(meters); });
}

void AF_SceneSetDiffractionFlat(AF_SceneHandle scene, int flat) {
    SceneBox* b = asBox(scene);
    if (b) b->post([flat](Scene& s) { s.setDiffractionFlat(flat != 0); });
}

void AF_SceneSetRoomChamferFull(AF_SceneHandle scene, int full) {
    SceneBox* b = asBox(scene);
    if (b) b->post([full](Scene& s) { s.setRoomChamferFull(full != 0); });
}

int AF_SceneRoomWeights(AF_SceneHandle scene, AF_Vector3 p, float radius,
                        int* outRooms, float* outWeights, int maxOut) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    return s->roomWeights(toVec3(p), radius, outRooms, outWeights, maxOut);
}

float AF_SceneRoomShareTotalAt(AF_SceneHandle scene, AF_Vector3 p, float radius) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();   // 幾何の問い合わせ（非同期でも仕事を待たない）
    return s ? s->roomShareTotalAt(toVec3(p), radius) : 0.0f;
}

int AF_SceneRoomShareAt(AF_SceneHandle scene, AF_Vector3 p, float radius,
                        int* outRooms, float* outWeights, int maxOut) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !outRooms || !outWeights || maxOut <= 0) return 0;
    return s->roomShare(toVec3(p), radius, outRooms, outWeights, maxOut);
}

int AF_SceneDebugSurvivalParts(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source,
                               float* outSoft6, float* outDif6) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s || !outSoft6 || !outDif6) return 0;
    float g[6];
    s->computeDirectSoft(toVec3(listener), toVec3(source), g, 8, 0.4f, nullptr, false, outSoft6, outDif6);
    return 6;
}

float AF_SceneRoomVolumeAt(AF_SceneHandle scene, AF_Vector3 p, float radius) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? s->roomVolumeAt(toVec3(p), radius) : 0.0f;
}

int AF_SceneRt60At(AF_SceneHandle scene, AF_Vector3 p, float radius, float* out, int count) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    if (!s) return 0;
    return s->rt60At(toVec3(p), radius, out, count);
}

int AF_SceneRoomAcoustics(AF_SceneHandle scene, int room, float* outSurface,
                          float* outOpenArea, float* outAbsorb6, float* outRt60_6) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    return s ? static_cast<int>(s->roomGraph().apertures.size()) : 0;
}

int AF_SceneApertureInfo(AF_SceneHandle scene, int index,
                         float* outArea, AF_Vector3* outCenter, AF_Vector3* outNormal,
                         int* outRoomA, int* outRoomB) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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
    SceneBox* b = asBox(scene);
    if (b) b->post([p](Scene& s) { s.setApertureContrast(p); });
}

void AF_SceneSetApertureTimbre(AF_SceneHandle scene, float k) {
    SceneBox* b = asBox(scene);
    if (b) b->post([k](Scene& s) { s.setApertureTimbre(k); });
}

void AF_SceneSetUseBtm(AF_SceneHandle scene, int on) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on](Scene& s) { s.setUseBtm(on); });
}

void AF_SceneSetDirectPenumbra(AF_SceneHandle scene, int on) {
    SceneBox* b = asBox(scene);
    if (b) b->post([on](Scene& s) { s.setDirectPenumbra(on); });
}

void AF_SceneSetApertureLaw(AF_SceneHandle scene, int law) {
    SceneBox* b = asBox(scene);
    if (b) b->post([law](Scene& s) { s.setApertureLaw(law); });
}

void AF_SceneSetApertureDeltaWeight(AF_SceneHandle scene, float w) {
    SceneBox* b = asBox(scene);
    if (b) b->post([w](Scene& s) { s.setApertureDeltaWeight(w); });
}

void AF_SceneSetSlitBandSlope(AF_SceneHandle scene, float slope) {
    SceneBox* b = asBox(scene);
    if (b) b->post([slope](Scene& s) { s.setSlitBandSlope(slope); });
}

int AF_SceneMeasureApertureFresnel(AF_SceneHandle scene, AF_Vector3 listener,
                                   AF_Vector3 source, float* outFrac6) {
    ReadGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
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

/* ============================================================================
 * 録った .afcap を開いて調べる（ホスト側の道具用）
 *
 * ★走査は**この DLL の中**で回す。回帰テストが呼ぶのと同じ関数でなければ
 *   「道具は見つけたのに検査は通る」が起きるため。C# へ写経すると必ずずれる。
 * ========================================================================== */

namespace {

struct CaptureBundle {
    acoustic::dbg::CaptureFile file;
    std::vector<af::scan::Mark> marks;
    bool scanned = false;
    std::vector<unsigned long long> ids;
};

CaptureBundle* asCapture(AF_CaptureHandle h) { return static_cast<CaptureBundle*>(h); }

void copyStr(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    for (; src && src[i] && i < cap - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

}  // namespace

AF_CaptureHandle AF_CaptureOpen(const char* path) {
    if (!path) return nullptr;
    CaptureBundle* b = new (std::nothrow) CaptureBundle();
    if (!b) return nullptr;
    if (!acoustic::dbg::readCapture(path, b->file)) { delete b; return nullptr; }
    for (const auto& row : b->file.sources)
        for (const auto& s : row) {
            bool seen = false;
            for (auto id : b->ids) if (id == s.id) { seen = true; break; }
            if (!seen) b->ids.push_back(s.id);
        }
    return b;
}

void AF_CaptureClose(AF_CaptureHandle cap) { delete asCapture(cap); }

int AF_CaptureGetInfo(AF_CaptureHandle cap, AF_CaptureInfo* out) {
    CaptureBundle* b = asCapture(cap);
    if (!b || !out) return 0;
    out->frames         = b->file.frames;
    out->sampleRate     = b->file.sampleRate;
    out->maxSources     = b->file.maxSources;
    out->preroll        = b->file.preroll;
    out->postroll       = b->file.postroll;
    out->workerThreads  = b->file.workerThreads;
    out->markedFrame    = static_cast<int>(b->file.markedFrame);
    out->dllHash        = b->file.dllHash;
    out->boxCount       = static_cast<int>(b->file.boxes.size());
    out->meshCount      = b->file.meshCount;
    out->materialCount  = static_cast<int>(b->file.materials.size());
    out->pcmFrames      = static_cast<int>(b->file.pcm.size() / 2);
    out->sourceCount    = static_cast<int>(b->ids.size());
    return 1;
}

int AF_CaptureGetSceneName(AF_CaptureHandle cap, char* buf, int bufSize) {
    CaptureBundle* b = asCapture(cap);
    if (!b || !buf || bufSize <= 0) return 0;
    copyStr(buf, bufSize, b->file.scene.c_str());
    return static_cast<int>(std::strlen(buf));
}

int AF_CaptureGetSourceIds(AF_CaptureHandle cap, unsigned long long* out, int maxOut) {
    CaptureBundle* b = asCapture(cap);
    if (!b || !out || maxOut <= 0) return 0;
    const int n = std::min<int>(maxOut, static_cast<int>(b->ids.size()));
    for (int i = 0; i < n; ++i) out[i] = b->ids[static_cast<size_t>(i)];
    return n;
}

int AF_CaptureScan(AF_CaptureHandle cap, AF_CaptureMark* out, int maxOut) {
    CaptureBundle* b = asCapture(cap);
    if (!b) return 0;
    if (!b->scanned) { b->marks = af::scan::scan(b->file); b->scanned = true; }
    const int total = static_cast<int>(b->marks.size());
    if (!out || maxOut <= 0) return total;
    const int n = std::min(maxOut, total);
    for (int i = 0; i < n; ++i) {
        const af::scan::Mark& m = b->marks[static_cast<size_t>(i)];
        out[i].sourceId = m.sourceId;
        out[i].frame    = m.frame;
        out[i].seconds  = m.seconds;
        out[i].amount   = m.amount;
        out[i].reserved = 0;
        copyStr(out[i].templateName, static_cast<int>(sizeof(out[i].templateName)),
                m.templateName);
        copyStr(out[i].what, static_cast<int>(sizeof(out[i].what)), m.what);
    }
    return total;
}

int AF_CaptureScanRuns(AF_CaptureHandle cap, AF_CaptureRun* out, int maxOut) {
    CaptureBundle* b = asCapture(cap);
    if (!b) return 0;
    if (!b->scanned) { b->marks = af::scan::scan(b->file); b->scanned = true; }
    /* ★まとめ方は AfCapScan と同じ関数（af::scan::groupRuns）。
     *   画面とコマンドラインで数が違うと、どちらを信じるか分からなくなる。 */
    const std::vector<af::scan::MarkRun> runs = af::scan::groupRuns(b->marks);
    const int total = static_cast<int>(runs.size());
    if (!out || maxOut <= 0) return total;
    const int n = std::min(maxOut, total);
    for (int i = 0; i < n; ++i) {
        const af::scan::MarkRun& r = runs[static_cast<size_t>(i)];
        out[i].sourceId     = r.first.sourceId;
        out[i].firstFrame   = r.first.frame;
        out[i].lastFrame    = r.lastFrame;
        out[i].count        = r.count;
        out[i].reserved     = 0;
        out[i].firstSeconds = r.first.seconds;
        out[i].lastSeconds  = r.lastSeconds;
        out[i].worst        = r.worst;
        out[i].reserved2    = 0.0f;
        copyStr(out[i].templateName, static_cast<int>(sizeof(out[i].templateName)),
                r.first.templateName);
        copyStr(out[i].what, static_cast<int>(sizeof(out[i].what)), r.first.what);
    }
    return total;
}

int AF_CaptureGetPcm(AF_CaptureHandle cap, float* outInterleaved, int maxFrames) {
    CaptureBundle* b = asCapture(cap);
    if (!b || !outInterleaved || maxFrames <= 0) return 0;
    const int have = static_cast<int>(b->file.pcm.size() / 2);
    const int n = std::min(maxFrames, have);
    for (int i = 0; i < n * 2; ++i)
        outInterleaved[i] = static_cast<float>(b->file.pcm[static_cast<size_t>(i)]) / 32768.0f;
    return n;
}

int AF_CaptureApplyFrame(AF_SceneHandle scene, AF_CaptureHandle cap, int frameIndex) {
    SyncGuard afGuard(asBox(scene)); Scene* s = afGuard.get();
    CaptureBundle* b = asCapture(cap);
    if (!s || !b || frameIndex < 0 || frameIndex >= b->file.frames) return 0;
    const size_t k = static_cast<size_t>(frameIndex);
    /* 押す順は録音時と同じでなければならない: 動いた実体 → リスナー → 音源。 */
    for (const auto& m : b->file.moved[k]) {
        const AF_Vector3 c{m.cx, m.cy, m.cz}, he{m.hx, m.hy, m.hz};
        const AF_Vector3 r{m.rx, m.ry, m.rz}, u{m.ux, m.uy, m.uz};
        s->updateInstanceTransform(m.instance, makeObb(c, he, r, u));
    }
    const auto& g = b->file.global[k];
    s->setListener(Vec3{g.lx, g.ly, g.lz});
    for (const auto& src : b->file.sources[k]) s->setSource(src.id, Vec3{src.sx, src.sy, src.sz});
    /* ★段の位相は**リスナーを押したあと**に戻す（周回の基準点を合わせるため）。
     *   戻さないと尾が永久にずれる（docs/SOUND_DEBUG_TOOL.md §3.1）。 */
    if (frameIndex == 0) {
        int ph[8];
        for (int i = 0; i < 8; ++i) ph[i] = g.phase[i];
        s->debugSetStagePhase(ph);
    }
    return 1;
}

int AF_CaptureEmitCase(AF_CaptureHandle cap, int markIndex, const char* testName,
                       char* buf, int bufSize) {
    CaptureBundle* b = asCapture(cap);
    if (!b || !buf || bufSize <= 0) return 0;
    if (!b->scanned) { b->marks = af::scan::scan(b->file); b->scanned = true; }
    if (markIndex < 0 || markIndex >= static_cast<int>(b->marks.size())) return 0;
    const std::string src = af::emit::emitCase(
        b->file, b->marks[static_cast<size_t>(markIndex)],
        (testName && *testName) ? testName : "testGenerated_FromCapture");
    copyStr(buf, bufSize, src.c_str());
    return static_cast<int>(src.size());
}
