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
// ■ 点と拡散を分ける（laneModel 1、2026-09-12。既定）
//   88d5f0b で「レーンの耳ごとに別の行」にした（自室の尾だけの両耳相関 0.860 → 0.354）。同じ扱いが「点」
//   （戸口越しの尾）にも当たり、2 つ壊れていた（DspRegressionTest [FDN・方向] の 2 件。探りで切り分けた）:
//     ・点でも左右が別の波形なので相関が無い（右 90° の点で IACC 0.023 ＝ 独立な 2 行を並べた床 0.026）。
//       ITD を切っても変わらない ＝ 上の Woodworth の ITD が効いていない。向きの手がかりが ILD だけになる。
//     ・向きが変わると聞こえる行が別の行に替わる ＝ 尾の波形が入れ替わる。持続音では行ごとに振幅も位相も違うので、
//       正弦 48 点（ブロック同期）で隣り合うブロックの段差が最大 12.26 dB、くぼみ −24 dB。90°/s でなめらかに回しても 9.29 dB。
//       （88d5f0b より前の「左右へ複製」でも 11/48 点で落ちる。220 Hz 1 点の検査で通っていたのは運）
//   そこで 1 部屋の尾を 2 つに分ける:
//     ・拡散（量の ed）  … 今までどおり枠（レーン×2＋耳）ごとに行 1 + (枠 % 15)。重みは一様 × √ed。
//                          広がり 1（自室）は laneModel 0 と 1 ビットも同じ。
//     ・点（量の 1−ed）  … 行 1 を **1 本の波形** にし、耳ごとに「点の向き」の ITD（小数）で遅らせてから、
//                          隣り合う 2 レーンへ等パワーで振る。1 つの到来は 1 本の波形・ITD は到来ごと・HRIR はレーン、
//                          という反射タップ（面の線音源）と同じ決まり。向きが変わっても波形は替わらず、重みと遅れが動くだけ。
//     ・ed = sp² / (sp² + (1 − sp)²) … 今までの「振幅で混ぜて正規化」と同じ量の配分（一様と点を直交とみたとき）。
//   ITD は 60 ms で追い、ブロック内で線形に動かす。小数の遅れは 8 点の Lanczos（a = 4、1/256 刻みの表）で読む。
//   読みの窓の半分（4 サンプル ＝ 0.08 ms）だけ両耳に同じ遅れが乗る。点が鳴っていないときは遅れを目標へ跳ばす。
//   ★退けた書き方:
//     (1) 点の波形に行 0（M ＝ 全線の和）。行 1〜15 との量の比が部屋で −0.1〜+3.7 dB（帯域別で最大 +8.5 dB）動き、
//         定数で揃えられない（注ぎ込みが全線に同じ符号で入るので、低域・短い線ほど行 0 だけ揃って足される）。
//     (2) 点の ITD をレーンの整数のまま。レーンの間を渡るとき遅れの違う 2 本を混ぜるので、33 サンプル差なら
//         727 Hz の奇数倍で打ち消し合う（最大 10.72 dB、12/48 点）。
//     (3) 小数の遅れを線形補間で読む。遠い耳の高域が痩せて −2.5 dB（Lanczos で −0.6 dB）。
//     (4) 拡散を 14 本で回して行 15 を点専用に。同じ耳に同じ波形が 2 回載り、自室の左耳が +1.16 dB。
//     (5) 点を行 15（拡散の枠 1 つと共用）。広がり 0.5 の両耳相関が 0.313（行 1 は 0.204）、部屋ごとの量の揺れも大きい。
//   残り: 行 1 は拡散の枠 0（レーン 0 の左耳）と枠 15（レーン 7 の右耳）と同じ波形なので、広がりの途中では
//     点と拡散が少し相関する（広がり 0.5 で両耳相関 0.204、量は +0.55 dB 以内）。戸口をまたいで点から一様へ移るときは
//     波形が点から拡散の行へ替わるので、持続音で段差が出る（どの作りでも残る。段 2-g の「残り」と同じ物）。
//
// ■ 尾の配り方の形（2026-09-19、setListenerLaneShape）
//   自分の部屋の尾は今まで全レーンへ一様（拡散音場）だった。形を置くと「一様」の代わりにその形で配る
//   （レーン k のエネルギーの割合。World が耳から各レーンの向きの壁までの距離で作る ── 近い壁の側ほど濃い）。
//   点（戸口越しの向き）との混ぜ方は今までと同じ式で、一様の所が形に替わるだけ。形を置かなければ 1 ビットも同じ。
//   上・下のレーン（DirectionBus の verticalLanes）があれば、天井・床の近さでも配る。点の振り分けは水平の環だけ。
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
#include "hrtf_processor.h"

namespace af {
namespace dsp {

class FdnRoomMix {
public:
    static constexpr int kNumBands = FdnTail::kNumBands;
    static constexpr int kMaxRooms = 16;
    static constexpr int kMaxLanes = DirectionBus::kMaxLanes;
    static constexpr int kItdMax = 64;                     // レーンの ITD の上限（サンプル。48 kHz で 1.3 ms）

