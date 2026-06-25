/* Core/source_directivity.cpp
 * 指向性ゲインの実装（B プラン: 広帯域ゲイン + 背面ローパス）。
 *
 * ※ 各タイプの鋭さ(exponent)・下限ゲイン・ローパス量は暫定 placeholder。
 *    傾向（前方ほど大きくクリア、側面/背面ほど小さくこもる）だけは妥当。
 *    本番では文献/実測・実際の聴感で要差し替え。
 */
#include "Core/source_directivity.h"

#include <algorithm>  // std::max
#include <cmath>      // std::pow, std::fabs

namespace acoustic {

namespace {

// 指向性パターンの「形」を表す暫定パラメータ（placeholder・要差し替え）。
struct DirectivityShape {
    float exponent;    // cos^n の n。0 なら無指向。
    bool  bidirectional;  // true なら |cos|（前後対称）で評価する。
    float minGain;     // 最も外れた向きでも残すゲイン（完全無音を避ける）。
    float maxLowpass;  // 最も外れた向きでのこもり量（ローパス）。
};

// TODO(directivity): 数値はすべて暫定。聴感・文献で要調整。
DirectivityShape shapeFor(DirectivityType type) {
    switch (type) {
        case DirectivityType::Omni:
            return { 0.0f, false, 1.00f, 0.00f };
        case DirectivityType::Cardioid:
            return { 1.0f, false, 0.12f, 0.55f };
        case DirectivityType::Supercardioid:
            return { 3.0f, false, 0.06f, 0.75f };
        case DirectivityType::Bidirectional:
            return { 1.0f, true,  0.12f, 0.55f };
        case DirectivityType::Beam:
            return { 8.0f, false, 0.03f, 0.90f };
    }
    return { 0.0f, false, 1.0f, 0.0f };  // 未知タイプは無指向扱い（安全側）。
}

}  // namespace

float computeDirectivity(const Vec3& sourcePos,
                         const Vec3& sourceForward,
                         const Vec3& listenerPos,
                         DirectivityType type,
                         float* outLowpass) {
    const DirectivityShape s = shapeFor(type);

    // 音源の主放射方向と、音源→リスナー方向の cosθ。
    const Vec3 toListener = normalized(listenerPos - sourcePos);
    const Vec3 fwd = normalized(sourceForward);
    const float c = dot(fwd, toListener);  // -1..1

    // パターン値 raw を 0..1 で求める。
    //   無指向(exponent=0): 常に 1
    //   前方型:   max(0, cosθ)^n（背面は 0）
    //   双方向:   |cosθ|^n（前後対称）
    float raw;
    if (s.exponent <= 0.0f) {
        raw = 1.0f;
    } else {
        const float base = s.bidirectional ? std::fabs(c) : std::max(0.0f, c);
        raw = std::pow(base, s.exponent);
    }

    // raw(0..1) を minGain..1 にマップ。完全無音は不自然なので下限を持たせる。
    const float gain = s.minGain + (1.0f - s.minGain) * raw;

    // 外れた向きほどローパスを強める（=こもる）。
    if (outLowpass) {
        *outLowpass = (1.0f - raw) * s.maxLowpass;
    }
    return gain;
}

}  // namespace acoustic
