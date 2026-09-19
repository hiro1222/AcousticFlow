// voice_renderer.h — 1 音源ぶんの信号フローを束ねる。
//
//   dry(モノラル)
//     ├─ 早期反射マルチタップ ──┬→ 鏡面 L/R ──────────────┐
//     │                          ├→ 直接音(モノ) → HRTF ──┤
//     │                          └→ 拡散送り → allpass ───┤→ ×outputGain → L/R
//     └─ 後期尾(非一様分割畳み込み) ─────────────────────┘
//
// 各部品は個別にテスト済み（dsp_regression.cpp）。ここが担うのは**配線と段別の計測**だけ。
//
// 拡散器を早期反射と分けている理由:
//   拡散器は固定ネットワークなので、タップ集合が切り替わっても状態は連続でよい
//   （送り込む信号側は EarlyReflectConv がクロスフェード済み）。
//   L/R で互いに素っぽい別の長さにして、拡散成分を左右で相関させない。
//
// スレッド規約:
//   setTaps / setTailIr / setDirection … 制御スレッド
//   render()                            … オーディオスレッド（確保・ロックなし）
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/IrConvolver.cs の OnAudioFilterRead。
// FDN 尾（tailMode=Fdn）は比較用の旧方式なので移していない。本命は実測エコグラム由来の尾。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "early_reflect_conv.h"
#include "hrtf_processor.h"
#include "nonuniform_convolver.h"
#include "reverb_tail_ir.h"
#include "tail_bus.h"
#include "direction_bus.h"
#include "fdn_room_mix.h"   // 尾の FDN（tailModel=1、docs/TAIL_FDN_PLAN.md 手順 4）
#include <atomic>

namespace af {
namespace dsp {

// Schroeder allpass の直列。位相だけ撹拌して振幅特性は平坦（＝音色を変えずに滲ませる）。
class AllpassChain {
public:
    void init(const float* delaysMs, int count, int sampleRate) {
        len_.resize(static_cast<std::size_t>(count));
        pos_.assign(static_cast<std::size_t>(count), 0);
        offset_.resize(static_cast<std::size_t>(count));
        int total = 0;
        for (int i = 0; i < count; ++i) {
            len_[static_cast<std::size_t>(i)] =
                std::max(1, static_cast<int>(std::lround(delaysMs[i] * 0.001f * sampleRate)));
            offset_[static_cast<std::size_t>(i)] = total;
            total += len_[static_cast<std::size_t>(i)];
        }
        buf_.assign(static_cast<std::size_t>(total), 0.0f);
    }

    float process(float x, float g) {
        const int n = static_cast<int>(len_.size());
        for (int i = 0; i < n; ++i) {
            float* b = buf_.data() + offset_[static_cast<std::size_t>(i)];
            int& p = pos_[static_cast<std::size_t>(i)];
            const float bo = b[p];
            const float di = x + g * bo;
            b[p] = di;
            x = bo - g * di;
            if (++p >= len_[static_cast<std::size_t>(i)]) p = 0;
        }
        return x;
    }

private:
    std::vector<float> buf_;
    std::vector<int> len_, pos_, offset_;
};

class VoiceRenderer {
public:
    // 段別の計測（このブロックの RMS）。内訳が読めないと調整できないので分けて出す。
    struct Metering {
        float rmsDirect = 0.0f;    // 直接音（HRTF 後）
        float rmsEarly = 0.0f;     // 早期反射の鏡面成分（直接音を除く）
        float rmsScatter = 0.0f;   // 散乱スメア
        float rmsTail = 0.0f;      // 後期尾
        float rmsOut = 0.0f;       // 最終出力
    };

    struct Config {
        int sampleRate = 48000;
        int maxFrames = 4096;        // 1回の render で処理する最大サンプル数
        float tailSeconds = 1.0f;    // 後期尾の長さ
        int earlyMaxDelaySamples = 0;// 0 なら 120ms 相当を自動で確保
        float tapCrossfadeMs = 30.0f;
        float hrtfCrossfadeMs = 12.0f;
        float hrtfCrossoverHz = 700.0f;
        int tailFirstBlock = 64;     // 非一様分割の最小ブロック（＝尾の遅延）
        int tailCapBlock = 8192;
        // 尾 IR を差し替えるときのクロスフェード(ms)。0 で即差し替え（従来）。
        //
        // ★2026-09-08 に直した。それまでは既定 0＝無効で、有効にすると**悪化した**。
        //   有効(200ms 並走)  ピーク -3.26 dB ← 凹む   無効(即差し替え) ピーク +1.97 dB
        //   原因は「器を 2 つ並べて交互に使う」形にしたこと。休んでいる側の
        //   周波数領域遅延線(FDL)が止まったままなので、役割を入れ替えた瞬間に
        //   過去の入力を持たない（あるいは古い）状態で鳴り出す。
        //
        //   いまは器を 1 つに戻し、**FDL は共有のまま IR スペクトルだけ 2 世代混ぜる**
        //   （partitioned_convolver.h）。FDL が持っているのは変換済みの入力で IR とは
        //   無関係なので、新旧どちらも過去の入力を全部持ったまま鳴る＝凹まないし膨らまない。
        //   費用はフェード中だけ掛け算と逆 FFT が 2 倍（入力 FFT は 1 回のまま）。
        //   メモリは器が 1 つになったぶん半分。
        float tailCrossfadeMs = 50.0f;
    };

