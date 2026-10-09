// block_source.h — ホストが音を受け取る唯一の口。
//
// ★ここがミドルウェアとシステムの境目。
//
//   ホストは「誰が鳴らしているか」を知りません。このファイルにも、この
//   プロジェクトのどのファイルにも、音響エンジン（AcousticFlow）は出てきません。
//   `Middleware/` は AcousticEngine にリンクしていない ── 意図的にそうしてあります。
//
//   エンジンを載せるときは、読み込んだ DLL を呼ぶ BlockSource を 1 つ足すだけです。
//   ホスト・デバイス出力・実時間スレッドの側は一切変わりません。
//
// ⚠ render() は**実時間スレッドから**呼ばれます。ここで確保しない・待たない・
//   ログを吐かない。守れないものは実時間スレッドに置かない、が唯一の規律です。

#pragma once

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace af {

// 実時間スレッドが 1 ブロックぶんの音を要求する口。
class BlockSource {
public:
    virtual ~BlockSource() = default;

    // interleaved に frames × channels サンプルを書く。
    // ★必ず全部書くこと（前のブロックの残骸が残ると耳に出る）。
    virtual void render(float* interleaved, int frames, int channels) = 0;

    // 画面に出す名前。実時間スレッドからは呼ばない。
    virtual const char* name() const = 0;

    // 出力を止める／再開する。実時間スレッドの外から呼ばれる。
    // ★即座に 0 にしない ── 途中で切ると必ずプツッと鳴る。実装側でなだらかに。
    virtual void setActive(bool on) = 0;
};

// 何も鳴らさない。デバイスが開いているだけの状態。
class SilenceSource : public BlockSource {
public:
    void render(float* out, int frames, int channels) override {
        std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames) * channels);
    }
    const char* name() const override { return "無音"; }
    void setActive(bool) override {}
};

// 試験信号。デバイスまでの経路が生きていることを耳で確かめるためのもの。
//
// ★なぜ単純な正弦波ではなく、立ち上がりに傾斜を付けるのか
//   この作品の第一制約は「不連続は間違いとして即座に聞こえる」です。
//   道具の側が自分でプツッと鳴らしていたら、エンジンの不連続と区別が付きません。
//   ここで 20ms の傾斜を掛けているのは、**道具由来の不連続を 0 にする**ためです。
class ToneSource : public BlockSource {
public:
    explicit ToneSource(double sampleRate, double hz = 440.0, float peak = 0.1f)
        : sr_(sampleRate), hz_(hz), peak_(peak) {
        // 20ms で 0 ↔ 1 を渡り切る 1 サンプルあたりの歩幅。
        step_ = static_cast<float>(1.0 / (0.020 * sampleRate));
    }

    // ★周波数と大きさは画面から動かせる。実時間スレッドが読むので atomic。
    //   ブロックの頭で 1 回読んで、その中では固定して使う
    //   （途中で変えると位相が飛んで、それ自体が不連続になる）。
    void setFrequency(double hz) { hz_.store(hz, std::memory_order_relaxed); }
    void setLevel(float peak)    { peak_.store(peak, std::memory_order_relaxed); }
    double frequency() const     { return hz_.load(std::memory_order_relaxed); }
    float  level() const         { return peak_.load(std::memory_order_relaxed); }

    void render(float* out, int frames, int channels) override {
        const double inc  = 6.283185307179586 * hz_.load(std::memory_order_relaxed) / sr_;
        const float  peak = peak_.load(std::memory_order_relaxed);
        const float target = active_ ? 1.0f : 0.0f;

        for (int i = 0; i < frames; ++i) {
            // 傾斜を 1 サンプルずつ目標へ寄せる。到達したら動かない。
            if (gain_ < target)      gain_ = (gain_ + step_ > target) ? target : gain_ + step_;
            else if (gain_ > target) gain_ = (gain_ - step_ < target) ? target : gain_ - step_;

            const float v = static_cast<float>(std::sin(phase_)) * peak * gain_;
            phase_ += inc;
            if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;

            for (int c = 0; c < channels; ++c) out[i * channels + c] = v;
        }
    }

