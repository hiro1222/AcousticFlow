/* Flow/surface_bvh.h ── 面（箱）の BVH
 *
 * ■ 役割
 *   Surfaces の当たり判定（最近ヒットと線分の横切り）を総当たりから木へ変える。
 *   目的は 2 つ。①箱が増えても費用が線形に伸びない ②**GPU へ持っていける形にする**。
 *   GPU では 1 スレッド ＝ レイ 1 本で木を歩く。だから木は「ノードの配列 ＋ 添字の配列」だけで、
 *   ポインタも再帰も持たない。走査のスタックも配列で持つ。
 *
 * ■ 中の仕組み
 *   1) 葉に入るのは **Surfaces の添字**。葉の中の当たり判定は今までと同じ rayIntersectsObb。
 *      ★三角形でなく箱を包む: 新コアの面は箱だけ。三角形が来ても同じ木に混ぜられるよう、
 *        木は「AABB で包める何か」の添字しか知らない作りにしてある。
 *   2) 分割は重心の広がりがいちばん大きい軸で中央値。葉は最大 4 個。
 *   3) 動く箱があるので**毎フレーム作り直す**。数十個なら作り直しでも数 us。
 *   4) 枝は左右のノード番号を両方持つ。左を持って右を「次」と決める書き方もあるが、
 *      その約束は作る順に依存していて、GPU へ写すときに崩れやすい。番号は素直に 2 つ持つ。
 *
 * ■ 繋がり
 *   受ける: Surfaces（箱と active の印）。
 *   渡す:   Surfaces::nearest / transmittance がここを歩く。
 *
 * ■ 退けた書き方
 *   ・Core/bvh.h（三角形の BVH）をそのまま使う: あれは三角形を持つ木で、箱を入れられない。
 *     添字だけを持つ木にすれば、箱でも三角形でも同じ木に乗る。
 *   ・葉で AABB のまま当てる: 箱は回っている（OBB）ので、AABB で当てると回った箱の角が太る。
 *     枝刈りは AABB、当てるのは OBB。
 *   ・木を持ったまま箱を動かす: 位置が変わったのに包みが古いと、当たるべき箱を枝刈りで捨てる。
 *
 * ■ 壊れる所
 *   ・active でない箱を木に入れると、消したはずの物に当たる。構築時に外す。
 *   ・葉を回る順が総当たりと違うので、**まったく同じ t の箱が 2 つある**と勝者が変わりうる。
 *     判定は t < best.t の厳密比較のままにしてある（同点は起きない前提）。
 *   ・横切りの積は掛ける順で末尾が変わる。呼び出し側が添字の昇順に並べ直してから掛ける。
 */
#ifndef ACOUSTICFLOW_FLOW_SURFACE_BVH_H
#define ACOUSTICFLOW_FLOW_SURFACE_BVH_H

#include <algorithm>
#include <vector>

#include "Core/aabb.h"
#include "Core/vec3.h"

namespace acoustic {
namespace flow {

/// 回った箱（OBB）を包む軸並行の箱。8 隅を回すだけ。
///   ★room_graph.h に同じ物があるが、そこを取り込むと部屋グラフごと引き込むので、ここに置く。
inline Aabb obbAabb(const Obb& b) {
    Aabb r;
    r.min = Vec3( 1e30f,  1e30f,  1e30f);
    r.max = Vec3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sy = (i & 2) ? 1.0f : -1.0f;
        const float sz = (i & 4) ? 1.0f : -1.0f;
        const Vec3 p = b.center + b.axisX * (b.halfExtents.x * sx)
                                + b.axisY * (b.halfExtents.y * sy)
                                + b.axisZ * (b.halfExtents.z * sz);
        r.min = Vec3(std::min(r.min.x, p.x), std::min(r.min.y, p.y), std::min(r.min.z, p.z));
        r.max = Vec3(std::max(r.max.x, p.x), std::max(r.max.y, p.y), std::max(r.max.z, p.z));
    }
    return r;
}

class SurfaceBvh {
public:
    struct Node {
        Aabb bounds;
        int  left = -1, right = -1;   // 枝のとき。葉なら −1
        int  first = 0, count = 0;    // 葉のとき items_ の範囲
    };

