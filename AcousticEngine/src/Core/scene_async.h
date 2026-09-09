// scene_async.h ── 更新の非同期化（2026-09-04）。主スレッドの費用を音響の計算時間から切り離す。
//
//   何を解くか: AF_SceneUpdate が同期だと、主スレッドが 1 フレームに 5〜10 ms を音響に払う。
//   ゲームは音響に 1 コアの 5〜10% しか渡せないので、解くのを別スレッドへ移し、主スレッドは
//   「入力を渡す・答えを読む」だけにする。
//
//   形（決めごと #1: 同じ問いに 2 つの答えを持たせない ── 規則はここに 1 つ、口は AF_* の分類だけ）:
//     ・設定（SetListener / SetSource / UpdateInstance / SetUpdateConfig …）は**待ち行列**へ。
//       着手のとき（主スレッド、仕事が走っていない瞬間）に順に適用する。呼び手の意味は変わらない。
//     ・結果（GetSourceOcclusion / GetEarlyReflections / GetEchogramBands / 段 / 代表 …）は**写し**から読む。
//       写しは仕事が終わった次の AF_SceneUpdate で取り替える。答えは 1 フレーム前の入力に対する物
//       （出力の平滑 0.35 s より十分短い）。
//     ・幾何の問い合わせ（ComputeSoftOcclusion / RoomAt / DiffractionPath …）は**共有ロック**で走る。
//       仕事は「作り直し（排他）→ 解く（共有）」の 2 段で、解く段は幾何を読むだけなので問い合わせと並走できる。
//       作り直し（BVH・部屋グラフ・自動ポータル・エッジカタログ・面の焼き）の間だけ待ち合う。
//     ・構築・焼き・書き出し・キャプチャ操作など稀な口は**同期**: 仕事を待ち、待ち行列を反映してから排他で実行。
//   ★仕事の計算の段が幾何を書かない前提は、複数コアで音源を割るときと同じ規約
//     （「音源ごとに自分の枠にしか書かない」。worker_pool.h）。遅延構築（ensureBvh / roomGraph）は
//     prepare で済ませ、compute の中では dirty が立たない（設定は待ち行列なので、走っている間に幾何は変わらない）。
//   ★ロックは**書き手優先**（RwLock）。std::shared_mutex で組んだら、主スレッドが問い合わせを切れ目なく叩くと
//     作り直しの排他が入れず、120 フレームで仕事が 1 つも終わらなかった（実測: 追いつかず 119/120）。
//     書き手が待っている間は新しい読み手を止める。
//   ★AF_SceneSetAsync(0) と破棄は仕事を待ってから畳む。DLL のアンロード中に join しないよう、
//     ホストはシーンを破棄してから終わること（プールと同じ理由）。
//   ⚠ 仕事が 1 フレームに収まらないと、そのフレームの AF_SceneUpdate は何もせず（入力は溜まる）、
//     答えは古いまま。何フレーム古いかは AF_SceneGetUpdateStats で読める。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "scene.h"

namespace acoustic {

/// 書き手優先の読み書きロック。読み手（問い合わせ・解く段）は並走、書き手（作り直し・同期の口・待ち行列の適用）は独占。
///   書き手が待っている間は新しい読み手を入れない（飢えさせない）。
class RwLock {
public:
    void lockShared() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !writer_ && writersWaiting_ == 0; });
        ++readers_;
    }
    void unlockShared() {
        std::lock_guard<std::mutex> lk(m_);
        if (--readers_ == 0) cv_.notify_all();
    }
    void lock() {
        std::unique_lock<std::mutex> lk(m_);
        ++writersWaiting_;
        cv_.wait(lk, [&] { return !writer_ && readers_ == 0; });
        --writersWaiting_;
        writer_ = true;
    }
    void unlock() {
        std::lock_guard<std::mutex> lk(m_);
        writer_ = false;
        cv_.notify_all();
    }
    /// RAII。既定構築は「持っていない」。移動できる。
    class Read {
    public:
        Read() = default;
        explicit Read(RwLock& l) : l_(&l) { l_->lockShared(); }
        ~Read() { if (l_) l_->unlockShared(); }
        Read(Read&& o) noexcept : l_(o.l_) { o.l_ = nullptr; }
        Read& operator=(Read&& o) noexcept { if (this != &o) { if (l_) l_->unlockShared(); l_ = o.l_; o.l_ = nullptr; } return *this; }
        Read(const Read&) = delete; Read& operator=(const Read&) = delete;
    private:
        RwLock* l_ = nullptr;
    };
    class Write {
    public:
        Write() = default;
        explicit Write(RwLock& l) : l_(&l) { l_->lock(); }
        ~Write() { if (l_) l_->unlock(); }
        Write(Write&& o) noexcept : l_(o.l_) { o.l_ = nullptr; }
        Write& operator=(Write&& o) noexcept { if (this != &o) { if (l_) l_->unlock(); l_ = o.l_; o.l_ = nullptr; } return *this; }
        Write(const Write&) = delete; Write& operator=(const Write&) = delete;
    private:
        RwLock* l_ = nullptr;
    };
