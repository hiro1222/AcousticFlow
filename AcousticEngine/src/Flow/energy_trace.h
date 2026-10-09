/* Flow/energy_trace.h ── レイトレース（段 2。段 5 で直接音を aperture に渡した）
 *
 * ■ 役割
 *   音源から出た音のエネルギーが、面で 反射／透過／吸収 に分かれながら、リスナーにどれだけ・いつ届くかを
 *   帯域別に集計する。設計文書 Ⅵ「レイトレース（BVH）── 素材の透過率・反射率でエネルギー分配 → 帯域別エネルギー総量」。
 *   出すのは 3 つ: 自由音場の直接音（決定的。見通しの割合は aperture が別に出す）、反射の初期・後期（レイで統計的）。
 *
 * ■ 中の仕組み
 *   単位は「音源の出力を 1 としたときの、リスナーの点に届く強さ（1/m²）」。直接音なら 1/(4πd²)。
 *   1) 自由音場の直接 freeDirect6 = 1/(4πd²) × 空気吸収。**レイを飛ばさない**。見通し（割合と τ）は段 5 の aperture が
 *      解析で出し、distribute が 直接 = free × 割合、透過 = free × (1−割合) × τ にする（追記 A）。
 *      直線が横切った壁の枚数 directCrossings は情報として残す。
 *   2) 反射は N 本のレイ。各当たり点で
 *          吸収 a·e を帳簿へ、残り (r+t)·e を運ぶ。反射か透過かは r:t の確率で片方だけ選ぶ（期待値は不偏）。
 *      反射方向は材質の散乱率で 鏡面⇄拡散 を振り分ける（旧 scene.h の scatteredDir と同じ）。
 *   3) **次イベント推定（NEE）**: 当たり点ごとにリスナーへ影レイを 1 本引き、拡散の放射（ランバート）として
 *          届く強さ = e_side · cosθ / (π d²) · τ_途中 · 空気
 *      を足す。e_side はリスナーが面の表側なら r·e、裏側なら t·e。到達時刻 (経路長 + d)/c が mixingSec 未満なら
 *      初期、以上なら後期。★レイがリスナーの球に当たるのを待たない（当たらない＝分散が大きい）。
 *      「diffuse rain」と呼ばれる標準の推定法。
 *   4) 打ち切り: 帯域の最大エネルギーが出発の 1e-4（−40 dB）を切るか、maxBounces に達したら残りを帳簿の
 *      remainder へ。保存則  emitted = absorbed + remainder + escaped  が帳簿で成り立つ（検査）。
 *   5) **乱数の種は音源ごとに固定**。同じ幾何・同じ位置なら毎フレーム同じレイが飛び、結果がビット一致する。
 *      → 静止していれば揺れない（追記 D）。動けば相関した標本が滑らかに追う。
 *
 * ■ 繋がり
 *   受ける: Surfaces、MaterialTable、Emitter の位置、Listener の位置、TraceParams（本数は budget が決める）。
 *   渡す:   TraceResult（freeDirect6 / early6 / late6 と帳簿）を distribute へ。
 *
 * ■ 退けた書き方
 *   ・リスナーの球にレイが当たったら集計（検出球）: 当たる本数が少なく、256 本では初期反射がほぼ 0 か 1 本。
 *   ・直接音もレイで出す: 決定的に出せる物を統計で出す理由が無い。揺れの源を増やすだけ。
 *   ・直接音の τ をここで直線から出す（段 4 までの形）: 幅を持つ音源が半分だけ板に隠れるとき、中心の直線は見通しでも
 *     隠れた半分は板を通る。τ は「遮っている物」の物で、aperture が影ごとに出すのが筋。段 5 でこちらから外した。
 *   ・反射と透過の両方にレイを分岐させる: 本数が指数で増える。片方を確率で選び重みで補うのが標準。
 *
 * ■ 壊れる所
 *   ・影レイの原点を当たり点ちょうどに置くと自分の面に当たって全部遮られる（kEps で法線側へ押す）。
 *   ・cosθ を絶対値にすると、面の裏側のリスナーに反射エネルギーが届く（壁を透ける）。表裏で e_side を分ける。
 *   ・NEE の 1/π を忘れると反射が π 倍（+5 dB）。
 *   ・種を毎フレーム変えると静止で揺れる。
 *   ・帳簿は double。4096 本 × 40 回のヒットで float では 2e-4 ずれて保存則の検査が落ちた（段 2）。
 *   ・後期の出どころ（段 2-f）: lateOther6 は late6 の**内訳**。distribute で late6 と足すと後期が二重になる。
 *     押し出しが格子 1 個より短いと、面の点が壁のボクセルに残って全部「不明」（-1）になり、戸口越しが 0 に見える。
 */
