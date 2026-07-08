/* Core/ray.h
 * レイ(光線)と、その当たり判定結果。
 *
 * isOccluded は「線分が遮られるか(真偽)」だけだったが、
 * レイキャストでは「どこに・どれだけの距離でぶつかったか・面の向き」まで要る。
 * 反射方向の計算に法線(normal)が必要なため、ここで結果型を定義する。
 */
#ifndef ACOUSTICFLOW_CORE_RAY_H
#define ACOUSTICFLOW_CORE_RAY_H

#include "Core/vec3.h"

namespace acoustic {

struct AcousticMaterial;  // 前方宣言（ポインタで参照するだけ。実体は material.h）

// レイの当たり判定結果。
struct RayHit {
    bool  hit = false;            // 何かにぶつかったか
    float distance = 0.0f;        // 原点からヒット点までの距離
    Vec3  point;                  // ヒットした座標
    Vec3  normal;                 // ヒット面の法線（反射計算に使う）
    int   boxIndex = -1;          // ぶつかった箱障害物のインデックス（メッシュ時は -1）
    // ぶつかった障害物の材質。箱でもメッシュでも共通に参照できるようポインタで持つ。
    const AcousticMaterial* material = nullptr;
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_RAY_H