private:
    std::mutex m_;
    std::condition_variable cv_;
    int readers_ = 0;
    int writersWaiting_ = 0;
    bool writer_ = false;
};

class SceneBox {
public:
    Scene scene;
    // 閉じる印。AF_SceneDestroy が立てる。オーディオスレッドから来る AF_SceneCapturePushAudio はこれを見て帰る。
    //   器そのものは AF_SceneDestroy の後も 2 秒残す（scene_api.cpp の墓地）── Play を止めた瞬間に
    //   まだ走っているオーディオの呼び出しが解放済みの器へ触らないように。
    std::atomic<bool> closing{false};

    SceneBox() = default;
    ~SceneBox() { setAsync(false); }
    SceneBox(const SceneBox&) = delete;
    SceneBox& operator=(const SceneBox&) = delete;

    bool async() const { return async_; }

    void setAsync(bool on) {
        if (on == async_) return;
        if (on) {
            {
                std::lock_guard<std::mutex> lk(jobMx_);
                quit_ = false; jobPending_ = false; running_ = false; jobDone_ = false;
            }
            worker_ = std::thread([this] { workerLoop_(); });
            async_ = true;
            return;
        }
        waitJob();
        {
            std::lock_guard<std::mutex> lk(jobMx_);
            quit_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        applyQueue_();     // 残りの設定を反映（同期に戻るので即時）
        publishIfDone_();
        async_ = false;
    }

    /// 毎フレーム 1 発。
    ///   同期: そのまま解く。
    ///   非同期: 前の仕事が終わっていれば写しを取り替え、待ち行列を適用して次を投げる。終わっていなければ何もしない。
    void update(float dt) {
        if (!async_) { scene.update(dt); return; }
        ++submitted_;
        {
            std::lock_guard<std::mutex> lk(jobMx_);
            if (running_ || jobPending_) { ++skipped_; return; }
        }
        publishIfDone_();
        applyQueue_();
        {
            std::lock_guard<std::mutex> lk(jobMx_);
            jobDt_ = dt;
            jobSubmitFrame_ = submitted_;
            jobPending_ = true;
        }
        cv_.notify_all();
    }

    /// 走っている仕事を待ち、答えを写す（検査・「このフレームの答えが要る」ホスト用）。同期なら何もしない。
    void wait() {
        if (!async_) return;
        waitJob();
        publishIfDone_();
    }

    /// 設定: 非同期なら待ち行列へ、同期なら即時。
    template <class F>
    void post(F&& f) {
        if (!async_) { f(scene); return; }
        std::lock_guard<std::mutex> lk(queueMx_);
        queue_.emplace_back(std::forward<F>(f));
    }

    /// 結果の写し。非同期なら写し（主スレッドが持つ）、同期なら null（Scene 自身の結果を読む）。
    const Scene::Results* snapshot() const { return async_ ? &front_ : nullptr; }

    /// 統計: 直近の仕事の計算時間(ms)、写しの古さ（フレーム）、追いつかなかったフレーム数、待ち行列の長さ。
    void stats(float* outComputeMs, int* outLagFrames, int* outSkipped, int* outQueued) const {
        if (outComputeMs) *outComputeMs = lastMs_.load(std::memory_order_relaxed);
        if (outLagFrames) *outLagFrames = async_ ? std::max(0, submitted_ - frontFrame_) : 0;
        if (outSkipped)   *outSkipped = skipped_;
        if (outQueued) {
            std::lock_guard<std::mutex> lk(queueMx_);
            *outQueued = static_cast<int>(queue_.size());
        }
    }

    // ── 口の分類に使う番人（scene_api.cpp）──
    /// 同期の口: 仕事を待ち、待ち行列を反映してから排他で入る。同期モードなら素通し。
    struct SyncGuard {
        explicit SyncGuard(SceneBox* b) : box(b) {
            if (!box || !box->async_) return;
            box->waitJob();
            box->applyQueue_();
            lk = RwLock::Write(box->geom_);
        }
        Scene* get() const { return box ? &box->scene : nullptr; }
        SceneBox* box;
        RwLock::Write lk;
    };
    /// 幾何の問い合わせ: 共有ロックで入る（解く段と並走、作り直しの段とは待ち合う）。同期モードなら素通し。
    struct ReadGuard {
        explicit ReadGuard(SceneBox* b) : box(b) {
            if (!box || !box->async_) return;
            lk = RwLock::Read(box->geom_);
        }
        Scene* get() const { return box ? &box->scene : nullptr; }
        SceneBox* box;
        RwLock::Read lk;
    };

private:
    void workerLoop_() {
        using clk = std::chrono::steady_clock;
        for (;;) {
            float dt = 0.0f;
            {
                std::unique_lock<std::mutex> lk(jobMx_);
                cv_.wait(lk, [&] { return jobPending_ || quit_; });
                if (quit_) return;
                jobPending_ = false;
                running_ = true;
                dt = jobDt_;
            }
            const auto t0 = clk::now();
            {
                RwLock::Write ex(geom_);      // 作り直し: 排他（書き手優先なので問い合わせの切れ目を待たない）
                scene.updatePrepare();
            }
            {
                RwLock::Read sh(geom_);       // 解く: 共有（問い合わせと並走）
                scene.updateCompute(dt);
            }
            lastMs_.store(static_cast<float>(std::chrono::duration<double, std::milli>(clk::now() - t0).count()),
                          std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(jobMx_);
                running_ = false;
                jobDone_ = true;
                doneFrame_ = jobSubmitFrame_;
            }
            cv_.notify_all();
        }
    }

    void waitJob() {
        std::unique_lock<std::mutex> lk(jobMx_);
        cv_.wait(lk, [&] { return !running_ && !jobPending_; });
    }

    // 仕事が走っていないときだけ呼ぶこと（update / wait / setAsync(false) から）。
    void publishIfDone_() {
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(jobMx_);
            done = jobDone_;
            jobDone_ = false;
        }
        if (!done) return;
        front_ = scene.results();        // 写し（数十 KB。主スレッドで 10 µs の桁）
        frontFrame_ = doneFrame_;
    }