#ifndef ACOUSTICFLOW_FLOW_ENERGY_TRACE_H
#define ACOUSTICFLOW_FLOW_ENERGY_TRACE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Core/material.h"
#include "Core/room_graph.h"      // kAirDbPerM（空気吸収の表はここ 1 か所）
#include "Core/vec3.h"
#include "Flow/surfaces.h"
#include "Flow/trace_scene.h"
#include "Flow/receiver_layout.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

constexpr float kPi = 3.14159265358979f;

// ── 乱数と半球（旧 scene.h から持ち込み。PCG 系のハッシュ乱数と余弦重み半球）──
inline float rand01(std::uint32_t& state) {
    state = state * 747796405u + 2891336453u;
    std::uint32_t w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    w = (w >> 22) ^ w;
    return static_cast<float>(w) * (1.0f / 4294967296.0f);
}
inline Vec3 cosineHemisphere(const Vec3& n, std::uint32_t& rng) {
    const float u1 = rand01(rng), u2 = rand01(rng);
    const float r = std::sqrt(u1), th = 2.0f * kPi * u2;
    const float x = r * std::cos(th), y = r * std::sin(th);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    const Vec3 t = (std::fabs(n.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 b1 = normalized(cross(n, t));
    const Vec3 b2 = cross(n, b1);
    return normalized(b1 * x + b2 * y + n * z);
}
inline Vec3 uniformSphere(std::uint32_t& rng) {
    const float z = 1.0f - 2.0f * rand01(rng);
    const float th = 2.0f * kPi * rand01(rng);
    const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3(s * std::cos(th), z, s * std::sin(th));
}
/// 空気吸収（エネルギー比）。kAirDbPerM は dB/m。
inline float airEnergy(int band, float dist) {
    return std::pow(10.0f, -static_cast<float>(rooms::kAirDbPerM[band]) * dist * 0.1f);
}

struct TraceGroups { static constexpr int kMax = 8; };
/// TraceParams::listenerRoom の「耳が外にいる」（World::setOutsideMouth）。−1（分けない）と区別する。GPU の核も同じ値で見る。
constexpr int kListenerOutside = -2;

struct TraceParams {
    int   rays = 256;               // 予算（budget が音源ごとに決める）。この本数で割った 1 本の重み 1/rays を使う
    int   maxBounces = 24;
    float mixingSec = 0.03f;        // 初期／後期の境（到達時刻）。世界が probe から出す（3 × 平均自由行程 / c）
    std::uint32_t seed = 1u;        // 音源ごとに固定
    // ── フレーム分散（設計文書 Ⅶ「レイ更新のフレーム分散。総予算は固定」）──
    //   1 フレームで rays 本 全部を飛ばすと 512 本で 6 ms 掛かる（実測、1 本 12 µs）。
    //   rays 本を group 個の組に分け、毎フレーム 1 組だけ飛ばして、他の組は前回の結果を使う。
    //   ★組ごとの結果は幾何が同じなら毎回同じ（レイの種を「音源 × レイ番号」で作るので、
    //     どの組に入っていても同じレイは同じ道を通る）。だから静止していれば合計も一定＝揺れない。
    //   動いたときは group フレーム（既定 4 = 43 ms @ 60fps）で全部が入れ替わる。遅れの閾値 70 ms の下。
    int   groups = 1;               // 1 で分散なし（全部を毎フレーム）
    int   group = 0;                // 今フレームに飛ばす組（0..groups-1）
    // ── 後期の出どころ（段 2-f、2026-09-11）──
    //   耳のいる部屋。0 以上なら、後期の NEE を「放射した面が耳と同じ部屋に面しているか」で分け、
    //   別の部屋（戸口越し）の分を lateOther6 と otherDir に**足す**。late6 は総量のまま変えない。
    //   -1 なら分けない（今までと 1 ビットも同じ）。
    //   kListenerOutside（−2）は「耳が外にいる」（World::setOutsideMouth）。どれかの部屋に面した面から来た分を全部、別の部屋の分にする。
    int   listenerRoom = -1;
    // ── 壁越しの反射（2026-09-12）──
    //   1 なら旧: 影の線が壁を横切っても τ を掛けて初期・後期に数える（向こうの部屋の反射と残響が壁越しに薄まって届く）。
    //   0 なら壁を横切った分は数えない。壁を抜けるのは解析で出す透過の直接音だけになる（World の既定）。
    //   ★試聴「壁の向こうの透過音がダブる」: 壁を通る分は直接も反射も同じ τ で縮むので、向こうの部屋の
    //     反射÷直接の比がそのまま残り、透過の直接音の数 ms 後ろに「方向なしの初期」の写しが 1 本立っていた。
    //   開いた戸口を通る影の線は横切りが無いので今までどおり通る。扉が開くにつれて通る本数が連続に増える。
    //   ★影の線だけ止めても 12% しか減らなかった（実測）。大半は**レイそのものが壁を透過して**隣の部屋に入り、そこで
    //     反射した分（ロシアンルーレットの「透過」）。閉じた扉の向こうでは透過の直接音の 8 倍あった。だから 0 では
    //     レイの透過も止める（当たったら反射だけ。透過するはずだった分は帳簿の escaped6 へ ＝ 保存則は保つ）。
    //     当たった面の裏側にいる耳へ、その面を通して届く分（NEE の !front）も止める。
    int   wallReflect = 1;
};

struct TraceResult {
    float freeDirect6[kNumBands] = {};  // 自由音場の直接（1/4πd² × 空気）。見通しは aperture が別に出す
    float early6[kNumBands] = {};       // 反射、t < mixing
    float late6[kNumBands] = {};        // 反射、t ≥ mixing
    float firstReflectSec = -1.0f;      // 最初に届いた反射の時刻（尾の開始の代用。ISM が来たら置き換え）
    float directSec = 0.0f;             // 直接音の到達時刻
    float directDist = 0.0f;            // 直接音の距離
    int   directCrossings = 0;          // 直線が横切った壁の枚数（情報）
    // 帳簿（レイ側）。保存則: emitted = absorbed + remainder + escaped
    //   escaped は「場面の外へ逃げた分」＋ wallReflect=0 のとき「壁の向こうへ抜けたので数えない分」
    double emitted6[kNumBands] = {}, absorbed6[kNumBands] = {}, remainder6[kNumBands] = {}, escaped6[kNumBands] = {};
    int   raysTraced = 0, hits = 0, neeVisible = 0;
    // 後期の出どころ（段 2-f）。lateOther6 は late6 の内訳（戸口越しの面から来た分）。
    //   otherDir は その分の「耳 → 放射点」の単位ベクトルをエネルギー（帯域の和）で重み付けした和（ワールド、正規化しない）。
    //   |otherDir| ÷ Σ lateOther6 が集まり具合 R（1 で一点から、0 で全方向から）。
    float lateOther6[kNumBands] = {};
    float otherDir[3] = {0.0f, 0.0f, 0.0f};

    float reflected6(int b) const { return early6[b] + late6[b]; }

    /// 組ごとの結果を足し合わせる（フレーム分散。反射の集計と帳簿は加算、直接と時刻は最後に測った物）。
    static void sumGroups(const TraceResult* parts, int n, TraceResult& out) {
        out = TraceResult{};
        float firstBest = -1.0f;
        for (int k = 0; k < n; ++k) {
            const TraceResult& p = parts[k];
            for (int b = 0; b < kNumBands; ++b) {
                out.early6[b] += p.early6[b]; out.late6[b] += p.late6[b];
                out.emitted6[b] += p.emitted6[b]; out.absorbed6[b] += p.absorbed6[b];
                out.remainder6[b] += p.remainder6[b]; out.escaped6[b] += p.escaped6[b];
                out.lateOther6[b] += p.lateOther6[b];
            }
            for (int q = 0; q < 3; ++q) out.otherDir[q] += p.otherDir[q];
            out.raysTraced += p.raysTraced; out.hits += p.hits; out.neeVisible += p.neeVisible;
            if (p.firstReflectSec > 0.0f && (firstBest < 0.0f || p.firstReflectSec < firstBest)) firstBest = p.firstReflectSec;
            if (p.directDist > 0.0f) {   // 直接は決定的なので、どの組でも同じ（最後に書いた物を採る）
                for (int b = 0; b < kNumBands; ++b) out.freeDirect6[b] = p.freeDirect6[b];
                out.directSec = p.directSec; out.directDist = p.directDist; out.directCrossings = p.directCrossings;
            }
        }
        out.firstReflectSec = firstBest;
    }
};

/// レイ 1 本ぶんの取り分。★GPU では 1 スレッド = 1 本がこれを埋め、あとで足し合わせる形になる。
///   ここを「共有の帳簿へ直接足す」ままにすると、スレッドごとに書き先が競合して並列化できない。
///   1 本を純粋な関数にしておけば、CPU と GPU で同じ計算を回して突き合わせられる（段 2）。
struct RayPartial {
    float  early6[kNumBands] = {};
    float  late6[kNumBands] = {};
    double emitted6[kNumBands] = {}, absorbed6[kNumBands] = {}, remainder6[kNumBands] = {}, escaped6[kNumBands] = {};
    float  firstReflectSec = -1.0f;
    int    hits = 0, neeVisible = 0;
    float  lateOther6[kNumBands] = {};          // 段 2-f。late6 の内訳（戸口越しの面から）
    float  otherDir[3] = {0.0f, 0.0f, 0.0f};    // 段 2-f。その分の向きの重み付き和（ワールド）
};

/// 自由音場の直接音（決定的）。★レイを飛ばさない。GPU の道でも**ここは CPU で出す**。
///   決定的に出せる物を GPU へ持っていく理由が無い。転送と読み戻しのほうが高くつく。
inline void fillDirect(const TraceScene& sc, const Vec3& source, const Vec3& listener, TraceResult& R) {
    const float d = std::max(length(listener - source), kEps);
    float tr[kNumBands]; int crossings = 0;
    sceneTransmittance(sc, source, listener, -1, tr, &crossings);   // 枚数だけ使う（τ は aperture の物）
    R.directCrossings = crossings;
    R.directDist = d;
    R.directSec = d / kSpeedOfSound;
    const float geo = 1.0f / (4.0f * kPi * d * d);
    for (int b = 0; b < kNumBands; ++b) R.freeDirect6[b] = geo * airEnergy(b, d);
}

/// レイ 1 本を追う（純粋な関数）。i は**全体での本数の中の番号**で、種はここから作る。
///   受ける物は全部読み取り専用。書くのは out だけ。
///   sink を渡すと、mixing time より前の当たりごとに受け取り（受取面、receiver_layout.h）を置く。doNee=false で耳への影の線を引かない
///   （受け取りだけを取るとき。GPU が NEE を引く道で使う）。どちらも既定のままなら今までと 1 ビットも同じ。
inline void traceRay(const TraceScene& sc,
                     const Vec3& source, const Vec3& listener,
                     const TraceParams& prm, int i, float e0, RayPartial& out,
                     DepositSink* sink = nullptr, bool doNee = true) {
    // ★レイごとに種を作る（音源 × レイ番号）。逐次の 1 本の流れにすると、組で間引いたとき
    //   同じレイ番号が別の道を通ってしまい、静止していても合計が組ごとに変わる（＝揺れる）。
    std::uint32_t rng = (prm.seed * 2654435761u + 0x9E3779B9u) ^ (static_cast<std::uint32_t>(i) * 2246822519u);
    rng = rng * 747796405u + 2891336453u;
    float e[kNumBands];
    for (int b = 0; b < kNumBands; ++b) { e[b] = e0; out.emitted6[b] += e0; }
    Vec3 pos = source;
    Vec3 dir = uniformSphere(rng);
    float pathLen = 0.0f;
    int skip = -1;
    bool terminated = false;
    std::int32_t prevKey = -1;                    // 受取面: ひとつ前に当たった面（箱*6+面）
    for (int bounce = 0; bounce < prm.maxBounces; ++bounce) {
        const SurfaceHit h = sceneNearest(sc, pos, dir, 1e4f, skip);
        if (!h.hit) { for (int b = 0; b < kNumBands; ++b) out.escaped6[b] += e[b]; terminated = true; break; }
        ++out.hits;
        pathLen += h.t;
        const int mi = sc.material[static_cast<std::size_t>(h.index)];
        const SurfaceSplit* tbl = &sc.split[static_cast<std::size_t>(mi) * kNumBands];
        float rMean = 0.0f, tMean = 0.0f;
        SurfaceSplit sp[kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
            sp[b] = tbl[b];
            out.absorbed6[b] += e[b] * sp[b].absorb;
            rMean += sp[b].reflect; tMean += sp[b].transmit;
        }
        rMean /= kNumBands; tMean /= kNumBands;
        const Vec3 nFace = (dot(h.normal, dir) < 0.0f) ? h.normal : h.normal * -1.0f;
        // ── 受取面（2026-09-13）: この当たりを小片に置く。法線の側（箱の外）から当たったときだけ ──
        if (sink && sink->out && sink->layout) {
            const float ts = pathLen / kSpeedOfSound;
            if (ts < sink->maxSec && dot(h.normal, nFace) > 0.0f) {
                float lu = 0.5f, lv = 0.5f;
                const int pch = patchOfHit(sc, *sink->layout, h.index, h.point, h.normal, &lu, &lv);
                const std::int32_t keyHere = (pch >= 0) ? sink->layout->faceOfPatch[static_cast<std::size_t>(pch)] : -1;
                if (pch >= 0) {
                    Deposit d;
                    d.patch = pch; d.order = static_cast<std::uint8_t>(std::min(bounce, 255)); d.tSec = ts;
                    d.u8 = static_cast<std::uint8_t>(std::min(255.0f, lu * 256.0f)); d.v8 = static_cast<std::uint8_t>(std::min(255.0f, lv * 256.0f));
                    for (int b = 0; b < kNumBands; ++b) d.e6[b] = e[b] * sp[b].reflect * airEnergy(b, pathLen);
                    d.inDir[0] = dir.x; d.inDir[1] = dir.y; d.inDir[2] = dir.z;
                    d.prevKey = prevKey;
                    sink->out->push_back(d);
                }
                prevKey = keyHere;
            } else {
                prevKey = -1;
            }
        }
        // ── NEE: この当たり点からリスナーへ ──
        if (doNee) {
            const Vec3 toL = listener - h.point;
            const float d = length(toL);
            if (d > kEps) {
                const Vec3 u = toL * (1.0f / d);
                const float cosF = dot(nFace, u);
                const bool front = cosF > 0.0f;
                const float cosT = std::fabs(cosF);
                const Vec3 side = front ? nFace : nFace * -1.0f;      // リスナーの側（放射する面の向き）
                const Vec3 org = h.point + side * kEps;
                float tr[kNumBands]; int cr = 0;
                sceneTransmittance(sc, org, listener, h.index, tr, &cr);
                const float tSec = (pathLen + d) / kSpeedOfSound;
                const float geo = cosT / (kPi * d * d);
                bool any = false;
                float c6[kNumBands] = {};
                const bool passWall = (prm.wallReflect != 0 || (cr == 0 && front));   // 壁を横切る影の線と、面の裏側への分は数えない（上の■）
                for (int b = 0; passWall && b < kNumBands; ++b) {
                    const float eSide = e[b] * (front ? sp[b].reflect : sp[b].transmit);
                    const float c = eSide * geo * tr[b] * airEnergy(b, pathLen + d);
                    if (c <= 0.0f) continue;
                    any = true;
                    c6[b] = c;
                    if (tSec < prm.mixingSec) out.early6[b] += c; else out.late6[b] += c;
                }
                // ── 後期の出どころ（段 2-f）: 放射した面が耳と同じ部屋に面しているか ──
                //   ★面の上の点は格子では壁の中（実体）に落ちるので、リスナーの側へ押し出した点で部屋を引く。
                //     押す量は 0.3 m と格子 1.25 個ぶんの大きい方（格子が粗くなる場面でも壁のボクセルを抜けるように）。
                //   ★足すだけで late6 は触らない。分けても分けなくても後期の総量は同じ（検査で 1 ビット一致を見る）。
                if (any && (prm.listenerRoom >= 0 || prm.listenerRoom == kListenerOutside) && tSec >= prm.mixingSec) {
                    const float push = std::max(0.3f, 1.25f * sc.roomCell);
                    const int room = sceneRoomAt(sc, h.point + side * push);
                    if (room >= 0 && room != prm.listenerRoom) {
                        float cs = 0.0f;
                        for (int b = 0; b < kNumBands; ++b) { out.lateOther6[b] += c6[b]; cs += c6[b]; }
                        out.otherDir[0] -= cs * u.x; out.otherDir[1] -= cs * u.y; out.otherDir[2] -= cs * u.z;
                    }
                }
                if (any) {
                    ++out.neeVisible;
                    if (out.firstReflectSec < 0.0f || tSec < out.firstReflectSec) out.firstReflectSec = tSec;
                }
            }
        }
        // ── 続きの経路: 反射か透過か（確率で片方、重みは r+t）──
        //   wallReflect=0 では透過を引かない（反射だけ、重みは r）。透過するはずだった分は「壁の向こうへ抜けた」として escaped6 へ。
        const bool passWallRay = (prm.wallReflect != 0);
        if (!passWallRay) for (int b = 0; b < kNumBands; ++b) out.escaped6[b] += e[b] * sp[b].transmit;
        const float carry = passWallRay ? (rMean + tMean) : rMean;
        if (carry <= 1e-6f) { terminated = true; break; }
        const bool goReflect = passWallRay ? (rand01(rng) < (rMean / carry)) : true;
        for (int b = 0; b < kNumBands; ++b) e[b] *= passWallRay ? (sp[b].reflect + sp[b].transmit) : sp[b].reflect;
        float eMax = 0.0f; for (int b = 0; b < kNumBands; ++b) eMax = std::max(eMax, e[b]);
        if (eMax < e0 * 1e-4f) { for (int b = 0; b < kNumBands; ++b) out.remainder6[b] += e[b]; terminated = true; break; }
        if (goReflect) {
            const float s = sc.scatter1k[static_cast<std::size_t>(mi)];   // 1 kHz の散乱率で向きを決める
            dir = (rand01(rng) < s) ? cosineHemisphere(nFace, rng) : reflect(dir, nFace);
            if (dot(dir, nFace) <= 0.0f) dir = cosineHemisphere(nFace, rng);
            pos = h.point + nFace * kEps;
        } else {
            pos = h.point - nFace * kEps;
        }
        skip = h.index;
    }
    if (!terminated) for (int b = 0; b < kNumBands; ++b) out.remainder6[b] += e[b];
}

class EnergyTrace {
public:
    /// 受け取りだけを取る（耳への影の線を引かない）。GPU が NEE を引くフレームの受取面の道。組の間引きは run と同じ。
    void runDeposit(const TraceScene& sc, const Vec3& source, const TraceParams& prm, DepositSink& sink) const {
        const int N = std::max(1, prm.rays);
        const float e0 = 1.0f / static_cast<float>(N);
        const int G = std::max(1, prm.groups);
        const int g0 = ((prm.group % G) + G) % G;
        for (int i = g0; i < N; i += G) {
            RayPartial p;
            traceRay(sc, source, source, prm, i, e0, p, &sink, false);
        }
    }

    /// 便利版: Surfaces から場面を組んで回す（検査や道具から呼ぶ）。毎回組むので実時間の道では使わない。
    TraceResult run(const Surfaces& surf, const MaterialTable& mats,
                    const Vec3& source, const Vec3& listener, const TraceParams& prm) const {
        TraceScene sc;
        buildTraceScene(surf, mats, sc);
        return run(sc, source, listener, prm);
    }

    /// 本体: 平らな場面だけを見る。★GPU 版もこれと同じ入力を受ける。
    TraceResult run(const TraceScene& sc,
                    const Vec3& source, const Vec3& listener, const TraceParams& prm, DepositSink* sink = nullptr) const {
        TraceResult R;
        fillDirect(sc, source, listener, R);
        // ── 2) 反射（レイ + NEE）──
        //   ★1 本ずつ独立した取り分に書き、**本の順に**足し合わせる。
        //     こうしておくと 1 本が純粋な関数になり、そのまま GPU の 1 スレッドに載る（段 1）。
        //     足す順を本の順に固定してあるので、並列で解いても結果は変わらない。
        const int N = std::max(1, prm.rays);
        const float e0 = 1.0f / static_cast<float>(N);          // 1 本 = 出力の 1/N（分散していても割るのは総数）
        const int G = std::max(1, prm.groups);
        const int g0 = ((prm.group % G) + G) % G;
        for (int i = g0; i < N; i += G) {
            RayPartial p;
            traceRay(sc, source, listener, prm, i, e0, p, sink);
            ++R.raysTraced;
            R.hits += p.hits; R.neeVisible += p.neeVisible;
            for (int b = 0; b < kNumBands; ++b) {
                R.early6[b] += p.early6[b]; R.late6[b] += p.late6[b];
                R.emitted6[b] += p.emitted6[b]; R.absorbed6[b] += p.absorbed6[b];
                R.remainder6[b] += p.remainder6[b]; R.escaped6[b] += p.escaped6[b];
                R.lateOther6[b] += p.lateOther6[b];
            }
            for (int q = 0; q < 3; ++q) R.otherDir[q] += p.otherDir[q];
            if (p.firstReflectSec > 0.0f && (R.firstReflectSec < 0.0f || p.firstReflectSec < R.firstReflectSec))
                R.firstReflectSec = p.firstReflectSec;
        }
        return R;
    }
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_ENERGY_TRACE_H