    const char* name() const override { return "試験信号 440Hz"; }
    void setActive(bool on) override { active_ = on; }

    // 傾斜が下り切ったか（止めたあと本当に無音になったか）。
    bool settled() const { return gain_ == (active_ ? 1.0f : 0.0f); }

private:
    double              sr_;
    std::atomic<double> hz_;
    std::atomic<float>  peak_;
    float               step_;
    double              phase_ = 0.0;
    float               gain_  = 0.0f;

    // 実時間スレッドと UI スレッドで触るので atomic 相当の扱いが要るが、
    // bool 1 個の読み書きは x86 で分割されない。ここは素の bool で足りる。
    volatile bool active_ = false;
};

// 取り込んだ音を 1 本鳴らす。SoundLibrary の中身を耳で確かめるためのもの。
//
// ★1 本ぶんです。同時に数本鳴らすのは `voice_bus.h` が束ねます。
//   ⚠ これが 1 本しか無かった頃、動作を 2 つ持つイベントは
//     最後の 1 つしか鳴っていませんでした。
//
// ★持つのは**鳴らし方**だけ（音量・ピッチ・低域通過・高域通過・ループ）。
//   素材そのものは書き換えません。焼いた資産にこの値が乗るかどうかは、
//   焼く道具を作る段の別の決めごとです。
//
// ⚠ 中身の差し替えは**デバイスを止めてから**行うこと。
//   実時間スレッドが読んでいる配列を入れ替えると競合します。
//   止めて・入れて・開け直す、が確実で、試聴の道具にはその 100ms が許されます。
//   （待たない・確保しない、を実時間スレッド側で守り切るための割り切り）
class ClipSource : public BlockSource {
public:
    enum class State { Stopped, Playing, Paused };

    // ★端＝素通り。つまみをここへ置いたらフィルタを掛けません。
    //   画面の目盛りもこの値で「切」と出します（同じ数を 2 箇所に書かない）。
    static constexpr float kLpOff = 20000.0f;   // 低い方を通す（上端＝切）
    static constexpr float kHpOff =    20.0f;   // 高い方を通す（下端＝切）

    // ★出入りの傾斜は 5ms。これより短いと不連続が残り、長いと「切った」感じが遅れる。
    //   奪取時のフェードとして一般に使われるのも 5〜20ms の範囲。
    static constexpr float kRampSeconds = 0.005f;
    // 音量を追いかける 1 極の時定数。5ms なら 95% までおよそ 15ms。
    static constexpr float kGainTau     = 0.005f;

