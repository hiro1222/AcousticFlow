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

void AF_WorldSetLeakModel(AF_WorldHandle w, int model) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    W->leakModel = model;
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
void AF_WorldSetWorkers(AF_WorldHandle w, int workers) { if (acoustic::flow::World* W = asWorld(w)) W->setWorkers(workers); }
int  AF_WorldSpentRays(AF_WorldHandle w) { acoustic::flow::World* W = asWorld(w); return W ? W->spentRays() : 0; }
int  AF_WorldEmitterTier(AF_WorldHandle w, int e) { acoustic::flow::World* W = asWorld(w); return W ? W->tierOf(e) : 2; }
int  AF_WorldEmitterRays(AF_WorldHandle w, int e) { acoustic::flow::World* W = asWorld(w); return W ? W->raysOf(e) : 0; }

void AF_WorldSetWeights(AF_WorldHandle w, const float* w5) {
    acoustic::flow::World* W = asWorld(w); if (!W) return;
    for (int c = 0; c < acoustic::flow::kNumComponents; ++c) W->rules.weights.w[c] = w5 ? w5[c] : 1.0f;
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
