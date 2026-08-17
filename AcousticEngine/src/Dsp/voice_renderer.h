// voice_renderer.h — 1 音源ぶんの信号フローを束ねる。
//
//   dry(モノラル)
//     ├─ 早期反射マルチタップ ──┬→ 鏡面 L/R ──────────────┐
//     │                          ├→ 直接音(モノ) → HRTF ──┤
//     │                          └→ 拡散送り → allpass ───┤→ ×outputGain → L/R
//     └─ 後期尾(非一様分割畳み込み) ─────────────────────┘
//
// 各部品は個別にテスト済み（dsp_regression.cpp）。ここが担うのは**配線と段別の計測**だけ。
//
// 拡散器を早期反射と分けている理由:
//   拡散器は固定ネットワークなので、タップ集合が切り替わっても状態は連続でよい
//   （送り込む信号側は EarlyReflectConv がクロスフェード済み）。
//   L/R で互いに素っぽい別の長さにして、拡散成分を左右で相関させない。
//
// スレッド規約:
//   setTaps / setTailIr / setDirection … 制御スレッド
//   render()                            … オーディオスレッド（確保・ロックなし）
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/IrConvolver.cs の OnAudioFilterRead。
// FDN 尾（tailMode=Fdn）は比較用の旧方式なので移していない。本命は実測エコグラム由来の尾。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "early_reflect_conv.h"
#include "hrtf_processor.h"
#include "nonuniform_convolver.h"
#include "reverb_tail_ir.h"

namespace af {
namespace dsp {

// Schroeder allpass の直列。位相だけ撹拌して振幅特性は平坦（＝音色を変えずに滲ませる）。
class AllpassChain {
public:
    void init(const float* delaysMs, int count, int sampleRate) {
        len_.resize(static_cast<std::size_t>(count));
        pos_.assign(static_cast<std::size_t>(count), 0);
        offset_.resize(static_cast<std::size_t>(count));
        int total = 0;
        for (int i = 0; i < count; ++i) {
            len_[static_cast<std::size_t>(i)] =
                std::max(1, static_cast<int>(std::lround(delaysMs[i] * 0.001f * sampleRate)));
            offset_[static_cast<std::size_t>(i)] = total;
            total += len_[static_cast<std::size_t>(i)];
        }
        buf_.assign(static_cast<std::size_t>(total), 0.0f);
    }

    float process(float x, float g) {
        const int n = static_cast<int>(len_.size());
        for (int i = 0; i < n; ++i) {
            float* b = buf_.data() + offset_[static_cast<std::size_t>(i)];
            int& p = pos_[static_cast<std::size_t>(i)];
            const float bo = b[p];
            const float di = x + g * bo;
            b[p] = di;
            x = bo - g * di;
            if (++p >= len_[static_cast<std::size_t>(i)]) p = 0;
        }
        return x;
    }

private:
    std::vector<float> buf_;
    std::vector<int> len_, pos_, offset_;
};

class VoiceRenderer {
public:
    // 段別の計測（このブロックの RMS）。内訳が読めないと調整できないので分けて出す。
    struct Metering {
        float rmsDirect = 0.0f;    // 直接音（HRTF 後）
        float rmsEarly = 0.0f;     // 早期反射の鏡面成分（直接音を除く）
        float rmsScatter = 0.0f;   // 散乱スメア
        float rmsTail = 0.0f;      // 後期尾
        float rmsOut = 0.0f;       // 最終出力
    };

    struct Config {
        int sampleRate = 48000;
        int maxFrames = 4096;        // 1回の render で処理する最大サンプル数
        float tailSeconds = 1.0f;    // 後期尾の長さ
        int earlyMaxDelaySamples = 0;// 0 なら 120ms 相当を自動で確保
        float tapCrossfadeMs = 30.0f;
        float hrtfCrossfadeMs = 12.0f;
        float hrtfCrossoverHz = 700.0f;
        int tailFirstBlock = 64;     // 非一様分割の最小ブロック（＝尾の遅延）
        int tailCapBlock = 8192;
    };

