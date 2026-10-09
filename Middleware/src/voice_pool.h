// voice_pool.h — 同時発音の枠と、埋まったときの奪い方。
//
// ★これが「ボイス管理」です。`voice_bus.h`（道具の試聴用・8 本・優先度なし）は
//   踏み台で、**こちらが出来たら消えます**。それまで足し込みの形が 2 箇所にありますが、
//   片方は期限つきです。
//
// ── 解いている問題
//   ゲームでは音が上限を超えます。そのとき 3 つ決める必要があります。
//     ① 誰を止めるか（方針）
//     ② ⚠ **どうやって止めるか** ── 即停止は必ずクリックになる
//     ③ 止めている間、枠をどう扱うか
//
//   ⚠ ②③ が厄介です。**傾斜で落とすには 5ms かかるのに、新しい音は今すぐ鳴らしたい。**
//     素直に書くと「枠が空くまで待つ」＝新しい音が 5ms 遅れるか、
//     「即 0 にして即使う」＝プチッと鳴る、のどちらかになります。
//
// ★★ 答え ── **札（枠）と、音の入れ物を分ける**
//
//   最初は「奪った枠の音を予備へ**移す**」つもりでしたが、書けませんでした ──
//   ⚠ `ClipSource` は `std::atomic` を持つので**コピーも移動もできません**。
//     実時間スレッドが読んでいる最中に中身を移すこと自体が危うい、という設計の答えでもあります。
//
//   そこで動かすのを逆にしました。**音はその場に置いたまま、札のほうを付け替えます。**
//     ・奪われた入れ物は「傾斜中」になり、その場で下り切る
//     ・札は空いている別の入れ物に繋ぎ直される ＝ **新しい音は待たない**
//
//   ⚠ 予備も尽きたら、最後の手段として硬く止めます（クリックが出る）。
//     **そのときは数えます** ── 数えないと「たまにプチッと鳴る」としか分かりません。
//
// スレッド規約:
//   render()   … 実時間スレッドから。**確保しない・待たない**
//   それ以外   … 制御スレッドから（ゲームの指示は輪を通って来る）

#pragma once

#include "afr_api.h"
#include "block_source.h"
#include "voice_slots.h"

#include <memory>
#include <vector>

namespace afr {

// 誰を止めるか。
enum class StealPolicy {
    LowestPriorityThenOldest,   // 既定。優先度が同じなら古いほう
    OldestOnly,                 // 優先度を見ない
    RejectNew,                  // 奪わない。新しいほうを断る
};

class VoicePool {
public:
    // 消えかけを置く予備の数。
    // ★鳴らせる枠とは別勘定です。⚠ ここを 0 にすると、奪取のたびに
    //   「待つ」か「プチッと鳴る」のどちらかになります。
    static constexpr int kFadeReserve = 4;

    void reset(int capacity, int sampleRate) {
        if (capacity < 1) capacity = 1;
        cap_   = capacity;
        rate_  = sampleRate > 0 ? sampleRate : 48000;
        nSrc_  = cap_ + kFadeReserve;

        slots_.reset(cap_);
        // ★入れ物は**ここで 1 度だけ**取ります。以後 render では確保しません。
        //   ⚠ `std::vector<ClipSource>` にはできません（atomic を持つので移動できず、
        //     再確保で詰みます）。配列を 1 回 new する形にしてあります。
        src_ = std::make_unique<af::ClipSource[]>(static_cast<std::size_t>(nSrc_));
        state_.assign(static_cast<std::size_t>(nSrc_), 0);   // 0=空き 1=枠が使用中 2=傾斜中
        fadeAge_.assign(static_cast<std::size_t>(nSrc_), 0ull);
        srcOf_.assign(static_cast<std::size_t>(cap_), -1);
        prio_.assign(static_cast<std::size_t>(cap_), 0);
        age_.assign(static_cast<std::size_t>(cap_), 0ull);
        clock_    = 0;
        hardStop_ = 0;
        mix_.assign(4096u * 8u, 0.0f);
    }