    // デバイスに合わせて並べ直した済みのものを渡す（インターリーブ）。
    // ⚠ これだけは**デバイスを止めてから**呼ぶこと（配列ごと入れ替えるため）。
    void setClip(std::vector<float>&& interleaved, int channels, int sampleRate) {
        clip_     = std::move(interleaved);
        channels_ = channels;
        rate_     = sampleRate;
        posF_     = 0.0;
        posPub_.store(0, std::memory_order_relaxed);
        resetFilters();
        seekTo_.store(-1, std::memory_order_relaxed);
        state_.store(State::Stopped, std::memory_order_relaxed);
        rewindReq_.store(false, std::memory_order_relaxed);
        envGain_ = 0.0f;
        gainCur_ = gain_.load(std::memory_order_relaxed);

        // ★傾斜の速さは**周波数から出す**。固定の歩幅にすると 44.1k と 48k で
        //   掛かる時間が変わり、「5ms のはず」が説明できなくなります。
        const float sr = rate_ > 0 ? static_cast<float>(rate_) : 44100.0f;
        rampStep_ = 1.0f / (sr * kRampSeconds);
        // 1 極の係数。時定数 5ms 相当（1 - exp(-1/(τ·fs)) の近似で 1/(τ·fs)）。
        gainCoef_ = 1.0f / (sr * kGainTau);
        if (gainCoef_ > 1.0f) gainCoef_ = 1.0f;
    }
    void render(float* out, int frames, int channels) override {
        // ★頭出しは「注文」として受け取る。
        //   画面から pos_ を直に書くと、実時間スレッドが進めている最中と衝突します。
        //   ここで 1 回だけ引き取って、実時間スレッドの側で動かす。
        const long long sk = seekTo_.exchange(-1, std::memory_order_relaxed);
        if (sk >= 0) {
            double p = static_cast<double>(sk);
            const double last = static_cast<double>(frameCount());
            if (p < 0.0)  p = 0.0;
            if (p > last) p = last;
            posF_ = p;
            resetFilters();          // 飛んだ先の音でフィルタを鳴らし直す（余韻を引きずらない）
        }

        // ★「鳴らしたい状態か」と「いま音が出ているか」を分けます。
        //   止めた直後も**傾斜が下り切るまでは音が出ている**ので、
        //   state だけで切ると波形の途中で 0 になり、必ずプチッと鳴ります。
        const State  st   = state_.load(std::memory_order_relaxed);
        const bool   want = (st == State::Playing);
        const bool   alive = (want || envGain_ > 0.0f) &&
                             channels_ == channels && !clip_.empty();
        const float  gTarget = gain_.load(std::memory_order_relaxed);
        const bool   loop = loop_.load(std::memory_order_relaxed);
        const double step = static_cast<double>(rateMul_.load(std::memory_order_relaxed));
        const double n    = static_cast<double>(frameCount());

        // ★係数はブロックの頭で 1 回だけ作り直す。
        //   1 サンプルごとに作ると三角関数を毎回呼ぶことになります。
        //   ⚠ 確保もロックもしないので、実時間スレッドで作って構いません。
        updateFilters();

        const unsigned mute = muteMask_.load(std::memory_order_relaxed);

        for (int i = 0; i < frames; ++i) {
            if (alive && loop && posF_ >= n) posF_ -= n * std::floor(posF_ / n);

            // ── 出入りの傾斜（再生・一時停止・停止に共通）
            if (want) { envGain_ += rampStep_; if (envGain_ > 1.0f) envGain_ = 1.0f; }
            else      { envGain_ -= rampStep_; if (envGain_ < 0.0f) envGain_ = 0.0f; }

            // ── 音量は 1 極で追いかける。
            //   ★変化量に比例して動くので、**下げ幅が違っても掛かる時間がほぼ同じ**。
            //   一定歩幅にすると -60dB への移動だけ遅くなります。
            gainCur_ += (gTarget - gainCur_) * gainCoef_;

            const bool have = alive && posF_ >= 0.0 && posF_ < n;
            for (int c = 0; c < channels; ++c) {
                float v = have ? sampleAt(posF_, c) * gainCur_ : 0.0f;
                if (have) v = filterOne(v, c);

                // ★片耳ずつ黙らせる。⚠ 一気に 0 にすると弾けるので傾斜で落とす。
                //   ここは「聴き比べ」の道具なので、切り替えるたびに
                //   プチッと鳴ると比べたいものが聞こえません。
                if (c < kMaxCh) {
                    const float wantCh = (mute & (1u << c)) ? 0.0f : 1.0f;
                    float&      cur    = muteGain_[c];
                    if (cur < wantCh)      { cur += rampStep_; if (cur > wantCh) cur = wantCh; }
                    else if (cur > wantCh) { cur -= rampStep_; if (cur < wantCh) cur = wantCh; }
                    v *= cur;
                }
                out[i * channels + c] = v * envGain_;
            }
            // ⚠ 傾斜で降りている間も**読み進めます**。止めると、その 1 サンプルを
            //   引き伸ばした音が尾に付きます（一時停止の位置は 5ms ずれますが聞こえません）。
            if (have) posF_ += step;
        }

        // ★鳴り切ったら自分で止まる。画面が押されるのを待たない。
        //   ⚠ ここでは頭出しを戻しません。戻すと `finished()` が false に化けて、
        //     「鳴り切るまで回す」ような呼び方が**永久に終わらなく**なります。
        if (want && !loop && posF_ >= n) state_.store(State::Stopped, std::memory_order_relaxed);

        // ★止める注文は、傾斜が下り切ってから効かせます。
        //   ⚠ 先に頭出しを戻すと、**鳴らしながら位置だけ頭へ飛びます**。
        if (!want && envGain_ <= 0.0f &&
            rewindReq_.exchange(false, std::memory_order_relaxed)) {
            posF_ = 0.0;
            resetFilters();
        }

        posPub_.store(publishedPos(), std::memory_order_relaxed);
    }


