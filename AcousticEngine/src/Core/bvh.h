/* Core/bvh.h
 * 三角形の BVH（Bounding Volume Hierarchy）。メッシュ occluder の高速化。
 *
 * 構築: 三角形の重心を最長軸で中央値分割する素朴な BVH（葉は最大4三角形）。
 *       構築は一度きり（静的ジオメトリ前提。毎フレーム作り直さない）。
 * 走査: ノード AABB をスラブ法(aabb.h)で枝刈りしつつ、葉の三角形を総当り。
 *   - raycast        : 最近ヒット（反射・next-event の土台）
 *   - occludes       : 線分が1つでも三角形に当たるか（二値遮蔽）
 *   - countCrossings : 線分が横切る三角形の数（透過の掛け合わせ回数）
 *
 * ヘッダオンリー（CMake のソース一覧を触らずに済むように）。
 */
#ifndef ACOUSTICFLOW_CORE_BVH_H
#define ACOUSTICFLOW_CORE_BVH_H

#include <algorithm>
#include <vector>

#include "Core/aabb.h"
#include "Core/triangle.h"
#include "Core/vec3.h"

namespace acoustic {

class TriangleBvh {
public:
    // 三角形列を受け取り BVH を構築する（ムーブで取り込む）。
    void build(std::vector<Triangle> tris) {
        tris_ = std::move(tris);
        nodes_.clear();
        index_.clear();
        const int n = static_cast<int>(tris_.size());
        if (n == 0) return;
        index_.resize(n);
        for (int i = 0; i < n; ++i) index_[i] = i;
        nodes_.reserve(static_cast<size_t>(2 * n + 1));  // 予約して再確保を防ぐ（参照安定）
        buildRecursive(0, n);
    }

    bool empty() const { return tris_.empty(); }
    int triangleCount() const { return static_cast<int>(tris_.size()); }

    // 最近ヒット。ヒットしたら outT/outNormal を更新して true。
    // maxDist は探索上限（呼び出し側の現 best 距離）。
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist,
                 float& outT, Vec3& outNormal) const {
        if (nodes_.empty()) return false;
        bool hit = false;
        float best = maxDist;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& nd = nodes_[stack[--sp]];
            float bt;
            Vec3 bn;
            if (!rayIntersectsAabb(origin, dir, nd.box, best, bt, bn)) continue;
            if (bt > best) continue;  // ノード入口が現 best より遠い＝枝刈り
            if (nd.count > 0) {
                for (int i = 0; i < nd.count; ++i) {
                    const Triangle& tri = tris_[index_[nd.start + i]];
                    float t;
                    Vec3 nrm;
                    if (rayIntersectsTriangle(origin, dir, tri, best, t, nrm) && t < best) {
                        best = t;
                        outT = t;
                        outNormal = nrm;
                        hit = true;
                    }
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left;
                stack[sp++] = nd.right;
            }
        }
        return hit;
    }

    // 線分 p0->p1 が三角形に1つでも当たるか（any-hit で早期 return）。
    bool occludes(const Vec3& p0, const Vec3& p1) const {
        if (nodes_.empty()) return false;
        const Vec3 d = p1 - p0;
        const float len = length(d);
        if (len < 1e-9f) return false;
        const Vec3 dir = d * (1.0f / len);
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& nd = nodes_[stack[--sp]];
            float bt;
            Vec3 bn;
            if (!rayIntersectsAabb(p0, dir, nd.box, len, bt, bn)) continue;
            if (nd.count > 0) {
                for (int i = 0; i < nd.count; ++i) {
                    const Triangle& tri = tris_[index_[nd.start + i]];
                    float t;
                    Vec3 nrm;
                    if (rayIntersectsTriangle(p0, dir, tri, len, t, nrm)) return true;
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left;
                stack[sp++] = nd.right;
            }
        }
        return false;
    }

