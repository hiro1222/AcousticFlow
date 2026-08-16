// fft.h — 反復 radix-2 複素 FFT。
//
// 段4（DSP の C++ 移行）の土台。分割畳み込み・残響尾IR・HRTF が全部これに乗る。
//
// 設計方針:
//   ・バタフライ用の回転因子とビット反転表は構築時に作る。**実行時に確保しない**。
//     オーディオスレッドで回るので、確保もロックも例外も許されない。
//   ・in-place。呼び出し側が用意した re/im を書き換える。
//   ・逆変換は 1/N 正規化つき（順→逆で元に戻る）。
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/PartitionedConvolver.cs の Fft クラス。
// 数値が一致することを AcousticEngineTest/dsp_regression.cpp で素朴なDFTと突き合わせて確認する。
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace af {
namespace dsp {

class Fft {
public:
    // n は 2 の冪（2 以上）。満たさない場合は size()==0 の無効な器になる。
    explicit Fft(int n) {
        if (n < 2 || (n & (n - 1)) != 0) return;
        n_ = n;

        int bits = 0;
        while ((1 << bits) < n) ++bits;

        rev_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[static_cast<std::size_t>(i)] = r;
        }

        const std::size_t half = static_cast<std::size_t>(n / 2);
        cos_.resize(half);
        sin_.resize(half);
        for (std::size_t i = 0; i < half; ++i) {
            const double a = -2.0 * 3.14159265358979323846 * static_cast<double>(i)
                           / static_cast<double>(n);
            cos_[i] = static_cast<float>(std::cos(a));
            sin_[i] = static_cast<float>(std::sin(a));
        }
    }

    int size() const { return n_; }
    bool valid() const { return n_ >= 2; }

    // in-place 変換。inverse=true で逆変換（1/N 正規化つき）。
    //   re / im は少なくとも size() 要素。確保も例外も無い。
    void transform(float* re, float* im, bool inverse) const {
        if (n_ < 2 || !re || !im) return;
        const int n = n_;

        for (int i = 0; i < n; ++i) {
            const int j = rev_[static_cast<std::size_t>(i)];
            if (j > i) {
                float t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i];      im[i] = im[j]; im[j] = t;
            }
        }

        for (int len = 2; len <= n; len <<= 1) {
            const int halfLen = len >> 1;
            const int step = n / len;
            for (int i = 0; i < n; i += len) {
                int k = 0;
                for (int j = 0; j < halfLen; ++j, k += step) {
                    const float wr = cos_[static_cast<std::size_t>(k)];
                    const float wi = inverse ? -sin_[static_cast<std::size_t>(k)]
                                             :  sin_[static_cast<std::size_t>(k)];
                    const int a = i + j, b = a + halfLen;
                    const float xr = re[b] * wr - im[b] * wi;
                    const float xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi;
                    re[a] += xr;        im[a] += xi;
                }
            }
        }

        if (inverse) {
            const float s = 1.0f / static_cast<float>(n);
            for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
        }
    }

private:
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

}  // namespace dsp
}  // namespace af
