/* Export/world_api.cpp ── 新コア（Flow）の C API の実装（段 4）
 *
 * 役割: acoustic_world.h の関数を Flow/world.h に流すだけ。数値の変換（AF_Vector3 ⇄ Vec3、Obb の組み立て）はここ。
 * 判断や計算はしない（それは Flow の中）。スレッド: 全部メインスレッドから。オーディオスレッドから呼ぶ物は無い
 * （Render は AF_Voice* / AF_FdnMix* 側）。
 */
#include <new>
#include "acoustic_world.h"
#include "Core/material.h"
#include "Core/vec3.h"
#include "Core/aabb.h"
#include "Dsp/fdn_room_mix.h"
#include "Dsp/voice_renderer.h"
#include "Flow/world.h"

namespace {
using acoustic::Vec3;
using acoustic::Obb;
acoustic::flow::World* asWorld(AF_WorldHandle h) { return static_cast<acoustic::flow::World*>(h); }
Vec3 V(AF_Vector3 v) { return Vec3(v.x, v.y, v.z); }
Obb makeObb(AF_Vector3 c, AF_Vector3 he, AF_Vector3 ax, AF_Vector3 ay) {
    Obb b;
    b.center = V(c); b.halfExtents = V(he);
    b.axisX = acoustic::normalized(V(ax));
    b.axisY = acoustic::normalized(V(ay));
    b.axisZ = acoustic::normalized(acoustic::cross(b.axisX, b.axisY));   // 左手系: z = x × y
    return b;
}
acoustic::AcousticMaterial presetMaterial(int preset) {
    using acoustic::AcousticMaterial;
    switch (preset) {
        case 1: return AcousticMaterial::concrete();
        case 2: return AcousticMaterial::glass();
        case 3: return AcousticMaterial::opaque();
        case 4: return AcousticMaterial::woodDoor();
        case 5: return AcousticMaterial::woodRoom();
        case 6: return AcousticMaterial::stone();
        case 7: return AcousticMaterial::cave();
        case 8: return AcousticMaterial::snow();
        default: return AcousticMaterial::defaultWall();
    }
}
}  // namespace

AF_WorldHandle AF_WorldCreate(void) { return new (std::nothrow) acoustic::flow::World(); }
void AF_WorldDestroy(AF_WorldHandle w) { delete asWorld(w); }

int AF_WorldAddMaterial(AF_WorldHandle w, const float* tr, const float* ab, const float* sc) {
    acoustic::flow::World* W = asWorld(w); if (!W) return -1;
    acoustic::AcousticMaterial m = acoustic::AcousticMaterial::defaultWall();
    for (int b = 0; b < acoustic::kNumBands; ++b) {
        if (tr) m.transmission[b] = tr[b];
        if (ab) m.absorption[b] = ab[b];
        if (sc) m.scattering[b] = sc[b];
    }
    return W->rules.materials.add(m);
}
int AF_WorldAddMaterialPreset(AF_WorldHandle w, int preset) {
    acoustic::flow::World* W = asWorld(w); if (!W) return -1;
    return W->rules.materials.add(presetMaterial(preset));
}
void AF_WorldSetBoxMaterial(AF_WorldHandle w, int box, int material) {
    if (acoustic::flow::World* W = asWorld(w)) W->setBoxMaterial(box, material);
}
int AF_WorldUpdateMaterial(AF_WorldHandle w, int material, const float* tr, const float* ab, const float* sc) {
    acoustic::flow::World* W = asWorld(w); if (!W) return 0;
    if (material < 0 || material >= W->rules.materials.count()) return 0;
    acoustic::AcousticMaterial m = W->rules.materials.get(material);
    for (int b = 0; b < acoustic::kNumBands; ++b) {
        if (tr) m.transmission[b] = tr[b];
        if (ab) m.absorption[b] = ab[b];
        if (sc) m.scattering[b] = sc[b];
    }
    return W->updateMaterial(material, m) ? 1 : 0;
}
int AF_WorldUpdateMaterialPreset(AF_WorldHandle w, int material, int preset) {
    acoustic::flow::World* W = asWorld(w); if (!W) return 0;
    return W->updateMaterial(material, presetMaterial(preset)) ? 1 : 0;
}
int AF_WorldMaterialCount(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->rules.materials.count() : 0;
}

