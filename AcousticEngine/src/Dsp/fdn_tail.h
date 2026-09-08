// fdn_tail.h ── 後期残響の尾を FDN（帰還遅延網）で生成する。2026-09-09。
//
// ■ なぜこれか（docs/TAIL_FDN_PLAN.md）
//   尾は mixing time より後の拡散音場で、耳が聞き分けるのは帯域別の減衰・量・方向という統計。
//   幾何の細部（到達時刻・面の形・扉の開口の色）は mixing time より前を面の線音源が持つ。
//   だから尾は「測って再生する」（実測エコグラム → IR → 畳み込み）でなく
//   「特徴を数値にして生成する」で足りる。生成器なら
//     ・変わる物が全部ゲインか係数で、状態（遅延線の中身）は入れ替わらない → 連続性が構造で出る
//     ・扉は隣室の FDN への配線のゲインになる → 状態を数え上げて焼く必要が無い
//     ・費用が固定で RT60 の長さに依らない
//
// ■ 作り（帯域ごとに独立した Jot 型 FDN を 6 本）
//   入力 → 4 段の直列 allpass（拡散。粒感・金属感を消す）
//        → 直列クロスオーバーで 6 帯域に分ける（EarlyReflectConv / ReverbTailIr と同じ 177/354/707/1414/2828 Hz）
//        → 帯域 b の信号は FDN_b へ: 16 本の遅延線（互いに素っぽい長さ。C# 旧版で詰めた値）
//           各線の出力に**スカラ**の減衰 g[b][i] → 高速 Walsh–Hadamard × 1/√N（無損失の混合）→ 各線の入口へ
//        → 出力は 6 本の FDN の読み出しの和 × 1/√N。
//
//   ★入口のクロスオーバーは 4 次（Linkwitz–Riley、2 次 Butterworth を 2 段）。EarlyReflectConv の
//     2 次より急にしてある。理由: 2 次だと 1414 Hz の境で 2 kHz の成分の 1/4 が下の輪（RT60 が長い）に
//     入り、聞こえる 2 kHz の減衰が長くなる（実測: 目標 0.45 s に対し 2 次の分析で 0.73 s）。
//     4 次にすると境の 1 オクターブ上で −24 dB → 漏れは 1/16。切る周波数は同じ。
//   ★退けた書き方: 1 本の FDN の輪の中で各線の出力を 6 帯域に分けて帯域ごとの減衰を掛ける。
//     クロスオーバー（2 次 lowpass、12 dB/oct）は帯域が重なるので、たとえば 2 kHz の成分の一部が
//     隣の帯域の減衰で回る。**輪の中で分けると漏れが 1 周ごとに積み重なり、減衰の遅い隣の帯域が
//     時間とともに勝つ**。実測: 高域ほど短い RT60（1.2/1.0/0.8/0.6/0.45/0.3 s）を与えて
//     T60 が +1.5 / +18 / +31 / +49 / +62 / +34 % 長く出た（EDC 当てはめ）。
//     帯域ごとに輪を分ければ漏れは入口の 1 回だけで、各輪の減衰は厳密に g_b になる。
//     費用は遅延線が 6 倍になるが、輪の中のフィルタ（16 本 × 5 段の biquad）が消えるので同程度。
//
// ■ 減衰の係数（Jot）
//   線 i（長さ L_i サンプル）が 1 周するごとに g = 10^(−3·L_i / (fs·T60_b))。T60_b 秒で 60 dB 落ちる。
//   線ごとに長さに比例した減衰を与えるので、混合後の全モードが同じ速さで減る（Jot の条件）。
//   ★「高域が先に減る」は係数で捏造しない。T60_b は部屋グラフの Sabine（材質＋空気吸収）から来る。
//
// ■ 量の正規化
//   帯域ごとに、その帯域のインパルス応答のエネルギーが約 1 になる入力ゲイン √(1 − ḡ_b²)。
//   これで尾の**スペクトルは入力のスペクトルのまま**（帯域ごとに量が 1）で、RT60 の違いは
//   減衰の速さにだけ出る。ReverbTailIr と同じ「エネルギー 1」の規約なので、尾の量の規則
//   freeFieldDirect × √tailRatio × tailSrcLevel がそのまま載る。検査で ±2 dB を確かめる。
//
// ■ 連続性
//   setRt60 は目標を置くだけ。render が 1 ブロックの中で係数を線形に目標へ寄せる。
//   状態は触らないので、いま溜まっているエネルギーが次のサンプルから新しい速さで減るだけ。
//   ★遅延線の長さは実行時に変えない（変えると音程が動く）。部屋の違いは係数・量・開始時刻で出す。
//
// ■ スレッド規約
//   setRt60 … 制御スレッド（目標を書いて版を上げる）
//   render  … オーディオスレッド（確保・ロックなし。版が変わっていれば目標を取り込む）
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace af {
namespace dsp {

class FdnTail {
public:
    static constexpr int kNumBands = 6;
    static constexpr int kLines = 16;
    static constexpr int kAllpass = 4;

    /// diffusion : 入力 allpass の係数（0 で無効。0.5〜0.7 が定番）
    explicit FdnTail(int sampleRate, float diffusion = 0.6f)
        : fs_(std::max(8000, sampleRate)), diffusion_(clampf(diffusion, 0.0f, 0.95f)) {
        // 遅延線の長さ（ms）。C# 旧版（IrConvolver）で 4 ＝ 金属的／8／16 と詰めた値。互いに素っぽく広く分散。
        static const float kLineMs[kLines] = {
            15.3f, 18.1f, 21.7f, 25.3f, 29.1f, 33.7f, 37.9f, 42.3f,
            46.7f, 51.1f, 55.9f, 60.3f, 65.1f, 69.7f, 73.9f, 78.3f };
        static const float kApMs[kAllpass] = { 7.3f, 9.9f, 12.7f, 15.1f };
        static const float kCrossHz[kNumBands - 1] = { 177.0f, 354.0f, 707.0f, 1414.0f, 2828.0f };

        std::size_t total = 0;
        for (int i = 0; i < kLines; ++i) {
            lineLen_[i] = std::max(1, static_cast<int>(std::lround(kLineMs[i] * 0.001f * fs_)));
            lineOff_[i] = total; total += static_cast<std::size_t>(lineLen_[i]);
        }
        bandStride_ = total;                                   // 帯域ごとに同じ並びの遅延線
        lineBuf_.assign(bandStride_ * kNumBands, 0.0f);
        for (int b = 0; b < kNumBands; ++b)
            for (int i = 0; i < kLines; ++i) linePos_[b][i] = 0;

        total = 0;
        for (int k = 0; k < kAllpass; ++k) {
            apLen_[k] = std::max(1, static_cast<int>(std::lround(kApMs[k] * 0.001f * fs_)));
            apOff_[k] = total; total += static_cast<std::size_t>(apLen_[k]);
            apPos_[k] = 0;
        }
        apBuf_.assign(total, 0.0f);
        for (int c = 0; c < kNumBands - 1; ++c)
            for (int s = 0; s < 2; ++s) split_[c][s].setLowpass(kCrossHz[c], static_cast<float>(fs_));

        float rt[kNumBands] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        computeGains(rt, gainCur_, inGainCur_);
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) gainTgt_[b][i] = gainCur_[b][i];
            inGainTgt_[b] = inGainCur_[b];
            rt60_[b] = rt[b];
        }
        version_.store(0, std::memory_order_release);
        seen_ = 0;
    }

    int sampleRate() const { return fs_; }

    // ── 制御スレッド ──

    /// 帯域別の残響時間(s)。部屋グラフの Sabine から毎フレーム渡してよい（目標を置くだけ）。
    void setRt60(const float* rt60Sec6) {
        if (!rt60Sec6) return;
        float g[kNumBands][kLines]; float ig[kNumBands];
        computeGains(rt60Sec6, g, ig);
        // 目標を書いてから版を上げる。render は版を見て取り込む。
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) pendGain_[b][i] = g[b][i];
            pendInGain_[b] = ig[b];
            rt60_[b] = rt60Sec6[b];
        }
        version_.fetch_add(1, std::memory_order_release);
    }

    /// 状態を捨てる（場面の切り替え用。通常は呼ばない ── 連続性が消える）。
    void reset() {
        std::fill(lineBuf_.begin(), lineBuf_.end(), 0.0f);
        std::fill(apBuf_.begin(), apBuf_.end(), 0.0f);
        for (int c = 0; c < kNumBands - 1; ++c) { split_[c][0].clear(); split_[c][1].clear(); }
    }

    /// 診断: 帯域 b の入力ゲインと、帯域 b・線 i の 1 周あたりの減衰。
    float inputGain(int band = 3) const { return (band >= 0 && band < kNumBands) ? inGainCur_[band] : 0.0f; }
    float gain(int band, int line) const {
        if (line < 0 || line >= kLines || band < 0 || band >= kNumBands) return 0.0f;
        return gainCur_[band][line];
    }
    float rt60(int band) const { return (band >= 0 && band < kNumBands) ? rt60_[band] : 0.0f; }

    // ── オーディオスレッド ──

    /// in をモノラルで frames サンプル入れ、尾を out に**上書き**する。in と out は別の配列。
    ///   係数は 1 ブロックの中で目標へ線形に寄る（連続性の担保はここ）。
    void render(const float* in, int frames, float* out) { renderImpl(in, frames, out, nullptr); }
    /// 診断: 帯域ごとの輪の出力を別々に書く（outBands[b][n]）。減衰の検査が輪を直接測るのに使う。
    void renderBands(const float* in, int frames, float* const* outBands) { renderImpl(in, frames, nullptr, outBands); }

