// partitioned_convolver.h — 一様分割 overlap-save による周波数領域畳み込み。
//
// なぜ必要か:
//   後期残響の尾は 1 秒級＝数万サンプルある。時間領域のマルチタップでは畳み込めないので、
//   「密なフルIR」を鳴らすには周波数領域の分割畳み込みが要る。
//
// なぜ大きいブロックで良いか（重要）:
//   一様分割 overlap-save の固有遅延は 1 ブロック分。通常この遅延が問題になるので
//   ブロックを小さくして数を増やす（＝重い）が、ここで畳み込むのは「後期尾」だけで、
//   尾はそもそも早期部（〜120ms）より後から始まる。つまりブロック遅延を尾の立ち上がりの
//   中に隠せる。ブロック 2048(≒43ms) でも遅れて聞こえない代わりに、パーティション数が
//   1/8 になって劇的に軽くなる。
//
// 入力はモノラル、出力は複数チャンネル（L/R）。入力 FFT は 1 回だけ行い、
// チャンネルごとに異なる IR スペクトルと掛け合わせる（＝ステレオ化がほぼ半額）。
//
// IR の差し替え（クロスフェード）:
//   周波数領域遅延線(FDL)が持っているのは**変換済みの入力**で、IR とは無関係。
//   だから器を 2 つ並べる必要はない ── **FDL は 1 本のまま、IR スペクトルだけ 2 世代持って混ぜる**。
//   器を 2 つにすると新しい器の FDL が空で、フェードイン中の新側が痩せる（実測 -3.26dB の凹み）。
//   世代を混ぜる形なら新旧どちらも過去の入力を全部持ったまま鳴るので、凹みも膨らみも出ない。
//   重みは**等振幅**（新旧は同じ入力を同じエネルギーの IR に通した相関のある信号なので、
//   等パワーにすると混合中に +3dB 膨らむ）。
//
// スレッド規約:
//   setIr()    … 制御スレッドから呼ぶ。ここでだけ確保する。
//   processAdd() … オーディオスレッドから呼ぶ。**確保・ロック・例外なし**。
//   差し替えはブロック境界で起きる（波形の切れ目が最小）。
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/PartitionedConvolver.cs。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "fft.h"

namespace af {
namespace dsp {

class PartitionedConvolver {
public:
    PartitionedConvolver(int blockSize, int numPartitions, int channels)
        : b_(nextPow2(std::max(64, blockSize))),
          fftLen_(b_ * 2),
          numParts_(std::max(1, numPartitions)),
          channels_(std::max(1, channels)),
          fft_(fftLen_) {
        const std::size_t fl = static_cast<std::size_t>(fftLen_);
        fdlRe_.assign(static_cast<std::size_t>(numParts_) * fl, 0.0f);
        fdlIm_.assign(static_cast<std::size_t>(numParts_) * fl, 0.0f);
        wRe_.assign(fl, 0.0f);
        wIm_.assign(fl, 0.0f);
        accRe_.assign(fl, 0.0f);
        accIm_.assign(fl, 0.0f);
        inBuf_.assign(fl, 0.0f);
        outBuf_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(b_), 0.0f);
        outBufOld_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(b_), 0.0f);
        // 出力キューは無音で始まる → 最初の 1 ブロックぶんは無音が出る（＝固有遅延 B）。
    }

