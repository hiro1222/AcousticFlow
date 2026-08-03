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
        std::printf("      %8.2f  %6.3f  %6.3f   %6.3f  %6.3f%s\n",
                    x, g[0], g[5], u[0], u[5],
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

    // 大きな壁。x∈[-10,2] が壁、x>2 が唯一の開口。上下も十分に伸ばしてある。
    //   このとき二次音源は「x=+2 の縁」に強く集中すべきである。
    //   当の箱を遮蔽判定から丸ごと除外していると、壁を貫いて到達する稜線まで候補に通るため、
    //   重みが分散して「どこから抜けてくるか」がぼやける（実測 0.31 まで低下）。
    AF_SceneAddInstanceBox(s, V(-4, 2, 0), V(6, 4, 0.2f), V(1, 0, 0), V(0, 1, 0), mat);

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

    // 開口が1つしかないのだから、重みはそこに集中していなければならない。
    //   分散していると「どの方向から抜けてくるか」が伝わらず、定位がぼやける。
    //   壁を貫く経路を候補に通していた頃はここが 0.31 だった。
    char b2[96];
    std::snprintf(b2, sizeof(b2), "(最大 %.3f)", top);
    check("重みが唯一の開口に集中している(最大>0.5)", top > 0.5f, b2);

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

    const AF_Vector3 L = V(0, 1.6f, -3), S = V(0, 1.6f, 3);
    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);

    std::printf("      開き角   125Hz    4kHz   遮蔽  開口位置(最有力)        重み\n");

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

        if (bi >= 0) {
            std::printf("      %5.1f°  %6.3f  %6.3f    %d   (%6.2f,%6.2f,%6.2f)  %.3f\n",
                        deg, g[0], g[5], occ, pos[bi].x, pos[bi].y, pos[bi].z, bw);
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
            std::printf("      %5.1f°  %6.3f  %6.3f    %d   (開口なし)\n", deg, g[0], g[5], occ);
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

        if (occ && !(g[0] > g[5])) lowOverHigh = false;   // 性質4
    }

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
    diagnoseShadowBoundary();
    diagnoseApertureDirection();
    diagnoseSwingDoor();
    testMesh();
    testDirectionalProbe();
    testRobustness();

    std::printf("\n----\n");
    if (g_failures == 0) {
        std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);
    return 1;
}
