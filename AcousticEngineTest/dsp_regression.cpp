// dsp_regression.cpp — DSP（段4: C++ 移行）の数値回帰テスト。
//
// 方針は scene_regression.cpp と同じで、**絶対値ではなく関係**を確かめる。
// ただし FFT だけは「素朴な DFT と一致する」という絶対の正解があるので、それで押さえる。
// 移行は「Unity C# 版と同じ音が出る」ことが要件なので、各段で参照実装と突き合わせる。
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>

#include <algorithm>

#include "../AcousticEngine/src/Debug/detectors.h"          // 破れの型紙。走査（デバッグツール）と検査で同じ式を使う
#include "../AcousticEngine/src/Dsp/fft.h"
#include "../AcousticEngine/src/Dsp/partitioned_convolver.h"
#include "../AcousticEngine/src/Dsp/nonuniform_convolver.h"
#include "../AcousticEngine/src/Dsp/reverb_tail_ir.h"
#include "../AcousticEngine/src/Dsp/hrtf_set.h"
#include "../AcousticEngine/src/Dsp/hrtf_processor.h"
#include "../AcousticEngine/src/Dsp/early_reflect_conv.h"
#include "../AcousticEngine/src/Dsp/voice_renderer.h"
#include "../AcousticEngine/src/Dsp/fdn_tail.h"
#include "../AcousticEngine/src/Dsp/fdn_room_mix.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const char* name, bool ok, const char* detail = "") {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("    [%s] %-44s %s\n", ok ? "OK" : "FAIL", name, detail);
}

void checkNear(const char* name, double got, double want, double tol) {
    char b[128];
    std::snprintf(b, sizeof(b), "(got %.6g want %.6g tol %.1g)", got, want, tol);
    check(name, std::fabs(got - want) <= tol, b);
}

// 素朴な離散フーリエ変換。FFT の正解として使う（速度は問わない）。
void naiveDft(const std::vector<float>& re, const std::vector<float>& im,
              std::vector<float>& outRe, std::vector<float>& outIm) {
    const int n = static_cast<int>(re.size());
    outRe.assign(static_cast<std::size_t>(n), 0.0f);
    outIm.assign(static_cast<std::size_t>(n), 0.0f);
    for (int k = 0; k < n; ++k) {
        double sr = 0.0, si = 0.0;
        for (int t = 0; t < n; ++t) {
            const double a = -2.0 * 3.14159265358979323846 * k * t / n;
            const double c = std::cos(a), s = std::sin(a);
            sr += re[static_cast<std::size_t>(t)] * c - im[static_cast<std::size_t>(t)] * s;
            si += re[static_cast<std::size_t>(t)] * s + im[static_cast<std::size_t>(t)] * c;
        }
        outRe[static_cast<std::size_t>(k)] = static_cast<float>(sr);
        outIm[static_cast<std::size_t>(k)] = static_cast<float>(si);
    }
}

// 決定的な擬似乱数（テストを再現可能にする）。
struct Rng {
    unsigned int s = 12345u;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float>(static_cast<int>(s)) * (1.0f / 2147483648.0f);
    }
};

void testFft() {
    std::printf("\n[FFT] 素朴なDFTと一致するか / 順→逆で戻るか\n");

    check("2の冪でない長さは無効になる", !af::dsp::Fft(6).valid());
    check("長さ1は無効になる", !af::dsp::Fft(1).valid());

    for (int n : {8, 64, 256}) {
        af::dsp::Fft fft(n);
        char nameBuf[64];

        Rng rng;
        std::vector<float> re(static_cast<std::size_t>(n)), im(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            re[static_cast<std::size_t>(i)] = rng.next();
            im[static_cast<std::size_t>(i)] = rng.next();
        }
        const std::vector<float> re0 = re, im0 = im;

        std::vector<float> wantRe, wantIm;
        naiveDft(re0, im0, wantRe, wantIm);

        fft.transform(re.data(), im.data(), false);

        // 誤差は N に比例して積み上がるので許容も N でスケールさせる。
        double worst = 0.0;
        for (int i = 0; i < n; ++i) {
            worst = std::max(worst, static_cast<double>(std::fabs(
                re[static_cast<std::size_t>(i)] - wantRe[static_cast<std::size_t>(i)])));
            worst = std::max(worst, static_cast<double>(std::fabs(
                im[static_cast<std::size_t>(i)] - wantIm[static_cast<std::size_t>(i)])));
        }
        std::snprintf(nameBuf, sizeof(nameBuf), "N=%d 素朴なDFTと一致", n);
        checkNear(nameBuf, worst, 0.0, 1e-3 * n / 8.0);

        // 逆変換で元に戻る。
        fft.transform(re.data(), im.data(), true);
        worst = 0.0;
        for (int i = 0; i < n; ++i) {
            worst = std::max(worst, static_cast<double>(std::fabs(
                re[static_cast<std::size_t>(i)] - re0[static_cast<std::size_t>(i)])));
            worst = std::max(worst, static_cast<double>(std::fabs(
                im[static_cast<std::size_t>(i)] - im0[static_cast<std::size_t>(i)])));
        }
        std::snprintf(nameBuf, sizeof(nameBuf), "N=%d 順→逆で元に戻る", n);
        checkNear(nameBuf, worst, 0.0, 1e-5);
    }

    // 単位インパルスの変換は全ビン 1（位相 0）。境界の目視確認になる。
    {
        const int n = 16;
        af::dsp::Fft fft(n);
        std::vector<float> re(static_cast<std::size_t>(n), 0.0f), im(static_cast<std::size_t>(n), 0.0f);
        re[0] = 1.0f;
        fft.transform(re.data(), im.data(), false);
        bool allOne = true;
        for (int i = 0; i < n; ++i)
            if (std::fabs(re[static_cast<std::size_t>(i)] - 1.0f) > 1e-6f ||
                std::fabs(im[static_cast<std::size_t>(i)]) > 1e-6f) allOne = false;
        check("インパルスの変換は全ビン 1", allOne);
    }

    // null / 無効な器で落ちない（移行中に呼び出し規約が動くので境界は明示的に守る）。
    {
        af::dsp::Fft bad(0);
        bad.transform(nullptr, nullptr, false);
        af::dsp::Fft ok(8);
        ok.transform(nullptr, nullptr, false);
        check("null / 無効な器で落ちない", true);
    }
}

// ---------------------------------------------------------------- 分割畳み込み
// 周波数領域の分割畳み込みは、**時間領域の直接畳み込みと一致しなければならない**。
// ここは近似ではないので、関係ではなく値そのもので押さえられる数少ない部分。
//   固有遅延はブロック長 B（出力キューが無音で始まるため）。比較時にその分ずらす。
void testPartitionedConvolver() {
    std::printf("\n[分割畳み込み] 直接畳み込みと一致するか\n");

    const int B = 64, parts = 4, channels = 2;
    const int irLen = B * parts;
    af::dsp::PartitionedConvolver conv(B, parts, channels);

    check("IR を入れる前は HasIr が false", !conv.hasIr());
    checkNear("尾の長さ = B × パーティション数", conv.tailSamples(), irLen, 0.0);

    // チャンネルごとに違う IR（L/R が別スペクトルで畳まれることの確認を兼ねる）。
    Rng rng;
    std::vector<std::vector<float>> ir(2, std::vector<float>(static_cast<std::size_t>(irLen)));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < irLen; ++i) {
            // 後ろほど小さい＝残響尾らしい形。末尾は 0 にして activeParts の打ち切りも見る。
            const float env = (i < irLen * 3 / 4) ? std::exp(-3.0f * i / irLen) : 0.0f;
            ir[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = rng.next() * env;
        }
    const float* irPtr[2] = { ir[0].data(), ir[1].data() };
    const int irLens[2] = { irLen, irLen };
    conv.setIr(irPtr, irLens);
    check("IR を入れたら HasIr が true", conv.hasIr());

    // 入力を流す。
    const int nSamples = B * 12;
    std::vector<float> input(static_cast<std::size_t>(nSamples));
    for (int i = 0; i < nSamples; ++i) input[static_cast<std::size_t>(i)] = rng.next();

    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(nSamples), 0.0f));
    float* outPtr[2] = { out[0].data(), out[1].data() };
    // ブロック境界をまたぐ半端な刻みで呼ぶ（実機のコールバックは B の倍数とは限らない）。
    int done = 0;
    while (done < nSamples) {
        const int n = std::min(37, nSamples - done);
        conv.processAdd(input.data(), done, n, outPtr, done, 1.0f);
        done += n;
    }
    check("パーティション打ち切りが効いている(< 全数)", conv.activeParts() < parts,
          conv.activeParts() == parts ? "(打ち切られていない)" : "");

    // 直接畳み込みの正解。
    double worst = 0.0;
    for (int c = 0; c < channels; ++c) {
        for (int t = B; t < nSamples; ++t) {          // 先頭 B は固有遅延ぶん
            double want = 0.0;
            const int src = t - B;                     // 出力 t は入力 t-B に対応
            for (int k = 0; k < irLen && k <= src; ++k)
                want += static_cast<double>(input[static_cast<std::size_t>(src - k)])
                      * ir[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
            worst = std::max(worst, std::fabs(
                static_cast<double>(out[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)])
                - want));
        }
    }
    checkNear("直接畳み込みと一致（誤差 < 1e-4）", worst, 0.0, 1e-4);

    // 固有遅延の確認：先頭 B サンプルは無音。
    bool headSilent = true;
    for (int c = 0; c < channels; ++c)
        for (int t = 0; t < B; ++t)
            if (std::fabs(out[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)]) > 1e-6f)
                headSilent = false;
    check("固有遅延はブロック長ぶん（先頭Bは無音）", headSilent);

    // IR 差し替えがブロック境界で反映され、落ちないこと。
    std::vector<float> zero(static_cast<std::size_t>(irLen), 0.0f);
    const float* zeroPtr[2] = { zero.data(), zero.data() };
    conv.setIr(zeroPtr, irLens);
    conv.setIr(irPtr, irLens);            // 取り込まれる前に二度差し替える（世代の解放を突く）
    std::fill(out[0].begin(), out[0].end(), 0.0f);
    std::fill(out[1].begin(), out[1].end(), 0.0f);
    conv.processAdd(input.data(), 0, nSamples, outPtr, 0, 1.0f);
    check("取り込み前の二重差し替えで落ちない", true);

    // null 引数で落ちない。
    conv.processAdd(nullptr, 0, 16, outPtr, 0, 1.0f);
    conv.setIr(nullptr, nullptr);
    check("null 引数で落ちない", true);
}

// ---------------------------------------------------------------- 非一様分割
// 一様分割と同じ「直接畳み込みと一致する」を要求する。違うのは遅延だけ:
//   一様   … ブロック長 B ぶん遅れる
//   非一様 … 最小ブロック(firstBlock)ぶんしか遅れない ← これが導入の目的
void testNonUniformConvolver() {
    std::printf("\n[非一様分割] 直接畳み込みと一致するか / 遅延が最小ブロックか\n");

    const int irLen = 2048, channels = 2, firstBlock = 64, capBlock = 512;
    af::dsp::NonUniformConvolver conv(irLen, channels, firstBlock, capBlock);
    std::printf("      %s\n", conv.describeSchedule().c_str());

    checkNear("遅延 = 最小ブロック", conv.latency(), firstBlock, 0.0);
    check("段が複数に分かれている", conv.stageCount() >= 3);
    check("IR を入れる前は HasIr が false", !conv.hasIr());

    Rng rng;
    std::vector<std::vector<float>> ir(2, std::vector<float>(static_cast<std::size_t>(irLen)));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < irLen; ++i)
            ir[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                rng.next() * std::exp(-4.0f * i / irLen);
    const float* irPtr[2] = { ir[0].data(), ir[1].data() };
    const int irLens[2] = { irLen, irLen };
    conv.setIr(irPtr, irLens);
    check("IR を入れたら HasIr が true", conv.hasIr());

    const int nSamples = irLen * 2;
    std::vector<float> input(static_cast<std::size_t>(nSamples));
    for (int i = 0; i < nSamples; ++i) input[static_cast<std::size_t>(i)] = rng.next();

    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(nSamples), 0.0f));
    float* outPtr[2] = { out[0].data(), out[1].data() };
    int done = 0;
    while (done < nSamples) {
        const int n = std::min(101, nSamples - done);   // 半端な刻みで呼ぶ
        conv.processAdd(input.data(), done, n, outPtr, done, 1.0f);
        done += n;
    }

    double worst = 0.0;
    for (int c = 0; c < channels; ++c) {
        for (int t = firstBlock; t < nSamples; ++t) {
            double want = 0.0;
            const int src = t - firstBlock;
            for (int k = 0; k < irLen && k <= src; ++k)
                want += static_cast<double>(input[static_cast<std::size_t>(src - k)])
                      * ir[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
            worst = std::max(worst, std::fabs(
                static_cast<double>(out[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)])
                - want));
        }
    }
    checkNear("直接畳み込みと一致（誤差 < 1e-3）", worst, 0.0, 1e-3);

    bool headSilent = true;
    for (int c = 0; c < channels; ++c)
        for (int t = 0; t < firstBlock; ++t)
            if (std::fabs(out[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)]) > 1e-6f)
                headSilent = false;
    check("遅延は最小ブロックぶんだけ（先頭64が無音）", headSilent);

    // 一様分割なら同じ IR 長で遅延がずっと大きくなる ── 導入の意味を数値で残す。
    {
        af::dsp::PartitionedConvolver uni(capBlock, irLen / capBlock, channels);
        char b[96];
        std::snprintf(b, sizeof(b), "(非一様 %d samp / 一様 %d samp)", conv.latency(), uni.blockSize());
        check("一様分割より遅延が小さい", conv.latency() < uni.blockSize(), b);
    }

    // 作業領域を超える長さを一度に要求しても落ちない（内部で分割される）。
    conv.processAdd(input.data(), 0, nSamples, outPtr, 0, 0.0f);
    conv.processAdd(nullptr, 0, 16, outPtr, 0, 1.0f);
    conv.setIr(nullptr, nullptr);
    check("長い要求 / null 引数で落ちない", true);
}

// ---------------------------------------------------------------- 残響尾IR
// ここは「正解の波形」が無い（尾はノイズなので、どのノイズかに音質上の意味は無い）。
// なので scene_regression.cpp と同じく**関係**で押さえる:
//   ・エネルギー1に正規化されている（絶対レベルは呼び手が D/R 比から決める前提）
//   ・startMs より前は無音（早期反射と二重計上しない）
//   ・減衰が速い部屋ほど尾のエネルギー比が小さい
//   ・高域が先に減るエコグラムを渡すと、尾の後半で高域が減っている
void testReverbTailIr() {
    std::printf("\n[残響尾IR] エコグラム → 尾の波形\n");

    const int sr = 48000, len = sr / 2, channels = 2;   // 0.5 秒
    const int bins = 100;
    const float binMs = 5.0f;
    af::dsp::ReverbTailIr tail(sr, len, channels, bins);

    // 指数減衰のエコグラム（全帯域同じ減衰）。
    auto makeEcho = [&](float decayPerBin, std::vector<float>& echo) {
        echo.assign(static_cast<std::size_t>(bins) * 6, 0.0f);
        for (int k = 0; k < bins; ++k) {
            const float e = std::pow(decayPerBin, static_cast<float>(k));
            for (int b = 0; b < 6; ++b) echo[static_cast<std::size_t>(k) * 6 + b] = e;
        }
    };

    std::vector<float> echo;
    makeEcho(0.95f, echo);
    const float startMs = 40.0f;
    const float ratio = tail.build(echo.data(), bins, binMs, startMs, 10.0f, 0.0f, 0.0f, 1.0f);

    check("尾のエネルギー比が正", ratio > 0.0f);

    // 全チャンネル合計でエネルギー1。ステレオで √2 大きくならないことの確認を兼ねる。
    double energy = 0.0;
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < len; ++i) {
            const double v = tail.ir(c)[i];
            energy += v * v;
        }
    checkNear("全チャンネル合計でエネルギー1", energy, 1.0, 1e-3);

    // startMs より前は無音（早期反射側の担当なので二重計上しない）。
    const int startSample = static_cast<int>(startMs * 0.001f * sr);
    bool headSilent = true;
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < startSample; ++i)
            if (std::fabs(tail.ir(c)[i]) > 1e-9f) headSilent = false;
    check("startMs より前は無音", headSilent);

    // L/R が無相関（チャンネルごとに独立ノイズ＝尾が広がる）。
    {
        double dot = 0.0, nl = 0.0, nr = 0.0;
        for (int i = 0; i < len; ++i) {
            const double l = tail.ir(0)[i], r = tail.ir(1)[i];
            dot += l * r; nl += l * l; nr += r * r;
        }
        const double corr = (nl > 0 && nr > 0) ? std::fabs(dot / std::sqrt(nl * nr)) : 1.0;
        char b[64];
        std::snprintf(b, sizeof(b), "(相関 %.3f)", corr);
        check("L/R が無相関（相関 < 0.1）", corr < 0.1, b);
    }

    // 減衰が速い部屋ほど尾/直接の比が小さい。
    {
        std::vector<float> fast;
        makeEcho(0.80f, fast);
        const float ratioFast = tail.build(fast.data(), bins, binMs, startMs, 10.0f, 0.0f, 0.0f, 1.0f);
        char b[96];
        std::snprintf(b, sizeof(b), "(遅い減衰 %.3f > 速い減衰 %.3f)", ratio, ratioFast);
        check("減衰が速いほど尾が短い（比が小さい）", ratioFast < ratio, b);
    }

    // 高域が先に減るエコグラムなら、尾の後半で高域の寄与が減っている。
    {
        std::vector<float> tilt(static_cast<std::size_t>(bins) * 6, 0.0f);
        for (int k = 0; k < bins; ++k)
            for (int b = 0; b < 6; ++b) {
                // 帯域が上ほど速く減る（実際の部屋の挙動）。
                const float d = 0.97f - 0.03f * b;
                tilt[static_cast<std::size_t>(k) * 6 + b] = std::pow(d, static_cast<float>(k));
            }
        tail.build(tilt.data(), bins, binMs, startMs, 10.0f, 0.0f, 0.0f, 1.0f);
        // 前半と後半の RMS を比べる。高域が落ちるので後半ほど「鈍い」＝ゼロ交差が減る。
        int zcEarly = 0, zcLate = 0;
        const int mid = (startSample + len) / 2;
        for (int i = startSample + 1; i < mid; ++i)
            if ((tail.ir(0)[i - 1] < 0) != (tail.ir(0)[i] < 0)) ++zcEarly;
        for (int i = mid + 1; i < len; ++i)
            if ((tail.ir(0)[i - 1] < 0) != (tail.ir(0)[i] < 0)) ++zcLate;
        const double rEarly = static_cast<double>(zcEarly) / std::max(1, mid - startSample);
        const double rLate = static_cast<double>(zcLate) / std::max(1, len - mid);
        char b[96];
        std::snprintf(b, sizeof(b), "(前半 %.3f > 後半 %.3f)", rEarly, rLate);
        check("高域が先に減る（後半ほどゼロ交差が少ない）", rLate < rEarly, b);
    }

    // 絶対ゲインの決め方。尾IRはエネルギー1なので tailGain = directGain × √target。
    checkNear("calibrateGain: 目標比の平方根倍",
              af::dsp::ReverbTailIr::calibrateGain(0.5f, 4.0f), 1.0, 1e-6);
    checkNear("calibrateGain: 目標比0で0",
              af::dsp::ReverbTailIr::calibrateGain(0.5f, 0.0f), 0.0, 1e-9);

    // null / 異常値で落ちない。
    tail.build(nullptr, bins, binMs, startMs, 10.0f, 0.0f, 0.0f, 1.0f);
    tail.build(echo.data(), 0, binMs, startMs, 10.0f, 0.0f, 0.0f, 1.0f);
    tail.build(echo.data(), bins, 0.0f, startMs, 10.0f, 0.0f, 0.0f, 1.0f);
    check("null / 異常値で落ちない", true);
}

// ---------------------------------------------------------------- HRTF
// 実測データが無い環境でも回るよう合成HRTF（球体頭）を持つ。ここで確かめるのは
// 「両耳の手がかりが物理的に正しい向きに出ているか」:
//   ・音源が右なら右耳が先に鳴る（ITD の符号）
//   ・音源が右なら左耳が影になる（ILD／高域の減り）
//   ・ITD が頭囲に比例してスケールする（個人最適化の要）
// .afhr の読み込みは、生成したバイト列を食わせて往復で確かめる。
void testHrtf() {
    std::printf("\n[HRTF] 両耳の手がかりが正しい向きに出るか\n");

    const int sr = 48000;
    af::dsp::HrtfSet syn = af::dsp::HrtfSet::createSynthetic(sr, 5, 10);
    check("合成HRTFが有効", syn.isValid());
    check("方向が十分にある(>500)", syn.directionCount() > 500);

    auto at = [&](float az, float el) {
        float d[3];
        af::dsp::HrtfSet::angleToVector(az, el, d);
        return syn.nearestIndex(d);
    };

    // 方向検索が正しい測定点を引く。
    {
        float a = 0, e = 0;
        syn.getAngles(at(90.0f, 0.0f), a, e);
        char b[64];
        std::snprintf(b, sizeof(b), "(az %.0f el %.0f)", a, e);
        check("右(az=90)を引くと az≈90 が返る", std::fabs(a - 90.0f) < 6.0f, b);
    }

    // ITD の符号: 音源が右 → 右耳が先 → ITD は負（正=右耳が遅い）。
    {
        const float itdR = syn.itdSeconds(at(90.0f, 0.0f));
        const float itdL = syn.itdSeconds(at(-90.0f, 0.0f));
        char b[96];
        std::snprintf(b, sizeof(b), "(右 %.1fus / 左 %.1fus)", itdR * 1e6f, itdL * 1e6f);
        check("音源が右なら右耳が先（ITD<0）", itdR < 0.0f, b);
        check("音源が左なら右耳が遅い（ITD>0）", itdL > 0.0f, b);
        checkNear("左右で対称", static_cast<double>(itdR + itdL), 0.0, 2e-5);
    }
    checkNear("正面の ITD は 0", syn.itdSeconds(at(0.0f, 0.0f)), 0.0, 2e-5);

    // ITD の大きさが妥当（頭半径 8.75cm なら最大 ±0.6ms 程度）。
    {
        const float itd = std::fabs(syn.itdSeconds(at(90.0f, 0.0f)));
        char b[64];
        std::snprintf(b, sizeof(b), "(%.0f us)", itd * 1e6f);
        check("真横の ITD が 400〜900us", itd > 400e-6f && itd < 900e-6f, b);
    }

    // ILD: 音源が右なら左耳が影＝高域が減る。HRIR のエネルギーと「鈍さ」で見る。
    {
        const int idx = at(90.0f, 0.0f);
        const float* l = syn.hrir(idx, 0);
        const float* r = syn.hrir(idx, 1);
        double el = 0.0, er = 0.0;
        for (int i = 0; i < syn.irLength(); ++i) { el += l[i] * l[i]; er += r[i] * r[i]; }
        // 高域の残り方 = 隣接差分のエネルギー比（鈍い波形ほど小さい）。
        double hl = 0.0, hr = 0.0;
        for (int i = 1; i < syn.irLength(); ++i) {
            const double dl = l[i] - l[i - 1], dr = r[i] - r[i - 1];
            hl += dl * dl; hr += dr * dr;
        }
        char b[96];
        std::snprintf(b, sizeof(b), "(高域 左 %.4f / 右 %.4f)", hl, hr);
        check("音源が右なら左耳の高域が減る（頭部の影）", hl < hr, b);
        check("エネルギーは左耳の方が小さい", el < er);
    }

    // ITD の個人スケール。頭囲に比例する。
    {
        const int idx = at(90.0f, 0.0f);
        const float base = syn.itdSeconds(idx);
        const float ref = syn.referenceHeadCircumferenceCm();
        const float big = syn.itdSecondsScaled(idx, ref * 2.0f);
        checkNear("頭囲2倍で ITD も2倍", big, base * 2.0f, 1e-9);
        checkNear("基準頭囲ならそのまま", syn.itdSecondsScaled(idx, ref), base, 1e-9);
    }

    // .afhr の往復。バイト列を組み立てて読み戻す。
    {
        const int n = 2, irLen = 8;
        std::vector<unsigned char> buf;
        auto push32 = [&](const void* p) {
            const unsigned char* b = static_cast<const unsigned char*>(p);
            for (int i = 0; i < 4; ++i) buf.push_back(b[i]);
        };
        const std::uint32_t magic = af::dsp::HrtfSet::kMagic;
        const std::int32_t ver = af::dsp::HrtfSet::kVersion, sr2 = 44100, nn = n, il = irLen;
        push32(&magic); push32(&ver); push32(&sr2); push32(&nn); push32(&il);
        const float angles[4] = {0.0f, 0.0f, 90.0f, 0.0f};
        for (int i = 0; i < 4; ++i) push32(&angles[i]);
        // dir0: 左右同時（ITD 0）/ dir1: 右耳が 2 サンプル先（ITD 負）
        for (int d = 0; d < n; ++d)
            for (int ear = 0; ear < 2; ++ear)
                for (int k = 0; k < irLen; ++k) {
                    float v = 0.0f;
                    const int onset = (d == 1 && ear == 1) ? 1 : 3;
                    if (k == onset) v = 1.0f;
                    push32(&v);
                }

        af::dsp::HrtfSet loaded;
        const bool ok = af::dsp::HrtfSet::loadFromBytes(buf.data(), buf.size(), "test", loaded);
        check(".afhr が読める", ok);
        if (ok) {
            checkNear(".afhr の sampleRate", loaded.sampleRate(), 44100, 0.0);
            checkNear(".afhr の方向数", loaded.directionCount(), n, 0.0);
            // dir1 は右耳が 2 サンプル先 → ITD = +2/44100（正=右耳が遅い、の逆なので負）
            const float itd = loaded.itdSeconds(1);
            char b[64];
            std::snprintf(b, sizeof(b), "(%.1f us)", itd * 1e6f);
            check("焼き込まれた ITD が抜き出される", itd < 0.0f, b);
            // 立ち上がりは先頭へ揃っている。
            const float* r = loaded.hrir(1, 1);
            check("立ち上がりが先頭へ揃う", r && std::fabs(r[0] - 1.0f) < 1e-6f);
        }
    }

    // 壊れた入力で落ちない。
    {
        af::dsp::HrtfSet bad;
        unsigned char junk[32] = {0};
        check("magic 不一致は失敗を返す",
              !af::dsp::HrtfSet::loadFromBytes(junk, sizeof(junk), "junk", bad));
        check("短すぎる入力は失敗を返す",
              !af::dsp::HrtfSet::loadFromBytes(junk, 4, "junk", bad));
        check("null で落ちない", !af::dsp::HrtfSet::loadFromBytes(nullptr, 0, nullptr, bad));
        check("無効な器の検索が -1", bad.nearestIndex(nullptr) < 0);
    }
}