    const char* name() const override { return "取り込んだ音"; }

    // BlockSource の口。押す／止めるだけの相手向け。
    void setActive(bool on) override { on ? play() : pause(); }

    // ── 操作盤（画面から呼ぶ）
    void play() {
        if (clip_.empty()) return;
        // 端まで行っていたら頭から。止めた所から鳴らし直せないと使いにくい。
        if (posPub_.load(std::memory_order_relaxed) >= frames())
            seekTo_.store(0, std::memory_order_relaxed);
        state_.store(State::Playing, std::memory_order_relaxed);
    }
    void pause() {
        if (state_.load(std::memory_order_relaxed) == State::Playing)
            state_.store(State::Paused, std::memory_order_relaxed);
    }
    void stop() {
        // ★ここでは state を変えるだけ。⚠ 頭出しを 0 に戻すのは**実時間スレッドが
        //   傾斜を下ろし切ってから**（先に戻すと、鳴らしながら位置だけ飛びます）。
        rewindReq_.store(true, std::memory_order_relaxed);
        state_.store(State::Stopped, std::memory_order_relaxed);
    }
    void seekSeconds(double s) {
        if (rate_ <= 0) return;
        long long f = static_cast<long long>(s * rate_);
        if (f < 0) f = 0;
        const long long last = static_cast<long long>(frames());
        if (f > last) f = last;
        seekTo_.store(f, std::memory_order_relaxed);
        // 止まっている間に動かしても、つまみが戻らないようにする。
        if (state_.load(std::memory_order_relaxed) != State::Playing)
            posPub_.store(static_cast<std::size_t>(f),
                          std::memory_order_relaxed);
    }

    // ── 試聴の調整（プロパティから触る）
    //
    // ★どれも「鳴らし方」です。素材そのものは書き換えません。
    //   値はプロジェクトに保存され、焼く段でどう扱うかは別の決めごとです。
    void setGainDb(float db) {
        gain_.store(db <= -59.9f ? 0.0f : std::pow(10.0f, db / 20.0f),
                    std::memory_order_relaxed);
    }
    void setLoop(bool on) { loop_.store(on, std::memory_order_relaxed); }
    bool loop() const { return loop_.load(std::memory_order_relaxed); }

    // ★ピッチは**読む速さ**を変えているだけです（時間も一緒に伸び縮みする）。
    //   長さを保ったまま高さだけ変えるには位相ボコーダが要りますが、
    //   試聴の道具には過剰で、しかも**素材の粗が隠れます**。
    //   半音 n → 速さ 2^(n/12)。
    void setPitchSemitones(float semi) {
        if (semi < -24.0f) semi = -24.0f;
        if (semi >  24.0f) semi =  24.0f;
        rateMul_.store(std::pow(2.0f, semi / 12.0f), std::memory_order_relaxed);
    }

    // 低い方を通す／高い方を通す。⚠ 端に置いたら**素通り**です（掛けない）。
    void setLowPassHz(float hz)  { lpHz_.store(hz, std::memory_order_relaxed); }
    void setHighPassHz(float hz) { hpHz_.store(hz, std::memory_order_relaxed); }

