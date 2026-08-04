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
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Core/aabb.h"
#include "Core/bvh.h"
#include "Core/triangle.h"
#include "Core/vec3.h"

namespace acoustic {

// 潰れた軸（平面メッシュなど）で 0 除算しないための最小半径。
// 板を置いたときに名目上の厚みとして働き、実際の厚みはインスタンスの halfExtents が決める。
constexpr float kMeshMinHalfExtent = 1e-3f;

// 回折しうる稜線（正規化ローカル空間）。
//   隣り合う 2 三角形が平面（二面角 ≈ 180°）なら、その稜線では回折が起きない。
//   候補になるのは「二面角が有意に折れている稜線」と「境界稜線（三角形が片側にしかない）」だけ。
//   これで**候補数がテッセレーションに依存しなくなる**（壁を 10 倍細分しても稜線本数は変わらない）。
struct DiffractionEdgeLocal {
    Vec3 a, b;   // 稜線の両端（正規化ローカル）
};

struct MeshGeometry {
    TriangleBvh bvh;        // 正規化ローカル空間（AABB = [-1,1]^3）で構築
    std::vector<DiffractionEdgeLocal> edges;  // 回折稜線（同上）
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

        buildDiffractionEdges(tris);
        bvh.build(std::move(tris));
        used = true;
        return true;
    }