    // 仕事が走っていないときだけ呼ぶこと。
    void applyQueue_() {
        std::vector<std::function<void(Scene&)>> q;
        {
            std::lock_guard<std::mutex> lk(queueMx_);
            q.swap(queue_);
        }
        if (q.empty()) return;
        RwLock::Write ex(geom_);
        for (auto& f : q) f(scene);
    }

    bool async_ = false;
    std::thread worker_;
    std::mutex jobMx_;
    std::condition_variable cv_;
    bool jobPending_ = false, running_ = false, jobDone_ = false, quit_ = false;
    float jobDt_ = 0.0f;
    int jobSubmitFrame_ = 0, doneFrame_ = 0;

    mutable std::mutex queueMx_;
    std::vector<std::function<void(Scene&)>> queue_;

    RwLock geom_;                        // 幾何: 作り直しと同期の口は排他、解く段と問い合わせは共有（書き手優先）

    Scene::Results front_;               // 主スレッドが読む写し
    int frontFrame_ = 0;                 // 写しの入力のフレーム番号
    int submitted_ = 0;                  // update の通し番号（主スレッド）
    int skipped_ = 0;                    // 仕事が終わらず投げられなかったフレーム
    std::atomic<float> lastMs_{0.0f};
};

}  // namespace acoustic