    ~PartitionedConvolver() {
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);
        delete live_;
        delete fading_;
    }

    PartitionedConvolver(const PartitionedConvolver&) = delete;
    PartitionedConvolver& operator=(const PartitionedConvolver&) = delete;

    int blockSize() const { return b_; }
    int numPartitions() const { return numParts_; }
    int tailSamples() const { return b_ * numParts_; }
    int activeParts() const { return activeParts_; }

    // 差し替え待ちも「IRあり」とみなす。取り込みは runBlock の中で起きるが、その runBlock は
    // processAdd からしか呼ばれない。live_ だけを見ると「IRが無いから処理しない →
    // 処理しないから取り込まれない」で永久に始まらない。
    bool hasIr() const {
        return live_ != nullptr || pending_.load(std::memory_order_acquire) != nullptr;
    }

    /// IR 差し替えのクロスフェード長（サンプル）。0 で即差し替え（従来）。制御スレッドから。
    ///   ★器は増やさない。FDL は共有のまま IR スペクトルだけ 2 世代持って混ぜる。
    void setCrossfadeSamples(int n) { xfadeLen_.store(n < 0 ? 0 : n, std::memory_order_relaxed); }
    int crossfadeSamples() const { return xfadeLen_.load(std::memory_order_relaxed); }

    // IR を差し替える（制御スレッドから）。ir[c] は tailSamples() 以下の長さ。
    //   実際の切替は次のブロック境界。ここでだけ確保し、オーディオスレッドは触らない。
    void setIr(const float* const* ir, const int* irLen) {
        if (!ir || !irLen) return;

        // 前回退役したぶんをここで解放する（オーディオスレッドで delete しないため）。
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);

        auto next = std::unique_ptr<IrSpectra>(new IrSpectra(channels_, numParts_, fftLen_));
        std::vector<float> re(static_cast<std::size_t>(fftLen_));
        std::vector<float> im(static_cast<std::size_t>(fftLen_));
        int active = 0;

        for (int c = 0; c < channels_; ++c) {
            const float* src = ir[c];
            const int len = src ? irLen[c] : 0;
            for (int p = 0; p < numParts_; ++p) {
                std::fill(re.begin(), re.end(), 0.0f);
                std::fill(im.begin(), im.end(), 0.0f);
                const int off = p * b_;
                const int n = std::min(b_, std::max(0, len - off));
                // overlap-save: パーティションは FFT 長の前半に置く。
                bool any = false;
                for (int i = 0; i < n; ++i) {
                    const float v = src[off + i];
                    re[static_cast<std::size_t>(i)] = v;
                    if (v != 0.0f) any = true;
                }
                // 実際に中身のあるパーティション数を数える。尾が短い部屋では大半が無音なので、
                // そこを掛け算しても 0 が増えるだけ。走査を打ち切ると小部屋が大幅に安くなる。
                if (any && p + 1 > active) active = p + 1;
                fft_.transform(re.data(), im.data(), false);
                float* dstRe = next->re(c, p);
                float* dstIm = next->im(c, p);
                std::copy(re.begin(), re.end(), dstRe);
                std::copy(im.begin(), im.end(), dstIm);
            }
        }
        next->activeParts = std::max(1, active);

        // 公開。オーディオスレッドは次のブロック境界でこれを取り込む。
        IrSpectra* stale = pending_.exchange(next.release(), std::memory_order_acq_rel);
        delete stale;   // まだ取り込まれていなかった世代。制御スレッドなので解放してよい。
    }

    // n サンプル処理する。input[i] が入力、outCh[c][outOffset + i] に**加算**する。
    // 加算なので呼び手は他の成分と混ぜられる。
    void processAdd(const float* input, int inOffset, int n,
                    float* const* outCh, int outOffset, float gain) {
        if (!input || !outCh) return;
        for (int i = 0; i < n; ++i) {
            // 出力は「前のブロックで計算済みのぶん」を吐く。
            //   世代を混ぜている間は、同じ遅延線から作った新旧 2 本を等振幅で足す。
            const std::size_t pos = static_cast<std::size_t>(pos_);
            const std::size_t stride = static_cast<std::size_t>(b_);
            if (xfading_) {
                const float wNew = static_cast<float>(xfadePos_) / static_cast<float>(xfadeSpan_);
                const float wOld = 1.0f - wNew;
                for (int c = 0; c < channels_; ++c) {
                    const std::size_t k = static_cast<std::size_t>(c) * stride + pos;
                    outCh[c][outOffset + i] += (outBuf_[k] * wNew + outBufOld_[k] * wOld) * gain;
                }
                // 混ぜ終わり。古い世代は次の差し替えまで置いたまま（解放は制御スレッド）。
                if (++xfadePos_ >= xfadeSpan_) xfading_ = false;
            } else {
                for (int c = 0; c < channels_; ++c)
                    outCh[c][outOffset + i] +=
                        outBuf_[static_cast<std::size_t>(c) * stride + pos] * gain;
            }

            // 同じ歩幅で次ブロック用の入力を溜める。
            inBuf_[static_cast<std::size_t>(b_ + pos_)] = input[inOffset + i];

            if (++pos_ >= b_) { runBlock(); pos_ = 0; }
        }
    }

