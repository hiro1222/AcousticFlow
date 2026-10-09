/* Flow/diffraction.h ── 回折（段 6）。前川式
 *
 * ■ 役割
 *   遮られた直接音の代わり（追記 B）。直接音のうち見通しの無い分 (1−visible) が、最寄りの稜線を回って届く。
 *   量は前川式（既存 Core/maekawa.h）の帯域別の減衰、方向は稜線の点、到達は迂回した経路長。
 *   設計文書 Ⅵ「回折音 ── 前川式（第一候補）」。
 *
 * ■ 中の仕組み
 *   1) 候補の稜線: 直線 L→S を遮っている箱（segmentIntersectsObb）の 12 本の稜線。
 *   2) 各稜線で、経路長 f(t) = |L − P(t)| + |P(t) − S|（P は稜線上の点）を最小にする t を黄金分割で探す
 *      （f は t に対して凸なので 1 つの谷）。
 *   3) その点 P を稜線の外側（隣り合う 2 面の法線の和の向き）へ 1 mm 押し、L→P と P→S の 2 本の脚が
 *      他の箱に遮られていないことを確かめる（遮られていれば無効）。
 *   4) 有効な候補のうち最短の経路を採る。迂回長 δ = f_min − |L−S| から
 *          N_b = 2δ/λ_b、A_b = 5 + 20 log10(√(2πN)/tanh√(2πN)) dB（前川。上限 24 dB）
 *      帯域別の振幅比 g_b = 10^(−A_b/20)。エネルギーは g_b²。高域ほど N が大きく減衰が強い（回折のハイ落ち）。
 *   5) 方向 = リスナーから P への向き（リスナー座標）。到達 = f_min / c。
 *   ★δ は候補の最小値なので、稜線が入れ替わっても値は飛ばない（min は連続。Core/maekawa.h の注記）。
 *     UTD のように稜線ごとの幾何に依存する式だと、入れ替わりで最大 7.9 dB 跳んだ（旧実装の実測）。
 *
 * ■ 繋がり
 *   受ける: Surfaces、リスナーと音源の位置、見通し（aperture の visible。1 なら回折は要らない）。
 *   渡す:   Diffraction（帯域別のエネルギー比・方向・到達）を distribute へ。
 *           distribute は 回折 = free × (1−visible) × g² を回折のタップにする。
 *
 * ■ 退けた書き方
 *   ・UTD / BTM（旧実装）: 時間領域の応答を持つのでタップ組み（ISM）とは合うが、配分の枠では減衰量だけあればよい
 *     （追記 H）。しかも稜線の入れ替わりで跳ぶ。
 *   ・2 本の稜線を回る経路（扉と枠の隙間）: 段 6 は 1 本のまま。ただし**隙間の中に点を置く**ことで
 *     閉じかけの扉も 1 本で扱う（2026-09-24。押し出しを隙間の半分で頭打ちに）。2 本にすると点が 2 つに増え、
 *     どちらを回るかの選び直しで跳ぶ。狭い隙間を通る量は幾何でなく GapOpen（幅 ÷ フレネル半径）が決める。
 *   ・見通しの割合で回折を「消す」（visible が正なら回折 0）: 幅を持つ音源が半分隠れているとき、隠れた半分は回る。
 *     (1−visible) を掛けるのが筋。
 *
 * ■ 壊れる所
 *   ・P を押し出さないと脚が自分の箱に当たって全部無効になる（見通しの影レイと同じ罠）。
 *   ・押し出す量が大きいと枠の中に入って無効になる角度が増える（扉が開いているのに回折が立たない）。1 mm。
 *   ・δ に符号を付けて照射側を扱おうとすると、visible と二重になる。ここは影の側（δ ≥ 0）だけ。
 *   ・候補を「最寄りの 1 本」に決め打ちすると、脚が遮られる稜線を選んで回折が消える。全候補を見て有効な最短。
 */
#ifndef ACOUSTICFLOW_FLOW_DIFFRACTION_H
#define ACOUSTICFLOW_FLOW_DIFFRACTION_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Core/aabb.h"
#include "Core/maekawa.h"
#include "Core/material.h"
#include "Core/vec3.h"
#include "Flow/aperture.h"
#include "Flow/emitter.h"
#include "Flow/surfaces.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

