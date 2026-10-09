// console_main.cpp — コンソール版のホスト（AfHostCheck）。
//
// ★GUI 版（AfHost）とは別の実行ファイルにしてある。
//   理由は 1 つ: **キーボードも画面も無い所で検査を回したいから。**
//   GUI 版に --selftest を持たせると、コンソールを付け外しする細工が要る上に、
//   落ちたときに画面が出ないと結果が読めない。
//
//   音響エンジンにはリンクしていない（GUI 版と同じ）。

#include "audio_device.h"
#include "block_source.h"
#include "host_util.h"
#include "module.h"
#include "selftest.h"

#include <windows.h>

#include <conio.h>

#include <cstdio>
#include <string>

namespace {

void printHelp() {
    std::printf(
        "\n"
        "  p : 試験信号を鳴らす／止める        l : DLL を読み込む\n"
        "  i : 状態を出す                      u : DLL を解放する\n"
        "  c : 実測値をリセット                r : 掴み直す（ビルドし直した DLL 用）\n"
        "  t : 自己検査を回す                  q : 終了\n"
        "  h : この一覧\n"
        "\n");
}

int printReport(const af::SelfTestReport& r) {
    std::printf("\n  ── 自己検査 ────────────────────────────\n");
    for (const auto& c : r.checks)
        std::printf("  %s %s%s%s\n", c.ok ? "✔" : "✘", c.what.c_str(),
                    c.detail.empty() ? "" : " ── ", c.detail.c_str());
    std::printf("  ────────────────────────────────────────\n");
    std::printf("  %s（落ちた項目 %d）\n\n",
                r.allPassed() ? "全部通りました" : "★落ちています", r.failed);
    return r.allPassed() ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 日本語をそのまま出す。ソースは UTF-8（/utf-8 でビルドしている）。
    ::SetConsoleOutputCP(CP_UTF8);

    std::printf("AfHostCheck — ミドルウェアのホスト（コンソール版・システム未搭載）\n");
    std::printf("────────────────────────────────────────────────\n");

    bool selftest = false;
    std::wstring explicitPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--selftest") selftest = true;
        else if (explicitPath.empty()) explicitPath = a;
    }

    const std::wstring target =
        explicitPath.empty() ? af::findDefaultModule() : af::absolutePath(explicitPath);

    if (target.empty())
        std::printf("・読み込む対象 : 見つかりません（起動時の引数で DLL のパスを渡せます）\n");
    else
        std::printf("・読み込む対象 : %s\n", af::wideToUtf8(target).c_str());

    if (selftest) return printReport(af::runSelfTest(target));

    // ★音源を作る前に形式を見る（開いてから作り直すと実時間スレッドと競合する）。
    int probeRate = 48000, probeCh = 2;
    std::string perr;
    if (!af::AudioDevice::probeDefaultFormat(&probeRate, &probeCh, &perr))
        std::printf("・形式を見られませんでした（%s）。%d Hz として続けます\n",
                    perr.c_str(), probeRate);

    af::AudioDevice device;
    af::ToneSource  tone(static_cast<double>(probeRate));

    std::string err;
    if (!device.start(&tone, &err)) {
        std::printf("✘ 出力デバイスを開けませんでした ── %s\n", err.c_str());
        std::printf("  （デバイスが無い環境でも、DLL の読み込みだけは試せます）\n");
    } else {
        std::printf("✔ 出力 : %s\n", device.deviceName().c_str());
        std::printf("  形式 : %d Hz / %d ch / 1 ブロック %d サンプル（%.1f ms）\n",
                    device.sampleRate(), device.channels(), device.bufferFrames(),
                    1000.0 * device.bufferFrames() / device.sampleRate());
    }

    af::Module mod;
    printHelp();

    bool toneOn = false;
    bool quit   = false;

    while (!quit) {
        if (!::_kbhit()) { ::Sleep(30); continue; }

        switch (::_getch()) {
        case 'q': case 'Q': quit = true; break;
        case 'h': case 'H': printHelp(); break;

        case 'p': case 'P':
            if (!device.running()) { std::printf("・デバイスが開いていません\n"); break; }
            toneOn = !toneOn;
            tone.setActive(toneOn);
            std::printf("・試験信号 : %s\n", toneOn ? "鳴らす" : "止める");
            break;

        case 'l': case 'L': {
            if (target.empty()) { std::printf("✘ 読み込む対象が決まっていません\n"); break; }
            std::string e;
            if (!mod.load(target, &e)) { std::printf("✘ 読み込めません ── %s\n", e.c_str()); break; }
            std::printf("✔ 読み込みました\n  影武者 : %s\n", af::wideToUtf8(mod.shadowPath()).c_str());
            std::printf("  %s 原本は掴まれていない\n",
                        af::Module::originalIsWritable(mod.originalPath()) ? "✔" : "✘");
            break;
        }

        case 'u': case 'U':
            if (!mod.loaded()) { std::printf("・まだ読み込んでいません\n"); break; }
            mod.unload();
            std::printf("✔ 解放しました\n");
            break;

        case 'r': case 'R': {
            std::string e;
            std::printf(mod.reload(&e) ? "✔ 掴み直しました\n" : "✘ 掴み直せません\n");
            break;
        }

        case 'c': case 'C':
            device.resetStats();
            std::printf("・実測値をリセットしました\n");
            break;

        case 't': case 'T':
            // ★検査は自前でデバイスを開くので、先にこちらを閉じる。
            device.stop();
            printReport(af::runSelfTest(target));
            if (!device.start(&tone, &err))
                std::printf("⚠ 検査のあとデバイスを開き直せませんでした ── %s\n", err.c_str());
            break;

        case 'i': case 'I': {
            std::printf("\n  ── 状態 ──────────────────────────────\n");
            if (device.running()) {
                std::printf("  出力     : %s\n", device.deviceName().c_str());
                std::printf("  形式     : %d Hz / %d ch / %d サンプル（持ち時間 %.2f ms）\n",
                            device.sampleRate(), device.channels(), device.bufferFrames(),
                            1000.0 * device.bufferFrames() / device.sampleRate());
                std::printf("  ブロック : %llu 回\n", device.callbacks());
                std::printf("  1 ブロック: 直近 %.4f ms / 最悪 %.4f ms（持ち時間の %.2f %%）\n",
                            device.lastBlockMs(), device.worstBlockMs(), device.worstLoadPercent());
                std::printf("  間に合わなかった回 : %llu\n", device.lateBlocks());
            } else {
                std::printf("  出力     : 開いていません\n");
            }
            std::printf("  読み込み : %s\n",
                        mod.loaded() ? af::wideToUtf8(mod.originalPath()).c_str() : "無し");
            std::printf("  ────────────────────────────────────────\n\n");
            break;
        }

        default: break;
        }
    }

    // ★止める順序に意味がある。
    //   先にデバイスを止めないと、実時間スレッドが音源を触っている最中に
    //   音源が消える。ここは「あとで直す」が効かない箇所なので順序を固定する。
    device.stop();
    mod.unload();

    std::printf("終了しました。\n");
    return 0;
}
