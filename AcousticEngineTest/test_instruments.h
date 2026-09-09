/* AcousticEngineTest/test_instruments.h ── 検査の物差し（段 1）
 *
 * ■ 役割
 *   新コアの検査（flow_regression.cpp）が使う「測る道具」。合否の期待値は持たない。
 *   旧 scene_regression.cpp / dsp_regression.cpp から抜き出した物差しを、1 か所に集めた。
 *   ★期待値は旧コアの物なので捨てたが、**物差しを捨てると同じ穴を踏む**（地図 4 章）。
 *
 * ■ 中の仕組み
 *   1) check / dB: 合否を数えて印字する最小の枠。落ちると終了コード 1。
 *   2) Bq / split6: 出力を 6 帯域に分ける直列クロスオーバー。境目は world_rules::kCrossHz（DSP と同じ）。
 *      order=2 で LR4（4 次）。帯域の**信号**の和は元に完全に戻る（rest = 元 − Σ低域、構成上）。
 *      ★帯域の**エネルギー**の和は元より大きい（境目で隣と重なる。白色雑音で約 +2 dB）。
 *        帯域の数字は同じ物差しどうしで比べること。「和が元と一致」を検査に書いて 1 回落ちた（段 1）。
 *   3) fitT60: Schroeder の後方積分（EDC）で減衰を当てる。startSec から 20 dB 落ちるまでを直線に。
 *      ★包絡の山から追うと立ち上がりを減衰と読む（旧実装で傾きが 30〜80% 浅く出た）。EDC は単調なので騙されない。
 *   4) blockLevelSteps: 隣り合うブロックの RMS の比（dB）の最大。**歩行の連続性**の物差し。
 *      「行動していないときは揺れない」（設計文書 追記 D）はこれで測る。
 *   5) sampleStepRatio: ブロック内の「隣り合うサンプルの最大差 ÷ RMS」。クリック（波形の飛び）の物差し。
 *      正弦なら平常 0.04 前後。音色の変化では動かず、飛びだけに出る。
 *   6) SineSum: 5 つの正弦の和。★白色雑音では段差が埋もれて見えず、単一の正弦は経路長の変化で櫛が立つ。
 *      和なら両方を避けられる（旧 [歩行] で実証）。
 *      ★★周波数は fs/block の整数倍（**ブロックと同期**）にする。そうでないと 5 音のうねりの周期が
 *        ブロックより長く、静止していてもブロックの RMS が ±3 dB 動いて「揺れ」に見える。
 *        旧 [歩行] の教訓そのもので、段 1 で一度そのまま踏んだ（6.1 dB 出た）。
 *
 * ■ 繋がり
 *   受ける: 音の配列（float）。
 *   渡す:   数字。flow_regression.cpp が check に掛ける。
 *
 * ■ 退けた書き方
 *   ・各テストで測り方を書く: 旧 scene_regression.cpp は 14,511 行のうち相当が物差しの重複だった。
 *   ・FFT で帯域を切る: DSP 側は IIR のクロスオーバーで切っているので、同じ物差しで測らないと
 *     「帯域の色」の検査が DSP の実装と食い違う。
 *
 * ■ 壊れる所
 *   ・fitT60 の startSec を短くしすぎると、FDN の線が混ざり切る前の階段を減衰と読む（線の最長の約 3 周を待つ）。
 *     合成の指数減衰なら 0.02 s でよい。
 *   ・blockLevelSteps を白色雑音で測ると、雑音そのものの揺れ（±1 dB）が出る。正弦の和で測ること。
 */
#ifndef ACOUSTICFLOW_TEST_INSTRUMENTS_H
#define ACOUSTICFLOW_TEST_INSTRUMENTS_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../AcousticEngine/src/Flow/world_rules.h"

