// direction_bus.h ── 方向バス（リスナー座標で固定した N 方向 × 2 耳のレーン）。2026-09-04。
//
//   反射・回折のタップを 1 本ずつ両耳化（軽量両耳化 = 耳ごとの遅延＋帯域別 ILD、0.13%/本）すると、
//   面の線で R タップが 48 本になったとき音源あたり 8%、6 音源で半コアになった（AfDspBench）。
//   ここでは、タップを隣り合う 2 方向へ等パワーで振り、方向ごとに**固定の HRIR** で畳む。
//     ・費用が O(タップ) → O(方向)。レーンはリスナーに 1 組（全音源で共有）なので音源数に依らない。
//     ・方向は頭に対して固定。HRIR は起動時に決まり、頭が回っても重みが隣へ移るだけで
//       クロスフェード（HrtfProcessor の 12 ms）は要らない。
//     ・振り分けは等パワー（w0² + w1² = 1）。境をまたいでも音量が跳ばない。
//   ★ITD はタップごと、ILD とスペクトルは方向ごと。
//     方向を量子化して ITD まで方向の代表値にすると、同じ方向に集まった反射が同じ ITD を持って
//     左右の相関が上がる（8 方向で環の IACC 0.19 → 0.46、12 方向でも 0.31。基準は 1 本ずつ HRIR）。
//     そこでレーンを「方向 × 耳」の 2 本にし、タップは自分の ITD（耳ごとの遅延）で 2 本へ別々に送る。
//     方向の HRIR は ITD を抜いて揃えた物（hrir() はそう保存されている）を耳ごとにモノラルで畳む。
//     畳み込みの本数は 8 方向 × 2 耳 = 16 だが、1 本ずつがモノラルなので費用はステレオ 8 本と同じ。
//   使い方は尾のバス（TailBus）と同じ: 音源が render の中で add() し、AudioListener のフィルタが render() を
//   1 回呼んで左右へ足す。add() と render() は同じオーディオスレッドから順に呼ばれる前提。
//   上下は畳む（水平の環）。本数の目安: 8（45°）が出発点、12（30°）が反射の弁別（10〜15°）と同じ桁で上限。
//   ■ 低域と高域を分ける（2026-09-12。setCrossover、既定 700 Hz。0 で旧＝全帯域を畳む）
//     直接音の HrtfProcessor は 700 Hz で分け、低域は HRIR を通さず ITD だけ、高域だけを畳む。バスは全帯域を畳んでいた。
//     合成 HRTF（低域の利得 1）では差が出ないが、実測の kemar は測定の都合で低域が落ちている（50 Hz −11.6 dB、
//     200〜1000 Hz −3〜−4.5 dB、2〜4 kHz +5〜+7 dB）。全帯域を畳むと反射と尾の低域がその分だけ薄くなり、
//     試聴 WAV の低域寄りの信号で尾が 8 dB 落ちた（実測）。頭は波長より小さいので、低域は物理的にも素通しでよい。
//     ★量の揃え方もここに移した。分けるときはバス自身が HRIR を「レーン × 耳の平均パワー 1」に揃える
//       （低域は 1 のまま、高域は平均で 1）。送る側（FdnRoomMix の laneNorm_、VoiceRenderer のレーン）は割らない
//       ＝ meanPowerGain() が 1 を返す。旧（全帯域）は FdnRoomMix が HrtfSet の全方向の平均パワー（kemar +3.2 dB、
//       2〜4 kHz の山に引っ張られた値）で割っていて、低域寄りの音ではその分だけ損をしていた。
//     低域の素通しは畳み込みの固有遅延（firstBlock）ぶん遅らせて高域と揃える（行ごとの短い環）。
//   分割の最小ブロック（＝固有遅延）は 128（2.7 ms）。64 だと小さな FFT が増えて 2 倍重く、256 以上は
//   遅延ぶんタップを早めきれない反射（壁ぎわの 5 ms 未満）が出る。実測: 8 方向で 64→4.6% / 128→2.0% / 256→2.1%。
#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "hrtf_set.h"
#include "nonuniform_convolver.h"

namespace af {
namespace dsp {

class DirectionBus {
public:
    static constexpr int kMaxLanes = 16;
    static constexpr int kMinLanes = 4;
    // 行の HRIR の長さ（タップ）。反射用なので 256 → 128（2.7 ms）に切る。費用が半分になり、低域の ILD の細部だけ落ちる。
    static constexpr int kLaneIrMax = 128;

