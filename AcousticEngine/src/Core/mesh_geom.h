/* Core/mesh_geom.h ── メッシュ形状（BLAS）
 *
 * 形状（ここ）と配置（Scene::Instance）を分ける。グラフィックスの BLAS/TLAS と同じ構造で、
 * Instance.geomId のコメント「将来の BLAS(メッシュ)識別用」が意図していたもの。
 *
 * ── なぜ分けるか ────────────────────────────────────────────────
 *   本システムの主張は「レベルの形状が実行時に変わる前提で作られている」こと。
 *   形状と配置を分けると、変化の大半が**配置の操作**になり、形状の再構築が要らなくなる。
 *
 *     移動・回転           → インスタンスの transform 更新のみ
 *     スケール変化(部屋の内寸) → 同上
 *     追加・削除(破壊)      → インスタンスの増減のみ（破片形状は事前登録）
 *     形状の生成(ランタイム破砕) → この 1 個ぶんの構築だけ。レベル全体の再計算は不要
 *
 * ── ローカル空間の取り方 ──────────────────────────────────────
 *   頂点を「ローカル AABB が単位箱 [-1,1]^3 になる」よう正規化して保持する。
 *   こうすると Instance.obb（中心・正規直交軸・halfExtents）が**そのまま変換行列**になり、
 *   同時に**ワールド空間の境界ボックス**にもなる。追加のデータを持たずに済む:
 *
 *     world = center + axisX*(l.x*hx) + axisY*(l.y*hy) + axisZ*(l.z*hz)
 *
 *   非一様スケール(hx≠hy≠hz)を許す。部屋の内寸を変える＝壁を非一様に伸ばすことなので、
 *   ここを一様に制限すると主張の中核が成立しなくなる。
 *   コストは小さい ── **ローカルのレイ方向を正規化しなければ t の意味がワールドと一致する**ので
 *   距離比較は破綻せず、法線は逆転置 = R·S^-1（軸ごとに 1/h を掛けるだけ）で済む。
 */
#ifndef ACOUSTICFLOW_CORE_MESH_GEOM_H
#define ACOUSTICFLOW_CORE_MESH_GEOM_H

#include <algorithm>
#include <vector>

#include "Core/aabb.h"
#include "Core/bvh.h"
#include "Core/triangle.h"
#include "Core/vec3.h"

namespace acoustic {

// 潰れた軸（平面メッシュなど）で 0 除算しないための最小半径。
// 板を置いたときに名目上の厚みとして働き、実際の厚みはインスタンスの halfExtents が決める。
constexpr float kMeshMinHalfExtent = 1e-3f;

struct MeshGeometry {
    TriangleBvh bvh;        // 正規化ローカル空間（AABB = [-1,1]^3）で構築
    Vec3 localCenter{0, 0, 0};       // 正規化前のローカル AABB 中心
    Vec3 localHalfExtents{1, 1, 1};  // 同 half extents（潰れた軸は floor 済み）
    bool used = false;      // フリーリストのスロット状態

    // 頂点配列とインデックス配列から構築する。範囲外インデックスの三角形は捨てる。
    // 成功したら true。三角形が 1 枚も無ければ false。
    bool build(const float* verticesXYZ, int vertexCount, const int* indices, int indexCount) {
        if (verticesXYZ == nullptr || indices == nullptr || vertexCount <= 0 || indexCount < 3)
            return false;

        // ローカル AABB。
        Vec3 lo(verticesXYZ[0], verticesXYZ[1], verticesXYZ[2]);
        Vec3 hi = lo;
        for (int i = 1; i < vertexCount; ++i) {
            const Vec3 v(verticesXYZ[i * 3], verticesXYZ[i * 3 + 1], verticesXYZ[i * 3 + 2]);
            lo = Vec3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
            hi = Vec3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
        }
        localCenter = (lo + hi) * 0.5f;
        localHalfExtents = Vec3(
            std::max((hi.x - lo.x) * 0.5f, kMeshMinHalfExtent),
            std::max((hi.y - lo.y) * 0.5f, kMeshMinHalfExtent),
            std::max((hi.z - lo.z) * 0.5f, kMeshMinHalfExtent));

        const Vec3 inv(1.0f / localHalfExtents.x, 1.0f / localHalfExtents.y,
                       1.0f / localHalfExtents.z);
        auto norm = [&](int vi) {
            const Vec3 v(verticesXYZ[vi * 3], verticesXYZ[vi * 3 + 1], verticesXYZ[vi * 3 + 2]);
            const Vec3 d = v - localCenter;
            return Vec3(d.x * inv.x, d.y * inv.y, d.z * inv.z);
        };

        std::vector<Triangle> tris;
        tris.reserve(static_cast<size_t>(indexCount / 3));
        for (int i = 0; i + 2 < indexCount; i += 3) {
            const int a = indices[i], b = indices[i + 1], c = indices[i + 2];
            if (a < 0 || b < 0 || c < 0 || a >= vertexCount || b >= vertexCount || c >= vertexCount)
                continue;
            tris.push_back(Triangle{norm(a), norm(b), norm(c)});
        }
        if (tris.empty()) return false;

        bvh.build(std::move(tris));
        used = true;
        return true;
    }

    void clear() {
        bvh.build({});
        used = false;
        localCenter = Vec3(0, 0, 0);
        localHalfExtents = Vec3(1, 1, 1);
    }
};

// ── ワールド ⇄ 正規化ローカル ────────────────────────────────
// obbToLocalPoint は軸へ射影するだけ（halfExtents で割らない）ので、ここで割って
// 単位箱の座標系へ落とす。

inline Vec3 meshWorldToLocalPoint(const Vec3& p, const Obb& b) {
    const Vec3 l = obbToLocalPoint(p, b);
    return Vec3(l.x / b.halfExtents.x, l.y / b.halfExtents.y, l.z / b.halfExtents.z);
}

// 方向は**正規化しない**。そうすることでローカル側の媒介変数 t が
// ワールドの距離とそのまま一致し、他のインスタンスとの最近ヒット比較が破綻しない。
inline Vec3 meshWorldToLocalDir(const Vec3& v, const Obb& b) {
    const Vec3 l = obbToLocalDir(v, b);
    return Vec3(l.x / b.halfExtents.x, l.y / b.halfExtents.y, l.z / b.halfExtents.z);
}

// 法線は逆転置で戻す。M = R·S（S は halfExtents の対角）なので (M^-1)^T = R·S^-1。
inline Vec3 meshLocalNormalToWorld(const Vec3& n, const Obb& b) {
    const Vec3 w = b.axisX * (n.x / b.halfExtents.x)
                 + b.axisY * (n.y / b.halfExtents.y)
                 + b.axisZ * (n.z / b.halfExtents.z);
    return normalized(w);
}

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_MESH_GEOM_H
