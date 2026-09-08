// fdn_room_mix.h ── 部屋ごとの FDN の配線（2026-09-09、docs/TAIL_FDN_PLAN.md の手順 2）。
//
// ■ 何をする物か
//   部屋ごとに FdnTail を 1 本持ち、音源は自分の部屋（と、戸口越しに見える隣室）の FDN へ送り、
//   リスナーは自分の部屋の FDN（重み ＝ roomShare）と隣室の FDN（重み ＝ 開口率 α × 幾何）を聞く。
//   ★配線は**前向きだけ**（音源 → 部屋 → リスナー）。FDN どうしを信号で結び返さない（下）。
//
// ■ 連成残響（扉を閉めると部屋が乾く）はこの配線から出る
//   08-24 の実測: 吸う小部屋で扉を閉めると減衰 181 → 447 dB/s、RT60 0.33 → 0.13 s、量は 0.1 dB。
//   扉が開いていると、音源の音が戸口から隣の響く大部屋へ入り（音源→隣室の送り）、大部屋の遅い尾が
//   戸口からリスナーへ戻る（リスナー←隣室の重み）。小部屋の速い減衰が終わったあとに遅い尾が残るので、
//   **後半の減衰が隣室の速さになる**。閉めれば α = 0 で両方消え、自分の速さだけが残る。
//   IR の道では「開で焼いて閉を引く」が成り立たなかった（引く量が定義できない）。ここでは
//   配線のゲインなので、α が連続に動けば尾も連続に動く。
//
// ■ ★退けた書き方: 部屋どうしの拡散エネルギーの交換を、FDN の出力を相手の入口へ戻す信号の結合で作る
//   物理では「部屋 r のエネルギーのうち戸口から出る割合 ＝ 戸口の面積 × α ／ 総吸音面積」で、
//   往復の利得は 0.001 以下になる（小部屋 4.5% × 大部屋 1.7%）。それでも **α = 1 で発散した**
//   （実測: 0.5〜1 s の RMS 0.01 → 3〜4 s で 13,508。40 dB/s で増える）。
//   FDN は共振器で、モードの山では平均の 10〜100 倍の利得がある。2 つの共振器を信号で結び返すと
//   山が重なる周波数で往復の利得が 1 を超える。エネルギーの平均で見て小さくても、山では足りない。
//   物理の交換は「拡散エネルギー」の話で、位相の揃った信号の帰還ではない。だから信号では結ばない。
//   部屋自身の RT60 が扉で変わる分（開口が塞がれて吸音が減る）は**パラメータ側**で出す
//   （部屋グラフの開口の吸音率を α で動かす。手順 3）。二次の往復（隣室へ出た音がまた戻る）は
//   捨てる ── Raghuvanshi 2021 も同じ近似で、そこは聞こえない。
//
// ■ 帯域
//   隣室の重みは**帯域別**（開口率 α は帯域別に来る。狭い隙間ほど高域が通る）。
//   FdnTail が帯域ごとの L/R を出すので、帯域ごとに掛けて足すだけで済む（フィルタを足さない）。
//
// ■ 連続性
//   重みは目標を置くだけで、render がブロック内で線形に寄せる（FdnTail の係数と同じ流儀）。
//
// ■ スレッド規約
//   addRoom / setRoomRt60 / setListenerWeight … 制御スレッド（addRoom だけ確保する）
//   add / render … オーディオスレッド（同じスレッドから順に。TailBus と同じ）
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "fdn_tail.h"

namespace af {
namespace dsp {

class FdnRoomMix {
public:
    static constexpr int kNumBands = FdnTail::kNumBands;
    static constexpr int kMaxRooms = 16;

    FdnRoomMix(int sampleRate, int maxFrames, float diffusion = 0.6f)
        : fs_(std::max(8000, sampleRate)), maxFrames_(maxFrames > 0 ? maxFrames : 1024), diffusion_(diffusion) {}

    int sampleRate() const { return fs_; }
    int maxFrames() const { return maxFrames_; }
    int roomCount() const { return static_cast<int>(rooms_.size()); }

    // ── 制御スレッド ──

    /// 部屋を足す。lineScale は平均自由行程なり（FdnTail 参照）。戻り値は部屋の番号（0..）。
    ///   ★確保する。オーディオが回る前か、ホストが「この間 render は来ない」を保証できる時に呼ぶ。
    int addRoom(float lineScale, const float* rt60Sec6) {
        if (static_cast<int>(rooms_.size()) >= kMaxRooms) return -1;
        auto r = std::make_unique<Room>();
        r->fdn = std::make_unique<FdnTail>(fs_, diffusion_, lineScale);
        if (rt60Sec6) r->fdn->setRt60(rt60Sec6);
        const std::size_t N = static_cast<std::size_t>(maxFrames_);
        r->in.assign(N, 0.0f);
        for (int b = 0; b < kNumBands; ++b) {
            r->l[b].assign(N, 0.0f); r->rr[b].assign(N, 0.0f);
            r->pl[b] = r->l[b].data(); r->pr[b] = r->rr[b].data();
            r->wCur[b] = 0.0f; r->wTgt[b] = 0.0f; r->wPend[b] = 0.0f;
        }
        rooms_.push_back(std::move(r));
        return static_cast<int>(rooms_.size()) - 1;
    }

