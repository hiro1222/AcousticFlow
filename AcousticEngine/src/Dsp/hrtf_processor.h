// hrtf_processor.h — モノラル信号を、指定方向から聞こえるバイノーラル(L/R)に変換する。
//
// 構成:
//   入力 → [ITD 遅延: 耳ごと・小数サンプル精度] → [HRIR 畳み込み: 耳ごと] → L/R
//
// ITD を畳み込みと分けている理由は hrtf_set.h の冒頭コメントを参照（個人最適化のため）。
//
// 方向が変わったときのクリック対策:
//   HRIR を差し替えると波形が不連続になりプチッと鳴る。新旧2本を同時に畳み込んで
//   クロスフェードする。フェード中だけ倍のコスト。
//
// コストの見積り（HRIR 128 タップ / 48kHz）:
//   通常時 128×2ch = 256 MAC/sample、フェード中はその倍。
//   早期反射までHRTF化すると タップ数×これ になるため、まず直接音だけに適用する。
//   反射は先行音効果でほとんど定位に寄与しないので、費用対効果が悪い。
//
// スレッド規約:
//   setHrtfSet / setDirection … 制御スレッド
//   beginBlock / processSample / processAdd … オーディオスレッド（確保なし）
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/HrtfProcessor.cs。
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

#include "hrtf_set.h"

namespace af {
namespace dsp {

class HrtfProcessor {
public:
    explicit HrtfProcessor(int sampleRate, float crossfadeMs = 12.0f, float crossoverHz = 700.0f)
        : sampleRate_(std::max(8000, sampleRate)) {
        xfadeLen_ = std::max(1, static_cast<int>(std::lround(crossfadeMs * 0.001f * sampleRate_)));
        setCrossover(crossoverHz);
    }

    bool isReady() const { return set_ && set_->isValid() && curL_ != nullptr; }
    const char* setName() const { return set_ ? set_->name().c_str() : "(none)"; }
    int currentIrLength() const { return irLen_; }

    /// 低域/高域の分割周波数。これより下は HRIR を通さず ITD だけ適用する。
    ///   一次(6dB/oct)の緩い分割にしているのは、低域と高域の位相の食い違いを小さく保つため。
    ///   低域 = LP(x)、高域 = x - LP(x) なので、足すと必ず元に戻る（完全再構成）。
    void setCrossover(float hz) {
        hz = std::min(4000.0f, std::max(100.0f, hz));
        lpCoef_ = std::min(1.0f, std::max(0.0f,
                    2.0f * 3.14159265358979323846f * hz / static_cast<float>(sampleRate_)));
    }

    /// データセットを差し替える（制御スレッド）。set は呼び手が生存を保証する。
    void setHrtfSet(const HrtfSet* set) {
        set_ = set;
        if (!set || !set->isValid()) { curL_ = nullptr; curR_ = nullptr; return; }

        irLen_ = set->irLength();
        // 履歴は「HRIR長 + 最大ITD遅延」ぶん必要。ITD は最大 1ms 程度だが余裕を見て 8ms。
        const int need = nextPow2(irLen_ + static_cast<int>(std::lround(0.008f * sampleRate_)) + 8);
        if (static_cast<int>(srcRing_.size()) < need) {
            srcRing_.assign(static_cast<std::size_t>(need), 0.0f);
            lowRing_.assign(static_cast<std::size_t>(need), 0.0f);
            srcMask_ = need - 1;
        }
        // 初期 HRIR（正面）を入れておく。
        float fwd[3] = {0.0f, 0.0f, 1.0f};
        const int idx = set->nearestIndex(fwd);
        curL_ = set->hrir(idx, 0);
        curR_ = set->hrir(idx, 1);
        delayL_ = delayR_ = 0.0f;
        fading_ = false;
        hasPending_.store(false, std::memory_order_release);
    }