    // ★片耳ずつ黙らせる（0 = 左 / 1 = 右）。
    //   ⚠ これは**聴き比べのための道具**で、音源の設定ではありません。
    //     だからプロジェクトには保存しません。保存すると、
    //     片方を黙らせたまま閉じて、次に開いたとき原因の分からない片鳴りになります。
    void setChannelMute(int ch, bool mute) {
        if (ch < 0 || ch >= kMaxCh) return;
        unsigned m = muteMask_.load(std::memory_order_relaxed);
        if (mute) m |=  (1u << ch);
        else      m &= ~(1u << ch);
        muteMask_.store(m, std::memory_order_relaxed);
    }
    bool channelMuted(int ch) const {
        if (ch < 0 || ch >= kMaxCh) return false;
        return (muteMask_.load(std::memory_order_relaxed) & (1u << ch)) != 0;
    }
    void clearChannelMutes() { muteMask_.store(0, std::memory_order_relaxed); }

    // 傾斜も下り切って、本当に何も出していないか。
    // ★予備の枠を回収してよいかの判断に使います。
    //   ⚠ `state() == Stopped` だけでは足りません ── 止めた直後はまだ
    //     包絡が残っていて**音が出ています**。回収すると途中で切れます。
    bool silent() const { return envGain_ <= 0.0f; }

    // ── 状態（画面が読む）
    State state() const { return state_.load(std::memory_order_relaxed); }
    bool  empty() const { return clip_.empty(); }
    bool  finished() const {
        return clip_.empty() || posPub_.load(std::memory_order_relaxed) >= frames();
    }

    std::size_t frames() const {
        return channels_ > 0 ? clip_.size() / static_cast<std::size_t>(channels_) : 0;
    }
    double durationSeconds() const {
        return rate_ > 0 ? static_cast<double>(frames()) / rate_ : 0.0;
    }
    double positionSeconds() const {
        if (rate_ <= 0 || channels_ <= 0) return 0.0;
        return static_cast<double>(posPub_.load(std::memory_order_relaxed)) / rate_;
    }
    double remainSeconds() const {
        const double r = durationSeconds() - positionSeconds();
        return r > 0.0 ? r : 0.0;
    }
    float progress() const {
        const double d = durationSeconds();
        return d > 0.0 ? static_cast<float>(positionSeconds() / d) : 0.0f;
    }

private:
    // ── 内部の道具（実時間スレッドからしか触りません）

    std::size_t frameCount() const {
        return channels_ > 0 ? clip_.size() / static_cast<std::size_t>(channels_) : 0;
    }
    std::size_t publishedPos() const {
        const double n = static_cast<double>(frameCount());
        double p = posF_;
        if (p < 0.0) p = 0.0;
        if (p > n)   p = n;
        return static_cast<std::size_t>(p);
    }

    // ★線形補間で読む。ピッチを変えると読む位置が整数から外れるため。
    //   ⚠ 高くする（速く読む）ほうは折り返し歪みが出ます。試聴には十分ですが、
    //     焼く段で速さを変えるなら、そこはちゃんと帯域制限すること。
    float sampleAt(double p, int c) const {
        const std::size_t n = frameCount();
        if (n == 0) return 0.0f;
        const std::size_t i0 = static_cast<std::size_t>(p);
        if (i0 >= n) return 0.0f;
        const std::size_t i1 = (i0 + 1 < n) ? i0 + 1 : i0;
        const float t = static_cast<float>(p - static_cast<double>(i0));
        const std::size_t ch = static_cast<std::size_t>(channels_);
        const float a = clip_[i0 * ch + static_cast<std::size_t>(c)];
        const float b = clip_[i1 * ch + static_cast<std::size_t>(c)];
        return a + (b - a) * t;
    }