    explicit VoiceRenderer(const Config& cfg)
        : cfg_(cfg),
          sampleRate_(std::max(8000, cfg.sampleRate)),
          tailSamples_(std::max(1, static_cast<int>(cfg.tailSeconds * sampleRate_))),
          early_(sampleRate_,
                 cfg.earlyMaxDelaySamples > 0 ? cfg.earlyMaxDelaySamples
                                              : static_cast<int>(0.120f * sampleRate_),
                 cfg.tapCrossfadeMs),
          hrtf_(sampleRate_, cfg.hrtfCrossfadeMs, cfg.hrtfCrossoverHz),
          hrtfDif_(sampleRate_, cfg.hrtfCrossfadeMs, cfg.hrtfCrossoverHz),
          tailConv_(tailSamples_, 2, cfg.tailFirstBlock, cfg.tailCapBlock, cfg.maxFrames),
          tailIr_(sampleRate_, tailSamples_, 2) {
        tailConv_.setCrossfadeSamples(
            std::max(0, static_cast<int>(cfg.tailCrossfadeMs * 0.001f * sampleRate_)));
        const float scatL[3] = {3.7f, 6.1f, 9.7f};
        const float scatR[3] = {4.3f, 7.3f, 11.3f};
        diffL_.init(scatL, 3, sampleRate_);
        diffR_.init(scatR, 3, sampleRate_);

        const std::size_t mf = static_cast<std::size_t>(std::max(64, cfg.maxFrames));
        tailIn_.assign(mf, 0.0f);
        tailOutL_.assign(mf, 0.0f);
        tailOutR_.assign(mf, 0.0f);
        maxFrames_ = static_cast<int>(mf);
        // 尾の FDN の開始（手順 5）: 送りの前の遅延線。上限 240 ms ＋ 1 チャンク。
        fdnDelayLen_ = static_cast<int>(0.25f * static_cast<float>(sampleRate_)) + maxFrames_ + 2;
        fdnDelay_.assign(static_cast<std::size_t>(fdnDelayLen_), 0.0f);
        fdnSendBuf_.assign(mf, 0.0f);
    }

    VoiceRenderer(const VoiceRenderer&) = delete;
    VoiceRenderer& operator=(const VoiceRenderer&) = delete;

    // ── 制御スレッド ──

    /// タップを差し替える。方向を持つタップには**軽量な両耳化**（ITD ＋ 帯域別 ILD）を
    /// ここで焼き込む。HRTF セットを持っているのがこの層なので、変換はここで行う。
    ///   ★反射を 1 本ずつ HRIR で畳み込むと重い（HRTF 1 本 0.05〜0.12ms/block）。
    ///     タップは元から「小数遅延＋6帯域ゲイン」なので、耳ごとに 2 組持てば
    ///     畳み込みを増やさずに ITD と ILD が載る。
    void setTaps(const EarlyReflectConv::Tap* taps, int count) {
        if (!taps || count <= 0) { early_.setTaps(taps, count); return; }
        tapScratch_.assign(taps, taps + count);
        const bool cues = earCues_ && hrtfSet_ && hrtfSet_->hasEarBands();
        const int lanes = dirBus_ ? dirBus_->horizontalLanes() : 0;   // タップは水平の環へ振る（上下のレーンは尾だけ）
        const int busLatency = dirBus_ ? dirBus_->latency() : 0;
        for (int i = 1; i < count; ++i) {           // index 0 は直接音＝フル HRTF が担当
            EarlyReflectConv::Tap& d = tapScratch_[static_cast<std::size_t>(i)];
            if (!d.dirValid || d.hrtfWeight > 0.0f) continue;   // 方向なし／HRTF バス行き
            if (lanes > 0) {
                // 方向バス行き: 隣り合う 2 レーンへ等パワー。バスの固有遅延（firstBlock）ぶんタップを早める。
                DirectionBus::laneWeights(d.dir, lanes, d.lane, d.laneW);
                d.laneUse = true;
                if (d.width > 1e-4f) {
                    // 幅（虚像の面音源）: 方位角 ±width の弧を刻み、各点の 2 レーンの量（重み²）を平均して幅の中のレーンの重みにする。
                    //   幅の割合 amt = 幅 / (レーンの間隔の半分) を幅の分へ、残りを点の分（隣り合う 2 レーン）へ。量の和は変えない。
                    //   幅 → 0 で点のパンに連続につながる（弧の平均が点に縮み、amt も 0 へ）。
                    const float spacing = 6.28318531f / static_cast<float>(lanes);
                    const float w = std::min(d.width, 3.14159265f);
                    const float amt = std::min(1.0f, w / (0.5f * spacing));
                    const int half = std::max(1, static_cast<int>(std::ceil(w / (0.25f * spacing))));
                    const int M = 1 + 2 * half;
                    float E[EarlyReflectConv::kWideLanes] = {};
                    const float az0 = std::atan2(d.dir[0], d.dir[2]);
                    for (int m = 0; m < M; ++m) {
                        const float az = az0 + w * (2.0f * static_cast<float>(m) / static_cast<float>(M - 1) - 1.0f);
                        const float dd[3] = {std::sin(az), 0.0f, std::cos(az)};
                        int ln[2]; float lw[2];
                        DirectionBus::laneWeights(dd, lanes, ln, lw);
                        for (int q = 0; q < 2; ++q)
                            if (ln[q] >= 0 && ln[q] < EarlyReflectConv::kWideLanes) E[ln[q]] += lw[q] * lw[q] / static_cast<float>(M);
                    }
                    for (int l = 0; l < EarlyReflectConv::kWideLanes; ++l) d.wideW[l] = std::sqrt(E[l] * amt);
                    const float keep = std::sqrt(1.0f - amt);
                    d.laneW[0] *= keep; d.laneW[1] *= keep;
                }
                d.delaySamples = std::max(0, d.delaySamples - busLatency);
                // ITD はタップ自身の物を持つ（方向 × 耳の行へ別々に送る。ILD とスペクトルは行の HRIR が担当）。
                if (hrtfSet_ && hrtfSet_->isValid()) {
                    const int idx = hrtfSet_->nearestIndex(d.dir);
                    if (idx >= 0) {
                        // ITD は近い 4 方向から補間（格子をまたぐたびに跳ばないように。HrtfSet::itdSecondsSmooth の注記）
                        const float samp = hrtfSet_->itdSecondsSmooth(d.dir, headCm_) * static_cast<float>(sampleRate_);
                        d.earDelay[0] = (samp < 0.0f) ? -samp : 0.0f;   // 右が先 → 左耳が遅れる
                        d.earDelay[1] = (samp > 0.0f) ? samp : 0.0f;    // 左が先 → 右耳が遅れる
                    }
                }
                continue;
            }
            if (!cues) continue;
            const int idx = hrtfSet_->nearestIndex(d.dir);
            if (idx < 0) continue;
            const float* gl = hrtfSet_->earBandGains(idx, 0);
            const float* gr = hrtfSet_->earBandGains(idx, 1);
            if (!gl || !gr) continue;
            const float itd = hrtfSet_->itdSecondsScaled(idx, headCm_);
            const float samp = itd * static_cast<float>(sampleRate_);
            d.earDelay[0] = (samp < 0.0f) ? -samp : 0.0f;   // 右が先 → 左耳が遅れる
            d.earDelay[1] = (samp > 0.0f) ? samp : 0.0f;    // 左が先 → 右耳が遅れる
            for (int b = 0; b < EarlyReflectConv::kNumBands; ++b) {
                d.earGain[0][b] = gl[b];
                d.earGain[1][b] = gr[b];
            }
            d.earUse = true;
        }
        early_.setTaps(tapScratch_.data(), count);
    }