    void setRoomRt60(int room, const float* rt60Sec6) {
        if (!valid(room) || !rt60Sec6) return;
        rooms_[static_cast<std::size_t>(room)]->fdn->setRt60(rt60Sec6);
    }

    /// リスナーがその部屋の FDN をどれだけ聞くか（帯域別の振幅）。自分の部屋は roomShare、
    /// 隣室は 開口率 α × 幾何（戸口の立体角）。聞かない部屋は 0。
    void setListenerWeight(int room, const float* w6) {
        if (!valid(room) || !w6) return;
        Room& r = *rooms_[static_cast<std::size_t>(room)];
        for (int b = 0; b < kNumBands; ++b) r.wPend[b] = w6[b];
        version_.fetch_add(1, std::memory_order_release);
    }

    // ── オーディオスレッド ──

    /// 音源の送り。TailBus と同じ契約（dstOffset はこのブロックの中の位置、gain は線形にランプ）。
    ///   隣室へも送るときは、その部屋の番号でもう一度呼ぶ（gain ＝ 開口率 × 幾何）。
    void add(int room, const float* mono, int frames, float gainStart, float gainEnd, int dstOffset = 0) {
        if (!valid(room) || !mono || frames <= 0 || dstOffset < 0) return;
        Room& r = *rooms_[static_cast<std::size_t>(room)];
        const int end = std::min(dstOffset + frames, maxFrames_);
        if (end <= dstOffset) return;
        const int span = end - dstOffset;
        const float step = (span > 1) ? (gainEnd - gainStart) / static_cast<float>(span - 1) : 0.0f;
        float g = gainStart;
        for (int i = dstOffset; i < end; ++i) { r.in[static_cast<std::size_t>(i)] += mono[i - dstOffset] * g; g += step; }
    }
    void add(int room, const float* mono, int frames, float gain, int dstOffset = 0) { add(room, mono, frames, gain, gain, dstOffset); }

    /// 全部屋の FDN を回し、リスナーの L/R を**上書き**する。溜めた送りはここで消費される。
    void render(int frames, float* outL, float* outR) {
        const int n = std::min(frames, maxFrames_);
        if (outL) std::fill(outL, outL + std::max(0, frames), 0.0f);
        if (outR) std::fill(outR, outR + std::max(0, frames), 0.0f);
        if (n <= 0 || rooms_.empty()) return;
        // 版が変わっていれば目標を取り込む。
        const int v = version_.load(std::memory_order_acquire);
        if (v != seen_) {
            for (auto& rp : rooms_) for (int b = 0; b < kNumBands; ++b) rp->wTgt[b] = rp->wPend[b];
            seen_ = v;
        }
        const float inv = 1.0f / static_cast<float>(n);
        for (auto& rp : rooms_) {
            Room& r = *rp;
            // 1) 送りを入れて回す（帯域ごとの L/R）。
            r.fdn->renderBandsMLR(r.in.data(), n, nullptr, r.pl, r.pr);
            std::fill(r.in.begin(), r.in.begin() + n, 0.0f);
            // 2) リスナーの重み（帯域別、ブロック内でランプ）を掛けて足す。
            for (int b = 0; b < kNumBands; ++b) {
                const float w0 = r.wCur[b], dw = (r.wTgt[b] - r.wCur[b]) * inv;
                if (w0 == 0.0f && dw == 0.0f) continue;
                const float* L = r.l[b].data(); const float* R = r.rr[b].data();
                float w = w0;
                for (int i = 0; i < n; ++i) { w += dw; if (outL) outL[i] += L[i] * w; if (outR) outR[i] += R[i] * w; }
                r.wCur[b] = r.wTgt[b];
            }
        }
    }

    /// 診断: 部屋の FDN（線の長さ・係数を読む用）。
    const FdnTail* fdn(int room) const { return valid(room) ? rooms_[static_cast<std::size_t>(room)]->fdn.get() : nullptr; }

private:
    struct Room {
        std::unique_ptr<FdnTail> fdn;
        std::vector<float> in;
        std::vector<float> l[kNumBands], rr[kNumBands];
        float* pl[kNumBands]; float* pr[kNumBands];
        float wCur[kNumBands], wTgt[kNumBands], wPend[kNumBands];
    };

    bool valid(int room) const { return room >= 0 && room < static_cast<int>(rooms_.size()); }

    const int fs_;
    const int maxFrames_;
    const float diffusion_;
    std::vector<std::unique_ptr<Room>> rooms_;
    std::atomic<int> version_{0};
    int seen_ = 0;
};

}  // namespace dsp
}  // namespace af