    /// 到来方向（リスナー座標系: +x=右, +y=上, +z=前）と頭囲から HRIR/ITD を選ぶ（制御スレッド）。
    /// 実際の切替は次のオーディオブロックでクロスフェードして行われる。
    void setDirection(const float dirListenerLocal[3], float headCircumferenceCm) {
        if (!set_ || !set_->isValid()) return;
        const int idx = set_->nearestIndex(dirListenerLocal);
        if (idx < 0) return;

        const float itd = set_->itdSecondsScaled(idx, headCircumferenceCm);
        // 正=右耳が遅い。負の遅延は作れないので、遅い側だけを遅らせる。
        pendDelayL_ = (itd < 0.0f) ? -itd * sampleRate_ : 0.0f;
        pendDelayR_ = (itd > 0.0f) ?  itd * sampleRate_ : 0.0f;
        pendL_ = set_->hrir(idx, 0);
        pendR_ = set_->hrir(idx, 1);
        hasPending_.store(true, std::memory_order_release);
    }

    /// オーディオスレッド：ブロックの先頭で1回呼ぶ。保留中の HRIR を取り込む。
    void beginBlock() {
        if (!hasPending_.exchange(false, std::memory_order_acq_rel)) return;
        // フェード中に更に新しいものが来たら、進行中の「次」を「現在」に確定させてから差し替える。
        if (fading_) {
            curL_ = nextL_; curR_ = nextR_;
            delayL_ = delayLNext_; delayR_ = delayRNext_;
        }
        nextL_ = pendL_; nextR_ = pendR_;
        delayLNext_ = pendDelayL_; delayRNext_ = pendDelayR_;
        xfadePos_ = 0;
        fading_ = true;
    }

    /// オーディオスレッド：1サンプルをバイノーラル化する。
    /// タップ合成のサンプルループ内から直接音タップに適用するため、
    /// ブロック単位ではなくサンプル単位の入口を用意している。
    void processSample(float x, float& outLs, float& outRs) {
        if (!isReady()) { outLs = 0.0f; outRs = 0.0f; return; }

        // 低域/高域に分ける。低域は HRIR を通さず ITD だけ（下の srcRing_ のコメント参照）。
        lpState_ += lpCoef_ * (x - lpState_);
        const int wp = srcWritePos_ & srcMask_;
        lowRing_[static_cast<std::size_t>(wp)] = lpState_;
        srcRing_[static_cast<std::size_t>(wp)] = x - lpState_;

        if (fading_) {
            const float t = static_cast<float>(xfadePos_) / static_cast<float>(xfadeLen_);
            const float aL = convolve(curL_, delayL_),  aR = convolve(curR_, delayR_);
            const float bL = convolve(nextL_, delayLNext_), bR = convolve(nextR_, delayRNext_);
            outLs = aL * (1.0f - t) + bL * t;
            outRs = aR * (1.0f - t) + bR * t;
            // 低域も遅延だけはクロスフェードする（ITD が急に飛ぶとクリックになる）。
            outLs += readLow(delayL_) * (1.0f - t) + readLow(delayLNext_) * t;
            outRs += readLow(delayR_) * (1.0f - t) + readLow(delayRNext_) * t;
            if (++xfadePos_ >= xfadeLen_) {
                fading_ = false;
                curL_ = nextL_; curR_ = nextR_;
                delayL_ = delayLNext_; delayR_ = delayRNext_;
            }
        } else {
            outLs = convolve(curL_, delayL_) + readLow(delayL_);
            outRs = convolve(curR_, delayR_) + readLow(delayR_);
        }
        ++srcWritePos_;
    }

    /// オーディオスレッド：モノラル入力を畳み込み、outL/outR に**加算**する（ブロック単位）。
    void processAdd(const float* input, int inOffset, int n,
                    float* outL, float* outR, int outOffset, float gain) {
        if (!isReady() || !input || !outL || !outR) return;
        beginBlock();
        for (int i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            processSample(input[inOffset + i], l, r);
            outL[outOffset + i] += l * gain;
            outR[outOffset + i] += r * gain;
        }
    }

private:
    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // 低域を ITD 遅延つきで読む（畳み込みはしない）。
    //   低域は頭を回り込むので両耳とも減衰しない＝利得 1.0 のまま通す。
    float readLow(float delay) const {
        const int d0 = static_cast<int>(delay);
        const float frac = delay - static_cast<float>(d0);
        const int baseIdx = srcWritePos_ - d0;
        const float s0 = lowRing_[static_cast<std::size_t>(baseIdx & srcMask_)];
        if (frac <= 1e-6f) return s0;
        const float s1 = lowRing_[static_cast<std::size_t>((baseIdx - 1) & srcMask_)];
        return s0 + frac * (s1 - s0);
    }