    /// 反射タップの軽量な両耳化。既定 ON。切ると従来の等パワーパンに戻る（A/B 用）。
    int liveTapCount() const { return early_.liveTapCount(); }   // 診断用
    void setEarCuesEnabled(bool on) { earCues_ = on; }
    bool earCuesEnabled() const { return earCues_; }

    /// HRTF データセットを差し替える。set は呼び手が生存を保証する。
    void setHrtfSet(const HrtfSet* set) {
        hrtfSet_ = set;
        hrtf_.setHrtfSet(set);
        hrtfDif_.setHrtfSet(set);
        // 回折バスだけ、パンと同じ音量になるように平均ゲインを揃える。
        //   HRIR の絶対ゲインは測定系の都合で決まっていて、kemar は等パワーパンより
        //   **+3.2dB 大きい**（実測: 平均パワーゲイン 2.11）。補正しないと
        //   「回折タップを HRTF に載せた」だけで回折が 3dB 持ち上がり、
        //   D⊕F の混ぜ合わせ（occFrac の連続クロスフェード）が壊れる。
        //   補正後の残りは方向ごとのばらつき（実測: 右90°で +1.75dB）＝頭部の影そのもの。
        //   ★B1 は**同じエネルギーの空間化を変えるだけ**という約束なので、ここは揃える。
        //   ★直接音側(hrtf_)には掛けない。あちらは元からこのゲインで鳴っていて、
        //     尾の較正も残響比もその音量を前提に決まっている。触ると出荷音が動く。
        difNorm_ = (set && set->isValid())
                 ? 1.0f / std::sqrt(std::max(1e-6f, set->meanPowerGain())) : 1.0f;
    }
    void setHrtfEnabled(bool on) { hrtfEnabled_ = on; }
    void setDirection(const float dirListenerLocal[3], float headCircumferenceCm) {
        headCm_ = headCircumferenceCm;      // 反射タップの ITD もこの頭囲で作る
        hrtf_.setDirection(dirListenerLocal, headCircumferenceCm);
    }

    /// 回折バス（hrtfWeight>0 のタップ）の到来方向。直接音とは**別の方向**を持つ。
    ///   遮蔽されているとき、直接音は壁の向こうの音源方向を指したまま材質の透過まで落ち、
    ///   実際に耳へ届くのは開口を回り込んだ成分になる。その成分の方向がこれ。
    ///   ★別インスタンスなのは、1 本の HrtfProcessor が持てる方向が 1 つだからで、
    ///     直接音の方向を回折の方向で上書きすると、開けた場所での定位が壊れる。
    void setDiffractionDirection(const float dirListenerLocal[3], float headCircumferenceCm) {
        hrtfDif_.setDirection(dirListenerLocal, headCircumferenceCm);
    }

    /// 実測エコグラムから後期尾を組み直す。戻り値は 尾/直接 のエネルギー比。
    ///   directGain は直接音タップの広帯域ゲイン（距離減衰・透過込み＝絶対レベルの基準）。
    ///   targetRatio は残響/直接エネルギーの目標比（呼び手が物理式から算出）。
    float rebuildTail(const float* echoBands, int binCount, float binMs, float startMs,
                      float fadeMs, float smoothMs, float smoothGrowth, float envAlpha,
                      float directGain, float targetRatio,
                      const float* earBandGain = nullptr, int earBandGainLen = 0) {
        // ★★ 共有バスに預けていて自分が代表でないなら、**IR を組まない** ★★
        //   同じ部屋の音源は同じエコグラムを渡してくるので、出来る IR は代表とまったく同じ。
        //   組んで捨てるだけの仕事だった。実測: 代表でない 1 本 0.521 ms、
        //   8 本なら 1 回の組み直しで **3.645 ms が無駄**。しかも組み直しは
        //   数フレームに 1 回まとめて走るので、そのフレームだけの山になる。
        //   ★必要なのは tailGain_（＝音源ごとの尾の量）だけで、これは IR に依存しない。
        //   ⚠ 戻り値（尾のエネルギー比）はホストが捨てているので 1.0 を返してよい。
        //     使うようになったら、ここが嘘をつくことになるので注意。
        if (tailBus_ && !tailBusOwner_) {
            tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
            return 1.0f;
        }
        // ★尾の FDN に預けているなら IR を組まない（形は部屋の FDN が持つ。ここは量だけ）。
        if (fdnMix_) {
            tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
            return 1.0f;
        }
        const float ratio = tailIr_.build(echoBands, binCount, binMs, startMs, fadeMs,
                                          smoothMs, smoothGrowth, envAlpha,
                                          earBandGain, earBandGainLen);
        if (ratio <= 0.0f) { tailGain_ = 0.0f; return 0.0f; }
        const float* ir[2] = { tailIr_.ir(0), tailIr_.ir(1) };
        const int len[2] = { tailIr_.length(), tailIr_.length() };

        // 【クロスフェード】新しい IR は同じ器へ入れる。器の中で、共有の遅延線に対して
        //   新旧 2 世代の IR スペクトルを等振幅で混ぜる（partitioned_convolver.h）。
        //   ★即差し替えにすると、周波数領域遅延線に溜まっている**過去の入力**が
        //     新しい IR で畳み直される。尾IRはエネルギー1に正規化されているので
        //     本来レベルは動かないはず（実測: 響く部屋と吸う部屋の定常RMSは差 -0.08dB）
        //     なのに、実測で **+1.97dB のふくらみが 0.35 秒続いた**。まるごと人工物。
        //   ★器を 2 つ並べて並走させる形も試した（2026-09-08 まで）。休んでいる器の
        //     遅延線が止まったままなので、入れ替えた瞬間に痩せて **-3.26dB 凹んだ**。
        //     世代を混ぜる形はこれが原理的に起きない。
        // ★共有バスを使っているときは、IR はバスが持つ。
        //   代表の音源だけが入れる。全員が入れると、同じ IR で何度もクロスフェードが始まる。
        //   ⚠ tailGain_ は**音源ごとに**計算し続ける。これが音源ごとの尾の量になる。
        if (tailBus_) {   // ここへ来るのは代表だけ（代表でない側は上で帰っている）
            tailBus_->setIr(ir, len);
            tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
            return ratio;
        }
        tailConv_.setIr(ir, len);
        // 尾IRはエネルギー1に正規化済み。絶対レベルは D/R 比から一意に決まる。
        tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
        return ratio;
    }