    explicit VoiceRenderer(const Config& cfg)
        : cfg_(cfg),
          sampleRate_(std::max(8000, cfg.sampleRate)),
          tailSamples_(std::max(1, static_cast<int>(cfg.tailSeconds * sampleRate_))),
          early_(sampleRate_,
                 cfg.earlyMaxDelaySamples > 0 ? cfg.earlyMaxDelaySamples
                                              : static_cast<int>(0.120f * sampleRate_),
                 cfg.tapCrossfadeMs),
          hrtf_(sampleRate_, cfg.hrtfCrossfadeMs, cfg.hrtfCrossoverHz),
          tailConv_(tailSamples_, 2, cfg.tailFirstBlock, cfg.tailCapBlock, cfg.maxFrames),
          tailIr_(sampleRate_, tailSamples_, 2) {
        const float scatL[3] = {3.7f, 6.1f, 9.7f};
        const float scatR[3] = {4.3f, 7.3f, 11.3f};
        diffL_.init(scatL, 3, sampleRate_);
        diffR_.init(scatR, 3, sampleRate_);

        const std::size_t mf = static_cast<std::size_t>(std::max(64, cfg.maxFrames));
        tailOutL_.assign(mf, 0.0f);
        tailOutR_.assign(mf, 0.0f);
        maxFrames_ = static_cast<int>(mf);
    }

    VoiceRenderer(const VoiceRenderer&) = delete;
    VoiceRenderer& operator=(const VoiceRenderer&) = delete;

    // ── 制御スレッド ──

    void setTaps(const EarlyReflectConv::Tap* taps, int count) { early_.setTaps(taps, count); }

    /// HRTF データセットを差し替える。set は呼び手が生存を保証する。
    void setHrtfSet(const HrtfSet* set) { hrtf_.setHrtfSet(set); }
    void setHrtfEnabled(bool on) { hrtfEnabled_ = on; }
    void setDirection(const float dirListenerLocal[3], float headCircumferenceCm) {
        hrtf_.setDirection(dirListenerLocal, headCircumferenceCm);
    }

    /// 実測エコグラムから後期尾を組み直す。戻り値は 尾/直接 のエネルギー比。
    ///   directGain は直接音タップの広帯域ゲイン（距離減衰・透過込み＝絶対レベルの基準）。
    ///   targetRatio は残響/直接エネルギーの目標比（呼び手が物理式から算出）。
    float rebuildTail(const float* echoBands, int binCount, float binMs, float startMs,
                      float fadeMs, float smoothMs, float smoothGrowth, float envAlpha,
                      float directGain, float targetRatio,
                      const float* earBandGain = nullptr, int earBandGainLen = 0) {
        const float ratio = tailIr_.build(echoBands, binCount, binMs, startMs, fadeMs,
                                          smoothMs, smoothGrowth, envAlpha,
                                          earBandGain, earBandGainLen);
        if (ratio <= 0.0f) { tailGain_ = 0.0f; return 0.0f; }
        const float* ir[2] = { tailIr_.ir(0), tailIr_.ir(1) };
        const int len[2] = { tailIr_.length(), tailIr_.length() };
        tailConv_.setIr(ir, len);
        // 尾IRはエネルギー1に正規化済み。絶対レベルは D/R 比から一意に決まる。
        tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
        return ratio;
    }

    void setOutputGain(float g) { outputGain_ = g; }
    void setTailLevel(float g) { tailLevel_ = g; }        // 1.0 が物理どおり。好みの微調整
    void setScatterDiffusion(float g) { scatterDiffusion_ = g; }

    /// 尾の絶対ゲインの掛かり方。
    ///   srcLevel : 音源がこの部屋にどれだけエネルギーを注げているか（反射込みの生存）。
    ///   wet      : **尾のレベルには掛けない**。残響/直接比は rebuildTail に渡す target で
    ///              既に決まっているので、ここで wet を掛けると二重計上になる
    ///              （wet 自体が target から作られているため、なおさら）。
    ///              引数は互換のために残してあるが、診断で覗く以外の用途は無い。
    void setTailEnvelope(float wet, float srcLevel) {
        tailWet_ = clamp01(wet);
        tailSrcLevel_ = (srcLevel > 0.0f) ? srcLevel : 1.0f;
    }

    const char* hrtfName() const { return hrtf_.setName(); }
    int tailPartitions() const { return tailConv_.totalPartitions(); }
    int tailLatency() const { return tailConv_.latency(); }

    // ── オーディオスレッド ──

