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
#include <cmath>
#include <cstdio>
#include <vector>

#include "acoustic_scene.h"

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
    const int gotBins = AF_SceneGetEchogramBands(s, batchEcho.data(), kBins);
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
    AF_SceneGetEchogramBands(s, e1.data(), 50);
    float sum1 = 0.0f;
    for (float v : e1) sum1 += v;
    check("初回updateでエコグラムが埋まる", sum1 > 0.0f);

    // 音源を大きく動かしてから 1 フレームだけ回す。
    // role2EveryN=4 なのでエコグラムはまだ更新されない＝前回値のままのはず。
    AF_SceneSetSource(s, 1, V(4, 1.6f, 3));
    AF_SceneUpdate(s, 0.016f);
    std::vector<float> e2(50 * kBands, 0.0f);
    AF_SceneGetEchogramBands(s, e2.data(), 50);
    bool unchanged = true;
    for (int i = 0; i < 50 * kBands; ++i) if (std::fabs(e1[i] - e2[i]) > 1e-9f) unchanged = false;
    check("role2EveryN=4 なので次フレームでは再計算されない", unchanged);

    // さらに 3 フレーム進めるとカウンタが 0 になり再計算される。
    AF_SceneUpdate(s, 0.016f);
    AF_SceneUpdate(s, 0.016f);
    AF_SceneUpdate(s, 0.016f);
    std::vector<float> e3(50 * kBands, 0.0f);
    AF_SceneGetEchogramBands(s, e3.data(), 50);
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

// ---------------------------------------------------------------- 頑健性
// 不正入力で落ちない（移行中に呼び出し規約を変えるので、境界は明示的に守る）。
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
    testRobustness();

    std::printf("\n----\n");
    if (g_failures == 0) {
        std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);
    return 1;
}