void AF_WorldSetGpuTrace(AF_WorldHandle w, int on) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->gpuTrace = on;
}
int AF_WorldGpuActive(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return (W && W->gpuActive()) ? 1 : 0;
}
int AF_WorldGpuInfo(AF_WorldHandle w, char* buf, int bufBytes) {
    acoustic::flow::World* W = asWorld(w);
    if (!W || !buf || bufBytes <= 0) return 0;
    const std::string& s = W->gpuActive() ? W->gpuAdapter() : W->gpuError();
    const int n = static_cast<int>(s.size() < static_cast<std::size_t>(bufBytes - 1) ? s.size() : static_cast<std::size_t>(bufBytes - 1));
    for (int i = 0; i < n; ++i) buf[i] = s[static_cast<std::size_t>(i)];
    buf[n] = 0;
    return n;
}

void AF_WorldSetLeakModel(AF_WorldHandle w, int model) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->leakModel = model;
}
void AF_WorldSetLateThrough(AF_WorldHandle w, int on) {
    if (acoustic::flow::World* W = asWorld(w)) W->lateThrough = (on < 0) ? 0 : (on > 2 ? 2 : on);   // 0 旧 / 1 案1 / 2 戸口の線音源
}
void AF_WorldSetDoorPull(AF_WorldHandle w, float pull) {
    if (acoustic::flow::World* W = asWorld(w)) W->doorPull = (pull < 0.0f) ? 0.0f : (pull > 1.0f ? 1.0f : pull);
}
float AF_WorldDoorPull(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->doorPull : 0.0f;
}
void AF_WorldSetDoorCoherence(AF_WorldHandle w, float hz) {
    if (acoustic::flow::World* W = asWorld(w)) W->doorCoherenceHz = (hz < 0.0f) ? 0.0f : hz;
}
float AF_WorldDoorCoherence(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->doorCoherenceHz : 0.0f;
}
void AF_WorldSetPrecedence(AF_WorldHandle w, float db, float sec) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->precedenceDb = (db < 0.0f) ? 0.0f : db;
    W->precedenceSec = (sec < 0.001f) ? 0.001f : sec;
}
void AF_WorldSetImageSurface(AF_WorldHandle w, float roughDeg, float connectDeg, float connectMs, float densityPow, float nearPow) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    const float k = 3.14159265f / 180.0f;
    W->surface.roughRad = std::max(0.0f, roughDeg) * k;
    W->surface.connectRad = std::max(0.0f, connectDeg) * k;
    W->surface.connectSec = std::max(1e-4f, connectMs * 0.001f);
    W->surface.densityPow = std::max(0.0f, densityPow);
    W->surface.nearPow = std::max(0.0f, nearPow);
}
void AF_WorldSetLateDistanceShape(AF_WorldHandle w, float pow) {
    if (acoustic::flow::World* W = asWorld(w)) W->lateDistancePow = std::max(0.0f, pow);
}
float AF_WorldLateDistanceShape(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->lateDistancePow : 0.0f;
}
int AF_WorldLateLaneShape(AF_WorldHandle w, float* outShare, float* outDistance, float* outDir3, int maxLanes) {
    acoustic::flow::World* W = asWorld(w);
    if (!W || maxLanes <= 0) return 0;
    int n = 0;
    const float* share = W->lateLaneShape(n);
    const float* dist = W->lateLaneDistance();
    if (n > maxLanes) n = maxLanes;
    for (int k = 0; k < n; ++k) {
        if (outShare) outShare[k] = share[k];
        if (outDistance) outDistance[k] = dist[k];
        if (outDir3) {
            float d3[3] = {0.0f, 0.0f, 0.0f};
            W->lateLaneDirection(k, d3);                 // 器が無ければ 0 のまま（呼び手は長さで判る）
            outDir3[3 * k] = d3[0]; outDir3[3 * k + 1] = d3[1]; outDir3[3 * k + 2] = d3[2];
        }
    }
    return n;
}
void AF_WorldSetAdjacentContain(AF_WorldHandle w, float amount) {
    if (acoustic::flow::World* W = asWorld(w)) W->adjacentContain = (amount < 0.0f) ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}
