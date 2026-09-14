/* Flow/receiver.h ── 受取面の耳の側: 小片 → 耳を解析で配り、面ごとのタップにまとめる（2026-09-13）
 *
 * ■ 役割
 *   receiver_layout.h でレイが置いた受け取りを、耳へ届く初期反射にする。
 *   量は小片の四角を耳から見た立体角で解析的に配り（耳はレイの計算の外）、面ごと・「1 回目の当たり／それ以降」ごとに
 *   1 本のタップ（量・遅れ・向き・広がり）にまとめる。distribute が虚像の代わりにこれを鳴らす（World::earlyModel = 1）。
 *
 * ■ 中の仕組み
 *   1) 小片の見え方（ReceiverView、1 フレーム 1 回・全音源で共有）。どれも音源に依らないので、使われた小片だけを 1 回ずつ解く。
 *      ・立体角 Ω: 四角を耳の足元を原点にした 4 つの角の四角に割った符号付きの和（atan の閉じた式）。
 *      ・向きのベクトル F: 多角形を見込む向きの積分 ∫ω dΩ ＝ ½ Σ γ_i m̂_i（Lambert の多角形の式。γ は隣り合う角の間の角度、
 *        m̂ は耳と辺を通る平面の法線）。平均の向き F/Ω は長さ ≤ 1 で、近い大きな面ほど短い ＝ 広がり。見本点を使わないので跳ねない。
 *      ・係数 geoBase = Ω / (π·面積)。ランバート（今の NEE と同じ模型）で、出ていく量の密度を面で積分した物。
 *        耳のそばでも立体角で頭打ちになり、NEE の cosθ/(π d²) のような跳ねが無い（探り: 壁から 5 cm で種の揺れ 1.67 → 0.73 dB）。
 *      ・見え方は小片で決めない。受け取り 1 件ごとに、当たった位置から耳へ影の線を引く（buildFaceTaps）。
 *   2) 面ごとのタップ（buildFaceTaps、音源ごと）。受け取り 1 件ごとに
 *      到達 t = 音源→面の時刻 ＋ 面→耳の距離 / c。t < mixing の物だけ、当たった位置から耳へ影の線を引き（横切り無し・別の箱に埋まっていない）、
 *      量 c = 出ていく量 × geoBase × 空気(面→耳)。
 *      ・量は c をそのまま足す（答えは 1 つ。初期の総量はここで決まる）。
 *      ・向きと遅れは「鏡面の重み」wd = c·(s + (1−s)·lobe) で平均する。lobe は入ってきた向きを法線で折り返した向きが耳を向くほど 1。
 *        量はランバートのまま、向きと遅れだけが鏡の点へ寄る。1 回目の当たりのタップは、壁際で虚像の遅れ（0.6 ms）に寄る。
 *      ・広がり = 1 − |wd で平均した F/Ω|。面が近いほど、面の中で向きが散るほど広い。
 *      ・素性（id）は 箱×面×「1 回目か」で決まる。DSP がフレームをまたいで同じタップとして繋ぐ。
 *      ・タップの上限を超えたら小さい物から方向なしの 1 本へ畳む。
 *
 * ■ 繋がり
 *   受ける: World が音源ごと・組ごとの受け取り（std::vector<Deposit>）と、このフレームの TraceScene・PatchLayout・耳の位置。
 *   渡す:   FaceTapSet を DistributeInput::faceTaps へ。World はその total6 を TraceResult::early6 に書き戻してから配分する（帳簿の初期）。
 *
 * ■ 退けた書き方
 *   ・当たりの点ごとに耳へ影の線を引いて面で束ねる（最初の案）: 耳がレイの計算の中に残り、耳のそばの 1/d² の跳ねも残る。
 *   ・向きを見本点の平均で出す: 見本の数が距離で変わる所で向きが段になる。多角形の式なら連続。
 *   ・見え方を小片の上の見本点（2〜4 × 2〜4）で決める（最初の実装）: 3 つ穴があった。
 *     ① 小片が遮る物をまたぐと、音源側の受け取りを向こう側の見本点が「見える」として配る（閉じた扉の向こうへ 3.6e-4 漏れた）。
 *     ② 見本点が別の箱に埋まると、影の線の始点の箱が数えられずに見える（床の小片が仕切りの下にかかり 1.1e-4 漏れた）。
 *     ③ 細い隙間越しにしか見えない小片は見本点が全部遮られて小片ごと落ち、隙間が広がって見本点が 1 つ見えた瞬間にまとめて出る
 *       （扉 21〜24° で初期が NEE より 5.5 dB 小さく、24° で +4.4 dB 跳んだ。clicks の扉だけ 虚像 0.75 → 1.81 dB）。
 *     受け取りごとの影の線にすると 3 つとも消える。費用は初期の当たりの数だけ（後期の NEE は今のまま）。
 *   ・鏡面を虚像で別に鳴らし、量も虚像に持たせる: レイの量と二重に数える（「答えが 2 つ」）。鏡面は向きと遅れの重みだけに使う。
 *   ・面を 1 本のタップにまとめる（1 回目とそれ以降を分けない）: 壁際の近さ（1 回目の短い遅れ）が高次の遅れで数 ms 後ろへ引かれる。
 *
 * ■ 壊れる所
 *   ・lobe の幅 kLobe を狭くしすぎると、小片の間隔（3 m 先で 10°）より細くなり、歩くと向きがちらつく。
 *   ・影の線は受け取りの位置（小片の中の u, v を 1/256 に丸めた物、0.5 m の小片で 2 mm）から引く。丸めを粗くすると縁で漏れる。
 *   ・geo は耳が面の表側にいるときだけ。面の裏（壁の中や向こう）は 0（wallReflect=0 と同じ決まり）。
 */
