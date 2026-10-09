// dsp_bench.cpp ── 畳み込み側（AF_Voice / AF_TailBus）の費用だけを測る。
//   幾何（AF_Scene）は一切呼ばない。「IR が畳み込みまで持っているから CPU が厳しい」を
//   数字で確かめるための道具（2026-09-03）。実時間に対する割合で出す。
//   ⚠ 機械の負荷が高いときは数字が膨らむ。30% 未満のときに読むこと。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "acoustic_scene.h"   // AF_Vector3
#include "acoustic_voice.h"

namespace {

constexpr int   kRate = 48000;
constexpr int   kBlock = 512;      // Unity の DSP バッファ（Best Latency 相当）に近い
constexpr float kSeconds = 10.0f;

AF_HrtfHandle loadHrtf() {
    const char* paths[] = {"Projects/UnityDemo/Assets/StreamingAssets/kemar.afhr",
                           "../Projects/UnityDemo/Assets/StreamingAssets/kemar.afhr",
                           "../../Projects/UnityDemo/Assets/StreamingAssets/kemar.afhr"};
    for (const char* p : paths) { AF_HrtfHandle h = AF_HrtfLoadFile(p); if (h) return h; }
    return AF_HrtfCreateSynthetic(kRate);
}

// 直接 1 本 ＋ 反射 nR 本（軽量両耳化＝ITD＋ILD） ＋ 回折 nF 本（HRTF 丸ごと）。
std::vector<AF_VoiceTap> makeTaps(int nR, int nF) {
    std::vector<AF_VoiceTap> t;
    auto add = [&](int delay, float g, float pl, float pr, float hw, float dx, float dz) {
        AF_VoiceTap tp{};
        tp.delaySamples = delay;
        for (int b = 0; b < 6; ++b) tp.gain6[b] = g;
        tp.panL = pl; tp.panR = pr; tp.gSpec = 1.0f; tp.gDiff = 0.0f;
        tp.hrtfWeight = hw; tp.dirX = dx; tp.dirY = 0.0f; tp.dirZ = dz;
        t.push_back(tp);
    };
    add(0, 1.0f, 0.707f, 0.707f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < nR; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(std::max(1, nR));
        add(200 + i * 40, 0.3f, std::fabs(std::cos(a * 0.5f)), std::fabs(std::sin(a * 0.5f)),
            0.0f, std::sin(a), std::cos(a));
    }
    for (int i = 0; i < nF; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(std::max(1, nF));
        add(150 + i * 60, 0.2f, 0.707f, 0.707f, 1.0f, std::sin(a), std::cos(a));
    }
    return t;
}

// 指数減衰のエコグラム（RT60 秒）。尾の IR の長さは cfg.tailSeconds で決まる。
std::vector<float> echogram(int bins, float rt60) {
    std::vector<float> e(static_cast<size_t>(bins) * 6);
    for (int k = 0; k < bins; ++k) {
        const float tsec = 0.01f * static_cast<float>(k);
        const float v = std::pow(10.0f, -6.0f * tsec / rt60) * 0.05f;
        for (int b = 0; b < 6; ++b) e[static_cast<size_t>(k) * 6 + b] = v;
    }
    return e;
}

struct Bench { double voiceMs = 0.0; double busMs = 0.0; };

// voices 本を bus（無しも可）に預けて kSeconds ぶん回し、ボイス側とバス側の時間を分けて測る。
Bench run(int voices, bool ownTail, bool useBus, AF_HrtfHandle hrtf, int nR, int nF, int dirLanes = 0) {
    AF_VoiceConfig cfg{};
    cfg.sampleRate = kRate; cfg.maxFrames = kBlock; cfg.tailSeconds = 1.0f;
    AF_TailBusHandle bus = useBus ? AF_TailBusCreate(kRate, 1.0f, 64, 8192, kBlock) : nullptr;
    const int firstBlock = std::getenv("AF_DIRBUS_FIRST") ? std::atoi(std::getenv("AF_DIRBUS_FIRST")) : 128;
    AF_DirectionBusHandle dbus = (dirLanes > 0) ? AF_DirectionBusCreateEx(kRate, dirLanes, kBlock, firstBlock, 1024) : nullptr;
    if (dbus) AF_DirectionBusSetHrtf(dbus, hrtf, 57.0f);
    const std::vector<AF_VoiceTap> taps = makeTaps(nR, nF);
    const std::vector<float> eg = echogram(100, 1.0f);
    std::vector<AF_VoiceHandle> vs;
    for (int i = 0; i < voices; ++i) {
        AF_VoiceHandle v = AF_VoiceCreate(&cfg);
        AF_VoiceSetHrtf(v, hrtf);
        AF_VoiceSetHrtfEnabled(v, 1);
        AF_VoiceSetDirection(v, AF_Vector3{0.3f, 0.0f, 0.95f}, 57.0f);
        if (dbus) AF_VoiceSetDirectionBus(v, dbus);   // 次の SetTaps から効くので、先に付ける
        AF_VoiceSetTaps(v, taps.data(), static_cast<int>(taps.size()));
        if (useBus) AF_VoiceSetTailBus(v, bus, (i == 0) ? 1 : 0);
        if (ownTail || (useBus && i == 0))
            AF_VoiceRebuildTail(v, eg.data(), 100, 10.0f, 20.0f, 10.0f, 20.0f, 1.5f, 0.5f,
                                1.0f, 1.0f, nullptr, 0);
        AF_VoiceSetTailLevel(v, (ownTail || useBus) ? 1.0f : 0.0f);
        vs.push_back(v);
    }
    std::vector<float> dry(kBlock), l(kBlock), r(kBlock), ml(kBlock), mr(kBlock);
    unsigned rs = 12345u;
    const int blocks = static_cast<int>(kSeconds * kRate / kBlock);
    Bench b;
    using clk = std::chrono::high_resolution_clock;
    for (int k = 0; k < blocks; ++k) {
        for (int i = 0; i < kBlock; ++i) {
            rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
            dry[static_cast<size_t>(i)] = static_cast<int>(rs) * (0.3f / 2147483648.0f);
        }
        const auto t0 = clk::now();
        for (AF_VoiceHandle v : vs) AF_VoiceRender(v, dry.data(), kBlock, l.data(), r.data(), nullptr);
        const auto t1 = clk::now();
        if (useBus) {
            std::fill(ml.begin(), ml.end(), 0.0f);
            std::fill(mr.begin(), mr.end(), 0.0f);
            AF_TailBusRender(bus, kBlock, ml.data(), mr.data());
        }
        if (dbus) AF_DirectionBusRender(dbus, kBlock, ml.data(), mr.data());
        const auto t2 = clk::now();
        b.voiceMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        b.busMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
    }
    if (!vs.empty()) std::printf("      （尾のパーティション %d）\n", AF_VoiceTailPartitions(vs[0]));
    for (AF_VoiceHandle v : vs) AF_VoiceDestroy(v);
    if (bus) AF_TailBusDestroy(bus);
    if (dbus) AF_DirectionBusDestroy(dbus);
    return b;
}

}  // namespace