float AF_WorldAdjacentContain(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->adjacentContain : 0.0f;
}
void AF_WorldSetEarlyModel(AF_WorldHandle w, int model) {
    if (acoustic::flow::World* W = asWorld(w)) W->earlyModel = std::min(4, std::max(0, model));
}
int AF_WorldEarlyModel(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->earlyModel : 4;
}
void AF_WorldSetEmitterEarlyModel(AF_WorldHandle w, int e, int model) {
    if (acoustic::flow::World* W = asWorld(w)) W->setEmitterEarlyModel(e, model);
}
void AF_WorldSetEmitterAdjacentContain(AF_WorldHandle w, int e, float amount) {
    if (acoustic::flow::World* W = asWorld(w)) W->setEmitterAdjacentContain(e, amount);
}
void AF_WorldSetWallReflect(AF_WorldHandle w, int on) {
    if (acoustic::flow::World* W = asWorld(w)) W->wallReflect = (on != 0) ? 1 : 0;
}
int AF_WorldWallReflect(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->wallReflect : 0;
}
int AF_WorldLateThrough(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->lateThrough : 0;
}
void AF_WorldSetLaneModel(AF_WorldHandle w, int model) {
    if (acoustic::flow::World* W = asWorld(w)) W->laneModel = (model <= 0) ? 0 : 1;   // 0 耳ごとの行 / 1 点と拡散を分ける
}
int AF_WorldLaneModel(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->laneModel : 1;
}
int AF_WorldLeakModel(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->leakModel : 0;
}

void AF_WorldSetRoomCell(AF_WorldHandle w, float meters) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->setRoomCell(meters);
}
void AF_WorldRoomCellInfo(AF_WorldHandle w, float* req, float* eff, double* vox, double* maxVox) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    if (req) *req = W->roomCellRequested();
    if (eff) *eff = W->roomCellEffective();
    if (vox) *vox = W->roomVoxels();
    if (maxVox) *maxVox = W->roomMaxVoxels();
}

void AF_WorldSetRoomSeedRadius(AF_WorldHandle w, float meters) {
    if (acoustic::flow::World* W = asWorld(w)) W->setRoomSeedRadius((meters < 0.0f) ? 0.0f : (meters > 10.0f ? 10.0f : meters));
}
float AF_WorldRoomSeedRadius(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->roomSeedRadius() : 0.0f;
}
void AF_WorldSetOutsideMouth(AF_WorldHandle w, int on) {
    if (acoustic::flow::World* W = asWorld(w)) W->setOutsideMouth(on);
}
int AF_WorldOutsideMouth(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->outsideMouth() : 0;
}

int AF_WorldAddBox(AF_WorldHandle w, AF_Vector3 c, AF_Vector3 he, AF_Vector3 ax, AF_Vector3 ay, int material, int dynamic) {
    acoustic::flow::World* W = asWorld(w); if (!W) return -1;
    return W->addBox(makeObb(c, he, ax, ay), material, dynamic != 0);
}
void AF_WorldSetBoxTransform(AF_WorldHandle w, int box, AF_Vector3 c, AF_Vector3 ax, AF_Vector3 ay) {
    acoustic::flow::World* W = asWorld(w); if (!W || box < 0 || box >= W->surfaces.count()) return;
    const Obb old = W->surfaces.at(box).obb;
    AF_Vector3 he{old.halfExtents.x, old.halfExtents.y, old.halfExtents.z};
    W->setBoxTransform(box, makeObb(c, he, ax, ay));
}
void AF_WorldSetBoxActive(AF_WorldHandle w, int box, int active) {
    acoustic::flow::World* W = asWorld(w); if (!W || box < 0 || box >= W->surfaces.count()) return;
    W->surfaces.at(box).active = (active != 0);
}

