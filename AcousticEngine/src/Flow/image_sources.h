/* Flow/image_sources.h ── 虚像法 ISM（段 7）
 *
 * ■ 役割
 *   初期反射の**方向と正規化重み**を出す。量は持たない（設計文書 Ⅵ「ISM は音量を持たない」）。
 *   各虚像のエネルギー = レイの初期の総量 × ここの正規化重み（distribute が掛ける）。
 *   ASW（音源依存の広がり）の材料。方向バスのレーンへ流れる。
 *
 * ■ 中の仕組み
 *   1) 面: 静的な箱の 6 面（外向き法線、矩形）。音源とリスナーが同じ側にある面だけが候補。
 *   2) 虚像: 1 次は面で音源を鏡映、2 次は 1 次の虚像をもう 1 面で鏡映。経路長が c × (mixingSec + 余裕) を超える物は捨てる
 *      （それより後は FDN が受け持つ。「FDN のエコー密度が十分になるまでを ISM が埋める」）。
 *   3) **妥当性は 0/1 でなく割合**（地図 7 章「ISM の妥当性を面の可視率に」）:
 *      虚像のまわりに円盤（半径は音源の実効の幅、下限 15 cm）を置き、リスナーから円盤への線が
 *        ・鏡映した面の矩形の中を通り（窓）、
 *        ・展開した経路の各脚（L→Q2、Q2→Q1、Q1→S）が他の箱に遮られない
 *      割合を、開口と同じ走査線＋二分探索＋erf で出す。
 *      ★点の虚像は面の縁で 0/1 に跳ぶ（旧実装で歩行の揺れが 4〜7.6 倍）。割合なら縁で連続に消える（1.1〜1.5 倍）。
 *   4) 重み_i,b = 割合_i × Π(面の反射率_b) / d_i²、帯域ごとに Σ_i = 1 に正規化。
 *      距離と累積反射率（設計文書「重みは距離 → 距離＋累積反射率」）。正規化は連続（和が正のあいだ）。
 *   5) 尾の開始の材料: 有効な虚像の最短の到達（ITDG。地図 7 章）。
 *
 * ■ 繋がり
 *   受ける: Surfaces、MaterialTable、Listener、音源の位置と幅、mixingSec。
 *   渡す:   ImageSet（虚像ごとの位置・到達・帯域別の正規化重み）を distribute へ。
 *
 * ■ 退けた書き方
 *   ・BVH の全面（三角形）で虚像を作る: 面が増えると 2 次の虚像が爆発する。箱の面だけ、同じ側だけ、経路長で切る。
 *   ・妥当性を反射点が矩形の中か（0/1）で決める: 上記の跳び。
 *   ・虚像にエネルギーを持たせてレイと足す: 同じ反射を二重に数える。ここは配分の重みだけ。
 *   ・虚像の円盤の半径を音源の幅そのまま: 遠い音源は見込み角で点に落ちるので、虚像が 0/1 に戻って歩くと跳ぶ。
 *     ISM の円盤は下限 15 cm を持つ（費用は走査線 5 本で小さい）。
 *
 * ■ 壊れる所
 *   ・面の「同じ側」を忘れると、壁の裏側の面で虚像ができて壁越しに反射が鳴る。
 *   ・2 次の展開で脚の順を間違える（Q1 と Q2）と、遮蔽の判定が別の空間で行われて何でも通る。
 *   ・正規化の和が 0（全部無効）のとき割ると NaN。その時は count=0 で返し、distribute が方向なしの 1 本に落とす。
 *   ・経路長の上限を mixingSec ちょうどにすると、境の虚像が出たり消えたりして重みが揺れる。余裕を 3 m 持つ。
 */
#ifndef ACOUSTICFLOW_FLOW_IMAGE_SOURCES_H
#define ACOUSTICFLOW_FLOW_IMAGE_SOURCES_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/vec3.h"
#include "Flow/aperture.h"
#include "Flow/emitter.h"
#include "Flow/surfaces.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

struct Face {
    int   box = -1;
    Vec3  center{0, 0, 0}, normal{0, 0, 0}, axisU{0, 0, 0}, axisV{0, 0, 0};
    float halfU = 0.0f, halfV = 0.0f;
    float reflect6[kNumBands] = {};
    float scatter1k = 0.5f;             // 1 kHz の散乱率（虚像の面音源の幅に使う）
};