    void prepare(int maxFrames, int channels) {
        const std::size_t need = static_cast<std::size_t>(maxFrames) *
                                 static_cast<std::size_t>(channels > 0 ? channels : 2);
        if (mix_.size() < need) mix_.assign(need, 0.0f);
    }

    int capacity() const { return cap_; }

    // 枠を 1 つ取る。埋まっていたら方針にしたがって奪います。
    // 取れなければ AFR_VOICE_NONE（RejectNew のとき、または全部が自分より上のとき）。
    AFR_Voice acquire(int priority, StealPolicy policy = StealPolicy::LowestPriorityThenOldest) {
        AFR_Voice v = slots_.acquire();
        if (v == AFR_VOICE_NONE) {
            if (policy == StealPolicy::RejectNew) return AFR_VOICE_NONE;
            const int victim = pickVictim(priority, policy);
            if (victim < 0) return AFR_VOICE_NONE;      // 全部が自分より大事だった
            retireSlot(victim);                         // ★音は傾斜へ。枠はここで空く
            v = slots_.acquire();
            if (v == AFR_VOICE_NONE) return AFR_VOICE_NONE;
        }
        const int i = slots_.indexOf(v);
        const int s = takeFreeSource();
        if (s < 0) { slots_.release(v); return AFR_VOICE_NONE; }

        srcOf_[static_cast<std::size_t>(i)] = s;
        state_[static_cast<std::size_t>(s)] = 1;
        prio_[static_cast<std::size_t>(i)]  = priority;
        age_[static_cast<std::size_t>(i)]   = ++clock_;
        return v;
    }

    // 明示的に止める。★ここも傾斜で落とします（枠はすぐ返ります）。
    bool release(AFR_Voice v) {
        const int i = slots_.indexOf(v);
        if (i < 0) return false;
        retireSlot(i);
        return true;
    }

    // その札が使っている入れ物。★呼ぶ側はここへ音を入れて play() します。
    af::ClipSource* find(AFR_Voice v) {
        const int i = slots_.indexOf(v);
        if (i < 0) return nullptr;
        const int s = srcOf_[static_cast<std::size_t>(i)];
        return s < 0 ? nullptr : &src_[static_cast<std::size_t>(s)];
    }
    bool valid(AFR_Voice v) const { return slots_.valid(v); }

    // [audio] 全部足して出す。
    void render(float* out, int frames, int channels) {
        const std::size_t n = static_cast<std::size_t>(frames) *
                              static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < n; ++i) out[i] = 0.0f;
        if (mix_.size() < n) return;                    // ⚠ ここで伸ばさない

        for (int s = 0; s < nSrc_; ++s) {
            if (state_[static_cast<std::size_t>(s)] == 0) continue;
            af::ClipSource& v = src_[static_cast<std::size_t>(s)];
            if (v.empty()) continue;
            v.render(mix_.data(), frames, channels);
            for (std::size_t i = 0; i < n; ++i) out[i] += mix_[i];
        }
        // 足して溢れたら挟む（挟まないと耳とスピーカーを痛める）。
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] >  1.0f) out[i] =  1.0f;
            if (out[i] < -1.0f) out[i] = -1.0f;
        }
        reclaim();
    }

    int liveCount() const { return slots_.liveCount(); }
    int fadingCount() const {
        int n = 0;
        for (int s = 0; s < nSrc_; ++s) if (state_[static_cast<std::size_t>(s)] == 2) ++n;
        return n;
    }

    // ⚠ 予備が尽きて**硬く止めた**回数。0 でなければクリックが出ています。
    //   数えないと「たまにプチッと鳴る」としか分かりません。
    unsigned long long hardStops() const { return hardStop_; }

    // 傾斜が下り切ったものを空きへ戻す。★render の中から呼ばれます。
    void reclaim() {
        for (int s = 0; s < nSrc_; ++s) {
            if (state_[static_cast<std::size_t>(s)] != 2) continue;
            af::ClipSource& v = src_[static_cast<std::size_t>(s)];
            // ⚠ `Stopped` だけでは足りません ── 止めた直後はまだ包絡が残っていて
            //   **音が出ています**。回収すると途中で切れます。
            if (v.empty() || (v.state() == af::ClipSource::State::Stopped && v.silent())) {
                v.setClip(std::vector<float>(), 0, rate_);
                state_[static_cast<std::size_t>(s)] = 0;
            }
        }
    }

