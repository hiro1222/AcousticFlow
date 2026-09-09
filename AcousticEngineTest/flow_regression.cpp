/* AcousticEngineTest/flow_regression.cpp ── 新コアの検査（段 1: 形式）
 *
 * 期待値は「絶対値」でなく「関係」で書く（保存則、単調、一致、静止で 0）。
 * 段が進むごとにここへ足す。AF_ONLY=<name> で 1 つだけ走らせられる。
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    // 直接音: 見通しで 1/(4πd²)
    const float d = length(L - S);
    const float expect = 1.0f / (4.0f * 3.14159265f * d * d);
    std::snprintf(buf, sizeof(buf), "(d %.2f m: 直接 %.3e 対 1/4πd² %.3e、横切り %d 枚)", d, R.direct6[2], expect, R.directCrossings);
    check("[レイ] 見通しの直接音は 1/(4πd²)（空気吸収を除いて 0.1%）", std::fabs(R.direct6[2] / expect - 1.0f) < 0.01f && R.directCrossings == 0, buf);

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
            if (a.direct6[i] != b.direct6[i] || a.transmit6[i] != b.transmit6[i] || a.early6[i] != b.early6[i] || a.late6[i] != b.late6[i]
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
    std::snprintf(buf, sizeof(buf), "(横切り %d 枚: 直接 %.2e、透過 %.2e = 1/4πd² × %.3f)", T.directCrossings, T.direct6[2], T.transmit6[2], T.transmit6[2] / expect);
    check("[レイ] 板を挟むと直接が 0 になり透過に τ が掛かる（0.01）", T.directCrossings == 1 && T.direct6[2] == 0.0f && std::fabs(T.transmit6[2] / expect - 0.01f) < 0.002f, buf);
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
    check("[世界] 2 本目の音源も配分される", w->mix(e2) && w->mix(e2)->conserves(0.01f) && w->mix(e2)->tapCount == 3);
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
        for (int k = 0; k < 30; ++k) w->update(dt);       // 重みは出口だけなので即時。念のため流す
        af::dsp::VoiceRenderer::Config vc; vc.sampleRate = fs; vc.maxFrames = block; vc.tailSeconds = 1.0f;
        af::dsp::VoiceRenderer v(vc);
        v.setOutputGain(1.0f); v.setHrtfEnabled(false); v.setTailLevel(1.0f);
        af::dsp::FdnRoomMix fdn2(fs, block, 0.6f);
        w->bindFdn(&fdn2);
        w->update(dt);                                     // ★器を差し替えたら 1 回回す（listenerWeight を置く。置かないと既定 0 で無音）
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
    const double expTaps = bwMean(kDirect) + bwMean(kEarly) + bwMean(kTransmit);
    std::snprintf(buf, sizeof(buf), "(全部: タップ %.3e 対 %.3e（%+.2f dB）、FDN %.3e 対 %.3e（%+.2f dB）)", tap, expTaps, afti::dB(tap / expTaps), fd, expLate, afti::dB(fd / expLate));
    check("[橋] タップ（直接＋初期＋透過）の出力が帳簿と ±0.5 dB", std::fabs(afti::dB(tap / expTaps)) < 0.5, buf);
    check("[橋] FDN（後期）の出力が帳簿と ±1.5 dB（校正の残り +1.3 dB の中）", std::fabs(afti::dB(fd / expLate)) < 1.5, buf);
    const double ratioDb = afti::dB(fd / tap);
    std::snprintf(buf, sizeof(buf), "(後期/タップ %+.1f dB。7×7×3 m、α 0.2、距離 3.6 m)", ratioDb);
    check("[橋] 後期の量は 0 でも支配でもない（−20〜+10 dB の間）", ratioDb > -20.0 && ratioDb < 10.0, buf);
    delete w;
}

}  // namespace

int main() {
    std::printf("=== 新コア（Flow）の数値回帰テスト ===\n");
    const char* only = std::getenv("AF_ONLY");
    struct { const char* name; void (*fn)(); } suites[] = {
        {"rules", testWorldRules}, {"probe", testProbe}, {"emitter", testEmitter}, {"mix", testMix}, {"instruments", testInstruments},
        {"trace", testEnergyTrace}, {"response", testResponse}, {"distribute", testDistribute},
        {"world", testWorld}, {"bridge", testBridge},
    };
    for (const auto& s : suites) if (!only || std::strcmp(only, s.name) == 0) s.fn();
    return afti::finish("Flow");
}
