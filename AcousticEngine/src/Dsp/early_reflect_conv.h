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
    // 方向バスへ書くときの ITD の上限（サンプル）。1 ms ≒ 48 本。行の末尾にこのぶんの余裕を持つ。
    static constexpr int kLaneItdMax = 64;

    // 1 タップ。audio thread で毎サンプル sqrt を呼ばないよう、構築時に畳んである。
    struct Tap {
        int delaySamples = 0;
        // ★素性。フレームをまたいで「同じタップ」を繋ぐための番号（呼び出し側が決める）。
        //   −1 なら従来どおり遅延の近さで貪欲に繋ぐ。
        //   直接・透過・回折・最初の虚像は 1 ms 以内に並ぶことがあり、遅延の許容 256 サンプル
        //   （5.3 ms）では区別できない。区別できないと、扉が開いて直接音が起き上がる瞬間に
        //   回折タップと入れ替わり、乗り換えの 30 ms だけ音が凹む（実測 −2.5 dB／55°）。
        int id = -1;
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
        // ── 方向バス（DirectionBus）行き ──
        //   隣り合う 2 レーンへ等パワーで振る。laneUse のタップは軽量両耳化もパンも通さず、レーンへ送るだけ。
        //   VoiceRenderer::setTaps がバスの本数から決める。バスが無ければ false のまま。
        bool  laneUse = false;
        int   lane[2] = {-1, -1};
        float laneW[2] = {0.0f, 0.0f};
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

        // ①素性のあるタップを先に繋ぐ。②残りだけを遅延の近さで繋ぐ。
        //   ①を先にしないと、素性のあるタップの相手を素性の無いタップに取られる。
        taken_.assign(static_cast<std::size_t>(nNew), false);
        for (int j = 0; j < nNew; ++j) {
            const Tap& nt = p->taps[static_cast<std::size_t>(j)];
            if (nt.id < 0) continue;
            for (int i = 0; i < nCur; ++i) {
                if (matched_[static_cast<std::size_t>(i)] || from_[static_cast<std::size_t>(i)].id != nt.id) continue;
                matched_[static_cast<std::size_t>(i)] = true;
                taken_[static_cast<std::size_t>(j)] = true;
                to_[static_cast<std::size_t>(i)] = RtTap::from(nt);
                break;
            }
        }
        for (int j = 0; j < nNew; ++j) {
            if (taken_[static_cast<std::size_t>(j)]) continue;
            const Tap& nt = p->taps[static_cast<std::size_t>(j)];
            int best = -1;
            float bestD = kMatchDelayTolerance;
            // 素性のあるタップは、素性の合う相手がいなければ**新規**。遅延で拾うと別物と繋がる。
            for (int i = 0; nt.id < 0 && i < nCur; ++i) {
                if (matched_[static_cast<std::size_t>(i)] || from_[static_cast<std::size_t>(i)].id >= 0) continue;
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
    ///   outDiffuseL/R : 拡散送り（呼び出し側の拡散器へ）。**左右別**。
    ///                   モノラル 1 本だと、左右のオールパスへ通しても両耳差が作れない。
    ///   outDirect   : 直接音タップのモノラル値（HRTF で両耳化する用）
    ///   outHrtfMono : hrtfWeight>0 のタップのモノラル和（**別方向の** HRTF で両耳化する用）
    void processSample(float x, bool hrtfActive,
                       float& outL, float& outR,
                       float& outDiffuseL, float& outDiffuseR, float& outDirect,
                       float& outHrtfMono,
                       float* outLanes = nullptr, int laneStride = 1) {
        // 6 帯域に分ける。低域を抜き取った残りを次へ送るので、足すと元に戻る。
        const int wp = writePos_ & ringMask_;
        float rest = x;
        for (int b = 0; b < kNumBands - 1; ++b) {
            const float lo = split_[b].process(rest);
            ring_[bandIndex(b, wp)] = lo;
            rest -= lo;
        }
        ring_[bandIndex(kNumBands - 1, wp)] = rest;

        outL = 0.0f; outR = 0.0f; outDirect = 0.0f; outHrtfMono = 0.0f;
        outDiffuseL = 0.0f; outDiffuseR = 0.0f;

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
            // ★拡散ぶんも**そのタップの方向へ**送る。
            //   以前はモノラル 1 本に足し込んでいたので、呼び出し側で左右のオールパスに
            //   通しても**左右のレベルが同じ**＝構造的に ILD が 0 だった。
            //   散乱は「その面から来る」音なので、方向を捨てる理由がない。
            //   実測（く字廊下・角の向こう）: 出力に占める割合は
            //     タップ（方向あり）57〜119% に対し 散乱（方向なし）100〜179%。
            //   方向を持たない成分のほうが大きく、回折を +12.9dB 上げても
            //   その上から被さって定位が消えていた。実測 |ILD| 0.2dB。
            //   左右別に送ってもオールパスは元から 2 本なので**コストは増えない**。
            const float dv = tv * gDiff;
            outDiffuseL += dv * (a.panL + (b.panL - a.panL) * t);
            outDiffuseR += dv * (a.panR + (b.panR - a.panR) * t);
            const float sp = tv * gSpec;

            // ── 軽量な両耳化 ──
            //   耳ごとに「遅延をずらして読み直し、帯域ゲインを変える」だけ。
            //   畳み込みは増えず、リングをもう一度読むぶんで済む。
            //   ★index 0（直接音）と HRTF バスに載せたタップはここを通さない。
            //     あちらはフル HRTF が担当なので、二重に方向を付けることになる。
            // ── 方向バス行き ──
            //   バスが付いているとき（outLanes != null）、方向のあるタップは隣り合う 2 方向へ等パワーで送る。
            //   ★ITD はタップ自身の物（earDelay）を「書き先の位置」で付ける。リングは 1 回しか読まない。
            //     耳ごとに読み直す書き方は、行が 16 本に散る書き込みと合わせて 1 音源 9% → 14% に増えた（実測）。
            //     遅い耳の行へは f + earDelay の位置に書く（呼び手は行の末尾に kLaneItdMax の余裕を持ち、次の
            //     チャンクへ持ち越す）。ILD とスペクトルは行の固定 HRIR が担当。
            //   from/to で方向が違えば from は (1−t)、to は t で両方へ送る（境をまたいでも連続）。HRTF バス行きは従来どおり。
            if (outLanes && k != 0 && (a.laneUse || b.laneUse)) {
                bool onHrtfBus = false;
                if (hrtfActive) { const float hw = a.hrtfW + (b.hrtfW - a.hrtfW) * t; onHrtfBus = hw > 0.0f; }
                if (!onHrtfBus) {
                    // ★ITD は**小数のまま**隣り合う 2 サンプルへ分けて書く。
                    //   丸めて 1 か所に書くと、方向が連続に動いて ed が半サンプルの境を跨ぐたびに
                    //   書き先が 1 サンプル跳び、そのタップの寄与が不連続になる ＝ ぷつぷつ。
                    //   虚像（ISM）を入れて方向つきのタップが 25 本になったら実機で聞こえた（段 9-b）。
                    //   読む側（バスを使わない earUse の経路）は元から補間していて、書く側だけ丸めていた。
                    //   1 サンプル ＝ 21 µs は ITD の弁別閾（10〜20 µs）より粗いので、分けるのは定位にも効く。
                    int off[2]; float fr[2];
                    for (int e = 0; e < 2; ++e) {
                        float ed = a.earDelay[e] + (b.earDelay[e] - a.earDelay[e]) * t;
                        ed = (ed < 0.0f) ? 0.0f : (ed > static_cast<float>(kLaneItdMax - 1) ? static_cast<float>(kLaneItdMax - 1) : ed);
                        off[e] = static_cast<int>(ed);
                        fr[e] = ed - static_cast<float>(off[e]);
                    }
                    auto scatter = [&](int lane, float gain) {
                        for (int e = 0; e < 2; ++e) {
                            float* row = outLanes + static_cast<std::size_t>(lane * 2 + e) * laneStride;
                            row[off[e]]     += gain * (1.0f - fr[e]);
                            row[off[e] + 1] += gain * fr[e];
                        }
                    };
                    if (a.laneUse && t < 1.0f)
                        for (int q = 0; q < 2; ++q)
                            if (a.lane[q] >= 0) scatter(a.lane[q], sp * a.laneW[q] * (1.0f - t));
                    if (b.laneUse)
                        for (int q = 0; q < 2; ++q)
                            if (b.lane[q] >= 0) scatter(b.lane[q], sp * b.laneW[q] * t);
                    continue;
                }
            }
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
                    float* outHrtfMono = nullptr, float* outDiffuseR = nullptr) {
        if (!input || !outL || !outR) return;
        beginBlock();
        for (int i = 0; i < n; ++i) {
            float l, r, dL, dR, dir, hm;
            processSample(input[inOffset + i], hrtfActive, l, r, dL, dR, dir, hm);
            outL[outOffset + i] += l * gain;
            outR[outOffset + i] += r * gain;
            // ★モノラルに畳む互換経路は置かない。panL²+panR²=1 の量を 1 本へ潰すと
            //   パンに依って power が変わる（ハード左のタップで 0.75 に落ちた）。
            //   拡散送りは左右そのまま受け取ること。
            if (outDiffuse) outDiffuse[i] = dL;
            if (outDiffuseR) outDiffuseR[i] = dR;
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
        int   id = -1;                        // 素性（Tap::id の写し）
        float g[kNumBands] = {0, 0, 0, 0, 0, 0};
        float panL = 0.70710678f, panR = 0.70710678f;
        float gSpec = 1.0f, gDiff = 0.0f;
        float hrtfW = 0.0f;
        bool  earUse = false;
        float earDelay[2] = {0.0f, 0.0f};
        float earGain[2][kNumBands] = {{1,1,1,1,1,1}, {1,1,1,1,1,1}};
        bool  laneUse = false;
        int   lane[2] = {-1, -1};
        float laneW[2] = {0.0f, 0.0f};

        static RtTap from(const Tap& t) {
            RtTap r;
            r.delay = static_cast<float>(t.delaySamples);
            r.id = t.id;
            for (int b = 0; b < kNumBands; ++b) r.g[b] = t.g[b];
            r.panL = t.panL; r.panR = t.panR;
            r.gSpec = t.gSpec; r.gDiff = t.gDiff;
            r.hrtfW = t.hrtfWeight;
            r.earUse = t.earUse;
            r.laneUse = t.laneUse;
            for (int e = 0; e < 2; ++e) { r.lane[e] = t.lane[e]; r.laneW[e] = t.laneW[e]; }
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
            // レーンが同じなら重みを補間。違えば to の組へ乗り換え、今の組で同じレーンに乗っていた分を足し込む。
            //   ★以前は「途中の写しは稀」として to の重み × t だけにしていた。受取面のタップは向きが毎フレーム少しずつ動き、
            //     差し替え（毎フレーム 10.7 ms）が補間（30 ms）の途中で来るので、途中の写しは普通に起きる。組がレーンの中心で
            //     (0,1) → (1,2) と変わるたびに、共有するレーン 1 の分（ほぼ全部）を捨てて t ≈ 0.36 倍から立ち上げ直し、
            //     タップの量が一瞬 1/3 に落ちていた（clicks の扉だけ、初期だけで −2.6 dB のくぼみ）。
            //   外れるレーンの分は捨てる。組が中心で変わるとき、その重みはほぼ 0 なので落ちる量は無視できる。
            if (a.laneUse && b.laneUse && a.lane[0] == b.lane[0] && a.lane[1] == b.lane[1]) {
                for (int e = 0; e < 2; ++e) a.laneW[e] += (b.laneW[e] - a.laneW[e]) * t;
            } else if (b.laneUse) {
                float nw[2] = {b.laneW[0] * t, b.laneW[1] * t};
                if (a.laneUse)
                    for (int q = 0; q < 2; ++q)
                        for (int e = 0; e < 2; ++e)
                            if (a.lane[e] >= 0 && a.lane[e] == b.lane[q]) nw[q] += a.laneW[e] * (1.0f - t);
                a.laneUse = true;
                for (int e = 0; e < 2; ++e) { a.lane[e] = b.lane[e]; a.laneW[e] = nw[e]; }
            }
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
    std::vector<bool> matched_;  // beginBlock の対応付け用（確保を繰り返さないよう保持）
    std::vector<bool> taken_;    // 同上。素性で繋がった新タップの印
    std::atomic<TapSet*> pending_{nullptr};
    std::atomic<TapSet*> retired_{nullptr};
    bool lerping_ = false;
    bool hrtfBus_ = false;
    int lerpPos_ = 0, lerpLen_ = 1;
};

}  // namespace dsp
}  // namespace af
