/* Flow/receiver_layout.h ── 受取面の小片の割り付けと、レイが置いていく「受け取り」（2026-09-13）
 *
 * ■ 役割
 *   発注者の案「虚像を音源にするのでなく、面をレイの受取面にする」の、レイの側の半分。
 *   箱の 6 面を一辺 cell（既定 0.5 m）の小片に割り、レイが面に当たるたびに
 *   「どの小片に・音源から何秒で・どれだけ反射して出ていき・どの向きから入ってきたか」を 1 件置いていく。
 *   耳の側の半分（小片 → 耳を解析で配り、面ごとのタップにまとめる）は receiver.h。
 *
 * ■ 中の仕組み
 *   1) 割り付け（PatchLayout）: 箱 i の面 f（軸 a の ± 側）を、残りの 2 軸の半幅を cell で割った nu × nv に割る。
 *      小片の番号 = faceFirst[i*6+f] + iu + iv*nu。無効な箱も席を残す（番号が場面の出入りで動かないように）。
 *      箱の半幅は動かない（Unity の AF_WorldSetBoxTransform は位置と向きだけ）ので、割り付けは箱の数が変わったときだけ作り直す。
 *      小片の位置と向きは、そのフレームの箱（TraceScene の obb）から毎回出す ── 扉の板が動いても番号は同じで、位置だけ付いていく。
 *   2) 受け取り（Deposit）: 音源から当たり点までの時刻 tSec が maxSec（＝ mixing time）より前の当たりだけ置く。
 *      量は「当たる直前のエネルギー × 反射率 × 音源→当たり点の空気」＝ 面から出ていく分。耳までの空気は耳の側で掛ける（空気は距離で掛け算に割れる）。
 *      order は何回目の当たりか（0 が最初）。耳の側で「1 回目の反射」を別のタップにするのに使う ── 壁際の近さはそこに出る。
 *
 * ■ 繋がり
 *   受ける: energy_trace.h の traceRay が当たりごとに pushDeposit を呼ぶ（DepositSink を渡されたときだけ）。
 *   渡す:   World が音源ごと・組ごとに std::vector<Deposit> を持ち、receiver.h が耳へ配ってタップにする。
 *
 * ■ 退けた書き方
 *   ・小片ごとに時刻の箱（0.25 ms × mixing ぶん）を持つ（探り AF_ONLY=receiver の形）: 小片 1908 × 108 × 6 帯域で 1 音源 5 MB。
 *     16 音源で 79 MB になる。受け取りを 1 件ずつ持てば、初期に入る当たりは 1 本あたり数件なので 4096 本でも 1 MB 未満。
 *     しかも耳の距離を足した到達時刻で初期／後期を分けられる（箱の丸めが無い）。
 *   ・面を割らずに面 1 枚を受取面にする: 7 m の壁の明るさのむら（音源の近くが明るい）が消え、向きが面の中心に寄る。
 *   ・当たりの点そのものを持つ（小片に丸めない）: 耳の側で点ごとに見通しを引くことになり、NEE と費用が変わらない。
 *
 * ■ 壊れる所
 *   ・箱の半幅を実行中に変える道を足したら、割り付けを作り直すこと（番号がずれて別の面に量が乗る）。
 *   ・maxSec を mixing time より短くすると、初期の量が足りなくなる（耳までの時間を足す前に切っているので）。
 *   ・当たりを「箱の内側から」数えると、壁の厚みの中で量が増える。置くのは法線の側から当たったときだけ。
 */
#ifndef ACOUSTICFLOW_FLOW_RECEIVER_LAYOUT_H
#define ACOUSTICFLOW_FLOW_RECEIVER_LAYOUT_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/vec3.h"
#include "Flow/trace_scene.h"

