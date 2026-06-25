/* Core/material.cpp
 * マテリアルのプリセット値。
 * ※ いずれの数値も暫定（それらしい placeholder）。本番は文献/実測で要確認。
 *   傾向だけは物理的に妥当：低域ほど透過しやすく、高域ほど遮られる。
 */
#include "Core/material.h"

namespace acoustic {

AcousticMaterial AcousticMaterial::defaultWall() {
    return AcousticMaterial{
        // transmission: 125  250  500  1k    2k    4k
        { 0.50f, 0.40f, 0.30f, 0.20f, 0.12f, 0.08f },
        // absorption
        { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
    };
}

AcousticMaterial AcousticMaterial::concrete() {
    return AcousticMaterial{
        { 0.15f, 0.10f, 0.06f, 0.03f, 0.02f, 0.01f },  // よく遮る
        { 0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f },
    };
}

AcousticMaterial AcousticMaterial::glass() {
    return AcousticMaterial{
        { 0.40f, 0.30f, 0.25f, 0.20f, 0.15f, 0.10f },  // やや抜ける
        { 0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f },
    };
}

}  // namespace acoustic