private:
    void renderImpl(const float* in, int frames, float* out, float* const* outBands) {
        if ((!out && !outBands) || frames <= 0) return;
        // 版が変わっていれば目標を取り込む（浮動小数の書きは版の release より前、読みは acquire の後）。
        const int v = version_.load(std::memory_order_acquire);
        if (v != seen_) {
            for (int b = 0; b < kNumBands; ++b) {
                for (int i = 0; i < kLines; ++i) gainTgt_[b][i] = pendGain_[b][i];
                inGainTgt_[b] = pendInGain_[b];
            }
            seen_ = v;
        }
        // ブロック内の線形ランプ。
        const float inv = 1.0f / static_cast<float>(frames);
        float gStep[kNumBands][kLines]; float igStep[kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) gStep[b][i] = (gainTgt_[b][i] - gainCur_[b][i]) * inv;
            igStep[b] = (inGainTgt_[b] - inGainCur_[b]) * inv;
        }
        const float oNorm = 1.0f / std::sqrt(static_cast<float>(kLines));

        for (int n = 0; n < frames; ++n) {
            for (int b = 0; b < kNumBands; ++b) {
                for (int i = 0; i < kLines; ++i) gainCur_[b][i] += gStep[b][i];
                inGainCur_[b] += igStep[b];
            }

            // 入力の拡散（直列 allpass）→ 6 帯域へ。
            float x = in ? in[n] : 0.0f;
            if (diffusion_ > 0.0f)
                for (int k = 0; k < kAllpass; ++k) x = allpass(k, x);
            float xb[kNumBands];
            {
                float rest = x;
                for (int c = 0; c < kNumBands - 1; ++c) {
                    const float lo = split_[c][1].process(split_[c][0].process(rest));
                    xb[c] = lo; rest -= lo;
                }
                xb[kNumBands - 1] = rest;
            }

            float sum = 0.0f;
            for (int b = 0; b < kNumBands; ++b) {
                float* base = lineBuf_.data() + bandStride_ * static_cast<std::size_t>(b);
                float f[kLines]; float bsum = 0.0f;
                for (int i = 0; i < kLines; ++i) {
                    const float y = base[lineOff_[i] + static_cast<std::size_t>(linePos_[b][i])];
                    bsum += y;
                    f[i] = y * gainCur_[b][i];
                }
                sum += bsum;
                if (outBands) outBands[b][n] = bsum * oNorm;
                // 高速 Walsh–Hadamard（無損失の混合）。
                for (int len = 1; len < kLines; len <<= 1)
                    for (int j = 0; j < kLines; j += len << 1)
                        for (int k = j; k < j + len; ++k) {
                            const float a = f[k], c = f[k + len];
                            f[k] = a + c; f[k + len] = a - c;
                        }
                const float xin = xb[b] * inGainCur_[b];
                for (int i = 0; i < kLines; ++i) {
                    base[lineOff_[i] + static_cast<std::size_t>(linePos_[b][i])] = xin + f[i] * oNorm;
                    if (++linePos_[b][i] >= lineLen_[i]) linePos_[b][i] = 0;
                }
            }
            if (out) out[n] = sum * oNorm;
        }
        // 丸め誤差の蓄積を止める（目標に着いたことにする）。
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) gainCur_[b][i] = gainTgt_[b][i];
            inGainCur_[b] = inGainTgt_[b];
        }
    }

    // RBJ バイカッド（直接形II転置）。EarlyReflectConv / ReverbTailIr と同じ物。
    //   ★帯域の切り方を系全体で 1 つにするため、定数も式も揃えてある。変えるときは 3 か所とも。
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void setLowpass(float fc, float fs) {
            const float w0 = 2.0f * 3.14159265358979323846f * fc / fs;
            const float cw = std::cos(w0), sw = std::sin(w0);
            const float alpha = sw / (2.0f * 0.70710678f);
            b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; b2 = (1.0f - cw) * 0.5f;
            const float a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
            b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
            z1 = 0.0f; z2 = 0.0f;
        }
        void clear() { z1 = 0.0f; z2 = 0.0f; }
        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Schroeder allpass 1 段。位相だけ撹拌して振幅特性は平坦。
    float allpass(int k, float x) {
        float* buf = apBuf_.data() + apOff_[k];
        const float d = buf[apPos_[k]];
        const float g = diffusion_;
        const float v = x - g * d;
        buf[apPos_[k]] = v;
        if (++apPos_[k] >= apLen_[k]) apPos_[k] = 0;
        return d + g * v;
    }

    // Jot の減衰係数と、帯域ごとのエネルギー 1 の入力ゲイン。
    void computeGains(const float* rt60, float (*g)[kLines], float* inGain) const {
        for (int b = 0; b < kNumBands; ++b) {
            // RT60 は 0.05〜30 秒に丸める。短すぎると係数が 0 に張り付き、長すぎると無損失に近づく。
            const double t60 = std::min(30.0, std::max(0.05, static_cast<double>(rt60[b])));
            double sumG2 = 0.0;
            for (int i = 0; i < kLines; ++i) {
                const double Li = static_cast<double>(lineLen_[i]);
                double gv = std::pow(10.0, -3.0 * Li / (static_cast<double>(fs_) * t60));
                if (gv > 0.9999) gv = 0.9999;          // 無損失の輪を作らない
                g[b][i] = static_cast<float>(gv);
                sumG2 += gv * gv;
            }
            const double meanG2 = sumG2 / static_cast<double>(kLines);
            inGain[b] = static_cast<float>(std::sqrt(std::max(1.0 - meanG2, 1e-3)));
        }
    }

    const int fs_;
    const float diffusion_;

    std::vector<float> lineBuf_;          // [帯域][線][時間]
    std::size_t bandStride_ = 0;
    std::size_t lineOff_[kLines] = {};
    int lineLen_[kLines] = {};
    int linePos_[kNumBands][kLines] = {};

    std::vector<float> apBuf_;
    std::size_t apOff_[kAllpass] = {};
    int apLen_[kAllpass] = {};
    int apPos_[kAllpass] = {};

    Biquad split_[kNumBands - 1][2];      // 入口の直列クロスオーバー（LR4 ＝ 2 段。1 組だけ）

    float gainCur_[kNumBands][kLines] = {};
    float gainTgt_[kNumBands][kLines] = {};
    float pendGain_[kNumBands][kLines] = {};
    float inGainCur_[kNumBands] = {}, inGainTgt_[kNumBands] = {}, pendInGain_[kNumBands] = {};
    float rt60_[kNumBands] = {};
    std::atomic<int> version_{0};
    int seen_ = 0;
};

}  // namespace dsp
}  // namespace af