namespace acoustic {
namespace flow {

struct PatchLayout {
    float cell = 0.5f;
    int   boxCount = 0;
    int   patchCount = 0;
    std::vector<std::int32_t> faceFirst;     // [箱*6+面] 最初の小片の番号
    std::vector<std::int32_t> faceNu, faceNv;
    std::vector<std::int32_t> faceOfPatch;   // [小片] → 箱*6+面
};

inline Vec3  obbAxisOf(const Obb& o, int a) { return a == 0 ? o.axisX : (a == 1 ? o.axisY : o.axisZ); }
inline float obbHalfOf(const Obb& o, int a) { return a == 0 ? o.halfExtents.x : (a == 1 ? o.halfExtents.y : o.halfExtents.z); }

inline void buildPatchLayout(const TraceScene& sc, float cell, PatchLayout& L) {
    L.cell = std::max(0.05f, cell);
    L.boxCount = sc.boxCount();
    L.faceFirst.assign(static_cast<std::size_t>(L.boxCount) * 6, 0);
    L.faceNu.assign(static_cast<std::size_t>(L.boxCount) * 6, 1);
    L.faceNv.assign(static_cast<std::size_t>(L.boxCount) * 6, 1);
    L.faceOfPatch.clear();
    int next = 0;
    for (int i = 0; i < L.boxCount; ++i) {
        const Obb& o = sc.obb[static_cast<std::size_t>(i)];
        for (int f = 0; f < 6; ++f) {
            const int a = f / 2, au = (a + 1) % 3, av = (a + 2) % 3;
            const int nu = std::max(1, static_cast<int>(std::ceil(2.0f * obbHalfOf(o, au) / L.cell - 1e-4f)));
            const int nv = std::max(1, static_cast<int>(std::ceil(2.0f * obbHalfOf(o, av) / L.cell - 1e-4f)));
            const std::size_t key = static_cast<std::size_t>(i) * 6 + static_cast<std::size_t>(f);
            L.faceFirst[key] = next; L.faceNu[key] = nu; L.faceNv[key] = nv;
            for (int q = 0; q < nu * nv; ++q) L.faceOfPatch.push_back(static_cast<std::int32_t>(key));
            next += nu * nv;
        }
    }
    L.patchCount = next;
}

/// 当たり点 → 小片の番号（無ければ −1）。法線は箱の外向き。lu, lv を渡すと小片の中の位置（0..1）を返す。
inline int patchOfHit(const TraceScene& sc, const PatchLayout& L, int box, const Vec3& point, const Vec3& normal,
                      float* lu = nullptr, float* lv = nullptr) {
    if (box < 0 || box >= L.boxCount) return -1;
    const Obb& o = sc.obb[static_cast<std::size_t>(box)];
    int a = 0; float best = -1.0f;
    for (int q = 0; q < 3; ++q) { const float d = std::fabs(dot(normal, obbAxisOf(o, q))); if (d > best) { best = d; a = q; } }
    const int f = a * 2 + (dot(normal, obbAxisOf(o, a)) > 0.0f ? 0 : 1);
    const std::size_t key = static_cast<std::size_t>(box) * 6 + static_cast<std::size_t>(f);
    const int au = (a + 1) % 3, av = (a + 2) % 3;
    const float hu = obbHalfOf(o, au), hv = obbHalfOf(o, av);
    const Vec3 rel = point - o.center;
    const int nu = L.faceNu[key], nv = L.faceNv[key];
    const float fu = (hu > 1e-6f) ? (dot(rel, obbAxisOf(o, au)) + hu) / (2.0f * hu) : 0.5f;
    const float fv = (hv > 1e-6f) ? (dot(rel, obbAxisOf(o, av)) + hv) / (2.0f * hv) : 0.5f;
    const int iu = std::min(nu - 1, std::max(0, static_cast<int>(std::floor(fu * static_cast<float>(nu)))));
    const int iv = std::min(nv - 1, std::max(0, static_cast<int>(std::floor(fv * static_cast<float>(nv)))));
    if (lu) *lu = std::min(1.0f, std::max(0.0f, fu * static_cast<float>(nu) - static_cast<float>(iu)));
    if (lv) *lv = std::min(1.0f, std::max(0.0f, fv * static_cast<float>(nv) - static_cast<float>(iv)));
    return L.faceFirst[key] + iu + iv * nu;
}

/// 小片の今フレームの形（位置・向き・半幅）。
struct PatchGeom { Vec3 c{0, 0, 0}, n{0, 0, 1}, U{1, 0, 0}, V{0, 1, 0}; float hu = 0.0f, hv = 0.0f, area = 0.0f; int box = -1, face = -1; };

inline PatchGeom patchGeomOf(const TraceScene& sc, const PatchLayout& L, int patch) {
    PatchGeom g;
    if (patch < 0 || patch >= L.patchCount) return g;
    const int key = L.faceOfPatch[static_cast<std::size_t>(patch)];
    const int box = key / 6, f = key % 6;
    const Obb& o = sc.obb[static_cast<std::size_t>(box)];
    const int a = f / 2, au = (a + 1) % 3, av = (a + 2) % 3;
    const float sgn = (f % 2 == 0) ? 1.0f : -1.0f;
    const int nu = L.faceNu[static_cast<std::size_t>(key)], nv = L.faceNv[static_cast<std::size_t>(key)];
    const int local = patch - L.faceFirst[static_cast<std::size_t>(key)];
    const int iu = local % nu, iv = local / nu;
    const float hu = obbHalfOf(o, au), hv = obbHalfOf(o, av);
    const float su = 2.0f * hu / static_cast<float>(nu), sv = 2.0f * hv / static_cast<float>(nv);
    g.box = box; g.face = f;
    g.n = obbAxisOf(o, a) * sgn; g.U = obbAxisOf(o, au); g.V = obbAxisOf(o, av);
    g.hu = 0.5f * su; g.hv = 0.5f * sv; g.area = su * sv;
    g.c = o.center + g.n * obbHalfOf(o, a) + g.U * (-hu + su * (static_cast<float>(iu) + 0.5f)) + g.V * (-hv + sv * (static_cast<float>(iv) + 0.5f));
    return g;
}

/// レイが置いていく受け取り 1 件。
struct Deposit {
    std::int32_t patch = -1;
    std::uint8_t order = 0;                  // 何回目の当たりか（0 が最初）
    // 小片の中のどこに当たったか（0..255 ＝ 0..1）。耳の側はこの位置の見え方で配る。
    //   ★小片 1 枚が遮る物をまたぐとき（扉の枠の奥行き 0.4 m に厚み 6 cm の板）、小片の平均の見え方で配ると
    //     音源側で受けた量を向こう側の見本点が「見える」として耳へ出す。閉じた扉の向こうで初期が 3.6e-4 漏れた（実測）。
    std::uint8_t u8 = 128, v8 = 128;
    // ひとつ前に当たった面（箱*6+面、無ければ −1）。2 次の虚像（f1 → f0）の量を「f1 の次に f0 に当たった」受け取りで引く（receiver.h 3）。
    std::int32_t prevKey = -1;
    float tSec = 0.0f;                       // 音源 → 当たり点
    float e6[kNumBands] = {};                // 反射して出ていく量（音源 → 当たり点の空気込み）
    float inDir[3] = {0.0f, 0.0f, 0.0f};     // 入ってきた向き（単位、進む向き）
};

/// traceRay に渡す置き場。layout と out が揃っているときだけ置く。
struct DepositSink {
    const PatchLayout* layout = nullptr;
    std::vector<Deposit>* out = nullptr;
    float maxSec = 0.0f;                     // これより遅い当たりは置かない（mixing time）
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_RECEIVER_LAYOUT_H