    /// 尾の畳み込みを共有バスへ預ける。null で自前に戻る（既定）。
    ///   isOwner=true の音源だけが IR をバスへ入れる（部屋の代表）。
    ///   ⚠ **同じ IR を使う音源だけ**を同じバスへ入れること。違う部屋を混ぜると、
    ///     片方の部屋の響きがもう片方に付く。
    ///   ⚠ owner を 1 本も差さないとバスに IR が入らず、尾が丸ごと鳴らない。
    void setTailBus(TailBus* bus, bool isOwner) {
        tailBus_ = bus;
        tailBusOwner_ = isOwner;
    }
    const TailBus* tailBus() const { return tailBus_; }

    // ── 尾の FDN（tailModel=1、docs/TAIL_FDN_PLAN.md 手順 4）──
    /// 部屋ごとの FDN へ預ける。null で畳み込みに戻る（既定）。**制御スレッド**。
    ///   差すと、この音源は IR を組まず（rebuildTail は量だけ）、送るだけになる。畳み込みの器はそのまま眠る。
    void setFdnMix(FdnRoomMix* mix) { fdnMix_ = mix; }
    const FdnRoomMix* fdnMix() const { return fdnMix_; }
    /// 送り先の部屋と重み（振幅、最大 kMaxFdnSends 本）。ホストが毎フレーム置く（scene の fdnRoomWeights）。
    ///   重みは render がチャンク内で線形に繋ぐ。並びが変わっても同じ部屋への送りは前の値から繋ぐ。
    void setFdnSends(const int* rooms, const float* gains, int n) {
        n = std::max(0, std::min(n, kMaxFdnSends));
        for (int k = 0; k < kMaxFdnSends; ++k) {
            pendFdnRoom_[k] = (k < n && rooms) ? rooms[k] : -1;
            pendFdnGain_[k] = (k < n && gains) ? std::max(0.0f, gains[k]) : 0.0f;
        }
        fdnVersion_.fetch_add(1, std::memory_order_release);
    }
    /// 尾の量だけを置く（IR を組まない道）。tailGain = 自由音場の直接 × √目標比（rebuildTail と同じ規約）。
    void setTailAmount(float directGain, float targetRatio) {
        tailGain_ = ReverbTailIr::calibrateGain(directGain, targetRatio);
    }
    /// 【手順 5】尾の開始（ms、直接音からの相対）。最初の壁の反射の到達 ＝ ITDG を渡す。
    ///   √V（反射が密になる統計の目安）だと 7 m を超える部屋で最初の壁より先に尾が鳴った（SPATIAL_DEPTH.md）。
    ///   送りの前の遅延線で作る。FDN 自身の最短の線（≒平均自由行程 1 つ）がその上に乗るので、尾の最初の
    ///   反射は「最初の壁 ＋ 壁 1 つ」＝ 2 次以降の反射の位置に来る（1 次は面の線音源が持つ）。
    ///   変えるときはチャンクの中で新旧の遅延の読みを線形に混ぜる（ピッチを動かさず、段も付けない）。
    void setFdnOnsetMs(float ms) {
        const float s = std::min(240.0f, std::max(0.0f, ms)) * 0.001f * static_cast<float>(sampleRate_);
        if (std::fabs(s - pendOnset_) < 0.25f) return;      // 同じ目標なら触らない（渡りの途中を乱さない）
        pendOnset_ = s;
        onsetVersion_.fetch_add(1, std::memory_order_release);
    }
    float fdnOnsetMs() const { return pendOnset_ * 1000.0f / static_cast<float>(sampleRate_); }
    static constexpr int kMaxFdnSends = 4;

    /// 反射・回折のタップを方向バスへ預ける。bus=NULL で自前の両耳化（軽量両耳化／パン）に戻る。
    ///   ★次の setTaps から効く（レーンの割り当てはタップを受け取るときに決める）。
    void setDirectionBus(DirectionBus* bus) {
        dirBus_ = bus;
        laneStride_ = maxFrames_ + EarlyReflectConv::kLaneItdMax;
        if (bus) laneBuf_.assign(static_cast<std::size_t>(bus->rows()) * laneStride_, 0.0f);
        laneCarry_ = 0;
    }
    const DirectionBus* directionBus() const { return dirBus_; }