// ---------------------------------------------------------------- HRTF 処理
// 「モノラルを、その方向から聞こえるステレオにする」ことを確かめる:
//   ・音源が右なら右チャンネルが大きく、かつ先に立ち上がる
//   ・低域は HRIR を通さず素通し（実測 HRIR は低域を持っていないため。痩せ対策）
//   ・方向が急に変わってもクリックが出ない（クロスフェード）
void testHrtfProcessor() {
    std::printf("\n[HRTF処理] モノラル → バイノーラル\n");

    const int sr = 48000;
    af::dsp::HrtfSet set = af::dsp::HrtfSet::createSynthetic(sr, 5, 10);
    af::dsp::HrtfProcessor proc(sr, 12.0f, 700.0f);

    check("セット前は未準備", !proc.isReady());
    proc.setHrtfSet(&set);
    check("セット後は準備完了", proc.isReady());

    const int n = 4096;
    std::vector<float> input(static_cast<std::size_t>(n), 0.0f);
    // インパルス。立ち上がりの前後関係を見るため。
    //   ★クロスフェード（12ms=576サンプル）が終わってから鳴らすこと。
    //     途中に置くと、まだ正面向きの HRIR（ITD 0）が支配的で左右差が出ない。
    input[2048] = 1.0f;

    auto render = [&](float az, std::vector<float>& l, std::vector<float>& r) {
        af::dsp::HrtfProcessor p(sr, 12.0f, 700.0f);
        p.setHrtfSet(&set);
        float d[3];
        af::dsp::HrtfSet::angleToVector(az, 0.0f, d);
        p.setDirection(d, 57.0f);
        l.assign(static_cast<std::size_t>(n), 0.0f);
        r.assign(static_cast<std::size_t>(n), 0.0f);
        p.processAdd(input.data(), 0, n, l.data(), r.data(), 0, 1.0f);
    };

    // 右から鳴らす。
    std::vector<float> l, r;
    render(90.0f, l, r);

    // 立ち上がり位置（最初にピークの15%を超える点）で前後関係を見る。
    auto onset = [&](const std::vector<float>& v) {
        float peak = 0.0f;
        for (float x : v) peak = std::max(peak, std::fabs(x));
        if (peak <= 1e-9f) return -1;
        for (int i = 0; i < n; ++i) if (std::fabs(v[static_cast<std::size_t>(i)]) >= peak * 0.15f) return i;
        return -1;
    };
    {
        const int ol = onset(l), orr = onset(r);
        char b[96];
        std::snprintf(b, sizeof(b), "(左 %d / 右 %d samp)", ol, orr);
        check("音源が右なら右耳が先に立ち上がる", orr >= 0 && ol > orr, b);
    }
    {
        double el = 0.0, er = 0.0;
        for (int i = 0; i < n; ++i) {
            el += l[static_cast<std::size_t>(i)] * l[static_cast<std::size_t>(i)];
            er += r[static_cast<std::size_t>(i)] * r[static_cast<std::size_t>(i)];
        }
        char b[96];
        std::snprintf(b, sizeof(b), "(左 %.4f / 右 %.4f)", el, er);
        check("音源が右なら右耳の方が大きい", er > el, b);
    }

    // 左右対称：az=-90 は az=+90 の左右反転になっているはず。
    {
        std::vector<float> l2, r2;
        render(-90.0f, l2, r2);
        double diff = 0.0, scale = 0.0;
        for (int i = 0; i < n; ++i) {
            diff += std::fabs(l[static_cast<std::size_t>(i)] - r2[static_cast<std::size_t>(i)]);
            scale += std::fabs(l[static_cast<std::size_t>(i)]);
        }
        const double rel = (scale > 0) ? diff / scale : 1.0;
        char b[64];
        std::snprintf(b, sizeof(b), "(相対差 %.4f)", rel);
        check("左右対称（+90の左耳 ≒ -90の右耳）", rel < 0.02, b);
    }

    // 低域は素通し（実測 HRIR は低域を持っていないので痩せる。ITD だけ適用する設計）。
    {
        // 正面から低い正弦波を入れて、入出力のパワー比を見る。
        std::vector<float> lo(static_cast<std::size_t>(n));
        const float f = 80.0f;
        for (int i = 0; i < n; ++i)
            lo[static_cast<std::size_t>(i)] =
                std::sin(2.0f * 3.14159265f * f * i / static_cast<float>(sr));
        af::dsp::HrtfProcessor p(sr, 12.0f, 700.0f);
        p.setHrtfSet(&set);
        float d[3];
        af::dsp::HrtfSet::angleToVector(0.0f, 0.0f, d);
        p.setDirection(d, 57.0f);
        std::vector<float> ol(static_cast<std::size_t>(n), 0.0f), orr(static_cast<std::size_t>(n), 0.0f);
        p.processAdd(lo.data(), 0, n, ol.data(), orr.data(), 0, 1.0f);
        // 後半（過渡を避ける）のRMSで比べる。
        double pin = 0.0, pout = 0.0;
        for (int i = n / 2; i < n; ++i) {
            pin += lo[static_cast<std::size_t>(i)] * lo[static_cast<std::size_t>(i)];
            pout += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
        }
        const double ratio = (pin > 0) ? std::sqrt(pout / pin) : 0.0;
        char b[64];
        std::snprintf(b, sizeof(b), "(利得 %.3f)", ratio);
        check("低域(80Hz)が痩せない（利得 0.8〜1.3）", ratio > 0.8 && ratio < 1.3, b);
    }

    // 方向を急変させてもクリックが出ない（クロスフェード）。
    {
        af::dsp::HrtfProcessor p(sr, 12.0f, 700.0f);
        p.setHrtfSet(&set);
        std::vector<float> tone(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            tone[static_cast<std::size_t>(i)] =
                std::sin(2.0f * 3.14159265f * 1000.0f * i / static_cast<float>(sr));
        std::vector<float> ol(static_cast<std::size_t>(n), 0.0f), orr(static_cast<std::size_t>(n), 0.0f);
        float d[3];
        af::dsp::HrtfSet::angleToVector(-90.0f, 0.0f, d);
        p.setDirection(d, 57.0f);
        p.processAdd(tone.data(), 0, n / 2, ol.data(), orr.data(), 0, 1.0f);
        af::dsp::HrtfSet::angleToVector(90.0f, 0.0f, d);   // 左→右へ一気に飛ばす
        p.setDirection(d, 57.0f);
        p.processAdd(tone.data(), n / 2, n / 2, ol.data(), orr.data(), n / 2, 1.0f);

        // 隣接サンプル差の最大が、信号自身の傾きの数倍を超えないこと＝段差なし。
        double maxStep = 0.0, typical = 0.0;
        for (int i = 1; i < n; ++i) {
            const double s = std::fabs(ol[static_cast<std::size_t>(i)] - ol[static_cast<std::size_t>(i - 1)]);
            maxStep = std::max(maxStep, s);
            typical += s;
        }
        typical /= (n - 1);
        char b[96];
        std::snprintf(b, sizeof(b), "(最大差 %.4f / 平均 %.4f)", maxStep, typical);
        check("方向の急変でクリックが出ない", maxStep < typical * 8.0, b);
    }

    // null / 未初期化で落ちない。
    {
        af::dsp::HrtfProcessor p(sr);
        std::vector<float> o(16, 0.0f);
        p.processAdd(input.data(), 0, 16, o.data(), o.data(), 0, 1.0f);   // セット無し
        p.setHrtfSet(nullptr);
        p.setDirection(nullptr, 57.0f);
        float a = 0, bb = 0;
        p.processSample(1.0f, a, bb);
        check("未初期化 / null で落ちない", true);
    }
}

// ---------------------------------------------------------------- 早期反射
// エンジンのタップ（遅延・6帯域ゲイン・パン・散乱）が、そのまま波形になることを確かめる:
//   ・タップの遅延どおりの位置にインパルスが出る
//   ・6帯域分割が完全再構成（全帯域同じゲインなら素通し）
//   ・パンが左右に効く
//   ・散乱ぶんが拡散送りへ回り、鏡面＋拡散でパワーが保存される
//   ・タップ差し替えでクリックが出ない
void testEarlyReflectConv() {
    std::printf("\n[早期反射] タップ → 波形\n");

    const int sr = 48000;
    af::dsp::EarlyReflectConv conv(sr, 4800, 5.0f);   // 100ms ぶんの履歴 / 5ms クロスフェード
    check("タップを入れる前は false", !conv.hasTaps());

    // 全帯域ゲイン1・パン中央・散乱なしのタップを2本。
    af::dsp::EarlyReflectConv::Tap taps[2];
    for (int b = 0; b < 6; ++b) { taps[0].g[b] = 1.0f; taps[1].g[b] = 0.5f; }
    taps[0].delaySamples = 0;
    taps[1].delaySamples = 100;
    taps[0].panL = taps[0].panR = 0.70710678f;
    taps[1].panL = 1.0f; taps[1].panR = 0.0f;         // 2本目は左だけ
    conv.setTaps(taps, 2);
    check("タップを入れたら true", conv.hasTaps());

    const int n = 2048;
    std::vector<float> input(static_cast<std::size_t>(n), 0.0f);
    std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(static_cast<std::size_t>(n), 0.0f);
    std::vector<float> diff(static_cast<std::size_t>(n), 0.0f);
    // クロスフェード(5ms=240samp)が済んでからインパルスを置く。
    input[1024] = 1.0f;
    conv.processAdd(input.data(), 0, n, false, l.data(), r.data(), 0, 1.0f, diff.data());

    // 遅延どおりの位置にピークが出る。
    auto peakAt = [&](const std::vector<float>& v, int from, int to) {
        int best = from; float bv = 0.0f;
        for (int i = from; i < to; ++i)
            if (std::fabs(v[static_cast<std::size_t>(i)]) > bv) { bv = std::fabs(v[static_cast<std::size_t>(i)]); best = i; }
        return best;
    };
    checkNear("タップ0が遅延0の位置に出る", peakAt(l, 1000, 1060), 1024, 2.0);
    {
        // 2本目は左だけ。右チャンネルの 1124 付近は静か。
        const int pl = peakAt(l, 1100, 1160);
        char b[96];
        std::snprintf(b, sizeof(b), "(左ピーク %d)", pl);
        check("タップ1が遅延100の位置に出る", std::abs(pl - 1124) <= 3, b);
        double er = 0.0;
        for (int i = 1110; i < 1140; ++i) er += r[static_cast<std::size_t>(i)] * r[static_cast<std::size_t>(i)];
        check("タップ1は左だけ（右は静か）", er < 1e-4, "");
    }

    // 6帯域分割の完全再構成：全帯域ゲイン1・遅延0・散乱0なら入力がそのまま出る。
    {
        af::dsp::EarlyReflectConv c2(sr, 1024, 0.02f);
        af::dsp::EarlyReflectConv::Tap t1;
        for (int b = 0; b < 6; ++b) t1.g[b] = 1.0f;
        t1.panL = 1.0f; t1.panR = 1.0f;
        c2.setTaps(&t1, 1);

        Rng rng;
        const int m = 4096;
        std::vector<float> in(static_cast<std::size_t>(m)), ol(static_cast<std::size_t>(m), 0.0f),
                           orr(static_cast<std::size_t>(m), 0.0f);
        for (int i = 0; i < m; ++i) in[static_cast<std::size_t>(i)] = rng.next();
        c2.processAdd(in.data(), 0, m, false, ol.data(), orr.data(), 0, 1.0f);

        double worst = 0.0;
        for (int i = m / 2; i < m; ++i)   // 過渡を避けて後半で比べる
            worst = std::max(worst, static_cast<double>(std::fabs(
                ol[static_cast<std::size_t>(i)] - in[static_cast<std::size_t>(i)])));
        checkNear("6帯域分割が完全再構成（足すと元に戻る）", worst, 0.0, 1e-4);
    }

    // 散乱：鏡面＋拡散でパワーが保存される（√で分けているので (1-s)+s=1）。
    {
        af::dsp::EarlyReflectConv c3(sr, 1024, 0.02f);
        af::dsp::EarlyReflectConv::Tap t1;
        for (int b = 0; b < 6; ++b) t1.g[b] = 1.0f;
        t1.panL = 1.0f; t1.panR = 0.0f;
        af::dsp::EarlyReflectConv::scatterSplit(10.0f, 20.0f, 0.0f, 1.0f, t1.gSpec, t1.gDiff);
        char b[96];
        std::snprintf(b, sizeof(b), "(鏡面 %.3f / 拡散 %.3f)", t1.gSpec, t1.gDiff);
        checkNear("散乱の鏡面²+拡散²=1", t1.gSpec * t1.gSpec + t1.gDiff * t1.gDiff, 1.0, 1e-6);
        check("mixing time の半分なら半々くらい", t1.gDiff > 0.5f && t1.gDiff < 0.8f, b);

        c3.setTaps(&t1, 1);
        Rng rng;
        const int m = 4096;
        std::vector<float> in(static_cast<std::size_t>(m)), ol(static_cast<std::size_t>(m), 0.0f),
                           orr(static_cast<std::size_t>(m), 0.0f), dd(static_cast<std::size_t>(m), 0.0f);
        for (int i = 0; i < m; ++i) in[static_cast<std::size_t>(i)] = rng.next();
        // 拡散送りは**左右別**になった（散乱にも方向を持たせるため）。
        // パワーを見るときは両方を足すこと。片方だけだとパンのぶん足りなくなる。
        std::vector<float> dr(static_cast<std::size_t>(m), 0.0f);
        c3.processAdd(in.data(), 0, m, false, ol.data(), orr.data(), 0, 1.0f, dd.data(),
                      nullptr, nullptr, dr.data());
        double pIn = 0.0, pSpec = 0.0, pDiff = 0.0;
        for (int i = m / 2; i < m; ++i) {
            pIn += in[static_cast<std::size_t>(i)] * in[static_cast<std::size_t>(i)];
            pSpec += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
            pDiff += dd[static_cast<std::size_t>(i)] * dd[static_cast<std::size_t>(i)]
                   + dr[static_cast<std::size_t>(i)] * dr[static_cast<std::size_t>(i)];
        }
        char b2[96];
        std::snprintf(b2, sizeof(b2), "(入 %.1f / 鏡面+拡散 %.1f)", pIn, pSpec + pDiff);
        checkNear("鏡面+拡散でパワーが保存される", (pSpec + pDiff) / pIn, 1.0, 0.02);
        std::printf("        %s\n", b2);
    }

    // 散乱率は遅延とともに上がる（高次反射ほど拡散へ溶ける）。
    {
        float s1 = 0, d1 = 0, s2 = 0, d2 = 0;
        af::dsp::EarlyReflectConv::scatterSplit(2.0f, 20.0f, 0.0f, 1.0f, s1, d1);
        af::dsp::EarlyReflectConv::scatterSplit(18.0f, 20.0f, 0.0f, 1.0f, s2, d2);
        char b[96];
        std::snprintf(b, sizeof(b), "(2ms %.3f → 18ms %.3f)", d1, d2);
        check("遅いタップほど拡散が増える", d2 > d1, b);
    }

    // HRTF 有効時は直接音が L/R に入らず、モノラルで出てくる。
    {
        af::dsp::EarlyReflectConv c4(sr, 1024, 0.02f);
        af::dsp::EarlyReflectConv::Tap t1;
        for (int b = 0; b < 6; ++b) t1.g[b] = 1.0f;
        c4.setTaps(&t1, 1);
        const int m = 512;
        std::vector<float> in(static_cast<std::size_t>(m), 0.0f), ol(static_cast<std::size_t>(m), 0.0f),
                           orr(static_cast<std::size_t>(m), 0.0f), dm(static_cast<std::size_t>(m), 0.0f);
        in[256] = 1.0f;
        c4.processAdd(in.data(), 0, m, true, ol.data(), orr.data(), 0, 1.0f, nullptr, dm.data());
        double el = 0.0, edm = 0.0;
        for (int i = 0; i < m; ++i) {
            el += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
            edm += dm[static_cast<std::size_t>(i)] * dm[static_cast<std::size_t>(i)];
        }
        check("HRTF有効時は直接音がL/Rに入らない", el < 1e-6);
        check("HRTF有効時は直接音がモノラルで出る", edm > 0.5);
    }

    // タップ差し替えでクリックが出ない。
    {
        af::dsp::EarlyReflectConv c5(sr, 1024, 5.0f);
        af::dsp::EarlyReflectConv::Tap a, b2;
        for (int b = 0; b < 6; ++b) { a.g[b] = 1.0f; b2.g[b] = 0.1f; }
        a.panL = b2.panL = 1.0f; a.panR = b2.panR = 1.0f;
        c5.setTaps(&a, 1);
        const int m = 4096;
        std::vector<float> tone(static_cast<std::size_t>(m)), ol(static_cast<std::size_t>(m), 0.0f),
                           orr(static_cast<std::size_t>(m), 0.0f);
        for (int i = 0; i < m; ++i)
            tone[static_cast<std::size_t>(i)] = std::sin(2.0f * 3.14159265f * 500.0f * i / sr);
        c5.processAdd(tone.data(), 0, m / 2, false, ol.data(), orr.data(), 0, 1.0f);
        c5.setTaps(&b2, 1);                    // 1.0 → 0.1 の大きな差し替え
        c5.processAdd(tone.data(), m / 2, m / 2, false, ol.data(), orr.data(), m / 2, 1.0f);
        double maxStep = 0.0, typical = 0.0;
        for (int i = 1; i < m; ++i) {
            const double s = std::fabs(ol[static_cast<std::size_t>(i)] - ol[static_cast<std::size_t>(i - 1)]);
            maxStep = std::max(maxStep, s);
            typical += s;
        }
        typical /= (m - 1);
        char bb[96];
        std::snprintf(bb, sizeof(bb), "(最大差 %.4f / 平均 %.4f)", maxStep, typical);
        check("タップ差し替えでクリックが出ない", maxStep < typical * 8.0, bb);
    }

    // ★新しく現れたタップは**ゲイン0から立ち上がる**（対応相手がいないので）。
    //   出力クロスフェードだと、現れた瞬間に本来のゲインで混ざり始めて段差になる。
    //   パラメータ補間なら構造的に 0 から始まる ── 「カチッ」が「ぐわっ」になる。
    {
        const int sr2 = 48000;
        af::dsp::EarlyReflectConv c(sr2, 4800, 40.0f);   // 40ms 補間
        af::dsp::EarlyReflectConv::Tap one;
        for (int b = 0; b < 6; ++b) one.g[b] = 1.0f;
        one.panL = one.panR = 0.70710678f;
        c.setTaps(&one, 1);

        // ★信号はノイズにする。純音だと、遅延差がちょうど半周期の倍数になったとき
        //   2本目が逆位相で1本目を打ち消し、「補間が壊れている」ように見える
        //   （300Hz・2000サンプル＝12.5周期でそうなった）。無相関ならパワーが加算される。
        const int m = 8192;
        Rng rng2;
        std::vector<float> tone(static_cast<std::size_t>(m));
        for (int i = 0; i < m; ++i) tone[static_cast<std::size_t>(i)] = rng2.next();
        std::vector<float> ol(static_cast<std::size_t>(m), 0.0f), orr(static_cast<std::size_t>(m), 0.0f);
        c.processAdd(tone.data(), 0, m / 2, false, ol.data(), orr.data(), 0, 1.0f);

        // 2本目を「遠い遅延」で足す＝対応相手なし＝新しく現れた開口。
        af::dsp::EarlyReflectConv::Tap two[2];
        two[0] = one;
        for (int b = 0; b < 6; ++b) two[1].g[b] = 1.0f;
        two[1].delaySamples = 2000;
        two[1].panL = two[1].panR = 0.70710678f;
        c.setTaps(two, 2);
        c.processAdd(tone.data(), m / 2, 1, false, ol.data(), orr.data(), m / 2, 1.0f);
        std::printf("        差し替え直後 進捗%.3f: ", c.debugProgress());
        for (int i = 0; i < c.liveTapCount(); ++i)
            std::printf("[%d: from=%.2f to=%.2f d=%.0f] ",
                        i, c.debugStartGain(i), c.debugTargetGain(i), c.debugTargetDelay(i));
        std::printf("\n");
        c.processAdd(tone.data(), m / 2 + 1, m / 2 - 1, false, ol.data(), orr.data(), m / 2 + 1, 1.0f);

        // 差し替え直後の包絡が、いきなり倍にならず徐々に上がること。
        auto rms = [&](int from, int to) {
            double s = 0.0;
            for (int i = from; i < to; ++i) s += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
            return std::sqrt(s / std::max(1, to - from));
        };
        const double before = rms(m / 2 - 400, m / 2);          // 1本のとき
        const double justAfter = rms(m / 2, m / 2 + 200);       // 補間の入り口
        const double after = rms(m - 400, m);                   // 補間が終わった後
        std::printf("        包絡: ");
        for (int i = m / 2; i < m; i += 512)
            std::printf("%.3f ", rms(i, std::min(m, i + 256)));
        std::printf(" タップ %d本", c.liveTapCount());
        for (int i = 0; i < c.liveTapCount(); ++i)
            std::printf(" [%d: g=%.2f d=%.0f]", i, c.debugTargetGain(i), c.debugTargetDelay(i));
        std::printf("\n");
        char b[128];
        std::snprintf(b, sizeof(b), "(前 %.3f → 直後 %.3f → 後 %.3f)", before, justAfter, after);
        check("新タップは0から立ち上がる（直後は最終値より小さい）",
              justAfter < after * 0.95 && justAfter >= before * 0.9, b);
        check("補間が終われば本来の値になる", after > before * 1.2, b);
    }

    // 遅延の補間：小数遅延で読むので、遅延を動かしてもジリつかない。
    {
        const int sr2 = 48000;
        af::dsp::EarlyReflectConv c(sr2, 4800, 40.0f);
        af::dsp::EarlyReflectConv::Tap a;
        for (int b = 0; b < 6; ++b) a.g[b] = 1.0f;
        a.panL = a.panR = 1.0f;
        a.delaySamples = 100;
        c.setTaps(&a, 1);
        const int m = 8192;
        std::vector<float> tone(static_cast<std::size_t>(m));
        for (int i = 0; i < m; ++i)
            tone[static_cast<std::size_t>(i)] = std::sin(2.0f * 3.14159265f * 400.0f * i / sr2);
        std::vector<float> ol(static_cast<std::size_t>(m), 0.0f), orr(static_cast<std::size_t>(m), 0.0f);
        c.processAdd(tone.data(), 0, m / 2, false, ol.data(), orr.data(), 0, 1.0f);
        a.delaySamples = 180;                       // 近いので同じ開口として対応付く
        c.setTaps(&a, 1);
        c.processAdd(tone.data(), m / 2, m / 2, false, ol.data(), orr.data(), m / 2, 1.0f);

        double maxStep = 0.0, typical = 0.0;
        for (int i = 1; i < m; ++i) {
            const double s = std::fabs(ol[static_cast<std::size_t>(i)] - ol[static_cast<std::size_t>(i - 1)]);
            maxStep = std::max(maxStep, s);
            typical += s;
        }
        typical /= (m - 1);
        char b[96];
        std::snprintf(b, sizeof(b), "(最大差 %.4f / 平均 %.4f)", maxStep, typical);
        check("遅延を動かしても段差が出ない", maxStep < typical * 6.0, b);
    }

    // null / 0本で落ちない。
    conv.setTaps(nullptr, 0);
    conv.processAdd(nullptr, 0, 16, false, l.data(), r.data(), 0, 1.0f);
    check("null / 0本で落ちない", true);
}

// ---------------------------------------------------------------- 信号フロー統合
// 部品は個別に検証済みなので、ここで見るのは**配線**:
//   ・直接音／早期反射／散乱／尾 が段別の計測に正しく分かれて出る
//   ・尾を切ると尾だけが消える（他段に混ざっていない）
//   ・HRTF を入れると直接音が両耳化される
//   ・分割して呼んでも一括で呼んでも同じ波形（ブロック境界に依存しない）
// 「壁を通した瞬間にドラムが削れて低音が壊れる」を数字にする。
//   タップの6帯域ゲインが**急峻に低域寄り**（＝壁の透過）のとき、
//   出てくる波形の周波数特性が本当にその形になっているかを測る。
//   さらに、透過タップ＋回折タップ（平坦・別の遅延）を同時に鳴らしたときの干渉も見る。
void diagnoseBandResponse() {
    std::printf("\n[診断] 帯域ゲインどおりの周波数特性が出ているか\n");
    const int sr = 48000;
    const int n = 16384;

    // インパルス応答から特定周波数の振幅を出す（素朴な DFT）。
    auto magAt = [&](const std::vector<float>& h, double hz) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * 3.14159265358979 * hz / sr;
        for (int i = 0; i < n; ++i) {
            const double v = h[static_cast<std::size_t>(i)];
            re += v * std::cos(w * i);
            im -= v * std::sin(w * i);
        }
        return std::sqrt(re * re + im * im);
    };
    auto db = [](double a) { return (a > 1e-12) ? 20.0 * std::log10(a) : -240.0; };

    // 実測に使う周波数。低域は細かく見る（壊れているのはそこ、という訴えなので）。
    static const double hz[] = {60, 90, 125, 180, 250, 355, 500, 707, 1000, 1414, 2000, 2828, 4000};
    const int nh = static_cast<int>(sizeof(hz) / sizeof(hz[0]));

    auto run = [&](const char* label, const af::dsp::EarlyReflectConv::Tap* taps, int nt,
                   const float* want6) {
        af::dsp::EarlyReflectConv conv(sr, 4096, 0.02f);
        conv.setTaps(taps, nt);
        std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(static_cast<std::size_t>(n), 0.0f);
        in[8] = 1.0f;
        conv.processAdd(in.data(), 0, n, false, l.data(), r.data(), 0, 1.0f);
        std::printf("      %s\n        Hz  ", label);
        for (int i = 0; i < nh; ++i) std::printf("%6.0f", hz[i]);
        std::printf("\n        dB  ");
        double worst = 0.0; double worstHz = 0.0;
        for (int i = 0; i < nh; ++i) {
            const double got = db(magAt(l, hz[i]) / 0.70710678);   // パン中央ぶんを戻す
            std::printf("%6.1f", got);
            if (want6) {
                // 6帯域の中心（125..4k）だけ、狙いの値と比べる。
                static const double ctr[6] = {125, 250, 500, 1000, 2000, 4000};
                for (int b = 0; b < 6; ++b)
                    if (std::fabs(hz[i] - ctr[b]) < 1.0) {
                        const double aim = db(want6[b]);
                        if (std::fabs(got - aim) > worst) { worst = std::fabs(got - aim); worstHz = hz[i]; }
                    }
            }
        }
        std::printf("\n");
        if (want6) {
            std::printf("        狙い");
            for (int i = 0; i < nh; ++i) {
                static const double ctr[6] = {125, 250, 500, 1000, 2000, 4000};
                bool hit = false;
                for (int b = 0; b < 6 && !hit; ++b)
                    if (std::fabs(hz[i] - ctr[b]) < 1.0) { std::printf("%6.1f", db(want6[b])); hit = true; }
                if (!hit) std::printf("     -");
            }
            std::printf("\n        → 帯域中心での最大ずれ %.1f dB @ %.0fHz\n", worst, worstHz);
        }
    };

    // ① 壁の透過カーブ（コンクリの 6 帯域透過に近い形）。低域ほど通る。
    const float wall[6] = {0.180f, 0.126f, 0.089f, 0.063f, 0.050f, 0.045f};
    {
        af::dsp::EarlyReflectConv::Tap t;
        for (int b = 0; b < 6; ++b) t.g[b] = wall[b];
        t.panL = t.panR = 0.70710678f;
        run("① 壁の透過だけ（遅延0）", &t, 1, wall);
    }
    // ② もっと急峻な壁（高域が 30dB 落ちる）。急峻さで崩れないか。
    const float steep[6] = {0.300f, 0.150f, 0.070f, 0.030f, 0.014f, 0.009f};
    {
        af::dsp::EarlyReflectConv::Tap t;
        for (int b = 0; b < 6; ++b) t.g[b] = steep[b];
        t.panL = t.panR = 0.70710678f;
        run("② 急峻な壁（125Hz→4kHz で 30dB 落ち）", &t, 1, steep);
    }
    // ③ 壁の透過 ＋ 回折タップ（平坦・別の遅延）。実機で同時に鳴っている状態。
    //    経路長の差 0.5m ≒ 1.46ms ≒ 70 サンプル。
    {
        af::dsp::EarlyReflectConv::Tap t[2];
        for (int b = 0; b < 6; ++b) { t[0].g[b] = wall[b]; t[1].g[b] = 0.25f; }
        t[0].delaySamples = 0;  t[1].delaySamples = 70;
        t[0].panL = t[0].panR = 0.70710678f;
        t[1].panL = t[1].panR = 0.70710678f;
        run("③ 壁の透過 ＋ 回折(平坦, 1.5ms 遅れ)", t, 2, nullptr);
    }
    // ④ ③ の回折を +6dB（今回 diffractionGainDb の既定にした量）。
    {
        af::dsp::EarlyReflectConv::Tap t[2];
        for (int b = 0; b < 6; ++b) { t[0].g[b] = wall[b]; t[1].g[b] = 0.25f * 1.995f; }
        t[0].delaySamples = 0;  t[1].delaySamples = 70;
        t[0].panL = t[0].panR = 0.70710678f;
        t[1].panL = t[1].panR = 0.70710678f;
        run("④ ③ の回折を +6dB", t, 2, nullptr);
    }
    // ⑤ 回折を**開口の広がり**として複数点から鳴らす（apertureSpread）。
    //    現実の戸口は面なので、面の各所からわずかに違う経路長で届く。
    //    1点だと同相で足されて深い櫛形になる。散らせば埋まるはず。
    //    戸口 1.2m を 3 点：経路差 ±0.25m ≒ ±35 サンプル。
    {
        af::dsp::EarlyReflectConv::Tap t[4];
        for (int b = 0; b < 6; ++b) t[0].g[b] = wall[b];
        t[0].delaySamples = 0; t[0].panL = t[0].panR = 0.70710678f;
        const int dly[3] = {70 - 35, 70, 70 + 35};
        for (int k = 0; k < 3; ++k) {
            for (int b = 0; b < 6; ++b) t[1 + k].g[b] = 0.25f / 3.0f;
            t[1 + k].delaySamples = dly[k];
            t[1 + k].panL = t[1 + k].panR = 0.70710678f;
        }
        run("⑤ ③ の回折を開口の3点へ散らす（apertureSpread=3 相当）", t, 4, nullptr);
    }
    // ⑥ 5点に増やす。
    {
        af::dsp::EarlyReflectConv::Tap t[6];
        for (int b = 0; b < 6; ++b) t[0].g[b] = wall[b];
        t[0].delaySamples = 0; t[0].panL = t[0].panR = 0.70710678f;
        const int dly[5] = {70 - 40, 70 - 20, 70, 70 + 20, 70 + 40};
        for (int k = 0; k < 5; ++k) {
            for (int b = 0; b < 6; ++b) t[1 + k].g[b] = 0.25f / 5.0f;
            t[1 + k].delaySamples = dly[k];
            t[1 + k].panL = t[1 + k].panR = 0.70710678f;
        }
        run("⑥ 5点へ散らす（apertureSpread=5 相当）", t, 6, nullptr);
    }
}