    void clear() { nodes_.clear(); items_.clear(); }
    bool empty() const { return nodes_.empty(); }
    int  nodeCount() const { return static_cast<int>(nodes_.size()); }
    int  itemCount() const { return static_cast<int>(items_.size()); }

    /// bounds(i) が i 番の AABB、keep(i) が false の物は入れない。
    template <class BoundsFn, class KeepFn>
    void build(int n, BoundsFn bounds, KeepFn keep) {
        clear();
        items_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            if (!keep(i)) continue;
            Item it; it.idx = i; it.box = bounds(i);
            it.cent = (it.box.min + it.box.max) * 0.5f;
            items_.push_back(it);
        }
        if (items_.empty()) return;
        nodes_.reserve(items_.size() * 2 + 1);
        buildRange(0, static_cast<int>(items_.size()));
    }

    /// レイで歩く。葉の中の添字ごとに leaf(i) を呼ぶ。tMax は呼び出し側が縮めてよい。
    template <class LeafFn>
    void traverseRay(const Vec3& origin, const Vec3& dir, const float& tMax, LeafFn leaf) const {
        if (nodes_.empty()) return;
        int stack[64]; int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& nd = nodes_[static_cast<std::size_t>(stack[--sp])];
            float t; Vec3 nrm;
            if (!rayIntersectsAabb(origin, dir, nd.bounds, tMax, t, nrm)) continue;
            if (nd.count > 0) {
                for (int k = 0; k < nd.count; ++k) leaf(items_[static_cast<std::size_t>(nd.first + k)].idx);
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left;
                stack[sp++] = nd.right;
            }
        }
    }

private:
    struct Item { int idx = -1; Aabb box; Vec3 cent; };

    int buildRange(int lo, int hi) {
        const int me = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{});
        Aabb b = items_[static_cast<std::size_t>(lo)].box;
        for (int k = lo + 1; k < hi; ++k) {
            const Aabb& c = items_[static_cast<std::size_t>(k)].box;
            b.min = Vec3(std::min(b.min.x, c.min.x), std::min(b.min.y, c.min.y), std::min(b.min.z, c.min.z));
            b.max = Vec3(std::max(b.max.x, c.max.x), std::max(b.max.y, c.max.y), std::max(b.max.z, c.max.z));
        }
        const int n = hi - lo;
        if (n <= 4) {
            Node& nd = nodes_[static_cast<std::size_t>(me)];
            nd.bounds = b; nd.first = lo; nd.count = n; nd.left = nd.right = -1;
            return me;
        }
        Vec3 cmin = items_[static_cast<std::size_t>(lo)].cent, cmax = cmin;
        for (int k = lo + 1; k < hi; ++k) {
            const Vec3& c = items_[static_cast<std::size_t>(k)].cent;
            cmin = Vec3(std::min(cmin.x, c.x), std::min(cmin.y, c.y), std::min(cmin.z, c.z));
            cmax = Vec3(std::max(cmax.x, c.x), std::max(cmax.y, c.y), std::max(cmax.z, c.z));
        }
        const Vec3 ext = cmax - cmin;
        const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : ((ext.y >= ext.z) ? 1 : 2);
        const int mid = lo + n / 2;
        std::nth_element(items_.begin() + lo, items_.begin() + mid, items_.begin() + hi,
                         [axis](const Item& a, const Item& c) {
                             return (axis == 0) ? (a.cent.x < c.cent.x)
                                  : (axis == 1) ? (a.cent.y < c.cent.y)
                                                : (a.cent.z < c.cent.z);
                         });
        const int l = buildRange(lo, mid);
        const int r = buildRange(mid, hi);
        Node& nd = nodes_[static_cast<std::size_t>(me)];
        nd.bounds = b; nd.left = l; nd.right = r; nd.count = 0;
        return me;
    }

    std::vector<Node> nodes_;
    std::vector<Item> items_;
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_SURFACE_BVH_H
