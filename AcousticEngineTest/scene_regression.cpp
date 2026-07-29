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
    testRobustness();

    std::printf("\n----\n");
    if (g_failures == 0) {
        std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);
    return 1;
}