struct Diffraction {
    bool  valid = false;
    float energy6[kNumBands] = {};       // 帯域別のエネルギー比（前川 × 隙間の通り）。自由音場に対して
    float gapOpen6[kNumBands] = {};      // 隙間の通り（0..1、帯域別）。診断用
    float gapWidth = 0.0f;               // 隙間の幅（m）。診断用
    float delta = 0.0f;                  // 迂回長 δ（m）
    float weight = 0.0f;                 // 脚の貫通による重み 0..1（1 = 脚が完全に通る）
    float pathSec = 0.0f;                // 到達（秒）
    Vec3  point{0, 0, 0};                // 回折点（world）
    Vec3  dirLocal{0, 0, 0};             // リスナー座標の到来方向
    int   box = -1, edge = -1;
};

namespace detail {
/// 箱の稜線 e（0..11）の両端と、隣り合う 2 面の外向き法線の和（押し出す向き）。
inline void boxEdge(const Obb& b, int e, Vec3& A, Vec3& B, Vec3& outward) {
    // 軸 a に平行な稜線が 4 本ずつ。他の 2 軸の符号 (s1, s2) で 4 本を区別する。
    const int axis = e / 4, k = e % 4;
    const float s1 = (k & 1) ? 1.0f : -1.0f, s2 = (k & 2) ? 1.0f : -1.0f;
    const Vec3 ax[3] = {b.axisX, b.axisY, b.axisZ};
    const float he[3] = {b.halfExtents.x, b.halfExtents.y, b.halfExtents.z};
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    const Vec3 mid = b.center + ax[a1] * (he[a1] * s1) + ax[a2] * (he[a2] * s2);
    A = mid - ax[axis] * he[axis];
    B = mid + ax[axis] * he[axis];
    outward = normalized(ax[a1] * s1 + ax[a2] * s2);
}
}  // namespace detail

