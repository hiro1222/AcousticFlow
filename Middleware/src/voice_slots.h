// voice_slots.h — 鳴っている音を指す「札」の配り方と、寿命の見分け方。
//
// ★解いている問題
//   枠は使い回します。足音が鳴り終われば、その枠は次の音に回る。
//   ⚠ このとき**古い札で Stop を呼ぶと、無関係な音が止まります**。
//     ゲーム側は「自分が鳴らした音を止めた」つもりなので、原因に辿り着けません。
//     ハンドルを扱うコードのバグの大半がここから出ます。
//
// ★解き方 ── 札に**世代**を入れる
//   札 = 上位 32 ビット（世代）＋ 下位 32 ビット（枠の番号）。
//   枠を手放すたびに世代を 1 つ進めるので、**古い札は世代が合わず、その場で弾けます**。
//
//   ⚠ 世代を持たずに「使用中フラグ」だけで見分けようとすると通りません。
//     手放した枠がすぐ別の音に使われると、フラグは立ったままだからです。
//     **「使われているか」ではなく「同じ音か」**を見る必要があります。
//
// ★ここは確保しません。枠は最初に全部取ります（実時間スレッドが触るため）。

#pragma once

#include "afr_api.h"

#include <vector>

namespace afr {

inline AFR_Voice makeVoice(unsigned index, unsigned gen) {
    return (static_cast<AFR_Voice>(gen) << 32) | static_cast<AFR_Voice>(index);
}
inline unsigned voiceIndex(AFR_Voice v) {
    return static_cast<unsigned>(v & 0xFFFFFFFFull);
}
inline unsigned voiceGen(AFR_Voice v) {
    return static_cast<unsigned>(v >> 32);
}

class VoiceSlots {
public:
    // ★枠はここで全部取ります。以後は増やしません。
    void reset(int count) {
        if (count < 1) count = 1;
        gen_.assign(static_cast<std::size_t>(count), 1u);   // ⚠ 0 から始めない（下記）
        used_.assign(static_cast<std::size_t>(count), 0u);
        live_ = 0;
    }

    int capacity() const { return static_cast<int>(gen_.size()); }
    int liveCount() const { return live_; }

    // 空いている枠を 1 つ取る。無ければ AFR_VOICE_NONE。
    // ★誰を追い出すかはここで決めません ── 奪取の方針は上の層の仕事です。
    //   ここは「空きを配る」だけに絞ります（そのほうが書き直しやすい）。
    AFR_Voice acquire() {
        for (std::size_t i = 0; i < used_.size(); ++i) {
            if (!used_[i]) {
                used_[i] = 1u;
                ++live_;
                return makeVoice(static_cast<unsigned>(i), gen_[i]);
            }
        }
        return AFR_VOICE_NONE;
    }

    // その札が指す枠を手放す。★手放した瞬間に世代を進めます。
    // 既に手放されている／古い札なら false（**二度手放しをここで止める**）。
    bool release(AFR_Voice v) {
        if (!valid(v)) return false;
        const unsigned i = voiceIndex(v);
        used_[i] = 0u;
        // ⚠ 世代は必ず進める。進めないと、次にこの枠を配ったとき
        //   **古い札と新しい札が同じ値になります**。
        ++gen_[i];
        if (gen_[i] == 0u) gen_[i] = 1u;   // 一周したら 1 へ（0 は「無効」の意味に使う）
        --live_;
        return true;
    }

    // その札はいま生きているか。★古い札はここで落ちます。
    bool valid(AFR_Voice v) const {
        if (v == AFR_VOICE_NONE) return false;
        const unsigned i = voiceIndex(v);
        if (i >= gen_.size()) return false;
        return used_[i] != 0u && gen_[i] == voiceGen(v);
    }

    // その枠がいま配っている札を作り直す。★奪取のとき、番号しか無い所から
    //   手放すために要ります（番号→札の変換）。
    AFR_Voice handleAt(int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= gen_.size()) return AFR_VOICE_NONE;
        return makeVoice(static_cast<unsigned>(index), gen_[static_cast<std::size_t>(index)]);
    }

    // 枠の番号（呼ぶ前に valid を確かめること）。
    int indexOf(AFR_Voice v) const {
        return valid(v) ? static_cast<int>(voiceIndex(v)) : -1;
    }

private:
    // ⚠ 世代を 1 から始めます。0 だと、枠 0 の最初の札が
    //   `(0 << 32) | 0 == 0` ＝ AFR_VOICE_NONE と同じ値になり、
    //   **有効な札が「無効」と読まれます**。
    std::vector<unsigned> gen_;
    std::vector<unsigned char> used_;
    int live_ = 0;
};

}  // namespace afr
