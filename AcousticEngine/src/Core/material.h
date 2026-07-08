/* Core/material.h
 * 表面の音響特性（マテリアル）。周波数6帯域で持つ。
 *   帯域: 125, 250, 500, 1k, 2k, 4k Hz
 *
 * transmission: 各帯域の透過率(0..1)。1=素通り / 0=完全遮断。
 *               低域ほど高い（壁越しにベースが聞こえる現象）。
 * absorption:   各帯域の吸収率(0..1)。反射時に失うエネルギー。
 * scattering:   各帯域の散乱率(0..1)。反射方向の「鏡面⇄拡散」の混合率。
 *               0=完全鏡面（平行反射）/ 1=完全拡散（ランバート）。表面の粗さ/波長で決まり
 *               高域ほど散る。反射エネルギー(=1-α-τ)の向きだけを変える（エネルギー総量は不変）。
 *
 * ※ 係数の具体値は現状「暫定（それらしい placeholder）」。
 *    本番では文献値/実測に差し替える前提（数値は要確認）。
 */
#ifndef ACOUSTICFLOW_CORE_MATERIAL_H
#define ACOUSTICFLOW_CORE_MATERIAL_H

namespace acoustic {

constexpr int kNumBands = 6;  // 125,250,500,1k,2k,4kHz

struct AcousticMaterial {
    float transmission[kNumBands];
    float absorption[kNumBands];
    float scattering[kNumBands];

    // --- プリセット（暫定値）---
    static AcousticMaterial defaultWall();  // 一般的な内壁（石膏ボード相当）
    static AcousticMaterial concrete();     // コンクリート（よく遮る）
    static AcousticMaterial glass();        // ガラス（やや抜ける）
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_MATERIAL_H
