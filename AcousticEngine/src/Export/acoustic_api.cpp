/* acoustic_api.cpp
 * C API 窓口層の実装。
 * 役割は「翻訳」: C# から来た POD(AF_Vector3) と不透明ハンドルを、
 * Core 層(acoustic::AcousticWorld / Vec3)に変換して呼び出す。
 * 物理計算そのものは Core 層が持ち、ここには書かない。
 */
#include "acoustic_engine.h"

#include "Adapter/wwise_adapter.h"  // Wwise 非依存の薄いインターフェースのみ
#include "Core/acoustic_world.h"
#include "Core/material.h"          // kNumBands（帯域数）
#include "Core/source_directivity.h"

// 境界の AF_Vector3 と Core の Vec3 は「float ×3」で同一レイアウト。
// reinterpret_cast で詰め替え無しに書き込めることを保証する。
static_assert(sizeof(AF_Vector3) == sizeof(acoustic::Vec3),
              "AF_Vector3 と acoustic::Vec3 のサイズが一致しません");

namespace {

// バージョン定義。今は土台確認用の 0.1.0。
constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

// ハンドル(void*) <-> Core クラス の相互変換ヘルパ。
acoustic::AcousticWorld* asWorld(AcousticEngineHandle h) {
    return static_cast<acoustic::AcousticWorld*>(h);
}

// 境界の AF_Vector3 を Core 内部の Vec3 へ変換する。
acoustic::Vec3 toVec3(const AF_Vector3& v) {
    return acoustic::Vec3(v.x, v.y, v.z);
}

// 帯域別の transmission/absorption/scattering 配列から AcousticMaterial を作る。
// 各配列は独立に反映（null の配列だけ既定値のまま）。numBands<=0 は全て既定。
// 内部帯域数に足りないぶんは既定値のまま残す。AddBoxOriented / AddMesh で共用する。
acoustic::AcousticMaterial materialFromArrays(const float* transmission,
                                              const float* absorption,
                                              const float* scattering, int numBands) {
    acoustic::AcousticMaterial m = acoustic::AcousticMaterial::defaultWall();
    if (numBands > 0) {
        const int n = (numBands < acoustic::kNumBands) ? numBands : acoustic::kNumBands;
        for (int b = 0; b < n; ++b) {
            if (transmission != nullptr) m.transmission[b] = transmission[b];
            if (absorption != nullptr)   m.absorption[b]   = absorption[b];
            if (scattering != nullptr)   m.scattering[b]   = scattering[b];
        }
    }
    return m;
}

}  // namespace

int AcousticEngine_GetVersion(void) {
    return kVersionMajor * 10000 + kVersionMinor * 100 + kVersionPatch;
}

AcousticEngineHandle AcousticEngine_Create(void) {
    // new した実体のポインタをそのままハンドルとして返す。
    return static_cast<AcousticEngineHandle>(new acoustic::AcousticWorld());
}

void AcousticEngine_Destroy(AcousticEngineHandle engine) {
    // NULL を delete しても安全だが、明示的に対称性を保つ。
    delete asWorld(engine);
}

void AcousticEngine_AddBox(AcousticEngineHandle engine,
                           AF_Vector3 center,
                           AF_Vector3 halfExtents) {
    if (!engine) return;
    asWorld(engine)->addBox(toVec3(center), toVec3(halfExtents));
}

void AcousticEngine_AddBoxOriented(AcousticEngineHandle engine,
                                   AF_Vector3 center,
                                   AF_Vector3 halfExtents,
                                   AF_Vector3 right,
                                   AF_Vector3 up,
                                   const float* transmission,
                                   const float* absorption,
                                   const float* scattering,
                                   int numBands) {
    if (!engine) return;
    asWorld(engine)->addBoxOriented(
        toVec3(center), toVec3(halfExtents), toVec3(right), toVec3(up),
        materialFromArrays(transmission, absorption, scattering, numBands));
}

int AcousticEngine_AddMesh(AcousticEngineHandle engine,
                           const float* vertices,
                           int vertexCount,
                           const int* indices,
                           int indexCount,
                           const float* transmission,
                           const float* absorption,
                           const float* scattering,
                           int numBands) {
    if (!engine) return -1;
    return asWorld(engine)->addMesh(vertices, vertexCount, indices, indexCount,
                                    materialFromArrays(transmission, absorption, scattering, numBands));
}

void AcousticEngine_SetMeshActive(AcousticEngineHandle engine, int meshId, int active) {
    if (!engine) return;
    asWorld(engine)->setMeshActive(meshId, active != 0);
}

void AcousticEngine_ClearGeometry(AcousticEngineHandle engine) {
    if (!engine) return;
    asWorld(engine)->clearGeometry();
}

void AcousticEngine_ClearMeshes(AcousticEngineHandle engine) {
    if (!engine) return;
    asWorld(engine)->clearMeshes();
}

