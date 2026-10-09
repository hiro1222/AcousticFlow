// host_api.cpp ── 外の器（Wwise のプラグインなど）と自前の DSP を繋ぐ台帳の実装（acoustic_host.h の注記）。
//
// ■ 中の仕組み（順に）
//   1) ゲームのスレッドは作業用の表（g_work）に書く。AF_HostCommit で公開用の表（g_pub）へ写す。
//      写すときは seqlock: 番号を奇数にしてから中身を書き、偶数に戻す。
//   2) 音のスレッドの AF_HostSnapshot は、番号が偶数で、写す前後で同じだったときだけ採る。
//      違ったら（書いている最中だったら）-1 を返す。呼び手は前の写しを使い続ける ── 待たない。
//   3) 引退した Voice は時刻と一緒に並べ、1 秒経ったら AF_HostTick が壊す。
//      ★1 秒の根拠: 音のスレッドが写しを握っているのは 1 ブロック（1024 / 48 kHz ≒ 21 ms）。桁で余裕を取る
//        （録音の器で落ちた件と同じ考え方。あちらは 2 秒）。
//
// ■ 退けた書き方
//   ・mutex で守る: 音のスレッドがゲームのスレッドの書き込みを待つと、そのブロックの音が遅れて途切れる。
//   ・Voice の寿命を参照の数で数える: 音のスレッドで数を減らした物を誰が壊すかが決まらない（音のスレッドでは壊せない）。
#include "acoustic_host.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct Table {
    AF_HostVoice  v[AF_HOST_MAX_VOICES];
    int           n = 0;
    AF_HostShared shared{nullptr, nullptr, 0};
};

Table g_work;                          // ゲームのスレッドだけが触る
Table g_pub;                           // seqlock で公開
std::atomic<unsigned> g_seq{0};

// 聞き手（ゲームのスレッドだけ）
float g_lp[3] = {0, 0, 0}, g_lf[3] = {0, 0, 1}, g_lu[3] = {0, 1, 0}, g_lr[3] = {1, 0, 0};

// 引退した Voice（ゲームのスレッドだけ）
struct Retired { AF_VoiceHandle v; std::chrono::steady_clock::time_point t; };
std::vector<Retired> g_retired;
struct RetiredHrtf { AF_HrtfHandle h; std::chrono::steady_clock::time_point t; };
std::vector<RetiredHrtf> g_retiredHrtf;

// 音のスレッドからの報告（表示用）
std::atomic<int>   g_statMatched{0}, g_statMissed{0}, g_statSpace{-1}, g_statBlocks{0};
std::atomic<float> g_statErr{0.0f};
std::atomic<long long> g_totMatched{0}, g_totMissed{0};   // 対応付けの累計（AF_HostTotals）
std::atomic<float> g_maxErr{0.0f};
std::atomic<int>   g_lvInstances{0};
std::atomic<float> g_lvIn{0.0f}, g_lvGain{0.0f}, g_lvOut{0.0f};   // 音の大きさの計器（AF_HostReportLevels）
std::atomic<long long> g_lastReportMs{0};
std::atomic<float> g_unitsPerMeter{1.0f};    // 1 m が何単位か（Unity 1、Unreal 100）

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ── 左右の計器（AF_HostMeterWrite）──
#pragma pack(push, 1)
struct MeterEntry {
    double  timeSec;
    int32_t frames, sampleRate;
    float   rmsL[AF_METER_STAGES], rmsR[AF_METER_STAGES], peakL[AF_METER_STAGES], peakR[AF_METER_STAGES];
};
struct MeterShared {
    char     magic[8];
    uint32_t version, capacity, stages, reserved;
    int64_t  writeCount;
    MeterEntry e[AF_METER_CAPACITY];
};
#pragma pack(pop)
static_assert(sizeof(MeterEntry) == 80, "ビューア（tools/af_meter.py）と同じ並び");
std::atomic<MeterShared*> g_meter{nullptr};

// 共有メモリを作る（ゲームのスレッド、最初の 1 回だけ）。先にビューアが作っていれば、それを開く（同じ名前・同じ大きさ）。
void ensureMeter() {
#ifdef _WIN32
    static bool tried = false;
    if (tried) return;
    tried = true;
    HANDLE h = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(MeterShared)), AF_METER_SHM_NAME);
    if (!h) return;
    void* p = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MeterShared));
    if (!p) { ::CloseHandle(h); return; }
    MeterShared* m = static_cast<MeterShared*>(p);
    std::memcpy(m->magic, "AFMETER1", 8);
    m->version = 1; m->capacity = AF_METER_CAPACITY; m->stages = AF_METER_STAGES; m->reserved = 0;
    g_meter.store(m, std::memory_order_release);   // 片付けない（プロセスの終わりに任せる。ビューアが握っていても困らない）
#endif
}

void normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

}  // namespace

extern "C" {

void AF_HostBegin(void) { g_work.n = 0; }

void AF_HostPutVoice(int key, AF_VoiceHandle voice, float x, float y, float z) {
    if (!voice || g_work.n >= AF_HOST_MAX_VOICES) return;
    AF_HostVoice& s = g_work.v[g_work.n++];
    s.key = key; s.voice = voice; s.x = x; s.y = y; s.z = z;
    // 聞き手から見た座標（Unity も Wwise も左手系: +x 右 / +y 上 / +z 前）
    const float d[3] = {x - g_lp[0], y - g_lp[1], z - g_lp[2]};
    s.lx = d[0] * g_lr[0] + d[1] * g_lr[1] + d[2] * g_lr[2];
    s.ly = d[0] * g_lu[0] + d[1] * g_lu[1] + d[2] * g_lu[2];
    s.lz = d[0] * g_lf[0] + d[1] * g_lf[1] + d[2] * g_lf[2];
}

void AF_HostSetListener(float px, float py, float pz, float fx, float fy, float fz, float ux, float uy, float uz) {
    g_lp[0] = px; g_lp[1] = py; g_lp[2] = pz;
    g_lf[0] = fx; g_lf[1] = fy; g_lf[2] = fz; normalize3(g_lf);
    g_lu[0] = ux; g_lu[1] = uy; g_lu[2] = uz; normalize3(g_lu);
    // 右 = 上 × 前（左手系。Unity の transform.right と同じ向き）
    g_lr[0] = g_lu[1] * g_lf[2] - g_lu[2] * g_lf[1];
    g_lr[1] = g_lu[2] * g_lf[0] - g_lu[0] * g_lf[2];
    g_lr[2] = g_lu[0] * g_lf[1] - g_lu[1] * g_lf[0];
    normalize3(g_lr);
}

void AF_HostSetShared(AF_FdnMixHandle fdn, AF_DirectionBusHandle bus, int sampleRate) {
    g_work.shared.fdn = fdn; g_work.shared.bus = bus; g_work.shared.sampleRate = sampleRate;
}

void AF_HostSetUnitsPerMeter(float unitsPerMeter) {
    g_unitsPerMeter.store(unitsPerMeter > 1e-6f ? unitsPerMeter : 1.0f, std::memory_order_relaxed);
}

float AF_HostUnitsPerMeter(void) { return g_unitsPerMeter.load(std::memory_order_relaxed); }

void AF_HostCommit(void) {
    const unsigned s = g_seq.load(std::memory_order_relaxed);
    g_seq.store(s + 1, std::memory_order_relaxed);             // 奇数 ＝ 書いている最中
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(g_pub.v, g_work.v, sizeof(AF_HostVoice) * static_cast<std::size_t>(g_work.n));
    g_pub.n = g_work.n;
    g_pub.shared = g_work.shared;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.store(s + 2, std::memory_order_release);             // 偶数 ＝ 読んでよい
    ensureMeter();                                             // 左右の計器の置き場（最初の 1 回だけ）
}

void AF_HostMeterWrite(const AF_HostMeterBlock* b) {
    MeterShared* m = g_meter.load(std::memory_order_acquire);
    if (!m || !b) return;
    const int64_t n = m->writeCount;                           // 書き手は 1 人（音のスレッドの係）
    MeterEntry& e = m->e[static_cast<std::size_t>(n % AF_METER_CAPACITY)];
    e.timeSec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    e.frames = b->frames; e.sampleRate = b->sampleRate;
    for (int s = 0; s < AF_METER_STAGES; ++s) { e.rmsL[s] = b->rmsL[s]; e.rmsR[s] = b->rmsR[s]; e.peakL[s] = b->peakL[s]; e.peakR[s] = b->peakR[s]; }
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<int64_t>*>(&m->writeCount)->store(n + 1, std::memory_order_release);   // 最後に進めて公開
}

int AF_HostMeterReady(void) { return g_meter.load(std::memory_order_acquire) ? 1 : 0; }

void AF_HostRetireVoice(AF_VoiceHandle voice) {
    if (voice) g_retired.push_back({voice, std::chrono::steady_clock::now()});
}

void AF_HostRetireHrtf(AF_HrtfHandle hrtf) {
    if (hrtf) g_retiredHrtf.push_back({hrtf, std::chrono::steady_clock::now()});
}

void AF_HostTick(void) {
    const auto now = std::chrono::steady_clock::now();
    // ★Voice を先に壊す（HRTF は Voice が握っている）。
    for (std::size_t i = 0; i < g_retired.size();) {
        if (now - g_retired[i].t >= std::chrono::seconds(1)) {
            AF_VoiceDestroy(g_retired[i].v);
            g_retired[i] = g_retired.back();
            g_retired.pop_back();
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < g_retiredHrtf.size();) {
        if (now - g_retiredHrtf[i].t >= std::chrono::milliseconds(1100)) {   // Voice より少し後
            AF_HrtfDestroy(g_retiredHrtf[i].h);
            g_retiredHrtf[i] = g_retiredHrtf.back();
            g_retiredHrtf.pop_back();
        } else {
            ++i;
        }
    }
}

int AF_HostActive(void) {
    const long long last = g_lastReportMs.load(std::memory_order_relaxed);
    return (last > 0 && nowMs() - last < 500) ? 1 : 0;
}

int AF_HostSnapshot(AF_HostVoice* out, int maxOut, AF_HostShared* outShared) {
    if (!out || maxOut <= 0) return -1;
    // ★一旦この読み手専用の置き場へ写し、確かめてから out へ渡す。out へ直に写すと、取り損ねた（書いている最中の）
    //   ブロックで呼び手の「前の写し」を壊してしまい、「取れなければ前の写しを使う」が成り立たない（2026-09-29 に発見）。
    thread_local AF_HostVoice tmp[AF_HOST_MAX_VOICES];
    const unsigned s0 = g_seq.load(std::memory_order_acquire);
    if (s0 & 1u) return -1;                                    // 書いている最中。前の写しを使ってもらう
    std::atomic_thread_fence(std::memory_order_acquire);
    int n = g_pub.n;
    if (n > maxOut) n = maxOut;
    if (n > AF_HOST_MAX_VOICES) n = AF_HOST_MAX_VOICES;
    if (n < 0) n = 0;
    std::memcpy(tmp, g_pub.v, sizeof(AF_HostVoice) * static_cast<std::size_t>(n));
    AF_HostShared sh = g_pub.shared;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_seq.load(std::memory_order_acquire) != s0) return -1; // 途中で書き換わった（out は触っていない）
    std::memcpy(out, tmp, sizeof(AF_HostVoice) * static_cast<std::size_t>(n));
    if (outShared) *outShared = sh;
    return n;
}

void AF_HostReport(int matched, int missed, int space, float meanErr) {
    g_statMatched.store(matched, std::memory_order_relaxed);
    g_statMissed.store(missed, std::memory_order_relaxed);
    g_statSpace.store(space, std::memory_order_relaxed);
    g_statErr.store(meanErr, std::memory_order_relaxed);
    g_statBlocks.fetch_add(1, std::memory_order_relaxed);
    g_lastReportMs.store(nowMs(), std::memory_order_relaxed);
    g_totMatched.fetch_add(matched, std::memory_order_relaxed);
    g_totMissed.fetch_add(missed, std::memory_order_relaxed);
    // 最大は書き手が 1 つ（音のスレッド）なので、読み手が 0 に戻すのと競っても 1 ブロックぶん取りこぼすだけ
    if (meanErr > g_maxErr.load(std::memory_order_relaxed)) g_maxErr.store(meanErr, std::memory_order_relaxed);
}

void AF_HostTotals(long long* outMatched, long long* outMissed, float* outMaxErr) {
    if (outMatched) *outMatched = g_totMatched.load(std::memory_order_relaxed);
    if (outMissed) *outMissed = g_totMissed.load(std::memory_order_relaxed);
    const float e = g_maxErr.exchange(0.0f, std::memory_order_relaxed);
    if (outMaxErr) *outMaxErr = e;
}

void AF_HostReportLevels(int instances, float inRms, float gain, float outRms) {
    g_lvInstances.store(instances, std::memory_order_relaxed);
    g_lvIn.store(inRms, std::memory_order_relaxed);
    g_lvGain.store(gain, std::memory_order_relaxed);
    g_lvOut.store(outRms, std::memory_order_relaxed);
}

void AF_HostLevels(int* outInstances, float* outInRms, float* outGain, float* outOutRms) {
    if (outInstances) *outInstances = g_lvInstances.load(std::memory_order_relaxed);
    if (outInRms) *outInRms = g_lvIn.load(std::memory_order_relaxed);
    if (outGain) *outGain = g_lvGain.load(std::memory_order_relaxed);
    if (outOutRms) *outOutRms = g_lvOut.load(std::memory_order_relaxed);
}

void AF_HostStats(int* outMatched, int* outMissed, int* outSpace, float* outMeanErr, int* outBlocks) {
    if (outMatched) *outMatched = g_statMatched.load(std::memory_order_relaxed);
    if (outMissed) *outMissed = g_statMissed.load(std::memory_order_relaxed);
    if (outSpace) *outSpace = g_statSpace.load(std::memory_order_relaxed);
    if (outMeanErr) *outMeanErr = g_statErr.load(std::memory_order_relaxed);
    if (outBlocks) *outBlocks = g_statBlocks.load(std::memory_order_relaxed);
}

}  // extern "C"
