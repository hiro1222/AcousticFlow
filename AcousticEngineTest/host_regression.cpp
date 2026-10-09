// host_regression.cpp ── 外の器（Wwise のプラグイン）との台帳（acoustic_host.h）の検査（2026-09-29）
//
// ■ 何を確かめるか
//   1) 書いた物がそのまま写しで読める（鍵・Voice・位置・共有の器・標本化周波数）
//   2) 聞き手から見た座標が正しい（正面を向いた聞き手、横を向いた聞き手）
//   3) 書いている最中に読んでも、壊れた写し（前半と後半が別のフレーム）を渡さない（seqlock）
//   4) 引退させた Voice と HRTF が 1 秒後に壊れる（落ちない）
//   5) 報告が表示へ届き、報告が来ている間は「鳴っている」
// ■ なぜ DLL にリンクするか
//   台帳は DLL の中のグローバル。Wwise のプラグインも Unity と同じ DLL を名前で引いて同じ台帳を読む。検査も同じ形で読む。
#include "acoustic_host.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <cstring>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
int g_fail = 0, g_count = 0;
void check(const char* name, bool ok, const char* detail = "") {
    ++g_count;
    if (!ok) ++g_fail;
    std::printf("  [%s] %s %s\n", ok ? "OK" : "FAIL", name, detail);
}
AF_VoiceHandle fakeVoice(std::uintptr_t n) { return reinterpret_cast<AF_VoiceHandle>(n); }
}  // namespace