/// 遮られた直接音の回折。見通し（aperture の Visibility）が 1 なら valid=false（要らない）。
///   候補の箱は Visibility が数えた「円盤に影を落とした箱」（中心線でなく円盤に揃える。aperture.h の注記）。
inline Diffraction edgeDiffraction(const Surfaces& surf, const Listener& listener, const Vec3& S, const Visibility& vis,
                                   int leakModel = 0) {
    Diffraction out;
    if (vis.visible >= 0.999f) return out;
    const Vec3 L = listener.pos;
    const float direct = length(S - L);
    if (direct <= kEps) return out;
    std::vector<int> shadowers;
    auto addOnce = [&](int i) {
        for (int k : shadowers) if (k == i) return;
        shadowers.push_back(i);
    };
    for (int k = 0; k < std::min(vis.shadowers, Visibility::kMaxShadowers); ++k) addOnce(vis.shadowBox[k]);
    // ★候補は「音源の円盤に影を落とした箱」だけでは足りない（段 9-c）。
    //   扉が 20° 開いた所で耳を左右に振ると、±1.5 m より外で回折が丸ごと消えた（実測）。
    //   そこで影を落としているのは仕切りで、その稜線を回る脚は開いた板に潰される。
    //   実際に音を通しているのは**板の自由端**なのに、板は円盤に影を落としていないので候補に入っていない。
    //   動く箱は「開口に立ちはだかる物」そのものなので、影を落としていなくても常に候補に入れる。
    //   数は場面で数個なので費用は小さい（動かない箱は増やさない）。
    for (int i = 0; i < surf.count(); ++i)
        if (surf.at(i).active && surf.at(i).dynamic) addOnce(i);
    if (shadowers.empty())                                       // 念のため: 円盤の影が無いのに visible<1 は無いはずだが、中心線で補う
        for (int i = 0; i < surf.count(); ++i)
            if (surf.at(i).active && segmentIntersectsObb(L, S, surf.at(i).obb)) addOnce(i);
    if (shadowers.empty()) return out;
    float best = 0.0f, bestLen = 0.0f, bestW = 0.0f, bestA = 0.0f;
    GapOpen bestGap;
    for (int i : shadowers) {
        const Obb& b = surf.at(i).obb;
        for (int e = 0; e < 12; ++e) {
            Vec3 A, B, outward; detail::boxEdge(b, e, A, B, outward);
            // 黄金分割で f(t) = |L−P| + |P−S| の最小
            auto f = [&](float t) { const Vec3 P = A + (B - A) * t; return length(P - L) + length(S - P); };
            float lo = 0.0f, hi = 1.0f;
            const float g = 0.6180339887f;
            float x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo), f1 = f(x1), f2 = f(x2);
            for (int it = 0; it < 24; ++it) {
                if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - g * (hi - lo); f1 = f(x1); }
                else { lo = x1; x1 = x2; f1 = f2; x2 = lo + g * (hi - lo); f2 = f(x2); }
            }
            const float t = 0.5f * (lo + hi);
            const float len = f(t);
            // 回折点は稜線から**両隣の面の外側へ**押し出す（各面の法線の向きに 半厚み（上限 3 cm）＋1 mm）。
            //   ★薄い板の端を回る経路は、稜線ちょうどに点を置くと脚が板の厚みを幾何的に必ず貫く（旧 Core/aabb.h の注記の罠）。
            //     最初は「自分の箱の貫通は厚みまで許す」と書いたが、閉じた扉の**面**を貫く経路まで通って
            //     閉扉で −8.6 dB の回折が立った。押し出せば脚は板の端の脇を通り、面を貫く経路は貫通で落ちる。
            //     厚い壁（0.2 m）の角では 3 cm 外に置くぶん δ が最大 3 cm 長く出る（1 kHz で 1.5 dB）。連続性を優先。
            const float halfMin = std::min(b.halfExtents.x, std::min(b.halfExtents.y, b.halfExtents.z));
            float push = (std::min(halfMin, 0.03f) + kEps) * 1.41421356f;
            // ★押し出しは「隣の実体までの隙間の半分」を超えない（2026-09-24、資料 7 の即時性）。
            //   閉じかけの扉では 4.2 cm 押すと点が**枠の中**へ入り、そこから出る脚が枠を 0.2 m 貫いて
            //   候補が全滅する ＝ 0〜2.1° が丸ごと無反応だった（実測: 0〜2.0° で −20.8 dB・こもり +21.0 のまま）。
            //   隙間（自由端と枠の弦、角度にほぼ比例して 1 m の扉で 1°≒1.7 cm）の**中**に点を置けば、
            //   通る量は隙間の通り（GapOpen: 幅 ÷ フレネル半径）が帯域ごとに決める ＝ 高い音から先に通る。
            //   ★0.9 倍の理由: gapWidthAt は向きを問わない最短距離なので、0.9 倍までなら押した後も
            //     相手まで 0.1·gap 残る ＝ どの向きへ押しても相手の箱へ入らない（0/1 の判定を増やさない）。
            //     半分にすると点が自分の板へ寄りすぎ、浅い角度の脚が板を掠める長さが伸びて
            //     3〜5° の回折が落ちた（実測: 立ち上がりが 2.2° → 4.5° へ後退）。
            //   ★開いた扉では何も変わらない: 5° で隙間 8.7 cm、その 0.9 倍は押し出し 4.2 cm より広い。
            const Vec3 P0 = A + (B - A) * t;
            push = std::min(push, 0.9f * gapWidthAt(surf, P0, i));
            const Vec3 P = P0 + outward * push;
            // 脚が遮られていないか。貫通の**長さ**で見る。2 cm までは通し、2〜7 cm で線形に消す（重み w）。
            //   ★0/1 にすると、板の自由端が枠を抜ける角度の前後で「有効」が行き来して 12 dB 跳んだ（段 6 の実測）。
            //     貫通が伸びるほど隙間がフレネル帯より細くなる、の代理として重みを掛ける。消える幅は扉で 1〜2°。
            //     壁を本当に通る脚は 0.2 m 以上貫くので w=0。
            //   ★自分の箱（稜線の持ち主）は別扱い: 許すのは**半厚み（上限 3 cm）**まで。
            //     押し出した点から出る脚は、浅い角度で来ると自分の端を数 mm 掠める（12° の扉で 9 mm。5 mm の厳密判定だと
            //     正しい候補が落ちて 12 dB 跳んだ）。一方「板の面を通る」経路は厚みぶん（6 cm）貫くので半厚みで落ちる。
            //     閉じた扉で回折が立った件（−19.8 dB）はこれで防げる。
            //     ★段 9 以降はこの判定が段を作らない ── 隙間の通り（fresnelOpenAt）が 0 に近いので、
            //       境目で候補が出入りしても量はどちらもほぼ 0。判定は「どの稜線を選ぶか」だけの役になった。
            //     ★段 9: これも 0/1 でなく**傾斜**にした。板の自由端は前後 2 本の稜線を持ち、開いていくと
            //       奥の稜線は板の陰に入って見えなくなる。0/1 だと見えなくなった瞬間に手前の稜線へ乗り換え、
            //       隙間が 0.18 → 0.08 m へ跳んで **2.6 dB 落ちた**（19°、実測）。半厚みから厚みまでで消せば連続。
            //       厚みぶん貫く「板の面を通る」経路は 0 のままなので、閉扉で回折が立つ件は防げたまま。
            //     ★★ leakModel=1（漏れの案 A）: 上の「0.2 m 以上貫く」という前提が**薄い板で破れる**。
            //       扉の板は厚み 6 cm しかないので、板の面を真っ直ぐ通る脚でも貫通は 6 cm で止まり、
            //       許容 7 cm の中に収まって w=0.19 が残る。閉じた扉から回折が漏れていた正体はこれ
            //       （実測: 閉扉・耳 x=0 で回折 −44.6 dB、同じ場面の後期が −41 dB）。
            //       案 A は許容を固定の 7 cm でなく**その箱自身の薄さ**に対する割合で測る。
            //         割合 = 貫通 ÷ 箱のいちばん薄い辺の長さ
            //       半分まで潜るのは「掠め」、厚みぶん潜ったら「通過」。板でも壁でも同じ言い方になる。
            //       12° の扉で 9 mm 掠める例は 9/60 = 0.15 で通り、板の面を通る例は 60/60 = 1.0 で落ちる。
            const float ownerTol = std::min(halfMin, 0.03f) + kEps;
            float pen = 0.0f, ownerPen = 0.0f, othersW = 1.0f;
            for (int j = 0; j < surf.count(); ++j) {
                if (!surf.at(j).active) continue;
                const float pj = std::max(segmentObbPenetration(L, P, surf.at(j).obb), segmentObbPenetration(P, S, surf.at(j).obb));
                if (j == i) { ownerPen = pj; continue; }
                pen = std::max(pen, pj);
                if (pj <= 0.0f) continue;
                float wj;
                if (leakModel == 1 || leakModel == 3) {
                    const Vec3& he = surf.at(j).obb.halfExtents;
                    const float thin = 2.0f * std::min(he.x, std::min(he.y, he.z));
                    const float ratio = pj / std::max(1e-3f, thin);
                    wj = std::min(1.0f, std::max(0.0f, (1.0f - ratio) / 0.5f));
                } else {
                    wj = std::min(1.0f, std::max(0.0f, (0.07f - pj) / 0.05f));
                }
                othersW = std::min(othersW, wj);
                if (othersW <= 0.0f) break;
            }
            const float ownerW = std::min(1.0f, std::max(0.0f, (2.0f * ownerTol - ownerPen) / ownerTol));
            if (ownerW <= 0.0f) continue;
            const float w = othersW * ownerW;
            if (w <= 0.0f) continue;
            // ★候補は**出てくる量そのもの**で選ぶ（経路の長さで選ばない）。
            //   隙間の通り（段 9）を掛けると、経路の長さがほぼ同じで隙間の広さが違う稜線が並ぶ
            //   （扉の自由端は前後 2 本の稜線を持ち、厚み 6 cm ぶん隙間が違う）。長さで選ぶと、
            //   乗り換えた瞬間に隙間が 0.28 → 0.18 m へ跳んで **2.6 dB 落ちた**（19°、実測）。
            //   量で選べば、乗り換えは「2 つが等しい所」で起きるので値は跳ばない（max は連続）。
            //   ★隙間の通り: 回折点から他の実体までの最短距離 a と、波長ごとのフレネル半径 r_b の比。
            //     これが無いと「経路が生まれた瞬間に段で立ち上がり、あとは開けてもほとんど変わらない」になる
            //     （8→9° で +10.5 dB、9→50° は 2.6 dB。試聴で「急な変化が激しい」）。
            const float a = gapWidthAt(surf, P, i);
            const GapOpen gp = gapOpen(a, length(P - L), length(S - P));
            const float dl = std::max(0.0f, len - direct);
            float gn[kNumBands]; maekawa::gainBands(dl, gn, true);
            float score = 0.0f;
            for (int b = 0; b < kNumBands; ++b) score += gn[b] * gn[b] * gp.open6[b];
            score = score / kNumBands * w;
            if (score <= best) continue;
            best = score; bestLen = len; bestW = w; bestGap = gp; bestA = a;
            out.valid = true; out.point = P; out.box = i; out.edge = e; out.pathSec = len / kSpeedOfSound;
        }
    }
    if (!out.valid) return out;
    out.delta = std::max(0.0f, bestLen - direct);
    out.weight = bestW;
    out.gapWidth = bestA;
    float gain[kNumBands];
    maekawa::gainBands(out.delta, gain, true);
    for (int b = 0; b < kNumBands; ++b) {
        out.energy6[b] = gain[b] * gain[b] * bestW * bestGap.open6[b];
        out.gapOpen6[b] = bestGap.open6[b];
    }
    out.dirLocal = listener.toLocal(out.point - L);
    return out;
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_DIFFRACTION_H