    DirectionBus(int sampleRate, int lanes, int maxFrames, int firstBlock = 128, int capBlock = 1024)
        : sampleRate_(sampleRate > 0 ? sampleRate : 48000),
          lanes_(std::max(kMinLanes, std::min(lanes, kMaxLanes))),
          maxFrames_(std::max(64, maxFrames)),
          firstBlock_(firstBlock), capBlock_(capBlock) {
        in_.assign(static_cast<std::size_t>(rows()) * maxFrames_, 0.0f);
        scratchL_.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
        scratchR_.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
        hfBuf_.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
        setCrossover(700.0f);
        for (int k = 0; k < lanes_; ++k) {
            const float az = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(lanes_);
            laneDir_[k][0] = std::sin(az);   // +x = 右
            laneDir_[k][1] = 0.0f;
            laneDir_[k][2] = std::cos(az);   // +z = 正面
        }
    }

    int lanes() const { return lanes_; }
    /// 送りの行数 = 方向 × 2 耳。行 r = 方向 k * 2 + 耳 e（0 = 左 / 1 = 右）。
    int rows() const { return lanes_ * 2; }
    int maxFrames() const { return maxFrames_; }
    bool hasHrtf() const { return hasHrtf_; }
    /// 差してある HRTF（無ければ nullptr）。戸口の線音源が点ごとの HRTF に使う（段 2-g）。生存は呼び手が保証する。
    const HrtfSet* hrtfSet() const { return hasHrtf_ ? hrtfSet_ : nullptr; }
    /// 畳み込みの固有遅延（firstBlock）。タップはこのぶん早めて送ること（VoiceRenderer::setTaps）。
    int latency() const { return conv_.empty() ? 0 : conv_[0]->latency(); }
    void laneDirection(int k, float out[3]) const {
        if (k < 0 || k >= lanes_) { out[0] = 0; out[1] = 0; out[2] = 1; return; }
        out[0] = laneDir_[k][0]; out[1] = laneDir_[k][1]; out[2] = laneDir_[k][2];
    }

    /// タップの方向（リスナー座標。+x 右 / +z 正面）→ 隣り合う 2 方向と等パワーの重み。
    ///   方位角だけを見る（上下は畳む）。真上・真下（水平成分が無い）は正面扱い。
    static void laneWeights(const float dir[3], int lanes, int outLane[2], float outW[2]) {
        lanes = std::max(kMinLanes, std::min(lanes, kMaxLanes));
        float az = std::atan2(dir[0], dir[2]);           // 0 = 正面、+ = 右回り
        if (az < 0.0f) az += 2.0f * kPi;
        const float spacing = 2.0f * kPi / static_cast<float>(lanes);
        float pos = az / spacing;
        if (pos >= static_cast<float>(lanes)) pos -= static_cast<float>(lanes);
        const int k0 = static_cast<int>(std::floor(pos)) % lanes;
        const float f = pos - std::floor(pos);
        outLane[0] = k0;
        outLane[1] = (k0 + 1) % lanes;
        outW[0] = std::cos(f * 0.5f * kPi);
        outW[1] = std::sin(f * 0.5f * kPi);
    }

    /// 低域と高域の境（Hz）。0 で分けない（旧: 全帯域を HRIR で畳む）。★HRTF を差す前に決める（量の揃え方が変わる）。
    void setCrossover(float hz) {
        xoverHz_ = hz;
        xoverCoef_ = (hz > 0.0f) ? std::min(1.0f, 2.0f * kPi * hz / static_cast<float>(sampleRate_)) : 0.0f;
    }
    float crossoverHz() const { return xoverHz_; }

