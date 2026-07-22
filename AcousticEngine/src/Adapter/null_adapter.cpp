/* Adapter/null_adapter.cpp
 * Wwise が無い環境向けのスタブ実装。
 * CMake が Wwise SDK を見つけられなかったときにビルドされる。
 *
 * これにより「Wwise 未導入の PC でも DLL 全体がビルドでき、
 * Core(計算)の開発やテストを進められる」状態を保てる。
 * 音声系 API は呼べるが、常に「利用不可」を返す。
 */
#include "Adapter/wwise_adapter.h"

namespace acoustic {
namespace adapter {

bool isWwiseAvailable() {
    return false;  // スタブ実装
}

bool initAudio() {
    return false;  // Wwise が無いので初期化できない
}

bool isAudioInitialized() {
    return false;
}

void shutdownAudio() {
    // 何もしない
}

// ===== 再生系（すべて no-op / 失敗を返す） =====
bool setBankPath(const char*) { return false; }
bool loadBank(const char*) { return false; }
void registerGameObject(unsigned long long, const char*) {}
void unregisterGameObject(unsigned long long) {}
void setGameObjectPosition(unsigned long long,
                           float, float, float,
                           float, float, float,
                           float, float, float) {}
void setDefaultListener(unsigned long long) {}
unsigned int postEvent(const char*, unsigned long long) { return 0; }
void executeActionOnEvent(const char*, int, unsigned long long) {}
void setObstructionOcclusion(unsigned long long, unsigned long long, float, float) {}
void setEmitterListenerVolume(unsigned long long, unsigned long long, float) {}
void setState(const char*, const char*) {}
void setRTPCValue(const char*, float) {}
void setRTPCValueOnObject(const char*, float, unsigned long long) {}
void setEarlyReflections(unsigned long long, const char*, const float*, const float*, int) {}
void getOutputLevels(float* l, float* r) { if (l) *l = 0.0f; if (r) *r = 0.0f; }
void renderAudio() {}

}  // namespace adapter
}  // namespace acoustic