private:
    // IR スペクトル一式。[channel][partition][fftLen] を平坦に持つ。
    struct IrSpectra {
        IrSpectra(int channels, int numParts, int fftLen)
            : parts(numParts), len(fftLen),
              reBuf(static_cast<std::size_t>(channels) * numParts * fftLen, 0.0f),
              imBuf(static_cast<std::size_t>(channels) * numParts * fftLen, 0.0f) {}
        float* re(int c, int p) {
            return reBuf.data() + (static_cast<std::size_t>(c) * parts + p) * len;
        }
        float* im(int c, int p) {
            return imBuf.data() + (static_cast<std::size_t>(c) * parts + p) * len;
        }
        const float* re(int c, int p) const {
            return reBuf.data() + (static_cast<std::size_t>(c) * parts + p) * len;
        }
        const float* im(int c, int p) const {
            return imBuf.data() + (static_cast<std::size_t>(c) * parts + p) * len;
        }
        int parts;
        int len;
        int activeParts = 1;
        std::vector<float> reBuf, imBuf;
    };

    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // 1 ブロック分を周波数領域で畳む。オーディオスレッド専用。
    void runBlock() {
        // 保留中の IR があればここで差し替える（ブロック境界＝波形の切れ目が最小）。
        if (IrSpectra* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            const int xlen = xfadeLen_.load(std::memory_order_relaxed);
            IrSpectra* drop = nullptr;
            if (xlen > 0 && live_) {
                // いま鳴っている世代を「古い側」に回して混ぜ始める。遅延線は共有なので、
                // 古い側も新しい側も**過去の入力を全部持ったまま**鳴る（＝新側が痩せない）。
                //   進行中のフェードは打ち切る（世代を 3 つは持たない）。
                drop = fading_;
                fading_ = live_;
                fadingParts_ = activeParts_;
                xfadeSpan_ = xlen;
                xfadePos_ = 0;
                xfading_ = true;
            } else {
                drop = live_;
                fading_ = nullptr;
                xfading_ = false;
            }
            live_ = p;                                   // 先に差し替え、
            activeParts_ = std::min(std::max(p->activeParts, 1), numParts_);
            // その後で退役させる。この順序なら drop はもうどこからも読まれていない。
            delete retired_.exchange(drop, std::memory_order_acq_rel);
        }

        const std::size_t fl = static_cast<std::size_t>(fftLen_);

        // 入力 2B を FFT して周波数領域遅延線(FDL)に積む。
        std::copy(inBuf_.begin(), inBuf_.end(), wRe_.begin());
        std::fill(wIm_.begin(), wIm_.end(), 0.0f);
        fft_.transform(wRe_.data(), wIm_.data(), false);
        std::copy(wRe_.begin(), wRe_.end(), fdlRe_.begin() + static_cast<std::size_t>(fdlPos_) * fl);
        std::copy(wIm_.begin(), wIm_.end(), fdlIm_.begin() + static_cast<std::size_t>(fdlPos_) * fl);

        // 次ブロックの「前半」は今回の「後半」。
        std::copy(inBuf_.begin() + b_, inBuf_.end(), inBuf_.begin());

        if (!live_) {
            std::fill(outBuf_.begin(), outBuf_.end(), 0.0f);
            xfading_ = false;
            fdlPos_ = (fdlPos_ + 1) % numParts_;
            return;
        }

        convolveInto_(live_, activeParts_, outBuf_);
        // 混ぜている間だけ、同じ遅延線に古い世代の IR も掛ける（費用はフェード中だけ倍）。
        if (xfading_ && fading_) convolveInto_(fading_, fadingParts_, outBufOld_);
        else                     xfading_ = false;

        fdlPos_ = (fdlPos_ + 1) % numParts_;
    }

    // IR 1 世代ぶんを、共有の周波数領域遅延線(FDL)に掛けて dst へ書く。オーディオスレッド専用。
    void convolveInto_(const IrSpectra* ir, int parts, std::vector<float>& dst) {
        const std::size_t fl = static_cast<std::size_t>(fftLen_);
        for (int c = 0; c < channels_; ++c) {
            std::fill(accRe_.begin(), accRe_.end(), 0.0f);
            std::fill(accIm_.begin(), accIm_.end(), 0.0f);
            for (int p = 0; p < parts; ++p) {
                int slot = fdlPos_ - p;
                if (slot < 0) slot += numParts_;
                const float* xr = fdlRe_.data() + static_cast<std::size_t>(slot) * fl;
                const float* xi = fdlIm_.data() + static_cast<std::size_t>(slot) * fl;
                const float* hr = ir->re(c, p);
                const float* hi = ir->im(c, p);
                for (int k = 0; k < fftLen_; ++k) {
                    accRe_[static_cast<std::size_t>(k)] += xr[k] * hr[k] - xi[k] * hi[k];
                    accIm_[static_cast<std::size_t>(k)] += xr[k] * hi[k] + xi[k] * hr[k];
                }
            }
            fft_.transform(accRe_.data(), accIm_.data(), true);
            // overlap-save: 有効なのは後半 B サンプル（前半は循環畳み込みの巻き込み）。
            std::copy(accRe_.begin() + b_, accRe_.end(),
                      dst.begin() + static_cast<std::size_t>(c) * static_cast<std::size_t>(b_));
        }
    }

    const int b_;
    const int fftLen_;
    const int numParts_;
    const int channels_;
    Fft fft_;

    std::vector<float> fdlRe_, fdlIm_;      // [part][fftLen]
    int fdlPos_ = 0;

    std::vector<float> wRe_, wIm_, accRe_, accIm_;
    std::vector<float> inBuf_;              // 直前ブロック＋今のブロック（2B）
    std::vector<float> outBuf_;             // [channel][B]
    std::vector<float> outBufOld_;          // [channel][B] 混ぜている間の古い世代の出力
    // ブロック内の位置。入力の書き込みと出力の読み出しが同じ歩幅で進むので 1 本で足りる。
    int pos_ = 0;

    IrSpectra* live_ = nullptr;             // オーディオスレッドだけが触る
    std::atomic<IrSpectra*> pending_{nullptr};
    std::atomic<IrSpectra*> retired_{nullptr};
    int activeParts_ = 1;
    // IR の世代の混ぜ（遅延線は共有）。fading_ は差し替え前の世代。
    IrSpectra* fading_ = nullptr;           // オーディオスレッドだけが触る
    int  fadingParts_ = 0;
    std::atomic<int> xfadeLen_{0};          // 制御スレッドが決める長さ(サンプル)。0 で即差し替え
    int  xfadeSpan_ = 0;                    // このフェードの長さ（開始時に固定）
    int  xfadePos_ = 0;
    bool xfading_ = false;
};

}  // namespace dsp
}  // namespace af
