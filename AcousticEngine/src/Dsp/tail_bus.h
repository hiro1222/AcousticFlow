/* tail_bus.h — 後期残響の尾を**音源で共有する**畳み込みバス。
 *
 * ★何のためにあるか
 *   これまで尾の畳み込みは音源 1 本につき 1 個だった（VoiceRenderer が
 *   NonUniformConvolver を 2 面ずつ持つ）。音源数に比例してオーディオスレッドの
 *   費用が増える。しかも**同じ部屋の音源は同じ IR を使う**ので、まるごと無駄。
 *
 * ★なぜ 1 本にまとめてよいか（近似ではない）
 *   畳み込みは線形なので、同じ IR に対して
 *       conv(IR, g1·x1) + conv(IR, g2·x2) = conv(IR, g1·x1 + g2·x2)
 *   が**厳密に**成り立つ。音源ごとのレベル g は足す前に掛ければよい。
 *   つまり「音源ごとのレベル」は保ったまま、畳み込みだけを 1 回にできる。
 *   ★逆に言えば、**IR が違う音源を同じバスへ入れてはいけない**。部屋ごとに 1 本持つ。
 *
 * ★順序の問題と、その避け方（Unity）
 *   音源の OnAudioFilterRead が呼ばれる順は保証されない。だから「全員が送り終えてから
 *   畳む」を素直に書けない。1 ブロック遅らせる手もあるが、尾の立ち上がりが
 *   DSP バッファぶん（48kHz/1024 で 21ms）ずれる。この作品は「部屋に入る瞬間」を
 *   主題にしているので、そこを鈍らせたくない。
 *   → **AudioListener に付けたフィルタは全音源のミックス後に走る**ことを使う。
 *     音源は送るだけ、バスの畳み込みはリスナー側で 1 回。**遅延は増えない。**
 *
 * ★スレッド
 *   add() と render() は**同じオーディオスレッドから順に**呼ばれる前提。
 *   setIr() は制御スレッドから。IR の差し替えは並走クロスフェードで行う
 *   （1 つの器で差し替えると、遅延線に溜まった過去の入力が新しい IR で畳み直されて
 *     実測 +1.97dB のふくらみが 0.35 秒続く。VoiceRenderer と同じ理由）。
 */
#pragma once

#include <algorithm>
#include <vector>

#include <cmath>

#include "nonuniform_convolver.h"

namespace af {
namespace dsp {

class TailBus {
public:
    TailBus(int tailSamples, int firstBlock, int capBlock, int maxFrames, int sampleRate)
        : maxFrames_(maxFrames > 0 ? maxFrames : 1024),
          sampleRate_(sampleRate > 0 ? sampleRate : 48000),
          convA_(tailSamples, 2, firstBlock, capBlock, maxFrames_),
          convB_(tailSamples, 2, firstBlock, capBlock, maxFrames_) {
        in_.assign(static_cast<size_t>(maxFrames_), 0.0f);
    }

    /// クロスフェードの長さ(ms)。0 で即差し替え。
    void setCrossfadeMs(float ms) {
        xfadeLen_ = std::max(0, static_cast<int>(ms * 0.001f * static_cast<float>(sampleRate_)));
    }

    /// 新しい尾 IR を入れる（部屋の代表音源だけが呼ぶ）。
    ///   ★入れ替えるのはクロスフェードが有効なときだけ。無効時に入れ替えると、
    ///     過去の入力が空の器へ切り替わって尾が無音になる（VoiceRenderer で実測 -227dB）。
    void setIr(const float* const* ir, const int* len) {
        if (xfadeLen_ > 0 && cur().hasIr()) {
            useB_ = !useB_;
            xfade_ = 0;
            xfading_ = true;
        }
        cur().setIr(ir, len);
        hasIr_ = true;
    }

    bool hasIr() const { return hasIr_; }