    // ── フィルタ（RBJ の双 2 次。低い方を通す／高い方を通す）
    //
    // ★係数はブロックの頭で作り直します。三角関数を呼びますが、
    //   **確保もロックもしない**ので実時間スレッドで構いません（値が変わった時だけ）。
    // ⚠ 端に置いたら素通りさせること。掛けっぱなしにすると、切っているつもりでも
    //   位相だけ回って、切り忘れに気づけません。
    static constexpr int kMaxCh = 8;
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        bool  on = false;
        float z1[kMaxCh] = {}, z2[kMaxCh] = {};
        void clear() {
            for (int i = 0; i < kMaxCh; ++i) { z1[i] = z2[i] = 0.0f; }
        }
        float run(float x, int c) {
            if (!on || c >= kMaxCh) return x;
            const float y = b0 * x + z1[c];
            z1[c] = b1 * x - a1 * y + z2[c];
            z2[c] = b2 * x - a2 * y;
            return y;
        }
    };

    void resetFilters() { lp_.clear(); hp_.clear(); }

    void updateFilters() {
        const float lp = lpHz_.load(std::memory_order_relaxed);
        const float hp = hpHz_.load(std::memory_order_relaxed);
        if (lp == lpApplied_ && hp == hpApplied_ && rate_ == rateApplied_) return;
        lpApplied_ = lp; hpApplied_ = hp; rateApplied_ = rate_;
        if (rate_ <= 0) return;

        const float nyq = static_cast<float>(rate_) * 0.5f;
        design(&lp_, lp, true,  nyq);
        design(&hp_, hp, false, nyq);
    }

    static void design(Biquad* f, float hz, bool lowPass, float nyq) {
        // 端＝素通り。低い方を通す側は上端、高い方を通す側は下端。
        if (( lowPass && hz >= kLpOff) || (!lowPass && hz <= kHpOff)) { f->on = false; return; }
        if (hz < 10.0f)        hz = 10.0f;
        if (hz > nyq * 0.98f)  hz = nyq * 0.98f;

        const float w0 = 6.2831853f * hz / (nyq * 2.0f);
        const float cw = std::cos(w0);
        const float sw = std::sin(w0);
        const float q  = 0.70710678f;            // バターワース（山を作らない）
        const float al = sw / (2.0f * q);

        float b0, b1, b2;
        if (lowPass) { b0 = (1 - cw) * 0.5f; b1 = 1 - cw;    b2 = b0; }
        else         { b0 = (1 + cw) * 0.5f; b1 = -(1 + cw); b2 = b0; }
        const float a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;

        f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0;
        f->a1 = a1 / a0; f->a2 = a2 / a0;
        // ★係数を替えたら中身を捨てる。前の係数で積んだ値をそのまま使うと弾けます。
        f->clear();
        f->on = true;
    }

    float filterOne(float x, int c) { return lp_.run(hp_.run(x, c), c); }

    std::vector<float> clip_;
    int                channels_ = 0;
    int                rate_     = 0;

    // posF_ は**実時間スレッドだけ**が進める。画面は posPub_（フレーム番号）を読む。
    // ★整数ではなく小数です。ピッチで読む速さが変わるため。
    double                   posF_ = 0.0;
    std::atomic<std::size_t> posPub_{0};
    std::atomic<long long>   seekTo_{-1};
    std::atomic<State>       state_{State::Stopped};
    std::atomic<float>       gain_{1.0f};
    std::atomic<bool>        loop_{false};
    std::atomic<float>       rateMul_{1.0f};       // 読む速さ（ピッチ）
    std::atomic<float>       lpHz_{kLpOff};
    std::atomic<float>       hpHz_{kHpOff};
    std::atomic<unsigned>    muteMask_{0};        // 1 ビット 1 チャンネル

    float  muteGain_[kMaxCh] = {1, 1, 1, 1, 1, 1, 1, 1};

    // 実時間スレッドだけが触る。画面からは見えない。
    float  envGain_  = 0.0f;    // 出入りの包絡（再生 1 / 止 0）
    float  gainCur_  = 1.0f;    // いま掛かっている音量（目標を追いかける）
    float  rampStep_ = 1.0f / 220.0f;
    float  gainCoef_ = 1.0f / 220.0f;
    std::atomic<bool> rewindReq_{false};   // 止めたあと頭へ戻す注文

    Biquad lp_, hp_;
    float  lpApplied_ = -1.0f, hpApplied_ = -1.0f;
    int    rateApplied_ = 0;
};

}  // namespace af