// 尾の絶対レベルは tailGain = 直接 × √target で決めている。つまり
// **鳴らした結果の 尾/直接 が √target に一致する**のが設計の約束。
// Unity の画面では 尾/直接 = 0.92 に対し 目標 4.09（13dB 未達）と出ていたので、
// 鎖の端から端まで通して測り、どこでずれるのかを数字にする。
// オーディオスレッド側のコスト。シーン側（ゲームスレッド）は scene_regression の
// [1フレームの音響計算] で測っているが、畳み込み・HRTF・尾はオーディオスレッドで回るので
// そちらは別に測らないと全体像にならない。
//   基準: 48kHz・512 フレームのバッファ 1 個 = 10.67ms ぶんの音。
//   render() がその何割で済むかが「1 音源あたりのオーディオ負荷」。
// 尾を「音源ごとに畳む」のと「まとめて 1 回畳む」のが一致するか。
//   エンジンのエコグラムは全音源まとめて 1 本しか作らない（computeEchogramBands は
//   全音源を同じ配列へ積む）。尾のノイズも既定の種 12345 で全 VoiceRenderer 共通。
//   つまり**今でも全音源が同一の尾 IR を畳んでいて**、違うのは音量(tailGain)だけ。
//   畳み込みは線形なので Σ(gi·xi) * h == Σ(gi·(xi * h)) のはずだが、
//   分割畳み込みはブロック処理・FFT なので、実物で確かめないと言い切れない。
// 尾の IR を差し替えた瞬間に、出力がどれだけ飛ぶか（不具合 #3）。
//   分割畳み込みは過去の入力ブロックを周波数領域遅延線(FDL)に保持しているので、
//   IR を即差し替えると「もう出ている尾」が別の IR で畳み直されて段差が出る。
//   2026-09-08: FDL は共有のまま IR スペクトルだけ 2 世代混ぜる形にした。
//   ここでクロスフェードの長さを振り、既定の長さで人工物が 1 dB 以下に収まることを見る。
void testTailSwapContinuity() {
    std::printf("\n[尾] IR を差し替えた瞬間の人工物（クロスフェードの長さを振る）\n");
    const int sr = 48000;
    const int block = 512;
    af::dsp::VoiceRenderer::Config cfg;
    cfg.sampleRate = sr;
    cfg.maxFrames = block;
    cfg.tailSeconds = 1.0f;
    cfg.tapCrossfadeMs = 30.0f;

    // 響く部屋 A → 吸う部屋 B（実測の 2 部屋に近い減衰差をつける）
    const int bins = 100;
    auto makeEcho = [&](float decay) {
        std::vector<float> e(static_cast<std::size_t>(bins) * 6, 0.0f);
        for (int k = 0; k < bins; ++k)
            for (int b = 0; b < 6; ++b)
                e[static_cast<std::size_t>(k) * 6 + b] = std::pow(decay, static_cast<float>(k));
        return e;
    };
    const std::vector<float> echoA = makeEcho(0.97f);   // 長い尾
    const std::vector<float> echoB = makeEcho(0.85f);   // 短い尾

    // ★切り分けの要。差し替え後に動いた量が「人工物」なのか「移った先の部屋の正しい値」
    //   なのかは、B 単独で暖機した定常値と比べないと言えない。尾 IR はエネルギー1に
    //   正規化されるので、短い尾ほど同じエネルギーが短時間に集まり定常RMSは上がる。
    auto steadyRmsOf = [&](const std::vector<float>& echo) {
        af::dsp::VoiceRenderer v(cfg);
        v.setOutputGain(1.0f);
        v.setHrtfEnabled(false);
        v.setTailLevel(1.0f);
        v.setTailEnvelope(1.0f, 1.0f);
        af::dsp::EarlyReflectConv::Tap t;
        for (int b = 0; b < 6; ++b) t.g[b] = 0.0f;
        t.delaySamples = 0; t.gSpec = 1.0f; t.gDiff = 0.0f;
        v.setTaps(&t, 1);
        for (int it = 0; it < 16; ++it)
            v.rebuildTail(echo.data(), bins, 10.0f, 20.0f, 8.0f, 30.0f, 0.0f, 1.0f,
                          1.0f, 4.0f, nullptr, 0);
        Rng r;
        std::vector<float> in(static_cast<std::size_t>(block));
        std::vector<float> a(static_cast<std::size_t>(block)), b2(static_cast<std::size_t>(block));
        double acc = 0.0;
        for (int blk = 0; blk < 240; ++blk) {
            for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = r.next() * 0.3f;
            v.render(in.data(), block, a.data(), b2.data(), nullptr);
            if (blk >= 232) {
                double s = 0.0;
                for (int i = 0; i < block; ++i)
                    s += a[static_cast<std::size_t>(i)] * a[static_cast<std::size_t>(i)];
                acc += std::sqrt(s / block) / 8.0;
            }
        }
        return acc;
    };
    const double steadyA = steadyRmsOf(echoA);
    const double steadyB = steadyRmsOf(echoB);
    std::printf("        参考: 単独で暖機したときの定常RMS  響く部屋A %.5f / 吸う部屋B %.5f"
                "（差 %.2f dB）\n",
                steadyA, steadyB, 20.0 * std::log10(steadyB / steadyA));

    // 差し替え後は 1 ブロックだけ見ても足りない（実際 +1blk は丸ごと旧 IR のままで
    // 両者が一致する）。段ごとに取り込みのタイミングが違うので、尾の長さぶん追う。
    // 基準は「移った先の部屋を単独で暖機した定常値」。そこから外れたぶんが人工物。
    const int kAfter = 94;   // 1.0s ぶん ≒ 尾の全長
    std::printf("        場面               混ぜms  比(尾/直接)   基準RMS  ピークdB  到達blk  収束blk  跳び/定常\n");

    // ★1 ブロック(512サンプル)の RMS 推定は誤差 ±0.27dB あり、94 ブロック中の最大を
    //   取ると偶然だけで 0.8dB に届く（対照群で実測した）。種を変えて平均し、
    //   推定誤差を √kTrials ぶん落とさないと 2dB 級の人工物と区別が付かない。
    const int kTrials = 8;
    struct C { const char* name; bool change; };
    const C cases[] = { {"同じ内容で差し替え", false},
                        {"響く部屋→吸う部屋", true} };
    // ★クロスフェードの長さを振る。0 = 即差し替え（2026-09-08 まではこれが既定）。
    //   器は 1 つのままで、中で新旧の IR スペクトルを混ぜる（遅延線は共有＝新側が痩せない）。
    const float kXfades[] = { 0.0f, 20.0f, 50.0f, 100.0f };
    const float kDefaultXfadeMs = 50.0f;   // VoiceRenderer::Config の既定と合わせること
    double worstAt[2] = { 0.0, 0.0 };
    double capWorst = 0.0, capWorst2 = 0.0;
    for (float xf : kXfades) {
    cfg.tailCrossfadeMs = xf;
    for (const C& c : cases) {
        std::vector<double> msAfter(static_cast<std::size_t>(kAfter), 0.0);
        double stepRatio = 0.0;
        float ratioBefore = 0.0f, ratioAfter = 0.0f;

        for (int trial = 0; trial < kTrials; ++trial) {
            af::dsp::VoiceRenderer voice(cfg);
            voice.setOutputGain(1.0f);
            voice.setHrtfEnabled(false);
            voice.setTailLevel(1.0f);
            voice.setTailEnvelope(1.0f, 1.0f);
            // 直接音を切って尾だけ見る（段差を埋もれさせない）。
            af::dsp::EarlyReflectConv::Tap tap;
            for (int b = 0; b < 6; ++b) tap.g[b] = 0.0f;
            tap.delaySamples = 0; tap.gSpec = 1.0f; tap.gDiff = 0.0f;
            voice.setTaps(&tap, 1);
            for (int it = 0; it < 16; ++it)
                ratioBefore = voice.rebuildTail(echoA.data(), bins, 10.0f, 20.0f, 8.0f, 30.0f,
                                                0.0f, 0.6f, 1.0f, 4.0f, nullptr, 0);

            Rng rng;
            rng.s = 12345u + static_cast<unsigned int>(trial) * 7919u;
            std::vector<float> in(static_cast<std::size_t>(block));
            std::vector<float> ol(static_cast<std::size_t>(block)), orr(static_cast<std::size_t>(block));

            // ★暖機は tailSeconds を十分に超えるまで回す。尾の長さ(1.0s = 94ブロック)に
            //   届かないうちに測ると、対照群まで 0.5dB 動いて段差と区別が付かない。
            const int kWarm = 240, kSteadyFrom = 232;   // 240blk = 2.56s
            double stepSteady = 0.0, prev = 0.0;
            for (int blk = 0; blk < kWarm; ++blk) {
                for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
                voice.render(in.data(), block, ol.data(), orr.data(), nullptr);
                if (blk >= kSteadyFrom) {
                    for (int i = 0; i < block; ++i) {
                        const double v = ol[static_cast<std::size_t>(i)];
                        stepSteady = std::max(stepSteady, std::fabs(v - prev));
                        prev = v;
                    }
                } else {
                    prev = ol[static_cast<std::size_t>(block) - 1];
                }
            }

            // ここで差し替え（envAlpha=1 で即座に切り替わる）。
            const std::vector<float>& next = c.change ? echoB : echoA;
            ratioAfter = voice.rebuildTail(next.data(), bins, 10.0f, 20.0f, 8.0f, 30.0f,
                                           0.0f, 1.0f, 1.0f, 4.0f, nullptr, 0);

            double stepAfter = 0.0;
            for (int k = 0; k < kAfter; ++k) {
                for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
                voice.render(in.data(), block, ol.data(), orr.data(), nullptr);
                double s = 0.0;
                for (int i = 0; i < block; ++i) {
                    const double v = ol[static_cast<std::size_t>(i)];
                    s += v * v;
                    stepAfter = std::max(stepAfter, std::fabs(v - prev));
                    prev = v;
                }
                msAfter[static_cast<std::size_t>(k)] += (s / block) / kTrials;
            }
            stepRatio += (stepAfter / std::max(stepSteady, 1e-12)) / kTrials;
        }

        // 基準は「移った先の部屋を単独で暖機した定常値」。差し替えが理想的なら
        // ここから動かないはずで、動いたぶんがそのまま人工物になる。
        const double refRms = c.change ? steadyB : steadyA;
        double worstDb = 0.0;
        int peakBlk = 0;
        std::vector<double> dbs(static_cast<std::size_t>(kAfter), 0.0);
        for (int k = 0; k < kAfter; ++k) {
            const double r = std::sqrt(msAfter[static_cast<std::size_t>(k)]);
            const double db = 20.0 * std::log10(std::max(r, 1e-12) / std::max(refRms, 1e-12));
            dbs[static_cast<std::size_t>(k)] = db;
            if (std::fabs(db) > std::fabs(worstDb)) { worstDb = db; peakBlk = k + 1; }
        }
        // 収束＝以降ずっと ±0.5dB に収まる最初のブロック。
        int settleBlk = kAfter + 1;
        for (int k = kAfter - 1; k >= 0; --k) {
            if (std::fabs(dbs[static_cast<std::size_t>(k)]) >= 0.5) break;
            settleBlk = k + 1;
        }

        std::printf("        %-18s %5.0f %7.2f→%-7.2f %8.5f %9.2f %8d %8d %10.2f\n",
                    c.name, xf, ratioBefore, ratioAfter, refRms, worstDb, peakBlk,
                    settleBlk, stepRatio);
        if (xf == kDefaultXfadeMs && cfg.tailCapBlock == 8192) worstAt[c.change ? 1 : 0] = worstDb;
        if (xf == kDefaultXfadeMs && cfg.tailCapBlock != 8192 && c.change) capWorst = worstDb;
    }
    }
    // ★切り分け: 部屋を移るときの膨らみはクロスフェードでは動かない（0→100ms で 1.97→1.86）。
    //   非一様分割は段ごとにブロック境界が違うので、**IR の後ろの方を担う段ほど取り込みが遅い**
    //   （最大ブロック 8192 = 170ms）。その間だけ「頭は新しい部屋・尾の後ろは古い部屋」になり、
    //   どちらもエネルギー 1 に正規化されているので合計が増える。段差ではなく**取り込みの遅れ**。
    //   最大ブロックを小さくすると減るはず ── ここで確かめる（費用は分割数が増えるぶん上がる）。
    cfg.tailCapBlock = 1024;
    cfg.tailCrossfadeMs = kDefaultXfadeMs;
    for (const C& c : cases) {
        if (!c.change) continue;
        // 上と同じ手順を最大ブロック 1024 で 1 ケースだけ。
        double ms2[94] = {};
        for (int trial = 0; trial < kTrials; ++trial) {
            af::dsp::VoiceRenderer voice(cfg);
            voice.setOutputGain(1.0f); voice.setHrtfEnabled(false);
            voice.setTailLevel(1.0f); voice.setTailEnvelope(1.0f, 1.0f);
            af::dsp::EarlyReflectConv::Tap tap;
            for (int b = 0; b < 6; ++b) tap.g[b] = 0.0f;
            tap.delaySamples = 0; tap.gSpec = 1.0f; tap.gDiff = 0.0f;
            voice.setTaps(&tap, 1);
            for (int it = 0; it < 16; ++it)
                voice.rebuildTail(echoA.data(), bins, 10.0f, 20.0f, 8.0f, 30.0f,
                                  0.0f, 0.6f, 1.0f, 4.0f, nullptr, 0);
            Rng rng; rng.s = 12345u + static_cast<unsigned int>(trial) * 7919u;
            std::vector<float> in(static_cast<std::size_t>(block));
            std::vector<float> ol(static_cast<std::size_t>(block)), orr(static_cast<std::size_t>(block));
            for (int blk = 0; blk < 240; ++blk) {
                for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
                voice.render(in.data(), block, ol.data(), orr.data(), nullptr);
            }
            voice.rebuildTail(echoB.data(), bins, 10.0f, 20.0f, 8.0f, 30.0f,
                              0.0f, 1.0f, 1.0f, 4.0f, nullptr, 0);
            for (int k = 0; k < 94; ++k) {
                for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
                voice.render(in.data(), block, ol.data(), orr.data(), nullptr);
                double s = 0.0;
                for (int i = 0; i < block; ++i) {
                    const double v = ol[static_cast<std::size_t>(i)];
                    s += v * v;
                }
                ms2[k] += (s / block) / kTrials;
            }
        }
        for (int k = 0; k < 94; ++k) {
            const double r = std::sqrt(ms2[k]);
            const double db = 20.0 * std::log10(std::max(r, 1e-12) / std::max(steadyB, 1e-12));
            if (std::fabs(db) > std::fabs(capWorst2)) capWorst2 = db;
        }
        std::printf("        %-18s %5.0f （最大ブロック 1024）                  %9.2f\n",
                    c.name, kDefaultXfadeMs, capWorst2);
    }
    (void)capWorst;
    char nb[200];
    std::snprintf(nb, sizeof(nb), "(混ぜ %.0f ms: 同じ内容 %+.2f dB / 部屋を移る %+.2f dB / 最大ブロック 1024 で %+.2f dB)",
                  kDefaultXfadeMs, worstAt[0], worstAt[1], capWorst2);
    check("[尾] 同じ内容で差し替えたときの人工物が 0.5 dB 以下", std::fabs(worstAt[0]) <= 0.5, nb);
    // 部屋を移るときの膨らみは段の取り込み遅れ（上の切り分け）。段差ではないので、
    // ここは「悪化していないこと」を見る。最大ブロックを縮めれば下がることも合わせて見る。
    check("[尾] 部屋を移るときの膨らみが 2.5 dB 以下", std::fabs(worstAt[1]) <= 2.5, nb);
    check("[尾] 膨らみの正体は段の取り込み遅れ（最大ブロックを縮めると減る）",
          std::fabs(capWorst2) < std::fabs(worstAt[1]), nb);
    std::printf("      ※比(尾/直接) が動いていなければ、差し替えそのものが効いていない。\n"
                "        跳び/定常 が 1.0 前後ならクリックは出ていない。\n");
}