#ifndef ACOUSTICFLOW_FLOW_RECEIVER_H
#define ACOUSTICFLOW_FLOW_RECEIVER_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Core/material.h"
#include "Core/vec3.h"
#include "Flow/energy_trace.h"
#include "Flow/receiver_layout.h"
#include "Flow/trace_scene.h"

namespace acoustic {
namespace flow {

/// 小片 → 耳（今フレーム）。
struct PatchView {
    bool  ok = false;
    float geo = 0.0f;                    // Ω/(π·面積) × 見える割合
    float dist = 0.0f;                   // 小片の中心 → 耳
    float air[kNumBands] = {};           // 空気（小片 → 耳）
    Vec3  meanDir{0, 0, 0};              // F/Ω（耳 → 小片、長さ ≤ 1）
    Vec3  toL{0, 0, 1};                  // 小片の中心 → 耳（単位）
    Vec3  n{0, 0, 1};
    Vec3  c{0, 0, 0};
    float scatter = 0.5f;                // 1 kHz の散乱率（鏡面の重み）
    int   room = -1;                     // 小片が面している部屋（表側へ押し出した点で引く）。隣の部屋の閉じ込めに使う
    float geoBase = 0.0f;                // Ω/(π·面積)（見え方は受け取りごとの影の線で掛ける）
    int   box = -1;
    Vec3  U{1, 0, 0}, V{0, 1, 0};
    float hu = 0.0f, hv = 0.0f;
    /// 受け取りの位置（小片の中の u, v）を点に戻す。
    Vec3 pointAt(float u, float v) const { return c + U * (hu * (2.0f * u - 1.0f)) + V * (hv * (2.0f * v - 1.0f)); }

};

/// 点が skip 以外の有効な箱の中にあるか（当たり判定の木で引く）。
///   ★見本点が別の箱に埋まっていると、sceneTransmittance は始点の箱を横切りとして数えない（交差の距離が 0）。
///     床の小片が仕切りの壁の下にかかり、壁の中の見本点から耳が「見えた」ので、閉じた扉の向こうへ 1.1e-4 漏れた（実測）。
inline bool scenePointInsideOther(const TraceScene& sc, const Vec3& p, int skip) {
    auto inside = [&](int i) {
        if (i == skip || !sc.active[static_cast<std::size_t>(i)]) return false;
        const Obb& o = sc.obb[static_cast<std::size_t>(i)];
        const Vec3 r = p - o.center;
        return std::fabs(dot(r, o.axisX)) < o.halfExtents.x && std::fabs(dot(r, o.axisY)) < o.halfExtents.y && std::fabs(dot(r, o.axisZ)) < o.halfExtents.z;
    };
    if (sc.hasTree()) {
        int stack[64]; int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const SurfaceBvh::Node& nd = sc.node[static_cast<std::size_t>(stack[--sp])];
            if (p.x < nd.bounds.min.x || p.y < nd.bounds.min.y || p.z < nd.bounds.min.z ||
                p.x > nd.bounds.max.x || p.y > nd.bounds.max.y || p.z > nd.bounds.max.z) continue;
            if (nd.count > 0) {
                for (int k = 0; k < nd.count; ++k) if (inside(sc.item[static_cast<std::size_t>(nd.first + k)])) return true;
            } else if (sp + 2 <= 64) {
                stack[sp++] = nd.left; stack[sp++] = nd.right;
            }
        }
        return false;
    }
    for (int i = 0; i < sc.boxCount(); ++i) if (inside(i)) return true;
    return false;
}

/// 四角 [x0,x1]×[y0,y1]（面の上、耳の足元が原点）を高さ h から見た立体角。
inline double rectSolidAngle(double h, double x0, double x1, double y0, double y1) {
    auto S = [&](double x, double y) { return std::atan2(x * y, h * std::sqrt(x * x + y * y + h * h)); };
    return S(x1, y1) - S(x0, y1) - S(x1, y0) + S(x0, y0);
}

class ReceiverView {
public:
    /// フレームの頭。小片の数が変わったら器を作り直す。
    void begin(int patchCount) {
        if (static_cast<int>(stamp_.size()) != patchCount) {
            stamp_.assign(static_cast<std::size_t>(patchCount), 0u);
            views_.assign(static_cast<std::size_t>(patchCount), PatchView{});
            frame_ = 0u;
        }
        ++frame_;
        if (frame_ == 0u) { std::fill(stamp_.begin(), stamp_.end(), 0u); frame_ = 1u; }
        pending_.clear();
        visTests_ = 0;
    }
    /// このフレームに使う小片を印す（直列で呼ぶ）。
    void touch(int patch) {
        if (patch < 0 || patch >= static_cast<int>(stamp_.size())) return;
        if (stamp_[static_cast<std::size_t>(patch)] == frame_) return;
        stamp_[static_cast<std::size_t>(patch)] = frame_;
        pending_.push_back(patch);
    }
    /// 印した小片を解く。
    void solve(const TraceScene& sc, const PatchLayout& L, const Vec3& listener) {
        for (int p : pending_) {
            PatchView& v = views_[static_cast<std::size_t>(p)];
            v = PatchView{};
            const PatchGeom g = patchGeomOf(sc, L, p);
            if (g.box < 0 || g.area <= 0.0f) continue;
            const Vec3 rel = listener - g.c;
            const double h = dot(rel, g.n);
            if (h <= 1e-4) continue;                           // 面の裏
            const double px = dot(rel, g.U), py = dot(rel, g.V);
            const double omega = rectSolidAngle(h, -g.hu - px, g.hu - px, -g.hv - py, g.hv - py);
            if (!(omega > 0.0)) continue;
            // 向きのベクトル ∫ω dΩ ＝ ½ Σ γ_i m̂_i（角は耳から見て並べる）
            const Vec3 corner[4] = {
                g.c + g.U * (-g.hu) + g.V * (-g.hv) - listener, g.c + g.U * (g.hu) + g.V * (-g.hv) - listener,
                g.c + g.U * (g.hu) + g.V * (g.hv) - listener,   g.c + g.U * (-g.hu) + g.V * (g.hv) - listener };
            double F[3] = {0.0, 0.0, 0.0};
            for (int k = 0; k < 4; ++k) {
                const Vec3& a = corner[k];
                const Vec3& b = corner[(k + 1) % 4];
                const double la = length(a), lb = length(b);
                if (la < 1e-9 || lb < 1e-9) continue;
                const double cg = std::min(1.0, std::max(-1.0, static_cast<double>(dot(a, b)) / (la * lb)));
                const double gamma = std::acos(cg);
                const Vec3 m = cross(a, b);
                const double lm = length(m);
                if (lm < 1e-12) continue;
                F[0] += 0.5 * gamma * m.x / lm; F[1] += 0.5 * gamma * m.y / lm; F[2] += 0.5 * gamma * m.z / lm;
            }
            // 並べ方で符号が逆になるので、小片の側を向くように揃える
            const Vec3 toPatch = g.c - listener;
            if (F[0] * toPatch.x + F[1] * toPatch.y + F[2] * toPatch.z < 0.0) { F[0] = -F[0]; F[1] = -F[1]; F[2] = -F[2]; }
            const double dist = length(rel);
            v.ok = true;
            v.box = g.box; v.U = g.U; v.V = g.V; v.hu = g.hu; v.hv = g.hv;
            v.geoBase = static_cast<float>(omega / (kPi * g.area));
            v.room = sceneRoomAt(sc, g.c + g.n * std::max(0.3f, 1.25f * sc.roomCell));
            v.dist = static_cast<float>(dist);
            for (int b = 0; b < kNumBands; ++b) v.air[b] = airEnergy(b, v.dist);
            v.meanDir = Vec3(static_cast<float>(F[0] / omega), static_cast<float>(F[1] / omega), static_cast<float>(F[2] / omega));
            v.toL = rel * static_cast<float>(1.0 / dist);
            v.n = g.n; v.c = g.c;
            const int mi = sc.material[static_cast<std::size_t>(g.box)];
            v.scatter = (mi >= 0 && mi < static_cast<int>(sc.scatter1k.size())) ? sc.scatter1k[static_cast<std::size_t>(mi)] : 0.5f;
        }
    }
    const PatchView& view(int patch) const { return views_[static_cast<std::size_t>(patch)]; }
    bool touched(int patch) const { return patch >= 0 && patch < static_cast<int>(stamp_.size()) && stamp_[static_cast<std::size_t>(patch)] == frame_; }
    int  visTests() const { return visTests_; }