void AF_WorldBuild(AF_WorldHandle w) { if (acoustic::flow::World* W = asWorld(w)) W->build(); }
int  AF_WorldRoomCount(AF_WorldHandle w) { acoustic::flow::World* W = asWorld(w); return W ? W->roomCount() : 0; }
int  AF_WorldBuildCount(AF_WorldHandle w) { acoustic::flow::World* W = asWorld(w); return W ? W->buildCount() : 0; }
int  AF_WorldRoomAt(AF_WorldHandle w, AF_Vector3 p) { acoustic::flow::World* W = asWorld(w); return W ? W->roomAt(V(p)) : -1; }
int  AF_WorldRoomInfo(AF_WorldHandle w, int room, float* rt, float* vol, float* surf, float* mfp) {
    acoustic::flow::World* W = asWorld(w); if (!W || room < 0 || room >= W->roomCount()) return 0;
    const acoustic::flow::Probe& p = W->probe(room);
    if (rt) for (int b = 0; b < acoustic::kNumBands; ++b) rt[b] = p.rt60[b];
    if (vol) *vol = p.volume; if (surf) *surf = p.surface; if (mfp) *mfp = p.meanFreePath;
    return 1;
}

void AF_WorldSetListener(AF_WorldHandle w, AF_Vector3 p, AF_Vector3 f, AF_Vector3 u) { if (acoustic::flow::World* W = asWorld(w)) W->setListener(V(p), V(f), V(u)); }
int  AF_WorldAddEmitter(AF_WorldHandle w, AF_Vector3 p, float r) { acoustic::flow::World* W = asWorld(w); return W ? W->addEmitter(V(p), r) : -1; }
void AF_WorldRemoveEmitter(AF_WorldHandle w, int e) { if (acoustic::flow::World* W = asWorld(w)) W->removeEmitter(e); }
void AF_WorldSetEmitter(AF_WorldHandle w, int e, AF_Vector3 p, float r, int operated, float loudness) {
    if (acoustic::flow::World* W = asWorld(w)) W->setEmitter(e, V(p), r, operated != 0, loudness);
}

void AF_WorldSetRays(AF_WorldHandle w, int rays, int bounces) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    if (rays > 0) W->raysPerEmitter = rays;
    if (bounces > 0) W->maxBounces = bounces;
}
void AF_WorldSetBudget(AF_WorldHandle w, int totalRays, int fullSlots, int lightSlots, int probesPerFrame) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->budget.cfg.totalRays = (totalRays < 0) ? 0 : totalRays;
    if (fullSlots > 0) W->budget.cfg.fullSlots = fullSlots;
    if (lightSlots > 0) W->budget.cfg.lightSlots = lightSlots;
    if (probesPerFrame >= 0) W->budget.cfg.probesPerFrame = probesPerFrame;
}
void AF_WorldSetRayGroups(AF_WorldHandle w, int g) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->rayGroups = (g < 1) ? 1 : (g > acoustic::flow::TraceGroups::kMax ? acoustic::flow::TraceGroups::kMax : g);
}
void AF_WorldSetMaxRaysPerEmitter(AF_WorldHandle w, int maxRays) {
    acoustic::flow::World* W = asWorld(w); if (!W || maxRays <= 0) return;
    // ★上限 65536: 全音源を 1 回で流すので、出力は（音源 × 本数 × 160 B）。
    //   D3D11 のバッファの大きさは 32 ビットなので、ここで抑えないと音源が多いときに確保が黙って失敗する。
    const int lo = W->budget.cfg.minPerEmitter;
    W->budget.cfg.maxPerEmitter = (maxRays < lo) ? lo : (maxRays > 65536 ? 65536 : maxRays);
}
void AF_WorldSetWorkers(AF_WorldHandle w, int workers) { if (acoustic::flow::World* W = asWorld(w)) W->setWorkers(workers); }
int  AF_WorldSpentRays(AF_WorldHandle w) { acoustic::flow::World* W = asWorld(w); return W ? W->spentRays() : 0; }
int  AF_WorldEmitterTier(AF_WorldHandle w, int e) { acoustic::flow::World* W = asWorld(w); return W ? W->tierOf(e) : 2; }
int  AF_WorldEmitterRays(AF_WorldHandle w, int e) { acoustic::flow::World* W = asWorld(w); return W ? W->raysOf(e) : 0; }