// 【出荷側で効く検査】ホストは数フレームに 1 回 IR を組み直す（部屋が変わらなくても）。
//   だから「差し替えのたびに何かが起きる」なら、それは**常時鳴り続ける人工物**になる。
//   ★2026-09-08 まで、共有バスは器を 2 つ持って交互に使っていた（ホスト既定 60ms）。
//     休んでいる側の周波数領域遅延線は止まったままなので、入れ替えた瞬間に
//     **古い入力を持った状態**で鳴り出す。同じ IR を入れ直しただけで音が変わっていた。
//   いまは遅延線を共有して IR スペクトルだけ混ぜるので、同じ IR なら
//   混ぜても w·y + (1-w)·y = y ── 入れ直さない対照と**一致するはず**。そこを見る。
void testTailRebuildLevelStability() {
    std::printf("\n[尾] 同じ IR を入れ直し続けても尾が動かない（ホストは 8 フレームに 1 回組み直す）\n");
    const int sr = 48000, block = 512, irLen = 24000;
    Rng irRng;
    std::vector<float> hL(static_cast<std::size_t>(irLen)), hR(static_cast<std::size_t>(irLen));
    for (int i = 0; i < irLen; ++i) {
        const float env = std::pow(0.9997f, static_cast<float>(i));
        hL[static_cast<std::size_t>(i)] = irRng.next() * env * 0.01f;
        hR[static_cast<std::size_t>(i)] = irRng.next() * env * 0.01f;
    }
    const float* ir[2] = { hL.data(), hR.data() };
    const int len[2] = { irLen, irLen };

    struct Case { const char* name; float xfadeMs; };
    const Case cases[] = { {"即差し替え(0ms)", 0.0f}, {"混ぜ 60ms（ホスト既定）", 60.0f} };
    for (const Case& cs : cases) {
        af::dsp::TailBus a(irLen, 64, 8192, block, sr);   // 入れ直す側
        af::dsp::TailBus b(irLen, 64, 8192, block, sr);   // 対照（入れ直さない）
        a.setCrossfadeMs(cs.xfadeMs);
        b.setCrossfadeMs(cs.xfadeMs);
        a.setIr(ir, len);
        b.setIr(ir, len);
        Rng rng;
        std::vector<float> in(static_cast<std::size_t>(block));
        std::vector<float> aL(static_cast<std::size_t>(block)), aR(static_cast<std::size_t>(block));
        std::vector<float> bL(static_cast<std::size_t>(block)), bR(static_cast<std::size_t>(block));
        double worst = 0.0, refSq = 0.0; int nRef = 0;
        for (int blk = 0; blk < 200; ++blk) {
            if (blk >= 80 && blk % 8 == 0) a.setIr(ir, len);   // ホストと同じ周期で入れ直す
            for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
            std::fill(aL.begin(), aL.end(), 0.0f); std::fill(aR.begin(), aR.end(), 0.0f);
            std::fill(bL.begin(), bL.end(), 0.0f); std::fill(bR.begin(), bR.end(), 0.0f);
            a.add(in.data(), block, 1.0f); a.render(block, aL.data(), aR.data());
            b.add(in.data(), block, 1.0f); b.render(block, bL.data(), bR.data());
            if (blk < 80) continue;
            for (int i = 0; i < block; ++i) {
                const double d = std::fabs(static_cast<double>(aL[static_cast<std::size_t>(i)])
                                         - static_cast<double>(bL[static_cast<std::size_t>(i)]));
                if (d > worst) worst = d;
                refSq += static_cast<double>(bL[static_cast<std::size_t>(i)])
                       * static_cast<double>(bL[static_cast<std::size_t>(i)]);
                ++nRef;
            }
        }
        const double refRms = std::sqrt(refSq / std::max(1, nRef));
        const double db = 20.0 * std::log10(std::max(worst, 1e-12) / std::max(refRms, 1e-12));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "(対照との最大差 %.3e ／ 尾の RMS %.5f ＝ %+.1f dB)",
                      worst, refRms, db);
        char label[128];
        std::snprintf(label, sizeof(label), "[尾] 入れ直しが対照と一致（%s）", cs.name);
        check(label, db < -60.0, buf);
    }
}

void testSharedTailBusEquivalence() {
    std::printf("\n[尾] 音源ごとに畳む vs まとめて 1 回畳む\n");
    const int irLen = 24000;          // 0.5s @48k
    const int block = 512;
    const int nSrc = 6;
    const int nFrames = block * 24;

    // 共有の尾 IR（減衰ノイズ、2ch）。
    Rng irRng;
    std::vector<float> hL(static_cast<std::size_t>(irLen)), hR(static_cast<std::size_t>(irLen));
    for (int i = 0; i < irLen; ++i) {
        const float env = std::pow(0.9997f, static_cast<float>(i));
        hL[static_cast<std::size_t>(i)] = irRng.next() * env * 0.01f;
        hR[static_cast<std::size_t>(i)] = irRng.next() * env * 0.01f;
    }
    const float* irPtr[2] = { hL.data(), hR.data() };
    const int irLens[2] = { irLen, irLen };

    // 音源ごとの信号と送出量。
    Rng sRng;
    std::vector<std::vector<float>> x(static_cast<std::size_t>(nSrc));
    std::vector<float> gain(static_cast<std::size_t>(nSrc));
    for (int s = 0; s < nSrc; ++s) {
        x[static_cast<std::size_t>(s)].resize(static_cast<std::size_t>(nFrames));
        for (int i = 0; i < nFrames; ++i)
            x[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)] = sRng.next() * 0.3f;
        gain[static_cast<std::size_t>(s)] = 0.2f + 0.15f * static_cast<float>(s);
    }

    // A) 音源ごとに 1 本ずつ畳んで足す（今の作り）。
    std::vector<float> aL(static_cast<std::size_t>(nFrames), 0.0f), aR(static_cast<std::size_t>(nFrames), 0.0f);
    {
        std::vector<std::unique_ptr<af::dsp::NonUniformConvolver>> convs;
        for (int s = 0; s < nSrc; ++s) {
            convs.emplace_back(new af::dsp::NonUniformConvolver(irLen, 2, 64, 8192, block));
            convs.back()->setIr(irPtr, irLens);
        }
        for (int off = 0; off < nFrames; off += block) {
            float* dst[2] = { aL.data(), aR.data() };
            for (int s = 0; s < nSrc; ++s)
                convs[static_cast<std::size_t>(s)]->processAdd(
                    x[static_cast<std::size_t>(s)].data(), off, block, dst, off,
                    gain[static_cast<std::size_t>(s)]);
        }
    }

    // B) 送出量を掛けて足してから、1 本で畳む（共有バス）。
    std::vector<float> bL(static_cast<std::size_t>(nFrames), 0.0f), bR(static_cast<std::size_t>(nFrames), 0.0f);
    {
        std::vector<float> mix(static_cast<std::size_t>(nFrames), 0.0f);
        for (int s = 0; s < nSrc; ++s)
            for (int i = 0; i < nFrames; ++i)
                mix[static_cast<std::size_t>(i)] +=
                    x[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)]
                    * gain[static_cast<std::size_t>(s)];
        af::dsp::NonUniformConvolver conv(irLen, 2, 64, 8192, block);
        conv.setIr(irPtr, irLens);
        for (int off = 0; off < nFrames; off += block) {
            float* dst[2] = { bL.data(), bR.data() };
            conv.processAdd(mix.data(), off, block, dst, off, 1.0f);
        }
    }

    // 突き合わせ。
    double maxAbs = 0.0, sumA = 0.0, sumB = 0.0;
    for (int i = 0; i < nFrames; ++i) {
        maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(
            aL[static_cast<std::size_t>(i)] - bL[static_cast<std::size_t>(i)])));
        maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(
            aR[static_cast<std::size_t>(i)] - bR[static_cast<std::size_t>(i)])));
        sumA += static_cast<double>(aL[static_cast<std::size_t>(i)]) * aL[static_cast<std::size_t>(i)];
        sumB += static_cast<double>(bL[static_cast<std::size_t>(i)]) * bL[static_cast<std::size_t>(i)];
    }
    const double rmsA = std::sqrt(sumA / nFrames), rmsB = std::sqrt(sumB / nFrames);
    const double rel = (rmsA > 1e-12) ? maxAbs / rmsA : 0.0;
    std::printf("        音源 %d 本 / %d フレーム\n", nSrc, nFrames);
    std::printf("        個別に畳む RMS %.6f / まとめて畳む RMS %.6f\n", rmsA, rmsB);
    std::printf("        最大の差 %.3e（RMS 比 %.2e ＝ %.1f dB 下）\n",
                maxAbs, rel, 20.0 * std::log10(std::max(rel, 1e-12)));
    check("[尾] 共有バスは音源ごとの畳み込みと一致する（-100dB 以下）", rel < 1e-5);
}

void diagnoseDspCost() {
    std::printf("\n[診断] オーディオスレッドのコスト（1 音源あたり）\n");
    const int sr = 48000;
    const int block = 512;
    const double blockMs = 1000.0 * block / sr;

    struct C { const char* name; bool hrtf; bool tail; int taps; bool difHrtf; bool ear; };
    const C cases[] = {
        {"直接音のみ                ", false, false, 1, false, false},
        {"＋早期反射 8 タップ       ", false, false, 8, false, false},
        {"＋HRTF                    ", true,  false, 8, false, false},
        {"＋後期尾 1.0s（全部入り） ", true,  true,  8, false, false},
        // B1。遮蔽されている間だけ回る（見通せていれば回折タップが無いので 1 段上と同じ）。
        {"＋回折の HRTF（遮蔽時）   ", true,  true,  8, true,  false},
        // 反射タップの軽量な両耳化。畳み込みは増えず、リングをもう一度読むぶんだけ。
        {"＋反射の両耳化（ITD＋ILD）", true,  true,  8, true,  true },
    };
    std::printf("        構成                        1ブロック   実時間比   1音源の負荷\n");
    for (const C& c : cases) {
        af::dsp::VoiceRenderer::Config cfg;
        cfg.sampleRate = sr;
        cfg.maxFrames = block;
        cfg.tailSeconds = 1.0f;
        cfg.tapCrossfadeMs = 30.0f;
        af::dsp::VoiceRenderer voice(cfg);
        voice.setOutputGain(1.0f);

        af::dsp::HrtfSet syn = af::dsp::HrtfSet::createSynthetic(sr, 5, 10);
        if (c.hrtf) { voice.setHrtfSet(&syn); voice.setHrtfEnabled(true); }
        else voice.setHrtfEnabled(false);
        voice.setEarCuesEnabled(c.ear);
        const float dirFwd[3] = {0.3f, 0.0f, 1.0f};
        voice.setDirection(dirFwd, 57.0f);

        std::vector<af::dsp::EarlyReflectConv::Tap> taps(static_cast<std::size_t>(c.taps));
        for (int i = 0; i < c.taps; ++i) {
            for (int b = 0; b < 6; ++b) taps[static_cast<std::size_t>(i)].g[b] = (i == 0) ? 1.0f : 0.35f;
            taps[static_cast<std::size_t>(i)].delaySamples = i * 190;
            taps[static_cast<std::size_t>(i)].gSpec = 1.0f;
            taps[static_cast<std::size_t>(i)].gDiff = 0.0f;
        }
        if (c.difHrtf && c.taps > 1) {
            if (c.ear) for (int i = 1; i < c.taps; ++i) {
                const float az = -60.0f + 120.0f * (float)i / (float)std::max(1, c.taps - 1);
                af::dsp::HrtfSet::angleToVector(az, 0.0f, taps[(std::size_t)i].dir);
                taps[(std::size_t)i].dirValid = true;
            }
            taps[1].hrtfWeight = 1.0f;           // 回折タップ 1 本を HRTF に載せる
            const float dirAp[3] = {1.0f, 0.0f, 0.2f};
            voice.setDiffractionDirection(dirAp, 57.0f);
        }
        voice.setTaps(taps.data(), c.taps);

        if (c.tail) {
            const int bins = 100;
            std::vector<float> echo(static_cast<std::size_t>(bins) * 6, 0.0f);
            for (int k = 0; k < bins; ++k)
                for (int b = 0; b < 6; ++b)
                    echo[static_cast<std::size_t>(k) * 6 + b] = std::pow(0.95f, static_cast<float>(k));
            for (int it = 0; it < 12; ++it)
                voice.rebuildTail(echo.data(), bins, 10.0f, 20.0f, 8.0f, 30.0f, 0.0f, 0.6f,
                                  1.0f, 4.0f, nullptr, 0);
        }

        Rng rng;
        std::vector<float> in(static_cast<std::size_t>(block));
        std::vector<float> ol(static_cast<std::size_t>(block)), orr(static_cast<std::size_t>(block));
        for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;

        const int warm = 20, iters = 400;
        for (int i = 0; i < warm; ++i) voice.render(in.data(), block, ol.data(), orr.data(), nullptr);
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iters; ++i) voice.render(in.data(), block, ol.data(), orr.data(), nullptr);
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
        std::printf("        %s  %6.3f ms   %6.1f 倍   %5.2f %%\n",
                    c.name, ms, blockMs / ms, ms / blockMs * 100.0);
    }
    std::printf("      ※48kHz・512フレーム = 1 ブロック 10.67ms ぶんの音。\n"
                "        「1音源の負荷」はオーディオスレッド 1 本に対する割合。\n"
                "        GPU は engine 側で一切使っていない（GPU化は後回しと決めた通り）。\n");
}