    FdnRoomMix(int sampleRate, int maxFrames, float diffusion = 0.6f)
        : fs_(std::max(8000, sampleRate)), maxFrames_(maxFrames > 0 ? maxFrames : 1024), diffusion_(diffusion) {
        // 点の ITD を小数で読む係数表（laneModel 1）。最初の 1 回だけ作る（制御スレッド。オーディオスレッドでは作らない）。
        lanczos_ = lanczosTable();
        // 戸口の線音源の器は作るときに全部確保する（オーディオスレッドでは確保しない）。
        for (int s = 0; s < kMaxPortals; ++s) {
            Portal& p = portals_[s];
            for (int k = 0; k < kPortalPoints; ++k) p.hp[k] = std::make_unique<HrtfProcessor>(fs_);
            p.sig.assign(static_cast<std::size_t>(kPortalPoints) * static_cast<std::size_t>(maxFrames_), 0.0f);
            p.tmp.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
            p.feed.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
            p.scratchL.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
            p.scratchR.assign(static_cast<std::size_t>(maxFrames_), 0.0f);
        }
    }

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
        // ★部屋の番号を種に渡す（2026-09-11）。同じ大きさの部屋が同じ遅延線になると出力が相関 1 になり、
        //   下の render が前提にしている「部屋どうしは無相関なのでエネルギーで足す」が崩れる（FdnTail の■部屋ごとの種）。
        //   0 番の部屋は今までと 1 サンプルも同じ。
        r->fdn = std::make_unique<FdnTail>(fs_, diffusion_, lineScale, static_cast<std::uint32_t>(n));
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
        // laneModel 1 用: 拡散は一様、点は無し。点の波形の履歴と耳ごとの読み出し。
        uniformLanes(r->difCur); uniformLanes(r->difTgt); uniformLanes(r->difPend);
        for (int k = 0; k < kMaxLanes; ++k) { r->ptCur[k] = 0.0f; r->ptTgt[k] = 0.0f; r->ptPend[k] = 0.0f; }
        for (int ear = 0; ear < 2; ++ear) {
            r->itdCur[ear] = 0.0f; r->itdTgt[ear] = 0.0f; r->itdPend[ear] = 0.0f;
            r->ptEar[ear].assign(N, 0.0f);
        }
        r->ptHist.assign(kPtHist, 0.0f);
        r->ptPos = 0;
        r->dirX = 0.0f; r->dirZ = 1.0f; r->spread = 1.0f;
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
        // ★差す前に作った部屋のレーンの重みを、このバスのレーン数で揃え直す（2026-09-11）。
        //   addRoom はバスが無いと kMaxLanes 本で一様（1/√kMaxLanes）にする。あとから少ないレーンのバスを差すと
        //   鳴るのはバスのレーンだけで Σw² が 1 に足りず、**尾がそのぶん小さかった**（8 レーンなら半分 ＝ −3 dB）。
        //   World も Unity も試聴の道具も「部屋を入れてからバスを差す」順なので、全部の尾に効いていた。
        //   setListenerDirection は置くたびにバスのレーン数で正規化するので、向きを置く道（段 2-f の戸口越し）だけ
        //   正しい量に戻り、切り替えの A/B が向きでなく量でずれていた（検査 [後期の向き] ⑧ で見つかった）。
        headRadius_ = radius;
        const int nr = count_.load(std::memory_order_acquire);
        for (int k = 0; k < nr; ++k) {
            Room& r = *rooms_[static_cast<std::size_t>(k)];
            r.hasShape = false;                     // 形はレーンの数ごとの物なので、バスを差し直したら外す（World が次のフレームで置き直す）
            renormLanes(r.laneCur); renormLanes(r.laneTgt); renormLanes(r.lanePend);
            // laneModel 1 の重みは、最後に置かれた向きからこのバスのレーン数で作り直す（器を作った直後なので今・目標・置いた値を揃える）。
            //   点が無ければ拡散は laneModel 0 の重みをそのまま写す（今・目標もそれぞれ写す ＝ 広がり 1 で 1 ビットも同じ）。
            const bool point = splitWeights(r.dirX, r.dirZ, r.spread, std::min(L, kMaxLanes), r.lanePend, r.difPend, r.ptPend, r.itdPend, nullptr);
            for (int l = 0; l < kMaxLanes; ++l) {
                r.difTgt[l] = point ? r.difPend[l] : r.laneTgt[l];
                r.difCur[l] = point ? r.difPend[l] : r.laneCur[l];
                r.ptTgt[l] = r.ptPend[l];
                r.ptCur[l] = r.ptPend[l];
            }
            for (int ear = 0; ear < 2; ++ear) r.itdTgt[ear] = r.itdCur[ear] = r.itdPend[ear];
        }
    }
    const DirectionBus* directionBus() const { return bus_; }

    /// 尾のレーンの作り（2026-09-12、上の■点と拡散を分ける）。**既定 1。**実行中に変えてよい（試聴の A/B）。
    ///   0 耳ごとの行（88d5f0b〜）: 点の向きでも左右が別の波形。ITD が効かず、向きが変わると尾の波形が入れ替わる
    ///   1 点と拡散を分ける: 拡散は 0 と同じ行（広がり 1 は 1 ビットも同じ）、点は行 1 を 1 本の波形＋点の向きの ITD
    ///   ★切り替えた瞬間は波形が替わるので 1 回鳴る（重みと遅れはどちらの作りでも毎ブロック追っているので、そこは揃っている）。
    void setLaneModel(int model) { laneModel_.store(model <= 0 ? 0 : 1, std::memory_order_release); }
    int laneModel() const { return laneModel_.load(std::memory_order_acquire); }

    /// 戸口の線音源とレーンの受け渡しの時間（秒、2026-10-04）。**既定 0.3。**0 で旧（1 ブロックで切り替え）。方向バスがあるときだけ効く。
    ///   部屋の尾を「戸口の線音源（口の幅から）」で鳴らすか「レーン（周りから）」で鳴らすかは、耳がその部屋の外か中かで替わる。
    ///   旧は戸口の線音源が 1 ブロックで 0 へ落ちてからレーンが入り、またいだ瞬間に 1 ブロックだけ尾が痩せた
    ///   （洞窟の口で −3.6 dB の凹み。AF_ONLY=cavewalk）。部屋ごとに「レーンで鳴らす割合」x を持ち、この時間で 0⇔1 へ動かす。
    ///   レーンは √x、戸口の線音源は √(1−x) を掛ける（パワー相補）。量は保ったまま、包まれ具合だけが移る
    ///   （発注者「エリアは音源に帰属する。中に入った時に変わるのは包まれ具合」）。
    ///   ★受け渡しの間、戸口の線音源は直前の点の重みと量を保つ（新しい目標は 0 なので、それを追うと 1 ブロックで消える）。
    ///   ★同じ枠を次の部屋が待っているときは、前の部屋を消し切ってから乗り換える（扉をくぐると 2 部屋が同時に入れ替わり、合計で 2 倍かかる）。
    void setPortalCrossfade(float sec) { portalXfadeSec_.store(std::max(0.0f, sec), std::memory_order_release); }
    float portalCrossfade() const { return portalXfadeSec_.load(std::memory_order_acquire); }