int main() {
    std::printf("=== 台帳（外の器との受け渡し）の検査 ===\n");
    char buf[256];
    AF_HostVoice snap[AF_HOST_MAX_VOICES];
    AF_HostShared sh{};

    // 1) 2) 正面を向いた聞き手
    {
        AF_HostBegin();
        AF_HostSetListener(0, 0, -3, 0, 0, 1, 0, 1, 0);
        AF_HostPutVoice(7, fakeVoice(0x10), 1, 0, -3);    // 聞き手の右 1 m
        AF_HostPutVoice(8, fakeVoice(0x20), 0, 0, 0);     // 聞き手の前 3 m
        AF_HostSetShared(reinterpret_cast<AF_FdnMixHandle>(0x30), reinterpret_cast<AF_DirectionBusHandle>(0x40), 48000);
        AF_HostCommit();
        const int n = AF_HostSnapshot(snap, AF_HOST_MAX_VOICES, &sh);
        std::snprintf(buf, sizeof buf, "(本数 %d、鍵 %d/%d、共有 %p/%p、%d Hz)", n, n > 0 ? snap[0].key : -1, n > 1 ? snap[1].key : -1, sh.fdn, sh.bus, sh.sampleRate);
        check("書いた物がそのまま写しで読める", n == 2 && snap[0].key == 7 && snap[1].key == 8 && snap[0].voice == fakeVoice(0x10)
              && sh.fdn == reinterpret_cast<AF_FdnMixHandle>(0x30) && sh.bus == reinterpret_cast<AF_DirectionBusHandle>(0x40) && sh.sampleRate == 48000, buf);
        std::snprintf(buf, sizeof buf, "(右の音源 %.2f,%.2f,%.2f ／ 前の音源 %.2f,%.2f,%.2f)", snap[0].lx, snap[0].ly, snap[0].lz, snap[1].lx, snap[1].ly, snap[1].lz);
        check("正面を向いた聞き手: 右の音源は +x、前の音源は +z", std::fabs(snap[0].lx - 1) < 1e-5f && std::fabs(snap[0].lz) < 1e-5f
              && std::fabs(snap[1].lz - 3) < 1e-5f && std::fabs(snap[1].lx) < 1e-5f, buf);
    }
    // 2) +x を向いた聞き手（右は −z）
    {
        AF_HostBegin();
        AF_HostSetListener(0, 0, 0, 1, 0, 0, 0, 1, 0);
        AF_HostPutVoice(1, fakeVoice(0x10), 0, 0, -1);    // 世界の −z ＝ この聞き手の右
        AF_HostPutVoice(2, fakeVoice(0x20), 2, 0, 0);     // 世界の +x ＝ この聞き手の前 2 m
        AF_HostCommit();
        const int n = AF_HostSnapshot(snap, AF_HOST_MAX_VOICES, &sh);
        std::snprintf(buf, sizeof buf, "(右 %.2f,%.2f,%.2f ／ 前 %.2f,%.2f,%.2f)", snap[0].lx, snap[0].ly, snap[0].lz, snap[1].lx, snap[1].ly, snap[1].lz);
        check("横を向いた聞き手: 右と前が聞き手の向きで決まる（左手系）", n == 2 && std::fabs(snap[0].lx - 1) < 1e-5f && std::fabs(snap[1].lz - 2) < 1e-5f, buf);
    }
    // 3) 書いている最中に読んでも壊れた写しを渡さない。書き手はフレームごとに全員の鍵を同じ番号にする。
    {
        // ★前の検査が残した表（鍵 1 と 2。中身は正しい）を「破れ」と数えないよう、書き手を動かす前に空にする。
        AF_HostBegin(); AF_HostCommit();
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            for (int f = 1; !stop.load(); ++f) {
                AF_HostBegin();
                for (int k = 0; k < 64; ++k) AF_HostPutVoice(f, fakeVoice(static_cast<std::uintptr_t>(k + 1)), static_cast<float>(f), 0, 0);
                AF_HostCommit();
            }
        });
        int reads = 0, busy = 0, torn = 0, clobbered = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
            // 取り損ねたときに呼び手の配列（前の写し）を壊していないか: 目印を置いてから呼ぶ
            snap[0].key = -12345;
            const int n = AF_HostSnapshot(snap, AF_HOST_MAX_VOICES, &sh);
            if (n < 0) { ++busy; if (snap[0].key != -12345) ++clobbered; continue; }
            ++reads;
            for (int k = 1; k < n; ++k) if (snap[k].key != snap[0].key || snap[k].x != snap[0].x) {
                if (torn < 3) std::printf("      破れの例: 本数 %d、[0] 鍵 %d x %.0f ／ [%d] 鍵 %d x %.0f\n", n, snap[0].key, snap[0].x, k, snap[k].key, snap[k].x);
                ++torn; break; }
        }
        stop = true;
        writer.join();
        std::snprintf(buf, sizeof buf, "(読めた %d 回・書いている最中で見送り %d 回・壊れた写し %d 回・見送りで前の写しを壊した %d 回)", reads, busy, torn, clobbered);
        check("書いている最中に読んでも、壊れた写しを渡さない（見送りのときは前の写しを触らない）", reads > 1000 && torn == 0 && clobbered == 0, buf);
    }
    // 4) 引退させた Voice と HRTF が 1 秒後に壊れる（本物の器で。落ちなければ合格）
    {
        AF_VoiceConfig cfg{};
        cfg.sampleRate = 48000; cfg.maxFrames = 1024; cfg.tailSeconds = 1.0f;
        cfg.tapCrossfadeMs = 30.0f; cfg.hrtfCrossfadeMs = 12.0f; cfg.hrtfCrossoverHz = 700.0f; cfg.tailFirstBlock = 64; cfg.tailCapBlock = 8192;
        AF_VoiceHandle v = AF_VoiceCreate(&cfg);
        AF_HrtfHandle h = AF_HrtfCreateSynthetic(48000);
        AF_VoiceSetHrtf(v, h);
        AF_HostRetireVoice(v);
        AF_HostRetireHrtf(h);
        AF_HostTick();                                           // まだ壊さない
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        AF_HostTick();                                           // ここで Voice → HRTF の順に壊れる
        check("引退させた Voice と HRTF を 1 秒後に壊しても落ちない", v != nullptr && h != nullptr);
    }
    // 5) 報告が表示へ届く
    {
        AF_HostReport(3, 1, 1, 0.012f);
        int m = 0, mi = 0, sp = -1, bl = 0; float e = 0;
        AF_HostStats(&m, &mi, &sp, &e, &bl);
        std::snprintf(buf, sizeof buf, "(当たり %d・外れ %d・座標 %d・誤差 %.3f m・ブロック %d・鳴っている %d)", m, mi, sp, e, bl, AF_HostActive());
        check("報告が表示へ届き、報告が来ている間は「鳴っている」", m == 3 && mi == 1 && sp == 1 && std::fabs(e - 0.012f) < 1e-6f && bl >= 1 && AF_HostActive() == 1, buf);
    }

    // 6) 位置の単位（既定 1、Unreal なら 100）
    {
        const float d = AF_HostUnitsPerMeter();
        AF_HostSetUnitsPerMeter(100.0f);
        const float u = AF_HostUnitsPerMeter();
        AF_HostSetUnitsPerMeter(0.0f);                  // 0 や負は 1 に戻す
        const float z = AF_HostUnitsPerMeter();
        std::snprintf(buf, sizeof buf, "(既定 %.0f・100 を入れて %.0f・0 を入れて %.0f)", d, u, z);
        check("位置の単位（1 m が何単位か）を渡せる。既定は 1、0 は 1 に戻す", d == 1.0f && u == 100.0f && z == 1.0f, buf);
    }

    // 7) 音の大きさの計器（プラグインが返し、ゲームが表示する）
    {
        AF_HostReportLevels(2, 0.25f, 0.5f, 0.125f);
        int inst = 0; float in = 0, g = 0, out = 0;
        AF_HostLevels(&inst, &in, &g, &out);
        AF_HostLevels(nullptr, nullptr, nullptr, nullptr);   // 要らない所は null でよい
        std::snprintf(buf, sizeof buf, "(実体 %d・受けた %.3f・音量 %.3f・出した %.3f)", inst, in, g, out);
        check("音の大きさの計器が表示へ届く（null の受け取りで落ちない）", inst == 2 && in == 0.25f && g == 0.5f && out == 0.125f, buf);
    }

    // 8) 対応付けの累計（最後のブロックだけでは、途中で外れたブロックが見えない）
    {
        long long m0 = 0, x0 = 0; float e0 = 0;
        AF_HostTotals(&m0, &x0, &e0);                    // ここまでの分を読み、最大を 0 に戻す
        AF_HostReport(1, 0, 1, 0.02f);
        AF_HostReport(0, 1, 1, 0.30f);
        AF_HostReport(1, 0, 1, 0.01f);
        long long m1 = 0, x1 = 0; float e1 = 0, e2 = 0;
        AF_HostTotals(&m1, &x1, &e1);
        AF_HostTotals(nullptr, nullptr, &e2);            // 読むと最大は 0 に戻る
        std::snprintf(buf, sizeof buf, "(当たり +%lld・外れ +%lld・最大の誤差 %.2f → 読み直し %.2f)", m1 - m0, x1 - x0, e1, e2);
        check("途中で外れたブロックも累計に残り、最大の誤差は読むたびに 0 に戻る", m1 - m0 == 2 && x1 - x0 == 1 && e1 == 0.30f && e2 == 0.0f, buf);
    }

    // 9) 左右の計器（AF_HostMeterWrite）: 共有メモリを名前で開くと、書いた値がそのまま読める（別の窓 tools/af_meter.py と同じ読み方）
    {
        AF_HostBegin(); AF_HostCommit();                 // 置き場はゲームのスレッドの公開で作られる
        AF_HostMeterBlock b{};
        b.frames = 512; b.sampleRate = 48000;
        for (int st = 0; st < AF_METER_STAGES; ++st) { b.rmsL[st] = 0.1f * (st + 1); b.rmsR[st] = 0.05f * (st + 1); b.peakL[st] = 0.5f; b.peakR[st] = 0.25f; }
        bool ok = AF_HostMeterReady() == 1;
        long long before = -1, after = -1; float rl2 = 0, rr3 = 0; int fr = 0;
#ifdef _WIN32
        HANDLE h = ::OpenFileMappingA(FILE_MAP_READ, FALSE, AF_METER_SHM_NAME);
        const unsigned char* base = h ? static_cast<const unsigned char*>(::MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        if (base) {
            std::memcpy(&before, base + 24, 8);
            AF_HostMeterWrite(&b);
            std::memcpy(&after, base + 24, 8);
            const unsigned char* e = base + 32 + 80 * static_cast<std::size_t>(before % AF_METER_CAPACITY);
            std::memcpy(&fr, e + 8, 4);
            std::memcpy(&rl2, e + 16 + 4 * 2, 4);          // rmsL[2]
            std::memcpy(&rr3, e + 16 + 16 + 4 * 3, 4);     // rmsR[3]
            ok = ok && std::memcmp(base, "AFMETER1", 8) == 0 && after == before + 1 && fr == 512 && rl2 == 0.3f && rr3 == 0.2f;
            ::UnmapViewOfFile(base);
        } else ok = false;
        if (h) ::CloseHandle(h);
#endif
        std::snprintf(buf, sizeof buf, "(置き場 %d・書いた数 %lld → %lld・frames %d・段 2 の L %.2f・段 3 の R %.2f)", AF_HostMeterReady(), before, after, fr, rl2, rr3);
        check("左右の計器: 共有メモリを名前で開くと、書いた値がそのまま読める", ok, buf);
    }

    if (g_fail == 0) std::printf("[OK] 台帳: %d 件のチェックすべてに合格しました。\n", g_count);
    else std::printf("[FAIL] 台帳: %d / %d 件のチェックに失敗しました。\n", g_fail, g_count);
    return g_fail == 0 ? 0 : 1;
}
