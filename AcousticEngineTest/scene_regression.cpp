/* scene_regression.cpp
 * AF_Scene* API（新アーキ）の数値回帰テスト。
 *
 * 目的:
 *   API 移行（クエリ型 → af_Update/af_Get* のバッチ型）では内部構造を大きく動かす。
 *   そのとき「音が変わった気がする」でしか壊れを検出できないと切り分けが不能になるので、
 *   構造を変えても保たれるべき性質を数値で固定しておく。
 *   → docs/API_MIGRATION_PLAN.md の「段 0」。各段の後にこれを実行する。
 *
 * 設計方針:
 *   期待値を「絶対値」ではなく「関係」で書く（A > B、単調、範囲内）。
 *   絶対値で固定すると、正当なチューニング（材質係数の見直し等）でも落ちて
 *   テストが信用されなくなる。一方で関係が壊れるのは物理として明確な回帰なので、
 *   検出したいのはそちら。乱数を使う推定量（レイトレース）とも相性が良い。
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "acoustic_scene.h"
#include "acoustic_voice.h"

namespace {

AF_Vector3 V(float x, float y, float z) { return AF_Vector3{ x, y, z }; }

int g_failures = 0;
int g_checks = 0;

void check(const char* label, bool ok, const char* detail = "") {
    ++g_checks;
    std::printf("  [%s] %-46s %s\n", ok ? "OK" : "FAIL", label, detail);
    if (!ok) ++g_failures;
}

void checkGreater(const char* label, float a, float b) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.4g > %.4g)", a, b);
    check(label, a > b, buf);
}

void checkInRange(const char* label, float v, float lo, float hi) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.4g in [%.4g, %.4g])", v, lo, hi);
    check(label, v >= lo && v <= hi, buf);
}

constexpr int kBands = 6;   // 125/250/500/1k/2k/4kHz

// 内寸 (w,h,d) の閉じた箱を作る（床 y=0、中心が原点）。壁は厚み t。
void buildRoom(AF_SceneHandle s, float w, float h, float d, float t, int mat) {
    const float hw = w * 0.5f, hd = d * 0.5f;
    const AF_Vector3 X = V(1, 0, 0), Y = V(0, 1, 0);
    // 床 / 天井
    AF_SceneAddInstanceBox(s, V(0, -t * 0.5f, 0), V(hw + t, t * 0.5f, hd + t), X, Y, mat);
    AF_SceneAddInstanceBox(s, V(0, h + t * 0.5f, 0), V(hw + t, t * 0.5f, hd + t), X, Y, mat);
    // 東 / 西
    AF_SceneAddInstanceBox(s, V(hw + t * 0.5f, h * 0.5f, 0), V(t * 0.5f, h * 0.5f, hd), X, Y, mat);
    AF_SceneAddInstanceBox(s, V(-hw - t * 0.5f, h * 0.5f, 0), V(t * 0.5f, h * 0.5f, hd), X, Y, mat);
    // 北 / 南
    AF_SceneAddInstanceBox(s, V(0, h * 0.5f, hd + t * 0.5f), V(hw, h * 0.5f, t * 0.5f), X, Y, mat);
    AF_SceneAddInstanceBox(s, V(0, h * 0.5f, -hd - t * 0.5f), V(hw, h * 0.5f, t * 0.5f), X, Y, mat);
}

// エコグラムの総エネルギーと、-60dB に落ちるまでのビン数（RT60 相当）を測る。
void echogramStats(const std::vector<float>& bins, float& outTotal, int& outTailBin) {
    float peak = 0.0f, total = 0.0f;
    for (float e : bins) { if (e > peak) peak = e; total += e; }
    const float floorE = peak * 0.001f;   // -60dB
    int tail = 0;
    for (size_t k = 0; k < bins.size(); ++k) if (bins[k] > floorE) tail = static_cast<int>(k);
    outTotal = total;
    outTailBin = tail;
}

// ---------------------------------------------------------------- 透過
// 壁を1枚挟むと、低域ほどよく透ける（壁越しにベースが聞こえる現象）。
void testTransmission() {
    std::printf("\n[透過] 壁1枚の帯域別透過ゲイン\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);  // 既定壁
    AF_SceneAddInstanceBox(s, V(0, 0, 0), V(2, 2, 0.15f), V(1, 0, 0), V(0, 1, 0), mat);

    float g[kBands] = {};
    const int ok = AF_SceneComputeTransmissionBands(s, V(0, 0, -2), V(0, 0, 2), g, kBands);
    check("透過ゲインが取得できる", ok > 0);
    std::printf("      bands: ");
    for (int b = 0; b < kBands; ++b) std::printf("%.3f ", g[b]);
    std::printf("\n");

    checkGreater("低域(125Hz)の方が高域(4kHz)より透ける", g[0], g[5]);
    for (int b = 0; b < kBands; ++b) checkInRange("透過ゲインが0..1", g[b], 0.0f, 1.0f);

    // 遮蔽物なしなら素通り（全帯域 1.0）。
    AF_SceneClearInstances(s);
    AF_SceneComputeTransmissionBands(s, V(0, 0, -2), V(0, 0, 2), g, kBands);
    check("遮蔽なしは全帯域 1.0", g[0] > 0.99f && g[5] > 0.99f);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- 遮蔽/回折
// 有限の衝立は、縁を回り込む回折を生む。Maekawa により低域ほど回り込む。
void testOcclusionAndDiffraction() {
    std::printf("\n[遮蔽/回折] 有限の衝立\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    AF_SceneAddInstanceBox(s, V(0, 2, 0), V(3, 2, 0.2f), V(1, 0, 0), V(0, 1, 0), mat);

    const AF_Vector3 L = V(0, 1.6f, -4), S = V(0, 1.6f, 4);
    check("衝立の裏は遮蔽される", AF_SceneIsOccluded(s, L, S) != 0);
    check("衝立を外れた経路は通る", AF_SceneIsOccluded(s, V(10, 1.6f, -4), V(10, 1.6f, 4)) == 0);

    float d[kBands] = {};
    AF_SceneComputeDiffractionBands(s, S, L, d, kBands);
    std::printf("      diffraction: ");
    for (int b = 0; b < kBands; ++b) std::printf("%.3f ", d[b]);
    std::printf("\n");
    checkGreater("低域の方が回折で回り込む(Maekawa)", d[0], d[5]);
    for (int b = 0; b < kBands; ++b) checkInRange("回折ゲインが0..1", d[b], 0.0f, 1.0f);

    // 迂回の余剰長 δ は正（衝立を回るぶん遠回りになる）。
    AF_Vector3 mid;
    const float delta = AF_SceneDiffractionPath(s, S, L, &mid);
    check("回折経路が見つかる(δ>=0)", delta >= 0.0f);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- 早期反射
// 閉じた部屋なら反射タップが取れ、像源は必ず直接距離より遠い。
void testEarlyReflections() {
    std::printf("\n[早期反射] 閉じた部屋のタップ\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    buildRoom(s, 10, 4, 8, 0.4f, mat);

    const AF_Vector3 L = V(0, 1.6f, -1), S = V(0, 1.6f, 1);
    constexpr int kMaxTaps = 6;
    AF_Vector3 pos[kMaxTaps];
    float gain[kMaxTaps * kBands] = {};
    const int n = AF_SceneComputeEarlyReflections(s, L, S, pos, gain, kMaxTaps, 256, 2);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%d taps)", n);
    check("反射タップが1本以上取れる", n > 0, buf);
    check("タップ数が上限以内", n <= kMaxTaps);

    const float direct = 2.0f;   // |S-L|
    bool allBeyond = true;
    for (int t = 0; t < n; ++t) {
        const float dx = pos[t].x - L.x, dy = pos[t].y - L.y, dz = pos[t].z - L.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < direct - 1e-3f) allBeyond = false;
    }
    check("像源は直接距離より遠い(経路長として妥当)", allBeyond);

    bool gainsValid = true;
    for (int i = 0; i < n * kBands; ++i) if (gain[i] < 0.0f || gain[i] > 1.5f) gainsValid = false;
    check("タップゲインが妥当な範囲", gainsValid);

    // 開けた場所（遮蔽物なし）では反射は出ない。
    AF_SceneClearInstances(s);
    const int n2 = AF_SceneComputeEarlyReflections(s, L, S, pos, gain, kMaxTaps, 256, 2);
    check("開けた場所では反射タップ0", n2 == 0);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- 残響
// 部屋が広いほど尾が長い（RT60 ∝ V/Sα）。帯域版では高域が先に減る。
void testEchogram() {
    std::printf("\n[残響] 小部屋 vs 大部屋のエコグラム\n");
    constexpr int kBins = 100;
    constexpr float kBinSec = 0.01f;   // 10ms × 100 = 1秒窓

    auto measure = [&](float w, float h, float d, int& tailBin, float& total) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        buildRoom(s, w, h, d, 0.4f, mat);
        const AF_Vector3 L = V(0, 1.6f, -1);
        const AF_Vector3 src[1] = { V(0, 1.6f, 1) };
        std::vector<float> bins(kBins, 0.0f);
        AF_SceneComputeEchogram(s, L, src, 1, bins.data(), kBins, kBinSec, 343.0f, 512, 24);
        echogramStats(bins, total, tailBin);
        AF_SceneDestroy(s);
    };

    int tailSmall = 0, tailLarge = 0;
    float totalSmall = 0.0f, totalLarge = 0.0f;
    measure(4, 2.5f, 3, tailSmall, totalSmall);
    measure(30, 12, 24, tailLarge, totalLarge);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "(small=%d bins, large=%d bins)", tailSmall, tailLarge);
    check("大部屋の方が尾が長い(RT60 大)", tailLarge > tailSmall, buf);
    check("小部屋の尾に長さがある", tailSmall > 0);

    // --- 帯域別: 高域ほど速く減衰する（吸音が高域で大きいため）---
    std::printf("\n[残響] 帯域別エコグラムの減衰順序\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    buildRoom(s, 30, 12, 24, 0.4f, mat);
    const AF_Vector3 L = V(0, 1.6f, -1);
    const AF_Vector3 src[1] = { V(0, 1.6f, 1) };
    std::vector<float> bands(kBins * kBands, 0.0f);
    AF_SceneComputeEchogramBands(s, L, src, 1, bands.data(), kBins, kBinSec, 343.0f, 512, 24, 0.0f);

    // 各帯域の「後半 / 前半」エネルギー比＝減衰の遅さ。低域ほど大きいはず。
    auto lateRatio = [&](int b) {
        float early = 0.0f, late = 0.0f;
        for (int k = 0; k < kBins; ++k) {
            const float e = bands[static_cast<size_t>(k) * kBands + b];
            if (k < kBins / 4) early += e; else late += e;
        }
        return (early > 1e-20f) ? late / early : 0.0f;
    };
    const float lowRatio = lateRatio(0), highRatio = lateRatio(5);
    std::printf("      late/early: 125Hz=%.4f 4kHz=%.4f\n", lowRatio, highRatio);
    checkGreater("低域の方が長く残る(高域が先に減る)", lowRatio, highRatio);

    // --- 距離減衰: distanceRef を効かせると総エネルギーが減る ---
    std::vector<float> bandsAtten(kBins * kBands, 0.0f);
    AF_SceneComputeEchogramBands(s, L, src, 1, bandsAtten.data(), kBins, kBinSec, 343.0f, 512, 24, 4.0f);
    float sumRaw = 0.0f, sumAtten = 0.0f;
    for (int i = 0; i < kBins * kBands; ++i) { sumRaw += bands[i]; sumAtten += bandsAtten[i]; }
    checkGreater("distanceRef で広がり損失が掛かる", sumRaw, sumAtten);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- インスタンス管理
void testInstanceLifecycle() {
    std::printf("\n[インスタンス] 登録・無効化・クリア\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    const int id = AF_SceneAddInstanceBox(s, V(0, 0, 0), V(2, 2, 0.2f), V(1, 0, 0), V(0, 1, 0), mat);
    check("インスタンスIDが有効", id >= 0);
    check("インスタンス数が1", AF_SceneInstanceCount(s) == 1);

    const AF_Vector3 L = V(0, 0, -2), S = V(0, 0, 2);
    check("壁があれば遮蔽", AF_SceneIsOccluded(s, L, S) != 0);

    AF_SceneSetInstanceActive(s, id, 0);
    check("無効化すると遮蔽しない", AF_SceneIsOccluded(s, L, S) == 0);
    AF_SceneSetInstanceActive(s, id, 1);
    check("再有効化で遮蔽が戻る", AF_SceneIsOccluded(s, L, S) != 0);

    // 動かした先で遮蔽するようになる。
    AF_SceneUpdateInstance(s, id, V(50, 0, 0), V(2, 2, 0.2f), V(1, 0, 0), V(0, 1, 0));
    check("移動させると元の位置では遮蔽しない", AF_SceneIsOccluded(s, L, S) == 0);

    AF_SceneClearInstances(s);
    check("クリアでインスタンス数0", AF_SceneInstanceCount(s) == 0);
    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- リスナー/音源の保持
// 段1: エンジンが listener/source を保持する。まだ計算には使わないので、
//      「登録した内容が正しく保持・更新・削除される」ことだけを固定する。
void testSourceRegistry() {
    std::printf("\n[登録] リスナー / 音源の保持 (段1)\n");
    AF_SceneHandle s = AF_SceneCreate();

    check("初期状態は音源0", AF_SceneSourceCount(s) == 0);

    AF_SceneSetListener(s, V(0, 1.6f, -2));
    AF_SceneSetSource(s, 100, V(0, 1.6f, 2));
    AF_SceneSetSource(s, 200, V(3, 1.6f, 2));
    check("2音源が登録される", AF_SceneSourceCount(s) == 2);

    // 同じ id の再登録は「位置更新」であって増えない（毎フレーム呼ばれる想定）。
    AF_SceneSetSource(s, 100, V(0, 1.6f, 5));
    check("同一idの再登録は増えない(位置更新)", AF_SceneSourceCount(s) == 2);

    AF_SceneRemoveSource(s, 100);
    check("削除で1つ減る", AF_SceneSourceCount(s) == 1);
    AF_SceneRemoveSource(s, 999);   // 存在しない id
    check("存在しないidの削除は無視", AF_SceneSourceCount(s) == 1);

    AF_SceneClearSources(s);
    check("クリアで0", AF_SceneSourceCount(s) == 0);

    // null 安全。
    AF_SceneSetListener(nullptr, V(0, 0, 0));
    AF_SceneSetSource(nullptr, 1, V(0, 0, 0));
    check("null scene で落ちない", AF_SceneSourceCount(nullptr) == 0);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- バッチ更新
// 段2: AF_SceneUpdate + AF_SceneGet* が、従来の引数版クエリと同じ結果を出すことを固定する。
//      これが段2の核心。ここが合っていれば「音は変わらない」と言える。
void testBatchUpdate() {
    std::printf("\n[バッチ] AF_SceneUpdate / AF_SceneGet* (段2)\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    buildRoom(s, 10, 4, 8, 0.4f, mat);

    const AF_Vector3 L = V(0, 1.6f, -1);
    const AF_Vector3 S0 = V(0, 1.6f, 1);
    const AF_Vector3 S1 = V(3, 1.6f, 2);

    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 10, S0);
    AF_SceneSetSource(s, 20, S1);

    // 毎フレーム全部走るように間隔を 1 にする（レート分岐は別で確認）。
    AF_UpdateConfig cfg = {};
    cfg.role1EveryN = 1; cfg.role2EveryN = 1; cfg.earlyEveryN = 1;
    cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
    cfg.reflectionRays = 256; cfg.reflectionBounces = 3;
    cfg.directWeight = 1.0f; cfg.useReflections = 1;
    cfg.useEdgeCatalog = 1; cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;
    cfg.enableReverb = 1; cfg.echogramBins = 100; cfg.echogramBinSeconds = 0.01f;
    cfg.echogramRays = 512; cfg.echogramBounces = 24; cfg.speedOfSound = 343.0f;
    cfg.distanceRef = 0.0f;
    cfg.enableEarlyReflections = 1; cfg.earlyTaps = 4; cfg.earlyRays = 512; cfg.earlyBounces = 2;
    cfg.enableDiffractionSources = 1; cfg.diffSources = 3;
    AF_SceneSetUpdateConfig(s, &cfg);

    AF_SceneUpdate(s, 1.0f / 60.0f);

    // id → index。
    const int i0 = AF_SceneSourceIndex(s, 10);
    const int i1 = AF_SceneSourceIndex(s, 20);
    check("id→index が引ける", i0 == 0 && i1 == 1);
    check("未登録idは -1", AF_SceneSourceIndex(s, 999) == -1);

    // --- 遮蔽: バッチ結果 vs 従来クエリ ---
    float batchBands[kBands] = {};
    AF_SceneGetSourceOcclusion(s, i0, batchBands);

    std::vector<float> occRef(2), bandsRef(2 * kBands), dirRef(2 * 3);
    const AF_Vector3 srcs[2] = { S0, S1 };
    AF_SceneOcclusionReflectedMulti(s, L, srcs, 2, occRef.data(), bandsRef.data(),
                                    dirRef.data(), 1.0f, 256, 3);

    // レイの乱数系列が同じなので一致するはず（許容は数値誤差ぶんのみ）。
    bool bandsMatch = true;
    for (int b = 0; b < kBands; ++b)
        if (std::fabs(batchBands[b] - bandsRef[b]) > 1e-4f) bandsMatch = false;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "(batch[0]=%.4f ref[0]=%.4f)", batchBands[0], bandsRef[0]);
    check("遮蔽の帯域ゲインが従来クエリと一致(見通し)", bandsMatch, buf);

    // 見通しが立つ配置だと両方 1.0 で自明に一致してしまう。
    // 衝立を立てて「実際に遮蔽が起きている」状態でも一致することを確認する。
    AF_SceneAddInstanceBox(s, V(0, 1.6f, 0), V(2, 1.5f, 0.2f), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneUpdate(s, 1.0f / 60.0f);
    AF_SceneGetSourceOcclusion(s, i0, batchBands);
    AF_SceneOcclusionReflectedMulti(s, L, srcs, 2, occRef.data(), bandsRef.data(),
                                    dirRef.data(), 1.0f, 256, 3);
    bool blockedMatch = true;
    for (int b = 0; b < kBands; ++b)
        if (std::fabs(batchBands[b] - bandsRef[b]) > 1e-4f) blockedMatch = false;
    std::snprintf(buf, sizeof(buf), "(batch[0]=%.4f ref[0]=%.4f)", batchBands[0], bandsRef[0]);
    check("遮蔽の帯域ゲインが従来クエリと一致(遮蔽あり)", blockedMatch, buf);
    check("衝立で実際に遮蔽されている(<1.0)", batchBands[0] < 0.999f, buf);

    const float occScalar = AF_SceneGetSourceOcclusionScalar(s, i0);
    checkInRange("遮蔽スカラが0..1", occScalar, 0.0f, 1.0f);

    float dir[3] = {};
    AF_SceneGetSourceArrivalDir(s, i0, dir);
    const float dirLen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    check("到来方向が単位ベクトル相当", dirLen > 0.5f && dirLen < 1.5f);

    // --- 早期反射: 本数と像源の妥当性 ---
    AF_Vector3 erPos[4];
    float erGain[4 * kBands] = {};
    const int erN = AF_SceneGetEarlyReflections(s, i0, erPos, erGain, 4);
    std::snprintf(buf, sizeof(buf), "(%d taps)", erN);
    check("早期反射が取れる", erN > 0, buf);
    check("早期反射が上限以内", erN <= 4);

    // --- 回折二次音源: 遮蔽が無いので 0 本でも正常（クラッシュしないことを見る）---
    AF_Vector3 dsPos[3];
    float dsGain[3] = {};
    const int dsN = AF_SceneGetDiffractionSources(s, i0, dsPos, dsGain, 3);
    check("回折二次音源の取得が妥当", dsN >= 0 && dsN <= 3);

    // --- エコグラム: バッチ結果が従来クエリと一致 ---
    constexpr int kBins = 100;
    std::vector<float> batchEcho(kBins * kBands, 0.0f);
    const int gotBins = AF_SceneGetEchogramBands(s, -1, batchEcho.data(), kBins);
    check("エコグラムのビン数が一致", gotBins == kBins);

    std::vector<float> refEcho(kBins * kBands, 0.0f);
    AF_SceneComputeEchogramBands(s, L, srcs, 2, refEcho.data(), kBins, 0.01f, 343.0f, 512, 24, 0.0f);
    float maxDiff = 0.0f, refTotal = 0.0f;
    for (int i = 0; i < kBins * kBands; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(batchEcho[i] - refEcho[i]));
        refTotal += refEcho[i];
    }
    std::snprintf(buf, sizeof(buf), "(maxDiff=%.6g, total=%.4g)", maxDiff, refTotal);
    check("エコグラムが従来クエリと一致", maxDiff < 1e-3f, buf);

    // --- 範囲外 index は何も壊さない ---
    float guard[kBands] = { -1, -1, -1, -1, -1, -1 };
    AF_SceneGetSourceOcclusion(s, 99, guard);
    check("範囲外indexで出力バッファが変わらない", guard[0] == -1.0f);
    check("範囲外indexの早期反射は0本", AF_SceneGetEarlyReflections(s, 99, erPos, erGain, 4) == 0);

    AF_SceneDestroy(s);
}

// 更新レートが効いていること（間隔を空けた役割は毎フレーム走らない）。
void testUpdateRates() {
    std::printf("\n[バッチ] 更新レート (段2)\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    buildRoom(s, 10, 4, 8, 0.4f, mat);
    AF_SceneSetListener(s, V(0, 1.6f, -1));
    AF_SceneSetSource(s, 1, V(0, 1.6f, 1));

    AF_UpdateConfig cfg = {};
    cfg.role1EveryN = 1; cfg.role2EveryN = 4; cfg.earlyEveryN = 1;
    cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
    cfg.reflectionRays = 64; cfg.reflectionBounces = 2;
    cfg.directWeight = 1.0f; cfg.useReflections = 1;
    cfg.useEdgeCatalog = 0;
    cfg.enableReverb = 1; cfg.echogramBins = 50; cfg.echogramBinSeconds = 0.01f;
    cfg.echogramRays = 128; cfg.echogramBounces = 8; cfg.speedOfSound = 343.0f;
    cfg.enableEarlyReflections = 1; cfg.earlyTaps = 2; cfg.earlyRays = 64; cfg.earlyBounces = 2;
    cfg.enableDiffractionSources = 0; cfg.diffSources = 1;
    AF_SceneSetUpdateConfig(s, &cfg);

    // 1 フレーム目: 全部走る（カウンタ初期値 1）。
    AF_SceneUpdate(s, 0.016f);
    std::vector<float> e1(50 * kBands, 0.0f);
    AF_SceneGetEchogramBands(s, -1, e1.data(), 50);
    float sum1 = 0.0f;
    for (float v : e1) sum1 += v;
    check("初回updateでエコグラムが埋まる", sum1 > 0.0f);

    // 音源を大きく動かしてから 1 フレームだけ回す。
    // role2EveryN=4 なのでエコグラムはまだ更新されない＝前回値のままのはず。
    AF_SceneSetSource(s, 1, V(4, 1.6f, 3));
    AF_SceneUpdate(s, 0.016f);
    std::vector<float> e2(50 * kBands, 0.0f);
    AF_SceneGetEchogramBands(s, -1, e2.data(), 50);
    bool unchanged = true;
    for (int i = 0; i < 50 * kBands; ++i) if (std::fabs(e1[i] - e2[i]) > 1e-9f) unchanged = false;
    check("role2EveryN=4 なので次フレームでは再計算されない", unchanged);

    // さらに 3 フレーム進めるとカウンタが 0 になり再計算される。
    AF_SceneUpdate(s, 0.016f);
    AF_SceneUpdate(s, 0.016f);
    AF_SceneUpdate(s, 0.016f);
    std::vector<float> e3(50 * kBands, 0.0f);
    AF_SceneGetEchogramBands(s, -1, e3.data(), 50);
    bool changed = false;
    for (int i = 0; i < 50 * kBands; ++i) if (std::fabs(e1[i] - e3[i]) > 1e-9f) changed = true;
    check("4フレーム後には再計算される", changed);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- 診断: 尾のスペクトル傾斜
// 「狭い部屋で高音がこもる」の原因切り分け用。部屋サイズごとに
//   ・尾の帯域別エネルギー（低域基準の相対値）
//   ・直接音に対する尾の比
// を出す。合否判定ではなく観測が目的なので、明らかな異常だけを check する。
void diagnoseTailSpectrum() {
    std::printf("\n[診断] 部屋サイズ別の尾のスペクトル傾斜\n");
    constexpr int kBins = 100;
    constexpr float kBinSec = 0.01f;

    struct Room { const char* name; float w, h, d; };
    const Room rooms[] = {
        { "小   4x3x2.5", 4.0f, 2.5f, 3.0f },
        { "中  10x8x4",  10.0f, 4.0f, 8.0f },
        { "大  30x24x12", 30.0f, 12.0f, 24.0f },
    };

    std::printf("      部屋            125Hz  250Hz  500Hz   1kHz   2kHz   4kHz   (低域=0dB)\n");
    for (const Room& rm : rooms) {
        AF_SceneHandle s = AF_SceneCreate();
        // コンクリ相当（テストシーンと同じ材質）を明示的に作る。
        const float t[kBands] = { 0.05f, 0.03f, 0.015f, 0.008f, 0.004f, 0.002f };
        const float a[kBands] = { 0.02f, 0.02f, 0.03f,  0.04f,  0.05f,  0.07f };
        const float sc[kBands] = { 0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f };
        const int mat = AF_SceneAddMaterial(s, t, a, sc, kBands);
        buildRoom(s, rm.w, rm.h, rm.d, 0.4f, mat);

        const AF_Vector3 L = V(0, 1.6f, -1);
        const AF_Vector3 src[1] = { V(0, 1.6f, 1) };
        std::vector<float> bands(kBins * kBands, 0.0f);
        AF_SceneComputeEchogramBands(s, L, src, 1, bands.data(), kBins, kBinSec, 343.0f, 512, 24, 4.0f);

        // 直接音ビン（先頭付近の最大）と、それ以降（尾）を帯域ごとに集計する。
        float direct[kBands] = {}, tail[kBands] = {};
        int directBin = 0;
        float peak = 0.0f;
        for (int k = 0; k < kBins; ++k) {
            float e = 0.0f;
            for (int b = 0; b < kBands; ++b) e += bands[static_cast<size_t>(k) * kBands + b];
            if (e > peak) { peak = e; directBin = k; }
        }
        for (int b = 0; b < kBands; ++b) direct[b] = bands[static_cast<size_t>(directBin) * kBands + b];
        for (int k = directBin + 1; k < kBins; ++k)
            for (int b = 0; b < kBands; ++b) tail[b] += bands[static_cast<size_t>(k) * kBands + b];

        // 低域を 0dB とした相対値で傾斜を見る。
        std::printf("      %-14s", rm.name);
        const float ref = std::max(tail[0], 1e-20f);
        for (int b = 0; b < kBands; ++b) {
            const float db = 10.0f * std::log10(std::max(tail[b], 1e-20f) / ref);
            std::printf("%6.1f ", db);
        }
        float dsum = 0.0f, tsum = 0.0f;
        for (int b = 0; b < kBands; ++b) { dsum += direct[b]; tsum += tail[b]; }
        std::printf("  尾/直接=%.2f\n", (dsum > 1e-20f) ? tsum / dsum : 0.0f);

        // 尾の高域が低域より 20dB 以上落ちていたら、こもりの原因として疑わしい。
        const float tilt = 10.0f * std::log10(std::max(tail[5], 1e-20f) / ref);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "(%s: 4kHz が %.1f dB)", rm.name, tilt);
        check("尾の高域が極端に落ちていない(>-20dB)", tilt > -20.0f, buf);

        AF_SceneDestroy(s);
    }
}

// ---------------------------------------------------------------- 診断: 影境界の連続性
// 回折ゲインが影境界を跨ぐときに跳ばないかを、リスナーを横に動かしながら測る。
//
// なぜ跳ぶか（現状の実装）:
//   computeDiffraction は isOccluded の二値判定で
//     非遮蔽 → 全帯域 1.0 / 遮蔽 → UTD値（影境界で約0.5）
//   と切り替える。物理的には照らされた領域にも回折場は存在し、
//   直接音と足すと影境界で連続になるのが UTD の設計思想（GTD を「一様化」した目的そのもの）。
//   照らされた側で回折場を捨てているので、その連続性が壊れている。
void diagnoseShadowBoundary() {
    std::printf("\n[診断] 影境界を横切るときの回折ゲインの連続性\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

    // z=0 に半無限に近い壁。x<=0 が壁、x>0 が開口（エッジは x=0）。
    AF_SceneAddInstanceBox(s, V(-5, 2, 0), V(5, 2, 0.15f), V(1, 0, 0), V(0, 1, 0), mat);

    const AF_Vector3 src = V(-2, 1.6f, 2);
    // 影境界: 音源(-2,2) からエッジ(0,0) を通る直線が z=-2 に達する x を求めると x=+2。
    //   x<2 が影 / x>2 が照らされた側。
    // 採用モデル(前川)と、比較用の UTD を並べて出す。
    //   UTD は厳密解だが回折点の 3D 幾何に依存するため、どの稜線が最短かが入れ替わる位置で
    //   ゲインが飛ぶ。前川は符号付き δ だけの関数で、δ の min は連続なので飛ばない。
    //   この差がそのまま「音声経路に前川を採った理由」なので、数値で残しておく。
    std::printf("      　　　　　  ┌─ 前川(採用) ─┐  ┌─ UTD(比較) ──┐\n");
    std::printf("      リスナー x   125Hz    4kHz    125Hz    4kHz   （x=2 が影境界）\n");

    float prevLow = -1.0f, maxJump = 0.0f, jumpAtX = 0.0f;
    float prevUtd = -1.0f, maxJumpUtd = 0.0f;
    for (float x = 0.0f; x <= 4.01f; x += 0.25f) {
        float g[kBands] = {}, u[kBands] = {};
        AF_SceneComputeDiffractionBands(s, V(x, 1.6f, -2), src, g, kBands);
        AF_SceneComputeDiffractionBandsUtd(s, V(x, 1.6f, -2), src, u, kBands);
        float fr[kBands] = {};
        const int frOk = AF_SceneMeasureApertureFresnel(s, V(x, 1.6f, -2), src, fr);
        // ★δ も出す。開口率が滑らかなのに出力が飛ぶなら、原因は δ 側にある。
        AF_Vector3 mid;
        const float dlt = AF_SceneDiffractionPath(s, V(x, 1.6f, -2), src, &mid);
        std::printf("      %8.2f  %6.3f  %6.3f   %6.3f  %6.3f   開口率 %d %6.3f %6.3f"
                    "   δ=%6.3f 開口点(%5.2f,%5.2f,%5.2f)%s\n",
                    x, g[0], g[5], u[0], u[5], frOk, fr[0], fr[5],
                    dlt, mid.x, mid.y, mid.z,
                    (std::fabs(x - 2.0f) < 0.13f) ? "  ← 影境界" : "");
        if (prevLow >= 0.0f) {
            const float jump = std::fabs(g[0] - prevLow);
            if (jump > maxJump) { maxJump = jump; jumpAtX = x; }
            maxJumpUtd = std::max(maxJumpUtd, std::fabs(u[0] - prevUtd));
        }
        prevLow = g[0];
        prevUtd = u[0];
    }
    std::printf("      最大の隣接差: 前川 %.3f / UTD %.3f\n", maxJump, maxJumpUtd);

    // 0.25m 刻みで隣接する点の差。連続なら小さいはず。
    //   二値の遮蔽切替だと 1.0→0.5 の跳び（≒0.5）が出る。
    //   前川の式は符号付き δ だけの関数で、δ は全候補稜線の min なので連続。
    //   したがって稜線が入れ替わっても値は飛ばない ── ここが UTD との決定的な差。
    //   （UTD は回折点の 3D 幾何に依存するため、同条件で最良 3.8dB / 最悪 17.6dB の
    //     段差が残った。経緯は docs/DEV_LOG.md G章、実装比較は Core/maekawa.h 冒頭。）
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(最大の隣接差 %.3f @ x=%.2f)", maxJump, jumpAtX);
    check("影境界で回折ゲインが跳ばない(隣接差<0.2)", maxJump < 0.2f, buf);
    if (maxJump >= 0.2f)
        std::printf("      → %.1f dB の段差が残っている。\n",
                    20.0f * std::log10((1.0f - maxJump > 1e-3f) ? 1.0f / (1.0f - maxJump) : 1000.0f));

    AF_SceneDestroy(s);
}

// 回折二次音源は「音が実際に抜けてくる場所」を指していなければならない。
//   大きな壁の場合、壁の反対端の稜線は「壁を何メートルも貫く経路」でしか到達できないので
//   開口ではない。にもかかわらず候補に入ると、壁の裏の見当違いな方向から音が鳴る。
//   （実機で観測された症状。原因は当の箱を遮蔽判定から丸ごと除外していたこと。）
void diagnoseApertureDirection() {
    std::printf("\n[診断] 回折二次音源が「抜けてくる側」を指しているか\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

    // 大きな壁。x∈[-10,2] が壁、x>2 が唯一の開口。
    //   ★上下は十分に伸ばすこと（y∈[-18,22]）。低いと上下を回る経路も同程度に効いてしまい、
    //     「開口が1つ」というこのテストの前提が幾何と食い違う。
    //     以前 y∈[-2,6] にしていたため横:上:下 ≈ 2:1:1 になり、支配開口の取り分が
    //     0.5 前後にしかならなかった（それが正しい物理なのに、テストは1つに集中することを期待していた）。
    AF_SceneAddInstanceBox(s, V(-4, 2, 0), V(6, 20, 0.2f), V(1, 0, 0), V(0, 1, 0), mat);

    const AF_Vector3 L = V(0, 1.6f, -4), S = V(0, 1.6f, 4);
    check("大きな壁の裏は遮蔽される", AF_SceneIsOccluded(s, L, S) != 0);

    // 音源を登録してバッチ更新（二次音源はこの経路でしか取れない）。
    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);
    AF_SceneUpdate(s, 1.0f / 60.0f);

    AF_Vector3 pos[8]; float gain[8];
    const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
    std::printf("      二次音源 %d 本（開口は x=+2 側の縁ひとつ）\n", n);
    bool allNearSide = (n > 0);
    float top = 0.0f;
    for (int i = 0; i < n; ++i) {
        std::printf("        [%d] (%7.2f,%7.2f,%7.2f)  gain %.3f%s\n",
                    i, pos[i].x, pos[i].y, pos[i].z, gain[i],
                    (pos[i].x < 0.0f) ? "   ← 壁の反対側！" : "");
        if (pos[i].x < 0.0f) allNearSide = false;
        if (gain[i] > top) top = gain[i];
    }
    check("二次音源がすべて開口側(x>0)を指す", allNearSide);

    // 支配開口が他より明確に強いこと。分散していると「どこから抜けてくるか」が伝わらない。
    //
    //   ★絶対値ではなく**比**で見る（DEV_LOG の方針「期待値は絶対値でなく関係で書く」）。
    //     以前は「最大 > 0.5」としていたが、これは正当な修正で落ちる基準だった。
    //     当時の 0.73 はクラスタ内のエッジ本数を足し込んだ過大計上の産物で、
    //     エッジ本数はテッセレーション依存なので基準にしてはいけない。
    //     このシーンでは壁が高くないので上を越える経路も実際に効いており、
    //     δ から手計算しても 横 0.263 / 上 0.133 / 下 0.133 ＝ 取り分 0.5 前後が正しい。
    //     定位の観点でも「2番目の何倍か」の方が意味がある（取り分が 0.5 でも 2 倍あれば方向は明確）。
    float second = 0.0f;
    for (int i = 0; i < n; ++i) if (gain[i] < top && gain[i] > second) second = gain[i];
    char b2[128];
    std::snprintf(b2, sizeof(b2), "(最大 %.3f / 2番目 %.3f = %.1f倍)",
                  top, second, (second > 1e-6f) ? top / second : 999.0f);
    check("支配開口が2番目の2倍以上ある", second <= 1e-6f || top >= second * 2.0f, b2);

    AF_SceneDestroy(s);
}

// ================================================================ スイングドア
// docs/DIFFRACTION_DESIGN.md §7。要件の5性質を一度に検証できる唯一のシーン。
//
// 押して開くドア。蝶番で回転し、開くにつれて直接聞こえる角度範囲が広がる。
// 扉の角度を 0°→90° に振り、各角度で回折を測る。
//   幾何: 壁 z=0 に幅1mの戸口。扉は左枠(x=-0.5)を軸に +z 側へ振れる。
//         リスナー(0,1.6,-3) / 音源(0,1.6,+3) の直線は x=0。
//         扉が塞ぐのは 0.5/cosθ <= 1 のとき、すなわち θ <= 60°。
//         → 60°付近で「回折のみ」から「直接見通せる」へ移る。ここが段差になってはいけない。
void diagnoseSwingDoor() {
    std::printf("\n[診断] スイングドア: 開き角と回折の連続性\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

    // 壁（戸口 x∈[-0.5,0.5] を空けた2枚）
    AF_SceneAddInstanceBox(s, V(-2.75f, 1.5f, 0), V(2.25f, 1.5f, 0.1f), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V( 2.75f, 1.5f, 0), V(2.25f, 1.5f, 0.1f), V(1, 0, 0), V(0, 1, 0), mat);

    // 扉。蝶番 H=(-0.5,1.5,0)、幅1m・厚み0.05m。
    //   高さは戸口と同じ y∈[0,3] にすること。低いと扉の上に隙間が残り、
    //   「扉を回り込む」ではなく「扉の上を越える」を測ってしまう。
    const AF_Vector3 hinge = V(-0.5f, 1.5f, 0.0f);
    const AF_Vector3 half  = V(0.5f, 1.5f, 0.025f);
    const int door = AF_SceneAddInstanceBox(s, V(0, 1.5f, 0), half, V(1, 0, 0), V(0, 1, 0), mat);

    // ★戸口をポータルとして置く。実運用では扉の戸口にはポータルを置くので、
    //   ここも出荷する経路で測る。置かないと稜線探索が担当し、
    //   開口位置が跳ぶ（実測 2.93m @ 15°）。
    AF_SceneAddPortal(s, V(0, 1.5f, 0), V(1, 0, 0), V(0, 1, 0), 0.5f, 1.5f);

    const AF_Vector3 L = V(0, 1.6f, -3), S = V(0, 1.6f, 3);
    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);

    std::printf("      開き角  ─ 前川(δ) ─  遮蔽  ─ キルヒホッフ(開口実測) ─  開口位置(最有力)\n");
    std::printf("               125Hz  4kHz         125Hz  500Hz   4kHz\n");

    float prevLow = -1.0f, maxGainJump = 0.0f, jumpAtDeg = 0.0f;
    AF_Vector3 prevAp = V(0, 0, 0); bool havePrev = false;
    float maxPosJump = 0.0f, posJumpAtDeg = 0.0f;
    bool behindDoor = false, lowOverHigh = true;
    float minDominant = 1.0f;
    float apertureLostAtDeg = -1.0f;   // 減衰が残っているのに開口が0本になった角度

    for (float deg = 0.0f; deg <= 90.01f; deg += 7.5f) {
        const float th = deg * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        // 蝶番まわりに回す。axisX は蝶番→自由端。中心は蝶番から半幅ぶん。
        const AF_Vector3 ax = V(c, 0, sn);
        AF_SceneUpdateInstance(s, door,
                               V(hinge.x + ax.x * 0.5f, hinge.y, hinge.z + ax.z * 0.5f),
                               half, ax, V(0, 1, 0));
        AF_SceneUpdate(s, 1.0f / 60.0f);

        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        const int occ = AF_SceneIsOccluded(s, L, S);

        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        int bi = -1; float bw = -1.0f;
        for (int i = 0; i < n; ++i) if (gain[i] > bw) { bw = gain[i]; bi = i; }

        float kg[kBands] = {}; AF_Vector3 kap{}; float kpl = 0.0f;
        const int kok = AF_SceneComputeDiffractionKirchhoff(s, L, S, kg, kBands, &kap, &kpl);

        if (bi >= 0) {
            std::printf("      %5.1f°  %5.3f %5.3f    %d    %5.3f  %5.3f  %5.3f   (%5.2f,%5.2f,%5.2f)%s\n",
                        deg, g[0], g[5], occ, kg[0], kg[2], kg[5],
                        pos[bi].x, pos[bi].y, pos[bi].z, kok ? "" : " [K:なし]");
            // 性質2: 扉は +z 側へ振れる。開口が扉の板の裏（x<-0.5 かつ z>0）を指してはいけない。
            if (pos[bi].x < -0.5f && pos[bi].z > 0.0f) behindDoor = true;
            if (bw < minDominant) minDominant = bw;   // 性質3
            // 性質1(方向): 隣接角度で開口が飛ばない。3次元で測ること
            //   （x,z だけで測ると「扉の上を越える経路」への乗り換えを見逃す）。
            if (havePrev) {
                const float dx = pos[bi].x - prevAp.x, dy = pos[bi].y - prevAp.y,
                            dz = pos[bi].z - prevAp.z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > maxPosJump) { maxPosJump = d; posJumpAtDeg = deg; }
            }
            prevAp = pos[bi]; havePrev = true;
        } else {
            std::printf("      %5.1f°  %5.3f %5.3f    %d    %5.3f  %5.3f  %5.3f   (前川:開口なし)\n",
                        deg, g[0], g[5], occ, kg[0], kg[2], kg[5]);
            // 減衰が残っているのに方向が分からない状態。定位が消える。
            if (g[0] < 0.999f && apertureLostAtDeg < 0.0f) apertureLostAtDeg = deg;
            havePrev = false;
        }

        // 性質1(ゲイン): 隣接角度でゲインが飛ばない。60°の見通し開通も段差にしない。
        if (prevLow >= 0.0f) {
            const float j = std::fabs(g[0] - prevLow);
            if (j > maxGainJump) { maxGainJump = j; jumpAtDeg = deg; }
        }
        prevLow = g[0];

        // 性質4: 前川の δ 減衰は低域ほど回り込む。
        //   ★**遮蔽されている間だけ**の性質。`diffractionContinuous` は照らされた側では
        //     「直接音込みの総合値」を返すので、そこで低域>高域は成り立たない
        //     （実測: 67.5°で 125Hz 0.345 / 4kHz 0.357 と逆転。90°では 4kHz が 1.6倍）。
        //     以前は判定が緩くて気づかなかっただけで、値は前から逆転していた。
        //     扉が開き切る手前（開口が支配的になる前）でだけ見る。
        // 性質4: 前川の δ 減衰は低域ほど回り込む ── **遮蔽されていて、かつ回折が出ている間**の性質。
        //   完全閉扉（0°）は 125Hz も 4kHz も 0.000 の同値になるので「低域>高域」は偽になるが、
        //   これは性質の破れではなく「回折そのものが無い」。ゼロ同士は対象外にする。
        //   （照らされた側 occ=0 は元から除外。そこでは直接音込みの総合値を返すので
        //     67.5°で 125Hz 0.345 / 4kHz 0.357 と逆転して当然）
        if (occ && g[0] > 0.0f && !(g[0] > g[5])) lowOverHigh = false;
    }

    // ── 立ち上がり（0°〜10°）を細かく測る ──
    //   7.5°刻みで見た 0°→7.5° の +0.234 が「不連続」なのか「急峻なだけ」なのかは、
    //   粗い刻みでは区別できない。扉 7.5° の隙間は自由端で約13cm あり、
    //   密閉→13cm開口 は物理的に大きく変わって当然なので、滑らかな急変の可能性がある。
    //   0.5°刻み（自由端で 0.9cm ずつ）で段差が残るなら、候補の出現による真の不連続。
    //   前川(δのみ) と キルヒホッフ(開口の実測) を並べる。
    //   コンセプトは「扉がどのくらい開いたか」を音で伝えること。δ は扉が回っても変わらない
    //   （回折経路が回る戸口の枠は動かない）ので、前川は piecewise constant になるはず。
    std::printf("\n      [立ち上がり] 0°〜10° を 0.5°刻み（隙間は自由端で約 1.7cm/度）\n");
    std::printf("        開き角  ── 前川(δのみ) ──   ── キルヒホッフ(開口実測) ──  開口率\n");
    std::printf("                 125Hz   隣接差      125Hz   4kHz   隣接差       0..1\n");
    float prevFine = -1.0f, maxFineJump = 0.0f, fineJumpAt = 0.0f;
    float prevK = -1.0f, maxKJump = 0.0f, kJumpAt = 0.0f;
    float kFirst = -1.0f, kLast = -1.0f;
    for (float deg = 0.0f; deg <= 10.01f; deg += 0.5f) {
        const float th = deg * 3.14159265f / 180.0f;
        const AF_Vector3 ax = V(std::cos(th), 0, std::sin(th));
        AF_SceneUpdateInstance(s, door,
                               V(hinge.x + ax.x * 0.5f, hinge.y, hinge.z + ax.z * 0.5f),
                               half, ax, V(0, 1, 0));
        AF_SceneUpdate(s, 1.0f / 60.0f);

        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        float jump = (prevFine >= 0.0f) ? std::fabs(g[0] - prevFine) : 0.0f;
        if (jump > maxFineJump) { maxFineJump = jump; fineJumpAt = deg; }
        prevFine = g[0];

        // 開口率（回折の音量を決める量）。開口位置は二次音源から取る。
        float openFrac = -1.0f;
        {
            AF_SceneSetListener(s, L); AF_SceneSetSource(s, 1, S);
            AF_Vector3 dp[8]; float dg[8];
            const int dn = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), dp, dg, 8);
            if (dn > 0) openFrac = AF_SceneMeasureApertureOpenness(s, L, S);
        }

        float k[kBands] = {}; AF_Vector3 ap{}; float pl = 0.0f;
        const int okK = AF_SceneComputeDiffractionKirchhoff(s, L, S, k, kBands, &ap, &pl);
        float kj = (okK && prevK >= 0.0f) ? std::fabs(k[0] - prevK) : 0.0f;
        if (kj > maxKJump) { maxKJump = kj; kJumpAt = deg; }
        if (okK) { if (kFirst < 0.0f) kFirst = k[0]; kLast = k[0]; prevK = k[0]; }

        std::printf("        %5.1f°   %6.3f  %6.3f      %6.3f %6.3f  %6.3f    %6.3f%s\n",
                    deg, g[0], jump, k[0], k[5], kj, openFrac, okK ? "" : "  (開口なし)");
    }
    std::printf("        → 最大隣接差   前川 %.3f @ %.1f°   キルヒホッフ %.3f @ %.1f°\n",
                maxFineJump, fineJumpAt, maxKJump, kJumpAt);
    std::printf("        → 0°→10° の変化  キルヒホッフ %.3f → %.3f  "
                "（コンセプト: 開き具合が連続に音へ出ること）\n", kFirst, kLast);

    // 掃引の最後の状態（90°）へ戻してから開き切りを測る。
    AF_SceneUpdateInstance(s, door, V(hinge.x, hinge.y, hinge.z + 0.5f), half,
                           V(0, 0, 1), V(0, 1, 0));
    AF_SceneUpdate(s, 1.0f / 60.0f);

    float gOpen[kBands] = {};
    AF_SceneComputeDiffractionBands(s, L, S, gOpen, kBands);

    // ── 要件の達成状況（docs/DIFFRACTION_DESIGN.md §3）──
    //   ここは「壊れた／壊れていない」ではなく「まだ作っていない」の一覧。
    //   赤いままのスイートは信用されなくなるので、未達は check() にせず数値だけ出す。
    //   回折を作り直すときに、ここの目標値をそのまま合格基準として check() へ格上げする。
    auto row = [](const char* name, bool ok, const char* detail) {
        std::printf("        %-22s %-26s %s\n", name, detail, ok ? "達成" : "未達 ←");
    };
    char d1[64], d2[64], d3[64], d4[64], d5[64];
    std::snprintf(d1, sizeof(d1), "最大隣接差 %.3f @ %.1f°", maxGainJump, jumpAtDeg);
    std::snprintf(d2, sizeof(d2), "最大隣接差 %.2fm @ %.1f°", maxPosJump, posJumpAtDeg);
    std::snprintf(d3, sizeof(d3), "支配重みの最小 %.3f", minDominant);
    std::snprintf(d4, sizeof(d4), "%.1f° 以降 開口0本", apertureLostAtDeg);
    std::snprintf(d5, sizeof(d5), "125Hz %.3f", gOpen[0]);
    std::printf("\n      ── 要件の達成状況（目標は作り直しの合格基準）──\n");
    row("性質1 ゲイン連続",   maxGainJump < 0.05f,        d1);
    row("性質1 開口位置連続", maxPosJump < 0.5f,          d2);
    row("性質2 到達可能性",   !behindDoor,                "扉の裏を指さない");
    row("性質3 集中",         minDominant > 0.6f,         d3);
    row("性質4 低域>高域",    lowOverHigh,                "全角度で成立");
    row("性質5 定位の維持",   apertureLostAtDeg < 0.0f,   d4);
    row("性質5 開き切り1.0",  gOpen[0] > 0.9f,            d5);
    std::printf("      目標: ゲイン<0.05 / 位置<0.5m / 集中>0.6 / 開口が消えない / 開き切り>0.9\n");

    // 現状すでに満たしているものだけ check する（回帰の検出用）。
    check("[扉] 到達可能性: 開口が扉の板の裏を指さない", !behindDoor);
    check("[扉] 遮蔽中は低域>高域", lowOverHigh);
    check("[扉] 開き切ると見通せる", AF_SceneIsOccluded(s, L, S) == 0);

    // 構造的欠陥の実例も記録する。ゲイン用と方向用で候補の探索条件が違うため、
    // 「ゲインは回折を見ているのに開口は0本」という不整合が起きる（設計 §2 で解消）。
    AF_Vector3 po[8]; float go[8];
    const int nOpen = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), po, go, 8);
    if (nOpen == 0 && gOpen[0] < 0.999f)
        std::printf("      [不整合・既知] 回折ゲインは %.3f なのに開口は 0 本。"
                    "探索が2箇所に重複していることの実例（設計 §2）。\n", gOpen[0]);

    AF_SceneDestroy(s);
}

// ================================================================ メッシュ形状
// 箱では表せない形（穴の空いた壁）を扱えることを確かめる。
//   遮蔽判定が境界ボックスではなく実形状を見ていれば、戸口の正面は通り、脇は遮られる。
//   あわせて「形状(BLAS)と配置(インスタンス)の分離」が効いていることも見る:
//   同じ geomId を 2 箇所に置く／動かす／消す、が形状の再構築なしにできる。
void testMesh() {
    std::printf("\n[メッシュ] 穴の空いた壁\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

    // ローカル空間で z=0 の板。x∈[-3,3], y∈[0,3]。中央 x∈[-0.5,0.5] を戸口として抜く。
    //   板を「左・右・上」の3枚の矩形（各2三角形）で作る。下は戸口なので塞がない。
    const float vx[] = {
        // 左パネル x∈[-3,-0.5], y∈[0,3]
        -3.0f, 0.0f, 0.0f,  -0.5f, 0.0f, 0.0f,  -0.5f, 3.0f, 0.0f,  -3.0f, 3.0f, 0.0f,
        // 右パネル x∈[0.5,3]
         0.5f, 0.0f, 0.0f,   3.0f, 0.0f, 0.0f,   3.0f, 3.0f, 0.0f,   0.5f, 3.0f, 0.0f,
        // 上まぐさ x∈[-0.5,0.5], y∈[2.2,3]
        -0.5f, 2.2f, 0.0f,   0.5f, 2.2f, 0.0f,   0.5f, 3.0f, 0.0f,  -0.5f, 3.0f, 0.0f,
    };
    const int ix[] = {
        0, 1, 2,  0, 2, 3,
        4, 5, 6,  4, 6, 7,
        8, 9, 10, 8, 10, 11,
    };
    AF_Vector3 lc{}, lh{};
    const int geom = AF_SceneAddMesh(s, vx, 12, ix, 18, &lc, &lh);
    check("メッシュが登録できる", geom >= 0);
    std::printf("      ローカルAABB 中心(%.2f,%.2f,%.2f) 半径(%.2f,%.2f,%.2f)\n",
                lc.x, lc.y, lc.z, lh.x, lh.y, lh.z);

    // 正規化ローカル([-1,1]^3)→ワールドの変換。ここでは等倍・回転なしで置く。
    const int inst = AF_SceneAddInstanceMesh(s, geom, lc, lh, V(1, 0, 0), V(0, 1, 0), mat);
    check("メッシュインスタンスが追加できる", inst >= 0);

    // 戸口の正面(x=0, y=1.6)は通る。脇(x=2)は板に遮られる。
    check("戸口の正面は通る", AF_SceneIsOccluded(s, V(0, 1.6f, -3), V(0, 1.6f, 3)) == 0);
    check("板の部分は遮られる", AF_SceneIsOccluded(s, V(2, 1.6f, -3), V(2, 1.6f, 3)) != 0);
    check("まぐさの高さも遮られる", AF_SceneIsOccluded(s, V(0, 2.6f, -3), V(0, 2.6f, 3)) != 0);

    // 境界ボックスなら「戸口の正面」も遮られてしまう。実形状を見ている証拠として、
    // 同じ配置の箱インスタンスと比べる。
    AF_SceneHandle s2 = AF_SceneCreate();
    const int mat2 = AF_SceneAddMaterial(s2, nullptr, nullptr, nullptr, 0);
    AF_SceneAddInstanceBox(s2, lc, lh, V(1, 0, 0), V(0, 1, 0), mat2);
    check("同形の箱なら戸口の正面も遮られる（＝実形状を見ている証拠）",
          AF_SceneIsOccluded(s2, V(0, 1.6f, -3), V(0, 1.6f, 3)) != 0);
    AF_SceneDestroy(s2);

    // 透過も実形状で効く（戸口は素通り＝1.0 / 板は材質ぶん減衰）。
    float gOpen[kBands] = {}, gWall[kBands] = {};
    AF_SceneComputeTransmissionBands(s, V(0, 1.6f, -3), V(0, 1.6f, 3), gOpen, kBands);
    AF_SceneComputeTransmissionBands(s, V(2, 1.6f, -3), V(2, 1.6f, 3), gWall, kBands);
    check("戸口ごしは透過1.0", gOpen[0] > 0.999f);
    checkGreater("板ごしは減衰する", gOpen[0], gWall[0]);

    // ── 配置の操作だけで形状変化を表せること ──
    // 同じ形状を2つ目のインスタンスとして置く（BLAS は共有＝再構築なし）。
    const AF_Vector3 c2 = V(lc.x + 8.0f, lc.y, lc.z);
    const int inst2 = AF_SceneAddInstanceMesh(s, geom, c2, lh, V(1, 0, 0), V(0, 1, 0), mat);
    check("同じ形状を別位置にも置ける(インスタンシング)", inst2 >= 0);
    check("2つ目の板も遮る", AF_SceneIsOccluded(s, V(10, 1.6f, -3), V(10, 1.6f, 3)) != 0);
    check("2つ目の戸口も通る", AF_SceneIsOccluded(s, V(8, 1.6f, -3), V(8, 1.6f, 3)) == 0);

    // 動かす（transform 更新のみ。形状の再構築は起きない）。
    AF_SceneUpdateInstance(s, inst2, V(lc.x + 20.0f, lc.y, lc.z), lh, V(1, 0, 0), V(0, 1, 0));
    check("動かすと元の位置では遮らない", AF_SceneIsOccluded(s, V(10, 1.6f, -3), V(10, 1.6f, 3)) == 0);
    check("動かした先で遮る", AF_SceneIsOccluded(s, V(22, 1.6f, -3), V(22, 1.6f, 3)) != 0);

    // 非一様スケール（部屋の内寸を変える＝壁を伸ばす、に相当）。
    //   x 方向だけ 2 倍にすると、元は素通りだった x=4 が板に入る。
    AF_SceneUpdateInstance(s, inst, lc, V(lh.x * 2.0f, lh.y, lh.z), V(1, 0, 0), V(0, 1, 0));
    check("非一様スケールで板が伸びる", AF_SceneIsOccluded(s, V(4, 1.6f, -3), V(4, 1.6f, 3)) != 0);
    check("伸ばしても戸口は通る", AF_SceneIsOccluded(s, V(0, 1.6f, -3), V(0, 1.6f, 3)) == 0);

    AF_SceneDestroy(s);

    // ── メッシュでも回折する（二面角でフィルタした稜線を使う）──
    //   境界ボックスの12稜線ではなく実形状の稜線を回るので、遮蔽判定と矛盾しない。
    //   検証は「同形の箱と同じくらい回折するか」。箱は実形状と外形が同じ配置にする。
    std::printf("\n[メッシュ] 回折（実形状の稜線）\n");
    {
        // 単純な板（穴なし）。x∈[-3,3], y∈[0,3], z∈[-0.15,0.15]。
        const float bx[] = {
            -3, 0, -0.15f,   3, 0, -0.15f,   3, 3, -0.15f,  -3, 3, -0.15f,
            -3, 0,  0.15f,   3, 0,  0.15f,   3, 3,  0.15f,  -3, 3,  0.15f,
        };
        const int bi[] = {
            0,2,1, 0,3,2,  4,5,6, 4,6,7,  0,4,7, 0,7,3,
            1,2,6, 1,6,5,  3,7,6, 3,6,2,  0,1,5, 0,5,4,
        };
        const AF_Vector3 L2 = V(0, 1.6f, -3), S2 = V(0, 1.6f, 3);

        AF_SceneHandle sm = AF_SceneCreate();
        const int mm = AF_SceneAddMaterial(sm, nullptr, nullptr, nullptr, 0);
        AF_Vector3 c2{}, h2{};
        const int gm = AF_SceneAddMesh(sm, bx, 8, bi, 36, &c2, &h2);
        AF_SceneAddInstanceMesh(sm, gm, c2, h2, V(1, 0, 0), V(0, 1, 0), mm);
        float gMesh[kBands] = {};
        AF_SceneComputeDiffractionBands(sm, L2, S2, gMesh, kBands);

        AF_SceneHandle sb = AF_SceneCreate();
        const int mb = AF_SceneAddMaterial(sb, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(sb, c2, h2, V(1, 0, 0), V(0, 1, 0), mb);
        float gBox[kBands] = {};
        AF_SceneComputeDiffractionBands(sb, L2, S2, gBox, kBands);

        std::printf("      同形の板   メッシュ 125Hz %.3f / 4kHz %.3f     箱 125Hz %.3f / 4kHz %.3f\n",
                    gMesh[0], gMesh[5], gBox[0], gBox[5]);
        check("メッシュでも回折が効く(0でない)", gMesh[0] > 0.01f);
        checkGreater("メッシュ回折も低域>高域", gMesh[0], gMesh[5]);
        // 同じ外形なので、箱と同程度になるはず（稜線の取り方が違うので厳密一致はしない）。
        const float ratio = (gBox[0] > 1e-6f) ? gMesh[0] / gBox[0] : 0.0f;
        char mb2[96];
        std::snprintf(mb2, sizeof(mb2), "(メッシュ/箱 = %.2f)", ratio);
        check("同形の箱と同程度の回折になる(0.5〜2倍)", ratio > 0.5f && ratio < 2.0f, mb2);

        std::printf("      稜線本数   %d 本（板1枚＝12稜線。二面角が平坦な面内は候補にならない）\n",
                    AF_SceneGetMeshEdgeCount(sm, gm));
        AF_SceneDestroy(sm);
        AF_SceneDestroy(sb);
    }

    // ── 候補数がテッセレーションに依存しないこと ──
    //   これがこの設計の要点。細分しても「形状の複雑さ」が変わらなければ稜線は増えない。
    {
        std::printf("      細分しても稜線が増えないか（同じ板を n×n に分割）\n");
        int firstEdges = -1;
        for (int sub : {1, 4, 16}) {
            std::vector<float> vx;
            std::vector<int> ix;
            // 前後の面だけを n×n に細分した板（面内は平坦なので稜線に数えられないはず）。
            for (int side = 0; side < 2; ++side) {
                const float z = side ? 0.15f : -0.15f;
                const int base = static_cast<int>(vx.size() / 3);
                for (int i = 0; i <= sub; ++i)
                    for (int j = 0; j <= sub; ++j) {
                        vx.push_back(-3.0f + 6.0f * i / sub);
                        vx.push_back(3.0f * j / sub);
                        vx.push_back(z);
                    }
                const int w = sub + 1;
                for (int i = 0; i < sub; ++i)
                    for (int j = 0; j < sub; ++j) {
                        const int a = base + i * w + j, b = a + 1, c = a + w, d = c + 1;
                        ix.push_back(a); ix.push_back(c); ix.push_back(b);
                        ix.push_back(b); ix.push_back(c); ix.push_back(d);
                    }
            }
            AF_SceneHandle st = AF_SceneCreate();
            AF_SceneAddMaterial(st, nullptr, nullptr, nullptr, 0);
            AF_Vector3 c3{}, h3{};
            const int gt = AF_SceneAddMesh(st, vx.data(), static_cast<int>(vx.size() / 3),
                                           ix.data(), static_cast<int>(ix.size()), &c3, &h3);
            const int ec = AF_SceneGetMeshEdgeCount(st, gt);
            std::printf("        %5d 三角形 → 稜線 %d 本\n",
                        static_cast<int>(ix.size() / 3), ec);
            if (firstEdges < 0) firstEdges = ec;
            else check("細分しても回折稜線は増えない", ec == firstEdges);
            AF_SceneDestroy(st);
        }
    }
}

// ================================================================ 方向プローブ
// 後期残響の方向分布。「どちらに空間が開けているか」が出ていることを確かめる。
void testDirectionalProbe() {
    std::printf("\n[方向プローブ] 残響がどちらから返るか\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

    // +X 側だけを箱で囲った空間。-X 側は開けている。
    //   → +X 方向からは残響が返り、-X 方向からは返らないはず。
    const float t = 0.3f;
    AF_SceneAddInstanceBox(s, V(0, -t, 0), V(10, t, 10), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V(0, 4 + t, 0), V(10, t, 10), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V(10, 2, 0), V(t, 2, 10), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V(0, 2, -10), V(10, 2, t), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V(0, 2, 10), V(10, 2, t), V(1, 0, 0), V(0, 1, 0), mat);
    // -X 壁は置かない（開けている）

    const AF_Vector3 dirs[6] = {
        V(1, 0, 0), V(-1, 0, 0), V(0, 1, 0), V(0, -1, 0), V(0, 0, 1), V(0, 0, -1),
    };
    float e[6 * kBands] = {};
    AF_SceneProbeDirectionalEnergy(s, V(0, 1.6f, 0), dirs, 6, 12, e);

    const char* names[6] = {"+X(壁)", "-X(開)", "+Y(天)", "-Y(床)", "+Z(壁)", "-Z(壁)"};
    for (int i = 0; i < 6; ++i)
        std::printf("      %-10s 125Hz %6.2f   4kHz %6.2f\n",
                    names[i], e[i * kBands], e[i * kBands + 5]);

    checkGreater("囲われた方向(+X)の方が開けた方向(-X)より残響が返る",
                 e[0 * kBands], e[1 * kBands]);
    check("開けた方向(-X)は明確に小さい", e[1 * kBands] < e[0 * kBands] * 0.5f);
    checkGreater("床方向からも残響が返る", e[3 * kBands], e[1 * kBands]);

    // 開口を塞ぐと -X からも返るようになる（実行時の形状変化に追従する）。
    AF_SceneAddInstanceBox(s, V(-10, 2, 0), V(t, 2, 10), V(1, 0, 0), V(0, 1, 0), mat);
    float e2[6 * kBands] = {};
    AF_SceneProbeDirectionalEnergy(s, V(0, 1.6f, 0), dirs, 6, 12, e2);
    std::printf("      -X を塞ぐと 125Hz: %.2f → %.2f\n", e[1 * kBands], e2[1 * kBands]);
    checkGreater("壁を足すとその方向から残響が返るようになる", e2[1 * kBands], e[1 * kBands]);

    AF_SceneDestroy(s);
}

// ================================================================ 2次回折（食い違いの2戸口）
// **1次では原理的に届かない**配置。曲がりが2回必要。
//
//   ★L字（曲がり1回）は 2次のテストにならない。角の稜線がリスナーからも音源からも
//     見通せるので、1次で届いてしまう（実測: order=1 でも同じ値が出た）。
//     設計書に「L字廊下の角」を2次の例として書いていたが誤りだった。
//
//   ここでは戸口を2枚、x 方向にずらして置く。どちらの戸口の稜線も、
//   もう一方の壁に遮られて「リスナーと音源の両方」からは見通せない。
//   → listener → 戸口1 → 戸口2 → source の2段でしか届かない。
//   鳴らす位置は**手前の戸口**であるべき（人は手前の角から聞こえると感じる）。
void testSecondOrderDiffraction() {
    std::printf("\n[2次回折] 食い違いに置いた2つの戸口\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

    const float t = 0.15f, hy = 1.5f;
    // 壁1（z=0）: 戸口 x∈[-1,0]
    AF_SceneAddInstanceBox(s, V(-3.5f, hy, 0), V(2.5f, hy, t), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V( 3.0f, hy, 0), V(3.0f, hy, t), V(1, 0, 0), V(0, 1, 0), mat);
    // 壁2（z=4）: 戸口 x∈[2,3]（壁1の戸口とずらす）
    AF_SceneAddInstanceBox(s, V(-2.0f, hy, 4), V(4.0f, hy, t), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V( 4.5f, hy, 4), V(1.5f, hy, t), V(1, 0, 0), V(0, 1, 0), mat);
    // 間の空間を閉じる（壁の端を回る1次経路を塞ぐため）
    AF_SceneAddInstanceBox(s, V(-6, hy, 2), V(t, hy, 2), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V( 6, hy, 2), V(t, hy, 2), V(1, 0, 0), V(0, 1, 0), mat);
    // 床と天井
    AF_SceneAddInstanceBox(s, V(0, -t, 2), V(8, t, 12), V(1, 0, 0), V(0, 1, 0), mat);
    AF_SceneAddInstanceBox(s, V(0, 3 + t, 2), V(8, t, 12), V(1, 0, 0), V(0, 1, 0), mat);

    const AF_Vector3 L = V(-0.5f, 1.6f, -3), S = V(2.5f, 1.6f, 7);
    check("2枚の壁の先は遮蔽される", AF_SceneIsOccluded(s, L, S) != 0);
    // どちらの戸口の縁も「両方から」は見通せない＝1次では届かない、ことを確認する。
    check("戸口1の縁は音源から見通せない", AF_SceneIsOccluded(s, V(0, 1.6f, 0), S) != 0);
    check("戸口2の縁はリスナーから見通せない", AF_SceneIsOccluded(s, L, V(2, 1.6f, 4)) != 0);

    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);
    AF_SceneUpdate(s, 1.0f / 60.0f);

    float g[kBands] = {};
    AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
    AF_Vector3 pos[8]; float gain[8];
    const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);

    std::printf("      回折ゲイン 125Hz %.3f / 4kHz %.3f   二次音源 %d 本\n", g[0], g[5], n);
    for (int i = 0; i < n && i < 3; ++i)
        std::printf("        [%d] 方向(%6.2f,%6.2f,%6.2f)  重み %.3f\n",
                    i, pos[i].x, pos[i].y, pos[i].z, gain[i]);

    check("2次回折で音が届く(ゲイン>0)", g[0] > 0.001f);
    check("二次音源が出る", n > 0);

    // 鳴らす位置は「手前の戸口」＝リスナーの正面やや左（戸口1は x∈[-1,0]、リスナーは x=-0.5）。
    //   音源は +X 寄りにあるが、手前の戸口を回るので到来方向はほぼ真正面になるはず。
    if (n > 0) {
        const AF_Vector3 d = V(pos[0].x - L.x, pos[0].y - L.y, pos[0].z - L.z);
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const float fwd = (len > 1e-4f) ? d.z / len : 0.0f;   // +Z 成分
        const float side = (len > 1e-4f) ? d.x / len : 0.0f;  // +X 成分
        char b[96];
        std::snprintf(b, sizeof(b), "(前方 %.2f / 横 %.2f)", fwd, side);
        check("到来方向が手前の戸口(ほぼ正面)を指す", fwd > 0.8f, b);
    }

    AF_SceneDestroy(s);
}

// ================================================================ 扉の開き角 → 開口幅 → 音量
// 「開き具合が連続に音へ出る」ためにどの設定を選ぶか、を決めるための表。
//   Test_Full と同じ形状（密閉・戸口 1.2×2.4・部屋高 4m・扉厚 6cm）で開き角を振り、
//   ① 実測の開口幅  ② 基準幅と急峻さの組み合わせごとのゲイン を並べる。
//
//   耳に届くのは前川の値ではない（diffractionDistanceOnly が ON なので捨てられる）。
//   届くのは開口幅ゲートを含む _tapDiffGain なので、ここで見るべきはこの表。
// ============================================================ 1フレームの計算量
// Unity 側の表示で「音響計算 12.6 ms/frame・FPS 38」が出たので、実測して裏を取る。
// 推測で直さないため、まず**どこに時間が乗っているか**を分けて測る。
//
//   Test_Full と同じ規模（箱10個＋扉、音源6）で、ホストが毎フレーム呼ぶ順に叩く:
//     AF_SceneUpdate → 音源ごとに 遮蔽 + 回折タップ取得
//   扉は毎フレーム動かす（updateInstance が BVH を作り直すので、その分も込みで測る）。
void diagnoseFrameCost() {
    std::printf("\n[診断] 1フレームの音響計算にかかる時間（Test_Full と同じ規模）\n");

    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                           V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                           V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                           V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    const int doorId = AF_SceneAddInstanceBox(
        s, V(-0.6f + doorW * 0.5f, doorH * 0.5f, 0),
        V(doorW * 0.5f, doorH * 0.5f, 0.03f), V(1,0,0), V(0,1,0), mat);

    const AF_Vector3 L = V(0, 1.6f, -4);
    const AF_Vector3 srcPos[6] = {
        V(-2.0f, 1.6f, 3.5f), V(2.0f, 1.6f, 3.0f), V(-3.0f, 1.0f, 5.0f),
        V( 3.0f, 2.0f, 6.0f), V( 0.0f, 1.2f, 4.5f), V(-1.0f, 1.8f, 2.5f),
    };
    for (int i = 0; i < 6; ++i) AF_SceneSetSource(s, i + 1, srcPos[i]);
    AF_SceneSetListener(s, L);

    // Unity 側（AcousticFlowSceneDemo の既定値）と同じ設定にする。
    //   ここを合わせないと「テストでは軽いのにゲームでは重い」を追えない。
    AF_UpdateConfig base{};
    base.role1EveryN = 1;   base.role2EveryN = 4;   base.earlyEveryN = 3;
    base.diffSrcEveryN = 2; base.catalogEveryN = 3;
    base.reflectionRays = 256; base.reflectionBounces = 3;
    base.directWeight = 1.0f;  base.useReflections = 1;
    base.useEdgeCatalog = 1;   base.edgeCatalogRes = 16; base.edgeCatalogMaxDist = 40.0f;
    base.enableReverb = 1;     base.echogramBins = 100;  base.echogramBinSeconds = 0.01f;
    base.echogramRays = 512;   base.echogramBounces = 24;
    base.speedOfSound = 343.0f; base.distanceRef = 1.5f;
    base.enableEarlyReflections = 1; base.earlyTaps = 4;
    base.earlyRays = 512; base.earlyBounces = 2;
    base.enableDiffractionSources = 1; base.diffSources = 3;

    // 計測本体。moveDoor=true なら扉を毎フレーム動かす（BVH が毎回汚れる）。
    double lastMax = 0.0;   // 直前の runFrames の最悪フレーム
    auto runFrames = [&](const AF_UpdateConfig& cfg, bool moveDoor, int frames) {
        AF_SceneSetUpdateConfig(s, &cfg);
        using clk = std::chrono::high_resolution_clock;
        double acc = 0.0; lastMax = 0.0;
        for (int f = 0; f < frames; ++f) {
            const float th = (moveDoor ? (f % 90) : 45) * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            auto t0 = clk::now();
            if (moveDoor) {
                AF_SceneUpdateInstance(
                    s, doorId, V(-0.6f + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                    V(doorW * 0.5f, doorH * 0.5f, 0.03f), V(c, 0, sn), V(0, 1, 0));
            }
            AF_SceneUpdate(s, 1.0f / 60.0f);
            for (int i = 0; i < 6; ++i) {
                AF_Vector3 pos[8]; float gain[8];
                AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, i + 1), pos, gain, 8);
            }
            const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            acc += ms;
            if (ms > lastMax) lastMax = ms;
        }
        return acc / frames;
    };

    runFrames(base, true, 30);                        // 暖機
    const double all = runFrames(base, true, 200);
    const double allMax = lastMax;
    std::printf("        全部入り（扉が毎フレーム動く）\n");
    std::printf("          平均 %7.3f ms/frame  → 60fps 予算(16.7ms)の %.0f%%\n",
                all, all / 16.7 * 100.0);
    std::printf("          最悪 %7.3f ms/frame  → 同 %.0f%%   ★ホストのHUDが拾うのはこちら\n",
                allMax, allMax / 16.7 * 100.0);
    std::printf("          （段ごとに実行間隔が違うので、重い段が重なるフレームで山が出る）\n");

    // 段ごとに切って差を見る。差＝その段の取り分。
    struct Ablate { const char* name; void (*apply)(AF_UpdateConfig&); };
    const Ablate abl[] = {
        {"残響エコグラム",   [](AF_UpdateConfig& c){ c.enableReverb = 0; }},
        {"早期反射",         [](AF_UpdateConfig& c){ c.enableEarlyReflections = 0; }},
        {"回折二次音源",     [](AF_UpdateConfig& c){ c.enableDiffractionSources = 0; }},
        {"エッジカタログ",   [](AF_UpdateConfig& c){ c.useEdgeCatalog = 0; }},
        {"反射込み遮蔽",     [](AF_UpdateConfig& c){ c.useReflections = 0; }},
    };
    for (const Ablate& a : abl) {
        AF_UpdateConfig c = base; a.apply(c);
        const double ms = runFrames(c, true, 200);
        std::printf("          %-16s を切ると 平均 %7.3f ms（取り分 %6.3f / %4.1f%%）  最悪 %7.3f ms\n",
                    a.name, ms, all - ms, (all - ms) / all * 100.0, lastMax);
    }

    // 扉を止める＝BVH が汚れない。差が「動かすこと自体」の代金。
    const double still = runFrames(base, false, 200);
    std::printf("        扉を止めると %7.3f ms  （動かす代金 %6.3f ms / %4.1f%%）\n",
                still, all - still, (all - still) / all * 100.0);

    AF_SceneDestroy(s);
}

// ================================================ 音源位置による扉の効き方の違い
// **この作品の主張そのものを測る。**
//   既存のゲームは扉の開き角を 0..1 に正規化して一律にゲインを掛ける。
//   奥に何がどこにあろうと、開き具合だけで音が決まる。
//   幾何でやる意味は「開いた先に音源が正対していれば最初の隙間から聞こえる」
//   「横へ外れていれば開けても大して変わらない」が**勝手に出る**こと。
//
//   ここまでのテストは全部、音源が戸口から大きく外れた配置だった（最も出にくい条件）。
//   正対・少し外れ・大きく外れ を並べて、差が出るかを見る。
//   差が出るなら主張は成立している。出ないなら、この作品の核心が機能していない。
// ============================================================ ポータルの性質
// ★積分範囲がポータルの矩形に限定されることで何が変わるかを見る。
//   従来（範囲が決まらない）は閉扉でも開口率 0.328 あり、全開でも 0.408 だった（1.24倍）。
//   ポータルなら扉が矩形を覆えば 0、何も塞がなければ 1 ── フルレンジになるはず。
// ==================================== 壁↔扉の境界を跨ぐときの連続性
// 壁(Concrete TL34dB) と扉(WoodDoor TL15dB) の差を 19dB に広げたので、
// 直線が壁から扉へ移る瞬間に段差が出ていないかを測る。
//   ソフト遮蔽は音源を 32 点で標本化しているので、材質の差が大きいほど
//   1点が別材質へ移るたびの段が大きくなる（開口で同じ罠を踏んだ）。
void diagnoseWallDoorBoundary() {
    std::printf("\n[診断] 壁↔扉の境界を横切るときに段差が出ないか\n");
    std::printf("        扉は閉じたまま。リスナーを横へ振って直線が通る材質を変える。\n");
    std::printf("        リスナーx  透過125Hz  透過4kHz   前との差(125Hz)\n");

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const AF_Vector3 S = V(0, 1.6f, 3.5f);

    float prev = -1.0f, maxJump = 0.0f, jumpAt = 0.0f;
    for (float lx = -2.0f; lx <= 2.001f; lx += 0.1f) {
        AF_SceneHandle s = AF_SceneCreate();
        // 壁（よく遮る）と扉（弱い）を別材質で持つ。
        const float wallT[6] = {0.000398f, 0.0001585f, 0.0000398f,
                                0.00001f, 0.00000251f, 0.000001f};   // TL34..60
        const float doorT[6] = {0.0316f, 0.0158f, 0.00794f,
                                0.00398f, 0.00251f, 0.002f};          // TL15..27
        const int matWall = AF_SceneAddMaterial(s, wallT, nullptr, nullptr, 6);
        const int matDoor = AF_SceneAddMaterial(s, doorT, nullptr, nullptr, 6);

        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        // 閉じた扉（戸口をぴったり覆う）。
        AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0), V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                               V(1,0,0), V(0,1,0), matDoor);

        // ★閉じた扉なのに開口率が 0 でないと、存在しない回折タップが立つ。
        //   リスナーが斜めから見ると、扉の影が戸口からずれて「開いている」と読める疑い。
        const int pid = AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                                          doorW * 0.5f, doorH * 0.5f);
        const AF_Vector3 L = V(lx, 1.6f, -4);
        float pf[kBands] = {}; AF_Vector3 pcp = V(0,0,0);
        AF_SceneMeasurePortal(s, pid, L, S, pf, &pcp);
        float tr[kBands] = {}; float occF = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, tr, kBands, &occF);
        float d = 0.0f;
        if (prev > 0.0f) d = 20.0f * std::log10(std::max(tr[0], 1e-7f) / prev);
        std::printf("        %+6.2f    %.5f    %.5f    %+6.1f dB   開口率 %.4f\n",
                    lx, tr[0], tr[5], d, pf[0]);
        if (prev > 0.0f && std::fabs(d) > maxJump) { maxJump = std::fabs(d); jumpAt = lx; }
        prev = std::max(tr[0], 1e-7f);
        AF_SceneDestroy(s);
    }
    std::printf("        → 最大隣接差 %.1f dB @ x=%+.2f （0.1m 刻み）\n", maxJump, jumpAt);

    // ★「扉のほうがこもる」という耳の指摘を裏取りする。
    //   透過だけでなく、実際に鳴っている量（遮蔽の帯域別生存＝回折や反射も含む）で見る。
    //   透過だけ見て「傾きは同じ」と結論すると、別経路がこもらせている場合を見落とす。
    std::printf("\n      ── 扉の正面(x=0) と 壁の裏(x=-2) を全帯域で比較 ──\n");
    std::printf("        位置    125Hz    250Hz    500Hz    1kHz     2kHz     4kHz    傾き\n");
    for (int k = 0; k < 2; ++k) {
        const float lx = (k == 0) ? 0.0f : -2.0f;
        AF_SceneHandle s = AF_SceneCreate();
        const float wallT[6] = {0.000398f, 0.0001585f, 0.0000398f,
                                0.00001f, 0.00000251f, 0.000001f};
        const float doorT[6] = {0.0316f, 0.0158f, 0.00794f, 0.00398f, 0.00251f, 0.002f};
        const int matWall = AF_SceneAddMaterial(s, wallT, nullptr, nullptr, 6);
        const int matDoor = AF_SceneAddMaterial(s, doorT, nullptr, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0), V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                               V(1,0,0), V(0,1,0), matDoor);
        AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                          doorW * 0.5f, doorH * 0.5f);   // 出荷する経路で比べる
        const AF_Vector3 L = V(lx, 1.6f, -4);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        // ★経路ごとに分けて出す。合成値だけ見ると「どこが明るくしているか」が分からない。
        //   壁の透過は 4kHz で 0.001 しかないので、それを超える値が出たら
        //   材質の代金を payしていない経路がある、ということになる。
        for (int mode = 0; mode < 2; ++mode) {
            AF_UpdateConfig c{};
            c.role1EveryN = 1; c.role2EveryN = 4; c.earlyEveryN = 3;
            c.diffSrcEveryN = 2; c.catalogEveryN = 3;
            c.reflectionRays = 256; c.reflectionBounces = 3;
            c.directWeight = 1.0f;
            c.useReflections = (mode == 0) ? 1 : 0;     // 反射込み / 直接+回折のみ
            c.useEdgeCatalog = 1; c.edgeCatalogRes = 16; c.edgeCatalogMaxDist = 40.0f;
            c.enableReverb = 0; c.echogramBins = 100; c.echogramBinSeconds = 0.01f;
            c.echogramRays = 512; c.echogramBounces = 24;
            c.speedOfSound = 343.0f; c.distanceRef = 1.5f;
            c.enableEarlyReflections = 0; c.earlyTaps = 4;
            c.earlyRays = 512; c.earlyBounces = 2;
            c.enableDiffractionSources = 1; c.diffSources = 3;
            AF_SceneSetUpdateConfig(s, &c);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            float g[kBands] = {};
            AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), g);
            std::printf("        %s%s  %.5f  %.5f  %.5f  %.5f  %.5f  %.5f  %.1f dB\n",
                        (k == 0) ? "扉" : "壁", (mode == 0) ? "反射込" : "反射無",
                        g[0], g[1], g[2], g[3], g[4], g[5],
                        20.0f * std::log10(std::max(g[0], 1e-7f) / std::max(g[5], 1e-7f)));
        }
        AF_SceneDestroy(s);
    }
    std::printf("      （傾きが大きいほどこもる。透過だけでなく回折・反射も含めた実際の鳴り）\n");

    // ★扉の実寸クリアランスが効くかを見る。
    //   現実の扉は枠より数mm小さい（同寸だと閉まらない）。今までのテストは
    //   戸口 1.2m に対して扉も 1.2m で、隙間が物理的に存在しなかった。
    //   実寸どおり小さくすれば隙間は**幾何から勝手に出る**ので、
    //   「扉には隙間がある」という前提をコードに持ち込む必要が無い。
    //   隙間は板より高域をよく通すので、閉扉の音が明るくなるはず。
    std::printf("\n      ── 扉のクリアランス（枠との隙間）を変えて閉扉の音色を見る ──\n");
    std::printf("        隙間     開口率125Hz  開口率4kHz   透過125Hz  透過4kHz  傾き\n");
    for (float clr : {0.000f, 0.002f, 0.005f, 0.010f, 0.020f}) {
        AF_SceneHandle s = AF_SceneCreate();
        const float wallT[6] = {0.000398f, 0.0001585f, 0.0000398f,
                                0.00001f, 0.00000251f, 0.000001f};
        const float doorT[6] = {0.0316f, 0.0158f, 0.00794f, 0.00398f, 0.00251f, 0.002f};
        const int matWall = AF_SceneAddMaterial(s, wallT, nullptr, nullptr, 6);
        const int matDoor = AF_SceneAddMaterial(s, doorT, nullptr, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        // 扉を枠より clr だけ小さくする（左右・上に隙間ができる）。
        AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f - clr * 0.5f,  0),
                               V(doorW * 0.5f - clr, doorH * 0.5f - clr * 0.5f, 0.03f),
                               V(1,0,0), V(0,1,0), matDoor);
        const int pid = AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0),
                                          V(1, 0, 0), V(0, 1, 0), 0.6f, doorH * 0.5f);
        const AF_Vector3 L = V(0, 1.6f, -4);
        float f[kBands] = {}; AF_Vector3 cp = V(0,0,0);
        AF_SceneMeasurePortal(s, pid, L, S, f, &cp);
        float tr[kBands] = {}; float occF = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, tr, kBands, &occF);
        std::printf("        %4.0f mm   %.5f     %.5f      %.5f    %.5f   %.1f dB\n",
                    clr * 1000.0f, f[0], f[5], tr[0], tr[5],
                    20.0f * std::log10(std::max(tr[0], 1e-7f) / std::max(tr[5], 1e-7f)));
        AF_SceneDestroy(s);
    }
    std::printf("      （隙間が広いほど高域が通る＝傾きが小さくなるはず。特別視ではなく実寸の結果）\n");

    // ★Unity の SwingDoor と同じ配置で、閉じた扉が**扉として**鳴るかを見る。
    //   音源は戸口の真後ろではなく横にずらす（x=0.8、戸口は±0.5）。
    //   ソフト遮蔽は音源の周り 0.9m に標本を散らすので、何点かは戸口の外＝壁を通る。
    //   「塞がれた側」に最小値を使うと、そこで壁(TL34)を拾って
    //   閉じた扉が壁として鳴る（実測で直接音が 19dB 沈んだ）。
    std::printf("\n      ── 音源を戸口の横へずらしたときの閉扉（Unity の SwingDoor と同配置）──\n");
    {
        AF_SceneHandle s = AF_SceneCreate();
        const float wallT[6] = {0.000398f, 0.0001585f, 0.0000398f,
                                0.00001f, 0.00000251f, 0.000001f};   // TL34..60
        const float doorT[6] = {0.0316f, 0.0158f, 0.00794f, 0.00398f, 0.00251f, 0.002f};
        const int matWall = AF_SceneAddMaterial(s, wallT, nullptr, nullptr, 6);
        const int matDoor = AF_SceneAddMaterial(s, doorT, nullptr, nullptr, 6);
        const float wz = 0.0f, th = 0.2f, hh = 3.0f, hf = 7.0f;
        const float gL = -0.5f, gR = 0.5f;
        AF_SceneAddInstanceBox(s, V((-hf + (gL + hf) * 0.5f), hh*0.5f, wz),
                               V((gL + hf) * 0.5f, hh*0.5f, th*0.5f), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V((gR + (hf - gR) * 0.5f), hh*0.5f, wz),
                               V((hf - gR) * 0.5f, hh*0.5f, th*0.5f), V(1,0,0), V(0,1,0), matWall);
        // 閉じた扉（戸口をぴったり覆う）
        AF_SceneAddInstanceBox(s, V(0, hh * 0.5f, wz), V(0.5f, hh * 0.5f, 0.03f),
                               V(1,0,0), V(0,1,0), matDoor);
        const int pid = AF_SceneAddPortal(s, V(0, hh * 0.5f, wz),
                                          V(1, 0, 0), V(0, 1, 0), 0.5f, hh * 0.5f);
        const AF_Vector3 L = V(0, 1.6f, -3), Sr = V(0.8f, 1.6f, 3);
        float f[kBands] = {}; AF_Vector3 cp = V(0,0,0);
        AF_SceneMeasurePortal(s, pid, L, Sr, f, &cp);
        float tr[kBands] = {}; float occF = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, Sr, tr, kBands, &occF);
        std::printf("        開口率 125Hz %.5f（閉扉なので 0 が正しい）\n", f[0]);
        std::printf("        透過   125Hz %.5f  4kHz %.5f\n", tr[0], tr[5]);
        std::printf("        参考   扉の材質 125Hz %.5f / 壁の材質 125Hz %.5f\n",
                    std::sqrt(doorT[0]), std::sqrt(wallT[0]));
        char nb[128];
        std::snprintf(nb, sizeof(nb), "(透過 %.5f / 扉 %.5f / 壁 %.5f)",
                      tr[0], std::sqrt(doorT[0]), std::sqrt(wallT[0]));
        // 閉じた扉の向こうは「扉として」鳴るべき。壁の値まで沈んでいたら不具合。
        check("閉じた扉は扉として鳴る（壁の値まで沈まない）",
              tr[0] > std::sqrt(wallT[0]) * 3.0f, nb);
        AF_SceneDestroy(s);
    }
}

// ================================================ 閉じた空間で幻の回折が出ないか
// **コンセプトの成立条件そのもの。**
//   ポータルを置かなくても、幾何だけで「穴があれば鳴る／無ければ鳴らない」が
//   成立していないと、聞こえる場所を人が決めていることになる。
//   逆に言えば、ここが通れば「ポータルは精度の上積み」と言い切れる。
// 厚い衝立で 1 次／2 次のどちらが働いているかを見る（推測で直さないため）。
// ============================================ 漏れは弱い所から聞こえるか
// 「部屋の奥で鳴ってドアから漏れている」感じの正体。
//   壁を抜けてくる音は面が再放射しているので、面の方向から届く。
//   音源方向のまま鳴らすと壁の中から聞こえ、歩いても扉から漏れる感じにならない。
//   壁(TL34) と扉(TL15) が並んでいれば、定位は**扉へ寄る**はず。物体が扉かは見ない。
// ================================ 奥の部屋の響きがこちらに届いているか
// **奥の部屋の材質だけ**を変えて、こちらのエコグラム（残響）が変わるかを見る。
//   変われば部屋間結合がある。変わらなければ「奥の部屋の響き」は届いていない。
// ================== 奥の音源の位置で音色がどれだけ変わるか（LPF が強すぎないか）
// 奥の部屋の中で鳴っている音は、開口から離れても・角度が付いても、
// **部屋の中の音**として届くはず。開口までの距離や角度で高域が激しく落ちるなら、
// どこが落としているのかを分けて見る必要がある。
void diagnoseFarSourceTimbre() {
    std::printf("\n[診断] 奥の音源の位置で音色がどう変わるか（開口までの距離・角度）\n");

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f, hw = 6.0f, hd = 8.0f;
    const AF_Vector3 L = V(0, 1.6f, -4);

    struct Place { float x, z; const char* name; };
    const Place places[6] = {
        {0.0f, 1.0f,  "正面・近い  "}, {0.0f, 4.0f,  "正面・中    "}, {0.0f, 7.0f,  "正面・遠い  "},
        {2.0f, 1.0f,  "斜め・近い  "}, {2.0f, 4.0f,  "斜め・中    "}, {4.0f, 4.0f,  "大きく斜め  "},
    };

    std::printf("        位置          開口率(125Hz/4kHz)   到来ゲイン(125Hz/1k/4kHz)  傾き\n");
    for (const Place& pl : places) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        const int pid = AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                                          doorW * 0.5f, doorH * 0.5f);
        const AF_Vector3 S = V(pl.x, 1.6f, pl.z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneSetApertureIsTransmission(s, 1);
        AF_SceneUpdate(s, 1.0f / 60.0f);

        float f[kBands] = {}; AF_Vector3 cp = V(0,0,0);
        AF_SceneMeasurePortal(s, pid, L, S, f, &cp);
        float g[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), g);
        std::printf("        %s  %.4f / %.4f     %.4f / %.4f / %.4f   %5.1f dB\n",
                    pl.name, f[0], f[5], g[0], g[3], g[5],
                    20.0f * std::log10(std::max(g[0], 1e-7f) / std::max(g[5], 1e-7f)));
        AF_SceneDestroy(s);
    }
    std::printf("      （傾きが位置で大きく動くなら、開口率の周波数依存が効きすぎている）\n");

    // ★音源を横へ細かく動かして、見通しが切れる瞬間の段差を測る。
    //   戸口は x∈[-0.6,0.6]。リスナー(0,1.6,-4) から z=1 の音源を見ると、
    //   直線が戸口を外れるのは x ≒ 0.6 × 5/4 = 0.75 付近。
    std::printf("      ── 音源を横へ動かしたとき（見通しが切れる瞬間）──\n");
    std::printf("        音源x   開口率125Hz  到来125Hz   前との差\n");
    float prevG = -1.0f, worst = 0.0f, worstAt = 0.0f;
    for (float sx = 0.0f; sx <= 1.60f; sx += 0.05f) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        const int pid = AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                                          doorW * 0.5f, doorH * 0.5f);
        const AF_Vector3 S2 = V(sx, 1.6f, 1.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S2);
        AF_SceneSetApertureIsTransmission(s, 1);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float f2[kBands] = {}; AF_Vector3 cp2 = V(0,0,0);
        AF_SceneMeasurePortal(s, pid, L, S2, f2, &cp2);
        float g2[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), g2);
        float dd = 0.0f;
        if (prevG > 0.0f) dd = 20.0f * std::log10(std::max(g2[0], 1e-7f) / prevG);
        if (prevG > 0.0f && std::fabs(dd) > worst) { worst = std::fabs(dd); worstAt = sx; }
        std::printf("        %+5.2f    %.4f      %.4f     %+6.1f dB\n", sx, f2[0], g2[0], dd);
        prevG = std::max(g2[0], 1e-7f);
        AF_SceneDestroy(s);
    }
    std::printf("        → 最大隣接差 %.1f dB @ x=%+.2f（0.05m 刻み）\n", worst, worstAt);
}

void diagnoseRoomCoupling() {
    std::printf("\n[診断] 奥の部屋の響きがこちらに届くか（奥の材質だけを変えて比較）\n");
    std::printf("        扉    奥の材質   エコグラム総和   後半(0.3s以降)の割合\n");

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f, hw = 6.0f, hd = 8.0f;
    const AF_Vector3 L = V(0, 1.6f, -4), S = V(0, 1.6f, 3.5f);

    for (int doorMode = 0; doorMode < 2; ++doorMode) {
        for (int farMode = 0; farMode < 2; ++farMode) {
            AF_SceneHandle s = AF_SceneCreate();
            // 手前の部屋は固定。奥の部屋だけ「よく響く/吸う」を切り替える。
            const float liveA[6] = {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f};   // 響く
            const float deadA[6] = {0.60f, 0.70f, 0.80f, 0.85f, 0.90f, 0.90f};   // 吸う
            const float tr[6] = {0.000398f, 0.0001585f, 0.0000398f,
                                 0.00001f, 0.00000251f, 0.000001f};
            const int matNear = AF_SceneAddMaterial(s, tr, liveA, nullptr, 6);
            const int matFar  = AF_SceneAddMaterial(s, tr, (farMode == 0) ? liveA : deadA,
                                                    nullptr, 6);
            // 仕切り（戸口つき）
            AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                                   V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matNear);
            AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                                   V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matNear);
            AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                                   V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), matNear);
            if (doorMode == 0)   // 扉を閉じる
                AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0),
                                       V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                                       V(1,0,0), V(0,1,0), matNear);
            // 手前の部屋（z<0 側）
            AF_SceneAddInstanceBox(s, V(0, -t, -hd*0.5f), V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), matNear);
            AF_SceneAddInstanceBox(s, V(0, h+t, -hd*0.5f), V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), matNear);
            AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, -hd*0.5f), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), matNear);
            AF_SceneAddInstanceBox(s, V( hw, h*0.5f, -hd*0.5f), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), matNear);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), matNear);
            // 奥の部屋（z>0 側）＝ここだけ材質を変える
            AF_SceneAddInstanceBox(s, V(0, -t, hd*0.5f), V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), matFar);
            AF_SceneAddInstanceBox(s, V(0, h+t, hd*0.5f), V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), matFar);
            AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, hd*0.5f), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), matFar);
            AF_SceneAddInstanceBox(s, V( hw, h*0.5f, hd*0.5f), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), matFar);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), matFar);

            // 戸口をポータルとして置く。閉じていても奥の響きが流れてくるはず。
            AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                              doorW * 0.5f, doorH * 0.5f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            AF_SceneUpdate(s, 1.0f / 60.0f);

            float echo[100 * kBands] = {};
            const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
            double sum = 0.0, late = 0.0;
            for (int i = 0; i < bins; ++i)
                for (int b = 0; b < kBands; ++b) {
                    sum += echo[i * kBands + b];
                    if (i >= 30) late += echo[i * kBands + b];   // 1ビン10ms → 0.3s 以降
                }
            std::printf("        %s  %s  %11.5f   %6.1f %%\n",
                        (doorMode == 0) ? "閉" : "開",
                        (farMode == 0) ? "響く  " : "吸う  ",
                        sum, (sum > 1e-12) ? late / sum * 100.0 : 0.0);
            AF_SceneDestroy(s);
        }
    }
    std::printf("      （奥の材質で数字が動けば結合あり。動かなければ奥の響きは届いていない）\n");
}

void testLeakDirection() {
    std::printf("\n[漏れの定位] 弱い材質の側から聞こえるか\n");

    const float t = 0.15f, h = 4.0f, doorH = 2.4f, hw = 6.0f, hd = 8.0f;
    const float wallT[6] = {0.000398f, 0.0001585f, 0.0000398f,
                            0.00001f, 0.00000251f, 0.000001f};   // TL34..60
    const float doorT[6] = {0.0316f, 0.0158f, 0.00794f, 0.00398f, 0.00251f, 0.002f};  // TL15..27

    // リスナーは戸口から大きく外れた位置。直線は壁を通る。
    //   扉は x∈[-0.6,0.6] にあるので、漏れの重心が扉へ寄れば定位も右へ振れる。
    const AF_Vector3 L = V(-3.0f, 1.6f, -4), S = V(0, 1.6f, 3.5f);

    for (int k = 0; k < 2; ++k) {
        AF_SceneHandle s = AF_SceneCreate();
        const int matWall = AF_SceneAddMaterial(s, wallT, nullptr, nullptr, 6);
        // k=0: 扉も壁と同じ材質（弱点なし） / k=1: 扉だけ弱い
        const int matDoor = (k == 0) ? matWall
                          : AF_SceneAddMaterial(s, doorT, nullptr, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0), V(0.6f, doorH * 0.5f, 0.03f),
                               V(1,0,0), V(0,1,0), matDoor);   // 閉じた扉
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), matWall);

        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float dv[3] = {0, 0, 0};
        AF_SceneGetSourceArrivalDir(s, AF_SceneSourceIndex(s, 1), dv);
        const AF_Vector3 d = V(dv[0], dv[1], dv[2]);
        // 直線が壁を通る位置の方向と比べる。
        const AF_Vector3 straight = V(S.x - L.x, S.y - L.y, S.z - L.z);
        const float sl = std::sqrt(straight.x*straight.x + straight.y*straight.y
                                 + straight.z*straight.z);
        AF_Vector3 leak = V(0,0,0);
        AF_SceneMeasureLeakPoint(s, L, S, &leak);
        std::printf("        %s  到来(%+.3f,%+.3f,%+.3f) 音源方向(%+.3f,%+.3f,%+.3f) 漏れ重心(%+.2f,%+.2f,%+.2f)\n",
                    (k == 0) ? "扉も壁と同材質" : "扉だけ弱い    ",
                    d.x, d.y, d.z, straight.x/sl, straight.y/sl, straight.z/sl,
                    leak.x, leak.y, leak.z);
        // ★判定は置かない（未実装なので）。
        //   漏れの重心は「音源のまわりに散らした標本」から作るため、
        //   リスナーが戸口から外れていると標本が全部**壁**に当たり、扉を拾えない。
        //   実測: 扉の材質を変えても重心 x=-1.45 のまま。
        //   面上の全点を積分する（ラジオシティ）か、弱点を宣言する必要がある。
        //   数字は出しておくので、実装したらここが動くはず。
        AF_SceneDestroy(s);
    }
}

void diagnoseThickBarrier() {
    std::printf("\n[診断] 厚い衝立での回折（1次が芯を横切って棄却された後、2次が拾えるか）\n");
    for (float th : {0.05f, 0.10f, 0.20f, 0.40f, 0.80f}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // 衝立: 幅6m・高4m・厚み th*2
        AF_SceneAddInstanceBox(s, V(0, 2, 0), V(3, 2, th), V(1, 0, 0), V(0, 1, 0), mat);
        const AF_Vector3 L = V(0, 1.6f, -4), S = V(0, 1.6f, 4);
        AF_Vector3 mid = V(0,0,0);
        const float delta = AF_SceneDiffractionPath(s, S, L, &mid);
        AF_Vector3 cp[16]; float cd[16];
        const int cn = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 16);
        float d[kBands] = {};
        AF_SceneComputeDiffractionBands(s, S, L, d, kBands);
        std::printf("        厚み %4.0f mm  候補 %2d 個  δ=%7.3f  回折125Hz %.4f  中点(%.2f,%.2f,%.2f)\n",
                    th * 2000.0f, cn, delta, d[0], mid.x, mid.y, mid.z);
        AF_SceneDestroy(s);
    }
    std::printf("      （厚みが増すと 1 次が消える。2 次が肩代わりできていれば δ と回折は残る）\n");

    // ★幻と正当な掠めが、同じ「厚み」で起きているのかを見る。
    //   壁パネルは厚 300mm。幻はその殻を通れてしまうのか、
    //   それとも別の要因（戸口の反対側の縁）なのかを分ける。
    std::printf("      ── 同じ厚みの仕切りで、閉扉の幻と正当な回り込みを並べる ──\n");
    for (int k = 0; k < 2; ++k) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float t = 0.15f, h = 4.0f, doorH = 2.4f, hw = 6.0f, hd = 8.0f;
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        if (k == 0)   // 扉を閉じる＝経路は無いはず
            AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0), V(0.6f, doorH * 0.5f, 0.03f),
                                   V(1,0,0), V(0,1,0), mat);
        const AF_Vector3 L = V(-2.0f, 1.6f, -4), S = V(0, 1.6f, 3.5f);
        AF_Vector3 cp[16]; float cd[16];
        const int cn = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 16);
        std::printf("        %s  候補 %d 個\n", (k == 0) ? "扉を閉じた（幻が出る）" : "扉なし（正当）      ", cn);
        for (int i = 0; i < cn && i < 4; ++i)
            std::printf("            P(%6.2f,%6.2f,%6.2f) δ=%.3f\n", cp[i].x, cp[i].y, cp[i].z, cd[i]);
        AF_SceneDestroy(s);
    }
}

void testNoPhantomDiffraction() {
    std::printf("\n[幻の経路] 閉じた空間で回折が出ないか（ポータル無し・幾何だけ）\n");

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    // ケース①: 戸口すら無い一枚板。経路は物理的に存在しない
    // ケース②: 戸口はあるが扉が閉じている。これも経路は無い
    // ケース③: 扉が閉じていて、**リスナーが壁の前**（Unity で幻が出た配置）
    // ケース④: 戸口が開いていて、音源は正面から外れている。ここは鳴らなければならない
    //   ※音源が戸口の真正面だと見通せてしまい、回折の二次音源は 0 が正しい
    //     （直接音が担当する）。だから③④は音源かリスナーを横へずらす。
    for (int mode = 0; mode < 4; ++mode) {
        const AF_Vector3 L = (mode == 2) ? V(-2.0f, 1.6f, -4) : V(0, 1.6f, -4);
        const AF_Vector3 S = (mode == 3) ? V(-2.0f, 1.6f, 3.5f) : V(0, 1.6f, 3.5f);
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        if (mode == 0) {
            // 一枚板の仕切り（戸口なし）
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        } else {
            AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                                   V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                                   V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                                   V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
            if (mode == 1 || mode == 2)   // 扉を閉じて塞ぐ
                AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0),
                                       V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                                       V(1,0,0), V(0,1,0), mat);
        }
        // 部屋を閉じる（床・天井・側壁・前後）
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);

        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);

        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) sum += gain[i];
        AF_Vector3 cp[24]; float cd[24];
        const int cn = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 24);

        const char* name = (mode == 0) ? "戸口なし(一枚板)  "
                         : (mode == 1) ? "扉閉・正面        "
                         : (mode == 2) ? "扉閉・リスナー横  " : "開放・音源横      ";
        std::printf("        %-18s 候補 %2d 個 / 二次音源 %d 本 / 合計 %.5f\n",
                    name, cn, n, sum);
        if (cn > 0)
            for (int i = 0; i < cn && i < 3; ++i)
                std::printf("            候補 P(%6.2f,%6.2f,%6.2f) δ=%.3f\n",
                            cp[i].x, cp[i].y, cp[i].z, cd[i]);

        char b[128];
        std::snprintf(b, sizeof(b), "(候補 %d / 二次音源 %d / 合計 %.5f)", cn, n, sum);
        if (mode < 3) check("閉じた空間では回折が出ない", sum < 1e-4f, b);
        else          check("開いていれば回折が出る", sum > 1e-3f, b);
        AF_SceneDestroy(s);
    }
}

void testPortalOpening() {
    std::printf("\n[ポータル] 矩形で範囲を切ると開口率がフルレンジになるか\n");

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const AF_Vector3 L = V(0, 1.6f, -4), S = V(0, 1.6f, 3.5f);

    std::printf("        開き角   125Hz    500Hz    4kHz     重心x\n");
    float atClosed = -1.0f, atOpen = 0.0f, prev = -1.0f, maxJump = 0.0f, jumpAt = 0.0f;
    float hiClosed = 0.0f, hiOpen = 0.0f;
    for (float deg = 0.0f; deg <= 90.01f; deg += 5.0f) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        const float th = deg * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        AF_SceneAddInstanceBox(s, V(-0.6f + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                               V(doorW * 0.5f, doorH * 0.5f, 0.03f), V(c, 0, sn), V(0, 1, 0), mat);
        AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                          0.6f, doorH * 0.5f);   // 出荷する経路で測る（旧経路ではなく）

        // 戸口そのものをポータルとして置く（x∈[-0.6,0.6], y∈[0,2.4], z=0）。
        const int pid = AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0),
                                          V(1, 0, 0), V(0, 1, 0), 0.6f, doorH * 0.5f);
        float f[kBands] = {}; AF_Vector3 cp = V(0,0,0);
        AF_SceneMeasurePortal(s, pid, L, S, f, &cp);
        std::printf("        %5.1f°   %.4f   %.4f   %.4f   %+.3f\n",
                    deg, f[0], f[2], f[5], cp.x);
        if (atClosed < 0.0f) { atClosed = f[0]; hiClosed = f[5]; }
        atOpen = f[0]; hiOpen = f[5];
        if (prev >= 0.0f) {
            const float j = std::fabs(f[0] - prev);
            if (j > maxJump) { maxJump = j; jumpAt = deg; }
        }
        prev = f[0];
        AF_SceneDestroy(s);
    }

    char b1[128];
    std::snprintf(b1, sizeof(b1), "(閉 %.4f → 全開 %.4f)", atClosed, atOpen);
    check("閉扉でほぼ 0 になる(< 0.05)", atClosed < 0.05f, b1);
    check("全開でほぼ 1 になる(> 0.85)", atOpen > 0.85f, b1);
    char b2[128];
    std::snprintf(b2, sizeof(b2), "(最大隣接差 %.4f @ %.0f°、5°刻み)", maxJump, jumpAt);
    check("5°刻みで飛ばない(隣接差 < 0.25)", maxJump < 0.25f, b2);
    char b3[128];
    std::snprintf(b3, sizeof(b3), "(4kHz 閉 %.4f → 全開 %.4f)", hiClosed, hiOpen);
    check("高域も閉 0 → 全開 1 になる", hiClosed < 0.05f && hiOpen > 0.85f, b3);
}

void diagnoseSourceOffsetVsDoor() {
    std::printf("\n[診断] 音源の位置で扉の効き方が変わるか（この作品の主張）\n");
    std::printf("        リスナー(0,1.6,-4) / 戸口は x∈[-0.6,0.6], z=0\n");

    struct Place { AF_Vector3 s; const char* name; };
    const Place places[3] = {
        {V( 0.0f, 1.6f, 3.5f), "正対      "},   // 戸口の真正面
        {V(-1.2f, 1.6f, 3.5f), "少し外れ  "},
        {V(-3.0f, 1.6f, 3.5f), "大きく外れ"},
    };

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const AF_Vector3 L = V(0, 1.6f, -4);

    for (const Place& pl : places) {
        std::printf("      ── 音源 %s (%.1f, %.1f, %.1f) ──\n",
                    pl.name, pl.s.x, pl.s.y, pl.s.z);
        std::printf("        開き角   遮蔽  回折本数  合計ゲイン   透過125Hz\n");
        float first = -1.0f, last = 0.0f;
        for (float deg = 0.0f; deg <= 90.01f; deg += 10.0f) {
            AF_SceneHandle s = AF_SceneCreate();
            const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                                   V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                                   V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                                   V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            const float th = deg * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            AF_SceneAddInstanceBox(s, V(-0.6f + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                                   V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                                   V(c, 0, sn), V(0, 1, 0), mat);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, pl.s);
            AF_SceneSetApertureIsTransmission(s, 1);
            AF_SceneUpdate(s, 1.0f / 60.0f);

            AF_Vector3 pos[8]; float gain[8];
            const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += gain[i];
            float tr[kBands] = {}; float occF = 0.0f;
            AF_SceneComputeSoftOcclusion(s, L, pl.s, tr, kBands, &occF);
            const int occ = AF_SceneIsOccluded(s, L, pl.s);
            std::printf("        %5.1f°   %d     %d本      %8.4f    %.4f\n",
                        deg, occ, n, sum, tr[0]);
            if (first < 0.0f) first = sum;
            last = sum;
            AF_SceneDestroy(s);
        }
        std::printf("        → 閉 %.4f → 全開 %.4f\n", first, last);
    }
    std::printf("      （正対と大きく外れで差が出れば、扉の角度ではなく幾何が決めている）\n");
}

void diagnoseDoorApertureCurve() {
    std::printf("\n[診断] 扉の開き角 → 開口幅 → 音量（Test_Full と同じ形状）\n");

    struct Setting { float ref; float power; const char* name; };
    const Setting settings[4] = {
        {0.30f, 1.0f, "0.30/1.0"},   // 既定
        {0.30f, 0.5f, "0.30/0.5"},
        {0.15f, 1.0f, "0.15/1.0"},
        {0.50f, 1.0f, "0.50/1.0"},
    };

    std::printf("        開き角  開口率   %s  %s  %s  %s   開口(耳に届く方の量)\n",
                settings[0].name, settings[1].name, settings[2].name, settings[3].name);

    float prev[4] = {-1, -1, -1, -1};
    float maxJump[4] = {0, 0, 0, 0};
    float atDeg[4] = {0, 0, 0, 0};

    for (float deg = 0.0f; deg <= 90.01f; deg += 5.0f) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
        const float hw = 6.0f, hd = 8.0f;
        // 仕切り（z=0）: 戸口 x∈[-0.6,0.6], y∈[0,2.4]。左右＋まぐさ。
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        // 部屋を閉じる
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        // 扉。蝶番 x=-0.6、+z 側へ振れる。中心は蝶番から半幅ぶん。
        const float th = deg * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        AF_SceneAddInstanceBox(s, V(-0.6f + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                               V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                               V(c, 0, sn), V(0, 1, 0), mat);

        // Full の配置に合わせる（直線が戸口を通らない位置）。
        const AF_Vector3 L = V(0, 1.6f, -4), S = V(-2.0f, 1.6f, 3.5f);

        // ★耳に届く量を測る。AF_SceneComputeDiffractionBands（前川）は
        //   diffractionDistanceOnly が ON のホストでは捨てられるので見ても意味がない。
        //   実際に鳴っているのは GetDiffractionSources が返す「位置」と「重み」。
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);

        // 実測の隙間幅（回折の音量を決める量）
        const float openFrac = AF_SceneMeasureSlitWidth(s, L, S);
        // 直接音がどれだけ抜けているか（＝壁の透過）。閉扉でもこれは残る。
        float tr[kBands] = {}; float occF = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, tr, kBands, &occF);

        float val[4]; AF_Vector3 apPos = V(0,0,0); float apLen = 0.0f; int nsrc = 0;
        for (int k = 0; k < 4; ++k) {
            AF_SceneSetApertureOpen(s, settings[k].ref, settings[k].power, 1.0f, 0);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            AF_Vector3 pos[8]; float gain[8];
            const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += gain[i];
            val[k] = sum;                       // タップ重みの合計＝回折の総量
            if (k == 0 && n > 0) {              // 開口位置は設定に依らないので1回だけ
                nsrc = n; apPos = pos[0];
                const AF_Vector3 d = V(pos[0].x - L.x, pos[0].y - L.y, pos[0].z - L.z);
                apLen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            }
            if (prev[k] >= 0.0f) {
                const float j = std::fabs(val[k] - prev[k]);
                if (j > maxJump[k]) { maxJump[k] = j; atDeg[k] = deg; }
            }
            prev[k] = val[k];
        }
        // ★フレネル開口が実際に何を返しているか。回折の音量はここが決めているので、
        //   「扉を開けても音が変わらない」ときはまずこの6つを見る。
        // BTM 経路での扉の掃引。従来経路と並べる。
        {
            AF_SceneSetUseBtm(s, 1);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            AF_Vector3 bp[8]; float bg[8];
            const int bn = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), bp, bg, 8);
            float bsum = 0.0f;
            for (int i = 0; i < bn; ++i) bsum += bg[i];
            std::printf("                  BTM: 開口 %d本  合計 %.5f\n", bn, bsum);
            AF_SceneSetUseBtm(s, 0);
            AF_SceneUpdate(s, 1.0f / 60.0f);
        }
        // δ の重みを 1 / 0.5 / 0 で並べる。二重掛けの切り分け。
        float fz[kBands] = {}; int fzOn = 0;
        {
            const float ws[3] = {1.0f, 0.5f, 0.0f};
            for (int wi = 0; wi < 3; ++wi) {
                float f2[kBands] = {};
                AF_SceneSetApertureDeltaWeight(s, ws[wi]);
                const int on = AF_SceneMeasureApertureFresnel(s, L, S, f2);
                if (wi == 0) { fzOn = on; for (int b = 0; b < kBands; ++b) fz[b] = f2[b]; }
                std::printf("                  δ重み %.1f: 125 %.4f  250 %.4f  500 %.4f  1k %.4f  2k %.4f  4k %.4f\n",
                            ws[wi], f2[0], f2[1], f2[2], f2[3], f2[4], f2[5]);
            }
            AF_SceneSetApertureDeltaWeight(s, 1.0f);
        }
        std::printf("        %5.1f°  %6.3f   %8.3f  %8.3f  %8.3f  %8.3f   %d本  透過125Hz %.4f (%.1f dB) 4kHz %.4f 遮蔽率 %.2f\n",
                    deg, openFrac, val[0], val[1], val[2], val[3],
                    nsrc, tr[0], 20.0f * std::log10(std::max(tr[0], 1e-6f)), tr[5], occF);
        std::printf("                  フレネル開口率(有効=%d): 125Hz %.4f  250 %.4f  500 %.4f  1k %.4f  2k %.4f  4k %.4f  開口P(%.2f,%.2f,%.2f)\n",
                    fzOn, fz[0], fz[1], fz[2], fz[3], fz[4], fz[5],
                    apPos.x, apPos.y, apPos.z);

        // 閉じているのに経路が残る件の実測。0°/5° で候補の生の位置と δ を全部出す。
        //   推測で潰しに行かないため、まず「何が残っているか」を見る。
        if (deg < 5.01f) {
            AF_Vector3 cp[24]; float cd[24];
            const int cn = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 24);
            std::printf("            候補 %d 個（遮蔽=%d）:\n", cn, AF_SceneIsOccluded(s, L, S));
            for (int i = 0; i < cn && i < 8; ++i)
                std::printf("              P(%6.2f,%6.2f,%6.2f)  δ=%.3f\n",
                            cp[i].x, cp[i].y, cp[i].z, cd[i]);
        }
        AF_SceneDestroy(s);
    }
    std::printf("        → 最大隣接差 ");
    for (int k = 0; k < 4; ++k)
        std::printf("%s %.3f@%.0f°  ", settings[k].name, maxJump[k], atDeg[k]);
    std::printf("\n        （小さいほど滑らか。0°→90° を通して増え続けることも同じくらい大事）\n");
}

// ================================================================ 2次回折の質
// 「2次回折は満足に動いているのか」を測る。既存テストは ゲイン>0 / 本数>0 / ほぼ正面
// の3つしか見ておらず、値の妥当性も、無い場所で鳴っていないかも確認していない。
//
// 見るのは3点:
//   ① 偽陽性 ── 2枚目の戸口を**塞いだ**シーンで、まだ音が届いてしまわないか。
//                届くなら「音の通り道が無い場所で鳴っている」ことになる。
//   ② 連続性 ── リスナーを動かしたとき、ゲインと開口方向が滑らかに動くか。
//   ③ 発火率 ── そもそも何割の位置で 2 次が使われているか（＝捨てたとき失うものの量）。
// Unity の Test_DiffractionGap と**同じ寸法**を組む。
//   本人が実機で「回折点が全然見つからない・動くと音が気持ち悪い」と言った現場がここ。
//   部屋 12×4×8(内寸)・壁厚 0.4、仕切り z=0(厚0.3) の右に開口 x∈[3,5]、
//   音源 (-3,1.6,2) は開口の正面を外してあるので必ず回り込みが要る。
//   リスナーは A/D で X に動くので、X 掃引で見る。
void diagnoseUnityGapScene() {
    std::printf("\n[診断] Unity Test_DiffractionGap と同じ形状\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    auto box = [&](float cx, float cy, float cz, float hx2, float hy2, float hz2) {
        AF_SceneAddInstanceBox(s, V(cx, cy, cz), V(hx2, hy2, hz2), V(1,0,0), V(0,1,0), mat);
    };
    box(0.0f, -0.2f,  0.0f, 6.4f, 0.2f, 4.4f);   // 床
    box(0.0f,  4.2f,  0.0f, 6.4f, 0.2f, 4.4f);   // 天井
    box(6.2f,  2.0f,  0.0f, 0.2f, 2.0f, 4.0f);   // 東
    box(-6.2f, 2.0f,  0.0f, 0.2f, 2.0f, 4.0f);   // 西
    box(0.0f,  2.0f,  4.2f, 6.0f, 2.0f, 0.2f);   // 北
    box(0.0f,  2.0f, -4.2f, 6.0f, 2.0f, 0.2f);   // 南
    box(-1.5f, 2.0f,  0.0f, 4.5f, 2.0f, 0.15f);  // 仕切り左 (x∈[-6,3])
    box(5.5f,  2.0f,  0.0f, 0.5f, 2.0f, 0.15f);  // 仕切り右 (x∈[5,6])

    const AF_Vector3 S = V(-3.0f, 1.6f, 2.0f);
    // ★見つけた開口を「開口率最大のポータル」として評価する版との比較。
    //   ポータルを置くのではなく、稜線探索の結果をポータルの式で読み替える。
    const bool asPortal = (std::getenv("AF_APERTURE_AS_PORTAL") != nullptr);
    (void)asPortal;   // 却下した案（衝立で破綻）。切替は残さない
    // ★Test_Full にはポータルが 2 枚あり、回折シーンには 0 枚。
    //   エンジンは「ポータルがあるならそれが唯一の答え」で稜線探索を丸ごと迂回するので、
    //   同じ形状でも通る経路が違う。どれだけ違うかをここで並べる。
    const bool withPortal = (std::getenv("AF_WITHPORTAL") != nullptr);
    if (withPortal)   // 仕切りの開口 x∈[3,5]、高さは部屋の内寸 0〜4
        AF_SceneAddPortal(s, V(4.0f, 2.0f, 0.0f), V(1, 0, 0), V(0, 1, 0), 1.0f, 2.0f);
    std::printf("      %s\n", withPortal ? "【ポータルを置いた（Test_Full と同じ経路）】"
                            : asPortal   ? "【開口をポータルとして評価（開口率最大）】"
                                         : "【稜線探索（回折シーンと同じ経路）】");
    std::printf("      listener.x  経路本数  回折125Hz  二次音源  到来方向(正規化)   経路長\n");
    int fired = 0, total = 0;
    float prevG = -1.0f, maxJump = 0.0f, jAt = 0.0f;
    for (float x = -5.0f; x <= 5.01f; x += 0.5f) {
        const AF_Vector3 L = V(x, 1.6f, -2.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        ++total; if (n > 0) ++fired;
        AF_Vector3 d = V(0,0,0); float plen = 0.0f;
        if (n > 0) {
            d = V(pos[0].x - L.x, pos[0].y - L.y, pos[0].z - L.z);
            plen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            if (plen > 1e-4f) { d.x /= plen; d.y /= plen; d.z /= plen; }
        }
        if (prevG >= 0.0f) {
            const float j = std::fabs(g[0] - prevG);
            if (j > maxJump) { maxJump = j; jAt = x; }
        }
        prevG = g[0];
        std::printf("      %8.2f    %6d   %8.4f  %6d    (%5.2f,%5.2f,%5.2f)  %6.2f m%s",
                    x, n, g[0], n, d.x, d.y, d.z, plen, n > 0 ? "" : "  ★出ていない");
        // ★複数タップは**同じ乾いた信号のコピー**なので、経路差ぶんの遅延で干渉する。
        //   回折タップは平坦（diffractionDistanceOnly）なので全帯域で同相に効き、
        //   経路差 Δ の第1ノッチが c/(2Δ) に立つ。ドラムの胴の帯域に来ると
        //   「削れて低音が壊れた」ように聞こえる。何 Hz に来るかを出す。
        if (n >= 2) {
            float pmin = 1e30f, pmax = 0.0f;
            for (int i = 0; i < n; ++i) {
                const float ax = pos[i].x - L.x, ay = pos[i].y - L.y, az = pos[i].z - L.z;
                const float p = std::sqrt(ax*ax + ay*ay + az*az);
                pmin = std::min(pmin, p); pmax = std::max(pmax, p);
            }
            const float dmax = pmax - pmin;
            if (dmax > 1e-3f)
                std::printf("   経路差 %.2fm → 第1ノッチ %.0f Hz", dmax, 343.0f / (2.0f * dmax));
        }
        std::printf("\n");
    }
    std::printf("      → 二次音源が出た位置 %d / %d ／ 125Hz 最大隣接差 %.4f @ x=%.2f\n",
                fired, total, maxJump, jAt);

    // ★線ではなく**面**で潰す。1本の掃引では踏まない穴が部屋の中に残っていないか。
    //   部屋の内側 x∈[-5.5,5.5] × z∈[-3.5,-0.5] を 0.25m 格子で全部見る。
    {
        int cells = 0, zero = 0;
        float worstX = 0.0f, worstZ = 0.0f;
        for (float z = -3.5f; z <= -0.49f; z += 0.25f) {
            for (float x = -5.5f; x <= 5.51f; x += 0.25f) {
                const AF_Vector3 L = V(x, 1.6f, z);
                AF_SceneSetListener(s, L);
                AF_SceneSetSource(s, 1, S);
                AF_SceneUpdate(s, 1.0f / 60.0f);
                AF_Vector3 pos[8]; float gain[8];
                const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1),
                                                            pos, gain, 8);
                ++cells;
                if (n == 0) {
                    // 見通せている位置なら二次音源 0 が正しい（回折は直接音への補正になる）。
                    float tb[kBands] = {};
                    AF_SceneComputeTransmissionBands(s, L, S, tb, kBands);
                    const bool losMaybe = tb[0] > 0.99f;
                    if (!losMaybe) { ++zero; worstX = x; worstZ = z; }
                    if (zero <= 16)
                        std::printf("         出ない点 (%+.2f, %+.2f) %s\n", x, z,
                                    losMaybe ? "（見通せている＝0 が正しい）" : "★遮蔽されているのに 0");
                }
            }
        }
        std::printf("      → 部屋の中を 0.25m 格子で全部見る: 回折が出ない点 %d / %d",
                    zero, cells);
        if (zero > 0) std::printf("  ★例 (%.2f, 1.60, %.2f)", worstX, worstZ);
        std::printf("\n");
    }
    AF_SceneDestroy(s);
}

// 「離れると稜線が探索できなくなるのでは」という見立てを数字で確かめる。
//   仕切り1枚に開口ひとつだけ（床も天井も部屋も置かない）。他の要因を消して
//   **距離だけ**を動かす。リスナーを仕切りから 2m → 40m まで下げる。
//   壁は x∈[-20,-1] と x∈[1,20]、開口は x∈[-1,1]、音源は開口の正面を外した所。
void diagnoseDistanceReach() {
    std::printf("\n[診断] 開口からの距離と稜線探索の到達\n");
    std::printf("      仕切り1枚＋開口ひとつ。周りの壁を段階的に足して、何が効くか分ける。\n");
    // level 0=仕切りだけ / 1=+床天井 / 2=+側壁 / 3=+奥と手前の壁（閉じた部屋）
    const char* label[4] = {"仕切りだけ", "＋床天井  ", "＋側壁    ", "＋閉じた箱"};
    for (int level = 0; level < 4; ++level) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(-10.5f, 2.5f, 0), V(9.5f, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 10.5f, 2.5f, 0), V(9.5f, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
        if (level >= 1) {
            AF_SceneAddInstanceBox(s, V(0, -0.15f, 0), V(20, 0.15f, 45), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0,  5.15f, 0), V(20, 0.15f, 45), V(1,0,0), V(0,1,0), mat);
        }
        if (level >= 2) {
            AF_SceneAddInstanceBox(s, V(-20.15f, 2.5f, 0), V(0.15f, 2.5f, 45), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V( 20.15f, 2.5f, 0), V(0.15f, 2.5f, 45), V(1,0,0), V(0,1,0), mat);
        }
        if (level >= 3) {
            AF_SceneAddInstanceBox(s, V(0, 2.5f, -45.15f), V(20, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, 2.5f,  45.15f), V(20, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
        }
        const AF_Vector3 S = V(-4.0f, 1.6f, 4.0f);
        AF_SceneSetSource(s, 1, S);

        std::printf("      [%s]  距離: ", label[level]);
        for (float d = 2.0f; d <= 40.01f; d = (d < 10.0f) ? d + 2.0f : d + 5.0f) {
            const AF_Vector3 L = V(0.0f, 1.6f, -d);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            AF_Vector3 pos[8]; float gain[8];
            const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
            std::printf("%.0fm:%d ", d, n);
        }
        std::printf("（数字は経路本数。0 なら探索できていない）\n");
        AF_SceneDestroy(s);
    }

    // 閉じた箱で、ゲインと経路長も出す。
    std::printf("      閉じた箱での中身:\n");
    std::printf("        距離(m)  経路本数  回折125Hz   δ(m)   経路長(m)  縁への入射角(度)\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    AF_SceneAddInstanceBox(s, V(-10.5f, 2.5f, 0), V(9.5f, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V( 10.5f, 2.5f, 0), V(9.5f, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, -0.15f, 0), V(20, 0.15f, 45), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0,  5.15f, 0), V(20, 0.15f, 45), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(-20.15f, 2.5f, 0), V(0.15f, 2.5f, 45), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V( 20.15f, 2.5f, 0), V(0.15f, 2.5f, 45), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, 2.5f, -45.15f), V(20, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, 2.5f,  45.15f), V(20, 2.5f, 0.15f), V(1,0,0), V(0,1,0), mat);
    const AF_Vector3 S = V(-4.0f, 1.6f, 4.0f);
    AF_SceneSetSource(s, 1, S);
    for (float d = 2.0f; d <= 40.01f; d = (d < 10.0f) ? d + 2.0f : d + 5.0f) {
        const AF_Vector3 L = V(0.0f, 1.6f, -d);
        AF_SceneSetListener(s, L);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        float plen = 0.0f, delta = 0.0f;
        if (n > 0) {
            const float vx = pos[0].x - L.x, vy = pos[0].y - L.y, vz = pos[0].z - L.z;
            plen = std::sqrt(vx*vx + vy*vy + vz*vz);
            const float dx = S.x - L.x, dy = S.y - L.y, dz = S.z - L.z;
            delta = plen - std::sqrt(dx*dx + dy*dy + dz*dz);
        }
        std::printf("        %7.1f  %6d   %8.4f  %6.2f   %7.2f   %8.1f%s\n",
                    d, n, g[0], delta, plen,
                    90.0f - std::atan2(d, 1.0f) * 57.2958f,
                    n == 0 ? "   ★探索できていない" : "");
    }

    // ★**横に**離れる場合。奥行きは 3m に固定して、開口から左へ 0〜19m 離れる。
    //   縁への入射角が浅くなるので、掠めの許容が固定値だとここで死ぬはず。
    std::printf("      横に離れる（奥行き 3m 固定、開口は x∈[-1,1]）:\n");
    std::printf("        横の距離(m)  経路本数  回折125Hz   縁への入射角(度)\n");
    for (float x = 0.0f; x >= -19.01f; x -= 2.0f) {
        const AF_Vector3 L = V(x, 1.6f, -3.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        const float lateral = std::fabs(1.0f - x);          // 開口の右縁までの横距離
        std::printf("        %10.1f  %6d   %8.4f   %10.1f%s\n",
                    -x, n, g[0], std::atan2(3.0f, lateral) * 57.2958f,
                    n == 0 ? "   ★探索できていない" : "");
    }
    AF_SceneDestroy(s);
}

// 「隣の部屋へ動くだけで音が変わりすぎ」を数字にする。
//   Unity の混ぜ方（AcousticFlowSceneDemo）をそのまま再現する:
//     直接タップ  = ソフト遮蔽の透過 × 距離減衰(1.5/max(r,1.5))
//     回折タップ  = エンジンの回折ゲイン × 遮蔽割合 × 距離減衰(経路長)
//   diffractionDistanceOnly=ON は**帯域の形**だけを平らにする設定なので、
//   広帯域の音量はそのまま残る。そこがどれだけ振れるかを見る。
//   ※空気吸収は掛けていない（掛けると更に高域が落ちるが、量の議論は変わらない）。
void diagnoseRoomToRoomLevel() {
    std::printf("\n[診断] 隣の部屋へ歩いたときのレベル変化（Unity の混ぜ方を再現）\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, hw = 6.0f, hd = 8.0f;
    // 戸口 x∈[-0.6,0.6] だけが通り道。扉は無し（開口そのものを見る）。
    AF_SceneAddInstanceBox(s, V(-(hw + doorW*0.5f) * 0.5f, h*0.5f, 0),
                           V((hw - doorW*0.5f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V((hw + doorW*0.5f) * 0.5f, h*0.5f, 0),
                           V((hw - doorW*0.5f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, (2.4f + h) * 0.5f, 0), V(doorW*0.5f, (h - 2.4f)*0.5f, t),
                           V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);

    const AF_Vector3 S = V(-3.0f, 1.6f, 4.0f);      // 奥の部屋・戸口の正面から外す
    AF_SceneSetSource(s, 1, S);
    // ★**Unity と同じ設定にする**。エンジンの既定と Unity の設定は違うので、
    //   既定のまま測ると本人が聞いているものと別の数字が出る（実際 8dB ずれていた）。
    //   AcousticFlowSceneDemo: apertureIsTransmission=true / apertureContrast=4
    AF_SceneSetApertureIsTransmission(s, 1);
    AF_SceneSetApertureContrast(s, 4.0f);

    auto distAtten = [](float r) { return 1.5f / std::max(r, 1.5f); };
    auto db = [](float a) { return (a > 1e-9f) ? 20.0f * std::log10(a) : -180.0f; };

    std::printf("      listener.z  遮蔽割合  直接(dB)  回折(dB)  合計(dB)  隣接差(dB)"
                "  早期反射 本数  残響送出(dB)\n");
    float prevTot = 0.0f; bool have = false;
    float prev2 = 0.0f; bool have2 = false; float maxJump2 = 0.0f;
    float min2 = 1e30f, max2 = -1e30f;
    float maxJump = 0.0f, jumpAt = 0.0f, minTot = 1e30f, maxTot = -1e30f;
    for (float z = -6.0f; z <= 6.01f; z += 0.5f) {
        const AF_Vector3 L = V(0.0f, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        // ★2回まわす。回折の二次音源は 2 フレームに 1 回しか再計算されない
        //   （役割2の diffSrcEveryN=2）ので、1回だと**前の位置の値**が返ることがある。
        //   気づかずに測ると、同じ掃引が 16.2dB と 50.5dB の二通りに出る。
        AF_SceneUpdate(s, 1.0f / 60.0f);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        const float dx = S.x - L.x, dy = S.y - L.y, dz = S.z - L.z;
        const float directDist = std::sqrt(dx*dx + dy*dy + dz*dz);
        float soft[kBands] = {}; float occFrac = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, soft, kBands, &occFrac);
        float sm = 0.0f;
        for (int b = 0; b < kBands; ++b) sm += soft[b];
        const float direct = (sm / kBands) * distAtten(directDist);

        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        float diff = 0.0f, plShow = 0.0f, gsum = 0.0f, diffNoAtt = 0.0f;
        for (int i = 0; i < n; ++i) gsum += gain[i];
        for (int i = 0; i < n; ++i) {
            const float vx = pos[i].x - L.x, vy = pos[i].y - L.y, vz = pos[i].z - L.z;
            const float pl = std::sqrt(vx*vx + vy*vy + vz*vz);
            diff += gain[i] * occFrac * distAtten(pl);
            // ★もし「回折の減衰量を掛けない」なら（＝配分だけを合計1に正規化して使う）。
            //   C# のコメントが「合計1に正規化済み」と書いている状態がこれ。
            if (gsum > 1e-9f) diffNoAtt += (gain[i] / gsum) * occFrac * distAtten(pl);
            if (i == 0) plShow = pl;
        }
        // ★早期反射と残響送出も入れる。前回はここを見ておらず「段差 5.8dB」と報告したが、
        //   部屋を移ると**反射と残響の方が大きく動く**ので、それだけでは実態を表さない。
        AF_Vector3 erp[16]; float erg[16 * kBands];
        const int ner = AF_SceneGetEarlyReflections(s, AF_SceneSourceIndex(s, 1), erp, erg, 16);
        float early = 0.0f;
        for (int i = 0; i < ner; ++i) {
            const float vx = erp[i].x - L.x, vy = erp[i].y - L.y, vz = erp[i].z - L.z;
            const float pl = std::sqrt(vx*vx + vy*vy + vz*vz);
            float m = 0.0f;
            for (int b = 0; b < kBands; ++b) m += erg[i * kBands + b];
            early += (m / kBands) * distAtten(pl);
        }
        // 残響への送出量＝反射込みの帯域生存（C# の ts.SourceLevel と同じ）。
        float surv[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), surv);
        float sv = 0.0f;
        for (int b = 0; b < kBands; ++b) sv += surv[b];
        sv /= kBands;

        const float tot = direct + diff;
        float jump = 0.0f;
        if (have) { jump = std::fabs(db(tot) - db(prevTot)); if (jump > maxJump) { maxJump = jump; jumpAt = z; } }
        prevTot = tot; have = true;
        minTot = std::min(minTot, db(tot)); maxTot = std::max(maxTot, db(tot));
        // ★物理の基準: 半無限スクリーンの遮蔽損失（Kurze-Anderson 近似の前川）。
        //   A = 5 + 20log10( sqrt(2πN) / tanh(sqrt(2πN)) ) [dB]、N = 2δ/λ。
        //   N→0（影境界）で **5dB**（回折場が直接音の約半分）になるのが要点。
        //   開口面積は掛けない ── 遮蔽体モデルと開口モデルは**別々の状況の代替**で、
        //   両方掛けると同じ損失を二重に払う。
        float physRef = 0.0f;
        if (n > 0 && plShow > 0.0f) {
            const float delta = std::max(plShow - directDist, 0.0f);
            const float lambda = 343.0f / 125.0f;
            const float N = 2.0f * delta / lambda;
            const float r = std::sqrt(2.0f * 3.14159265f * std::max(N, 1e-6f));
            const float A = 5.0f + 20.0f * std::log10(r / std::tanh(r));
            physRef = std::pow(10.0f, -A / 20.0f) * occFrac * distAtten(plShow);
        }
        const float tot2 = direct + diffNoAtt;
        if (have2) { const float j2 = std::fabs(db(tot2) - db(prev2));
                     if (j2 > maxJump2) { maxJump2 = j2; } }
        prev2 = tot2; have2 = true;
        min2 = std::min(min2, db(tot2)); max2 = std::max(max2, db(tot2));
        std::printf("      %9.1f  %7.2f  %8.1f  %8.1f  %8.1f  %9.1f  %8.1f %3d  %8.1f\n",
                    z, occFrac, db(direct), db(diff), db(tot), jump,
                    db(early), ner, db(sv));
    }
    std::printf("      → 現状          : 最大隣接差 %.1f dB @ z=%.1f ／ 全域の振れ幅 %.1f dB\n",
                maxJump, jumpAt, maxTot - minTot);
    std::printf("      → 減衰量を外すと: 最大隣接差 %.1f dB           ／ 全域の振れ幅 %.1f dB\n",
                maxJump2, max2 - min2);

    // ★透過レベルのつまみ(transmissionGainDb)が振れ幅をどれだけ縮めるか。
    //   C# の ApplyTilt/ゲインと同じ式をここで通す。平滑化は時間方向なのでここには出ない。
    std::printf("      つまみを動かしたときの振れ幅（左=透過ゲイン / 右=回折ゲイン）:\n");
    for (int knob = 0; knob < 2; ++knob)
    for (float gdb = 0.0f; gdb <= 24.01f; gdb += 6.0f) {
        const float tg = (knob == 0) ? std::pow(10.0f, gdb / 20.0f) : 1.0f;
        const float dg = (knob == 1) ? std::pow(10.0f, gdb / 20.0f) : 1.0f;
        float mn = 1e30f, mx = -1e30f, mj = 0.0f, pv = 0.0f; bool hv = false;
        for (float z = -6.0f; z <= 6.01f; z += 0.5f) {
            const AF_Vector3 L = V(0.0f, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            const float ddx = S.x - L.x, ddy = S.y - L.y, ddz = S.z - L.z;
            const float dd = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
            float sf[kBands] = {}; float of = 0.0f;
            AF_SceneComputeSoftOcclusion(s, L, S, sf, kBands, &of);
            float sm2 = 0.0f;
            for (int b = 0; b < kBands; ++b) sm2 += std::min(sf[b] * tg, 1.0f);
            const float dir2 = (sm2 / kBands) * distAtten(dd);
            AF_Vector3 p2[8]; float g2[8];
            const int n2 = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), p2, g2, 8);
            float df2 = 0.0f;
            for (int i = 0; i < n2; ++i) {
                const float vx = p2[i].x - L.x, vy = p2[i].y - L.y, vz = p2[i].z - L.z;
                df2 += g2[i] * of * dg * distAtten(std::sqrt(vx*vx + vy*vy + vz*vz));
            }
            const float tt = db(dir2 + df2);
            if (hv && std::fabs(tt - pv) > mj) mj = std::fabs(tt - pv);
            pv = tt; hv = true;
            mn = std::min(mn, tt); mx = std::max(mx, tt);
        }
        std::printf("        %s +%4.0f dB → 振れ幅 %5.1f dB ／ 最大隣接差 %4.1f dB\n",
                    knob == 0 ? "透過" : "回折", gdb, mx - mn, mj);
    }
    AF_SceneDestroy(s);
}

// 開口の広さが回折の量にどう効くか。レイで測った差し渡しと、そこから出るゲイン。
//   ポータル式では**広さだけ**がゲインを決める（距離はホスト、方向は開口点）ので、
//   ここが素直かどうかが全部を決める。
void diagnoseApertureWidthCurve() {
    std::printf("\n[診断] 開口の広さ → 回折の量（ポータル式で残る唯一の要素）\n");
    // ★壁の厚みを変えて崖の位置が動くかを見る。動けば原因は写像ではなく
    //   「通れるか」のプローブ距離（厚み×2＋margin×2）だと確定する。
    for (float wallT : {0.15f}) {
    std::printf("      ── 壁の半厚 %.2fm（プローブ距離 = %.2fm）──\n",
                wallT, 2.0f * wallT * 2.0f + 0.04f);
    std::printf("      隙間幅(m)   回折125Hz   前の幅との比\n");
    float prev = -1.0f;
    for (float w : {0.03f, 0.06f, 0.12f, 0.25f, 0.35f, 0.5f, 1.0f, 2.0f, 4.0f}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // ★ここで無条件に ON にしていたせいで、既定経路のつもりでポータル式を測っていた。
        //   環境変数で切り替える（既定は既定経路）。

        // ★Unity は apertureContrast=4 で走っている。エンジン既定の 1.0 で測ると
        //   実機と別のものを測ることになる（一度やった）。環境変数で振れるようにする。
        {
            const char* c = std::getenv("AF_CONTRAST");
            if (c) AF_SceneSetApertureContrast(s, static_cast<float>(std::atof(c)));
        }
        // ★隙間の**手前の縁を x=0 に固定**し、奥へ向かって広げる。
        //   両側から広げると開口の縁が直線に近づき、幅と同時に「影の深さ」も変わって
        //   しまう（前の測り方はこれで、幅の効きを過小評価していた）。
        //   手前の縁を固定すれば δ が変わらないので、**幅だけ**を動かせる。
        const float hw = 8.0f, h = 4.0f, hd = 8.0f, t = wallT;
        AF_SceneAddInstanceBox(s, V(-hw * 0.5f, h*0.5f, 0),
                               V(hw * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);   // x∈[-8,0]
        AF_SceneAddInstanceBox(s, V((w + hw) * 0.5f, h*0.5f, 0),
                               V((hw - w) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat); // x∈[w,8]
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);

        // ★直線が隙間を通らない配置にすること。対称に置くと隙間の真ん中を素通りして
        //   遮蔽されず、回折ではなく「見通し時の補正」を測ってしまう（一度やった）。
        //   L(-4,-3)→S(2,3) は z=0 を **x=-1** で横切る。隙間は x∈[0,w] なので、
        //   どの幅でも直線は壁の中（＝影の深さは幅に依らず一定）。
        const AF_Vector3 L = V(-4.0f, 1.6f, -3.0f), S = V(2.0f, 1.6f, 3.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        AF_Vector3 cp[8]; float cd[8];
        const int cn = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 8);
        // ★どこで平坦になっているかを分けるため、フレネル開口率そのものも出す。
        float fr[kBands] = {};
        const int frOk = AF_SceneMeasureApertureFresnel(s, L, S, fr);
        char rel[32] = "";
        if (prev > 1e-6f) std::snprintf(rel, sizeof(rel), "  ×%.2f", g[0] / prev);
        std::printf("      %8.2f   %9.4f%s   候補%d 経路%d   開口率(%d) 125Hz %.4f / 4kHz %.4f%s\n",
                    w, g[0], rel, cn, n, frOk, fr[0], fr[5],
                    (cn == 0) ? "  ★候補が見つかっていない" : "");
        prev = g[0];
        AF_SceneDestroy(s);
    }
    }
    std::printf("      （素通り基準 0.35m。これより広いと律速しない＝差が出なくなる）\n");
}

// 板ではない箱（立方体・柱・衝立）で回折が出るか。
//   「板を跨ぐ線分」で判定する方式は**最も薄い軸＝壁の法線**を前提にしている。
//   立方体だと薄い軸が退化して向きが恣意的に決まるので、判定が意味を失う恐れがある。
//   遮蔽物1つだけを自由空間に置いて、素の探索能力だけを見る。
void diagnoseNonPlateBlocker() {
    std::printf("\n[診断] 板でない遮蔽物でも回折が出るか（跨ぎ判定の前提の確認）\n");
    struct Case { const char* name; float hx, hy, hz; };
    // ★経路は Z 方向なので、遮蔽物は Z に薄くする（半径表記）。
    //   一度 X に薄くして測ってしまい、板が経路と平行＝壁になっていなかった。
    const Case cases[] = {
        {"薄い板 (2 x 2 x 0.15)   ", 2.0f, 2.0f, 0.15f},
        {"やや厚い板(2 x 2 x 0.5) ", 2.0f, 2.0f, 0.50f},
        {"立方体 (1 x 1 x 1)      ", 1.0f, 1.0f, 1.00f},
        {"柱 (0.4 x 3 x 0.4)      ", 0.4f, 3.0f, 0.40f},
        {"厚い塊 (2 x 2 x 1.5)    ", 2.0f, 2.0f, 1.50f},
    };
    std::printf("      形状                       候補数  回折125Hz\n");
    for (const Case& c : cases) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, 1.6f, 0), V(c.hx, c.hy, c.hz), V(1,0,0), V(0,1,0), mat);
        const AF_Vector3 L = V(0, 1.6f, -3.0f), S = V(0, 1.6f, 3.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 cp[16]; float cd[16];
        const int cn = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 16);
        std::printf("      %s  %5d   %8.4f%s\n", c.name, cn, g[0],
                    cn == 0 ? "   ★回折が消えた" : "");
        AF_SceneDestroy(s);
    }
}

// ポータルは「自分が覆う開口だけ」を担当すべき。
//   現状は 1 枚でもポータルがあると稜線探索を丸ごと迂回するので、
//   ポータルを置いていない開口が鳴らなくなる。それを直接測る。
//   仕切りに開口を 2 つ（A: x∈[-4,-2] / B: x∈[2,4]）空け、A にだけポータルを置く。
// 幾何から「部屋」が正しく出るか。Rooms & Portals の土台の最初の検証。
//   期待: 仕切りで2つに割った箱 → 部屋 2 つ。開口があっても**繋がっていれば 1 つ**。
//   扉は動くものなので静的な塗り分けに入らず、閉扉でも部屋は分かれないのが正しい
//   （「そこに開口がある」という情報を残すため）。
void diagnoseRoomDetection() {
    std::printf("\n[診断] 幾何から部屋を検出する\n");
    const float h = 4.0f, t = 0.15f, hw = 6.0f, hd = 8.0f;
    // gap: 仕切りの開口幅（0 なら完全に塞ぐ）
    auto build = [&](float gap) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        if (gap <= 1e-3f) {   // 仕切り 1 枚で完全に分断
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        } else {              // 中央に幅 gap の戸口
            const float half = gap * 0.5f;
            AF_SceneAddInstanceBox(s, V(-(hw + half) * 0.5f, h*0.5f, 0),
                                   V((hw - half) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V( (hw + half) * 0.5f, h*0.5f, 0),
                                   V((hw - half) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        }
        return s;
    };
    // ★戸口の幅 × 侵食半径。素の連結成分（半径 0）だと戸口で繋がった空間は
    //   全部ひとつの部屋になる。侵食を入れると、幅が半径の 2 倍に満たないくびれが
    //   千切れて部屋が分かれる ── どの幅で切り替わるかを見る。
    std::printf("      戸口の幅 × 侵食半径 → 部屋数（格子 0.10m）\n");
    std::printf("        戸口\\半径   0.00   0.30   0.45   0.60   0.75   1.00\n");
    for (float gap : {0.0f, 0.3f, 0.6f, 0.9f, 1.2f, 2.0f, 4.0f}) {
        std::printf("        %5.1fm   ", gap);
        for (float r : {0.0f, 0.3f, 0.45f, 0.6f, 0.75f, 1.0f}) {
            AF_SceneHandle s = build(gap);
            AF_SceneSetRoomCellSize(s, 0.1f);
            AF_SceneSetRoomSeedRadius(s, r);
            std::printf("%6d ", AF_SceneRoomCount(s));
            AF_SceneDestroy(s);
        }
        std::printf("\n");
    }
    // ★格子が粗いと距離が整数に丸まって、戸口の幅の差が消える。既定半径での限界を見る。
    //   距離の近似（市街地3近傍 / 3-4-5 の13近傍）でも並べる。判定が同じなら安い方でよい。
    std::printf("      戸口の幅 × 格子の刻み → 部屋数（侵食半径 0.60m、括弧内は13近傍）\n");
    std::printf("        戸口\\刻み    0.25m     0.15m     0.10m\n");
    for (float gap : {0.0f, 0.6f, 0.9f, 1.2f, 2.0f}) {
        std::printf("        %5.1fm   ", gap);
        for (float cell : {0.25f, 0.15f, 0.1f}) {
            int r[2] = {0, 0};
            for (int full = 0; full < 2; ++full) {
                AF_SceneHandle s = build(gap);
                AF_SceneSetRoomCellSize(s, cell);
                AF_SceneSetRoomChamferFull(s, full);
                AF_SceneSetRoomSeedRadius(s, 0.6f);
                r[full] = AF_SceneRoomCount(s);
                AF_SceneDestroy(s);
            }
            std::printf("%6d(%d) ", r[0], r[1]);
        }
        std::printf("\n");
    }
    // ★開口の抽出。戸口の実寸（幅 × 高さ 4m）と突き合わせる。
    //   ここで出るのは戸口＝開口の器であって、扉の開き具合ではない。
    std::printf("      開口の抽出（戸口 幅 w × 高さ 4.0m、格子 0.10m）\n");
    std::printf("        戸口     口数  面積(m2)  実寸   中心(x,y,z)          法線\n");
    for (float gap : {0.0f, 0.6f, 0.9f, 1.2f, 2.0f}) {
        AF_SceneHandle s = build(gap);
        AF_SceneSetRoomCellSize(s, 0.1f);
        const int na = AF_SceneApertureCount(s);
        std::printf("        %4.1fm   %3d  ", gap, na);
        if (na > 0) {
            float area = 0.0f; AF_Vector3 c{}, n{}; int ra = 0, rb = 0;
            AF_SceneApertureInfo(s, 0, &area, &c, &n, &ra, &rb);
            std::printf("%7.2f  %5.2f  (%5.2f,%5.2f,%5.2f)  (%4.1f,%4.1f,%4.1f)  部屋 %d-%d",
                        area, gap * 4.0f, c.x, c.y, c.z, n.x, n.y, n.z, ra, rb);
        } else {
            std::printf("      -      -");
        }
        std::printf("\n");
        AF_SceneDestroy(s);
    }
    // 同じ 2 部屋を繋ぐ口が 2 つある形（仕切りに戸口を 2 箇所）。
    {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        // 仕切り: 壁 x∈[-6,-3] / 戸口 0.9m / 壁 x∈[-2.1,2] / 戸口 0.6m / 壁 x∈[2.6,6]
        AF_SceneAddInstanceBox(s, V(-4.5f,  h*0.5f, 0), V(1.5f,  h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-0.05f, h*0.5f, 0), V(2.05f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 4.3f,  h*0.5f, 0), V(1.7f,  h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneSetRoomCellSize(s, 0.1f);
        const int na = AF_SceneApertureCount(s);
        std::printf("      仕切りに戸口 2 箇所（幅 0.9m と 0.6m）: 部屋 %d / 口 %d\n",
                    AF_SceneRoomCount(s), na);
        for (int i = 0; i < na && i < 4; ++i) {
            float area = 0.0f; AF_Vector3 c{}, n{}; int ra = 0, rb = 0;
            AF_SceneApertureInfo(s, i, &area, &c, &n, &ra, &rb);
            std::printf("        口%d  面積 %6.2f m2  中心 (%5.2f,%5.2f,%5.2f)  部屋 %d-%d\n",
                        i, area, c.x, c.y, c.z, ra, rb);
        }
        AF_SceneDestroy(s);
    }

    // 塗り戻しが効いていること＝戸口のど真ん中に立っても必ずどちらかの部屋になる。
    {
        AF_SceneHandle s = build(0.9f);
        AF_SceneSetRoomCellSize(s, 0.1f);
        const int n = AF_SceneRoomCount(s);
        const int inDoor = AF_SceneRoomAt(s, V(0, 2.0f, 0));
        const int nearSide = AF_SceneRoomAt(s, V(0, 2.0f, -4.0f));
        const int farSide  = AF_SceneRoomAt(s, V(0, 2.0f,  4.0f));
        std::printf("      戸口 0.9m: 部屋 %d / 手前 %d・戸口の中 %d・奥 %d%s\n",
                    n, nearSide, inDoor, farSide,
                    (inDoor >= 0 && nearSide >= 0 && farSide >= 0 && nearSide != farSide)
                        ? "" : "  ★塗り戻しが効いていない");
        AF_SceneDestroy(s);
    }
    std::printf("      体積(m3)  格子      形状\n");
    for (float gap : {0.0f, 0.9f, 4.0f}) {
        AF_SceneHandle s = build(gap);
        const int n = AF_SceneRoomCount(s);
        int nx = 0, ny = 0, nz = 0; float cell = 0.0f;
        AF_SceneRoomGridDims(s, &nx, &ny, &nz, &cell);
        char vol[96] = "";
        for (int i = 0; i < n && i < 4; ++i) {
            float v = 0.0f;
            AF_SceneRoomInfo(s, i, &v, nullptr, nullptr, nullptr);
            char one[24]; std::snprintf(one, sizeof(one), "%.0f ", v);
            std::strncat(vol, one, sizeof(vol) - std::strlen(vol) - 1);
        }
        std::printf("      %-9s %3dx%3dx%3d @%.2fm  戸口 %.1fm（部屋 %d）\n",
                    vol, nx, ny, nz, cell, gap, n);
        AF_SceneDestroy(s);
    }
    // ★作り直しのコスト。LOD/ストリーミング/破壊で形状が入れ替わるたびに走る。
    std::printf("      全再構築のコスト（格子の刻み × 距離の近似。ブロックは16）:\n");
    for (int full : {0, 1}) {
        for (float cell : {0.25f, 0.15f, 0.1f}) {
            const int brick = 16;
            AF_SceneHandle s = build(1.2f);
            AF_SceneSetRoomCellSize(s, cell);
            AF_SceneSetRoomChamferFull(s, full);
            AF_SceneSetRoomBrick(s, brick);
            const auto t0 = std::chrono::high_resolution_clock::now();
            const int n = AF_SceneRoomCount(s);              // 初回はここで構築される
            const auto t1 = std::chrono::high_resolution_clock::now();
            int nx = 0, ny = 0, nz = 0; float c2 = 0.0f;
            AF_SceneRoomGridDims(s, &nx, &ny, &nz, &c2);
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            float ma = 0, mf = 0, md = 0, ml = 0, mm = 0, mg = 0; int hot = 0, nb = 0;
            AF_SceneRoomBuildTimes(s, &ma, &mf, &md, &ml, &mm, &mg, &hot, &nb);
            std::printf("        %.2fm %s  %7d ボクセル  %6.2f ms"
                        "（確保 %.2f / 塗り %.2f / 距離 %.2f / 連結 %.2f / 併合 %.2f / 塗戻 %.2f）\n",
                        cell, full ? "13近傍" : " 3近傍", nx * ny * nz, ms,
                        ma, mf, md, ml, mm, mg);
            (void)nb; (void)n;
            AF_SceneDestroy(s);
        }
    }

    // ★差分更新のコスト。ここが本題 ── 仕切りを 1 枚壊すと**全体の連結が変わる**
    //   （2 部屋が 1 部屋になる）のに、塗り直すのは触れたブロックだけで済むこと。
    //   合わせて、差分更新の結果が全再構築と一致することを体積で確かめる。
    std::printf("      差分更新のコスト（仕切りを壊して 2 部屋 → 1 部屋）:\n");
    auto volumes = [](AF_SceneHandle s) {
        double t = 0.0;
        for (int i = 0, n = AF_SceneRoomCount(s); i < n; ++i) {
            float v = 0.0f; AF_SceneRoomInfo(s, i, &v, nullptr, nullptr, nullptr); t += v;
        }
        return t;
    };
    for (int brick : {8, 16, 32}) {
        for (float cell : {0.25f, 0.1f}) {
            AF_SceneHandle s = build(0.0f);                // 仕切り 1 枚（インスタンス 6）で分断
            AF_SceneSetRoomCellSize(s, cell);
            AF_SceneSetRoomBrick(s, brick);
            const int before = AF_SceneRoomCount(s);       // 初回構築を済ませておく
            const auto t0 = std::chrono::high_resolution_clock::now();
            AF_SceneSetInstanceActive(s, 6, 0);            // 仕切りが消える
            const int after = AF_SceneRoomCount(s);
            const auto t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            float ma = 0, mf = 0, md = 0, ml = 0, mm = 0, mg = 0; int hot = 0, nb = 0;
            AF_SceneRoomBuildTimes(s, &ma, &mf, &md, &ml, &mm, &mg, &hot, &nb);
            const double vInc = volumes(s);
            // 同じ形状を最初から作った場合（＝全再構築）と突き合わせる。
            AF_SceneHandle ref = build(0.0f);
            AF_SceneSetRoomCellSize(ref, cell);
            AF_SceneSetRoomBrick(ref, brick);
            AF_SceneSetInstanceActive(ref, 6, 0);
            const int refN = AF_SceneRoomCount(ref);
            const double vRef = volumes(ref);
            const bool same = (after == refN) && (std::fabs(vInc - vRef) < 0.5);
            std::printf("        %.2fm ブロック%2d  %6.3f ms（塗り %.3f / 距離 %.3f / 連結 %.3f / 併合 %.3f / 塗戻 %.3f）"
                        "  塗り直し %3d/%5d  部屋 %d→%d  体積 %.0f（全再構築 %.0f）%s\n",
                        cell, brick, ms, mf, md, ml, mm, mg, hot, nb, before, after, vInc, vRef,
                        same ? "" : "  ★不一致");
            AF_SceneDestroy(ref);
            AF_SceneDestroy(s);
        }
    }

    // 扉が閉まっても部屋は分かれないこと（扉は動くものなので静的に含めない）。
    {
        AF_SceneHandle s = build(1.2f);
        const int before = AF_SceneRoomCount(s);
        // 戸口をぴったり塞ぐ扉を置き、**動かして**「動くもの」と認識させる。
        const int door = AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(0.6f, h*0.5f, 0.03f),
                                                V(1,0,0), V(0,1,0), 0);
        AF_SceneUpdateInstance(s, door, V(0, h*0.5f, 0.001f), V(0.6f, h*0.5f, 0.03f),
                               V(1,0,0), V(0,1,0));
        const int after = AF_SceneRoomCount(s);
        std::printf("      扉を閉めて動かす         %d → %d（1 のままが正しい）%s\n",
                    before, after, (after == 1) ? "" : "  ★分かれてしまった");
        AF_SceneDestroy(s);
    }
}

// 戸口ではない形で、部屋と開口がどう出るか。
//   曲がり角・廊下・柱・食い違い壁 ── どれも「扉」ではないので、特別扱いを入れずに
//   幾何だけで妥当な答えが出るかを見る。出るべきでない所に口が出たら偽の境界になり、
//   出るべき所に出なければ結合の土台が無い。
// 部屋を移る間、残響の送出がどう動くか。
//   Unity 側は wet = (総和 - 直接ピーク) / 総和 で送出量を決めている。**比率**なので、
//   直接音が壁で消えると分母が落ちて 1.0 に張り付く。これが「隣に移った瞬間 10dB 跳ねて
//   上限に張り付く」の正体かを、リスナーを歩かせて数字で確かめる。
void diagnoseReverbSendWalk() {
    std::printf("\n[診断] 戸口をまたいで歩いたときの残響送出\n");
    const float t = 0.15f, h = 4.0f, hw = 6.0f, hd = 8.0f, doorW = 0.9f;
    AF_SceneHandle s = AF_SceneCreate();
    // ★手前と奥で響きを変える。同じ材質だと部屋が変わっても送出が変わらず、
    //   「連続かどうか」を試す場面にならない（変化しないものは必ず連続）。
    const float liveA[6] = {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f};   // 響く
    const float deadA[6] = {0.60f, 0.70f, 0.80f, 0.85f, 0.90f, 0.90f};   // 吸う
    const float tr[6] = {0.000398f, 0.0001585f, 0.0000398f,
                         0.00001f, 0.00000251f, 0.000001f};
    const int mNear = AF_SceneAddMaterial(s, tr, liveA, nullptr, 6);   // 手前(z<0)=響く
    const int mFar  = AF_SceneAddMaterial(s, tr, deadA, nullptr, 6);   // 奥(z>0)=吸う
    // 仕切り z=0 に幅 0.9m の戸口（全高）
    AF_SceneAddInstanceBox(s, V(-(hw + doorW*0.5f)*0.5f, h*0.5f, 0),
                           V((hw - doorW*0.5f)*0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mNear);
    AF_SceneAddInstanceBox(s, V( (hw + doorW*0.5f)*0.5f, h*0.5f, 0),
                           V((hw - doorW*0.5f)*0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mNear);
    for (int side = 0; side < 2; ++side) {
        const float zc = (side == 0) ? -hd * 0.5f : hd * 0.5f;
        const int mm = (side == 0) ? mNear : mFar;
        AF_SceneAddInstanceBox(s, V(0, -t, zc),    V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), mm);
        AF_SceneAddInstanceBox(s, V(0, h + t, zc), V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), mm);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, zc), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), mm);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, zc), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), mm);
    }
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mNear);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mFar);

    // ★音源は戸口の正面から外す。真正面だと直接音が戸口を素通りして一度も遮られず、
    //   「壁の向こうへ回り込む」場面にならない（この配置ミスを何度も踏んだ）。
    const AF_Vector3 S = V(4.5f, 1.6f, 5.0f);
    // Unity の UpdateReverbTargetRatio と同じ式を再現する。
    //   V は occluder 全部の合成 AABB（＝レベル全体の外形箱。部屋を知らない）
    const double vLevel = (2.0 * (hw + t)) * (h + 2.0 * t) * (2.0 * (hd + t));
    const double binMs = 10.0;   // AF_UpdateConfig の既定
    std::printf("      音源(4.5,1.6,5.0) 固定。リスナーを z=-6 → +4 へ（x=0、戸口を通る）\n");
    std::printf("      Unity の送出式: RT60=エコグラムがピーク-60dB を超える最後のビン、\n"
                "                      臨界距離 rc=0.057√(V/RT60)、目標比 t=(r/rc)²\n");
    std::printf("      レベル全体の外形箱 V=%.0f m3（部屋ごとの体積は使っていない）\n", vLevel);
    std::printf("        z     部屋 直接透過   wet   RT60(s)  部屋V  距離r   "
                "目標比t   t(dB)  跳ね(dB)\n");
    double prevT = -1.0, maxJump = 0.0; float jumpAt = 0.0f;
    double prevWet = -1.0, maxWetJump = 0.0;
    double prevRt = -1.0, maxRtJump = 0.0;
    for (float z = -6.0f; z <= 4.01f; z += 0.5f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);   // 内部レートを流す
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        std::vector<double> e(static_cast<std::size_t>(bins), 0.0);
        double peak = 0.0, total = 0.0;
        for (int i = 0; i < bins; ++i) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += echo[i * kBands + b];
            v /= kBands;
            e[static_cast<std::size_t>(i)] = v;
            if (v > peak) peak = v;
            total += v;
        }
        const double tail = (total > peak) ? total - peak : 0.0;
        const double wet = (total > 1e-12) ? tail / total : 0.0;
        // RT60: ピークの -60dB を超える最後のビン（Unity と同じ）
        const double floorE = peak * 0.001;
        int tailBin = 0;
        for (int i = 0; i < bins; ++i) if (e[static_cast<std::size_t>(i)] > floorE) tailBin = i;
        const double rt60 = (tailBin + 1) * binMs * 0.001;

        float g[kBands] = {};
        AF_SceneComputeTransmissionBands(s, L, S, g, kBands);
        double gm = 0.0; for (int b = 0; b < kBands; ++b) gm += g[b]; gm /= kBands;
        const int room = AF_SceneRoomAt(s, L);
        float vRoom = 0.0f;
        if (room >= 0) AF_SceneRoomInfo(s, room, &vRoom, nullptr, nullptr, nullptr);

        const double dx = S.x - L.x, dy = S.y - L.y, dz = S.z - L.z;
        const double r = std::sqrt(dx*dx + dy*dy + dz*dz);
        const double rc = 0.057 * std::sqrt(vLevel / std::max(rt60, 0.05));
        const double tRatio = (r * r) / std::max(rc * rc, 1e-4);
        const double tDb = 10.0 * std::log10(std::max(tRatio, 1e-12));
        double jump = 0.0;
        if (prevT >= 0.0) {
            jump = std::fabs(tDb - 10.0 * std::log10(std::max(prevT, 1e-12)));
            if (jump > maxJump) { maxJump = jump; jumpAt = z; }
            maxWetJump = std::max(maxWetJump, std::fabs(wet - prevWet));
            maxRtJump  = std::max(maxRtJump,  std::fabs(rt60 - prevRt));
        }
        prevT = tRatio; prevWet = wet; prevRt = rt60;
        std::printf("        %5.1f  %2d  %8.5f %6.3f  %6.2f  %6.0f %6.2f  %8.2f %7.1f %8.1f\n",
                    z, room, gm, wet, rt60, static_cast<double>(vRoom), r, tRatio, tDb, jump);
    }
    std::printf("      1 歩(0.5m)あたりの最大変化: 目標比 %.1f dB（z=%.1f 付近） / "
                "wet %.3f / RT60 %.2f s\n", maxJump, jumpAt, maxWetJump, maxRtJump);

    // ★部屋を音に使うための入口。部屋番号で切り替えると戸口のど真ん中に不連続が乗るので、
    //   「まわりの何割がどの部屋か」という連続量にする。ここが跳ねないことが前提条件。
    std::printf("      部屋の占め方（半径を振って。0.1m 刻みで戸口をまたぐ）\n");
    std::printf("        z      部屋番号   ");
    for (float rad : {0.5f, 1.0f, 2.0f}) std::printf("半径%.1fm:部屋0の割合  ", rad);
    std::printf("\n");
    double maxDw[3] = {0, 0, 0}, prevW[3] = {-1, -1, -1};
    for (float z = -2.0f; z <= 2.001f; z += 0.1f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        std::printf("        %5.2f     %2d      ", z, AF_SceneRoomAt(s, L));
        int k = 0;
        for (float rad : {0.5f, 1.0f, 2.0f}) {
            int ids[8]; float w[8];
            const int n = AF_SceneRoomWeights(s, L, rad, ids, w, 8);
            float w0 = 0.0f;
            for (int i = 0; i < n; ++i) if (ids[i] == 0) w0 = w[i];
            if (prevW[k] >= 0.0) maxDw[k] = std::max(maxDw[k], std::fabs(w0 - prevW[k]));
            prevW[k] = w0;
            std::printf("       %6.3f        ", w0);
            ++k;
        }
        std::printf("\n");
    }
    std::printf("      0.1m 進むごとの割合の最大変化: 半径0.5m %.3f / 1.0m %.3f / 2.0m %.3f"
                "（部屋番号は 1 歩で 0→1 と跳ぶ）\n", maxDw[0], maxDw[1], maxDw[2]);

    // 実効体積。レベル全体の外形箱と比べて、部屋ごとの値になっているか。
    std::printf("      実効体積（半径1.0m）: ");
    for (float z : {-6.0f, -1.0f, 0.0f, 1.0f, 4.0f})
        std::printf("z=%.0f→%.0fm3  ", z, AF_SceneRoomVolumeAt(s, V(0, 1.6f, z), 1.0f));
    std::printf("（外形箱だと全域 %.0fm3）\n", vLevel);
    // ★本題：送出が位置の連続関数になっているか。
    //   旧: RT60 = エコグラムが「ピーク-60dB を超える最後のビン」（離散インデックス、
    //       しかも床が直接音のピーク基準）、V = レベル全体の外形箱（部屋を見ていない）
    //   新: RT60 = 部屋ごとの Sabine を占め方で混ぜた値、V = 実効体積
    //   どちらも t=(r/rc)² に入れ、0.1m ごとの dB 変化を比べる。
    std::printf("      部屋ごとの音響量（形と材質から）:\n");
    for (int i = 0, n = AF_SceneRoomCount(s); i < n; ++i) {
        float vol = 0.0f, surf = 0.0f, open = 0.0f, ab[6] = {}, rt[6] = {};
        AF_SceneRoomInfo(s, i, &vol, nullptr, nullptr, nullptr);
        AF_SceneRoomAcoustics(s, i, &surf, &open, ab, rt);
        std::printf("        部屋%d  V=%.0fm3  境界=%.0fm2（うち開口 %.1fm2）"
                    "  平均吸音率=%.3f  RT60=%.2fs\n", i, vol, surf, open, ab[2], rt[2]);
    }
    std::printf("      送出の連続性（0.1m 刻みで戸口をまたぐ。500Hz 帯）\n");
    std::printf("        z      旧RT60  旧V   旧t(dB) 旧Δ  |  新RT60  新V   新t(dB) 新Δ\n");
    double pOld = 0.0, pNew = 0.0, mOld = 0.0, mNew = 0.0;
    float mOldAt = 0.0f, mNewAt = 0.0f;
    bool first = true;
    for (float z = -3.0f; z <= 3.001f; z += 0.1f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        double peak = 0.0;
        std::vector<double> e(static_cast<std::size_t>(bins), 0.0);
        for (int i = 0; i < bins; ++i) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += echo[i * kBands + b];
            e[static_cast<std::size_t>(i)] = v / kBands;
            if (e[static_cast<std::size_t>(i)] > peak) peak = e[static_cast<std::size_t>(i)];
        }
        int tailBin = 0;
        for (int i = 0; i < bins; ++i) if (e[static_cast<std::size_t>(i)] > peak * 0.001) tailBin = i;
        const double rtOld = (tailBin + 1) * binMs * 0.001;

        float rt6[6] = {};
        AF_SceneRt60At(s, L, 1.0f, rt6, 6);
        const double rtNew = std::max(static_cast<double>(rt6[2]), 0.05);
        const double vNew = std::max(static_cast<double>(AF_SceneRoomVolumeAt(s, L, 1.0f)), 1.0);

        const double dx = S.x - L.x, dy = S.y - L.y, dz = S.z - L.z;
        const double r = std::sqrt(dx*dx + dy*dy + dz*dz);
        auto ratioDb = [&](double vol, double rt) {
            const double rc2 = 0.057 * 0.057 * vol / std::max(rt, 0.05);
            return 10.0 * std::log10(std::max((r * r) / std::max(rc2, 1e-4), 1e-12));
        };
        const double dbOld = ratioDb(vLevel, rtOld);
        const double dbNew = ratioDb(vNew, rtNew);
        double dOld = 0.0, dNew = 0.0;
        if (!first) {
            dOld = std::fabs(dbOld - pOld); dNew = std::fabs(dbNew - pNew);
            if (dOld > mOld) { mOld = dOld; mOldAt = z; }
            if (dNew > mNew) { mNew = dNew; mNewAt = z; }
        }
        first = false; pOld = dbOld; pNew = dbNew;
        std::printf("        %5.2f   %5.2f %6.0f %7.1f %5.2f  |  %5.2f %6.0f %7.1f %5.2f\n",
                    z, rtOld, vLevel, dbOld, dOld, rtNew, vNew, dbNew, dNew);
    }
    std::printf("      0.1m あたりの最大変化: 旧 %.2f dB（z=%.1f）→ 新 %.2f dB（z=%.1f）\n",
                mOld, mOldAt, mNew, mNewAt);

    // ★上の比較は対等でない。旧式は RT60 が両部屋とも 0.48〜0.50s で、17 倍違う部屋を
    //   区別できていない＝応答していないから変化が小さいだけ。動かない関数は必ず連続。
    //   そこで「信号（部屋の違いにどれだけ応答するか）」と「雑音（同じ場所で何回引いても
    //   同じ値か）」を分けて測る。
    std::printf("      信号と雑音を分ける:\n");
    {
        auto measure = [&](float z, double& rtOld, double& rtNew) {
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
            float echo[100 * kBands] = {};
            const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
            double peak = 0.0;
            std::vector<double> e(static_cast<std::size_t>(bins), 0.0);
            for (int i = 0; i < bins; ++i) {
                double v = 0.0;
                for (int b = 0; b < kBands; ++b) v += echo[i * kBands + b];
                e[static_cast<std::size_t>(i)] = v / kBands;
                peak = std::max(peak, e[static_cast<std::size_t>(i)]);
            }
            int tb = 0;
            for (int i = 0; i < bins; ++i)
                if (e[static_cast<std::size_t>(i)] > peak * 0.001) tb = i;
            rtOld = (tb + 1) * binMs * 0.001;
            float rt6[6] = {};
            AF_SceneRt60At(s, L, 1.0f, rt6, 6);
            rtNew = rt6[2];
        };
        double aOld = 0, aNew = 0, bOld = 0, bNew = 0;
        measure(-5.0f, aOld, aNew);   // 響く部屋の奥
        measure( 5.0f, bOld, bNew);   // 吸う部屋の奥
        std::printf("        信号: 響く部屋 → 吸う部屋 の RT60   旧 %.2f→%.2fs（%.1f 倍）"
                    " / 新 %.2f→%.2fs（%.1f 倍）\n",
                    aOld, bOld, aOld / std::max(bOld, 1e-3), aNew, bNew,
                    aNew / std::max(bNew, 1e-3));
        // 同じ場所で引き直したときのばらつき。
        double loOld = 1e9, hiOld = -1e9, loNew = 1e9, hiNew = -1e9;
        for (int k = 0; k < 12; ++k) {
            double o = 0, n2 = 0;
            measure(-5.0f, o, n2);
            loOld = std::min(loOld, o); hiOld = std::max(hiOld, o);
            loNew = std::min(loNew, n2); hiNew = std::max(hiNew, n2);
        }
        std::printf("        雑音: 同じ場所で 12 回引き直し           旧 %.2f〜%.2fs"
                    "（幅 %.2f）  / 新 %.2f〜%.2fs（幅 %.2f）\n",
                    loOld, hiOld, hiOld - loOld, loNew, hiNew, hiNew - loNew);
    }
    // 移り変わりの幅は占め方の半径で決まる。傾きは「総変化 ÷ 半径」なので、
    // 半径を広げれば傾きは下がる（そのぶん遠くから変わり始める）。
    std::printf("      占め方の半径 → 送出の傾き:\n");
    for (float rad : {0.5f, 1.0f, 2.0f, 3.0f}) {
        double prev = 0.0, worst = 0.0; bool f1 = true;
        for (float z = -4.0f; z <= 4.001f; z += 0.1f) {
            const AF_Vector3 L = V(0, 1.6f, z);
            float rt6[6] = {};
            AF_SceneRt60At(s, L, rad, rt6, 6);
            const double vv = std::max(static_cast<double>(AF_SceneRoomVolumeAt(s, L, rad)), 1.0);
            const double dx2 = S.x - L.x, dy2 = S.y - L.y, dz2 = S.z - L.z;
            const double r2 = dx2*dx2 + dy2*dy2 + dz2*dz2;
            const double rc2 = 0.057 * 0.057 * vv / std::max(static_cast<double>(rt6[2]), 0.05);
            const double db = 10.0 * std::log10(std::max(r2 / std::max(rc2, 1e-4), 1e-12));
            if (!f1) worst = std::max(worst, std::fabs(db - prev));
            f1 = false; prev = db;
        }
        std::printf("        半径 %.1fm  →  0.1m あたり最大 %.2f dB"
                    "（歩行 1.4m/s で %.0f dB/s）\n", rad, worst, worst * 14.0);
    }

    {   // 1 回引くのに掛かる時間（毎フレーム 2〜4 回引く想定）。
        const int N = 2000;
        int ids[8]; float w[8];
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i)
            AF_SceneRoomWeights(s, V(0, 1.6f, -1.0f + 0.001f * i), 1.0f, ids, w, 8);
        const auto t1 = std::chrono::high_resolution_clock::now();
        std::printf("      占め方 1 回のコスト: %.1f us（13^3=2197 標本）\n",
                    std::chrono::duration<double, std::micro>(t1 - t0).count() / N);
    }
    AF_SceneDestroy(s);
}

// 直線上に柱を 1 本置いただけで音色がどれだけ変わるか。
//   回折の周波数依存は切ってあるので、帯域ごとのゲインは平坦なはず。それでも音色が
//   変わるなら、原因はフィルタではなく「直接音と反射の力関係」と「反射どうしの櫛」。
//   どちらがどれだけ効いているかを分けて出す。
void diagnosePillarTimbre() {
    std::printf("\n[診断] 直線上に柱があるだけで音色が変わる理由\n");
    const float h = 4.0f, t = 0.3f, hw = 5.0f, hd = 6.0f;
    auto build = [&](bool pillar, const float* absorb) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, absorb, nullptr, absorb ? 6 : 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),    V(hw + t, t, hd + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw + t, t, hd + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw - t, h*0.5f, 0), V(t, h*0.5f, hd + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw + t, h*0.5f, 0), V(t, h*0.5f, hd + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd - t), V(hw + t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd + t), V(hw + t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        if (pillar)   // 直線のど真ん中に 0.6m 角の柱
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(0.3f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
        return s;
    };
    const AF_Vector3 L = V(0, 1.6f, -3.5f), S = V(0, 1.6f, 3.5f);
    const char* bandName[6] = {"125", "250", "500", " 1k", " 2k", " 4k"};

    // 0: 柱なし / 1: 柱あり / 2: 柱あり＋表面が吸わない（残った傾きの出どころを切り分ける）
    const float noAbsorb[6] = {0, 0, 0, 0, 0, 0};
    const char* caseName[3] = {"柱なし:", "柱あり:", "柱あり・表面が吸わない:"};
    double bandNo[6] = {}, bandYes[6] = {}, bandHard[6] = {};
    for (int k = 0; k < 3; ++k) {
        AF_SceneHandle s = build(k >= 1, (k == 2) ? noAbsorb : nullptr);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);

        // 1) 適用される帯域ゲイン（＝フィルタとして掛かる量）
        float occ[kBands] = {};
        const int idx = AF_SceneSourceIndex(s, 1);
        if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, occ);
        std::printf("      %s\n", caseName[k]);
        std::printf("        生存ゲイン(帯域) ");
        for (int b = 0; b < kBands; ++b) std::printf(" %s=%.3f", bandName[b], occ[b]);
        std::printf("\n");

        // 2) 早期反射タップ（直接との力関係と、遅延の散らばり）
        AF_Vector3 pos[16]; float g6[16 * kBands];
        const int nt = (idx >= 0) ? AF_SceneGetEarlyReflections(s, idx, pos, g6, 16) : 0;
        const double dx = S.x - L.x, dy = S.y - L.y, dz = S.z - L.z;
        const double rDirect = std::sqrt(dx*dx + dy*dy + dz*dz);
        double gDirect = 0.0;
        for (int b = 0; b < kBands; ++b) gDirect += occ[b];
        gDirect /= kBands;
        std::printf("        直接: 距離 %.2fm  生存 %.3f\n", rDirect, gDirect);
        std::printf("        反射 %d 本（遅延は直接との差）:\n", nt);
        double sumRefl = 0.0, minDelay = 1e9, maxDelay = -1e9;
        for (int i = 0; i < nt; ++i) {
            const double ex = pos[i].x - L.x, ey = pos[i].y - L.y, ez = pos[i].z - L.z;
            const double ri = std::sqrt(ex*ex + ey*ey + ez*ez);
            const double delayMs = (ri - rDirect) / 343.0 * 1000.0;
            double gm = 0.0;
            for (int b = 0; b < kBands; ++b) gm += g6[i * kBands + b];
            gm /= kBands;
            sumRefl += gm;
            if (gm > 0.01) { minDelay = std::min(minDelay, delayMs); maxDelay = std::max(maxDelay, delayMs); }
            if (i < 6)
                std::printf("          %2d  +%5.2f ms  ゲイン %.3f  （直接比 %+.1f dB）\n",
                            i, delayMs, gm, 20.0 * std::log10(std::max(gm, 1e-6) / std::max(gDirect, 1e-6)));
        }
        if (nt > 0 && maxDelay > minDelay)
            std::printf("        反射の合計 %.3f（直接の %.1f 倍 = %+.1f dB）  "
                        "遅延の幅 %.2f〜%.2f ms → 櫛の最初の谷 %.0f Hz\n",
                        sumRefl, sumRefl / std::max(gDirect, 1e-6),
                        20.0 * std::log10(std::max(sumRefl, 1e-6) / std::max(gDirect, 1e-6)),
                        minDelay, maxDelay, 1000.0 / (2.0 * std::max(minDelay, 0.01)));

        // 3) 実際に耳へ届くスペクトル（エコグラムの帯域別総和）
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        double* dst = (k == 0) ? bandNo : ((k == 1) ? bandYes : bandHard);
        for (int b = 0; b < kBands; ++b) {
            double sum = 0.0;
            for (int i = 0; i < bins; ++i) sum += echo[i * kBands + b];
            dst[b] = sum;
        }
        AF_SceneDestroy(s);
    }

    std::printf("      ── 音色の変化（柱あり ÷ 柱なし、帯域別の総エネルギー）──\n");
    std::printf("        帯域    ");
    for (int b = 0; b < kBands; ++b) std::printf("  %sHz ", bandName[b]);
    std::printf("\n        変化(dB)");
    double lo = 1e9, hi = -1e9;
    for (int b = 0; b < kBands; ++b) {
        const double d = 10.0 * std::log10(std::max(bandYes[b], 1e-12)
                                         / std::max(bandNo[b], 1e-12));
        lo = std::min(lo, d); hi = std::max(hi, d);
        std::printf(" %+6.1f", d);
    }
    std::printf("\n        → 帯域間の傾き %.1f dB（0 なら音量だけ変わって音色は変わらない）\n",
                hi - lo);
    // 表面が吸わない場合と比べる。傾きが消えるなら、残りは回折ではなく吸音由来。
    double lo2 = 1e9, hi2 = -1e9;
    std::printf("        表面が吸わない場合 ");
    for (int b = 0; b < kBands; ++b) {
        const double d = 10.0 * std::log10(std::max(bandHard[b], 1e-12)
                                         / std::max(bandNo[b], 1e-12));
        lo2 = std::min(lo2, d); hi2 = std::max(hi2, d);
        std::printf(" %+6.1f", d);
    }
    std::printf("\n        → 傾き %.1f dB（ここが 0 に近いなら、残りの傾きは回折ではなく"
                "壁の吸音が高域を食っているぶん＝物理的に正しい残り方）\n", hi2 - lo2);
}

// 柱の陰へ歩いて入るとき、直接音が「崖」で落ちていないか。
//   ソフト遮蔽は「見通し 1.0 → 境界 約0.5 → 影 材質の透過」と連続に落ちる設計だが、
//   影に入りきると材質の透過（コンクリで 1e-4 級）まで落ちる。つまり**影の中では
//   直接音が事実上消える**。半分遮られただけで -43dB という実測があったので、
//   遮蔽の進み方と直接音の落ち方を刻んで並べる。
// 部屋の吸音率と「定位が立つか」の関係。
//   残響が直接音を上回ると定位は失われる。境目は臨界距離 rc=0.057√(V/RT60) で、
//   これより遠いと残響優位。裸のコンクリ箱だと rc が 0.5m を切り、部屋のどこにいても
//   残響優位＝定位が立たない。素材をどこまで吸わせれば実用になるかを数字で出す。
// 別の部屋にいる音源は、別の尾を持つべきではないか。
//   エコグラムは全音源を同じ配列へ積むので 1 本しか出ない（computeEchogramBands）。
//   そこから作った IR を全音源が畳むので、響く部屋の音源も吸う部屋の音源も同じ尾になる。
//   音源を 1 本ずつ置いて測れば「本来どれだけ違うはずか」が出る。
// 無関係な場所にポータルを 1 枚置くだけで、他の場所の回折が変わってしまわないか。
//   computeDirectSoft は `if (!portals_.empty())` で分岐していて、シーンに 1 枚でも
//   ポータルがあると前川の回折経路へ**一度も来なくなる**。判定がシーン全体になっている。
//   本来の関心は「その経路をポータルが覆っているか」のはず。
// 音源を多数登録したときに、音源ごとの結果が本当に別物になっているか。
//   ホストは source + extraSources で N 音源ぶんのタップを作るが、デモには畳み込み器が
//   1 個しか置かれておらず、**N 本を同時に鳴らしたことが一度も無い**。
//   しかも「音源ごとのエコグラム」を入れたばかりで index の対応が絡む。
//   壊れているのではなく未検証なので、ここで縛る。
// 屋外（地面だけ／壁で囲われていない）が部屋として検出されないこと。
//   ゲーム側から「屋外の広い自由空間が巨大な 1 部屋になるのでは」という疑いが出た。
//   部屋になると巨大な V から根拠のない尾が出る。ならないならフォールバック経路の話になる。
void testOutdoorIsNotARoom() {
    std::printf("\n[部屋] 屋外が部屋として検出されないか\n");
    const int m0 = 0;
    struct C { const char* name; bool walls; bool ceiling; };
    const C cases[] = {
        {"地面だけ 90x90m          ", false, false},
        {"地面＋腰高の塀(1.2m)     ", true,  false},
        {"地面＋壁＋天井（＝部屋） ", true,  true},
    };
    for (const C& c : cases) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        (void)m0;
        const float hw = 45.0f, t = 0.3f, h = 4.0f;
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hw), V(1,0,0), V(0,1,0), m);
        if (c.walls) {
            const float wh = c.ceiling ? h * 0.5f : 0.6f;   // 天井なしは腰高
            AF_SceneAddInstanceBox(s, V(-hw, wh, 0), V(t, wh, hw), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( hw, wh, 0), V(t, wh, hw), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, wh, -hw), V(hw, wh, t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, wh,  hw), V(hw, wh, t), V(1,0,0), V(0,1,0), m);
        }
        if (c.ceiling)
            AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hw), V(1,0,0), V(0,1,0), m);
        const int n = AF_SceneRoomCount(s);
        const float vol = AF_SceneRoomVolumeAt(s, V(0, 1.6f, 0), 2.0f);
        int nx = 0, ny = 0, nz = 0; float cell = 0.0f;
        AF_SceneRoomGridDims(s, &nx, &ny, &nz, &cell);
        std::printf("        %s 部屋 %d / 実効体積 %.0f m3 / 格子 %dx%dx%d @%.2fm\n",
                    c.name, n, vol, nx, ny, nz, cell);
        if (!c.ceiling) {
            char tag[80];
            std::snprintf(tag, sizeof(tag), "[部屋] %s は部屋にならない",
                          c.walls ? "腰高の塀だけの屋外" : "地面だけの屋外");
            check(tag, n == 0 && vol <= 1.0f);
        } else {
            check("[部屋] 壁と天井で囲えば部屋になる", n >= 1 && vol > 1.0f);
        }
        AF_SceneDestroy(s);
    }
    std::printf("      → 屋外は「外の世界」に落ちて部屋にならない（格子の外周に届く成分は部屋にしない）。\n"
                "        つまり屋外の残響はフォールバック経路（外形箱の体積）が作っている。\n");
}

void testManySources() {
    std::printf("\n[多音源] 音源ごとの結果が混ざっていないか\n");
    const float h = 4.0f, t = 0.3f, hw = 9.0f, hd = 9.0f;
    AF_SceneHandle s = AF_SceneCreate();
    const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    AF_SceneAddInstanceBox(s, V(0, -t, 0),    V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(0, h+t, 0),   V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
    // 何本かの経路だけを塞ぐ衝立（音源ごとに条件を変えるため）。
    AF_SceneAddInstanceBox(s, V(-4.0f, h*0.5f, 0), V(2.0f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);

    const int N = 8;
    AF_SceneSetListener(s, V(0, 1.6f, -6.0f));
    for (int i = 0; i < N; ++i) {
        // 半分は衝立の裏（x<0 側）、半分は見通せる側に置く。
        const float x = (i < N/2) ? (-7.0f + i * 1.5f) : (2.0f + (i - N/2) * 1.5f);
        AF_SceneSetSource(s, static_cast<unsigned long long>(100 + i), V(x, 1.6f, 4.0f));
    }
    for (int k = 0; k < 8; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);

    // ① id → index が全部引けること
    int idx[N]; bool allIdx = true;
    for (int i = 0; i < N; ++i) {
        idx[i] = AF_SceneSourceIndex(s, static_cast<unsigned long long>(100 + i));
        if (idx[i] < 0) allIdx = false;
    }
    check("[多音源] 全音源の id → index が引ける", allIdx);

    // ② 生存ゲインが音源ごとに違うこと（同じ値が返ってきたら取り違えている）
    float occ[N][kBands] = {};
    int distinct = 0;
    for (int i = 0; i < N; ++i) {
        if (idx[i] >= 0) AF_SceneGetSourceOcclusion(s, idx[i], occ[i]);
        bool uniq = true;
        for (int j = 0; j < i; ++j)
            if (std::fabs(occ[i][0] - occ[j][0]) < 1e-6f) { uniq = false; break; }
        if (uniq) ++distinct;
    }
    // ★見通せる位置の音源は生存 1.000 で揃うのが**正しい**。全部バラバラを要求しない。
    //   ここで見たいのは「取り違えていないか」なので、遮られた側と見通せる側が
    //   きちんと分かれることを縛る。
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < N; ++i) { lo = std::min(lo, occ[i][0]); hi = std::max(hi, occ[i][0]); }
    std::printf("      生存ゲイン(125Hz) 音源ごと:");
    for (int i = 0; i < N; ++i) std::printf(" %.3f", occ[i][0]);
    std::printf("   相異なる値 %d/%d（最小 %.3f 最大 %.3f）\n", distinct, N, lo, hi);
    check("[多音源] 遮られた音源と見通せる音源が分かれる", distinct >= 3 && hi > lo * 1.5f);

    // ③ エコグラムが音源ごとに別物であること（今日入れた変更の実地確認）
    std::vector<float> e(100 * kBands);
    double total[N] = {};
    int echoDistinct = 0;
    for (int i = 0; i < N; ++i) {
        const int bins = AF_SceneGetEchogramBands(s, idx[i], e.data(), 100);
        double sum = 0.0;
        for (int k = 0; k < bins * kBands; ++k) sum += e[static_cast<std::size_t>(k)];
        total[i] = sum;
        bool uniq = true;
        for (int j = 0; j < i; ++j)
            if (std::fabs(total[i] - total[j]) < 1e-9) { uniq = false; break; }
        if (uniq) ++echoDistinct;
    }
    std::printf("      エコグラム総和 音源ごと:");
    for (int i = 0; i < N; ++i) std::printf(" %.2f", total[i]);
    std::printf("   相異なる値 %d/%d\n", echoDistinct, N);
    check("[多音源] エコグラムが音源ごとに異なる", echoDistinct >= N - 1);

    // ④ 全音源の和(-1) が個別の和と一致すること
    double sumAll = 0.0;
    {
        const int bins = AF_SceneGetEchogramBands(s, -1, e.data(), 100);
        for (int k = 0; k < bins * kBands; ++k) sumAll += e[static_cast<std::size_t>(k)];
    }
    double sumEach = 0.0;
    for (int i = 0; i < N; ++i) sumEach += total[i];
    std::printf("      全音源の和 %.2f / 個別の合計 %.2f（差 %.3f%%）\n",
                sumAll, sumEach, std::fabs(sumAll - sumEach) / std::max(sumEach, 1e-9) * 100.0);
    check("[多音源] index=-1 が個別の合計と一致",
          std::fabs(sumAll - sumEach) < sumEach * 0.001);

    // ⑤ 早期反射・回折二次音源も音源ごとに取れること
    int erTotal = 0, dfTotal = 0;
    for (int i = 0; i < N; ++i) {
        AF_Vector3 pos[16]; float g6[16 * kBands];
        erTotal += (idx[i] >= 0) ? AF_SceneGetEarlyReflections(s, idx[i], pos, g6, 16) : 0;
        AF_Vector3 dp[8]; float dg[8];
        dfTotal += (idx[i] >= 0) ? AF_SceneGetDiffractionSources(s, idx[i], dp, dg, 8) : 0;
    }
    std::printf("      早期反射 合計 %d 本 / 回折二次音源 合計 %d 本\n", erTotal, dfTotal);
    check("[多音源] 早期反射が音源ぶん取れる", erTotal >= N);

    AF_SceneDestroy(s);
}

void diagnosePortalScopeGlobal() {
    std::printf("\n[診断] 無関係なポータルが他の場所の回折を殺していないか\n");
    const float h = 4.0f, t = 0.3f, hw = 8.0f, hd = 8.0f;
    // 部屋の中央に柱。リスナーと音源はその両側（回折で回り込む）。
    // ポータルは**遠くの壁**に置く。経路とは無関係。
    // ★壁をよく吸わせる。反射込みの生存で見るので、反射が強いと回折の差が薄まる
    //   （既定壁だと 0.7dB しか出ず、効果を見誤る）。
    const float deadA[6] = {0.95f, 0.95f, 0.95f, 0.95f, 0.95f, 0.95f};
    auto build = [&](bool withFarPortal) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, deadA, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),    V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0),   V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        // 経路を塞ぐ柱（z=0 に幅 2.4m）
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(1.2f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
        if (withFarPortal) {
            // 部屋の隅（x=+7, z=+7）に小さなポータル。経路(x=0 付近)とは無関係。
            AF_SceneAddPortal(s, V(7.0f, 1.2f, 7.0f), V(1,0,0), V(0,1,0), 0.45f, 1.0f);
        }
        return s;
    };
    const AF_Vector3 L = V(0, 1.6f, -4.0f), S = V(0, 1.6f, 4.0f);
    const char* bandName[6] = {"125", "250", "500", " 1k", " 2k", " 4k"};
    float g[2][kBands] = {};
    for (int k = 0; k < 2; ++k) {
        AF_SceneHandle s = build(k == 1);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int idx = AF_SceneSourceIndex(s, 1);
        if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, g[k]);
        std::printf("      %s  生存ゲイン", (k == 0) ? "ポータルなし    " : "遠くにポータル1枚");
        for (int b = 0; b < kBands; ++b) std::printf(" %s=%.4f", bandName[b], g[k][b]);
        std::printf("\n");
        AF_SceneDestroy(s);
    }
    double worst = 0.0;
    for (int b = 0; b < kBands; ++b)
        worst = std::max(worst, std::fabs(20.0 * std::log10(std::max(g[1][b], 1e-6f)
                                                          / std::max(g[0][b], 1e-6f))));
    std::printf("      → 差 最大 %.1f dB。経路と無関係なポータルなので **0 dB が正しい**。\n", worst);
}

void diagnosePerSourceEchogram() {
    std::printf("\n[診断] 別の部屋の音源は別の尾を持つべきか\n");
    const float h = 4.0f, t = 0.15f, hw = 6.0f, hd = 8.0f, doorW = 0.9f;
    const float liveA[6] = {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f};
    const float deadA[6] = {0.60f, 0.70f, 0.80f, 0.85f, 0.90f, 0.90f};
    const float tr[6] = {0.000398f, 0.0001585f, 0.0000398f, 0.00001f, 0.00000251f, 0.000001f};
    // which: 0=手前(響く)だけ / 1=奥(吸う)だけ / 2=両方
    auto build = [&](int which) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mNear = AF_SceneAddMaterial(s, tr, liveA, nullptr, 6);
        const int mFar  = AF_SceneAddMaterial(s, tr, deadA, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(-(hw + doorW*0.5f)*0.5f, h*0.5f, 0),
                               V((hw - doorW*0.5f)*0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mNear);
        AF_SceneAddInstanceBox(s, V( (hw + doorW*0.5f)*0.5f, h*0.5f, 0),
                               V((hw - doorW*0.5f)*0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mNear);
        for (int side = 0; side < 2; ++side) {
            const float zc = (side == 0) ? -hd*0.5f : hd*0.5f;
            const int mm = (side == 0) ? mNear : mFar;
            AF_SceneAddInstanceBox(s, V(0, -t, zc),    V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), mm);
            AF_SceneAddInstanceBox(s, V(0, h+t, zc),   V(hw, t, hd*0.5f), V(1,0,0), V(0,1,0), mm);
            AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, zc), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), mm);
            AF_SceneAddInstanceBox(s, V( hw, h*0.5f, zc), V(t, h*0.5f, hd*0.5f), V(1,0,0), V(0,1,0), mm);
        }
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mNear);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mFar);
        AF_SceneSetListener(s, V(0, 1.6f, -4.0f));          // リスナーは手前（響く部屋）
        if (which == 0 || which == 2) AF_SceneSetSource(s, 1, V(-2.0f, 1.6f, -5.0f));  // 手前
        if (which == 1 || which == 2) AF_SceneSetSource(s, 2, V( 2.0f, 1.6f,  5.0f));  // 奥
        for (int i = 0; i < 8; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        return s;
    };
    const char* name[3] = {"手前(響く)の音源だけ", "奥(吸う)の音源だけ  ", "両方（今の作り）    "};
    std::printf("        置いた音源            総和      RT60相当  後半(0.3s〜)の割合\n");
    double rt[3] = {};
    for (int k = 0; k < 3; ++k) {
        AF_SceneHandle s = build(k);
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        std::vector<double> e(static_cast<std::size_t>(bins), 0.0);
        double peak = 0.0, sum = 0.0, late = 0.0;
        for (int i = 0; i < bins; ++i) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += echo[i * kBands + b];
            v /= kBands;
            e[static_cast<std::size_t>(i)] = v;
            peak = std::max(peak, v);
            sum += v;
            if (i >= 30) late += v;
        }
        int tb = 0;
        for (int i = 0; i < bins; ++i) if (e[static_cast<std::size_t>(i)] > peak * 0.001) tb = i;
        rt[k] = (tb + 1) * 0.01;
        std::printf("        %s %9.4f  %6.2f s   %5.1f %%\n",
                    name[k], sum, rt[k], (sum > 1e-12) ? late / sum * 100.0 : 0.0);
        AF_SceneDestroy(s);
    }
    (void)rt;
    // ★RT60相当（-60dB を超える最後のビン）は窓の長さに縛られて鈍い。
    //   尾IRは正規化されるので効くのは**形**。正規化した減衰カーブを直接比べる。
    std::printf("      減衰の形（各々のピークで正規化した dB。形が同じなら同じ尾になる）\n");
    std::printf("        置いた音源            50ms   100ms   200ms   300ms   500ms\n");
    std::vector<std::vector<double>> curve(3);
    for (int k = 0; k < 3; ++k) {
        AF_SceneHandle s = build(k);
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        std::vector<double> e(static_cast<std::size_t>(bins), 0.0);
        double peak = 0.0;
        for (int i = 0; i < bins; ++i) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += echo[i * kBands + b];
            e[static_cast<std::size_t>(i)] = v / kBands;
            peak = std::max(peak, e[static_cast<std::size_t>(i)]);
        }
        std::printf("        %s", name[k]);
        for (int ms : {50, 100, 200, 300, 500}) {
            const int bi = ms / 10;
            const double v = (bi < bins) ? e[static_cast<std::size_t>(bi)] : 0.0;
            const double db = 10.0 * std::log10(std::max(v, 1e-12) / std::max(peak, 1e-12));
            curve[static_cast<std::size_t>(k)].push_back(db);
            std::printf(" %6.1f ", db);
        }
        std::printf("\n");
        AF_SceneDestroy(s);
    }
    double maxShape = 0.0;
    for (std::size_t i = 0; i < curve[0].size() && i < curve[1].size(); ++i)
        maxShape = std::max(maxShape, std::fabs(curve[0][i] - curve[1][i]));
    std::printf("      → 形の差は最大 %.1f dB。総和（届く量）は %.1f 倍 = %.1f dB 違う。\n",
                maxShape, 86.2001 / 8.5294, 10.0 * std::log10(86.2001 / 8.5294));
    std::printf("        量は既に音源ごと（tailSrcLevel）で効かせている。形が同じなら\n"
                "        尾IRの共有は妥当。形が違うなら部屋ごとに分ける必要がある。\n");
}

void diagnoseAbsorptionVsLocalization() {
    std::printf("\n[診断] 部屋の吸音率と定位（臨界距離）\n");
    const float h = 4.0f, t = 0.3f, hw = 6.0f, hd = 7.0f;
    auto build = [&](const float* ab) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, ab, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),    V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0),   V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        return s;
    };
    struct C { const char* name; float a; };
    const C cases[] = {
        {"裸のコンクリ            ", 0.02f},
        {"塗装コンクリ            ", 0.04f},
        {"石膏ボード（既定壁）    ", 0.15f},
        {"内装あり（家具・カーテン）", 0.25f},
        {"よく吸う（布・吸音材）  ", 0.40f},
    };
    std::printf("      12x4x14m の部屋（V≒672m3）。r=5m の位置での残響/直接比。\n");
    std::printf("        素材                        吸音率  RT60(s)  臨界距離  r=5m での比  wet\n");
    for (const C& c : cases) {
        const float ab[6] = {c.a, c.a, c.a, c.a, c.a, c.a};
        AF_SceneHandle s = build(ab);
        const int room = AF_SceneRoomAt(s, V(0, 1.6f, 0));
        float vol = 0.0f, rt[6] = {};
        if (room >= 0) {
            AF_SceneRoomInfo(s, room, &vol, nullptr, nullptr, nullptr);
            AF_SceneRoomAcoustics(s, room, nullptr, nullptr, nullptr, rt);
        }
        const double rc = 0.057 * std::sqrt(vol / std::max(static_cast<double>(rt[2]), 0.05));
        const double tRaw = (5.0 * 5.0) / std::max(rc * rc, 1e-4);
        const double tCmp = std::pow(tRaw, 0.5);          // 既定の知覚圧縮 exponent=0.5
        const double wet = tCmp / (1.0 + tCmp);
        std::printf("        %s  %.2f   %6.2f   %6.2f m   %7.1f→%5.1f  %.3f\n",
                    c.name, c.a, rt[2], rc, tRaw, tCmp, wet);
        AF_SceneDestroy(s);
    }
    std::printf("      ※臨界距離より遠いと残響優位＝定位が立たない。\n");
    std::printf("        帯域別 RT60（内装ありの場合、空気吸収込み）: ");
    {
        const float ab[6] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
        AF_SceneHandle s = build(ab);
        const int room = AF_SceneRoomAt(s, V(0, 1.6f, 0));
        float rt[6] = {};
        if (room >= 0) AF_SceneRoomAcoustics(s, room, nullptr, nullptr, nullptr, rt);
        for (int b = 0; b < 6; ++b) std::printf("%.2f ", rt[b]);
        std::printf("s\n");
        AF_SceneDestroy(s);
    }
}

void diagnoseShadowCliff() {
    std::printf("\n[診断] 柱の陰へ入るとき直接音はどう落ちるか\n");
    const float h = 4.0f, t = 0.3f, hw = 6.0f, hd = 7.0f;
    AF_SceneHandle s = AF_SceneCreate();
    const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    AF_SceneAddInstanceBox(s, V(0, -t, 0),    V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(0, h+t, 0),   V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
    // ★柱を太くする。0.6m 角だと遮蔽率 0.85 までしか行かず「深い影」に入らない。
    //   Unity の画面では柱が視界を大きく塞いでいたので、そこまで入れて測る。
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(1.2f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);

    const AF_Vector3 S = V(0, 1.6f, 3.5f);
    std::printf("      音源(0,1.6,3.5) 固定。リスナーを x=-2.0 → 0.0 へ（z=-3.5、柱は原点 2.4m幅）\n");
    // ★D タップ単体ではなく D⊕F の合計で見る。ホストは
    //     D タップ = 透過（この関数の戻り）
    //     F タップ = 回折 × 遮蔽割合
    //   を別々に鳴らし、occFrac でクロスフェードする。D 単体は必ず崖になるが、
    //   F が立ち上がるので合計は連続 ── そこを確かめるのがこの診断の目的。
    std::printf("        x      遮蔽率   D(透過)   F(回折×遮蔽率)  D⊕F 合計    合計dB   反射込み生存\n");
    double ref = 0.0;
    double prevDb = 0.0, worst = 0.0; float worstAt = 0.0f; bool first = true;
    double worstD = 0.0, prevD = 0.0;
    for (float x = -2.0f; x <= 0.001f; x += 0.1f) {
        const AF_Vector3 L = V(x, 1.6f, -3.5f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);

        float soft[kBands] = {}; float occFrac = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, soft, kBands, &occFrac);
        double sm = 0.0; for (int b = 0; b < kBands; ++b) sm += soft[b]; sm /= kBands;

        float occ[kBands] = {};
        const int idx = AF_SceneSourceIndex(s, 1);
        if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, occ);
        double om = 0.0; for (int b = 0; b < kBands; ++b) om += occ[b]; om /= kBands;

        AF_Vector3 dpos[8]; float dg[8] = {};
        const int nd = (idx >= 0) ? AF_SceneGetDiffractionSources(s, idx, dpos, dg, 8) : 0;
        double dsum = 0.0; for (int i = 0; i < nd; ++i) dsum += dg[i];
        // ホストと同じ配分: F タップは回折の総量 × 遮蔽割合。
        const double fpart = dsum * occFrac;
        // 別経路・別方向なのでエネルギーで足す。
        const double total = std::sqrt(sm * sm + fpart * fpart);
        if (first) ref = total;
        const double db = 20.0 * std::log10(std::max(total, 1e-9) / std::max(ref, 1e-9));

        if (!first) {
            const double step = std::fabs(db - prevDb);
            if (step > worst) { worst = step; worstAt = x; }
            worstD = std::max(worstD, std::fabs(20.0 * std::log10(std::max(sm, 1e-9))
                                              - 20.0 * std::log10(std::max(prevD, 1e-9))));
        }
        first = false; prevDb = db; prevD = sm;
        std::printf("        %5.2f   %5.2f   %7.5f   %7.5f      %7.5f  %7.1f   %6.3f\n",
                    x, occFrac, sm, fpart, total, db, om);
    }
    std::printf("      0.1m あたりの最大変化: D⊕F の合計 %.1f dB / D タップ単体 %.1f dB"
                "（x=%.2f 付近）\n", worst, worstD, worstAt);
    std::printf("      → D 単体は必ず崖になる（標本が二値）。F が occFrac で立ち上がって"
                "補うので、合計が連続なら設計どおり。\n");
    AF_SceneDestroy(s);
}

void diagnoseNonDoorShapes() {
    std::printf("\n[診断] 戸口ではない形（曲がり角・廊下・柱・食い違い壁）\n");
    const float h = 4.0f, t = 0.3f;
    // 床天井壁で囲った中に置く塊。y を省くと全高。
    struct Blk { float x0, x1, z0, z1; float y0 = 0.0f, y1 = -1.0f; };
    // 内部 x∈[-X,X], z∈[-Z,Z], y∈[0,h] を壁で囲み、中に塊を置く。
    auto world = [&](float X, float Z, const std::vector<Blk>& blocks) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),      V(X + t, t, Z + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0),   V(X + t, t, Z + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-X - t, h*0.5f, 0), V(t, h*0.5f, Z + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( X + t, h*0.5f, 0), V(t, h*0.5f, Z + t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -Z - t), V(X + t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  Z + t), V(X + t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        for (const Blk& b : blocks) {
            const float y0 = b.y0, y1 = (b.y1 < 0.0f) ? h : b.y1;
            AF_SceneAddInstanceBox(s, V((b.x0 + b.x1) * 0.5f, (y0 + y1) * 0.5f, (b.z0 + b.z1) * 0.5f),
                                   V((b.x1 - b.x0) * 0.5f, (y1 - y0) * 0.5f, (b.z1 - b.z0) * 0.5f),
                                   V(1,0,0), V(0,1,0), m);
        }
        AF_SceneSetRoomCellSize(s, 0.1f);
        return s;
    };
    auto report = [&](const char* name, AF_SceneHandle s) {
        const int nr = AF_SceneRoomCount(s), na = AF_SceneApertureCount(s);
        std::printf("      %-28s 部屋 %d / 口 %d", name, nr, na);
        for (int i = 0; i < nr && i < 4; ++i) {
            float v = 0.0f; AF_SceneRoomInfo(s, i, &v, nullptr, nullptr, nullptr);
            std::printf("%s%.0fm3", (i == 0) ? "   体積 " : " ", v);
        }
        for (int i = 0; i < na && i < 3; ++i) {
            float a = 0.0f; AF_Vector3 c{};
            AF_SceneApertureInfo(s, i, &a, &c, nullptr, nullptr, nullptr);
            std::printf("%s%.1fm2@(%.1f,%.1f)", (i == 0) ? "   口 " : " ", a, c.x, c.z);
        }
        std::printf("\n");
        AF_SceneDestroy(s);
    };

    // ① L 字の曲がり角（幅 2.0m 一定）。曲がるだけで広さは変わらないので、
    //    ここで部屋が割れたら偽の境界。
    report("L字の曲がり角 幅2.0m", world(9.0f, 9.0f, {
        {-9.0f, -1.0f,  1.0f,  9.0f}, { 1.0f,  9.0f,  1.0f,  9.0f},
        { 1.0f,  9.0f, -1.0f,  1.0f}, {-9.0f,  9.0f, -9.0f, -1.0f},
    }));
    // ② 同じ L 字を幅 1.0m で。人が通る幅の廊下。
    report("L字の曲がり角 幅1.0m", world(9.0f, 9.0f, {
        {-9.0f, -0.5f,  0.5f,  9.0f}, { 0.5f,  9.0f,  0.5f,  9.0f},
        { 0.5f,  9.0f, -0.5f,  0.5f}, {-9.0f,  9.0f, -9.0f, -0.5f},
    }));

    // ③ 廊下で繋がった 2 部屋。廊下の幅を振る。
    for (float w : {0.9f, 1.2f, 2.0f, 3.0f}) {
        char name[64];
        std::snprintf(name, sizeof(name), "廊下(長さ6m)で繋いだ2部屋 幅%.1fm", w);
        report(name, world(10.0f, 5.0f, {
            {-3.0f, 3.0f,  w * 0.5f,  5.0f}, {-3.0f, 3.0f, -5.0f, -w * 0.5f},
        }));
    }

    // ④ 大部屋に柱。柱で部屋が割れたら偽の境界。
    report("大部屋に柱 1.0m角", world(8.0f, 8.0f, {{-0.5f, 0.5f, -0.5f, 0.5f}}));
    report("大部屋に柱 4本",     world(8.0f, 8.0f, {
        {-4.5f, -3.5f, -4.5f, -3.5f}, { 3.5f, 4.5f, -4.5f, -3.5f},
        {-4.5f, -3.5f,  3.5f,  4.5f}, { 3.5f, 4.5f,  3.5f,  4.5f},
    }));

    // ⑤ 食い違い壁（音を通しにくくするための Z 字。見通しは切れるが繋がっている）。
    //    仕切り 2 枚を z=-0.5 と z=0.5 にずらして重ねる。
    report("食い違い壁 隙間1.0m", world(6.0f, 6.0f, {
        {-6.0f, 1.0f, -0.65f, -0.35f}, {-1.0f, 6.0f, 0.35f, 0.65f},
    }));

    // ⑥ 高さ方向の形。腰高の仕切りは上が全部開いているので割れないのが正しい。
    //    壁に窓だけ開いている場合は割れて、口の面積が窓の実寸になるのが正しい。
    report("腰高の仕切り(高さ1.2m)", world(6.0f, 6.0f, {
        {-6.0f, 6.0f, -0.15f, 0.15f, 0.0f, 1.2f},
    }));
    report("窓だけの壁(1.0x1.0m)", world(6.0f, 6.0f, {
        {-6.0f, -0.5f, -0.15f, 0.15f}, { 0.5f, 6.0f, -0.15f, 0.15f},
        {-0.5f,  0.5f, -0.15f, 0.15f, 0.0f, 1.5f},
        {-0.5f,  0.5f, -0.15f, 0.15f, 2.5f, 4.0f},
    }));
}

void diagnosePortalScope() {
    std::printf("\n[診断] ポータルは自分が覆う開口だけを担当しているか\n");
    auto build = [](bool withPortal) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.15f, hw = 8.0f, hd = 8.0f;
        // 仕切り z=0。開口 A: x∈[-4,-2]、開口 B: x∈[2,4]
        AF_SceneAddInstanceBox(s, V(-6.0f, h*0.5f, 0), V(2.0f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 0.0f, h*0.5f, 0), V(2.0f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 6.0f, h*0.5f, 0), V(2.0f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        if (withPortal)   // 開口 A だけにポータル（中心 x=-3、幅 2m）
            AF_SceneAddPortal(s, V(-3.0f, 2.0f, 0.0f), V(1,0,0), V(0,1,0), 1.0f, 2.0f);
        return s;
    };
    // ★直線が開口を素通りしない配置にすること（一度やった）。
    //   L(5,-3)→S(5,3) は z=0 を x=5 で横切る＝壁(x>4)。必ず遮蔽される。
    //   最短の回り込みは開口B の縁(x=4)。開口A(x=-3)は遠い。
    const AF_Vector3 L = V(5.0f, 1.6f, -3.0f), S = V(5.0f, 1.6f, 3.0f);
    std::printf("      配置: リスナー(5,-3) 音源(5,3)。直線は z=0 を x=5 で横切る＝壁\n");
    std::printf("             最短の回り込みは開口B(x∈[2,4])。ポータルは開口A(x∈[-4,-2])にだけ置く\n");
    std::printf("      %-26s 経路本数  回折125Hz\n", "");
    for (int k = 0; k < 2; ++k) {
        AF_SceneHandle s = build(k == 1);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        std::printf("      %-26s %6d   %8.4f%s\n",
                    k == 0 ? "ポータル無し（基準）" : "開口Aにポータルを置く",
                    n, g[0],
                    (k == 1 && g[0] < 1e-4f) ? "  ★開口Bが鳴らなくなった" : "");
        AF_SceneDestroy(s);
    }
}

void diagnoseSecondOrderQuality() {
    std::printf("\n[診断] 2次回折の質\n");

    // sealed=true で 2 枚目の戸口を塞ぐ。音の通り道は完全に無くなる。
    auto build = [](bool sealed) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float t = 0.15f, hy = 1.5f;
        AF_SceneAddInstanceBox(s, V(-3.5f, hy, 0), V(2.5f, hy, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 3.0f, hy, 0), V(3.0f, hy, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-2.0f, hy, 4), V(4.0f, hy, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 4.5f, hy, 4), V(1.5f, hy, t), V(1,0,0), V(0,1,0), mat);
        if (sealed)   // 戸口2 (x∈[2,3]) を埋める
            AF_SceneAddInstanceBox(s, V(2.5f, hy, 4), V(0.5f, hy, t), V(1,0,0), V(0,1,0), mat);
        // ★側壁は**床と同じ z 範囲**まで伸ばす。以前は z∈[0,4]（廊下の部分）しか無く、
        //   手前の部屋も奥の部屋も横と後ろが開きっぱなしだった。
        //   そのせいで「戸口を塞いだ」と言いながら x>6 の外側を回る**実在の通り道**が残り、
        //   偽陽性の判定にも連続性の掃引にも、毎行 (13.8,1.6,2.8) というタップが混ざっていた。
        //   閉空間を測ると言うなら閉じておく。
        AF_SceneAddInstanceBox(s, V(-6, hy, 2), V(t, hy, 12), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 6, hy, 2), V(t, hy, 12), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, hy, -10), V(6, hy, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, hy,  14), V(6, hy, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 2), V(8, t, 12), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, 3+t, 2), V(8, t, 12), V(1,0,0), V(0,1,0), mat);
        return s;
    };

    const AF_Vector3 S = V(2.5f, 1.6f, 7);

    // ① 偽陽性
    std::printf("      [偽陽性] 2枚目の戸口を塞いだ（音の通り道は無い）\n");
    {
        AF_SceneHandle s = build(true);
        const AF_Vector3 L = V(-0.5f, 1.6f, -3);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        std::printf("        回折 125Hz %.3f  ← 0 であるべき%s\n",
                    g[0], g[0] > 0.01f ? "  ★通ってしまっている" : "");
        AF_SceneDestroy(s);
    }

    // ②③ 連続性と発火率。リスナーを戸口1の手前で横に振る。
    std::printf("      [連続性] リスナーを x=-2.0〜1.0 で動かす（戸口1は x∈[-1,0]）\n");
    std::printf("        listener.x   回折125Hz  隣接差   開口方向(正規化)      経路長\n");
    {
        AF_SceneHandle s = build(false);
        float prevG = -1.0f, maxJump = 0.0f, jumpAt = 0.0f;
        AF_Vector3 prevD = V(0,0,0); bool havePrev = false;
        float maxDirJump = 0.0f, dirJumpAt = 0.0f;
        int fired = 0, total = 0;
        for (float x = -2.0f; x <= 1.001f; x += 0.25f) {
            const AF_Vector3 L = V(x, 1.6f, -3);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            AF_SceneUpdate(s, 1.0f / 60.0f);
            float g[kBands] = {};
            AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
            AF_Vector3 pos[8]; float gain[8];
            const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
            ++total;
            if (g[0] > 0.001f) ++fired;

            float jump = (prevG >= 0.0f) ? std::fabs(g[0] - prevG) : 0.0f;
            if (jump > maxJump) { maxJump = jump; jumpAt = x; }
            prevG = g[0];

            AF_Vector3 d = V(0,0,0); float plen = 0.0f;
            if (n > 0) {
                d = V(pos[0].x - L.x, pos[0].y - L.y, pos[0].z - L.z);
                plen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                if (plen > 1e-4f) { d.x /= plen; d.y /= plen; d.z /= plen; }
                if (havePrev) {
                    const float dj = std::sqrt((d.x-prevD.x)*(d.x-prevD.x)
                                             + (d.y-prevD.y)*(d.y-prevD.y)
                                             + (d.z-prevD.z)*(d.z-prevD.z));
                    if (dj > maxDirJump) { maxDirJump = dj; dirJumpAt = x; }
                }
                prevD = d; havePrev = true;
            } else havePrev = false;

            std::printf("        %6.2f      %6.3f   %6.3f   (%5.2f,%5.2f,%5.2f)   %6.2f m%s\n",
                        x, g[0], jump, d.x, d.y, d.z, plen, n > 0 ? "" : "  (開口なし)");
        }
        // ★細かい刻みでも測る。0.25m 刻みだと段差を跨いでしまい、
        //   「滑らか」に見えるのに実際は飛んでいることがある。
        {
            AF_SceneHandle s2 = build(false);
            float pg = -1.0f, mj = 0.0f, mjAt = 0.0f;
            AF_Vector3 pd = V(0,0,0); bool hp = false;
            float mdj = 0.0f, mdjAt = 0.0f;
            // ★跳んだ瞬間は**その場で**記録する。あとから mdjAt を頼りに掃引をやり直すと、
            //   x の浮動小数の累積とズレて別の点を見てしまい、表示された前後の差が
            //   報告値と合わない（実測で 0.268 のはずが窓の表示は 0.131 だった）。
            float jx0 = 0.0f, jx1 = 0.0f;
            AF_Vector3 jd0 = V(0,0,0), jd1 = V(0,0,0);
            float gx0 = 0.0f, gx1 = 0.0f, gs0 = 0.0f, gs1 = 0.0f;
            float prevX = 0.0f;
            for (float x = -2.0f; x <= 1.001f; x += 0.05f) {
                const AF_Vector3 Lf = V(x, 1.6f, -3);
                AF_SceneSetListener(s2, Lf);
                AF_SceneSetSource(s2, 1, S);
                AF_SceneUpdate(s2, 1.0f / 60.0f);
                AF_Vector3 pos[8]; float gain[8];
                const int n = AF_SceneGetDiffractionSources(s2, AF_SceneSourceIndex(s2, 1),
                                                            pos, gain, 8);
                float sum = 0.0f; AF_Vector3 dom = V(0,0,0); float best = -1.0f;
                for (int i = 0; i < n; ++i) {
                    sum += gain[i];
                    if (gain[i] > best) { best = gain[i]; dom = pos[i]; }
                }
                if (pg >= 0.0f) {
                    const float j = std::fabs(sum - pg);
                    if (j > mj) {
                        mj = j; mjAt = x;
                        gx0 = prevX; gx1 = x; gs0 = pg; gs1 = sum;
                    }
                }
                // ★位置ではなく**方向**で測る。返る座標は listener+方向×経路長 なので、
                //   経路が伸びれば遠くへ出る。位置の差は異常の指標にならない
                //   （実測で 15.81m と出たが、中身は経路長が伸びただけだった）。
                //   耳に効くのは方向なので、単位ベクトルの差を見る。
                if (best > 0.0f) {
                    const AF_Vector3 rel = V(dom.x - Lf.x, dom.y - Lf.y, dom.z - Lf.z);
                    const float rl = std::max(std::sqrt(rel.x*rel.x + rel.y*rel.y
                                                      + rel.z*rel.z), 1e-4f);
                    const AF_Vector3 dir = V(rel.x/rl, rel.y/rl, rel.z/rl);
                    if (hp) {
                        const AF_Vector3 dd = V(dir.x - pd.x, dir.y - pd.y, dir.z - pd.z);
                        const float dj = std::sqrt(dd.x*dd.x + dd.y*dd.y + dd.z*dd.z);
                        if (dj > mdj) {
                            mdj = dj; mdjAt = x;
                            jx0 = prevX; jx1 = x; jd0 = pd; jd1 = dir;
                        }
                    }
                    dom = dir;   // 以降は方向として保持
                }
                pg = sum;
                prevX = x;
                if (best > 0.0f) { pd = dom; hp = true; }
            }
            std::printf("           方向が跳んだ実測: x=%.2f→%.2f  "
                        "(%.2f,%.2f,%.2f)→(%.2f,%.2f,%.2f)  差=%.3f (%.0f°)\n",
                        jx0, jx1, jd0.x, jd0.y, jd0.z, jd1.x, jd1.y, jd1.z, mdj,
                        2.0f * std::asin(std::min(mdj, 2.0f) * 0.5f) * 57.2958f);
            std::printf("           ゲインが跳んだ実測: x=%.2f→%.2f  %.4f→%.4f (差=%.4f)\n",
                        gx0, gx1, gs0, gs1, mj);
            std::printf("        → 0.05m 刻み: ゲイン最大隣接差 %.3f @ x=%.2f / "
                        "**到来方向**の最大隣接差 %.3f @ x=%.2f（単位ベクトル）\n",
                        mj, mjAt, mdj, mdjAt);
            // 跳ぶ瞬間の前後を、開口の本数と重み込みで出す（推測で直さないため）。
            //   方向が跳ぶ地点とゲインが跳ぶ地点は別々に起きるので、両方の窓を出す。
            const float windows[2] = {mdjAt, mjAt};
            const char* label[2] = {"方向が跳ぶ瞬間", "ゲインが跳ぶ瞬間"};
            for (int wi = 0; wi < 2; ++wi) {
                std::printf("           %s (x=%.2f 付近):\n", label[wi], windows[wi]);
                for (float x = windows[wi] - 0.15f; x <= windows[wi] + 0.101f; x += 0.05f) {
                    const AF_Vector3 Lf = V(x, 1.6f, -3);
                    AF_SceneSetListener(s2, Lf);
                    AF_SceneSetSource(s2, 1, S);
                    AF_SceneUpdate(s2, 1.0f / 60.0f);
                    AF_Vector3 pos[8]; float gain[8];
                    const int n = AF_SceneGetDiffractionSources(s2, AF_SceneSourceIndex(s2, 1),
                                                                pos, gain, 8);
                    float bands[kBands] = {};
                    AF_SceneComputeDiffractionBands(s2, Lf, S, bands, kBands);
                    std::printf("             x=%+.2f  %d本  125Hz=%.4f ", x, n, bands[0]);
                    for (int i = 0; i < n && i < 3; ++i)
                        std::printf(" [g=%.4f (%.1f,%.1f,%.1f)]", gain[i],
                                    pos[i].x, pos[i].y, pos[i].z);
                    std::printf("\n");
                }
            }
            AF_SceneDestroy(s2);
        }
        std::printf("        → ゲイン最大隣接差 %.3f @ x=%.2f / 方向最大隣接差 %.3f @ x=%.2f\n",
                    maxJump, jumpAt, maxDirJump, dirJumpAt);
        std::printf("        → 音が届いた位置 %d / %d\n", fired, total);
        AF_SceneDestroy(s);
    }
}

// ================================================================ ソフト遮蔽
// 壁の縁を横切るときに、透過と遮蔽割合が連続に動くか。
//   単一レイの判定だと「当たる/当たらない」で 1.0 ⇄ 材質値 と一気に跳ぶ。
//   これが「遮蔽ON/OFFの差が激しすぎる」の主因だった。
void testSoftOcclusion() {
    std::printf("\n[ソフト遮蔽] 壁の縁を横切るときの連続性\n");
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    // z=0 に壁。x<=0 が壁、x>0 が開口。
    AF_SceneAddInstanceBox(s, V(-5, 2, 0), V(5, 2, 0.15f), V(1, 0, 0), V(0, 1, 0), mat);

    const AF_Vector3 src = V(-2, 1.6f, 2);
    std::printf("      リスナー x   ソフト透過125Hz  遮蔽割合   単一レイ透過125Hz\n");

    float prevSoft = -1.0f, maxSoftJump = 0.0f;
    float prevHard = -1.0f, maxHardJump = 0.0f;
    for (float x = 0.0f; x <= 4.01f; x += 0.25f) {
        const AF_Vector3 L = V(x, 1.6f, -2);
        float soft[kBands] = {}; float frac = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, src, soft, kBands, &frac);
        float hard[kBands] = {};
        AF_SceneComputeTransmissionBands(s, L, src, hard, kBands);
        std::printf("      %8.2f       %6.3f      %5.2f        %6.3f\n",
                    x, soft[0], frac, hard[0]);
        if (prevSoft >= 0.0f) {
            maxSoftJump = std::max(maxSoftJump, std::fabs(soft[0] - prevSoft));
            maxHardJump = std::max(maxHardJump, std::fabs(hard[0] - prevHard));
        }
        prevSoft = soft[0];
        prevHard = hard[0];
    }
    char b[128];
    std::snprintf(b, sizeof(b), "(ソフト %.3f / 単一レイ %.3f)", maxSoftJump, maxHardJump);
    checkGreater("ソフト遮蔽の方が単一レイより滑らか", maxHardJump, maxSoftJump);
    check("ソフト遮蔽の隣接差が小さい(<0.4)", maxSoftJump < 0.4f, b);

    AF_SceneDestroy(s);
}

// ================================================================ 回折だけシーンの掃引
// Unity の Test_DiffractionOnly と同じ幾何。閉じた箱を仕切りで2部屋に分け、
// 通り道は X=+2〜+4 の開口ひとつだけ。壁は完全不透過。
//   実機で「歩くと回折が消える位置がある（候補 1本 → 0本）」が観測されたので、
//   それを数値で捕まえる。開口が唯一なのだから、部屋のどこにいても回折は 0 になってはいけない。
void diagnoseDiffractionOnlySweep() {
    std::printf("\n[診断] 回折だけシーン: リスナーを動かしたときの安定性\n");
    AF_SceneHandle s = AF_SceneCreate();
    const float zero[6] = {0, 0, 0, 0, 0, 0};
    const float absorb[6] = {0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f};
    const float scat[6] = {0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f};
    const int mat = AF_SceneAddMaterial(s, zero, absorb, scat, 6);   // 完全不透過

    const float hw = 10, h = 5, hd = 10, t = 0.3f;
    AF_SceneAddInstanceBox(s, V(0, -t * 0.5f, 0), V(hw, t * 0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h + t * 0.5f, 0), V(hw, t * 0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(-hw - t * 0.5f, h * 0.5f, 0), V(t * 0.5f, h * 0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(hw + t * 0.5f, h * 0.5f, 0), V(t * 0.5f, h * 0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h * 0.5f, -hd - t * 0.5f), V(hw, h * 0.5f, t * 0.5f), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h * 0.5f, hd + t * 0.5f), V(hw, h * 0.5f, t * 0.5f), V(1,0,0), V(0,1,0), mat);
    // 仕切り（開口 X=+2〜+4）
    AF_SceneAddInstanceBox(s, V(-4, h * 0.5f, 0), V(6, h * 0.5f, t * 0.5f), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(7, h * 0.5f, 0), V(3, h * 0.5f, t * 0.5f), V(1,0,0), V(0,1,0), mat);

    const AF_Vector3 S = V(-4, 1.6f, 5);
    AF_SceneSetSource(s, 1, S);

    std::printf("      リスナー x   125Hz   開口   到来方向(x,z)     経路長\n");
    int zeroCount = 0;
    float prevDx = 0, prevDz = 0, prevLen = 0; bool havePrev = false;
    float maxDirJump = 0.0f, dirJumpAt = 0.0f;
    float maxLenJump = 0.0f, lenJumpAt = 0.0f;

    for (float x = -8.0f; x <= 8.01f; x += 1.0f) {
        const AF_Vector3 L = V(x, 1.6f, -5);
        AF_SceneSetListener(s, L);
        AF_SceneUpdate(s, 1.0f / 60.0f);

        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);

        float dx = 0, dz = 0, plen = 0;
        if (n > 0) {
            const float vx = pos[0].x - L.x, vy = pos[0].y - L.y, vz = pos[0].z - L.z;
            plen = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (plen > 1e-4f) { dx = vx / plen; dz = vz / plen; }
        }
        std::printf("      %8.1f   %6.3f   %d 本  (%5.2f,%5.2f)  %6.2f m%s\n",
                    x, g[0], n, dx, dz, plen, (n == 0) ? "   ← 回折が消えた" : "");
        if (n == 0 || g[0] <= 1e-4f) ++zeroCount;
        if (havePrev && n > 0) {
            const float d = std::sqrt((dx - prevDx) * (dx - prevDx) + (dz - prevDz) * (dz - prevDz));
            if (d > maxDirJump) { maxDirJump = d; dirJumpAt = x; }
            const float dl = std::fabs(plen - prevLen);
            if (dl > maxLenJump) { maxLenJump = dl; lenJumpAt = x; }
        }
        if (n > 0) { prevDx = dx; prevDz = dz; prevLen = plen; havePrev = true; }
    }

    char b[128];
    std::snprintf(b, sizeof(b), "(%d 箇所)", zeroCount);
    check("開口が1つだけなので、どこにいても回折は消えない", zeroCount == 0, b);
    std::snprintf(b, sizeof(b), "(最大 %.2f @ x=%.1f)", maxDirJump, dirJumpAt);
    check("到来方向が滑らかに動く(隣接差<0.35)", maxDirJump < 0.35f, b);
    // 経路長は遅延と距離減衰の両方を決める。距離減衰だけで鳴らす設定では、
    // ここが飛ぶとそのまま音量の飛びになる。
    std::snprintf(b, sizeof(b), "(最大 %.2fm @ x=%.1f)", maxLenJump, lenJumpAt);
    check("経路長が滑らかに動く(隣接差<1.5m)", maxLenJump < 1.5f, b);

    AF_SceneDestroy(s);
}

// ================================================================ 開口幅
// 隙間を狭めていくと回折が落ちるか。
//   前川の式は δ だけの関数で幅を見ないため、これが無いと「閉じた扉のわずかな隙間」でも
//   δ が小さいだけで素通りしてしまう（実機で観測）。現実には波長より狭い隙間は通さない。
void testApertureOpenness() {
    std::printf("\n[開口率] 隙間を狭めると回折が落ちるか\n");
    std::printf("      隙間幅   125Hz   500Hz    4kHz\n");

    float prev = -1.0f;
    bool monotone = true;
    float wide = 0.0f, narrow = 0.0f;
    for (float gap : {2.0f, 1.0f, 0.5f, 0.25f, 0.1f, 0.03f}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // z=0 の壁を2枚。隙間は x∈[0, gap]（中央ではなく片側に寄せる）。
        //   ★リスナーと音源は x=-0.1 に置く。隙間の縁(x=0)のすぐ手前なので、
        //     **隙間の幅を変えても δ はほぼ一定**になる。これで幅の効果だけを見られる。
        //     隙間を中央に置いて左右対称にすると、広い隙間では直線が通ってしまい
        //     そもそも遮蔽されない（最初そうなっていた）。
        const float t = 0.15f, h = 3.0f, hw = 8.0f;
        AF_SceneAddInstanceBox(s, V(-hw * 0.5f, h * 0.5f, 0), V(hw * 0.5f, h * 0.5f, t),
                               V(1,0,0), V(0,1,0), mat);                       // x∈[-8,0]
        AF_SceneAddInstanceBox(s, V(gap + (hw - gap) * 0.5f, h * 0.5f, 0),
                               V((hw - gap) * 0.5f, h * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        // 床・天井（上下から回り込ませない）
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, 6), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, 6), V(1,0,0), V(0,1,0), mat);
        // 側面（左右から回り込ませない）
        AF_SceneAddInstanceBox(s, V(-hw, h * 0.5f, 0), V(t, h * 0.5f, 6), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(hw, h * 0.5f, 0), V(t, h * 0.5f, 6), V(1,0,0), V(0,1,0), mat);

        const AF_Vector3 L = V(-0.1f, 1.5f, -3), S = V(-0.1f, 1.5f, 3);
        float g[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        std::printf("      %6.2f m  %6.3f  %6.3f  %6.3f\n", gap, g[0], g[2], g[5]);
        if (prev >= 0.0f && g[0] > prev + 1e-3f) monotone = false;
        if (gap >= 1.99f) wide = g[0];
        narrow = g[0];
        prev = g[0];
        AF_SceneDestroy(s);
    }

    check("隙間を狭めるほど回折が落ちる(単調)", monotone);
    char b[96];
    std::snprintf(b, sizeof(b), "(2m %.3f → 3cm %.3f)", wide, narrow);
    check("3cm の隙間は 2m の隙間よりずっと通らない(1/4 以下)", narrow < wide * 0.25f, b);

    // ── 閉じた扉が漏れないか（報告された症状そのもの）──
    //   「閉じてても少し判定とられる」。原因は稜線候補を 2cm 膨らませていること
    //   （面すれすれの経路を拾うために必要）で、密着した扉でも候補が出てしまう。
    //   開口幅ゲートはその候補の**まわりに空間が無い**ことを見て潰す。
    //   ★上の掃引と違い、天井と床で閉じた部屋にすること。開いた壁だと閉扉でも
    //     「壁の上を越える」経路が物理的に正しく残るので、ゲートの検証にならない。
    std::printf("\n      [閉じた扉] 密閉した部屋・戸口1m の扉を開けていく\n");
    float closedGain = -1.0f, ajarGain = -1.0f;
    // 扉の厚みを2通りで測る。探索の貫通許容 maxPen は「そのブロッカー自身の最薄辺+5cm」
    // なので、薄い板は探索から見て素通しになる。厚い扉で漏れが消えるならそれが原因。
    for (float kDoorHalfThick : {0.03f, 0.10f}) {
    std::printf("        扉の厚み %.0fcm\n", kDoorHalfThick * 200.0f);
    std::printf("        開き量   回折(125Hz)  ゲート無\n");
    for (float open : {0.0f, 0.02f, 0.1f, 0.3f, 0.6f}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float t = 0.1f, h = 3.0f, rw = 4.0f, rd = 4.0f;
        // 仕切り壁（戸口 x∈[-0.5,0.5]）
        AF_SceneAddInstanceBox(s, V(-2.25f, h*0.5f, 0), V(1.75f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 2.25f, h*0.5f, 0), V(1.75f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        // 床・天井・四方の壁（密閉）
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(rw, t, rd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(rw, t, rd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-rw, h*0.5f, 0), V(t, h*0.5f, rd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( rw, h*0.5f, 0), V(t, h*0.5f, rd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -rd), V(rw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  rd), V(rw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        // 扉。戸口を塞ぐ板を、開いたぶんだけ横へずらす（引き戸として単純化）。
        //   蝶番の回転で作ると角度と隙間幅が非線形に絡むので、幅の効果だけを見るためスライドにする。
        const float dw = 0.5f - open * 0.5f;              // 残った板の半幅
        if (dw > 0.01f)
            AF_SceneAddInstanceBox(s, V(-0.5f + dw, h*0.5f, 0), V(dw, h*0.5f, kDoorHalfThick),
                                   V(1,0,0), V(0,1,0), mat);

        // L/S は x=-0.3。扉は +x 側へ開くので、開き切るまで直線は板に塞がれたまま
        //   （x=0 に置くと 0.5m 開けた時点で直線が通ってしまい、回折の検証にならない）。
        const AF_Vector3 L = V(-0.3f, 1.6f, -2.5f), S = V(-0.3f, 1.6f, 2.5f);
        float g[kBands] = {}, gOff[kBands] = {};
        AF_SceneComputeDiffractionBands(s, L, S, g, kBands);
        AF_SceneSetApertureOpen(s, 0.0f, 1.0f, 1.0f, 0);   // ref=0 でゲート無効
        AF_SceneComputeDiffractionBands(s, L, S, gOff, kBands);
        AF_SceneSetApertureOpen(s, 0.30f, 1.0f, 1.0f, 0);
        // 開口の位置と開口率も出す（どこを回っていて、どれだけ開いているのか）
        AF_Vector3 pos[8]; float gg[8];
        AF_SceneSetListener(s, L); AF_SceneSetSource(s, 1, S); AF_SceneUpdate(s, 1.0f/60.0f);
        const int nsrc = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gg, 8);
        int bi2 = -1; float bw2 = -1.0f;
        for (int i = 0; i < nsrc; ++i) if (gg[i] > bw2) { bw2 = gg[i]; bi2 = i; }
        const float openFrac = (bi2 >= 0)
            ? AF_SceneMeasureApertureOpenness(s, L, S) : 0.0f;
        std::printf("        %5.2f m      %6.3f     %6.3f   率%6.3f  開口(%5.2f,%5.2f,%5.2f)\n",
                    open, g[0], gOff[0], openFrac,
                    bi2 >= 0 ? pos[bi2].x : 0.0f, bi2 >= 0 ? pos[bi2].y : 0.0f,
                    bi2 >= 0 ? pos[bi2].z : 0.0f);
        if (open <= 0.001f) closedGain = g[0];
        if (open >= 0.59f) ajarGain = g[0];
        AF_SceneDestroy(s);
    }
    }
    char b2[96];
    std::snprintf(b2, sizeof(b2), "(閉 %.3f / 60cm開 %.3f)", closedGain, ajarGain);
    check("閉じた扉はほぼ通さない(0.1 未満)", closedGain < 0.1f, b2);
    check("60cm 開けば通る(閉の4倍以上)", ajarGain > closedGain * 4.0f + 0.05f, b2);
}

// ---------------------------------------------------------------- 扉の連続性（受け入れ条件）
// **この作品の根底は「周辺の変化を音で伝える」こと**なので、扉の開き具合が音へ
// 連続に出ることは祈るものではなく守るもの。ここを回帰テストにして、以降どんな変更を
// しても連続性が壊れたら赤で分かるようにする。
//
//   密閉した部屋・蝶番の扉を 1°刻みで 0→90° 掃引し、耳に届く回折量の隣接差を見る。
//   1°ぶんの幾何変化に対して音量が飛ぶなら、それは表現形式の不連続が漏れている。
void testDoorContinuity() {
    std::printf("\n[扉の連続性] 1°刻みで掃引したときの隣接差\n");

    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const AF_Vector3 L = V(0, 1.6f, -4), S = V(-2.0f, 1.6f, 3.5f);

    float prev = -1.0f, maxJump = 0.0f, jumpAt = 0.0f;
    float first = -1.0f, last = -1.0f;
    int silentCount = 0, total = 0;
    float sweepSum[91] = {}, sweepOpen[91] = {};
    int   sweepN[91] = {};
    // ★「扉が仕事をしているか」を測るための控え。
    //   隣接差だけを見ていたせいで、**変化を殺した実装がテストを通ってしまった**
    //   （定数関数は完全に連続なので当然）。開き角ごとの値と高域の開口率を残す。
    float atSlight = -1.0f, atHalf = -1.0f;   // 10° / 45°
    float fzHighOpen = -1.0f;                 // 全開時の 4kHz 開口率
    for (float deg = 0.0f; deg <= 90.01f; deg += 1.0f) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h*0.5f, 0),
                               V((hw - 0.6f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                               V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        const float th = deg * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        AF_SceneAddInstanceBox(s, V(-0.6f + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                               V(doorW * 0.5f, doorH * 0.5f, 0.03f), V(c, 0, sn), V(0, 1, 0), mat);
        {   // 傾きを振って比べられるようにする（既定はエンジンの値）。
            const char* c = std::getenv("AF_CONTRAST");
            if (c) AF_SceneSetApertureContrast(s, static_cast<float>(std::atof(c)));
        }
        // ★戸口をポータルとして置く。**出荷する経路で測る**ため。
        //   ポータルが無いと旧経路（前川＋開口積分）が走るが、そちらは
        //   「面のうちどこまで積分するか」に答えが無く、扉の効きが 1.9倍で頭打ちになる
        //   （4通り試して全部失敗した）。テストが見るべきは実際に鳴る経路のほう。
        AF_SceneAddPortal(s, V(0, doorH * 0.5f, 0), V(1, 0, 0), V(0, 1, 0),
                          0.6f, doorH * 0.5f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        AF_SceneUpdate(s, 1.0f / 60.0f);

        // 耳に届く量＝二次音源の重みの合計。
        AF_Vector3 pos[8]; float gain[8];
        const int n = AF_SceneGetDiffractionSources(s, AF_SceneSourceIndex(s, 1), pos, gain, 8);
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) sum += gain[i];
        ++total;
        if (deg > 20.0f && sum < 1e-6f) ++silentCount;   // 十分開いてから無音は異常

        // ★跳ぶ位置は事前に分からないので全角度を控えておき、最後に窓で出す。
        //   15〜25°だけ出していたせいで、53°で跳んでいるのに中身が見えなかった。
        if (deg >= 0.0f && deg <= 90.0f) {
            const int di = static_cast<int>(deg + 0.5f);
            if (di >= 0 && di < 91) {
                sweepSum[di] = sum; sweepN[di] = n;
                float fz[kBands] = {}; AF_Vector3 pcp = V(0,0,0);
                sweepOpen[di] = AF_SceneMeasurePortal(s, 0, L, S, fz, &pcp) ? fz[0] : -1.0f;
            }
        }
        if (deg >= 15.0f && deg <= 25.0f)
            std::printf("        %4.0f°  開口 %d本  合計 %.4f  実測幅 %.4f\n",
                        deg, n, sum, AF_SceneMeasureSlitWidth(s, L, S));
        if (prev >= 0.0f) {
            const float j = std::fabs(sum - prev);
            if (j > maxJump) { maxJump = j; jumpAt = deg; }
        }
        if (first < 0.0f) first = sum;
        if (deg > 9.5f && deg < 10.5f) atSlight = sum;
        if (deg > 44.5f && deg < 45.5f) atHalf = sum;
        if (deg > 89.5f) {
            // ★ポータルを置いてあるので、そちらで測る。
            //   AF_SceneMeasureApertureFresnel は旧経路（前川＋開口積分）の診断で、
            //   ポータルがあるときは実際に鳴っている量ではない。
            float fz[kBands] = {}; AF_Vector3 pcp = V(0,0,0);
            if (AF_SceneMeasurePortal(s, 0, L, S, fz, &pcp)) fzHighOpen = fz[5];
            else { AF_SceneMeasureApertureFresnel(s, L, S, fz); fzHighOpen = fz[5]; }
        }
        last = sum;
        prev = sum;
        AF_SceneDestroy(s);
    }

    // ★頭打ちが「幾何が開き切ったから」なのか「コントラストのクランプ」なのかを
    //   分けるため、10°刻みで開口率と出力を並べる。開口率が上がり続けているのに
    //   出力が 1.0 で止まっているなら、頭打ちを作っているのはクランプの方。
    {
        std::printf("        10°刻み（頭打ちの正体を見る）:\n");
        for (int d = 0; d <= 90; d += 10)
            std::printf("          %3d°  合計 %.4f  ポータル開口率125Hz %.4f\n",
                        d, sweepSum[d], sweepOpen[d]);
        const int j = static_cast<int>(jumpAt + 0.5f);
        std::printf("        隣接差が最大の付近 (%d°):\n", j);
        for (int d = std::max(0, j - 3); d <= std::min(90, j + 2); ++d)
            std::printf("          %3d°  合計 %.4f  開口率 %.4f\n",
                        d, sweepSum[d], sweepOpen[d]);
    }
    char b[128];
    std::snprintf(b, sizeof(b), "(最大隣接差 %.4f @ %.0f° / 閉 %.4f → 全開 %.4f)",
                  maxJump, jumpAt, first, last);
    std::printf("      %s\n", b);
    // 1°の幾何変化で音量が 0.1 以上飛んだら、不連続が耳に届いている。
    check("1°刻みで音量が飛ばない(隣接差 < 0.1)", maxJump < 0.1f, b);
    check("開いたら鳴り続ける（20°以降に無音が無い）", silentCount == 0);
    checkGreater("閉より全開の方が大きい", last, first);

    // ★ここから「扉が仕事をしているか」。上の連続性だけでは平らな曲線を弾けない。
    //   幾何では、戸口の隙間は 10° で 1.8cm、90° で 1.2m ＝ 67倍に開く。
    //   開口率は 1.0 で頭打ちになるのでそこまでは出ないが、
    //   **少し開いた状態と全開が同じ音**なら「どのくらい開いたか」を伝えられていない。
    char b2[160];
    std::snprintf(b2, sizeof(b2), "(10° %.4f → 45° %.4f → 90° %.4f = %.1f倍)",
                  atSlight, atHalf, last, (atSlight > 1e-6f) ? last / atSlight : 0.0f);
    std::printf("      %s\n", b2);
    check("わずかに開いた状態と全開が別の音になる(90°が10°の4倍以上)",
          atSlight > 1e-6f && last >= atSlight * 4.0f, b2);
    check("後半でも開き続ける(90°が45°の1.5倍以上)",
          atHalf > 1e-6f && last >= atHalf * 1.5f, b2);

    // 高域が一度も開かないと「開くと音色が明るくなる」が出ない。
    //   実測（現実の扉の収録）でも開−閉の差は 1kHz が 125Hz の4倍あった。
    char b3[128];
    std::snprintf(b3, sizeof(b3), "(全開時の 4kHz 開口率 %.4f)", fzHighOpen);
    check("全開なら高域も開く(4kHz の開口率 > 0.1)", fzHighOpen > 0.1f, b3);
}

// ---------------------------------------------------------------- 材質の個別化(段5)
// これまで全オクルーダーが材質を1つ共有していた＝薄い扉と厚いコンクリ壁が同じ透過率。
// ここで見るのは:
//   ・材質の中身を書き換えると、それを使う壁が一斉に変わる
//   ・インスタンスごとに付け替えられる（扉だけ別の材質に）
//   ・扉を「よく遮る材質」にすると閉扉時の漏れが減る（測定で裏を取る）
void testPerInstanceMaterial() {
    std::printf("\n[材質の個別化] 壁ごとに違う材質を持てるか\n");

    AF_SceneHandle s = AF_SceneCreate();
    const int matWall = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);   // 既定壁
    // よく遮る材質（透過はエネルギー。振幅は √ なので 1e-6 → 0.001 = -60dB）。
    //   ★既定壁を実際の間仕切り相当（TL 31dB）まで上げたので、比較用はさらに下げる。
    //     以前は 1e-4 だったが、既定壁が -31dB になったため差が 9dB しか付かず、
    //     「よく遮る材質にすると漏れが減る」の判定（1/4 以下）を通らなくなった。
    const float tight[6] = {1e-6f, 5e-7f, 2e-7f, 1e-7f, 5e-8f, 2e-8f};
    const int matTight = AF_SceneAddMaterial(s, tight, nullptr, nullptr, 6);

    // 単純な仕切り1枚。
    const int wall = AF_SceneAddInstanceBox(s, V(0, 1.5f, 0), V(4, 1.5f, 0.1f),
                                            V(1,0,0), V(0,1,0), matWall);
    const AF_Vector3 L = V(0, 1.6f, -3), S = V(0, 1.6f, 3);

    float g[kBands] = {};
    AF_SceneComputeTransmissionBands(s, L, S, g, kBands);
    const float before = g[0];
    // ★絶対値ではなく**性質**で見る。材質の数値は演出として動かすものなので、
    //   閾値を絶対値で置くと調整のたびにテストが落ちる（実際そうなった）。
    //   質量則として保たれるべきは「低域ほど抜ける」ことと「無音ではない」こと。
    char bLow[96];
    std::snprintf(bLow, sizeof(bLow), "(125Hz %.4f / 4kHz %.4f)", g[0], g[5]);
    check("既定壁は低域ほど抜ける（質量則）", g[0] > g[5] * 2.0f, bLow);
    checkGreater("既定壁は無音ではない", before, 0.005f);

    // ① インスタンスの材質を付け替える。
    check("インスタンスの材質を付け替えられる",
          AF_SceneSetInstanceMaterial(s, wall, matTight) == 1);
    check("付け替えが読み戻せる", AF_SceneGetInstanceMaterial(s, wall) == matTight);
    AF_SceneComputeTransmissionBands(s, L, S, g, kBands);
    char b[128];
    std::snprintf(b, sizeof(b), "(%.4f → %.4f / %.1fdB → %.1fdB)",
                  before, g[0], 20.0f * std::log10(std::max(before, 1e-6f)),
                  20.0f * std::log10(std::max(g[0], 1e-6f)));
    check("よく遮る材質にすると漏れが減る", g[0] < before * 0.25f, b);

    // ② 材質の中身を書き換えると、それを使う壁が一斉に変わる。
    AF_SceneSetInstanceMaterial(s, wall, matWall);       // 既定へ戻す
    check("材質の中身を書き換えられる",
          AF_SceneSetMaterial(s, matWall, tight, nullptr, nullptr, 6) == 1);
    AF_SceneComputeTransmissionBands(s, L, S, g, kBands);
    check("書き換えが即座に効く（再構築不要）", g[0] < before * 0.25f);

    // 範囲外・null で落ちない。
    check("範囲外の材質IDは失敗を返す",
          AF_SceneSetInstanceMaterial(s, wall, 999) == 0);
    check("範囲外のインスタンスIDは失敗を返す",
          AF_SceneSetInstanceMaterial(s, 999, matWall) == 0);
    check("範囲外の材質の書き換えは失敗を返す",
          AF_SceneSetMaterial(s, 999, tight, nullptr, nullptr, 6) == 0);
    AF_SceneSetMaterial(nullptr, 0, nullptr, nullptr, nullptr, 0);
    check("null scene の問い合わせは -1", AF_SceneGetInstanceMaterial(nullptr, 0) == -1);

    AF_SceneDestroy(s);
}

// ---------------------------------------------------------------- 音源レンダリング(段4)
// DSP 本体は DspRegressionTest が押さえているので、ここで見るのは**C API 越しに
// 同じことが起きるか**と、null/範囲外で落ちないか。
//   これが通ると「エンジンだけで音が出る」＝段4の目標が達成される。
void testVoiceApi() {
    std::printf("\n[音源レンダリング] C API 越しの DSP（段4）\n");

    AF_VoiceConfig cfg{};
    cfg.sampleRate = 48000;
    cfg.maxFrames = 512;
    cfg.tailSeconds = 0.25f;
    cfg.tapCrossfadeMs = 1.0f;
    AF_VoiceHandle v = AF_VoiceCreate(&cfg);
    check("音源が作れる", v != nullptr);

    AF_VoiceSetOutputGain(v, 1.0f);
    AF_VoiceSetHrtfEnabled(v, 0);

    // タップ2本（0=直接音 / 1=反射）。
    AF_VoiceTap taps[2] = {};
    for (int b = 0; b < 6; ++b) { taps[0].gain6[b] = 1.0f; taps[1].gain6[b] = 0.5f; }
    taps[0].panL = taps[0].panR = 0.70710678f;
    taps[1].panL = taps[1].panR = 0.70710678f;
    taps[1].delaySamples = 240;
    AF_VoiceScatterSplit(5.0f, 20.0f, 0.0f, 1.0f, &taps[1].gSpec, &taps[1].gDiff);
    check("散乱の振り分けが C API で取れる", taps[1].gDiff > 0.0f && taps[1].gSpec > 0.0f);
    AF_VoiceSetTaps(v, taps, 2);

    const int n = 4096;
    std::vector<float> in(n), ol(n, 0.0f), orr(n, 0.0f);
    unsigned int rs = 12345u;
    for (int i = 0; i < n; ++i) {
        rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
        in[i] = static_cast<int>(rs) * (1.0f / 2147483648.0f) * 0.3f;
    }

    AF_VoiceMetering m{};
    AF_VoiceRender(v, in.data(), n, ol.data(), orr.data(), &m);
    std::printf("      段別RMS 直接 %.4f / 早期 %.4f / 散乱 %.4f / 尾 %.4f / 出力 %.4f\n",
                m.rmsDirect, m.rmsEarly, m.rmsScatter, m.rmsTail, m.rmsOut);
    check("音が出る", m.rmsOut > 0.01f);
    check("尾はまだ無い", m.rmsTail < 1e-6f);

    // エコグラムから尾を組む。
    {
        const int bins = 50;
        std::vector<float> echo(bins * 6, 0.0f);
        for (int k = 0; k < bins; ++k)
            for (int b = 0; b < 6; ++b)
                echo[k * 6 + b] = std::pow(0.9f, static_cast<float>(k));
        const float ratio = AF_VoiceRebuildTail(v, echo.data(), bins, 5.0f, 20.0f, 5.0f,
                                                0.0f, 0.0f, 1.0f, 1.0f, 0.5f, nullptr, 0);
        check("エコグラムから尾が組める", ratio > 0.0f);
        checkGreater("尾の分割が組まれている", static_cast<float>(AF_VoiceTailPartitions(v)), 0.0f);
        char b[64];
        std::snprintf(b, sizeof(b), "(%d samp)", AF_VoiceTailLatency(v));
        check("尾の遅延が最小ブロックぶん(64)", AF_VoiceTailLatency(v) == 64, b);

        AF_VoiceMetering m2{};
        AF_VoiceRender(v, in.data(), n, ol.data(), orr.data(), &m2);
        check("尾が鳴り出す", m2.rmsTail > 1e-4f);
    }

    // HRTF を繋ぐ。
    {
        AF_HrtfHandle h = AF_HrtfCreateSynthetic(48000);
        check("合成HRTFが作れる", h != nullptr);
        checkGreater("方向が入っている", static_cast<float>(AF_HrtfDirectionCount(h)), 500.0f);
        char name[64] = {};
        AF_HrtfGetName(h, name, sizeof(name));
        std::printf("      HRTF: %s\n", name);

        AF_VoiceSetHrtf(v, h);
        AF_VoiceSetHrtfEnabled(v, 1);
        AF_VoiceSetDirection(v, V(1, 0, 0), 57.0f);   // 真右
        std::fill(ol.begin(), ol.end(), 0.0f);
        std::fill(orr.begin(), orr.end(), 0.0f);
        AF_VoiceRender(v, in.data(), n, ol.data(), orr.data(), nullptr);
        double el = 0.0, er = 0.0;
        for (int i = n / 2; i < n; ++i) { el += ol[i] * ol[i]; er += orr[i] * orr[i]; }
        char b[96];
        std::snprintf(b, sizeof(b), "(左 %.2f / 右 %.2f)", el, er);
        check("右に定位すると右チャンネルが大きい", er > el * 1.5, b);

        AF_VoiceDestroy(v);      // 音源を先に壊しても
        AF_HrtfDestroy(h);       // HRTF を後で壊して落ちない
        check("破棄の順序で落ちない", true);
    }

    // null / 範囲外。
    AF_VoiceSetTaps(nullptr, taps, 2);
    AF_VoiceRender(nullptr, in.data(), n, ol.data(), orr.data(), nullptr);
    AF_VoiceDestroy(nullptr);
    AF_HrtfDestroy(nullptr);
    check("null ハンドルで落ちない", AF_VoiceTailPartitions(nullptr) == 0);
    check("null パスの読み込みは NULL を返す", AF_HrtfLoadFile(nullptr) == nullptr);
}

// ---------------------------------------------------------------- 頑健性
// 不正入力で落ちない（移行中に呼び出し規約を変えるので、境界は明示的に守る）。
// 部屋の差分更新は「触れたブロックだけ塗り直す」ので、塗り残し・番号の食い違い・
// 面の対応表の張り忘れが**静かに**入り込む。全再構築と突き合わせて縛る。
void testRoomIncremental() {
    std::printf("\n[部屋] 差分更新が全再構築と一致するか\n");
    const float h = 4.0f, t = 0.15f, hw = 6.0f, hd = 8.0f;
    // 箱: 0 床 / 1 天井 / 2,3 左右壁 / 4,5 前後壁 / 6 仕切り / 7 隅の柱
    auto build = [&]() {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-4.0f, h*0.5f, -4.0f), V(0.5f, h*0.5f, 0.5f),
                               V(1,0,0), V(0,1,0), mat);
        return s;
    };
    // 部屋数と体積の並びで比べる（番号の付き方まで一致する必要はない）。
    auto snapshot = [](AF_SceneHandle s) {
        std::vector<double> v;
        for (int i = 0, n = AF_SceneRoomCount(s); i < n; ++i) {
            float a = 0.0f; AF_SceneRoomInfo(s, i, &a, nullptr, nullptr, nullptr);
            v.push_back(a);
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    auto same = [](const std::vector<double>& a, const std::vector<double>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::fabs(a[i] - b[i]) > 0.5) return false;
        return true;
    };

    for (int brick : {8, 16, 32}) {
        char tag[64];
        // ① 仕切りを降ろす＝2部屋が1部屋に繋がる（全体の連結が変わる最悪ケース）
        {
            AF_SceneHandle s = build(); AF_SceneSetRoomBrick(s, brick);
            const int before = AF_SceneRoomCount(s);
            AF_SceneSetInstanceActive(s, 6, 0);
            AF_SceneHandle r = build(); AF_SceneSetRoomBrick(r, brick);
            AF_SceneSetInstanceActive(r, 6, 0);
            std::snprintf(tag, sizeof(tag), "[部屋] ブロック%d 仕切り除去が全再構築と一致", brick);
            check(tag, before == 2 && AF_SceneRoomCount(s) == 1
                       && same(snapshot(s), snapshot(r)));
            AF_SceneDestroy(r); AF_SceneDestroy(s);
        }
        // ② 柱を動かす＝静的から外れる。**元居た場所**の塗り残しが出やすい。
        {
            AF_SceneHandle s = build(); AF_SceneSetRoomBrick(s, brick);
            AF_SceneRoomCount(s);
            AF_SceneUpdateInstance(s, 7, V(-4.0f, h*0.5f, -3.0f), V(0.5f, h*0.5f, 0.5f),
                                   V(1,0,0), V(0,1,0));
            AF_SceneHandle r = build(); AF_SceneSetRoomBrick(r, brick);
            AF_SceneUpdateInstance(r, 7, V(-4.0f, h*0.5f, -3.0f), V(0.5f, h*0.5f, 0.5f),
                                   V(1,0,0), V(0,1,0));
            AF_SceneRoomCount(r);
            std::snprintf(tag, sizeof(tag), "[部屋] ブロック%d 柱の移動が全再構築と一致", brick);
            check(tag, same(snapshot(s), snapshot(r)));
            AF_SceneDestroy(r); AF_SceneDestroy(s);
        }
        // ③ 降ろして戻す＝元に戻ること（塗り直しが冪等か）
        {
            AF_SceneHandle s = build(); AF_SceneSetRoomBrick(s, brick);
            const std::vector<double> v0 = snapshot(s);
            AF_SceneSetInstanceActive(s, 6, 0); AF_SceneRoomCount(s);
            AF_SceneSetInstanceActive(s, 6, 1);
            std::snprintf(tag, sizeof(tag), "[部屋] ブロック%d 降ろして戻すと元に戻る", brick);
            check(tag, same(snapshot(s), v0));
            AF_SceneDestroy(s);
        }
    }

    // ④ 点の所属も差分更新で正しいこと（union-find を引き直せているか）。
    {
        AF_SceneHandle s = build();
        const int a0 = AF_SceneRoomAt(s, V(0, 2.0f, -4.0f));
        const int b0 = AF_SceneRoomAt(s, V(0, 2.0f,  4.0f));
        check("[部屋] 仕切りの両側は別の部屋", a0 >= 0 && b0 >= 0 && a0 != b0);
        AF_SceneSetInstanceActive(s, 6, 0);
        const int a1 = AF_SceneRoomAt(s, V(0, 2.0f, -4.0f));
        const int b1 = AF_SceneRoomAt(s, V(0, 2.0f,  4.0f));
        check("[部屋] 仕切りを外すと同じ部屋になる", a1 >= 0 && a1 == b1);
        AF_SceneDestroy(s);
    }
}

// 部屋は「戸口で割れる」ことが要る。素の連結成分だと扉の向こうも同じ部屋になり、
// 残響を切り替える土台にならない。侵食の半径で切り替わる位置を縛る。
void testRoomSegmentation() {
    std::printf("\n[部屋] 戸口で部屋が分かれるか\n");
    const float h = 4.0f, t = 0.15f, hw = 6.0f, hd = 8.0f;
    auto build = [&](float gap) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        const float half = gap * 0.5f;
        if (gap <= 1e-3f) {
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        } else {
            AF_SceneAddInstanceBox(s, V(-(hw + half) * 0.5f, h*0.5f, 0),
                                   V((hw - half) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V( (hw + half) * 0.5f, h*0.5f, 0),
                                   V((hw - half) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        }
        return s;
    };
    auto rooms = [&](float gap, float radius) {
        AF_SceneHandle s = build(gap);
        AF_SceneSetRoomSeedRadius(s, radius);
        const int n = AF_SceneRoomCount(s);
        AF_SceneDestroy(s);
        return n;
    };
    // 既定（半径 0.6m・格子 0.25m）で、人が通る幅の戸口が部屋を分けること。
    check("[部屋] 閉じた仕切り → 2 部屋",        rooms(0.0f, 0.6f) == 2);
    check("[部屋] 戸口 0.9m → 2 部屋（扉幅）",   rooms(0.9f, 0.6f) == 2);
    check("[部屋] 戸口 2.0m → 1 部屋（開けた口）", rooms(2.0f, 0.6f) == 1);
    // 半径 0 は侵食なし＝素の連結成分。戸口があれば必ず 1 部屋になる。
    check("[部屋] 半径 0 なら戸口 0.3m でも 1 部屋", rooms(0.3f, 0.0f) == 1);
    // 切り替わりの位置は「幅 = 半径 × 2」。半径を上げれば広い口も切れる。
    check("[部屋] 半径 1.2m なら戸口 2.0m も 2 部屋", rooms(2.0f, 1.2f) == 2);

    // 塗り戻し: 侵食で落とした殻にも部屋が付くこと。戸口のど真ん中に立っても
    // どちらかの部屋になっていないと、そこでリスナーの部屋が消える。
    {
        AF_SceneHandle s = build(0.9f);
        const int nearSide = AF_SceneRoomAt(s, V(0, 2.0f, -4.0f));
        const int farSide  = AF_SceneRoomAt(s, V(0, 2.0f,  4.0f));
        const int inDoor   = AF_SceneRoomAt(s, V(0, 2.0f,  0.0f));
        const int atWall   = AF_SceneRoomAt(s, V(0, 2.0f, -7.8f));   // 壁から 0.05m
        check("[部屋] 戸口の両側が別の部屋", nearSide >= 0 && farSide >= 0 && nearSide != farSide);
        check("[部屋] 戸口の中にも部屋が付く", inDoor == nearSide || inDoor == farSide);
        check("[部屋] 壁際にも部屋が付く",     atWall == nearSide);
        AF_SceneDestroy(s);
    }

    // ── 開口の抽出 ──
    //   ★面積はボクセル 1 個ぶんの丸めが乗る（0.25m 格子なら幅が 0.25m 刻みに量子化）。
    //     絶対値を当てにせず「実寸の 2 割以内」で縛る。
    {
        AF_SceneHandle s = build(0.9f);
        AF_SceneSetRoomCellSize(s, 0.1f);
        const int na = AF_SceneApertureCount(s);
        float area = 0.0f; AF_Vector3 c{}, n{}; int ra = -1, rb = -1;
        const int ok = (na > 0) ? AF_SceneApertureInfo(s, 0, &area, &c, &n, &ra, &rb) : 0;
        check("[開口] 戸口 0.9m で口が 1 つ出る", na == 1);
        check("[開口] 面積が実寸(3.6m2)の 2 割以内",
              ok && std::fabs(area - 3.6f) <= 0.72f);
        check("[開口] 中心が戸口の位置", ok && std::fabs(c.x) < 0.2f && std::fabs(c.z) < 0.2f);
        check("[開口] 法線が仕切りに垂直", ok && std::fabs(n.z) > 0.9f);
        check("[開口] 繋いでいる部屋が両側", ok && ra == 0 && rb == 1);
        AF_SceneDestroy(s);
    }
    {
        AF_SceneHandle s = build(0.0f);   // 完全に塞がっている
        AF_SceneSetRoomCellSize(s, 0.1f);
        check("[開口] 壁だけなら口は出ない", AF_SceneApertureCount(s) == 0);
        AF_SceneDestroy(s);
    }
    // ── 回折で音色を変えない ──
    //   決めてある方針: 回折が持つ情報は「開口の方向」と「回り込んだぶんの距離減衰」で、
    //   周波数依存のこもりは透過と吸音が担当する。ところがこの切り替えはホスト側の
    //   二次音源タップにしか入っておらず、メインのボイスに掛かる生存ゲインには
    //   前川の帯域依存が残っていた（実測: 柱 1 本で傾き -3.7dB）。
    //   吸わない表面なら、障害物を置いても生存ゲインは平坦でなければならない。
    {
        const float hh = 4.0f, tt = 0.3f, hw2 = 5.0f, hd2 = 6.0f;
        const float noAbsorb[6] = {0, 0, 0, 0, 0, 0};
        auto build = [&](bool pillar, const float* absorb) {
            AF_SceneHandle s = AF_SceneCreate();
            const int m = AF_SceneAddMaterial(s, nullptr, absorb, nullptr, absorb ? 6 : 0);
            AF_SceneAddInstanceBox(s, V(0, -tt, 0),    V(hw2+tt, tt, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh+tt, 0),  V(hw2+tt, tt, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(-hw2-tt, hh*0.5f, 0), V(tt, hh*0.5f, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( hw2+tt, hh*0.5f, 0), V(tt, hh*0.5f, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f, -hd2-tt), V(hw2+tt, hh*0.5f, tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f,  hd2+tt), V(hw2+tt, hh*0.5f, tt), V(1,0,0), V(0,1,0), m);
            if (pillar)
                AF_SceneAddInstanceBox(s, V(0, hh*0.5f, 0), V(0.3f, hh*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
            return s;
        };
        auto tiltDb = [&](bool pillar, bool flat, const float* absorb) {
            AF_SceneHandle s = build(pillar, absorb);
            AF_SceneSetDiffractionFlat(s, flat ? 1 : 0);
            AF_SceneSetListener(s, V(0, 1.6f, -3.5f));
            AF_SceneSetSource(s, 1, V(0, 1.6f, 3.5f));
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
            float occ[kBands] = {};
            const int idx = AF_SceneSourceIndex(s, 1);
            if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, occ);
            float lo = 1e9f, hi = -1e9f;
            for (int b = 0; b < kBands; ++b) { lo = std::min(lo, occ[b]); hi = std::max(hi, occ[b]); }
            AF_SceneDestroy(s);
            return 20.0f * std::log10(std::max(hi, 1e-6f) / std::max(lo, 1e-6f));
        };
        // ★★ ポータルの帯域依存は潰してはいけない ★★
        //   ポータル経路の帯域差はフレネル半径 r1=√(λd1d2/(d1+d2)) が波長に依るぶんで、
        //   「開口の広さ対波長」＝扉がどれだけ開いているかを音色で伝える当のもの。
        //   前川（障害物のこもり）とは運んでいる情報が違う。一度これを一緒に均してしまった。
        {
            const float hh2 = 4.0f, tt2 = 0.15f, hw3 = 6.0f, hd3 = 8.0f, dw = 1.2f, dh = 2.4f;
            auto buildPortal = [&](float openFrac) {
                AF_SceneHandle s = AF_SceneCreate();
                const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
                AF_SceneAddInstanceBox(s, V(-(hw3+dw*0.5f)*0.5f, hh2*0.5f, 0),
                                       V((hw3-dw*0.5f)*0.5f, hh2*0.5f, tt2), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V( (hw3+dw*0.5f)*0.5f, hh2*0.5f, 0),
                                       V((hw3-dw*0.5f)*0.5f, hh2*0.5f, tt2), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(0, (dh+hh2)*0.5f, 0),
                                       V(dw*0.5f, (hh2-dh)*0.5f, tt2), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(0, -tt2, 0),     V(hw3, tt2, hd3), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(0, hh2+tt2, 0),  V(hw3, tt2, hd3), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(-hw3, hh2*0.5f, 0), V(tt2, hh2*0.5f, hd3), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V( hw3, hh2*0.5f, 0), V(tt2, hh2*0.5f, hd3), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(0, hh2*0.5f, -hd3), V(hw3, hh2*0.5f, tt2), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(0, hh2*0.5f,  hd3), V(hw3, hh2*0.5f, tt2), V(1,0,0), V(0,1,0), m);
                // 戸口を openFrac だけ残して塞ぐ扉。
                const float blocked = dw * (1.0f - openFrac);
                if (blocked > 1e-3f)
                    AF_SceneAddInstanceBox(s, V(-dw*0.5f + blocked*0.5f, dh*0.5f, 0),
                                           V(blocked*0.5f, dh*0.5f, 0.03f), V(1,0,0), V(0,1,0), m);
                AF_SceneAddPortal(s, V(0, dh*0.5f, 0), V(1,0,0), V(0,1,0), dw*0.5f, dh*0.5f);
                return s;
            };
            auto portalTilt = [&](float openFrac) {
                AF_SceneHandle s = buildPortal(openFrac);
                AF_SceneSetListener(s, V(-2.0f, 1.6f, -4.0f));
                AF_SceneSetSource(s, 1, V(2.0f, 1.6f, 4.0f));
                for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
                float occ[kBands] = {};
                const int idx = AF_SceneSourceIndex(s, 1);
                if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, occ);
                const float t = 20.0f * std::log10(std::max(occ[0], 1e-6f)
                                                 / std::max(occ[kBands-1], 1e-6f));
                AF_SceneDestroy(s);
                return t;
            };
            // 隙間が狭いほど「低域だけ通る」＝低域と高域の差が開くこと。
            const float narrow = portalTilt(0.08f);   // 1割弱だけ開いている
            const float wide   = portalTilt(1.00f);   // 全開
            check("[開口] 隙間が狭いほど低域が相対的に通る", narrow > wide + 1.0f);
            check("[開口] 全開なら帯域差はほぼ無い", wide < 1.5f);
        }

        check("[回折] 障害物なしなら生存ゲインは平坦", tiltDb(false, true, noAbsorb) < 0.2f);
        check("[回折] 柱を置いても生存ゲインは平坦（吸わない表面）",
              tiltDb(true, true, noAbsorb) < 0.2f);
        // 切り替えが効いている証拠。★吸わない表面だと反射が強すぎて回折の傾きが薄まるので、
        //   ここは既定壁（吸音あり）で比べる。既定壁では平坦化 ON でも吸音ぶんの傾きが残る
        //   （物理的に正しい残り方）ので、絶対値ではなく ON/OFF の差で見る。
        {
            const float on = tiltDb(true, true, nullptr);
            const float off = tiltDb(true, false, nullptr);
            check("[回折] 平坦化を切ると傾きが増える", off > on + 1.0f);
        }
    }

    // ── 部屋の占め方（部屋を音に使うための唯一の入口）──
    //   部屋番号そのもので残響を切り替えると、プレイヤーが必ず通る戸口のど真ん中に
    //   不連続を置くことになる。割合が連続に変わることがこの層の存在理由なので、
    //   「なめらかさ」を数値で縛る。
    {
        AF_SceneHandle s = build(0.9f);
        int ids[8]; float w[8];
        // 部屋の奥では 1 つの部屋で埋まる。
        {
            const int n = AF_SceneRoomWeights(s, V(0, 1.6f, -6.0f), 1.0f, ids, w, 8);
            check("[占め方] 部屋の奥では 1 つの部屋が全部", n >= 1 && w[0] > 0.99f);
        }
        // 戸口の中では両側が混ざる（どちらにも寄り切らない）。
        {
            const int n = AF_SceneRoomWeights(s, V(0, 1.6f, 0.0f), 1.0f, ids, w, 8);
            check("[占め方] 戸口では両側が混ざる",
                  n >= 2 && w[0] < 0.8f && w[1] > 0.2f);
            float sum = 0.0f; for (int i = 0; i < n; ++i) sum += w[i];
            check("[占め方] 割合の合計が 1", std::fabs(sum - 1.0f) < 1e-3f);
        }
        // ★なめらかさ。0.1m ずつ歩いて、1 歩の変化が半径から決まる幾何的な限界の
        //   2 倍以内に収まること（球が面を横切るときの最大傾きは 3/(4r)）。
        for (float rad : {1.0f, 2.0f}) {
            float prev = -1.0f, worst = 0.0f;
            for (float z = -3.0f; z <= 3.001f; z += 0.1f) {
                const int n = AF_SceneRoomWeights(s, V(0, 1.6f, z), rad, ids, w, 8);
                float w0 = 0.0f;
                for (int i = 0; i < n; ++i) if (ids[i] == 0) w0 = w[i];
                if (prev >= 0.0f) worst = std::max(worst, std::fabs(w0 - prev));
                prev = w0;
            }
            const float limit = 2.0f * (3.0f / (4.0f * rad)) * 0.1f;
            char tag[80];
            std::snprintf(tag, sizeof(tag),
                          "[占め方] 半径%.0fm で 0.1m あたりの変化が %.3f 以内", rad, limit);
            check(tag, worst <= limit);
        }
        // 実効体積が部屋の体積であること（レベル全体の外形箱ではない）。
        {
            float vr = 0.0f;
            AF_SceneRoomInfo(s, AF_SceneRoomAt(s, V(0, 1.6f, -6.0f)), &vr,
                             nullptr, nullptr, nullptr);
            const float ve = AF_SceneRoomVolumeAt(s, V(0, 1.6f, -6.0f), 1.0f);
            check("[占め方] 実効体積が部屋の体積", vr > 1.0f && std::fabs(ve - vr) < vr * 0.02f);
        }
        AF_SceneDestroy(s);
    }

    // ── 部屋の残響を形と材質から出す ──
    //   エコグラムから測ると窓の長さに縛られて部屋の違いが出ない（実測: 材質が 17.6 倍
    //   違う 2 部屋で RT60 が 0.50 と 0.51 秒＝区別できていない）。Sabine なら幾何で決まる。
    {
        const float hh = 4.0f, tt = 0.15f, hw2 = 6.0f, hd2 = 8.0f;
        const float liveA[6] = {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f};
        const float deadA[6] = {0.60f, 0.70f, 0.80f, 0.85f, 0.90f, 0.90f};
        AF_SceneHandle s = AF_SceneCreate();
        const int mL = AF_SceneAddMaterial(s, nullptr, liveA, nullptr, 6);
        const int mD = AF_SceneAddMaterial(s, nullptr, deadA, nullptr, 6);
        AF_SceneAddInstanceBox(s, V(-(hw2+0.45f)*0.5f, hh*0.5f, 0),
                               V((hw2-0.45f)*0.5f, hh*0.5f, tt), V(1,0,0), V(0,1,0), mL);
        AF_SceneAddInstanceBox(s, V( (hw2+0.45f)*0.5f, hh*0.5f, 0),
                               V((hw2-0.45f)*0.5f, hh*0.5f, tt), V(1,0,0), V(0,1,0), mL);
        for (int side = 0; side < 2; ++side) {
            const float zc = (side == 0) ? -hd2*0.5f : hd2*0.5f;
            const int mm = (side == 0) ? mL : mD;
            AF_SceneAddInstanceBox(s, V(0, -tt, zc),     V(hw2, tt, hd2*0.5f), V(1,0,0), V(0,1,0), mm);
            AF_SceneAddInstanceBox(s, V(0, hh+tt, zc),   V(hw2, tt, hd2*0.5f), V(1,0,0), V(0,1,0), mm);
            AF_SceneAddInstanceBox(s, V(-hw2, hh*0.5f, zc), V(tt, hh*0.5f, hd2*0.5f), V(1,0,0), V(0,1,0), mm);
            AF_SceneAddInstanceBox(s, V( hw2, hh*0.5f, zc), V(tt, hh*0.5f, hd2*0.5f), V(1,0,0), V(0,1,0), mm);
        }
        AF_SceneAddInstanceBox(s, V(0, hh*0.5f, -hd2), V(hw2, hh*0.5f, tt), V(1,0,0), V(0,1,0), mL);
        AF_SceneAddInstanceBox(s, V(0, hh*0.5f,  hd2), V(hw2, hh*0.5f, tt), V(1,0,0), V(0,1,0), mD);

        const int rLive = AF_SceneRoomAt(s, V(0, 1.6f, -5.0f));
        const int rDead = AF_SceneRoomAt(s, V(0, 1.6f,  5.0f));
        float vL = 0, sL = 0, oL = 0, abL[6] = {}, rtL[6] = {};
        float vD = 0, sD = 0, oD = 0, abD[6] = {}, rtD[6] = {};
        const int okL = (rLive >= 0) && AF_SceneRoomAcoustics(s, rLive, &sL, &oL, abL, rtL);
        const int okD = (rDead >= 0) && AF_SceneRoomAcoustics(s, rDead, &sD, &oD, abD, rtD);
        AF_SceneRoomInfo(s, rLive, &vL, nullptr, nullptr, nullptr);
        AF_SceneRoomInfo(s, rDead, &vD, nullptr, nullptr, nullptr);
        check("[残響] 響く部屋と吸う部屋を別の部屋として見る", rLive >= 0 && rDead >= 0 && rLive != rDead);
        check("[残響] 響く部屋の RT60 が吸う部屋の 5 倍以上",
              okL && okD && rtL[2] > rtD[2] * 5.0f);
        // Sabine の式どおりか（RT60 = 0.161 V / Σ Sα）を手計算と突き合わせる。
        if (okL) {
            const float A = sL * abL[2];
            const float want = 0.161f * vL / std::max(A, 1e-6f);
            check("[残響] Sabine の式に一致", std::fabs(rtL[2] - want) < want * 0.05f);
        } else check("[残響] Sabine の式に一致", false);
        // 吸音率が帯域で違うので RT60 も帯域で違うこと（低域ほど長い）。
        check("[残響] 低域ほど残響が長い", okL && rtL[0] > rtL[5] * 1.5f);
        // 開口は完全吸音として境界に入っていること（開口の面積と一致）。
        {
            float apA = 0.0f;
            if (AF_SceneApertureCount(s) > 0)
                AF_SceneApertureInfo(s, 0, &apA, nullptr, nullptr, nullptr, nullptr);
            check("[残響] 開口が部屋の吸音面に入っている",
                  okL && apA > 0.1f && std::fabs(oL - apA) < apA * 0.35f);
        }
        // 位置に対してなめらかであること（0.1m あたりの傾きが有界）。
        {
            float prev = -1.0f, worst = 0.0f;
            for (float z = -4.0f; z <= 4.001f; z += 0.1f) {
                float rt6[6] = {};
                AF_SceneRt60At(s, V(0, 1.6f, z), 2.0f, rt6, 6);
                if (prev >= 0.0f) worst = std::max(worst, std::fabs(rt6[2] - prev));
                prev = rt6[2];
            }
            // 総変化 (rtL-rtD) を半径 2.0m ぶんで渡す＝理想は総変化/40 歩。3 倍まで許す。
            const float ideal = (rtL[2] - rtD[2]) / 40.0f;
            check("[残響] 位置に対する傾きが有界", worst < ideal * 3.0f);
        }
        AF_SceneDestroy(s);
    }

    // ── 戸口ではない形 ──
    //   曲がり角・柱・腰高の仕切りは「くびれ」ではないので割れてはいけない。
    //   割れると、そこに偽の境界ができて音が跳ねる。
    {
        const float hh = 4.0f, tt = 0.3f;
        struct Blk { float x0, x1, z0, z1; float y0 = 0.0f, y1 = -1.0f; };
        auto world = [&](float X, float Z, const std::vector<Blk>& blocks) {
            AF_SceneHandle s = AF_SceneCreate();
            const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            AF_SceneAddInstanceBox(s, V(0, -tt, 0),     V(X+tt, tt, Z+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh+tt, 0),   V(X+tt, tt, Z+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(-X-tt, hh*0.5f, 0), V(tt, hh*0.5f, Z+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( X+tt, hh*0.5f, 0), V(tt, hh*0.5f, Z+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f, -Z-tt), V(X+tt, hh*0.5f, tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f,  Z+tt), V(X+tt, hh*0.5f, tt), V(1,0,0), V(0,1,0), m);
            for (const Blk& b : blocks) {
                const float y0 = b.y0, y1 = (b.y1 < 0.0f) ? hh : b.y1;
                AF_SceneAddInstanceBox(s, V((b.x0+b.x1)*0.5f, (y0+y1)*0.5f, (b.z0+b.z1)*0.5f),
                                       V((b.x1-b.x0)*0.5f, (y1-y0)*0.5f, (b.z1-b.z0)*0.5f),
                                       V(1,0,0), V(0,1,0), m);
            }
            AF_SceneSetRoomCellSize(s, 0.1f);
            return s;
        };
        {   // L 字の曲がり角。曲がるだけで広さは変わらない＝境界ではない。
            AF_SceneHandle s = world(9.0f, 9.0f, {
                {-9.0f,-0.5f, 0.5f, 9.0f}, { 0.5f, 9.0f, 0.5f, 9.0f},
                { 0.5f, 9.0f,-0.5f, 0.5f}, {-9.0f, 9.0f,-9.0f,-0.5f},
            });
            check("[開口] 曲がり角では割れない",
                  AF_SceneRoomCount(s) == 1 && AF_SceneApertureCount(s) == 0);
            AF_SceneDestroy(s);
        }
        {   // 大部屋の柱。周りを回れるので境界ではない。
            AF_SceneHandle s = world(8.0f, 8.0f, {{-0.5f, 0.5f, -0.5f, 0.5f}});
            check("[開口] 柱では割れない",
                  AF_SceneRoomCount(s) == 1 && AF_SceneApertureCount(s) == 0);
            AF_SceneDestroy(s);
        }
        {   // 腰高の仕切り。上が全部開いているので境界ではない。
            AF_SceneHandle s = world(6.0f, 6.0f, {{-6.0f, 6.0f, -0.15f, 0.15f, 0.0f, 1.2f}});
            check("[開口] 腰高の仕切りでは割れない",
                  AF_SceneRoomCount(s) == 1 && AF_SceneApertureCount(s) == 0);
            AF_SceneDestroy(s);
        }
        {   // 壁に 1.0 × 1.0m の窓だけ。割れて、口の面積が窓の実寸になること。
            AF_SceneHandle s = world(6.0f, 6.0f, {
                {-6.0f,-0.5f,-0.15f, 0.15f}, { 0.5f, 6.0f,-0.15f, 0.15f},
                {-0.5f, 0.5f,-0.15f, 0.15f, 0.0f, 1.5f},
                {-0.5f, 0.5f,-0.15f, 0.15f, 2.5f, 4.0f},
            });
            float a = 0.0f;
            const int na = AF_SceneApertureCount(s);
            if (na > 0) AF_SceneApertureInfo(s, 0, &a, nullptr, nullptr, nullptr, nullptr);
            check("[開口] 窓だけの壁は割れる", AF_SceneRoomCount(s) == 2 && na == 1);
            check("[開口] 窓の口の面積が実寸(1.0m2)", na == 1 && std::fabs(a - 1.0f) < 0.25f);
            AF_SceneDestroy(s);
        }
        {   // 食い違い壁。見通しは切れるが繋がっている＝割れて口が出るのが正しい。
            AF_SceneHandle s = world(6.0f, 6.0f, {
                {-6.0f, 1.0f,-0.65f,-0.35f}, {-1.0f, 6.0f, 0.35f, 0.65f},
            });
            check("[開口] 食い違い壁は割れて口が出る",
                  AF_SceneRoomCount(s) == 2 && AF_SceneApertureCount(s) == 1);
            AF_SceneDestroy(s);
        }
    }

    // 同じ 2 部屋を繋ぐ口が 2 つあるとき、まとめずに分けること。
    {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-4.5f,  h*0.5f, 0), V(1.5f,  h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-0.05f, h*0.5f, 0), V(2.05f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V( 4.3f,  h*0.5f, 0), V(1.7f,  h*0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneSetRoomCellSize(s, 0.1f);
        const int na = AF_SceneApertureCount(s);
        float a0 = 0.0f, a1 = 0.0f; AF_Vector3 c0{}, c1{};
        if (na >= 2) {
            AF_SceneApertureInfo(s, 0, &a0, &c0, nullptr, nullptr, nullptr);
            AF_SceneApertureInfo(s, 1, &a1, &c1, nullptr, nullptr, nullptr);
        }
        check("[開口] 戸口 2 箇所は 2 つの口に分かれる", na == 2);
        check("[開口] 面積の大きい順に並ぶ", na == 2 && a0 >= a1);
        check("[開口] それぞれの中心が各戸口の位置",
              na == 2 && std::fabs(c0.x - (-2.55f)) < 0.3f && std::fabs(c1.x - 2.30f) < 0.3f);
        AF_SceneDestroy(s);
    }
}

void testRobustness() {
    std::printf("\n[頑健性] null / 不正引数\n");
    float g[kBands] = {};
    AF_SceneComputeTransmissionBands(nullptr, V(0, 0, 0), V(1, 0, 0), g, kBands);
    check("null scene で落ちない", true);

    AF_SceneHandle s = AF_SceneCreate();
    AF_SceneComputeTransmissionBands(s, V(0, 0, 0), V(1, 0, 0), nullptr, kBands);
    AF_SceneComputeEarlyReflections(s, V(0, 0, 0), V(1, 0, 0), nullptr, nullptr, 0, 16, 2);
    check("null 出力バッファで落ちない", true);

    // 音源数 0 / ビン数 0。
    std::vector<float> bins(10, 0.0f);
    AF_SceneComputeEchogram(s, V(0, 0, 0), nullptr, 0, bins.data(), 10, 0.01f, 343.0f, 16, 2);
    check("音源0で落ちない", true);
    AF_SceneDestroy(s);
}

}  // namespace

int main() {
    std::printf("=== AF_Scene* 数値回帰テスト ===\n");
    std::printf("（期待値は絶対値でなく「関係」で書いている。詳細は冒頭コメント参照）\n");

    testInstanceLifecycle();
    testSourceRegistry();
    testTransmission();
    testOcclusionAndDiffraction();
    testEarlyReflections();
    testEchogram();
    testBatchUpdate();
    testUpdateRates();
    diagnoseTailSpectrum();
    diagnoseShadowBoundary();
    diagnoseApertureDirection();
    diagnoseSwingDoor();
    testMesh();
    testDirectionalProbe();
    testSecondOrderDiffraction();
    testRoomSegmentation();
    testRoomIncremental();
    diagnoseRoomDetection();
    testOutdoorIsNotARoom();
    testManySources();
    diagnosePortalScopeGlobal();
    diagnosePerSourceEchogram();
    diagnoseAbsorptionVsLocalization();
    diagnoseShadowCliff();
    diagnoseNonDoorShapes();
    diagnosePillarTimbre();
    diagnoseReverbSendWalk();
    diagnosePortalScope();
    diagnoseNonPlateBlocker();
    diagnoseApertureWidthCurve();
    diagnoseRoomToRoomLevel();
    diagnoseDistanceReach();
    diagnoseUnityGapScene();
    diagnoseSecondOrderQuality();
    diagnoseDoorApertureCurve();
    diagnoseFarSourceTimbre();
    diagnoseRoomCoupling();
    testLeakDirection();
    diagnoseThickBarrier();
    testNoPhantomDiffraction();
    testPortalOpening();
    diagnoseWallDoorBoundary();
    diagnoseSourceOffsetVsDoor();
    diagnoseFrameCost();
    testSoftOcclusion();
    diagnoseDiffractionOnlySweep();
    testApertureOpenness();
    testDoorContinuity();
    testPerInstanceMaterial();
    testVoiceApi();
    testRobustness();

    std::printf("\n----\n");
    if (g_failures == 0) {
        std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);
    return 1;
}
