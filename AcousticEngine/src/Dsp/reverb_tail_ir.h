// reverb_tail_ir.h — 実測エコグラム（帯域別）から後期残響の尾 IR を組み立てる。
//
// 考え方:
//   エコグラムのビンは「その時刻に届くエネルギー」＝尾のエネルギー包絡そのもの。
//   後期残響の波形は知覚的にはノイズなので、
//       尾IR(t) = ノイズ(t) × √(エネルギー包絡(t))
//   で作れる（エネルギー→振幅なので √）。帯域ごとに包絡が違うので、
//   1 本のノイズを 6 帯域に分けてから各々の包絡を掛け、足し戻す。
//   こうすると「高域が先に減る」という実際の部屋の挙動が、ダンピング係数の捏造ではなく
//   実測カーブとして出る。
//
// ノイズ系列は固定:
//   包絡が更新されるたびにノイズを引き直すと、IR 差し替え時に波形が完全に別物になって
//   プチッと鳴る。系列を固定して包絡だけ差し替えれば、前後の IR は強く相関するので
//   ブロック境界での切替がほぼ聞こえない。尾はもともとノイズなので、
//   「どのノイズか」に音質上の意味は無い。
//
// スレッド規約: build() は制御スレッドから呼ぶ（IR の作り直し）。確保は構築時に済ませてある。
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/ReverbTailIr.cs。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace af {
namespace dsp {

class ReverbTailIr {
public:
    static constexpr int kNumBands = 6;

    /// length   : 尾IRの長さ(サンプル)
    /// maxBins  : エコグラムの最大ビン数（包絡の作業領域をここで確保しておく）
    ReverbTailIr(int sampleRate, int length, int channels, int maxBins = 512, unsigned int seed = 12345u)
        : sampleRate_(sampleRate),
          length_(std::max(1, length)),
          channels_(std::max(1, channels)),
          maxBins_(std::max(1, maxBins)) {
        const std::size_t L = static_cast<std::size_t>(length_);
        ir_.assign(static_cast<std::size_t>(channels_) * L, 0.0f);
        bandNoise_.assign(static_cast<std::size_t>(channels_) * kNumBands * L, 0.0f);

        std::vector<float> noise(L);
        std::vector<float> split(static_cast<std::size_t>(kNumBands) * L);
        unsigned int rng = seed;
        for (int c = 0; c < channels_; ++c) {
            // チャンネルごとに独立なノイズ＝L/R が無相関になり、尾が広がる。
            for (std::size_t i = 0; i < L; ++i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                noise[i] = static_cast<float>(static_cast<int>(rng)) * (1.0f / 2147483648.0f);
            }
            splitBands(noise.data(), static_cast<int>(L), sampleRate_, split.data());
            for (int b = 0; b < kNumBands; ++b)
                std::copy(split.begin() + static_cast<std::size_t>(b) * L,
                          split.begin() + static_cast<std::size_t>(b + 1) * L,
                          bandNoise_.begin() + noiseIndex(c, b, 0));
        }

        env_.assign(static_cast<std::size_t>(maxBins_) * kNumBands, 0.0f);
        envAcc_.assign(static_cast<std::size_t>(maxBins_) * kNumBands, 0.0f);
    }

    int length() const { return length_; }
    int channels() const { return channels_; }
    const float* ir(int c) const {
        return ir_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(length_);
    }

    /// 帯域別エコグラムから尾 IR を組み立てる。
    ///   echoBands[k*6 + b] : 時間ビン k・帯域 b のエネルギー
    ///   binMs              : 1 ビンの長さ(ms)
    ///   startMs            : ここより前は早期反射側が担当するので 0 にする（二重計上を防ぐ）
    ///   fadeMs             : startMs 付近の立ち上がりをなめらかにする幅
    ///   smoothMs           : 包絡の平滑幅（山彦対策）。0 で無効
    ///   earBandGain        : 耳ごと×6帯域のゲイン [c*6+b]（エネルギー比）。null なら均一
    /// 戻り値: 尾/直接 のエネルギー比（エコグラム単位）。0 なら尾なし。
    ///   IR は「エネルギー1」に正規化して返すので、絶対レベルは呼び手がこの比から決める
    ///   （calibrateGain 参照）。
    float build(const float* echoBands, int binCount, float binMs, float startMs, float fadeMs,
                float smoothMs, float smoothGrowth, float envAlpha,
                const float* earBandGain = nullptr, int earBandGainLen = 0) {
        std::fill(ir_.begin(), ir_.end(), 0.0f);
        if (!echoBands || binCount <= 0 || binMs <= 0.0f) return 0.0f;
        binCount = std::min(binCount, maxBins_);

        // 平滑 ＋ 更新をまたいだ時間平均。以降は env_（加工済み）を使う。
        // ※ 距離減衰はエンジン側（音源→反射点の区間）で掛け済み。ここでは掛けない。
        //    以前はここでビンの到達時刻＝総経路長から掛けていたが、それだと尾に
        //    1/t² の偽の減衰が乗り、広い部屋の中央ほど反響が痩せた。
        smoothEnvelope(echoBands, binCount,
                       static_cast<int>(std::lround(smoothMs / binMs * 0.5f)), smoothGrowth);
        accumulateEnvelope(binCount, envAlpha);
        const float* env = env_.data();

        // 直接音ビン（＝立ち上がりの最大ビン）と、尾の総エネルギーを測る。
        //   IR のエネルギーは加算的なので、ビンを足したものがそのまま尾のエネルギー。
        const int startBinIdx = clampi(static_cast<int>(std::floor(startMs / binMs)), 0, binCount);
        float directEnergy = 0.0f, tailEnergy = 0.0f;
        for (int k = 0; k < binCount; ++k) {
            float e = 0.0f;
            for (int b = 0; b < kNumBands; ++b) e += env[static_cast<std::size_t>(k) * kNumBands + b];
            if (k < startBinIdx) { if (e > directEnergy) directEnergy = e; }
            else tailEnergy += e;
        }
        const float ratio = (directEnergy > 1e-20f) ? tailEnergy / directEnergy : 0.0f;

        const float samplesPerBin = binMs * 0.001f * static_cast<float>(sampleRate_);
        if (samplesPerBin < 1.0f) return 0.0f;
        const int startSample =
            clampi(static_cast<int>(std::lround(startMs * 0.001f * sampleRate_)), 0, length_);
        const int fadeSamples =
            std::max(1, static_cast<int>(std::lround(fadeMs * 0.001f * sampleRate_)));

        float totalEnergy = 0.0f;
        for (int c = 0; c < channels_; ++c) {
            float* dst = ir_.data() + static_cast<std::size_t>(c) * length_;
            for (int b = 0; b < kNumBands; ++b) {
                const float* nb = bandNoise_.data() + noiseIndex(c, b, 0);
                // 耳ごと×帯域の方向ゲイン（エネルギー比で渡ってくるので振幅は √）。
                float dirAmp = 1.0f;
                const int gi = c * kNumBands + b;
                if (earBandGain && gi < earBandGainLen)
                    dirAmp = std::sqrt(std::max(0.0f, earBandGain[gi]));
                for (int i = startSample; i < length_; ++i) {
                    // ビン境界で段差が出ないよう、包絡はビン間を線形補間する。
                    const float x = static_cast<float>(i) / samplesPerBin - 0.5f;
                    const int k0 = static_cast<int>(std::floor(x));
                    const float f = x - static_cast<float>(k0);
                    const float e0 = (k0 >= 0 && k0 < binCount)
                                   ? env[static_cast<std::size_t>(k0) * kNumBands + b] : 0.0f;
                    const int k1 = k0 + 1;
                    const float e1 = (k1 >= 0 && k1 < binCount)
                                   ? env[static_cast<std::size_t>(k1) * kNumBands + b] : 0.0f;
                    const float e = e0 + (e1 - e0) * f;
                    if (e <= 0.0f) continue;

                    // エネルギー→振幅は √。ビン内に散らばる分の正規化も掛ける。
                    float amp = std::sqrt(e / samplesPerBin) * dirAmp;
                    // 早期部との継ぎ目をなめらかに。
                    const int d = i - startSample;
                    if (d < fadeSamples) amp *= static_cast<float>(d) / static_cast<float>(fadeSamples);
                    dst[i] += nb[i] * amp;
                }
            }
            for (int i = 0; i < length_; ++i) totalEnergy += dst[i] * dst[i];
        }
        if (totalEnergy <= 1e-20f) return 0.0f;

        // エネルギー1に正規化する。
        //   こうしておくと「この IR で畳み込むと、入力とほぼ同じパワーが出る」状態になり、
        //   絶対レベルは呼び手が D/R 比から一意に決められる（下の calibrateGain）。
        //   全チャンネル合計でエネルギー1にする。ここを /channels にすると
        //   「1チャンネルあたり1」＝合計2になり、ステレオのとき尾が √2 だけ大きく鳴る。
        const float norm = 1.0f / std::sqrt(totalEnergy);
        for (float& v : ir_) v *= norm;
        return ratio;
    }

    /// 尾の絶対ゲインを決める。
    ///   directGain : 直接音タップの広帯域ゲイン（距離減衰・透過を含む＝絶対レベルの基準）
    ///   target     : 残響/直接エネルギーの目標比 (r/r_c)²（物理式・呼び手が算出）
    /// 尾IRはエネルギー1に正規化済みなので、持続入力に対する畳み込み出力パワーは入力と等しい。
    /// よって tailGain = directGain × √target で、出力の 尾/直接 パワー比がちょうど target。
    static float calibrateGain(float directGain, float target) {
        if (target <= 0.0f || directGain <= 0.0f) return 0.0f;
        return directGain * std::sqrt(target);
    }

private:
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

    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    std::size_t noiseIndex(int c, int b, int i) const {
        return ((static_cast<std::size_t>(c) * kNumBands + b) * static_cast<std::size_t>(length_))
               + static_cast<std::size_t>(i);
    }

    // 白色ノイズを 6 帯域に分ける（IrConvolver と同じ直列クロスオーバー）。
    // 全帯域を足すと元のノイズに戻る＝包絡が全帯域同じなら白色のまま。
    static void splitBands(const float* src, int n, int sampleRate, float* out /*[6][n]*/) {
        static const float kCrossHz[kNumBands - 1] = {177.0f, 354.0f, 707.0f, 1414.0f, 2828.0f};
        Biquad lp[kNumBands - 1];
        for (int i = 0; i < kNumBands - 1; ++i)
            lp[i].setLowpass(kCrossHz[i], static_cast<float>(sampleRate));

        for (int i = 0; i < n; ++i) {
            float rest = src[i];
            for (int b = 0; b < kNumBands - 1; ++b) {
                const float lo = lp[b].process(rest);
                out[static_cast<std::size_t>(b) * n + i] = lo;
                rest -= lo;
            }
            out[static_cast<std::size_t>(kNumBands - 1) * n + i] = rest;
        }
    }

    // エコグラムの包絡を時間方向に平滑する。
    //
    // なぜ必要か:
    //   レイは有限本数・有限バウンスなので、到達時刻が平均自由行程の倍数付近に固まる。
    //   その塊をそのまま包絡に使うと、ノイズに周期的な振幅変調が掛かって
    //   「なめらかな尾」ではなく「山彦（離散したエコー）」に聞こえる。
    //   実際の部屋では反射密度は t² で増えて滑らかになるので、この塊は
    //   部屋の性質ではなく推定量のばらつき＝平滑して消すのが正しい。
    //   減衰カーブそのもの（数百ms スケール）は平滑しても保たれる。
    // growth: 平滑幅を時刻に比例して広げる割合（0 で固定幅）。
    //   後期ほど推定の分散が大きい。エネルギーが減るうえ、レイのバウンス数上限のせいで
    //   遅い時刻に届く経路の本数自体が激減するため。固定幅だと後半だけガタついたままになり、
    //   それがノイズへの振幅変調＝残響の粒立ちとして聞こえる。
    void smoothEnvelope(const float* echoBands, int binCount, int halfWin, float growth) {
        const std::size_t need = static_cast<std::size_t>(binCount) * kNumBands;
        if (halfWin <= 0 && growth <= 0.0f) {
            std::copy(echoBands, echoBands + need, env_.begin());
            return;
        }
        for (int b = 0; b < kNumBands; ++b) {
            for (int k = 0; k < binCount; ++k) {
                const int w = std::max(halfWin, static_cast<int>(std::lround(k * growth)));
                float sum = 0.0f;
                int n = 0;
                const int lo = std::max(0, k - w), hi = std::min(binCount - 1, k + w);
                for (int j = lo; j <= hi; ++j) {
                    sum += echoBands[static_cast<std::size_t>(j) * kNumBands + b];
                    ++n;
                }
                env_[static_cast<std::size_t>(k) * kNumBands + b] = (n > 0) ? sum / n : 0.0f;
            }
        }
    }

    // 測定した包絡を、更新をまたいで時間平均する。
    //
    // 二重の効果がある:
    //   ① 分散が下がる。レイの本数を増やさずに推定を滑らかにできるので、
    //      空間方向の平滑（減衰カーブの情報を潰す）に頼らずに粒立ちを減らせる。
    //   ② 更新ごとの IR の差が小さくなる。尾の畳み込みは IR をブロック境界で
    //      ハードスワップするので、差が大きいと切替が音として聞こえる。
    // alpha=1 で平均なし（毎回の測定をそのまま使う）。
    void accumulateEnvelope(int binCount, float alpha) {
        const std::size_t need = static_cast<std::size_t>(binCount) * kNumBands;
        if (envAccBins_ != binCount) {
            envAccBins_ = binCount;
            std::copy(env_.begin(), env_.begin() + need, envAcc_.begin());   // 初回は測定値で埋める
            return;
        }
        alpha = std::min(1.0f, std::max(0.0f, alpha));
        for (std::size_t i = 0; i < need; ++i) envAcc_[i] += (env_[i] - envAcc_[i]) * alpha;
        std::copy(envAcc_.begin(), envAcc_.begin() + need, env_.begin());
    }

    const int sampleRate_;
    const int length_;
    const int channels_;
    const int maxBins_;

    std::vector<float> ir_;          // [channel][length]
    std::vector<float> bandNoise_;   // [channel][band][length] 固定ノイズを帯域分解したもの
    std::vector<float> env_;         // [bin][band] 加工済み包絡
    std::vector<float> envAcc_;      // [bin][band] 更新をまたいで保持する包絡
    int envAccBins_ = -1;
};

}  // namespace dsp
}  // namespace af