struct ImageSource {
    Vec3  pos{0, 0, 0};                 // 虚像の位置（world）
    float pathSec = 0.0f;               // 到達（秒）
    float dist = 0.0f;
    float weight6[kNumBands] = {};      // 正規化した重み（Σ_i = 1）
    float validity = 0.0f;              // 見える割合 0..1
    int   order = 0;
    static constexpr int kMaxOrder = 3;
    int   face[kMaxOrder] = {-1, -1, -1};   // 経路の面（**リスナー側から**）。使うのは order 本だけ
};

struct ImageSet {
    static constexpr int kMaxImages = 48;
    ImageSource img[kMaxImages];
    int   count = 0;
    float firstSec = -1.0f;             // 有効な虚像の最短の到達
    int   candidates = 0;               // 検討した虚像の数（費用の目安）
    // ★方向の分かっている割合（帯域別 0..1）＝ 実際に通っている鏡面経路 ÷ 通れば届くはずの鏡面経路。
    //   正規化重みは Σ=1 なので「どの虚像へ」しか言えない。「そもそも鏡面で届いているのか」は
    //   この割合が持つ。全部が僅かにしか見えていない（妥当 0.0004）ときに重み 0.32 が立つ、
    //   という段 7 の穴をここで塞ぐ。残りは方向なしの初期タップへ回る（distribute）。
    float directional6[kNumBands] = {};
};

/// 静的な箱の面を並べる。
/// 虚像のタップの素性（面の組で決まる。並び順に依らない）。面の番号が 127 を超えたら −1（遅延で繋ぐ）。
///   distribute の ISM の道と、受取面で虚像をつなぐ道（receiver.h）で同じ式を使う ── 切り替えても同じタップとして繋がる。
inline int imageTapId(const ImageSource& src) {
    const int f0 = src.face[0] + 1, f1 = src.face[1] + 1, f2 = src.face[2] + 1;
    return (f0 < 128 && f1 < 128 && f2 < 128) ? (16 + f0 + f1 * 128 + f2 * 16384) : -1;
}

inline void collectFaces(const Surfaces& surf, const MaterialTable& mats, std::vector<Face>& out) {
    out.clear();
    for (int i = 0; i < surf.count(); ++i) {
        const Surface& s = surf.at(i);
        if (!s.active || s.dynamic) continue;
        const Obb& b = s.obb;
        const Vec3 ax[3] = {b.axisX, b.axisY, b.axisZ};
        const float he[3] = {b.halfExtents.x, b.halfExtents.y, b.halfExtents.z};
        const AcousticMaterial& m = mats.get(s.material);
        for (int a = 0; a < 3; ++a) for (int sgn = -1; sgn <= 1; sgn += 2) {
            Face f; f.box = i;
            f.normal = ax[a] * static_cast<float>(sgn);
            f.center = b.center + f.normal * he[a];
            f.axisU = ax[(a + 1) % 3]; f.axisV = ax[(a + 2) % 3];
            f.halfU = he[(a + 1) % 3]; f.halfV = he[(a + 2) % 3];
            for (int k = 0; k < kNumBands; ++k) f.reflect6[k] = splitAt(m, k).reflect;
            f.scatter1k = std::min(1.0f, std::max(0.0f, m.scattering[3]));
            out.push_back(f);
        }
    }
}