    int  solvedCount() const { return static_cast<int>(pending_.size()); }

private:
    std::vector<std::uint32_t> stamp_;
    std::vector<PatchView> views_;
    std::vector<int> pending_;
    std::uint32_t frame_ = 0u;
    int visTests_ = 0;
};

/// 面ごとのタップ。
struct FaceTap {
    int   id = -1;
    int   box = -1, face = -1, order = 0;    // order: 0 = 1 回目の当たり、1 = それ以降
    float delaySec = 0.0f;
    Vec3  dirWorld{0, 0, 0};                 // 耳 → 面（単位。方向なしの 1 本は 0）
    float spread = 1.0f;
    float e6[kNumBands] = {};
    Vec3  point{0, 0, 0};                    // 量で重みを付けた小片の中心（地図用）
};

struct FaceTapSet {
    static constexpr int kMax = 48;
    static constexpr int kIdBase = 0x40000000;          // 虚像の素性（16 + … < 2^22）と重ならない
    static constexpr int kIdRest = 0x40000000 - 1;      // 上限を超えて畳んだ方向なしの 1 本
    static constexpr int kIdDoor = 0x40000000 - 2;      // 隣の部屋の閉じ込め: 耳の部屋の面の反射を移した戸口の 1 本
    FaceTap tap[kMax];
    int   count = 0;
    float total6[kNumBands] = {};                        // 初期の総量（帳簿の初期）
    float firstSec = -1.0f;                              // 量の 2% 以上あるタップの最短の遅れ（尾の開始）
    int   deposits = 0;                                  // 数えた受け取りの件数（情報）
    int   shadowRays = 0;                                // 縁の小片で引いた影の線の本数（情報）
};

/// 隣の部屋の閉じ込め（2026-09-14）。音源が耳と別の部屋にいるとき、耳の部屋の面で受けた反射を amount だけ戸口の 1 本へ移す。
///   向きは戸口の中心、遅れは戸口を通る経路（音源 → 戸口 → 耳）、広がりは戸口の見込み。量は移すだけ（総量は変えない）。
///   ★発注者「隣の部屋の残響・反射は今いる部屋では反響させず、ドアから鳴る音が絶対に支配的になるように」。
struct FaceContain {
    bool  active = false;
    float amount = 0.0f;
    int   listenerRoom = -1;
    Vec3  doorPoint{0, 0, 0};
    float doorDelaySec = 0.0f;
    float doorSpread = 0.0f;
};

/// 面ごとにまとめる途中の器（音源ごとに持ち回す。確保をフレームごとにしない）。
struct FaceAccum {
    int key = -1;
    double e6[kNumBands] = {};
    double c = 0.0, ct = 0.0, ct2 = 0.0;                 // 量の和と、量で重みを付けた到達の和・到達の二乗の和
    double wd = 0.0, wdt = 0.0;                          // 鏡面の重みの和と、それで重みを付けた到達の和
    double ws = 0.0, wst = 0.0, wst2 = 0.0;              // 鏡面の分だけ（量 × lobe²）の和と到達の和・二乗の和（1 回目の遅れ）
    double dir[3] = {0.0, 0.0, 0.0};                     // 鏡面の重み × 向きのベクトル
    double pt[3] = {0.0, 0.0, 0.0};                      // 量 × 小片の中心
};
struct FaceScratch {
    std::vector<int> slotOfKey;                          // [箱*6+面]*2 → accum の番号（stamp が今のときだけ有効）
    std::vector<std::uint32_t> stamp;
    std::uint32_t frame = 0u;
    std::vector<FaceAccum> acc;
};

/// 鏡面の重みの幅（1 − cos の尺度）。0.1 ＝ およそ 26° で 1/e。小片の間隔（3 m 先で 10°）より十分広い。
constexpr float kFaceLobe = 0.1f;
/// 到達時刻の散らばりを広がりに換える尺度（秒）。σ = 2 ms で広がり 0.63。
///   2 ms は 500 Hz の半周期。これより散った写しを 1 本にそろえると、500 Hz より上で櫛形が掃いて聞こえる。
constexpr double kFaceTimeSpread = 0.002;

/// 受け取り（組ごとの配列の並び）→ 面ごとのタップ。
inline void buildFaceTaps(const TraceScene& sc, const PatchLayout& L, const ReceiverView& V, const Vec3& listener,
                          const std::vector<Deposit>* const* groups, int nGroups,
                          float mixSec, FaceScratch& S, FaceTapSet& out, const FaceContain* contain = nullptr) {
    const bool doContain = contain && contain->active && contain->amount > 0.0f;
    double doorE6[kNumBands] = {};
    out = FaceTapSet{};
    const std::size_t keys = static_cast<std::size_t>(L.boxCount) * 12;
    if (S.slotOfKey.size() != keys) { S.slotOfKey.assign(keys, -1); S.stamp.assign(keys, 0u); S.frame = 0u; }
    ++S.frame;
    if (S.frame == 0u) { std::fill(S.stamp.begin(), S.stamp.end(), 0u); S.frame = 1u; }
    S.acc.clear();
    for (int g = 0; g < nGroups; ++g) {
        const std::vector<Deposit>* dv = groups[g];
        if (!dv) continue;
        for (const Deposit& d : *dv) {
            if (d.patch < 0 || d.patch >= L.patchCount || !V.touched(d.patch)) continue;
            const PatchView& v = V.view(d.patch);
            if (!v.ok) continue;
            const double t = static_cast<double>(d.tSec) + static_cast<double>(v.dist) / kSpeedOfSound;
            if (t >= mixSec) continue;
            ++out.deposits;
            // 見え方: 当たった位置から耳へ影の線（横切り無し・別の箱に埋まっていない）
            {
                const float du = (static_cast<float>(d.u8) + 0.5f) / 256.0f, dvv = (static_cast<float>(d.v8) + 0.5f) / 256.0f;
                const Vec3 q0 = v.pointAt(du, dvv) + v.n * kEps;
                float tr[kNumBands]; int cr = 0;
                if (scenePointInsideOther(sc, q0, v.box)) cr = 1;
                else sceneTransmittance(sc, q0, listener, v.box, tr, &cr);
                ++out.shadowRays;
                if (cr != 0) continue;
            }
            double cb[kNumBands]; double cm = 0.0;
            for (int b = 0; b < kNumBands; ++b) { cb[b] = static_cast<double>(d.e6[b]) * v.geoBase * v.air[b]; cm += cb[b]; }
            cm /= kNumBands;
            if (!(cm > 0.0)) continue;
            // 隣の部屋の閉じ込め: 耳の部屋の面で受けた分を amount だけ戸口の 1 本へ
            if (doContain && v.room == contain->listenerRoom) {
                const double a = contain->amount;
                for (int b = 0; b < kNumBands; ++b) { doorE6[b] += cb[b] * a; cb[b] *= (1.0 - a); }
                cm *= (1.0 - a);
                if (!(cm > 0.0)) continue;
            }
            // 鏡面の重み: 入ってきた向きを法線で折り返した向きが、耳を向くほど 1
            const double dn = d.inDir[0] * v.n.x + d.inDir[1] * v.n.y + d.inDir[2] * v.n.z;
            const double rx = d.inDir[0] - 2.0 * dn * v.n.x, ry = d.inDir[1] - 2.0 * dn * v.n.y, rz = d.inDir[2] - 2.0 * dn * v.n.z;
            const double cosR = rx * v.toL.x + ry * v.toL.y + rz * v.toL.z;
            const double lobe = std::exp((cosR - 1.0) / kFaceLobe);
            const double s = std::min(1.0f, std::max(0.0f, v.scatter));
            const double wd = cm * (s + (1.0 - s) * lobe);
            const int key = (L.faceOfPatch[static_cast<std::size_t>(d.patch)] * 2) + (d.order == 0 ? 0 : 1);
            int slot;
            if (S.stamp[static_cast<std::size_t>(key)] != S.frame) {
                S.stamp[static_cast<std::size_t>(key)] = S.frame;
                slot = static_cast<int>(S.acc.size());
                S.slotOfKey[static_cast<std::size_t>(key)] = slot;
                S.acc.push_back(FaceAccum{});
                S.acc.back().key = key;
            } else {
                slot = S.slotOfKey[static_cast<std::size_t>(key)];
            }
            FaceAccum& a = S.acc[static_cast<std::size_t>(slot)];
            for (int b = 0; b < kNumBands; ++b) a.e6[b] += cb[b];
            a.c += cm; a.ct += cm * t; a.ct2 += cm * t * t;
            a.wd += wd; a.wdt += wd * t;
            { const double wsp = cm * lobe * lobe; a.ws += wsp; a.wst += wsp * t; a.wst2 += wsp * t * t; }
            a.dir[0] += wd * v.meanDir.x; a.dir[1] += wd * v.meanDir.y; a.dir[2] += wd * v.meanDir.z;
            a.pt[0] += cm * v.c.x; a.pt[1] += cm * v.c.y; a.pt[2] += cm * v.c.z;
        }
    }
    double doorC = 0.0;
    for (int b = 0; b < kNumBands; ++b) doorC += doorE6[b];
    doorC /= kNumBands;
    if (S.acc.empty() && !(doorC > 0.0)) return;
    // 量の大きい順。上限を超えた分は方向なしの 1 本へ（戸口の 1 本の席は別に残す）
    std::sort(S.acc.begin(), S.acc.end(), [](const FaceAccum& x, const FaceAccum& y) { return x.c > y.c; });
    double totalC = doorC;
    for (int b = 0; b < kNumBands; ++b) out.total6[b] += static_cast<float>(doorE6[b]);
    for (const FaceAccum& a : S.acc) { totalC += a.c; for (int b = 0; b < kNumBands; ++b) out.total6[b] += static_cast<float>(a.e6[b]); }
    const int keep = std::min(static_cast<int>(S.acc.size()), FaceTapSet::kMax - 2);
    for (int i = 0; i < keep; ++i) {
        const FaceAccum& a = S.acc[static_cast<std::size_t>(i)];
        FaceTap& t = out.tap[out.count++];
        const int faceKey = a.key / 2;
        t.box = faceKey / 6; t.face = faceKey % 6; t.order = a.key % 2;
        t.id = FaceTapSet::kIdBase + a.key;
        for (int b = 0; b < kNumBands; ++b) t.e6[b] = static_cast<float>(a.e6[b]);
        // 遅れ: 1 回目は鏡面の分だけの重み（量 × lobe²）で虚像の遅れへ寄せる。鏡面が無い面（lobe がどこでも小さい）は量で。
        //   ★散乱の分まで入れた重み（wd）だと、面全体の遠い所の遅れが混じり、壁から 0.1 m で直接の後 0.6 ms のはずが 1.27 ms になった。
        //     近さの手がかりは 1 回目の短い遅れそのものなので、遅れだけは鏡の点で決める（量と向きは今のまま）。
        const bool specOk = (t.order == 0 && a.ws > 1e-6 * a.c);
        t.delaySec = static_cast<float>(specOk ? a.wst / a.ws : a.ct / a.c);
        const double dx = (a.wd > 0.0) ? a.dir[0] / a.wd : 0.0, dy = (a.wd > 0.0) ? a.dir[1] / a.wd : 0.0, dz = (a.wd > 0.0) ? a.dir[2] / a.wd : 0.0;
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        t.dirWorld = (len > 1e-6) ? Vec3(static_cast<float>(dx / len), static_cast<float>(dy / len), static_cast<float>(dz / len)) : Vec3(0, 0, 0);
        // 広がり = 向きの散らばり（1 − |平均の向き|）と、到達時刻の散らばりの大きい方。
        //   ★面のタップは多くの経路を 1 本にまとめた物で、到達は散っている（それ以降のタップは数 ms〜数十 ms）。それを遅れ 1 つの
        //     そろった写しで鳴らすと、扉と一緒に遅れが動いたとき櫛形のくぼみが正弦の成分を掃き、音量が −1.3 dB くぼんだ
        //     （clicks の扉だけ 44〜65°）。散らばり σ を 1 − e^(−σ/kFaceTimeSpread) で広がりに換え、散った分は拡散の道（位相を撹拌）へ回す。
        //   ★1 回目のタップは鏡面の重み（lobe²）で σ を出す。壁際の鏡の点の遅れはそろっているので広がりは増えず、近さの手がかりは残る。
        const bool specSigma = (t.order == 0 && a.ws > 1e-6 * a.c);
        const double mean = specSigma ? a.wst / a.ws : a.ct / a.c;
        const double var = specSigma ? (a.wst2 / a.ws - mean * mean) : (a.ct2 / a.c - mean * mean);
        const double sigma = std::sqrt(std::max(0.0, var));
        const double timeSpread = 1.0 - std::exp(-sigma / kFaceTimeSpread);
        t.spread = static_cast<float>(std::min(1.0, std::max({0.0, 1.0 - len, timeSpread})));
        t.point = Vec3(static_cast<float>(a.pt[0] / a.c), static_cast<float>(a.pt[1] / a.c), static_cast<float>(a.pt[2] / a.c));
    }
    if (static_cast<int>(S.acc.size()) > keep) {
        FaceTap& t = out.tap[out.count++];
        t.id = FaceTapSet::kIdRest; t.order = 1; t.spread = 1.0f;
        double c = 0.0, ct = 0.0;
        for (std::size_t i = static_cast<std::size_t>(keep); i < S.acc.size(); ++i) {
            const FaceAccum& a = S.acc[i];
            for (int b = 0; b < kNumBands; ++b) t.e6[b] += static_cast<float>(a.e6[b]);
            c += a.c; ct += a.ct;
        }
        t.delaySec = static_cast<float>(c > 0.0 ? ct / c : 0.0);
    }
    if (doorC > 0.0) {
        FaceTap& t = out.tap[out.count++];
        t.id = FaceTapSet::kIdDoor; t.order = 0; t.box = -1; t.face = -1;
        for (int b = 0; b < kNumBands; ++b) t.e6[b] = static_cast<float>(doorE6[b]);
        const Vec3 d = contain->doorPoint - listener;
        const float ld = length(d);
        t.dirWorld = (ld > 1e-4f) ? d * (1.0f / ld) : Vec3(0, 0, 0);
        t.spread = (ld > 1e-4f) ? std::min(1.0f, std::max(0.0f, contain->doorSpread)) : 1.0f;
        t.delaySec = contain->doorDelaySec;
        t.point = contain->doorPoint;
    }
    for (int i = 0; i < out.count; ++i) {
        double cm = 0.0; for (int b = 0; b < kNumBands; ++b) cm += out.tap[i].e6[b];
        cm /= kNumBands;
        if (cm >= 0.02 * totalC && (out.firstSec < 0.0f || out.tap[i].delaySec < out.firstSec)) out.firstSec = out.tap[i].delaySec;
    }
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_RECEIVER_H
