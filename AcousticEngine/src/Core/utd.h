/* Core/utd.h ── UTD(Uniform Theory of Diffraction) 回折係数（Phase 4-C）
 *
 * 理論: Kouyoumjian & Pathak (1974) の UTD ウェッジ回折。GTD の影/反射境界での発散を
 *   遷移関数 F(X)（Fresnel積分ベース）で連続化した「一様」版。
 *   回折点 P で、入射(s')・回折(s)・エッジ角 n・入射角φ'・回折角φ・Kellerコーン角β0 から
 *   複素回折係数 D を出し、直接(自由音場)に対する相対ゲイン(0..1)を6帯域で返す。
 *
 * 相対ゲイン = |Σ(4項) cot·F| / (2n·√(2π k L)) ,  L = s'·s·sin²β0/(s'+s)
 *   影境界で ~0.5(-6dB)・低域ほど回り込む・ウェッジ角 n で挙動が変わる（n=2で半無限スクリーン）。
 */
#ifndef ACOUSTICFLOW_CORE_UTD_H
#define ACOUSTICFLOW_CORE_UTD_H

#include <cmath>
#include <complex>

#include "Core/material.h"  // kNumBands
#include "Core/vec3.h"

namespace acoustic {
namespace utd {

using cf = std::complex<float>;

constexpr float kPi = 3.14159265358979f;
constexpr float kSpeed = 343.0f;

// Fresnel積分 C(x)=∫0^x cos(πt²/2)dt, S(x)=∫0^x sin(πt²/2)dt。
// Abramowitz & Stegun 7.3.32-33 の有理近似（|誤差|<2e-3、x≥0）。
inline void fresnelCS(float x, float& C, float& S) {
    const float ax = std::fabs(x);
    const float f = (1.0f + 0.926f * ax) / (2.0f + 1.792f * ax + 3.104f * ax * ax);
    const float g = 1.0f / (2.0f + 4.142f * ax + 3.492f * ax * ax + 6.670f * ax * ax * ax);
    const float a = 0.5f * kPi * ax * ax;
    C = 0.5f + f * std::sin(a) - g * std::cos(a);
    S = 0.5f - f * std::cos(a) - g * std::sin(a);
    if (x < 0.0f) { C = -C; S = -S; }  // C,S は奇関数
}

// UTD 遷移関数 F(X) = 2j√X e^{jX} ∫_{√X}^∞ e^{-jτ²}dτ。
//   X→0 で F→0（境界）、X→大 で F→1（GTD に一致）。
inline cf transitionF(float X) {
    if (X <= 1e-7f) return cf(0.0f, 0.0f);
    // ∫_{√X}^∞ e^{-jτ²}dτ = √(π/8)(1-j) - √(π/2)[C(w) - j S(w)],  w=√(2X/π)
    const float w = std::sqrt(2.0f * X / kPi);
    float C, S;
    fresnelCS(w, C, S);
    const cf tailInt = cf(std::sqrt(kPi / 8.0f), -std::sqrt(kPi / 8.0f))
                     - std::sqrt(kPi / 2.0f) * cf(C, -S);
    const float sx = std::sqrt(X);
    const cf pref = cf(0.0f, 2.0f * sx) * std::exp(cf(0.0f, X));  // 2j√X e^{jX}
    return pref * tailInt;
}

inline float cot(float x) {
    const float s = std::sin(x);
    if (std::fabs(s) < 1e-6f) return (s >= 0.0f ? 1e6f : -1e6f);  // 特異点を有限にクランプ
    return std::cos(x) / s;
}

// a±(β) = 2cos²((2nπN± − β)/2),  N± = round((β±π)/(2nπ))。
inline float aParam(float beta, float n, int sign) {
    const float twoNpi = 2.0f * n * kPi;
    const float N = std::round((beta + sign * kPi) / twoNpi);
    const float c = std::cos((twoNpi * N - beta) * 0.5f);
    return 2.0f * c * c;
}

// cot((π±β)/2n)·F(kL·a) を計算。影/反射境界(cotArg≈mπ)で cot は特異だが F→0 と相殺して
// 有限。境界近傍は Kouyoumjian-Pathak の極限 n√(2πkL)·sgn·e^{jπ/4}（|term|/(2n√)=0.5）に切替。
inline cf cotTimesF(float cotArg, float kLa, float n, float kL) {
    const float m = std::round(cotArg / kPi);
    const float dev = cotArg - m * kPi;
    if (std::fabs(dev) < 1e-2f) {
        const float sgn = (dev >= 0.0f) ? 1.0f : -1.0f;
        return n * std::sqrt(2.0f * kPi * kL) * sgn * std::exp(cf(0.0f, kPi / 4.0f));
    }
    return cot(cotArg) * transitionF(kLa);
}

// 1つの β(=φ∓φ') に対する2項（+/−）の和。
inline cf termPair(float beta, float n, float kL) {
    return cotTimesF((kPi + beta) / (2.0f * n), kL * aParam(beta, n, +1), n, kL)
         + cotTimesF((kPi - beta) / (2.0f * n), kL * aParam(beta, n, -1), n, kL);
}

// 角度直接版：n(ウェッジ指数), β0(Kellerコーン角,rad), φ'(入射), φ(回折), s'/s(距離) →
// 6帯域の相対回折ゲイン(0..1)を outGain に書く。検証しやすいよう角度を直接受ける。
inline void utdGain(float n, float beta0, float phiPrime, float phi,
                    float sPrime, float s, float outGain[kNumBands]) {
    const float sinB0 = std::max(std::sin(beta0), 1e-3f);
    const float L = sPrime * s * sinB0 * sinB0 / std::max(sPrime + s, 1e-3f);
    const float bandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
    const float dphiMinus = phi - phiPrime;
    const float dphiPlus = phi + phiPrime;
    for (int b = 0; b < kNumBands; ++b) {
        const float k = 2.0f * kPi * bandFreq[b] / kSpeed;
        const float kL = k * L;
        const cf sum = termPair(dphiMinus, n, kL) + termPair(dphiPlus, n, kL);
        // 相対ゲイン = |Σ cot·F| / (2n √(2π k L))
        const float denom = 2.0f * n * std::sqrt(2.0f * kPi * k * L);
        float gain = (denom > 1e-9f) ? std::abs(sum) / denom : 0.0f;
        if (gain > 1.0f) gain = 1.0f;
        if (gain < 0.0f) gain = 0.0f;
        outGain[b] = gain;
    }
}

// 位置＋幾何版：source/P(回折点)/listener と エッジ方向・0面接線(refTangent, 面内)・n から
// 角度を作って utdGain を呼ぶ。refTangent は 0面の「エッジから外側へ」向く単位方向(⊥edgeDir)。
inline void utdWedgeGain(const Vec3& source, const Vec3& P, const Vec3& listener,
                         const Vec3& edgeDir, const Vec3& refTangent, float n,
                         float outGain[kNumBands]) {
    const Vec3 e = normalized(edgeDir);
    const float sPrime = std::max(length(P - source), 1e-3f);
    const float s = std::max(length(listener - P), 1e-3f);
    const Vec3 sp = normalized(P - source);                 // 入射伝播方向
    const float sinB0 = length(cross(sp, e));               // sinβ0
    const float beta0 = std::asin(std::min(std::max(sinB0, 0.0f), 1.0f));
    // エッジ⊥平面の基底。
    Vec3 t0 = refTangent - e * dot(refTangent, e);
    t0 = normalized(t0);
    const Vec3 t1 = cross(e, t0);
    auto angleOf = [&](const Vec3& target) {
        Vec3 d = target - P;
        d = d - e * dot(d, e);
        float a = std::atan2(dot(d, t1), dot(d, t0));
        if (a < 0.0f) a += 2.0f * kPi;
        return a;
    };
    const float phiPrime = angleOf(source);
    const float phi = angleOf(listener);
    utdGain(n, beta0, phiPrime, phi, sPrime, s, outGain);
}

}  // namespace utd
}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_UTD_H