    /// 音源からの送り。**レベルを掛けてから足す**（線形なので畳み込み前で等価）。
    ///   dstOffset … このブロックの中での書き込み位置。
    ///   ★VoiceRenderer::render は 1 回のコールバックを maxFrames ごとに分けて処理する。
    ///     offset を渡さないと、2 つ目以降のチャンクが**先頭に重ね書き**されて音が壊れる。
    ///     Unity の既定（maxFrames == バッファ長）では分割が起きないので普段は出ないが、
    ///     バッファ設定次第で突然出る類の壊れ方になる。
    void add(const float* mono, int frames, float gain, int dstOffset = 0) {
        if (!mono || frames <= 0 || gain == 0.0f || dstOffset < 0) return;
        const int end = std::min(dstOffset + frames, maxFrames_);
        if (end <= dstOffset) return;
        if (pending_ < end) {
            // このブロックで初めて触る範囲は 0 で埋めてから足す（前ブロックの残りを消す）。
            std::fill(in_.begin() + pending_, in_.begin() + end, 0.0f);
            pending_ = end;
        }
        for (int i = dstOffset; i < end; ++i)
            in_[static_cast<size_t>(i)] += mono[i - dstOffset] * gain;
    }

    /// 溜まった送りを 1 回だけ畳んで outL/outR へ**足す**。呼んだ時点で送りは空になる。
    ///   ★音源が 1 本も送っていないブロックでも呼ぶこと。畳み込み器の遅延線を
    ///     進めないと、次に送りが来たときに尾が飛ぶ（無音を入れて進めるのが正しい）。
    void render(int frames, float* outL, float* outR) {
        if (!outL || !outR || frames <= 0) return;
        const int n = std::min(frames, maxFrames_);
        if (pending_ < n) {
            std::fill(in_.begin() + pending_, in_.begin() + n, 0.0f);
            pending_ = n;
        }
        if (scratchL_.size() < static_cast<size_t>(n)) {
            scratchL_.assign(static_cast<size_t>(maxFrames_), 0.0f);
            scratchR_.assign(static_cast<size_t>(maxFrames_), 0.0f);
        }
        std::fill(scratchL_.begin(), scratchL_.begin() + n, 0.0f);
        std::fill(scratchR_.begin(), scratchR_.begin() + n, 0.0f);

        // 等振幅のクロスフェード（等パワーだと相関のある信号なので +3dB ふくらむ）。
        float wNew = 1.0f, wOld = 0.0f;
        if (xfading_ && xfadeLen_ > 0) {
            const float t = static_cast<float>(xfade_) / static_cast<float>(xfadeLen_);
            wNew = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            wOld = 1.0f - wNew;
        } else {
            xfading_ = false;
        }
        float* dst[2] = { scratchL_.data(), scratchR_.data() };
        if (cur().hasIr() && wNew > 0.0f)
            cur().processAdd(in_.data(), 0, n, dst, 0, wNew);
        if (xfading_ && old().hasIr() && wOld > 0.0f)
            old().processAdd(in_.data(), 0, n, dst, 0, wOld);
        if (xfading_) {
            xfade_ += n;
            if (xfade_ >= xfadeLen_) { xfading_ = false; xfade_ = 0; }
        }
        for (int i = 0; i < n; ++i) { outL[i] += scratchL_[static_cast<size_t>(i)];
                                     outR[i] += scratchR_[static_cast<size_t>(i)]; }
        rms_ = 0.0f;
        for (int i = 0; i < n; ++i)
            rms_ += scratchL_[static_cast<size_t>(i)] * scratchL_[static_cast<size_t>(i)];
        rms_ = (n > 0) ? std::sqrt(rms_ / static_cast<float>(n)) : 0.0f;
        pending_ = 0;      // 次のブロックへ
    }

    /// 直近ブロックの尾の RMS（左）。音源ごとの計器が使えなくなるので、ここで出す。
    float rms() const { return rms_; }
    int partitions() const { return const_cast<TailBus*>(this)->cur().totalPartitions(); }

private:
    NonUniformConvolver& cur() { return useB_ ? convB_ : convA_; }
    NonUniformConvolver& old() { return useB_ ? convA_ : convB_; }

    int maxFrames_;
    int sampleRate_;
    NonUniformConvolver convA_;
    NonUniformConvolver convB_;
    bool useB_ = false;
    bool hasIr_ = false;
    bool xfading_ = false;
    int xfade_ = 0;
    int xfadeLen_ = 0;
    int pending_ = 0;               // このブロックで既に 0 埋め済みの長さ
    float rms_ = 0.0f;
    std::vector<float> in_;
    std::vector<float> scratchL_;
    std::vector<float> scratchR_;
};

}  // namespace dsp
}  // namespace af
