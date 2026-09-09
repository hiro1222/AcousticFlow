/* Flow/probe.h ── プローブ（段 1）
 *
 * ■ 役割
 *   部屋 1 つの「響きの数字」を持つ器。帯域別 RT60・体積・表面積・平均自由行程。
 *   設計文書 Ⅵ「プローブ（帯域別 RT60・体積・方向重み）」の形式。
 *   ★値は部屋グラフ（Core/room_graph.h）から自動で作る。手で置くのは上書きしたいときだけ（追記 E）。
 *   ★方向重みは持たない。戸口越しの尾の向きは幾何で毎フレーム出す（distribute の仕事。地図 7 章）。
 *
 * ■ 中の仕組み
 *   1) probeFromRoom: 部屋グラフの Room から体積 V（ボクセル数 × セル³）、境界面積 S、面積平均の吸音率、
 *      平均自由行程 4V/S を写す。RT60 は Sabine:
 *          RT60_b = 0.161 · V / (A_b + 4 m_b V)
 *      A_b = S · ᾱ_b（開口は吸音率 1 で入っている＝出た音は戻らない）、m_b は空気吸収（rooms::kAirDbPerM）。
 *      0.161 は 24 ln10 / c（c = 343）。4mV は空気吸収を面積に換算した項。
 *   2) applyOpenings: 開口に板（扉）があるとき、その面積ぶんの吸音率を
 *          eff_b = a + (1 − a) · min(1, α_leaf,b + τ_leaf,b)
 *      に置き換える。a は板の素通しの面積率（0 閉 … 1 全開）。全開なら 1（穴）、閉じれば板の吸音＋透過
 *      （透過した音もこの部屋には戻らないので吸音と同じ扱い）。
 *      ★これが「扉を閉めると部屋自身の RT60 も伸びる」の正体で、旧実装の 手順 3 と同じ式（閉扉で +4.5%）。
 *      同じ a が distribute の部屋間混合にも使われる（同じ α で両方に効く）。
 *   3) fdnLineScale: FDN の線の長さの尺度。平均自由行程 ÷ 音速 ÷ 12 ms。追記 G の「係数 1 つ」。
 *
 * ■ 繋がり
 *   受ける: rooms::Room（部屋グラフ）、OpeningState（どの口に板があり、どれだけ開いているか。
 *           世界側が扉の角度から出す）。
 *   渡す:   rt60 と fdnLineScale を FdnRoomMix（addRoom / setRoomRt60）へ。V・S は distribute の結合の式へ。
 *
 * ■ 退けた書き方
 *   ・RT60 を IR（エコグラム）から測る: 同じ部屋・同じ材質に対して Sabine と実測の 2 つの答えができ、
 *     旧実装では 500 Hz で 0.88 s 対 0.71 s に割れた。形と材質から出す 1 つの答えにする。
 *   ・プローブを手で置く: authoring になる。部屋グラフが自動で出すので、上書きの口だけ残す。
 *   ・開口の板を面積の増減で表す: 面積は固定して係数だけ動かす。部屋グラフを作り直さずに済む
 *     （作り直しは 16 ms、係数は µs）。
 *
 * ■ 壊れる所
 *   ・Sabine の式を他所（room_graph.h の rt60 欄）と別々に持つと、また答えが 2 つになる。
 *     ここでは検査で「開口を全開にした probe.rt60 == room.rt60」を毎回確かめている。
 *     段 10 で room_graph 側の rt60 欄は消し、ここだけにする。
 *   ・eff を a だけにする（板の吸音を忘れる）と、閉めた扉が「完全反射の壁」になり、
 *     木の扉なのに石壁より響く。
 *   ・A_b が 0 に落ちる（全面が完全反射）と RT60 が無限大。max(A, 1e-3) で受ける。
 */
#ifndef ACOUSTICFLOW_FLOW_PROBE_H
#define ACOUSTICFLOW_FLOW_PROBE_H

#include <algorithm>
#include <cmath>
#include "Core/material.h"
#include "Core/room_graph.h"
#include "Core/vec3.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