int AcousticEngine_IsOccluded(AcousticEngineHandle engine,
                              AF_Vector3 from,
                              AF_Vector3 to) {
    if (!engine) return 0;
    return asWorld(engine)->isOccluded(toVec3(from), toVec3(to)) ? 1 : 0;
}

float AcousticEngine_ComputeOcclusion(AcousticEngineHandle engine,
                                      AF_Vector3 from,
                                      AF_Vector3 to) {
    if (!engine) return 0.0f;
    return asWorld(engine)->occlusionScalar(toVec3(from), toVec3(to));
}

float AcousticEngine_ComputeOcclusionRayIntegrated(AcousticEngineHandle engine,
                                                   AF_Vector3 source,
                                                   AF_Vector3 listener,
                                                   int numRays,
                                                   int maxBounces) {
    if (!engine) return 0.0f;
    return asWorld(engine)->occlusionScalarRayIntegrated(
        toVec3(source), toVec3(listener), numRays, maxBounces);
}

void AcousticEngine_ComputeOcclusionMultiSource(AcousticEngineHandle engine,
                                                AF_Vector3 listener,
                                                const AF_Vector3* sources,
                                                int count,
                                                float* outOcc,
                                                int numRays,
                                                int maxBounces) {
    if (!engine || !sources || !outOcc || count <= 0) return;
    // AF_Vector3 と Vec3 は同一レイアウト（先頭の static_assert で保証）なので、
    // 音源配列をそのまま Vec3* として渡す（コピー不要）。
    asWorld(engine)->occlusionScalarMultiSource(
        toVec3(listener),
        reinterpret_cast<const acoustic::Vec3*>(sources), count,
        outOcc, numRays, maxBounces);
}

int AcousticEngine_ComputeTransmissionBands(AcousticEngineHandle engine,
                                            AF_Vector3 from,
                                            AF_Vector3 to,
                                            float* outGains,
                                            int count) {
    if (!engine || !outGains || count <= 0) return 0;

    // Core は常に kNumBands 個に書き込む前提。呼び出し側の容量が小さくても
    // 安全なよう、いったんローカルに受けてから count 個ぶんだけコピーする。
    float gains[acoustic::kNumBands];
    asWorld(engine)->computeTransmission(toVec3(from), toVec3(to), gains);

    const int n = (count < acoustic::kNumBands) ? count : acoustic::kNumBands;
    for (int b = 0; b < n; ++b) outGains[b] = gains[b];
    return n;
}

int AcousticEngine_ComputeDiffractionBands(AcousticEngineHandle engine,
                                           AF_Vector3 from,
                                           AF_Vector3 to,
                                           float* outGains,
                                           int count) {
    if (!engine || !outGains || count <= 0) return 0;
    float gains[acoustic::kNumBands];
    asWorld(engine)->computeDiffraction(toVec3(from), toVec3(to), gains);
    const int n = (count < acoustic::kNumBands) ? count : acoustic::kNumBands;
    for (int b = 0; b < n; ++b) outGains[b] = gains[b];
    return n;
}

float AcousticEngine_DebugRaycast(AcousticEngineHandle engine,
                                  AF_Vector3 origin,
                                  AF_Vector3 dir,
                                  float maxDist) {
    if (!engine) return -1.0f;
    const acoustic::RayHit hit =
        asWorld(engine)->raycastClosest(toVec3(origin), toVec3(dir), maxDist);
    return hit.hit ? hit.distance : -1.0f;
}

int AcousticEngine_TraceReflectionPath(AcousticEngineHandle engine,
                                       AF_Vector3 origin,
                                       AF_Vector3 dir,
                                       float maxDist,
                                       int maxBounces,
                                       AF_Vector3* outPoints,
                                       int maxPoints) {
    if (!engine || !outPoints || maxPoints <= 0) return 0;

    // AF_Vector3 と Vec3 は同一レイアウトなので、出力バッファをそのまま
    // Vec3* として渡す（中間バッファの確保もコピーも不要 = 多数レイでも軽い）。
    return asWorld(engine)->traceReflectionPath(
        toVec3(origin), toVec3(dir), maxDist, maxBounces,
        reinterpret_cast<acoustic::Vec3*>(outPoints), maxPoints);
}

int AcousticEngine_ComputeEchogram(AcousticEngineHandle engine,
                                   AF_Vector3 listener,
                                   const AF_Vector3* sources,
                                   int count,
                                   int numRays,
                                   int maxBounces,
                                   float* outBins,
                                   int numBins,
                                   float binSeconds,
                                   float speedOfSound) {
    if (!engine || !sources || !outBins || count <= 0 || numBins <= 0) return 0;
    asWorld(engine)->computeEchogram(
        toVec3(listener), reinterpret_cast<const acoustic::Vec3*>(sources), count,
        numRays, maxBounces, outBins, numBins, binSeconds, speedOfSound);
    return numBins;
}

