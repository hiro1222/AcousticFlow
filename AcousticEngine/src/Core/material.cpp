/* Core/material.cpp
 * マテリアルのプリセット値。C# 側 AcousticMaterial.cs と同値に保つこと。
 * 透過率は質量則沿いの現実的な遮音量に寄せた目安（低域ほど漏れ、高域ほど遮る）。
 *   コンクリ -30〜40dB級 / 石膏ボード -18〜28dB級。absorption/scattering は暫定。
 */
#include "Core/material.h"

namespace acoustic {

AcousticMaterial AcousticMaterial::defaultWall() {
    return AcousticMaterial{
        // transmission: 125   250    500    1k     2k     4k  （石膏ボード相当・低域は少し漏れる）
        { 0.12f, 0.07f, 0.04f, 0.02f, 0.012f, 0.008f },
        // absorption
        { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
        // scattering（高域ほど散る。一般的な内壁のやや粗い面）
        { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f },
    };
}

AcousticMaterial AcousticMaterial::concrete() {
    return AcousticMaterial{
        { 0.05f, 0.03f, 0.015f, 0.008f, 0.004f, 0.002f },  // コンクリ相当（ほぼ遮断）
        { 0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f },
        { 0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f },  // 打ちっぱなし=わりと平滑
    };
}

AcousticMaterial AcousticMaterial::glass() {
    return AcousticMaterial{
        { 0.10f, 0.07f, 0.05f, 0.035f, 0.025f, 0.018f },  // ガラス（やや抜ける）
        { 0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f },
        { 0.02f, 0.03f, 0.05f, 0.08f, 0.10f, 0.15f },  // 平滑＝ほぼ鏡面
    };
}

}  // namespace acoustic
