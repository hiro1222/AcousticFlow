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
        // 尾 IR を差し替えるときの並走クロスフェード(ms)。**既定 0＝無効**。
        //
        // ★これを有効にすると今は悪化する。実測（同じ内容で差し替えた対照ケース）:
        //     無効(単一の器で差し替え)  ピーク +0.27 dB
        //     有効(200ms 並走)          ピーク −3.26 dB   ← 凹む
        //   原因は、新しい畳み込み器の**周波数領域遅延線(FDL)が空**なこと。
        //   FDL には過去の入力ブロックが溜まっていて、それが無いと尾を出し切れない。
        //   フェードイン中の新側が痩せているので、混ぜると凹む。
        //
        //   正しい直し方は「古い器の FDL を新しい器へ引き継ぐ」。FDL が持っているのは
        //   変換済みの**入力**で IR とは無関係なので、そのまま移せば新側も最初から
        //   完全な尾を出せる。ただし FDL はオーディオスレッド所有で setIr は制御スレッドなので、
        //   受け渡しの設計（二重化か、次の runBlock で取り込むか）が要る。未対応。
        //
        // 単一の器での差し替えにも人工物はある（過去の入力が新 IR で畳み直され、
        // +1.97dB のふくらみが 0.35 秒）。どちらも 2〜3dB 級で、まだ選ぶ根拠が無い。
        float tailCrossfadeMs = 0.0f;
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
          tailConvA_(tailSamples_, 2, cfg.tailFirstBlock, cfg.tailCapBlock, cfg.maxFrames),
          tailConvB_(tailSamples_, 2, cfg.tailFirstBlock, cfg.tailCapBlock, cfg.maxFrames),
          tailIr_(sampleRate_, tailSamples_, 2) {
        tailXfadeLen_ = std::max(0, static_cast<int>(cfg.tailCrossfadeMs * 0.001f * sampleRate_));
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

        // 【並走クロスフェード】新しい IR は**別の畳み込み器**へ入れ、古い方は自分の IR の
        //   まま鳴らし続けて音量だけ落とす。
        //   ★1 つの器で差し替えると、周波数領域遅延線に溜まっている**過去の入力**が
        //     新しい IR で畳み直される。尾IRはエネルギー1に正規化されているので
        //     本来レベルは動かないはず（実測: 響く部屋と吸う部屋の定常RMSは差 -0.08dB）
        //     なのに、実測で **+1.97dB のふくらみが 0.35 秒続いた**。まるごと人工物。
        //     並走なら過去の入力が新 IR に当たること自体が起きない。
        //   コストはクロスフェード中だけ倍（0.205 → 0.41 ms/block）。
        // ★入れ替えるのはクロスフェードが有効なときだけ。無効時に入れ替えると、
        //   FDL（過去の入力）が空の器へ切り替わって尾が無音になる（実測 -227dB）。
        if (tailXfadeLen_ > 0 && tailCur().hasIr()) {
            // 役割を入れ替える。今鳴っている方が「古い側」になり、空いた方に新 IR を入れる。
            // 進行中のクロスフェードは打ち切る（3 重に重ねない）。
            tailGainOld_ = tailGain_;
            tailUseB_ = !tailUseB_;
            tailXfade_ = 0;
            tailXfading_ = true;
        }
        tailCur().setIr(ir, len);
        // 尾IRはエネルギー1に正規化済み。絶対レベルは D/R 比から一意に決まる。
        tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
        return ratio;
    }

    /// クロスフェードの長さ(ms)。0 で即差し替え（旧挙動）。
    void setTailCrossfadeMs(float ms) {
        tailXfadeLen_ = std::max(0, static_cast<int>(ms * 0.001f * sampleRate_));
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
    int tailPartitions() const { return tailCur().totalPartitions(); }
    int tailLatency() const { return tailCur().latency(); }

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
        // 尾の絶対レベル ＝ tailGain(= 自由音場の直接 × √target) × 好みの倍率 × 部屋への注入量。
        //   ★ここに tailWet_ を掛けてはいけない。残響/直接比は tailGain の √target で
        //     既に決まっており二重計上になる（C# 経路は掛けていなかったので 3.9dB 差が出た）。
        const float lvl = tailLevel_ * tailSrcLevel_;
        // クロスフェードの重み。等パワーではなく**等振幅**にする ── 新旧は同じ入力を
        //   同じエネルギーの IR に通した相関のある信号なので、振幅で足して 1 になるのが正しい
        //   （等パワーにすると混合中に +3dB 膨らむ）。
        float wNew = 1.0f, wOld = 0.0f;
        if (tailXfading_ && tailXfadeLen_ > 0) {
            const float t = static_cast<float>(tailXfade_) / static_cast<float>(tailXfadeLen_);
            wNew = clamp01(t);
            wOld = 1.0f - wNew;
        } else {
            tailXfading_ = false;
        }
        float* dst[2] = { tailOutL_.data(), tailOutR_.data() };
        if (tailGain_ > 0.0f && tailCur().hasIr() && wNew > 0.0f)
            tailCur().processAdd(input, 0, n, dst, 0, tailGain_ * lvl * wNew);
        if (tailXfading_ && tailGainOld_ > 0.0f && tailOld().hasIr() && wOld > 0.0f)
            tailOld().processAdd(input, 0, n, dst, 0, tailGainOld_ * lvl * wOld);
        if (tailXfading_) {
            tailXfade_ += n;
            if (tailXfade_ >= tailXfadeLen_) { tailXfading_ = false; tailXfade_ = 0; }
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
    // 並走クロスフェード用に 2 面持つ。役割は tailUseB_ で入れ替える
    // （NonUniformConvolver は atomic を持つので swap できない）。
    NonUniformConvolver tailConvA_;
    NonUniformConvolver tailConvB_;
    bool tailUseB_ = false;
    NonUniformConvolver& tailCur() { return tailUseB_ ? tailConvB_ : tailConvA_; }
    const NonUniformConvolver& tailCur() const { return tailUseB_ ? tailConvB_ : tailConvA_; }
    NonUniformConvolver& tailOld() { return tailUseB_ ? tailConvA_ : tailConvB_; }
    ReverbTailIr tailIr_;
    // クロスフェードの状態。既定 200ms（実測でふくらみが収まるのが 0.35 秒、到達が 0.1 秒）。
    int   tailXfadeLen_ = 0;
    int   tailXfade_ = 0;
    bool  tailXfading_ = false;
    float tailGainOld_ = 0.0f;
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