    // 入力履歴を「delay サンプル前」から読みつつ HRIR と畳み込む。
    //   遅延を読み出し位置に折り込むので、耳ごとの中間バッファが要らない。
    //   ＝クロスフェード中に新旧が別の遅延で同時に走っても干渉しない。
    //   遅延は小数サンプル。ITD の弁別限は 10〜20µs で 48kHz の 1 サンプル未満なので、
    //   整数に丸めると定位が粗くなる。よってタップごとに線形補間する。
    float convolve(const float* h, float delay) const {
        if (!h) return 0.0f;
        const int d0 = static_cast<int>(delay);
        const float frac = delay - static_cast<float>(d0);
        const int baseIdx = srcWritePos_ - d0;
        float s = 0.0f;
        if (frac <= 1e-6f) {
            for (int k = 0; k < irLen_; ++k)
                s += h[k] * srcRing_[static_cast<std::size_t>((baseIdx - k) & srcMask_)];
        } else {
            for (int k = 0; k < irLen_; ++k) {
                const float s0 = srcRing_[static_cast<std::size_t>((baseIdx - k) & srcMask_)];
                const float s1 = srcRing_[static_cast<std::size_t>((baseIdx - k - 1) & srcMask_)];
                s += h[k] * (s0 + frac * (s1 - s0));
            }
        }
        return s;
    }

    const HrtfSet* set_ = nullptr;
    int sampleRate_;
    int irLen_ = 0;

    // 入力履歴。ITD 遅延は「畳み込みの読み出し位置」に折り込むので、
    // 耳ごとの遅延後バッファは持たない（持つとクロスフェード中に
    // 新旧が同じバッファを奪い合って壊れる）。
    //
    // 低域と高域で別のリングを持つ理由（重要）:
    //   実測 HRIR は低域を正しく持っていない。128タップ(2.9ms)では約350Hz以下を
    //   表現できず、測定スピーカーの低域ロールオフも含まれる。実測すると
    //   直流利得は約 0.15(-16dB)で、広帯域RMS(約1.0)に対して大きく欠けている。
    //   そのまま畳み込むと低音が痩せる（「音が軽くなる」）。
    //   物理的にも、低域は波長が頭より遥かに長いので頭を回り込み、減衰しない。
    //   低域の定位手がかりは ITD だけで、耳介も頭部の影も効かない。
    //   → 低域は HRIR を通さず ITD だけ適用し、高域だけ HRTF で畳み込む。
    std::vector<float> srcRing_;   // 高域（HRIR と畳み込む）
    std::vector<float> lowRing_;   // 低域（ITD 遅延だけ適用）
    int srcMask_ = 0;
    int srcWritePos_ = 0;
    float lpState_ = 0.0f;
    float lpCoef_ = 0.0f;

    // 現在/次の HRIR とクロスフェード状態。オーディオスレッドが読む。
    const float* curL_ = nullptr;
    const float* curR_ = nullptr;
    const float* nextL_ = nullptr;
    const float* nextR_ = nullptr;
    std::atomic<bool> hasPending_{false};
    const float* pendL_ = nullptr;
    const float* pendR_ = nullptr;
    float pendDelayL_ = 0.0f, pendDelayR_ = 0.0f;
    bool fading_ = false;
    int xfadePos_ = 0, xfadeLen_ = 1;

    // ITD 由来の遅延（サンプル。小数可）。耳ごと。
    float delayL_ = 0.0f, delayR_ = 0.0f;
    float delayLNext_ = 0.0f, delayRNext_ = 0.0f;
};

}  // namespace dsp
}  // namespace af