// 【B1】回折タップの HRTF。
//
//   遮蔽されると直接音タップは材質の透過まで落ちる（コンクリなら 1e-4 級）。
//   HRTF が掛かっているのがその直接音だけだと、**実際に聞こえている音のほうに
//   定位の手がかりが無い**という状態になる。実エネルギーを運ぶのは開口を
//   回り込んだ回折タップで、そちらは L/R バランスだけ ── ITD なし・前後の区別なし。
//
//   「回折が運ぶのは開口の方向」「音の方へ進むと穴に着く」はコンセプトの中心なので、
//   ここが立たないと壁の向こう・柱の陰という中心動作が成立しない。
//
//   ★却下済みの「ソフト遮蔽に回折の床を足す」とは別物。あれはエネルギーを足して
//     二重計上になった話で、こちらは**同じエネルギーの空間化を変えるだけ**。
//     エネルギーが動いていないことは下の「総量」で測る。
void testDiffractionHrtf() {
    std::printf("\n[B1] 回折タップの HRTF\n");
    const int block = 512;
    const int tapDelay = 240;

    // ★実測の KEMAR を使う。合成 HRTF は**球体頭モデル**で、方向依存は
    //   lateral = sin(az)·cos(el) しか無い ── つまり前(az=0)と後(az=180)が
    //   完全に同一の HRIR になる。前後の区別は耳介の形が作るので、
    //   合成セットでは原理的に測れない（実測: 前後差 -115dB ＝ 数値誤差だけ）。
    af::dsp::HrtfSet real;
    bool haveReal = false;
    const char* kPaths[] = {
        "UnityDemo/Assets/StreamingAssets/kemar.afhr",
        "../UnityDemo/Assets/StreamingAssets/kemar.afhr",
        "../../UnityDemo/Assets/StreamingAssets/kemar.afhr",
        "../../../UnityDemo/Assets/StreamingAssets/kemar.afhr",
    };
    for (const char* p : kPaths)
        if (af::dsp::HrtfSet::loadFromFile(p, real) && real.isValid()) { haveReal = true; break; }

    af::dsp::HrtfSet syn = af::dsp::HrtfSet::createSynthetic(48000, 5, 10);
    const af::dsp::HrtfSet& set = haveReal ? real : syn;
    const int sr = set.sampleRate();
    std::printf("        HRTF: %s（%d 方向 / %d タップ / %d Hz / 平均パワーゲイン %.2f"
                " = 等パワーパン比 %+.1f dB）\n",
                set.name().c_str(), set.directionCount(), set.irLength(), sr,
                static_cast<double>(set.meanPowerGain()),
                10.0 * std::log10(std::max(1e-9f, set.meanPowerGain())));

    // 遮蔽された場面を作る: 直接音は透過まで落ち、回折タップがエネルギーを運ぶ。
    //   dirDirect  : 音源の方向（壁の向こう）。前
    //   dirDiff    : 開口の方向。ここを振って測る
    //   hrtfWeight : 0=従来（等パワーパンだけ） / 1=B1（HRTF に載せる）
    struct Out { std::vector<float> l, r; };
    auto renderCase = [&](float azDiffDeg, float weight, float panL, float panR,
                          float directGain, int frames) {
        af::dsp::VoiceRenderer::Config cfg;
        cfg.sampleRate = sr;
        cfg.maxFrames = block;
        cfg.tailSeconds = 0.25f;
        cfg.tapCrossfadeMs = 30.0f;
        af::dsp::VoiceRenderer v(cfg);
        v.setOutputGain(1.0f);
        v.setHrtfSet(&set);
        v.setHrtfEnabled(true);

        const float dirDirect[3] = {0.0f, 0.0f, 1.0f};       // 音源は正面（壁の向こう）
        v.setDirection(dirDirect, 57.0f);
        float dirDiff[3];
        af::dsp::HrtfSet::angleToVector(azDiffDeg, 0.0f, dirDiff);
        v.setDiffractionDirection(dirDiff, 57.0f);

        af::dsp::EarlyReflectConv::Tap taps[2];
        for (int b = 0; b < 6; ++b) taps[0].g[b] = directGain;   // 直接音（透過ぶん）
        taps[0].delaySamples = 0;
        taps[0].gSpec = 1.0f; taps[0].gDiff = 0.0f;
        for (int b = 0; b < 6; ++b) taps[1].g[b] = 1.0f;         // 回折タップ
        taps[1].delaySamples = tapDelay;
        taps[1].gSpec = 1.0f; taps[1].gDiff = 0.0f;
        taps[1].panL = panL; taps[1].panR = panR;
        taps[1].hrtfWeight = weight;
        v.setTaps(taps, 2);

        // 暖機：タップ補間 30ms と HRTF の方向クロスフェード 12ms を無音で終わらせる。
        //   ここを飛ばすと過渡の途中を測ることになる。
        std::vector<float> zero(static_cast<std::size_t>(block), 0.0f);
        std::vector<float> tl(static_cast<std::size_t>(block)), tr(static_cast<std::size_t>(block));
        for (int i = 0; i < 8; ++i) v.render(zero.data(), block, tl.data(), tr.data(), nullptr);

        Out o;
        o.l.assign(static_cast<std::size_t>(frames), 0.0f);
        o.r.assign(static_cast<std::size_t>(frames), 0.0f);
        std::vector<float> in(static_cast<std::size_t>(frames), 0.0f);
        in[0] = 1.0f;                                            // インパルス
        v.render(in.data(), frames, o.l.data(), o.r.data(), nullptr);
        return o;
    };

    // L と R の相互相関がいちばん高くなるずれ（サンプル）。ITD の実測値。
    //   argmax|x| だと HRIR の山に引っ張られて量子化するので、相関で取る。
    auto itdSamples = [](const Out& o, int from, int to) {
        int bestLag = 0;
        double best = -1e18;
        for (int lag = -80; lag <= 80; ++lag) {
            double s = 0.0;
            for (int i = from; i < to; ++i) {
                const int j = i + lag;
                if (j < 0 || j >= static_cast<int>(o.r.size())) continue;
                s += static_cast<double>(o.l[static_cast<std::size_t>(i)])
                   * static_cast<double>(o.r[static_cast<std::size_t>(j)]);
            }
            if (s > best) { best = s; bestLag = lag; }
        }
        return bestLag;
    };
    auto rms2 = [](const Out& o, int from, int to) {
        double s = 0.0;
        for (int i = from; i < to; ++i)
            s += static_cast<double>(o.l[static_cast<std::size_t>(i)]) * o.l[static_cast<std::size_t>(i)]
               + static_cast<double>(o.r[static_cast<std::size_t>(i)]) * o.r[static_cast<std::size_t>(i)];
        return std::sqrt(s / (2.0 * (to - from)));
    };
    auto diffRms = [](const Out& a, const Out& b, int from, int to) {
        double s = 0.0;
        for (int i = from; i < to; ++i) {
            const double dl = static_cast<double>(a.l[static_cast<std::size_t>(i)])
                            - b.l[static_cast<std::size_t>(i)];
            const double dr = static_cast<double>(a.r[static_cast<std::size_t>(i)])
                            - b.r[static_cast<std::size_t>(i)];
            s += dl * dl + dr * dr;
        }
        return std::sqrt(s / (2.0 * (to - from)));
    };

    const int frames = 2048;
    const int w0 = tapDelay - 8, w1 = tapDelay + 400;   // 回折タップの到来まわりだけ見る
    const float occluded = 0.001f;                       // 直接音は透過まで落ちている

    // ── ⓪ HRTF そのものが左右対称か ──
    //   B1 で回折タップが HRTF を通るようになったので、「同じ開口の右側と左側で音色が違う」
    //   の原因候補に HRTF が加わった。本物のダミーヘッドなら左右の耳は厳密には同じでない。
    //   → **実測 0.00dB。この kemar は左右対称化されている**ので原因ではない。
    //     開口の左右差を追うときはここを除外してよい（この行はそのための証拠）。
    {
        const float az[] = {30.0f, 60.0f, 90.0f};
        std::printf("        HRTF の左右差（鏡像の方向で HRIR のパワーを比べる）:\n          ");
        for (float a : az) {
            float dl[3], dr[3];
            af::dsp::HrtfSet::angleToVector(-a, 0.0f, dl);
            af::dsp::HrtfSet::angleToVector( a, 0.0f, dr);
            const int il = set.nearestIndex(dl), ir = set.nearestIndex(dr);
            // 左方向のときの「近い側の耳」＝左耳、右方向のときは右耳。鏡像なら等しいはず。
            auto pw = [&](int idx, int ear) {
                const float* hh = set.hrir(idx, ear);
                double s = 0.0;
                for (int k = 0; k < set.irLength(); ++k) s += (double)hh[k] * hh[k];
                return s;
            };
            const double nearL = pw(il, 0), nearR = pw(ir, 1);
            std::printf("±%.0f°:%+.2fdB ", a, 10.0 * std::log10(nearR / std::max(nearL, 1e-20)));
        }
        std::printf("\n");
    }

    // ── ① ITD。右 60° の開口 ──
    //   等パワーパンは左右の**レベル差**しか作らない。同じ波形を定数倍しているだけなので
    //   両耳のずれは 0 サンプル。ここでは直接音を切って（深く遮蔽された状態）
    //   回折タップだけを鳴らし、混入を無くして測る。
    //   ★90° ちょうどだと等パワーパンで左が完全に無音になり、相関の分母が消えて
    //     測定そのものが成立しない。60° にしてある。
    {
        const float lateral = 0.8660254f;                    // sin(60°)
        const float t60 = (lateral + 1.0f) * 0.5f;
        const float panL60 = std::sqrt(1.0f - t60), panR60 = std::sqrt(t60);
        const Out pan = renderCase(60.0f, 0.0f, panL60, panR60, 0.0f, frames);
        const Out hrtf = renderCase(60.0f, 1.0f, panL60, panR60, 0.0f, frames);
        const int lagPan = itdSamples(pan, w0, w1);
        const int lagHrtf = itdSamples(hrtf, w0, w1);
        const double usPan = 1e6 * lagPan / sr;
        const double usHrtf = 1e6 * lagHrtf / sr;
        std::printf("        右60°の開口   ITD  パンのみ %+4d samp (%+6.0f us)"
                    " → HRTF %+4d samp (%+6.0f us)\n", lagPan, usPan, lagHrtf, usHrtf);
        check("[B1] 等パワーパンには ITD が無い", lagPan == 0);
        // 右から来るので右耳が先。相関 L[i]·R[i+lag] は**負のラグ**で最大になる。
        // 右 60°・頭囲 57cm なら 400〜700us 級。
        check("[B1] HRTF を載せると ITD が出る（右耳が先）",
              usHrtf <= -250.0 && usHrtf >= -1100.0);
    }

    // ── ② 前後の区別 ──
    //   正面と真後ろは左右軸への投影が同じ（x=0）なので、パンでは**完全に同じ音**になる。
    {
        const float panC = 0.70710678f;
        const Out panF = renderCase(0.0f, 0.0f, panC, panC, occluded, frames);
        const Out panB = renderCase(180.0f, 0.0f, panC, panC, occluded, frames);
        const Out hrF = renderCase(0.0f, 1.0f, panC, panC, occluded, frames);
        const Out hrB = renderCase(180.0f, 1.0f, panC, panC, occluded, frames);
        const double panDiff = diffRms(panF, panB, w0, w1) / rms2(panF, w0, w1);
        const double hrDiff = diffRms(hrF, hrB, w0, w1) / rms2(hrF, w0, w1);
        std::printf("        前 vs 後      差   パンのみ %.4f (%.1f dB) → HRTF %.4f (%.1f dB)\n",
                    panDiff, 20.0 * std::log10(std::max(panDiff, 1e-9)),
                    hrDiff, 20.0 * std::log10(std::max(hrDiff, 1e-9)));
        check("[B1] パンだけでは前後が区別できない", panDiff < 1e-6);
        if (haveReal) {
            check("[B1] HRTF を載せると前後が別の音になる", hrDiff > 0.05);
        } else {
            std::printf("      ※合成HRTF（球体頭）は耳介を持たないので前後が同一。"
                        "この検査は kemar.afhr がある時だけ意味を持つ。\n");
        }
    }

    // ── ③ エネルギーを足していないこと ──
    //   却下済みの「回折の床を足す」との違いはここ。空間化を変えただけで総量は動かない。
    {
        const float panL90 = 0.0f, panR90 = 1.0f;
        const Out pan = renderCase(90.0f, 0.0f, panL90, panR90, occluded, frames);
        const Out hrtf = renderCase(90.0f, 1.0f, panL90, panR90, occluded, frames);
        const double db = 20.0 * std::log10(rms2(hrtf, w0, w1) / rms2(pan, w0, w1));
        std::printf("        総量          パンのみ→HRTF  %+.2f dB\n", db);
        check("[B1] 総エネルギーが動かない（±3dB 以内）", std::fabs(db) <= 3.0);
    }

    // ── ④ 直接音の方向を汚していないこと ──
    //   1 本の HrtfProcessor は方向を 1 つしか持てない。直接音の方向を回折で
    //   上書きすると、開けた場所（遮蔽なし）の定位が壊れる。別インスタンスである根拠。
    {
        const float panC = 0.70710678f;
        // 直接音だけ鳴らす（回折タップのゲインは同じだが hrtfWeight=1 で右 90°）。
        //   直接音は正面なので L/R がほぼ等しくなるはず。
        const Out o = renderCase(90.0f, 1.0f, panC, panC, 1.0f, frames);
        double eL = 0.0, eR = 0.0;
        for (int i = 0; i < tapDelay - 16; ++i) {     // 回折タップが届く前＝直接音だけの区間
            eL += static_cast<double>(o.l[static_cast<std::size_t>(i)]) * o.l[static_cast<std::size_t>(i)];
            eR += static_cast<double>(o.r[static_cast<std::size_t>(i)]) * o.r[static_cast<std::size_t>(i)];
        }
        const double balDb = 10.0 * std::log10(std::max(eR, 1e-20) / std::max(eL, 1e-20));
        // 回折タップの区間は右に寄っているはず。
        double dL = 0.0, dR = 0.0;
        for (int i = w0; i < w1; ++i) {
            dL += static_cast<double>(o.l[static_cast<std::size_t>(i)]) * o.l[static_cast<std::size_t>(i)];
            dR += static_cast<double>(o.r[static_cast<std::size_t>(i)]) * o.r[static_cast<std::size_t>(i)];
        }
        const double difDb = 10.0 * std::log10(std::max(dR, 1e-20) / std::max(dL, 1e-20));
        std::printf("        方向の独立    直接音(正面) R/L %+.2f dB / 回折(右90°) R/L %+.2f dB\n",
                    balDb, difDb);
        check("[B1] 直接音は正面のまま（回折の方向に引っぱられない）", std::fabs(balDb) < 3.0);
        check("[B1] 回折タップは右へ寄る", difDb > 4.0);
    }

    // ── ⑤ 載っているタップが無いときは HRTF を回さない ──
    //   HrtfProcessor は入力が 0 でも IR 長ぶんを毎サンプル畳む。見通せている間ずっと
    //   空回しすると、何も鳴っていないのにコストだけ払い続けることになる。
    {
        af::dsp::EarlyReflectConv conv(sr, 4096, 30.0f);
        af::dsp::EarlyReflectConv::Tap t[2];
        for (int b = 0; b < 6; ++b) { t[0].g[b] = 1.0f; t[1].g[b] = 0.5f; }
        t[1].delaySamples = 100;
        float l, r, dL, dR, dir, hm;
        conv.setTaps(t, 2);
        conv.beginBlock();
        conv.processSample(1.0f, true, l, r, dL, dR, dir, hm);
        check("[B1] 回折タップが無ければバスは立たない", !conv.hasHrtfBus());
        t[1].hrtfWeight = 1.0f;
        conv.setTaps(t, 2);
        conv.beginBlock();
        conv.processSample(1.0f, true, l, r, dL, dR, dir, hm);
        check("[B1] 回折タップを載せるとバスが立つ", conv.hasHrtfBus());
    }

    // ── ⑥ 乗り換えの連続性 ──
    //   開口が 2 つあって強さが入れ替わるとき、載せ替えが 1 フレームで起きると段差になる。
    //   タップ補間（30ms）が weight も一緒に動かしているかを、重みを 0→1 に振って測る。
    {
        const float panC = 0.70710678f;
        double prev = -1e9, worst = 0.0;
        std::printf("        乗り換え      重み ");
        for (int k = 0; k <= 4; ++k) {
            const float w = k * 0.25f;
            const Out o = renderCase(90.0f, w, panC, panC, occluded, frames);
            const double lv = 20.0 * std::log10(std::max(rms2(o, w0, w1), 1e-12));
            std::printf("%.2f:%.1fdB ", static_cast<double>(w), lv);
            if (prev > -900.0) worst = std::max(worst, std::fabs(lv - prev));
            prev = lv;
        }
        std::printf("\n                      重み 0.25 刻みの最大段差 %.2f dB\n", worst);
        check("[B1] 重みを振っても段差が出ない（0.25 刻みで 2dB 以内）", worst <= 2.0);
    }
}

// 【反射タップの軽量な両耳化】ITD ＋ 帯域別 ILD が、畳み込みを増やさずに載るか。
//
//   く字の廊下の実測で、方向を運べるエネルギーの大半が早期反射側にあり
//   （早期反射 16.24 対 回折二次音源 0.0070 ＝ 2300 倍）、そこが等パワーパンだった。
//   反射を 1 本ずつ HRIR で畳み込むと重い（HRTF 1 本 0.05〜0.12ms/block）ので、
//   HRIR から ITD と帯域別 ILD だけを抜いてタップに載せる。
void testTapEarCues() {
    std::printf("\n[反射の両耳化] ITD と帯域別 ILD が畳み込み無しで載るか\n");
    const int block = 512;
    af::dsp::HrtfSet real;
    bool haveReal = false;
    const char* kPaths[] = {
        "UnityDemo/Assets/StreamingAssets/kemar.afhr",
        "../UnityDemo/Assets/StreamingAssets/kemar.afhr",
        "../../UnityDemo/Assets/StreamingAssets/kemar.afhr",
        "../../../UnityDemo/Assets/StreamingAssets/kemar.afhr",
    };
    for (const char* p : kPaths)
        if (af::dsp::HrtfSet::loadFromFile(p, real) && real.isValid()) { haveReal = true; break; }
    af::dsp::HrtfSet syn = af::dsp::HrtfSet::createSynthetic(48000, 5, 10);
    const af::dsp::HrtfSet& set = haveReal ? real : syn;
    const int sr = set.sampleRate();
    check("[反射] HRTF から耳ごとの帯域ゲインが作れる", set.hasEarBands());

    const int tapDelay = 240, frames = 2048;
    struct Out { std::vector<float> l, r; };
    auto render = [&](float azDeg, bool ear) {
        af::dsp::VoiceRenderer::Config cfg;
        cfg.sampleRate = sr; cfg.maxFrames = block;
        cfg.tailSeconds = 0.25f; cfg.tapCrossfadeMs = 30.0f;
        af::dsp::VoiceRenderer v(cfg);
        v.setOutputGain(1.0f);
        v.setHrtfSet(&set);
        v.setHrtfEnabled(true);
        v.setEarCuesEnabled(ear);
        const float fwd[3] = {0, 0, 1};
        v.setDirection(fwd, 57.0f);
        af::dsp::EarlyReflectConv::Tap taps[2];
        for (int b = 0; b < 6; ++b) taps[0].g[b] = 0.0f;      // 直接音は鳴らさない
        taps[0].delaySamples = 0;
        for (int b = 0; b < 6; ++b) taps[1].g[b] = 1.0f;      // 反射タップ 1 本だけ
        taps[1].delaySamples = tapDelay;
        af::dsp::HrtfSet::angleToVector(azDeg, 0.0f, taps[1].dir);
        taps[1].dirValid = true;
        // 等パワーパン（従来）も同じ方向で作る。
        const float lateral = std::sin(azDeg * 3.14159265f / 180.0f);
        const float tt = (lateral + 1.0f) * 0.5f;
        taps[1].panL = std::sqrt(1.0f - tt);
        taps[1].panR = std::sqrt(tt);
        v.setTaps(taps, 2);
        std::vector<float> zero(static_cast<std::size_t>(block), 0.0f);
        std::vector<float> tl(static_cast<std::size_t>(block)), tr(static_cast<std::size_t>(block));
        for (int i = 0; i < 8; ++i) v.render(zero.data(), block, tl.data(), tr.data(), nullptr);
        Out o;
        o.l.assign(static_cast<std::size_t>(frames), 0.0f);
        o.r.assign(static_cast<std::size_t>(frames), 0.0f);
        std::vector<float> in(static_cast<std::size_t>(frames), 0.0f);
        in[0] = 1.0f;
        v.render(in.data(), frames, o.l.data(), o.r.data(), nullptr);
        return o;
    };
    auto itdSamples = [](const Out& o, int from, int to) {
        int bestLag = 0; double best = -1e18;
        for (int lag = -80; lag <= 80; ++lag) {
            double s = 0.0;
            for (int i = from; i < to; ++i) {
                const int j = i + lag;
                if (j < 0 || j >= (int)o.r.size()) continue;
                s += (double)o.l[(std::size_t)i] * o.r[(std::size_t)j];
            }
            if (s > best) { best = s; bestLag = lag; }
        }
        return bestLag;
    };
    auto rms2 = [](const Out& o, int from, int to) {
        double s = 0.0;
        for (int i = from; i < to; ++i)
            s += (double)o.l[(std::size_t)i]*o.l[(std::size_t)i]
               + (double)o.r[(std::size_t)i]*o.r[(std::size_t)i];
        return std::sqrt(s / (2.0 * (to - from)));
    };
    const int w0 = tapDelay - 40, w1 = tapDelay + 400;
    const float azs[] = {30.0f, 60.0f, 90.0f};
    std::printf("        方位   パンのみ ITD   両耳化 ITD      総量の差\n");
    for (float az : azs) {
        const Out pan = render(az, false);
        const Out ear = render(az, true);
        const int lp = itdSamples(pan, w0, w1), le = itdSamples(ear, w0, w1);
        const double db = 20.0 * std::log10(std::max(rms2(ear, w0, w1), 1e-12)
                                          / std::max(rms2(pan, w0, w1), 1e-12));
        std::printf("        右%2.0f°   %+3d samp (%+5.0fus)  %+3d samp (%+5.0fus)  %+.2f dB\n",
                    az, lp, 1e6*lp/sr, le, 1e6*le/sr, db);
        if (az == 60.0f) {
            check("[反射] 等パワーパンには ITD が無い", lp == 0);
            check("[反射] 両耳化で ITD が出る（右耳が先）",
                  1e6*le/sr <= -250.0 && 1e6*le/sr >= -1100.0);
            check("[反射] 総量が動かない（±3dB 以内）", std::fabs(db) <= 3.0);
        }
    }
    // 前後は載らない（ITD/ILD は円錐の曖昧さを解けない）。そこは主役の 1 本が持つ。
    {
        const Out f = render(0.0f, true), b = render(180.0f, true);
        double d = 0.0, n = 0.0;
        for (int i = w0; i < w1; ++i) {
            const double dl = (double)f.l[(std::size_t)i] - b.l[(std::size_t)i];
            const double dr = (double)f.r[(std::size_t)i] - b.r[(std::size_t)i];
            d += dl*dl + dr*dr;
            n += (double)f.l[(std::size_t)i]*f.l[(std::size_t)i]
               + (double)f.r[(std::size_t)i]*f.r[(std::size_t)i];
        }
        const double rel = (n > 1e-20) ? std::sqrt(d / n) : 0.0;
        std::printf("        前 vs 後の差 %.4f（%.1f dB）── 載らないのは承知の上。\n",
                    rel, 20.0 * std::log10(std::max(rel, 1e-9)));
    }
}

