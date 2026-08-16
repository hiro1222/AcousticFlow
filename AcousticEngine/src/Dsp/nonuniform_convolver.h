// nonuniform_convolver.h — 非一様分割による畳み込み
//                          (Gardner 1995 "Efficient Convolution Without Input-Output Delay")
//
// なぜ必要か:
//   一様分割 overlap-save は構造上 1ブロック(B)ぶん遅れる。この遅延は「尾の開始時刻
//   （mixing time）」の中に隠す必要があるので、B < mixing time が制約になる。
//   mixing time ≒ √V(ms) なので、6畳(24m³)なら 4.9ms＝約235サンプル。
//   一様分割で B をそこまで小さくすると、1秒の尾に 750 パーティションも要って現実的でない。
//   その妥協として境目に下限を置いていたが、そのせいで「本来は拡散として扱うべき反射」を
//   少数の離散タップで表現することになり、狭い部屋でコムフィルタ（箱っぽく高い音）が立つ。
//     体積 625m³(約8.5m立方)未満は全部この制約に掛かる＝普通の部屋はほぼ全滅だった。
//
// 解決の要点:
//   各段は IR の一部分だけを担当する。段 k が担当するのは irOffset_k から始まる区間なので、
//   その段の出力はもともと irOffset_k サンプル遅れてよい。
//   一様分割器の持つ遅延 B_k は、この「もともと必要な遅れ」に吸収できる。
//     追加で入れる遅延 = irOffset_k + B_0 - B_k  （倍々＋各段2個なら常に非負）
//   結果、全体の遅延は最小ブロック B_0 だけで決まる（64サンプル = 1.3ms）。
//
//   48kHz / 1秒の尾での比較:
//     一様 B=1024 … 遅延 21.3ms / 47パーティション
//     非一様      … 遅延  1.3ms / 18パーティション
//   パーティションが減るぶんと小サイズFFTが増えるぶんが相殺し、
//   コストはほぼ同じで遅延だけ 1/16 になる。
//
// 実装方針:
//   PartitionedConvolver（一様分割・直接畳み込みと一致することを検証済み）を段ごとに持ち、
//   出力を遅延して足す。ゼロから書き直すより、テスト済みの部品を組み合わせる方が安全。
//
// C# 版からの変更点:
//   作業領域を**構築時に確保**し、処理長がそれを超えるときは分割して回す。
//   C# 版は audio thread で EnsureScratch が確保していた（実機で確保が起きうる書き方）。
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/NonUniformConvolver.cs。
#pragma once

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "partitioned_convolver.h"

namespace af {
namespace dsp {

class NonUniformConvolver {
public:
    /// irLength   : 畳み込む IR の長さ(サンプル)
    /// firstBlock : 最小ブロック。これが全体の遅延になる（小さいほど低遅延・高コスト）
    /// capBlock   : 最大ブロック。これ以上は大きくせず、残りを同サイズで埋める
    /// maxFrames  : 一度に処理する最大サンプル数。これを超える要求は内部で分割する
    NonUniformConvolver(int irLength, int channels,
                        int firstBlock = 64, int capBlock = 8192, int maxFrames = 4096)
        : channels_(std::max(1, channels)),
          maxFrames_(std::max(64, maxFrames)) {
        firstBlock = nextPow2(std::max(16, firstBlock));
        capBlock = nextPow2(std::max(firstBlock, capBlock));
        irLength = std::max(1, irLength);
        latency_ = firstBlock;

        // ── 分割スケジュールを組む ──
        // ブロックを倍々にしながら各段 2 パーティション。上限に達したら残りを埋める。
        //   各段2個にするのは「追加遅延 irOffset + B0 - B が非負」を満たすため。
        //   1個だと次の段のブロックが開始位置を超えてしまい、遅延を隠せない。
        int b = firstBlock;
        int offset = 0;
        while (offset < irLength) {
            const int remaining = irLength - offset;
            int parts = (b >= capBlock) ? ceilDiv(remaining, b) : 2;
            if (b < capBlock && b * parts >= remaining) parts = ceilDiv(remaining, b);
            parts = std::max(1, parts);

            Stage s;
            s.blockSize = b;
            s.numParts = parts;
            s.irOffset = offset;
            s.length = b * parts;
            s.extraDelay = offset + firstBlock - b;
            stages_.push_back(std::move(s));

            offset += b * parts;
            if (b < capBlock) b *= 2;
        }

        for (Stage& s : stages_) {
            s.conv.reset(new PartitionedConvolver(s.blockSize, s.numParts, channels_));
            // 追加遅延が負になるとスケジュールが破綻している（遅延を隠せない）。
            // 上の組み方では起きないが、パラメータを変えたときのために潰しておく。
            if (s.extraDelay < 0) s.extraDelay = 0;
        }

        // 集約リングは「最大の追加遅延 + 1回の処理長」を収められる大きさに。
        int maxDelay = 0;
        for (const Stage& s : stages_) maxDelay = std::max(maxDelay, s.extraDelay);
        const int ringSize = nextPow2(maxDelay + maxFrames_ + 1);
        ringMask_ = ringSize - 1;
        accRing_.assign(static_cast<std::size_t>(channels_) * ringSize, 0.0f);

        scratch_.assign(static_cast<std::size_t>(channels_) * maxFrames_, 0.0f);
        scratchPtr_.resize(static_cast<std::size_t>(channels_));

        // setIr 用の切り出しバッファ（制御スレッド専用）。最長の段に合わせて一度だけ確保。
        int maxStageLen = 1;
        for (const Stage& s : stages_) maxStageLen = std::max(maxStageLen, s.length);
        sliceBuf_.assign(static_cast<std::size_t>(channels_) * maxStageLen, 0.0f);
        slicePtr_.resize(static_cast<std::size_t>(channels_));
        sliceLen_.resize(static_cast<std::size_t>(channels_));
        maxStageLen_ = maxStageLen;
    }