    /// 尾 IR のクロスフェードの長さ(ms)。0 で即差し替え（＝ふくらむ。A/B 用）。
    void setTailCrossfadeMs(float ms) {
        tailConv_.setCrossfadeSamples(std::max(0, static_cast<int>(ms * 0.001f * sampleRate_)));
    }

    void setOutputGain(float g) { outputGain_ = g; }
    void setTailLevel(float g) { tailLevel_ = g; }        // 1.0 が物理どおり。好みの微調整
    void setScatterDiffusion(float g) { scatterDiffusion_ = g; }

    /// 尾の絶対ゲインの掛かり方。
    ///   srcLevel : 音源がこの部屋にどれだけエネルギーを注げているか（反射込みの生存）。
    ///   wet      : **尾のレベルには掛けない**。残響/直接比は rebuildTail に渡す target で
    ///              既に決まっているので、ここで wet を掛けると二重計上になる
    ///              （wet 自体が target から作られているため、なおさら）。
    ///              引数は互換のために残してあるが、診断で覗く以外の用途は無い。
    void setTailEnvelope(float wet, float srcLevel) {
        tailWet_ = clamp01(wet);
        tailSrcLevel_ = (srcLevel > 0.0f) ? srcLevel : 1.0f;
    }

    const char* hrtfName() const { return hrtf_.setName(); }
    int tailPartitions() const { return tailConv_.totalPartitions(); }
    int tailLatency() const { return tailConv_.latency(); }

    // ── オーディオスレッド ──