void testTailCalibration() {
    std::printf("\n[尾の較正] 鳴らした結果が目標比に一致するか\n");
    const int sr = 48000;
    af::dsp::VoiceRenderer::Config cfg;
    cfg.sampleRate = sr;
    cfg.maxFrames = 512;
    cfg.tailSeconds = 0.5f;
    cfg.tapCrossfadeMs = 1.0f;

    // 直接音だけのタップ（反射なし）。これで 尾/直接 が素直に出る。
    af::dsp::EarlyReflectConv::Tap tap;
    for (int b = 0; b < 6; ++b) tap.g[b] = 1.0f;
    tap.delaySamples = 0;
    tap.gSpec = 1.0f; tap.gDiff = 0.0f;

    // 指数減衰のエコグラム（帯域一様）。形は何でもよく、量は target が決める。
    const int bins = 60;
    const float binMs = 10.0f;
    std::vector<float> echo(static_cast<std::size_t>(bins) * 6, 0.0f);
    for (int k = 0; k < bins; ++k) {
        const float e = std::pow(0.90f, static_cast<float>(k));
        for (int b = 0; b < 6; ++b) echo[static_cast<std::size_t>(k) * 6 + b] = e;
    }

    std::printf("        目標比 target   目標 尾/直接(√target)   実測 尾/直接   ずれ(dB)\n");
    double worst = 0.0;
    for (float target : {0.25f, 1.0f, 4.0f, 16.0f}) {
        af::dsp::VoiceRenderer voice(cfg);
        voice.setOutputGain(1.0f);
        voice.setHrtfEnabled(false);       // HRIR のゲインを混ぜない（較正だけを見る）
        voice.setTailLevel(1.0f);
        voice.setTailEnvelope(1.0f, 1.0f); // wet は尾に掛からない / srcLevel=1
        voice.setTaps(&tap, 1);
        // directGain=1 なので、鳴った 尾/直接 がそのまま √target と比べられる。
        //   ・startMs は早期↔後期の境目。0 だと直接ビンと尾を切り分けられない。
        //   ・envAlpha は更新をまたいだ時間平均なので、実使用と同じく数回呼んで落ち着かせる。
        float ratio = 0.0f;
        for (int it = 0; it < 12; ++it)
            ratio = voice.rebuildTail(echo.data(), bins, binMs, 20.0f, 8.0f, 30.0f, 0.0f, 0.6f,
                                      1.0f, target, nullptr, 0);
        if (ratio <= 0.0f) std::printf("        （尾のエネルギー比が 0。エコグラムが空）\n");

        const int n = 48000;
        Rng rng;
        std::vector<float> in(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
        std::vector<float> ol(static_cast<std::size_t>(n), 0.0f), orr(static_cast<std::size_t>(n), 0.0f);
        af::dsp::VoiceRenderer::Metering m{};
        voice.render(in.data(), n, ol.data(), orr.data(), &m);

        const double want = std::sqrt(static_cast<double>(target));
        const double got = (m.rmsDirect > 1e-9) ? m.rmsTail / m.rmsDirect : 0.0;
        const double db = 20.0 * std::log10(std::max(got, 1e-9) / std::max(want, 1e-9));
        worst = std::max(worst, std::fabs(db));
        std::printf("        %8.2f       %8.2f              %8.2f      %+6.1f\n",
                    target, want, got, db);
    }
    std::printf("      最大のずれ %.1f dB\n", worst);
    check("[尾] 鳴らした 尾/直接 が目標(√target)に一致する（±2dB）", worst < 2.0);

    // ★HRTF を通すと 尾/直接 がどれだけずれるか。
    //   rmsDirect は HRTF を**通した後**、rmsTail は通していない。HRIR のゲインが 1 で
    //   なければ、較正が正しくても表示上の比がずれる。Unity で 尾/直接 0.92 に対し
    //   目標 4.09（-13dB）と出ていたのはここの疑いが濃い。
    //   ★これは表示だけの問題ではない。尾は HRTF を通らないので、HRIR にゲインが
    //     あれば直接音だけが持ち上がり、**実際に耳へ届く比も変わる**。
    {
        const float target = 4.0f;
        double ratioNo = 0.0, ratioYes = 0.0;
        for (int k = 0; k < 2; ++k) {
            af::dsp::VoiceRenderer voice(cfg);
            voice.setOutputGain(1.0f);
            voice.setTailLevel(1.0f);
            voice.setTailEnvelope(1.0f, 1.0f);
            voice.setTaps(&tap, 1);
            af::dsp::HrtfSet syn = af::dsp::HrtfSet::createSynthetic(sr, 5, 10);
            if (k == 1) { voice.setHrtfSet(&syn); voice.setHrtfEnabled(true); }
            else        { voice.setHrtfEnabled(false); }
            const float dirFwd[3] = {0.0f, 0.0f, 1.0f};
            voice.setDirection(dirFwd, 57.0f);
            for (int it = 0; it < 12; ++it)
                voice.rebuildTail(echo.data(), bins, binMs, 20.0f, 8.0f, 30.0f, 0.0f, 0.6f,
                                  1.0f, target, nullptr, 0);
            const int n = 48000;
            Rng rng;
            std::vector<float> in(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
            std::vector<float> ol(static_cast<std::size_t>(n), 0.0f), orr(static_cast<std::size_t>(n), 0.0f);
            af::dsp::VoiceRenderer::Metering m{};
            voice.render(in.data(), n, ol.data(), orr.data(), &m);
            const double r = (m.rmsDirect > 1e-9) ? m.rmsTail / m.rmsDirect : 0.0;
            if (k == 0) ratioNo = r; else ratioYes = r;
        }
        const double shift = 20.0 * std::log10(std::max(ratioYes, 1e-9) / std::max(ratioNo, 1e-9));
        std::printf("      HRTF OFF の 尾/直接 %.2f → ON %.2f（ずれ %+.1f dB）\n",
                    ratioNo, ratioYes, shift);
        check("[尾] HRTF を通しても 尾/直接 が大きく動かない（±3dB）", std::fabs(shift) < 3.0);
    }
}

// ─────────────────────────────────────────────────────────────────────
// 【尾・段階②】VoiceRenderer に共有バスを差したとき、音が同じで費用が減るか
//
//   段階①（尾の**形**を部屋ごとに共有）は済んでいた。ここは段階②＝**畳み込みそのもの**を
//   1 回にまとめる話。畳み込み器レベルの等価は testSharedTailBusEquivalence で
//   -119.6dB まで確認済みなので、ここで見るのは**配線が正しいか**。
//
//   ★見たいのは 3 つ:
//     ① バスあり／なしで出力が一致する（音源ごとのレベルが保たれている）
//     ② 音源数が増えても畳み込みは 1 回のまま＝費用が音源数に比例しない
//     ③ 代表を 1 本も差さないと尾が鳴らない（配線ミスが黙って通らないこと）
// ─────────────────────────────────────────────────────────────────────
void testVoiceTailBus() {
    std::printf("\n[尾・段階②] 共有バスを VoiceRenderer に差す\n");
    const int sr = 48000;
    const int nSrc = 8;
    const int n = 8192;

    af::dsp::VoiceRenderer::Config cfg;
    cfg.sampleRate = sr;
    cfg.maxFrames = 512;
    cfg.tailSeconds = 0.25f;
    cfg.tapCrossfadeMs = 1.0f;

    // 同じ部屋＝同じエコグラム。音源ごとに直接音のゲインだけ変える（＝尾の量が変わる）。
    const int bins = 50;
    std::vector<float> echo(static_cast<std::size_t>(bins) * 6, 0.0f);
    for (int k = 0; k < bins; ++k)
        for (int b = 0; b < 6; ++b)
            echo[static_cast<std::size_t>(k) * 6 + b] = std::pow(0.9f, static_cast<float>(k));

    Rng rng;
    std::vector<std::vector<float>> in(static_cast<std::size_t>(nSrc));
    for (int s = 0; s < nSrc; ++s) {
        in[static_cast<std::size_t>(s)].resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            in[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)] = rng.next() * 0.3f;
    }
    auto directGainOf = [](int s) { return 0.4f + 0.08f * static_cast<float>(s); };

    // 出力を貯める箱。
    auto runAll = [&](bool useBus, std::vector<float>& sumL, std::vector<float>& sumR,
                      double* outMs, int* outPartitions) {
        sumL.assign(static_cast<std::size_t>(n), 0.0f);
        sumR.assign(static_cast<std::size_t>(n), 0.0f);
        std::vector<af::dsp::VoiceRenderer*> voices;
        // 尾の長さはバスも音源も同じにする（比較の前提）。
        const int tailSamples = static_cast<int>(cfg.tailSeconds * static_cast<float>(sr));
        af::dsp::TailBus bus(tailSamples, cfg.tailFirstBlock, cfg.tailCapBlock,
                             cfg.maxFrames, sr);
        for (int s = 0; s < nSrc; ++s) {
            auto* v = new af::dsp::VoiceRenderer(cfg);
            // ★等倍にしない。**音源ごとに違う値**にする。
            //   最初この検査は setOutputGain(1.0f) で回していて、そのせいで
            //   「バスへ回すと outputGain の掛け忘れで尾だけ大きくなる」を見逃した。
            //   実機で「聞こえ方が壊れる」と報告されて分かった。等倍だと掛け忘れが
            //   1.0 倍になって**見えない**。検査は倍率の間違いを見つけられる形にすること。
            v->setOutputGain(0.35f + 0.05f * static_cast<float>(s));
            v->setHrtfEnabled(false);
            af::dsp::EarlyReflectConv::Tap tap;
            for (int b = 0; b < 6; ++b) tap.g[b] = 1.0f;
            tap.delaySamples = 0;
            v->setTaps(&tap, 1);
            if (useBus) v->setTailBus(&bus, /*isOwner=*/s == 0);   // 代表は 1 本だけ
            v->rebuildTail(echo.data(), bins, 5.0f, 20.0f, 5.0f, 0.0f, 0.0f, 1.0f,
                           directGainOf(s), 0.5f);
            voices.push_back(v);
        }
        std::vector<float> ol(static_cast<std::size_t>(cfg.maxFrames), 0.0f);
        std::vector<float> orr(static_cast<std::size_t>(cfg.maxFrames), 0.0f);
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int off = 0; off + cfg.maxFrames <= n; off += cfg.maxFrames) {
            for (int s = 0; s < nSrc; ++s) {
                std::fill(ol.begin(), ol.end(), 0.0f);
                std::fill(orr.begin(), orr.end(), 0.0f);
                af::dsp::VoiceRenderer::Metering mm{};
                voices[static_cast<std::size_t>(s)]->render(
                    in[static_cast<std::size_t>(s)].data() + off, cfg.maxFrames,
                    ol.data(), orr.data(), &mm);
                for (int i = 0; i < cfg.maxFrames; ++i) {
                    sumL[static_cast<std::size_t>(off + i)] += ol[static_cast<std::size_t>(i)];
                    sumR[static_cast<std::size_t>(off + i)] += orr[static_cast<std::size_t>(i)];
                }
            }
            if (useBus) {
                // ★リスナー側で 1 回だけ。Unity では AudioListener のフィルタがここに当たる
                //   （全音源のミックス後に走るので、遅延を足さずに順序が保証される）。
                std::fill(ol.begin(), ol.end(), 0.0f);
                std::fill(orr.begin(), orr.end(), 0.0f);
                bus.render(cfg.maxFrames, ol.data(), orr.data());
                for (int i = 0; i < cfg.maxFrames; ++i) {
                    sumL[static_cast<std::size_t>(off + i)] += ol[static_cast<std::size_t>(i)];
                    sumR[static_cast<std::size_t>(off + i)] += orr[static_cast<std::size_t>(i)];
                }
            }
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        if (outMs) *outMs = std::chrono::duration<double, std::milli>(t1 - t0).count()
                            / (n / cfg.maxFrames);
        if (outPartitions) *outPartitions = useBus ? bus.partitions()
                                                   : voices[0]->tailPartitions() * nSrc;
        for (auto* v : voices) delete v;
    };

    std::vector<float> aL, aR, bL, bR;
    double msPer = 0.0, msBus = 0.0;
    int partPer = 0, partBus = 0;
    runAll(false, aL, aR, &msPer, &partPer);
    runAll(true,  bL, bR, &msBus, &partBus);

    double sa = 0.0, sb = 0.0, worst = 0.0;
    for (int i = 0; i < n; ++i) {
        sa += aL[static_cast<std::size_t>(i)] * aL[static_cast<std::size_t>(i)];
        sb += bL[static_cast<std::size_t>(i)] * bL[static_cast<std::size_t>(i)];
        worst = std::max(worst, std::fabs(static_cast<double>(aL[static_cast<std::size_t>(i)])
                                        - static_cast<double>(bL[static_cast<std::size_t>(i)])));
        worst = std::max(worst, std::fabs(static_cast<double>(aR[static_cast<std::size_t>(i)])
                                        - static_cast<double>(bR[static_cast<std::size_t>(i)])));
    }
    const double rmsA = std::sqrt(sa / n), rmsB = std::sqrt(sb / n);
    const double rel = (rmsA > 1e-12) ? worst / rmsA : 0.0;
    std::printf("        音源 %d 本 / 個別 RMS %.6f / バス RMS %.6f\n", nSrc, rmsA, rmsB);
    std::printf("        最大の差 %.3e（RMS 比 %.2e ＝ %.1f dB 下）\n",
                worst, rel, 20.0 * std::log10(std::max(rel, 1e-12)));
    char note[128];
    std::snprintf(note, sizeof(note), "(RMS 比 %.1f dB 下)", 20.0 * std::log10(std::max(rel, 1e-12)));
    check("[尾②] バスに差しても出力が変わらない（-100dB 以下）", rel < 1e-5, note);

    // ── 型紙 A（比が保たれる）を共有の式で（detectors.h）──
    //   ★元になったバグ: バスへ回すと **尾だけ 8.3 倍**（outputGain の掛け忘れ）。
    //     跳びでも幻でもなく「**一定して**間違っている」ので他の型紙では捕まらない。
    //   ⚠ サンプル単位の比はゼロ交差で暴れる。**ブロックごとの RMS の比**で見る。
    {
        std::vector<float> rmsPer, rmsBus, atBlock;
        for (int off = 0; off + cfg.maxFrames <= n; off += cfg.maxFrames) {
            double pa = 0.0, pb = 0.0;
            for (int i = 0; i < cfg.maxFrames; ++i) {
                const double x = aL[static_cast<std::size_t>(off + i)];
                const double y = bL[static_cast<std::size_t>(off + i)];
                pa += x * x; pb += y * y;
            }
            rmsPer.push_back(static_cast<float>(std::sqrt(pa / cfg.maxFrames)));
            rmsBus.push_back(static_cast<float>(std::sqrt(pb / cfg.maxFrames)));
            atBlock.push_back(static_cast<float>(off) / static_cast<float>(sr));
        }
        af::detect::Break br[8];
        const int nr = af::detect::ratioInRange(rmsBus.data(), rmsPer.data(), atBlock.data(),
                                                static_cast<int>(rmsBus.size()),
                                                0.99f, 1.01f, "bus/individual", br, 8);
        af::detect::report("型紙A 比が保たれる", br, nr, " 倍");
        std::snprintf(note, sizeof(note), "(%zu ブロック中 外れ %d 個)", rmsBus.size(), nr);
        check("[尾②] 型紙A バスと個別の比が 1.00 のまま（倍率の掛け忘れを捕まえる）",
              nr == 0, note);
    }

    std::snprintf(note, sizeof(note), "(個別 %.3f ms → バス %.3f ms / %.2f 倍)",
                  msPer, msBus, (msBus > 0) ? msPer / msBus : 0.0);
    std::printf("        1 ブロックの費用: 個別 %.3f ms → バス %.3f ms（%.2f 倍）\n",
                msPer, msBus, (msBus > 0) ? msPer / msBus : 0.0);
    check("[尾②] 音源 8 本でバスのほうが安い", msBus < msPer * 0.8, note);

    std::snprintf(note, sizeof(note), "(個別 合計 %d 分割 → バス %d 分割)", partPer, partBus);
    check("[尾②] 畳み込みの分割が音源数ぶん減る", partBus * 2 <= partPer, note);

    // ④ 尾 IR の**組み直し**の費用。制御スレッド側。
    //   共有バスにすると畳み込みは 1 回になるが、IR を組む方は各音源が呼び続ける。
    //   同じ部屋なら同じエコグラムを渡すので**まったく同じ IR を人数ぶん作って捨てている**。
    //   ここが効くなら、代表以外は組むのを飛ばせる。
    {
        const int tailSamples = static_cast<int>(cfg.tailSeconds * static_cast<float>(sr));
        af::dsp::TailBus bus(tailSamples, cfg.tailFirstBlock, cfg.tailCapBlock, cfg.maxFrames, sr);
        af::dsp::VoiceRenderer owner(cfg), guest(cfg);
        owner.setTailBus(&bus, true);
        guest.setTailBus(&bus, false);
        auto timeRebuild = [&](af::dsp::VoiceRenderer& v, int iters) {
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int k = 0; k < iters; ++k)
                v.rebuildTail(echo.data(), bins, 5.0f, 20.0f, 5.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
            const auto t1 = std::chrono::high_resolution_clock::now();
            return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
        };
        timeRebuild(owner, 5);                      // 暖機
        const double msOwner = timeRebuild(owner, 60);
        const double msGuest = timeRebuild(guest, 60);
        std::printf("        尾IRの組み直し: 代表 %.3f ms / 代表でない %.3f ms\n", msOwner, msGuest);
        std::printf("        → 同じ部屋なら同じ IR なので、代表でない側は組まない。\n"
                    "          直す前は 1 本 0.521ms・8 本で 1 回の組み直しに 3.645ms 無駄だった。\n");
        char n2[128];
        std::snprintf(n2, sizeof(n2), "(代表 %.3f ms / 代表でない %.3f ms)", msOwner, msGuest);
        check("[尾②] 代表でない音源は尾IRを組まない", msGuest < msOwner * 0.2, n2);
    }

    // ③ 代表を 1 本も差さないと尾が鳴らない ── 配線ミスが黙って通らないこと。
    {
        const int tailSamples = static_cast<int>(cfg.tailSeconds * static_cast<float>(sr));
        af::dsp::TailBus bus(tailSamples, cfg.tailFirstBlock, cfg.tailCapBlock,
                             cfg.maxFrames, sr);
        af::dsp::VoiceRenderer v(cfg);
        v.setHrtfEnabled(false);
        v.setTailBus(&bus, /*isOwner=*/false);          // 代表にしない
        v.rebuildTail(echo.data(), bins, 5.0f, 20.0f, 5.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
        std::snprintf(note, sizeof(note), "(IR あり=%d)", bus.hasIr() ? 1 : 0);
        check("[尾②] 代表を差さないとバスに IR が入らない（黙って鳴らない状態を検出できる）",
              !bus.hasIr(), note);
    }
}

void testVoiceRenderer() {
    std::printf("\n[信号フロー] 部品の配線と段別の内訳\n");

    const int sr = 48000;
    af::dsp::VoiceRenderer::Config cfg;
    cfg.sampleRate = sr;
    cfg.maxFrames = 512;
    cfg.tailSeconds = 0.25f;
    cfg.tapCrossfadeMs = 1.0f;
    af::dsp::VoiceRenderer voice(cfg);
    voice.setOutputGain(1.0f);
    voice.setHrtfEnabled(false);

    // タップ: 0=直接音（散乱なし）, 1=反射（散乱あり）
    af::dsp::EarlyReflectConv::Tap taps[2];
    for (int b = 0; b < 6; ++b) { taps[0].g[b] = 1.0f; taps[1].g[b] = 0.5f; }
    taps[0].delaySamples = 0;
    taps[1].delaySamples = 240;
    af::dsp::EarlyReflectConv::scatterSplit(5.0f, 20.0f, 0.0f, 1.0f, taps[1].gSpec, taps[1].gDiff);
    voice.setTaps(taps, 2);

    const int n = 8192;
    Rng rng;
    std::vector<float> in(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
    std::vector<float> ol(static_cast<std::size_t>(n), 0.0f), orr(static_cast<std::size_t>(n), 0.0f);

    af::dsp::VoiceRenderer::Metering m{};
    voice.render(in.data(), n, ol.data(), orr.data(), &m);
    std::printf("      段別RMS 直接 %.4f / 早期 %.4f / 散乱 %.4f / 尾 %.4f / 出力 %.4f\n",
                m.rmsDirect, m.rmsEarly, m.rmsScatter, m.rmsTail, m.rmsOut);

    check("直接音が出ている", m.rmsDirect > 0.01f);
    check("早期反射が出ている", m.rmsEarly > 0.001f);
    check("散乱が出ている", m.rmsScatter > 0.001f);
    check("尾はまだ無い（IRを入れていない）", m.rmsTail < 1e-6f);
    check("出力が出ている", m.rmsOut > 0.01f);

    // 尾の IR を入れると尾だけが立ち上がる。
    {
        const int bins = 50;
        std::vector<float> echo(static_cast<std::size_t>(bins) * 6, 0.0f);
        for (int k = 0; k < bins; ++k)
            for (int b = 0; b < 6; ++b)
                echo[static_cast<std::size_t>(k) * 6 + b] = std::pow(0.9f, static_cast<float>(k));
        const float ratio = voice.rebuildTail(echo.data(), bins, 5.0f, 20.0f, 5.0f,
                                              0.0f, 0.0f, 1.0f, /*directGain*/1.0f,
                                              /*targetRatio*/0.5f);
        check("尾のエネルギー比が返る", ratio > 0.0f);
        check("尾の分割が組まれている", voice.tailPartitions() > 0);

        af::dsp::VoiceRenderer::Metering m2{};
        voice.render(in.data(), n, ol.data(), orr.data(), &m2);
        std::printf("      尾を入れた後   直接 %.4f / 早期 %.4f / 散乱 %.4f / 尾 %.4f\n",
                    m2.rmsDirect, m2.rmsEarly, m2.rmsScatter, m2.rmsTail);
        check("尾が鳴り出す", m2.rmsTail > 1e-4f);
        // 他の段は変わらない（尾が混ざっていない）。
        checkNear("直接音は尾に影響されない", m2.rmsDirect, m.rmsDirect, m.rmsDirect * 0.05);
        checkNear("早期反射は尾に影響されない", m2.rmsEarly, m.rmsEarly, m.rmsEarly * 0.05 + 1e-6);
    }

    // 分割して呼んでも一括で呼んでも同じ（ブロック境界に依存しない）。
    {
        af::dsp::VoiceRenderer a(cfg), b(cfg);
        a.setOutputGain(1.0f); b.setOutputGain(1.0f);
        a.setHrtfEnabled(false); b.setHrtfEnabled(false);
        a.setTaps(taps, 2); b.setTaps(taps, 2);
        std::vector<float> al(static_cast<std::size_t>(n), 0.0f), ar(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> bl(static_cast<std::size_t>(n), 0.0f), br(static_cast<std::size_t>(n), 0.0f);
        a.render(in.data(), n, al.data(), ar.data());
        int done = 0;
        while (done < n) {                       // 半端な刻みで分割
            const int k = std::min(97, n - done);
            b.render(in.data() + done, k, bl.data() + done, br.data() + done);
            done += k;
        }
        double worst = 0.0;
        for (int i = 0; i < n; ++i)
            worst = std::max(worst, static_cast<double>(std::fabs(
                al[static_cast<std::size_t>(i)] - bl[static_cast<std::size_t>(i)])));
        checkNear("呼び出し粒度に依存しない", worst, 0.0, 1e-5);
    }

    // HRTF を入れると直接音が両耳化される（左右差が出る）。
    {
        af::dsp::HrtfSet set = af::dsp::HrtfSet::createSynthetic(sr, 5, 10);
        af::dsp::VoiceRenderer v(cfg);
        v.setOutputGain(1.0f);
        v.setHrtfSet(&set);
        v.setHrtfEnabled(true);
        af::dsp::EarlyReflectConv::Tap only;
        for (int b = 0; b < 6; ++b) only.g[b] = 1.0f;
        v.setTaps(&only, 1);
        float d[3];
        af::dsp::HrtfSet::angleToVector(90.0f, 0.0f, d);
        v.setDirection(d, 57.0f);

        std::vector<float> hl(static_cast<std::size_t>(n), 0.0f), hr(static_cast<std::size_t>(n), 0.0f);
        v.render(in.data(), n, hl.data(), hr.data());
        double el = 0.0, er = 0.0;
        for (int i = n / 2; i < n; ++i) {        // クロスフェード後の後半で見る
            el += hl[static_cast<std::size_t>(i)] * hl[static_cast<std::size_t>(i)];
            er += hr[static_cast<std::size_t>(i)] * hr[static_cast<std::size_t>(i)];
        }
        char b[96];
        std::snprintf(b, sizeof(b), "(左 %.3f / 右 %.3f)", el, er);
        check("HRTF で音源が右なら右が大きい", er > el * 1.5, b);
        std::printf("      HRTF セット名: %s\n", v.hrtfName());
    }

    // null / 0 フレームで落ちない。
    voice.render(nullptr, n, ol.data(), orr.data());
    voice.render(in.data(), 0, ol.data(), orr.data());
    check("null / 0フレームで落ちない", true);
}

}  // namespace

// ---------------------------------------------------------------- 方向バス
// 2026-09-04: 反射・回折のタップをリスナー座標で固定した N 本のレーンへ振って、レーンごとに固定の HRIR で畳む。
//   物差し: 1 本ずつその方向の HRIR で畳んだ基準と、8 レーン／12 レーンで、両耳相関（IACC）と ILD を比べる。
namespace dirbus {
double iacc(const std::vector<float>& L, const std::vector<float>& R, int maxLag) {
    double el = 0.0, er = 0.0;
    for (std::size_t i = 0; i < L.size(); ++i) { el += L[i] * L[i]; er += R[i] * R[i]; }
    if (el <= 0.0 || er <= 0.0) return 0.0;
    double best = 0.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double s = 0.0;
        for (std::size_t i = 0; i < L.size(); ++i) {
            const long j = static_cast<long>(i) + lag;
            if (j < 0 || j >= static_cast<long>(R.size())) continue;
            s += L[i] * R[static_cast<std::size_t>(j)];
        }
        best = std::max(best, std::fabs(s));
    }
    return best / std::sqrt(el * er);
}
double ildDb(const std::vector<float>& L, const std::vector<float>& R) {
    double el = 0.0, er = 0.0;
    for (std::size_t i = 0; i < L.size(); ++i) { el += L[i] * L[i]; er += R[i] * R[i]; }
    return 10.0 * std::log10(std::max(er, 1e-12) / std::max(el, 1e-12));
}
struct Ring { std::vector<float> az; std::vector<int> delay; };
// 基準: タップごとに、その方向の HRIR（ITD 込み）で畳む。
void renderReference(const af::dsp::HrtfSet& set, const std::vector<float>& x, const Ring& ring, int sr,
                     std::vector<float>& L, std::vector<float>& R) {
    const int N = static_cast<int>(x.size());
    L.assign(static_cast<std::size_t>(N), 0.0f); R.assign(static_cast<std::size_t>(N), 0.0f);
    const int irLen = set.irLength();
    for (std::size_t t = 0; t < ring.az.size(); ++t) {
        const float az = ring.az[t] * 3.14159265f / 180.0f;
        const float dir[3] = { std::sin(az), 0.0f, std::cos(az) };
        const int idx = set.nearestIndex(dir);
        const float itd = set.itdSecondsScaled(idx, 57.0f);
        const int dL = (itd < 0.0f) ? static_cast<int>(std::lround(-itd * sr)) : 0;
        const int dR = (itd > 0.0f) ? static_cast<int>(std::lround( itd * sr)) : 0;
        const float* hl = set.hrir(idx, 0);
        const float* hr = set.hrir(idx, 1);
        const int d0 = ring.delay[t];
        for (int i = 0; i < N; ++i) {
            const int src = i - d0;
            if (src < 0) continue;
            const float v = x[static_cast<std::size_t>(src)];
            if (v == 0.0f) continue;
            for (int k = 0; k < irLen; ++k) {
                const int jl = i + k + dL, jr = i + k + dR;
                if (jl < N) L[static_cast<std::size_t>(jl)] += v * hl[k];
                if (jr < N) R[static_cast<std::size_t>(jr)] += v * hr[k];
            }
        }
    }
}
// レーン: タップを 2 方向へ等パワーで振り、耳ごとに自分の ITD で遅らせて 方向×耳 の行へ送り、バスで畳む。
void renderLanes(const af::dsp::HrtfSet& set, const std::vector<float>& x, const Ring& ring, int sr, int lanes,
                 std::vector<float>& L, std::vector<float>& R) {
    using af::dsp::DirectionBus;
    const int N = static_cast<int>(x.size());
    const int B = 512;
    DirectionBus bus(sr, lanes, B);
    bus.setHrtfSet(&set, 57.0f);
    const int lat = bus.latency();
    L.assign(static_cast<std::size_t>(N), 0.0f); R.assign(static_cast<std::size_t>(N), 0.0f);
    std::vector<int> ln(ring.az.size() * 2); std::vector<float> w(ring.az.size() * 2);
    std::vector<int> earD(ring.az.size() * 2);
    for (std::size_t t = 0; t < ring.az.size(); ++t) {
        const float az = ring.az[t] * 3.14159265f / 180.0f;
        const float dir[3] = { std::sin(az), 0.0f, std::cos(az) };
        DirectionBus::laneWeights(dir, lanes, &ln[t * 2], &w[t * 2]);
        const int idx = set.nearestIndex(dir);
        const float itd = set.itdSecondsScaled(idx, 57.0f);
        earD[t * 2 + 0] = (itd < 0.0f) ? static_cast<int>(std::lround(-itd * sr)) : 0;
        earD[t * 2 + 1] = (itd > 0.0f) ? static_cast<int>(std::lround( itd * sr)) : 0;
    }
    const int rows = bus.rows();
    std::vector<float> lane(static_cast<std::size_t>(rows) * B);
    std::vector<float> oL(B), oR(B);
    for (int pos = 0; pos < N; pos += B) {
        const int n = std::min(B, N - pos);
        std::fill(lane.begin(), lane.end(), 0.0f);
        for (std::size_t t = 0; t < ring.az.size(); ++t) {
            for (int e = 0; e < 2; ++e) {
                const int d0 = std::max(0, ring.delay[t] - lat) + earD[t * 2 + e];
                for (int i = 0; i < n; ++i) {
                    const int src = pos + i - d0;
                    if (src < 0) continue;
                    const float v = x[static_cast<std::size_t>(src)];
                    lane[static_cast<std::size_t>(ln[t * 2] * 2 + e) * B + i]     += v * w[t * 2];
                    lane[static_cast<std::size_t>(ln[t * 2 + 1] * 2 + e) * B + i] += v * w[t * 2 + 1];
                }
            }
        }
        bus.add(lane.data(), n, B, 1.0f, 0);
        std::fill(oL.begin(), oL.end(), 0.0f); std::fill(oR.begin(), oR.end(), 0.0f);
        bus.render(n, oL.data(), oR.data());
        for (int i = 0; i < n; ++i) { L[static_cast<std::size_t>(pos + i)] = oL[i]; R[static_cast<std::size_t>(pos + i)] = oR[i]; }
    }
}
}  // namespace dirbus

void testDirectionBus() {
    using af::dsp::DirectionBus;
    std::printf("\n[方向バス] レーンへの振り分けと、両耳の物差し（IACC / ILD）\n");
    char buf[200];
    // 1) 重み: レーンの真上は 1 本 100%、間は等パワー（w0² + w1² = 1）。
    {
        int ln[2]; float w[2];
        const float front[3] = {0, 0, 1}; DirectionBus::laneWeights(front, 8, ln, w);
        check("正面はレーン 0 に 100%", ln[0] == 0 && std::fabs(w[0] - 1.0f) < 1e-5f && w[1] < 1e-5f);
        const float right[3] = {1, 0, 0}; DirectionBus::laneWeights(right, 8, ln, w);
        check("真右（90°）は 8 レーンならレーン 2 に 100%", ln[0] == 2 && std::fabs(w[0] - 1.0f) < 1e-5f);
        bool ok = true; float worst = 0.0f;
        for (int a = 0; a < 360; ++a) {
            const float az = a * 3.14159265f / 180.0f;
            const float d[3] = { std::sin(az), 0.0f, std::cos(az) };
            DirectionBus::laneWeights(d, 8, ln, w);
            const float e = w[0] * w[0] + w[1] * w[1];
            worst = std::max(worst, std::fabs(e - 1.0f));
            ok = ok && ln[0] >= 0 && ln[0] < 8 && ln[1] == (ln[0] + 1) % 8;
        }
        std::snprintf(buf, sizeof(buf), "(w0²+w1² の 1 からのずれ 最大 %.2e)", worst);
        check("全方位で等パワー（隣り合う 2 レーン）", ok && worst < 1e-5f, buf);
    }
    // 2) インパルス: 方向 2（右 90°）の右の行へ入れたら右にだけ出て、固有遅延の後にピークが立つ。
    {
        af::dsp::HrtfSet set = af::dsp::HrtfSet::createSynthetic(48000);
        DirectionBus bus(48000, 8, 512);
        bus.setHrtfSet(&set, 57.0f);
        check("HRIR を焼けた", bus.hasHrtf());
        auto energy = [](const std::vector<float>& v) { double e = 0; for (float s : v) e += s * s; return e; };
        auto peakAt = [](const std::vector<float>& v) { int p = 0; for (std::size_t i = 1; i < v.size(); ++i) if (std::fabs(v[i]) > std::fabs(v[static_cast<std::size_t>(p)])) p = static_cast<int>(i); return p; };
        std::vector<float> in(static_cast<std::size_t>(bus.rows()) * 512, 0.0f);
        in[static_cast<std::size_t>(2 * 2 + 1) * 512] = 1.0f;     // 方向 2、右耳の行
        std::vector<float> L(1024, 0.0f), R(1024, 0.0f);
        bus.add(in.data(), 512, 512, 1.0f, 0); bus.render(512, L.data(), R.data());
        std::fill(in.begin(), in.end(), 0.0f);
        bus.add(in.data(), 512, 512, 1.0f, 0); bus.render(512, L.data() + 512, R.data() + 512);
        std::snprintf(buf, sizeof(buf), "(左 %.2e / 右 %.2e、右のピーク %d サンプル、固有遅延 %d)", energy(L), energy(R), peakAt(R), bus.latency());
        check("右の行に入れると右耳にだけ出る（ピークは固有遅延の後）", energy(L) < 1e-9 && energy(R) > 1e-4 && peakAt(R) >= bus.latency(), buf);
    }
    // 3) 物差し: 右半分の環（0°〜180°、30° 刻み、7 本）と、レーンの真ん中の 1 本（22.5°）。
    {
        const int sr = 48000, N = sr;   // 1 秒
        af::dsp::HrtfSet set = af::dsp::HrtfSet::createSynthetic(sr);
        std::vector<float> x(static_cast<std::size_t>(N));
        unsigned rs = 2463534242u;
        for (int i = 0; i < N; ++i) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; x[static_cast<std::size_t>(i)] = static_cast<int>(rs) * (0.5f / 2147483648.0f); }
        struct Case { const char* name; dirbus::Ring ring; float iaccTol, ildTol; };
        std::vector<Case> cases;
        { dirbus::Ring r; for (int k = 0; k <= 7; ++k) { r.az.push_back(25.0f * k); r.delay.push_back(200 + 37 * k); } cases.push_back({"右半分の環 8 本（25° 刻み）", r, 0.10f, 2.0f}); }
        { dirbus::Ring r; r.az.push_back(22.5f); r.delay.push_back(200); cases.push_back({"レーンの真ん中 1 本（22.5°）", r, 0.15f, 3.0f}); }
        { dirbus::Ring r; for (int k = 0; k < 14; ++k) { r.az.push_back(25.0f * k); r.delay.push_back(200 + 37 * k); } cases.push_back({"全周の環 14 本（25° 刻み）", r, 0.10f, 2.0f}); }
        std::printf("      %-32s  %8s %8s %8s   %8s %8s %8s\n", "配置", "IACC基準", "IACC 8", "IACC 12", "ILD基準", "ILD 8", "ILD 12");
        for (const Case& c : cases) {
            std::vector<float> L0, R0, L8, R8, L12, R12;
            dirbus::renderReference(set, x, c.ring, sr, L0, R0);
            dirbus::renderLanes(set, x, c.ring, sr, 8, L8, R8);
            dirbus::renderLanes(set, x, c.ring, sr, 12, L12, R12);
            const int lag = sr / 1000;   // ±1 ms
            const double i0 = dirbus::iacc(L0, R0, lag), i8 = dirbus::iacc(L8, R8, lag), i12 = dirbus::iacc(L12, R12, lag);
            const double d0 = dirbus::ildDb(L0, R0), d8 = dirbus::ildDb(L8, R8), d12 = dirbus::ildDb(L12, R12);
            std::printf("      %-32s  %8.3f %8.3f %8.3f   %+7.1f %+7.1f %+7.1f dB\n", c.name, i0, i8, i12, d0, d8, d12);
            std::snprintf(buf, sizeof(buf), "(IACC 基準 %.3f / 8 レーン %.3f、ILD 基準 %+.1f / 8 レーン %+.1f dB)", i0, i8, d0, d8);
            std::string nm = std::string("[物差し] ") + c.name + ": 8 レーンが基準と両耳相関・ILD で近い";
            check(nm.c_str(), std::fabs(i8 - i0) < c.iaccTol && std::fabs(d8 - d0) < c.ildTol, buf);
        }
    }
}


// ================================ 【探り】扉が開いてから尾（残響）が追い付くまで
// 2026-09-08 の試聴「ドア動かしてから音が追い付いてくるまで若干ラグがある」の続き。
//
// エンジン側（タップとエコグラム）は AF_ONLY=lag で 83 ms 以内に収まることが分かった。
// 残るのは尾の IR の作り直しで、ここには**更新をまたいだ時間平均**が入っている。
//   envAcc += (env - envAcc) * envAlpha        （reverb_tail_ir.h の accumulateEnvelope）
// ホストは envAlpha = 0.6 を渡す（VoiceConvolver.cs）。1 回の作り直しで差の 6 割しか詰めない。
// 作り直しはエコグラムの版が変わったときだけなので、実測の更新間隔 83 ms を刻みにして測る。
void diagnoseTailCatchUp() {
    std::printf("\n[探り] 扉が開いてから尾（残響）が追い付くまで\n");

    const int sr = 48000, len = sr / 2, channels = 2;
    const int bins = 100;
    const float binMs = 10.0f;                 // ホストの既定（echogramBinSeconds = 0.01）
    const float startMs = 22.8f;               // Test_SwingDoor の尾の開始（√V）
    // 作り直しの間隔は 2 つの門で決まる。
    //   ・エコグラムの版が変わる  … 実測 83 ms（AF_ONLY=lag）
    //   ・tailRebuildEveryFrames  … VoiceConvolver の既定 8 フレーム = 133 ms
    // 遅いほうが効くので、いまは 133 ms が実際の刻み。両方振って効きを見る。
    const float periods[] = { 83.0f, 133.0f, 267.0f };

    // 扉が開くと、隣の部屋が繋がって尾の**減衰が遅くなる**。量ではなく形が変わる。
    //   （量＝尾の比は扉の開閉でほとんど動かない。AF_ONLY=lag の実測で −0.20 dB。
    //     IR はエネルギー 1 に正規化されるので、量は呼び手が別に決めている。）
    auto makeEcho = [&](float decay, std::vector<float>& echo) {
        echo.assign(static_cast<std::size_t>(bins) * 6, 0.0f);
        for (int k = 0; k < bins; ++k) {
            const float e = std::pow(decay, static_cast<float>(k));
            for (int b = 0; b < 6; ++b) echo[static_cast<std::size_t>(k) * 6 + b] = e;
        }
    };
    std::vector<float> closed, open;
    makeEcho(0.85f, closed);                   // 閉: 早く減る
    makeEcho(0.95f, open);                     // 開: 長く残る

    // 形の物差し: IR の後半（300〜500 ms）が全体の何 dB か。減衰が遅いほど大きい。
    auto lateDb = [&](const af::dsp::ReverbTailIr& t) {
        double late = 0.0, all = 0.0;
        const int a = static_cast<int>(0.300 * sr), b2 = static_cast<int>(0.500 * sr);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < len; ++i) {
                const double v = t.ir(c)[i]; const double e = v * v;
                all += e;
                if (i >= a && i < b2) late += e;
            }
        return 10.0 * std::log10(std::max(late / std::max(all, 1e-20), 1e-10));
    };

    std::printf("        envAlpha  作り直し   形の変化 dB   63%%      90%%      99%%\n");
    const float alphas[] = { 0.6f, 0.9f, 1.0f };
    for (int pi = 0; pi < 3; ++pi) {
    const float rebuildMs = periods[pi];
    for (int a = 0; a < 3; ++a) {
        af::dsp::ReverbTailIr tail(sr, len, channels, bins);
        // 閉じた状態で落ち着かせる。
        for (int i = 0; i < 40; ++i)
            tail.build(closed.data(), bins, binMs, startMs, 20.0f, 30.0f, 0.0f, alphas[a]);
        const float beforeDb = static_cast<float>(lateDb(tail));

        // 扉が開いた。以降は作り直しのたびに開のエコグラムを渡す。
        float db[80]; int nb = 0;
        for (int i = 0; i < 60; ++i) {
            tail.build(open.data(), bins, binMs, startMs, 20.0f, 30.0f, 0.0f, alphas[a]);
            db[nb++] = static_cast<float>(lateDb(tail));
        }
        const float afterDb = db[nb - 1];
        const float span = afterDb - beforeDb;
        float t63 = -1, t90 = -1, t99 = -1;
        for (int i = 0; i < nb; ++i) {
            const float f = (db[i] - beforeDb) / span;
            if (t63 < 0 && f >= 0.63f) t63 = (i + 1) * rebuildMs;
            if (t90 < 0 && f >= 0.90f) t90 = (i + 1) * rebuildMs;
            if (t99 < 0 && f >= 0.99f) t99 = (i + 1) * rebuildMs;
        }
        std::printf("        %8.2f %7.0f ms %9.1f %8.0f %8.0f %8.0f%s\n",
                    alphas[a], rebuildMs, span, t63, t90, t99,
                    (std::fabs(rebuildMs - 133.0f) < 1.0f && std::fabs(alphas[a] - 0.6f) < 0.01f)
                        ? "   <- いまの既定" : "");
    }
    }
    std::printf("      ※ envAlpha = 0.6 がホストの既定（VoiceConvolver.cs）。1.0 は時間平均なし。\n"
                "        これに非同期の写し 16.7 ms・IR 差し替えの渡り 50 ms・Unity の DSP バッファが乗る。\n"
                "        尾の左右（方向）はさらに別の平滑を通る: directionalTailSmoothSec = 0.35 s。\n");
}