void AF_WorldSetWeights(AF_WorldHandle w, const float* w5) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    for (int c = 0; c < acoustic::flow::kNumComponents; ++c) W->rules.weights.w[c] = w5 ? w5[c] : 1.0f;
}
void AF_WorldSetAdjacentLateWeight(AF_WorldHandle w, float weight) {
    if (acoustic::flow::World* W = asWorld(w)) W->rules.weights.lateAdjacent = (weight < 0.0f) ? 0.0f : (weight > 4.0f ? 4.0f : weight);
}
float AF_WorldAdjacentLateWeight(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->rules.weights.lateAdjacent : 1.0f;
}
void AF_WorldSetShadowMuffle(AF_WorldHandle w, float db) {
    if (acoustic::flow::World* W = asWorld(w)) W->rules.weights.shadowMuffleDb = (db > 0.0f) ? (db < 24.0f ? db : 24.0f) : 0.0f;   // NaN も 0
}
float AF_WorldShadowMuffle(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w); return W ? W->rules.weights.shadowMuffleDb : 0.0f;
}
void AF_WorldSetResponse(AF_WorldHandle w, float level, float colour, float stat, float dir) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->response.levelSec = level; W->response.colourSec = colour; W->response.statSec = stat; W->response.directionSec = dir;
}
void AF_WorldSetHeadCm(AF_WorldHandle w, float cm) { if (acoustic::flow::World* W = asWorld(w)) W->headCm = cm; }

void AF_WorldUpdate(AF_WorldHandle w, float dt) { if (acoustic::flow::World* W = asWorld(w)) W->update(dt); }

void AF_WorldBindFdn(AF_WorldHandle w, AF_FdnMixHandle fdn) {
    if (acoustic::flow::World* W = asWorld(w)) W->bindFdn(static_cast<af::dsp::FdnRoomMix*>(fdn));
}
int AF_WorldFdnStale(AF_WorldHandle w) { acoustic::flow::World* W = asWorld(w); return (W && W->fdnStale()) ? 1 : 0; }
void AF_WorldApplyVoice(AF_WorldHandle w, int e, AF_VoiceHandle voice, int sampleRate) {
    acoustic::flow::World* W = asWorld(w);
    af::dsp::VoiceRenderer* v = static_cast<af::dsp::VoiceRenderer*>(voice);
    if (W && v) W->applyToVoice(e, *v, sampleRate);
}