    /// モノラル入力 → ステレオ出力（**上書き**）。frames は maxFrames 以下に分割して回す。
    void render(const float* input, int frames, float* outL, float* outR, Metering* meter = nullptr) {
        if (!input || !outL || !outR || frames <= 0) return;
        Metering m;
        int done = 0;
        while (done < frames) {
            const int n = std::min(frames - done, maxFrames_);
            renderChunk(input + done, n, outL + done, outR + done, m, done);
            done += n;
        }
        const float inv = 1.0f / static_cast<float>(std::max(1, frames));
        m.rmsDirect = std::sqrt(m.rmsDirect * inv);
        m.rmsEarly = std::sqrt(m.rmsEarly * inv);
        m.rmsScatter = std::sqrt(m.rmsScatter * inv);
        m.rmsTail = std::sqrt(m.rmsTail * inv);
        m.rmsOut = std::sqrt(m.rmsOut * inv);
        if (meter) *meter = m;
    }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    void renderChunk(const float* input, int n, float* outL, float* outR, Metering& m,
                     int dstOffset = 0) {
        // ① 後期尾はブロック単位（周波数領域）。サンプルループより前に済ませる。
        std::fill(tailOutL_.begin(), tailOutL_.begin() + n, 0.0f);
        std::fill(tailOutR_.begin(), tailOutR_.begin() + n, 0.0f);
        // 尾の絶対レベル ＝ tailGain(= 自由音場の直接 × √target) × 好みの倍率 × 部屋への注入量。
        //   ★ここに tailWet_ を掛けてはいけない。残響/直接比は tailGain の √target で
        //     既に決まっており二重計上になる（C# 経路は掛けていなかったので 3.9dB 差が出た）。
        //   ★尾の FDN に預けているときは tailSrcLevel_（反射込みの生存＝遮蔽）を掛けない。
        //     部屋をまたぐ減りは配線（開口率² × 立体角）が持つので、ここでも掛けると二重になる
        //     （実測 2026-09-09: 扉 60° 越しで畳み込みより −21 dB。配線ぶんがそのまま出ていた）。
        const float lvl = tailLevel_ * (fdnMix_ ? 1.0f : tailSrcLevel_);
        // 差し替えの混ぜは器の中（新旧の IR スペクトルを等振幅で。遅延線は共有）。
        // 【尾の量の傾斜】tailGain_ は組み直し（ホストは 8 フレームに 1 回）でしか動かない。
        //   チャンク境界でそのまま掛けると**段差**になり、扉が動くと量が速く動くので
        //   それが「ぷつぷつ」になる（実測: 段差が平常の 51.8 倍。傾斜にすると 1.03 倍）。
        //   タップの差し替えと同じ考え方 ── 量は必ず時間方向に繋ぐ。ここはチャンク内で線形。
        //   ★tailSrcLevel_ の変化もここで一緒に飲む（掛けた後の値を持ち越しているため）。
        const float tgEnd = tailGain_ * lvl;
        const float tgStart = tailAppliedGain_;
        tailAppliedGain_ = tgEnd;
        // ★★ 共有バスがあるなら、自前では畳まず**送るだけ** ★★
        //   畳み込みは線形なので conv(IR, Σ gᵢ·xᵢ) = Σ conv(IR, gᵢ·xᵢ)。
        //   音源ごとのレベル（tailGain_ × lvl）は足す前に掛けるので、
        //   **音源ごとの尾の量は保たれる**。減るのは畳み込みの回数だけ。
        //   ⚠ IR が違う音源を同じバスへ入れてはいけない。部屋ごとに 1 本。
        //   ⚠ クロスフェードもバスが持つ（IR がバス側にあるので）。
        //   ⚠ この音源の rmsTail は 0 になる。計器はバス側の rms() を見ること。
        if (fdnMix_) {
            // 【尾の FDN】部屋ごとの FDN へ**送るだけ**（tailModel=1）。IR も器も無い。
            //   送り先と重みはホストが毎フレーム置く（fdnRoomWeights: 自分の部屋＝占め方、戸口越しの隣室＝開口率²×立体角）。
            //   量の傾斜は共有バスと同じ（tgStart → tgEnd をチャンク内で線形）。送りの重みも前のチャンクの値から繋ぐ。
            //   outputGain_ もここで掛ける（バスと同じ理由: リスナー側で足されるので最後の 1 回を通らない）。
            const int v = fdnVersion_.load(std::memory_order_acquire);
            if (v != fdnSeen_) {
                int room[kMaxFdnSends]; float tgt[kMaxFdnSends], cur[kMaxFdnSends];
                for (int k = 0; k < kMaxFdnSends; ++k) {
                    room[k] = pendFdnRoom_[k]; tgt[k] = pendFdnGain_[k]; cur[k] = 0.0f;
                    // 同じ部屋への送りが前からあれば、その重みから繋ぐ（並びが変わっても段にしない）。
                    for (int j = 0; j < kMaxFdnSends; ++j)
                        if (room[k] >= 0 && fdnRoom_[j] == room[k]) cur[k] = fdnGainCur_[j];
                }
                // 消えた送りは 1 チャンクかけて 0 へ繋ぐ（空きがあれば）。
                for (int j = 0; j < kMaxFdnSends; ++j) {
                    if (fdnRoom_[j] < 0 || fdnGainCur_[j] <= 0.0f) continue;
                    bool present = false;
                    for (int k = 0; k < kMaxFdnSends; ++k) if (room[k] == fdnRoom_[j]) present = true;
                    if (present) continue;
                    for (int k = 0; k < kMaxFdnSends; ++k)
                        if (room[k] < 0) { room[k] = fdnRoom_[j]; tgt[k] = 0.0f; cur[k] = fdnGainCur_[j]; break; }
                }
                for (int k = 0; k < kMaxFdnSends; ++k) { fdnRoom_[k] = room[k]; fdnGainTgt_[k] = tgt[k]; fdnGainCur_[k] = cur[k]; }
                fdnSeen_ = v;
            }
            // 尾の開始（手順 5）: 送りを ITDG ぶん遅らせる。新旧の遅延の読みをチャンク内で線形に混ぜる。
            const float* sendSrc = input;
            {
                const int len = fdnDelayLen_;
                float* ring = fdnDelay_.data();
                for (int i = 0; i < n; ++i) ring[(fdnDelayW_ + i) % len] = input[i];
                // 目標が変わっていれば取り込む。渡りの長さは跳びの 4 倍（1 チャンク〜200 ms）。
                //   ★1 ブロックで混ぜ切ると、正弦では位相の跳び（35 ms ＝ 7.7 周期）が FDN の定常状態を崩して
                //     隣り合うブロックで 11.7 dB 跳んだ（実測）。歩きでは ITDG は連続に動く（跳びは 1 ms 未満 → 1 ブロック）。
                //     大きく跳ぶのは最初の反射が別の面に替わったとき（扉が開いた等）で、そこは跳びなりに長く混ぜる。
                {
                    const int ov = onsetVersion_.load(std::memory_order_acquire);
                    if (ov != onsetSeen_) {
                        const float s = pendOnset_;
                        // 渡りの途中なら、半分を過ぎていれば向かっていた先を旧にする（半分前なら旧のまま）。
                        if (fdnXf_ < 1.0f && fdnXf_ > 0.5f) fdnOnsetOld_ = fdnOnsetNew_;
                        else if (fdnXf_ >= 1.0f) fdnOnsetOld_ = fdnOnsetNew_;
                        fdnOnsetNew_ = s;
                        const float jump = std::fabs(fdnOnsetNew_ - fdnOnsetOld_);
                        const float T = std::min(0.2f * static_cast<float>(sampleRate_), std::max(static_cast<float>(maxFrames_), jump * 4.0f));
                        fdnXf_ = 0.0f;
                        fdnXfStep_ = 1.0f / T;
                        onsetSeen_ = ov;
                    }
                }
                auto readAt = [&](int i, float d) {
                    const float pos = static_cast<float>(fdnDelayW_ + i) - d;   // 遅延 d（小数）だけ前
                    const int p0 = static_cast<int>(std::floor(pos));
                    const float fr = pos - static_cast<float>(p0);
                    const int a = ((p0 % len) + len) % len, b2 = (a + 1) % len;
                    return ring[a] * (1.0f - fr) + ring[b2] * fr;
                };
                float* dst = fdnSendBuf_.data();
                if (fdnXf_ >= 1.0f) {
                    for (int i = 0; i < n; ++i) dst[i] = readAt(i, fdnOnsetNew_);
                } else {
                    for (int i = 0; i < n; ++i) {
                        const float t = std::min(1.0f, fdnXf_ + fdnXfStep_ * static_cast<float>(i + 1));
                        dst[i] = readAt(i, fdnOnsetOld_) * (1.0f - t) + readAt(i, fdnOnsetNew_) * t;
                    }
                    fdnXf_ = std::min(1.0f, fdnXf_ + fdnXfStep_ * static_cast<float>(n));
                    if (fdnXf_ >= 1.0f) fdnOnsetOld_ = fdnOnsetNew_;
                }
                fdnDelayW_ = (fdnDelayW_ + n) % len;
                sendSrc = dst;
            }
            for (int k = 0; k < kMaxFdnSends; ++k) {
                if (fdnRoom_[k] < 0) continue;
                const float gS = tgStart * outputGain_ * fdnGainCur_[k];
                const float gE = tgEnd * outputGain_ * fdnGainTgt_[k];
                if (gS > 0.0f || gE > 0.0f) fdnMix_->add(fdnRoom_[k], sendSrc, n, gS, gE, dstOffset);
                fdnGainCur_[k] = fdnGainTgt_[k];
                if (fdnGainTgt_[k] <= 0.0f) fdnRoom_[k] = -1;   // 繋ぎ終えた送りは片付ける
            }
        } else if (tailBus_) {
            // 送るだけ。tailOutL_/R_ は上で 0 埋め済みなので、この音源からは尾が出ない。
            //
            // ★★ outputGain_ を**ここで掛ける** ★★
            //   自前で畳むとき、尾は他の段と一緒に最後で `l *= outputGain_` を受ける。
            //   バスへ回すとリスナー側で足されるので、**その 1 回を通らない**。
            //   掛け忘れると尾だけ 1/outputGain 倍（既定 0.6 なら 1.67 倍、
            //   負荷検証シーンの 0.12 なら **8.3 倍**）大きくなる。実機で「聞こえ方が壊れる」
            //   と報告されて分かった。
            //   ⚠ 検査が見逃したのは setOutputGain(1.0f) で回していたから。
            //     **等倍だと掛け忘れが見えない。**検査側は音源ごとに違う値にしてある。
            //   ⚠ AudioSource の volume はホスト側で OnAudioFilterRead の後に掛かるので、
            //     ここでは拾えない。音源ごとに volume を変えるなら outputGain に寄せること。
            if (tgStart > 0.0f || tgEnd > 0.0f)
                tailBus_->add(input, n, tgStart * outputGain_, tgEnd * outputGain_, dstOffset);
        } else {
            float* dst[2] = { tailOutL_.data(), tailOutR_.data() };
            // ★量は**入力側**で渡す。共有バスは全音源を足してから畳むので入力側でしか掛けられず、
            //   ここで出力側に掛けると、量が動いている間だけバスと結果が食い違う
            //   （回帰 [尾②]「バスに差しても出力が変わらない」が -8.1dB でこれを捕まえた）。
            if ((tgStart > 0.0f || tgEnd > 0.0f) && tailConv_.hasIr()) {
                const float step = (n > 1) ? (tgEnd - tgStart) / static_cast<float>(n - 1) : 0.0f;
                float g = (n > 1) ? tgStart : tgEnd;
                for (int i = 0; i < n; ++i) {
                    tailIn_[static_cast<std::size_t>(i)] = input[i] * g;
                    g += step;
                }
                tailConv_.processAdd(tailIn_.data(), 0, n, dst, 0, 1.0f);
            }
        }

        // ② HRTF はブロック境界で HRIR を取り込む（方向変化のクロスフェード開始）。
        const bool hrtfActive = hrtfEnabled_ && hrtf_.isReady();
        // 回折バスは**載っているタップがあるときだけ**回す。HrtfProcessor は入力が 0 でも
        //   IR 長ぶんを毎サンプル畳むので、見通せている間ずっと空回しすると
        //   何も鳴っていないのにコストだけ +16% 払い続けることになる。
        const bool difActive = hrtfActive && early_.hasHrtfBus() && hrtfDif_.isReady();
        if (hrtfActive) hrtf_.beginBlock();
        if (difActive) hrtfDif_.beginBlock();
        early_.beginBlock();
        // 方向バスがあれば、このチャンクぶんのレーンの箱を 0 にしておく（[レーン][maxFrames]）。
        const int rows = dirBus_ ? dirBus_->rows() : 0;   // 方向 × 2 耳
        float* laneOut = nullptr;
        if (rows > 0) {
            // 行は maxFrames + kLaneItdMax。前のチャンクで末尾の余裕へ書かれた ITD ぶんを頭へ持ち越してから、残りを 0 にする。
            if (laneBuf_.size() < static_cast<std::size_t>(rows) * laneStride_)
                laneBuf_.assign(static_cast<std::size_t>(rows) * laneStride_, 0.0f);
            const int carry = EarlyReflectConv::kLaneItdMax;
            for (int k = 0; k < rows; ++k) {
                float* row = laneBuf_.data() + static_cast<std::size_t>(k) * laneStride_;
                if (laneCarry_ > 0) std::copy(row + laneCarry_, row + laneCarry_ + carry, row);
                else std::fill(row, row + carry, 0.0f);
                std::fill(row + carry, row + n + carry, 0.0f);
            }
            laneOut = laneBuf_.data();
        }

        for (int f = 0; f < n; ++f) {
            const float dry = input[f];

            float l, r, scatSendL, scatSendR, directMono, difMono;
            early_.processSample(dry, hrtfActive, l, r,
                                 scatSendL, scatSendR, directMono, difMono,
                                 laneOut ? laneOut + f : nullptr, laneStride_);

            // 直接音を HRTF で両耳化して足す（パンの代わり）。
            float dirL = 0.0f, dirR = 0.0f;
            if (hrtfActive) {
                hrtf_.processSample(directMono, dirL, dirR);
                l += dirL; r += dirR;
            } else {
                // HRTF 無効時、直接音は early_ 側でパン済み。計測用に切り分けられないので
                // モノラル値をそのまま両耳ぶんとして数える。
                dirL = dirR = directMono * 0.70710678f;
            }

            // 回折バスを**別方向の** HRTF で両耳化して足す。
            //   これは早期成分なので m.rmsEarly に入る（下の eL/eR に含まれる）。
            if (difActive) {
                float dfL = 0.0f, dfR = 0.0f;
                hrtfDif_.processSample(difMono, dfL, dfR);
                l += dfL * difNorm_; r += dfR * difNorm_;
            }

            m.rmsDirect += (dirL * dirL + dirR * dirR) * 0.5f;
            const float eL = l - dirL, eR = r - dirR;
            m.rmsEarly += (eL * eL + eR * eR) * 0.5f;

            // ③ 散乱スメア：拡散成分を allpass で撹拌して時間方向に滲ませる。
            //   ★左右別の送りを、それぞれの拡散器へ通す。オールパスは元から 2 本なので
            //     コストは変わらないが、**散乱が方向を持つ**ようになる。
            //     モノラル 1 本を 2 本の拡散器へ通していたときは、左右のレベルが必ず同じ＝
            //     構造的に ILD が 0 で、しかも散乱が出力の 100〜179% を占めていた
            //     （実測・く字廊下）。方向を持つ成分がその下に埋もれていた。
            // ★拡散の分は 1 サンプル遅らせてから撹拌する（2026-09-13）。allpass 3 段の最初のサンプルは (−g)³（g 0.62 で −0.238）なので、
            //   同じタップの鏡面の分（素通し）と同じ瞬間に打ち消し合い、広がり s のタップが 2·√(s(1−s))·(−0.238) だけ小さくなっていた
            //   （s 0.5 で −1.2 dB）。虚像のタップは広がりがほぼ 0 で隠れていて、受取面（広がり 0.3〜0.8）の [橋] で −0.57 dB として出た。
            //   1 サンプル（21 µs）遅らせると、自分の鏡面の分との重なりは h(−1) = 0 で消える。
            const float scL = diffL_.process(scatPrevL_, scatterDiffusion_);
            const float scR = diffR_.process(scatPrevR_, scatterDiffusion_);
            scatPrevL_ = scatSendL; scatPrevR_ = scatSendR;
            m.rmsScatter += (scL * scL + scR * scR) * 0.5f;
            l += scL; r += scR;

            // ④ 実測尾を加算。
            const float tL = tailOutL_[static_cast<std::size_t>(f)];
            const float tR = tailOutR_[static_cast<std::size_t>(f)];
            m.rmsTail += (tL * tL + tR * tR) * 0.5f;
            l += tL; r += tR;

            l *= outputGain_; r *= outputGain_;
            m.rmsOut += (l * l + r * r) * 0.5f;
            outL[f] = l;
            outR[f] = r;
        }
        // 方向バスへ送る。outputGain_ はここで掛ける（尾のバスと同じ理由: リスナー側で足されるので、
        //   最後の l *= outputGain_ を通らない）。計器には両耳ぶんとして足しておく（実際の両耳化はバス側）。
        if (laneOut) {
            float e = 0.0f;
            for (int k = 0; k < rows; ++k) {
                const float* v = laneOut + static_cast<std::size_t>(k) * laneStride_;
                for (int i = 0; i < n; ++i) e += v[i] * v[i];
            }
            m.rmsEarly += e * outputGain_ * outputGain_;
            dirBus_->add(laneOut, n, laneStride_, outputGain_, dstOffset);
            laneCarry_ = n;   // 末尾の余裕（[n, n+kLaneItdMax)）は次のチャンクの頭へ
        }
    }

