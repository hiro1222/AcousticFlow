// dsp_regression.cpp — DSP（段4: C++ 移行）の数値回帰テスト。
//
// 方針は scene_regression.cpp と同じで、**絶対値ではなく関係**を確かめる。
// ただし FFT だけは「素朴な DFT と一致する」という絶対の正解があるので、それで押さえる。
// 移行は「Unity C# 版と同じ音が出る」ことが要件なので、各段で参照実装と突き合わせる。
#include <cmath>
#include <cstdio>
#include <vector>

#include <algorithm>

#include "../AcousticEngine/src/Dsp/fft.h"
#include "../AcousticEngine/src/Dsp/partitioned_convolver.h"
#include "../AcousticEngine/src/Dsp/nonuniform_convolver.h"
#include "../AcousticEngine/src/Dsp/reverb_tail_ir.h"
#include "../AcousticEngine/src/Dsp/hrtf_set.h"
#include "../AcousticEngine/src/Dsp/hrtf_processor.h"
#include "../AcousticEngine/src/Dsp/early_reflect_conv.h"
#include "../AcousticEngine/src/Dsp/voice_renderer.h"

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
        c3.processAdd(in.data(), 0, m, false, ol.data(), orr.data(), 0, 1.0f, dd.data());
        double pIn = 0.0, pSpec = 0.0, pDiff = 0.0;
        for (int i = m / 2; i < m; ++i) {
            pIn += in[static_cast<std::size_t>(i)] * in[static_cast<std::size_t>(i)];
            pSpec += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
            pDiff += dd[static_cast<std::size_t>(i)] * dd[static_cast<std::size_t>(i)];
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
    testTailCalibration();
    testVoiceRenderer();

    std::printf("\n----\n");
    if (g_failures == 0) {
        std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);
    return 1;
}
