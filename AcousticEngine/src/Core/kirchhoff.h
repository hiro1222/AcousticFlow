/* Core/kirchhoff.h ── 矩形開口のフレネル・キルヒホッフ回折（解析解）
 *
 * ── なぜこれが「元の法則」なのか ──────────────────────────────
 *   音が障害物の脇を通って届く現象の一般形は、フレネル・キルヒホッフの回折積分:
 *
 *     音源と受音点の間に面を1枚置く。受音点の音場は、その面のうち**塞がれていない部分**に
 *     わたる積分で決まる。各点の寄与は経路長 r1+r2 に応じた位相 e^{-jk(r1+r2)} を持つ。
 *
 *   これまで使ってきたものは、すべてこの積分の近似だった:
 *     ・前川の式 … 半無限スクリーンに対する実験式（積分の結果を式で当てたもの）
 *     ・UTD      … 同じ積分の漸近展開（波長が形状より十分小さい極限）
 *
 *   近似を使うと、近似が捨てた情報は取り戻せない。実際に問題になったのがこれ:
 *     **前川の式は開口の幅を入力に持たない**（δ だけの関数）。
 *     そのため「扉がどのくらい開いたか」に反応できない ── 扉が回っても、回折経路が回る
 *     戸口の枠は動かないので δ が変わらないため。piecewise constant になる。
 *     作品のコンセプト（周辺環境の変化を音で伝える）の中核が、モデルの構造で表現できない。
 *
 * ── なぜ矩形で解析的に解くのか ──────────────────────────────
 *   積分を素朴にサンプリングすると、必要な解像度が波長で決まるため高域が破綻する
 *   （4kHz で 6mm の隙間を解像するには 1万点規模）。
 *   ところが**開口を矩形で近似すれば積分は変数分離でき、閉形式で解ける**。
 *   コストは開口あたり O(1)、量子化も無い。
 *
 * ── 何が自動的に出るか ──────────────────────────────────────
 *   開口の幅 / 扉の開き具合 / 周波数依存 / 半無限スクリーン（＝片側が無限の矩形）
 *   いずれも同じ式の特殊ケースとして出る。閾値も場合分けも要らない。
 */
#ifndef ACOUSTICFLOW_CORE_KIRCHHOFF_H
#define ACOUSTICFLOW_CORE_KIRCHHOFF_H

#include <cmath>
#include <complex>

#include "Core/material.h"  // kNumBands
#include "Core/utd.h"       // fresnelCS（A&S の有理近似）を共用する

namespace acoustic {
namespace kirchhoff {

using cf = std::complex<float>;

constexpr float kSpeed = 343.0f;
constexpr float kBandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};

// 実質的な無限（C,S が漸近値 ±0.5 に十分近づく引数）。半無限スクリーンを表すのに使う。
constexpr float kInf = 8.0f;

// 面上の座標 x(m) → 無次元フレネル変数 w。
//   経路超過 δ(x) ≈ x²/(2D),  D = d1·d2/(d1+d2)
//   位相 = kδ = π x²/(λD) を、正規化フレネル積分の位相 π w²/2 に合わせると w = x√(2/(λD))。
inline float fresnelVar(float x, float lambda, float D) {
    const float t = 2.0f / std::max(lambda * D, 1e-9f);
    return x * std::sqrt(t);
}

// 1 軸ぶんの複素振幅比（自由音場で 1）。
//   [w1, w2] が開いている区間。半無限なら片側に ±kInf を渡す。
//
//   検算: ナイフエッジ（w1=-∞, w2=0）で 0.5 ＝ −6dB。影境界の古典解に一致する。
inline cf axisRatio(float w1, float w2) {
    float c1, s1, c2, s2;
    utd::fresnelCS(w1, c1, s1);
    utd::fresnelCS(w2, c2, s2);
    const cf open(c2 - c1, -(s2 - s1));
    const cf full(1.0f, -1.0f);   // w1=-∞, w2=+∞ のとき (C:1, S:1)
    return open / full;
}

// 矩形開口の帯域別ゲイン(0..1)。
//   u1,u2 / v1,v2 : 面上の開口範囲(m)。中心は直線が面と交わる点（＝軸上が 0）。
//                   半無限側には ±1e9 のような大きな値を渡してよい（内部でクランプ）。
//   d1,d2         : 音源→面 / 面→受音点 の距離(m)。
//
//   変数分離: 矩形なので u 方向と v 方向の積分が独立に解け、比の積になる。
inline void apertureGain(float u1, float u2, float v1, float v2,
                         float d1, float d2, float outGain[kNumBands]) {
    const float D = (d1 * d2) / std::max(d1 + d2, 1e-6f);
    for (int b = 0; b < kNumBands; ++b) {
        const float lambda = kSpeed / kBandFreq[b];
        auto clampW = [&](float x) {
            const float w = fresnelVar(x, lambda, D);
            return (w > kInf) ? kInf : ((w < -kInf) ? -kInf : w);
        };
        const cf a = axisRatio(clampW(u1), clampW(u2));
        const cf c = axisRatio(clampW(v1), clampW(v2));
        float g = std::abs(a * c);
        if (g > 1.0f) g = 1.0f;      // 開口の縁付近では干渉で 1 を僅かに超えうる
        outGain[b] = g;
    }
}

}  // namespace kirchhoff
}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_KIRCHHOFF_H