// 扉が「振れて」動くときの遅れ。上の 399 ms は 1 フレームで瞬間に開いた場合の落ち着き時間で、
// 実際の扉は 0.5〜1 秒かけて振れる。そのとき効くのは落ち着き時間ではなく**追従の遅れ**
//   ＝ 入力が動いているあいだ、出力が何 ms 後ろを走るか。
// 一次遅れなら、傾斜入力に対する遅れは時定数そのものになる（τ = 刻み / −ln(1−α)）。
// 実際にそうなるかを測る。時間平均なし（α=1）を「理想」とし、同じ高さに達する時刻の差を取る。
void diagnoseTailTrackingLag() {
    std::printf("\n[探り] 扉が振れて動くときの、尾の追従の遅れ\n");

    const int sr = 48000, len = sr / 2, channels = 2;
    const int bins = 100;
    const float binMs = 10.0f;
    const float startMs = 22.8f;

    auto makeEcho = [&](float decay, std::vector<float>& echo) {
        echo.assign(static_cast<std::size_t>(bins) * 6, 0.0f);
        for (int k = 0; k < bins; ++k) {
            const float e = std::pow(decay, static_cast<float>(k));
            for (int b = 0; b < 6; ++b) echo[static_cast<std::size_t>(k) * 6 + b] = e;
        }
    };
    auto lateDb = [&](const af::dsp::ReverbTailIr& t) {
        double late = 0.0, all = 0.0;
        const int a = static_cast<int>(0.300 * sr), b2 = static_cast<int>(0.500 * sr);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < len; ++i) {
                const double v = t.ir(c)[i]; const double e = v * v;
                all += e;
                if (i >= a && i < b2) late += e;
            }
        return 10.0 * std::log10(std::max(late / std::max(all, 1e-20), 1e-10));
    };
    // 曲線が height（0..1）を最初に超える時刻。線形補間で刻みより細かく出す。
    auto crossMs = [](const float* v, int n, float lo, float hi, float height, float stepMs) {
        const float want = lo + (hi - lo) * height;
        for (int i = 1; i < n; ++i)
            if ((v[i] - want) * (hi - lo) >= 0.0f) {
                const float d = v[i] - v[i - 1];
                const float f = (std::fabs(d) > 1e-6f) ? (want - v[i - 1]) / d : 0.0f;
                return (i - 1 + f) * stepMs;
            }
        return -1.0f;
    };

    std::printf("        扉の振れ  刻み  envAlpha   25%%到達   50%%到達   75%%到達   追従の遅れ(50%%)\n");
    const float swings[] = { 300.0f, 600.0f, 1000.0f };
    const float stepMs = 133.0f;                 // tailRebuildEveryFrames = 8 @ 60fps
    for (int sw = 0; sw < 3; ++sw) {
        float ref50 = 0.0f;
        for (int a = 1; a >= 0; --a) {           // 先に α=1（理想）を採ってから 0.6 と比べる
            const float alpha = (a == 1) ? 1.0f : 0.6f;
            af::dsp::ReverbTailIr tail(sr, len, channels, bins);
            std::vector<float> e0; makeEcho(0.85f, e0);
            for (int i = 0; i < 40; ++i)
                tail.build(e0.data(), bins, binMs, startMs, 20.0f, 30.0f, 0.0f, alpha);
            const float lo = static_cast<float>(lateDb(tail));

            // 扉が swings[sw] ミリ秒かけて振れる。刻みごとに現在の角度のエコグラムを渡す。
            float curve[120]; int n = 0;
            const int N = static_cast<int>(swings[sw] / stepMs) + 24;
            for (int i = 0; i < N && n < 120; ++i) {
                const float t = (i + 1) * stepMs;
                const float u = std::min(1.0f, t / swings[sw]);
                std::vector<float> ei; makeEcho(0.85f + 0.10f * u, ei);
                tail.build(ei.data(), bins, binMs, startMs, 20.0f, 30.0f, 0.0f, alpha);
                curve[n++] = static_cast<float>(lateDb(tail));
            }
            const float hi = curve[n - 1];
            const float t25 = crossMs(curve, n, lo, hi, 0.25f, stepMs);
            const float t50 = crossMs(curve, n, lo, hi, 0.50f, stepMs);
            const float t75 = crossMs(curve, n, lo, hi, 0.75f, stepMs);
            if (a == 1) ref50 = t50;
            std::printf("        %6.0f ms %5.0f %8.2f %9.0f %9.0f %9.0f %13.0f ms\n",
                        swings[sw], stepMs, alpha, t25, t50, t75,
                        (a == 1) ? 0.0f : (t50 - ref50));
        }
    }
    // 尾の**方向**（左右バランス）はホスト側の一次遅れを通る。
    //   k = 1 - exp(-dt*stride / directionalTailSmoothSec) の Lerp（AcousticFlowSceneDemo.cs）。
    //   同じ形の入力を通して 50%% 到達の差を採る。C# と同じ式をここで回す。
    std::printf("\n        参考: 尾の**方向**（directionalTailSmoothSec の一次遅れ）\n");
    std::printf("        扉の振れ   時定数    50%%到達   追従の遅れ(50%%)\n");
    for (int sw = 0; sw < 3; ++sw) {
        const float taus[] = { 0.35f, 0.15f };
        for (int ti = 0; ti < 2; ++ti) {
            const float tau = taus[ti];
            const float dt = 1.0f / 60.0f;
            float y = 0.0f; float t50 = -1.0f, ideal50 = -1.0f;
            const int N = static_cast<int>((swings[sw] + 2000.0f) / 1000.0f * 60.0f);
            for (int i = 0; i < N; ++i) {
                const float t = (i + 1) * dt * 1000.0f;
                const float target = std::min(1.0f, t / swings[sw]);   // 扉の振れ（傾斜）
                const float k = 1.0f - std::exp(-dt / tau);
                y += (target - y) * k;
                if (ideal50 < 0.0f && target >= 0.5f) ideal50 = t;
                if (t50 < 0.0f && y >= 0.5f) t50 = t;
            }
            std::printf("        %6.0f ms %8.2f s %9.0f %13.0f ms%s\n",
                        swings[sw], tau, t50, t50 - ideal50,
                        (ti == 0) ? "   <- いまの既定" : "");
        }
    }
    std::printf("      ※「追従の遅れ」は時間平均なし（α=1）との 50%% 到達時刻の差。\n"
                "        扉が動いているあいだ、響きが何 ms 後ろを走るか。\n"
                "        1 フレームで瞬間に開いた場合の落ち着き時間（99%% で 399 ms）とは別の量。\n"
                "        尾の**方向**は directionalTailSmoothSec = 0.35 s の一次遅れなので、\n"
                "        傾斜入力への遅れはそのまま 350 ms。こちらのほうが大きい。\n");
}