    Config cfg_;
    const int sampleRate_;
    const int tailSamples_;
    int maxFrames_ = 4096;

    EarlyReflectConv early_;
    HrtfProcessor hrtf_;
    // 回折バス用の 2 本目。方向が違うので別インスタンスが要る。
    //   回折タップが 1 本も無いフレームでは回さない（下の difActive）。
    HrtfProcessor hrtfDif_;
    // 共有バス。null なら自前で畳む（既定＝これまでどおり）。
    //   バスを差すと、この音源は尾を**送るだけ**になり、畳み込みは 1 回に集約される。
    TailBus* tailBus_ = nullptr;
    DirectionBus* dirBus_ = nullptr;   // 反射・回折タップの行き先（リスナーに 1 組、全音源で共有）
    std::vector<float> laneBuf_;       // [行][maxFrames + kLaneItdMax] このチャンクの送り（末尾は ITD の持ち越し）
    int laneStride_ = 0;
    int laneCarry_ = 0;                 // 前のチャンクの長さ（持ち越しの読み出し位置）
    bool tailBusOwner_ = false;    // この音源が IR をバスへ入れる係か（部屋の代表）
    // 尾の FDN（tailModel=1）。null なら従来（畳み込み）。差すと IR を組まず、部屋ごとの FDN へ送るだけ。
    FdnRoomMix* fdnMix_ = nullptr;
    int   fdnRoom_[kMaxFdnSends] = { -1, -1, -1, -1 };      // オーディオスレッドの写し
    float fdnGainTgt_[kMaxFdnSends] = {};
    float fdnGainCur_[kMaxFdnSends] = {};                   // 直前のチャンクの終わりの重み（傾斜の出発点）
    int   pendFdnRoom_[kMaxFdnSends] = { -1, -1, -1, -1 };  // 制御スレッドが書く（版で受け渡す）
    float pendFdnGain_[kMaxFdnSends] = {};
    std::atomic<int> fdnVersion_{0};
    int   fdnSeen_ = 0;
    // 尾の開始（手順 5）: 送りの前の遅延線と、その読み位置（サンプル。小数）。
    std::vector<float> fdnDelay_;
    std::vector<float> fdnSendBuf_;
    int   fdnDelayLen_ = 1;
    int   fdnDelayW_ = 0;
    float pendOnset_ = 0.0f;                 // 制御スレッドが書く目標（サンプル）
    std::atomic<int> onsetVersion_{0};
    int   onsetSeen_ = 0;
    float fdnOnsetOld_ = 0.0f, fdnOnsetNew_ = 0.0f;   // 渡りの旧と新（オーディオスレッド）
    float fdnXf_ = 1.0f, fdnXfStep_ = 0.0f;           // 渡りの位置（0..1）と 1 サンプルあたりの進み
    // 器は 1 つ。差し替えの混ぜは中（遅延線は共有、IR スペクトルだけ 2 世代）。
    NonUniformConvolver tailConv_;
    ReverbTailIr tailIr_;
    AllpassChain diffL_, diffR_;

