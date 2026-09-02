/* afcap_scan.cpp ── 録れた .afcap をコマンド 1 本で走査する。
 *
 *   AfCapScan <path.afcap> [--emit <出力.cpp.txt> [印の番号]]
 *
 * ★型紙は Unity のパネルが呼ぶのと**同じ関数**（AcousticEngine/src/Debug/detectors.h）。
 *   走査の答えを 2 つ持たせないため（「道具は見つけたのに検査は通る」を避ける）。
 *
 * ★これは「実機で録れたものを、開発機で見る」ための口。
 *   Unity を立ち上げずに中身が見えるので、キャプチャを送ってもらって調べられる。
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../AcousticEngine/src/Debug/capture_emit.h"
#include "../AcousticEngine/src/Debug/capture_scan.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("使い方: AfCapScan <path.afcap> [--emit <出力> [印の番号]]\n");
        return 2;
    }
    const char* path = argv[1];
    acoustic::dbg::CaptureFile cap;
    std::string err;
    if (!acoustic::dbg::readCapture(path, cap, &err)) {
        std::printf("開けません: %s (%s)\n", path, err.c_str());
        return 1;
    }

    std::printf("=== %s ===\n", path);
    std::printf("場面 %s / dllHash %08x / 並列 %d\n",
                cap.scene.c_str(), cap.dllHash, cap.workerThreads);
    std::printf("%d フレーム（前 %.1fs 後 %.1fs / 印は frame %u）\n",
                cap.frames, cap.preroll / 60.0f, cap.postroll / 60.0f, cap.markedFrame);
    std::printf("箱 %d / 材質 %d / メッシュ %d / PCM %.2f 秒\n",
                (int)cap.boxes.size(), (int)cap.materials.size(), cap.meshCount,
                cap.sampleRate > 0 ? (float)(cap.pcm.size() / 2) / cap.sampleRate : 0.0f);
    if (cap.workerThreads > 1)
        std::printf("⚠ 並列で録れています。診断の列は workerThreads=1 でしか信用できません。\n");
    if (cap.meshCount > 0)
        std::printf("⚠ メッシュを含む場面です。C++ の検査には吐けません（docs §5.3）。\n");

    // 音源の一覧。
    std::vector<unsigned long long> ids;
    for (const auto& row : cap.sources)
        for (const auto& s : row) {
            bool seen = false;
            for (auto id : ids) if (id == s.id) { seen = true; break; }
            if (!seen) ids.push_back(s.id);
        }
    // 音源ごとの様子。★印の数だけ見ても「なぜ出たか」が分からないので、
    //   段・遮蔽・帯域の傾きの範囲を先に出す。
    std::printf("音源 %d 本\n", (int)ids.size());
    std::printf("  %-8s %-18s %-14s %-16s %s\n",
                "id", "段(厳/簡/仮)", "遮蔽 min..max", "傾き(低-高)dB", "経路 min..max");
    for (auto id : ids) {
        int tierN[3] = {0, 0, 0};
        float occLo = 1e9f, occHi = -1e9f, tiltLo = 1e9f, tiltHi = -1e9f;
        int pathLo = 1 << 30, pathHi = 0, rows = 0;
        for (const auto& row : cap.sources)
            for (const auto& s : row) {
                if (s.id != id) continue;
                ++rows;
                if (s.tier < 3) ++tierN[s.tier];
                occLo = s.occ < occLo ? s.occ : occLo;
                occHi = s.occ > occHi ? s.occ : occHi;
                const float lo = s.band[0], hi = s.band[acoustic::dbg::kCapBands - 1];
                if (lo > 1e-6f || hi > 1e-6f) {
                    const float t = af::detect::db(lo) - af::detect::db(hi);
                    tiltLo = t < tiltLo ? t : tiltLo;
                    tiltHi = t > tiltHi ? t : tiltHi;
                }
                const int paths = (int)s.earlyTaps + (int)s.diffSrcs;
                pathLo = paths < pathLo ? paths : pathLo;
                pathHi = paths > pathHi ? paths : pathHi;
            }
        char tiers[32];
        std::snprintf(tiers, sizeof(tiers), "%d/%d/%d", tierN[0], tierN[1], tierN[2]);
        std::printf("  %-8llu %-18s %5.2f..%-8.2f %6.2f..%-9.2f %d..%d  (%d 行)\n",
                    (unsigned long long)id, tiers, occLo, occHi, tiltLo, tiltHi,
                    pathLo, pathHi, rows);
    }
    std::printf("\n");

    const std::vector<af::scan::Mark> marks = af::scan::scan(cap);
    std::printf("印 %d 件\n", (int)marks.size());

    // 型紙ごとの内訳。★どれが多いかが分かると、次に見る所が決まる。
    struct Tally { const char* name; int n; };
    std::vector<Tally> tally;
    for (const auto& m : marks) {
        bool found = false;
        for (auto& t : tally)
            if (std::strcmp(t.name, m.templateName) == 0) { ++t.n; found = true; break; }
        if (!found) tally.push_back(Tally{m.templateName, 1});
    }
    for (const auto& t : tally) std::printf("  %-24s %d 件\n", t.name, t.n);
    std::printf("\n");

    // ★連続した印はひとまとめにする。閉扉の幻は**何秒も続く**ので、生のまま並べると読めない。
    //   まとめた行には**続いた長さ**を出すこと（そこが重要。1 フレームの瞬きとは意味が違う）。
    const std::vector<af::scan::MarkRun> runs = af::scan::groupRuns(marks);
    std::printf("まとめると %d か所\n", (int)runs.size());
    const int show = (int)runs.size() < 30 ? (int)runs.size() : 30;
    for (int i = 0; i < show; ++i) {
        const auto& r = runs[(size_t)i];
        char span[64];
        if (r.count == 1) std::snprintf(span, sizeof(span), "f%d", r.first.frame);
        else std::snprintf(span, sizeof(span), "f%d..%d (%d フレーム %.2fs 続く)",
                           r.first.frame, r.lastFrame, r.count,
                           r.lastSeconds - r.first.seconds);
        std::printf("  [%2d] %-34s 音源 %-5llu %-22s %s 最大 %.3f\n",
                    i, span, (unsigned long long)r.first.sourceId,
                    r.first.templateName, r.first.what, r.worst);
    }
    if ((int)runs.size() > show) std::printf("  … 他 %d か所\n", (int)runs.size() - show);

    // ── 生の行を覗く（--rows <音源id> <開始フレーム> <本数>）──
    //   ★印の数だけ見ていると「検出器が甘いのか本物が続いているのか」が分からない。
    //     実際 816 件の 型紙4 が出たとき、これを見て初めて中身が分かった。
    for (int i = 2; i + 3 < argc; ++i) {
        if (std::strcmp(argv[i], "--rows") != 0) continue;
        const unsigned long long want = std::strtoull(argv[i + 1], nullptr, 10);
        const int from = std::atoi(argv[i + 2]);
        const int n = std::atoi(argv[i + 3]);
        std::printf("\n音源 %llu の生の行\n", want);
        std::printf("  %-7s %-6s %-9s %-9s %-6s %s\n",
                    "frame", "段", "平均dB", "傾きdB", "遮蔽", "125Hz..4kHz");
        int shown = 0;
        for (int k = 0; k < cap.frames && shown < n; ++k) {
            if ((int)cap.global[(size_t)k].frame < from) continue;
            for (const auto& s : cap.sources[(size_t)k]) {
                if (s.id != want) continue;
                float sum = 0.0f;
                for (int b = 0; b < acoustic::dbg::kCapBands; ++b) sum += s.band[b];
                const float mean = sum / acoustic::dbg::kCapBands;
                const float tilt = af::detect::db(s.band[0])
                                 - af::detect::db(s.band[acoustic::dbg::kCapBands - 1]);
                std::printf("  %-7u %-6d %-9.2f %-9.2f %-6.2f",
                            cap.global[(size_t)k].frame, (int)s.tier,
                            af::detect::db(mean), tilt, s.occ);
                for (int b = 0; b < acoustic::dbg::kCapBands; ++b)
                    std::printf(" %.4f", s.band[b]);
                std::printf("\n");
                ++shown;
            }
        }
        break;
    }

    // ── 印から回帰テストを吐く ──
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--emit") != 0 || i + 1 >= argc) continue;
        const char* out = argv[i + 1];
        const int idx = (i + 2 < argc) ? std::atoi(argv[i + 2]) : 0;
        if (idx < 0 || idx >= (int)marks.size()) { std::printf("印 %d 番はありません\n", idx); break; }
        const std::string src = af::emit::emitCase(cap, marks[(size_t)idx],
                                                   "testGenerated_FromCapture");
        if (std::FILE* f = std::fopen(out, "wb")) {
            std::fwrite(src.data(), 1, src.size(), f);
            std::fclose(f);
            std::printf("\n印 %d から検査を吐きました → %s (%zu バイト)\n", idx, out, src.size());
        } else {
            std::printf("\n書けません: %s\n", out);
        }
        break;
    }
    return 0;
}