int AcousticEngine_ComputeFaceReachability(AcousticEngineHandle engine,
                                           AF_Vector3 faceCenter,
                                           AF_Vector3 faceNormal,
                                           AF_Vector3 faceRight,
                                           float halfWidth,
                                           float halfHeight,
                                           int shape,
                                           AF_Vector3 listener,
                                           float* outGrid,
                                           int cols,
                                           int rows) {
    if (!engine || !outGrid || cols <= 0 || rows <= 0) return 0;
    return asWorld(engine)->computeFaceReachability(
        toVec3(faceCenter), toVec3(faceNormal), toVec3(faceRight),
        halfWidth, halfHeight, shape, toVec3(listener), outGrid, cols, rows);
}

float AcousticEngine_ComputeDirectivity(AF_Vector3 sourcePos,
                                        AF_Vector3 sourceForward,
                                        AF_Vector3 listenerPos,
                                        int directivityType,
                                        float* outLowpass) {
    // int -> enum。範囲外は無指向(Omni)に倒す（安全側）。
    acoustic::DirectivityType type = acoustic::DirectivityType::Omni;
    if (directivityType >= 0 && directivityType <= 4) {
        type = static_cast<acoustic::DirectivityType>(directivityType);
    }
    // 物理計算は Core に委譲。outLowpass はそのまま渡せる（null は Core 側で無視）。
    return acoustic::computeDirectivity(
        toVec3(sourcePos), toVec3(sourceForward), toVec3(listenerPos),
        type, outLowpass);
}

/* ----- 音声バックエンド(Wwise) ----- */

int AcousticEngine_IsWwiseAvailable(void) {
    return acoustic::adapter::isWwiseAvailable() ? 1 : 0;
}

int AcousticEngine_InitAudio(void) {
    return acoustic::adapter::initAudio() ? 1 : 0;
}

int AcousticEngine_IsAudioInitialized(void) {
    return acoustic::adapter::isAudioInitialized() ? 1 : 0;
}

void AcousticEngine_ShutdownAudio(void) {
    acoustic::adapter::shutdownAudio();
}

/* ----- 再生系（Adapter へ委譲。AF_Vector3 は float に展開して渡す）----- */

int AcousticEngine_SetBankPath(const char* utf8Path) {
    return acoustic::adapter::setBankPath(utf8Path) ? 1 : 0;
}

int AcousticEngine_LoadBank(const char* bankName) {
    return acoustic::adapter::loadBank(bankName) ? 1 : 0;
}

void AcousticEngine_RegisterGameObject(unsigned long long id, const char* name) {
    acoustic::adapter::registerGameObject(id, name);
}

void AcousticEngine_UnregisterGameObject(unsigned long long id) {
    acoustic::adapter::unregisterGameObject(id);
}

void AcousticEngine_SetGameObjectPosition(unsigned long long id,
                                          AF_Vector3 position,
                                          AF_Vector3 front,
                                          AF_Vector3 top) {
    acoustic::adapter::setGameObjectPosition(id,
        position.x, position.y, position.z,
        front.x, front.y, front.z,
        top.x, top.y, top.z);
}

void AcousticEngine_SetDefaultListener(unsigned long long id) {
    acoustic::adapter::setDefaultListener(id);
}

unsigned int AcousticEngine_PostEvent(const char* eventName, unsigned long long gameObjectId) {
    return acoustic::adapter::postEvent(eventName, gameObjectId);
}

void AcousticEngine_ExecuteActionOnEvent(const char* eventName, int actionType,
                                         unsigned long long gameObjectId) {
    acoustic::adapter::executeActionOnEvent(eventName, actionType, gameObjectId);
}

void AcousticEngine_SetObstructionOcclusion(unsigned long long emitterId,
                                            unsigned long long listenerId,
                                            float obstruction, float occlusion) {
    acoustic::adapter::setObstructionOcclusion(emitterId, listenerId, obstruction, occlusion);
}

void AcousticEngine_SetEmitterListenerVolume(unsigned long long emitterId,
                                             unsigned long long listenerId,
                                             float volume) {
    acoustic::adapter::setEmitterListenerVolume(emitterId, listenerId, volume);
}

void AcousticEngine_SetState(const char* stateGroup, const char* state) {
    acoustic::adapter::setState(stateGroup, state);
}

void AcousticEngine_GetOutputLevels(float* outLeft, float* outRight) {
    acoustic::adapter::getOutputLevels(outLeft, outRight);
}

void AcousticEngine_SetRTPCValue(const char* name, float value) {
    acoustic::adapter::setRTPCValue(name, value);
}

void AcousticEngine_SetRTPCValueOnObject(const char* name, float value,
                                         unsigned long long gameObjectId) {
    acoustic::adapter::setRTPCValueOnObject(name, value, gameObjectId);
}

void AcousticEngine_RenderAudio(void) {
    acoustic::adapter::renderAudio();
}