    // 線分 p0->p1 が横切る三角形の数（透過の掛け合わせ回数用）。
    int countCrossings(const Vec3& p0, const Vec3& p1) const {
        if (nodes_.empty()) return 0;
        const Vec3 d = p1 - p0;
        const float len = length(d);
        if (len < 1e-9f) return 0;
        const Vec3 dir = d * (1.0f / len);
        int crossings = 0;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& nd = nodes_[stack[--sp]];
            float bt;
            Vec3 bn;
            if (!rayIntersectsAabb(p0, dir, nd.box, len, bt, bn)) continue;
            if (nd.count > 0) {
                for (int i = 0; i < nd.count; ++i) {
                    const Triangle& tri = tris_[index_[nd.start + i]];
                    float t;
                    Vec3 nrm;
                    if (rayIntersectsTriangle(p0, dir, tri, len, t, nrm)) ++crossings;
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left;
                stack[sp++] = nd.right;
            }
        }
        return crossings;
    }

private:
    struct Node {
        Aabb box;
        int start = 0;   // 葉: index_ 内の開始位置
        int count = 0;   // 葉: 三角形数（0 = 内部ノード）
        int left = -1;   // 内部: 左子ノードのインデックス
        int right = -1;  // 内部: 右子ノードのインデックス
    };

    // index_[start,end) を分割してノードを作り、そのノードのインデックスを返す。
    int buildRecursive(int start, int end) {
        const int nodeIdx = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{});  // スロットを確保（reserve 済みで再確保されない）

        // 全体 bounds と重心 bounds を求める。
        Aabb bounds = triBounds(index_[start]);
        Vec3 c0 = triCentroid(index_[start]);
        Vec3 cmin = c0;
        Vec3 cmax = c0;
        for (int i = start + 1; i < end; ++i) {
            bounds = mergeAabb(bounds, triBounds(index_[i]));
            const Vec3 c = triCentroid(index_[i]);
            cmin = minVec(cmin, c);
            cmax = maxVec(cmax, c);
        }

        Node nd;
        nd.box = bounds;
        const int count = end - start;
        const int kLeaf = 4;

        const Vec3 ext = cmax - cmin;
        const bool degenerate = (ext.x + ext.y + ext.z) < 1e-9f;
        if (count <= kLeaf || degenerate) {
            nd.start = start;
            nd.count = count;
            nodes_[nodeIdx] = nd;
            return nodeIdx;
        }

        // 最長軸で中央値分割。
        const int axis = (ext.x > ext.y && ext.x > ext.z) ? 0 : (ext.y > ext.z ? 1 : 2);
        const int mid = (start + end) / 2;
        std::nth_element(index_.begin() + start, index_.begin() + mid, index_.begin() + end,
                         [&](int a, int b) {
                             return axisVal(triCentroid(a), axis) < axisVal(triCentroid(b), axis);
                         });

        const int leftChild = buildRecursive(start, mid);
        const int rightChild = buildRecursive(mid, end);
        nd.count = 0;
        nd.left = leftChild;
        nd.right = rightChild;
        nodes_[nodeIdx] = nd;
        return nodeIdx;
    }

    Aabb triBounds(int triIdx) const {
        const Triangle& t = tris_[triIdx];
        Aabb b;
        b.min = Vec3(std::min(std::min(t.v0.x, t.v1.x), t.v2.x),
                     std::min(std::min(t.v0.y, t.v1.y), t.v2.y),
                     std::min(std::min(t.v0.z, t.v1.z), t.v2.z));
        b.max = Vec3(std::max(std::max(t.v0.x, t.v1.x), t.v2.x),
                     std::max(std::max(t.v0.y, t.v1.y), t.v2.y),
                     std::max(std::max(t.v0.z, t.v1.z), t.v2.z));
        return b;
    }

    Vec3 triCentroid(int triIdx) const {
        const Triangle& t = tris_[triIdx];
        return Vec3((t.v0.x + t.v1.x + t.v2.x) / 3.0f,
                    (t.v0.y + t.v1.y + t.v2.y) / 3.0f,
                    (t.v0.z + t.v1.z + t.v2.z) / 3.0f);
    }

    static float axisVal(const Vec3& v, int axis) {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    }
    static Vec3 minVec(const Vec3& a, const Vec3& b) {
        return Vec3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
    }
    static Vec3 maxVec(const Vec3& a, const Vec3& b) {
        return Vec3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
    }
    static Aabb mergeAabb(const Aabb& a, const Aabb& b) {
        Aabb r;
        r.min = minVec(a.min, b.min);
        r.max = maxVec(a.max, b.max);
        return r;
    }

    std::vector<Triangle> tris_;
    std::vector<int> index_;
    std::vector<Node> nodes_;
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_BVH_H