namespace afti {

inline int g_checks = 0;
inline int g_failures = 0;

inline void check(const char* label, bool ok, const char* detail = "") {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  [%s] %s %s\n", ok ? "OK" : "FAIL", label, detail);
}
inline int finish(const char* suite) {
    if (g_failures == 0) std::printf("[OK] %s: %d 件のチェックすべてに合格しました。\n", suite, g_checks);
    else std::printf("[FAIL] %s: %d / %d 件のチェックに失敗しました。\n", suite, g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
inline double dB(double e) { return 10.0 * std::log10(std::max(e, 1e-30)); }

// ── 帯域分割（DSP と同じ直列クロスオーバー）──
struct Bq {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setLowpass(float fc, float fs) {
        const float w0 = 2.0f * 3.14159265358979323846f * fc / fs;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float alpha = sw / (2.0f * 0.70710678f);          // Q = 1/√2（Butterworth）
        b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; b2 = (1.0f - cw) * 0.5f;
        const float a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    float process(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
/// out[b*n + i]。order = 1: 2 次／2: 4 次（LR4）。
inline void split6(const float* src, int n, int fs, std::vector<float>& out, int order = 2) {
    using acoustic::flow::kCrossHz;
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
inline double energy(const float* x, int n) {
    double e = 0.0; for (int i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i]; return e;
}
/// 帯域ごとのエネルギー（dB）。
inline void bandEnergyDb(const float* src, int n, int fs, double* out6) {
    std::vector<float> bands; split6(src, n, fs, bands, 2);
    for (int b = 0; b < 6; ++b) out6[b] = dB(energy(bands.data() + static_cast<std::size_t>(b) * n, n));
}

// ── 減衰（EDC）──
inline float fitT60(const float* x, int n, int fs, float startSec, float* outSlopeDbPerSec = nullptr) {
    std::vector<double> edc(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = n - 1; i >= 0; --i) edc[static_cast<std::size_t>(i)] = edc[static_cast<std::size_t>(i) + 1] + static_cast<double>(x[i]) * x[i];
    const double e0 = edc[0];
    if (e0 <= 1e-30) return -1.0f;
    const int step = std::max(1, fs / 1000);
    const int i0 = std::min(n - 1, static_cast<int>(startSec * fs));
    const double d0 = dB(edc[static_cast<std::size_t>(i0)] / e0);
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (int i = i0; i < n; i += step) {
        const double d = dB(edc[static_cast<std::size_t>(i)] / e0);
        if (d < d0 - 20.0) break;
        const double t = static_cast<double>(i) / fs;
        sx += t; sy += d; sxx += t * t; sxy += t * d; ++m;
    }
    if (m < 4) return -1.0f;
    const double slope = (m * sxy - sx * sy) / std::max(m * sxx - sx * sx, 1e-12);
    if (outSlopeDbPerSec) *outSlopeDbPerSec = static_cast<float>(slope);
    return (slope < -1e-6) ? static_cast<float>(-60.0 / slope) : -1.0f;
}

// ── 連続性（歩行）とクリック ──
/// 隣り合うブロックの RMS の比（dB）の最大。skipBlocks で立ち上がりを飛ばす。
inline double blockLevelSteps(const float* x, int n, int block, int skipBlocks = 2) {
    double prev = -1.0, worst = 0.0; int k = 0;
    for (int p = 0; p + block <= n; p += block, ++k) {
        const double e = energy(x + p, block) / block;
        if (k >= skipBlocks && prev > 0.0 && e > 0.0) worst = std::max(worst, std::fabs(dB(e) - dB(prev)));
        prev = e;
    }
    return worst;
}
/// ブロック内の「隣り合うサンプルの最大差 ÷ RMS」。
inline double sampleStepRatio(const float* x, int n) {
    double maxd = 0.0;
    for (int i = 1; i < n; ++i) maxd = std::max(maxd, static_cast<double>(std::fabs(x[i] - x[i - 1])));
    const double rms = std::sqrt(energy(x, n) / std::max(n, 1));
    return (rms > 1e-9) ? maxd / rms : 0.0;
}

// ── 信号 ──
struct SineSum {
    double phase[5] = {0, 0, 0, 0, 0};
    double hz[5] = {};
    float amp = 0.2f;
    int fs = 48000;
    // 基本 = fs/block（512 なら 93.75 Hz）の 2・3・5・7・11 倍。互いに素なので上音が重ならず、
    // どの音もブロックに整数周期で収まる（＝ブロックの RMS が信号自身では動かない）。
    SineSum(int fs_, int block) : fs(fs_) {
        const double base = static_cast<double>(fs_) / block;
        const int m[5] = {2, 3, 5, 7, 11};
        for (int k = 0; k < 5; ++k) hz[k] = base * m[k];
    }
    float next() {
        float s = 0.0f;
        for (int k = 0; k < 5; ++k) {
            s += static_cast<float>(std::sin(phase[k])) * amp;
            phase[k] += 2.0 * 3.14159265358979323846 * hz[k] / fs;
            if (phase[k] > 2.0 * 3.14159265358979323846) phase[k] -= 2.0 * 3.14159265358979323846;
        }
        return s;
    }
};
struct Xorshift {
    unsigned int s = 20260910u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<int>(s) * (1.0f / 2147483648.0f); }
};

}  // namespace afti

#endif  // ACOUSTICFLOW_TEST_INSTRUMENTS_H