int AF_WorldMixInfo(AF_WorldHandle w, int e, AF_MixInfo* out) {
    acoustic::flow::World* W = asWorld(w); if (!W || !out) return 0;
    const acoustic::flow::Mix* m = W->mix(e);
    const acoustic::flow::TraceResult* t = W->trace(e);
    const acoustic::flow::Emitter* em = W->emitter(e);
    if (!m || !t || !em) return 0;
    for (int b = 0; b < 6; ++b) {
        out->energy6[b] = m->energy6[b];
        for (int c = 0; c < 5; ++c) out->component6[c][b] = m->component6[c][b];
    }
    out->onsetSec = m->onsetSec; out->directSec = t->directSec;
    out->tapCount = m->tapCount; out->sendCount = m->sendCount;
    out->room = em->room; out->directCrossings = t->directCrossings;
    out->firstReflectSec = t->firstReflectSec; out->raysTraced = t->raysTraced; out->hits = t->hits;
    const acoustic::flow::Visibility* vis = W->visibility(e);
    out->visibleFraction = vis ? vis->visible : 1.0f;
    out->shadowers = vis ? vis->shadowers : 0;
    const acoustic::flow::ImageSet* im = W->images(e);
    out->imageCount = im ? im->count : 0;
    out->imageCandidates = im ? im->candidates : 0;
    return 1;
}
int AF_WorldArrivals(AF_WorldHandle w, int e, AF_Arrival* out, int maxOut) {
    // 聞こえている音の到来（配分タブ用）。配分の出口（Mix のタップと送り）を到来ごとに並べ直すだけで、計算は足さない。
    //   出どころ（origin）は地図用: 直接・透過は音源、回折は稜線の点、初期（虚像）は耳の側の壁の反射点、戸口は戸口の点。
    acoustic::flow::World* W = asWorld(w); if (!W || !out || maxOut <= 0) return 0;
    const acoustic::flow::Mix* m = W->mix(e);
    const acoustic::flow::Emitter* em = W->emitter(e);
    if (!m || !em) return 0;
    int n = 0;
    auto mean6 = [](const float* v) { double s = 0.0; for (int b = 0; b < 6; ++b) s += v[b]; return static_cast<float>(s / 6.0); };
    const acoustic::Vec3 none(0.0f, 0.0f, 0.0f);
    auto push = [&](int kind, float dx, float dy, float dz, float spread, float energy, float delaySec,
                    bool hasOrigin, const acoustic::Vec3& origin, int box) {
        if (n >= maxOut || !(energy > 0.0f)) return;
        AF_Arrival& a = out[n++];
        a.kind = kind; a.emitter = e;
        a.dirLocal[0] = dx; a.dirLocal[1] = dy; a.dirLocal[2] = dz;
        a.spread = spread; a.energy = energy; a.delaySec = delaySec;
        a.origin[0] = origin.x; a.origin[1] = origin.y; a.origin[2] = origin.z;
        a.hasOrigin = hasOrigin ? 1 : 0; a.box = box;
    };
    const acoustic::Vec3 L = W->listener().pos;
    const acoustic::flow::ImageSet* im = W->images(e);
    const acoustic::flow::FaceTapSet* ftaps = W->faceTaps(e);
    const std::vector<acoustic::flow::Face>& faces = W->faces();
    const acoustic::flow::Diffraction* df = W->diffraction(e);
    int imageIdx = 0;
    for (int i = 0; i < m->tapCount; ++i) {
        const acoustic::flow::MixTap& t = m->taps[i];
        const float et = mean6(t.e6);
        if (t.kind == acoustic::flow::TapKind::Direct) {
            push(AF_ARRIVAL_DIRECT, t.dirLocal.x, t.dirLocal.y, t.dirLocal.z, t.spread, et, t.delaySec, true, em->pos, -1);
            continue;
        }
        if (t.kind == acoustic::flow::TapKind::Transmit) {
            push(AF_ARRIVAL_TRANSMIT, t.dirLocal.x, t.dirLocal.y, t.dirLocal.z, t.spread, et, t.delaySec, true, em->pos, -1);
            continue;
        }
        if (t.kind == acoustic::flow::TapKind::Diffract) {
            const bool ok = df && df->valid;
            push(AF_ARRIVAL_DIFFRACT, t.dirLocal.x, t.dirLocal.y, t.dirLocal.z, t.spread, et, t.delaySec, ok, ok ? df->point : none, -1);
            continue;
        }
        const bool fromFaces = ftaps && ftaps->count > 0 && W->emitterEarlyModel(e) >= 1 && W->emitterEarlyModel(e) <= 3;
        if (fromFaces) {
            // 受取面のタップ: 出どころは量で重みを付けた小片の中心、箱は面の持ち主
            const acoustic::flow::FaceTap* hit = nullptr;
            for (int q = 0; q < ftaps->count; ++q) if (ftaps->tap[q].id == t.id) { hit = &ftaps->tap[q]; break; }
            const bool dirOk = acoustic::length(t.dirLocal) > 0.5f;
            // 戸口の 1 本（隣の部屋の閉じ込め）の出どころは戸口の中心
            push(dirOk ? AF_ARRIVAL_EARLY : AF_ARRIVAL_EARLY_DIFFUSE, t.dirLocal.x, t.dirLocal.y, t.dirLocal.z, dirOk ? t.spread : 1.0f, et, t.delaySec,
                 hit != nullptr && t.id != acoustic::flow::FaceTapSet::kIdRest, hit ? hit->point : none, hit ? hit->box : -1);
            continue;
        }
        if (t.id == acoustic::flow::FaceTapSet::kIdDoor) {                  // 虚像の面音源の閉じ込めの戸口の 1 本
            push(AF_ARRIVAL_EARLY, t.dirLocal.x, t.dirLocal.y, t.dirLocal.z, t.width / 1.5707963f, et, t.delaySec, false, none, -1);
            continue;
        }
        if (t.id == 4) {                                                    // 方向なしの初期（distribute の素性 4）
            push(AF_ARRIVAL_EARLY_DIFFUSE, 0.0f, 0.0f, 0.0f, 1.0f, et, t.delaySec, false, none, -1);
            continue;
        }
        // 初期（虚像）: distribute は虚像の順にタップを積むので、方向なし以外の初期の k 本目が k 番の虚像。
        //   反射点 ＝ 耳から虚像への線が、耳の側で返った面（face[0]）の平面と交わる点。
        acoustic::Vec3 origin = none;
        bool has = false;
        int box = -1;
        if (im && imageIdx < im->count) {
            const acoustic::flow::ImageSource& src = im->img[imageIdx];
            const int f0 = src.face[0];
            if (f0 >= 0 && f0 < static_cast<int>(faces.size())) {
                const acoustic::flow::Face& fc = faces[static_cast<std::size_t>(f0)];
                const acoustic::Vec3 d = src.pos - L;
                const float denom = acoustic::dot(fc.normal, d);
                if (std::fabs(denom) > 1e-6f) {
                    const float tt = acoustic::dot(fc.normal, fc.center - L) / denom;
                    if (tt > 0.0f && tt < 1.0f) { origin = L + d * tt; has = true; }
                }
                box = fc.box;
            }
        }
        ++imageIdx;
        push(AF_ARRIVAL_EARLY, t.dirLocal.x, t.dirLocal.y, t.dirLocal.z, t.spread, et, t.delaySec, has, origin, box);
    }
    const int lroom = W->roomAt(L);
    const std::vector<acoustic::flow::World::PortalDiag>& diag = W->portalDiag();
    for (int k = 0; k < m->sendCount; ++k) {
        const acoustic::flow::FdnSend& sd = m->sends[k];
        const float eAll = mean6(sd.e6);
        if (sd.room == lroom) { push(AF_ARRIVAL_LATE_ROOM, 0.0f, 0.0f, 0.0f, 1.0f, eAll, m->onsetSec, false, none, -1); continue; }
        const acoustic::flow::World::PortalDiag* pd = nullptr;
        for (const acoustic::flow::World::PortalDiag& d : diag) if (d.room == sd.room) { pd = &d; break; }
        float eThru = mean6(sd.thru6);
        if (eThru > eAll) eThru = eAll;
        if (W->lateThrough == 2 && pd) {
            // 戸口の線音源: 戸口から直接の分を点の量（Σ² = 1）で配り、残りは耳の部屋へ流した響き
            for (int j = 0; j < af::dsp::FdnRoomMix::kPortalPoints; ++j) {
                const float az = pd->pointAz[j] * 3.14159265f / 180.0f;
                const acoustic::Vec3 pp(pd->pointPos[j * 3], pd->pointPos[j * 3 + 1], pd->pointPos[j * 3 + 2]);
                push(AF_ARRIVAL_LATE_DOOR, std::sin(az), 0.0f, std::cos(az), 0.0f, eThru * pd->pointGain[j] * pd->pointGain[j], m->onsetSec, true, pp, -1);
            }
            push(AF_ARRIVAL_LATE_DOOR_FEED, 0.0f, 0.0f, 0.0f, 1.0f, eAll - eThru, m->onsetSec, false, none, -1);
        } else {
            // 戸口の向きの点（案1）: 耳の部屋と音源の部屋を繋ぐ戸口の中心を出どころに
            acoustic::Vec3 origin = none;
            bool has = false;
            for (int a = 0; a < W->apertureCount(); ++a) {
                const acoustic::rooms::Aperture& ap = W->aperture(a);
                if ((ap.roomA == sd.room && ap.roomB == lroom) || (ap.roomB == sd.room && ap.roomA == lroom)) { origin = ap.rectCenter; has = true; break; }
            }
            const acoustic::Vec3 dl = W->listener().toLocal(acoustic::Vec3(sd.dir[0], sd.dir[1], sd.dir[2]));
            push(AF_ARRIVAL_LATE_POINT, dl.x, dl.y, dl.z, 1.0f - sd.focus, eAll, m->onsetSec, has, origin, -1);
        }
    }
    return n;
}

