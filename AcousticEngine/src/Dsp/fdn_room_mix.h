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
// ■ 方向（手順 6、2026-09-09）
//   方向バス（DirectionBus）が差してあれば、L/R の 2 本でなく**レーンごとに独立した Hadamard の行**を出す
//   （8 レーンなら行 1〜8。行どうしは直交＝拡散状態で無相関なので、方向ごとに別のノイズ）。
//   部屋ごとにレーンの重み lane_k（Σ lane_k² = 1）を持ち、
//       lane_k = 正規化( spread·一様 + (1 − spread)·点(dir) )
//     ・dir    … その部屋の尾が来る向き（リスナー座標）。隣室なら戸口の向き（scene の fdnRoomWeights が
//                口の寄与のエネルギー重み付き平均で出す）。自室は向き無し
//     ・spread … 1 で一様（自室＝拡散音場）、0 で点。戸口越しは口の立体角で広がる（面の上では一様）
//   点は隣り合う 2 レーンへ等パワーで振る（反射タップと同じ流儀）。ITD はレーンの向きから Woodworth で
//   耳ごとに付ける（方向バスの HRIR は ITD を抜いてあるので、送る側が持つ規約）。
//   量は HRIR の平均パワーで割って等パワーのパンに揃える（VoiceRenderer の difNorm_ と同じ）。
//   ★退けた書き方: 方向プローブの分布を FOA に復号してレーンへ。プローブは周期を落として測り 250〜317 ms で
//     平滑する物なので、扉を開けた瞬間に尾の向きが追わない（扉の遅れの正体。DOOR_LAG.md）。
//     戸口の向きは幾何そのものなので毎フレーム出せる。自室の分布（プローブ）は後で足せる（spread の中身）。
//
// ■ 尾の開始（手順 5）はここでなく VoiceRenderer 側（音源ごとの ITDG を送りの前の遅延に）。
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
#include "direction_bus.h"

namespace af {
namespace dsp {

class FdnRoomMix {
public:
    static constexpr int kNumBands = FdnTail::kNumBands;
    static constexpr int kMaxRooms = 16;
    static constexpr int kMaxLanes = DirectionBus::kMaxLanes;
    static constexpr int kItdMax = 64;                     // レーンの ITD の上限（サンプル。48 kHz で 1.3 ms）

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
        // レーン用（方向バスがあるとき）。行 × 帯域のバッファと、一様のレーンの重み。
        // ★レーン 1 本につき**耳ごとに別の行**を持つ（2026-09-10）。同じ波形を左右へ複製すると
        //   両耳が完全に相関し、尾の左右差が消える（実測: 尾だけの両耳相関 バスあり 0.860 / なし 0.154）。
        //   反射タップなら 1 つの到来なので複製で正しいが、残響の尾は 1 つの到来ではない。
        r->rowBuf.assign(static_cast<std::size_t>(kMaxLanes) * 2 * kNumBands * N, 0.0f);
        for (int k = 0; k < kMaxLanes * 2; ++k)
            for (int b = 0; b < kNumBands; ++b)
                r->prow[k * kNumBands + b] = r->rowBuf.data() + (static_cast<std::size_t>(k) * kNumBands + b) * N;
        r->laneSig.assign(N, 0.0f);
        uniformLanes(r->laneCur); uniformLanes(r->laneTgt); uniformLanes(r->lanePend);
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

    /// 方向バスを差す（null で L/R の 2 本に戻る）。レーンの ITD を向きから Woodworth で作り、量を HRIR の平均パワーで揃える。
    ///   ★制御スレッド。オーディオが回る前か、ホストが器を作った直後に。
    void setDirectionBus(DirectionBus* bus, float headCircumferenceCm = 57.0f) {
        bus_ = bus;
        if (!bus) return;
        const int L = bus->lanes();
        const float radius = std::max(0.05f, headCircumferenceCm * 0.01f / (2.0f * 3.14159265f));   // 57 cm → 9.1 cm
        for (int k = 0; k < L && k < kMaxLanes; ++k) {
            float d[3]; bus->laneDirection(k, d);
            // Woodworth の球体近似 ITD = (r/c)(θ + sinθ)。θ は正中面からの角（|x| = sinθ）。右にある向きは左耳が遅れる。
            const float sx = std::min(1.0f, std::max(-1.0f, d[0]));
            const float th = std::asin(std::fabs(sx));
            const float itdSec = radius / 343.0f * (th + std::sin(th));
            const int itd = std::min(kItdMax, std::max(0, static_cast<int>(std::lround(itdSec * static_cast<float>(fs_)))));
            itdL_[k] = (sx > 0.0f) ? itd : 0;
            itdR_[k] = (sx < 0.0f) ? itd : 0;
        }
        laneNorm_ = 1.0f / std::sqrt(std::max(1e-6f, bus->meanPowerGain()));
        laneStride_ = maxFrames_ + kItdMax;
        laneRows_.assign(static_cast<std::size_t>(bus->rows()) * laneStride_, 0.0f);
        lanePrevN_ = 0;
    }
    const DirectionBus* directionBus() const { return bus_; }

