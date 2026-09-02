/* worker_pool.h — 音源ごとのループを複数コアへ割るための最小のプール。
 *
 * ★何のためにあるか
 *   費用の内訳を測ると、音源 1 本あたりの遮蔽・回折（role1）が per-source の床を作る。
 *   遮蔽された音源はここで diffractionComposite が走るため、見通せる音源の 8 倍高い。
 *   この段は **音源ごとに自分の枠にしか書かない**ので、音源で割れば
 *   **各音源の計算順も丸めも変わらない ＝ 結果はビット一致**のまま並列にできる。
 *
 * ★設計で外せない 2 点（Unity のネイティブプラグインとして生きるため）
 *
 *   1. **プールはシーンが持ち、AF_SceneDestroy で join する。**
 *      静的オブジェクトのデストラクタで join してはいけない。DLL のアンロード中に
 *      スレッドを join するとローダロックで**エディタが固まる**（この手の定番事故）。
 *      Unity 側は OnDisable → Dispose → AF_SceneDestroy を必ず通るので、
 *      そこに乗せるのがいちばん確実。
 *
 *   2. **既定はスレッドなし（直列）。**ホストが明示的に要求したときだけ起こす。
 *      理由は 2 つ。検査ハーネスを直列に保てば「変えたつもりが無い変更」を
 *      厳密差分で判定でき続ける。もう 1 つは、Unity は既に全コアでジョブを回しており、
 *      黙って自前のスレッドを増やすと食い合って**他が遅くなる**ため。
 *
 * ★使い方
 *      WorkerPool pool(4);
 *      pool.parallelFor(n, [&](int i){ ... });   // 呼び出し元も働く。戻ったら全部終わっている
 *   size() == 1 のときはスレッドを 1 本も作らず、その場で回すだけ（分岐 1 個ぶんの代金）。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace acoustic {

class WorkerPool {
public:
    // workers <= 1 ならスレッドを作らない（＝直列。既定はこちら）。
    explicit WorkerPool(int workers)
        : total_(workers < 1 ? 1 : workers) {
        threads_.reserve(static_cast<size_t>(total_ - 1));
        for (int i = 1; i < total_; ++i)
            threads_.emplace_back([this] { workerLoop(); });
    }

    ~WorkerPool() { shutdown(); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // 走っている仕事が無いときに呼ぶこと（所有者は 1 スレッドなので自然に満たされる）。
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            ++gen_;                 // 待っているワーカーを起こす
        }
        cv_.notify_all();
        for (std::thread& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
        total_ = 1;
    }

    int size() const { return total_; }

    // [0,n) を全ワーカーで食い合う。呼び出し元も 1 人として働く。
    //   仕事の取り方は atomic の先頭取りなので、音源ごとに費用が違っても偏らない
    //   （遮蔽された音源だけ 8 倍高いので、均等割りだと 1 人だけ残る）。
    void parallelFor(int n, const std::function<void(int)>& fn) {
        if (n <= 0) return;
        if (total_ <= 1 || n == 1) {
            for (int i = 0; i < n; ++i) fn(i);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &fn;
            n_ = n;
            next_.store(0, std::memory_order_relaxed);
            active_ = total_ - 1;   // 起こすワーカーの数
            ++gen_;
        }
        cv_.notify_all();
        drain();                    // 呼び出し元も働く
        {
            std::unique_lock<std::mutex> lk(m_);
            doneCv_.wait(lk, [this] { return active_ == 0; });
            job_ = nullptr;
        }
    }

private:
    void drain() {
        for (;;) {
            const int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= n_) return;
            (*job_)(i);
        }
    }

    void workerLoop() {
        unsigned long long seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this, &seen] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
            }
            drain();
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--active_ == 0) doneCv_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_;      // 仕事が来た合図
    std::condition_variable doneCv_;  // 全員終わった合図
    const std::function<void(int)>* job_ = nullptr;
    std::atomic<int> next_{0};
    int n_ = 0;
    int active_ = 0;
    unsigned long long gen_ = 0;      // 仕事の世代。取りこぼしを防ぐ
    bool stop_ = false;
    int total_ = 1;
};

}  // namespace acoustic