    NonUniformConvolver(const NonUniformConvolver&) = delete;
    NonUniformConvolver& operator=(const NonUniformConvolver&) = delete;

    int latency() const { return latency_; }
    int stageCount() const { return static_cast<int>(stages_.size()); }
    bool hasIr() const { return hasIr_; }
    int totalPartitions() const {
        int n = 0;
        for (const Stage& s : stages_) n += s.numParts;
        return n;
    }

    /// IR を差し替える（制御スレッド）。ir[c] は irLen[c] サンプル。段ごとに切り出して渡す。
    void setIr(const float* const* ir, const int* irLen) {
        if (!ir || !irLen) return;
        for (Stage& s : stages_) {
            for (int c = 0; c < channels_; ++c) {
                float* dst = sliceBuf_.data() + static_cast<std::size_t>(c) * maxStageLen_;
                std::fill(dst, dst + s.length, 0.0f);
                const float* src = ir[c];
                if (src) {
                    const int n = std::min(s.length, std::max(0, irLen[c] - s.irOffset));
                    for (int i = 0; i < n; ++i) dst[i] = src[s.irOffset + i];
                }
                slicePtr_[static_cast<std::size_t>(c)] = dst;
                sliceLen_[static_cast<std::size_t>(c)] = s.length;
            }
            s.conv->setIr(slicePtr_.data(), sliceLen_.data());
        }
        hasIr_ = true;
    }

    /// オーディオスレッド：モノラル入力を畳み込み、outCh に**加算**する。
    void processAdd(const float* input, int inOffset, int n,
                    float* const* outCh, int outOffset, float gain) {
        if (!input || !outCh || n <= 0) return;
        // 作業領域を超える要求は分割する（audio thread では確保しない）。
        while (n > 0) {
            const int chunk = std::min(n, maxFrames_);
            processChunk(input, inOffset, chunk, outCh, outOffset, gain);
            inOffset += chunk;
            outOffset += chunk;
            n -= chunk;
        }
    }

    /// 分割の内訳を1行で返す（診断表示用）。
    std::string describeSchedule() const {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "遅延 %dsamp / 段 %d / 計 %d分割: ",
                      latency_, stageCount(), totalPartitions());
        std::string out(buf);
        for (const Stage& s : stages_) {
            std::snprintf(buf, sizeof(buf), "[%dx%d]", s.blockSize, s.numParts);
            out += buf;
        }
        return out;
    }

private:
    struct Stage {
        std::unique_ptr<PartitionedConvolver> conv;
        int irOffset = 0;     // 担当する IR の開始位置(サンプル)
        int length = 0;       // 担当する長さ
        int extraDelay = 0;   // 出力に足す遅延 = irOffset + B0 - blockSize
        int blockSize = 0;
        int numParts = 0;
    };

    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    static int ceilDiv(int a, int b) { return (a + b - 1) / b; }

    void processChunk(const float* input, int inOffset, int n,
                      float* const* outCh, int outOffset, float gain) {
        const int ringSize = ringMask_ + 1;

        // 各段を回し、それぞれの追加遅延ぶんずらして集約リングへ足す。
        for (Stage& s : stages_) {
            for (int c = 0; c < channels_; ++c) {
                float* sc = scratch_.data() + static_cast<std::size_t>(c) * maxFrames_;
                std::fill(sc, sc + n, 0.0f);
                scratchPtr_[static_cast<std::size_t>(c)] = sc;
            }
            s.conv->processAdd(input, inOffset, n, scratchPtr_.data(), 0, 1.0f);

            for (int c = 0; c < channels_; ++c) {
                float* ring = accRing_.data() + static_cast<std::size_t>(c) * ringSize;
                const float* sc = scratchPtr_[static_cast<std::size_t>(c)];
                const int d = s.extraDelay;
                for (int i = 0; i < n; ++i) ring[(readPos_ + i + d) & ringMask_] += sc[i];
            }
        }

        // 集約リングから読み出して出力へ。読んだ場所はゼロに戻す
        // （次にこの位置へ書かれるまでに必ずクリアされている必要がある）。
        for (int c = 0; c < channels_; ++c) {
            float* ring = accRing_.data() + static_cast<std::size_t>(c) * ringSize;
            float* dst = outCh[c];
            for (int i = 0; i < n; ++i) {
                const int p = (readPos_ + i) & ringMask_;
                dst[outOffset + i] += ring[p] * gain;
                ring[p] = 0.0f;
            }
        }
        readPos_ = (readPos_ + n) & ringMask_;
    }

    const int channels_;
    const int maxFrames_;
    int latency_ = 0;
    std::vector<Stage> stages_;

    // 段の出力を集める共有リング。読み出したらゼロに戻す。[channel][ringSize]
    std::vector<float> accRing_;
    int ringMask_ = 0;
    int readPos_ = 0;

    std::vector<float> scratch_;            // [channel][maxFrames]
    std::vector<float*> scratchPtr_;

    std::vector<float> sliceBuf_;           // 制御スレッド専用 [channel][maxStageLen]
    std::vector<const float*> slicePtr_;
    std::vector<int> sliceLen_;
    int maxStageLen_ = 1;

    bool hasIr_ = false;
};

}  // namespace dsp
}  // namespace af