    /// 方向 × 耳ごとに固定の HRIR（ITD を抜いて揃えた物）をモノラルで焼く。ITD はタップが持つ。
    void setHrtfSet(const HrtfSet* set, float /*headCircumferenceCm*/) {
        hrtfSet_ = set;                                  // 戸口の線音源（FdnRoomMix、段 2-g）が同じ HRTF を借りる
        hasHrtf_ = false;
        conv_.clear();
        meanPowerGain_ = 1.0f;
        if (!set || !set->isValid()) return;
        const int irLen = std::min(set->irLength(), kLaneIrMax);
        const int fade = std::min(16, irLen / 4);
        auto fadeAt = [&](int i) {
            return (i >= irLen - fade) ? 0.5f * (1.0f + std::cos(kPi * static_cast<float>(i - (irLen - fade)) / static_cast<float>(fade))) : 1.0f;
        };
        // 量の揃え方（上の■）。分けるときはここで HRIR をレーン × 耳の平均パワー 1 に揃え、送る側は割らない。
        float irNorm = 1.0f;
        if (xoverCoef_ > 0.0f) {
            double pw = 0.0; int cnt = 0;
            for (int k = 0; k < lanes_; ++k) {
                const int idx = set->nearestIndex(laneDir_[k]);
                if (idx < 0) return;
                for (int e = 0; e < 2; ++e) {
                    const float* src = set->hrir(idx, e);
                    for (int i = 0; i < irLen; ++i) { const double v = static_cast<double>(src[i]) * fadeAt(i); pw += v * v; }
                    ++cnt;
                }
            }
            const double mean = (cnt > 0) ? pw / cnt : 1.0;
            irNorm = (mean > 1e-12) ? static_cast<float>(1.0 / std::sqrt(mean)) : 1.0f;
        } else {
            meanPowerGain_ = std::max(1e-6f, set->meanPowerGain());   // 旧: 尾の FDN が全方向の平均パワーで割る
        }
        std::vector<float> cut(static_cast<std::size_t>(irLen));
        conv_.reserve(static_cast<std::size_t>(rows()));
        for (int k = 0; k < lanes_; ++k) {
            const int idx = set->nearestIndex(laneDir_[k]);
            if (idx < 0) { conv_.clear(); return; }
            for (int e = 0; e < 2; ++e) {
                auto c = std::make_unique<NonUniformConvolver>(irLen, 1, firstBlock_, capBlock_, maxFrames_);
                const float* src = set->hrir(idx, e);
                for (int i = 0; i < irLen; ++i) cut[static_cast<std::size_t>(i)] = src[i] * fadeAt(i) * irNorm;
                const float* ir[1] = { cut.data() };
                const int    n1[1] = { irLen };
                c->setIr(ir, n1);
                conv_.push_back(std::move(c));
            }
        }
        // 低域の素通しの環（畳み込みの固有遅延ぶん）と一次 LP の状態。行ごと。
        lfLen_ = conv_.empty() ? 0 : conv_[0]->latency();
        lpState_.assign(static_cast<std::size_t>(rows()), 0.0f);
        lfDelay_.assign(static_cast<std::size_t>(rows()) * static_cast<std::size_t>(std::max(1, lfLen_)), 0.0f);
        lfPos_ = 0;
        hasHrtf_ = true;
    }

    /// 音源からの送り。rowsMono は [行][サンプル] の並び（行 = 方向×2＋耳、stride = 1 行の間隔）。
    ///   dstOffset … このブロックの中での書き込み位置（VoiceRenderer が maxFrames ごとに分けるぶん）。
    void add(const float* rowsMono, int frames, int stride, float gain, int dstOffset = 0) {
        if (!rowsMono || frames <= 0 || gain == 0.0f || dstOffset < 0 || stride < frames) return;
        const int end = std::min(dstOffset + frames, maxFrames_);
        if (end <= dstOffset) return;
        const int R = rows();
        if (pending_ < end) {
            for (int r = 0; r < R; ++r) {
                float* dst = in_.data() + static_cast<std::size_t>(r) * maxFrames_;
                std::fill(dst + pending_, dst + end, 0.0f);
            }
            pending_ = end;
        }
        for (int r = 0; r < R; ++r) {
            float* dst = in_.data() + static_cast<std::size_t>(r) * maxFrames_;
            const float* src = rowsMono + static_cast<std::size_t>(r) * stride;
            for (int i = dstOffset; i < end; ++i) dst[i] += src[i - dstOffset] * gain;
        }
    }

