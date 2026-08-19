// early_reflect_conv.h — 早期反射のマルチタップ畳み込み（6帯域分割・パン・散乱・IR切替）。
//
// エンジンが出すタップ（直接音／透過／回折／早期反射）を、離散的な遅延タップの列として
// 畳み込む。後期尾は別（nonuniform_convolver + reverb_tail_ir）が担当する。
//
// 6 帯域に分ける理由:
//   エンジンのゲインは 125/250/500/1k/2k/4kHz の 6 帯域で出てくる。入力を同じ境目で
//   分けておけば、タップごとに 6 個の係数を掛けるだけで周波数特性を再現できる。
//   クロスオーバーは隣接帯域の幾何平均（オクターブバンドの境目）。
//   直列に LP を掛けて「低域を抜き取った残り」を次へ送るので、全帯域を足すと元に戻る。
//
// 鏡面と拡散の分割:
//   各タップを 鏡面=√(1-s) / 拡散=√s に分ける。振幅ではなく √ で分けるのは、
//   両者が無相関でパワーが加算されるため（(1-s)+s=1 で総量保存）。
//   拡散ぶんはまとめて呼び出し側の拡散器へ送り、時間方向に滲ませる。
//   散乱率 s は遅延とともに上がる ── 反射は1回バウンスするごとに散乱が加わるので、
//   遅く届くタップ＝高次反射ほど鏡面成分が減って拡散へ溶ける。
//
// タップ差し替えの繋ぎ方 ── **出力のクロスフェードではなくパラメータの補間**:
//   素朴には新旧2本を同時に畳み込んで混ぜる（クロスフェード）が、それだと
//     ・遅延の違う2本が同時に鳴る＝一瞬ダブって聞こえる／櫛が立つ
//     ・フェード中だけコストが倍
//     ・新しく現れたタップが**いきなり本来のゲインで**混ざり始める
//   の3つが起きる。遅延・ゲイン・パンを補間すれば、音像がスッと動くだけで済む。
//
//   タップの対応付けは遅延の近さで貪欲に取る。開口の並び順は重み順なので、
//   更新のたびに index が入れ替わりうる。index で対応させると、別の開口どうしを
//   補間して遅延が大きく掃引され「ヒュン」と鳴る。
//
//   ★対応相手がいないタップは**ゲイン0から**立ち上がる／**ゲイン0へ**落ちる。
//     これで「開口が生えた瞬間に本来の音量で鳴り出す」段差が構造的に消える。
//
//   幾何の更新は数フレームに1回へ間引かれている（60fps で 15Hz など）。
//   その間をここで埋めるので、移動する音源や動く扉でも音像が階段状にならない。
//
// スレッド規約:
//   setTaps() … 制御スレッド。ここでだけ確保する。
//   processAdd()/processSample() … オーディオスレッド。確保・ロックなし。
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/IrConvolver.cs の
// ConvTap / RebuildIr / ConvOne / 帯域分割まわり。
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace af {
namespace dsp {

class EarlyReflectConv {
public:
    static constexpr int kNumBands = 6;

    // 1 タップ。audio thread で毎サンプル sqrt を呼ばないよう、構築時に畳んである。
    struct Tap {
        int delaySamples = 0;
        float g[kNumBands] = {0, 0, 0, 0, 0, 0};   // 6帯域それぞれのゲイン
        float panL = 0.70710678f, panR = 0.70710678f;
        float gSpec = 1.0f;    // √(1-s) 鏡面（方向つき）
        float gDiff = 0.0f;    // √s     拡散（方向を失った成分）
        // このタップを HRTF バスへ載せる割合。0=パンのまま / 1=丸ごと HRTF。
        //   直接音(index 0)は従来どおり outDirect へ出るので、ここは使わない。
        //   遮蔽されると直接音は材質の透過まで落ち、実エネルギーを運ぶのは回折タップになる。
        //   そのとき HRTF が掛かっているのが直接音だけだと、**聞こえている音のほうに
        //   ITD も前後の手がかりも無い**。開口の方向を運ぶタップにこそ掛ける必要がある。
        //   ★呼び出し側は 0/1 で渡してよい。切り替えの連続性はタップ補間が受け持つ
        //     （乗り換え中は両方が中間値になり、片方がフェードアウトしながら他方が入る）。
        float hrtfWeight = 0.0f;

        // ── 軽量な両耳化（ITD ＋ 帯域別 ILD）──
        //
        //   反射タップを 1 本ずつ HRIR で畳み込むと重い（HRTF 1 本 0.05〜0.12ms/block、
        //   反射 4 本で音源あたり +0.2〜0.47ms。しかも反射は常に鳴っている）。
        //   タップは元から「小数遅延 ＋ 6 帯域ゲイン」なので、それを**耳ごとに 2 組**
        //   持つだけで ITD と ILD が載る ── 畳み込みは増えない。
        //   ★実測（く字の廊下）では、方向を運べるエネルギーの大半が早期反射側にあり
        //     （早期反射 16.24 対 回折二次音源 0.0070 ＝ 2300 倍）、そこが
        //     等パワーパンのままだった。少数の強いタップだけ HRTF に載せても届かない。
        //   ★前後・上下は載らない（ITD/ILD は円錐の曖昧さを解けない）。そこは
        //     直接音と最強の回折タップがフル HRTF を通るので、そちらが持つ。
        //   earUse=false なら panL/panR の従来どおり。
        bool  earUse = false;
        float earDelay[2] = {0.0f, 0.0f};        // 耳ごとの追加遅延(サンプル)。ITD
        float earGain[2][kNumBands] = {{1,1,1,1,1,1}, {1,1,1,1,1,1}};
        // 到来方向（リスナー座標系）。呼び出し側はこれだけ渡せばよく、
        // 上の 3 つは VoiceRenderer が HRTF セットから埋める。
        float dir[3] = {0.0f, 0.0f, 1.0f};
        bool  dirValid = false;
    };

    /// maxDelaySamples : 履歴リングの長さの目安（早期↔後期の境目ぶん）
    EarlyReflectConv(int sampleRate, int maxDelaySamples, float crossfadeMs = 30.0f)
        : sampleRate_(std::max(8000, sampleRate)) {
        const int need = nextPow2(std::max(64, maxDelaySamples) + 8);
        ringMask_ = need - 1;
        ring_.assign(static_cast<std::size_t>(kNumBands) * need, 0.0f);
        lerpLen_ = std::max(1, static_cast<int>(std::lround(crossfadeMs * 0.001f * sampleRate_)));

        static const float kCrossHz[kNumBands - 1] = {177.0f, 354.0f, 707.0f, 1414.0f, 2828.0f};
        for (int i = 0; i < kNumBands - 1; ++i)
            split_[i].setLowpass(kCrossHz[i], static_cast<float>(sampleRate_));
    }

    ~EarlyReflectConv() {
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    }

    EarlyReflectConv(const EarlyReflectConv&) = delete;
    EarlyReflectConv& operator=(const EarlyReflectConv&) = delete;

    int maxDelaySamples() const { return ringMask_; }
    bool hasTaps() const {
        return !from_.empty() || pending_.load(std::memory_order_acquire) != nullptr;
    }
    int liveTapCount() const { return static_cast<int>(from_.size()); }
    /// HRTF バスに載っているタップがあるか（無ければ呼び出し側は HRTF を回さなくてよい）。
    bool hasHrtfBus() const { return hrtfBus_; }
    /// 診断用: 補間の終端(to_)における i 番目のタップの低域ゲインと遅延。
    float debugTargetGain(int i) const {
        return (i >= 0 && i < static_cast<int>(to_.size())) ? to_[static_cast<std::size_t>(i)].g[0] : -1.0f;
    }
    float debugStartGain(int i) const {
        return (i >= 0 && i < static_cast<int>(from_.size())) ? from_[static_cast<std::size_t>(i)].g[0] : -1.0f;
    }
    float debugProgress() const {
        return lerping_ ? static_cast<float>(lerpPos_) / static_cast<float>(lerpLen_) : -1.0f;
    }
    float debugTargetDelay(int i) const {
        return (i >= 0 && i < static_cast<int>(to_.size())) ? to_[static_cast<std::size_t>(i)].delay : -1.0f;
    }

    /// タップ集合を差し替える（制御スレッド）。実際の切替は次の processSample から
    /// クロスフェードして行われる。taps[0] は直接音であること（呼び出し側の並び順）。
    void setTaps(const Tap* taps, int count) {
        if (!taps || count < 0) return;
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);   // 前回退役ぶんを解放
        TapSet* set = new TapSet();
        set->taps.assign(taps, taps + count);
        delete pending_.exchange(set, std::memory_order_acq_rel);
    }

    /// オーディオスレッド：ブロックの先頭で1回呼ぶ。保留中のタップ集合を取り込む。
    void beginBlock() {
        TapSet* p = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (!p) return;

        // 進行中の補間があれば、今の瞬間の値を新しい出発点にする（途中で飛ばさない）。
        snapshotCurrent();

        // 対応付け: 新しいタップごとに、遅延がいちばん近い未使用の現タップを取る。
        //   index で対応させると別の開口どうしを繋いで遅延が掃引される（ヒュンと鳴る）。
        const int nNew = static_cast<int>(p->taps.size());
        const int nCur = static_cast<int>(from_.size());
        matched_.assign(static_cast<std::size_t>(nCur), false);
        to_.assign(from_.begin(), from_.end());          // まず現状を写す
        for (RtTap& t : to_) t.setGain(0.0f);            // 相手のいない現タップは 0 へ落とす

        for (int j = 0; j < nNew; ++j) {
            const Tap& nt = p->taps[static_cast<std::size_t>(j)];
            int best = -1;
            float bestD = kMatchDelayTolerance;
            for (int i = 0; i < nCur; ++i) {
                if (matched_[static_cast<std::size_t>(i)]) continue;
                const float d = std::fabs(from_[static_cast<std::size_t>(i)].delay
                                          - static_cast<float>(nt.delaySamples));
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0) {
                matched_[static_cast<std::size_t>(best)] = true;
                to_[static_cast<std::size_t>(best)] = RtTap::from(nt);
            } else {
                // 相手がいない＝新しく現れた開口。**ゲイン0から**立ち上げる。
                RtTap start = RtTap::from(nt);
                start.setGain(0.0f);
                from_.push_back(start);
                to_.push_back(RtTap::from(nt));
            }
        }

        lerpPos_ = 0;
        lerping_ = true;
        updateHrtfBus();
        delete retired_.exchange(p, std::memory_order_acq_rel);
    }

    /// オーディオスレッド：1サンプル。
    ///   outL/outR   : 鏡面成分（直接音は hrtfActive のとき含まれない）
    ///   outDiffuse  : 拡散送り（呼び出し側の拡散器へ）
    ///   outDirect   : 直接音タップのモノラル値（HRTF で両耳化する用）
    ///   outHrtfMono : hrtfWeight>0 のタップのモノラル和（**別方向の** HRTF で両耳化する用）
    void processSample(float x, bool hrtfActive,
                       float& outL, float& outR, float& outDiffuse, float& outDirect,
                       float& outHrtfMono) {
        // 6 帯域に分ける。低域を抜き取った残りを次へ送るので、足すと元に戻る。
        const int wp = writePos_ & ringMask_;
        float rest = x;
        for (int b = 0; b < kNumBands - 1; ++b) {
            const float lo = split_[b].process(rest);
            ring_[bandIndex(b, wp)] = lo;
            rest -= lo;
        }
        ring_[bandIndex(kNumBands - 1, wp)] = rest;

        outL = 0.0f; outR = 0.0f; outDiffuse = 0.0f; outDirect = 0.0f; outHrtfMono = 0.0f;

        // 補間の進み具合。0..1。
        //   ★from_ は「補間の出発点」で不変。進捗はここでだけ読む。
        const float t = lerping_
            ? static_cast<float>(lerpPos_) / static_cast<float>(lerpLen_) : 1.0f;

        const int n = static_cast<int>(to_.size());
        for (int k = 0; k < n; ++k) {
            const RtTap& a = from_[static_cast<std::size_t>(k)];
            const RtTap& b = to_[static_cast<std::size_t>(k)];

            // 遅延は小数のまま補間し、リングを線形補間で読む。
            //   整数に丸めるとタップが1サンプルずつ飛んで「ジリッ」と鳴る。
            const float delay = a.delay + (b.delay - a.delay) * t;
            const int d0 = static_cast<int>(delay);
            const float frac = delay - static_cast<float>(d0);
            const int rp0 = (wp - d0) & ringMask_;
            const int rp1 = (wp - d0 - 1) & ringMask_;

            float tv = 0.0f;
            for (int bd = 0; bd < kNumBands; ++bd) {
                const float g = a.g[bd] + (b.g[bd] - a.g[bd]) * t;
                if (g == 0.0f) continue;
                const float s0 = ring_[bandIndex(bd, rp0)];
                const float s1 = ring_[bandIndex(bd, rp1)];
                tv += g * (s0 + frac * (s1 - s0));
            }
            const float gDiff = a.gDiff + (b.gDiff - a.gDiff) * t;
            const float gSpec = a.gSpec + (b.gSpec - a.gSpec) * t;
            outDiffuse += tv * gDiff;
            const float sp = tv * gSpec;

            // ── 軽量な両耳化 ──
            //   耳ごとに「遅延をずらして読み直し、帯域ゲインを変える」だけ。
            //   畳み込みは増えず、リングをもう一度読むぶんで済む。
            //   ★index 0（直接音）と HRTF バスに載せたタップはここを通さない。
            //     あちらはフル HRTF が担当なので、二重に方向を付けることになる。
            if (a.earUse && k != 0) {
                bool skipPan = true;
                if (hrtfActive) {
                    const float hw = a.hrtfW + (b.hrtfW - a.hrtfW) * t;
                    if (hw > 0.0f) skipPan = false;      // HRTF バス側が担当
                }
                if (skipPan) {
                    for (int e = 0; e < 2; ++e) {
                        const float ed = a.earDelay[e] + (b.earDelay[e] - a.earDelay[e]) * t;
                        const float dl = delay + ed;
                        const int e0 = static_cast<int>(dl);
                        const float ef = dl - static_cast<float>(e0);
                        const int ep0 = (wp - e0) & ringMask_;
                        const int ep1 = (wp - e0 - 1) & ringMask_;
                        float ev = 0.0f;
                        for (int bd = 0; bd < kNumBands; ++bd) {
                            const float g = a.g[bd] + (b.g[bd] - a.g[bd]) * t;
                            if (g == 0.0f) continue;
                            const float eg = a.earGain[e][bd]
                                           + (b.earGain[e][bd] - a.earGain[e][bd]) * t;
                            const float s0 = ring_[bandIndex(bd, ep0)];
                            const float s1 = ring_[bandIndex(bd, ep1)];
                            ev += g * eg * (s0 + ef * (s1 - s0));
                        }
                        if (e == 0) outL += ev * gSpec; else outR += ev * gSpec;
                    }
                    continue;      // パンは通さない（両耳ぶんは上で出した）
                }
            }
            // index 0 は必ず直接音（呼び出し側の並び順）。
            if (k == 0) {
                outDirect = sp;
                if (hrtfActive) continue;   // 両耳化は呼び出し側に任せる
            }
            float pan = 1.0f;
            if (hrtfActive) {
                // HRTF バスへ回すぶんを抜く。残り(1-hw)はこれまでどおりパンで出す。
                //   乗り換え中だけ 0<hw<1 になり、両経路に割れる。HRTF もパンも
                //   おおむねエネルギーを保つので、その間の総量はほぼ動かない。
                const float hw = a.hrtfW + (b.hrtfW - a.hrtfW) * t;
                if (hw > 0.0f) {
                    outHrtfMono += sp * hw;
                    pan = 1.0f - hw;
                    if (pan <= 0.0f) continue;
                }
            }
            outL += sp * pan * (a.panL + (b.panL - a.panL) * t);
            outR += sp * pan * (a.panR + (b.panR - a.panR) * t);
        }

        if (lerping_ && ++lerpPos_ >= lerpLen_) {
            lerping_ = false;
            from_ = to_;
            // ゲインが 0 に落ちきったタップは捨てる（毎回走査すると無駄なので）。
            std::size_t keep = 0;
            for (std::size_t i = 0; i < from_.size(); ++i) {
                if (i == 0 || !from_[i].silent()) {
                    if (keep != i) from_[keep] = from_[i];
                    ++keep;
                }
            }
            from_.resize(keep);
            to_ = from_;
            updateHrtfBus();      // 落ちきったタップが捨てられた＝バスが空になったかもしれない
        }
        ++writePos_;
    }

    /// オーディオスレッド：ブロック単位。outL/outR に**加算**する。
    /// 直接音は hrtfActive のとき outDirectMono へ書き出すので、呼び出し側が HRTF に通す。
    void processAdd(const float* input, int inOffset, int n, bool hrtfActive,
                    float* outL, float* outR, int outOffset, float gain,
                    float* outDiffuse = nullptr, float* outDirectMono = nullptr,
                    float* outHrtfMono = nullptr) {
        if (!input || !outL || !outR) return;
        beginBlock();
        for (int i = 0; i < n; ++i) {
            float l, r, d, dir, hm;
            processSample(input[inOffset + i], hrtfActive, l, r, d, dir, hm);
            outL[outOffset + i] += l * gain;
            outR[outOffset + i] += r * gain;
            if (outDiffuse) outDiffuse[i] = d;
            if (outDirectMono) outDirectMono[i] = dir;
            if (outHrtfMono) outHrtfMono[i] = hm;
        }
    }

    /// 遅延から散乱率を決める補助（制御スレッドでタップを組むときに使う）。
    ///   反射は1回バウンスするごとに散乱が加わるので、遅く届くタップほど拡散へ溶ける。
    ///   mixing time は定義上「音場が拡散したとみなせる時刻」なので、そこで拡散率 1 に達する。
    ///   ★正規化には**頭打ち前の真の mixing time** を使うこと。畳み込みブロックの都合で
    ///     下限を掛けた値で割ると、狭い部屋（√V≒5〜8ms）では実際の4倍以上に膨らみ、
    ///     2〜7ms に来る反射が「まだ拡散していない」と判定されて効果が消える。
    static void scatterSplit(float delayMs, float mixingTimeMs, float scatterAmount,
                             float timeGrowth, float& outSpec, float& outDiff) {
        const float mix = (mixingTimeMs > 1e-3f) ? mixingTimeMs : 1e-3f;
        const float t = clamp01(delayMs / mix) * timeGrowth;
        const float s = clamp01(scatterAmount + (1.0f - scatterAmount) * clamp01(t));
        outSpec = std::sqrt(1.0f - s);
        outDiff = std::sqrt(s);
    }

private:
    struct TapSet {
        std::vector<Tap> taps;
    };

    // 実行時のタップ。遅延を**小数**で持つのが Tap との違い（補間するため）。
    struct RtTap {
        float delay = 0.0f;
        float g[kNumBands] = {0, 0, 0, 0, 0, 0};
        float panL = 0.70710678f, panR = 0.70710678f;
        float gSpec = 1.0f, gDiff = 0.0f;
        float hrtfW = 0.0f;
        bool  earUse = false;
        float earDelay[2] = {0.0f, 0.0f};
        float earGain[2][kNumBands] = {{1,1,1,1,1,1}, {1,1,1,1,1,1}};

        static RtTap from(const Tap& t) {
            RtTap r;
            r.delay = static_cast<float>(t.delaySamples);
            for (int b = 0; b < kNumBands; ++b) r.g[b] = t.g[b];
            r.panL = t.panL; r.panR = t.panR;
            r.gSpec = t.gSpec; r.gDiff = t.gDiff;
            r.hrtfW = t.hrtfWeight;
            r.earUse = t.earUse;
            for (int e = 0; e < 2; ++e) {
                r.earDelay[e] = t.earDelay[e];
                for (int b = 0; b < kNumBands; ++b) r.earGain[e][b] = t.earGain[e][b];
            }
            return r;
        }
        void setGain(float v) { for (int b = 0; b < kNumBands; ++b) g[b] = v; }
        bool silent() const {
            for (int b = 0; b < kNumBands; ++b) if (g[b] != 0.0f) return false;
            return true;
        }
    };

    // 対応付けで「同じ開口」とみなす遅延の差（サンプル）。
    //   開口はフレーム間で数サンプルしか動かない。これより離れていれば別の開口とみなし、
    //   ゲイン0から立ち上げる（＝繋がずに新しく生やす）。
    static constexpr float kMatchDelayTolerance = 256.0f;

    // 進行中の補間があれば、今の瞬間の値を from_ に固定する。
    void snapshotCurrent() {
        if (!lerping_) return;
        const float t = static_cast<float>(lerpPos_) / static_cast<float>(lerpLen_);
        for (std::size_t i = 0; i < from_.size(); ++i) {
            RtTap& a = from_[i];
            const RtTap& b = to_[i];
            a.delay += (b.delay - a.delay) * t;
            for (int k = 0; k < kNumBands; ++k) a.g[k] += (b.g[k] - a.g[k]) * t;
            a.panL += (b.panL - a.panL) * t;
            a.panR += (b.panR - a.panR) * t;
            a.gSpec += (b.gSpec - a.gSpec) * t;
            a.gDiff += (b.gDiff - a.gDiff) * t;
            a.hrtfW += (b.hrtfW - a.hrtfW) * t;
            for (int e = 0; e < 2; ++e) {
                a.earDelay[e] += (b.earDelay[e] - a.earDelay[e]) * t;
                for (int k = 0; k < kNumBands; ++k)
                    a.earGain[e][k] += (b.earGain[e][k] - a.earGain[e][k]) * t;
            }
        }
        lerping_ = false;
    }

    // HRTF バスに載っているタップがあるか。無ければ呼び出し側が HRTF 1 本ぶんを丸ごと省ける
    //   （HrtfProcessor は入力が 0 でも 139 タップ × 2 耳を毎サンプル畳むので、
    //     見通せている間ずっと空回しさせると +16% を払い続けることになる）。
    void updateHrtfBus() {
        hrtfBus_ = false;
        for (std::size_t i = 1; i < to_.size(); ++i) {          // index 0 は直接音なので見ない
            if (to_[i].hrtfW > 0.0f && !to_[i].silent()) { hrtfBus_ = true; return; }
            if (i < from_.size() && from_[i].hrtfW > 0.0f && !from_[i].silent()) {
                hrtfBus_ = true; return;
            }
        }
    }

    // RBJ バイカッド（直接形II転置）。
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void setLowpass(float fc, float fs) {
            const float w0 = 2.0f * 3.14159265358979323846f * fc / fs;
            const float cw = std::cos(w0), sw = std::sin(w0);
            const float alpha = sw / (2.0f * 0.70710678f);
            b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; b2 = (1.0f - cw) * 0.5f;
            const float a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
            b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
            z1 = 0.0f; z2 = 0.0f;
        }
        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static int nextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    std::size_t bandIndex(int band, int pos) const {
        return static_cast<std::size_t>(band) * (ringMask_ + 1) + static_cast<std::size_t>(pos);
    }

    const int sampleRate_;
    std::vector<float> ring_;      // [帯域][時間] の入力履歴
    int ringMask_ = 0;
    int writePos_ = 0;
    Biquad split_[kNumBands - 1];  // 直列クロスオーバー用の lowpass 群

    // 補間の両端。オーディオスレッドだけが触る。長さは常に等しい。
    std::vector<RtTap> from_, to_;
    std::vector<bool> matched_;    // beginBlock の対応付け用（確保を繰り返さないよう保持）
    std::atomic<TapSet*> pending_{nullptr};
    std::atomic<TapSet*> retired_{nullptr};
    bool lerping_ = false;
    bool hrtfBus_ = false;
    int lerpPos_ = 0, lerpLen_ = 1;
};

}  // namespace dsp
}  // namespace af
