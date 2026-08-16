/* Core/material.h
 * 表面の音響特性（マテリアル）。周波数6帯域で持つ。
 *   帯域: 125, 250, 500, 1k, 2k, 4k Hz
 *
 * ★単位の規約: transmission / absorption はいずれも **エネルギー比**（0..1）。
 *   振幅として使うときは必ず sqrt を取ること。
 *     エンジン内部（反射係数 1-α-τ、エコーグラムのビン）… エネルギーのまま
 *     ホストへ返すタップのゲイン（直接・早期反射・回折）… **振幅**（= sqrt 済み）
 *   ここが混ざると、壁越しの音が二乗ぶん小さくなる等の誤りになる。
 *
 * transmission: 各帯域の透過率(0..1、エネルギー)。1=素通り / 0=完全遮断。
 *               透過損失 TL[dB] = -10*log10(transmission)。
 *               低域ほど高い（壁越しにベースが聞こえる現象）。
 * absorption:   各帯域の吸収率(0..1、エネルギー)。反射時に失うエネルギー。
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
    static AcousticMaterial woodDoor();     // 木の扉（壁より弱い＝部屋の弱点）
    // 完全不透過（検証用）。透過を 0 にすると「回り込んだ音だけ」が残るので、
    // 回折の定位と減衰を単体で確かめられる。現実の材質ではない。
    static AcousticMaterial opaque();
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_MATERIAL_H
