// command_queue.h — ゲームスレッド → オーディオスレッドへ指示を渡す輪。
//
// ★何のためか（3 つ）
//   ① **ロックを使わずにスレッドをまたぐ。**オーディオスレッドでロックを待つと、
//      ゲームスレッドがロックを持ったまま OS に横取りされたときに、
//      **優先度の高いほうが低いほうを待ちます**（優先度逆転）。
//      10ms 以内に音を作れず、ブツッと鳴る。⚠ **負荷が高いときだけ**起きるので
//      開発中は再現しません。
//   ② **ゲーム側が即戻る。**構造体を 1 つ書いて番号を 1 進めるだけ。
//   ③ ★ **これが「文脈付き比較」の記録媒体になります。**ゲームからの指示が
//      全部ここを通るなら、**輪を保存すること＝その 30 秒を保存すること**です。
//      録音では候補を差し替えられず、プレイし直しでは同じ 30 秒になりません。
//
// ★片側 1 本ずつ（SPSC）に限定しています。
//   積むのはゲームスレッド 1 本、取り出すのはオーディオスレッド 1 本。
//   これなら**両側とも待ちません**（wait-free）。多対多にすると一気に難しくなります。
//   ⚠ ゲーム側が複数スレッドから積みたいなら、**積む側だけロックして構いません** ──
//     オーディオスレッドが待たなければ実時間制約は破れないからです。
//
// ★確保しません。輪は `reset` で 1 度だけ取ります。

#pragma once

#include "afr_api.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace afr {

// 積む指示 1 つ。★POD に保つこと（そのまま書き出せば記録になる）。
enum class Cmd : unsigned {
    None = 0,
    Play,          // u = soundId,  a = fadeInMs
    Stop,          // a = fadeOutMs
    Pause,         // a = fadeMs
    Resume,        // a = fadeMs
    StopAll,       // a = fadeOutMs
    SetGainDb,     // a = dB
    SetPitch,      // a = 半音
    SetOcclusion,  // band[6]
    SetPosition,   // a,b,c = x,y,z
};

struct Command {
    // 適用するサンプル位置。★**0 は「すぐ」**。
    // ⚠ 時刻は単調に積むこと。FIFO なので、先の時刻を積むと
    //   **その後ろの「すぐ」も出てこなくなります**。
    unsigned long long frame = 0;
    Cmd       type  = Cmd::None;
    AFR_Voice voice = AFR_VOICE_NONE;
    unsigned  u     = 0;
    float     a = 0.0f, b = 0.0f, c = 0.0f;
    float     band[6] = {};
};

class CommandQueue {
public:
    // ★2 のべき乗へ切り上げます。取り出しは剰余ではなくマスクで済み、
    //   **オーディオスレッドで除算を踏みません**。
    // ⚠ 1 枠は常に空けます（満杯と空を同じ形にしないため）ので、
    //   実際に積めるのは capacity - 1 個です。
    void reset(int wanted) {
        if (wanted < 2) wanted = 2;
        std::size_t cap = 2;
        while (cap < static_cast<std::size_t>(wanted) + 1) cap <<= 1;
        buf_.assign(cap, Command{});
        mask_ = cap - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        failed_.store(0, std::memory_order_relaxed);
    }

    int capacity() const { return static_cast<int>(mask_); }   // 積める数

    // [game] 積む。満杯なら false。★確保しません。
    bool push(const Command& c) {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) {
            // ⚠ **新しいほうを捨てます。古いほうを上書きしません。**
            //   上書きすると、いちばん古い Play（＝ループを始めた指示）が消えて、
            //   **止められないループ音**が残ります。捨てるなら新しいほう。
            failed_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[h] = c;
        // ★ここは release でなければなりません。
        //   ⚠ relaxed にすると、**番号のほうが中身より先に見えます**。
        //     取り出す側が「1 つ増えた」と思って、まだ書かれていない枠を読みます。
        //     しかも**壊れ方が偶発的**で、再現しません。
        head_.store(next, std::memory_order_release);
        return true;
    }

    // [audio] 1 つ取り出す。★時刻が来ていないものは取り出しません。
    //   nowFrame までに適用すべきものだけが出てきます。
    bool pop(Command* out, unsigned long long nowFrame) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        // ★ここは acquire。push 側の release と対にして、
        //   **中身が書き終わっていることを保証**します。
        if (t == head_.load(std::memory_order_acquire)) return false;   // 空
        const Command& c = buf_[t];
        if (c.frame > nowFrame) return false;                          // まだ時刻が来ていない
        *out = c;
        tail_.store((t + 1) & mask_, std::memory_order_release);
        return true;
    }

    // [read] 積めなかった回数。
    //
    // ⚠ **「失われた数」ではありません。**積む側が粘り直せば失われていません。
    //   ゲームからの口（AFR_*）は**粘りません**（ゲームスレッドを止められないため）ので、
    //   あちらでは 1 回の失敗＝1 個の指示が届かなかった、になります。
    //   ★数える所と失われる所が違うので、名前を分けています ──
    //     「再生 2」と出しながら 1 本しか鳴っていなかった件と同じ間違いを避けるため。
    unsigned long long failedPushes() const {
        return failed_.load(std::memory_order_relaxed);
    }

    // [read] いま積まれている数（診断用。厳密な同時値ではありません）。
    int pending() const {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return static_cast<int>((h - t) & mask_);
    }

private:
    std::vector<Command> buf_;
    std::size_t          mask_ = 0;

    // ★別々のキャッシュラインに置きたい所ですが、まだ測っていないので
    //   足していません（測らずに効くはずの細工を入れない）。
    std::atomic<std::size_t> head_{0};   // 積む側だけが進める
    std::atomic<std::size_t> tail_{0};   // 取り出す側だけが進める
    std::atomic<unsigned long long> failed_{0};
};

}  // namespace afr