    /// その部屋の尾が来る向き（リスナー座標: +x 右 / +z 前）と広がり（0 = 点、1 = 一様）。方向バスがあるときだけ効く。
    ///   隣室なら戸口の向きと口の立体角、自室なら spread = 1（scene の fdnRoomWeights が出す）。
    void setListenerDirection(int room, const float* dirLocal3, float spread) {
        if (!valid(room)) return;
        Room& r = *rooms_[static_cast<std::size_t>(room)];
        const int L = bus_ ? std::min(bus_->lanes(), kMaxLanes) : 0;
        float w[kMaxLanes] = {};
        uniformLanes(w);
        if (L > 0) {
            const float sp = std::min(1.0f, std::max(0.0f, spread));
            float pt[kMaxLanes] = {};
            const float x = dirLocal3 ? dirLocal3[0] : 0.0f, z = dirLocal3 ? dirLocal3[2] : 1.0f;
            if (sp < 1.0f && (x * x + z * z) > 1e-8f) {
                // 隣り合う 2 レーンへ等パワーで振る（反射タップと同じ）。レーン k の方位 = 2πk/L、+x 右・+z 前。
                float az = std::atan2(x, z); if (az < 0.0f) az += 2.0f * 3.14159265f;
                const float u = az / (2.0f * 3.14159265f) * static_cast<float>(L);
                const int k0 = static_cast<int>(u) % L, k1 = (k0 + 1) % L;
                const float t = u - static_cast<float>(static_cast<int>(u));
                pt[k0] += std::cos(t * 1.5707963f); pt[k1] += std::sin(t * 1.5707963f);
            } else {
                for (int k = 0; k < L; ++k) pt[k] = w[k];
            }
            double e = 0.0;
            for (int k = 0; k < L; ++k) { w[k] = sp * w[k] + (1.0f - sp) * pt[k]; e += static_cast<double>(w[k]) * w[k]; }
            const float g = (e > 1e-12) ? static_cast<float>(1.0 / std::sqrt(e)) : 1.0f;
            for (int k = 0; k < L; ++k) w[k] *= g;
            for (int k = L; k < kMaxLanes; ++k) w[k] = 0.0f;
        }
        for (int k = 0; k < kMaxLanes; ++k) r.lanePend[k] = w[k];
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
            for (int k = 0; k < nr; ++k) {
                Room& r = *rooms_[static_cast<std::size_t>(k)];
                for (int b = 0; b < kNumBands; ++b) r.wTgt[b] = r.wPend[b];
                for (int l = 0; l < kMaxLanes; ++l) r.laneTgt[l] = r.lanePend[l];
            }
            seen_ = v;
        }
        const float inv = 1.0f / static_cast<float>(n);
        double e = 0.0;
        DirectionBus* bus = bus_;
        const int L = bus ? std::min(bus->lanes(), kMaxLanes) : 0;
        if (bus && L > 0) {
            // ── 方向バスへ: レーンごとに独立した行 × 帯域の重み × レーンの重み。ITD を耳ごとに付けて行へ足す ──
            const int stride = laneStride_;
            const int R = bus->rows();
            for (int rr2 = 0; rr2 < R; ++rr2) {
                float* row = laneRows_.data() + static_cast<std::size_t>(rr2) * stride;
                // 前のブロックの末尾（ITD で溢れたぶん）を頭へ持ち越し、残りを 0 に。
                if (lanePrevN_ > 0) std::copy(row + lanePrevN_, row + lanePrevN_ + kItdMax, row);
                else std::fill(row, row + kItdMax, 0.0f);
                std::fill(row + kItdMax, row + n + kItdMax, 0.0f);
            }
            // 行はレーン×耳で別々に取る。行 0（M ＝ 全線の和）は使わないので 1..15 を順に回す。
            //   8 レーン × 2 耳 = 16 に対して使える行が 15 本なので、最後の 1 つだけ先頭と重なる。
            //   重なる 2 つは別のレーン（＝別のレーン重みと別の ITD）なので実害は小さい。
            int rows[kMaxLanes * 2];
            for (int l = 0; l < L; ++l)
                for (int ear = 0; ear < 2; ++ear)
                    rows[l * 2 + ear] = 1 + ((l * 2 + ear) % (FdnTail::kLines - 1));
            for (int k = 0; k < nr; ++k) {
                Room& r = *rooms_[static_cast<std::size_t>(k)];
                r.fdn->renderBandsRows(r.in.data(), n, rows, L * 2, r.prow);
                std::fill(r.in.begin(), r.in.begin() + n, 0.0f);
                float wStep[kNumBands];
                for (int b = 0; b < kNumBands; ++b) wStep[b] = (r.wTgt[b] - r.wCur[b]) * inv;
                // レーンの重みは 60 ms で追う（帯域の重み w は毎フレーム連続に来るのでブロック内で寄せ切る）。
                //   ★1 ブロックで寄せ切ると、正弦（1 つの周波数）では行ごとのモードの応答が違うぶん
                //     隣り合うブロックで 4.3 dB 跳んだ（実測。広帯域なら行のエネルギーは ±0.4 dB で揃う）。
                //     60 ms に伸ばすと 1 ブロックあたり 1 dB 未満。扉の量（w）は別に毎フレーム追うので遅れは向きだけ。
                const float laneFollow = std::min(1.0f, static_cast<float>(n) / (0.06f * static_cast<float>(fs_)));
                for (int l = 0; l < L; ++l) {
                    const float lt = r.laneCur[l] + (r.laneTgt[l] - r.laneCur[l]) * laneFollow;
                    const float l0 = r.laneCur[l], dl = (lt - l0) * inv;
                    bool any = (l0 != 0.0f || lt != 0.0f);
                    if (!any) continue;
                    // ★耳ごとに別の行から作る。ここが左右差の出所。
                    for (int ear = 0; ear < 2; ++ear) {
                        float* sig = r.laneSig.data();
                        const int src = l * 2 + ear;
                        for (int i = 0; i < n; ++i) {
                            float acc = 0.0f;
                            for (int b = 0; b < kNumBands; ++b) acc += r.prow[src * kNumBands + b][i] * (r.wCur[b] + wStep[b] * static_cast<float>(i + 1));
                            const float y = acc * (l0 + dl * static_cast<float>(i + 1)) * laneNorm_;
                            sig[i] = y;
                            if (ear == 0) e += static_cast<double>(y) * y;   // 計器は片耳ぶん（今までと同じ意味）
                        }
                        float* row = laneRows_.data() + static_cast<std::size_t>(l * 2 + ear) * stride
                                   + (ear == 0 ? itdL_[l] : itdR_[l]);
                        for (int i = 0; i < n; ++i) row[i] += sig[i];
                    }
                }
                for (int b = 0; b < kNumBands; ++b) r.wCur[b] = r.wTgt[b];
                for (int l = 0; l < kMaxLanes; ++l) r.laneCur[l] += (r.laneTgt[l] - r.laneCur[l]) * laneFollow;
            }
            bus->add(laneRows_.data(), n, stride, 1.0f, 0);
            lanePrevN_ = n;
            rmsL_ = static_cast<float>(std::sqrt(e * inv * 0.5));   // レーンの和の片耳ぶん（計器）
            return;
        }
        for (int k = 0; k < nr; ++k) {
            Room& r = *rooms_[static_cast<std::size_t>(k)];
            // 1) 送りを入れて回す（帯域ごとの L/R）。
            r.fdn->renderBandsMLR(r.in.data(), n, nullptr, r.pl, r.pr);
            std::fill(r.in.begin(), r.in.begin() + n, 0.0f);
            for (int l = 0; l < kMaxLanes; ++l) r.laneCur[l] = r.laneTgt[l];
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
        // 方向バス用: 行（レーン）× 帯域の出力と、レーンの重み（Σ² = 1）。
        std::vector<float> rowBuf;
            float* prow[kMaxLanes * 2 * kNumBands];   // [(レーン×2＋耳) × 帯域]
        std::vector<float> laneSig;
        float laneCur[kMaxLanes], laneTgt[kMaxLanes], lanePend[kMaxLanes];
    };

    bool valid(int room) const { return room >= 0 && room < count_.load(std::memory_order_acquire); }

    // 一様のレーンの重み（バスのレーン数で 1/√L。バスが無ければ 1/√kMaxLanes。どちらも Σ² = 1）。
    void uniformLanes(float* w) const {
        const int L = bus_ ? std::min(bus_->lanes(), kMaxLanes) : kMaxLanes;
        const float g = 1.0f / std::sqrt(static_cast<float>(L));
        for (int k = 0; k < kMaxLanes; ++k) w[k] = (k < L) ? g : 0.0f;
    }

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
    DirectionBus* bus_ = nullptr;
    float laneNorm_ = 1.0f;
    int   itdL_[kMaxLanes] = {}, itdR_[kMaxLanes] = {};   // レーンごとの耳の遅れ（サンプル）
    std::vector<float> laneRows_;                          // [行][maxFrames + kItdMax] バスへの送り（末尾は ITD の持ち越し）
    int   laneStride_ = 0;
    int   lanePrevN_ = 0;
    std::atomic<int> count_{0};
    std::atomic<int> version_{0};
    int seen_ = 0;
    float rmsL_ = 0.0f;
};

}  // namespace dsp
}  // namespace af