private:
    // 誰を止めるか。返り値は枠の番号（-1 = 誰も止めない）。
    int pickVictim(int priority, StealPolicy policy) const {
        int best = -1;
        for (int i = 0; i < cap_; ++i) {
            const std::size_t a = static_cast<std::size_t>(i);
            if (policy == StealPolicy::LowestPriorityThenOldest) {
                // ⚠ 自分より優先度が高いものは止めません。
                //   ここを見ないと、遠くの環境音がセリフを殺します。
                if (prio_[a] > priority) continue;
            }
            if (best < 0) { best = i; continue; }
            const std::size_t b = static_cast<std::size_t>(best);
            if (policy == StealPolicy::LowestPriorityThenOldest && prio_[a] != prio_[b]) {
                if (prio_[a] < prio_[b]) best = i;
            } else if (age_[a] < age_[b]) {
                best = i;                               // 古いほう
            }
        }
        return best;
    }

    // その枠の音を「傾斜中」にして、札を手放す。★音は動かしません。
    void retireSlot(int index) {
        const std::size_t i = static_cast<std::size_t>(index);
        const int s = srcOf_[i];
        if (s >= 0) {
            af::ClipSource& v = src_[static_cast<std::size_t>(s)];
            if (v.empty()) {
                state_[static_cast<std::size_t>(s)] = 0;
            } else {
                v.stop();                               // 傾斜が始まる（即 0 ではない）
                state_[static_cast<std::size_t>(s)]   = 2;
                fadeAge_[static_cast<std::size_t>(s)] = ++clock_;
            }
            srcOf_[i] = -1;
        }
        slots_.release(slots_.handleAt(index));
    }

    // 空いている入れ物を 1 つ。無ければ一番古い傾斜中を硬く止めて奪う。
    int takeFreeSource() {
        for (int s = 0; s < nSrc_; ++s)
            if (state_[static_cast<std::size_t>(s)] == 0) return s;

        int oldest = -1;
        for (int s = 0; s < nSrc_; ++s) {
            if (state_[static_cast<std::size_t>(s)] != 2) continue;
            if (oldest < 0 || fadeAge_[static_cast<std::size_t>(s)] <
                              fadeAge_[static_cast<std::size_t>(oldest)]) oldest = s;
        }
        if (oldest < 0) return -1;

        // ⚠ ここはクリックが出ます。**最後の手段**なので数えます。
        ++hardStop_;
        src_[static_cast<std::size_t>(oldest)].setClip(std::vector<float>(), 0, rate_);
        state_[static_cast<std::size_t>(oldest)] = 0;
        return oldest;
    }

    VoiceSlots                          slots_;
    std::unique_ptr<af::ClipSource[]>   src_;      // 確保は reset で 1 回だけ
    std::vector<int>                    state_;    // 0=空き 1=枠が使用中 2=傾斜中
    std::vector<unsigned long long>     fadeAge_;
    std::vector<int>                    srcOf_;    // 枠 → 入れ物
    std::vector<int>                    prio_;
    std::vector<unsigned long long>     age_;
    std::vector<float>                  mix_;
    int                cap_  = 0;
    int                nSrc_ = 0;
    int                rate_ = 48000;
    unsigned long long clock_ = 0;
    unsigned long long hardStop_ = 0;
};

}  // namespace afr
