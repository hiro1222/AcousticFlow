
//遮蔽計算用、仮実装につき今後は回り込み制御に切替済んだらお役御免

#ifndef ACOUSTICFLOW_CORE_AABB_H
#define ACOUSTICFLOW_CORE_AABB_H

#include <algorithm>
#include <cmath>

#include "Core/vec3.h"

namespace acoustic {

struct Aabb {
    Vec3 min;  // 各軸の最小座標
    Vec3 max;  // 各軸の最大座標

    //基本的にサイズの半分を中心からとってAABB用意
    static Aabb fromCenterHalfExtents(const Vec3& center, const Vec3& halfExtents) {
        Aabb box;
        box.min = center - halfExtents;
        box.max = center + halfExtents;
        return box;
    }
};


//スラブ法使ってみる(もし今後OBBに変更するならSAT方式に切り替えてみる？)
/**************************************************************************************/

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
            if (org[axis] < bmin[axis] || org[axis] > bmax[axis]) {
                return false;
            }
        } else {
            const float ood = 1.0f / dir[axis];
            float t1 = (bmin[axis] - org[axis]) * ood;
            float t2 = (bmax[axis] - org[axis]) * ood;
            if (t1 > t2) std::swap(t1, t2); 
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) {
                //交わってない
                return false; 
            }
        }
    }
    return true;
}

// レイとAABBの交差

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
    float entrySign = 0.0f; // 法線の向き

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
        /*もし箱の中にリスナーが入った時用*/
        outT = 0.0f;
        outNormal = Vec3(0.0f, 1.0f, 0.0f);  // 仮法線・蟹工船みたい
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

/**************************************************************************************/
// OBB（有向境界ボックス）= 回転した箱。
// キモ：レイ/線分を箱のローカル空間へ移すと、その中では中心原点の軸並行AABBになる。
//       だから既存のスラブ法（上の2関数）をそのまま呼べる。法線だけワールドへ戻す。
//       AABBは「単位回転のOBB」なので、これ1本で軸並行も回転も扱える。

struct Obb {
    Vec3 center;
    Vec3 halfExtents;
    // ローカル軸のワールド向き（正規直交を前提）。Unityの transform.right/up/forward 相当。
    Vec3 axisX{1.0f, 0.0f, 0.0f};  // right
    Vec3 axisY{0.0f, 1.0f, 0.0f};  // up
    Vec3 axisZ{0.0f, 0.0f, 1.0f};  // forward

    // 軸並行（回転なし）。従来のAABDと同じ箱。
    static Obb axisAligned(const Vec3& center, const Vec3& halfExtents) {
        Obb b;
        b.center = center;
        b.halfExtents = halfExtents;
        return b;
    }

    // right/up から正規直交基底を作る（forward = right×up、up は直交化し直す）。
    // 入力が多少ずれてても内部で直す。
    static Obb oriented(const Vec3& center, const Vec3& halfExtents,
                        const Vec3& right, const Vec3& up) {
        Obb b;
        b.center = center;
        b.halfExtents = halfExtents;
        const Vec3 x = normalized(right);
        const Vec3 z = normalized(cross(x, up));  // forward
        const Vec3 y = cross(z, x);               // 直交化した up
        b.axisX = x;
        b.axisY = y;
        b.axisZ = z;
        return b;
    }
};

// ワールド点 → OBBローカル（中心原点・軸並行）。R^T を掛けるのと同義。
inline Vec3 obbToLocalPoint(const Vec3& p, const Obb& b) {
    const Vec3 d = p - b.center;
    return Vec3(dot(d, b.axisX), dot(d, b.axisY), dot(d, b.axisZ));
}

// ワールド方向 → OBBローカル（平行移動なし）。基底が正規直交なので長さは保存される。
inline Vec3 obbToLocalDir(const Vec3& v, const Obb& b) {
    return Vec3(dot(v, b.axisX), dot(v, b.axisY), dot(v, b.axisZ));
}

// 線分 p0->p1 が OBB と交わるか。端点をローカルへ移して既存のAABB線分判定に丸投げ。
inline bool segmentIntersectsObb(const Vec3& p0, const Vec3& p1, const Obb& b) {
    const Aabb local = Aabb::fromCenterHalfExtents(Vec3(0.0f, 0.0f, 0.0f), b.halfExtents);
    return segmentIntersectsAabb(obbToLocalPoint(p0, b), obbToLocalPoint(p1, b), local);
}

// 線分が OBB の内部を通る「長さ」(m)。交差しなければ 0。
//   交差の有無だけでは足りない場面がある。厚みのある壁の稜線を回る回折経路は、
//   幾何的に必ずその壁の厚みぶんを貫くので「交差する＝無効」にすると正しい経路まで消える。
//   一方、大きな壁の遠い稜線へ向かう経路は壁を何メートルも貫く。両者を分けるのは
//   「どれだけ通るか」であって「通るか否か」ではない。
inline float segmentObbPenetration(const Vec3& p0, const Vec3& p1, const Obb& b) {
    const Vec3 lp0 = obbToLocalPoint(p0, b), lp1 = obbToLocalPoint(p1, b);
    const float dir[3] = { lp1.x - lp0.x, lp1.y - lp0.y, lp1.z - lp0.z };
    const float org[3] = { lp0.x, lp0.y, lp0.z };
    const float h[3] = { b.halfExtents.x, b.halfExtents.y, b.halfExtents.z };

    float tmin = 0.0f, tmax = 1.0f;
    const float kEps = 1e-8f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < kEps) {
            if (org[axis] < -h[axis] || org[axis] > h[axis]) return 0.0f;
        } else {
            const float ood = 1.0f / dir[axis];
            float t1 = (-h[axis] - org[axis]) * ood;
            float t2 = ( h[axis] - org[axis]) * ood;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return 0.0f;
        }
    }
    return (tmax - tmin) * length(p1 - p0);
}

// レイと OBB の交差。ローカルでAABB判定し、出てきた法線を基底でワールドへ戻す。
// dir は呼び出し側で正規化済みを前提（outT がそのまま距離になる）。
inline bool rayIntersectsObb(const Vec3& origin, const Vec3& dir, const Obb& b,
                             float maxDist, float& outT, Vec3& outNormal) {
    const Aabb local = Aabb::fromCenterHalfExtents(Vec3(0.0f, 0.0f, 0.0f), b.halfExtents);
    const Vec3 lo = obbToLocalPoint(origin, b);
    const Vec3 ld = obbToLocalDir(dir, b);
    Vec3 nLocal;
    if (!rayIntersectsAabb(lo, ld, local, maxDist, outT, nLocal)) return false;
    // ローカル法線 (±e_k) を基底の線形結合でワールドへ。
    outNormal = b.axisX * nLocal.x + b.axisY * nLocal.y + b.axisZ * nLocal.z;
    return true;
}

}

#endif  // ACOUSTICFLOW_CORE_AABB_H
