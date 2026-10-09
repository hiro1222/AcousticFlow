// audio_device.h — 既定の出力デバイスへ実時間で音を出す。
//
// ★なぜ WASAPI を直接叩くか
//   ミドルウェアとしての価値は音響と道具に置く、と決めてあります。再生は
//   **試聴に足りるだけ**でよく、ミキサもボイス管理も作りません。
//   WASAPI 共有モードなら OS だけで足りて、外部ライブラリが 0 本のままです
//   （エンジン側の「外部ライブラリなし」という主張を道具側でも崩さない）。
//
// ⚠ 排他モードにはしていません。試聴のたびに他のアプリの音が止まると、
//   資料を見ながら聴く・動画を録るといった作業ができなくなるためです。

#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace af {

class BlockSource;

class AudioDevice {
public:
    AudioDevice() = default;
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // ★デバイスを開かずに、既定の出力の形式だけを見る。
    //
    //   音源は周波数を知らないと作れませんが、周波数はデバイスを開かないと分かりません。
    //   開いてから音源を作り直すと、**実時間スレッドが触っている物を差し替える**ことに
    //   なります（1 回きりでも競合は競合で、出るときは音で出る）。
    //   なので「見るだけ」を分けました。
    static bool probeDefaultFormat(int* sampleRate, int* channels, std::string* err);

    // 既定の出力デバイスを開いて実時間スレッドを起こす。
    bool start(BlockSource* source, std::string* err);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }

    // ── 開いたデバイスの素性（画面に出す用）
    const std::string& deviceName() const { return deviceName_; }
    int  sampleRate() const { return sampleRate_; }
    int  channels()   const { return channels_; }
    // デバイスが確保している輪の大きさ。
    int  bufferFrames() const { return bufferFrames_; }

    // ★1 回の呼び出しで実際に渡される数。**輪の大きさとは違う。**
    //   共有モードはデバイス周期ごとに起こしてくるので、輪 970 に対して
    //   1 回あたりは 441 前後しか空かない。持ち時間はこちらで決まる。
    int  framesPerCallback() const { return lastFrames_.load(std::memory_order_relaxed); }

    // ── 実測値。実時間スレッドが書いて UI スレッドが読む
    unsigned long long callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    unsigned long long lateBlocks() const { return late_.load(std::memory_order_relaxed); }
    double worstBlockMs() const { return worstMs_.load(std::memory_order_relaxed); }
    double lastBlockMs()  const { return lastMs_.load(std::memory_order_relaxed); }

    // 持ち時間に対して詰めるのに使った割合の最悪値。
    // ★実時間スレッド側で、そのブロック自身の持ち時間に対して出している。
    //   輪の大きさで割ると 2 倍以上甘い数字が出る（最初そうなっていた）。
    double worstLoadPercent() const { return worstLoad_.load(std::memory_order_relaxed); }

    void resetStats();

    // ── 実時間スレッドが画面のために公開するもの
    //
    // ★ロックは取りません。順番号を「奇数 → 書く → 偶数」で回し、
    //   読み手は前後の番号が一致したときだけ採用します（seqlock）。
    //   取りこぼしても 1 フレーム前の絵を出せばよいだけなので、**待つ理由が無い**。
    //   実時間スレッドで確保しない・待たない、を守るための形です。
    static constexpr int kScopeMax = 2048;

    // 直近ブロックの波形（左チャンネル）を写す。戻り値は取れた数。0 = 書き込み中。
    int copyScope(float* dst, int cap) const;

    // 直近ブロックの山（絶対値の最大）。
    float peak() const { return peak_.load(std::memory_order_relaxed); }

private:
    void threadMain();

    BlockSource* source_ = nullptr;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};

    std::string deviceName_ = "（未取得）";
    int sampleRate_   = 0;
    int channels_     = 0;
    int bufferFrames_ = 0;

    mutable std::atomic<unsigned> scopeSeq_{0};
    float                         scope_[kScopeMax] = {};
    std::atomic<int>              scopeCount_{0};
    std::atomic<float>            peak_{0.0f};

    std::atomic<unsigned long long> callbacks_{0};
    std::atomic<unsigned long long> late_{0};
    std::atomic<double> worstMs_{0.0};
    std::atomic<double> lastMs_{0.0};
    std::atomic<double> worstLoad_{0.0};
    std::atomic<int>    lastFrames_{0};

    // 起動時の失敗をスレッドから持ち帰るための置き場。
    std::string startError_;
    void*       startedEvent_ = nullptr;   // HANDLE
    std::atomic<bool> startOk_{false};
};

}  // namespace af