    std::vector<float> tailIn_;             // 量の傾斜を掛けた入力（バスと同じ扱いにするため）
    std::vector<float> tailOutL_, tailOutR_;

    bool hrtfEnabled_ = true;
    // 反射タップの軽量な両耳化（ITD ＋ 帯域別 ILD）。HRTF セットから焼き込む。
    const HrtfSet* hrtfSet_ = nullptr;
    bool  earCues_ = true;
    float headCm_ = 57.0f;
    std::vector<EarlyReflectConv::Tap> tapScratch_;
    float difNorm_ = 1.0f;      // 回折バスをパンと同音量に揃える係数（setHrtfSet で決まる）
    float outputGain_ = 0.6f;
    float tailGain_ = 0.0f;
    // 直前のチャンクの終わりで実際に掛けた量（＝次のチャンクの傾斜の出発点）。
    //   これが無いと、組み直しのたびに量がチャンク境界で飛ぶ。
    float tailAppliedGain_ = 0.0f;
    float tailLevel_ = 1.0f;
    float tailWet_ = 1.0f;
    float tailSrcLevel_ = 1.0f;
    float scatterDiffusion_ = 0.62f;
    float scatPrevL_ = 0.0f, scatPrevR_ = 0.0f;   // 拡散の分の 1 サンプル遅れ
};

}  // namespace dsp
}  // namespace af