int AF_WorldBoxCount(AF_WorldHandle w) {
    acoustic::flow::World* W = asWorld(w);
    return W ? W->surfaces.count() : 0;
}
int AF_WorldBoxInfo(AF_WorldHandle w, int i, AF_BoxInfo* out) {
    acoustic::flow::World* W = asWorld(w);
    if (!W || !out || i < 0 || i >= W->surfaces.count()) return 0;
    const acoustic::flow::Surface& s = W->surfaces.at(i);
    auto put = [](float* dst, const acoustic::Vec3& v) { dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; };
    put(out->center, s.obb.center); put(out->halfExtents, s.obb.halfExtents);
    put(out->axisX, s.obb.axisX); put(out->axisY, s.obb.axisY); put(out->axisZ, s.obb.axisZ);
    out->dynamic = s.dynamic ? 1 : 0;
    out->active = s.active ? 1 : 0;
    return 1;
}
int AF_WorldApertureInfo(AF_WorldHandle w, int a, AF_ApertureInfo* out) {
    acoustic::flow::World* W = asWorld(w);
    if (!W || !out || a < 0 || a >= W->apertureCount()) return 0;
    const acoustic::rooms::Aperture& ap = W->aperture(a);
    auto put = [](float* dst, const acoustic::Vec3& v) { dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; };
    put(out->center, ap.rectCenter); put(out->axisU, ap.axisU); put(out->axisV, ap.axisV);
    out->halfU = ap.halfU; out->halfV = ap.halfV;
    out->openFrac = W->apertureOpenFrac(a);
    out->roomA = ap.roomA; out->roomB = ap.roomB;
    return 1;
}
int AF_WorldDiffractionInfo(AF_WorldHandle w, int e, AF_DiffractionInfo* out) {
    acoustic::flow::World* W = asWorld(w); if (!W || !out) return 0;
    const acoustic::flow::Diffraction* d = W->diffraction(e);
    if (!d) return 0;
    out->valid = d->valid ? 1 : 0;
    out->box = d->box; out->edge = d->edge;
    out->delta = d->delta; out->gapWidth = d->gapWidth; out->weight = d->weight; out->pathSec = d->pathSec;
    for (int b = 0; b < 6; ++b) { out->energy6[b] = d->energy6[b]; out->gapOpen6[b] = d->gapOpen6[b]; }
    out->point[0] = d->point.x; out->point[1] = d->point.y; out->point[2] = d->point.z;
    out->dirLocal[0] = d->dirLocal.x; out->dirLocal[1] = d->dirLocal.y; out->dirLocal[2] = d->dirLocal.z;
    return 1;
}
int   AF_WorldApertureCount(AF_WorldHandle w) { acoustic::flow::World* W = asWorld(w); return W ? W->apertureCount() : 0; }
float AF_WorldApertureOpenFrac(AF_WorldHandle w, int a) { acoustic::flow::World* W = asWorld(w); return W ? W->apertureOpenFrac(a) : 1.0f; }
