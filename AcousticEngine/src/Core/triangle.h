/* Core/triangle.h
 * 三角形と、レイ/線分との交差判定（Möller–Trumbore 法）。
 *
 * メッシュ occluder の最小要素。BVH(bvh.h) がこれを大量に束ねて高速化する。
 * 箱(OBB)より忠実に形状を表せる代わりに重いので、CollArea 内など「実形状で
 * 見せたい領域」にだけ使う想定（外側は箱に丸める＝忠実度LOD）。
 */
#ifndef ACOUSTICFLOW_CORE_TRIANGLE_H
#define ACOUSTICFLOW_CORE_TRIANGLE_H

#include <cmath>

#include "Core/vec3.h"

namespace acoustic {

struct Triangle {
    Vec3 v0;
    Vec3 v1;
    Vec3 v2;
};

/* Möller–Trumbore。レイ(origin, dir=正規化)と三角形の交差。
 *   ヒットしたら outT(距離>eps)と幾何法線 outNormal を返す。
 *   両面ヒット扱い（法線は入射方向に向ける＝反射計算で表を向くように）。
 * det≈0 はレイが三角形平面に平行＝交差なし。
 */
inline bool rayIntersectsTriangle(const Vec3& origin, const Vec3& dir,
                                  const Triangle& tri, float maxDist,
                                  float& outT, Vec3& outNormal) {
    const float kEps = 1e-7f;
    const Vec3 e1 = tri.v1 - tri.v0;
    const Vec3 e2 = tri.v2 - tri.v0;
    const Vec3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < kEps) return false;   // レイが平面に平行
    const float inv = 1.0f / det;

    const Vec3 tvec = origin - tri.v0;
    const float u = dot(tvec, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;

    const Vec3 q = cross(tvec, e1);
    const float v = dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = dot(e2, q) * inv;
    if (t < kEps || t > maxDist) return false;

    outT = t;
    Vec3 n = normalized(cross(e1, e2));
    if (dot(n, dir) > 0.0f) n = Vec3(-n.x, -n.y, -n.z);  // 入射に向ける（両面）
    outNormal = n;
    return true;
}

/* 線分 p0->p1 が三角形と交わるか（遮蔽の有無判定用）。 */
inline bool segmentIntersectsTriangle(const Vec3& p0, const Vec3& p1, const Triangle& tri) {
    const Vec3 d = p1 - p0;
    const float len = length(d);
    if (len < 1e-9f) return false;
    const Vec3 dir = d * (1.0f / len);
    float t;
    Vec3 n;
    return rayIntersectsTriangle(p0, dir, tri, len, t, n);
}

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_TRIANGLE_H
