/* Core/material.cpp
 * マテリアルのプリセット値。C# 側 AcousticMaterial.cs と同値に保つこと。
 *
 * ★ transmission / absorption は **エネルギー比**（吸収率と同じ土俵）。
 *   透過損失 TL[dB] = -10*log10(transmission)。
 *   質量則沿いに、低域ほど漏れ・高域ほど遮る。
 *     石膏ボード -18〜28dB級 / コンクリ -26〜54dB級。
 *
 *   以前は同じ数値を**振幅**として使っていた（0.12 → -20log10 = 18.4dB）。
 *   ところが refl = 1-α-τ はエネルギーの式なので、そこだけ単位が壊れていた。
 *   エネルギー規約へ統一するにあたり、**旧値を二乗**してある
 *   （0.12 → 0.0144 = TL 18.4dB）。こうすると聞こえ方は変わらず、
 *   しかも上のコメントが意図していた遮音量とも一致する。
 */
#include "Core/material.h"

namespace acoustic {

AcousticMaterial AcousticMaterial::defaultWall() {
    return AcousticMaterial{
        // transmission(エネルギー): 125     250      500      1k       2k        4k
        //   TL[dB]:                 18.4    23.1     28.0     34.0     38.4      42.0
        { 0.0144f, 0.0049f, 0.0016f, 0.0004f, 0.000144f, 0.000064f },
        // absorption（エネルギー）
        { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
        // scattering（高域ほど散る。一般的な内壁のやや粗い面）
        { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f },
    };
}

AcousticMaterial AcousticMaterial::concrete() {
    return AcousticMaterial{
        //   TL[dB]: 26.0     30.5     36.5     42.0     48.0      54.0
        { 0.0025f, 0.0009f, 0.000225f, 0.000064f, 0.000016f, 0.000004f },
        { 0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f },
        { 0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f },  // 打ちっぱなし=わりと平滑
    };
}

AcousticMaterial AcousticMaterial::glass() {
    return AcousticMaterial{
        //   TL[dB]: 20.0     23.1     26.0     29.1     32.0      34.9
        { 0.01f, 0.0049f, 0.0025f, 0.001225f, 0.000625f, 0.000324f },
        { 0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f },
        { 0.02f, 0.03f, 0.05f, 0.08f, 0.10f, 0.15f },  // 平滑＝ほぼ鏡面
    };
}

AcousticMaterial AcousticMaterial::opaque() {
    // 完全不透過（検証用）。壁を抜けてくる成分を消すと、聞こえるのは回り込んだ音だけになる。
    //   吸収は中庸にして、反射・残響は普通に立つようにしてある
    //   （回折だけを聴きたいときは、呼び出し側で反射/残響を切ること）。
    return AcousticMaterial{
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
        { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f },
    };
}

}  // namespace acoustic