    void clear() {
        bvh.build({});
        edges.clear();
        used = false;
        localCenter = Vec3(0, 0, 0);
        localHalfExtents = Vec3(1, 1, 1);
    }

private:
    // 二面角でフィルタした回折稜線を抽出する。
    //
    //   ★頂点は**位置で溶接**する。インデックスで隣接を取ってはいけない。
    //     DCC やエンジンが出すメッシュは、ハードエッジや UV 継ぎ目で頂点を分割している。
    //     つまり**まさに回折したい稜線ほど、インデックスが別物になっている**。
    //     位置で溶接しないと、箱の角のような最も重要な稜線が 1 本も見つからない。
    void buildDiffractionEdges(const std::vector<Triangle>& tris) {
        edges.clear();
        const size_t nt = tris.size();
        if (nt == 0 || nt > 200000) return;   // 病的なメッシュは諦める（回折はプロキシに任せる）

        // 位置 → 溶接インデックス。正規化ローカルなので座標は [-1,1]、1e-4 で量子化すれば十分。
        //   線形探索だと O(n²) で実メッシュが固まるのでハッシュを使う。
        std::unordered_map<uint64_t, int> weldMap;
        std::vector<Vec3> pos;
        auto weld = [&](const Vec3& p) {
            const int64_t qx = std::lround(p.x * 10000.0f);
            const int64_t qy = std::lround(p.y * 10000.0f);
            const int64_t qz = std::lround(p.z * 10000.0f);
            const uint64_t key = (static_cast<uint64_t>(qx + 100000) * 1000003ull
                                + static_cast<uint64_t>(qy + 100000)) * 1000033ull
                                + static_cast<uint64_t>(qz + 100000);
            auto it = weldMap.find(key);
            if (it != weldMap.end()) return it->second;
            const int id = static_cast<int>(pos.size());
            weldMap.emplace(key, id);
            pos.push_back(p);
            return id;
        };

        // 溶接インデックスの対 → 隣接する三角形の法線（最大2枚ぶん）。
        struct EdgeRec { int v0, v1; Vec3 n0, n1; int count; };
        std::vector<EdgeRec> recs;
        std::unordered_map<uint64_t, int> edgeMap;
        auto addEdge = [&](int a, int b, const Vec3& n) {
            if (a > b) std::swap(a, b);
            const uint64_t key = static_cast<uint64_t>(a) * 4294967311ull + static_cast<uint64_t>(b);
            auto it = edgeMap.find(key);
            if (it != edgeMap.end()) {
                EdgeRec& e = recs[static_cast<size_t>(it->second)];
                if (e.count == 1) e.n1 = n;
                ++e.count;
                return;
            }
            edgeMap.emplace(key, static_cast<int>(recs.size()));
            recs.push_back(EdgeRec{a, b, n, Vec3(0, 0, 0), 1});
        };

        for (const Triangle& t : tris) {
            const int i0 = weld(t.v0), i1 = weld(t.v1), i2 = weld(t.v2);
            if (i0 == i1 || i1 == i2 || i2 == i0) continue;   // 退化三角形
            const Vec3 n = normalized(cross(t.v1 - t.v0, t.v2 - t.v0));
            addEdge(i0, i1, n);
            addEdge(i1, i2, n);
            addEdge(i2, i0, n);
        }

        // 二面角が平坦なものを捨てる。境界稜線（片側にしか三角形が無い＝穴の縁・板の縁）は残す。
        constexpr float kFlatCos = 0.985f;   // ≒10°。これ未満の折れは回折に効かない
        std::vector<std::pair<int, int>> kept;
        for (const EdgeRec& e : recs) {
            bool keep = (e.count == 1);                       // 境界稜線
            if (!keep && e.count >= 2)
                keep = (dot(e.n0, e.n1) < kFlatCos);          // 有意に折れている
            if (keep) kept.emplace_back(e.v0, e.v1);
        }

        // ★共線の隣接セグメントを併合する。
        //   これをやらないと「テッセレーション非依存」が成り立たない ── 面を細分すると
        //   長い稜線が細切れになり、本数が分割数に比例して増えてしまう（実測 8→32→128）。
        //   物理的には 1 本の稜線なので、繋いで最長の線分にする。
        std::vector<std::vector<int>> incident(pos.size());
        for (size_t i = 0; i < kept.size(); ++i) {
            incident[static_cast<size_t>(kept[i].first)].push_back(static_cast<int>(i));
            incident[static_cast<size_t>(kept[i].second)].push_back(static_cast<int>(i));
        }
        auto dirOf = [&](int ei) { return normalized(pos[kept[ei].second] - pos[kept[ei].first]); };
        constexpr float kColinearCos = 0.9995f;   // ≒1.8°

        // 頂点 v から、稜線 ei と共線に続く唯一の稜線を返す（無ければ -1）。
        //   分岐している頂点（3本以上）では繋がない ── そこは形状の変わり目なので。
        auto nextAt = [&](int v, int ei) {
            if (incident[static_cast<size_t>(v)].size() != 2) return -1;
            const int other = (incident[static_cast<size_t>(v)][0] == ei)
                            ? incident[static_cast<size_t>(v)][1]
                            : incident[static_cast<size_t>(v)][0];
            if (std::fabs(dot(dirOf(ei), dirOf(other))) < kColinearCos) return -1;
            return other;
        };

        std::vector<bool> visited(kept.size(), false);
        for (size_t i = 0; i < kept.size(); ++i) {
            if (visited[i]) continue;
            visited[i] = true;
            int endA = kept[i].first, endB = kept[i].second;
            // 両端へ伸ばせるだけ伸ばす。
            for (int side = 0; side < 2; ++side) {
                int cur = static_cast<int>(i);
                int tip = side ? endB : endA;
                for (;;) {
                    const int nxt = nextAt(tip, cur);
                    if (nxt < 0 || visited[static_cast<size_t>(nxt)]) break;
                    visited[static_cast<size_t>(nxt)] = true;
                    tip = (kept[static_cast<size_t>(nxt)].first == tip)
                        ? kept[static_cast<size_t>(nxt)].second
                        : kept[static_cast<size_t>(nxt)].first;
                    cur = nxt;
                }
                if (side) endB = tip; else endA = tip;
            }
            edges.push_back(DiffractionEdgeLocal{pos[static_cast<size_t>(endA)],
                                                 pos[static_cast<size_t>(endB)]});
        }
    }

public:
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

// 正規化ローカル → ワールド（obb がそのまま写像になっている）。
inline Vec3 meshLocalToWorldPoint(const Vec3& l, const Obb& b) {
    return b.center + b.axisX * (l.x * b.halfExtents.x)
                    + b.axisY * (l.y * b.halfExtents.y)
                    + b.axisZ * (l.z * b.halfExtents.z);
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