// ================================ [FDN] 帯域別 RT60 の帰還遅延網（docs/TAIL_FDN_PLAN.md の手順 1）
namespace fdntest {

// 出力を 6 帯域に分ける（FdnTail / EarlyReflectConv / ReverbTailIr と同じ直列クロスオーバー）。
struct Bq {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setLowpass(float fc, float fs) {
        const float w0 = 2.0f * 3.14159265358979323846f * fc / fs;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float alpha = sw / (2.0f * 0.70710678f);
        b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; b2 = (1.0f - cw) * 0.5f;
        const float a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    float process(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
// order = 1: 2 次（系の帯域＝EarlyReflectConv と同じ）／2: 4 次（LR4。耳の帯域幅に近づける）。
void split6(const float* src, int n, int fs, std::vector<float>& out /*[6][n]*/, int order = 1) {
    static const float kCrossHz[5] = {177.0f, 354.0f, 707.0f, 1414.0f, 2828.0f};
    Bq lp[5][2];
    for (int c = 0; c < 5; ++c) for (int s = 0; s < 2; ++s) lp[c][s].setLowpass(kCrossHz[c], static_cast<float>(fs));
    out.assign(static_cast<std::size_t>(6) * n, 0.0f);
    for (int i = 0; i < n; ++i) {
        float rest = src[i];
        for (int c = 0; c < 5; ++c) {
            float lo = lp[c][0].process(rest);
            if (order >= 2) lo = lp[c][1].process(lo);
            out[static_cast<std::size_t>(c) * n + i] = lo; rest -= lo;
        }
        out[static_cast<std::size_t>(5) * n + i] = rest;
    }
}

// 減衰から T60 を出す。Schroeder の後方積分（EDC）を −5〜−25 dB の区間で直線に当て、60 dB へ外挿
// （ISO 3382 の T20 と同じ形）。
//   ★最初は「包絡の山から −5〜−25 dB」で当てていて、FDN の立ち上がりが疎で山が早すぎるため
//     密度が上がる「立ち上がり」を減衰として拾い、傾きが 30〜80% 浅く出た。
//     エネルギーは 1 に合っていた（±1.5 dB）ので減衰は合っていて、物差しが外れていた。
//     EDC は定義から単調なので立ち上がりに騙されない。
float fitT60(const float* x, int n, int fs, float startSec = 0.25f, float* outSlopeDbPerSec = nullptr) {
    std::vector<double> edc(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = n - 1; i >= 0; --i) edc[static_cast<std::size_t>(i)] = edc[static_cast<std::size_t>(i) + 1] + static_cast<double>(x[i]) * x[i];
    const double e0 = edc[0];
    if (e0 <= 1e-30) return -1.0f;
    const int step = std::max(1, fs / 1000);             // 1 ms 刻みで当てる
    // 窓: 最初の 250 ms（遅延線の最長 78 ms の約 3 周）を外し、その時点の EDC から 20 dB 下まで。
    //   ★−10〜−30 dB のような**レベル**で窓を切ると、短い RT60 では −10 dB が 50 ms 付近に来て
    //     16 本の線の最初の到達（15〜78 ms）の階段が窓に入り、傾きが浅く出た（0.3 s で +8%、0.4 s で +14%）。
    //   ★120 ms 始まりでも足りなかった。平ら 0.4 s で 125 Hz が −10%・4 kHz が +6% と、
    //     **同じ係数の輪なのに帯域で差が出た**＝混ざり切る前の早い区間は入力の形（帯域の入口の
    //     フィルタの鳴り方）に依る。漸近の減衰を測るには線の最長の約 3 周を待つ。
    //   ★これは器の性質でもある: 線が 15〜78 ms だと混ざり切るのに約 200 ms 掛かり、RT60 0.3 s の
    //     乾いた小部屋では減衰の大半がその前に終わる。**線の長さは部屋ごとの器を作るときに
    //     mixing time なりに決める**（手順 2。実行時には変えない）。
    const int i0 = std::min(n - 1, static_cast<int>(startSec * fs));   // 既定 250 ms（基準の線の最長 78 ms の約 3 周）
    const double d0 = 10.0 * std::log10(std::max(edc[static_cast<std::size_t>(i0)] / e0, 1e-30));
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (int i = i0; i < n; i += step) {
        const double d = 10.0 * std::log10(std::max(edc[static_cast<std::size_t>(i)] / e0, 1e-30));
        if (d < d0 - 20.0) break;
        const double t = static_cast<double>(i) / fs;
        sx += t; sy += d; sxx += t * t; sxy += t * d; ++m;
    }
    if (m < 4) return -1.0f;
    const double slope = (m * sxy - sx * sy) / std::max(m * sxx - sx * sx, 1e-12);   // dB/s（負）
    if (outSlopeDbPerSec) *outSlopeDbPerSec = static_cast<float>(slope);
    return (slope < -1e-6) ? static_cast<float>(-60.0 / slope) : -1.0f;
}

}  // namespace fdntest

void testFdnTail() {
    std::printf("\n[FDN] 帯域別 RT60 の帰還遅延網 ── 減衰の傾き・量・決定性・連続性\n");
    const int fs = 48000;
    const int blk = 256;

    // ── 1. 帯域ごとの減衰が与えた RT60 に乗るか（±5% ＝ RT60 の弁別閾）──
    float lastInGain = 0.0f;
    // t60loop : 帯域ごとの輪の出力を直接測った T60（仕組みの検査。これが ±5% の合否）
    // t60mix2 : 混ぜた出力を 2 次の分析で測った T60（系の帯域での見え方。隣の帯域が漏れる）
    // t60mix4 : 混ぜた出力を 4 次の分析で測った T60（耳の帯域幅に近い見え方）
    auto measure = [&](const float* rt60, float seconds, std::vector<float>& outIr,
                       float* t60loop, float* t60mix2, float* t60mix4) {
        const int n = static_cast<int>(seconds * fs);
        outIr.assign(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
        in[0] = 1.0f;
        {
            af::dsp::FdnTail fdn(fs); fdn.setRt60(rt60);
            for (int p = 0; p < n; p += blk) fdn.render(in.data() + p, std::min(blk, n - p), outIr.data() + p);
            lastInGain = fdn.inputGain();
        }
        {
            af::dsp::FdnTail fdn(fs); fdn.setRt60(rt60);
            std::vector<float> loops(static_cast<std::size_t>(6) * n, 0.0f);
            float* ptr[6]; for (int b = 0; b < 6; ++b) ptr[b] = loops.data() + static_cast<std::size_t>(b) * n;
            for (int p = 0; p < n; p += blk) {
                float* pp[6]; for (int b = 0; b < 6; ++b) pp[b] = ptr[b] + p;
                fdn.renderBands(in.data() + p, std::min(blk, n - p), pp);
            }
            for (int b = 0; b < 6; ++b) t60loop[b] = fdntest::fitT60(ptr[b], n, fs);
        }
        std::vector<float> bands;
        fdntest::split6(outIr.data(), n, fs, bands, 1);
        for (int b = 0; b < 6; ++b) t60mix2[b] = fdntest::fitT60(bands.data() + static_cast<std::size_t>(b) * n, n, fs);
        fdntest::split6(outIr.data(), n, fs, bands, 2);
        for (int b = 0; b < 6; ++b) t60mix4[b] = fdntest::fitT60(bands.data() + static_cast<std::size_t>(b) * n, n, fs);
    };

    {
        const float rt[6] = { 1.2f, 1.0f, 0.8f, 0.6f, 0.45f, 0.3f };   // 高域ほど短い（実物の部屋の向き）
        std::vector<float> ir; float meas[6], mix2[6], mix4[6];
        measure(rt, 3.0f, ir, meas, mix2, mix4);
        std::printf("        帯域      目標 T60   輪の実測    ずれ     混ぜ(2次)  混ぜ(4次)\n");
        bool ok = true; char buf[160]; float worst = 0.0f;
        static const char* nm[6] = { "125", "250", "500", "1k", "2k", "4k" };
        for (int b = 0; b < 6; ++b) {
            const float err = (meas[b] > 0.0f) ? (meas[b] - rt[b]) / rt[b] : 9.0f;
            worst = std::max(worst, std::fabs(err));
            if (std::fabs(err) > 0.05f) ok = false;
            std::printf("        %-6s %10.2f s %9.2f s %+7.1f %%  %8.2f s %8.2f s\n", nm[b], rt[b], meas[b], err * 100.0f, mix2[b], mix4[b]);
        }
        std::printf("        ※輪の実測が仕組みの合否。混ぜた出力は分析フィルタの帯域が重なるぶん隣の帯域が漏れて見える。\n");
        std::snprintf(buf, sizeof(buf), "(最大のずれ %.1f %%)", worst * 100.0f);
        check("[FDN] 6 帯域とも減衰の傾きが与えた RT60 に ±5% で乗る（高域ほど短い設定）", ok, buf);

        // 量: インパルス応答のエネルギーが 1（±2 dB）。ReverbTailIr と同じ規約。
        double e = 0.0; for (float v : ir) e += static_cast<double>(v) * v;
        const double eDb = 10.0 * std::log10(std::max(e, 1e-30));
        std::snprintf(buf, sizeof(buf), "(エネルギー %.2f dB、入力ゲイン %.3f)", eDb, lastInGain);
        check("[FDN] インパルス応答のエネルギーが 1（±2 dB）", std::fabs(eDb) <= 2.0, buf);
        std::printf("        インパルス応答のエネルギー %.2f dB\n", eDb);
    }
    {
        const float rt[6] = { 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f };   // 短く平ら
        std::vector<float> ir; float meas[6], mix2[6], mix4[6];
        measure(rt, 1.5f, ir, meas, mix2, mix4);
        bool ok = true; float worst = 0.0f; char buf[96];
        static const char* nm2[6] = { "125", "250", "500", "1k", "2k", "4k" };
        std::printf("        平ら 0.4 s:");
        for (int b = 0; b < 6; ++b) {
            const float err = (meas[b] > 0.0f) ? (meas[b] - rt[b]) / rt[b] : 9.0f;
            worst = std::max(worst, std::fabs(err)); if (std::fabs(err) > 0.10f) ok = false;
            std::printf("  %s %.2fs(%+.0f%%)", nm2[b], meas[b], err * 100.0f);
        }
        std::printf("\n");
        // ★ここは ±10%。同じ係数の輪なのに 250 Hz が −8% に出る。16 本・合計約 640 ms の網では
        //   モードの密度が約 0.64 本/Hz で、低い帯域ほどモードがまばら → 入口の鳴り方で励振される
        //   モードが偏り、減衰に分散が出る（器の性質）。実物の部屋には必ず傾き（高域ほど短い）が
        //   あり、そちらは ±2.9% で乗っているので担保はそちらに置く。線の長さを部屋ごとに決める
        //   手順 2 で、乾いた小部屋について改めて見る。
        std::snprintf(buf, sizeof(buf), "(0.4 s 平ら: 最大のずれ %.1f %%。低域のモードがまばらな分)", worst * 100.0f);
        check("[FDN] 短く平らな RT60 でも 6 帯域が ±10% で乗る（低域のモード分散込み）", ok, buf);
        double e = 0.0; for (float v : ir) e += static_cast<double>(v) * v;
        const double eDb = 10.0 * std::log10(std::max(e, 1e-30));
        std::snprintf(buf, sizeof(buf), "(エネルギー %.2f dB)", eDb);
        check("[FDN] 短い RT60 でもエネルギーが 1（±2 dB）＝ 量が長さに引きずられない", std::fabs(eDb) <= 2.0, buf);
    }

    // ── 1b. 量の正規化のずれの形（診断）: RT60（平ら）と線の倍率と拡散を振って IR のエネルギーを見る ──
    //   √(1 − ḡ²) の近似がどこでどれだけ外れるかを知るため。表を見て正規化の直し方を決める。
    {
        std::printf("        量の正規化のずれ（IR のエネルギー dB。0 が理想）\n");
        std::printf("        倍率  拡散     T60=0.15  0.3    0.6    1.2    2.4\n");
        const float t60s[5] = { 0.15f, 0.3f, 0.6f, 1.2f, 2.4f };
        const float scales[2] = { 1.0f, 0.49f };
        const float diffs[2] = { 0.6f, 0.0f };
        for (int si = 0; si < 2; ++si) for (int di = 0; di < 2; ++di) {
            std::printf("        %4.2f  %4.2f   ", scales[si], diffs[di]);
            for (int ti = 0; ti < 5; ++ti) {
                float rt[6]; for (int b = 0; b < 6; ++b) rt[b] = t60s[ti];
                af::dsp::FdnTail fdn(fs, diffs[di], scales[si]); fdn.setRt60(rt);
                const int n = static_cast<int>(std::min(6.0f, t60s[ti] * 2.5f) * fs);
                std::vector<float> in(static_cast<std::size_t>(n), 0.0f), out(static_cast<std::size_t>(n), 0.0f);
                in[0] = 1.0f;
                for (int p = 0; p < n; p += blk) fdn.render(in.data() + p, std::min(blk, n - p), out.data() + p);
                double e = 0.0; for (float v : out) e += static_cast<double>(v) * v;
                std::printf("%6.2f ", 10.0 * std::log10(std::max(e, 1e-30)));
            }
            std::printf("\n");
        }
    }

    // ── 2. 決定性: 同じ入力で 2 回作ってビット一致（実行時に乱数を引かない）──
    {
        const float rt[6] = { 0.8f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f };
        af::dsp::FdnTail a(fs), b(fs); a.setRt60(rt); b.setRt60(rt);
        const int n = fs / 2;
        std::vector<float> in(static_cast<std::size_t>(n)), oa(static_cast<std::size_t>(n)), ob(static_cast<std::size_t>(n));
        unsigned int rng = 777u;
        for (int i = 0; i < n; ++i) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; in[static_cast<std::size_t>(i)] = static_cast<float>(static_cast<int>(rng)) * (1.0f / 2147483648.0f) * 0.1f; }
        for (int p = 0; p < n; p += blk) { a.render(in.data() + p, std::min(blk, n - p), oa.data() + p); b.render(in.data() + p, std::min(blk, n - p), ob.data() + p); }
        float maxd = 0.0f; for (int i = 0; i < n; ++i) maxd = std::max(maxd, std::fabs(oa[static_cast<std::size_t>(i)] - ob[static_cast<std::size_t>(i)]));
        check("[FDN] 同じ入力で 2 つの器がビット一致（決定性）", maxd == 0.0f);
    }

    // ── 3. 安定性: 長い RT60（20 s）で膨らまない ──
    {
        const float rt[6] = { 20.0f, 20.0f, 20.0f, 20.0f, 20.0f, 20.0f };
        af::dsp::FdnTail fdn(fs); fdn.setRt60(rt);
        const int n = fs * 4;
        std::vector<float> in(static_cast<std::size_t>(n), 0.0f), out(static_cast<std::size_t>(n), 0.0f);
        in[0] = 1.0f;
        for (int p = 0; p < n; p += blk) fdn.render(in.data() + p, std::min(blk, n - p), out.data() + p);
        auto rmsOf = [&](int from, int to) { double e = 0.0; for (int i = from; i < to; ++i) e += static_cast<double>(out[static_cast<std::size_t>(i)]) * out[static_cast<std::size_t>(i)]; return std::sqrt(e / std::max(1, to - from)); };
        const double r1 = rmsOf(fs / 2, fs), r2 = rmsOf(3 * fs, 4 * fs);
        float peak = 0.0f; for (float v : out) peak = std::max(peak, std::fabs(v));
        char buf[128]; std::snprintf(buf, sizeof(buf), "(0.5〜1 s の RMS %.4f → 3〜4 s %.4f、ピーク %.3f)", r1, r2, peak);
        check("[FDN] RT60 = 20 s でも減っていく（無損失の輪が無い）", r2 < r1 && peak < 4.0f, buf);
    }

    // ── 4. 連続性: 鳴っている最中に RT60 を 1.0 → 0.3 s へ切り替えても段差が出ない ──
    //   ★段差は正弦で測る（雑音では埋もれる。扉のプツプツの教訓）。
    {
        const float rtA[6] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        const float rtB[6] = { 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f };
        af::dsp::FdnTail fdn(fs); fdn.setRt60(rtA);
        const int n = fs * 2;
        std::vector<float> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = 0.1f * std::sin(2.0f * 3.14159265f * 220.0f * i / fs);
        // 1 秒鳴らして定常にし、そこで切り替える。以後 1 秒の各ブロックの RMS の隣り合う比を見る。
        float maxRatioDb = 0.0f; double prev = -1.0;
        for (int p = 0; p < n; p += blk) {
            if (p == fs) fdn.setRt60(rtB);
            const int m = std::min(blk, n - p);
            fdn.render(in.data() + p, m, out.data() + p);
            if (p >= fs - blk) {
                double e = 0.0; for (int i = 0; i < m; ++i) e += static_cast<double>(out[static_cast<std::size_t>(p + i)]) * out[static_cast<std::size_t>(p + i)];
                const double rms = std::sqrt(e / m);
                if (prev > 0.0) maxRatioDb = std::max(maxRatioDb, static_cast<float>(std::fabs(20.0 * std::log10(std::max(rms, 1e-12) / prev))));
                prev = rms;
            }
        }
        // 自然な減衰 1 ブロック（5.3 ms）ぶんは RT60 0.3 s で 60·5.3/300 ≈ 1.1 dB。切り替えの段差はそれに近いはず。
        char buf[96]; std::snprintf(buf, sizeof(buf), "(隣り合うブロックの RMS 比の最大 %.2f dB)", maxRatioDb);
        check("[FDN] RT60 を 1.0 → 0.3 s に切り替えても隣り合うブロックの段差が 3 dB 以下", maxRatioDb <= 3.0f, buf);
    }
}

// ================================ [FDN・配線] 部屋ごとの FDN と戸口の結合 ── 扉を閉めると部屋が乾くか（手順 2）
// 08-24 の場面を数値で組む: 響く大部屋（14 m 角・高 3 m）と吸う小部屋（4×3×2.5 m）を 0.9 m の戸口でつなぎ、
// 音源とリスナーを小部屋に置く。実測（レイ）では扉を閉めると減衰 181 → 447 dB/s、RT60 0.33 → 0.13 s、
// 量は 0.1 dB しか動かなかった。配線で同じ向きが出るかを見る。
void testFdnRoomMix() {
    std::printf("\n[FDN・配線] 部屋ごとの FDN ＋ 戸口の結合 ── 扉を閉めると部屋が乾くか\n");
    const int fs = 48000, blk = 256;

    // 部屋。平均自由行程 4V/S を 12 ms（基準）で割って線の長さの倍率にする。
    const float V_B = 14.0f * 14.0f * 3.0f, S_B = 2.0f * (14.0f * 14.0f + 14.0f * 3.0f * 2.0f);   // 588 / 560
    const float V_S = 4.0f * 3.0f * 2.5f,   S_S = 2.0f * (4.0f * 3.0f + 4.0f * 2.5f + 3.0f * 2.5f); // 30 / 59
    const float mfp_B = 4.0f * V_B / S_B, mfp_S = 4.0f * V_S / S_S;                                   // 4.2 m / 2.0 m
    const float scale_B = (mfp_B / 343.0f) / 0.012f, scale_S = (mfp_S / 343.0f) / 0.012f;              // 1.02 / 0.49
    const float rtB[6] = { 1.2f, 1.1f, 1.0f, 0.9f, 0.75f, 0.6f };
    const float rtS[6] = { 0.16f, 0.15f, 0.13f, 0.12f, 0.11f, 0.10f };
    const float doorArea = 0.9f * 2.0f;
    // 幾何: 音源とリスナーは戸口から 1.5 m。戸口の立体角 ≈ A/d² → /4π がエネルギーの割合、振幅はその平方根。
    const float geo = std::sqrt((doorArea / (1.5f * 1.5f)) / (4.0f * 3.14159265f));                   // ≈ 0.25

    // 開口率 α で 1 場面を組み、インパルス応答（L）を作って返す。
    auto run = [&](float alpha, float seconds, std::vector<float>& outL, float* t60out, double* energyDb, af::dsp::FdnRoomMix** keep, float fitStart = 0.06f) {
        auto* mix = new af::dsp::FdnRoomMix(fs, blk);
        const int B = mix->addRoom(scale_B, rtB);
        const int S = mix->addRoom(scale_S, rtS);
        float one[6] = { 1, 1, 1, 1, 1, 1 };
        float wB[6]; for (int b = 0; b < 6; ++b) wB[b] = alpha * geo;      // リスナー←大部屋: 開口率 × 幾何
        mix->setListenerWeight(S, one);                                    // 自分の部屋
        mix->setListenerWeight(B, wB);
        const int n = static_cast<int>(seconds * fs);
        outL.assign(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> outR(static_cast<std::size_t>(n), 0.0f), in(static_cast<std::size_t>(blk), 0.0f);
        for (int p = 0; p < n; p += blk) {
            const int m = std::min(blk, n - p);
            std::fill(in.begin(), in.end(), 0.0f);
            if (p == 0) in[0] = 1.0f;                                      // インパルスを 1 回
            mix->add(S, in.data(), m, 1.0f);                               // 音源→自分の部屋
            mix->add(B, in.data(), m, alpha * geo);                        // 音源→戸口越しの大部屋
            mix->render(m, outL.data() + p, outR.data() + p);
        }
        // 小部屋の線は短い（倍率 0.49、最長 38 ms）。当てはめは 60 ms から（RT60 0.12 s の部屋で聞こえるのは
        // 最初の 30 dB ＝ 60 ms まで。250 ms では −120 dB で、拡散器や遅いモードの残りを測ってしまう）。
        std::vector<float> bands;
        fdntest::split6(outL.data(), n, fs, bands, 2);
        for (int b = 0; b < 6; ++b) t60out[b] = fdntest::fitT60(bands.data() + static_cast<std::size_t>(b) * n, n, fs, fitStart);
        double e = 0.0; for (float v : outL) e += static_cast<double>(v) * v;
        *energyDb = 10.0 * std::log10(std::max(e, 1e-30));
        if (keep) *keep = mix; else delete mix;
    };

    std::vector<float> irClosed, irOpen, irHalf;
    float tC[6], tO[6], tH[6]; double eC, eO, eH;
    // 閉は 60 ms から（RT60 0.12 s で聞こえる最初の 30 dB）。開・半開は 150 ms から
    // ＝小部屋の速い減衰が 75 dB 落ちて消えたあと。「後半の減衰が隣室の速さになる」を測る。
    run(0.0f, 3.0f, irClosed, tC, &eC, nullptr, 0.06f);
    run(1.0f, 3.0f, irOpen,   tO, &eO, nullptr, 0.15f);
    run(0.3f, 3.0f, irHalf,   tH, &eH, nullptr, 0.15f);

    std::printf("        線の倍率: 大部屋 %.2f（平均自由行程 %.1f m）／小部屋 %.2f（%.1f m）。戸口の幾何（振幅） %.2f\n",
                scale_B, mfp_B, scale_S, mfp_S, geo);
    std::printf("        帯域   小部屋の RT60   閉(α=0)     半開(α=0.3)  開(α=1)     大部屋の RT60   （閉は 60 ms から、開・半開は 150 ms から当てる）\n");
    static const char* nm[6] = { "125", "250", "500", "1k", "2k", "4k" };
    for (int b = 0; b < 6; ++b)
        std::printf("        %-5s %10.2f s %10.2f s %10.2f s %10.2f s %10.2f s\n", nm[b], rtS[b], tC[b], tH[b], tO[b], rtB[b]);
    std::printf("        量（IR のエネルギー）: 閉 %.2f dB ／ 半開 %.2f dB ／ 開 %.2f dB\n", eC, eH, eO);

    char buf[160];
    // 閉: 小部屋の自分の RT60（1 kHz で ±15%。低域はモードがまばらなので 1 kHz で見る）
    const float errC = (tC[3] - rtS[3]) / rtS[3];
    std::snprintf(buf, sizeof(buf), "(1 kHz: 閉 %.2f s 対 小部屋 %.2f s、%+.0f%%)", tC[3], rtS[3], errC * 100.0f);
    check("[FDN・配線] 閉めると小部屋の自分の RT60 で減る（1 kHz ±15%）", std::fabs(errC) <= 0.15f, buf);
    // 開: 減衰が明らかに遅くなる（≥ 2 倍）＝ 08-24 の 447 → 181 dB/s の向き
    std::snprintf(buf, sizeof(buf), "(1 kHz: 閉 %.2f s → 開 %.2f s、%.1f 倍)", tC[3], tO[3], tO[3] / std::max(tC[3], 1e-3f));
    check("[FDN・配線] 開けると減衰が 2 倍以上遅くなる（連成: 隣室の遅い尾が後半を支配する）", tO[3] >= 2.0f * tC[3], buf);
    // 量: 開閉で 1.5 dB 以内（08-24 は 0.1 dB）
    std::snprintf(buf, sizeof(buf), "(閉 %.2f dB → 開 %.2f dB、差 %.2f dB)", eC, eO, eO - eC);
    check("[FDN・配線] 開閉で量は 1.5 dB 以内しか動かない（形が変わり、量は変わらない）", std::fabs(eO - eC) <= 1.5, buf);
    // 半開: 遅い尾の量が α で単調（500 ms 時点の EDC の高さ）
    auto edcAt = [&](const std::vector<float>& x, float sec) {
        double tot = 0.0, tail = 0.0; const int i0 = static_cast<int>(sec * fs);
        for (int i = 0; i < static_cast<int>(x.size()); ++i) { const double v = static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)]; tot += v; if (i >= i0) tail += v; }
        return 10.0 * std::log10(std::max(tail, 1e-30) / std::max(tot, 1e-30));
    };
    const double lC = edcAt(irClosed, 0.5f), lH = edcAt(irHalf, 0.5f), lO = edcAt(irOpen, 0.5f);
    std::snprintf(buf, sizeof(buf), "(500 ms 以降の残りエネルギー: 閉 %.1f dB ／ 半開 %.1f dB ／ 開 %.1f dB)", lC, lH, lO);
    check("[FDN・配線] 遅い尾の量が開口率で単調に増える（閉 < 半開 < 開、3 dB 以上ずつ）", lH >= lC + 3.0 && lO >= lH + 3.0, buf);

    // 連続性: 鳴らしながら扉を 0.3 秒で閉める（α 1 → 0）。隣り合うブロックの段差を正弦で測る。
    {
        af::dsp::FdnRoomMix* mix = nullptr;
        std::vector<float> dummy; float tt[6]; double ee;
        run(1.0f, 0.02f, dummy, tt, &ee, &mix);      // 器を作るだけ（短く回す）
        const int S = 1, B = 0;
        const int n = fs * 2;
        std::vector<float> in(static_cast<std::size_t>(blk)), L(static_cast<std::size_t>(n)), R(static_cast<std::size_t>(n));
        float maxRatioDb = 0.0f; double prev = -1.0;
        for (int p = 0; p < n; p += blk) {
            const int m = std::min(blk, n - p);
            for (int i = 0; i < m; ++i) in[static_cast<std::size_t>(i)] = 0.1f * std::sin(2.0f * 3.14159265f * 220.0f * (p + i) / fs);
            // 1.0 秒から 0.3 秒かけて閉める。
            float alpha = 1.0f;
            if (p >= fs) alpha = std::max(0.0f, 1.0f - (p - fs) / (0.3f * fs));
            float wB[6]; for (int b = 0; b < 6; ++b) wB[b] = alpha * geo;
            mix->setListenerWeight(B, wB);
            mix->add(S, in.data(), m, 1.0f);
            mix->add(B, in.data(), m, alpha * geo);
            mix->render(m, L.data() + p, R.data() + p);
            if (p >= fs - blk) {
                double e = 0.0; for (int i = 0; i < m; ++i) e += static_cast<double>(L[static_cast<std::size_t>(p + i)]) * L[static_cast<std::size_t>(p + i)];
                const double rms = std::sqrt(e / m);
                if (prev > 0.0) maxRatioDb = std::max(maxRatioDb, static_cast<float>(std::fabs(20.0 * std::log10(std::max(rms, 1e-12) / prev))));
                prev = rms;
            }
        }
        delete mix;
        std::snprintf(buf, sizeof(buf), "(隣り合うブロックの RMS 比の最大 %.2f dB)", maxRatioDb);
        check("[FDN・配線] 鳴らしながら扉を 0.3 秒で閉めても隣り合うブロックの段差が 3 dB 以下", maxRatioDb <= 3.0f, buf);
    }

    // 安定: 開けたまま 4 秒回して膨らまない（結合の往復の利得 < 1）。
    {
        std::vector<float> ir; float tt[6]; double ee;
        run(1.0f, 4.0f, ir, tt, &ee, nullptr);
        auto rmsOf = [&](int from, int to) { double e = 0.0; for (int i = from; i < to; ++i) e += static_cast<double>(ir[static_cast<std::size_t>(i)]) * ir[static_cast<std::size_t>(i)]; return std::sqrt(e / std::max(1, to - from)); };
        const double r1 = rmsOf(fs / 2, fs), r2 = rmsOf(3 * fs, 4 * fs);
        std::snprintf(buf, sizeof(buf), "(0.5〜1 s の RMS %.5f → 3〜4 s %.5f)", r1, r2);
        check("[FDN・配線] 開けたまま 4 秒回しても膨らまない（配線は前向きだけ）", r2 < r1, buf);
    }
}

int main() {
    std::printf("=== DSP 数値回帰テスト（段4: C++ 移行）===\n");
    testFft();
    testPartitionedConvolver();
    testNonUniformConvolver();
    testReverbTailIr();
    testHrtf();
    testHrtfProcessor();
    testEarlyReflectConv();
    diagnoseBandResponse();
    testTailSwapContinuity();
    testTailRebuildLevelStability();
    testSharedTailBusEquivalence();
    diagnoseDspCost();
    testDiffractionHrtf();
    testTapEarCues();
    testDirectionBus();
    testTailCalibration();
    testVoiceTailBus();
    testVoiceRenderer();
    diagnoseTailCatchUp();
    diagnoseTailTrackingLag();
    testFdnTail();
    testFdnRoomMix();

    std::printf("\n----\n");
    if (g_failures == 0) {
        std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);
    return 1;
}
