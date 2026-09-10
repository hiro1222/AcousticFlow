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
Obb doorLeaf(float thetaDeg, float gapL = -0.5f, float w = 1.0f, float h = 3.0f, float thick = 0.06f) {
    const float th = thetaDeg * 3.14159265f / 180.0f;
    const Vec3 right(std::cos(th), 0.0f, std::sin(th));
    const Vec3 hinge(gapL, h * 0.5f, 0.0f);
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
            timeIt("見通し（幅 0.2）", [&] { volatile auto v = discVisibility(box, mats, lis.pos, S, 0.2f); (void)v; });
            Visibility vv = discVisibility(box, mats, lis.pos, S, 0.2f);
            timeIt("回折", [&] { volatile auto d = edgeDiffraction(box, lis, S, vv); (void)d; });
            ImageSet im;
            timeIt("虚像（1・2 次）", [&] { buildImages(box, faces, lis, S, 0.2f, 0.06f, im); });
            std::printf("        虚像: 検討 %d / 有効 %d\n", im.candidates, im.count);
        }
    }
}

// ================================ [漏れ] 閉じた扉から回折が漏れるか（AF_ONLY=leak）
void testLeakModels() {
    std::printf("\n[漏れ] 閉じた扉の回折 ── 模型 0 今のまま / 1 厚みの割合 / 2 口の空き具合 / 3 両方\n");
    char buf[220];
    // 場面は回帰の 2 部屋。扉は閉じたまま、耳を戸口の前で左右に振る。
    //   ★「漏れが聞こえる位置と聞こえない位置がある」という試聴報告を数字にする物差し。
    auto sweep = [&](int model, double* outMax, int* outOn, int* outOff) {
        AcousticMaterial wall = AcousticMaterial::defaultWall();
        for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
        World w;
        w.leakModel = model;
        const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
        for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
        w.addBox(doorLeaf(0.0f), lm, true);                 // 閉じたまま動かさない
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
            if (d > tr) ++on; else ++off;                    // 板を透るぶんより回折が大きい ＝ 漏れが立っている
            mx = std::max(mx, d / std::max(tr, 1e-30));
        }
        *outMax = mx; *outOn = on; *outOff = off;
    };
    for (int model = 0; model <= 3; ++model) {
        double mx; int on, off;
        sweep(model, &mx, &on, &off);
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

/// 【探り】扉の角度ごとに成分の生の量を出す（段差の出所を見るための測り）
void testDoorSweep() {
    std::printf("\n[探り] 扉の角度ごとの成分（生・帯域幅平均でなく単純和。段差の出所）\n");
    AcousticMaterial wall = AcousticMaterial::defaultWall();
    for (int b = 0; b < kNumBands; ++b) { wall.absorption[b] = 0.2f; wall.transmission[b] = 0.001f; wall.scattering[b] = 0.5f; }
    World w;
    const int m2 = w.rules.materials.add(wall), lm = w.rules.materials.add(AcousticMaterial::woodDoor());
    for (const rooms::SolidBox& sb : twoRoomsWithDoor(wall)) w.addBox(sb.obb, m2, false);
    const int leaf = w.addBox(doorLeaf(0.0f), lm, true);
    w.raysPerEmitter = 256;
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
        const int leaf = w.addBox(doorLeaf(0.0f), lm, true);
        w.raysPerEmitter = 256;
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
                if (motion == 0 || motion == 2) w.setBoxTransform(leaf, doorLeaf(std::min(90.0f, 30.0f * el)));
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
        {"budget", testBudget}, {"clicks", testClicks}, {"leak", testLeakModels},
        {"doorsweep", testDoorSweep, true},   // 探り。名指しのときだけ（AF_ONLY=doorsweep）
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
