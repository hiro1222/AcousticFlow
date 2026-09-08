// fdn_room_mix.h ── 部屋ごとの FDN の配線（2026-09-09、docs/TAIL_FDN_PLAN.md の手順 2・4）。
//
// ■ 何をする物か
//   部屋ごとに FdnTail を 1 本持ち、音源は自分の部屋（と、戸口越しに見える隣室）の FDN へ送り、
//   リスナーは自分の部屋の FDN（重み ＝ 占め方）と隣室の FDN（重み ＝ 開口率 α × 立体角）を聞く。
//   送り先と重みは scene.h の fdnRoomWeights が 1 つの式で出す（リスナー側も音源側も同じ式）。
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
//   （部屋グラフの開口の吸音率を α で動かす。手順 3 ＝ scene.h の roomRt60Live）。
//   二次の往復（隣室へ出た音がまた戻る）は捨てる ── Raghuvanshi 2021 も同じ近似で、そこは聞こえない。
//
// ■ 帯域
//   隣室の重みは**帯域別**（開口率 α は帯域別に来る。狭い隙間ほど高域が通る）。
//   FdnTail が帯域ごとの L/R を出すので、帯域ごとに掛けて足すだけで済む（フィルタを足さない）。
//
// ■ 帯域の色（colour、手順 4 で足した）
//   FdnTail は帯域ごとにエネルギー 1 に正規化してあるので、そのままだと尾のスペクトルは入力のまま（平ら）。
//   拡散音場の定常エネルギーは帯域ごとに RT60_b に比例し、畳み込みの尾（エコグラム）はそれを帯域ごとの
//   総量として持っていた。同じ形にするため、帯域 b の入力を √(RT60_b / T̄) 倍する。
//   T̄ は**帯域幅で重み付けした平均**（Σ BW_b·RT60_b / Σ BW_b）。こうすると白色の入力に対して IR 全体の
//   エネルギーが 1 のまま（＝ ReverbTailIr の「IR 全体でエネルギー 1」と同じ規約）で、形だけが Sabine の色になる。
//   ★退けた書き方: 500 Hz 帯を基準（その帯域だけ量 1）にする。物理の言い方としては素直だが、白色の
//     入力では上の帯域（2.8k〜24k で帯域幅の 88%）が −4 dB 落ちるぶん全体が −4 dB 下がり、畳み込みと
//     並べると「FDN が小さい」に引っ張られる（実測 2026-09-09: 同じ部屋で合計 −2.0 dB、中域 −6〜−7 dB）。
//     A/B は量でなく形を比べたいので、量の規約は畳み込みに揃える。colour=false なら平ら。
//
// ■ 連続性
//   重みは目標を置くだけで、render がブロック内で線形に寄せる（FdnTail の係数と同じ流儀）。
//
// ■ スレッド規約
//   addRoom / setRoomRt60 / setListenerWeight … 制御スレッド
//     ★addRoom はオーディオが回っていても呼べる: 枠は固定（kMaxRooms）で、確保と校正を済ませてから
//       数を上げる（release）。render はその時点の数までしか見ない（acquire）。
//       減らす・作り直すは新しい FdnRoomMix を作って差し替える（ホスト側。古い物はオーディオが確実に離れてから捨てる）。
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
    int roomCount() const { return count_.load(std::memory_order_acquire); }

    // ── 制御スレッド ──

    /// 部屋を足す。lineScale は平均自由行程なり（FdnTail 参照）。戻り値は部屋の番号（0..）、満杯なら -1。
    ///   確保と校正（数十 ms）をしてから数を上げるので、オーディオが回っていてもよい。
    int addRoom(float lineScale, const float* rt60Sec6, bool colour = true) {
        const int n = count_.load(std::memory_order_acquire);
        if (n >= kMaxRooms) return -1;
        auto r = std::make_unique<Room>();
        r->fdn = std::make_unique<FdnTail>(fs_, diffusion_, lineScale);
        const std::size_t N = static_cast<std::size_t>(maxFrames_);
        r->in.assign(N, 0.0f);
        for (int b = 0; b < kNumBands; ++b) {
            r->l[b].assign(N, 0.0f); r->rr[b].assign(N, 0.0f);
            r->pl[b] = r->l[b].data(); r->pr[b] = r->rr[b].data();
            r->wCur[b] = 0.0f; r->wTgt[b] = 0.0f; r->wPend[b] = 0.0f;
        }
        if (rt60Sec6) setRt60Impl(*r, rt60Sec6, colour);
        rooms_[static_cast<std::size_t>(n)] = std::move(r);
        count_.store(n + 1, std::memory_order_release);
        return n;
    }

    /// 帯域別 RT60（毎フレーム置いてよい。目標を置くだけ）。colour なら帯域の色を付ける（上の■）。
    void setRoomRt60(int room, const float* rt60Sec6, bool colour = true) {
        if (!valid(room) || !rt60Sec6) return;
        setRt60Impl(*rooms_[static_cast<std::size_t>(room)], rt60Sec6, colour);
    }

    /// リスナーがその部屋の FDN をどれだけ聞くか（帯域別の振幅）。自分の部屋は占め方、
    /// 隣室は 開口率 α × 立体角（scene.h の fdnRoomWeights）。聞かない部屋は 0。
    void setListenerWeight(int room, const float* w6) {
        if (!valid(room) || !w6) return;
        Room& r = *rooms_[static_cast<std::size_t>(room)];
        for (int b = 0; b < kNumBands; ++b) r.wPend[b] = w6[b];
        version_.fetch_add(1, std::memory_order_release);
    }

    // ── オーディオスレッド ──

    /// 音源の送り。TailBus と同じ契約（dstOffset はこのブロックの中の位置、gain は線形にランプ）。
    ///   隣室へも送るときは、その部屋の番号でもう一度呼ぶ（gain ＝ 開口率 × 立体角）。
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

    /// 全部屋の FDN を回し、リスナーの L/R へ**足す**（TailBus::render と同じ契約。上書きしない）。
    /// 溜めた送りはここで消費される。送りが無いブロックでも呼ぶこと（遅延線を進める）。
    void render(int frames, float* outL, float* outR) {
        const int n = std::min(frames, maxFrames_);
        const int nr = count_.load(std::memory_order_acquire);
        if (n <= 0 || nr <= 0) { rmsL_ = 0.0f; return; }
        // 版が変わっていれば目標を取り込む。
        const int v = version_.load(std::memory_order_acquire);
        if (v != seen_) {
            for (int k = 0; k < nr; ++k) { Room& r = *rooms_[static_cast<std::size_t>(k)]; for (int b = 0; b < kNumBands; ++b) r.wTgt[b] = r.wPend[b]; }
            seen_ = v;
        }
        const float inv = 1.0f / static_cast<float>(n);
        double e = 0.0;
        for (int k = 0; k < nr; ++k) {
            Room& r = *rooms_[static_cast<std::size_t>(k)];
            // 1) 送りを入れて回す（帯域ごとの L/R）。
            r.fdn->renderBandsMLR(r.in.data(), n, nullptr, r.pl, r.pr);
            std::fill(r.in.begin(), r.in.begin() + n, 0.0f);
            // 2) リスナーの重み（帯域別、ブロック内でランプ）を掛けて足す。
            for (int b = 0; b < kNumBands; ++b) {
                const float w0 = r.wCur[b], dw = (r.wTgt[b] - r.wCur[b]) * inv;
                if (w0 == 0.0f && dw == 0.0f) continue;
                const float* L = r.l[b].data(); const float* R = r.rr[b].data();
                float w = w0;
                for (int i = 0; i < n; ++i) {
                    w += dw;
                    const float yl = L[i] * w;
                    if (outL) outL[i] += yl;
                    if (outR) outR[i] += R[i] * w;
                    e += static_cast<double>(yl) * yl;
                }
                r.wCur[b] = r.wTgt[b];
            }
        }
        rmsL_ = static_cast<float>(std::sqrt(e * inv));
    }

    /// 直前のブロックでリスナーへ足した尾の RMS（左）。計器用。
    float rms() const { return rmsL_; }

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

    bool valid(int room) const { return room >= 0 && room < count_.load(std::memory_order_acquire); }

    void setRt60Impl(Room& r, const float* rt60, bool colour) {
        float scale[kNumBands];
        if (colour) {
            // 帯域幅（LR4 の交点 177/354/707/1414/2828 Hz と fs/2 から）。
            const float bw[kNumBands] = { 177.0f, 177.0f, 353.0f, 707.0f, 1414.0f, std::max(1.0f, 0.5f * fs_ - 2828.0f) };
            double num = 0.0, den = 0.0;
            for (int b = 0; b < kNumBands; ++b) { num += static_cast<double>(bw[b]) * std::max(rt60[b], 1e-3f); den += bw[b]; }
            const double tbar = num / std::max(den, 1e-6);
            for (int b = 0; b < kNumBands; ++b) scale[b] = static_cast<float>(std::sqrt(std::max(rt60[b], 1e-3f) / tbar));
        } else {
            for (int b = 0; b < kNumBands; ++b) scale[b] = 1.0f;
        }
        r.fdn->setRt60(rt60, scale);
    }

    const int fs_;
    const int maxFrames_;
    const float diffusion_;
    std::unique_ptr<Room> rooms_[kMaxRooms];
    std::atomic<int> count_{0};
    std::atomic<int> version_{0};
    int seen_ = 0;
    float rmsL_ = 0.0f;
};

}  // namespace dsp
}  // namespace af