    /// その部屋の尾が来る向き（リスナー座標: +x 右 / +z 前）と広がり（0 = 点、1 = 一様）。方向バスがあるときだけ効く。
    ///   隣室なら戸口の向きと口の立体角、自室なら spread = 1（scene の fdnRoomWeights が出す）。
    void setListenerDirection(int room, const float* dirLocal3, float spread) {
        if (!valid(room)) return;
        Room& r = *rooms_[static_cast<std::size_t>(room)];
        const int L = bus_ ? std::min(bus_->lanes(), kMaxLanes) : 0;
        const int H = bus_ ? std::min(bus_->horizontalLanes(), kMaxLanes) : 0;   // 点を振るのは水平の環だけ
        float w[kMaxLanes] = {};
        diffuseLanes(r, w);
        if (L > 0) {
            const float sp = std::min(1.0f, std::max(0.0f, spread));
            float pt[kMaxLanes] = {};
            const float x = dirLocal3 ? dirLocal3[0] : 0.0f, z = dirLocal3 ? dirLocal3[2] : 1.0f;
            if (sp < 1.0f && H > 0 && (x * x + z * z) > 1e-8f) {
                // 隣り合う 2 レーンへ等パワーで振る（反射タップと同じ）。レーン k の方位 = 2πk/H、+x 右・+z 前。
                float az = std::atan2(x, z); if (az < 0.0f) az += 2.0f * 3.14159265f;
                const float u = az / (2.0f * 3.14159265f) * static_cast<float>(H);
                const int k0 = static_cast<int>(u) % H, k1 = (k0 + 1) % H;
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
        // laneModel 1 の重み（上の■点と拡散を分ける）。バスを差し直したときに作り直せるよう、置かれた向きも持つ。
        {
            const float sp = std::min(1.0f, std::max(0.0f, spread));
            r.dirX = dirLocal3 ? dirLocal3[0] : 0.0f;
            r.dirZ = dirLocal3 ? dirLocal3[2] : 1.0f;
            r.spread = sp;
            splitWeights(r.dirX, r.dirZ, sp, L, r.lanePend, r.difPend, r.ptPend, r.itdPend, r.hasShape ? r.shape : nullptr);
        }
        version_.fetch_add(1, std::memory_order_release);
    }

    /// 尾をレーンへ配るときの形（上の■尾の配り方の形）。energy[k] はレーン k のエネルギー（和で割って割合にする）。
    ///   nullptr か n <= 0 で形を外して一様に戻す。置いた向きと広がり（戸口越しの点）はそのまま混ぜ直す。制御スレッド。
    void setListenerLaneShape(int room, const float* energy, int n) {
        if (!valid(room)) return;
        Room& r = *rooms_[static_cast<std::size_t>(room)];
        const int L = bus_ ? std::min(bus_->lanes(), kMaxLanes) : 0;
        bool on = false;
        if (energy && n > 0 && L > 0) {
            const int m = std::min(n, L);
            double s = 0.0;
            for (int k = 0; k < m; ++k) s += std::max(0.0f, energy[k]);
            if (s > 1e-20) {
                for (int k = 0; k < kMaxLanes; ++k)
                    r.shape[k] = (k < m) ? static_cast<float>(std::sqrt(std::max(0.0f, energy[k]) / s)) : 0.0f;
                on = true;
            }
        }
        if (!on && !r.hasShape) return;             // 形を置いていない部屋に「外す」は何もしない（1 ビットも同じまま）
        r.hasShape = on;
        const float d[3] = { r.dirX, 0.0f, r.dirZ };
        setListenerDirection(room, d, r.spread);
    }

    // ── 戸口の線音源（段 2-g、2026-09-12）──
    //   隣の部屋の尾を、戸口の横幅に並べた kPortalPoints 個の点から HRTF で鳴らす。
    //   試聴の指摘「向こうの部屋の残響が全体から聞こえすぎ。ドア側に寄せたい」から。
    //   ★点ごとに FDN の**別の行**を入れる（行どうしは直交＝拡散状態で無相関）。同じ波形を全点に入れると
    //     打ち消し合って真ん中に像が 1 つできるだけで、幅にならない。
    //   ★srcRoom はリスナーへ直接は出さない（レーンにも L/R にも足さない）。聞こえるのは戸口からだけ。
    //   ★feedGain で dstRoom（耳の部屋）の FDN へ流す。戸口から入った音で耳の部屋が鳴り始める形。
    //     流す向きは src → dst の一方向だけで、dst が別の戸口の src になっている枠は流さない（信号の輪を作らない。
    //     部屋の FDN を信号で結び返すと α=1 で発散した: 上の■退けた書き方）。流した分は次のブロックで入る。
    //   ★乗り換え（src が変わる）は、今の量を 1 ブロックで 0 へ寄せ切ってから。HRTF の履歴は空にしてから使う。
    //   HRTF は方向バスが持っている物を借りる。見えた最初の 1 回だけ全点に差し、差し終えてから使う旗を立てる
    //   （オーディオスレッドは旗が立つまで HrtfProcessor に触らない＝競合しない）。無ければ左右の等パワーのパン。
    static constexpr int kMaxPortals = 4;
    static constexpr int kPortalPoints = 5;

    /// 戸口の線音源を置く（制御スレッド。毎フレーム置いてよい。目標を置くだけ）。
    ///   dirLocal … [点 × 3] リスナー座標の向き（+x 右 / +y 上 / +z 前）
    ///   pointGain … [点] 量（振幅。Σ² = 1 に揃えて渡す）
    ///   directGain … 戸口からリスナーへ出す量（振幅）／feedGain … dstRoom の FDN へ流す量（振幅）
    ///   srcRoom < 0 でその枠を空ける（量を 0 へ寄せてから外す）。
    void setPortal(int slot, int srcRoom, int dstRoom, const float* dirLocal, const float* pointGain, int nPoints,
                   float directGain, float feedGain, float headCircumferenceCm = 57.0f) {
        if (slot < 0 || slot >= kMaxPortals) return;
        ensurePortalHrtf();
        Portal& p = portals_[slot];
        const bool on = valid(srcRoom);
        const int np = std::min(std::max(nPoints, 0), kPortalPoints);
        p.srcPend = on ? srcRoom : -1;
        p.dstPend = (on && valid(dstRoom) && dstRoom != srcRoom) ? dstRoom : -1;
        p.directPend = on ? std::max(0.0f, directGain) : 0.0f;
        p.feedPend = on ? std::max(0.0f, feedGain) : 0.0f;
        const bool hrtf = portalHrtfReady_.load(std::memory_order_acquire);
        for (int k = 0; k < kPortalPoints; ++k) {
            const bool has = on && k < np;
            p.gPend[k] = (has && pointGain) ? std::max(0.0f, pointGain[k]) : 0.0f;
            if (!has || !dirLocal) continue;
            const float* d = dirLocal + k * 3;
            for (int c = 0; c < 3; ++c) p.dirPend[k * 3 + c] = d[c];
            // 向きがほとんど変わらないなら HRIR を置き直さない（毎フレーム置くとずっとクロスフェードで 2 倍重い）。
            const float dd = d[0] * p.dirSent[k * 3] + d[1] * p.dirSent[k * 3 + 1] + d[2] * p.dirSent[k * 3 + 2];
            if (hrtf && (!p.sentOnce[k] || dd < 0.99996f)) {       // 0.5 度より動いたら
                p.hp[k]->setDirection(d, headCircumferenceCm);
                for (int c = 0; c < 3; ++c) p.dirSent[k * 3 + c] = d[c];
                p.sentOnce[k] = true;
            }
        }
        version_.fetch_add(1, std::memory_order_release);
    }
    /// 戸口の線音源の低域の相関（Hz、2026-09-12）。この境より下は 5 点が**同じ波形**（振幅の和で 1）、上は点ごとに別の波形（パワーの和で 1）。
    ///   0 で全帯域を別の波形（旧）。
    ///   ★試聴「ドアから遠いと定位が出るが、近いと方向がわからない」。5 点全部を無相関にすると、近づいて 5 点が広い角度に散ったとき
    ///     両耳の相関が落ちて「前の雲」になる。向こうの部屋の拡散音場は戸口の幅の中で低域ほど相関が高い（波長 1 m の 340 Hz 以下は
    ///     ほぼ同じ波面、点の間隔 0.2 m の 850 Hz 以上は無相関）ので、低域を揃えるのが物理にも合う。
    ///   ★低域は振幅の和で 1 にする（同じ波形を N 点に配ると振幅で足されるため。パワーの和のままだと N 倍に膨らむ）。
    void setPortalCoherence(float hz) {
        portalCohHz_ = hz;
        portalCohCoef_ = (hz > 0.0f) ? std::min(1.0f, 2.0f * 3.14159265f * hz / static_cast<float>(fs_)) : 0.0f;
    }
    float portalCoherence() const { return portalCohHz_; }
    /// 診断: 枠が今運んでいる部屋（オーディオスレッドの状態。読むだけ）。
    int portalSource(int slot) const { return (slot >= 0 && slot < kMaxPortals) ? portals_[slot].src : -1; }
    /// 診断: 枠の量（directCur）と点の重みの Σg²、消えかけか。部屋をレーンで鳴らす割合 x（setPortalCrossfade）。読むだけ。
    float portalDirect(int slot) const { return (slot >= 0 && slot < kMaxPortals) ? portals_[slot].directCur : 0.0f; }
    float portalGainSq(int slot) const {
        if (slot < 0 || slot >= kMaxPortals) return 0.0f;
        float s2 = 0.0f; for (int k = 0; k < kPortalPoints; ++k) s2 += portals_[slot].gCur[k] * portals_[slot].gCur[k];
        return s2;
    }
    bool portalFading(int slot) const { return slot >= 0 && slot < kMaxPortals && portals_[slot].fadeOut; }
    float roomLaneShare(int room) const { return valid(room) ? rooms_[static_cast<std::size_t>(room)]->lanesMix : 0.0f; }
    /// 診断: 部屋の帯域の重み（wCur、平均の二乗が 1 になる形）の二乗平均。
    float roomWeightSq(int room) const {
        if (!valid(room)) return 0.0f;
        float s = 0.0f; for (int b = 0; b < kNumBands; ++b) s += rooms_[static_cast<std::size_t>(room)]->wCur[b] * rooms_[static_cast<std::size_t>(room)]->wCur[b];
        return s / kNumBands;
    }
    bool portalHrtfReady() const { return portalHrtfReady_.load(std::memory_order_acquire); }

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
                for (int l = 0; l < kMaxLanes; ++l) { r.laneTgt[l] = r.lanePend[l]; r.difTgt[l] = r.difPend[l]; r.ptTgt[l] = r.ptPend[l]; }
                r.itdTgt[0] = r.itdPend[0]; r.itdTgt[1] = r.itdPend[1];
            }
            for (int s = 0; s < kMaxPortals; ++s) {
                Portal& p = portals_[s];
                p.srcNext = p.srcPend; p.dstNext = p.dstPend;
                for (int k = 0; k < kPortalPoints; ++k) p.gTgt[k] = p.gPend[k];
                for (int c = 0; c < kPortalPoints * 3; ++c) p.dirTgt[c] = p.dirPend[c];
                p.directTgt = p.directPend; p.feedTgt = p.feedPend;
            }
            seen_ = v;
        }
        // 戸口の線音源: この枠が今どの部屋を運ぶか。変わるなら今の量を 0 へ寄せ切ってから乗り換える。
        int portalSrcMask = 0;
        for (int s = 0; s < kMaxPortals; ++s) {
            Portal& p = portals_[s];
            if (p.src != p.srcNext) {
                if (p.src >= 0) p.fadeOut = true;
                else { p.src = p.srcNext; p.dst = p.dstNext; p.flush = true; }
            } else {
                p.dst = p.dstNext;
            }
            if (p.src >= 0 && p.src < nr) portalSrcMask |= (1 << p.src);
        }
        int portalLiveMask = 0;                                  // 消えかけでない戸口が運んでいる部屋（受け渡しの目標）
        for (int s = 0; s < kMaxPortals; ++s)
            if (portals_[s].src >= 0 && portals_[s].src < nr && !portals_[s].fadeOut) portalLiveMask |= (1 << portals_[s].src);
        const float xfade = portalXfadeSec_.load(std::memory_order_acquire);
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
            // 行はレーン×耳（枠）で別々に取る。枠 s は行 1 + (s % 15)。行 0（M ＝ 全線の和）は使わない。
            //   8 レーン × 2 耳 = 16 に対して使える行が 15 本なので、最後の 1 つだけ先頭と重なる。
            //   重なる 2 つは別のレーン（＝別のレーン重みと別の ITD）なので実害は小さい。
            //   laneModel 0 は枠の数だけ行を並べて読む（今までと 1 ビットも同じ）。
            //   laneModel 1 は同じ行を 2 回読まないよう 15 本までにし、枠 s はバッファ s % 15 を読む（値は 0 と同じ）。
            //   点の波形は行 1（バッファ 0）。行 0 は量が部屋で動くので使わない（上の■点と拡散を分ける）。
            const int model = laneModel_.load(std::memory_order_acquire);
            int rows[kMaxLanes * 2];
            int nRowsRead = 0;
            if (model == 0) {
                for (int l = 0; l < L; ++l)
                    for (int ear = 0; ear < 2; ++ear)
                        rows[l * 2 + ear] = 1 + ((l * 2 + ear) % (FdnTail::kLines - 1));
                nRowsRead = L * 2;
            } else {
                nRowsRead = std::min(L * 2, FdnTail::kLines - 1);
                for (int u = 0; u < nRowsRead; ++u) rows[u] = 1 + u;
            }
            for (int k = 0; k < nr; ++k) {
                Room& r = *rooms_[static_cast<std::size_t>(k)];
                r.fdn->renderBandsRows(r.in.data(), n, rows, nRowsRead, r.prow);
                std::fill(r.in.begin(), r.in.begin() + n, 0.0f);
                float wStep[kNumBands];
                for (int b = 0; b < kNumBands; ++b) wStep[b] = (r.wTgt[b] - r.wCur[b]) * inv;
                if (portalSrcMask & (1 << k)) capturePortal(r, k, n, wStep, L * 2, true);   // 段 2-g: 戸口からだけ鳴らす
                // 受け渡し（setPortalCrossfade）: 戸口の線音源が生きて運んでいる部屋は x → 0、そうでなければ x → 1。
                const bool xf = xfade > 0.0f;
                if (xf) {
                    const float tgt = (portalLiveMask & (1 << k)) ? 0.0f : 1.0f;
                    const float step = static_cast<float>(n) / (xfade * static_cast<float>(fs_));
                    r.lanesMixPrev = r.lanesMix;
                    r.lanesMix = (tgt > r.lanesMix) ? std::min(tgt, r.lanesMix + step) : std::max(tgt, r.lanesMix - step);
                } else {
                    r.lanesMixPrev = r.lanesMix = (portalSrcMask & (1 << k)) ? 0.0f : 1.0f;
                }
                const float m0 = std::sqrt(r.lanesMixPrev), m1 = std::sqrt(r.lanesMix);
                // レーンの重みは 60 ms で追う（帯域の重み w は毎フレーム連続に来るのでブロック内で寄せ切る）。
                //   ★1 ブロックで寄せ切ると、正弦（1 つの周波数）では行ごとのモードの応答が違うぶん
                //     隣り合うブロックで 4.3 dB 跳んだ（実測。広帯域なら行のエネルギーは ±0.4 dB で揃う）。
                //     60 ms に伸ばすと 1 ブロックあたり 1 dB 未満。扉の量（w）は別に毎フレーム追うので遅れは向きだけ。
                const float laneFollow = std::min(1.0f, static_cast<float>(n) / (0.06f * static_cast<float>(fs_)));
                // 戸口の線音源だけで鳴らしている部屋はレーンへ出さない（受け渡しの間は両方）
                const int lanesHere = xf ? ((r.lanesMixPrev > 0.0f || r.lanesMix > 0.0f) ? L : 0) : ((portalSrcMask & (1 << k)) ? 0 : L);
                if (model == 0) {
                    // ── laneModel 0: 耳ごとの行（88d5f0b〜）。点も拡散も区別せず、レーンの重みで振る ──
                    for (int l = 0; l < lanesHere; ++l) {
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
                                const float y = acc * (l0 + dl * static_cast<float>(i + 1)) * laneNorm_ * (m0 + (m1 - m0) * static_cast<float>(i + 1) * inv);
                                sig[i] = y;
                                if (ear == 0) e += static_cast<double>(y) * y;   // 計器は片耳ぶん（今までと同じ意味）
                            }
                            float* row = laneRows_.data() + static_cast<std::size_t>(l * 2 + ear) * stride
                                       + (ear == 0 ? itdL_[l] : itdR_[l]);
                            for (int i = 0; i < n; ++i) row[i] += sig[i];
                        }
                    }
                    for (int ear = 0; ear < 2; ++ear) r.itdCur[ear] += (r.itdTgt[ear] - r.itdCur[ear]) * laneFollow;
                } else {
                    // ── laneModel 1: 点と拡散を分ける（上の■）──
                    renderSplit(r, n, inv, wStep, L, lanesHere, laneFollow, stride, e, m0, m1);
                }
                for (int b = 0; b < kNumBands; ++b) r.wCur[b] = r.wTgt[b];
                // 重みはどちらの作りでも毎ブロック追う（実行中に作りを切り替えても、重みの状態は揃っている）。
                for (int l = 0; l < kMaxLanes; ++l) {
                    r.laneCur[l] += (r.laneTgt[l] - r.laneCur[l]) * laneFollow;
                    r.difCur[l] += (r.difTgt[l] - r.difCur[l]) * laneFollow;
                    r.ptCur[l] += (r.ptTgt[l] - r.ptCur[l]) * laneFollow;
                }
            }
            renderPortals(n, inv, nr, outL, outR, portalSrcMask, e, xfade > 0.0f);     // 段 2-g（L/R へ足す。バスは通さない）
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
            for (int l = 0; l < kMaxLanes; ++l) { r.laneCur[l] = r.laneTgt[l]; r.difCur[l] = r.difTgt[l]; r.ptCur[l] = r.ptTgt[l]; }
            r.itdCur[0] = r.itdTgt[0]; r.itdCur[1] = r.itdTgt[1];
            if (portalSrcMask & (1 << k)) {                        // 段 2-g: 戸口からだけ鳴らす
                float wStep[kNumBands];
                for (int b = 0; b < kNumBands; ++b) wStep[b] = (r.wTgt[b] - r.wCur[b]) * inv;
                capturePortal(r, k, n, wStep, 2, false);
                for (int b = 0; b < kNumBands; ++b) r.wCur[b] = r.wTgt[b];
                continue;
            }
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
        renderPortals(n, inv, nr, outL, outR, portalSrcMask, e);     // 段 2-g
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
        // laneModel 1（点と拡散を分ける）: 拡散の重み・点の重み（レーンごと。Σdif² + Σpt² = 1）、点の ITD（耳ごと、サンプル。小数）。
        float difCur[kMaxLanes], difTgt[kMaxLanes], difPend[kMaxLanes];
        float ptCur[kMaxLanes], ptTgt[kMaxLanes], ptPend[kMaxLanes];
        float itdCur[2], itdTgt[2], itdPend[2];
        float dirX = 0.0f, dirZ = 1.0f, spread = 1.0f;   // 最後に置かれた向きと広がり（バスを差し直したら重みを作り直す）
        float lanesMix = 1.0f, lanesMixPrev = 1.0f;     // レーンで鳴らす割合 x（残りは戸口の線音源）。setPortalCrossfade
        float shape[kMaxLanes] = {};                     // 尾の配り方の形（振幅、Σ² = 1）。hasShape のときだけ一様の代わりに使う
        bool  hasShape = false;
        std::vector<float> ptHist;                       // [kPtHist] 点の波形の履歴（ptPos が最新）
        int ptPos = 0;
        std::vector<float> ptEar[2];                     // [maxFrames] 耳ごとに ITD ぶん遅らせた点の波形
    };

    bool valid(int room) const { return room >= 0 && room < count_.load(std::memory_order_acquire); }

    // レーンの重みを、今のバスのレーン数 L の中で Σ² = 1 に揃える（L から先は 0）。全部 0 なら一様にする。
    //   向きを置いた後なら向きは保つ（形はそのまま、量だけ揃える）。
    void renormLanes(float* w) const {
        const int L = bus_ ? std::min(bus_->lanes(), kMaxLanes) : kMaxLanes;
        double e = 0.0;
        for (int k = 0; k < kMaxLanes; ++k) {
            if (k >= L) w[k] = 0.0f;
            else e += static_cast<double>(w[k]) * w[k];
        }
        if (e <= 1e-12) { uniformLanes(w); return; }
        const float g = static_cast<float>(1.0 / std::sqrt(e));
        for (int k = 0; k < L; ++k) w[k] *= g;
    }

    // 拡散の配り方: 形が置いてあれば形、無ければ一様（上の■尾の配り方の形）。
    void diffuseLanes(const Room& r, float* w) const {
        if (!r.hasShape) { uniformLanes(w); return; }
        for (int k = 0; k < kMaxLanes; ++k) w[k] = r.shape[k];
    }

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

    // ── 点と拡散を分ける（laneModel 1、上の■）──
    static constexpr int kPtTaps = 8;        // 点の ITD を小数で読む窓（Lanczos a = 4）
    static constexpr int kPtLead = 4;        // 窓の半分。両耳に同じだけ遅れが乗る（48 kHz で 0.08 ms）
    static constexpr int kPtHist = 128;      // 点の波形の履歴。ITD の上限 64 ＋ 窓 8 より長い 2 の冪
    static constexpr int kPtFrac = 256;      // 小数の刻み（1/256 サンプル ＝ 48 kHz で 4 µs）
    std::atomic<int> laneModel_{1};
    std::atomic<float> portalXfadeSec_{0.3f};   // setPortalCrossfade
    float headRadius_ = 57.0f * 0.01f / (2.0f * 3.14159265f);   // 点の ITD に使う頭の半径（setDirectionBus で頭囲から）
    const float* lanczos_ = nullptr;         // [(kPtFrac + 1) × kPtTaps] 小数ごとの係数（行ごとに Σ = 1）

    // 8 点の Lanczos（a = 4）の係数表。小数 f = k / kPtFrac の行 k に、位置 −3..+4 の重みを並べる。
    //   ★行ごとに Σ = 1 に揃える（窓付き sinc の和は小数の位置で 1 から少しずれ、そのままだと遅れが動くたびに量が揺れる）。
    //   ★退けた書き方: 線形補間。遠い耳の高域が痩せて −2.5 dB（上の■(3)）。
    static const float* lanczosTable() {
        static const std::vector<float> table = [] {
            const double pi = 3.14159265358979323846;
            auto sinc = [pi](double x) { return (std::fabs(x) < 1e-12) ? 1.0 : std::sin(pi * x) / (pi * x); };
            std::vector<float> t(static_cast<std::size_t>(kPtFrac + 1) * kPtTaps, 0.0f);
            for (int k = 0; k <= kPtFrac; ++k) {
                const double fr = static_cast<double>(k) / kPtFrac;
                double w[kPtTaps], sum = 0.0;
                for (int j = 0; j < kPtTaps; ++j) {
                    const double x = static_cast<double>(j - (kPtTaps / 2 - 1)) - fr;   // j = 0..7 → 位置 −3..+4
                    w[j] = sinc(x) * sinc(x / (kPtTaps / 2));
                    sum += w[j];
                }
                for (int j = 0; j < kPtTaps; ++j) t[static_cast<std::size_t>(k) * kPtTaps + j] = static_cast<float>(w[j] / sum);
            }
            return t;
        }();
        return table.data();
    }

    // 点と拡散の重み（制御スレッド）。戻り値は「点があるか」。
    //   点がある: dif = 一様 × √ed、pt = 隣り合う 2 レーンへ等パワー × √(1−ed)、itd2 = 点の向きの ITD（耳ごと、サンプル）。
    //     ed = sp² / (sp² + (1 − sp)²) … 今までの「振幅 sp と (1 − sp) で一様と点を混ぜて正規化」と同じ量の配分
    //     （一様と点を直交とみなしたときのエネルギーの割合）。
    //   点が無い（向きが無い・広がり 1・バスが無い）: dif に laneW（laneModel 0 の重み）をそのまま写し、pt = 0。
    //     ★同じ数を使うので、広がり 1 は laneModel 0 と 1 ビットも同じになる（一様を作り直すと 1 ulp ずれうる）。
    bool splitWeights(float x, float z, float sp, int L, const float* laneW, float* dif, float* pt, float* itd2, const float* shape) const {
        for (int k = 0; k < kMaxLanes; ++k) pt[k] = 0.0f;
        itd2[0] = 0.0f; itd2[1] = 0.0f;
        if (L <= 0 || sp >= 1.0f || (x * x + z * z) <= 1e-8f) {
            for (int k = 0; k < kMaxLanes; ++k) dif[k] = laneW[k];
            return false;
        }
        if (shape) { for (int k = 0; k < kMaxLanes; ++k) dif[k] = shape[k]; }
        else uniformLanes(dif);
        const float ed = (sp * sp) / (sp * sp + (1.0f - sp) * (1.0f - sp));
        const float a = std::sqrt(ed), c = std::sqrt(1.0f - ed);
        for (int k = 0; k < L; ++k) dif[k] *= a;
        // 隣り合う 2 レーンへ等パワー（setListenerDirection と同じ式）。レーン k の方位 = 2πk/H（水平の環）、+x 右・+z 前。
        const int H = bus_ ? std::max(1, std::min(bus_->horizontalLanes(), kMaxLanes)) : L;
        float az = std::atan2(x, z); if (az < 0.0f) az += 2.0f * 3.14159265f;
        const float u = az / (2.0f * 3.14159265f) * static_cast<float>(H);
        const int k0 = static_cast<int>(u) % H, k1 = (k0 + 1) % H;
        const float t = u - static_cast<float>(static_cast<int>(u));
        pt[k0] += c * std::cos(t * 1.5707963f);
        pt[k1] += c * std::sin(t * 1.5707963f);
        // ITD は点の向きから Woodworth（(r/c)(θ + sinθ)、θ は正中面からの角）。レーンの整数ではなく小数のまま（上の■(2)）。
        const float sx = std::min(1.0f, std::fabs(x) / std::sqrt(x * x + z * z));
        const float th = std::asin(sx);
        const float itd = std::min(static_cast<float>(kItdMax), headRadius_ / 343.0f * (th + std::sin(th)) * static_cast<float>(fs_));
        itd2[0] = (x > 0.0f) ? itd : 0.0f;      // 右にあれば左耳が遅れる
        itd2[1] = (x < 0.0f) ? itd : 0.0f;
        return true;
    }

    // laneModel 1 の 1 部屋ぶん（オーディオスレッド）。
    //   ① 点の波形（行 1 × 帯域の重み）を履歴へ書く。鳴らさないブロック（戸口の線音源の部屋も）でも書く ── 鳴り始めに古い音を読まない。
    //   ② 耳ごとに、点の向きの ITD（60 ms で追い、ブロック内で線形）だけ遅らせて小数で読む。
    //   ③ 拡散は耳ごとの行（laneModel 0 と同じ行・同じ整数 ITD・同じ式）× 拡散の重み、点は ② × 点の重みをレーンへ足す。
    void renderSplit(Room& r, int n, float inv, const float* wStep, int L, int lanesHere, float follow, int stride, double& e,
                     float m0 = 1.0f, float m1 = 1.0f) {   // m0→m1: 受け渡しの量（√x、ブロックの中で直線）
        float* sig = r.laneSig.data();
        for (int i = 0; i < n; ++i) {
            float acc = 0.0f;
            for (int b = 0; b < kNumBands; ++b) acc += r.prow[b][i] * (r.wCur[b] + wStep[b] * static_cast<float>(i + 1));   // バッファ 0 ＝ 行 1
            sig[i] = acc * laneNorm_;
        }
        // 点が今聞こえていなければ、遅れは目標へ跳ばしてよい（聞こえる物が無いので継ぎ目が出ない）。
        double ptE = 0.0;
        bool ptOn = false;
        for (int l = 0; l < L; ++l) {
            ptE += static_cast<double>(r.ptCur[l]) * r.ptCur[l];
            if (r.ptCur[l] != 0.0f || r.ptTgt[l] != 0.0f) ptOn = true;
        }
        if (ptE < 1e-8) { r.itdCur[0] = r.itdTgt[0]; r.itdCur[1] = r.itdTgt[1]; }
        const float dNew[2] = { r.itdCur[0] + (r.itdTgt[0] - r.itdCur[0]) * follow,
                                r.itdCur[1] + (r.itdTgt[1] - r.itdCur[1]) * follow };
        const bool readPt = ptOn && lanesHere > 0;
        const int mask = kPtHist - 1;
        for (int i = 0; i < n; ++i) {
            r.ptPos = (r.ptPos + 1) & mask;
            r.ptHist[static_cast<std::size_t>(r.ptPos)] = sig[i];
            if (!readPt) continue;
            const float t = static_cast<float>(i + 1) * inv;
            for (int ear = 0; ear < 2; ++ear) {
                // 読む位置 = 最新 − (窓の半分 + ITD)。窓の右端（+4）が最新を越えない。
                const float p = static_cast<float>(r.ptPos) - (static_cast<float>(kPtLead) + r.itdCur[ear] + (dNew[ear] - r.itdCur[ear]) * t);
                const float fl = std::floor(p);
                const int i0 = static_cast<int>(fl);
                const int fk = static_cast<int>((p - fl) * static_cast<float>(kPtFrac) + 0.5f);
                const float* w = lanczos_ + static_cast<std::size_t>(fk) * kPtTaps;
                float y = 0.0f;
                for (int j = 0; j < kPtTaps; ++j) y += r.ptHist[static_cast<std::size_t>((i0 - (kPtTaps / 2 - 1) + j) & mask)] * w[j];
                r.ptEar[ear][static_cast<std::size_t>(i)] = y;
            }
        }
        r.itdCur[0] = dNew[0]; r.itdCur[1] = dNew[1];
        for (int l = 0; l < lanesHere; ++l) {
            const float d0 = r.difCur[l], dt = d0 + (r.difTgt[l] - d0) * follow, dd = (dt - d0) * inv;
            const float p0 = r.ptCur[l], pT = p0 + (r.ptTgt[l] - p0) * follow, dp = (pT - p0) * inv;
            const bool difOn = (d0 != 0.0f || dt != 0.0f);
            const bool ptHere = readPt && (p0 != 0.0f || pT != 0.0f);
            if (!difOn && !ptHere) continue;
            for (int ear = 0; ear < 2; ++ear) {
                const int slot = l * 2 + ear;
                float* row = laneRows_.data() + static_cast<std::size_t>(slot) * stride;
                if (difOn) {
                    // 拡散: laneModel 0 と同じ行（枠 % 15）・同じ整数 ITD・同じ式（広がり 1 で 1 ビットも同じになるように）。
                    const int buf = slot % (FdnTail::kLines - 1);
                    float* dst = row + (ear == 0 ? itdL_[l] : itdR_[l]);
                    for (int i = 0; i < n; ++i) {
                        float acc = 0.0f;
                        for (int b = 0; b < kNumBands; ++b) acc += r.prow[buf * kNumBands + b][i] * (r.wCur[b] + wStep[b] * static_cast<float>(i + 1));
                        const float y = acc * (d0 + dd * static_cast<float>(i + 1)) * laneNorm_ * (m0 + (m1 - m0) * static_cast<float>(i + 1) * inv);
                        dst[i] += y;
                        if (ear == 0) e += static_cast<double>(y) * y;   // 計器は片耳ぶん
                    }
                }
                if (ptHere) {
                    // 点: 1 本の波形を ITD ぶん遅らせた物を足す。レーンの整数 ITD は付けない（遅れは ② で付けてある）。
                    const float* src = r.ptEar[ear].data();
                    for (int i = 0; i < n; ++i) {
                        const float y = src[i] * (p0 + dp * static_cast<float>(i + 1)) * (m0 + (m1 - m0) * static_cast<float>(i + 1) * inv);
                        row[i] += y;
                        if (ear == 0) e += static_cast<double>(y) * y;
                    }
                }
            }
        }
    }

    // ── 戸口の線音源（段 2-g）──
    struct Portal {
        int srcPend = -1, dstPend = -1;                 // 制御スレッドが書く
        int srcNext = -1, dstNext = -1;                 // 版で取り込んだ目標
        int src = -1, dst = -1;                         // オーディオスレッドが今運んでいる部屋
        bool fadeOut = false, flush = false;
        float gPend[kPortalPoints] = {}, gTgt[kPortalPoints] = {}, gCur[kPortalPoints] = {};
        float dirPend[kPortalPoints * 3] = {}, dirTgt[kPortalPoints * 3] = {};
        float dirSent[kPortalPoints * 3] = {};
        bool  sentOnce[kPortalPoints] = {};
        float directPend = 0.0f, directTgt = 0.0f, directCur = 0.0f;
        float feedPend = 0.0f, feedTgt = 0.0f, feedCur = 0.0f;
        float lpShared = 0.0f, lpOwn[kPortalPoints] = {};   // 低域の相関（setPortalCoherence）の一次 LP の状態
        std::unique_ptr<HrtfProcessor> hp[kPortalPoints];
        std::vector<float> sig;                         // [点][maxFrames] 点ごとのモノラル（行 × 帯域の重み）
        std::vector<float> feed;                        // 耳の部屋へ流す分（真ん中の点の、低域の相関の処理より前の信号）
        std::vector<float> tmp, scratchL, scratchR;
    };
    Portal portals_[kMaxPortals];
    std::atomic<bool> portalHrtfReady_{false};
    float portalNorm_ = 1.0f;
    float portalCohHz_ = 0.0f, portalCohCoef_ = 0.0f;
    static constexpr float kSqrt2 = 1.41421356f;

    // 方向バスの HRTF を最初に見えたときに 1 回だけ全点へ差す（制御スレッド）。差し終えてから旗を立てる。
    void ensurePortalHrtf() {
        if (portalHrtfReady_.load(std::memory_order_acquire)) return;
        const HrtfSet* hs = bus_ ? bus_->hrtfSet() : nullptr;
        if (!hs || !hs->isValid()) return;
        for (int s = 0; s < kMaxPortals; ++s)
            for (int k = 0; k < kPortalPoints; ++k) portals_[s].hp[k]->setHrtfSet(hs);
        // 量はレーンと同じ規約（HRIR の平均パワーで等パワーのパンに揃える）。
        portalNorm_ = 1.0f / std::sqrt(std::max(1e-6f, hs->meanPowerGain()));
        portalHrtfReady_.store(true, std::memory_order_release);
    }

    // 戸口の線音源の元になる部屋の行を、点ごとのモノラルに取る（オーディオスレッド）。
    //   行 1 本は片耳ぶん（エネルギー 0.5）なので √2 倍してモノラルのエネルギー 1 にする。
    void capturePortal(Room& r, int roomIdx, int n, const float* wStep, int nRowsAvail, bool lanesPath) {
        for (int s = 0; s < kMaxPortals; ++s) {
            Portal& p = portals_[s];
            if (p.src != roomIdx) continue;
            for (int j = 0; j < kPortalPoints; ++j) {
                float* sig = p.sig.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(maxFrames_);
                if (lanesPath) {
                    const int src = (nRowsAvail > 0) ? (j * 3) % nRowsAvail : 0;   // 行 1, 4, 7, 10, 13
                    for (int i = 0; i < n; ++i) {
                        float acc = 0.0f;
                        for (int b = 0; b < kNumBands; ++b)
                            acc += r.prow[src * kNumBands + b][i] * (r.wCur[b] + wStep[b] * static_cast<float>(i + 1));
                        sig[i] = acc * kSqrt2;
                    }
                } else {
                    const bool left = (j % 2) == 0;                         // バスが無いときは L/R の 2 本を交互に
                    for (int i = 0; i < n; ++i) {
                        float acc = 0.0f;
                        for (int b = 0; b < kNumBands; ++b)
                            acc += (left ? r.l[b][static_cast<std::size_t>(i)] : r.rr[b][static_cast<std::size_t>(i)])
                                 * (r.wCur[b] + wStep[b] * static_cast<float>(i + 1));
                        sig[i] = acc * kSqrt2;
                    }
                }
            }
            // 耳の部屋へ流す分は、下の相関の処理より前の真ん中の信号を取っておく。
            //   ★処理後の信号を流すと、共有する低域が 1/Σg（5 点で −7 dB）に縮んだまま部屋へ入り、後期が −2.2 dB 足りなくなった（実測）。
            {
                const float* center = p.sig.data() + static_cast<std::size_t>(kPortalPoints / 2) * static_cast<std::size_t>(maxFrames_);
                std::copy(center, center + n, p.feed.data());
            }
            // 低域の相関（setPortalCoherence）: 境より下は真ん中の点の波形を全点で共有（振幅の和で 1）、上は点ごとの波形のまま。
            //   点 k = LP(真ん中) × (1/Σg) + (点 k − LP(点 k))。真ん中の点は LP + HP ＝ 元のまま。
            if (portalCohCoef_ > 0.0f) {
                const float coef = portalCohCoef_;
                // ★割るのは実際に鳴らしている重みの和。消えかけ（fadeOut）の間は目標が 0 になっているので、今の重み（gCur）で割る。
                //   目標で割っていた頃は、口をまたいだ瞬間に和が 0 → 割らない（×1）になり、低域だけ 1/Σg（5 点で約 2.2 倍、+6.9 dB）
                //   跳ねて出口が +4.7 dB 上がった（2026-10-04、AF_ONLY=cavewalk AF_CW_DIAG）。
                const float* gN = p.fadeOut ? p.gCur : p.gTgt;
                float sumG = 0.0f;
                for (int j = 0; j < kPortalPoints; ++j) sumG += gN[j];
                const float f = (sumG > 1e-6f) ? 1.0f / sumG : 1.0f;
                const float* center = p.sig.data() + static_cast<std::size_t>(kPortalPoints / 2) * static_cast<std::size_t>(maxFrames_);
                float lpS = p.lpShared;
                for (int i = 0; i < n; ++i) {
                    lpS += coef * (center[i] - lpS);
                    for (int j = 0; j < kPortalPoints; ++j) {
                        float* sig = p.sig.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(maxFrames_);
                        const float x = sig[i];
                        p.lpOwn[j] += coef * (x - p.lpOwn[j]);
                        sig[i] = lpS * f + (x - p.lpOwn[j]);
                    }
                }
                p.lpShared = lpS;
            }
        }
    }

    // 戸口の線音源を鳴らし、耳の部屋へ流す（オーディオスレッド）。量は全部ブロックの中で線形に寄せる。
    void renderPortals(int n, float inv, int nr, float* outL, float* outR, int srcMask, double& e, bool xf = false) {
        const bool useHrtf = portalHrtfReady_.load(std::memory_order_acquire);
        for (int s = 0; s < kMaxPortals; ++s) {
            Portal& p = portals_[s];
            if (p.src < 0 || p.src >= nr) continue;
            float* tmp = p.tmp.data();
            if (p.flush && useHrtf) {                    // 乗り換え直後: 前に運んでいた音の履歴を空にする
                std::fill(tmp, tmp + n, 0.0f);
                for (int j = 0; j < kPortalPoints; ++j)
                    p.hp[j]->processAdd(tmp, 0, n, p.scratchL.data(), p.scratchR.data(), 0, 0.0f);
            }
            if (p.flush) { p.lpShared = 0.0f; for (int j = 0; j < kPortalPoints; ++j) p.lpOwn[j] = 0.0f; }
            p.flush = false;
            const bool live = !p.fadeOut;
            // 受け渡し（xf）: 量は √(1−x)（x はその部屋をレーンで鳴らす割合）。消えかけの間は直前の重みと量を保ち、x が 1 になったら消し切る。
            const Room& src = *rooms_[static_cast<std::size_t>(p.src)];
            const float c0 = xf ? std::sqrt(std::max(0.0f, 1.0f - src.lanesMixPrev)) : 1.0f;
            const float c1 = xf ? std::sqrt(std::max(0.0f, 1.0f - src.lanesMix)) : 1.0f;
            const bool hold = xf && p.fadeOut;
            const float dT = hold ? p.directCur : (live ? p.directTgt : 0.0f), fT = hold ? p.feedCur : (live ? p.feedTgt : 0.0f);
            const float d0 = p.directCur, dd = (dT - d0) * inv;
            const float f0 = p.feedCur, df = (fT - f0) * inv;
            for (int j = 0; j < kPortalPoints; ++j) {
                const float gT = hold ? p.gCur[j] : (live ? p.gTgt[j] : 0.0f);
                const float g0 = p.gCur[j], dg = (gT - g0) * inv;
                const float* sig = p.sig.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(maxFrames_);
                for (int i = 0; i < n; ++i) {
                    const float t = static_cast<float>(i + 1);
                    tmp[i] = sig[i] * (g0 + dg * t) * (d0 + dd * t) * (c0 + (c1 - c0) * t * inv);
                }
                if (useHrtf && outL && outR) {
                    p.hp[j]->processAdd(tmp, 0, n, outL, outR, 0, portalNorm_);
                } else {
                    const float x = std::min(1.0f, std::max(-1.0f, p.dirTgt[j * 3]));
                    const float pl = std::sqrt(0.5f * (1.0f - x)), pr = std::sqrt(0.5f * (1.0f + x));
                    for (int i = 0; i < n; ++i) {
                        if (outL) outL[i] += tmp[i] * pl;
                        if (outR) outR[i] += tmp[i] * pr;
                    }
                }
                for (int i = 0; i < n; ++i) e += 0.5 * static_cast<double>(tmp[i]) * tmp[i];   // 計器（片耳ぶんの目安）
                p.gCur[j] = gT;
            }
            // 耳の部屋へ流す（真ん中の点の行）。dst が別の戸口の元になっていたら流さない（輪を作らない）。
            const bool feedOk = p.dst >= 0 && p.dst < nr && (srcMask & (1 << p.dst)) == 0;
            if (feedOk && (f0 > 0.0f || fT > 0.0f)) {
                const float* sig = p.feed.data();               // 相関の処理より前の真ん中の信号
                Room& d = *rooms_[static_cast<std::size_t>(p.dst)];
                for (int i = 0; i < n; ++i) d.in[static_cast<std::size_t>(i)] += sig[i] * (f0 + df * static_cast<float>(i + 1)) * (c0 + (c1 - c0) * static_cast<float>(i + 1) * inv);
            }
            p.directCur = dT; p.feedCur = fT;
            if (p.fadeOut && (!xf || c1 <= 0.0f)) {      // 0 へ寄せ切った（受け渡しなら x が 1 になった）。次のブロックから新しい部屋を運ぶ
                p.src = p.srcNext; p.dst = p.dstNext; p.fadeOut = false; p.flush = true;
                for (int j = 0; j < kPortalPoints; ++j) p.gCur[j] = 0.0f;
                p.directCur = 0.0f; p.feedCur = 0.0f;
            }
        }
    }
};

}  // namespace dsp
}  // namespace af