int main() {
    AF_HrtfHandle hrtf = loadHrtf();
    char nm[64] = {0};
    AF_HrtfGetName(hrtf, nm, 64);
    std::printf("=== 畳み込み側の費用（幾何なし。%d Hz、ブロック %d、%.0f 秒ぶん）HRTF: %s ===\n",
                kRate, kBlock, kSeconds, nm);
    const double realMs = kSeconds * 1000.0;
    auto show = [&](const char* label, const Bench& b, int voices) {
        std::printf("  %-40s ボイス %7.1f ms (%5.1f%% 実時間, 1 本 %5.2f%%)   バス %6.1f ms (%5.2f%%)\n",
                    label, b.voiceMs, 100.0 * b.voiceMs / realMs,
                    100.0 * b.voiceMs / realMs / voices, b.busMs, 100.0 * b.busMs / realMs);
    };
    show("1本: 直接+R48+F7, 尾なし",              run(1, false, false, hrtf, 48, 7), 1);
    show("1本: 直接+R4+F3, 尾なし（旧の本数）",     run(1, false, false, hrtf, 4, 3), 1);
    show("1本: 直接+R48+F7, 自前の尾 1.0 s",       run(1, true,  false, hrtf, 48, 7), 1);
    show("1本: 直接+R48+F7, 共有バスの尾 1.0 s",    run(1, false, true,  hrtf, 48, 7), 1);
    show("6本: 直接+R48+F7, 共有バス 1 本",         run(6, false, true,  hrtf, 48, 7), 6);
    show("1本: 直接+R48+F7, 方向バス 8 本",         run(1, false, true,  hrtf, 48, 7, 8), 1);
    show("6本: 直接+R48+F7, 共有バス + 方向バス 8",  run(6, false, true,  hrtf, 48, 7, 8), 6);
    show("6本: 直接+R48+F7, 共有バス + 方向バス 12", run(6, false, true,  hrtf, 48, 7, 12), 6);
    AF_HrtfDestroy(hrtf);
    return 0;
}
