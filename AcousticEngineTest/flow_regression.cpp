/* AcousticEngineTest/flow_regression.cpp ── 新コアの検査（段 1: 形式）
 *
 * 期待値は「絶対値」でなく「関係」で書く（保存則、単調、一致、静止で 0）。
 * 段が進むごとにここへ足す。AF_ONLY=<name> で 1 つだけ走らせられる。
 */
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "test_instruments.h"
#include "../AcousticEngine/src/Core/aabb.h"
#include "../AcousticEngine/src/Core/room_graph.h"
#include "../AcousticEngine/src/Flow/world_rules.h"
#include "../AcousticEngine/src/Flow/probe.h"
#include "../AcousticEngine/src/Flow/emitter.h"
#include "../AcousticEngine/src/Flow/mix.h"
#include "../AcousticEngine/src/Flow/surfaces.h"
#include "../AcousticEngine/src/Flow/energy_trace.h"
#include "../AcousticEngine/src/Flow/response.h"
#include "../AcousticEngine/src/Flow/diffraction.h"
#include "../AcousticEngine/src/Flow/budget.h"
#include "../AcousticEngine/src/Flow/image_sources.h"
#include "../AcousticEngine/src/Flow/distribute.h"
#include "../AcousticEngine/src/Flow/mix_to_voice.h"
#include "../AcousticEngine/src/Flow/world.h"
#include "../AcousticEngine/src/Gpu/compute_d3d11.h"
#include "../AcousticEngine/src/Gpu/trace_gpu.h"

using namespace acoustic;
using namespace acoustic::flow;
using afti::check;

namespace {

// 14 m 角・高さ 3 m の箱を z=0 の壁で 2 部屋に割り、幅 1 m の戸口を開ける（旧テストと同じ形）。
//   扉の板は入れない（動く物は部屋グラフから外す決まり）。
std::vector<rooms::SolidBox> twoRoomsWithDoor(const AcousticMaterial& mat) {
    const float t = 0.2f, h = 3.0f, half = 7.0f, doorW = 1.0f;
    std::vector<rooms::SolidBox> v;
    auto add = [&](Vec3 c, Vec3 he) {
        rooms::SolidBox sb; sb.obb = Obb::axisAligned(c, he);
        for (int b = 0; b < kNumBands; ++b) sb.absorption[b] = mat.absorption[b];
        v.push_back(sb);
    };
    add(Vec3(0, -t, 0), Vec3(half + t, t, half + t));
    add(Vec3(0, h + t, 0), Vec3(half + t, t, half + t));
    add(Vec3(-half - t, h * 0.5f, 0), Vec3(t, h * 0.5f, half + t));
    add(Vec3(half + t, h * 0.5f, 0), Vec3(t, h * 0.5f, half + t));
    add(Vec3(0, h * 0.5f, -half - t), Vec3(half + t, h * 0.5f, t));
    add(Vec3(0, h * 0.5f, half + t), Vec3(half + t, h * 0.5f, t));
    const float side = (2.0f * half - doorW) * 0.5f;
    add(Vec3(-(doorW * 0.5f + side * 0.5f), h * 0.5f, 0), Vec3(side * 0.5f, h * 0.5f, t));
    add(Vec3((doorW * 0.5f + side * 0.5f), h * 0.5f, 0), Vec3(side * 0.5f, h * 0.5f, t));
    return v;
}

// ================================ [決まり] world_rules
void testWorldRules() {
    std::printf("\n[決まり] 帯域表・世界の重み・面の分配\n");
    float worst = 0.0f;
    for (int c = 0; c < kNumBands - 1; ++c) {
        const float gm = std::sqrt(kBandHz[c] * kBandHz[c + 1]);
        worst = std::max(worst, std::fabs(kCrossHz[c] - gm) / gm);
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(相乗平均とのずれ 最大 %.4f%%)", worst * 100.0f);
    check("[決まり] 帯域の境目が隣り合う中心の相乗平均", worst < 1e-3f, buf);
    float sum = 0.0f; for (int b = 0; b < kNumBands; ++b) sum += bandWidthHz(b, 48000.0f);
    std::snprintf(buf, sizeof(buf), "(幅の和 %.1f Hz)", sum);
    check("[決まり] 帯域の幅の和が fs/2", std::fabs(sum - 24000.0f) < 0.5f, buf);
    WorldWeights w;
    check("[決まり] 既定の世界の重みは全部 1", w.isIdentity());
    MaterialTable tbl;
    const int id = tbl.add(AcousticMaterial::woodDoor());
    bool ok = true;
    for (int b = 0; b < kNumBands; ++b) {
        const SurfaceSplit s = splitAt(tbl.get(id), b);
        if (std::fabs(s.reflect + s.transmit + s.absorb - 1.0f) > 1e-6f || s.reflect < 0.0f) ok = false;
    }
    check("[決まり] 面の分配（反射+透過+吸収）の和が 1、反射は負にならない", ok);
    AcousticMaterial bad = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { bad.absorption[b] = 0.8f; bad.transmission[b] = 0.5f; }
    const SurfaceSplit s = splitAt(bad, 0);
    std::snprintf(buf, sizeof(buf), "(α 0.8 + τ 0.5 → 反射 %.2f 透過 %.2f)", s.reflect, s.transmit);
    check("[決まり] α+τ>1 の材質でも反射が負にならない（τ を頭打ち）", s.reflect >= 0.0f && std::fabs(s.reflect + s.transmit + s.absorb - 1.0f) < 1e-6f, buf);
    check("[決まり] 材質 0 番は既定の壁（無効な番号も 0 番に落ちる）", tbl.get(-1).absorption[0] == AcousticMaterial::defaultWall().absorption[0]);
}

// ================================ [プローブ] probe
void testProbe() {
    std::printf("\n[プローブ] 部屋グラフからの生成・開口の板・Sabine の一致\n");
    rooms::Builder builder;
    const auto boxes = twoRoomsWithDoor(AcousticMaterial::defaultWall());
    const rooms::Result& res = builder.build(boxes);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "(部屋 %d 個、開口 %d 箇所)", static_cast<int>(res.rooms.size()), static_cast<int>(res.apertures.size()));
    check("[プローブ] 戸口で割った箱は部屋 2 個・開口 1 箇所", res.rooms.size() == 2 && res.apertures.size() == 1, buf);
    if (res.rooms.size() != 2 || res.apertures.size() != 1) return;

    // 1) 全開（穴）のプローブは部屋グラフの Sabine と一致する（式が 1 つであることの証明）。
    float worst = 0.0f;
    for (std::size_t r = 0; r < res.rooms.size(); ++r) {
        const Probe p = probeFromRoom(res.rooms[r], res.grid.cell);
        for (int b = 0; b < kNumBands; ++b)
            worst = std::max(worst, std::fabs(p.rt60[b] - res.rooms[r].rt60[b]) / std::max(res.rooms[r].rt60[b], 1e-6f));
    }
    std::snprintf(buf, sizeof(buf), "(相対のずれ 最大 %.5f)", worst);
    check("[プローブ] 全開のプローブの RT60 が部屋グラフの Sabine と一致（Sabine は 1 か所）", worst < 1e-4f, buf);

    // 2) 開口の板: a=1 で不変、a=0（木の扉）で伸びる、a に対して単調。
    const rooms::Aperture& ap = res.apertures[0];
    const AcousticMaterial leaf = AcousticMaterial::woodDoor();
    auto rt500 = [&](int room, float a) {
        Probe p = probeFromRoom(res.rooms[static_cast<std::size_t>(room)], res.grid.cell);
        OpeningState o; o.room = room; o.area = ap.area; o.openFrac = a;
        for (int b = 0; b < kNumBands; ++b) o.leafAbsorb[b] = std::min(1.0f, leaf.absorption[b] + leaf.transmission[b]);
        applyOpenings(p, room, &o, 1);
        return p.rt60[2];
    };
    const int rA = ap.roomA, rB = ap.roomB;
    const float openA = rt500(rA, 1.0f), closedA = rt500(rA, 0.0f);
    const float openB = rt500(rB, 1.0f), closedB = rt500(rB, 0.0f);
    const float baseA = probeFromRoom(res.rooms[static_cast<std::size_t>(rA)], res.grid.cell).rt60[2];
    std::snprintf(buf, sizeof(buf), "(部屋%d 500Hz: 全開 %.3f s → 閉 %.3f s、部屋%d: %.3f → %.3f s)", rA, openA, closedA, rB, openB, closedB);
    check("[プローブ] 全開の板は RT60 を変えない", std::fabs(openA - baseA) < 1e-6f, buf);
    check("[プローブ] 扉を閉めると両部屋の RT60 が伸びる（開口が板になる）", closedA > openA && closedB > openB, buf);
    bool mono = true; float prev = -1.0f;
    for (int k = 0; k <= 10; ++k) { const float v = rt500(rA, 1.0f - 0.1f * k); if (prev > 0.0f && v < prev - 1e-6f) mono = false; prev = v; }
    check("[プローブ] 閉めるほど RT60 が単調に伸びる（0.1 刻み）", mono);

    // 3) 手で上書きしたプローブには触らない。
    Probe over = probeFromRoom(res.rooms[static_cast<std::size_t>(rA)], res.grid.cell);
    over.overridden = true; over.rt60[2] = 9.0f;
    OpeningState o; o.room = rA; o.area = ap.area; o.openFrac = 0.0f;
    applyOpenings(over, rA, &o, 1);
    check("[プローブ] 上書きされたプローブは開口の板で変わらない", over.rt60[2] == 9.0f);

    // 4) 線の尺度: 14 m の半分（7 m × 14 m × 3 m）で 1 を超え、上限 4 以内。
    const Probe pa = probeFromRoom(res.rooms[static_cast<std::size_t>(rA)], res.grid.cell);
    std::snprintf(buf, sizeof(buf), "(平均自由行程 %.2f m、lineScale %.2f、体積 %.0f m3)", pa.meanFreePath, fdnLineScale(pa), pa.volume);
    check("[プローブ] 7×14×3 m の部屋の線の尺度が 0.25〜4 の中", fdnLineScale(pa) > 0.25f && fdnLineScale(pa) < 4.0f, buf);
}

// ================================ [音源] emitter
void testEmitter() {
    std::printf("\n[音源] リスナー座標・変化量・見込み角\n");
    Listener L; L.pos = Vec3(0, 1.6f, 0); L.forward = Vec3(0, 0, 1); L.up = Vec3(0, 1, 0);
    const Vec3 r = L.toLocal(Vec3(1, 0, 0)), f = L.toLocal(Vec3(0, 0, 1));
    check("[音源] 正面向きで world +x は右（local +x）", std::fabs(r.x - 1.0f) < 1e-5f && std::fabs(r.z) < 1e-5f);
    check("[音源] 正面向きで world +z は前（local +z）", std::fabs(f.z - 1.0f) < 1e-5f && std::fabs(f.x) < 1e-5f);
    L.forward = Vec3(1, 0, 0);
    const Vec3 r2 = L.toLocal(Vec3(0, 0, -1));
    check("[音源] +x を向くと world −z が右（左手系の規約）", std::fabs(r2.x - 1.0f) < 1e-5f);
    L.forward = Vec3(0.6f, 0, 0.8f); L.up = Vec3(0, 3, 0);      // 正規化していない入力
    const Vec3 rr = L.toLocal(Vec3(0.8f, 0, -0.6f));
    check("[音源] forward/up が正規化されていなくても右が単位", std::fabs(rr.x - 1.0f) < 1e-4f);

    Emitter e; e.id = 0; e.pos = Vec3(3, 1, 4); e.prevPos = e.pos;
    e.beginFrame(1.0f / 60.0f);
    check("[音源] 静止で変化量が 0", e.change == 0.0f);
    e.pos = Vec3(3, 1, 4.1f); e.beginFrame(0.1f);
    char buf[96]; std::snprintf(buf, sizeof(buf), "(0.1 m を 0.1 s で → %.2f m/s)", e.change);
    check("[音源] 動くと変化量が速度（m/s）になる", std::fabs(e.change - 1.0f) < 1e-3f, buf);
    e.beginFrame(0.1f);
    check("[音源] 止まった次のフレームで変化量が 0 に戻る", e.change == 0.0f);
    e.addChange(2.0f); e.addChange(-5.0f);
    check("[音源] addChange は負を足さない", std::fabs(e.change - 2.0f) < 1e-6f);

    e.radius = 0.3f;
    const float near = e.effectiveRadius(1.0f), far = e.effectiveRadius(100.0f);
    std::snprintf(buf, sizeof(buf), "(0.3 m の音源: 1 m で %.2f、100 m で %.2f)", near, far);
    check("[音源] 近くでは幅のまま、遠くでは点", std::fabs(near - 0.3f) < 1e-5f && far == 0.0f, buf);
    bool mono = true; float prev = 1.0f;
    for (float d = 1.0f; d < 120.0f; d *= 1.1f) { const float w = e.effectiveRadius(d); if (w > prev + 1e-6f) mono = false; prev = w; }
    check("[音源] 幅は距離に対して単調に縮む（跳ばない）", mono);
    check("[音源] 予算の detail=0 で点", e.effectiveRadius(1.0f, 0.0f) == 0.0f);
    check("[音源] 幅 0 は常に点", Emitter{}.effectiveRadius(1.0f) == 0.0f);
}

// ================================ [配分の形式] mix
void testMix() {
    std::printf("\n[配分の形式] 帳簿の保存則\n");
    Mix m; m.clear();
    for (int b = 0; b < kNumBands; ++b) {
        m.component6[kDirect][b] = 0.5f; m.component6[kEarly][b] = 0.2f; m.component6[kLate][b] = 0.2f;
        m.component6[kDiffract][b] = 0.05f; m.component6[kTransmit][b] = 0.05f;
        m.energy6[b] = 1.0f;
    }
    check("[配分の形式] 五成分の和 ＝ 総量 なら conserves", m.conserves(0.1f));
    m.component6[kLate][3] = 0.1f;
    check("[配分の形式] 1 帯域でも 0.1 足りなければ落ちる（−0.46 dB）", !m.conserves(0.1f));
    m.clear();
    check("[配分の形式] 全部 0 は保存とみなす", m.conserves());
    for (int i = 0; i < Mix::kMaxTaps + 3; ++i) m.pushTap();
    check("[配分の形式] タップは上限で止まる（黙って落ちるのは呼び手の責任）", m.tapCount == Mix::kMaxTaps);
}

// ================================ [物差し] test_instruments
void testInstruments() {
    std::printf("\n[物差し] 帯域分割・T60・連続性・クリック\n");
    const int fs = 48000, n = fs * 2;
    afti::Xorshift rng;
    std::vector<float> noise(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) noise[static_cast<std::size_t>(i)] = rng.next() * 0.3f;
    std::vector<float> bands; afti::split6(noise.data(), n, fs, bands, 2);
    // 帯域の信号の和は元に戻る（構成上）。エネルギーの和は境目の重なりで元より大きい（参考に出す）。
    double maxErr = 0.0, sum = 0.0;
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int b = 0; b < 6; ++b) s += bands[static_cast<std::size_t>(b) * n + i];
        maxErr = std::max(maxErr, std::fabs(s - noise[static_cast<std::size_t>(i)]));
    }
    for (int b = 0; b < 6; ++b) sum += afti::energy(bands.data() + static_cast<std::size_t>(b) * n, n);
    const double tot = afti::energy(noise.data(), n);
    std::printf("      参考: 帯域のエネルギーの和 %.2f dB 対 元 %.2f dB（境目の重なり +%.2f dB。帯域の数字は同じ物差しどうしで比べる）\n",
                afti::dB(sum), afti::dB(tot), afti::dB(sum) - afti::dB(tot));
    char buf[128]; std::snprintf(buf, sizeof(buf), "(サンプルの最大誤差 %.2e)", maxErr);
    check("[物差し] 6 帯域の信号の和が元に戻る（構成上の完全再構成）", maxErr < 1e-4, buf);

    // 合成の指数減衰（T60 = 0.8 s）を当てる。
    std::vector<float> decay(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) decay[static_cast<std::size_t>(i)] = rng.next() * std::pow(10.0f, -3.0f * i / (0.8f * fs));
    const float t60 = afti::fitT60(decay.data(), n, fs, 0.02f);
    std::snprintf(buf, sizeof(buf), "(0.80 s の合成減衰 → %.3f s)", t60);
    check("[物差し] EDC の当てが合成の T60 を ±3% で戻す", std::fabs(t60 - 0.8f) < 0.024f, buf);

    // 定常の正弦の和は隣り合うブロックで揺れない。
    afti::SineSum sig(fs, 512); std::vector<float> tone(static_cast<std::size_t>(n));   // ★ブロックと同期
    for (int i = 0; i < n; ++i) tone[static_cast<std::size_t>(i)] = sig.next();
    const double steps = afti::blockLevelSteps(tone.data(), n, 512);
    std::snprintf(buf, sizeof(buf), "(最大 %.3f dB)", steps);
    check("[物差し] 定常の 5 音の和は隣り合うブロックで 0.2 dB も揺れない", steps < 0.2, buf);
    const double calm = afti::sampleStepRatio(tone.data(), 512);
    tone[300] += 0.5f;   // クリックを 1 つ入れる
    const double clicked = afti::sampleStepRatio(tone.data(), 512);
    std::snprintf(buf, sizeof(buf), "(平常 %.3f → クリック %.3f、%.0f 倍)", calm, clicked, clicked / std::max(calm, 1e-9));
    check("[物差し] 波形の飛びは平常の 4 倍以上に出る", clicked > 4.0 * calm, buf);
}

// ================================ [レイ] energy_trace（段 2）
//   閉じた箱（7×14×3 m、扉なし）で: 保存則、直接 1/(4πd²)、反射の総量が部屋定数 4/R と合うこと、
//   壁越しの透過、種の固定で静止なら揺れないこと、初期／後期の境。
Surfaces closedBox(const float half, const float h, int material, float t = 0.2f) {
    Surfaces s;
    auto add = [&](Vec3 c, Vec3 he) { s.add(Obb::axisAligned(c, he), material); };
    add(Vec3(0, -t, 0), Vec3(half + t, t, half + t));
    add(Vec3(0, h + t, 0), Vec3(half + t, t, half + t));
    add(Vec3(-half - t, h * 0.5f, 0), Vec3(t, h * 0.5f, half + t));
    add(Vec3(half + t, h * 0.5f, 0), Vec3(t, h * 0.5f, half + t));
    add(Vec3(0, h * 0.5f, -half - t), Vec3(half + t, h * 0.5f, t));
    add(Vec3(0, h * 0.5f, half + t), Vec3(half + t, h * 0.5f, t));
    return s;
}
void testEnergyTrace() {
    std::printf("\n[レイ] 保存則・直接音・部屋定数・透過・種の固定・初期と後期\n");
    MaterialTable mats;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.0f; wall.scattering[b] = 0.5f; }
    const int matId = mats.add(wall);
    const float half = 3.5f, h = 3.0f;                       // 7 × 7 × 3 m（部屋定数の式は等方な箱ほど合う）
    Surfaces box = closedBox(half, h, matId);
    const Vec3 S(1.5f, 1.6f, -1.0f), L(-1.0f, 1.2f, 1.5f);
    TraceParams prm; prm.rays = 4096; prm.maxBounces = 40; prm.mixingSec = 0.03f; prm.seed = 7u;
    EnergyTrace tr;
    const TraceResult R = tr.run(box, mats, S, L, prm);
    char buf[200];

    // 保存則（レイ側の帳簿）
    double worst = 0.0;
    for (int b = 0; b < kNumBands; ++b) {
        const double lhs = R.absorbed6[b] + R.remainder6[b] + R.escaped6[b];
        worst = std::max(worst, std::fabs(lhs - R.emitted6[b]) / std::max(R.emitted6[b], 1e-9));
    }
    std::snprintf(buf, sizeof(buf), "(放射 %.3f = 吸収 %.3f + 残り %.4f + 逃げ %.4f、ずれ %.2e、ヒット %d)",
                  R.emitted6[2], R.absorbed6[2], R.remainder6[2], R.escaped6[2], worst, R.hits);
    check("[レイ] 保存則: 放射 = 吸収 + 打ち切りの残り + 逃げ（全帯域 1e-4 以内）", worst < 1e-4f, buf);
    check("[レイ] 閉じた箱からは逃げない", R.escaped6[2] == 0.0f);

    // 直接音: 自由音場で 1/(4πd²)（見通しの割合は aperture が別に出す）
    const float d = length(L - S);
    const float expect = 1.0f / (4.0f * 3.14159265f * d * d);
    std::snprintf(buf, sizeof(buf), "(d %.2f m: 自由音場 %.3e 対 1/4πd² %.3e、横切り %d 枚)", d, R.freeDirect6[2], expect, R.directCrossings);
    check("[レイ] 自由音場の直接音は 1/(4πd²)（空気吸収を除いて 0.1%）、見通しなら横切り 0 枚", std::fabs(R.freeDirect6[2] / expect - 1.0f) < 0.01f && R.directCrossings == 0, buf);

    // 部屋定数: 反射の総量 ≈ 4(1−ᾱ)/(Sᾱ)（拡散音場の古典式）。
    const float Sarea = 2.0f * ((2 * half) * (2 * half) + 2.0f * (2 * half) * h);
    const float alpha = 0.2f;
    const float expectRev = 4.0f * (1.0f - alpha) / (Sarea * alpha);
    const float gotRev = R.reflected6(2);
    const double ratioDb = afti::dB(gotRev / expectRev);
    std::snprintf(buf, sizeof(buf), "(反射の総量 %.3e 対 4(1-a)/(S a) %.3e、%+.2f dB。初期 %.1f%% 後期 %.1f%%)",
                  gotRev, expectRev, ratioDb, 100.0f * R.early6[2] / gotRev, 100.0f * R.late6[2] / gotRev);
    check("[レイ] 反射の総量が部屋定数の式と ±1.5 dB で合う（同じ S と α から）", std::fabs(ratioDb) < 1.5, buf);
    check("[レイ] 初期にも後期にもエネルギーが入る", R.early6[2] > 0.0f && R.late6[2] > 0.0f);
    std::snprintf(buf, sizeof(buf), "(最初の反射 %.1f ms、直接 %.1f ms)", R.firstReflectSec * 1000.0f, R.directSec * 1000.0f);
    check("[レイ] 最初の反射は直接音より後に届く", R.firstReflectSec > R.directSec, buf);

    // 種の固定: 同じ入力なら同じ出力（ビット一致）。
    const TraceResult R2 = tr.run(box, mats, S, L, prm);
    // ★memcmp で構造体を比べない: double と float が混ざるとパディングが入り、そこは初期化されない。
    auto same = [](const TraceResult& a, const TraceResult& b) {
        for (int i = 0; i < kNumBands; ++i)
            if (a.freeDirect6[i] != b.freeDirect6[i] || a.early6[i] != b.early6[i] || a.late6[i] != b.late6[i]
                || a.emitted6[i] != b.emitted6[i] || a.absorbed6[i] != b.absorbed6[i] || a.remainder6[i] != b.remainder6[i]) return false;
        return a.firstReflectSec == b.firstReflectSec && a.hits == b.hits && a.neeVisible == b.neeVisible;
    };
    check("[レイ] 同じ種・同じ幾何なら結果がビット一致（静止で揺れない）", same(R, R2));
    // 2 cm 動いたときの変化は小さい（相関した標本）。新しい種だと分散ぶん動く。
    const TraceResult R3 = tr.run(box, mats, S, L + Vec3(0.02f, 0, 0), prm);
    TraceParams prm2 = prm; prm2.seed = 8u;
    const TraceResult R4 = tr.run(box, mats, S, L, prm2);
    const double stepSame = std::fabs(afti::dB(R3.reflected6(2) / R.reflected6(2)));
    const double stepSeed = std::fabs(afti::dB(R4.reflected6(2) / R.reflected6(2)));
    std::snprintf(buf, sizeof(buf), "(2 cm 動いて %.3f dB、種を変えると %.3f dB)", stepSame, stepSeed);
    check("[レイ] 2 cm の移動での変化は 0.3 dB 未満（種を固定した相関標本）", stepSame < 0.3, buf);

    // 透過: 音源と聞き手の間に板を置く。直接が消え、透過に τ の積が出る。
    AcousticMaterial leaf = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { leaf.absorption[b] = 0.1f; leaf.transmission[b] = 0.01f; }
    const int leafId = mats.add(leaf);
    Surfaces box2 = closedBox(half, h, matId);
    box2.add(Obb::axisAligned(Vec3(0, h * 0.5f, 0.25f), Vec3(half, h * 0.5f, 0.02f)), leafId);   // 間仕切り
    const TraceResult T = tr.run(box2, mats, S, L, prm);
    const Visibility vt = discVisibility(box2, mats, L, S, 0.0f);                    // 点音源の見通し（段 5 aperture）
    std::snprintf(buf, sizeof(buf), "(横切り %d 枚: 見通し %.2f、遮る物 %d、τ %.4f)", T.directCrossings, vt.visible, vt.shadowers, vt.shadowTau6[2]);
    check("[レイ] 板を挟むと横切り 1 枚、見通し 0、遮る物の τ = 0.01（直接 = free × 0、透過 = free × τ）",
          T.directCrossings == 1 && vt.visible == 0.0f && vt.shadowers == 1 && std::fabs(vt.shadowTau6[2] - 0.01f) < 1e-4f, buf);
    check("[レイ] 板を挟んでも反射の総量は残る（板の向こうで反射して抜けてくる）", T.reflected6(2) > 0.0f && T.reflected6(2) < gotRev);

    // 初期／後期の境: mixing を 0 にすると全部後期、大きくすると全部初期。
    TraceParams p0 = prm; p0.mixingSec = 0.0f;
    TraceParams p1 = prm; p1.mixingSec = 10.0f;
    const TraceResult A0 = tr.run(box, mats, S, L, p0), A1 = tr.run(box, mats, S, L, p1);
    check("[レイ] 境が 0 なら全部後期、境が大きければ全部初期（総量は同じ）",
          A0.early6[2] == 0.0f && A1.late6[2] == 0.0f && std::fabs(A0.reflected6(2) - A1.reflected6(2)) < 1e-6f);
    // ── BVH と総当たりが同じ答えを出すこと（GPU へ持っていく木の担保）──
    //   ★木は枝刈りするだけで、当てるのは今までと同じ OBB の判定。だから答えは変わらないはず。
    //     変わるとしたら ①active の扱い ②葉を回る順（同点の勝者）③横切りの積の順 の 3 か所。
    //     ここが崩れたら、GPU 版と CPU 版を突き合わせる土台が無くなる。
    {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.scattering[b] = 0.5f; wall.transmission[b] = 0.05f; }
        MaterialTable mats; const int matId = mats.add(wall);
        Surfaces box = closedBox(3.5f, 3.0f, matId);
        // 中に箱を撒く（木が枝分かれする数にする）
        afti::Xorshift rnd;
        for (int k = 0; k < 24; ++k) {
            const Vec3 c(rnd.next() * 2.5f, 1.0f + rnd.next() * 0.8f, rnd.next() * 2.5f);
            box.add(Obb::axisAligned(c, Vec3(0.2f, 0.4f, 0.2f)), matId, false);
        }
        box.at(7).active = false;        // 消した箱が木に混ざらないか
        const int trials = 400;
        int hitDiff = 0, idxDiff = 0, crossDiff = 0; double tWorst = 0.0, tauWorst = 0.0;
        for (int k = 0; k < trials; ++k) {
            const Vec3 o(rnd.next() * 3.0f, 1.0f + rnd.next(), rnd.next() * 3.0f);
            Vec3 d(rnd.next(), rnd.next() * 0.5f, rnd.next());
            if (length(d) < 1e-3f) d = Vec3(0, 0, 1);
            d = normalized(d);
            box.clearBvh();
            const SurfaceHit a = box.nearest(o, d, 100.0f, -1);
            float ta[kNumBands]; int ca = 0;
            box.transmittance(o, o + d * 6.0f, -1, mats, ta, &ca);
            box.rebuildBvh();
            const SurfaceHit b2 = box.nearest(o, d, 100.0f, -1);
            float tb[kNumBands]; int cb = 0;
            box.transmittance(o, o + d * 6.0f, -1, mats, tb, &cb);
            if (a.hit != b2.hit) ++hitDiff;
            if (a.hit && b2.hit) {
                if (a.index != b2.index) ++idxDiff;
                tWorst = std::max(tWorst, static_cast<double>(std::fabs(a.t - b2.t)));
            }
            if (ca != cb) ++crossDiff;
            for (int b = 0; b < kNumBands; ++b) tauWorst = std::max(tauWorst, static_cast<double>(std::fabs(ta[b] - tb[b])));
        }
        box.rebuildBvh();
        std::snprintf(buf, sizeof(buf), "(%d 本: 当たり有無の差 %d、当たった箱の差 %d、t の差 %.2e、横切り数の差 %d、τ の差 %.2e／ノード %d・葉に入った箱 %d)",
                      trials, hitDiff, idxDiff, tWorst, crossDiff, tauWorst, box.bvh().nodeCount(), box.bvh().itemCount());
        check("[レイ] BVH と総当たりが同じ答え（当たった箱・t・横切りの数・τ）",
              hitDiff == 0 && idxDiff == 0 && tWorst == 0.0 && crossDiff == 0 && tauWorst == 0.0, buf);
    }

    // ── レイ 1 本が独立していること（GPU へ移すための担保。段 1）──
    //   ★GPU は 1 スレッド = 1 本で走らせ、あとで足し合わせる。だから
    //     ①1 本の結果が他の本に依らない ②足す順を変えても答えが変わらない
    //     の 2 つが要る。ここが崩れたら GPU 版と CPU 版が一致しなくなる。
    {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.scattering[b] = 0.5f; wall.transmission[b] = 0.01f; }
        MaterialTable mats; const int matId = mats.add(wall);
        Surfaces box = closedBox(3.5f, 3.0f, matId);
        const Vec3 S(1.5f, 1.6f, -1.0f), L(-1.0f, 1.2f, 1.5f);
        TraceParams prm{256, 20, 0.03f, 7u, 1, 0};
        const float e0 = 1.0f / 256.0f;
        // ★レイが見るのは平らな場面だけ（GPU と同じ入力）。ここで 1 回組む。
        TraceScene sc; box.rebuildBvh(); buildTraceScene(box, mats, sc);
        // 順に足す（run と同じ順）
        double fwd[kNumBands] = {}, rev[kNumBands] = {};
        double fwdLate[kNumBands] = {}, revLate[kNumBands] = {};
        int hitsFwd = 0, hitsRev = 0;
        for (int i = 0; i < 256; ++i) {
            RayPartial p; traceRay(sc, S, L, prm, i, e0, p);
            hitsFwd += p.hits;
            for (int b = 0; b < kNumBands; ++b) { fwd[b] += p.early6[b]; fwdLate[b] += p.late6[b]; }
        }
        // **逆順**に足す（同じ本を同じ番号で引くが、足す順だけ変える）
        for (int i = 255; i >= 0; --i) {
            RayPartial p; traceRay(sc, S, L, prm, i, e0, p);
            hitsRev += p.hits;
            for (int b = 0; b < kNumBands; ++b) { rev[b] += p.early6[b]; revLate[b] += p.late6[b]; }
        }
        double worst = 0.0;
        for (int b = 0; b < kNumBands; ++b) {
            if (fwd[b] > 0.0) worst = std::max(worst, std::fabs(afti::dB(rev[b] / fwd[b])));
            if (fwdLate[b] > 0.0) worst = std::max(worst, std::fabs(afti::dB(revLate[b] / fwdLate[b])));
        }
        std::snprintf(buf, sizeof(buf), "(当たり 前から %d / 後ろから %d、初期と後期のずれ 最大 %.2e dB)", hitsFwd, hitsRev, worst);
        check("[レイ] 1 本は独立していて、足す順を変えても答えが変わらない（GPU へ移す担保）",
              hitsFwd == hitsRev && worst < 1e-6, buf);
        // 1 本の結果が「その本だけ」で決まること: 同じ番号を単独で引いても同じ
        RayPartial a, b2;
        traceRay(sc, S, L, prm, 77, e0, a);
        traceRay(sc, S, L, prm, 77, e0, b2);
        bool same = (a.hits == b2.hits && a.neeVisible == b2.neeVisible && a.firstReflectSec == b2.firstReflectSec);
        for (int b = 0; b < kNumBands && same; ++b) same = (a.early6[b] == b2.early6[b] && a.late6[b] == b2.late6[b]);
        std::snprintf(buf, sizeof(buf), "(77 番の本: 当たり %d、最初の反射 %.4f ms)", a.hits, a.firstReflectSec * 1000.0f);
        check("[レイ] 同じ番号の本は何度引いてもビット一致（種は本の番号だけで決まる）", same, buf);
    }
}

// ================================ [速度] response（段 3）
void testResponse() {
    std::printf("\n[速度] 量は即時・形は 40 ms・0 からの立ち上がり\n");
    Follower6 f; f.reset();
    const float dt = 512.0f / 48000.0f;
    const float x0[6] = {1, 1, 1, 1, 1, 1};
    float out[6];
    f.update(x0, dt, 0.0f, 0.04f, out);
    check("[速度] 最初のフレームは目標そのまま（0 から色を壊さず立ち上がる）", std::fabs(out[0] - 1.0f) < 1e-6f && std::fabs(out[5] - 1.0f) < 1e-6f);
    const float x1[6] = {4, 4, 4, 4, 4, 4};                  // 量が 4 倍、形は同じ
    f.update(x1, dt, 0.0f, 0.04f, out);
    check("[速度] 量（levelSec=0）は 1 フレームで追いつく", std::fabs(out[0] - 4.0f) < 1e-5f);
    const float x2[6] = {4, 4, 4, 4, 0, 0};                  // 量 16（前は 24）、形が変わる（高域が消える）
    f.update(x2, dt, 0.0f, 0.04f, out);
    // 期待: 量は即時に 16。形の 4k は 1/6 から 0 へ一次遅れ → 16 × (1/6) × exp(−dt/τ) = 2.042
    const float expect4k = 16.0f * (1.0f / 6.0f) * std::exp(-dt / 0.04f);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(1 フレーム後の 4k: %.3f、一次遅れの式 %.3f、目標 0。量の和 %.2f)", out[5], expect4k, out[0] + out[1] + out[2] + out[3] + out[4] + out[5]);
    check("[速度] 形（colourSec=40 ms）は 1 フレーム（10.7 ms）では追いつかず、一次遅れの式どおり", std::fabs(out[5] - expect4k) < 1e-3f, buf);
    check("[速度] 形が動いている間も量の和は保たれる", std::fabs((out[0] + out[1] + out[2] + out[3] + out[4] + out[5]) - 16.0f) < 1e-3f);
    for (int k = 0; k < 40; ++k) f.update(x2, dt, 0.0f, 0.04f, out);     // 430 ms 後
    check("[速度] 40 ms の一次遅れは 430 ms でほぼ追いつく（4k が 1e-3 未満）", out[5] < 1e-3f);
    const float z[6] = {0, 0, 0, 0, 0, 0};
    f.update(z, dt, 0.0f, 0.04f, out);
    check("[速度] 目標が 0 でも形は前の値を保つ（0 除算しない）", out[0] == 0.0f && f.shape[0] > 0.0f);
    check("[速度] τ=0 の係数は 1、τ=dt の係数は 1−1/e", followCoef(dt, 0.0f) == 1.0f && std::fabs(followCoef(dt, dt) - 0.6321f) < 1e-3f);
}

// ================================ [配分] distribute（段 3、最小形）
void testDistribute() {
    std::printf("\n[配分] 受け取った物を 1 回ずつ出す・重みは出口だけ・歩行の連続性\n");
    MaterialTable mats;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.0f; wall.scattering[b] = 0.5f; }
    const int matId = mats.add(wall);
    const float half = 3.5f, h = 3.0f;
    Surfaces box = closedBox(half, h, matId);
    Listener L; L.pos = Vec3(-1.0f, 1.2f, 1.5f); L.forward = Vec3(0, 0, 1); L.prevPos = L.pos;
    const Vec3 S(1.5f, 1.6f, -1.0f);
    TraceParams prm; prm.rays = 256; prm.maxBounces = 40; prm.mixingSec = 0.03f; prm.seed = 11u;
    EnergyTrace tr;
    WorldWeights W; Response rs;
    const float dt = 512.0f / 48000.0f;
    EmitterMixer mixer; mixer.reset();
    Mix mix;
    auto step = [&](int room) {
        const TraceResult T = tr.run(box, mats, S, L.pos, prm);
        DistributeInput in; in.trace = &T; in.sourcePos = S; in.listener = &L; in.listenerRoom = room;
        in.weights = &W; in.response = &rs; in.dt = dt;
        mixer.run(in, mix);
    };
    step(0);
    char buf[200];
    check("[配分] 帳簿: 受け取った 4 つを 1 回ずつ出した（conserves）", mix.conserves(0.01f));
    check("[配分] タップ 3 本（直接・透過・初期）と送り 1 本", mix.tapCount == 3 && mix.sendCount == 1 && mix.sends[0].room == 0);
    check("[配分] 直接のタップに方向があり、初期のタップは方向なし（広がり 1）",
          length(mix.taps[0].dirLocal) > 0.99f && mix.taps[2].kind == TapKind::Early && length(mix.taps[2].dirLocal) == 0.0f && mix.taps[2].spread == 1.0f);
    check("[配分] 尾の開始は最初の反射の到達（直接より後）", mix.onsetSec > mix.taps[0].delaySec);

    // 世界の重み: 出口だけに効き、帳簿は生のまま。
    W.w[kLate] = 2.0f;
    step(0);
    const float sendE = mix.sends[0].e6[2], rawLate = mix.component6[kLate][2];
    std::snprintf(buf, sizeof(buf), "(後期の重み 2: 送り %.3e、帳簿 %.3e、比 %.2f)", sendE, rawLate, sendE / rawLate);
    check("[配分] 重み 2 は送りを 2 倍にし、帳簿は変えない（conserves が通る）", std::fabs(sendE / rawLate - 2.0f) < 1e-3f && mix.conserves(0.01f), buf);
    W.w[kLate] = 1.0f;

    // 部屋が無ければ送りは作らないが、帳簿は受け取った通り。
    step(-1);
    check("[配分] 部屋が −1 なら送りを作らず、帳簿は保存", mix.sendCount == 0 && mix.conserves(0.01f));

    // 歩行の連続性: 静止で 0、1.4 m/s で歩いても小さい。成分ごとの出口のレベルで測る。
    auto levelDb = [&](int comp) {
        double e = 0.0;
        if (comp == kLate) { for (int b = 0; b < kNumBands; ++b) e += mix.sends[0].e6[b]; }
        else { for (int i = 0; i < mix.tapCount; ++i) if (static_cast<int>(mix.taps[i].kind) == (comp == kDirect ? 0 : comp == kEarly ? 1 : 3)) for (int b = 0; b < kNumBands; ++b) e += mix.taps[i].e6[b]; }
        return afti::dB(e);
    };
    mixer.reset();
    for (int k = 0; k < 10; ++k) step(0);                       // 立ち上がりを流す
    double still[3] = {0, 0, 0}, prev[3];
    for (int c = 0; c < 3; ++c) prev[c] = levelDb(c == 0 ? kDirect : c == 1 ? kEarly : kLate);
    for (int k = 0; k < 30; ++k) {
        step(0);
        for (int c = 0; c < 3; ++c) { const double v = levelDb(c == 0 ? kDirect : c == 1 ? kEarly : kLate); still[c] = std::max(still[c], std::fabs(v - prev[c])); prev[c] = v; }
    }
    std::snprintf(buf, sizeof(buf), "(静止 30 フレーム: 直接 %.4f / 初期 %.4f / 後期 %.4f dB)", still[0], still[1], still[2]);
    check("[配分] 静止なら 3 成分とも揺れない（0.001 dB 未満）", still[0] < 1e-3 && still[1] < 1e-3 && still[2] < 1e-3, buf);
    double walk[3] = {0, 0, 0};
    const float stepM = 1.4f * dt;                                // 1.4 m/s
    for (int k = 0; k < 60; ++k) {
        L.pos = L.pos + Vec3(0, 0, -stepM);                       // 音源へ向かって歩く（z 1.5 → 0.6）
        step(0);
        for (int c = 0; c < 3; ++c) { const double v = levelDb(c == 0 ? kDirect : c == 1 ? kEarly : kLate); walk[c] = std::max(walk[c], std::fabs(v - prev[c])); prev[c] = v; }
    }
    std::snprintf(buf, sizeof(buf), "(1.4 m/s で 60 フレーム: 直接 %.3f / 初期 %.3f / 後期 %.3f dB per frame)", walk[0], walk[1], walk[2]);
    check("[配分] 歩いても隣り合うフレームの段差が 0.5 dB 未満（初期・後期は種の固定＋50 ms）", walk[1] < 0.5 && walk[2] < 0.5, buf);
}

// ================================ [世界] world（段 4）
World* makeWorldBox(float half, float h, float alpha) {
    World* w = new World();
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = alpha; wall.transmission[b] = 0.0f; wall.scattering[b] = 0.5f; }
    const int mat = w->rules.materials.add(wall);
    const float t = 0.2f;
    auto add = [&](Vec3 c, Vec3 he) { w->addBox(Obb::axisAligned(c, he), mat, false); };
    add(Vec3(0, -t, 0), Vec3(half + t, t, half + t));
    add(Vec3(0, h + t, 0), Vec3(half + t, t, half + t));
    add(Vec3(-half - t, h * 0.5f, 0), Vec3(t, h * 0.5f, half + t));
    add(Vec3(half + t, h * 0.5f, 0), Vec3(t, h * 0.5f, half + t));
    add(Vec3(0, h * 0.5f, -half - t), Vec3(half + t, h * 0.5f, t));
    add(Vec3(0, h * 0.5f, half + t), Vec3(half + t, h * 0.5f, t));
    return w;
}
void testWorld() {
    std::printf("\n[世界] 1 フレームの流れ・部屋・帳簿・FDN の結び\n");
    World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
    w->raysPerEmitter = 256;
    w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    const int e = w->addEmitter(Vec3(1.5f, 1.6f, -1.0f), 0.0f);
    w->build();
    char buf[200];
    std::snprintf(buf, sizeof(buf), "(部屋 %d、リスナーの部屋 %d、体積 %.0f m3、RT60 500Hz %.2f s)", w->roomCount(), w->roomAt(w->listener().pos), w->roomCount() ? w->probe(0).volume : 0.0f, w->roomCount() ? w->probe(0).rt60[2] : 0.0f);
    check("[世界] 閉じた箱は部屋 1 個、リスナーも音源もその中", w->roomCount() == 1 && w->roomAt(w->listener().pos) == 0 && w->roomAt(Vec3(1.5f, 1.6f, -1.0f)) == 0, buf);
    check("[世界] 外の点は −1", w->roomAt(Vec3(50, 50, 50)) == -1);
    af::dsp::FdnRoomMix fdn(48000, 512, 0.6f);
    w->bindFdn(&fdn);
    check("[世界] FDN を結ぶと部屋が器に足される", fdn.roomCount() == 1 && w->fdnRoomOfProbe()[0] == 0);
    const float dt = 512.0f / 48000.0f;
    for (int k = 0; k < 20; ++k) w->update(dt);
    const Mix* m = w->mix(e);
    check("[世界] 更新後の配分は帳簿が保存されている", m && m->conserves(0.01f));
    check("[世界] 音源の部屋が入っている", w->emitter(e)->room == 0);
    const Mix m1 = *m; w->update(dt); const Mix m2 = *w->mix(e);
    bool same = true; for (int b = 0; b < kNumBands; ++b) if (m1.energy6[b] != m2.energy6[b]) same = false;
    check("[世界] 静止していれば帳簿がフレーム間で一致（種の固定）", same);
    const int e2 = w->addEmitter(Vec3(-2.0f, 1.0f, -2.0f), 0.3f);
    w->update(dt);
    check("[世界] 2 本目の音源も配分される", w->mix(e2) && w->mix(e2)->conserves(0.01f) && w->mix(e2)->tapCount >= 3);
    w->removeEmitter(e2);
    check("[世界] 消した音源の番号は使い回される", w->addEmitter(Vec3(0, 1, 0), 0.0f) == e2);
    delete w;
}

// ================================ [橋] mix_to_voice（段 4）── 出力のエネルギーが配分と一致する
void testBridge() {
    std::printf("\n[橋] 配分を DSP へ写して鳴らし、出力のエネルギーが帳簿と一致するか\n");
    const int fs = 48000, block = 512;
    World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
    w->raysPerEmitter = 512;
    w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    const int e = w->addEmitter(Vec3(1.5f, 1.6f, -1.0f), 0.0f);
    w->build();
    af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
    w->bindFdn(&fdn);
    const float dt = static_cast<float>(block) / fs;
    for (int k = 0; k < 30; ++k) w->update(dt);
    const Mix mix = *w->mix(e);

    // 鳴らす: 直接だけ／タップ全部／全部（FDN 込み）の 3 通り。インパルス 1 発、2.5 秒。
    auto renderEnergy = [&](bool tapsOn, bool lateOn, double* outTotal, double* tapPart, double* fdnPart) {
        WorldWeights W;
        W.w[kEarly] = tapsOn ? 1.0f : 0.0f; W.w[kTransmit] = tapsOn ? 1.0f : 0.0f; W.w[kLate] = lateOn ? 1.0f : 0.0f;
        w->rules.weights = W;
        af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
        af::dsp::VoiceRenderer v(vc);
        v.setOutputGain(1.0f); v.setHrtfEnabled(false); v.setTailLevel(1.0f);
        // ★器を作るのも繋ぐのも update より**先**。ここを逆にすると、2 回目以降の冒頭 30 フレームが
        //   前回の呼び出しで消えた fdn2（スタック上）を指したまま回り、World::updateFdn が解放済みの
        //   FdnTail に setRt60 する ＝ 解放後使用。ヒープの並び次第で落ちる（実際に落ちた）。
        af::dsp::FdnRoomMix fdn2(fs, block, 0.6f);
        w->bindFdn(&fdn2);
        for (int k = 0; k < 31; ++k) w->update(dt);       // 重みを流す＋listenerWeight を置く（置かないと既定 0 で無音）
        v.setFdnMix(&fdn2);
        w->applyToVoice(e, v, fs);
        // ★インパルスは 300 ms に置く。タップの差し替え（30 ms）と送りの傾斜は 0 から立ち上がるので、
        //   0 秒に置くと頭に当たってほぼ 0 になる（旧手順 5 の検査で踏んだのと同じ）。
        const int impulseAt = (fs * 3 / 10 / block) * block;
        const int total = impulseAt + fs * 5 / 2;
        std::vector<float> in(block, 0.0f), l(block), r(block), fl(block), fr(block);
        double eTap = 0.0, eFdn = 0.0;
        for (int p = 0; p < total; p += block) {
            std::fill(in.begin(), in.end(), 0.0f);
            if (p == impulseAt) in[0] = 1.0f;
            v.render(in.data(), block, l.data(), r.data(), nullptr);
            std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
            fdn2.render(block, fl.data(), fr.data());
            for (int i = 0; i < block; ++i) { eTap += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]; eFdn += static_cast<double>(fl[i]) * fl[i] + static_cast<double>(fr[i]) * fr[i]; }
        }
        *outTotal = eTap + eFdn; *tapPart = eTap; *fdnPart = eFdn;
        w->bindFdn(&fdn);       // ★fdn2 は今から消える。世界に生きている器を持たせてから返る
    };
    double tot, tap, fd;
    // 参考: 器に直接インパルスを入れたときの出力エネルギー（送りの経路を通さない）。1.0 が校正どおり。
    {
        af::dsp::FdnRoomMix fd0(fs, block, 0.6f);
        const int r0 = fd0.addRoom(fdnLineScale(w->probe(0)), w->probe(0).rt60, false);
        const float one[6] = {1, 1, 1, 1, 1, 1};
        fd0.setListenerWeight(r0, one);
        std::vector<float> in(block, 0.0f), l(block), r(block);
        double e = 0.0;
        const int at = (fs * 3 / 10 / block) * block, tot0 = at + fs * 5 / 2;
        for (int p = 0; p < tot0; p += block) {
            std::fill(in.begin(), in.end(), 0.0f);
            if (p == at) in[0] = 1.0f;
            fd0.add(r0, in.data(), block, 1.0f);
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            fd0.render(block, l.data(), r.data());
            for (int i = 0; i < block; ++i) e += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        }
        std::printf("      参考: 器に直接インパルス 1 → 両耳の出力エネルギー %.3f（%+.2f dB。RT60 500Hz %.2f s、lineScale %.2f）\n",
                    e, afti::dB(e), w->probe(0).rt60[2], fdnLineScale(w->probe(0)));
    }
    // ★期待値は**帯域幅で重み付けした平均**。白色のインパルスのエネルギーは帯域幅なりに散る（4k 帯が 88%）。
    //   DSP は帯域ごとに g_b² を掛けるので、総出力 = Σ_b (E_b K) · bw_b / Σbw。単純平均で書いて FDN が −2.1 dB に見えた
    //   （直接は帯域が平らなので単純平均でも合ってしまい、気づかなかった）。world_rules::bandWidthHz の注記そのもの。
    auto bwMean = [&](int comp) {
        double num = 0.0, den = 0.0;
        for (int b = 0; b < kNumBands; ++b) { const double bw = bandWidthHz(b, static_cast<float>(fs)); num += mix.component6[comp][b] * kEnergyToAmp * bw; den += bw; }
        return num / den;
    };
    // 直接だけ
    renderEnergy(false, false, &tot, &tap, &fd);
    const double expDirectMean = bwMean(kDirect);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "(直接だけ: 出力 %.3e 対 帳簿 %.3e、%+.2f dB。FDN %.1e)", tap, expDirectMean, afti::dB(tap / expDirectMean), fd);
    check("[橋] 直接だけを鳴らした出力エネルギーが帳簿 × K と ±0.5 dB", std::fabs(afti::dB(tap / expDirectMean)) < 0.5 && fd < expDirectMean * 1e-3, buf);
    // 全部
    renderEnergy(true, true, &tot, &tap, &fd);
    const double expLate = bwMean(kLate);
    // ★タップの期待は**干渉を含めて**出す（段 7 で必要になった）。隣り合う 2 面のコーナーの虚像は A→B と B→A の
    //   2 経路が同じ長さで同時に届き、DSP は振幅で足す（その対は +3 dB。物理でもそう）。帳簿はエネルギー（非干渉の和）
    //   なので、同じ遅延サンプルのタップは振幅で足してから二乗し、帯域幅で重み付けして総量にする。
    //   段 6 までは初期が 1 本だったので単純和で合っていた（+1.51 dB ずれて気づいた）。
    auto expTapsCoherent = [&]() {
        std::map<int, std::array<double, 6>> amp;
        float dSec = 0.0f;
        for (int i = 0; i < mix.tapCount; ++i) if (mix.taps[i].kind == TapKind::Direct) { dSec = mix.taps[i].delaySec; break; }
        for (int i = 0; i < mix.tapCount; ++i) {
            const int d = std::max(0, static_cast<int>(std::lround((mix.taps[i].delaySec - dSec) * fs)));
            auto& a = amp[d];
            for (int b = 0; b < kNumBands; ++b) a[static_cast<std::size_t>(b)] += std::sqrt(std::max(0.0f, mix.taps[i].e6[b]) * kEnergyToAmp);
        }
        double e = 0.0, den = 0.0;
        for (int b = 0; b < kNumBands; ++b) den += bandWidthHz(b, static_cast<float>(fs));
        for (const auto& kv : amp) for (int b = 0; b < kNumBands; ++b) e += kv.second[static_cast<std::size_t>(b)] * kv.second[static_cast<std::size_t>(b)] * bandWidthHz(b, static_cast<float>(fs)) / den;
        return e;
    };
    const double expTaps = expTapsCoherent();
    std::snprintf(buf, sizeof(buf), "(全部: タップ %.3e 対 %.3e（%+.2f dB）、FDN %.3e 対 %.3e（%+.2f dB）)", tap, expTaps, afti::dB(tap / expTaps), fd, expLate, afti::dB(fd / expLate));
    check("[橋] タップ（直接＋初期＋透過）の出力が帳簿と ±0.5 dB", std::fabs(afti::dB(tap / expTaps)) < 0.5, buf);
    check("[橋] FDN（後期）の出力が帳簿と ±1.5 dB（校正の残り +1.3 dB の中）", std::fabs(afti::dB(fd / expLate)) < 1.5, buf);
    const double ratioDb = afti::dB(fd / tap);
    std::snprintf(buf, sizeof(buf), "(後期/タップ %+.1f dB。7×7×3 m、α 0.2、距離 3.6 m)", ratioDb);
    check("[橋] 後期の量は 0 でも支配でもない（−20〜+10 dB の間）", ratioDb > -20.0 && ratioDb < 10.0, buf);
    delete w;
}

// ================================ [開口] aperture（段 5）
//   蝶番 (gapL, h/2, 0) を軸に +Z へ θ 開いた板の Obb（Test_SwingDoor と同じ置き方）。
// ★clearance: 枠との隙間(m)。**自由端と上下だけ**に付け、蝶番側は密着させる（実際の扉と同じ）。
//   Unity の SwingDoor は既定 0.004 m を持っている。ここを 0 にすると板が戸口を隙間なく塞ぐので、
//   「隙間から高域だけが漏れる」という作品の前提が形として存在しなくなる。
//   既定は 0（今までの検査の値を 1 ビットも変えないため）。漏れを測るときだけ渡す。
Obb doorLeaf(float thetaDeg, float gapL = -0.5f, float w = 1.0f, float h = 3.0f, float thick = 0.06f,
             float clearance = 0.0f) {
    const float th = thetaDeg * 3.14159265f / 180.0f;
    const float c = std::max(0.0f, clearance);
    w = std::max(0.01f, w - c);
    h = std::max(0.01f, h - 2.0f * c);
    const Vec3 right(std::cos(th), 0.0f, std::sin(th));
    const Vec3 hinge(gapL, h * 0.5f + c, 0.0f);
    Obb b; b.center = hinge + right * (w * 0.5f); b.halfExtents = Vec3(w * 0.5f, h * 0.5f, thick * 0.5f);
    b.axisX = right; b.axisY = Vec3(0, 1, 0); b.axisZ = cross(b.axisX, b.axisY);
    return b;
}
void testAperture() {
    std::printf("\n[開口] 見通しの割合（解析）・板の覆い・扉の開きに対する連続性\n");
    MaterialTable mats;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const int matId = mats.add(wall);
    const int leafId = mats.add(AcousticMaterial::woodDoor());
    Surfaces s;
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) s.add(sb.obb, matId, false);
    const Vec3 L(0, 1.6f, -3.0f), S(0, 1.6f, 3.0f);
    char buf[200];

    // 1) 点音源: 戸口の正面は見通し 1、仕切りの裏は 0（壁の τ）
    Visibility v0 = discVisibility(s, mats, L, S, 0.0f);
    Visibility v1 = discVisibility(s, mats, L, Vec3(3.0f, 1.6f, 3.0f), 0.0f);
    std::snprintf(buf, sizeof(buf), "(正面 %.2f / 仕切りの裏 %.2f、遮る物 %d、τ %.4f)", v0.visible, v1.visible, v1.shadowers, v1.shadowTau6[2]);
    check("[開口] 点音源: 戸口の正面は 1、仕切りの裏は 0 で壁の τ", v0.visible == 1.0f && v0.shadowers == 0 && v1.visible == 0.0f && v1.shadowers == 1 && std::fabs(v1.shadowTau6[2] - 0.001f) < 1e-5f, buf);

    // 2) 幅 0.3 m の音源を x=0 → 2.5 m へ 2 cm 刻みで動かす: 単調非増加、段差 0.05 未満、両端は 1 と 0
    {
        float prev = 2.0f, worst = 0.0f, first = -1.0f, last = -1.0f; bool mono = true;
        for (float x = 0.0f; x <= 2.5f; x += 0.02f) {
            const float vis = discVisibility(s, mats, L, Vec3(x, 1.6f, 3.0f), 0.3f).visible;
            if (first < 0.0f) first = vis;
            if (prev < 1.5f) { if (vis > prev + 1e-4f) mono = false; worst = std::max(worst, prev - vis); }
            prev = vis; last = vis;
        }
        // ★段差の上限は物理から: ガウス円盤（σ = r/2）の最大の傾きは 1/(σ√2π) [1/m]。r=0.3 なら 2 cm で 0.053。
        //   これより大きい段差が出たら不連続、これ以下なら「そういう形」。最初 0.05 と勘で書いて 0.058 で落ちた。
        const float slopeMax = 1.0f / (0.15f * 2.5066f), allowed = 1.3f * 0.02f * slopeMax;
        std::snprintf(buf, sizeof(buf), "(x=0 で %.3f、x=2.5 で %.3f、2 cm あたりの最大段差 %.4f。ガウスの傾きの上限 %.4f)", first, last, worst, allowed);
        check("[開口] 幅を持つ音源が仕切りに隠れていくとき、見通しは単調に減り、段差はガウス円盤の傾きの範囲", mono && first > 0.999f && last < 1e-3f && worst < allowed, buf);
    }

    // 3) 扉: 0° → 90° を 1° 刻み。閉で 0、単調増加、1° で既に正、90° で 0.9 以上、段差 0.05 未満
    {
        const int leaf = s.add(doorLeaf(0.0f), leafId, true);
        // ★音源が扉の真後ろ（x=0）だと、板は +Z 側へ振れて視線に居座り、**見通しが開くのは 57° 付近**。
        //   板の自由端の投影が円盤（r=0.2）を横切るのは約 5° の間で、その間の傾きはガウスの上限（1/(σ√2π)、σ=0.1）。
        //   「開き始めた瞬間の立ち上がり」（Ⅸ）は見通しでは出ない ── 隙間を回る回折（段 6）と板の透過の仕事。
        //   この検査は「見通しの割合そのもの」が連続で単調であることを見る。
        float prev = -1.0f, worst = 0.0f, atClosed = -1.0f, atOpen = -1.0f; bool mono = true;
        int firstPositive = -1, at05 = -1, at95 = -1;
        for (int deg = 0; deg <= 90; ++deg) {
            s.at(leaf).obb = doorLeaf(static_cast<float>(deg));
            const float vis = discVisibility(s, mats, L, S, 0.2f).visible;
            if (deg == 0) atClosed = vis; if (deg == 90) atOpen = vis;
            if (vis > 0.0f && firstPositive < 0) firstPositive = deg;
            if (vis > 0.05f && at05 < 0) at05 = deg;
            if (vis > 0.95f && at95 < 0) at95 = deg;
            if (prev >= 0.0f) { if (vis < prev - 1e-4f) mono = false; worst = std::max(worst, vis - prev); }
            prev = vis;
        }
        std::snprintf(buf, sizeof(buf), "(閉 %.3f、初めて正 %d°、0.05 超 %d°、0.95 超 %d°、90° %.3f、1° あたりの最大段差 %.4f)", atClosed, firstPositive, at05, at95, atOpen, worst);
        check("[開口] 扉: 閉で 0、90° で 0.9 以上、見通しが開くのは 90° より手前", atClosed == 0.0f && atOpen > 0.9f && firstPositive > 0 && firstPositive < 90, buf);
        check("[開口] 扉: 開くほど単調に増え、0.05 → 0.95 に 3° 以上かけて連続に開く（一段の跳びではない）", mono && at95 - at05 >= 3 && worst < 0.2f, buf);
        const Visibility vc = discVisibility(s, mats, L, S, 0.2f);   // 90° のまま
        check("[開口] 90° の扉の残りの影は木の扉の τ を持つ（遮る物 1）", vc.shadowers <= 1);
        s.at(leaf).obb = doorLeaf(30.0f);
        const Visibility v30 = discVisibility(s, mats, L, S, 0.2f);
        std::snprintf(buf, sizeof(buf), "(30°: 見通し %.3f、遮る物 %d、τ(1k) %.4f 対 木の扉 %.4f)", v30.visible, v30.shadowers, v30.shadowTau6[3], AcousticMaterial::woodDoor().transmission[3]);
        check("[開口] 30° の扉が遮る分の τ は木の扉の物", v30.shadowers == 1 && std::fabs(v30.shadowTau6[3] - AcousticMaterial::woodDoor().transmission[3]) < 1e-4f, buf);
    }

    // 4) 板の覆い（正射影）: 閉で 1、90° で 厚み/幅 ≒ 0.06、単調、遠くの板は 0
    {
        rooms::Builder builder;
        const rooms::Result& res = builder.build(twoRoomsWithDoor(wall));
        check("[開口] 部屋グラフの戸口が 1 つ", res.apertures.size() == 1);
        if (res.apertures.size() == 1) {
            const rooms::Aperture& ap = res.apertures[0];
            const float c0 = openingCoverage(ap, doorLeaf(0.0f)), c45 = openingCoverage(ap, doorLeaf(45.0f)), c90 = openingCoverage(ap, doorLeaf(90.0f));
            bool mono = true; float prev = 2.0f;
            for (int deg = 0; deg <= 90; deg += 5) { const float c = openingCoverage(ap, doorLeaf(static_cast<float>(deg))); if (c > prev + 1e-4f) mono = false; prev = c; }
            Obb far = doorLeaf(0.0f); far.center = far.center + Vec3(0, 0, 5.0f);
            std::snprintf(buf, sizeof(buf), "(閉 %.3f、45° %.3f、90° %.3f、5 m 先の板 %.3f。戸口 %.2f×%.2f m)", c0, c45, c90, openingCoverage(ap, far), 2 * ap.halfU, 2 * ap.halfV);
            check("[開口] 板の覆い: 閉で 0.9 以上、45° で cos45 の近く、90° で 0.15 未満、開くほど単調に減る", c0 > 0.9f && std::fabs(c45 - 0.707f) < 0.15f && c90 < 0.15f && mono, buf);
            check("[開口] 面から離れた板は覆いに数えない", openingCoverage(ap, far) == 0.0f);
        }
    }

    // 4.5) 隙間の通り（段 9。回折の量を決める）: 幅 a と波長の比で決まる。0 で閉じ、a ≥ r で全部通り、高域が先に開く。
    {
        const float d1 = 3.0f, d2 = 3.0f;
        const GapOpen g0 = gapOpen(0.0f, d1, d2), gTiny = gapOpen(0.06f, d1, d2), gMid = gapOpen(0.3f, d1, d2), gWide = gapOpen(3.0f, d1, d2);
        std::snprintf(buf, sizeof(buf), "(3+3 m のゾーン半径 125Hz %.2f m / 4k %.2f m。隙間 6 cm: 125Hz %.3f / 4k %.3f、30 cm: %.3f / %.3f)",
                      g0.radius6[0], g0.radius6[5], gTiny.open6[0], gTiny.open6[5], gMid.open6[0], gMid.open6[5]);
        check("[開口] 隙間: 閉じていれば 0、十分広ければ 1（全帯域）", g0.open6[0] == 0.0f && g0.open6[5] == 0.0f && gWide.open6[0] > 0.999f && gWide.open6[5] > 0.999f, buf);
        check("[開口] 隙間: 狭い隙間は高域が先に通る（同じ幅で 4kHz > 125Hz）", gTiny.open6[5] > 3.0f * gTiny.open6[0] && gMid.open6[5] > gMid.open6[0], buf);
        bool mono = true; float prev = -1.0f;
        for (float a = 0.0f; a <= 2.0f; a += 0.01f) { const float v = gapOpen(a, d1, d2).open6[2]; if (v < prev - 1e-6f) mono = false; prev = v; }
        check("[開口] 隙間: 幅に対して単調（1 cm 刻みで戻らない）", mono);
    }

    // 5) 世界を通す: 閉めると直接 ≈ 0 で透過が残り、開けると直接が戻る。閉めた部屋の RT60 が伸びる。
    {
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        const int leaf = w.addBox(doorLeaf(0.0f), lm, true);
        w.raysPerEmitter = 128;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        const float dt = 512.0f / 48000.0f;
        for (int k = 0; k < 5; ++k) w.update(dt);
        const int lroom = w.roomAt(L);
        const float rtClosed = w.probe(lroom).rt60[2];
        double dClosed = 0, tClosed = 0;
        for (int b = 0; b < kNumBands; ++b) { dClosed += w.mix(e)->component6[kDirect][b]; tClosed += w.mix(e)->component6[kTransmit][b]; }
        w.setBoxTransform(leaf, doorLeaf(90.0f));
        for (int k = 0; k < 5; ++k) w.update(dt);
        const float rtOpen = w.probe(lroom).rt60[2];
        double dOpen = 0, tOpen = 0;
        for (int b = 0; b < kNumBands; ++b) { dOpen += w.mix(e)->component6[kDirect][b]; tOpen += w.mix(e)->component6[kTransmit][b]; }
        std::snprintf(buf, sizeof(buf), "(閉: 直接 %.2e 透過 %.2e RT60 %.3f s → 開: 直接 %.2e 透過 %.2e RT60 %.3f s、素通し %.2f→%.2f)",
                      dClosed, tClosed, rtClosed, dOpen, tOpen, rtOpen, 0.0f, w.apertureOpenFrac(0));
        check("[開口] 世界: 閉めると直接 0 で透過が残り、開けると直接が戻って透過が減る", dClosed == 0.0 && tClosed > 0.0 && dOpen > 0.0 && tOpen < tClosed, buf);
        check("[開口] 世界: 閉めた扉でリスナーの部屋の RT60 が伸びる（同じ a が RT60 と直接音の両方に効く）", rtClosed > rtOpen, buf);
        check("[開口] 世界: 帳簿は扉の開閉でも保存", w.mix(e)->conserves(0.01f));
    }
}

// ================================ [回折] diffraction（段 6）
void testDiffraction() {
    std::printf("\n[回折] 前川式 ── 稜線を回る経路、帯域のハイ落ち、扉の開きに対する立ち上がりと連続性\n");
    MaterialTable mats;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const int matId = mats.add(wall);
    const int leafId = mats.add(AcousticMaterial::woodDoor());
    Surfaces s;
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) s.add(sb.obb, matId, false);
    Listener lis; lis.pos = Vec3(0, 1.6f, -3.0f); lis.forward = Vec3(0, 0, 1); lis.up = Vec3(0, 1, 0);
    const Vec3 L = lis.pos;
    char buf[220];

    // 1) 仕切りの裏の点音源: 有効、δ > 0、低域が高域より通る、方向は戸口の縁（右）、到達は直接より後
    {
        const Vec3 S(3.0f, 1.6f, 3.0f);
        const Visibility v = discVisibility(s, mats, L, S, 0.0f);
        const Diffraction d = edgeDiffraction(s, lis, S, v);
        float gain[kNumBands]; maekawa::gainBands(d.delta, gain, true);
        std::snprintf(buf, sizeof(buf), "(δ %.3f m、125Hz %.1f dB / 4k %.1f dB、方向 local (%.2f, %.2f, %.2f)、回折点 (%.2f, %.2f, %.2f))",
                      d.delta, afti::dB(d.energy6[0]), afti::dB(d.energy6[5]), d.dirLocal.x, d.dirLocal.y, d.dirLocal.z, d.point.x, d.point.y, d.point.z);
        check("[回折] 仕切りの裏の音源: 稜線を回る経路が見つかり δ > 0、到達は直接より後", d.valid && d.delta > 0.0f && d.pathSec > length(S - L) / kSpeedOfSound, buf);
        check("[回折] 低域ほど通る（125 Hz > 4 kHz）、量は前川式そのもの", d.energy6[0] > d.energy6[5] && std::fabs(d.energy6[3] - gain[3] * gain[3]) < 1e-6f, buf);
        check("[回折] 方向は戸口の縁（リスナーから見て右、前）", d.dirLocal.x > 0.05f && d.dirLocal.z > 0.5f, buf);
        const Visibility vOpen = discVisibility(s, mats, L, Vec3(0, 1.6f, 3.0f), 0.0f);
        check("[回折] 見通しがあれば回折は無効", !edgeDiffraction(s, lis, Vec3(0, 1.6f, 3.0f), vOpen).valid);
    }

    // 2) 扉 0→90°: 直接 + 回折 + 透過 の合計（自由音場に対する dB）。
    //    閉は透過だけ。回折が立つ角度は早い（枠の厚み 0.2 m を板の自由端が抜ける約 3°）。★その 1 段は物理（滑らかにしない）。
    //    立った後は連続（1° で 3 dB 未満）で、影の境（53〜68°）でも跳ばない。
    {
        const Vec3 S(0, 1.6f, 3.0f);
        const int leaf = s.add(doorLeaf(0.0f), leafId, true);
        int firstDiff = -1, worstDeg = -1; double prevDb = -999.0, worstAfter = 0.0, closedDb = 0.0; bool monoAfter = true;
        std::printf("      角度→合計(dB):");
        for (int deg = 0; deg <= 90; ++deg) {
            s.at(leaf).obb = doorLeaf(static_cast<float>(deg));
            const Visibility v = discVisibility(s, mats, L, S, 0.2f);
            const Diffraction d = edgeDiffraction(s, lis, S, v);
            double tot = 0.0;
            for (int b = 0; b < kNumBands; ++b) {
                const double e = v.visible + (1.0 - v.visible) * ((d.valid ? d.energy6[b] : 0.0f) + v.shadowTau6[b]);
                tot += e / kNumBands;
            }
            const double db = afti::dB(tot);
            if (deg % 5 == 0 || (deg >= 8 && deg <= 16) || (deg >= 50 && deg <= 70)) std::printf(" %d:%.1f", deg, db);
            if (deg == 0) closedDb = db;
            if (d.valid && firstDiff < 0) firstDiff = deg;
            if (deg > 0) {
                if (db < prevDb - 0.3) { monoAfter = false; std::printf(" [%d°で %.2f dB 戻り]", deg, prevDb - db); }
                if (std::fabs(db - prevDb) > worstAfter) { worstAfter = std::fabs(db - prevDb); worstDeg = deg; }
            }
            prevDb = db;
        }
        // ★回折が立つ角度は枠の厚みで決まる: 板の自由端が枠の奥行き（半厚み 0.1 m）＋板の半厚み 0.03 を抜ける
        //   sinθ > 0.13 → θ ≈ 7.5°。それまでの隙間は mm で、波長より小さく前川の範囲外（透過だけ）。余裕 +5° で 12°。
        //   0.2 m の壁ならこれが物理。10 cm の壁の家の扉なら 4° 前後で立つ。

        std::printf("\n");
        // 診断: 段差が出た角度で、候補の稜線ごとの経路を並べる（AF_DIFF_DEBUG があるとき）
        if (std::getenv("AF_DIFF_DEBUG") && worstDeg > 0) {
            for (int deg = worstDeg - 1; deg <= worstDeg; ++deg) {
                s.at(leaf).obb = doorLeaf(static_cast<float>(deg));
                const Visibility v = discVisibility(s, mats, L, S, 0.2f);
                const Diffraction d = edgeDiffraction(s, lis, S, v);
                double tot = 0.0;
                for (int b = 0; b < kNumBands; ++b) tot += (v.visible + (1.0 - v.visible) * ((d.valid ? d.energy6[b] : 0.0f) + v.shadowTau6[b])) / kNumBands;
                std::printf("      %d°: 合計 %.2f dB / 見通し %.3f 遮る物 %d τ(125) %.5f τ(1k) %.5f / 回折 %s 箱 %d 稜線 %d δ %.3f w %.2f 隙間 %.3f m 通り(125) %.3f (4k) %.3f 量(125) %.2f dB\n",
                            deg, afti::dB(tot), v.visible, v.shadowers, v.shadowTau6[0], v.shadowTau6[3],
                            d.valid ? "有効" : "無効", d.box, d.edge, d.delta, d.weight, d.gapWidth, d.gapOpen6[0], d.gapOpen6[5], afti::dB(d.energy6[0]));
                const Obb& b = s.at(leaf).obb;
                for (int e = 0; e < 12; ++e) {
                    Vec3 A, B, ow; detail::boxEdge(b, e, A, B, ow);
                    const float halfMin = std::min(b.halfExtents.x, std::min(b.halfExtents.y, b.halfExtents.z));
                    const Vec3 P = (A + B) * 0.5f + Vec3(0, 1.6f - (A.y + B.y) * 0.5f, 0) * ((std::fabs(B.y - A.y) > 1e-3f) ? 1.0f : 0.0f) + ow * ((std::min(halfMin, 0.03f) + kEps) * 1.41421356f);
                    float penOwn = std::max(segmentObbPenetration(L, P, b), segmentObbPenetration(P, S, b));
                    float penOther = 0.0f;
                    for (int j = 0; j < s.count(); ++j) if (j != leaf) penOther = std::max(penOther, std::max(segmentObbPenetration(L, P, s.at(j).obb), segmentObbPenetration(P, S, s.at(j).obb)));
                    std::printf("         稜線 %2d: P (%.3f, %.2f, %.3f) 長さ %.3f 自箱の貫通 %.3f 他の貫通 %.3f\n", e, P.x, P.y, P.z, length(P - L) + length(S - P), penOwn, penOther);
                }
            }
        }
        // ★段 9: 隙間の通り（aperture.h の gapOpen）を掛けたので**段が無くなった**。
        //   前は「経路が生まれた瞬間に +10.5 dB、その後 40° で 2.6 dB」（試聴で「急な変化が激しい」）。
        //   いまは 0°→90° の全域で単調に、しかも 1° あたり 2 dB 未満で開く。
        std::snprintf(buf, sizeof(buf), "(閉 %.1f dB → 90° %.1f dB、1° あたり最大 %.2f dB（%d°）。回折は %d° から)", closedDb, prevDb, worstAfter, worstDeg, firstDiff);
        check("[回折] 扉: 閉は透過だけ（−20 dB 以下）、全開で 0 dB", closedDb < -20.0 && prevDb > -0.5, buf);
        check("[回折] 扉: 0→90° の全域で単調に開き、どの 1° でも 2 dB より小さい（段が無い）", monoAfter && worstAfter < 2.0, buf);
        // ★色の変化（Ⅸ「開度の残り区間を担うのは鮮明さ」）: 狭い隙間は高域だけ通し、開くと低域が入ってくる。
        //   隙間の通り（gapOpen）は高域が先に開き、前川の減衰は高域を落とす。差し引きで、開くにつれて
        //   **高域の取り分が増えてから低域が追いつく**。低域／高域の比が角度で動くことを見る。
        double ratio[3] = {};   // 10°, 30°, 60° の 125Hz / 4kHz（回折のエネルギー比）
        for (int k = 0; k < 3; ++k) {
            const int deg = (k == 0) ? 10 : (k == 1) ? 30 : 60;
            s.at(leaf).obb = doorLeaf(static_cast<float>(deg));
            const Visibility v = discVisibility(s, mats, L, S, 0.2f);
            const Diffraction d = edgeDiffraction(s, lis, S, v);
            ratio[k] = (d.valid && d.energy6[5] > 0.0f) ? afti::dB(d.energy6[0] / d.energy6[5]) : 0.0;
        }
        std::snprintf(buf, sizeof(buf), "(125Hz − 4kHz: 10° %+.1f dB / 30° %+.1f dB / 60° %+.1f dB)", ratio[0], ratio[1], ratio[2]);
        check("[回折] 扉: 開くにつれて色が動く（低域／高域の比が 3 dB 以上変わる。狭い隙間は高域が先）", std::fabs(ratio[2] - ratio[0]) > 3.0, buf);
    }

    // 3) 世界を通す: 扉 20° で回折のタップがあり、方向は右、帳簿は保存
    {
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(20.0f), lm, true);
        w.raysPerEmitter = 128;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
        w.build();
        const float dt = 512.0f / 48000.0f;
        for (int k = 0; k < 5; ++k) w.update(dt);
        const Mix& m = *w.mix(e);
        int diffTap = -1;
        for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Diffract) diffTap = i;
        double dif = 0, dir = 0; for (int b = 0; b < kNumBands; ++b) { dif += m.component6[kDiffract][b]; dir += m.component6[kDirect][b]; }
        std::snprintf(buf, sizeof(buf), "(回折 %.2e、直接 %.2e、タップ %d 本、回折の方向 local x %.2f)", dif, dir, m.tapCount, diffTap >= 0 ? m.taps[diffTap].dirLocal.x : 0.0f);
        check("[回折] 世界: 扉 20° で回折のタップが立ち（直接は 0）、方向は隙間の側", diffTap >= 0 && dif > 0.0 && dir == 0.0 && m.taps[diffTap].dirLocal.x > 0.0f, buf);
        check("[回折] 世界: 帳簿は回折を入れても保存", m.conserves(0.01f));
    }
}

// ================================ [虚像] image_sources（段 7）
void testImageSources() {
    std::printf("\n[虚像] ISM ── 鏡映の位置、正規化、戸口を通る妥当性、歩行と面の縁での連続性\n");
    MaterialTable mats;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const int matId = mats.add(wall);
    char buf[220];

    // 1) 閉じた箱: 1 次の虚像は 6 面ぶん。床の虚像は y を鏡映。重みは帯域ごとに Σ = 1。最初の到達は直接より後。
    {
        Surfaces box = closedBox(3.5f, 3.0f, matId);
        std::vector<Face> faces; collectFaces(box, mats, faces);
        Listener lis; lis.pos = Vec3(-1.0f, 1.2f, 1.5f); lis.forward = Vec3(0, 0, 1); lis.up = Vec3(0, 1, 0);
        const Vec3 S(1.5f, 1.6f, -1.0f);
        ImageSet im; buildImages(box, faces, lis, S, 0.15f, 0.06f, im);
        int order1 = 0; bool floorOk = false; float sum[kNumBands] = {};
        for (int i = 0; i < im.count; ++i) {
            if (im.img[i].order == 1) ++order1;
            if (im.img[i].order == 1 && std::fabs(im.img[i].pos.y + 1.6f) < 1e-3f && std::fabs(im.img[i].pos.x - 1.5f) < 1e-3f) floorOk = true;
            for (int b = 0; b < kNumBands; ++b) sum[b] += im.img[i].weight6[b];
        }
        float worst = 0.0f; for (int b = 0; b < kNumBands; ++b) worst = std::max(worst, std::fabs(sum[b] - 1.0f));
        std::snprintf(buf, sizeof(buf), "(面 %d、検討 %d、有効 %d（1 次 %d）、Σ重み のずれ %.2e、最初の虚像 %.1f ms 対 直接 %.1f ms)",
                      static_cast<int>(faces.size()), im.candidates, im.count, order1, worst, im.firstSec * 1000.0f, length(S - lis.pos) / kSpeedOfSound * 1000.0f);
        check("[虚像] 閉じた箱: 36 面のうち 1 次の虚像が 6 本、床の虚像は y を鏡映した位置", faces.size() == 36 && order1 == 6 && floorOk, buf);
        check("[虚像] 重みは帯域ごとに Σ = 1（正規化）、2 次の虚像も入る", worst < 1e-4f && im.count > 6, buf);
        check("[虚像] 最初の虚像の到達は直接より後（尾の開始 = ITDG の材料）", im.firstSec > length(S - lis.pos) / kSpeedOfSound, buf);
    }

    // 2) 戸口: 向こうの部屋の音源。扉を閉めると虚像は全部無効（脚が板に遮られる）、開ければ向こうの壁の虚像が見える。
    {
        Surfaces s;
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) s.add(sb.obb, matId, false);
        const int leafId = mats.add(AcousticMaterial::woodDoor());
        std::vector<Face> faces; collectFaces(s, mats, faces);
        Listener lis; lis.pos = Vec3(0, 1.6f, -3.0f); lis.forward = Vec3(0, 0, 1); lis.up = Vec3(0, 1, 0);
        const Vec3 S(0, 1.6f, 3.0f);
        ImageSet open; buildImages(s, faces, lis, S, 0.2f, 0.06f, open);
        int farWall = 0; for (int i = 0; i < open.count; ++i) if (open.img[i].pos.z > 7.0f) ++farWall;
        const int leaf = s.add(doorLeaf(0.0f), leafId, true);
        ImageSet closed; buildImages(s, faces, lis, S, 0.2f, 0.06f, closed);
        std::snprintf(buf, sizeof(buf), "(開: 有効 %d（うち向こうの壁の虚像 %d）／閉: 有効 %d。面 %d)", open.count, farWall, closed.count, static_cast<int>(faces.size()));
        check("[虚像] 戸口が開いていれば向こうの部屋の壁の虚像が見える（「向こうの部屋の初期反射」）", open.count > 0 && farWall > 0, buf);
        check("[虚像] 扉を閉めると虚像は全部無効（脚が板に遮られる。板そのものは面に数えない）", closed.count == 0, buf);
        s.at(leaf).active = false;
        // 面の縁での連続性: リスナーを x 方向へ 2 cm ずつずらすと、向こうの壁の虚像は戸口の縁で消えていく。
        //   ★可視率そのものは速く動く: 戸口（3 m 先）の縁の影が 14 m 先の虚像の円盤（r 0.2）を横切るので、
        //     2 cm の移動が円盤の上では 9 cm（てこ）。これは物理（近い縁の向こうの遠い小さな音源は速く隠れる）。
        //     揺れとして効くのは**正規化した重み**（初期のエネルギーの取り分）で、遠い虚像は 1/d² で取り分が小さい。
        //     検査は 可視率が単調 と 重みの 2 cm あたりの動き < 0.05（初期の 5%）。
        float prevV = -1.0f, prevW = -1.0f, worstW = 0.0f; bool mono = true;
        for (float x = 0.0f; x <= 2.0f; x += 0.02f) {
            lis.pos = Vec3(x, 1.6f, -3.0f);
            ImageSet im; buildImages(s, faces, lis, S, 0.2f, 0.06f, im);
            float v = 0.0f, wgt = 0.0f;
            for (int i = 0; i < im.count; ++i) if (im.img[i].order == 1 && im.img[i].pos.z > 7.0f) { v = std::max(v, im.img[i].validity); wgt += im.img[i].weight6[2]; }
            if (prevV >= 0.0f) { if (v > prevV + 1e-3f) mono = false; worstW = std::max(worstW, std::fabs(wgt - prevW)); }
            prevV = v; prevW = wgt;
        }
        std::snprintf(buf, sizeof(buf), "(x=0→2 m で向こうの壁の虚像の可視率が 1→%.3f（単調）、正規化した重みの 2 cm あたり最大 %.4f)", prevV, worstW);
        check("[虚像] 横へ歩くと向こうの壁の虚像は戸口の縁で単調に消え、初期の取り分は 2 cm で 5% も動かない", mono && prevV < 0.05f && worstW < 0.05f, buf);
    }

    // 3) 世界を通す: 閉じた箱で初期のタップが虚像の数だけ立ち、方向は単位、和は初期の総量（帳簿は保存）。歩いても重みは揺れない。
    {
        World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
        w->raysPerEmitter = 128;
        w->earlyModel = 0;   // この検査は虚像（ISM）の道の見張り。受取面は [受取面] で見る
        w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w->addEmitter(Vec3(1.5f, 1.6f, -1.0f), 0.0f);
        w->build();
        const float dt = 512.0f / 48000.0f;
        for (int k = 0; k < 10; ++k) w->update(dt);
        const Mix& m = *w->mix(e);
        // 初期のタップ ＝ 虚像の数 ＋ 方向なしの残り 1 本（id 4）。虚像のほうは方向が単位。
        int early = 0, plain = 0; bool unit = true; double eSum = 0.0, eRaw = 0.0;
        for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) {
            ++early;
            if (m.taps[i].id == 4) ++plain;
            else if (std::fabs(length(m.taps[i].dirLocal) - 1.0f) > 1e-3f) unit = false;
            for (int b = 0; b < kNumBands; ++b) eSum += m.taps[i].e6[b];
        }
        for (int b = 0; b < kNumBands; ++b) eRaw += m.component6[kEarly][b];
        double dirFrac = 0.0; for (int b = 0; b < kNumBands; ++b) dirFrac += w->images(e)->directional6[b] / kNumBands;
        std::snprintf(buf, sizeof(buf), "(初期のタップ %d 本 = 虚像 %d ＋ 方向なし %d、方向の割合 %.3f、Σe6 %.3e 対 帳簿の初期 %.3e、尾の開始 %.1f ms)",
                      early, w->images(e)->count, plain, dirFrac, eSum, eRaw, m.onsetSec * 1000.0f);
        check("[虚像] 世界: 初期のタップは虚像＋方向なしの残り、虚像の方向は単位、和は初期の総量（平滑後）",
              early == w->images(e)->count + 1 && plain == 1 && early >= 6 && unit && std::fabs(afti::dB(eSum / eRaw)) < 0.5, buf);
        check("[虚像] 世界: 帳簿は虚像に割っても保存、尾の開始は最初の虚像", m.conserves(0.01f) && std::fabs(m.onsetSec - w->images(e)->firstSec) < 1e-6f, buf);
        // 歩行: 重みの最大の段差
        double worst = 0.0; std::vector<float> prevW;
        Vec3 L = w->listener().pos;
        for (int k = 0; k < 60; ++k) {
            L = L + Vec3(0, 0, -1.4f * dt); w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0)); w->update(dt);
            const ImageSet* im = w->images(e);
            std::vector<float> cur(static_cast<std::size_t>(im->count));
            for (int i = 0; i < im->count; ++i) { float sw = 0; for (int b = 0; b < kNumBands; ++b) sw += im->img[i].weight6[b]; cur[static_cast<std::size_t>(i)] = sw / kNumBands; }
            if (prevW.size() == cur.size()) for (std::size_t i = 0; i < cur.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(cur[i] - prevW[i])));
            prevW = cur;
        }
        std::snprintf(buf, sizeof(buf), "(1.4 m/s で 60 フレーム: 虚像の重みの 1 フレームあたり最大 %.4f)", worst);
        check("[虚像] 世界: 歩いても虚像の重みが 1 フレームで 0.05 も動かない", worst < 0.05, buf);
        delete w;
    }
}

// ================================ [予算] budget（段 8）
void testBudget() {
    std::printf("\n[予算] 総レイ数の固定・優先度・ヒステリシス・保持と探り・並列でビット一致\n");
    char buf[220];
    // 1) 点数と順位: 操作対象が最優先、次に距離、音量、変化量
    {
        Budget b; b.cfg.totalRays = 1000; b.cfg.fullSlots = 2; b.cfg.lightSlots = 2; b.cfg.promoteSec = 0.0f; b.cfg.demoteSec = 0.0f;
        Emitter e[5];
        for (int i = 0; i < 5; ++i) { e[i].id = i; e[i].loudness = 1.0f; e[i].pos = Vec3(0, 0, 2.0f + i); }
        e[4].operated = true;                              // いちばん遠いが操作対象
        e[1].loudness = 0.2f;                              // 近いが小さい
        e[2].change = 4.0f;                                // 動いている
        std::vector<const Emitter*> ems; for (int i = 0; i < 5; ++i) ems.push_back(&e[i]);
        std::vector<BudgetSlot> sl;
        b.update(ems, Vec3(0, 0, 0), 1.0f / 60.0f, 1.0f, 256, sl);
        std::snprintf(buf, sizeof(buf), "(順位: 操作対象(遠い) %d、静かで近い %d、動いている %d、点数 %.3f / %.3f / %.3f)",
                      sl[4].wantSlot, sl[1].wantSlot, sl[2].wantSlot, sl[4].score, sl[1].score, sl[2].score);
        check("[予算] 操作対象はいちばん遠くても 1 位（Ⅶ の優先度 1）", sl[4].wantSlot == 0, buf);
        check("[予算] 音量が小さい音源は同じ距離でも下がる（優先度 3）", sl[1].wantSlot > sl[0].wantSlot, buf);
        check("[予算] 動いている音源は静止した同距離より上がる（優先度 4 = 変化量）", sl[2].score > sl[3].score, buf);
        const int spent = Budget::spentRays(sl, 5);
        int nFull = 0, nHold = 0;
        for (int i = 0; i < 5; ++i) { if (sl[i].tier == Tier::Full) ++nFull; if (sl[i].tier == Tier::Hold) ++nHold; }
        std::snprintf(buf, sizeof(buf), "(厳密 %d / 保持 %d、飛ばした %d 本（総予算 1000）)", nFull, nHold, spent);
        check("[予算] 厳密は枠のぶんだけ、漏れた音源は保持（0 本）、合計は総予算を大きく超えない", nFull == 2 && nHold == 1 && spent <= 1000 + b.cfg.minPerEmitter, buf);
    }
    // 2) ヒステリシス: 昇格 0.5 s / 降格 1.0 s
    {
        Budget b; b.cfg.totalRays = 1000; b.cfg.fullSlots = 1; b.cfg.lightSlots = 0;
        Emitter a, c; a.id = 0; c.id = 1; a.loudness = 1.0f; c.loudness = 0.5f;
        a.pos = Vec3(0, 0, 1); c.pos = Vec3(0, 0, 2);
        std::vector<const Emitter*> ems{&a, &c};
        std::vector<BudgetSlot> sl;
        const float dt = 1.0f / 60.0f; float t = 0.0f;
        for (int k = 0; k < 60; ++k) { t += dt; b.update(ems, Vec3(0, 0, 0), dt, t, 256, sl); }
        check("[予算] 最初は点数の高い方が厳密", sl[0].tier == Tier::Full && sl[1].tier == Tier::Hold);
        // 入れ替える（c を近く大きく）。昇格は 0.5 s 掛かる
        c.loudness = 1.0f; c.pos = Vec3(0, 0, 0.5f);
        int upFrames = -1, downFrames = -1;
        for (int k = 0; k < 120; ++k) {
            t += dt; b.update(ems, Vec3(0, 0, 0), dt, t, 256, sl);
            if (upFrames < 0 && sl[1].tier == Tier::Full) upFrames = k + 1;
            if (downFrames < 0 && sl[0].tier != Tier::Full) downFrames = k + 1;
        }
        std::snprintf(buf, sizeof(buf), "(昇格 %d フレーム = %.2f s（0.5 s）、降格 %d フレーム = %.2f s（1.0 s）)", upFrames, upFrames * dt, downFrames, downFrames * dt);
        check("[予算] 昇格は 0.5 s、降格は 1.0 s 続けて条件を満たしてから（段の境で行き来しない）",
              upFrames > 25 && upFrames < 35 && downFrames > 55 && downFrames < 65, buf);
    }
    // 3) 探り: 保持の音源が順繰りに解き直される
    {
        Budget b; b.cfg.totalRays = 600; b.cfg.fullSlots = 1; b.cfg.lightSlots = 0; b.cfg.promoteSec = 0.0f; b.cfg.demoteSec = 0.0f; b.cfg.probesPerFrame = 1;
        Emitter e[4]; std::vector<const Emitter*> ems;
        for (int i = 0; i < 4; ++i) { e[i].id = i; e[i].loudness = 1.0f; e[i].pos = Vec3(0, 0, 1.0f + i); ems.push_back(&e[i]); }
        std::vector<BudgetSlot> sl;
        const float dt = 1.0f / 60.0f; float t = 0.0f;
        int probed[4] = {};
        for (int k = 0; k < 12; ++k) {
            t += dt; b.update(ems, Vec3(0, 0, 0), dt, t, 256, sl);
            for (int i = 0; i < 4; ++i) if (sl[i].probing) ++probed[i];
        }
        std::snprintf(buf, sizeof(buf), "(12 フレームで探られた回数: %d / %d / %d / %d（保持は 3 本）)", probed[0], probed[1], probed[2], probed[3]);
        check("[予算] 保持の 3 本が順繰りに探られる（12 フレームで各 4 回）", probed[1] == 4 && probed[2] == 4 && probed[3] == 4 && probed[0] == 0, buf);
    }
    // 4) 世界: 総予算を絞ると合計が収まり、保持の音源は最後の答えを保つ。並列でも直列とビット一致。
    {
        World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
        // ★ヒステリシスは既定のまま（0.5 / 1.0 s）。段は最初のフレームで決まる（primed）ので待つ必要がない。
        //   ここで promoteSec=0 にすると、下の「保持は最後の答えを保つ」で音源を動かした瞬間に
        //   変化量で点数が跳ねて即昇格し、検査の意味が消える（実際に踏んだ）。
        w->budget.cfg.totalRays = 512; w->budget.cfg.fullSlots = 1; w->budget.cfg.lightSlots = 1;
        w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        int ids[4];
        for (int i = 0; i < 4; ++i) ids[i] = w->addEmitter(Vec3(1.0f - 0.5f * i, 1.6f, -1.0f + 0.4f * i), 0.0f);
        w->build();
        const float dt = 512.0f / 48000.0f;
        for (int k = 0; k < 10; ++k) w->update(dt);
        int tiers[4]; float held[6];
        int fullId = -1, holdId = -1;
        for (int i = 0; i < 4; ++i) {
            tiers[i] = w->tierOf(ids[i]);
            if (tiers[i] == 0 && fullId < 0) fullId = ids[i];
            if (tiers[i] == 2 && holdId < 0) holdId = ids[i];
        }
        std::snprintf(buf, sizeof(buf), "(段 %d/%d/%d/%d、飛ばした %d 本（総予算 512）。厳密の本数 %d)", tiers[0], tiers[1], tiers[2], tiers[3], w->spentRays(), fullId >= 0 ? w->raysOf(fullId) : 0);
        check("[予算] 世界: 厳密・簡易・保持に分かれ、飛ばした本数が総予算に収まる", w->spentRays() <= 512 + 64 && fullId >= 0 && holdId >= 0, buf);
        // 保持の音源を動かしても Mix は変わらない（最後の答えを保つ）。
        //   ★探りを切って測る ── 探りが有効なら保持の音源も順繰りに解き直されるので変わる（それが探りの仕事。上の 3 で検査）。
        w->budget.cfg.probesPerFrame = 0;
        w->update(dt);
        for (int b = 0; b < kNumBands; ++b) held[b] = w->mix(holdId)->energy6[b];
        w->setEmitter(holdId, Vec3(3.0f, 1.6f, 3.0f), 0.0f, false, 1.0f);
        w->update(dt);
        bool same = true; for (int b = 0; b < kNumBands; ++b) if (w->mix(holdId)->energy6[b] != held[b]) same = false;
        std::snprintf(buf, sizeof(buf), "(保持の音源 %d: 段 %d、本数 %d、総量 500Hz %.4e → %.4e)", holdId, w->tierOf(holdId), w->raysOf(holdId), held[2], w->mix(holdId)->energy6[2]);
        check("[予算] 保持の音源は解かないので最後の答えを保つ（探りを切ったとき。素通しにも無音にもならない）", same && w->raysOf(holdId) == 0, buf);
        // 探りを戻すと、保持の音源も順繰りに更新される（扉が開いても眠らない）
        w->budget.cfg.probesPerFrame = 1;
        for (int k = 0; k < 6; ++k) w->update(dt);
        bool moved = false; for (int b = 0; b < kNumBands; ++b) if (w->mix(holdId)->energy6[b] != held[b]) moved = true;
        check("[予算] 探りを戻すと保持の音源も数フレームで更新される（扉が開いても眠らない）", moved);
        // 並列（4 スレッド）でも直列とビット一致
        World* w2 = makeWorldBox(3.5f, 3.0f, 0.2f);
        w2->budget.cfg.totalRays = 512; w2->budget.cfg.fullSlots = 4; w2->budget.cfg.lightSlots = 0;
        w2->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        int ids2[4]; for (int i = 0; i < 4; ++i) ids2[i] = w2->addEmitter(Vec3(1.0f - 0.5f * i, 1.6f, -1.0f + 0.4f * i), 0.0f);
        w2->build();
        for (int k = 0; k < 10; ++k) w2->update(dt);
        std::vector<float> serial;
        for (int i = 0; i < 4; ++i) for (int b = 0; b < kNumBands; ++b) serial.push_back(w2->mix(ids2[i])->energy6[b]);
        w2->setWorkers(4);
        for (int k = 0; k < 10; ++k) w2->update(dt);
        bool bitEqual = true; std::size_t p = 0;
        for (int i = 0; i < 4; ++i) for (int b = 0; b < kNumBands; ++b, ++p) if (w2->mix(ids2[i])->energy6[b] != serial[p]) bitEqual = false;
        std::snprintf(buf, sizeof(buf), "(スレッド %d、音源 4 本)", w2->workers());
        check("[予算] 4 スレッドで解いても直列とビット一致（音源ごとに自分の枠しか触らない）", bitEqual && w2->workers() == 4, buf);
        delete w2; delete w;
    }
    // 5) フレーム分散: 静止していれば分散していても揺れない（組の合計が一定）。動けば数フレームで入れ替わる。
    {
        World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
        w->budget.cfg.totalRays = 0; w->raysPerEmitter = 256; w->rayGroups = 4;
        w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w->addEmitter(Vec3(1.5f, 1.6f, -1.0f), 0.0f);
        w->build();
        const float dt = 512.0f / 48000.0f;
        for (int k = 0; k < 8; ++k) w->update(dt);              // 組を一周させる
        double worst = 0.0; float prev[6];
        for (int b = 0; b < kNumBands; ++b) prev[b] = w->mix(e)->energy6[b];
        for (int k = 0; k < 20; ++k) {
            w->update(dt);
            for (int b = 0; b < kNumBands; ++b) { worst = std::max(worst, std::fabs(afti::dB(w->mix(e)->energy6[b] / prev[b]))); prev[b] = w->mix(e)->energy6[b]; }
        }
        std::snprintf(buf, sizeof(buf), "(組 4、静止 20 フレームの帳簿の段差 最大 %.4f dB、レイ %d 本/フレーム（総数 256）)", worst, w->trace(e)->raysTraced / 4);
        check("[予算] フレーム分散: 静止していれば組を回しても揺れない（レイの種はレイ番号で作る）", worst < 1e-3, buf);
        // 分散なしと合計が一致する（同じレイを全部足したのと同じ）
        World* w1 = makeWorldBox(3.5f, 3.0f, 0.2f);
        w1->budget.cfg.totalRays = 0; w1->raysPerEmitter = 256; w1->rayGroups = 1;
        w1->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e1 = w1->addEmitter(Vec3(1.5f, 1.6f, -1.0f), 0.0f);
        w1->build();
        for (int k = 0; k < 8; ++k) w1->update(dt);
        double diff = 0.0;
        for (int b = 0; b < kNumBands; ++b) diff = std::max(diff, std::fabs(afti::dB(w->mix(e)->energy6[b] / w1->mix(e1)->energy6[b])));
        std::snprintf(buf, sizeof(buf), "(組 4 と 組 1 の帳簿の差 最大 %.4f dB)", diff);
        check("[予算] フレーム分散: 組に分けても合計は分散なしと一致（同じレイを足しているだけ）", diff < 1e-3, buf);
        delete w1; delete w;
    }
    // 6) 費用（参考。合否は付けない）: 音源を増やしても 1 フレームの時間が伸びないか
    {
        std::printf("      費用（1 フレームの主スレッド、7×7×3 m の箱。厳密 6 / 簡易 10）:\n");
        std::printf("        音源  分散なし・予算1536   分散 4 組・予算1536   ＋スレッド 4\n");
        for (int n : {1, 4, 12, 24}) {
            double ms[3] = {};
            for (int mode = 0; mode < 3; ++mode) {
                World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
                w->raysPerEmitter = 256;
                w->budget.cfg.totalRays = 1536; w->budget.cfg.fullSlots = 6; w->budget.cfg.lightSlots = 10;
                w->rayGroups = (mode == 0) ? 1 : 4;
                if (mode == 2) w->setWorkers(4);
                w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
                afti::Xorshift rng;
                for (int i = 0; i < n; ++i) w->addEmitter(Vec3(rng.next() * 3.0f, 1.0f + rng.next() * 0.5f, rng.next() * 3.0f), 0.0f);
                w->build();
                const float dt = 512.0f / 48000.0f;
                for (int k = 0; k < 3; ++k) w->update(dt);            // 温める
                const int reps = 10;
                const auto t0 = std::chrono::steady_clock::now();
                for (int k = 0; k < reps; ++k) w->update(dt);
                ms[mode] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
                delete w;
            }
            std::printf("        %3d   %8.2f ms          %8.2f ms      %8.2f ms\n", n, ms[0], ms[1], ms[2]);
        }
        // 内訳（音源 1 本、厳密 512 本）: レイ／見通し／回折／虚像 をそれぞれ単体で回す
        {
            MaterialTable mats;
            AcousticMaterial wall = AcousticMaterial::defaultWall();
            for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.0f; wall.scattering[b] = 0.5f; }
            const int matId = mats.add(wall);
            Surfaces box = closedBox(3.5f, 3.0f, matId);
            std::vector<Face> faces; collectFaces(box, mats, faces);
            Listener lis; lis.pos = Vec3(-1.0f, 1.2f, 1.5f); lis.forward = Vec3(0, 0, 1); lis.up = Vec3(0, 1, 0);
            const Vec3 S(1.5f, 1.6f, -1.0f);
            const int reps = 20;
            auto timeIt = [&](const char* name, const std::function<void()>& fn) {
                fn();
                const auto t0 = std::chrono::steady_clock::now();
                for (int k = 0; k < reps; ++k) fn();
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
                std::printf("        %-16s %7.3f ms\n", name, ms);
            };
            std::printf("      内訳（音源 1 本、箱 6 個・面 36）:\n");
            EnergyTrace tr; TraceParams prm; prm.rays = 512; prm.maxBounces = 40; prm.mixingSec = 0.03f; prm.seed = 7u;
            timeIt("レイ 512 本", [&] { volatile auto r = tr.run(box, mats, S, lis.pos, prm); (void)r; });
            // レイの費用の形（GPU へ移す価値を判断するため）。本数と跳ね返りで割る。
            //   ★1 本あたりの費用が本数に対して一定なら、そのまま並列化で線形に効く。
            std::printf("        レイの費用の形（当たり点ごとに影レイ 1 本＝次イベント推定）:\n");
            // 木と総当たりの分かれ目（箱の数を振る）。GPU 前提でも、CPU の基準実装は速いほうがよい。
            std::printf("        木と総当たりの分かれ目（レイ 256 本・跳ね 20）:\n");
            for (int nb2 : {6, 12, 24, 48, 96}) {
                Surfaces sb = closedBox(3.5f, 3.0f, matId);
                afti::Xorshift rr;
                for (int k = 6; k < nb2; ++k)
                    sb.add(Obb::axisAligned(Vec3(rr.next() * 2.8f, 1.0f + rr.next() * 0.8f, rr.next() * 2.8f),
                                            Vec3(0.15f, 0.3f, 0.15f)), matId, false);
                TraceParams pp{256, 20, 0.03f, 7u, 1, 0};
                sb.clearBvh();
                const auto t0 = std::chrono::steady_clock::now();
                for (int q = 0; q < 5; ++q) { volatile auto r2 = tr.run(sb, mats, S, lis.pos, pp); (void)r2; }
                const double msBrute = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0;
                const auto t1 = std::chrono::steady_clock::now();
                for (int q = 0; q < 5; ++q) { sb.rebuildBvh(); volatile auto r2 = tr.run(sb, mats, S, lis.pos, pp); (void)r2; }
                const double msTree = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / 5.0;
                std::printf("          箱 %3d : 総当たり %7.3f ms / 木 %7.3f ms（木の作り直し込み）  %+5.0f%%\n",
                            nb2, msBrute, msTree, (msTree / msBrute - 1.0) * 100.0);
            }
            for (int nr : {128, 512, 2048}) {
                for (int nb : {8, 20, 40}) {
                    TraceParams pp{nr, nb, 0.03f, 7u, 1, 0};
                    TraceResult rr = tr.run(box, mats, S, lis.pos, pp);
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int q = 0; q < 5; ++q) { volatile auto r2 = tr.run(box, mats, S, lis.pos, pp); (void)r2; }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0;
                    std::printf("          レイ %5d 跳ね %2d : %7.3f ms  当たり %6d  1 当たり %6.2f us\n",
                                nr, nb, ms, rr.hits, ms * 1000.0 / std::max(1, rr.hits));
                }
            }
            timeIt("見通し（幅 0.2）", [&] { volatile auto v = discVisibility(box, mats, lis.pos, S, 0.2f); (void)v; });
            Visibility vv = discVisibility(box, mats, lis.pos, S, 0.2f);
            timeIt("回折", [&] { volatile auto d = edgeDiffraction(box, lis, S, vv); (void)d; });
            // 動く箱を 1 つ足したときの回折の費用（段 9-c で候補に常時入れたぶん）
            {
                Surfaces box2 = box;
                box2.add(Obb::axisAligned(Vec3(0.0f, 1.2f, 0.0f), Vec3(0.5f, 1.2f, 0.03f)), matId, true);
                Visibility vv2 = discVisibility(box2, mats, lis.pos, S, 0.2f);
                timeIt("回折（動く箱 1 個）", [&] { volatile auto d = edgeDiffraction(box2, lis, S, vv2); (void)d; });
                // 遮る物がある配置での見通しの費用（簡易の段で点に落としている理由の検算）
                Visibility vs3 = discVisibility(box2, mats, lis.pos, S, 0.2f);
                std::printf("        （遮り %d 枚・見通し %.3f）\n", vs3.shadowers, vs3.visible);
                timeIt("見通し 幅0.2（遮りあり）", [&] { volatile auto v = discVisibility(box2, mats, lis.pos, S, 0.2f); (void)v; });
                timeIt("見通し 点（簡易の段）", [&] { volatile auto v = discVisibility(box2, mats, lis.pos, S, 0.0f); (void)v; });
            }
            ImageSet im;
            timeIt("虚像（1・2 次）", [&] { buildImages(box, faces, lis, S, 0.2f, 0.06f, im); });
            ImageSet im3;
            timeIt("虚像（1〜3 次）", [&] { buildImages(box, faces, lis, S, 0.2f, 0.06f, im3, 3); });
            std::printf("        虚像: 2 次 検討 %d / 有効 %d ／ 3 次 検討 %d / 有効 %d\n",
                        im.candidates, im.count, im3.candidates, im3.count);
        }
    }
}

// ================================ [漏れ] 閉じた扉から回折が漏れるか（AF_ONLY=leak）
void testLeakModels() {
    std::printf("\n[漏れ] 閉じた扉の回折 ── 模型 0 今のまま / 1 厚みの割合 / 2 口の空き具合 / 3 両方\n");
    char buf[220];
    // 場面は回帰の 2 部屋。扉は閉じたまま、耳を戸口の前で左右に振る。
    //   ★「漏れが聞こえる位置と聞こえない位置がある」という試聴報告を数字にする物差し。
    auto sweep = [&](int model, float clearance, double* outMax, int* outOn, int* outOff) {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
        World w;
        w.leakModel = model;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(0.0f, -0.5f, 1.0f, 3.0f, 0.06f, clearance), lm, true);   // 閉じたまま動かさない
        w.raysPerEmitter = 128; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(Vec3(0, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
        w.build();
        const float dt = 1.0f / 60.0f;
        double mx = 0.0; int on = 0, off = 0;
        for (int k = 0; k <= 24; ++k) {
            const float x = -3.0f + 0.25f * k;
            w.setListener(Vec3(x, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
            w.update(dt);
            const Mix* m = w.mix(e);
            double d = 0.0, tr = 0.0;
            for (int b = 0; b < kNumBands; ++b) { d += m->component6[kDiffract][b]; tr += m->component6[kTransmit][b]; }
            const Diffraction* dfm = w.diffraction(e);
            if (std::getenv("AF_LOUD"))
                std::printf("        x=%5.2f 回折 %10.3e 透過 %10.3e | %d/%d δ=%.4f a=%.4f w=%.2f\n",
                            x, d, tr, dfm->valid ? dfm->box : -1, dfm->valid ? dfm->edge : -1,
                            dfm->delta, dfm->gapWidth, dfm->weight);
            if (d > tr) ++on; else ++off;                    // 板を透るぶんより回折が大きい ＝ 漏れが立っている
            mx = std::max(mx, d / std::max(tr, 1e-30));
        }
        *outMax = mx; *outOn = on; *outOff = off;
    };
    const char* cle = std::getenv("AF_CLEARANCE");
    const float clr = cle ? static_cast<float>(std::atof(cle)) : 0.0f;
    std::printf("      枠との隙間 %.3f m（Unity の SwingDoor の既定は 0.004）\n", clr);
    for (int model = 0; model <= 3; ++model) {
        double mx; int on, off;
        sweep(model, clr, &mx, &on, &off);
        std::snprintf(buf, sizeof(buf), "(25 か所のうち漏れが透過を超えた所 %d、最大 透過の %.1f 倍 ＝ %+.1f dB)",
                      on, mx, afti::dB(mx));
        std::printf("      模型 %d %s\n", model, buf);
        if (model == 1 || model == 3) {
            std::snprintf(buf, sizeof(buf), "模型 %d（厚みの割合）: 閉じた扉から回折が漏れない（漏れた所 %d、最大 %+.1f dB）",
                          model, on, afti::dB(mx));
            check(buf, on == 0);
        }
        if (model == 0) {
            std::snprintf(buf, sizeof(buf), "模型 0（今のまま）: 漏れが残っていることを確かめる（漏れた所 %d）", on);
            check(buf, on > 0);      // ★基準点。ここが 0 になったら、この検査は何も見張っていない
        }
    }
}

// ================================ [本数の上限] Unity から 8192 本に届くか（AF_ONLY=raycap）
//   ★2026-09-11 に見つけた穴の見張り。raysPerEmitter を上げても、予算の上限 maxPerEmitter（既定 512）で
//     頭打ちになり、Unity からは GPU の効きが聞こえなかった。検査の口（AF_RAYS）は上限も一緒に上げていたので気付かなかった。
//     耳では「GPU を入れても何も変わらない」にしか聞こえないので、数で見張る。
void testRayCap() {
    std::puts("[本数の上限] raysPerEmitter と maxPerEmitter の関係");   // puts は改行を自分で足す
    char buf[256];
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    auto raysAfter = [&](int perEmitter, int maxPer) {
        World w;
        const int m2 = w.rules.materials.add(wall);
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.raysPerEmitter = perEmitter; w.maxBounces = 1; w.rayGroups = 1;
        w.budget.cfg.totalRays = 0;
        if (maxPer > 0) w.budget.cfg.maxPerEmitter = maxPer;
        w.setListener(Vec3(0, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int id = w.addEmitter(Vec3(0.0f, 1.6f, 2.0f), 0.2f);
        w.build();
        for (int f = 0; f < 70; ++f) w.update(1.0f / 60.0f);      // 昇格のヒステリシス（0.5 s）を越えるまで回す
        return w.raysOf(id);
    };
    const int stuck = raysAfter(8192, 0);
    std::snprintf(buf, sizeof(buf), "(raysPerEmitter 8192・上限は既定のまま → %d 本)", stuck);
    check("[本数の上限] 上限を上げなければ既定の 512 で止まる（Unity で GPU が効かなかった原因）", stuck == 512, buf);
    const int open = raysAfter(8192, 8192);
    std::snprintf(buf, sizeof(buf), "(raysPerEmitter 8192・上限 8192 → %d 本)", open);
    check("[本数の上限] 上限を上げれば 8192 本に届く（AF_WorldSetMaxRaysPerEmitter が効く道）", open == 8192, buf);
}

// ================================ [材質] 実行中に材質を変える（AF_ONLY=material）
//   Unity の Inspector で defaultMaterial や AcousticSurface を動かしたときの道。
//   ① 静的な箱が使う材質を書き換えると、次の update で部屋を組み直し、RT60 が追う
//   ② 動く箱（扉の板）だけが使う材質を書き換えても組み直さない
//   ③ 箱の材質を差し替えると組み直す。同じ材質を入れ直しても組み直さない
void testMaterialChange() {
    std::printf("\n[材質] 実行中に材質を変えると部屋の響きが追う\n");
    char buf[256];
    const float dt = 1.0f / 60.0f;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.05f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    World w;
    const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
    w.addBox(doorLeaf(90.0f), lm, true);
    w.raysPerEmitter = 64; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
    const Vec3 L(0, 1.6f, -3.0f);
    w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
    w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
    w.build();
    w.update(dt);
    const int room = w.roomAt(L);
    const int builds0 = w.buildCount();
    const float rt0 = (room >= 0) ? w.probe(room).rt60[3] : 0.0f;
    // ① 壁の材質の中身を吸う側へ
    AcousticMaterial absorb = wall;
    for (int b = 0; b < kNumBands; ++b) absorb.absorption[b] = 0.4f;
    const bool okSet = w.updateMaterial(m2, absorb);
    w.update(dt);
    const float rt1 = (room >= 0) ? w.probe(room).rt60[3] : 0.0f;
    std::snprintf(buf, sizeof(buf), "(組み直し %d → %d 回、部屋 %d の 1 kHz の RT60 %.2f → %.2f s)", builds0, w.buildCount(), room, rt0, rt1);
    check("[材質] 壁の吸音を上げると次の update で組み直し、RT60 が短くなる", okSet && room >= 0 && w.buildCount() == builds0 + 1 && rt1 < rt0 * 0.5f, buf);
    // ② 動く箱だけが使う材質
    const int builds1 = w.buildCount();
    AcousticMaterial door2 = AcousticMaterial::woodDoor();
    for (int b = 0; b < kNumBands; ++b) door2.absorption[b] = 0.5f;
    w.updateMaterial(lm, door2);
    w.update(dt);
    std::snprintf(buf, sizeof(buf), "(組み直し %d → %d 回)", builds1, w.buildCount());
    check("[材質] 動く箱だけが使う材質を変えても組み直さない（透過・吸音はそのフレームから効く）", w.buildCount() == builds1, buf);
    // ③ 箱の材質の差し替え
    const int m3 = w.rules.materials.add(wall);
    w.setBoxMaterial(0, m3);
    w.update(dt);
    const int builds2 = w.buildCount();
    w.setBoxMaterial(0, m3);
    w.update(dt);
    std::snprintf(buf, sizeof(buf), "(差し替えで %d → %d 回、同じ材質の入れ直しで %d 回)", builds1, builds2, w.buildCount());
    check("[材質] 箱の材質を差し替えると組み直し、同じ材質の入れ直しでは組み直さない", builds2 == builds1 + 1 && w.buildCount() == builds2, buf);
}

// ================================ [壁越し] 反射と残響は壁を抜けない（AF_ONLY=wall）
//   試聴「壁の向こうの透過音がダブる」。壁を横切る影の線を数えないと、閉じた扉の向こうでは
//   初期・後期がほぼ 0 になり、透過の直接音（解析）は変わらない。開いた戸口では通る分が残る。GPU も同じ。
void testWallReflect() {
    std::printf("\n[壁越し] 反射と残響は壁を抜けない（透過は直接だけ）\n");
    char buf[256];
    const float dt = 1.0f / 60.0f;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const Vec3 S(0.0f, 1.6f, 3.0f), L(0.0f, 1.6f, -3.0f);
    struct Got { double early = 0.0, late = 0.0, transmit = 0.0, direct = 0.0; };
    auto run = [&](float deg, int wallReflect, float clearance) {
        Got g;
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(deg, -0.5f, 1.0f, 3.0f, 0.06f, clearance), lm, true);
        w.wallReflect = wallReflect;
        w.raysPerEmitter = 2048; w.budget.cfg.maxPerEmitter = 2048; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        for (int f = 0; f < 40; ++f) w.update(dt);
        const Mix* mx = w.mix(e);
        for (int b = 0; b < kNumBands; ++b) {
            g.early += mx->component6[kEarly][b]; g.late += mx->component6[kLate][b];
            g.transmit += mx->component6[kTransmit][b]; g.direct += mx->component6[kDirect][b];
        }
        return g;
    };
    // 閉じた扉の向こう（隙間 0）: 旧は反射と残響が透過率で薄まって届く。新は透過の直接音だけ ＝ 初期・後期は厳密に 0
    const Got c1 = run(0.0f, 1, 0.0f), c0 = run(0.0f, 0, 0.0f);
    std::snprintf(buf, sizeof(buf), "(閉扉・隙間 0: 初期 %.2e → %.2e、後期 %.2e → %.2e、透過の直接 %.2e → %.2e)",
                  c1.early, c0.early, c1.late, c0.late, c1.transmit, c0.transmit);
    check("[壁越し] 閉じた扉（隙間 0）の向こうでは初期と後期が 0 になり、透過の直接音は変わらない",
          c0.early == 0.0 && c0.late == 0.0 && c0.transmit == c1.transmit && c1.late > 0.0, buf);
    // 閉じた扉（Unity と同じ 4 mm の隙間）: 隙間は本物の開口なので、そこを通る反射・残響は残る。
    //   ★4 mm をレイが引き当てるのはまれで、種によって旧の 0.1% だったり 11% だったりする（実測）。ここでは「旧より減る」だけを見る。
    const Got g1 = run(0.0f, 1, 0.004f), g0 = run(0.0f, 0, 0.004f);
    std::snprintf(buf, sizeof(buf), "(閉扉・隙間 4 mm: 初期＋後期 %.2e → %.2e ＝ 旧の %.1f%%。残りは隙間を通る本物の分)",
                  g1.early + g1.late, g0.early + g0.late, (g1.early + g1.late > 0.0) ? (g0.early + g0.late) / (g1.early + g1.late) * 100.0 : 0.0);
    check("[壁越し] 閉じた扉（隙間 4 mm）でも旧より減り、隙間を通る分だけが残る", g0.early + g0.late < g1.early + g1.late, buf);
    // 開いた扉: 戸口を通る分は残る（減るのは開いた板を横切る分だけ）
    const Got o1 = run(90.0f, 1, 0.004f), o0 = run(90.0f, 0, 0.004f);
    std::snprintf(buf, sizeof(buf), "(全開: 初期 %.2e → %.2e、後期 %.2e → %.2e ＝ %+.2f dB、直接 %.2e → %.2e)",
                  o1.early, o0.early, o1.late, o0.late, afti::dB(std::max(o0.late, 1e-30) / std::max(o1.late, 1e-30)), o1.direct, o0.direct);
    check("[壁越し] 全開の戸口を通る分は残る（後期が −1 dB 以内、直接は同じ）",
          o0.late > o1.late * 0.79 && o0.direct == o1.direct, buf);
    // GPU も同じ判定
    {
        acoustic::gpu::GpuTracer gt;
        if (!gt.init()) { std::printf("        GPU が使えないので GPU の突き合わせは飛ばします\n"); }
        else {
            World w;
            const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
            for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
            w.addBox(doorLeaf(0.0f, -0.5f, 1.0f, 3.0f, 0.06f, 0.0f), lm, true);   // 隙間 0（構造の確認。隙間があると本物の分がまれに通る）
            w.raysPerEmitter = 64; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
            w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            w.addEmitter(S, 0.2f);
            w.build(); w.update(dt);
            const TraceScene& sc = w.traceScene();
            TraceParams p; p.rays = 4096; p.maxBounces = 40; p.mixingSec = 0.03f; p.seed = 7u; p.wallReflect = 0;
            TraceParams p1 = p; p1.wallReflect = 1;
            EnergyTrace cpu;
            const TraceResult rc = cpu.run(sc, S, L, p), rc1 = cpu.run(sc, S, L, p1);
            TraceResult rg;
            const bool ok = gt.upload(sc) && gt.run(sc, S, L, p, rg);
            double lc = 0.0, lg = 0.0, l1 = 0.0, em = 0.0, ab = 0.0, rm = 0.0, es = 0.0;
            for (int b = 0; b < kNumBands; ++b) {
                lc += rc.late6[b] + rc.early6[b]; lg += rg.late6[b] + rg.early6[b]; l1 += rc1.late6[b] + rc1.early6[b];
                em += rg.emitted6[b]; ab += rg.absorbed6[b]; rm += rg.remainder6[b]; es += rg.escaped6[b];
            }
            const double cons = std::fabs(em - (ab + rm + es)) / std::max(em, 1e-30);
            std::snprintf(buf, sizeof(buf), "(閉扉・隙間 0: 旧 %.3e → 壁越しなし CPU %.3e / GPU %.3e。GPU の保存則のずれ %.1e)", l1, lc, lg, cons);
            check("[壁越し] GPU でも閉じた扉（隙間 0）の向こうの反射は 0 になり、保存則が立つ",
                  ok && lc == 0.0 && lg == 0.0 && l1 > 0.0 && cons < 1e-3, buf);
        }
    }
}

// ================================ [先着] 最初に届く音を重く（AF_ONLY=precedence）
//   発注者「一番最初に聞こえる音の重みを増やしたい。ゲームだから完全物理でなく聞こえ方がいい物を」。
//   出口だけの重み付けで、帳簿（component6）は物理のまま。直接・透過は変わらず、遅い到来ほど下がる。0 dB は今までと 1 ビットも同じ。
void testPrecedence() {
    std::puts("");
    std::puts("[先着] 最初に届く音を重く ── 遅い到来ほど出口で下げ、帳簿は物理のまま");
    char buf[256];
    const float dt = 1.0f / 60.0f;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const Vec3 S(0.0f, 1.6f, 3.0f);
    struct Got { double direct = 0.0, early = 0.0, diffract = 0.0, late = 0.0, thru = 0.0, ledger = 0.0, ledgerLate = 0.0; };
    auto run = [&](const Vec3& L, float db) {
        Got g;
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(90.0f), lm, true);
        w.precedenceDb = db; w.precedenceSec = 0.04f;
        w.raysPerEmitter = 512; w.budget.cfg.maxPerEmitter = 512; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        for (int f = 0; f < 60; ++f) w.update(dt);
        const Mix* mx = w.mix(e);
        for (int i = 0; i < mx->tapCount; ++i) {
            double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += mx->taps[i].e6[b];
            if (mx->taps[i].kind == TapKind::Direct) g.direct += te;
            else if (mx->taps[i].kind == TapKind::Early) g.early += te;
            else if (mx->taps[i].kind == TapKind::Diffract) g.diffract += te;
        }
        for (int i = 0; i < mx->sendCount; ++i)
            for (int b = 0; b < kNumBands; ++b) { g.late += mx->sends[i].e6[b]; g.thru += mx->sends[i].thru6[b]; }
        for (int c = 0; c < kNumComponents; ++c) for (int b = 0; b < kNumBands; ++b) g.ledger += mx->component6[c][b];
        for (int b = 0; b < kNumBands; ++b) g.ledgerLate += mx->component6[kLate][b];
        return g;
    };
    // 同じ部屋（見通しあり）: 直接は変わらず、初期は少し、後期は大きく下がる。帳簿は同じ
    {
        const Vec3 L(0.0f, 1.6f, 1.0f);
        const Got a = run(L, 0.0f), b = run(L, 12.0f);
        std::snprintf(buf, sizeof(buf), "(12 dB: 直接 %+.2f dB、初期 %+.2f dB、後期 %+.2f dB、帳簿 %.3e → %.3e)",
                      afti::dB(std::max(b.direct, 1e-30) / std::max(a.direct, 1e-30)), afti::dB(std::max(b.early, 1e-30) / std::max(a.early, 1e-30)),
                      afti::dB(std::max(b.late, 1e-30) / std::max(a.late, 1e-30)), a.ledger, b.ledger);
        check("[先着] 同じ部屋: 直接は変わらず、初期は少し、後期は大きく下がり、帳簿は 1 ビットも変わらない",
              b.direct == a.direct && b.early < a.early && b.early > a.early * 0.3 && b.late < a.late * 0.2 && b.ledger == a.ledger && b.ledgerLate == a.ledgerLate, buf);
    }
    // 隣の部屋（戸口の正面 3 m）: 戸口から直接の分（thru6）は部屋の響きより残る
    {
        const Vec3 L(0.0f, 1.6f, -3.0f);
        const Got a = run(L, 0.0f), b = run(L, 12.0f);
        const double roomA = a.late - a.thru, roomB = b.late - b.thru;
        std::snprintf(buf, sizeof(buf), "(12 dB: 直接 %+.2f dB、戸口から直接 %+.2f dB、部屋の響き %+.2f dB)",
                      afti::dB(std::max(b.direct, 1e-30) / std::max(a.direct, 1e-30)), afti::dB(std::max(b.thru, 1e-30) / std::max(a.thru, 1e-30)),
                      afti::dB(std::max(roomB, 1e-30) / std::max(roomA, 1e-30)));
        check("[先着] 隣の部屋: 戸口から直接の分は部屋の響きより残る（先に届く物が勝つ）",
              b.direct == a.direct && b.thru / a.thru > roomB / roomA && roomB < roomA * 0.2, buf);
    }
}

// ================================ [受取面] 面をレイの受取面にして、面ごとのタップで鳴らす（AF_ONLY=recvworld、2026-09-13）
//   World::earlyModel = 1。① 量が虚像の道（NEE の初期）と揃う ② タップの和が帳簿の初期と揃う（配分で解く）
//   ③ 壁際: 1 回目の当たりのタップが虚像の遅れと壁の向きに寄り、近いほど広い ④ 止まっていれば揺れない ⑤ GPU でも初期が同じ
void testReceiverWorld() {
    std::puts("");
    std::puts("[受取面] 面をレイの受取面にして、面ごとのタップで鳴らす（earlyModel = 1）");
    char buf[300];
    const float dt = 1.0f / 60.0f;
    const Vec3 S(1.5f, 1.6f, -1.0f);
    auto sumEarlyTaps = [](const Mix& m) {
        double s = 0.0;
        for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) for (int b = 0; b < kNumBands; ++b) s += m.taps[i].e6[b];
        return s;
    };
    auto ledgerEarly = [](const Mix& m) { double s = 0.0; for (int b = 0; b < kNumBands; ++b) s += m.component6[kEarly][b]; return s; };
    // ① ② ④
    {
        double early[2] = {0.0, 0.0};
        double tapSum = 0.0, ledger = 0.0, stillStep = 0.0;
        int faceTaps = 0; bool idsUnique = true, unitDirs = true;
        for (int model = 0; model < 2; ++model) {
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = model;
            w->raysPerEmitter = 2048; w->budget.cfg.maxPerEmitter = 2048; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            for (int k = 0; k < 60; ++k) w->update(dt);
            const Mix m60 = *w->mix(e);
            early[model] = ledgerEarly(m60);
            if (model == 1) {
                tapSum = sumEarlyTaps(m60); ledger = early[1];
                std::vector<int> ids;
                for (int i = 0; i < m60.tapCount; ++i) {
                    const MixTap& t = m60.taps[i];
                    if (t.kind != TapKind::Early) continue;
                    ++faceTaps;
                    for (int q : ids) if (q == t.id) idsUnique = false;
                    ids.push_back(t.id);
                    const float l = length(t.dirLocal);
                    if (l > 0.0f && std::fabs(l - 1.0f) > 1e-3f) unitDirs = false;
                }
                w->update(dt);
                const Mix& m61 = *w->mix(e);
                for (int i = 0; i < std::min(m60.tapCount, m61.tapCount); ++i)
                    for (int b = 0; b < kNumBands; ++b)
                        if (m60.taps[i].e6[b] > 0.0f) stillStep = std::max(stillStep, std::fabs(afti::dB(static_cast<double>(m61.taps[i].e6[b]) / m60.taps[i].e6[b])));
            }
            delete w;
        }
        const double dq = afti::dB(early[1] / std::max(early[0], 1e-30));
        std::snprintf(buf, sizeof buf, "(初期の帳簿: 虚像の道 %.4e → 受取面 %.4e ＝ %+.2f dB)", early[0], early[1], dq);
        check("[受取面] 初期の量は虚像の道（レイの NEE）と ±0.3 dB で揃う", std::fabs(dq) < 0.3, buf);
        std::snprintf(buf, sizeof buf, "(面のタップ %d 本、タップの和 %.4e 対 帳簿の初期 %.4e、素性の重なり %s、向き %s)",
                      faceTaps, tapSum, ledger, idsUnique ? "なし" : "あり", unitDirs ? "単位" : "単位でない物あり");
        check("[受取面] 初期のタップの和が帳簿の初期と 1% で揃い、素性は重ならず、向きは単位",
              faceTaps >= 6 && std::fabs(tapSum / std::max(ledger, 1e-30) - 1.0) < 0.01 && idsUnique && unitDirs, buf);
        std::snprintf(buf, sizeof buf, "(止まって 1 フレーム後のタップの量の最大の差 %.6f dB)", stillStep);
        check("[受取面] 止まっていれば次のフレームもタップの量が同じ（0.001 dB 未満）", stillStep < 0.001, buf);
    }
    // ③ 壁際: −x の壁（makeWorldBox の 3 番目の箱）の +x 面、1 回目の当たりのタップ
    {
        const int wantId = FaceTapSet::kIdBase + ((2 * 6 + 0) * 2 + 0);
        struct Got { bool found = false; float delay = 0.0f, spread = 0.0f, share = 0.0f; Vec3 dir{0, 0, 0}; };
        auto at = [&](float dw) {
            Got g;
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = 1;
            w->raysPerEmitter = 4096; w->budget.cfg.maxPerEmitter = 4096; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            const Vec3 L(-3.5f + dw, 1.2f, 0.0f);
            w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            for (int k = 0; k < 30; ++k) w->update(dt);
            const Mix& m = *w->mix(e);
            double tot = 0.0;
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) for (int b = 0; b < kNumBands; ++b) tot += m.taps[i].e6[b];
            for (int i = 0; i < m.tapCount; ++i)
                if (m.taps[i].id == wantId) {
                    g.found = true; g.delay = m.taps[i].delaySec; g.spread = m.taps[i].spread; g.dir = m.taps[i].dirLocal;
                    double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                    g.share = static_cast<float>(te / std::max(tot, 1e-30));
                }
            delete w;
            return g;
        };
        const Got nearG = at(0.1f), farG = at(1.0f);
        const Vec3 L(-3.4f, 1.2f, 0.0f), Simg(-7.0f - S.x, S.y, S.z);
        const float imgSec = length(Simg - L) / kSpeedOfSound;
        std::snprintf(buf, sizeof buf, "(0.1 m: 遅れ %.2f ms 対 虚像 %.2f ms、向き x %+.2f、広がり %.2f、取り分 %.1f%% ／ 1.0 m: 広がり %.2f、取り分 %.1f%%)",
                      nearG.delay * 1000.0f, imgSec * 1000.0f, nearG.dir.x, nearG.spread, nearG.share * 100.0f, farG.spread, farG.share * 100.0f);
        check("[受取面] 壁際: 1 回目の当たりのタップは虚像の遅れ（±1.5 ms）と壁の向き（左）に寄り、1 m より広い",
              nearG.found && farG.found && std::fabs(nearG.delay - imgSec) < 0.0015f && nearG.dir.x < -0.5f && nearG.spread > farG.spread, buf);
    }
    // ⑤ GPU
    {
        acoustic::gpu::GpuTracer probeGpu;
        if (!probeGpu.init()) { std::puts("        GPU が使えないので GPU の突き合わせは飛ばします"); return; }
        double early[2] = {0.0, 0.0};
        bool gpuOn = false;
        for (int g = 0; g < 2; ++g) {
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = 1; w->gpuTrace = g;
            w->raysPerEmitter = 2048; w->budget.cfg.maxPerEmitter = 2048; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            for (int k = 0; k < 5; ++k) w->update(dt);
            if (g == 1) gpuOn = w->gpuActive();
            early[g] = ledgerEarly(*w->mix(e));
            delete w;
        }
        std::snprintf(buf, sizeof buf, "(初期の帳簿: CPU %.6e ／ GPU %.6e、GPU %s)", early[0], early[1], gpuOn ? "入" : "切");
        check("[受取面] GPU の道でも初期の帳簿が CPU と同じ（受け取りは CPU で取る）", gpuOn && early[0] == early[1], buf);
    }
}

// ================================ [虚像の面] 虚像を面でつなぐ（AF_ONLY=imgface、2026-09-14）
//   World::earlyModel = 2。虚像の向き・遅れ・可視率はそのまま、量は虚像の面に届いたレイの受け取りから取る（receiver.h 3）。
//   ① 帳簿の初期は 1（壁の受取面）と 1 ビットも同じ。タップの和が帳簿と揃い、虚像の素性のタップがある
//   ② 壁際: 1 次の虚像のタップは遅れ・向きが虚像のまま、壁に近いほど取り分が増える（0 の虚像の道とも並べる）
//   ③ 止まっていれば揺れない ④ 音源ごとの上書き（初期反射の出し方・閉じ込め）が、その音源にだけ効く
void testImageFaces() {
    std::puts("");
    std::puts("[虚像の面] 虚像を面でつなぐ（earlyModel = 2）");
    char buf[360];
    const float dt = 1.0f / 60.0f;
    const Vec3 S(1.5f, 1.6f, -1.0f);
    auto isImageId = [](int id) { return id >= 16 && id < FaceTapSet::kIdDoor; };
    auto ledgerEarly = [](const Mix& m) { double s = 0.0; for (int b = 0; b < kNumBands; ++b) s += m.component6[kEarly][b]; return s; };
    // ① ③
    {
        double early[3] = {0.0, 0.0, 0.0};
        double tapSum = 0.0, imageSum = 0.0, stillStep = 0.0;
        int imageTaps = 0, faceTapsN = 0; bool idsUnique = true;
        for (int model = 1; model <= 2; ++model) {
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = model;
            w->raysPerEmitter = 2048; w->budget.cfg.maxPerEmitter = 2048; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            for (int k = 0; k < 60; ++k) w->update(dt);
            const Mix m60 = *w->mix(e);
            early[model] = ledgerEarly(m60);
            if (model == 2) {
                std::vector<int> ids;
                for (int i = 0; i < m60.tapCount; ++i) {
                    const MixTap& t = m60.taps[i];
                    if (t.kind != TapKind::Early) continue;
                    double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += t.e6[b];
                    tapSum += te;
                    if (isImageId(t.id)) { ++imageTaps; imageSum += te; } else ++faceTapsN;
                    for (int q : ids) if (q == t.id) idsUnique = false;
                    ids.push_back(t.id);
                }
                w->update(dt);
                const Mix& m61 = *w->mix(e);
                for (int i = 0; i < std::min(m60.tapCount, m61.tapCount); ++i)
                    for (int b = 0; b < kNumBands; ++b)
                        if (m60.taps[i].e6[b] > 0.0f) stillStep = std::max(stillStep, std::fabs(afti::dB(static_cast<double>(m61.taps[i].e6[b]) / m60.taps[i].e6[b])));
            }
            delete w;
        }
        std::snprintf(buf, sizeof buf, "(初期の帳簿: 壁の受取面 %.6e ／ 虚像を面でつなぐ %.6e、タップの和 %.4e、虚像のタップ %d 本（%.0f%%）・面のタップ %d 本、素性の重なり %s)",
                      early[1], early[2], tapSum, imageTaps, tapSum > 0 ? imageSum / tapSum * 100.0 : 0.0, faceTapsN, idsUnique ? "なし" : "あり");
        check("[虚像の面] 帳簿の初期は壁の受取面と 1 ビットも同じ、タップの和は帳簿と 1% で揃い、虚像の素性のタップがある",
              early[1] == early[2] && std::fabs(tapSum / std::max(early[2], 1e-30) - 1.0) < 0.01 && imageTaps >= 4 && idsUnique, buf);
        std::snprintf(buf, sizeof buf, "(止まって 1 フレーム後のタップの量の最大の差 %.6f dB)", stillStep);
        check("[虚像の面] 止まっていれば次のフレームもタップの量が同じ（0.001 dB 未満）", stillStep < 0.001, buf);
    }
    // ② 壁際: −x の壁（makeWorldBox の 3 番目の箱）の +x 面の 1 次の虚像
    {
        struct Got { bool found = false; float delay = 0.0f, spread = 0.0f, share = 0.0f; Vec3 dir{0, 0, 0}; };
        auto at = [&](int model, float dw) {
            Got g;
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = model;
            w->raysPerEmitter = 4096; w->budget.cfg.maxPerEmitter = 4096; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            const Vec3 L(-3.5f + dw, 1.2f, 0.0f);
            w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            int wantId = -1;
            for (std::size_t fi = 0; fi < w->faces().size(); ++fi)
                if (w->faces()[fi].box == 2 && w->faces()[fi].normal.x > 0.5f) wantId = 16 + static_cast<int>(fi) + 1;
            for (int k = 0; k < 30; ++k) w->update(dt);
            const Mix& m = *w->mix(e);
            double tot = 0.0;
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) for (int b = 0; b < kNumBands; ++b) tot += m.taps[i].e6[b];
            for (int i = 0; i < m.tapCount; ++i)
                if (m.taps[i].kind == TapKind::Early && m.taps[i].id == wantId) {
                    g.found = true; g.delay = m.taps[i].delaySec; g.spread = m.taps[i].spread; g.dir = m.taps[i].dirLocal;
                    double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                    g.share = static_cast<float>(te / std::max(tot, 1e-30));
                }
            delete w;
            return g;
        };
        const Got near2 = at(2, 0.1f), far2 = at(2, 1.0f), near0 = at(0, 0.1f), far0 = at(0, 1.0f);
        const Vec3 L(-3.4f, 1.2f, 0.0f), Simg(-7.0f - S.x, S.y, S.z);
        const float imgSec = length(Simg - L) / kSpeedOfSound;
        std::snprintf(buf, sizeof buf, "(0.1 m: 遅れ %.2f ms 対 虚像 %.2f ms、向き x %+.2f、広がり %.2f ／ 取り分 0.1 m・1.0 m: 虚像を面でつなぐ %.1f%%・%.1f%%、虚像の道 %.1f%%・%.1f%%)",
                      near2.delay * 1000.0f, imgSec * 1000.0f, near2.dir.x, near2.spread, near2.share * 100.0f, far2.share * 100.0f, near0.share * 100.0f, far0.share * 100.0f);
        check("[虚像の面] 壁際: 1 次の虚像のタップは虚像の遅れ（±0.3 ms）と向き（左）のまま、壁に近いほど取り分が大きい",
              near2.found && far2.found && std::fabs(near2.delay - imgSec) < 0.0003f && near2.dir.x < -0.9f && near2.share > far2.share, buf);
    }
    // ④ 音源ごとの上書き
    {
        World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
        w->earlyModel = 2;
        w->raysPerEmitter = 1024; w->budget.cfg.maxPerEmitter = 1024; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
        w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e0 = w->addEmitter(S, 0.2f), e1 = w->addEmitter(Vec3(1.0f, 1.6f, -1.5f), 0.2f);
        w->build();
        w->setEmitterEarlyModel(e0, 0);
        for (int k = 0; k < 20; ++k) w->update(dt);
        auto kinds = [&](int e, int& id4, int& img) {
            id4 = 0; img = 0;
            const Mix& m = *w->mix(e);
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) { if (m.taps[i].id == 4) ++id4; else if (isImageId(m.taps[i].id) && w->faceTaps(e)->count > 0 && w->emitterEarlyModel(e) == 2) ++img; }
        };
        int a4 = 0, aImg = 0, b4 = 0, bImg = 0;
        kinds(e0, a4, aImg); kinds(e1, b4, bImg);
        w->setEmitterEarlyModel(e0, -1);
        for (int k = 0; k < 20; ++k) w->update(dt);
        int c4 = 0, cImg = 0;
        kinds(e0, c4, cImg);
        delete w;
        std::snprintf(buf, sizeof buf, "(上書き 0 の音源: 方向なしの 1 本 %d・虚像を面でつないだタップ %d ／ 上書きなしの音源: %d・%d ／ 上書きを外した後: %d・%d)",
                      a4, aImg, b4, bImg, c4, cImg);
        check("[虚像の面] 初期反射の出し方の上書きはその音源にだけ効き、−1 で世界の設定に戻る",
              a4 == 1 && aImg == 0 && b4 == 0 && bImg >= 4 && c4 == 0 && cImg >= 4, buf);
    }
    {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(90.0f), lm, true);
        w.adjacentContain = 0.0f;
        w.raysPerEmitter = 1024; w.budget.cfg.maxPerEmitter = 1024; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(Vec3(0.0f, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e0 = w.addEmitter(Vec3(0.0f, 1.6f, 3.0f), 0.2f), e1 = w.addEmitter(Vec3(0.5f, 1.6f, 3.0f), 0.2f);
        w.build();
        w.setEmitterAdjacentContain(e0, 1.0f);
        for (int f = 0; f < 40; ++f) w.update(dt);
        auto doorShare = [&](int e) {
            const FaceTapSet* ft = w.faceTaps(e);
            double all = 0.0, door = 0.0;
            for (int i = 0; i < ft->count; ++i) { double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += ft->tap[i].e6[b]; all += te; if (ft->tap[i].id == FaceTapSet::kIdDoor) door += te; }
            return all > 0.0 ? door / all : 0.0;
        };
        const double d0 = doorShare(e0), d1 = doorShare(e1);
        std::snprintf(buf, sizeof buf, "(世界の閉じ込め 0 のまま、戸口の 1 本の割合: 上書き 1 の音源 %.0f%% ／ 上書きなしの音源 %.0f%%)", d0 * 100.0, d1 * 100.0);
        check("[虚像の面] 隣の部屋の閉じ込めの上書きはその音源にだけ効く", d0 > 0.2 && d1 == 0.0, buf);
    }
}

// ================================ [虚像の網] 虚像を結んだ網で見かけの音源の点を受ける（AF_ONLY=imglattice、2026-09-14）
//   World::earlyModel = 3（image_lattice.h・receiver.h 4）。
//   ① 網: 虚像の位置の点はその虚像へ可視率ぶん、重みの和は 1、点を 1 cm 動かしても重みが跳ばない
//   ② 帳簿の初期は壁の受取面と 1 ビットも同じ、タップの和が帳簿と揃い、虚像のタップがある。止まっていれば揺れない
//   ③ 壁際: 1 次の虚像のタップは遅れ・向きが虚像のまま、壁に近いほど取り分と広がりが大きい
void testImageLattice() {
    std::puts("");
    std::puts("[虚像の網] 虚像を結んだ網で見かけの音源の点を受ける（earlyModel = 3）");
    char buf[420];
    const float dt = 1.0f / 60.0f;
    const Vec3 S(1.5f, 1.6f, -1.0f);
    auto ledgerEarly = [](const Mix& m) { double s = 0.0; for (int b = 0; b < kNumBands; ++b) s += m.component6[kEarly][b]; return s; };
    auto isImageId = [](int id) { return id >= 16 && id < FaceTapSet::kIdNear; };
    // ①
    {
        World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
        w->earlyModel = 3;
        w->raysPerEmitter = 1024; w->budget.cfg.maxPerEmitter = 1024; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
        const Vec3 L(-1.0f, 1.2f, 1.5f);
        w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w->addEmitter(S, 0.2f);
        w->build();
        for (int k = 0; k < 3; ++k) w->update(dt);
        const ImageSet& im = *w->images(e);
        ImageLattice lat;
        lat.build(w->faces(), im, S, L);
        LatticeShare sh[ImageLattice::kMaxShares];
        // a) 虚像の位置の点
        double worstAt = 0.0; int nImg = 0;
        for (int i = 0; i < im.count; ++i) {
            const ImageSource& src = im.img[i];
            double sumV = 0.0;                                    // 同じ位置の虚像（2 次の 2 通り）の可視率の和
            for (int j = 0; j < im.count; ++j) if (length(im.img[j].pos - src.pos) < 1e-3f) sumV += im.img[j].validity;
            const int n = lat.shares(src.pos, sh);
            double got = 0.0;
            for (int q = 0; q < n; ++q)
                if (sh[q].image >= 0 && length(im.img[sh[q].image].pos - src.pos) < 1e-3f) got += sh[q].w;
            worstAt = std::max(worstAt, std::fabs(got - std::min(1.0, sumV)));
            ++nImg;
        }
        // b) 和が 1（部屋のまわり 24 m の立方体で 2000 点）
        double worstSum = 0.0;
        std::uint32_t seed = 12345u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return static_cast<float>((seed >> 8) & 0xFFFFFF) / 16777216.0f; };
        for (int k = 0; k < 2000; ++k) {
            const Vec3 P(-12.0f + 24.0f * rnd(), -12.0f + 24.0f * rnd(), -12.0f + 24.0f * rnd());
            const int n = lat.shares(P, sh);
            double s = 0.0; for (int q = 0; q < n; ++q) s += sh[q].w;
            worstSum = std::max(worstSum, std::fabs(s - 1.0));
        }
        // c) 連続: 斜めの線を 1 cm ずつ 20 m
        double worstStep = 0.0;
        std::vector<double> prev(static_cast<std::size_t>(im.count) + 2, 0.0), cur(prev.size(), 0.0);
        const Vec3 A(-10.0f, -4.0f, -9.0f), B(10.0f, 7.0f, 8.0f);
        const int steps = static_cast<int>(length(B - A) / 0.01f);
        for (int k = 0; k <= steps; ++k) {
            const Vec3 P = A + (B - A) * (static_cast<float>(k) / steps);
            std::fill(cur.begin(), cur.end(), 0.0);
            const int n = lat.shares(P, sh);
            for (int q = 0; q < n; ++q) cur[static_cast<std::size_t>(sh[q].image + 2)] += sh[q].w;
            if (k > 0) for (std::size_t q = 0; q < cur.size(); ++q) worstStep = std::max(worstStep, std::fabs(cur[q] - prev[q]));
            std::swap(cur, prev);
        }
        delete w;
        std::snprintf(buf, sizeof buf, "(軸 %d 本・点 %d、虚像 %d 本の位置で可視率との差 最大 %.5f、2000 点で和の差 最大 %.2e、1 cm ごとの重みの動き 最大 %.4f)",
                      lat.axisCount(), lat.pointCount(), nImg, worstAt, worstSum, worstStep);
        check("[虚像の網] 網: 虚像の位置の点はその虚像へ可視率ぶん（1e-3）、重みの和は 1（1e-4）、1 cm 動かしたときの重みの動きは 0.02 未満",
              nImg >= 6 && worstAt < 1e-3 && worstSum < 1e-4 && worstStep < 0.02, buf);
    }
    // ②
    {
        double early[4] = {0.0, 0.0, 0.0, 0.0};
        double tapSum = 0.0, imageSum = 0.0, nearSum = 0.0, stillStep = 0.0;
        int imageTaps = 0, otherTaps = 0; bool idsUnique = true;
        for (int model = 1; model <= 3; model += 2) {
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = model;
            w->raysPerEmitter = 2048; w->budget.cfg.maxPerEmitter = 2048; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            w->setListener(Vec3(-1.0f, 1.2f, 1.5f), Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            for (int k = 0; k < 60; ++k) w->update(dt);
            const Mix m60 = *w->mix(e);
            early[model] = ledgerEarly(m60);
            if (model == 3) {
                std::vector<int> ids;
                for (int i = 0; i < m60.tapCount; ++i) {
                    const MixTap& t = m60.taps[i];
                    if (t.kind != TapKind::Early) continue;
                    double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += t.e6[b];
                    tapSum += te;
                    if (isImageId(t.id)) { ++imageTaps; imageSum += te; } else { ++otherTaps; if (t.id == FaceTapSet::kIdNear) nearSum += te; }
                    for (int q : ids) if (q == t.id) idsUnique = false;
                    ids.push_back(t.id);
                }
                w->update(dt);
                const Mix& m61 = *w->mix(e);
                for (int i = 0; i < std::min(m60.tapCount, m61.tapCount); ++i)
                    for (int b = 0; b < kNumBands; ++b)
                        if (m60.taps[i].e6[b] > 0.0f) stillStep = std::max(stillStep, std::fabs(afti::dB(static_cast<double>(m61.taps[i].e6[b]) / m60.taps[i].e6[b])));
            }
            delete w;
        }
        std::snprintf(buf, sizeof buf, "(初期の帳簿: 壁の受取面 %.6e ／ 虚像の網 %.6e、タップの和 %.4e、虚像のタップ %d 本（%.0f%%）・音源の角 %.0f%%・ほか %d 本、素性の重なり %s)",
                      early[1], early[3], tapSum, imageTaps, tapSum > 0 ? imageSum / tapSum * 100.0 : 0.0, tapSum > 0 ? nearSum / tapSum * 100.0 : 0.0,
                      otherTaps, idsUnique ? "なし" : "あり");
        check("[虚像の網] 帳簿の初期は壁の受取面と 1 ビットも同じ、タップの和は帳簿と 1% で揃い、虚像のタップがある",
              early[1] == early[3] && std::fabs(tapSum / std::max(early[3], 1e-30) - 1.0) < 0.01 && imageTaps >= 4 && idsUnique, buf);
        std::snprintf(buf, sizeof buf, "(止まって 1 フレーム後のタップの量の最大の差 %.6f dB)", stillStep);
        check("[虚像の網] 止まっていれば次のフレームもタップの量が同じ（0.001 dB 未満）", stillStep < 0.001, buf);
    }
    // ③ 壁際: −x の壁（makeWorldBox の 3 番目の箱）の +x 面の 1 次の虚像
    {
        struct Got { bool found = false; float delay = 0.0f, spread = 0.0f, share = 0.0f; Vec3 dir{0, 0, 0}; };
        auto at = [&](int model, float dw) {
            Got g;
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = model;
            w->raysPerEmitter = 4096; w->budget.cfg.maxPerEmitter = 4096; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            const Vec3 L(-3.5f + dw, 1.2f, 0.0f);
            w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            int wantId = -1;
            for (std::size_t fi = 0; fi < w->faces().size(); ++fi)
                if (w->faces()[fi].box == 2 && w->faces()[fi].normal.x > 0.5f) wantId = 16 + static_cast<int>(fi) + 1;
            for (int k = 0; k < 30; ++k) w->update(dt);
            const Mix& m = *w->mix(e);
            double tot = 0.0;
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) for (int b = 0; b < kNumBands; ++b) tot += m.taps[i].e6[b];
            for (int i = 0; i < m.tapCount; ++i)
                if (m.taps[i].kind == TapKind::Early && m.taps[i].id == wantId) {
                    g.found = true; g.delay = m.taps[i].delaySec; g.spread = m.taps[i].spread; g.dir = m.taps[i].dirLocal;
                    double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                    g.share = static_cast<float>(te / std::max(tot, 1e-30));
                }
            delete w;
            return g;
        };
        const Got near3 = at(3, 0.1f), far3 = at(3, 1.0f), near2 = at(2, 0.1f), far2 = at(2, 1.0f);
        const Vec3 L(-3.4f, 1.2f, 0.0f), Simg(-7.0f - S.x, S.y, S.z);
        const float imgSec = length(Simg - L) / kSpeedOfSound;
        std::snprintf(buf, sizeof buf, "(0.1 m: 遅れ %.2f ms 対 虚像 %.2f ms、向き x %+.2f ／ 取り分・広がり 0.1 m・1.0 m: 虚像の網 %.1f%%・%.2f、%.1f%%・%.2f ／ 虚像を面でつなぐ %.1f%%・%.2f、%.1f%%・%.2f)",
                      near3.delay * 1000.0f, imgSec * 1000.0f, near3.dir.x, near3.share * 100.0f, near3.spread, far3.share * 100.0f, far3.spread,
                      near2.share * 100.0f, near2.spread, far2.share * 100.0f, far2.spread);
        check("[虚像の網] 壁際: 1 次の虚像のタップは虚像の遅れ（±0.3 ms）と向き（左）のまま、壁に近いほど取り分が大きい",
              near3.found && far3.found && std::fabs(near3.delay - imgSec) < 0.0003f && near3.dir.x < -0.9f && near3.share > far3.share, buf);
    }
}

// ================================ [閉じ込め] 隣の部屋の残響・反射を今いる部屋で響かせない（AF_ONLY=contain、2026-09-14）
//   World::adjacentContain。音源が耳と別の部屋にいるとき、耳の部屋で響かせる分を戸口へ移す（総量は変えない）。
//   ① 後期: 総量は同じまま、耳の部屋の FDN への送り（直接の送り＋戸口から流す分）が 0 になる
//   ② 初期（受取面）: 総量は同じまま、戸口の 1 本ができ、耳の部屋の面のタップが消える
//   ③ 音源が耳と同じ部屋なら何も変わらない（1 ビットも同じ）
void testAdjacentContain() {
    std::puts("");
    std::puts("[閉じ込め] 隣の部屋の残響・反射を今いる部屋で響かせない（adjacentContain）");
    char buf[320];
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    struct Got { double lateSum = 0.0, toListener = 0.0, earlySum = 0.0, doorTap = 0.0, listenerFaces = 0.0; Mix mix; int lroom = -1; };
    auto run = [&](float contain, const Vec3& L, const Vec3& S) {
        Got g;
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(90.0f), lm, true);
        w.adjacentContain = contain;
        w.raysPerEmitter = 1024; w.budget.cfg.maxPerEmitter = 1024; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        for (int f = 0; f < 60; ++f) w.update(1.0f / 60.0f);
        g.mix = *w.mix(e);
        g.lroom = w.roomAt(L);
        for (int i = 0; i < g.mix.sendCount; ++i) {
            const FdnSend& sd = g.mix.sends[i];
            for (int b = 0; b < kNumBands; ++b) {
                g.lateSum += sd.e6[b];
                if (sd.room == g.lroom) g.toListener += sd.e6[b];                 // 耳の部屋の FDN への直接の送り
                else g.toListener += std::max(0.0f, sd.e6[b] - sd.thru6[b]);      // 戸口の線音源から耳の部屋へ流す分
            }
        }
        const FaceTapSet* ft = w.faceTaps(e);
        for (int i = 0; i < ft->count; ++i) {
            double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += ft->tap[i].e6[b];
            g.earlySum += te;
            if (ft->tap[i].id == FaceTapSet::kIdDoor) g.doorTap += te;
        }
        // 耳の部屋の面のタップ: 箱が −z 側の壁（4）か、仕切りの −z 面（面 5 ＝ z の − 側）
        for (int i = 0; i < ft->count; ++i) {
            const FaceTap& t = ft->tap[i];
            if (t.id == FaceTapSet::kIdDoor) continue;
            if (t.box == 4 || ((t.box == 6 || t.box == 7) && t.face == 5)) { double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += t.e6[b]; g.listenerFaces += te; }
        }
        return g;
    };
    const Vec3 Lapart(0.0f, 1.6f, -3.0f), Sapart(0.0f, 1.6f, 3.0f);
    const Got a0 = run(0.0f, Lapart, Sapart), a1 = run(1.0f, Lapart, Sapart);
    std::snprintf(buf, sizeof buf, "(後期の総量 %.4e → %.4e、耳の部屋で響く分 %.3e（%.0f%%）→ %.3e)",
                  a0.lateSum, a1.lateSum, a0.toListener, a0.lateSum > 0 ? a0.toListener / a0.lateSum * 100.0 : 0.0, a1.toListener);
    check("[閉じ込め] 後期: 総量は同じまま（1e-4）、耳の部屋で響く分が 0 になる",
          std::fabs(a1.lateSum / std::max(a0.lateSum, 1e-30) - 1.0) < 1e-4 && a0.toListener > 0.0 && a1.toListener <= a0.toListener * 1e-6, buf);
    std::snprintf(buf, sizeof buf, "(初期の総量 %.4e → %.4e、戸口の 1 本 %.3e → %.3e（%.0f%%）、耳の部屋の面のタップ %.3e → %.3e)",
                  a0.earlySum, a1.earlySum, a0.doorTap, a1.doorTap, a1.earlySum > 0 ? a1.doorTap / a1.earlySum * 100.0 : 0.0, a0.listenerFaces, a1.listenerFaces);
    check("[閉じ込め] 初期: 総量は同じまま（1%）、戸口の 1 本ができ、耳の部屋の −z の壁と仕切りの面のタップが消える",
          std::fabs(a1.earlySum / std::max(a0.earlySum, 1e-30) - 1.0) < 0.01 && a0.doorTap == 0.0 && a1.doorTap > 0.0 && a1.listenerFaces == 0.0, buf);
    // 同じ部屋
    const Vec3 Lsame(1.0f, 1.6f, 1.5f);
    const Got s0 = run(0.0f, Lsame, Sapart), s1 = run(1.0f, Lsame, Sapart);
    bool same = (s0.mix.tapCount == s1.mix.tapCount) && (s0.mix.sendCount == s1.mix.sendCount);
    for (int i = 0; same && i < s0.mix.tapCount; ++i)
        for (int b = 0; b < kNumBands; ++b) if (s0.mix.taps[i].e6[b] != s1.mix.taps[i].e6[b]) same = false;
    for (int i = 0; same && i < s0.mix.sendCount; ++i)
        for (int b = 0; b < kNumBands; ++b) if (s0.mix.sends[i].e6[b] != s1.mix.sends[i].e6[b]) same = false;
    std::snprintf(buf, sizeof buf, "(タップ %d / %d 本、送り %d / %d 本)", s0.mix.tapCount, s1.mix.tapCount, s0.mix.sendCount, s1.mix.sendCount);
    check("[閉じ込め] 音源が耳と同じ部屋なら何も変わらない（1 ビットも同じ）", same, buf);
}

// ================================ [GPU] 計算デバイスの管が通るか（AF_ONLY=gpu）
void testGpuPipe() {
    std::printf("\n[GPU] エンジン自前の計算デバイス ── 管が通るか\n");
    char buf[256];
    acoustic::gpu::ComputeDevice dev;
    if (!dev.available()) {
        std::printf("      この機械では GPU の道が使えません（%s）。CPU のまま進みます。\n", dev.error().c_str());
        check("[GPU] デバイスが無くても検査は続く（CPU へ落ちる道がある）", true, "(available() = false)");
        return;
    }
    std::printf("      アダプタ: %s\n", dev.adapterName().c_str());

    // ★まず「管」だけを確かめる。音の計算を載せる前に、
    //   デバイス作成 → シェーダ翻訳 → 入力転送 → 実行 → 読み戻し が通ることを見る。
    //   ここが通らないうちにレイを載せると、間違いが幾何の側か配線の側か切り分けられない。
    static const char* kHlsl =
        "StructuredBuffer<float> gIn : register(t0);\n"
        "RWStructuredBuffer<float> gOut : register(u0);\n"
        "cbuffer Cb : register(b0) { uint gCount; float gScale; uint2 gPad; };\n"
        "[numthreads(64,1,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    if (id.x >= gCount) return;\n"
        "    gOut[id.x] = gIn[id.x] * gScale + (float)id.x;\n"
        "}\n";
    const bool okShader = dev.setShader(kHlsl, "main");
    std::snprintf(buf, sizeof(buf), "(%s)", okShader ? "翻訳できた" : dev.error().c_str());
    check("[GPU] HLSL を積める（DLL の中に文字列で持つ。ファイルを配らない）", okShader, buf);
    if (!okShader) return;

    const int n = 1000;
    std::vector<float> in(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.5f;
    struct Cb { unsigned int count; float scale; unsigned int pad[2]; } cb{static_cast<unsigned int>(n), 3.0f, {0, 0}};
    const bool okIn = dev.setInput(0, in.data(), in.size() * sizeof(float), sizeof(float));
    const bool okCb = dev.setConstants(&cb, sizeof(cb));
    const bool okOut = dev.setOutput(in.size() * sizeof(float), sizeof(float));
    check("[GPU] 入力・定数・出力のバッファを置ける", okIn && okCb && okOut, okIn && okCb && okOut ? "" : dev.error().c_str());
    if (!(okIn && okCb && okOut)) return;

    const int groups = (n + 63) / 64;      // ★シェーダの numthreads(64) と掛け算で n を超えるように
    const bool okRun = dev.dispatch(groups);
    std::vector<float> out(static_cast<std::size_t>(n), -1.0f);
    const bool okRead = dev.readOutput(out.data(), out.size() * sizeof(float));
    check("[GPU] 流して読み戻せる（staging を挟む）", okRun && okRead, okRun && okRead ? "" : dev.error().c_str());
    if (!(okRun && okRead)) return;

    double worst = 0.0; int bad = 0;
    for (int i = 0; i < n; ++i) {
        const double want = static_cast<double>(in[static_cast<std::size_t>(i)]) * 3.0 + i;
        const double got = out[static_cast<std::size_t>(i)];
        const double e = std::fabs(got - want);
        if (e > 1e-4) ++bad;
        worst = std::max(worst, e);
    }
    std::snprintf(buf, sizeof(buf), "(%d 要素、外れ %d 個、最大のずれ %.2e)", n, bad, worst);
    check("[GPU] 答えが CPU の期待と合う（in*3 + 番号）", bad == 0, buf);

    // ── レイ本体を GPU で解いて CPU と突き合わせる ──
    //   ★ビット一致はしない（丸めも sin/cos も違う）。見るのは 3 つ:
    //     ①保存則が GPU 側でも立つか ②当たりの数が近いか ③初期・後期が dB で近いか
    {
        acoustic::gpu::GpuTracer gt;
        if (!gt.init()) {
            std::printf("      レイの GPU 版は積めませんでした（%s）\n", gt.error().c_str());
            check("[GPU] シェーダが積めなくても CPU で進める", true, "(init 失敗。CPU のまま)");
        } else {
            AcousticMaterial wall = AcousticMaterial::defaultWall();
            for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.scattering[b] = 0.5f; wall.transmission[b] = 0.01f; }
            MaterialTable mats; const int matId = mats.add(wall);
            Surfaces box = closedBox(3.5f, 3.0f, matId);
            box.rebuildBvh();
            TraceScene sc; buildTraceScene(box, mats, sc);
            const Vec3 S(1.5f, 1.6f, -1.0f), L(-1.0f, 1.2f, 1.5f);
            TraceParams prm{512, 20, 0.03f, 7u, 1, 0};

            EnergyTrace cpu;
            const TraceResult rc = cpu.run(sc, S, L, prm);
            TraceResult rg;      // 反射だけ（直接は CPU が出す）
            const bool okUp = gt.upload(sc);
            const bool okRun = okUp && gt.run(sc, S, L, prm, rg);
            check("[GPU] 場面を送って流せる", okRun, okRun ? "" : gt.error().c_str());
            if (okRun) {
                // ① 保存則（GPU の帳簿だけで）
                double em = 0.0, ab = 0.0, rm = 0.0, es = 0.0;
                for (int b = 0; b < kNumBands; ++b) { em += rg.emitted6[b]; ab += rg.absorbed6[b]; rm += rg.remainder6[b]; es += rg.escaped6[b]; }
                const double consErr = std::fabs(em - (ab + rm + es)) / std::max(em, 1e-30);
                std::snprintf(buf, sizeof(buf), "(放射 %.4f = 吸収 %.4f + 残り %.4f + 逃げ %.4f、ずれ %.2e)", em, ab, rm, es, consErr);
                check("[GPU] 保存則が GPU 側でも立つ", consErr < 1e-3, buf);
                // ② 当たりの数
                const double hitRatio = static_cast<double>(rg.hits) / std::max(1, rc.hits);
                std::snprintf(buf, sizeof(buf), "(CPU %d / GPU %d ＝ %.4f 倍、レイ CPU %d / GPU %d)",
                              rc.hits, rg.hits, hitRatio, rc.raysTraced, rg.raysTraced);
                check("[GPU] 当たりの数が CPU と 2% 以内", std::fabs(hitRatio - 1.0) < 0.02, buf);
                // ③ 初期・後期
                double ce = 0.0, ge = 0.0, cl = 0.0, gl = 0.0;
                for (int b = 0; b < kNumBands; ++b) { ce += rc.early6[b]; ge += rg.early6[b]; cl += rc.late6[b]; gl += rg.late6[b]; }
                const double dE = afti::dB(std::max(ge, 1e-30) / std::max(ce, 1e-30));
                const double dL2 = afti::dB(std::max(gl, 1e-30) / std::max(cl, 1e-30));
                std::snprintf(buf, sizeof(buf), "(初期 CPU %.3e / GPU %.3e ＝ %+.2f dB、後期 %.3e / %.3e ＝ %+.2f dB)",
                              ce, ge, dE, cl, gl, dL2);
                check("[GPU] 初期・後期が CPU と ±1 dB 以内", std::fabs(dE) < 1.0 && std::fabs(dL2) < 1.0, buf);
                // ④ 何度流しても同じ（静止＝毎フレーム同じ、の GPU 側）
                TraceResult rg2;
                gt.run(sc, S, L, prm, rg2);
                bool same = (rg2.hits == rg.hits);
                for (int b = 0; b < kNumBands && same; ++b) same = (rg2.early6[b] == rg.early6[b] && rg2.late6[b] == rg.late6[b]);
                std::snprintf(buf, sizeof(buf), "(2 回目の当たり %d)", rg2.hits);
                check("[GPU] 同じ入力なら何度流しても同じ答え（静止していれば揺れない）", same, buf);
                // ⑤ 束ねても 1 本ずつと同じ答えか（段 2-e）
                //   ★ここが今回いちばん危ない所。音源ごとに本数が違うと、出力の区切り（rayBase）と
                //     組の切り上げ（blockFirst）がずれて、隣の音源の枠へ書き込む。
                //     症状は「特定の音源だけ音が化ける」で、聞いても原因が分からない。
                //     だから**本数をわざと 64 の倍数から外して**混ぜ、1 本ずつと突き合わせる。
                {
                    const int kN = 5;
                    const int rays[kN] = {100, 512, 37, 1000, 256};      // 64 の倍数はわざと 2 つだけ
                    const int grp[kN]  = {1, 4, 3, 4, 1};                // 群も混ぜる（詰め方の検査）
                    const int gsel[kN] = {0, 2, 1, 0, 0};
                    const Vec3 pos[kN] = {Vec3(1.5f, 1.6f, -1.0f), Vec3(-1.2f, 1.0f, 0.8f), Vec3(0.3f, 2.2f, -2.0f),
                                          Vec3(2.0f, 0.6f, 2.0f), Vec3(-2.5f, 1.8f, -0.4f)};
                    TraceResult one[kN], many[kN];
                    acoustic::gpu::BatchJob jb[kN];
                    for (int j = 0; j < kN; ++j) {
                        TraceParams pj{rays[j], 20, 0.03f, static_cast<std::uint32_t>(11 + j * 7), grp[j], gsel[j]};
                        gt.run(sc, pos[j], L, pj, one[j]);
                        jb[j].source = pos[j]; jb[j].prm = pj; jb[j].out = &many[j];
                    }
                    const bool okB = gt.runBatch(sc, L, jb, kN);
                    int badj = -1; double worstD = 0.0;
                    for (int j = 0; j < kN && okB; ++j) {
                        bool same = (one[j].hits == many[j].hits && one[j].raysTraced == many[j].raysTraced);
                        for (int b = 0; b < kNumBands; ++b) {
                            if (one[j].early6[b] != many[j].early6[b] || one[j].late6[b] != many[j].late6[b]) same = false;
                            worstD = std::max(worstD, std::fabs(static_cast<double>(one[j].early6[b] - many[j].early6[b])));
                        }
                        if (!same && badj < 0) badj = j;
                    }
                    std::snprintf(buf, sizeof(buf), "(%d 音源を 1 回で。合わない音源 %d、初期の最大のずれ %.2e)",
                                  kN, badj, worstD);
                    check("[GPU] 束ねても 1 本ずつと同じ答え（本数がばらばらでも枠を越えない）", okB && badj < 0, buf);
                }
                // ── 速さ（束ね）。同じ本数を「1 本ずつ」と「1 回で」並べる ──
                std::printf("      速さ（%d 音源ぶん。転送と読み戻し込み）:\n", 17);
                std::printf("        %8s | %12s %12s | %s\n", "レイ/音源", "1 音源ずつ", "1 回で", "倍率");
                for (int nr : {512, 2048, 8192}) {
                    const int kM = 17;
                    std::vector<Vec3> ps(static_cast<std::size_t>(kM));
                    std::vector<TraceResult> rs(static_cast<std::size_t>(kM));
                    std::vector<acoustic::gpu::BatchJob> js(static_cast<std::size_t>(kM));
                    for (int j = 0; j < kM; ++j) {
                        const float a = static_cast<float>(j) * 0.37f;
                        ps[static_cast<std::size_t>(j)] = Vec3(2.0f * std::cos(a), 1.0f + 0.1f * j, 2.0f * std::sin(a));
                        TraceParams pj{nr, 40, 0.03f, static_cast<std::uint32_t>(3 + j * 13), 1, 0};
                        js[static_cast<std::size_t>(j)].source = ps[static_cast<std::size_t>(j)];
                        js[static_cast<std::size_t>(j)].prm = pj;
                        js[static_cast<std::size_t>(j)].out = &rs[static_cast<std::size_t>(j)];
                    }
                    gt.runBatch(sc, L, js.data(), kM);                  // 温める
                    const auto s0 = std::chrono::steady_clock::now();
                    for (int q = 0; q < 3; ++q)
                        for (int j = 0; j < kM; ++j) { TraceResult t2; gt.run(sc, ps[static_cast<std::size_t>(j)], L, js[static_cast<std::size_t>(j)].prm, t2); }
                    const double msOne = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count() / 3.0;
                    const auto b0 = std::chrono::steady_clock::now();
                    for (int q = 0; q < 3; ++q) {
                        std::vector<TraceResult> tr(static_cast<std::size_t>(kM));
                        for (int j = 0; j < kM; ++j) js[static_cast<std::size_t>(j)].out = &tr[static_cast<std::size_t>(j)];
                        gt.runBatch(sc, L, js.data(), kM);
                    }
                    const double msBat = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count() / 3.0;
                    std::printf("        %8d | %10.3f ms %10.3f ms | %.1f 倍\n", nr, msOne, msBat, msOne / std::max(msBat, 1e-6));
                }
                // ── 速さ（本題）。本数と跳ね返りを振って CPU と並べる ──
                std::printf("      速さ（1 音源。GPU は転送と読み戻し込み）:\n");
                std::printf("        %8s %6s | %10s %10s | %s\n", "レイ", "跳ね", "CPU", "GPU", "倍率");
                for (int nr : {512, 2048, 8192, 32768}) {
                    TraceParams pp{nr, 40, 0.03f, 7u, 1, 0};
                    const auto c0 = std::chrono::steady_clock::now();
                    for (int q = 0; q < 3; ++q) { volatile auto r = cpu.run(sc, S, L, pp); (void)r; }
                    const double msC = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count() / 3.0;
                    TraceResult tmp;
                    gt.run(sc, S, L, pp, tmp);                       // 温める
                    const auto g0 = std::chrono::steady_clock::now();
                    for (int q = 0; q < 3; ++q) { TraceResult t2; gt.run(sc, S, L, pp, t2); }
                    const double msG = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() / 3.0;
                    std::printf("        %8d %6d | %8.3f ms %8.3f ms | %.1f 倍\n", nr, 40, msC, msG, msC / std::max(msG, 1e-6));
                }
            }
        }
    }
}

/// 【探り】壁に近づいたときの初期反射（AF_ONLY=nearwall）
///   面音源化の狙いのうち **「壁が近いと虚像の密度が上がり、到達が近く早くなる ＝ 奥行き感」**
///   が今の ISM で出ているかを測る。
///   出す物: 壁までの距離ごとに、直接・初期・後期の量、初期/直接の比、
///           最初の虚像の到達（ITDG）、虚像の数、1 次の虚像の最短到達。
void testNearWall() {
    std::printf("\n[探り] 壁に近づく ── 初期反射の量と到達（奥行き感の材料）\n");
    std::printf("        （%s を壁へ寄せる）\n", (std::getenv("AF_NEAR") != nullptr) ? "音源" : "耳");
    std::printf("        %7s | %9s %9s %9s | %8s | %8s %6s %8s\n",
                "壁まで", "直接", "初期", "後期", "初期/直接", "ITDG", "虚像", "最短虚像");
    const float half = 3.5f, h = 3.0f;
    for (int k = 0; k < 7; ++k) {
        const float d[7] = {3.5f, 2.5f, 1.5f, 1.0f, 0.5f, 0.25f, 0.1f};
        const float wallDist = d[k];
        World* w = makeWorldBox(half, h, 0.2f);
        w->raysPerEmitter = 512; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
        if (const char* em = std::getenv("AF_EARLY_MODEL")) w->earlyModel = std::atoi(em);   // 初期反射 0 虚像 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網
        { const char* io2 = std::getenv("AF_IMG_ORDER"); if (io2) w->imageOrder = std::atoi(io2); }
        // AF_NEAR=src なら**音源**を壁へ寄せる（耳は固定）。既定は耳を寄せる。
        //   ★虚像は「音源」の鏡映なので、音源が壁に近いほど虚像が音源のそばに集まる。
        //     耳を寄せた場合に近づくのは「その壁の 1 本」だけで、群としては集まらない。
        //     どちらの向きで狙いが出るかを分けて測る。
        const bool nearSrc = (std::getenv("AF_NEAR") != nullptr);
        const Vec3 L = nearSrc ? Vec3(1.5f, 1.2f, 0.0f) : Vec3(-half + wallDist, 1.2f, 0.0f);
        const Vec3 S = nearSrc ? Vec3(-half + wallDist, 1.6f, -1.0f) : Vec3(1.5f, 1.6f, -1.0f);
        w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w->addEmitter(S, 0.2f);
        w->build();
        const float dt = 1.0f / 60.0f;
        for (int q = 0; q < 30; ++q) w->update(dt);      // 追従を落ち着かせる
        const Mix* mx = w->mix(e);
        const ImageSet* im = w->images(e);
        double dir = 0.0, early = 0.0, late = 0.0;
        for (int b = 0; b < kNumBands; ++b) {
            dir += mx->component6[kDirect][b];
            early += mx->component6[kEarly][b];
            late += mx->component6[kLate][b];
        }
        // 1 次の虚像の最短到達（＝いちばん近い壁の反射が届く時刻）
        float first1 = -1.0f;
        for (int i = 0; i < im->count; ++i)
            if (im->img[i].order == 1 && (first1 < 0.0f || im->img[i].pathSec < first1)) first1 = im->img[i].pathSec;
        // 直接の到達は Mix のタップから拾う
        float directSec = 0.0f;
        for (int i = 0; i < mx->tapCount; ++i) if (mx->taps[i].kind == TapKind::Direct) { directSec = mx->taps[i].delaySec; break; }
        // ★集中度合い: 虚像の到達時刻を重み付きで見る。
        //   重心 = Σw·t / Σw、広がり = その標準偏差、詰まり = 最初の 5 ms に入る取り分。
        //   壁が近いほど「重心が前へ寄り、広がりが縮み、5 ms に入る取り分が増える」のが狙いの形。
        double sw = 0.0, swt = 0.0, swt2 = 0.0, w5ms = 0.0;
        for (int i = 0; i < im->count; ++i) {
            double wi = 0.0; for (int b = 0; b < kNumBands; ++b) wi += im->img[i].weight6[b];
            const double ti = (im->img[i].pathSec - directSec) * 1000.0;
            sw += wi; swt += wi * ti; swt2 += wi * ti * ti;
            if (ti <= 5.0) w5ms += wi;
        }
        const double cog = (sw > 0.0) ? swt / sw : 0.0;
        const double var = (sw > 0.0) ? std::max(0.0, swt2 / sw - cog * cog) : 0.0;
        const double spread = std::sqrt(var);
        const double frac5 = (sw > 0.0) ? w5ms / sw : 0.0;
        // いちばん早い虚像の取り分（正規化重み、帯域幅平均でなく 500 Hz 帯）と、妥当性
        float wFirst = 0.0f, vFirst = 0.0f;
        for (int i = 0; i < im->count; ++i)
            if (im->img[i].order == 1 && std::fabs(im->img[i].pathSec - first1) < 1e-6f) { wFirst = im->img[i].weight6[2]; vFirst = im->img[i].validity; }
        std::printf("        %7.2f | %9.2f %9.2f %9.2f | %8.2f | %6.1fms %6d %6.1fms  最早 %+6.2f dB 取り分 %.3f | 重心 %5.1fms 広がり %5.1fms 5ms内 %5.1f%%\n",
                    wallDist, afti::dB(dir), afti::dB(early), afti::dB(late), afti::dB(early / std::max(dir, 1e-30)),
                    (mx->onsetSec - directSec) * 1000.0f, im->count,
                    (first1 > 0.0f) ? (first1 - directSec) * 1000.0f : -1.0f,
                    afti::dB(early * wFirst / std::max(dir, 1e-30)), wFirst, cog, spread, frac5 * 100.0);
        (void)vFirst;
        delete w;
    }
    std::printf("        ★「最早 dB（直接比）」＝ いちばん早い虚像 1 本が直接音に対してどれだけ鳴っているか。\n");
    std::printf("          壁に近いほどこれが上がり、ITDG が短くなるのが狙いの形。\n");
}

/// 【探り】音源を増やして扉を動かす（AF_ONLY=manysrc）
///   試聴の「17 音源で扉を動かすと、開いてる途中や止めたときに音が遅くなってドロップアウトする」を数字にする。
///   出す物: 段の数、音源ごとの「最後に解いてから何フレーム経ったか」、レイの本数、
///           1 フレームで全群を引き直した回数（費用の山）。
void testManySources() {
    const char* ns = std::getenv("AF_SRC_N");
    const int N = ns ? std::atoi(ns) : 17;
    std::printf("\n[探り] 音源 %d 本で扉を動かす ── 段の巡りと答えの古さ\n", N);
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    World w;
    const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
    const int leaf = w.addBox(doorLeaf(0.0f, -0.5f, 1.0f, 3.0f, 0.06f, 0.004f), lm, true);
    // ★AF_RAYS は「1 音源の本数」を直に決める（上限も一緒に上げる）。
    //   AF_MAX_PER だけを上げても、予算の元になる raysPerEmitter が 256 のままなので増えない。
    { const char* r = std::getenv("AF_RAYS"); const int nr = r ? std::atoi(r) : 256;
      w.raysPerEmitter = nr; w.budget.cfg.maxPerEmitter = std::max(nr, w.budget.cfg.maxPerEmitter); }
    if (const char* wr = std::getenv("AF_WALL_REFLECT")) w.wallReflect = std::atoi(wr);   // 壁越しの反射 0/1
    if (const char* em = std::getenv("AF_EARLY_MODEL")) w.earlyModel = std::atoi(em);   // 初期反射 0 虚像 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網
    { const char* g = std::getenv("AF_GROUPS"); w.rayGroups = g ? std::atoi(g) : 4; }   // Unity と同じ既定 4
    { const char* tr = std::getenv("AF_TOTAL_RAYS"); if (tr) w.budget.cfg.totalRays = std::atoi(tr); }
    { const char* gp = std::getenv("AF_GPU"); if (gp) w.gpuTrace = std::atoi(gp); }
    { const char* mp = std::getenv("AF_MAX_PER"); if (mp) w.budget.cfg.maxPerEmitter = std::atoi(mp); }
    w.setListener(Vec3(0, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    // 奥の部屋に音源を並べる（戸口の向こう。扉の効きが全部に乗る配置）。
    const char* sr = std::getenv("AF_SRC_R");
    const float srcR = sr ? static_cast<float>(std::atof(sr)) : 0.2f;
    std::vector<int> ids;
    for (int i = 0; i < N; ++i) {
        const float t = (N > 1) ? static_cast<float>(i) / (N - 1) : 0.5f;
        ids.push_back(w.addEmitter(Vec3(-5.0f + 10.0f * t, 1.6f, 1.0f + 4.0f * ((i % 3) * 0.5f)), srcR));
    }
    w.build();
    const float dt = 1.0f / 60.0f;
    // 最後に「解かれた」フレーム（rays > 0 だったフレーム）を音源ごとに覚える。
    std::vector<int> lastSolved(static_cast<std::size_t>(N), -1);
    std::vector<int> worstStale(static_cast<std::size_t>(N), 0);
    double msSum = 0.0, msMax = 0.0;
    std::vector<int> prevRays(static_cast<std::size_t>(N), -1);
    std::vector<double> msAll;
    std::vector<float> prevVis(static_cast<std::size_t>(N), -1.0f);
    std::vector<double> prevTot(static_cast<std::size_t>(N), -1.0);
    int visJump = 0, visJumpLight = 0, worstStepAt = -1, worstStepTier = -1; double worstStep = 0.0;
    std::vector<int> prevTier(static_cast<std::size_t>(N), -1);
    int tierChanges = 0; double stepOnChange = 0.0, stepOnSame = 0.0;
    std::vector<double> prevComp(static_cast<std::size_t>(N) * kNumComponents, 0.0);
    double worstComp[kNumComponents] = {}, worstCompPrev[kNumComponents] = {};
    float worstVis = 0.0f, worstVisPrev = 0.0f; int worstRays = 0, worstImg = 0, worstWho = -1;
    int reTraceTotal = 0, reTraceMax = 0;
    std::printf("        %6s %6s | %5s %5s %5s | %8s %8s | %s\n",
                "フレーム", "扉°", "厳密", "簡易", "保持", "最古(f)", "平均(f)", "使ったレイ");
    const int frames = 240;
    for (int k = 0; k < frames; ++k) {
        // 0〜120: 30°/s で開く。120〜180: 止める。180〜240: 続きを開く。
        float deg;
        if (k < 120) deg = 30.0f * (k * dt);
        else if (k < 180) deg = 30.0f * (120 * dt);
        else deg = std::min(90.0f, 30.0f * ((k - 60) * dt));
        w.setBoxTransform(leaf, doorLeaf(deg, -0.5f, 1.0f, 3.0f, 0.06f, 0.004f));
        const auto t0 = std::chrono::steady_clock::now();
        w.update(dt);
        msSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        { const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          msMax = std::max(msMax, ms); if (k > 5) msAll.push_back(ms); }
        int full = 0, light = 0, hold = 0, reTrace = 0;
        for (int i = 0; i < N; ++i) {
            const int t = w.tierOf(ids[static_cast<std::size_t>(i)]);
            const int r = w.raysOf(ids[static_cast<std::size_t>(i)]);
            // ★本数が変わった音源は、その場で**全群を引き直す**（world.h: groupRays != bs.rays）。
            //   1 音源ぶんが 4 倍になるので、同じフレームで何本も変わると山になる。
            if (r != prevRays[static_cast<std::size_t>(i)] && r > 0 && prevRays[static_cast<std::size_t>(i)] >= 0) ++reTrace;
            prevRays[static_cast<std::size_t>(i)] = r;
            if (r > 0) lastSolved[static_cast<std::size_t>(i)] = k;
            // 見通しと総量。簡易（t==1）は円盤が点に落ちるので、扉が動く途中で 0/1 に跳びうる。
            {
                const Visibility* vs = w.visibility(ids[static_cast<std::size_t>(i)]);
                const Mix* mx = w.mix(ids[static_cast<std::size_t>(i)]);
                double tot = 0.0; for (int b = 0; b < kNumBands; ++b) tot += mx->energy6[b];
                const float v = vs ? vs->visible : 1.0f;
                const float pv = prevVis[static_cast<std::size_t>(i)];
                if (pv >= 0.0f && std::fabs(v - pv) > 0.5f) { ++visJump; if (t == 1) ++visJumpLight; }
                if (prevTot[static_cast<std::size_t>(i)] > 0.0 && tot > 0.0) {
                    const double st = std::fabs(afti::dB(tot / prevTot[static_cast<std::size_t>(i)]));
                    const bool changed = (prevTier[static_cast<std::size_t>(i)] >= 0 && prevTier[static_cast<std::size_t>(i)] != t);
                    if (changed) { ++tierChanges; stepOnChange = std::max(stepOnChange, st); }
                    else stepOnSame = std::max(stepOnSame, st);
                    if (st > worstStep) {
                        worstStep = st; worstStepAt = k; worstStepTier = t;
                        for (int c = 0; c < kNumComponents; ++c) {
                            double ce = 0.0; for (int b = 0; b < kNumBands; ++b) ce += mx->component6[c][b];
                            worstComp[c] = ce; worstCompPrev[c] = prevComp[static_cast<std::size_t>(i) * kNumComponents + c];
                        }
                        worstVis = v; worstVisPrev = pv; worstRays = r; worstImg = w.images(ids[static_cast<std::size_t>(i)])->count; worstWho = i;
                    }
                }
                prevTier[static_cast<std::size_t>(i)] = t;
                prevVis[static_cast<std::size_t>(i)] = v; prevTot[static_cast<std::size_t>(i)] = tot;
                for (int c = 0; c < kNumComponents; ++c) {
                    double ce = 0.0; for (int b = 0; b < kNumBands; ++b) ce += mx->component6[c][b];
                    prevComp[static_cast<std::size_t>(i) * kNumComponents + c] = ce;
                }
            }
            if (t == 0) ++full; else if (t == 1) ++light; else ++hold;
            const int stale = k - lastSolved[static_cast<std::size_t>(i)];
            if (k > 10 && stale > worstStale[static_cast<std::size_t>(i)]) worstStale[static_cast<std::size_t>(i)] = stale;
        }
        // AF_WATCH=<音源番号> でその音源の後期を毎フレーム出す（跳びが山か段かを見る）
        if (const char* wv = std::getenv("AF_WATCH")) {
            const int wi = std::atoi(wv);
            if (wi >= 0 && wi < N && k >= 25 && k <= 55) {
                const Mix* mx = w.mix(ids[static_cast<std::size_t>(wi)]);
                double late = 0.0, early = 0.0;
                for (int b = 0; b < kNumBands; ++b) { late += mx->component6[kLate][b]; early += mx->component6[kEarly][b]; }
                std::printf("        f%3d 扉%5.1f° 後期 %10.3e 初期 %10.3e レイ %3d 段 %d\n",
                            k, deg, late, early, w.raysOf(ids[static_cast<std::size_t>(wi)]), w.tierOf(ids[static_cast<std::size_t>(wi)]));
            }
        }
        reTraceTotal += reTrace; reTraceMax = std::max(reTraceMax, reTrace);
        if (k % 20 == 0 && k > 0) {
            int oldest = 0; double mean = 0.0;
            for (int i = 0; i < N; ++i) {
                const int stale = k - lastSolved[static_cast<std::size_t>(i)];
                oldest = std::max(oldest, stale); mean += stale;
            }
            std::printf("        %6d %6.1f | %5d %5d %5d | %8d %8.1f | %d\n",
                        k, deg, full, light, hold, oldest, mean / N, w.spentRays());
        }
    }
    int worst = 0; double meanWorst = 0.0;
    for (int i = 0; i < N; ++i) { worst = std::max(worst, worstStale[static_cast<std::size_t>(i)]); meanWorst += worstStale[static_cast<std::size_t>(i)]; }
    std::printf("        → 最悪の瞬間: 見通し %.4f → %.4f、レイ %d、虚像 %d\n（音源 %d）", worstVisPrev, worstVis, worstRays, worstImg, worstWho);
    { static const char* cn[5] = {"直接", "初期", "後期", "回折", "透過"};
      for (int c = 0; c < kNumComponents; ++c)
          std::printf("            %s %10.3e → %10.3e（%+.1f dB）\n", cn[c], worstCompPrev[c], worstComp[c],
                      afti::dB(std::max(worstComp[c], 1e-30) / std::max(worstCompPrev[c], 1e-30))); }
    std::printf("        → 段が入れ替わった回数 %d、そのフレームの最大の段差 %.2f dB（入れ替わらない所は %.2f dB）\n",
                tierChanges, stepOnChange, stepOnSame);
    std::printf("        → 見通しが 1 フレームで 0.5 以上跳んだ回数 %d（うち簡易の段 %d）\n", visJump, visJumpLight);
    std::printf("        → 1 音源の総量の 1 フレーム最大の段差 %.2f dB（フレーム %d、段 %d）\n", worstStep, worstStepAt, worstStepTier);
    { std::sort(msAll.begin(), msAll.end());
      const std::size_t m = msAll.size();
      int over = 0; for (double v : msAll) if (v > 16.7) ++over;
      std::printf("        → 中央 %.2f ms / 95%% %.2f ms / 最大 %.2f ms、16.7 ms 超え %d / %d フレーム\n",
                  msAll[m/2], msAll[m*95/100], msAll[m-1], over, static_cast<int>(m)); }
    std::printf("        → 全群の引き直し: 合計 %d 回 / 1 フレーム最大 %d 本（音源 %d 本中）\n", reTraceTotal, reTraceMax, N);
    std::printf("        → GPU: %s（%s）\n", w.gpuActive() ? "入" : "切",
                w.gpuActive() ? w.gpuAdapter().c_str() : w.gpuError().c_str());
    std::printf("        → 1 フレームの update: 平均 %.2f ms / 最大 %.2f ms（音源 %d 本、群 %d）\n", msSum / frames, msMax, N, w.rayGroups);
    std::printf("        → 答えが古くなった最大 %d フレーム（%.0f ms）、音源ごとの平均 %.1f フレーム\n",
                worst, worst * dt * 1000.0f, meanWorst / N);
}

// ================================ [後期の向き] 戸口越しの後期に向きを付ける（段 2-f、AF_ONLY=latedir）
//   ★試聴の指摘「同じ部屋は LEV でいいが、隣の部屋では扉からの指向性が強いはず」の見張り。数字の土台は docs/CORE_DIFF.md ④。
//   ① 出どころを分けても後期・初期・帳簿は 1 ビットも変わらない（足すだけ）
//   ② 隣の部屋では送りが 2 本に割れ、戸口越しの分が戸口を向く
//   ③ 同じ部屋では割れない（LEV のまま）       ④ 切り替え 0 で旧に戻る
//   ⑤ FDN の部屋ごとの向きと広がり             ⑥ GPU でも同じ出どころ（CPU と dB で近い／束ねても 1 本ずつと同じ）
//   ⑦ 戸口をまたいで歩いたときの段差（clicks と同じ配線。clicks の歩きは戸口の 0.86 m 手前で終わるので、ここは今まで測っていない）
void testLateThrough() {
    std::printf("\n[後期の向き] 戸口越しの後期に向きを付ける（段 2-f）\n");
    char buf[256];
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const Vec3 S(0.0f, 1.6f, 3.0f), doorC(0.0f, 1.5f, 0.0f);
    auto setup = [&](World& w, const Vec3& L, int rays) {
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(90.0f, -0.5f, 1.0f, 3.0f, 0.06f, 0.004f), lm, true);
        w.raysPerEmitter = rays; w.budget.cfg.maxPerEmitter = std::max(rays, w.budget.cfg.maxPerEmitter);
        w.lateThrough = 1;   // この検査は案1（段 2-f）の見張り。段 2-g の戸口の線音源は [戸口の線音源] で見る
        w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        return e;
    };
    auto angleDeg = [](const Vec3& a, const Vec3& b) {
        const float la = length(a), lb = length(b);
        if (la <= 0.0f || lb <= 0.0f) return 180.0f;
        const float c = std::min(1.0f, std::max(-1.0f, dot(a, b) / (la * lb)));
        return std::acos(c) * 180.0f / 3.14159265f;
    };
    auto sendE = [](const FdnSend& sd) { double e = 0.0; for (int b = 0; b < kNumBands; ++b) e += sd.e6[b]; return e; };

    // ① 足すだけ（分けても分けなくても、後期・初期・帳簿は 1 ビットも同じ）
    {
        World w; const Vec3 L(0.0f, 1.6f, -3.0f);
        setup(w, L, 64);
        w.update(1.0f / 60.0f);
        const TraceScene& sc = w.traceScene();
        const int lroom = w.roomAt(L);
        EnergyTrace cpu;
        TraceParams p0; p0.rays = 2048; p0.maxBounces = 40; p0.mixingSec = 0.03f; p0.seed = 7u;
        TraceParams p1 = p0; p1.listenerRoom = lroom;
        const TraceResult r0 = cpu.run(sc, S, L, p0), r1 = cpu.run(sc, S, L, p1);
        bool same = (r0.hits == r1.hits && r0.neeVisible == r1.neeVisible);
        bool within = true; double o0 = 0.0, o1 = 0.0, late = 0.0;
        for (int b = 0; b < kNumBands; ++b) {
            same = same && r0.early6[b] == r1.early6[b] && r0.late6[b] == r1.late6[b]
                        && r0.emitted6[b] == r1.emitted6[b] && r0.absorbed6[b] == r1.absorbed6[b]
                        && r0.remainder6[b] == r1.remainder6[b] && r0.escaped6[b] == r1.escaped6[b];
            within = within && (r1.lateOther6[b] <= r1.late6[b] * 1.000001f + 1e-12f);
            o0 += r0.lateOther6[b]; o1 += r1.lateOther6[b]; late += r1.late6[b];
        }
        std::snprintf(buf, sizeof(buf), "(耳の部屋 %d、戸口越し 分けない %.2e／分ける %.2e ＝ 後期 %.2e の %.1f%%)",
                      lroom, o0, o1, late, (late > 0.0) ? o1 / late * 100.0 : 0.0);
        check("[後期の向き] 出どころを分けても後期・初期・帳簿は 1 ビットも変わらない（足すだけ）",
              same && o0 == 0.0 && o1 > 0.0 && within, buf);
    }

    // ② 隣の部屋: 送りが 2 本に割れ、戸口越しの分が音源の部屋へ戸口を向いて行く
    std::printf("        %13s | %4s %9s | %8s %7s | %s\n", "耳", "送り", "戸口越し", "戸口へ", "集まり", "部屋（耳 / 音源）");
    {
        const float spots[4][2] = {{-3.0f, -3.0f}, {0.0f, -3.0f}, {3.0f, -3.0f}, {0.0f, -1.0f}};
        for (int k = 0; k < 4; ++k) {
            World w; const Vec3 L(spots[k][0], 1.6f, spots[k][1]);
            const int e = setup(w, L, 2048);
            for (int f = 0; f < 60; ++f) w.update(1.0f / 60.0f);
            const Mix* mx = w.mix(e);
            double e0 = 0.0, e1 = 0.0; float ang = -1.0f, foc = 0.0f;
            if (mx->sendCount >= 1) e0 = sendE(mx->sends[0]);
            if (mx->sendCount >= 2) {
                e1 = sendE(mx->sends[1]);
                ang = angleDeg(Vec3(mx->sends[1].dir[0], mx->sends[1].dir[1], mx->sends[1].dir[2]), doorC - L);
                foc = mx->sends[1].focus;
            }
            const double share = (e0 + e1 > 0.0) ? e1 / (e0 + e1) : 0.0;
            std::printf("        (%+.1f, %+.1f) | %4d %8.1f%% | %7.1f° %7.3f | %d / %d\n",
                        L.x, L.z, mx->sendCount, share * 100.0, ang, foc, w.roomAt(L), w.roomAt(S));
            if (k == 1) {
                std::snprintf(buf, sizeof(buf), "(送り %d 本、戸口越し %.1f%%、戸口の中心から %.1f°、集まり %.3f)",
                              mx->sendCount, share * 100.0, ang, foc);
                check("[後期の向き] 隣の部屋では後期が 2 本に割れ、戸口越しの分が音源の部屋へ戸口を向いて行く",
                      mx->sendCount == 2 && mx->sends[0].room == w.roomAt(L) && mx->sends[1].room == w.roomAt(S)
                      && ang < 10.0f && foc > 0.8f && share > 0.03 && share < 0.4, buf);
            }
        }
    }

    // ③ 同じ部屋では割らない
    {
        World w; const Vec3 L(0.0f, 1.6f, 1.5f);
        const int e = setup(w, L, 512);
        for (int f = 0; f < 60; ++f) w.update(1.0f / 60.0f);
        const Mix* mx = w.mix(e);
        std::snprintf(buf, sizeof(buf), "(送り %d 本)", mx->sendCount);
        check("[後期の向き] 同じ部屋では割らない（LEV のまま）", mx->sendCount == 1, buf);
    }
    // ④ 切り替え 0 で旧に戻る
    {
        World w; const Vec3 L(0.0f, 1.6f, -3.0f);
        const int e = setup(w, L, 512);
        w.lateThrough = 0;
        for (int f = 0; f < 60; ++f) w.update(1.0f / 60.0f);
        const Mix* mx = w.mix(e);
        std::snprintf(buf, sizeof(buf), "(送り %d 本)", mx->sendCount);
        check("[後期の向き] 切り替え 0 で旧に戻る（隣の部屋でも 1 本）", mx->sendCount == 1, buf);
    }
    // ⑤ FDN の部屋ごとの向きと広がり
    {
        af::dsp::FdnRoomMix fdn(48000, 512, 0.6f);
        World w; const Vec3 L(0.0f, 1.6f, -3.0f);
        setup(w, L, 2048);
        w.bindFdn(&fdn);                                  // ★update より先に繋ぐ（器の寿命は呼び出し側の責任）
        for (int f = 0; f < 60; ++f) { w.update(1.0f / 60.0f); if (w.fdnStale()) w.bindFdn(&fdn); }
        Vec3 dA, dB; float sA = -1.0f, sB = -1.0f;
        const bool okA = w.fdnDirection(w.roomAt(S), dA, sA), okB = w.fdnDirection(w.roomAt(L), dB, sB);
        std::snprintf(buf, sizeof(buf), "(音源の部屋: 向き %+.2f %+.2f %+.2f・広がり %.3f／耳の部屋: 広がり %.3f)",
                      dA.x, dA.y, dA.z, sA, sB);
        check("[後期の向き] 音源の部屋の FDN は正面（戸口）からほぼ点、耳の部屋の FDN は一様",
              okA && okB && dA.z > 0.95f && sA < 0.2f && sB == 1.0f, buf);
    }
    // ⑥ GPU でも同じ出どころ
    {
        acoustic::gpu::GpuTracer gt;
        if (!gt.init()) {
            std::printf("        GPU が使えないので ⑥ は飛ばします（%s）\n", gt.error().c_str());
        } else {
            World w; const Vec3 L(0.0f, 1.6f, -3.0f);
            setup(w, L, 64);
            w.update(1.0f / 60.0f);
            const TraceScene& sc = w.traceScene();
            TraceParams p; p.rays = 8192; p.maxBounces = 40; p.mixingSec = 0.03f; p.seed = 7u; p.listenerRoom = w.roomAt(L);
            EnergyTrace cpu;
            const TraceResult rc = cpu.run(sc, S, L, p);
            TraceResult rg;
            const bool ok = gt.upload(sc) && gt.run(sc, S, L, p, rg);
            double oc = 0.0, og = 0.0;
            for (int b = 0; b < kNumBands; ++b) { oc += rc.lateOther6[b]; og += rg.lateOther6[b]; }
            const float ang = angleDeg(Vec3(rc.otherDir[0], rc.otherDir[1], rc.otherDir[2]), Vec3(rg.otherDir[0], rg.otherDir[1], rg.otherDir[2]));
            const double dDb = afti::dB(std::max(og, 1e-30) / std::max(oc, 1e-30));
            std::snprintf(buf, sizeof(buf), "(戸口越し CPU %.3e / GPU %.3e ＝ %+.2f dB、向きのずれ %.2f°)", oc, og, dDb, ang);
            check("[後期の向き] GPU でも戸口越しの分が CPU と ±1 dB・向き 3° 以内", ok && std::fabs(dDb) < 1.0 && ang < 3.0f,
                  ok ? buf : gt.error().c_str());
            const Vec3 srcs[3] = {S, Vec3(-2.0f, 1.2f, 4.0f), Vec3(1.5f, 2.0f, 2.0f)};
            TraceResult one[3], many[3];
            acoustic::gpu::BatchJob jb[3];
            for (int j = 0; j < 3; ++j) {
                TraceParams pj = p; pj.rays = 700 + 300 * j; pj.seed = 5u + static_cast<std::uint32_t>(j);
                gt.run(sc, srcs[j], L, pj, one[j]);
                jb[j].source = srcs[j]; jb[j].prm = pj; jb[j].out = &many[j];
            }
            const bool okB = gt.runBatch(sc, L, jb, 3);
            bool same = okB;
            for (int j = 0; j < 3; ++j) {
                for (int b = 0; b < kNumBands; ++b) same = same && one[j].lateOther6[b] == many[j].lateOther6[b];
                for (int q = 0; q < 3; ++q) same = same && one[j].otherDir[q] == many[j].otherDir[q];
            }
            check("[後期の向き] GPU で束ねても、戸口越しの分と向きは 1 本ずつと同じ", same, okB ? "" : gt.error().c_str());
        }
    }
    // ⑦ 戸口をまたいで歩く（段差）。切り替え 0 と 1 を同じ台本で並べる
    {
        const int fs = 48000, block = 512;
        const float dt = static_cast<float>(block) / fs;
        // holdAt > -99 ならその z に立ったまま。uniformDir で音源の部屋の尾を一様に上書き、lateOnly で後期だけ、outMeanE に平均の量。
        auto walk = [&](int on, double* outCalm, double* outWorst, int* outSpikes, double* outBlock, float* outAtZ,
                        float holdAt = -100.0f, bool uniformDir = false, bool lateOnly = false, double* outMeanE = nullptr,
                        bool noiseIn = false) {
            World w;
            const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
            for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
            w.addBox(doorLeaf(90.0f), lm, true);
            w.lateThrough = on;
            // 探り: AF_WALK_LATEONLY で後期だけ、AF_WALK_MUTE=src|lis で音源の部屋／耳の部屋の FDN を黙らせる、AF_WALK_MEAN で平均の量を出す
            const bool lateOnlyAll = lateOnly || (std::getenv("AF_WALK_LATEONLY") != nullptr);
            if (lateOnlyAll) for (int c = 0; c < kNumComponents; ++c) w.rules.weights.w[c] = (c == kLate) ? 1.0f : 0.0f;
            const char* muteEnv = std::getenv("AF_WALK_MUTE");
            w.budget.cfg.totalRays = 1536; w.rayGroups = 4;            // Unity と同じ既定
            // 探り: AF_WALK_GROUPS で組の数、AF_WALK_RAYS で 1 音源の本数（予算なし）。レイの揺れのせいかを分ける
            if (const char* g = std::getenv("AF_WALK_GROUPS")) w.rayGroups = std::atoi(g);
            if (const char* r = std::getenv("AF_WALK_RAYS")) {
                const int nr = std::atoi(r);
                w.budget.cfg.totalRays = 0; w.raysPerEmitter = nr; w.budget.cfg.maxPerEmitter = std::max(nr, w.budget.cfg.maxPerEmitter);
            }
            Vec3 L(0, 1.6f, -2.5f);
            // 探り: AF_WALK_HOLD=z でその場に立ったまま（動きが作る段差か、止まっていても出る揺れかを分ける）
            //       AF_WALK_UNIFORM で、割るが向きは付けない（2 つの FDN に割ったせいか、1 本のレーンに寄せたせいかを分ける）
            const char* holdEnv = std::getenv("AF_WALK_HOLD");
            const bool hold = (holdEnv != nullptr) || holdAt > -99.0f;
            if (hold) L.z = holdEnv ? static_cast<float>(std::atof(holdEnv)) : holdAt;
            const bool forceUniform = (std::getenv("AF_WALK_UNIFORM") != nullptr) || uniformDir;
            double sumE = 0.0; int nSumE = 0;
            std::uint32_t noiseState = 20260911u;
            w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w.addEmitter(S, 0.2f);
            w.build();
            af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
            w.bindFdn(&fdn);
            af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
            af::dsp::VoiceRenderer v(vc);
            v.setOutputGain(1.0f); v.setTailLevel(1.0f);
            v.setFdnMix(&fdn);
            af::dsp::HrtfSet hrtf = af::dsp::HrtfSet::createSynthetic(fs);
            af::dsp::DirectionBus bus(fs, 8, block);
            v.setHrtfEnabled(true); v.setHrtfSet(&hrtf);
            bus.setHrtfSet(&hrtf, 57.0f); v.setDirectionBus(&bus); fdn.setDirectionBus(&bus, 57.0f);
            afti::SineSum sig(fs, block);
            std::vector<float> in(block), l(block), r(block), fl(block), fr(block), mix(block);
            double calm = 0.0, worst = 0.0, worstBlock = 0.0; int spikes = 0, nb = 0; float atZ = 0.0f;
            // 最悪の段差のフレームで何が動いたか（戸口越しの割合・音源の部屋の尾の向きと広がり）
            double prevSh = 0.0, curSh = 0.0, wSh0 = 0.0, wSh1 = 0.0, maxDSh = 0.0;
            float prevAz = 0.0f, curAz = 0.0f, prevSp = 1.0f, curSp = 1.0f;
            float wAz0 = 0.0f, wAz1 = 0.0f, wSp0 = 1.0f, wSp1 = 1.0f, maxDAz = 0.0f, maxDSp = 0.0f;
            // 探り AF_WALK_TRACE: 最悪の段差の前後の時系列（尾の開始・戸口越し・後期の送り・初期・直接）
            struct Row { float z; double db; float onsetMs; double share, lateE, earlyE, directE; };
            std::vector<Row> rows;
            int worstIdx = -1;
            double curLateE = 0.0, curEarlyE = 0.0, curDirE = 0.0; float curOnsetMs = 0.0f;
            double prevE = -1.0;
            int prevRoom = w.roomAt(L);
            const int frames = 380;                                    // 戸口の 2.5 m 手前から 2.5 m 先まで 1.4 m/s
            for (int k = 0; k < frames; ++k) {
                const float t = k * dt;
                if (k > 30 && !hold) { L.z = std::min(2.5f, -2.5f + 1.4f * (t - 30 * dt)); w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0)); }
                w.update(dt);
                if (w.fdnStale()) w.bindFdn(&fdn);
                if (muteEnv) {
                    const int rm = (std::strcmp(muteEnv, "src") == 0) ? w.roomAt(S) : w.roomAt(L);
                    const int* map = w.fdnRoomOfProbe();
                    if (map && rm >= 0 && rm < w.roomCount() && map[rm] >= 0) {
                        const float zero6[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                        fdn.setListenerWeight(map[rm], zero6);          // World が置いた重みを上書き（render の前）
                    }
                }
                if (forceUniform && on == 1) {
                    const int ra = w.roomAt(S);
                    const int* map = w.fdnRoomOfProbe();
                    if (map && ra >= 0 && ra < w.roomCount() && map[ra] >= 0) {
                        const float fwd[3] = {0.0f, 0.0f, 1.0f};
                        fdn.setListenerDirection(map[ra], fwd, 1.0f);     // World が置いた向きを上書き（render の前）
                    }
                }
                w.applyToVoice(e, v, fs);
                {
                    const Mix* mx = w.mix(e);
                    double a = 0.0, b2 = 0.0;
                    if (mx && mx->sendCount >= 1) a = sendE(mx->sends[0]);
                    if (mx && mx->sendCount >= 2) b2 = sendE(mx->sends[1]);
                    curSh = (a + b2 > 0.0) ? b2 / (a + b2) : 0.0;
                    curLateE = a + b2; curEarlyE = 0.0; curDirE = 0.0;
                    curOnsetMs = mx ? mx->onsetSec * 1000.0f : 0.0f;
                    if (mx) for (int i = 0; i < mx->tapCount; ++i) {
                        double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += mx->taps[i].e6[b];
                        if (mx->taps[i].kind == TapKind::Direct) curDirE += te;
                        else if (mx->taps[i].kind == TapKind::Early) curEarlyE += te;
                    }
                    Vec3 dA(0, 0, 1); float spA = 1.0f;
                    if (w.fdnDirection(w.roomAt(S), dA, spA)) { curAz = std::atan2(dA.x, dA.z) * 180.0f / 3.14159265f; curSp = spA; }
                }
                if (on == 1) {
                    const int rm = w.roomAt(L);
                    if (rm != prevRoom) { std::printf("        z=%+.2f で耳の部屋 %d → %d\n", L.z, prevRoom, rm); prevRoom = rm; }
                }
                // 量を比べるとき（⑧）は白色雑音。★正弦の和だと 2 つの FDN の出力が周波数ごとに決まった位相で干渉し、
                //   パワーの足し算が平均に寄らない（数本の正弦では ±1 dB 振れる）。段差を測るときは正弦の和のまま。
                for (int i = 0; i < block; ++i) {
                    if (noiseIn) { noiseState = noiseState * 1664525u + 1013904223u; in[static_cast<std::size_t>(i)] = static_cast<float>(noiseState >> 8) / 16777216.0f * 0.2f - 0.1f; }
                    else in[static_cast<std::size_t>(i)] = sig.next();
                }
                v.render(in.data(), block, l.data(), r.data(), nullptr);
                std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
                fdn.render(block, fl.data(), fr.data());
                for (int i = 0; i < block; ++i) mix[static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)] + fl[static_cast<std::size_t>(i)];
                std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
                bus.render(block, fl.data(), fr.data());
                for (int i = 0; i < block; ++i) mix[static_cast<std::size_t>(i)] += fl[static_cast<std::size_t>(i)];
                if (k < 40) continue;
                const double s = afti::sampleStepRatio(mix.data(), block);
                const double e2 = afti::energy(mix.data(), block) / block;
                const int rowIdx = static_cast<int>(rows.size());
                rows.push_back(Row{L.z, (prevE > 0.0 && e2 > 0.0) ? afti::dB(e2 / prevE) : 0.0, curOnsetMs, curSh, curLateE, curEarlyE, curDirE});
                if (nb < 20) calm = std::max(calm, s);
                else {
                    sumE += e2; ++nSumE;
                    if (s > worst) worst = s;
                    if (s > 4.0 * std::max(calm, 1e-6)) ++spikes;
                    if (prevE > 0.0 && e2 > 0.0) {
                        const double st = std::fabs(afti::dB(e2 / prevE));
                        if (st > worstBlock) { worstIdx = rowIdx; worstBlock = st; atZ = L.z; wSh0 = prevSh; wSh1 = curSh; wAz0 = prevAz; wAz1 = curAz; wSp0 = prevSp; wSp1 = curSp; }
                    }
                }
                if (nb >= 20) {
                    maxDAz = std::max(maxDAz, std::fabs(curAz - prevAz));
                    maxDSp = std::max(maxDSp, std::fabs(curSp - prevSp));
                    maxDSh = std::max(maxDSh, std::fabs(curSh - prevSh));
                }
                prevE = e2; prevSh = curSh; prevAz = curAz; prevSp = curSp;
                ++nb;
            }
            if (std::getenv("AF_WALK_TRACE") && worstIdx >= 0) {
                char line[256];
                std::snprintf(line, sizeof(line), "        [%s] 最悪の段差のまわり", on ? "新" : "旧");
                std::puts(line);
                for (int i = std::max(0, worstIdx - 6); i <= std::min(static_cast<int>(rows.size()) - 1, worstIdx + 3); ++i) {
                    const Row& rw = rows[static_cast<std::size_t>(i)];
                    std::snprintf(line, sizeof(line), "        %s z=%+.3f %+6.2f dB  尾の開始 %6.2f ms  戸口越し %5.1f%%  後期 %.3e  初期 %.3e  直接 %.3e",
                                  (i == worstIdx) ? ">" : " ", rw.z, rw.db, rw.onsetMs, rw.share * 100.0, rw.lateE, rw.earlyE, rw.directE);
                    std::puts(line);
                }
            }
            if (on == 1) {
                char line[256];
                std::snprintf(line, sizeof(line), "        新の最悪の段差のフレーム: 戸口越し %.1f%% → %.1f%%、音源の部屋の尾の向き %.1f° → %.1f°、広がり %.3f → %.3f",
                              wSh0 * 100.0, wSh1 * 100.0, wAz0, wAz1, wSp0, wSp1);
                std::puts(line);
                std::snprintf(line, sizeof(line), "        1 フレームの変化の最大: 戸口越し %.2f%%、向き %.2f°、広がり %.4f",
                              maxDSh * 100.0, maxDAz, maxDSp);
                std::puts(line);
            }
            *outCalm = calm; *outWorst = worst; *outSpikes = spikes; *outBlock = worstBlock; *outAtZ = atZ;
            if (outMeanE) *outMeanE = (nSumE > 0) ? sumE / nSumE : 0.0;
            if (std::getenv("AF_WALK_MEAN")) {
                char line[160];
                std::snprintf(line, sizeof(line), "        [%s] 平均の量 %.3e（%d ブロック）", on ? "新" : "旧", (nSumE > 0) ? sumE / nSumE : 0.0, nSumE);
                std::puts(line);
            }
        };
        double c0, w0, b0, c1, w1, b1; int s0, s1; float z0, z1;
        walk(0, &c0, &w0, &s0, &b0, &z0);
        walk(1, &c1, &w1, &s1, &b1, &z1);
        std::printf("        戸口をまたぐ 1.4 m/s: 旧 段差 %.2f dB（z=%+.2f）跳ね %.1f 倍・%d 回 ／ 新 段差 %.2f dB（z=%+.2f）跳ね %.1f 倍・%d 回\n",
                    b0, z0, w0 / std::max(c0, 1e-6), s0, b1, z1, w1 / std::max(c1, 1e-6), s1);
        std::snprintf(buf, sizeof(buf), "(跳ね 旧 %d 回 → 新 %d 回、段差 旧 %.2f → 新 %.2f dB)", s0, s1, b0, b1);
        check("[後期の向き] 戸口をまたいで歩いても、跳ねを旧より増やさない", s1 <= s0, buf);
        {
            double c2 = 0.0, w2 = 0.0, b2 = 0.0; int s2 = 0; float z2 = 0.0f;
            walk(2, &c2, &w2, &s2, &b2, &z2);
            std::snprintf(buf, sizeof(buf), "(跳ね 案1 %d 回 → 戸口の線音源 %d 回、段差 案1 %.2f → %.2f dB（z=%+.2f）)", s1, s2, b1, b2, z2);
            check("[戸口の線音源] 戸口をまたいで歩いても、跳ねを案1 より増やさない", s2 <= s1, buf);
        }
        // ⑧ 2 本に割っても後期の量は変わらない（止まって、後期だけ・向きなしで旧と新を比べる）
        //   ★部屋の FDN どうしが相関していると、√(1−t) と √t の振幅が揃って足され、量が (√(1−t)+√t)² 倍になる。
        //     同じ大きさの部屋は同じ遅延線で作られていたので、2026-09-11 にここで見つかった。
        double m0 = 0.0, m1 = 0.0, cc = 0.0, ww = 0.0, bb = 0.0; int ss = 0; float zz = 0.0f;
        walk(0, &cc, &ww, &ss, &bb, &zz, -3.0f, true, true, &m0, true);
        walk(1, &cc, &ww, &ss, &bb, &zz, -3.0f, true, true, &m1, true);
        const double dLate = afti::dB(std::max(m1, 1e-30) / std::max(m0, 1e-30));
        std::snprintf(buf, sizeof(buf), "(耳 z=-3、後期だけ・向きなし・白色雑音: 旧 %.3e → 新 %.3e ＝ %+.2f dB)", m0, m1, dLate);
        check("[後期の向き] 2 本に割っても後期の量は変わらない（部屋の FDN どうしが無相関）", std::fabs(dLate) < 1.0, buf);
    }
}

// ================================ [戸口の線音源] 隣の部屋の尾を戸口の横幅から鳴らす（段 2-g、AF_ONLY=doorline）
//   ★試聴の指摘「向こうの部屋の残響が全体から聞こえすぎ。ドア側に寄せたい」の見張り。
//   ① 幾何: 戸口の横幅の見込み角（正面 3 m で 19°、横 3 m・奥 3 m で 10°）、点の量は Σ² = 1
//   ② 閉じかけると、蝶番の側の点が隠れて像が隙間の側へ寄る
//   ③ 量: 案1（レーンの点）と戸口の線音源で、後期の量が同じ（白色雑音）
//   ④ 向き: 戸口が左前にある所で、左右差が旧（一様）より戸口の側へ寄る
//   ⑤ 立ち上がり: 耳の部屋の響きが戸口の音から鳴り始めるので、後期のエネルギーの重心が案1 より遅い
//   ⑥ 輪ができない: 戸口を行き来し続けても量が増え続けない
//   （戸口をまたいで歩く段差は [後期の向き] ⑦ に並べてある）
void testDoorLineSource() {
    std::printf("\n[戸口の線音源] 隣の部屋の尾を戸口の横幅から鳴らす（段 2-g）\n");
    char buf[256];
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const Vec3 S(0.0f, 1.6f, 3.0f);
    const int fs = 48000, block = 512;
    const float dt = static_cast<float>(block) / fs;

    // ①② 幾何だけ（音は鳴らさない。FDN は繋ぐ ── 戸口の線音源の設定は updateFdn が出す）
    auto geometry = [&](float deg, const Vec3& L, World::PortalDiag& out) {
        af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(deg), lm, true);
        w.lateThrough = 2;
        w.raysPerEmitter = 256; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        w.addEmitter(S, 0.2f);
        w.build();
        w.bindFdn(&fdn);
        for (int f = 0; f < 40; ++f) { w.update(dt); if (w.fdnStale()) w.bindFdn(&fdn); }
        if (w.portalDiag().empty()) return false;
        out = w.portalDiag()[0];
        return true;
    };
    auto centroidAz = [](const World::PortalDiag& d) {
        double num = 0.0, den = 0.0;
        for (int k = 0; k < af::dsp::FdnRoomMix::kPortalPoints; ++k) {
            const double g2 = static_cast<double>(d.pointGain[k]) * d.pointGain[k];
            num += g2 * d.pointAz[k]; den += g2;
        }
        return (den > 0.0) ? num / den : 0.0;
    };
    {
        World::PortalDiag a{}, b{};
        const bool okA = geometry(90.0f, Vec3(0.0f, 1.6f, -3.0f), a);
        const bool okB = geometry(90.0f, Vec3(3.0f, 1.6f, -3.0f), b);
        double sa = 0.0;
        for (int k = 0; k < af::dsp::FdnRoomMix::kPortalPoints; ++k) sa += static_cast<double>(a.pointGain[k]) * a.pointGain[k];
        std::printf("        扉 90°: 正面 3 m 横幅 %.1f°・見通し %.2f・点の方位 %+.1f %+.1f %+.1f %+.1f %+.1f／横 3 m 横幅 %.1f°\n",
                    a.spanDeg, a.visible, a.pointAz[0], a.pointAz[1], a.pointAz[2], a.pointAz[3], a.pointAz[4], b.spanDeg);
        std::printf("        量: 戸口から耳へ直接 %.3f・耳の部屋へ流す %.3f（振幅）\n", a.directGain, a.feedGain);
        std::snprintf(buf, sizeof(buf), "(正面 3 m %.1f°、横 3 m %.1f°、点の量の Σ² %.3f)", a.spanDeg, b.spanDeg, sa);
        check("[戸口の線音源] 戸口の横幅を見込み角で持つ（正面 3 m で 19°±2、横 3 m・奥 3 m で 10°±2、Σ² = 1）",
              okA && okB && std::fabs(a.spanDeg - 19.0f) < 2.0f && std::fabs(b.spanDeg - 10.0f) < 2.0f && std::fabs(sa - 1.0) < 1e-3, buf);
    }
    {
        World::PortalDiag open{}, ajar{};
        const bool ok1 = geometry(90.0f, Vec3(0.0f, 1.6f, -3.0f), open);
        // ★45° で見る。20° では正面から戸口の全部が板の後ろに隠れ、5 点の見通しが揃って下がるので寄りが出ない
        //   （揃って下がった分は量を配り直すと消える）。隙間が点にかかる角度で確かめる。
        const bool ok2 = geometry(45.0f, Vec3(0.0f, 1.6f, -3.0f), ajar);
        const double c1 = centroidAz(open), c2 = centroidAz(ajar);
        std::printf("        扉 45°: 点の量 %.2f %.2f %.2f %.2f %.2f（左 → 右。蝶番は左）\n",
                    ajar.pointGain[0], ajar.pointGain[1], ajar.pointGain[2], ajar.pointGain[3], ajar.pointGain[4]);
        std::snprintf(buf, sizeof(buf), "(像の方位の重心 扉 90° %+.1f° → 45° %+.1f°)", c1, c2);
        check("[戸口の線音源] 閉じかけると蝶番の側が隠れ、像が隙間の側（右）へ寄る", ok1 && ok2 && c2 > c1 + 1.0, buf);
    }

    // ③〜⑥ 鳴らして両耳を測る。mode = lateThrough、motion 0 止まる／1 戸口を ±1.5 m で行き来、
    //   input 0 白色雑音／1 50 ms の雑音の塊（立ち上がりを見る）。後期だけを鳴らす。
    struct Heard { double eL = 0.0, eR = 0.0, centroidSec = 0.0, earlyE = 0.0, lateE = 0.0; };
    auto listen = [&](int mode, const Vec3& L0, int motion, int input, int frames) {
        Heard h;
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(90.0f), lm, true);
        w.lateThrough = mode;
        for (int c = 0; c < kNumComponents; ++c) w.rules.weights.w[c] = (c == kLate) ? 1.0f : 0.0f;
        w.raysPerEmitter = 512; w.rayGroups = 4; w.budget.cfg.totalRays = 0;
        Vec3 L = L0;
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
        w.bindFdn(&fdn);
        af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
        af::dsp::VoiceRenderer v(vc);
        v.setOutputGain(1.0f); v.setTailLevel(1.0f);
        v.setFdnMix(&fdn);
        af::dsp::HrtfSet hrtf = af::dsp::HrtfSet::createSynthetic(fs);
        af::dsp::DirectionBus bus(fs, 8, block);
        v.setHrtfEnabled(true); v.setHrtfSet(&hrtf);
        bus.setHrtfSet(&hrtf, 57.0f); v.setDirectionBus(&bus); fdn.setDirectionBus(&bus, 57.0f);
        std::vector<float> in(block), l(block), r(block), fl(block), fr(block), bl(block), br(block);
        std::uint32_t st = 777u;
        double sumL = 0.0, sumR = 0.0, tw = 0.0, te = 0.0; int nSum = 0;
        for (int k = 0; k < frames; ++k) {
            const float t = k * dt;
            if (motion == 1) {
                const float ph = std::fmod(1.4f * t, 6.0f);
                L.z = (ph < 3.0f) ? (-1.5f + ph) : (4.5f - ph);
                w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            }
            w.update(dt);
            if (w.fdnStale()) w.bindFdn(&fdn);
            w.applyToVoice(e, v, fs);
            for (int i = 0; i < block; ++i) {
                float x = 0.0f;
                if (input == 0 || (input == 1 && k >= 150 && k < 155)) {
                    st = st * 1664525u + 1013904223u;
                    x = static_cast<float>(st >> 8) / 16777216.0f * 0.2f - 0.1f;
                }
                in[static_cast<std::size_t>(i)] = x;
            }
            v.render(in.data(), block, l.data(), r.data(), nullptr);
            std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
            fdn.render(block, fl.data(), fr.data());
            std::fill(bl.begin(), bl.end(), 0.0f); std::fill(br.begin(), br.end(), 0.0f);
            bus.render(block, bl.data(), br.data());
            double eL = 0.0, eR = 0.0;
            for (int i = 0; i < block; ++i) {
                const std::size_t q = static_cast<std::size_t>(i);
                const double yl = static_cast<double>(l[q]) + fl[q] + bl[q], yr = static_cast<double>(r[q]) + fr[q] + br[q];
                eL += yl * yl; eR += yr * yr;
            }
            eL /= block; eR /= block;
            if (input == 0 && k >= 100) { sumL += eL; sumR += eR; ++nSum; }
            if (input == 1 && k >= 150) { const double tt = (k - 150) * dt; tw += (eL + eR) * tt; te += eL + eR; }
            if (motion == 1) {
                if (k >= 200 && k < 400) h.earlyE += eL + eR;
                if (k >= frames - 200) h.lateE += eL + eR;
            }
        }
        h.eL = (nSum > 0) ? sumL / nSum : 0.0;
        h.eR = (nSum > 0) ? sumR / nSum : 0.0;
        h.centroidSec = (te > 0.0) ? tw / te : 0.0;
        return h;
    };
    // ③ 量
    {
        const Heard h1 = listen(1, Vec3(0.0f, 1.6f, -3.0f), 0, 0, 400);
        const Heard h2 = listen(2, Vec3(0.0f, 1.6f, -3.0f), 0, 0, 400);
        const double d = afti::dB(std::max(h2.eL + h2.eR, 1e-30) / std::max(h1.eL + h1.eR, 1e-30));
        std::snprintf(buf, sizeof(buf), "(耳 正面 3 m、後期だけ・白色雑音: 案1 %.3e → 戸口の線音源 %.3e ＝ %+.2f dB)", h1.eL + h1.eR, h2.eL + h2.eR, d);
        check("[戸口の線音源] 後期の量は案1 と同じ（±1 dB）", std::fabs(d) < 1.0, buf);
    }
    // ④ 向き（戸口は左前 45°）
    {
        const Vec3 Lside(3.0f, 1.6f, -3.0f);
        const Heard h0 = listen(0, Lside, 0, 0, 400), h1 = listen(1, Lside, 0, 0, 400), h2 = listen(2, Lside, 0, 0, 400);
        const double i0 = afti::dB(std::max(h0.eL, 1e-30) / std::max(h0.eR, 1e-30));
        const double i1 = afti::dB(std::max(h1.eL, 1e-30) / std::max(h1.eR, 1e-30));
        const double i2 = afti::dB(std::max(h2.eL, 1e-30) / std::max(h2.eR, 1e-30));
        std::snprintf(buf, sizeof(buf), "(耳 x=+3・z=-3、左 − 右: 旧 %+.2f dB ／ 案1 %+.2f dB ／ 戸口の線音源 %+.2f dB)", i0, i1, i2);
        check("[戸口の線音源] 戸口が左前にある所で、左右差が旧より戸口の側（左）へ寄る", i2 > i0, buf);
    }
    // ⑤ 立ち上がり
    {
        const Heard h1 = listen(1, Vec3(0.0f, 1.6f, -3.0f), 0, 1, 400);
        const Heard h2 = listen(2, Vec3(0.0f, 1.6f, -3.0f), 0, 1, 400);
        std::snprintf(buf, sizeof(buf), "(50 ms の塊の後期のエネルギーの重心: 案1 %.1f ms → 戸口の線音源 %.1f ms)",
                      h1.centroidSec * 1000.0, h2.centroidSec * 1000.0);
        check("[戸口の線音源] 耳の部屋の響きが戸口の音から鳴り始める（後期の重心が案1 より遅い）", h2.centroidSec > h1.centroidSec, buf);
    }
    // ⑥ 輪ができない（戸口を ±1.5 m で 12 秒行き来）
    {
        const int frames = 1125;
        const Heard h = listen(2, Vec3(0.0f, 1.6f, -1.5f), 1, 0, frames);
        const double grow = afti::dB(std::max(h.lateE, 1e-30) / std::max(h.earlyE, 1e-30));
        std::snprintf(buf, sizeof(buf), "(2〜4 秒 %.3e → 最後の 2 秒 %.3e ＝ %+.2f dB)", h.earlyE, h.lateE, grow);
        check("[戸口の線音源] 戸口を行き来し続けても量が増え続けない（+6 dB 未満）", grow < 6.0, buf);
    }
    // ⑦ 戸口寄せ: 耳の部屋へ流す分を戸口から直接へ移しても、送りの総量は 1 ビットも変わらない
    {
        double tot[3] = {}, thru[3] = {};
        const float pulls[3] = {0.0f, 0.5f, 1.0f};
        for (int k = 0; k < 3; ++k) {
            World w; const Vec3 L(0.0f, 1.6f, -3.0f);
            const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
            for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
            w.addBox(doorLeaf(90.0f), lm, true);
            w.lateThrough = 2; w.doorPull = pulls[k];
            w.raysPerEmitter = 512; w.budget.cfg.maxPerEmitter = 512; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
            w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w.addEmitter(S, 0.2f);
            w.build();
            for (int f = 0; f < 60; ++f) w.update(1.0f / 60.0f);
            const Mix* mx = w.mix(e);
            for (int i = 0; i < mx->sendCount; ++i)
                for (int b = 0; b < kNumBands; ++b) { tot[k] += mx->sends[i].e6[b]; thru[k] += mx->sends[i].thru6[b]; }
        }
        std::snprintf(buf, sizeof(buf), "(戸口から直接の割合 寄せ 0: %.1f%% / 0.5: %.1f%% / 1: %.1f%%、総量 %.4e / %.4e / %.4e)",
                      thru[0] / tot[0] * 100.0, thru[1] / tot[1] * 100.0, thru[2] / tot[2] * 100.0, tot[0], tot[1], tot[2]);
        check("[戸口の線音源] 戸口寄せは戸口から直接の分を増やし、総量は変えない", tot[0] == tot[1] && tot[1] == tot[2] && thru[1] > thru[0] && thru[2] > thru[1], buf);
    }
}

/// 【探り】戸口に聞き手の探針を置いたら何が聞こえるか（AF_ONLY=doorprobe）
///   ★試聴の問い「ドアの位置にリスナープローブを置き、そこで聞こえる残響・反射音を音源にする方針はできているか」の物差し。
///   段 2-g の戸口の線音源は「向こうの部屋の FDN（部屋全体の統計）」を戸口から鳴らし、量は耳の位置のレイで決めている。
///   戸口の位置で聞こえる音は測っていない。ここでは
///     1) 戸口の向こう側 0.3 m に探針を置き、traceRay と同じ式の NEE で反射音（初期＋後期）を測る。
///        耳の部屋へ向かって戸口の面を通る向きの流れ（片側の放射照度 E = Σ c·cosθ）も取る。
///     2) 戸口を面の拡散音源（ランバート、放射輝度 E/π）にして、耳の位置へ届く量を面積分で見積もる（見通し込み）。
///     3) 今の耳の位置のレイの「戸口越し」（段 2-f の分類）と「後期全体」と並べる。
///   ★見積もりが今の戸口越しと同じくらいなら、探針にしても戸口から来る量は変わらない（物理が同じ）。
///     大きく違うなら、戸口のそばの音の偏りを今の分け方が取りこぼしている。
void testDoorProbe() {
    std::printf("\n[探り] 戸口に聞き手の探針を置く ── そこで聞こえる反射音と、戸口から耳へ届く見積もり\n");
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const Vec3 S(0.0f, 1.6f, 3.0f);
    const char* rs_ = std::getenv("AF_RAYS");
    const int rays = rs_ ? std::atoi(rs_) : 16384;
    for (float deg : {90.0f, 45.0f}) {
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(deg, -0.5f, 1.0f, 3.0f, 0.06f, 0.004f), lm, true);
        w.raysPerEmitter = 64; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        const Vec3 L0(0.0f, 1.6f, -3.0f);
        w.setListener(L0, Vec3(0, 0, 1), Vec3(0, 1, 0));
        w.addEmitter(S, 0.2f);
        w.build();
        w.update(1.0f / 60.0f);
        const TraceScene& sc = w.traceScene();
        const int rsrc = w.roomAt(S), rlis = w.roomAt(L0);
        int ai = -1;
        for (int a = 0; a < w.apertureCount(); ++a) {
            const rooms::Aperture& ap = w.aperture(a);
            if ((ap.roomA == rsrc && ap.roomB == rlis) || (ap.roomA == rlis && ap.roomB == rsrc)) { ai = a; break; }
        }
        if (ai < 0) { std::printf("        扉 %.0f°: 戸口が見つからない（部屋 %d / %d）\n", deg, rsrc, rlis); continue; }
        const rooms::Aperture& ap = w.aperture(ai);
        const Vec3 toSrc = (ap.roomB == rsrc) ? ap.normal : ap.normal * -1.0f;   // 耳の部屋 → 音源の部屋
        const Vec3 nOut = toSrc * -1.0f;                                           // 戸口から耳の部屋へ
        const Vec3 P = ap.rectCenter + toSrc * 0.3f;
        auto mixingOf = [&](const Vec3& q) {
            const int r = w.roomAt(q);
            return (r >= 0) ? std::max(0.01f, std::min(0.12f, 3.0f * w.probe(r).meanFreePath / kSpeedOfSound)) : 0.03f;
        };

        // 1) 探針: traceRay と同じ式の NEE。反射音の全方向の量 G と、耳の部屋へ向かう片側の流れ E。
        const float mixP = mixingOf(P);
        double gAll = 0.0, gLate = 0.0, eOne = 0.0, eOneLate = 0.0;
        {
            const float e0 = 1.0f / static_cast<float>(rays);
            const std::uint32_t seed = 7u;
            for (int i = 0; i < rays; ++i) {
                std::uint32_t rng = (seed * 2654435761u + 0x9E3779B9u) ^ (static_cast<std::uint32_t>(i) * 2246822519u);
                rng = rng * 747796405u + 2891336453u;
                float e[kNumBands];
                for (int b = 0; b < kNumBands; ++b) e[b] = e0;
                Vec3 pos = S;
                Vec3 dir = uniformSphere(rng);
                float pathLen = 0.0f;
                int skip = -1;
                for (int bounce = 0; bounce < 40; ++bounce) {
                    const SurfaceHit h = sceneNearest(sc, pos, dir, 1e4f, skip);
                    if (!h.hit) break;
                    pathLen += h.t;
                    const int mi = sc.material[static_cast<std::size_t>(h.index)];
                    const SurfaceSplit* tbl = &sc.split[static_cast<std::size_t>(mi) * kNumBands];
                    float rMean = 0.0f, tMean = 0.0f;
                    for (int b = 0; b < kNumBands; ++b) { rMean += tbl[b].reflect; tMean += tbl[b].transmit; }
                    rMean /= kNumBands; tMean /= kNumBands;
                    const Vec3 nFace = (dot(h.normal, dir) < 0.0f) ? h.normal : h.normal * -1.0f;
                    {
                        const Vec3 toP = P - h.point;
                        const float d = length(toP);
                        if (d > kEps) {
                            const Vec3 u = toP * (1.0f / d);                 // 放射点 → 探針（音の進む向き）
                            const float cosF = dot(nFace, u);
                            const bool front = cosF > 0.0f;
                            const float cosT = std::fabs(cosF);
                            const Vec3 side = front ? nFace : nFace * -1.0f;
                            float tr[kNumBands]; int cr = 0;
                            sceneTransmittance(sc, h.point + side * kEps, P, h.index, tr, &cr);
                            const float tSec = (pathLen + d) / kSpeedOfSound;
                            const float geo = cosT / (kPi * d * d);
                            double cs = 0.0;
                            for (int b = 0; b < kNumBands; ++b) {
                                const float eSide = e[b] * (front ? tbl[b].reflect : tbl[b].transmit);
                                cs += std::max(0.0f, eSide * geo * tr[b] * airEnergy(b, pathLen + d));
                            }
                            if (cs > 0.0) {
                                const double through = dot(u, nOut);         // 耳の部屋へ向かって戸口の面を通る向きの成分
                                gAll += cs;
                                if (tSec >= mixP) gLate += cs;
                                if (through > 0.0) { eOne += cs * through; if (tSec >= mixP) eOneLate += cs * through; }
                            }
                        }
                    }
                    const float carry = rMean + tMean;
                    if (carry <= 1e-6f) break;
                    const bool goReflect = rand01(rng) < (rMean / carry);
                    for (int b = 0; b < kNumBands; ++b) e[b] *= (tbl[b].reflect + tbl[b].transmit);
                    float eMax = 0.0f; for (int b = 0; b < kNumBands; ++b) eMax = std::max(eMax, e[b]);
                    if (eMax < e0 * 1e-4f) break;
                    if (goReflect) {
                        const float sct = sc.scatter1k[static_cast<std::size_t>(mi)];
                        dir = (rand01(rng) < sct) ? cosineHemisphere(nFace, rng) : reflect(dir, nFace);
                        if (dot(dir, nFace) <= 0.0f) dir = cosineHemisphere(nFace, rng);
                        pos = h.point + nFace * kEps;
                    } else {
                        pos = h.point - nFace * kEps;
                    }
                    skip = h.index;
                }
            }
        }
        std::printf("        扉 %.0f°: 探針（戸口の向こう 0.3 m）反射音 %.3e（うち後期 %.3e）、耳の部屋へ向かう片側の流れ %.3e（後期 %.3e）\n",
                    deg, gAll, gLate, eOne, eOneLate);

        // 2) 戸口を面の拡散音源（放射輝度 E/π）にして耳へ。矩形を 10 × 30 に割り、見通しを掛ける。
        const bool uW = std::fabs(ap.axisU.y) <= std::fabs(ap.axisV.y);
        const Vec3 wAx = uW ? ap.axisU : ap.axisV, hAx = uW ? ap.axisV : ap.axisU;
        const float hw = uW ? ap.halfU : ap.halfV, hh = uW ? ap.halfV : ap.halfU;
        std::printf("        %13s | %11s %11s | %11s %11s | %8s %9s\n",
                    "耳", "探針→戸口", "(後期だけ)", "今の戸口越し", "今の後期", "後期どうし", "後期に占める");
        const float spots[4][2] = {{-3.0f, -3.0f}, {0.0f, -3.0f}, {3.0f, -3.0f}, {0.0f, -1.0f}};
        for (int k = 0; k < 4; ++k) {
            const Vec3 L(spots[k][0], 1.6f, spots[k][1]);
            double gDoor = 0.0, gDoorLate = 0.0;
            const int NU = 10, NV = 30;
            const double dS = (2.0 * hw / NU) * (2.0 * hh / NV);
            for (int iu = 0; iu < NU; ++iu)
                for (int iv = 0; iv < NV; ++iv) {
                    const Vec3 q = ap.rectCenter + wAx * (((static_cast<float>(iu) + 0.5f) / NU * 2.0f - 1.0f) * hw)
                                                 + hAx * (((static_cast<float>(iv) + 0.5f) / NV * 2.0f - 1.0f) * hh);
                    const Vec3 dv = L - q;
                    const double r2 = static_cast<double>(dot(dv, dv));
                    if (r2 < 1e-4) continue;
                    const double cosS = static_cast<double>(dot(dv, nOut)) / std::sqrt(r2);
                    if (cosS <= 0.0) continue;
                    float tau[kNumBands];
                    w.surfaces.transmittance(q + nOut * 0.02f, L, -1, w.rules.materials, tau);
                    double vis = 0.0; for (int b = 0; b < kNumBands; ++b) vis += tau[b]; vis /= kNumBands;
                    const double g = dS * cosS / r2 * vis / kPi;
                    gDoor += eOne * g; gDoorLate += eOneLate * g;
                }
            // 3) 今の耳の位置のレイ（段 2-f の分類）
            TraceParams p2; p2.rays = rays; p2.maxBounces = 40; p2.mixingSec = mixingOf(L); p2.seed = 7u; p2.listenerRoom = w.roomAt(L);
            EnergyTrace cpu;
            const TraceResult rr = cpu.run(sc, S, L, p2);
            double thru = 0.0, late = 0.0;
            for (int b = 0; b < kNumBands; ++b) { thru += rr.lateOther6[b]; late += rr.late6[b]; }
            std::printf("        (%+.1f, %+.1f) | %11.3e %11.3e | %11.3e %11.3e | %+7.2f dB %8.1f%%\n",
                        L.x, L.z, gDoor, gDoorLate, thru, late,
                        afti::dB(std::max(gDoorLate, 1e-30) / std::max(thru, 1e-30)), (late > 0.0) ? gDoorLate / late * 100.0 : 0.0);
        }
    }
    std::printf("        （後期どうし = 探針から見積もった戸口の後期 ÷ 今の戸口越し。後期に占める = 探針の見積もり ÷ 今の後期全体）\n");
}

/// 【探り】戸口の線音源の両耳相関 ── 距離ごと（AF_ONLY=doorcoh）
///   試聴「ドアから遠いと定位が出るが、近いと方向がわからない」。5 点に無相関な波形を入れているので、
///   近づいて 5 点が広い角度に散ると両耳の相関が落ちて「前の雲」になる、という見立ての物差し。
///   後期だけ・戸口寄せ 1（後期を全部戸口の線音源から）で、戸口の正面 0.5 / 1 / 2 / 3 m に立ち、
///   最終出力の IACC（±1 ms の中の正規化相互相関の最大）と、点の見込み角を出す。AF_DOOR_COH=c で低域の相関の摘みを試す。
void testDoorCoherence() {
    std::printf("\n[探り] 戸口の線音源の両耳相関（後期だけ・戸口寄せ 1・白色雑音）\n");
    const int fs = 48000, block = 512;
    const float dt = static_cast<float>(block) / fs;
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const Vec3 S(0.0f, 1.6f, 3.0f);
    const char* ce = std::getenv("AF_DOOR_COH");
    const float coh = ce ? static_cast<float>(std::atof(ce)) : -1.0f;
    std::printf("        %6s | %8s | %6s %8s | %s\n", "距離", "横幅", "IACC", "L-R dB", "IACC(低域 <700) / (高域 >700)");
    for (float dist : {0.5f, 1.0f, 2.0f, 3.0f}) {
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(90.0f), lm, true);
        w.lateThrough = 2; w.doorPull = 1.0f;
        for (int c = 0; c < kNumComponents; ++c) w.rules.weights.w[c] = (c == kLate) ? 1.0f : 0.0f;
        w.raysPerEmitter = 512; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        const Vec3 L(0.0f, 1.6f, -dist);
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, 0.2f);
        w.build();
        af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
        if (coh >= 0.0f) w.doorCoherenceHz = coh;
        w.bindFdn(&fdn);
        af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
        af::dsp::VoiceRenderer v(vc);
        v.setOutputGain(1.0f); v.setTailLevel(1.0f); v.setFdnMix(&fdn);
        af::dsp::HrtfSet hrtf = af::dsp::HrtfSet::createSynthetic(fs);
        af::dsp::DirectionBus bus(fs, 8, block);
        v.setHrtfEnabled(true); v.setHrtfSet(&hrtf);
        bus.setHrtfSet(&hrtf, 57.0f); v.setDirectionBus(&bus); fdn.setDirectionBus(&bus, 57.0f);
        const int frames = 400;
        std::vector<float> in(block), l(block), r(block), fl(block), fr(block), bl(block), br(block);
        std::vector<float> outL, outR;
        std::uint32_t st = 777u;
        for (int k = 0; k < frames; ++k) {
            w.update(dt);
            if (w.fdnStale()) w.bindFdn(&fdn);
            w.applyToVoice(e, v, fs);
            for (int i = 0; i < block; ++i) { st = st * 1664525u + 1013904223u; in[static_cast<std::size_t>(i)] = static_cast<float>(st >> 8) / 16777216.0f * 0.2f - 0.1f; }
            v.render(in.data(), block, l.data(), r.data(), nullptr);
            std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
            fdn.render(block, fl.data(), fr.data());
            std::fill(bl.begin(), bl.end(), 0.0f); std::fill(br.begin(), br.end(), 0.0f);
            bus.render(block, bl.data(), br.data());
            if (k < 100) continue;
            for (int i = 0; i < block; ++i) {
                const std::size_t q = static_cast<std::size_t>(i);
                outL.push_back(l[q] + fl[q] + bl[q]); outR.push_back(r[q] + fr[q] + br[q]);
            }
        }
        // IACC: ±1 ms の中の正規化相互相関の最大。帯域は一次 LP/HP（700 Hz）で分けて別に出す
        auto iacc = [&](const std::vector<float>& a, const std::vector<float>& b) {
            const int n = static_cast<int>(a.size()), maxLag = fs / 1000;
            double ea = 0.0, eb = 0.0;
            for (int i = 0; i < n; ++i) { ea += static_cast<double>(a[i]) * a[i]; eb += static_cast<double>(b[i]) * b[i]; }
            double best = 0.0;
            for (int lag = -maxLag; lag <= maxLag; ++lag) {
                double c = 0.0;
                for (int i = std::max(0, -lag); i < std::min(n, n - lag); ++i) c += static_cast<double>(a[i]) * b[i + lag];
                best = std::max(best, std::fabs(c));
            }
            return best / std::sqrt(std::max(ea * eb, 1e-30));
        };
        auto split = [&](const std::vector<float>& x, std::vector<float>& lo, std::vector<float>& hi) {
            const float coef = 2.0f * 3.14159265f * 700.0f / fs;
            float s2 = 0.0f;
            lo.resize(x.size()); hi.resize(x.size());
            for (std::size_t i = 0; i < x.size(); ++i) { s2 += coef * (x[i] - s2); lo[i] = s2; hi[i] = x[i] - s2; }
        };
        std::vector<float> lLo, lHi, rLo, rHi;
        split(outL, lLo, lHi); split(outR, rLo, rHi);
        double eL = 0.0, eR = 0.0;
        for (std::size_t i = 0; i < outL.size(); ++i) { eL += static_cast<double>(outL[i]) * outL[i]; eR += static_cast<double>(outR[i]) * outR[i]; }
        const float span = (w.portalDiag().empty()) ? 0.0f : w.portalDiag()[0].spanDeg;
        std::printf("        %5.1f m | %7.1f° | %6.3f %+8.2f | %.3f / %.3f | 量 %.2f dB\n", dist, span, iacc(outL, outR),
                    10.0 * std::log10(std::max(eL, 1e-30) / std::max(eR, 1e-30)), iacc(lLo, rLo), iacc(lHi, rHi),
                    10.0 * std::log10(std::max(eL + eR, 1e-30) / static_cast<double>(outL.size())));
    }
}

// ================================ 【探り】受取面（AF_ONLY=receiver、2026-09-13）
//   発注者の案「虚像を音源にするのでなく、面をレイの受取面にする」を、今の NEE と**同じレイ**で並べる。
//   レイは音源から面までだけを運び、面の小片に「反射して出ていく量」を到達時刻ごとに貯める（音源→面の空気込み）。
//   耳へ届く量は、小片の四角を耳から見た立体角 Ω で解析的に出す:
//     寄与 = 貯めた量 × Ω / (π·小片の面積) × 見える割合 × 空気(面→耳)
//   ★NEE（当たりごとに cosθ/(π d²)）と同じランバートの模型なので、期待値は一致するはず。違うのは
//     「耳がレイの計算の外にいる」こと（耳が動いてもレイの統計が変わらない）と、耳のそばで上限の無い 1/d² の跳ねが無いこと。
//   測る物: ① 量が一致するか ② 歩いたときの揺れ ③ 壁に寄ったときの近さ ④ 費用。場面は makeWorldBox（7×3×7 m、α 0.2）。
//   鏡面の向きはまだ持たない（今の NEE と同じ全部ランバート）。壁越しは wallReflect=0 と同じ（面の表側・横切り無しだけ）。
namespace recvprobe {

struct Patch { int box = -1; Vec3 c, n, U, V; float hu = 0.0f, hv = 0.0f, area = 0.0f; };

struct Field {
    float cell = 0.5f, binSec = 0.00025f, mix = 0.03f;
    int bins = 0;
    std::vector<Patch> patches;
    std::vector<int> faceFirst, faceNu, faceNv;          // [箱 * 6 + 面]
    std::vector<float> E;                                 // [小片][時刻][帯域]
    std::vector<std::uint8_t> used;                       // 小片に何か入ったか
    float& at(int p, int k, int b) { return E[(static_cast<std::size_t>(p) * static_cast<std::size_t>(bins) + static_cast<std::size_t>(k)) * kNumBands + static_cast<std::size_t>(b)]; }
    void clear() { std::fill(E.begin(), E.end(), 0.0f); std::fill(used.begin(), used.end(), 0u); }
};

inline Vec3 axisOf(const Obb& o, int a) { return a == 0 ? o.axisX : (a == 1 ? o.axisY : o.axisZ); }
inline float halfOf(const Obb& o, int a) { return a == 0 ? o.halfExtents.x : (a == 1 ? o.halfExtents.y : o.halfExtents.z); }

inline void build(const TraceScene& sc, float cell, float mix, Field& F) {
    F.cell = cell; F.mix = mix;
    F.bins = static_cast<int>(std::ceil(mix / F.binSec)) + 1;
    const int nb = sc.boxCount();
    F.faceFirst.assign(static_cast<std::size_t>(nb) * 6, -1);
    F.faceNu.assign(static_cast<std::size_t>(nb) * 6, 0); F.faceNv.assign(static_cast<std::size_t>(nb) * 6, 0);
    F.patches.clear();
    for (int i = 0; i < nb; ++i) {
        if (!sc.active[static_cast<std::size_t>(i)]) continue;
        const Obb& o = sc.obb[static_cast<std::size_t>(i)];
        for (int f = 0; f < 6; ++f) {
            const int a = f / 2; const float sgn = (f % 2 == 0) ? 1.0f : -1.0f;
            const int au = (a + 1) % 3, av = (a + 2) % 3;
            const Vec3 n = axisOf(o, a) * sgn, U = axisOf(o, au), V = axisOf(o, av);
            const float hu = halfOf(o, au), hv = halfOf(o, av);
            const int nu = std::max(1, static_cast<int>(std::ceil(2.0f * hu / cell - 1e-4f)));
            const int nv = std::max(1, static_cast<int>(std::ceil(2.0f * hv / cell - 1e-4f)));
            const Vec3 fc = o.center + n * halfOf(o, a);
            const std::size_t key = static_cast<std::size_t>(i) * 6 + static_cast<std::size_t>(f);
            F.faceFirst[key] = static_cast<int>(F.patches.size()); F.faceNu[key] = nu; F.faceNv[key] = nv;
            const float su = 2.0f * hu / nu, sv = 2.0f * hv / nv;
            for (int iv = 0; iv < nv; ++iv)
                for (int iu = 0; iu < nu; ++iu) {
                    Patch p; p.box = i; p.n = n; p.U = U; p.V = V; p.hu = su * 0.5f; p.hv = sv * 0.5f; p.area = su * sv;
                    p.c = fc + U * (-hu + su * (static_cast<float>(iu) + 0.5f)) + V * (-hv + sv * (static_cast<float>(iv) + 0.5f));
                    F.patches.push_back(p);
                }
        }
    }
    F.E.assign(F.patches.size() * static_cast<std::size_t>(F.bins) * kNumBands, 0.0f);
    F.used.assign(F.patches.size(), 0u);
}

inline int patchOf(const TraceScene& sc, const Field& F, const SurfaceHit& h) {
    const Obb& o = sc.obb[static_cast<std::size_t>(h.index)];
    int a = 0; float best = -1.0f;
    for (int q = 0; q < 3; ++q) { const float d = std::fabs(dot(h.normal, axisOf(o, q))); if (d > best) { best = d; a = q; } }
    const int f = a * 2 + (dot(h.normal, axisOf(o, a)) > 0.0f ? 0 : 1);
    const std::size_t key = static_cast<std::size_t>(h.index) * 6 + static_cast<std::size_t>(f);
    const int first = F.faceFirst[key];
    if (first < 0) return -1;
    const int au = (a + 1) % 3, av = (a + 2) % 3;
    const float hu = halfOf(o, au), hv = halfOf(o, av);
    const Vec3 rel = h.point - o.center;
    const float u = dot(rel, axisOf(o, au)), v = dot(rel, axisOf(o, av));
    const int nu = F.faceNu[key], nv = F.faceNv[key];
    const int iu = std::min(nu - 1, std::max(0, static_cast<int>(std::floor((u + hu) / (2.0f * hu) * static_cast<float>(nu)))));
    const int iv = std::min(nv - 1, std::max(0, static_cast<int>(std::floor((v + hv) / (2.0f * hv) * static_cast<float>(nv)))));
    return first + iu + iv * nu;
}

// 耳に届いた初期の量。hist は到達時刻（0.25 ms）ごと、dir は届いた量で重みを付けた「来る向き」の和。
struct Heard {
    double early[kNumBands] = {};
    double dir[3] = {0.0, 0.0, 0.0};
    double sum = 0.0, maxOne = 0.0;
    std::vector<double> hist;
    int visTests = 0;
    double total() const { double s = 0.0; for (int b = 0; b < kNumBands; ++b) s += early[b]; return s; }
    double focus() const { return (sum > 0.0) ? std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) / sum : 0.0; }
};

// traceRay（wallReflect=0）と 1 行ずつ同じ輸送。F があれば小片に貯め、nee と L があれば今の NEE を足す。
inline void trace(const TraceScene& sc, const Vec3& S, const Vec3* L, const TraceParams& prm, int i, float e0,
                  float mix, Field* F, Heard* nee) {
    std::uint32_t rng = (prm.seed * 2654435761u + 0x9E3779B9u) ^ (static_cast<std::uint32_t>(i) * 2246822519u);
    rng = rng * 747796405u + 2891336453u;
    float e[kNumBands];
    for (int b = 0; b < kNumBands; ++b) e[b] = e0;
    Vec3 pos = S, dir = uniformSphere(rng);
    float pathLen = 0.0f;
    int skip = -1;
    for (int bounce = 0; bounce < prm.maxBounces; ++bounce) {
        const SurfaceHit h = sceneNearest(sc, pos, dir, 1e4f, skip);
        if (!h.hit) break;
        pathLen += h.t;
        const int mi = sc.material[static_cast<std::size_t>(h.index)];
        const SurfaceSplit* tbl = &sc.split[static_cast<std::size_t>(mi) * kNumBands];
        float rMean = 0.0f;
        for (int b = 0; b < kNumBands; ++b) rMean += tbl[b].reflect;
        rMean /= kNumBands;
        const Vec3 nFace = (dot(h.normal, dir) < 0.0f) ? h.normal : h.normal * -1.0f;
        if (F) {
            const float ts = pathLen / kSpeedOfSound;
            const int k = static_cast<int>(ts / F->binSec);
            const int p = patchOf(sc, *F, h);
            if (p >= 0 && k < F->bins && dot(h.normal, nFace) > 0.0f) {
                for (int b = 0; b < kNumBands; ++b) F->at(p, k, b) += e[b] * tbl[b].reflect * airEnergy(b, pathLen);
                F->used[static_cast<std::size_t>(p)] = 1u;
            }
        }
        if (nee && L) {
            const Vec3 toL = *L - h.point;
            const float d = length(toL);
            if (d > kEps) {
                const Vec3 u = toL * (1.0f / d);
                const float cosF = dot(nFace, u);
                const bool front = cosF > 0.0f;
                const Vec3 org = h.point + (front ? nFace : nFace * -1.0f) * kEps;
                float tr[kNumBands]; int cr = 0;
                sceneTransmittance(sc, org, *L, h.index, tr, &cr);
                const float tSec = (pathLen + d) / kSpeedOfSound;
                if (cr == 0 && front && tSec < mix) {
                    const float geo = std::fabs(cosF) / (kPi * d * d);
                    double cs = 0.0;
                    for (int b = 0; b < kNumBands; ++b) {
                        const float c = e[b] * tbl[b].reflect * geo * tr[b] * airEnergy(b, pathLen + d);
                        nee->early[b] += c; cs += c;
                    }
                    nee->dir[0] -= cs * u.x; nee->dir[1] -= cs * u.y; nee->dir[2] -= cs * u.z;
                    nee->sum += cs; nee->maxOne = std::max(nee->maxOne, cs);
                    const int kk = static_cast<int>(tSec / 0.00025f);
                    if (kk >= 0 && kk < static_cast<int>(nee->hist.size())) nee->hist[static_cast<std::size_t>(kk)] += cs;
                }
            }
        }
        if (rMean <= 1e-6f) break;
        for (int b = 0; b < kNumBands; ++b) e[b] *= tbl[b].reflect;
        float eMax = 0.0f; for (int b = 0; b < kNumBands; ++b) eMax = std::max(eMax, e[b]);
        if (eMax < e0 * 1e-4f) break;
        const float s = sc.scatter1k[static_cast<std::size_t>(mi)];
        dir = (rand01(rng) < s) ? cosineHemisphere(nFace, rng) : reflect(dir, nFace);
        if (dot(dir, nFace) <= 0.0f) dir = cosineHemisphere(nFace, rng);
        pos = h.point + nFace * kEps;
        skip = h.index;
    }
}

// 四角 [x0,x1]×[y0,y1]（面の上、耳の足元が原点）を高さ h から見た立体角。角ごとの符号付きの和。
inline double rectSolidAngle(double h, double x0, double x1, double y0, double y1) {
    auto S = [&](double x, double y) { return std::atan2(x * y, h * std::sqrt(x * x + y * y + h * h)); };
    return S(x1, y1) - S(x0, y1) - S(x1, y0) + S(x0, y0);
}

// 小片から耳へ（解析）。見える割合と来る向きは小片の上の m×m 点で、cosθ/r² の重みで取る。
inline void gather(const TraceScene& sc, Field& F, const Vec3& L, Heard& g) {
    g = Heard{};
    g.hist.assign(static_cast<std::size_t>(F.bins) * 2, 0.0);
    for (std::size_t pi = 0; pi < F.patches.size(); ++pi) {
        if (!F.used[pi]) continue;
        const Patch& P = F.patches[pi];
        const Vec3 rel = L - P.c;
        const double h = dot(rel, P.n);
        if (h <= 1e-4) continue;
        const double px = dot(rel, P.U), py = dot(rel, P.V);
        const double omega = rectSolidAngle(h, -P.hu - px, P.hu - px, -P.hv - py, P.hv - py);
        if (omega <= 0.0) continue;
        const int m = std::min(8, std::max(2, static_cast<int>(std::ceil(4.0 * std::max(P.hu, P.hv) / h))));
        double wsum = 0.0, wvis = 0.0, vd[3] = {0.0, 0.0, 0.0};
        for (int sv = 0; sv < m; ++sv)
            for (int su = 0; su < m; ++su) {
                const Vec3 q = P.c + P.U * (P.hu * ((2.0f * su + 1.0f) / m - 1.0f)) + P.V * (P.hv * ((2.0f * sv + 1.0f) / m - 1.0f));
                const Vec3 d = L - q;
                const float r = length(d);
                if (r < 1e-5f) continue;
                const double w = std::max(0.0, static_cast<double>(dot(P.n, d)) / r) / (static_cast<double>(r) * r);
                float tr[kNumBands]; int cr = 0;
                sceneTransmittance(sc, q + P.n * kEps, L, P.box, tr, &cr);
                ++g.visTests;
                wsum += w;
                if (cr != 0) continue;
                wvis += w;
                vd[0] -= w * d.x / r; vd[1] -= w * d.y / r; vd[2] -= w * d.z / r;
            }
        if (wsum <= 0.0 || wvis <= 0.0) continue;
        const double vis = wvis / wsum;
        const double geo = omega / (kPi * P.area) * vis;
        const float dc = length(rel);
        double csP = 0.0;
        for (int k = 0; k < F.bins; ++k) {
            const double t = (k + 0.5) * F.binSec + dc / kSpeedOfSound;
            if (t >= F.mix) break;
            double ck = 0.0;
            for (int b = 0; b < kNumBands; ++b) {
                const double c = F.at(static_cast<int>(pi), k, b) * geo * airEnergy(b, dc);
                g.early[b] += c; ck += c;
            }
            const int kk = static_cast<int>(t / F.binSec);
            if (kk >= 0 && kk < static_cast<int>(g.hist.size())) g.hist[static_cast<std::size_t>(kk)] += ck;
            csP += ck;
        }
        for (int q = 0; q < 3; ++q) g.dir[q] += csP * vd[q] / wvis;
        g.sum += csP;
        g.maxOne = std::max(g.maxOne, csP);
    }
}

inline double firstMs(const std::vector<double>& hist, double total, double directSec, float binSec) {
    double acc = 0.0;
    for (std::size_t k = 0; k < hist.size(); ++k) {
        acc += hist[k];
        if (acc >= 0.01 * total) return ((static_cast<double>(k) + 0.5) * binSec - directSec) * 1000.0;
    }
    return -1.0;
}

}  // namespace recvprobe

void testReceiverFaces() {
    using namespace recvprobe;
    using clk = std::chrono::steady_clock;
    std::puts("");
    std::puts("[探り] 受取面 ── 面をレイの受取面にして、耳へは立体角で解析的に配る（今の NEE と同じレイで並べる）");
    char line[320];
    const float half = 3.5f, hgt = 3.0f;
    World* w = makeWorldBox(half, hgt, 0.2f);
    w->raysPerEmitter = 64; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
    const Vec3 S(1.5f, 1.6f, -1.0f);
    w->setListener(Vec3(0.0f, 1.2f, 0.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    w->addEmitter(S, 0.2f);
    w->build();
    w->update(1.0f / 60.0f);
    const TraceScene& sc = w->traceScene();
    const int room = w->roomAt(Vec3(0.0f, 1.2f, 0.0f));
    const float mfp = (room >= 0) ? w->probe(room).meanFreePath : 3.0f;
    const float mix = std::max(0.01f, std::min(0.12f, 3.0f * mfp / kSpeedOfSound));
    const float cellEnv = std::getenv("AF_PATCH") ? static_cast<float>(std::atof(std::getenv("AF_PATCH"))) : 0.5f;
    std::snprintf(line, sizeof line, "        場面 7×3×7 m・α 0.2、音源 (1.5, 1.6, -1)、mixing %.1f ms、小片 %.2f m", mix * 1000.0, cellEnv);
    std::puts(line);

    TraceParams prm; prm.maxBounces = 24; prm.mixingSec = mix; prm.wallReflect = 0;
    auto neeAt = [&](const Vec3& L, int rays, std::uint32_t seed) {
        Heard h; h.hist.assign(static_cast<std::size_t>(std::ceil(mix / 0.00025f)) + 2, 0.0);
        TraceParams p = prm; p.rays = rays; p.seed = seed;
        const float e0 = 1.0f / static_cast<float>(rays);
        for (int i = 0; i < rays; ++i) trace(sc, S, &L, p, i, e0, mix, nullptr, &h);
        return h;
    };
    auto deposit = [&](Field& Fd, int rays, std::uint32_t seed) {
        Fd.clear();
        TraceParams p = prm; p.rays = rays; p.seed = seed;
        const float e0 = 1.0f / static_cast<float>(rays);
        for (int i = 0; i < rays; ++i) trace(sc, S, nullptr, p, i, e0, mix, &Fd, nullptr);
    };
    auto dB = [](double a, double b) { return 10.0 * std::log10(std::max(a, 1e-30) / std::max(b, 1e-30)); };

    Field F; build(sc, cellEnv, mix, F);
    std::snprintf(line, sizeof line, "        小片 %zu 個・時刻の箱 %d（0.25 ms）", F.patches.size(), F.bins);
    std::puts(line);

    // ── ① 量が一致するか（初期の総量、4096 本。参照は NEE 32768 本）──
    std::puts("        ① 量: 初期の総量（参照 NEE 32768 本との差 dB）");
    std::puts("           耳                 | NEE 4096 | 受取面 4096 | 受取面 32768 | 本体の NEE と写しの差");
    {
        const Vec3 Ls[4] = {Vec3(-1.0f, 1.2f, 1.5f), Vec3(0.0f, 1.2f, 0.0f), Vec3(-2.5f, 1.2f, 2.5f), Vec3(2.5f, 1.2f, -2.5f)};
        Field Fbig; build(sc, cellEnv, mix, Fbig);
        deposit(F, 4096, 7u); deposit(Fbig, 32768, 7u);
        for (const Vec3& L : Ls) {
            const Heard ref = neeAt(L, 32768, 7u), n4 = neeAt(L, 4096, 7u);
            Heard g4, g32; gather(sc, F, L, g4); gather(sc, Fbig, L, g32);
            TraceParams p = prm; p.rays = 4096; p.seed = 7u;
            EnergyTrace et; const TraceResult tr = et.run(sc, S, L, p);
            double eng = 0.0; for (int b = 0; b < kNumBands; ++b) eng += tr.early6[b];
            std::snprintf(line, sizeof line, "           (%+.1f, %+.1f, %+.1f) | %+7.2f  | %+10.2f  | %+11.2f  | %+.4f dB",
                          L.x, L.y, L.z, dB(n4.total(), ref.total()), dB(g4.total(), ref.total()), dB(g32.total(), ref.total()), dB(eng, n4.total()));
            std::puts(line);
        }
    }

    // ── ② 歩いたときの揺れ（1.4 m/s・60 fps で 2 m、耳 (-2, 1.2, 1.5) → (0, 1.2, 1.5)）──
    std::puts("        ② 歩き: 隣り合うフレームの初期の総量の段差（最大 dB）と、参照（NEE 16384 本）からのずれ（RMS / 最大 dB）");
    {
        const int frames = 86;
        std::vector<double> ref(frames);
        for (int k = 0; k < frames; ++k) {
            const Vec3 L(-2.0f + 2.0f * static_cast<float>(k) / (frames - 1), 1.2f, 1.5f);
            ref[static_cast<std::size_t>(k)] = neeAt(L, 16384, 7u).total();
        }
        for (int rays : {512, 4096}) {
            for (int mode = 0; mode < 2; ++mode) {
                if (mode == 1) deposit(F, rays, 7u);
                double prev = -1.0, maxStep = 0.0, rms = 0.0, maxDev = 0.0;
                for (int k = 0; k < frames; ++k) {
                    const Vec3 L(-2.0f + 2.0f * static_cast<float>(k) / (frames - 1), 1.2f, 1.5f);
                    double v;
                    if (mode == 0) v = neeAt(L, rays, 7u).total();
                    else { Heard g; gather(sc, F, L, g); v = g.total(); }
                    if (prev > 0.0) maxStep = std::max(maxStep, std::fabs(dB(v, prev)));
                    const double dev = dB(v, ref[static_cast<std::size_t>(k)]);
                    rms += dev * dev; maxDev = std::max(maxDev, std::fabs(dev));
                    prev = v;
                }
                std::snprintf(line, sizeof line, "           %-9s %5d 本: 段差 最大 %.3f dB ／ ずれ RMS %.2f・最大 %.2f dB",
                              mode == 0 ? "NEE" : "受取面", rays, maxStep, std::sqrt(rms / frames), maxDev);
                std::puts(line);
            }
        }
    }

    // ── ③ 壁に寄る（耳を x=-3.5 の壁へ。4096 本・種 4 つ）──
    std::puts("        ③ 壁に寄る: 初期の総量（種 4 つの平均と揺れ）、向きのまとまり（1 点 … 0 一様）、最初の 1% が届く時刻（直接の後 ms）");
    std::puts("           壁まで | NEE 量 dB  揺れ   まとまり  最初  | 受取面 量 dB  揺れ   まとまり  最初");
    {
        bool haveBase = false; double base = 0.0;
        for (float dw : {1.0f, 0.5f, 0.25f, 0.1f, 0.05f}) {
            const Vec3 L(-half + dw, 1.2f, 0.0f);
            const double directSec = length(S - L) / kSpeedOfSound;
            double nSum = 0.0, nSq = 0.0, gSum = 0.0, gSq = 0.0, nFocus = 0.0, gFocus = 0.0, nFirst = 0.0, gFirst = 0.0;
            for (std::uint32_t seed = 1u; seed <= 4u; ++seed) {
                const Heard n = neeAt(L, 4096, seed);
                deposit(F, 4096, seed);
                Heard g; gather(sc, F, L, g);
                const double nv = 10.0 * std::log10(std::max(n.total(), 1e-30)), gv = 10.0 * std::log10(std::max(g.total(), 1e-30));
                nSum += nv; nSq += nv * nv; gSum += gv; gSq += gv * gv;
                nFocus += n.focus(); gFocus += g.focus();
                nFirst += firstMs(n.hist, n.total(), directSec, 0.00025f); gFirst += firstMs(g.hist, g.total(), directSec, F.binSec);
            }
            const double nMean = nSum / 4.0, gMean = gSum / 4.0;
            if (!haveBase) { base = nMean; haveBase = true; }
            std::snprintf(line, sizeof line, "           %5.2f  | %+7.2f  %5.2f   %5.3f  %5.1f | %+9.2f  %5.2f   %5.3f  %5.1f",
                          dw, nMean - base, std::sqrt(std::max(0.0, nSq / 4.0 - nMean * nMean)), nFocus / 4.0, nFirst / 4.0,
                          gMean - base, std::sqrt(std::max(0.0, gSq / 4.0 - gMean * gMean)), gFocus / 4.0, gFirst / 4.0);
            std::puts(line);
        }
        std::puts("           （量は NEE の 1.0 m を 0 dB とした差）");
    }

    // ── ④ 費用（1 音源・1 フレーム）──
    std::puts("        ④ 費用: 1 フレーム（NEE はレイを飛ばし直す。受取面は音源と壁が動いたときだけ貯め直し、耳の移動は配るだけ）");
    {
        const Vec3 L(-1.0f, 1.2f, 1.5f);
        for (int rays : {512, 4096}) {
            auto t0 = clk::now();
            const Heard n = neeAt(L, rays, 7u);
            auto t1 = clk::now();
            deposit(F, rays, 7u);
            auto t2 = clk::now();
            Heard g; gather(sc, F, L, g);
            auto t3 = clk::now();
            auto ms = [](clk::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
            std::snprintf(line, sizeof line, "           %5d 本: NEE %.2f ms ／ 受取面 貯める %.2f ms・配る %.2f ms（見通しの線 %d 本）",
                          rays, ms(t1 - t0), ms(t2 - t1), ms(t3 - t2), g.visTests);
            std::puts(line);
            (void)n;
        }
    }
    delete w;
}

/// 【探り】受取面のタップの中身（AF_ONLY=recvtaps）── どの面から何が届いているか
///   AF_DOOR_DEG（既定 0 ＝ 閉）、耳は隣の部屋の戸口の正面 3 m。箱の番号: 0 床 1 天井 2 −x 3 +x 4 −z 5 +z 6 仕切り左 7 仕切り右 8 扉の板。
void testReceiverTaps() {
    std::puts("");
    std::puts("[探り] 受取面のタップの中身（二部屋・扉、耳は隣の部屋）");
    char line[300];
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    const float deg = std::getenv("AF_DOOR_DEG") ? static_cast<float>(std::atof(std::getenv("AF_DOOR_DEG"))) : 0.0f;
    const float clear = std::getenv("AF_CLEAR") ? static_cast<float>(std::atof(std::getenv("AF_CLEAR"))) : 0.0f;
    World w;
    const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
    w.addBox(doorLeaf(deg, -0.5f, 1.0f, 3.0f, 0.06f, clear), lm, true);
    w.raysPerEmitter = 2048; w.budget.cfg.maxPerEmitter = 2048; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
    const Vec3 S(0.0f, 1.6f, 3.0f), L(0.0f, 1.6f, -3.0f);
    w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
    const int e = w.addEmitter(S, 0.2f);
    w.build();
    for (int f = 0; f < 10; ++f) w.update(1.0f / 60.0f);
    const FaceTapSet* ft = w.faceTaps(e);
    double tot = 0.0; for (int b = 0; b < kNumBands; ++b) tot += ft->total6[b];
    std::snprintf(line, sizeof line, "        扉 %.0f°・隙間 %.3f m: タップ %d 本、初期の総量 %.3e、受け取り %d 件、小片 %d、見通しの線 %d",
                  deg, clear, ft->count, tot, ft->deposits, w.receiverPatches(), w.receiverVisTests());
    std::puts(line);
    for (int i = 0; i < ft->count; ++i) {
        const FaceTap& t = ft->tap[i];
        double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += t.e6[b];
        std::snprintf(line, sizeof line, "          箱 %d 面 %d 次 %d | 量 %.3e (%.1f%%) | 遅れ %.1f ms | 点 (%+.2f %+.2f %+.2f) | 向き (%+.2f %+.2f %+.2f) 広がり %.2f",
                      t.box, t.face, t.order, te, tot > 0 ? te / tot * 100.0 : 0.0, t.delaySec * 1000.0f,
                      t.point.x, t.point.y, t.point.z, t.dirWorld.x, t.dirWorld.y, t.dirWorld.z, t.spread);
        std::puts(line);
    }
}

/// 【探り】壁に寄ると最初の反射がどう変わるか ── 虚像の道と受取面を並べる（AF_ONLY=recvnear）
///   場面 makeWorldBox（7×3×7 m・α 0.2）、音源 (1.5, 1.6, -1)、耳を −x の壁へ寄せる（前は +z）。
///   量が総初期の 2% 以上ある初期のタップのうち、いちばん早い物: 直接の後の遅れ・取り分・広がり・左右（x）。
void testReceiverNear() {
    std::puts("");
    std::puts("[探り] 壁に寄ると最初の反射がどう変わるか（0 虚像の道 / 1 壁の受取面 / 2 虚像を面でつなぐ、4096 本）");
    std::puts("        壁まで | 虚像: 遅れ ms  取り分  広がり  左右 | 受取面: 遅れ ms  取り分  広がり  左右 | 受取面の壁の 1 回目: 遅れ  取り分  広がり | 虚像を面で: 遅れ  取り分  広がり  左右 | その壁の虚像: 遅れ  取り分");
    char line[420];
    const Vec3 S(1.5f, 1.6f, -1.0f);
    for (float dw : {2.0f, 1.0f, 0.5f, 0.25f, 0.1f}) {
        double r[3][4] = {};
        double wallTap[3] = {-1.0, 0.0, 0.0}, wallImg[2] = {-1.0, 0.0};
        for (int model = 0; model < 3; ++model) {
            World* w = makeWorldBox(3.5f, 3.0f, 0.2f);
            w->earlyModel = model;
            w->raysPerEmitter = 4096; w->budget.cfg.maxPerEmitter = 4096; w->rayGroups = 1; w->budget.cfg.totalRays = 0;
            const Vec3 L(-3.5f + dw, 1.2f, 0.0f);
            w->setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
            const int e = w->addEmitter(S, 0.2f);
            w->build();
            for (int k = 0; k < 30; ++k) w->update(1.0f / 60.0f);
            const Mix& m = *w->mix(e);
            float directSec = 0.0f;
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Direct) { directSec = m.taps[i].delaySec; break; }
            double tot = 0.0;
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Early) for (int b = 0; b < kNumBands; ++b) tot += m.taps[i].e6[b];
            int best = -1;
            for (int i = 0; i < m.tapCount; ++i) {
                if (m.taps[i].kind != TapKind::Early) continue;
                double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                if (te < 0.02 * tot) continue;
                if (best < 0 || m.taps[i].delaySec < m.taps[best].delaySec) best = i;
            }
            if (best >= 0) {
                const MixTap& t = m.taps[best];
                double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += t.e6[b];
                r[model][0] = (t.delaySec - directSec) * 1000.0; r[model][1] = te / tot * 100.0; r[model][2] = t.spread; r[model][3] = t.dirLocal.x;
            }
            if (model == 1) {
                const int wantId = FaceTapSet::kIdBase + ((2 * 6 + 0) * 2 + 0);
                for (int i = 0; i < m.tapCount; ++i)
                    if (m.taps[i].id == wantId) {
                        double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                        wallTap[0] = (m.taps[i].delaySec - directSec) * 1000.0; wallTap[1] = te / tot * 100.0; wallTap[2] = m.taps[i].spread;
                    }
            }
            if (model == 2) {
                int wantId = -1;
                for (std::size_t fi = 0; fi < w->faces().size(); ++fi)
                    if (w->faces()[fi].box == 2 && w->faces()[fi].normal.x > 0.5f) wantId = 16 + static_cast<int>(fi) + 1;
                for (int i = 0; i < m.tapCount; ++i)
                    if (m.taps[i].kind == TapKind::Early && m.taps[i].id == wantId) {
                        double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                        wallImg[0] = (m.taps[i].delaySec - directSec) * 1000.0; wallImg[1] = te / tot * 100.0;
                    }
            }
            delete w;
        }
        std::snprintf(line, sizeof line, "        %5.2f  | %8.2f  %5.1f%%   %4.2f  %+5.2f | %10.2f  %5.1f%%   %4.2f  %+5.2f | %8.2f  %5.1f%%   %4.2f | %8.2f  %5.1f%%   %4.2f  %+5.2f | %8.2f  %5.1f%%",
                      dw, r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3], wallTap[0], wallTap[1], wallTap[2],
                      r[2][0], r[2][1], r[2][2], r[2][3], wallImg[0], wallImg[1]);
        std::puts(line);
    }
}

/// 【探り】扉を動かしたときの初期のフレームごとの中身（AF_ONLY=recvdoor）── 段差の出どころ
///   clicks の「扉だけ」と同じ場面（耳 z=−4、音源 z=+3、扉 30°/s、組 4・総予算 1536）。AF_FROM / AF_TO（度、既定 24〜36）。
void testReceiverDoor() {
    std::puts("");
    std::puts("[探り] 扉を動かしたときの初期（0 虚像の道 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網。AF_MODEL で 1 つだけ）");
    const int fs = 48000, block = 512;
    const float dt = static_cast<float>(block) / fs;
    const float from = std::getenv("AF_FROM") ? static_cast<float>(std::atof(std::getenv("AF_FROM"))) : 24.0f;
    const float to = std::getenv("AF_TO") ? static_cast<float>(std::atof(std::getenv("AF_TO"))) : 36.0f;
    char line[700];
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    for (int model = 0; model < 4; ++model) {
        if (std::getenv("AF_MODEL") && std::atoi(std::getenv("AF_MODEL")) != model) continue;
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        const int leaf = w.addBox(doorLeaf(0.0f), lm, true);
        w.earlyModel = model;
        w.raysPerEmitter = 256; w.rayGroups = 4; w.budget.cfg.totalRays = 1536;
        Vec3 L(0, 1.6f, -4.0f);
        const bool walkToo = std::getenv("AF_WALK") != nullptr;   // 耳も 1.4 m/s で戸口へ歩く（clicks の歩き＋扉）
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
        w.build();
        std::snprintf(line, sizeof line, "      ── %s ──", model == 0 ? "虚像の道" : model == 1 ? "壁の受取面" : model == 2 ? "虚像を面でつなぐ" : "虚像の網");
        std::puts(line);
        std::puts("        扉°   | 初期(生) dB  差    | 後期(生) dB | 尾の開始 ms | 初期のタップ 本  和 dB   差   | 主なタップ（素性の面・次、取り分、遅れ ms）");
        double prevE = -1.0, prevT = -1.0;
        for (int k = 0; k < 240; ++k) {
            const float t = k * dt;
            float deg = 0.0f;
            if (k > 30) {
                deg = std::min(90.0f, 30.0f * (t - 30 * dt)); w.setBoxTransform(leaf, doorLeaf(deg));
                if (walkToo) { L.z = -4.0f + 1.4f * (t - 30 * dt); w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0)); }
            }
            w.update(dt);
            const Mix& m = *w.mix(e);
            double er = 0.0, lr = 0.0; for (int b = 0; b < kNumBands; ++b) { er += m.component6[kEarly][b]; lr += m.component6[kLate][b]; }
            double ts = 0.0; int nt = 0;
            float directSec = 0.0f;
            for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Direct) directSec = m.taps[i].delaySec;
            int top[3] = {-1, -1, -1}; double topE[3] = {0, 0, 0};
            for (int i = 0; i < m.tapCount; ++i) {
                if (m.taps[i].kind != TapKind::Early) continue;
                ++nt;
                double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                ts += te;
                for (int q = 0; q < 3; ++q) if (te > topE[q]) { for (int z = 2; z > q; --z) { topE[z] = topE[z - 1]; top[z] = top[z - 1]; } topE[q] = te; top[q] = i; break; }
            }
            if (deg < from || deg > to) { prevE = er; prevT = ts; continue; }
            char tops[400] = {0};
            int off = 0;
            for (int q = 0; q < 3; ++q) {
                if (top[q] < 0) continue;
                const MixTap& tp = m.taps[top[q]];
                int key = tp.id - FaceTapSet::kIdBase;
                if (model >= 1 && key >= 0) off += std::snprintf(tops + off, sizeof(tops) - off, " 箱%d面%d次%d %.0f%% %.1f (%+.2f %+.2f %+.2f) 広%.2f 方位%.0f", key / 12, (key / 2) % 6, key % 2, topE[q] / ts * 100.0, (tp.delaySec - directSec) * 1000.0, tp.dirLocal.x, tp.dirLocal.y, tp.dirLocal.z, tp.spread, std::atan2(tp.dirLocal.x, tp.dirLocal.z) * 57.2958);
                else off += std::snprintf(tops + off, sizeof(tops) - off, " id%d %.0f%% %.1f", tp.id, topE[q] / ts * 100.0, (tp.delaySec - directSec) * 1000.0);
            }
            double dirT = 0.0, difT = 0.0, trT = 0.0;
            for (int i = 0; i < m.tapCount; ++i) {
                double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += m.taps[i].e6[b];
                if (m.taps[i].kind == TapKind::Direct) dirT += te; else if (m.taps[i].kind == TapKind::Diffract) difT += te; else if (m.taps[i].kind == TapKind::Transmit) trT += te;
            }
            std::snprintf(line, sizeof line, "        %5.1f z%+.2f | %+9.2f  %+5.2f | %+9.2f  | %8.2f    | %4d  %+7.2f  %+5.2f | 直 %+6.1f 回 %+6.1f 透 %+6.1f |%s",
                          deg, L.z, afti::dB(er), prevE > 0 ? afti::dB(er / prevE) : 0.0, afti::dB(lr), (m.onsetSec - directSec) * 1000.0f,
                          nt, afti::dB(ts), prevT > 0 ? afti::dB(ts / prevT) : 0.0, afti::dB(dirT), afti::dB(difT), afti::dB(trT), tops);
            std::puts(line);
            if (std::getenv("AF_ALLTAPS")) std::printf("            虚像 %d 本（候補 %d）・ISM の面 %d\n", w.images(e)->count, w.images(e)->candidates, static_cast<int>(w.faces().size()));
            if (std::getenv("AF_ALLTAPS")) {   // タップを全部（種類・素性・取り分・遅れ ms・広がり・方位）
                for (int i = 0; i < m.tapCount; ++i) {
                    const MixTap& tp = m.taps[i];
                    double te = 0.0; for (int b = 0; b < kNumBands; ++b) te += tp.e6[b];
                    std::printf("            k%d id%d %.2f%% %.2f ms 広%.2f 方位%.0f\n", static_cast<int>(tp.kind), tp.id, ts > 0 ? te / ts * 100.0 : 0.0,
                                (tp.delaySec - directSec) * 1000.0, tp.spread, std::atan2(tp.dirLocal.x, tp.dirLocal.z) * 57.2958);
                }
            }
            prevE = er; prevT = ts;
        }
    }
}

/// 【探り】後期はどこから来るか（AF_ONLY=lateorigin）
///   ★「隣の部屋にいるときは扉からの指向性が強いはず」を**レイで**測る物差し。エンジンは変えない。
///   traceRay と同じ式で NEE を追い、後期の寄与ごとに「放射した面が耳と同じ部屋に面しているか」で分ける。
///     同じ部屋 … 自室の拡散音場（LEV でよい分）
///     別の部屋 … 戸口越しに見えている向こうの面（扉の向きから来るべき分）
///   別の部屋の分は、耳から放射点への向きをエネルギーで重み付けして平均し、
///   合成の長さ R（1 = 1 点に集中、0 = 全方向に散る）と、戸口の中心への角度を出す。
///   ★面の上の点は格子では壁の中に落ちるので、**耳の側へ 0.3 m 押し出した点**で部屋を引く。
void testLateOrigin() {
    const char* rs = std::getenv("AF_RAYS");
    const int rays = rs ? std::atoi(rs) : 8192;
    std::printf("\n[探り] 後期はどこから来るか ── 耳と同じ部屋の面か、戸口越しの向こうの面か（レイ %d 本）\n", rays);
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    // AF_WALL_ABSORB=<吸音率> で壁の吸音を変える（既定 0.2。Unity の Concrete は 0.02〜0.05 なので、戸口越しの割合が場面でどう動くかを見る）
    { const char* ab = std::getenv("AF_WALL_ABSORB"); const float a = ab ? static_cast<float>(std::atof(ab)) : 0.2f;
      for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = a; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; } }
    const Vec3 S(0.0f, 1.6f, 3.0f);
    const Vec3 doorC(0.0f, 1.5f, 0.0f);
    std::printf("        %5s %14s | %9s | %7s %7s %7s | %6s %7s | %6s\n",
                "扉", "耳", "後期", "同部屋", "別部屋", "不明", "別のR", "戸口へ", "同のR");
    struct Spot { float x, z; };
    const Spot spots[] = {{-3.0f, -3.0f}, {-1.0f, -3.0f}, {0.0f, -3.0f}, {1.0f, -3.0f}, {3.0f, -3.0f}, {0.0f, -1.0f}, {0.0f, 1.5f}};
    for (float deg : {90.0f, 0.0f}) {
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(deg, -0.5f, 1.0f, 3.0f, 0.06f, 0.004f), lm, true);
        w.raysPerEmitter = 64; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(Vec3(0, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        w.addEmitter(S, 0.2f);
        w.build();
        w.update(1.0f / 60.0f);                       // 部屋グラフと木を作る
        TraceScene sc;
        buildTraceScene(w.surfaces, w.rules.materials, sc);
        for (const Spot& sp : spots) {
            const Vec3 L(sp.x, 1.6f, sp.z);
            const int lroom = w.roomAt(L);
            float mixingSec = 0.03f;
            if (lroom >= 0) mixingSec = std::max(0.01f, std::min(0.12f, 3.0f * w.probe(lroom).meanFreePath / kSpeedOfSound));
            TraceParams prm; prm.rays = rays; prm.maxBounces = 40; prm.mixingSec = mixingSec; prm.seed = 7u;
            const float e0 = 1.0f / static_cast<float>(rays);
            double eOwn = 0.0, eOther = 0.0, eUnk = 0.0;
            double oX = 0.0, oY = 0.0, oZ = 0.0, sX = 0.0, sY = 0.0, sZ = 0.0;
            for (int i = 0; i < rays; ++i) {
                // ── traceRay と同じ式（種・散乱・打ち切りまで）。後期の寄与だけを分類して数える ──
                std::uint32_t rng = (prm.seed * 2654435761u + 0x9E3779B9u) ^ (static_cast<std::uint32_t>(i) * 2246822519u);
                rng = rng * 747796405u + 2891336453u;
                float e[kNumBands];
                for (int b = 0; b < kNumBands; ++b) e[b] = e0;
                Vec3 pos = S;
                Vec3 dir = uniformSphere(rng);
                float pathLen = 0.0f;
                int skip = -1;
                for (int bounce = 0; bounce < prm.maxBounces; ++bounce) {
                    const SurfaceHit h = sceneNearest(sc, pos, dir, 1e4f, skip);
                    if (!h.hit) break;
                    pathLen += h.t;
                    const int mi = sc.material[static_cast<std::size_t>(h.index)];
                    const SurfaceSplit* tbl = &sc.split[static_cast<std::size_t>(mi) * kNumBands];
                    float rMean = 0.0f, tMean = 0.0f;
                    for (int b = 0; b < kNumBands; ++b) { rMean += tbl[b].reflect; tMean += tbl[b].transmit; }
                    rMean /= kNumBands; tMean /= kNumBands;
                    const Vec3 nFace = (dot(h.normal, dir) < 0.0f) ? h.normal : h.normal * -1.0f;
                    {
                        const Vec3 toL = L - h.point;
                        const float d = length(toL);
                        if (d > kEps) {
                            const Vec3 u = toL * (1.0f / d);
                            const float cosF = dot(nFace, u);
                            const bool front = cosF > 0.0f;
                            const float cosT = std::fabs(cosF);
                            const Vec3 side = front ? nFace : nFace * -1.0f;
                            const Vec3 org = h.point + side * kEps;
                            float tr[kNumBands]; int cr = 0;
                            sceneTransmittance(sc, org, L, h.index, tr, &cr);
                            const float tSec = (pathLen + d) / kSpeedOfSound;
                            const float geo = cosT / (kPi * d * d);
                            double c = 0.0;
                            for (int b = 0; b < kNumBands; ++b) {
                                const float eSide = e[b] * (front ? tbl[b].reflect : tbl[b].transmit);
                                c += std::max(0.0f, eSide * geo * tr[b] * airEnergy(b, pathLen + d));
                            }
                            if (c > 0.0 && tSec >= prm.mixingSec) {
                                const int room = w.roomAt(h.point + side * 0.3f);
                                const Vec3 toP = u * -1.0f;              // 耳から放射点への向き（ワールド）
                                if (room < 0) eUnk += c;
                                else if (room == lroom) { eOwn += c; sX += c * toP.x; sY += c * toP.y; sZ += c * toP.z; }
                                else { eOther += c; oX += c * toP.x; oY += c * toP.y; oZ += c * toP.z; }
                            }
                        }
                    }
                    const float carry = rMean + tMean;
                    if (carry <= 1e-6f) break;
                    const bool goReflect = rand01(rng) < (rMean / carry);
                    for (int b = 0; b < kNumBands; ++b) e[b] *= (tbl[b].reflect + tbl[b].transmit);
                    float eMax = 0.0f; for (int b = 0; b < kNumBands; ++b) eMax = std::max(eMax, e[b]);
                    if (eMax < e0 * 1e-4f) break;
                    if (goReflect) {
                        const float s = sc.scatter1k[static_cast<std::size_t>(mi)];
                        dir = (rand01(rng) < s) ? cosineHemisphere(nFace, rng) : reflect(dir, nFace);
                        if (dot(dir, nFace) <= 0.0f) dir = cosineHemisphere(nFace, rng);
                        pos = h.point + nFace * kEps;
                    } else {
                        pos = h.point - nFace * kEps;
                    }
                    skip = h.index;
                }
            }
            const double tot = eOwn + eOther + eUnk;
            if (tot <= 0.0) continue;
            const double rOther = (eOther > 0.0) ? std::sqrt(oX * oX + oY * oY + oZ * oZ) / eOther : 0.0;
            const double rOwn = (eOwn > 0.0) ? std::sqrt(sX * sX + sY * sY + sZ * sZ) / eOwn : 0.0;
            double angDoor = -1.0;
            if (eOther > 0.0) {
                Vec3 dd = doorC - L; const float dl = length(dd); dd = dd * (1.0f / std::max(dl, 1e-6f));
                const double ol = std::sqrt(oX * oX + oY * oY + oZ * oZ);
                const double cs = (ol > 0.0) ? (oX * dd.x + oY * dd.y + oZ * dd.z) / ol : 1.0;
                angDoor = std::acos(std::min(1.0, std::max(-1.0, cs))) * 180.0 / 3.14159265;
            }
            char where[32];
            std::snprintf(where, sizeof(where), "(%+.1f,%+.1f)%s", sp.x, sp.z, (lroom == w.roomAt(S)) ? "同" : "隣");
            std::printf("        %4.0f° %14s | %9.2e | %6.1f%% %6.1f%% %6.1f%% | %6.3f %6.1f° | %6.3f\n",
                        deg, where, tot, eOwn / tot * 100.0, eOther / tot * 100.0, eUnk / tot * 100.0,
                        rOther, angDoor, rOwn);
        }
    }
    std::printf("        （R: 1 = 一点から、0 = 全方向から。戸口へ = 別部屋の平均の向きと、耳から戸口の中心への向きの角度）\n");
}

/// 【探り】扉の脇へ歩いたときの定位と跳び（AF_ONLY=doorside）
///   扉を AF_DOOR_DEG（既定 20）度で止め、耳を戸口の前で左右に振る。
///   出す物: 方向を持つタップと方向なしのタップの取り分、回折の向き、1 歩ごとの変化。
///   ★「定位が完全に消えて両耳から聞こえる」「右へずれると音が変わる瞬間がある」を数字にする。
void testDoorSide() {
    const char* dd = std::getenv("AF_DOOR_DEG");
    const float deg = dd ? static_cast<float>(std::atof(dd)) : 20.0f;
    std::printf("\n[探り] 扉 %.0f 度で脇へ歩く ── 定位の取り分と跳び\n", deg);
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    World w;
    const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
    const int leaf = w.addBox(doorLeaf(deg, -0.5f, 1.0f, 3.0f, 0.06f, 0.004f), lm, true);
    (void)leaf;
    w.raysPerEmitter = 256; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
    w.setListener(Vec3(0, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
    w.build();
    const float dt = 1.0f / 60.0f;
    std::printf("        %7s | %8s %8s %8s | %8s | %-22s | %s\n",
                "耳の x", "方向あり", "方向なし", "尾", "回折%", "回折の向き(右,上,前)", "1歩の変化");
    double prevTot = -1.0;
    float prevDir[3] = {0, 0, 0};
    for (int k = 0; k <= 24; ++k) {
        const float x = -3.0f + 0.25f * k;
        w.setListener(Vec3(x, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        for (int q = 0; q < 4; ++q) w.update(dt);
        const Mix* mx = w.mix(e);
        const Diffraction* df = w.diffraction(e);
        // タップを「方向を持つ物」と「方向なし」に分ける。尾は送りなので別に数える。
        double withDir = 0.0, noDir = 0.0, tail = 0.0, diffE = 0.0;
        for (int i = 0; i < mx->tapCount; ++i) {
            const MixTap& t = mx->taps[i];
            double se = 0.0; for (int b = 0; b < kNumBands; ++b) se += t.e6[b];
            if (length(t.dirLocal) > 0.5f) withDir += se; else noDir += se;
            if (t.kind == TapKind::Diffract) diffE += se;
        }
        for (int i = 0; i < mx->sendCount; ++i)
            for (int b = 0; b < kNumBands; ++b) tail += mx->sends[i].e6[b];
        const double tot = withDir + noDir + tail;
        if (tot <= 0.0) continue;
        // 1 歩（0.25 m）の変化: 総量の dB と、回折の向きが振れた角度
        char chg[80] = "";
        if (prevTot > 0.0) {
            const double dbStep = afti::dB(tot / prevTot);
            float dot3 = prevDir[0] * df->dirLocal.x + prevDir[1] * df->dirLocal.y + prevDir[2] * df->dirLocal.z;
            dot3 = std::min(1.0f, std::max(-1.0f, dot3));
            const float turn = (df->valid && (prevDir[0] != 0.0f || prevDir[2] != 0.0f))
                             ? std::acos(dot3) * 180.0f / 3.14159265f : 0.0f;
            std::snprintf(chg, sizeof(chg), "%+6.2f dB  向き %5.1f 度", dbStep, turn);
        }
        std::printf("        %7.2f | %7.1f%% %7.1f%% %7.1f%% | %7.2f%% | %s | %s\n",
                    x, withDir / tot * 100.0, noDir / tot * 100.0, tail / tot * 100.0, diffE / tot * 100.0,
                    df->valid ? "見つかった" : "── 無し ──", chg);
        if (df->valid) std::printf("                                                              (%+.3f %+.3f %+.3f) 箱%d/稜%d\n",
                                   df->dirLocal.x, df->dirLocal.y, df->dirLocal.z, df->box, df->edge);
        prevTot = tot;
        if (df->valid) { prevDir[0] = df->dirLocal.x; prevDir[1] = df->dirLocal.y; prevDir[2] = df->dirLocal.z; }
    }
}

/// 【探り】壁の陰を横切るときの緩衝（AF_ONLY=wallshadow）
///   扉ではなく**ただの壁**。壁の端を回り込む所でリスナーを歩かせ、
///   直接音が消えて回折に入れ替わるまでに何度かかるかを測る。
///   ★「角度による減算のバッファ角」に当たる物がどこで決まっているかを見るための物差し。
void testWallShadow() {
    std::printf("\n[探り] 壁の陰を横切る ── 直接音と回折の受け渡しに何度かかるか\n");
    auto sweep = [&](float srcR, bool header) {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.0001f; wall.scattering[b] = 0.5f; }
        World w;
        const int m = w.rules.materials.add(wall);
        // 壁 1 枚。z=0 に置き、x∈[-6,0] を塞ぐ。端は x=0。床も天井も置かない（反射を減らして見やすく）。
        w.addBox(Obb::axisAligned(Vec3(-3.0f, 2.0f, 0.0f), Vec3(3.0f, 2.0f, 0.15f)), m, false);
        const Vec3 S(-2.0f, 1.6f, 3.0f);
        w.raysPerEmitter = 64; w.rayGroups = 1; w.budget.cfg.totalRays = 0;
        w.setListener(Vec3(0, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(S, srcR);
        w.build();
        // 影の境: 音源 S から壁の端(0,0) を通る直線を z=-3 まで伸ばした所。
        //   S=(-2,3) → 端(0,0) の方向は (2,-3)。z=-3 まで同じだけ進むので x=+2。
        // ★壁は厚み 0.3 m ある。影を広く落とすのは**奥の角**(z=+0.15) なので、そこを通る線で境を出す。
        //   z=0 で計算すると 4° ずれる（実測でそのぶん偏って見えた）。
        const float boundaryX = 2.0f * (3.0f + 0.15f) / (3.0f - 0.15f);
        const float dEdge = 3.0f;                       // 端からリスナーの面までの距離（z 方向）
        const float dt = 1.0f / 60.0f;
        float lo = 999.0f, hi = 999.0f;
        if (header)
            std::printf("        %8s %8s %9s %9s %9s %9s | %s\n",
                        "耳のx", "境から°", "見通し", "直接dB", "回折dB", "和dB", "回折の δ");
        for (int k = 0; k <= 40; ++k) {
            const float x = 0.0f + 0.1f * k;             // 影の中(0) から明るい側(+4) へ
            w.setListener(Vec3(x, 1.6f, -3.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
            for (int q = 0; q < 3; ++q) w.update(dt);    // 追従を落ち着かせる
            const Mix* mx = w.mix(e);
            const Visibility* vs = w.visibility(e);
            const Diffraction* df = w.diffraction(e);
            double dir = 0.0, dif = 0.0;
            for (int b = 0; b < kNumBands; ++b) { dir += mx->component6[kDirect][b]; dif += mx->component6[kDiffract][b]; }
            // 影の境からの角度（端を頂点として、境の方向とリスナー方向のなす角）
            const float degFromBoundary = std::atan2(x - boundaryX, dEdge) * 180.0f / 3.14159265f;
            if (vs->visible >= 0.05f && lo > 90.0f) lo = degFromBoundary;
            if (vs->visible >= 0.95f && hi > 90.0f) hi = degFromBoundary;
            std::printf("        %8.2f %8.2f %9.4f %9.2f %9.2f %9.2f | %.4f\n",
                        x, degFromBoundary, vs->visible, afti::dB(dir), afti::dB(dif), afti::dB(dir + dif),
                        df->valid ? df->delta : 0.0f);
        }
        std::printf("        → 緩衝の幅: 見通し 5%% (%.2f°) から 95%% (%.2f°) まで **%.1f 度**（音源の幅 %.2f m）\n",
                    lo, hi, hi - lo, srcR);
    };
    std::printf("      音源の幅 0.20 m（既定）:\n");
    sweep(0.20f, true);
    std::printf("      音源の幅 0.80 m:\n");
    sweep(0.80f, false);
}

/// 【探り】扉の角度ごとに成分の生の量を出す（段差の出所を見るための測り）
void testDoorSweep() {
    std::printf("\n[探り] 扉の角度ごとの成分（生・帯域幅平均でなく単純和。段差の出所）\n");
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    World w;
    const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
    const int leaf = w.addBox(doorLeaf(0.0f), lm, true);
    // ★AF_RAYS は「1 音源の本数」を直に決める（上限も一緒に上げる）。
    //   AF_MAX_PER だけを上げても、予算の元になる raysPerEmitter が 256 のままなので増えない。
    { const char* r = std::getenv("AF_RAYS"); const int nr = r ? std::atoi(r) : 256;
      w.raysPerEmitter = nr; w.budget.cfg.maxPerEmitter = std::max(nr, w.budget.cfg.maxPerEmitter); }
    if (const char* wr = std::getenv("AF_WALL_REFLECT")) w.wallReflect = std::atoi(wr);   // 壁越しの反射 0/1
    if (const char* em = std::getenv("AF_EARLY_MODEL")) w.earlyModel = std::atoi(em);   // 初期反射 0 虚像 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網
    { const char* g = std::getenv("AF_GROUPS"); w.rayGroups = g ? std::atoi(g) : 4; }   // Unity と同じ既定 4
    if (const char* lk = std::getenv("AF_LEAK")) w.leakModel = std::atoi(lk);   // 漏れの模型 0/1/2/3
    { const char* g = std::getenv("AF_GROUPS"); w.rayGroups = g ? std::atoi(g) : 1;
      const char* bd = std::getenv("AF_BUDGET"); w.budget.cfg.totalRays = bd ? std::atoi(bd) : 0; }
    { const char* cv = std::getenv("AF_COMP");
      if (cv) { const int keep = std::atoi(cv); for (int c = 0; c < kNumComponents; ++c) w.rules.weights.w[c] = (c == keep) ? 1.0f : 0.0f; } }
    w.setListener(Vec3(0, 1.6f, -4.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
    w.build();
    const int fs = 48000, block = 512;
    const float dt = static_cast<float>(block) / fs;
    af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
    w.bindFdn(&fdn);
    af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
    af::dsp::VoiceRenderer v(vc);
    v.setOutputGain(1.0f); v.setTailLevel(1.0f); v.setFdnMix(&fdn);
    af::dsp::HrtfSet hrtf = af::dsp::HrtfSet::createSynthetic(fs);
    af::dsp::DirectionBus bus(fs, 8, block);
    v.setHrtfEnabled(true); v.setHrtfSet(&hrtf);
    bus.setHrtfSet(&hrtf, 57.0f); v.setDirectionBus(&bus); fdn.setDirectionBus(&bus, 57.0f);
    afti::SineSum sig(fs, block);
    std::vector<float> sin_(block), sl(block), sr(block), gl(block), gr(block), smix(block);
    auto envF = [](const char* k, float d) { const char* v = std::getenv(k); return v ? static_cast<float>(std::atof(v)) : d; };
    const float from = envF("AF_FROM", 45.0f), to = envF("AF_TO", 75.0f), stepDeg = envF("AF_STEP", 0.32f);
    std::printf("        %6s %8s %10s %10s %10s %10s %10s %8s %8s %3s | %s\n",
                "角度", "見通し", "直接", "回折", "透過", "初期", "後期", "帳簿dB", "実音dB", "本", "回折の稜線 δ 隙間 重み");
    double prev = -1.0, prevR = -1.0, pV = -1.0, pF = -1.0, pB = -1.0;
    const bool quiet = std::getenv("AF_LOUD") == nullptr;   // 既定は要約だけ。AF_LOUD=1 で全行
    double wR = 0.0, wL = 0.0; float wRd = -1.0f, wLd = -1.0f; int rowN = 0;
    const bool walk = std::getenv("AF_WALK") != nullptr;   // 回帰と同じ足取り（扉 30°/s と 1.4 m/s を連動）
    for (float deg = from; deg <= to + 1e-4f; deg += stepDeg) {
        if (walk) w.setListener(Vec3(0, 1.6f, -4.0f + 1.4f * (deg / 30.0f)), Vec3(0, 0, 1), Vec3(0, 1, 0));
        w.setBoxTransform(leaf, doorLeaf(deg));
        w.update(dt);
        const Mix* mx = w.mix(e); const Visibility* vs = w.visibility(e); const Diffraction* df = w.diffraction(e);
        double tot = 0.0, cm[kNumComponents] = {};
        for (int c = 0; c < kNumComponents; ++c) { for (int b = 0; b < kNumBands; ++b) cm[c] += mx->component6[c][b]; tot += cm[c]; }
        const double step = (prev > 0.0 && tot > 0.0) ? afti::dB(tot / prev) : 0.0;
        // 実際に鳴らして、そのブロックの実効値も並べる（帳簿が滑らかでも描画が跳ぶことがある）
        w.applyToVoice(e, v, fs);
        for (int i = 0; i < block; ++i) sin_[static_cast<std::size_t>(i)] = sig.next();
        v.render(sin_.data(), block, sl.data(), sr.data(), nullptr);
        std::fill(gl.begin(), gl.end(), 0.0f); std::fill(gr.begin(), gr.end(), 0.0f);
        fdn.render(block, gl.data(), gr.data());
        const double eVoice = afti::energy(sl.data(), block) / block, eFdn = afti::energy(gl.data(), block) / block;
        for (int i = 0; i < block; ++i) smix[static_cast<std::size_t>(i)] = sl[static_cast<std::size_t>(i)] + gl[static_cast<std::size_t>(i)];
        std::fill(gl.begin(), gl.end(), 0.0f); std::fill(gr.begin(), gr.end(), 0.0f);
        bus.render(block, gl.data(), gr.data());
        const double eBus = afti::energy(gl.data(), block) / block;
        for (int i = 0; i < block; ++i) smix[static_cast<std::size_t>(i)] += gl[static_cast<std::size_t>(i)];
        const double re = afti::energy(smix.data(), block) / block;
        const double rstep = (prevR > 0.0 && re > 0.0) ? afti::dB(re / prevR) : 0.0;
        const double sVoice = (pV > 0.0 && eVoice > 0.0) ? afti::dB(eVoice / pV) : 0.0;
        const double sFdn = (pF > 0.0 && eFdn > 0.0) ? afti::dB(eFdn / pF) : 0.0;
        const double sBus = (pB > 0.0 && eBus > 0.0) ? afti::dB(eBus / pB) : 0.0;
        pV = eVoice; pF = eFdn; pB = eBus;
        if (!quiet) std::printf("        %6.2f %8.4f %10.3e %10.3e %10.3e %10.3e %10.3e %+8.2f %+8.2f %3d | 声%+6.2f 尾%+6.2f 束%+6.2f | %d/%d d=%.3f a=%.3f w=%.2f\n",
                    deg, vs->visible, cm[kDirect], cm[kDiffract], cm[kTransmit], cm[kEarly], cm[kLate], step, rstep, mx->tapCount, sVoice, sFdn, sBus,
                    df->valid ? df->box : -1, df->valid ? df->edge : -1, df->valid ? df->delta : 0.0f,
                    df->valid ? df->gapWidth : 0.0f, df->valid ? df->weight : 0.0f);
        if (!quiet) std::printf("             回折の点 (%.3f %.3f %.3f) 向き (%+.3f %+.3f %+.3f) 経路 %.4fs\n",
                    df->point.x, df->point.y, df->point.z, df->dirLocal.x, df->dirLocal.y, df->dirLocal.z, df->pathSec);
        const ImageSet* ims = w.images(e);
        if (!quiet) std::printf("             虚像 %d:", ims->count);
        for (int i = 0; quiet ? false : (i < ims->count && i < 8); ++i)
            std::printf("  [%d次 %.3fs 妥当%.4f 重み%.4f]", ims->img[i].order, ims->img[i].pathSec, ims->img[i].validity, ims->img[i].weight6[2]);
        if (!quiet) std::printf("\n");
        if (++rowN > 20) { if (std::fabs(rstep) > wR) { wR = std::fabs(rstep); wRd = deg; }
                           if (std::fabs(step) > wL) { wL = std::fabs(step); wLd = deg; } }
        prev = tot; prevR = re;
    }
    std::printf("        最悪: 実音 %.2f dB（%.1f 度）／帳簿 %.2f dB（%.1f 度）\n", wR, wRd, wL, wLd);
}

/// 【探り】扉の開き角 × 音源の方向の地図（AF_ONLY=doormap）── 最終資料用
///   ★場面は Unity の Flow_SwingDoor と同じ（検査の場面を実機と突き合わせる）:
///     14×3×14 m を厚さ 0.2 m のコンクリートで仕切り、幅 1 m の戸口。木の扉 1×3×0.06 m、枠との隙間 4 mm、
///     蝶番は左枠（x = −0.5）で +Z（向こうの部屋）へ振れる。耳 (0, 1.6, −3) で +Z を向く。音源は幅 0.2 m。
///     （回帰の twoRoomsWithDoor は壁が 0.4 m 厚なので使わない）
///   音源は戸口の中心から 3 m の円の上。方位 φ は耳から見て **負 = 蝶番の側（左）、正 = 自由端の側（右）**。
///   出す物（どれも自由音場の直接音に対する比。帳簿の生の値なので平滑も重みも入らない）:
///     合計 dB（帯域の単純平均）／低−高（125 Hz − 4 kHz、正でこもる）／到来の方位（エネルギーで重み付けした向き）／
///     主な経路（直接・透過・回折のうち最大）／回折の出どころ（板の稜線か、枠の稜線か）
///   AF_CSV=パス で全セルを CSV に書く（資料の図用）。
void testDoorMap() {
    std::printf("\n[探り] 扉の開き角 × 音源の方向（Flow_SwingDoor と同じ場面）\n");
    const float half = 7.0f, h = 3.0f, t = 0.2f, gapL = -0.5f, gapR = 0.5f;
    World w;
    const int mWall = w.rules.materials.add(AcousticMaterial::concrete());
    const int mLeaf = w.rules.materials.add(AcousticMaterial::woodDoor());
    auto box = [&](Vec3 c, Vec3 size) { w.addBox(Obb::axisAligned(c, size * 0.5f), mWall, false); };
    box(Vec3(0, -t * 0.5f, 0), Vec3(2 * half + 2 * t, t, 2 * half + 2 * t));
    box(Vec3(0, h + t * 0.5f, 0), Vec3(2 * half + 2 * t, t, 2 * half + 2 * t));
    box(Vec3(-half - t * 0.5f, h * 0.5f, 0), Vec3(t, h, 2 * half + 2 * t));
    box(Vec3(half + t * 0.5f, h * 0.5f, 0), Vec3(t, h, 2 * half + 2 * t));
    box(Vec3(0, h * 0.5f, -half - t * 0.5f), Vec3(2 * half + 2 * t, h, t));
    box(Vec3(0, h * 0.5f, half + t * 0.5f), Vec3(2 * half + 2 * t, h, t));
    const float leftW = gapL + half, rightW = half - gapR;
    box(Vec3(-half + leftW * 0.5f, h * 0.5f, 0), Vec3(leftW, h, t));
    box(Vec3(gapR + rightW * 0.5f, h * 0.5f, 0), Vec3(rightW, h, t));
    const int leaf = w.addBox(doorLeaf(0.0f, gapL, gapR - gapL, h, 0.06f, 0.004f), mLeaf, true);
    w.raysPerEmitter = 32; w.rayGroups = 1; w.budget.cfg.totalRays = 0; w.imageOrder = 1;
    const Vec3 L(0, 1.6f, -3.0f);
    w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
    const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
    w.build();
    const float dt = 1.0f / 60.0f;

    // AF_DEGS / AF_PHIS（コンマ区切り）で格子を差し替えられる。段が刻みの粗さか本物かを細かく見るとき用
    auto parseList = [](const char* env, std::vector<float> def) {
        const char* s = std::getenv(env);
        if (!s) return def;
        std::vector<float> v;
        for (const char* q = s; *q; ) { v.push_back(static_cast<float>(std::atof(q))); const char* c = std::strchr(q, ','); if (!c) break; q = c + 1; }
        return v.empty() ? def : v;
    };
    const std::vector<float> degs = parseList("AF_DEGS", {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90});
    const std::vector<float> phis = parseList("AF_PHIS", {-75, -60, -45, -30, -15, 0, 15, 30, 45, 60, 75});
    const float* kDeg = degs.data(); const float* kPhi = phis.data();
    const int nD = static_cast<int>(degs.size()), nP = static_cast<int>(phis.size());
    struct Cell { double tot, lowHigh, az, spread, share[3], db3[3], gap, dPtX; int main, src; float vis; };
    std::vector<Cell> cells(static_cast<std::size_t>(nD * nP));
    const int bLow = 0, bMid = 3, bHigh = 5;

    for (int ip = 0; ip < nP; ++ip) {
        const float ph = kPhi[ip] * 3.14159265f / 180.0f;
        const Vec3 S(3.0f * std::sin(ph), 1.6f, 3.0f * std::cos(ph));
        w.setEmitter(e, S, 0.2f, false, 1.0f);
        for (int id = 0; id < nD; ++id) {
            w.setBoxTransform(leaf, doorLeaf(kDeg[id], gapL, gapR - gapL, h, 0.06f, 0.004f));
            w.update(dt); w.update(dt);
            const Mix* mx = w.mix(e); const TraceResult* tr = w.trace(e); const Diffraction* df = w.diffraction(e);
            Cell& c = cells[static_cast<std::size_t>(id * nP + ip)];
            // 帯域ごとに自由音場で割る（空気吸収も距離も消える）
            double comp[3][kNumBands] = {}, sum[3] = {};
            static const int kC[3] = {kDirect, kTransmit, kDiffract};
            for (int k = 0; k < 3; ++k)
                for (int b = 0; b < kNumBands; ++b) {
                    const double f = std::max(1e-30, static_cast<double>(tr->freeDirect6[b]));
                    comp[k][b] = mx->component6[kC[k]][b] / f;
                    sum[k] += comp[k][b] / kNumBands;
                }
            const double tot = sum[0] + sum[1] + sum[2];
            c.tot = afti::dB(std::max(tot, 1e-30));
            auto bandDb = [&](int b) { return afti::dB(std::max(comp[0][b] + comp[1][b] + comp[2][b], 1e-30)); };
            c.db3[0] = bandDb(bLow); c.db3[1] = bandDb(bMid); c.db3[2] = bandDb(bHigh);
            c.lowHigh = c.db3[0] - c.db3[2];
            for (int k = 0; k < 3; ++k) c.share[k] = (tot > 0.0) ? sum[k] / tot : 0.0;
            c.main = (sum[0] >= sum[1] && sum[0] >= sum[2]) ? 0 : (sum[1] >= sum[2] ? 1 : 2);
            // 到来の向き: 直接と透過は音源の向き、回折は回折点の向き。エネルギーで重み付けして足す
            Vec3 toS = w.listener().toLocal(S - L); toS = toS * (1.0f / std::max(1e-6f, length(toS)));
            Vec3 acc = toS * static_cast<float>(sum[0] + sum[1]);
            if (df->valid) acc = acc + df->dirLocal * static_cast<float>(sum[2]);
            c.az = std::atan2(acc.x, acc.z) * 180.0 / 3.14159265;
            c.spread = (tot > 0.0) ? 1.0 - length(acc) / tot : 0.0;
            c.vis = w.visibility(e)->visible;
            c.gap = df->valid ? df->gapWidth : 0.0;
            c.dPtX = df->valid ? df->point.x : 0.0;
            c.src = df->valid ? (df->box == leaf ? 1 : 2) : 0;   // 1 板の稜線 / 2 枠（仕切り）の稜線
        }
    }

    auto header = [&](const char* title) {
        std::printf("\n      %s\n      %6s |", title, "角度＼φ");
        for (int ip = 0; ip < nP; ++ip) std::printf(" %+5.0f", kPhi[ip]);
        std::printf("\n");
    };
    auto grid = [&](const char* title, const std::function<void(const Cell&)>& put) {
        header(title);
        for (int id = 0; id < nD; ++id) {
            std::printf("      %6.1f |", kDeg[id]);
            for (int ip = 0; ip < nP; ++ip) put(cells[static_cast<std::size_t>(id * nP + ip)]);
            std::printf("\n");
        }
    };
    grid("合計 dB（自由音場比。φ 負 = 蝶番の側）", [](const Cell& c) { std::printf(" %5.1f", c.tot); });
    grid("低 − 高 dB（125 Hz − 4 kHz。正でこもる）", [](const Cell& c) { std::printf(" %+5.1f", c.lowHigh); });
    grid("到来の方位（度、耳から見て。正 = 右）", [](const Cell& c) { std::printf(" %+5.0f", c.az); });
    grid("主な経路（D 直接 / T 透過 / d 回折）と回折の出どころ（p 板 / f 枠 / - 無し）", [](const Cell& c) {
        static const char kM[3] = {'D', 'T', 'd'}; static const char kS[3] = {'-', 'p', 'f'};
        std::printf("   %c%c ", kM[c.main], kS[c.src]); });
    grid("見通しの割合", [](const Cell& c) { std::printf(" %5.2f", c.vis); });

    // 蝶番の非対称: 同じ角度で φ と −φ の合計の差（自由端の側 − 蝶番の側）。φ の並びが 0 を挟んで対称なときだけ
    bool symmetric = (nP % 2 == 1);
    for (int ip = 0; symmetric && ip < nP; ++ip) if (std::fabs(kPhi[ip] + kPhi[nP - 1 - ip]) > 1e-3f) symmetric = false;
    if (symmetric) {
        std::printf("\n      蝶番の非対称（合計 dB: 自由端の側 +φ − 蝶番の側 −φ）\n      %6s |", "角度＼|φ|");
        for (int ip = nP / 2 + 1; ip < nP; ++ip) std::printf(" %5.0f", kPhi[ip]);
        std::printf("\n");
        for (int id = 0; id < nD; ++id) {
            std::printf("      %6.1f |", kDeg[id]);
            for (int ip = nP / 2 + 1; ip < nP; ++ip) {
                const Cell& r = cells[static_cast<std::size_t>(id * nP + ip)];
                const Cell& l = cells[static_cast<std::size_t>(id * nP + (nP - 1 - ip))];
                std::printf(" %+5.1f", r.tot - l.tot);
            }
            std::printf("\n");
        }
    }

    if (const char* path = std::getenv("AF_CSV")) {
        if (FILE* fp = std::fopen(path, "w")) {
            std::fprintf(fp, "deg,phi,total_db,db125,db1k,db4k,low_minus_high_db,arrival_az_deg,spread,share_direct,share_transmit,share_diffract,main,diff_src,visible,gap_m,diff_point_x\n");
            for (int id = 0; id < nD; ++id)
                for (int ip = 0; ip < nP; ++ip) {
                    const Cell& c = cells[static_cast<std::size_t>(id * nP + ip)];
                    std::fprintf(fp, "%.2f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.4f,%.5f,%.5f,%.5f,%d,%d,%.4f,%.4f,%.4f\n",
                                 kDeg[id], kPhi[ip], c.tot, c.db3[0], c.db3[1], c.db3[2], c.lowHigh, c.az, c.spread,
                                 c.share[0], c.share[1], c.share[2], c.main, c.src, c.vis, c.gap, c.dPtX);
                }
            std::fclose(fp);
            std::printf("\n      CSV: %s\n", path);
        }
    }
}

void testClicks() {
    std::printf("\n[ぷつぷつ] 鳴らしながら波形の不連続を測る（正弦の和・ブロック同期）\n");
    const int fs = 48000, block = 512;
    const float dt = static_cast<float>(block) / fs;
    char buf[240];

    // 台本: 扉のある 2 部屋。リスナーが戸口へ 1.4 m/s で歩き、同時に扉が 30°/s で開く。
    //   ★Unity と同じ配線で測る（HRTF と方向バスを繋ぐ）。ここを外すと harness では跳ねが出ず、
    //     実機だけで「ぶつぶつ」になる ── 段 9-b で実際にそうなった。
    //   which: 0 全部 / 1 虚像なし / 2 分散なし / 3 予算なし / 4 方向バスなし / 5 HRTF なし
    //          6..10 成分をひとつだけ残す（直接・初期・後期・回折・透過）
    //   motion: 0 歩き＋扉 / 1 歩きだけ / 2 扉だけ / 3 静止
    auto run = [&](int which, int motion, double* outCalm, double* outWorst, int* outSpikes, double* outBlockStep) {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
        World w;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        const float clear = std::getenv("AF_CLEAR") ? static_cast<float>(std::atof(std::getenv("AF_CLEAR"))) : 0.0f;   // 枠との隙間（Unity は 4 mm）
        const int leaf = w.addBox(doorLeaf(0.0f, -0.5f, 1.0f, 3.0f, 0.06f, clear), lm, true);
    // ★AF_RAYS は「1 音源の本数」を直に決める（上限も一緒に上げる）。
    //   AF_MAX_PER だけを上げても、予算の元になる raysPerEmitter が 256 のままなので増えない。
    { const char* r = std::getenv("AF_RAYS"); const int nr = r ? std::atoi(r) : 256;
      w.raysPerEmitter = nr; w.budget.cfg.maxPerEmitter = std::max(nr, w.budget.cfg.maxPerEmitter); }
    { const char* g = std::getenv("AF_GROUPS"); w.rayGroups = g ? std::atoi(g) : 4; }   // Unity と同じ既定 4
    if (const char* lk = std::getenv("AF_LEAK")) w.leakModel = std::atoi(lk);   // 漏れの模型 0/1/2/3
    if (const char* wr = std::getenv("AF_WALL_REFLECT")) w.wallReflect = std::atoi(wr);   // 壁越しの反射 0/1
    if (const char* em = std::getenv("AF_EARLY_MODEL")) w.earlyModel = std::atoi(em);   // 初期反射 0 虚像 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網
    if (const char* ac = std::getenv("AF_CONTAIN")) w.adjacentContain = static_cast<float>(std::atof(ac));   // 隣の部屋の閉じ込め 0..1
        if (const char* lt = std::getenv("AF_LATE_THROUGH")) w.lateThrough = std::atoi(lt);   // 戸口越しの後期 0/1（段 2-f）
        w.budget.cfg.totalRays = (which == 3) ? 0 : 1536;
        w.rayGroups = (which == 2 || which == 3) ? 1 : 4;
        if (which == 1) w.budget.cfg.fullSlots = 0;                    // 簡易に落として虚像を作らせない
        if (which >= 6) for (int c = 0; c < kNumComponents; ++c) w.rules.weights.w[c] = (c == which - 6) ? 1.0f : 0.0f;
        Vec3 L(0, 1.6f, -4.0f);
        w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0));
        const int e = w.addEmitter(Vec3(0, 1.6f, 3.0f), 0.2f);
        w.build();
        af::dsp::FdnRoomMix fdn(fs, block, 0.6f);
        w.bindFdn(&fdn);
        af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
        af::dsp::VoiceRenderer v(vc);
        v.setOutputGain(1.0f); v.setTailLevel(1.0f);
        v.setFdnMix(&fdn);
        // Unity と同じ配線: HRTF（合成）と方向バス（8 レーン）
        af::dsp::HrtfSet hrtf = af::dsp::HrtfSet::createSynthetic(fs);
        af::dsp::DirectionBus bus(fs, 8, block);
        const bool useBus = (which != 4), useHrtf = (which != 5);
        v.setHrtfEnabled(useHrtf);
        if (useHrtf) v.setHrtfSet(&hrtf);
        if (useBus) { bus.setHrtfSet(&hrtf, 57.0f); v.setDirectionBus(&bus); fdn.setDirectionBus(&bus, 57.0f); }
        afti::SineSum sig(fs, block);
        std::vector<float> in(block), l(block), r(block), fl(block), fr(block), mix(block);
        double calm = 0.0, worst = 0.0, worstBlock = 0.0; int spikes = 0, nb = 0; float worstAtDeg = -1.0f, worstAtZ = 0.0f;
        double prevE = -1.0;
        const int frames = 240;                              // 4 秒
        for (int k = 0; k < frames; ++k) {
            const float t = k * dt;
            if (k > 30) {                                     // 立ち上がりを流してから動かす
                const float el = t - 30 * dt;
                if (motion == 0 || motion == 1) { L.z = -4.0f + 1.4f * el; w.setListener(L, Vec3(0, 0, 1), Vec3(0, 1, 0)); }
                if (motion == 0 || motion == 2) w.setBoxTransform(leaf, doorLeaf(std::min(90.0f, 30.0f * el), -0.5f, 1.0f, 3.0f, 0.06f, clear));
            }
            w.update(dt);
            if (w.fdnStale()) { w.bindFdn(&fdn); }
            w.applyToVoice(e, v, fs);
            for (int i = 0; i < block; ++i) in[static_cast<std::size_t>(i)] = sig.next();
            v.render(in.data(), block, l.data(), r.data(), nullptr);
            std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
            fdn.render(block, fl.data(), fr.data());          // FDN → 方向バス（バスがあるとき）
            for (int i = 0; i < block; ++i) mix[static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)] + fl[static_cast<std::size_t>(i)];
            if (useBus) {
                std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
                bus.render(block, fl.data(), fr.data());       // Unity では AudioListener で 1 回
                for (int i = 0; i < block; ++i) mix[static_cast<std::size_t>(i)] += fl[static_cast<std::size_t>(i)];
            }
            if (k < 40) continue;                             // 器の立ち上がりは測らない
            const double s = afti::sampleStepRatio(mix.data(), block);
            const double e2 = afti::energy(mix.data(), block) / block;
            if (nb < 20) calm = std::max(calm, s);            // 動き出す前の平常値
            else {
                if (s > worst) worst = s;
                if (s > 4.0 * std::max(calm, 1e-6)) ++spikes;
                if (prevE > 0.0 && e2 > 0.0) {
                    const double st = std::fabs(afti::dB(e2 / prevE));
                    if (st > worstBlock) { worstBlock = st; worstAtDeg = std::min(90.0f, 30.0f * (t - 30 * dt)); worstAtZ = L.z; }
                    // AF_STEP_LOG=<dB>: その段より大きいブロックを角度つきで出す（全部の成分・扉だけのとき）
                    if (which == (std::getenv("AF_STEP_WHICH") ? std::atoi(std::getenv("AF_STEP_WHICH")) : 0) && motion == (std::getenv("AF_STEP_MOTION") ? std::atoi(std::getenv("AF_STEP_MOTION")) : 2) && std::getenv("AF_STEP_LOG") && st > std::atof(std::getenv("AF_STEP_LOG")))
                        std::printf("        段 %+.2f dB 扉 %.1f° 耳 z %.2f（ブロック %d）\n", afti::dB(e2 / prevE), std::min(90.0f, 30.0f * (t - 30 * dt)), L.z, nb);
                }
            }
            prevE = e2;
            ++nb;
        }
        if (which == 0 && (motion == 0 || motion == 2))
            std::printf("      %s: 最悪の段差 %.2f dB は 扉 %.1f 度・耳 z=%.2f で\n",
                        (motion == 2) ? "扉だけ" : "歩き＋扉", worstBlock, worstAtDeg, worstAtZ);
        *outCalm = calm; *outWorst = worst; *outSpikes = spikes; *outBlockStep = worstBlock;
    };

    double calm, worst, blockStep; int spikes;

    run(0, 3, &calm, &worst, &spikes, &blockStep);
    std::snprintf(buf, sizeof buf, "静止: ブロック間の段差 %.4f dB（≤ 0.01）", blockStep);
    check(buf, blockStep <= 0.01);

    run(0, 1, &calm, &worst, &spikes, &blockStep);
    std::snprintf(buf, sizeof buf, "歩き 1.4 m/s: 跳ね %.1f 倍・%d 回、段差 %.2f dB", worst / std::max(calm, 1e-6), spikes, blockStep);
    check(buf, spikes == 0);

    run(0, 0, &calm, &worst, &spikes, &blockStep);
    std::snprintf(buf, sizeof buf, "歩き＋扉 30°/s: 跳ね %.1f 倍・%d 回、段差 %.2f dB", worst / std::max(calm, 1e-6), spikes, blockStep);
    check(buf, spikes == 0);

    // ── 切り分け（何を外すと段差が減るか）。44 通り回すので既定では出さない（AF_LOUD=1 で出す）──
    if (std::getenv("AF_LOUD") == nullptr) return;
    std::printf("      切り分け（ブロック間の段差 dB。★方向バス × HRTF の組で ITD の丸めが出る）:\n");
    std::printf("        %-22s %8s %8s %8s %8s\n", "", "静止", "歩き", "扉", "歩き+扉");
    static const char* kName[11] = {"全部", "虚像なし（簡易）", "分散なし", "予算なし・分散なし", "方向バスなし", "HRTF なし",
                                    "直接だけ", "初期だけ", "後期だけ", "回折だけ", "透過だけ"};
    for (int which = 0; which < 11; ++which) {
        double bs[4]; double c, w2; int sp;
        for (int m = 0; m < 4; ++m) run(which, (m == 0) ? 3 : (m == 1) ? 1 : (m == 2) ? 2 : 0, &c, &w2, &sp, &bs[m]);
        std::printf("        %-22s %8.3f %8.3f %8.3f %8.3f\n", kName[which], bs[0], bs[1], bs[2], bs[3]);
    }
}


}  // namespace

int main() {
    std::printf("=== 新コア（Flow）の数値回帰テスト ===\n");
    const char* only = std::getenv("AF_ONLY");
    struct { const char* name; void (*fn)(); bool optIn; } suites[] = {
        {"rules", testWorldRules}, {"probe", testProbe}, {"emitter", testEmitter}, {"mix", testMix}, {"instruments", testInstruments},
        {"trace", testEnergyTrace}, {"response", testResponse}, {"distribute", testDistribute},
        {"world", testWorld}, {"bridge", testBridge}, {"aperture", testAperture}, {"diffraction", testDiffraction}, {"images", testImageSources},
        {"budget", testBudget}, {"raycap", testRayCap}, {"material", testMaterialChange}, {"wall", testWallReflect}, {"precedence", testPrecedence}, {"recvworld", testReceiverWorld}, {"imgface", testImageFaces}, {"imglattice", testImageLattice}, {"contain", testAdjacentContain}, {"clicks", testClicks}, {"gpu", testGpuPipe}, {"leak", testLeakModels}, {"latedir", testLateThrough}, {"doorline", testDoorLineSource},
        {"doorsweep", testDoorSweep, true}, {"wallshadow", testWallShadow, true}, {"doorside", testDoorSide, true}, {"manysrc", testManySources, true}, {"nearwall", testNearWall, true}, {"lateorigin", testLateOrigin, true}, {"doorprobe", testDoorProbe, true}, {"doorcoh", testDoorCoherence, true}, {"receiver", testReceiverFaces, true}, {"recvtaps", testReceiverTaps, true}, {"recvnear", testReceiverNear, true}, {"recvdoor", testReceiverDoor, true}, {"doormap", testDoorMap, true},   // 探り。名指しのときだけ（AF_ONLY=doorsweep）
    };
    for (const auto& s : suites) {
        // AF_ONLY はコンマ区切りで複数指定できる（例 AF_ONLY=world,bridge）
        bool named = false;
        if (only) {
            const std::size_t ln = std::strlen(s.name);
            for (const char* q = only; *q; ) {
                const char* c = std::strchr(q, ',');
                const std::size_t len = c ? static_cast<std::size_t>(c - q) : std::strlen(q);
                if (len == ln && std::strncmp(q, s.name, ln) == 0) { named = true; break; }
                if (!c) break;
                q = c + 1;
            }
        }
        // 組の名前は stderr へ（stdout は溜まるので、落ちたときに最後に見えるのは古い行になる。
        //  段 9-b で bridge の解放後使用を追ったとき、これが無くて場所が分からなかった）。
        if (named || (!only && !s.optIn)) { std::fprintf(stderr, "-> %s\n", s.name); s.fn(); std::fflush(stdout); }
    }
    return afti::finish("Flow");
}