namespace detail {
inline Vec3 mirror(const Vec3& p, const Face& f) { return p - f.normal * (2.0f * dot(p - f.center, f.normal)); }
inline float side(const Vec3& p, const Face& f) { return dot(p - f.center, f.normal); }
/// 線分 a→b と面の平面の交点。矩形の中なら true。
inline bool hitRect(const Vec3& a, const Vec3& b, const Face& f, Vec3& q) {
    const float da = side(a, f), db = side(b, f);
    if ((da > 0.0f) == (db > 0.0f)) return false;
    const float t = da / (da - db);
    q = a + (b - a) * t;
    const Vec3 d = q - f.center;
    return std::fabs(dot(d, f.axisU)) <= f.halfU && std::fabs(dot(d, f.axisV)) <= f.halfV;
}
/// 脚が他の箱に遮られていないか（owner の箱は除く）。
inline bool legClear(const Surfaces& surf, const Vec3& a, const Vec3& b, int owner1, int owner2) {
    for (int j = 0; j < surf.count(); ++j) {
        if (j == owner1 || j == owner2 || !surf.at(j).active) continue;
        if (segmentObbPenetration(a, b, surf.at(j).obb) > 0.02f) return false;
    }
    return true;
}
/// リスナー L から点 P（虚像のまわりの点）への線が、経路（面 f2 → 面 f1 → 音源 S）として通るか。order 1 なら f1 だけ。
/// 面の鎖（chain[0] がリスナー側、chain[n-1] が音源側）で 1..3 次をまとめて扱う。
///   ★次数ごとに書き分けると、脚の除外（自分の箱と 1 つ前の箱）を間違える。
///     旧実装の 2 次専用版はここを手で書いていて、3 次を足すときに同じ間違いを繰り返す形だった。
///   毎段: リスナー側から見て窓（矩形）を通るか → その脚が他の箱に遮られていないか → 1 段展開を戻す。
inline bool pathOpen(const Surfaces& surf, const Vec3& L, const Vec3& P, const Vec3& S,
                     const Face* const* chain, int n) {
    Vec3 cur = L, pt = P;
    int prevBox = -1;
    for (int k = 0; k < n; ++k) {
        Vec3 q;
        if (!hitRect(cur, pt, *chain[k], q)) return false;             // 窓
        if (!legClear(surf, cur, q, chain[k]->box, prevBox)) return false;
        pt = mirror(pt, *chain[k]);                                     // 展開を 1 段戻す
        prevBox = chain[k]->box;
        cur = q;
    }
    return legClear(surf, cur, S, prevBox, -1);
}
/// 虚像 C のまわりの円盤（半径 r）のうち、経路として通る割合（走査線＋二分探索＋erf。aperture と同じ物差し）。
inline float imageVisibility(const Surfaces& surf, const Vec3& L, const Vec3& C, float r, const Vec3& S,
                             const Face* const* chain, int nChain, int lines = 5, int coarse = 8) {
    const Vec3 toL = L - C;
    const float dist = length(toL);
    if (dist <= kEps) return 0.0f;
    if (r <= 1e-4f) return pathOpen(surf, L, C, S, chain, nChain) ? 1.0f : 0.0f;
    const Vec3 n = toL * (1.0f / dist);
    const Vec3 t = (std::fabs(n.y) < 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 U = normalized(cross(t, n)), V = cross(n, U);
    const double sigma = r * 0.5;
    double num = 0.0, den = 0.0;
    for (int k = 0; k < lines; ++k) {
        const double y = -r + (k + 0.5) * (2.0 * r / lines);
        const double halfW = std::sqrt(std::max(0.0, static_cast<double>(r) * r - y * y));
        if (halfW <= 1e-6) continue;
        const double wy = std::exp(-y * y / (2.0 * sigma * sigma));
        const Vec3 O = C + V * static_cast<float>(y);
        const double total = gaussCdf(halfW, sigma) - gaussCdf(-halfW, sigma);
        auto open = [&](double x) { return pathOpen(surf, L, O + U * static_cast<float>(x), S, chain, nChain); };
        const int np = coarse + 1;
        double xs[64]; bool ok[64];
        for (int j = 0; j < np; ++j) { xs[j] = -halfW + 2.0 * halfW * j / (np - 1); ok[j] = open(xs[j]); }
        double lit = 0.0;
        double a = ok[0] ? -halfW : 0.0; bool inside = ok[0];
        for (int j = 0; j + 1 < np; ++j) {
            if (ok[j] == ok[j + 1]) continue;
            double x0 = xs[j], x1 = xs[j + 1]; const bool o0 = ok[j];
            for (int it = 0; it < 10; ++it) { const double xm = 0.5 * (x0 + x1); if (open(xm) == o0) x0 = xm; else x1 = xm; }
            const double xb = 0.5 * (x0 + x1);
            if (inside) { lit += gaussCdf(xb, sigma) - gaussCdf(a, sigma); inside = false; }
            else { a = xb; inside = true; }
        }
        if (inside) lit += gaussCdf(halfW, sigma) - gaussCdf(a, sigma);
        num += wy * lit; den += wy * total;
    }
    return (den > 0.0) ? static_cast<float>(std::min(1.0, std::max(0.0, num / den))) : 0.0f;
}
}  // namespace detail

/// 虚像を作る。rDisc は音源の実効の幅（下限 15 cm はここで掛ける）。maxPathSec を超える経路は捨てる。
inline void buildImages(const Surfaces& surf, const std::vector<Face>& faces, const Listener& listener, const Vec3& S,
                        float rDisc, float maxPathSec, ImageSet& out, int maxOrder = 2) {
    out.count = 0; out.firstSec = -1.0f; out.candidates = 0;
    const Vec3 L = listener.pos;
    const float r = std::max(rDisc, 0.15f);
    const float maxLen = maxPathSec * kSpeedOfSound;
    const int mo = std::min(ImageSource::kMaxOrder, std::max(1, maxOrder));
    double sumW[kNumBands] = {}, sumFull[kNumBands] = {};
    // chain[0] がリスナー側、chain[order-1] が音源側。鏡映は音源側から順に掛けるので、
    // 作る順（f1, f2, f3）と鎖の順は**逆**になる。ここを取り違えると窓の判定が別の空間で行われる。
    auto consider = [&](const Vec3& C, const Face* const* mk, int order) {
        ++out.candidates;
        const float d = length(C - L);
        if (d <= kEps || d > maxLen) return;
        const Face* chain[ImageSource::kMaxOrder];
        for (int k = 0; k < order; ++k) chain[k] = mk[order - 1 - k];
        // 「通れば届くはずの量」は妥当性より先に数える（満室で切るときも数える）。
        //   ここを数えないと割合がいつも 1 になり、見えていない虚像が満額で鳴る。
        float pot[kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
            pot[b] = 1.0f / (d * d);
            for (int k = 0; k < order; ++k) pot[b] *= mk[k]->reflect6[b];
            sumFull[b] += pot[b];
        }
        if (out.count >= ImageSet::kMaxImages) return;
        const float v = detail::imageVisibility(surf, L, C, r, S, chain, order);
        if (v <= 1e-4f) return;
        ImageSource& im = out.img[out.count++];
        im.pos = C; im.dist = d; im.pathSec = d / kSpeedOfSound; im.validity = v; im.order = order;
        for (int k = 0; k < ImageSource::kMaxOrder; ++k)
            im.face[k] = (k < order) ? static_cast<int>(chain[k] - faces.data()) : -1;
        for (int b = 0; b < kNumBands; ++b) { im.weight6[b] = pot[b] * v; sumW[b] += pot[b] * v; }
        if (out.firstSec < 0.0f || im.pathSec < out.firstSec) out.firstSec = im.pathSec;
    };
    // 同じ箱の表裏（厚みを往復する虚像は無意味）を弾く決まり。次数が増えても同じ規則を使う。
    auto sameBoxBackFace = [](const Face& a, const Face& b) {
        return a.box == b.box && dot(a.normal, b.normal) < -0.99f;
    };
    const Face* mk[ImageSource::kMaxOrder];
    // 1 次: 音源とリスナーが同じ側の面
    for (const Face& f1 : faces) {
        if (detail::side(S, f1) <= 0.0f || detail::side(L, f1) <= 0.0f) continue;
        const Vec3 S1 = detail::mirror(S, f1);
        mk[0] = &f1;
        consider(S1, mk, 1);
        if (mo < 2) continue;
        // 2 次: S1 とリスナーが同じ側の別の面
        for (const Face& f2 : faces) {
            if (&f2 == &f1) continue;
            if (detail::side(S1, f2) <= 0.0f || detail::side(L, f2) <= 0.0f) continue;
            if (sameBoxBackFace(f1, f2)) continue;
            const Vec3 S2 = detail::mirror(S1, f2);
            if (length(S2 - L) > maxLen) continue;
            mk[1] = &f2;
            consider(S2, mk, 2);
            if (mo < 3) continue;
            // 3 次: 壁際で「詰まった連続反射」を作るのはここ（同じ壁を繰り返し使う経路）。
            //   ★f3 == f1 を許す。禁じると「壁 → 別の面 → 同じ壁」が消え、
            //     まさに壁際で欲しい繰り返しの経路が出ない。連続する 2 枚が同じ面でなければよい。
            for (const Face& f3 : faces) {
                if (&f3 == &f2) continue;
                if (detail::side(S2, f3) <= 0.0f || detail::side(L, f3) <= 0.0f) continue;
                if (sameBoxBackFace(f2, f3)) continue;
                const Vec3 S3 = detail::mirror(S2, f3);
                if (length(S3 - L) > maxLen) continue;
                mk[2] = &f3;
                consider(S3, mk, 3);
            }
        }
    }
    // 方向の分かっている割合（正規化の前に取る。正規化すると消える量）
    for (int b = 0; b < kNumBands; ++b)
        out.directional6[b] = (sumFull[b] > 1e-30) ? static_cast<float>(std::min(1.0, sumW[b] / sumFull[b])) : 0.0f;
    // 正規化（帯域ごとに Σ = 1）
    for (int b = 0; b < kNumBands; ++b) {
        if (sumW[b] <= 1e-30) { for (int i = 0; i < out.count; ++i) out.img[i].weight6[b] = 0.0f; continue; }
        for (int i = 0; i < out.count; ++i) out.img[i].weight6[b] = static_cast<float>(out.img[i].weight6[b] / sumW[b]);
    }
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_IMAGE_SOURCES_H