/// 開口 1 つの「いまの状態」。世界側（扉の角度を知っている側）が作る。
struct OpeningState {
    int   room = -1;                 // どの部屋の口か（両側の部屋それぞれに 1 つずつ作る）
    float area = 0.0f;               // 口の面積 m²
    float openFrac = 1.0f;           // 板の素通しの面積率 a（0 閉 … 1 全開）。板が無ければ 1
    float leafAbsorb[kNumBands] = {1, 1, 1, 1, 1, 1};   // 板の 吸音+透過（エネルギー比、1 で頭打ち）。板が無ければ 1
};

struct Probe {
    float volume = 0.0f;             // m³
    float surface = 0.0f;            // m²。境界の面積（開口を含む）
    float openArea = 0.0f;           // m²。うち開口
    float absorbStatic[kNumBands] = {};   // 面積平均の吸音率（開口は 1 として入っている）
    float rt60[kNumBands] = {};      // 生きた RT60（開口の板を反映した後）
    float meanFreePath = 0.0f;       // 4V/S（m）。FDN の線の長さの尺度
    Vec3  centroid{0, 0, 0};
    Vec3  boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
    bool  overridden = false;        // 手で上書きされた（部屋グラフの再生成で戻さない）
};

/// Sabine（空気吸収込み）。★この式はここ 1 か所。
inline float sabineRt60(float volume, float absorbArea, int band) {
    const double mAir = rooms::kAirDbPerM[band] * 0.1151;                 // dB/m → ネーパ/m
    const double denom = std::max(static_cast<double>(absorbArea), 1e-3) + 4.0 * mAir * volume;
    return static_cast<float>(0.161 * volume / denom);
}

/// 部屋グラフの Room からプローブを作る（開口は全開＝穴として）。
inline Probe probeFromRoom(const rooms::Room& rm, float cell) {
    Probe p;
    const double c = cell;
    p.volume = static_cast<float>(rm.voxels * c * c * c);
    p.surface = rm.surface;
    p.openArea = rm.openArea;
    p.meanFreePath = (rm.surface > 1e-6f) ? 4.0f * p.volume / rm.surface : 0.0f;
    p.centroid = rm.centroid; p.boundsMin = rm.boundsMin; p.boundsMax = rm.boundsMax;
    for (int b = 0; b < kNumBands; ++b) {
        p.absorbStatic[b] = rm.absorb[b];
        p.rt60[b] = sabineRt60(p.volume, rm.absorb[b] * rm.surface, b);
    }
    return p;
}

/// 開口の板（扉）を反映して rt60 を出し直す。openings のうち p の部屋の物だけ効く。
///   room: この probe の部屋番号。手で上書きされた probe には触らない。
inline void applyOpenings(Probe& p, int room, const OpeningState* openings, int count) {
    if (p.overridden) return;
    double A[kNumBands];
    for (int b = 0; b < kNumBands; ++b) A[b] = static_cast<double>(p.absorbStatic[b]) * p.surface;
    for (int i = 0; i < count; ++i) {
        const OpeningState& o = openings[i];
        if (o.room != room || o.area <= 0.0f) continue;
        const double a = std::min(1.0, std::max(0.0, static_cast<double>(o.openFrac)));
        for (int b = 0; b < kNumBands; ++b) {
            const double leaf = std::min(1.0, std::max(0.0, static_cast<double>(o.leafAbsorb[b])));
            const double eff = a + (1.0 - a) * leaf;            // 穴の割合 + 板の割合 × 板の吸音
            A[b] -= static_cast<double>(o.area) * (1.0 - eff); // 開口は 1 で入っていたので、減る分だけ引く
        }
    }
    for (int b = 0; b < kNumBands; ++b) p.rt60[b] = sabineRt60(p.volume, static_cast<float>(A[b]), b);
}

/// FDN の線の長さの尺度（FdnRoomMix::addRoom の lineScale）。
///   基準の線（12 ms ≒ 4 m の部屋）に対する比。平均自由行程 ÷ 音速 ÷ 12 ms。
///   ★設計文書は「遅延線長は固定」だが、追記 G のとおり係数 1 つは残す（広さの手掛かり）。
///   1 m 〜 16 m の部屋を想定して 0.25 〜 4 に収める（線が短すぎると金属的、長すぎると疎になる）。
inline float fdnLineScale(const Probe& p) {
    const float base = 0.012f;
    const float t = p.meanFreePath / kSpeedOfSound;
    return std::max(0.25f, std::min(4.0f, (t > 1e-6f) ? t / base : 1.0f));
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_PROBE_H
