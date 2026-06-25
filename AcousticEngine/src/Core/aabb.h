/* Core/aabb.h
 * 軸並行ボックス(AABB)と、線分との交差判定。
 *
 * 障害物(壁)を最小実装として AABB で表す。
 * リスナー点と音源点を結ぶ線分が、このボックスを貫くかどうかで
 * 「遮蔽されているか」を判定する。
 */
#ifndef ACOUSTICFLOW_CORE_AABB_H
#define ACOUSTICFLOW_CORE_AABB_H

#include <algorithm>
#include <cmath>

#include "Core/vec3.h"

namespace acoustic {

struct Aabb {
    Vec3 min;  // 各軸の最小座標
    Vec3 max;  // 各軸の最大座標

    // 中心と「半分の大きさ(halfExtents)」から AABB を作る。
    // Unity の Box は中心+サイズで扱うことが多いので、この形を用意しておく。
    static Aabb fromCenterHalfExtents(const Vec3& center, const Vec3& halfExtents) {
        Aabb box;
        box.min = center - halfExtents;
        box.max = center + halfExtents;
        return box;
    }
};

/* 線分 p0->p1 が AABB と交わるか判定する（スラブ法）。
 *
 * スラブ法の考え方:
 *   AABB は「x の範囲」「y の範囲」「z の範囲」という3枚の板(スラブ)の重なり。
 *   線分を媒介変数 t(0..1) で表し、各軸ごとに「線分がその範囲内にいる t 区間」を求め、
 *   3軸すべての区間の共通部分が残れば交差している。
 *
 * 線分内に端点が含まれる場合(点がボックス内)も交差と判定する。
 */
inline bool segmentIntersectsAabb(const Vec3& p0, const Vec3& p1, const Aabb& box) {
    const float dir[3] = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
    const float org[3] = { p0.x, p0.y, p0.z };
    const float bmin[3] = { box.min.x, box.min.y, box.min.z };
    const float bmax[3] = { box.max.x, box.max.y, box.max.z };

    float tmin = 0.0f;  // 線分の始点
    float tmax = 1.0f;  // 線分の終点
    const float kEps = 1e-8f;

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < kEps) {
            // この軸方向に動いていない。範囲外に居れば交差し得ない。
            if (org[axis] < bmin[axis] || org[axis] > bmax[axis]) {
                return false;
            }
        } else {
            const float ood = 1.0f / dir[axis];
            float t1 = (bmin[axis] - org[axis]) * ood;
            float t2 = (bmax[axis] - org[axis]) * ood;
            if (t1 > t2) std::swap(t1, t2);  // t1 を近い側に揃える
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) {
                return false;  // 共通区間が消えた = 交差なし
            }
        }
    }
    return true;
}

/* レイ(origin から dir 方向)と AABB の交差判定。
 * 線分版との違い:
 *   - 終点を持たず、距離 maxDist までの「光線」として扱う
 *   - ヒットした距離(outT)と、ヒット面の法線(outNormal)を返す
 * dir は正規化済みであることを前提とする（outT がそのまま距離になる）。
 */
inline bool rayIntersectsAabb(const Vec3& origin, const Vec3& dir, const Aabb& box,
                              float maxDist, float& outT, Vec3& outNormal) {
    const float od[3]   = { origin.x, origin.y, origin.z };
    const float dd[3]   = { dir.x, dir.y, dir.z };
    const float bmin[3] = { box.min.x, box.min.y, box.min.z };
    const float bmax[3] = { box.max.x, box.max.y, box.max.z };
    const float kEps = 1e-8f;

    float tmin = 0.0f;
    float tmax = maxDist;
    int   entryAxis = -1;   // tmin を決めた軸
    float entrySign = 0.0f; // その面の法線の向き(+1/-1)

    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dd[a]) < kEps) {
            // 軸方向に進んでいない。範囲外ならヒットなし。
            if (od[a] < bmin[a] || od[a] > bmax[a]) return false;
        } else {
            const float ood = 1.0f / dd[a];
            float t1 = (bmin[a] - od[a]) * ood;  // min 面との交差
            float t2 = (bmax[a] - od[a]) * ood;  // max 面との交差
            float sign = -1.0f;                  // 入射面が min 面 -> 法線は -軸方向
            if (t1 > t2) { std::swap(t1, t2); sign = 1.0f; }  // max 面側から入射
            if (t1 > tmin) { tmin = t1; entryAxis = a; entrySign = sign; }
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }

    if (entryAxis < 0) {
        // 原点が箱の内部にある場合。距離0でヒット扱いにする。
        outT = 0.0f;
        outNormal = Vec3(0.0f, 1.0f, 0.0f);  // 便宜上の法線
        return true;
    }

    outT = tmin;
    Vec3 n(0.0f, 0.0f, 0.0f);
    if (entryAxis == 0)      n.x = entrySign;
    else if (entryAxis == 1) n.y = entrySign;
    else                     n.z = entrySign;
    outNormal = n;
    return true;
}

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_AABB_H