    /// モノラル入力 → ステレオ出力（**上書き**）。frames は maxFrames 以下に分割して回す。
    void render(const float* input, int frames, float* outL, float* outR, Metering* meter = nullptr) {
        if (!input || !outL || !outR || frames <= 0) return;
        Metering m;
        int done = 0;
        while (done < frames) {
            const int n = std::min(frames - done, maxFrames_);
            renderChunk(input + done, n, outL + done, outR + done, m);
            done += n;
        }
        const float inv = 1.0f / static_cast<float>(std::max(1, frames));
        m.rmsDirect = std::sqrt(m.rmsDirect * inv);
        m.rmsEarly = std::sqrt(m.rmsEarly * inv);
        m.rmsScatter = std::sqrt(m.rmsScatter * inv);
        m.rmsTail = std::sqrt(m.rmsTail * inv);
        m.rmsOut = std::sqrt(m.rmsOut * inv);
        if (meter) *meter = m;
    }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    void renderChunk(const float* input, int n, float* outL, float* outR, Metering& m) {
        // ① 後期尾はブロック単位（周波数領域）。サンプルループより前に済ませる。
        std::fill(tailOutL_.begin(), tailOutL_.begin() + n, 0.0f);
        std::fill(tailOutR_.begin(), tailOutR_.begin() + n, 0.0f);
        if (tailGain_ > 0.0f && tailConv_.hasIr()) {
            float* dst[2] = { tailOutL_.data(), tailOutR_.data() };
            // 尾の絶対レベル ＝ tailGain(= 自由音場の直接 × √target) × 好みの倍率 × 部屋への注入量。
            //   ★以前はここに tailWet_ も掛けていたが、残響/直接比は tailGain の
            //     √target で既に決まっており二重計上だった。C# 経路(IrConvolver)は
            //     掛けていなかったので、同じシーンで C# の方が 3.9dB 響いていた。
            tailConv_.processAdd(input, 0, n, dst, 0, tailGain_ * tailLevel_ * tailSrcLevel_);
        }

        // ② HRTF はブロック境界で HRIR を取り込む（方向変化のクロスフェード開始）。
        const bool hrtfActive = hrtfEnabled_ && hrtf_.isReady();
        if (hrtfActive) hrtf_.beginBlock();
        early_.beginBlock();

        for (int f = 0; f < n; ++f) {
            const float dry = input[f];

            float l, r, scatSend, directMono;
            early_.processSample(dry, hrtfActive, l, r, scatSend, directMono);

            // 直接音を HRTF で両耳化して足す（パンの代わり）。
            float dirL = 0.0f, dirR = 0.0f;
            if (hrtfActive) {
                hrtf_.processSample(directMono, dirL, dirR);
                l += dirL; r += dirR;
            } else {
                // HRTF 無効時、直接音は early_ 側でパン済み。計測用に切り分けられないので
                // モノラル値をそのまま両耳ぶんとして数える。
                dirL = dirR = directMono * 0.70710678f;
            }

            m.rmsDirect += (dirL * dirL + dirR * dirR) * 0.5f;
            const float eL = l - dirL, eR = r - dirR;
            m.rmsEarly += (eL * eL + eR * eR) * 0.5f;

            // ③ 散乱スメア：拡散成分を allpass で撹拌して時間方向に滲ませる。
            const float scL = diffL_.process(scatSend, scatterDiffusion_);
            const float scR = diffR_.process(scatSend, scatterDiffusion_);
            m.rmsScatter += (scL * scL + scR * scR) * 0.5f;
            l += scL; r += scR;

            // ④ 実測尾を加算。
            const float tL = tailOutL_[static_cast<std::size_t>(f)];
            const float tR = tailOutR_[static_cast<std::size_t>(f)];
            m.rmsTail += (tL * tL + tR * tR) * 0.5f;
            l += tL; r += tR;

            l *= outputGain_; r *= outputGain_;
            m.rmsOut += (l * l + r * r) * 0.5f;
            outL[f] = l;
            outR[f] = r;
        }
    }

    Config cfg_;
    const int sampleRate_;
    const int tailSamples_;
    int maxFrames_ = 4096;

    EarlyReflectConv early_;
    HrtfProcessor hrtf_;
    NonUniformConvolver tailConv_;
    ReverbTailIr tailIr_;
    AllpassChain diffL_, diffR_;

    std::vector<float> tailOutL_, tailOutR_;

    bool hrtfEnabled_ = true;
    float outputGain_ = 0.6f;
    float tailGain_ = 0.0f;
    float tailLevel_ = 1.0f;
    float tailWet_ = 1.0f;
    float tailSrcLevel_ = 1.0f;
    float scatterDiffusion_ = 0.62f;
};

}  // namespace dsp
}  // namespace af