    /// 溜まった送りを行ごとに固定の HRIR で畳んで outL/outR へ**足す**（左の行は左耳へ、右の行は右耳へ）。
    ///   HRIR が無ければそのまま耳へ（ITD だけの両耳化。無音にはしない）。
    ///   ★音源が 1 本も送っていないブロックでも呼ぶこと（畳み込み器の遅延線を進める）。
    void render(int frames, float* outL, float* outR) {
        if (!outL || !outR || frames <= 0) return;
        const int n = std::min(frames, maxFrames_);
        const int R = rows();
        if (pending_ < n) {
            for (int r = 0; r < R; ++r) {
                float* dst = in_.data() + static_cast<std::size_t>(r) * maxFrames_;
                std::fill(dst + pending_, dst + n, 0.0f);
            }
            pending_ = n;
        }
        std::fill(scratchL_.begin(), scratchL_.begin() + n, 0.0f);
        std::fill(scratchR_.begin(), scratchR_.begin() + n, 0.0f);
        for (int r = 0; r < R; ++r) {
            const float* row = in_.data() + static_cast<std::size_t>(r) * maxFrames_;
            float* ear = (r & 1) ? scratchR_.data() : scratchL_.data();
            if (hasHrtf_ && xoverCoef_ > 0.0f) {
                // 低域は素通し（固有遅延ぶん遅らせて高域と揃える）、高域だけ HRIR で畳む（上の■）
                float lp = lpState_[static_cast<std::size_t>(r)];
                float* hf = hfBuf_.data();
                float* ring = lfDelay_.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(std::max(1, lfLen_));
                int pos = lfPos_;
                for (int i = 0; i < n; ++i) {
                    lp += xoverCoef_ * (row[i] - lp);
                    hf[i] = row[i] - lp;
                    if (lfLen_ > 0) { const float d = ring[pos]; ring[pos] = lp; ear[i] += d; if (++pos >= lfLen_) pos = 0; }
                    else ear[i] += lp;
                }
                lpState_[static_cast<std::size_t>(r)] = lp;
                float* dst[1] = { ear };
                conv_[static_cast<std::size_t>(r)]->processAdd(hf, 0, n, dst, 0, 1.0f);
            } else if (hasHrtf_) {
                float* dst[1] = { ear };
                conv_[static_cast<std::size_t>(r)]->processAdd(row, 0, n, dst, 0, 1.0f);
            } else {
                for (int i = 0; i < n; ++i) ear[i] += row[i];
            }
        }
        if (hasHrtf_ && xoverCoef_ > 0.0f && lfLen_ > 0) lfPos_ = (lfPos_ + n) % lfLen_;
        float e = 0.0f;
        for (int i = 0; i < n; ++i) {
            outL[i] += scratchL_[static_cast<std::size_t>(i)];
            outR[i] += scratchR_[static_cast<std::size_t>(i)];
            e += scratchL_[static_cast<std::size_t>(i)] * scratchL_[static_cast<std::size_t>(i)];
        }
        rms_ = (n > 0) ? std::sqrt(e / static_cast<float>(n)) : 0.0f;
        pending_ = 0;
    }

    /// 直近ブロックの出力の RMS（左）。
    float rms() const { return rms_; }
    /// HRIR の平均パワーゲイン（HRIR 無しなら 1）。VoiceRenderer の difNorm_ と同じ揃え方に使う。
    float meanPowerGain() const { return meanPowerGain_; }

private:
    static constexpr float kPi = 3.14159265358979f;
    int sampleRate_;
    int lanes_;
    int maxFrames_;
    int firstBlock_, capBlock_;
    float laneDir_[kMaxLanes][3] = {};
    std::vector<std::unique_ptr<NonUniformConvolver>> conv_;   // 行ごと（モノラル IR）
    bool hasHrtf_ = false;
    const HrtfSet* hrtfSet_ = nullptr;
    std::vector<float> in_;                 // [行][maxFrames] 溜まった送り
    int pending_ = 0;                       // このブロックで触った長さ
    std::vector<float> scratchL_, scratchR_;
    float rms_ = 0.0f;
    float meanPowerGain_ = 1.0f;
    // 低域と高域を分ける（上の■）
    float xoverHz_ = 0.0f, xoverCoef_ = 0.0f;
    std::vector<float> hfBuf_;              // 高域（畳み込みへ）
    std::vector<float> lpState_;            // [行] 一次 LP の状態
    std::vector<float> lfDelay_;            // [行][固有遅延] 低域を遅らせる環
    int lfLen_ = 0, lfPos_ = 0;
};

}  // namespace dsp
}  // namespace af
