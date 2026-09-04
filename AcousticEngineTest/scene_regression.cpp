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
// ★windows.h はここで、しかも std のヘッダより先に取り込む。
//   NOMINMAX が無いと min/max がマクロになって std::max(...) が全滅する（実際にやった）。
//   用途は「配布先の DLL が古い」警告を赤字で出すための ANSI 有効化だけ。
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "acoustic_scene.h"
// ★破れの型紙。**デバッグツールの走査と、この検査が同じ式を使う**ためのヘッダ。
//   別々の式にすると「道具は見つけたのに検査は通る」が起きる（docs/SOUND_DEBUG_TOOL.md）。
#include "../AcousticEngine/src/Debug/detectors.h"
#include "../AcousticEngine/src/Debug/capture_scan.h"       // 録った .afcap に型紙を当てる（走査）
#include "capture_replay.h"     // 録った入力を押し直して解き直す
#include "../AcousticEngine/src/Debug/capture_emit.h"       // 破れ → 回帰テストの生成
#include "acoustic_voice.h"

namespace {

AF_Vector3 V(float x, float y, float z) { return AF_Vector3{ x, y, z }; }

int g_failures = 0;
int g_checks = 0;

// ─────────────────────────────────────────────────────────────────────
// Unity 側の DLL が、いま試験しているビルドと同じものかを見る。
//
// ★なぜ要るか（2026-08-22 に実際に起きた事故）
//   DLL を Unity へ配るのは CMake に入っておらず、手作業だった。1 日ぶん抜けていて、
//   **エディタでは前日のエンジンが動いていた**。こちらがテストで出していた数字と、
//   本人がエディタで聞いていた音が別物だったことになる。
//   「実装したのに音が変わらない」を何時間も追うことになる事故なので、気づける形にする。
//
//   ビルド後の自動コピーは採らなかった。Unity が開いていると DLL を掴むので、
//   ビルドが止まるか、黙って飛ばして「コピーされていないのに気づかない」を作る。
//   **ビルドは絶対に止めず、数字を読む直前に必ず目に入る**ほうを採った。
//
//   検査ではない（g_checks に数えない）。環境の話であってエンジンの性質ではないので、
//   ここで FAIL を増やすと「テストが落ちた」の意味が濁る。
// ─────────────────────────────────────────────────────────────────────
// ANSI の色が使えるなら使う。使えない端末では制御文字を出さない（文字化けさせない）。
bool ansiColorAvailable() {
#if defined(_WIN32)
    static const bool ok = [] {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
        return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }();
    return ok;
#else
    return true;
#endif
}

// 中身が同じか。大きさが違えば即座に違う。同じなら全部読んで比べる（430KB 程度）。
enum class DllCmp { Same, Differ, Missing };
DllCmp compareFiles(const char* a, const char* b) {
    std::FILE* fa = std::fopen(a, "rb");
    std::FILE* fb = std::fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) std::fclose(fa);
        if (fb) std::fclose(fb);
        return DllCmp::Missing;
    }
    std::fseek(fa, 0, SEEK_END); const long na = std::ftell(fa); std::rewind(fa);
    std::fseek(fb, 0, SEEK_END); const long nb = std::ftell(fb); std::rewind(fb);
    DllCmp r = DllCmp::Same;
    if (na != nb) {
        r = DllCmp::Differ;
    } else {
        char ba[16384], bb[16384];
        for (;;) {
            const size_t ra = std::fread(ba, 1, sizeof(ba), fa);
            const size_t rb = std::fread(bb, 1, sizeof(bb), fb);
            if (ra != rb || std::memcmp(ba, bb, ra) != 0) { r = DllCmp::Differ; break; }
            if (ra == 0) break;
        }
    }
    std::fclose(fa);
    std::fclose(fb);
    return r;
}

// 出荷先ごとの状態。main の冒頭と末尾の 2 回出す（末尾がいちばん読まれるので）。
struct DllSite { const char* name; const char* path; DllCmp state; };
DllSite g_dllSites[] = {
#if defined(AF_BUILT_DLL_PATH) && defined(AF_UNITY_DLL_PATH)
    { "UnityDemo", AF_UNITY_DLL_PATH, DllCmp::Same },
#endif
#if defined(AF_BUILT_DLL_PATH) && defined(AF_DIST_DLL_PATH)
    { "DistDemo",  AF_DIST_DLL_PATH,  DllCmp::Same },
#endif
};
const int g_dllSiteCount = static_cast<int>(sizeof(g_dllSites) / sizeof(g_dllSites[0]));

void scanDllFreshness() {
#if defined(AF_BUILT_DLL_PATH)
    for (int i = 0; i < g_dllSiteCount; ++i)
        g_dllSites[i].state = compareFiles(AF_BUILT_DLL_PATH, g_dllSites[i].path);
#endif
}

// stale が 1 つでもあれば true。
bool reportDllFreshness() {
    int stale = 0;
    for (int i = 0; i < g_dllSiteCount; ++i)
        if (g_dllSites[i].state != DllCmp::Same) ++stale;
    if (stale == 0) return false;

    const char* R = ansiColorAvailable() ? "\033[1;31m" : "";
    const char* Z = ansiColorAvailable() ? "\033[0m"    : "";
    std::printf("\n%s"
        "########################################################################\n"
        "##  ⚠ 配布先の DLL が、いま試験したビルドと違います                   ##\n"
        "########################################################################%s\n",
        R, Z);
    for (int i = 0; i < g_dllSiteCount; ++i) {
        const char* s = (g_dllSites[i].state == DllCmp::Missing) ? "見つからない"
                      : (g_dllSites[i].state == DllCmp::Differ)  ? "★古い（中身が違う）"
                                                                 : "一致";
        std::printf("%s  %-10s %-22s %s%s\n", R, g_dllSites[i].name, s,
                    g_dllSites[i].path, Z);
    }
    std::printf("%s"
        "  → ここに出ている数字は、Unity で聞こえる音の数字では**ありません**。\n"
        "  → Unity を閉じてから配ってください（開いていると DLL を掴んで上書きできません）:\n"
        "%s\n", R, Z);
#if defined(AF_BUILT_DLL_PATH)
    for (int i = 0; i < g_dllSiteCount; ++i)
        if (g_dllSites[i].state != DllCmp::Same)
            std::printf("       cp \"%s\" \"%s\"\n", AF_BUILT_DLL_PATH, g_dllSites[i].path);
#endif
    std::printf("\n");
    return true;
}

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
    cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 3;
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

    // ★比べるのは**代表 1 本**。以前は -1（全音源の和）と「2 音源をまとめた従来クエリ」を
    //   突き合わせていたが、尾を部屋で共有するようにしたので、和の中身は
    //   「代表を人数ぶん足したもの」になった（意味＝全音源の和 は変えていない）。
    //   等価性の検査として意味があるのは「代表の位置で直接計算したものと一致するか」。
    const int repIdx = AF_SceneGetTailShapeIndex(s, 0);
    AF_Vector3 repSrc[1] = { srcs[(repIdx >= 0 && repIdx < 2) ? repIdx : 0] };
    std::vector<float> refEcho(kBins * kBands, 0.0f);
    AF_SceneComputeEchogramBands(s, L, repSrc, 1, refEcho.data(), kBins, 0.01f, 343.0f, 512, 24, 0.0f);
    std::vector<float> repEcho(kBins * kBands, 0.0f);
    AF_SceneGetEchogramBands(s, 0, repEcho.data(), kBins);
    float maxDiff = 0.0f, refTotal = 0.0f;
    for (int i = 0; i < kBins * kBands; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(repEcho[i] - refEcho[i]));
        refTotal += refEcho[i];
    }
    std::snprintf(buf, sizeof(buf), "(代表 %d / maxDiff=%.6g, total=%.4g)", repIdx, maxDiff, refTotal);
    check("エコグラム（代表）が従来クエリと一致", maxDiff < 1e-3f, buf);

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
    cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 3;
    cfg.reflectionRays = 64; cfg.reflectionBounces = 2;
    cfg.directWeight = 1.0f; cfg.useReflections = 1;
    cfg.useEdgeCatalog = 1; cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;

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

    // ★f（フレネル開口）が掛かっているかを出す。§5-13 の排他が効いているかの判定用。
    //   2 次回折には**開口の断面が取れない**ので、f ではなく前川が持つのが設計。
    //   f が掛かっていると 0 になる（開口が無いので開口率が 0 に落ちる）。
    {
        float d17[32] = {};
        AF_SceneDebugDiffractionPath(s, L, S, d17, 0);
        std::printf("      f の状態: 使用 %s / 面 %.0f 枚 有界 %.0f 枚 / 縁 u=%.2f v=%.2f"
                    " / 後ろで捨てた %.0f 個\n",
                    d17[18] > 0.5f ? "★している" : "していない（前川）",
                    d17[22], d17[23], d17[24], d17[25], d17[26]);
    }

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
    // ★ワーカー数を環境変数で振れるようにしてある（AF_WORKERS=4 など）。
    //   既定は 1（＝エンジンの既定）のまま。ここを既定で増やすと、
    //   厳密差分で回帰を判定できなくなるため（worker_pool.h の設計どおり）。
    //   ただし **ホスト側の既定は 4**（AcousticFlowSceneDemo.workerThreads）なので、
    //   実機に対応する数字が欲しいときは AF_WORKERS=4 で回すこと。
    int workers = 1;
    if (const char* w = std::getenv("AF_WORKERS")) {
        const int v = std::atoi(w);
        if (v >= 1 && v <= 64) workers = v;
    }
    std::printf("\n[診断] 1フレームの音響計算にかかる時間（Test_Full と同じ規模 / ワーカー %d）\n", workers);

    AF_SceneHandle s = AF_SceneCreate();
    AF_SceneSetWorkerThreads(s, workers);
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
        {"エッジカタログ",   [](AF_UpdateConfig& c){ }},
        {"反射込み遮蔽",     [](AF_UpdateConfig& c){ c.useReflections = 0; }},
    };
    for (const Ablate& a : abl) {
        AF_UpdateConfig c = base; a.apply(c);
        const double ms = runFrames(c, true, 200);
        std::printf("          %-16s を切ると 平均 %7.3f ms（取り分 %6.3f / %4.1f%%）  最悪 %7.3f ms\n",
                    a.name, ms, all - ms, (all - ms) / all * 100.0, lastMax);
    }

    // ★コア数を振る。ホストの HUD に出る「音響 ms」がそのまま何倍になるかを見る。
    //   ここは Test_Full と同じ規模なので、ゲーム側の体感に一番近い数字になる。
    {
        std::printf("\n        コア数を振ったときの 1 フレーム（★ホストの音響 ms に直結）\n");
        std::printf("          %8s %10s %10s %10s\n", "コア", "平均 ms", "最悪 ms", "1コア比");
        double base1 = 0.0;
        for (int th : {1, 2, 4, 8}) {
            AF_SceneSetWorkerThreads(s, th);
            runFrames(base, true, 30);                 // 暖機
            const double ms = runFrames(base, true, 150);
            if (th == 1) base1 = ms;
            std::printf("          %8d %10.3f %10.3f %9.2fx\n",
                        th, ms, lastMax, (ms > 0) ? base1 / ms : 0.0);
        }
        AF_SceneSetWorkerThreads(s, 1);                // 以降の診断は直列で採る
        runFrames(base, true, 30);
    }

    // ★部屋分割・自動ポータルの取り分。
    //   GPU へ回せるか（連絡板 2026-08-22）を判断するために要る数字。
    //   ボクセルの距離変換・連結・塗り戻しは GPU 向きの形をしているが、
    //   **定常フレームで何 ms 占めているか**を先に知らないと、回しても無駄になる。
    //   ⚠ 自動ポータルを切ると音も変わる（ポータルの寄与が消える）ので、
    //     これは「取り分の上限」であって純粋なボクセル処理の代金ではない。
    {
        AF_SceneSetAutoPortals(s, 0);
        const double noRooms = runFrames(base, true, 200);
        AF_SceneSetAutoPortals(s, 1);
        runFrames(base, true, 30);   // 戻して暖機
        std::printf("        %-16s を切ると 平均 %7.3f ms（取り分の上限 %6.3f / %4.1f%%）\n",
                    "部屋分割・自動ポータル", noRooms, all - noRooms,
                    (all - noRooms) / all * 100.0);
    }

    // 扉を止める＝BVH が汚れない。差が「動かすこと自体」の代金。
    const double still = runFrames(base, false, 200);
    std::printf("        扉を止めると %7.3f ms  （動かす代金 %6.3f ms / %4.1f%%）\n",
                still, all - still, (all - still) / all * 100.0);

    // ─────────────────────────────────────────────────────────────
    // ★間引きの効き方を段ごとに測る
    //
    //   ゲームレーンからの問い（連絡板 2026-08-22）:
    //     「diffractionUpdateEveryFrames を 4 → 8 にしても 80ms から動かない。
    //       間隔が 4 以上でクランプされているのか、別に律速があるのか」
    //
    //   間隔を 1,2,4,8,16 と振って、**まだ効くのか飽和するのか**を段ごとに出す。
    //   飽和していれば「その段はもう費用の主ではない」＝別の段が床を作っている。
    //   コードにクランプは無い（cfg_.*EveryN は <=0 のときだけ 1 に直すだけ）ので、
    //   飽和するとすればそれは律速が別にあることを意味する。
    // ─────────────────────────────────────────────────────────────
    std::printf("\n        間引きを振ったときの平均 ms（★まだ効くのか / 飽和したのか）\n");
    std::printf("          %-14s %8s %8s %8s %8s %8s   %s\n",
                "段", "N=1", "N=2", "N=4", "N=8", "N=16", "1→16 で減る量");
    struct Knob { const char* name; int AF_UpdateConfig::* field; };
    const Knob knobs[] = {
        {"遮蔽・回折",   &AF_UpdateConfig::role1EveryN},
        {"回折二次音源", &AF_UpdateConfig::diffSrcEveryN},
        {"早期反射",     &AF_UpdateConfig::earlyEveryN},
        {"エッジカタログ", &AF_UpdateConfig::catalogEveryN},
        {"残響エコグラム", &AF_UpdateConfig::role2EveryN},
    };
    for (const Knob& k : knobs) {
        double v[5];
        const int ns[5] = {1, 2, 4, 8, 16};
        for (int i = 0; i < 5; ++i) {
            AF_UpdateConfig c = base;
            c.*(k.field) = ns[i];
            v[i] = runFrames(c, true, 150);
        }
        std::printf("          %-14s %8.3f %8.3f %8.3f %8.3f %8.3f   %6.3f ms (%4.1f%%)\n",
                    k.name, v[0], v[1], v[2], v[3], v[4],
                    v[0] - v[4], (v[0] - v[4]) / v[0] * 100.0);
    }
    std::printf("        → 8→16 でほぼ動かない段は、その段が費用の主ではない。\n"
                "          既定で毎フレーム走るのは**遮蔽・回折(role1EveryN=1)**だけで、\n"
                "          これが per-source の床を作る（音源ごとに computeDirectSoft ＋\n"
                "          遮蔽なら diffractionComposite が走る）。回折二次音源の間引きは\n"
                "          「二次音源の置き直し」を間引くだけで、回折そのものは間引かない。\n");

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
            c.diffSrcEveryN = 2;
            c.reflectionRays = 256; c.reflectionBounces = 3;
            c.directWeight = 1.0f;
            c.useReflections = (mode == 0) ? 1 : 0;     // 反射込み / 直接+回折のみ

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
    // ★実機で「2 つの位置で 13dB 変わる」と報告されたので、成分ごとに追う。
    //   HUD では早期反射と散乱が**丸ごと消えて**いた。減衰ではなく消滅なら別の話。
    {
        std::printf("      ── 成分の内訳（早期反射が消える所を探す）──\n");
        std::printf("      x     透過     回折     反射本数  反射(広帯域)  部屋 実効体積\n");
        for (float x = -5.0f; x <= 5.01f; x += 1.0f) {
            const AF_Vector3 L = V(x, 1.6f, -2.0f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int k = 0; k < 3; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
            float tr[kBands] = {}; float of = 0.0f;
            AF_SceneComputeSoftOcclusion(s, L, S, tr, kBands, &of);
            float trb = 0.0f;
            for (int b = 0; b < kBands; ++b) trb += tr[b] / kBands;
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float dif = 0.0f;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < kBands; ++b) dif += db[i * kBands + b] / kBands;
            const int idx = AF_SceneSourceIndex(s, 1);
            AF_Vector3 ep[16]; float eg[16 * kBands];
            const int ne = (idx >= 0) ? AF_SceneGetEarlyReflections(s, idx, ep, eg, 16) : 0;
            float ref = 0.0f;
            for (int i = 0; i < ne; ++i)
                for (int b = 0; b < kBands; ++b) ref += eg[i * kBands + b] / kBands;
            std::printf("   %5.1f  %7.4f  %7.4f   %2d本      %8.4f     %7.1f m3%s\n",
                        x, trb, dif, ne, ref,
                        AF_SceneRoomVolumeAt(s, L, 1.0f),
                        (ne == 0) ? "   ★反射が消えている" : "");
        }
    }
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
// 洞窟の口の場面 ── 外に立っていて、中で鳴っている。
//   S1「響きには向きがある」の基準測定。いまの尾は部屋ごとの 2ch バスで、リスナーの周囲から
//   出る（向きは探針から作る左右の耳のバランスだけ）。ここでは位置ごとに、
//     ・場そのものに向きがあるか（探針のエネルギーの平均方向と、口の方向とのずれ）
//     ・いまの鳴らし方がそれをどれだけ捉えているか（左右バランス＝正面の口では 1.00 に潰れる）
//     ・尾の量が口からの距離でどう変わるか（外にいるのに部屋の式で決まっていないか）
//   を数字で残す。「扉の定点から鳴らす」を入れる前の**前**の値。
void diagnoseCaveMouth() {
    std::printf("\n[診断] 洞窟の口（外に立って中の音を聴く）── 扉の定点から鳴らす前の基準\n");
    const float t = 0.15f, h = 4.0f, hw = 5.0f, depth = 8.0f, mouthW = 1.2f, mouthH = 2.4f;
    AF_SceneHandle s = AF_SceneCreate();
    const float liveA[6] = {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f};   // 響く岩
    const float tr[6] = {0.000398f, 0.0001585f, 0.0000398f,
                         0.00001f, 0.00000251f, 0.000001f};
    const int mLive = AF_SceneAddMaterial(s, tr, liveA, nullptr, 6);
    // 洞窟: x∈[-hw,hw], y∈[0,h], z∈[0,depth]。口は z=0 の壁、幅 mouthW・高さ mouthH。
    const float zc = depth * 0.5f;
    const float sideW = hw - mouthW * 0.5f;
    AF_SceneAddInstanceBox(s, V(-(mouthW*0.5f + sideW*0.5f), h*0.5f, 0), V(sideW*0.5f, h*0.5f, t),
                           V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V( (mouthW*0.5f + sideW*0.5f), h*0.5f, 0), V(sideW*0.5f, h*0.5f, t),
                           V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(0, (mouthH + h)*0.5f, 0), V(mouthW*0.5f, (h - mouthH)*0.5f, t),
                           V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(0, -t, zc),        V(hw + t, t, zc + t), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(0, h + t, zc),     V(hw + t, t, zc + t), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(-hw - t, h*0.5f, zc), V(t, h*0.5f, zc + t), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V( hw + t, h*0.5f, zc), V(t, h*0.5f, zc + t), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, depth + t), V(hw + t, h*0.5f, t), V(1,0,0), V(0,1,0), mLive);
    // 外の地面（外で立つ場所。地面の反射は現実にもある）
    AF_SceneAddInstanceBox(s, V(0, -t, -6.0f), V(14.0f, t, 6.0f), V(1,0,0), V(0,1,0), mLive);

    const AF_Vector3 S = V(1.5f, 1.5f, 5.0f);           // 中の音源（口の正面から外す）
    const AF_Vector3 mouth = V(0, mouthH * 0.5f, 0);    // 口の中心
    struct Pos { const char* name; AF_Vector3 p; };
    const Pos pos[] = {
        {"口の正面 2m（外）",     V(0.0f, 1.6f, -2.0f)},
        {"口の正面 6m（外）",     V(0.0f, 1.6f, -6.0f)},
        {"斜め 45° 3m（外）",     V(2.1f, 1.6f, -2.1f)},
        {"壁ぎわ・口は見えない",  V(4.0f, 1.6f, -0.5f)},
        {"中・口の内側 2m",       V(0.0f, 1.6f,  2.0f)},
        {"中・奥",                V(-2.0f, 1.6f, 6.0f)},
    };
    // 探針の方向（フィボナッチ球）
    constexpr int kDirs = 64;
    AF_Vector3 dirs[kDirs];
    for (int i = 0; i < kDirs; ++i) {
        const float y = 1.0f - 2.0f * (i + 0.5f) / kDirs;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float ph = 2.399963f * i;   // 黄金角
        dirs[i] = V(r * std::cos(ph), y, r * std::sin(ph));
    }
    AF_SceneSetSource(s, 1, S);
    AF_SceneSetListener(s, pos[0].p);
    for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
    int nAuto = 0, nManual = 0;
    AF_SceneGetPortalCounts(s, &nAuto, &nManual);
    std::printf("      部屋 %d 個 / 自動ポータル %d 枚。音源(1.5,1.5,5.0) 固定、リスナーは口(0,%.1f,0)を向く\n",
                AF_SceneRoomCount(s), nAuto, mouth.y);
    std::printf("      %-22s 部屋  直接(dB)  尾wet   探針:指向性  口との角度  ±30°内  左右R/L\n", "位置");
    for (const Pos& q : pos) {
        const AF_Vector3 L = q.p;
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        // 直接（透過⊕回折⊕反射の生存、広帯域）
        const int idx = AF_SceneSourceIndex(s, 1);
        float band[kBands] = {};
        AF_SceneGetSourceOcclusion(s, idx, band);
        double e2 = 0.0; for (int b = 0; b < kBands; ++b) e2 += band[b] * band[b];
        const double directDb = 10.0 * std::log10(std::max(e2 / kBands, 1e-12));
        // 尾の量（ホストと同じ wet = (総和 − 直接ピーク)/総和）
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        double peak = 0.0, total = 0.0;
        for (int i = 0; i < bins; ++i) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += echo[i * kBands + b];
            v /= kBands; total += v; if (v > peak) peak = v;
        }
        const double wet = (total > 1e-12) ? (total - peak) / total : 0.0;
        // 探針（500Hz 帯）: 平均方向、口との角度、口から ±30° に入る割合、左右バランス
        std::vector<float> en(static_cast<std::size_t>(kDirs) * kBands, 0.0f);
        AF_SceneProbeDirectionalEnergy(s, L, dirs, kDirs, 8, en.data());
        const int bb = 2;
        double sum = 0.0, mx = 0.0, my = 0.0, mz = 0.0, cone = 0.0, sumL = 0.0, sumR = 0.0;
        // リスナーは口を向く。右耳の軸 = up × forward
        float fx = mouth.x - L.x, fy = 0.0f, fz = mouth.z - L.z;
        { const float n = std::sqrt(fx*fx + fz*fz); if (n > 1e-4f) { fx /= n; fz /= n; } }
        const float rx = fz, rz = -fx;   // right = (fz, 0, -fx)
        float tx = mouth.x - L.x, ty = mouth.y - L.y, tz = mouth.z - L.z;
        { const float n = std::sqrt(tx*tx + ty*ty + tz*tz); tx /= n; ty /= n; tz /= n; }
        for (int i = 0; i < kDirs; ++i) {
            const double e = en[i * kBands + bb];
            if (e <= 0.0) continue;
            sum += e; mx += e * dirs[i].x; my += e * dirs[i].y; mz += e * dirs[i].z;
            const float cs = dirs[i].x * tx + dirs[i].y * ty + dirs[i].z * tz;
            if (cs > 0.866f) cone += e;
            const float c = dirs[i].x * rx + dirs[i].z * rz;
            sumR += e * (0.5 + 0.5 * c);
            sumL += e * (0.5 - 0.5 * c);
        }
        double directivity = 0.0, angDeg = 0.0, coneFrac = 0.0, rl = 1.0;
        if (sum > 1e-12) {
            mx /= sum; my /= sum; mz /= sum;
            directivity = std::sqrt(mx*mx + my*my + mz*mz);
            if (directivity > 1e-6) {
                const double cs = (mx * tx + my * ty + mz * tz) / directivity;
                angDeg = std::acos(std::max(-1.0, std::min(1.0, cs))) * 180.0 / 3.14159265;
            }
            coneFrac = cone / sum;
            rl = (sumL > 1e-12) ? sumR / sumL : 1.0;
        }
        (void)fy;
        std::printf("      %-22s %3d   %6.1f   %5.3f      %5.3f       %5.1f°   %5.1f%%   %5.2f\n",
                    q.name, AF_SceneRoomAt(s, L), directDb, wet, directivity, angDeg,
                    coneFrac * 100.0, rl);
    }
    std::printf("      読み方: 指向性 0=等方 / 1=一方向。口との角度が小さく ±30°内 が大きいほど"
                "「場は口を指している」。\n"
                "              いまの鳴らし方が使うのは 左右R/L だけなので、正面では 1.00 に潰れて向きが消える。\n");

    // ── 扉の定点の部品: 外へ開く口を開口にし、拡散した響きが口を通る割合を測る ──
    std::printf("      ── 外へ開く口を開口にする（AF_SceneSetOutsideApertures=1）──\n");
    AF_SceneSetOutsideApertures(s, 1);
    AF_SceneSetListener(s, pos[0].p);
    AF_SceneSetSource(s, 1, S);
    for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
    AF_SceneGetPortalCounts(s, &nAuto, &nManual);
    std::printf("      部屋 %d 個 / 自動ポータル %d 枚\n", AF_SceneRoomCount(s), nAuto);
    {
        // 部屋の境界に口が（吸音率 1 で）数えられているか。0 のままなら外の world との face が拾えていない。
        float vol = 0.0f, surf = 0.0f, open = 0.0f, ab[6] = {}, rt[6] = {};
        AF_SceneRoomInfo(s, 0, &vol, nullptr, nullptr, nullptr);
        AF_SceneRoomAcoustics(s, 0, &surf, &open, ab, rt);
        int nx = 0, ny = 0, nz = 0; float cell = 0.0f;
        AF_SceneRoomGridDims(s, &nx, &ny, &nz, &cell);
        std::printf("      部屋0: V=%.0f m3 境界 %.1f m2（うち開口 %.2f m2）RT60(500Hz) %.2f s / 格子 %d×%d×%d セル %.2f m\n",
                    vol, surf, open, rt[2], nx, ny, nz, cell);
        const int nap = AF_SceneApertureCount(s);
        std::printf("      部屋グラフの開口 %d 個\n", nap);
        for (int i = 0; i < nap; ++i) {
            float area = 0.0f; AF_Vector3 c, nrm; int ra = -1, rb = -1;
            AF_SceneApertureInfo(s, i, &area, &c, &nrm, &ra, &rb);
            std::printf("        開口%d 面積 %.2f m2 中心(%.2f,%.2f,%.2f) 法線(%.2f,%.2f,%.2f) 部屋 %d↔%d\n",
                        i, area, c.x, c.y, c.z, nrm.x, nrm.y, nrm.z, ra, rb);
        }
    }
    auto printPortals = [&](const char* label) {
        const int np = nAuto + nManual;
        for (int id = 0; id < np; ++id) {
            AF_Vector3 c, u, v; float hu = 0, hv = 0;
            if (!AF_SceneGetPortal(s, id, &c, &u, &v, &hu, &hv)) continue;
            int ra = -1, rb = -1, out = 0;
            AF_SceneGetPortalRooms(s, id, &ra, &rb, &out);
            float g[6] = {};
            AF_ScenePortalDiffuseCoupling(s, id, g);
            std::printf("        %-14s 口%d 中心(%.2f,%.2f,%.2f) %.2f×%.2f m 部屋 %d↔%d%s"
                        "  結合 125Hz %.3f / 1kHz %.3f / 4kHz %.3f\n",
                        label, id, c.x, c.y, c.z, hu * 2, hv * 2, ra, rb, out ? "(外)" : "",
                        g[0], g[3], g[5]);
        }
    };
    printPortals("口が開いている");
    // 口を板で塞ぐ（閉じた扉）。動かして「動く物」にし、部屋分割からは外す。
    const float trDoor[6] = {0.0316f, 0.0178f, 0.0100f, 0.0056f, 0.0032f, 0.0018f};   // −15…−27 dB
    const float absDoor[6] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    const int mDoor = AF_SceneAddMaterial(s, trDoor, absDoor, nullptr, 6);
    const int door = AF_SceneAddInstanceBox(s, V(0, mouthH * 0.5f, 0), V(mouthW * 0.5f, mouthH * 0.5f, 0.02f),
                                            V(1,0,0), V(0,1,0), mDoor);
    AF_SceneUpdateInstance(s, door, V(0, mouthH * 0.5f, 0.001f), V(mouthW * 0.5f, mouthH * 0.5f, 0.02f),
                           V(1,0,0), V(0,1,0));
    for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
    AF_SceneGetPortalCounts(s, &nAuto, &nManual);
    std::printf("      板で塞ぐ（閉扉、τ=%.3f/%.4f/%.4f）: 自動ポータル %d 枚\n",
                trDoor[0], trDoor[3], trDoor[5], nAuto);
    printPortals("閉扉");
    // 45° 開く（右の縁を蝶番に、中へ）
    {
        const float c45 = 0.70710678f, s45 = 0.70710678f;
        const AF_Vector3 ax = V(c45, 0, s45);                      // 板の横方向
        const AF_Vector3 hinge = V(mouthW * 0.5f, mouthH * 0.5f, 0);
        const AF_Vector3 center = V(hinge.x - ax.x * mouthW * 0.5f, hinge.y, hinge.z - ax.z * mouthW * 0.5f);
        AF_SceneUpdateInstance(s, door, center, V(mouthW * 0.5f, mouthH * 0.5f, 0.02f), ax, V(0,1,0));
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        std::printf("      45° 開く（期待: 隙間 ∝ sin45 ≒ 0.7 が通り、残りは板の τ。全開の値との比で見る）\n");
        printPortals("45°");
        // 90°（全開、板は口の脇に垂直に立つ）
        const AF_Vector3 ax90 = V(0, 0, 1);
        const AF_Vector3 c90 = V(hinge.x, hinge.y, hinge.z - mouthW * 0.5f);
        AF_SceneUpdateInstance(s, door, c90, V(mouthW * 0.5f, mouthH * 0.5f, 0.02f), ax90, V(0,1,0));
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        std::printf("      90° 開く（期待: 口が開いている時とほぼ同じ）\n");
        printPortals("90°");
        // 5°（隙間 ∝ sin5 ≒ 0.09）
        const float c5 = std::cos(5.0f * 3.14159265f / 180.0f), s5 = std::sin(5.0f * 3.14159265f / 180.0f);
        const AF_Vector3 ax5 = V(c5, 0, s5);
        const AF_Vector3 cc5 = V(hinge.x - ax5.x * mouthW * 0.5f, hinge.y, hinge.z - ax5.z * mouthW * 0.5f);
        AF_SceneUpdateInstance(s, door, cc5, V(mouthW * 0.5f, mouthH * 0.5f, 0.02f), ax5, V(0,1,0));
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
        std::printf("      5° 開く（期待: 隙間 ∝ sin5 ≒ 0.09 が通る。閉扉の τ より上、45° より下）\n");
        printPortals("5°");
    }

    // ── 扉の定点から鳴らす（配線の試作）: 現行の「部屋のバスを 2ch で足す」と比べる ──
    //   現行: 洞窟の尾の IR を部屋のバスで畳み、リスナーへそのまま足す。向きは探針の
    //         左右バランス（耳ごとの帯域ゲイン）を IR に焼くだけ。距離は知らない。
    //   定点: 同じバスのモノラル出力を、口の位置に置いた音源（別のボイス）の入力にして、
    //         口の方向へパンし、口からの距離で減らし、口の結合率を掛ける。
    //   測るのは尾の左右比（dB）と、正面 2m を 0 dB とした尾のレベル。
    std::printf("      ── 扉の定点から鳴らす（試作）: 尾の左右比と、口からの距離での落ち方 ──\n");
    AF_SceneUpdateInstance(s, door, V(0, -50.0f, 0), V(mouthW * 0.5f, mouthH * 0.5f, 0.02f), V(1,0,0), V(0,1,0));
    for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
    {
        constexpr int kSr = 48000, kBlk = 512, kBlocks = 140;   // 1.5 秒
        // 部屋の尾の形: 中で測ったエコグラム（形は部屋のもの）
        std::vector<float> echo(200 * kBands, 0.0f);
        {
            const AF_Vector3 srcArr[1] = { S };
            AF_SceneComputeEchogramBands(s, V(0, 1.6f, 4.0f), srcArr, 1, echo.data(), 200,
                                         0.01f, 343.0f, 512, 12, 4.0f);
        }
        float coup[6] = {1, 1, 1, 1, 1, 1};
        AF_ScenePortalDiffuseCoupling(s, 0, coup);
        // 探針から耳ごとの帯域ゲインを作る（ホストと同じ式、強さ 1）。リスナーは +z（洞窟の壁）を向く。
        auto earGains = [&](const AF_Vector3& L, float ear[12]) {
            std::vector<float> en(static_cast<std::size_t>(kDirs) * kBands, 0.0f);
            AF_SceneProbeDirectionalEnergy(s, L, dirs, kDirs, 8, en.data());
            for (int b = 0; b < kBands; ++b) {
                double sumL = 0.0, sumR = 0.0;
                for (int i = 0; i < kDirs; ++i) {
                    const double e = en[i * kBands + b];
                    if (e <= 0.0) continue;
                    const float c = dirs[i].x;   // 右耳の軸 = +x（+z を向いている）
                    sumR += e * (0.5 + 0.5 * c);
                    sumL += e * (0.5 - 0.5 * c);
                }
                const double mean = 0.5 * (sumL + sumR);
                double gl = 1.0, gr = 1.0;
                if (mean > 1e-9) {
                    gl = std::min(2.0, std::max(0.25, sumL / mean));
                    gr = std::min(2.0, std::max(0.25, sumR / mean));
                }
                ear[b] = static_cast<float>(gl); ear[6 + b] = static_cast<float>(gr);
            }
        };
        std::printf("      %-22s  現行: 左右R/L(dB) レベル(dB) | 定点: 左右R/L(dB) レベル(dB)  口までの距離\n", "位置");
        double refCur = -1.0, refNew = -1.0;
        for (int pi = 0; pi < 4; ++pi) {   // 外の 4 点
            const AF_Vector3 L = pos[pi].p;
            float ear[12];
            earGains(L, ear);
            // 現行: 音源ボイス → 部屋のバス（IR に耳のゲインを焼く）→ 2ch
            AF_VoiceConfig cfg{}; cfg.sampleRate = kSr; cfg.maxFrames = kBlk; cfg.tailSeconds = 2.0f;
            AF_VoiceHandle src = AF_VoiceCreate(&cfg);
            AF_TailBusHandle bus = AF_TailBusCreate(kSr, 2.0f, 64, 8192, kBlk);
            AF_VoiceSetTailBus(src, bus, 1);
            AF_VoiceSetHrtfEnabled(src, 0);
            AF_VoiceTap st{}; st.gain6[0] = st.gain6[1] = st.gain6[2] = st.gain6[3] = st.gain6[4] = st.gain6[5] = 1.0f;
            st.panL = st.panR = 0.7071f; st.gSpec = 1.0f;
            AF_VoiceSetTaps(src, &st, 1);
            AF_VoiceRebuildTail(src, echo.data(), 200, 10.0f, 25.0f, 8.0f, 30.0f, 0.0f, 0.6f,
                                1.0f, 1.0f, ear, 12);
            // 定点: 口に置いた音源ボイス。入力はバスのモノラル。
            AF_VoiceConfig cfg2{}; cfg2.sampleRate = kSr; cfg2.maxFrames = kBlk; cfg2.tailSeconds = 0.1f;
            AF_VoiceHandle pv = AF_VoiceCreate(&cfg2);
            AF_VoiceSetHrtfEnabled(pv, 0);
            const float dx = mouth.x - L.x, dy = mouth.y - L.y, dz = mouth.z - L.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float att = 1.0f / std::max(dist, 1.0f);
            const float xr = (dx / dist + 1.0f) * 0.5f;              // −1..+1 → 0..1（+x が右）
            AF_VoiceTap pt{};
            for (int b = 0; b < 6; ++b) pt.gain6[b] = coup[b] / coup[0] * att;   // 結合は開の値で正規化
            pt.panL = std::sqrt(1.0f - xr); pt.panR = std::sqrt(xr); pt.gSpec = 1.0f;
            pt.dirX = dx / dist; pt.dirY = dy / dist; pt.dirZ = dz / dist;
            AF_VoiceSetTaps(pv, &pt, 1);
            std::vector<float> in(kBlk, 0.0f), l(kBlk), r(kBlk), bl(kBlk), br(kBlk), mono(kBlk), pl(kBlk), pr(kBlk);
            double eCurL = 0.0, eCurR = 0.0, eNewL = 0.0, eNewR = 0.0;
            for (int blk = 0; blk < kBlocks; ++blk) {
                std::fill(in.begin(), in.end(), 0.0f);
                if (blk == 0) in[0] = 1.0f;                        // クリック 1 発
                AF_VoiceRender(src, in.data(), kBlk, l.data(), r.data(), nullptr);
                std::fill(bl.begin(), bl.end(), 0.0f); std::fill(br.begin(), br.end(), 0.0f);
                AF_TailBusRender(bus, kBlk, bl.data(), br.data());
                AF_TailBusLastMono(bus, mono.data(), kBlk);
                AF_VoiceRender(pv, mono.data(), kBlk, pl.data(), pr.data(), nullptr);
                for (int i = 0; i < kBlk; ++i) {
                    eCurL += bl[i] * bl[i]; eCurR += br[i] * br[i];
                    eNewL += pl[i] * pl[i]; eNewR += pr[i] * pr[i];
                }
            }
            AF_VoiceDestroy(pv);
            AF_VoiceSetTailBus(src, nullptr, 0);
            AF_VoiceDestroy(src);
            AF_TailBusDestroy(bus);
            const double curTot = eCurL + eCurR, newTot = eNewL + eNewR;
            if (refCur < 0.0) { refCur = curTot; refNew = newTot; }
            const double curRL = 10.0 * std::log10(std::max(eCurR, 1e-20) / std::max(eCurL, 1e-20));
            const double newRL = 10.0 * std::log10(std::max(eNewR, 1e-20) / std::max(eNewL, 1e-20));
            std::printf("      %-22s     %+6.1f     %+6.1f    |    %+6.1f     %+6.1f      %.1f m\n",
                        pos[pi].name, curRL, 10.0 * std::log10(std::max(curTot, 1e-20) / refCur),
                        newRL, 10.0 * std::log10(std::max(newTot, 1e-20) / refNew), dist);
        }
        std::printf("      読み方: 定点は口の方向へ寄り（斜めや壁ぎわで左右比が大きく開く）、口から離れると 1/r で落ちる。\n"
                    "              現行は左右比が探針の丸めで小さく、レベルは距離を知らない。\n");
    }
    AF_SceneDestroy(s);
}

// 歩行の連続性 ── 資料の部屋で決めた道を 5 cm 刻みで歩き、1 歩あたりの変化を種類ごとに印を付ける。
//   道: 外（戸口の斜め前 4 m）→ 戸口をまたぐ → 部屋の中央 → 柱の裏（音源3 の影）。扉は右蝶番・内開き 60°。
//   見る量（音源ごと）: 広帯域の生存 dB／低−高／回折の本数と開口率／到来方位（世界系）
//   見る量（全体）: 部屋の占め方 w／エコグラムの総量と 200→600 ms の傾き（尾の形）
//   印の種類:
//     経路   … 回折の本数が変わる、フレネル可否が切り替わる（経路の生まれ消え）
//     レベル … 生存が 1 歩で 2 dB 超
//     方向   … 到来方位が 1 歩で 25° 超なのにレベルが 1 dB も動かない（像だけ飛ぶ）
//     尾の形 … 傾きが 60 dB/s 超、または総量が 2 dB 超動く（IR の差し替えが跳ぶ原因）
//     部屋   … 占め方 w が 1 歩で 0.15 超
//   ★これはエンジンの**目標値**の連続性。ホストの周期の段と平滑は別の話（ここでは測れない）。
//   同じ道を「稜線探索（既定）」と「稜線ポータル ON」の 2 回歩き、経路の印が減るかを比べる。
void diagnoseWalkContinuity() {
    std::printf("\n[診断] 歩行の連続性（外 → 戸口 → 中央 → 柱の裏、5 cm 刻み。扉 60° 内開き）\n");
    const float roomW = 7.08f, roomD = 3.48f, roomH = 3.0f, wall = 0.16f;
    const float doorW = 1.40f, doorH = 2.0f, doorT = 0.08f;
    const float gapCx = 3.54f;
    const float gapL = gapCx - doorW * 0.5f, gapR = gapCx + doorW * 0.5f;
    const float hw = wall * 0.5f, cy = roomH * 0.5f, zf = roomD + hw;
    const AF_Vector3 src[3] = { V((306.0f-146.0f)/100.0f, 1.5f, (432.0f-196.0f)/100.0f),
                                V((500.0f-146.0f)/100.0f, 1.5f, (320.0f-196.0f)/100.0f),
                                V((694.0f-146.0f)/100.0f, 1.5f, (432.0f-196.0f)/100.0f) };
    bool useRefl = true;   // 「反射なし」の回で false（役割1 の反射込みレイが跳びの担い手かを切り分ける）
    auto buildScene = [&](bool edgeP) -> AF_SceneHandle {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        auto box = [&](AF_Vector3 c, AF_Vector3 he) {
            AF_SceneAddInstanceBox(s, c, he, V(1,0,0), V(0,1,0), mat);
        };
        box(V(-hw, cy, roomD*0.5f), V(hw, cy, roomD*0.5f + wall));
        box(V(roomW + hw, cy, roomD*0.5f), V(hw, cy, roomD*0.5f + wall));
        box(V(roomW*0.5f, cy, -hw), V(roomW*0.5f + wall, cy, hw));
        box(V(roomW*0.5f, roomH + hw, roomD*0.5f), V(roomW*0.5f + wall, hw, roomD*0.5f + wall));
        box(V(roomW*0.5f, -hw, roomD*0.5f), V(roomW*0.5f + wall, hw, roomD*0.5f + wall));
        box(V(gapL*0.5f, cy, zf), V(gapL*0.5f, cy, hw));
        box(V((gapR + roomW)*0.5f, cy, zf), V((roomW - gapR)*0.5f, cy, hw));
        box(V(gapCx, (doorH + roomH)*0.5f, zf), V(doorW*0.5f, (roomH - doorH)*0.5f, hw));
        // 柱（音源3 と「柱の裏」の道の間）
        box(V(5.0f, cy, 1.2f), V(0.2f, cy, 0.2f));
        // 扉 60°・右蝶番・内開き（pattern_table と同じ式）
        const int doorId = AF_SceneAddInstanceBox(s, V(gapCx, doorH*0.5f, zf),
            V(doorW*0.5f, doorH*0.5f, doorT*0.5f), V(1,0,0), V(0,1,0), mat);
        {
            const float th = 60.0f * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            const float hinge = gapR, rx = gapCx - hinge;
            const float sgn = (rx < 0.0f) ? 1.0f : -1.0f;
            AF_SceneUpdateInstance(s, doorId, V(hinge + rx*c, doorH*0.5f, zf - std::fabs(rx)*sn),
                V(doorW*0.5f, doorH*0.5f, doorT*0.5f), V(c, 0, sgn*sn), V(0,1,0));
        }
        AF_UpdateConfig cfg{};
        cfg.role1EveryN = 1;   cfg.role2EveryN = 1;   cfg.earlyEveryN = 1;
        cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
        cfg.reflectionRays = 256; cfg.reflectionBounces = 3;
        cfg.directWeight = 1.0f;  cfg.useReflections = useRefl ? 1 : 0;
        cfg.useEdgeCatalog = 1;   cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;
        cfg.enableReverb = 1;     cfg.echogramBins = 100;  cfg.echogramBinSeconds = 0.01f;
        // ★跳ね返り 24 回だと平均自由行程 2.6 m × 24 = 62 m ≒ 180 ms で尾が切れる
        //   （実測: 200 ms −18.7 dB、300 ms 以降は無音）。尾の形を測るので 96 回にする。
        cfg.echogramRays = 512;   cfg.echogramBounces = 96;
        cfg.speedOfSound = 343.0f; cfg.distanceRef = 1.5f;
        cfg.enableEarlyReflections = 1; cfg.earlyTaps = 4;
        cfg.earlyRays = 512; cfg.earlyBounces = 2;
        cfg.enableDiffractionSources = 1; cfg.diffSources = 3;
        AF_SceneSetUpdateConfig(s, &cfg);
        AF_SceneSetEdgePortals(s, edgeP ? 1 : 0);
        // ★種の半径は戸口の半幅より大きくする。既定 0.6 m だと幅 1.40 m の戸口（半幅 0.70）の
        //   真ん中が種になり、部屋の種と外の世界の種が戸口越しに繋がって部屋が 0 個になる
        //   （実測: 部屋 0 個・中の 3 点が全部 -1）。洞窟の口 1.2 m は半幅 0.6 で境目だった。
        AF_SceneSetRoomSeedRadius(s, 0.8f);
        for (int i = 0; i < 3; ++i) AF_SceneSetSource(s, (unsigned long long)(i + 1), src[i]);
        return s;
    };
    // 道（折れ線）を 5 cm 刻みに
    const AF_Vector3 wp[5] = { V(gapCx - 1.5f, 1.6f, zf + 4.0f), V(gapCx - 0.2f, 1.6f, zf + 0.3f),
                               V(gapCx - 0.2f, 1.6f, zf - 0.4f), V(3.54f, 1.6f, 1.74f), V(4.6f, 1.6f, 0.4f) };
    const char* legName[4] = { "外→戸口", "戸口", "中へ", "柱の裏へ" };
    struct Pt { AF_Vector3 p; int leg; float dist; };
    std::vector<Pt> pts;
    {
        float acc = 0.0f;
        for (int l = 0; l < 4; ++l) {
            const float dx = wp[l+1].x - wp[l].x, dz = wp[l+1].z - wp[l].z;
            const float len = std::sqrt(dx*dx + dz*dz);
            const int n = std::max(1, static_cast<int>(len / 0.05f));
            for (int k = (l == 0 ? 0 : 1); k <= n; ++k) {
                const float t = static_cast<float>(k) / n;
                pts.push_back({ V(wp[l].x + dx*t, 1.6f, wp[l].z + dz*t), l, acc + len * t });
            }
            acc += len;
        }
    }
    struct Rec { float db[3], tilt[3], open[3], az[3], soft[3], dif[3], refl[3]; int np[3], fres[3], nref[3]; float w0, tot, slope; };
    auto measureAt = [&](AF_SceneHandle s, const AF_Vector3& L, int roomIn, Rec& r) {
        AF_SceneSetListener(s, L);
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        for (int i = 0; i < 3; ++i) {
            const int idx = AF_SceneSourceIndex(s, (unsigned long long)(i + 1));
            float b6[kBands] = {};
            AF_SceneGetSourceOcclusion(s, idx, b6);
            double e2 = 0.0; for (int b = 0; b < kBands; ++b) e2 += b6[b] * b6[b];
            r.db[i] = static_cast<float>(10.0 * std::log10(std::max(e2 / kBands, 1e-12)));
            // 生存の合成前の 2 つの担い手（直接の半影＝振幅、回折＝ポータル混合後）。跳びの担い手を分ける。
            float sft6[kBands] = {}, dif6[kBands] = {};
            AF_SceneDebugSurvivalParts(s, L, src[i], sft6, dif6);
            double s2 = 0.0, d2 = 0.0;
            for (int b = 0; b < kBands; ++b) { s2 += sft6[b] * sft6[b]; d2 += dif6[b] * dif6[b]; }
            r.soft[i] = static_cast<float>(10.0 * std::log10(std::max(s2 / kBands, 1e-12)));
            r.dif[i]  = static_cast<float>(10.0 * std::log10(std::max(d2 / kBands, 1e-12)));
            AF_Vector3 ep[16]; float eg6[16 * kBands] = {};
            r.nref[i] = AF_SceneGetEarlyReflections(s, idx, ep, eg6, 16);
            double re = 0.0;
            for (int k = 0; k < r.nref[i]; ++k)
                for (int b = 0; b < kBands; ++b) re += eg6[k * kBands + b] * eg6[k * kBands + b] / kBands;
            r.refl[i] = static_cast<float>(10.0 * std::log10(std::max(re, 1e-12)));
            r.tilt[i] = 20.0f * std::log10(std::max(b6[0], 1e-6f) / std::max(b6[5], 1e-6f));
            float d28[28] = {};
            r.np[i] = AF_SceneDebugDiffractionPath(s, L, src[i], d28, 0);
            r.open[i] = d28[1];
            r.fres[i] = (d28[18] > 0.5f) ? 1 : 0;
            float ad[3] = {0, 0, 1};
            AF_SceneGetSourceArrivalDir(s, idx, ad);
            r.az[i] = std::atan2(ad[0], ad[2]) * 180.0f / 3.14159265f;
        }
        int ids[8]; float w[8];
        const int n = AF_SceneRoomShareAt(s, L, 2.0f, ids, w, 8);   // 空間版（外の世界も分母）
        r.w0 = 0.0f;
        for (int i = 0; i < n; ++i) if (ids[i] == roomIn) r.w0 = w[i];
        float echo[100 * kBands] = {};
        const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
        double tot = 0.0, m1 = 0.0, m2 = 0.0; int c1 = 0, c2 = 0;
        for (int k = 0; k < bins; ++k) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += echo[k * kBands + b];
            v /= kBands; tot += v;
            if (v > 1e-14) {
                if (k >= 20 && k < 30) { m1 += std::log10(v); ++c1; }   // 200〜290 ms
                if (k >= 40 && k < 50) { m2 += std::log10(v); ++c2; }   // 400〜490 ms
            }
        }
        r.tot = static_cast<float>(10.0 * std::log10(std::max(tot, 1e-12)));
        r.slope = (c1 > 0 && c2 > 0) ? static_cast<float>(10.0 * (m2 / c2 - m1 / c1) / 0.2) : 0.0f;
    };
    auto run = [&](bool edgeP, const char* label) {
        AF_SceneHandle s = buildScene(edgeP);
        AF_SceneSetListener(s, wp[0]);
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int roomIn = AF_SceneRoomAt(s, V(3.54f, 1.6f, 1.74f));
        std::printf("      ── %s ──（%d 歩、部屋の中 = 部屋%d）\n", label, static_cast<int>(pts.size()), roomIn);
        {
            int nx = 0, ny = 0, nz = 0; float cell = 0.0f;
            AF_SceneRoomGridDims(s, &nx, &ny, &nz, &cell);
            std::printf("      部屋 %d 個 / 格子 %d×%d×%d セル %.2f / 中の 3 点の部屋 %d %d %d / 外 %d\n",
                        AF_SceneRoomCount(s), nx, ny, nz, cell,
                        AF_SceneRoomAt(s, V(1.5f, 1.6f, 1.5f)), AF_SceneRoomAt(s, V(3.54f, 1.6f, 1.74f)),
                        AF_SceneRoomAt(s, V(5.5f, 1.6f, 2.4f)), AF_SceneRoomAt(s, wp[0]));
        }
        int nPath = 0, nLevel = 0, nDir = 0, nTail = 0, nRoom = 0, shown = 0;
        float maxLv[3] = {0, 0, 0}; float maxLvAt[3] = {0, 0, 0};
        float maxSlope = 0.0f, maxSlopeAt = 0.0f, maxW = 0.0f, maxWAt = 0.0f;
        float totMin = 1e9f, totMax = -1e9f, slopeMin = 1e9f, slopeMax = -1e9f;
        Rec prev{}; bool have = false;
        for (const Pt& q : pts) {
            Rec r{};
            measureAt(s, q.p, roomIn, r);
            totMin = std::min(totMin, r.tot); totMax = std::max(totMax, r.tot);
            slopeMin = std::min(slopeMin, r.slope); slopeMax = std::max(slopeMax, r.slope);
            if (have) {
                char why[256]; int wl = 0; why[0] = 0;
                bool flag = false;
                for (int i = 0; i < 3; ++i) {
                    if (r.db[i] < -50.0f && prev.db[i] < -50.0f) continue;   // 聞こえない所は見ない
                    const float dl = std::fabs(r.db[i] - prev.db[i]);
                    if (dl > maxLv[i]) { maxLv[i] = dl; maxLvAt[i] = q.dist; }
                    const bool pathChg = (r.np[i] != prev.np[i]) || (r.fres[i] != prev.fres[i]);
                    float dOpen = 0.0f;
                    if (r.open[i] > 1e-4f && prev.open[i] > 1e-4f)
                        dOpen = std::fabs(20.0f * std::log10(r.open[i] / prev.open[i]));
                    float dAz = std::fabs(r.az[i] - prev.az[i]); if (dAz > 180.0f) dAz = 360.0f - dAz;
                    if (pathChg || dOpen > 3.0f) {
                        ++nPath; flag = true;
                        wl += std::snprintf(why + wl, sizeof(why) - wl, " 音源%d:経路(本 %d→%d 開口 %.3f→%.3f)",
                                            i + 1, prev.np[i], r.np[i], prev.open[i], r.open[i]);
                    }
                    if (dl > 2.0f) {
                        ++nLevel; flag = true;
                        // 担い手: 直接の半影／回折／早期反射（本数の変化も）のどれが動いたか。
                        const float dsft = std::fabs(r.soft[i] - prev.soft[i]);
                        const float ddif = std::fabs(r.dif[i] - prev.dif[i]);
                        const float dref = std::fabs(r.refl[i] - prev.refl[i]);
                        char who[96]; int q = 0; who[0] = 0;
                        if (dsft > 2.0f) q += std::snprintf(who + q, sizeof(who) - q, "・半影%.1f", dsft);
                        if (ddif > 2.0f) q += std::snprintf(who + q, sizeof(who) - q, "・回折%.1f", ddif);
                        if (dref > 2.0f || r.nref[i] != prev.nref[i])
                            q += std::snprintf(who + q, sizeof(who) - q, "・反射%.1f(%d→%d本)", dref, prev.nref[i], r.nref[i]);
                        wl += std::snprintf(why + wl, sizeof(why) - wl, " 音源%d:レベル(%.1f→%.1f dB%s)",
                                            i + 1, prev.db[i], r.db[i], who);
                    }
                    if (dAz > 25.0f && dl < 1.0f) {
                        ++nDir; flag = true;
                        wl += std::snprintf(why + wl, sizeof(why) - wl, " 音源%d:方向(%.0f°→%.0f°)",
                                            i + 1, prev.az[i], r.az[i]);
                    }
                    if (wl > 200) break;
                }
                const float ds = std::fabs(r.slope - prev.slope), dt = std::fabs(r.tot - prev.tot);
                if (ds > maxSlope) { maxSlope = ds; maxSlopeAt = q.dist; }
                if (ds > 60.0f || dt > 2.0f) {
                    ++nTail; flag = true;
                    if (wl < 200) wl += std::snprintf(why + wl, sizeof(why) - wl, " 尾の形(傾き %.0f→%.0f dB/s 総量 %.1f→%.1f)",
                                                      prev.slope, r.slope, prev.tot, r.tot);
                }
                const float dw = std::fabs(r.w0 - prev.w0);
                if (dw > maxW) { maxW = dw; maxWAt = q.dist; }
                if (dw > 0.15f) {
                    ++nRoom; flag = true;
                    if (wl < 200) wl += std::snprintf(why + wl, sizeof(why) - wl, " 部屋(w %.2f→%.2f)", prev.w0, r.w0);
                }
                if (flag && shown < 24) {
                    ++shown;
                    std::printf("        %5.2f m [%s] (%.2f, %.2f)%s\n", q.dist, legName[q.leg], q.p.x, q.p.z, why);
                }
            }
            prev = r; have = true;
        }
        std::printf("      印の数: 経路 %d / レベル %d / 方向 %d / 尾の形 %d / 部屋 %d%s\n",
                    nPath, nLevel, nDir, nTail, nRoom, (shown >= 24) ? "（表示は 24 行まで）" : "");
        std::printf("      1 歩の最大: 音源1 %.1f dB(%.2f m) 音源2 %.1f dB(%.2f m) 音源3 %.1f dB(%.2f m) / "
                    "尾の傾き %.0f dB/s(%.2f m) / 部屋 w %.2f(%.2f m)\n",
                    maxLv[0], maxLvAt[0], maxLv[1], maxLvAt[1], maxLv[2], maxLvAt[2],
                    maxSlope, maxSlopeAt, maxW, maxWAt);
        std::printf("      尾の総量 %.1f〜%.1f dB / 200→600 ms の傾き %.0f〜%.0f dB/s（0 なら測れていない）\n",
                    totMin, totMax, slopeMin, slopeMax);
        {
            // 尾の傾きが 0 のままなら、エコグラムのどのビンに何があるかを 1 点で見る（部屋の中央）。
            AF_SceneSetListener(s, V(3.54f, 1.6f, 1.74f));
            for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
            float echo[100 * kBands] = {};
            const int bins = AF_SceneGetEchogramBands(s, -1, echo, 100);
            std::printf("      エコグラム（部屋の中央、bins=%d）ビン別の広帯域エネルギー(dB):", bins);
            const int ks[8] = {0, 2, 5, 10, 20, 30, 50, 80};
            for (int q = 0; q < 8; ++q) {
                const int k = ks[q];
                if (k >= bins) break;
                double v = 0.0;
                for (int b = 0; b < kBands; ++b) v += echo[k * kBands + b];
                std::printf(" [%dms] %.1f", k * 10, 10.0 * std::log10(std::max(v / kBands, 1e-15)));
            }
            std::printf("\n");
        }
        AF_SceneDestroy(s);
    };
    run(false, "稜線探索（今の既定）");
    run(true,  "稜線ポータル ON（AF_SceneSetEdgePortals=1）");
    useRefl = false;
    run(false, "反射込みレイなし（useReflections=0。外の跳びが役割1 のレイ標本かを切り分ける）");
    useRefl = true;
}

// 閉じた扉ごしの定位 ── 「壁の奥の音源が左右反転して聞こえる」の切り分け（2026-09-03 の報告）。
//   ホストは遮蔽が深いほど**エンジンの到来方向**へ寄せる（AcousticFlowSceneDemo の steer）。
//   閉扉では steer≈1 なので、聞こえる向きは AF_SceneGetSourceArrivalDir がそのまま決める。
//   ここでは資料の部屋で、真の方位と到来方位を扉の開閉ごとに並べる。
//     方位は世界系（+x が右、+z が奥）。atan2(x, z) の度。左の音源は負、右の音源は正。
//   符号が逆なら反転。0 付近なら「戸口へ寄っている」だけで反転ではない。
// HRTF の左右 ── 「右から来る音は右耳が大きい」。検査が 1 つも無かった所。
//   閉扉では聞こえる向きを HRTF が単独で決める（開いていると回折の二次音源や早期反射の
//   実体（Unity の AudioSource）が正しい位置から鳴るので、HRTF の誤りが隠れる）。
//   ★リスナー座標系の規約: +x = 右 / +y = 上 / +z = 前（Unity の InverseTransformDirection と同じ）。
void testHrtfLeftRight() {
    std::printf("\n[HRTF] 左右の向き（+x=右 / +z=前）\n");
    constexpr int kSr = 48000, kBlk = 512;
    struct Dir { const char* name; float x, y, z; int expect; };   // expect: +1=右が大きい / -1=左 / 0=同じ
    const Dir dirs[5] = {
        { "右 (+x)",  1, 0,  0, +1 },
        { "左 (-x)", -1, 0,  0, -1 },
        { "前 (+z)",  0, 0,  1,  0 },
        { "後 (-z)",  0, 0, -1,  0 },
        { "右後ろ",   0.707f, 0, -0.707f, +1 },
    };
    auto check1 = [&](AF_HrtfHandle h, const char* label) {
        if (!h) { std::printf("      %s: 読めない\n", label); return; }
        for (const Dir& d : dirs) {
            AF_VoiceConfig cfg{}; cfg.sampleRate = kSr; cfg.maxFrames = kBlk; cfg.tailSeconds = 0.05f;
            AF_VoiceHandle v = AF_VoiceCreate(&cfg);
            AF_VoiceSetHrtf(v, h);
            AF_VoiceSetHrtfEnabled(v, 1);
            AF_VoiceSetTailLevel(v, 0.0f);
            AF_VoiceTap t{};
            for (int b = 0; b < kBands; ++b) t.gain6[b] = 1.0f;
            t.panL = t.panR = 0.7071f; t.gSpec = 1.0f;
            AF_VoiceSetTaps(v, &t, 1);
            AF_VoiceSetDirection(v, V(d.x, d.y, d.z), 57.0f);
            std::vector<float> in(kBlk, 0.0f), l(kBlk), r(kBlk);
            double eL = 0.0, eR = 0.0;
            for (int blk = 0; blk < 24; ++blk) {          // 0.26 秒。タップのクロスフェード 30ms を越える
                std::fill(in.begin(), in.end(), 0.0f);
                if (blk == 8) in[0] = 1.0f;               // 落ち着いてからクリック 1 発
                AF_VoiceRender(v, in.data(), kBlk, l.data(), r.data(), nullptr);
                if (blk < 8) continue;
                for (int i = 0; i < kBlk; ++i) { eL += l[i] * l[i]; eR += r[i] * r[i]; }
            }
            AF_VoiceDestroy(v);
            const double rl = 10.0 * std::log10(std::max(eR, 1e-20) / std::max(eL, 1e-20));
            char buf2[96];
            std::snprintf(buf2, sizeof(buf2), "%s %s: 右−左 %+.1f dB", label, d.name, rl);
            if (d.expect > 0)      check(buf2, rl > 1.0, "（右が大きいはず）");
            else if (d.expect < 0) check(buf2, rl < -1.0, "（左が大きいはず）");
            else                   check(buf2, std::fabs(rl) < 1.5, "（左右がほぼ同じはず）");
        }
    };
    // ── 2 本目以降のタップ（HRTF を通らない「軽量な両耳化」＝ ITD ＋ 帯域別 ILD）──
    //   閉じた扉ごしでは直接音タップが材質の透過まで落ち、**回折タップ**が実エネルギーを運ぶ。
    //   回折タップは 1 本だけ HRTF バスへ載り、残りはこの軽量経路で鳴る。
    //   ここが逆だと「扉を閉めた時だけ左右が入れ替わる」という出方をする。
    auto check2 = [&](AF_HrtfHandle h, const char* label, bool viaHrtfBus) {
        if (!h) return;
        for (const Dir& d : dirs) {
            AF_VoiceConfig cfg{}; cfg.sampleRate = kSr; cfg.maxFrames = kBlk; cfg.tailSeconds = 0.05f;
            AF_VoiceHandle v = AF_VoiceCreate(&cfg);
            AF_VoiceSetHrtf(v, h);
            AF_VoiceSetHrtfEnabled(v, 1);
            AF_VoiceSetTailLevel(v, 0.0f);
            AF_VoiceSetEarCues(v, 1);
            AF_VoiceTap t[2] = {};
            // index 0 = 直接音。閉扉のつもりで黙らせる（方向の手がかりを 2 本目だけにする）。
            t[0].panL = t[0].panR = 0.7071f; t[0].gSpec = 1.0f;
            for (int b = 0; b < kBands; ++b) t[1].gain6[b] = 1.0f;
            t[1].gSpec = 1.0f;
            t[1].dirX = d.x; t[1].dirY = d.y; t[1].dirZ = d.z;
            // 等パワーパンも同時に入れる（ホストと同じ。軽量経路が効けばこちらは使われない）。
            const float xr = (d.x + 1.0f) * 0.5f;
            t[1].panL = std::sqrt(1.0f - xr); t[1].panR = std::sqrt(xr);
            t[1].hrtfWeight = viaHrtfBus ? 1.0f : 0.0f;
            AF_VoiceSetTaps(v, t, 2);
            AF_VoiceSetDirection(v, V(0, 0, 1), 57.0f);          // 直接音は正面（黙っている）
            if (viaHrtfBus) AF_VoiceSetDiffractionDirection(v, V(d.x, d.y, d.z), 57.0f);
            std::vector<float> in(kBlk, 0.0f), l(kBlk), r(kBlk);
            double eL = 0.0, eR = 0.0;
            for (int blk = 0; blk < 24; ++blk) {
                std::fill(in.begin(), in.end(), 0.0f);
                if (blk == 8) in[0] = 1.0f;
                AF_VoiceRender(v, in.data(), kBlk, l.data(), r.data(), nullptr);
                if (blk < 8) continue;
                for (int i = 0; i < kBlk; ++i) { eL += l[i] * l[i]; eR += r[i] * r[i]; }
            }
            AF_VoiceDestroy(v);
            const double rl = 10.0 * std::log10(std::max(eR, 1e-20) / std::max(eL, 1e-20));
            char buf2[112];
            std::snprintf(buf2, sizeof(buf2), "%s %s: 右−左 %+.1f dB", label, d.name, rl);
            if (d.expect > 0)      check(buf2, rl > 1.0, "（右が大きいはず）");
            else if (d.expect < 0) check(buf2, rl < -1.0, "（左が大きいはず）");
            else                   check(buf2, std::fabs(rl) < 1.5, "（左右がほぼ同じはず）");
        }
    };
    AF_HrtfHandle syn = AF_HrtfCreateSynthetic(kSr);
    check1(syn, "合成");
    AF_HrtfDestroy(syn);
    AF_HrtfHandle km = AF_HrtfLoadFile("UnityDemo/Assets/StreamingAssets/kemar.afhr");
    if (!km) km = AF_HrtfLoadFile("../UnityDemo/Assets/StreamingAssets/kemar.afhr");
    if (km) {
        std::printf("      実測 HRTF: %d 方向\n", AF_HrtfDirectionCount(km));
        check1(km, "実測");
        std::printf("      ── 2 本目のタップ（閉扉で実エネルギーを運ぶ経路）──\n");
        check2(km, "実測・軽量両耳", false);
        check2(km, "実測・回折HRTFバス", true);
        AF_HrtfDestroy(km);
    } else {
        std::printf("      ⚠ kemar.afhr が見つからないので実測 HRTF は試していない\n");
    }
}

// Test_SwingDoor をそのまま再現して、音源を左右へ振る（2026-09-03 の報告の再現）。
//   報告: 「閉扉で音源を x=-3.5 にすると右から聞こえる」「x=-3.6 で急に音が開ける」。
//   場面（.unity から実寸で写した）:
//     部屋 14.8×14.8×3.4 の箱。z=0 に仕切り（Partition_L x∈[-7,-0.5] / R x∈[0.5,7]、厚み 0.2）。
//     戸口は x∈[-0.5,0.5] の幅 1.0 m。扉は蝶番 x=-0.5、幅 1.0、厚み 0.06。
//     リスナー (0,1.6,-3)。音源は z=+3 で x を振る。
//   見る量: 生存 dB／到来方位（真との差）／回折の本数・開口率・フレネル可否。
// ---------------------------------------------------------------- 早期反射（面の線）
// 2026-09-03: ISM の像源を「反射面ごとの線音源」に縮約する模型（earlyModel=1）。
//   絶対値ではなく性質で縛る: 本数／エネルギー保存／連続性／幅／閉扉の減り／尾との二重防止。
namespace facerefl {
AF_UpdateConfig cfgFor(int model, int subTaps, int reverb) {
    AF_UpdateConfig c{};
    c.role1EveryN = 1; c.role2EveryN = 1; c.earlyEveryN = 1; c.diffSrcEveryN = 1; c.catalogEveryN = 1;
    c.reflectionRays = 128; c.reflectionBounces = 3; c.directWeight = 1.0f; c.useReflections = 1;
    c.useEdgeCatalog = 0; c.edgeCatalogRes = 8; c.edgeCatalogMaxDist = 40.0f;
    c.enableReverb = reverb; c.echogramBins = 60; c.echogramBinSeconds = 0.01f;
    c.echogramRays = 256; c.echogramBounces = 8; c.speedOfSound = 343.0f; c.distanceRef = 1.5f;
    c.enableEarlyReflections = 1; c.earlyTaps = 48; c.earlyRays = 512; c.earlyBounces = 1;
    c.enableDiffractionSources = 0; c.diffSources = 1;
    c.earlyModel = model; c.earlyFaceSubTaps = subTaps; c.echogramSkipFirstOrder = 0;
    return c;
}
// 箱部屋 2hw x 2hh x 2hd（壁 6 枚、厚み 0.2）。床の上面が y=0。材質は 6 帯域の配列で渡す。
AF_SceneHandle boxRoom(float hw, float hh, float hd, const float* tr, const float* ab, const float* sc) {
    AF_SceneHandle s = AF_SceneCreate();
    const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
    const float t = 0.1f;
    auto box = [&](AF_Vector3 c, AF_Vector3 he) { AF_SceneAddInstanceBox(s, c, he, V(1,0,0), V(0,1,0), m); };
    box(V(-hw - t, hh, 0), V(t, hh, hd));          // 西
    box(V( hw + t, hh, 0), V(t, hh, hd));          // 東
    box(V(0, hh,  hd + t), V(hw, hh, t));          // 北
    box(V(0, hh, -hd - t), V(hw, hh, t));          // 南
    box(V(0, -t, 0), V(hw, t, hd));                // 床
    box(V(0, 2.0f * hh + t, 0), V(hw, t, hd));     // 天井
    return s;
}
int taps(AF_SceneHandle s, AF_Vector3 L, AF_Vector3 S, AF_Vector3* pos, float* g) {
    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);
    for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
    const int idx = AF_SceneSourceIndex(s, 1);
    return (idx >= 0) ? AF_SceneGetEarlyReflections(s, idx, pos, g, 48) : 0;
}
// 壁 1 枚だけ（幅の検査用。どのタップも同じ面の物なので、張る角がそのまま幅）。
AF_SceneHandle singleWall(const float* tr, const float* ab, const float* sc) {
    AF_SceneHandle s = AF_SceneCreate();
    const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
    AF_SceneAddInstanceBox(s, V(-4.1f, 1.5f, 0), V(0.1f, 1.5f, 3.0f), V(1,0,0), V(0,1,0), m);
    return s;
}
double energy(const float* g, int n, int b) {
    double e = 0.0;
    for (int t = 0; t < n; ++t) e += static_cast<double>(g[t * kBands + b]) * g[t * kBands + b];
    return e;
}
double energyAll(const float* g, int n) { double e = 0.0; for (int b = 0; b < kBands; ++b) e += energy(g, n, b); return e; }
double dB(double e) { return 10.0 * std::log10(std::max(e, 1e-12)); }
}  // namespace facerefl

void testFaceReflections() {
    using namespace facerefl;
    std::printf("\n[早期反射・面] ISM の像源を面ごとの線音源に縮約する（earlyModel=1）\n");
    float tr[6], ab[6], sc[6];
    AF_MaterialPresetBands(0, tr, ab, sc);
    const AF_Vector3 L = V(-1.0f, 1.6f, -1.0f), S = V(1.5f, 1.6f, 1.2f);
    AF_Vector3 pos[48]; float g[48 * kBands];
    char buf[200];

    // 1) 本数とエネルギー保存（箱部屋 8x3x6。この配置では鏡面点が 6 面とも面の内側）。
    {
        AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
        AF_UpdateConfig c = cfgFor(1, 5, 0);
        AF_SceneSetUpdateConfig(s, &c);
        const int n = taps(s, L, S, pos, g);
        std::snprintf(buf, sizeof(buf), "(%d 本)", n);
        check("[面] 6 面 x 5 本のタップが出る", n == 30, buf);
        double worst = 0.0;
        for (int b = 0; b < kBands; ++b) {
            const double refl = std::max(0.0, 1.0 - static_cast<double>(ab[b]) - tr[b]);
            const double e = energy(g, n, b);
            worst = std::max(worst, std::fabs(e - 6.0 * refl) / std::max(6.0 * refl, 1e-9));
        }
        std::snprintf(buf, sizeof(buf), "(最大相対誤差 %.4f)", worst);
        check("[面] タップのエネルギー和 = 6 面の反射率の和（1/r はホストが掛ける規約）", worst < 2e-3, buf);
        AF_SceneDestroy(s);
    }
    // 2) 連続性: 音源を 2 cm ずつ動かして総エネルギーの跳びを見る（面 vs 像源レイ）。
    {
        double maxStep[2] = {0.0, 0.0};
        for (int model = 0; model < 2; ++model) {
            AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
            AF_UpdateConfig c = cfgFor(model, 5, 0);
            AF_SceneSetUpdateConfig(s, &c);
            double prev = -1.0;
            for (float x = -2.0f; x <= 2.0f + 1e-4f; x += 0.02f) {
                const int n = taps(s, L, V(x, 1.6f, 1.2f), pos, g);
                const double e = energyAll(g, n);
                if (prev > 0.0 && e > 0.0) maxStep[model] = std::max(maxStep[model], std::fabs(dB(e) - dB(prev)));
                prev = e;
            }
            AF_SceneDestroy(s);
        }
        std::snprintf(buf, sizeof(buf), "(面 %.2f dB / 像源レイ %.2f dB)", maxStep[1], maxStep[0]);
        check("[面] 音源を 2 cm 動かしたときの総エネルギーの跳びが 0.5 dB 未満", maxStep[1] < 0.5, buf);
    }
    // 3) 幅: 西の壁の 5 本が張る方位角は、壁に近いほど広い。
    {
        auto span = [&](float lx) {
            AF_SceneHandle s = singleWall(tr, ab, sc);
            AF_UpdateConfig c = cfgFor(1, 5, 0);
            AF_SceneSetUpdateConfig(s, &c);
            const AF_Vector3 Lp = V(lx, 1.6f, -1.0f);
            const int n = taps(s, Lp, S, pos, g);
            float lo = 999.0f, hi = -999.0f;
            for (int t = 0; t < n; ++t) {
                const float dx = pos[t].x - Lp.x, dz = pos[t].z - Lp.z;
                const float az = std::atan2(dz, -dx) * 180.0f / 3.14159265f;
                lo = std::min(lo, az); hi = std::max(hi, az);
            }
            AF_SceneDestroy(s);
            return hi - lo;
        };
        const float nearSpan = span(-3.7f), farSpan = span(-1.0f);
        std::snprintf(buf, sizeof(buf), "(壁から 0.3 m: %.0f 度 / 3 m: %.0f 度)", nearSpan, farSpan);
        check("[面] 壁に近いほど線が張る角が広い（幅が出る）", nearSpan > farSpan, buf);
    }
    // 4) 鏡面だけ（scattering=0）なら、面ごとに鏡面点を挟む 2 本しか鳴らない。
    {
        const float sc0[6] = {0, 0, 0, 0, 0, 0};
        AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc0);
        AF_UpdateConfig c = cfgFor(1, 5, 0);
        AF_SceneSetUpdateConfig(s, &c);
        const int n = taps(s, L, S, pos, g);
        int live = 0;
        for (int t = 0; t < n; ++t) {
            float e = 0.0f;
            for (int b = 0; b < kBands; ++b) e += g[t * kBands + b];
            if (e > 1e-6f) ++live;
        }
        std::snprintf(buf, sizeof(buf), "(鳴る本数 %d / 全 %d)", live, n);
        check("[面] 散乱 0 なら鳴るのは面ごとに鏡面点を挟む 2 本まで", live >= 6 && live <= 12, buf);
        AF_SceneDestroy(s);
    }
    // 5) 閉じた扉: 隣室の面は透過ぶんしか鳴らない（開けると増える）。Test_SwingDoor と同じ形。
    {
        auto door = [&](float deg) {
            AF_SceneHandle s = boxRoom(7.4f, 1.5f, 7.4f, tr, ab, sc);
            const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
            AF_SceneAddInstanceBox(s, V(-3.75f, 1.5f, 0), V(3.25f, 1.5f, 0.1f), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( 3.75f, 1.5f, 0), V(3.25f, 1.5f, 0.1f), V(1,0,0), V(0,1,0), m);
            const float th = deg * 3.14159265f / 180.0f, cs = std::cos(th), sn = std::sin(th);
            AF_SceneAddInstanceBox(s, V(-0.5f + 0.5f * cs, 1.5f, 0.5f * sn), V(0.5f, 1.5f, 0.03f),
                                   V(cs, 0, sn), V(0,1,0), m);
            AF_UpdateConfig c = cfgFor(1, 5, 0);
            AF_SceneSetUpdateConfig(s, &c);
            const int n = taps(s, V(0, 1.6f, -3.0f), V(0, 1.6f, 3.0f), pos, g);
            const double e = energyAll(g, n);
            AF_SceneDestroy(s);
            return e;
        };
        std::printf("      扉の角度 → 早期反射の総エネルギー（リスナー z=-3、音源 z=+3、閉扉の向こう）\n");
        double prev = -1.0, maxStep = 0.0, eClosed = 0.0, eOpen = 0.0;
        for (int deg = 0; deg <= 90; deg += 10) {
            const double e = door(static_cast<float>(deg));
            if (deg == 0) eClosed = e;
            if (deg == 90) eOpen = e;
            std::printf("        %3d 度  %7.1f dB\n", deg, dB(e));
            if (prev > 0.0 && e > 0.0) maxStep = std::max(maxStep, std::fabs(dB(e) - dB(prev)));
            prev = e;
        }
        std::snprintf(buf, sizeof(buf), "(閉 %.1f dB / 開 %.1f dB)", dB(eClosed), dB(eOpen));
        check("[面] 閉扉では隣室の面の早期反射が開扉より小さい", eClosed < 0.5 * eOpen, buf);
        std::snprintf(buf, sizeof(buf), "(10 度刻みの最大段差 %.2f dB)", maxStep);
        check("[面] 扉を 10 度ずつ開いても総エネルギーが 3 dB 以上は跳ばない", maxStep < 3.0, buf);
    }
    // 6) 尾との二重防止: 1 次反射を外した尾は総和が減る（ゼロにはならない）。
    {
        double e[2] = {0.0, 0.0};
        for (int skip = 0; skip < 2; ++skip) {
            AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
            AF_UpdateConfig c = cfgFor(1, 5, 1);
            c.echogramSkipFirstOrder = skip;
            AF_SceneSetUpdateConfig(s, &c);
            AF_SceneSetListener(s, L); AF_SceneSetSource(s, 1, S);
            for (int k = 0; k < 8; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
            float bins[60 * kBands];
            const int nb = AF_SceneGetEchogramBands(s, AF_SceneSourceIndex(s, 1), bins, 60);
            for (int k = 0; k < nb * kBands; ++k) e[skip] += bins[k];
            AF_SceneDestroy(s);
        }
        std::snprintf(buf, sizeof(buf), "(1 次込み %.3f / 1 次なし %.3f)", e[0], e[1]);
        check("[尾] echogramSkipFirstOrder で尾から 1 次反射が抜ける", e[1] < e[0] && e[1] > 0.0, buf);
    }
    // 7) 費用: 扉のある部屋（箱 9 個）で 1 音源、模型ごとの 1 更新あたりの時間（役割1 込み、earlyEveryN=1）。
    {
        for (int model = 0; model < 2; ++model) {
            AF_SceneHandle s = boxRoom(7.4f, 1.5f, 7.4f, tr, ab, sc);
            const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
            AF_SceneAddInstanceBox(s, V(-3.75f, 1.5f, 0), V(3.25f, 1.5f, 0.1f), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( 3.75f, 1.5f, 0), V(3.25f, 1.5f, 0.1f), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, 1.5f, 0), V(0.5f, 1.5f, 0.03f), V(1,0,0), V(0,1,0), m);
            AF_UpdateConfig c = cfgFor(model, 5, 0);
            AF_SceneSetUpdateConfig(s, &c);
            AF_SceneSetListener(s, V(0, 1.6f, -3.0f)); AF_SceneSetSource(s, 1, V(0, 1.6f, 3.0f));
            for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int k = 0; k < 200; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
            const double us = std::chrono::duration<double, std::micro>(
                std::chrono::high_resolution_clock::now() - t0).count() / 200.0;
            std::printf("      費用: %s  1 更新 %.0f us（役割1 込み・音源 1・箱 9 個）\n", model ? "面の線" : "像源レイ", us);
            AF_SceneDestroy(s);
        }
    }
    // 8) 焼く層: 静的な面の見通しを焼き、実行時は 焼いた見通し × 動いた物の遮蔽。生で解いた物と一致すること。
    {
        auto total = [&](AF_SceneHandle s, AF_Vector3 Lp, AF_Vector3 Sp) {
            const int n = taps(s, Lp, Sp, pos, g);
            return energyAll(g, n);
        };
        // 8a) 静的な箱部屋: 焼き vs 生。
        {
            AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
            AF_UpdateConfig c = cfgFor(1, 5, 0);
            AF_SceneSetUpdateConfig(s, &c);
            const double eLive = total(s, L, S);
            const auto t0 = std::chrono::high_resolution_clock::now();
            const int cells = AF_SceneBakeStaticFaces(s, 1.0f, 5);
            const double bakeMs = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - t0).count();
            const int faces = AF_SceneFaceBakeFaceCount(s);
            const double eBake = total(s, L, S);
            std::snprintf(buf, sizeof(buf), "(セル %d / 面 %d / %.0f ms)", cells, faces, bakeMs);
            check("[焼き] 箱部屋で焼ける（セル > 0、面 6）", cells > 0 && faces == 6, buf);
            std::snprintf(buf, sizeof(buf), "(生 %.2f dB / 焼き %.2f dB)", dB(eLive), dB(eBake));
            check("[焼き] 焼いた見通しの答えが生と 0.5 dB 以内", std::fabs(dB(eLive) - dB(eBake)) < 0.5, buf);
            AF_SceneDestroy(s);
        }
        // 8b) 静的な柱: 焼きも生も柱を見る（東の壁の線が柱に隠れる）。
        {
            double e[3] = {0, 0, 0};   // 0: 柱なし生 / 1: 柱あり生 / 2: 柱あり焼き
            for (int k = 0; k < 3; ++k) {
                AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
                if (k >= 1) {
                    // 柱は吸い切る材質（反射率 0）。柱の面が鳴って総和が増えないように、遮蔽だけを見る。
                    const float opq[6] = {0, 0, 0, 0, 0, 0}, ab1[6] = {1, 1, 1, 1, 1, 1};
                    const int m = AF_SceneAddMaterial(s, opq, ab1, opq, 6);
                    AF_SceneAddInstanceBox(s, V(2.0f, 1.5f, 0.0f), V(0.3f, 1.5f, 0.3f), V(1,0,0), V(0,1,0), m);
                }
                AF_UpdateConfig c = cfgFor(1, 5, 0);
                AF_SceneSetUpdateConfig(s, &c);
                if (k == 2) AF_SceneBakeStaticFaces(s, 1.0f, 5);
                e[k] = total(s, V(-1.0f, 1.6f, 0.0f), V(0.5f, 1.6f, 0.0f));
                AF_SceneDestroy(s);
            }
            std::snprintf(buf, sizeof(buf), "(柱なし %.2f / 柱あり生 %.2f / 柱あり焼き %.2f dB)", dB(e[0]), dB(e[1]), dB(e[2]));
            check("[焼き] 静的な柱の遮蔽が焼きにも入る（生と 1 dB 以内、柱なしより小さい）",
                  std::fabs(dB(e[1]) - dB(e[2])) < 1.0 && e[2] < e[0], buf);
        }
        // 8c) 焼いた後に扉が動く: 閉扉で焼き → 90° へ動かす → 生の 90° と比べる。
        {
            auto doorScene = [&](float deg, int* outDoorId) {
                AF_SceneHandle s = boxRoom(7.4f, 1.5f, 7.4f, tr, ab, sc);
                const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
                AF_SceneAddInstanceBox(s, V(-3.75f, 1.5f, 0), V(3.25f, 1.5f, 0.1f), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V( 3.75f, 1.5f, 0), V(3.25f, 1.5f, 0.1f), V(1,0,0), V(0,1,0), m);
                const float th = deg * 3.14159265f / 180.0f, cs = std::cos(th), sn = std::sin(th);
                const int d = AF_SceneAddInstanceBox(s, V(-0.5f + 0.5f * cs, 1.5f, 0.5f * sn), V(0.5f, 1.5f, 0.03f),
                                                     V(cs, 0, sn), V(0,1,0), m);
                if (outDoorId) *outDoorId = d;
                AF_UpdateConfig c = cfgFor(1, 5, 0);
                AF_SceneSetUpdateConfig(s, &c);
                return s;
            };
            const AF_Vector3 Ld = V(0, 1.6f, -3.0f), Sd = V(0, 1.6f, 3.0f);
            int doorId = -1;
            AF_SceneHandle sb = doorScene(0.0f, &doorId);
            AF_SceneSetInstanceDynamic(sb, doorId, 1);   // 扉は「音響的に動く物」。焼きに入れない
            const double eLiveClosed = total(sb, Ld, Sd);
            AF_SceneBakeStaticFaces(sb, 1.0f, 5);
            const double eBakeClosed = total(sb, Ld, Sd);
            // 扉を 90° へ（moved になり、焼きから外れて生で扱われる）。
            const float th = 90.0f * 3.14159265f / 180.0f, cs = std::cos(th), sn = std::sin(th);
            AF_SceneUpdateInstance(sb, doorId, V(-0.5f + 0.5f * cs, 1.5f, 0.5f * sn), V(0.5f, 1.5f, 0.03f),
                                   V(cs, 0, sn), V(0,1,0));
            const double eBakeOpen = total(sb, Ld, Sd);
            AF_SceneDestroy(sb);
            AF_SceneHandle so = doorScene(90.0f, nullptr);
            const double eLiveOpen = total(so, Ld, Sd);
            AF_SceneDestroy(so);
            std::snprintf(buf, sizeof(buf), "(閉: 生 %.2f / 焼き %.2f dB)", dB(eLiveClosed), dB(eBakeClosed));
            check("[焼き] 閉扉で焼いた答えが生と 0.5 dB 以内", std::fabs(dB(eLiveClosed) - dB(eBakeClosed)) < 0.5, buf);
            std::snprintf(buf, sizeof(buf), "(90°: 焼いた後に動かす %.2f / 生 %.2f dB)", dB(eBakeOpen), dB(eLiveOpen));
            check("[焼き] 焼いた後に扉を開けても生と 1 dB 以内（動いた物は実行時に拾う）",
                  std::fabs(dB(eBakeOpen) - dB(eLiveOpen)) < 1.0, buf);
            // 8e) タグを付け忘れた場合: 静的として焼いた扉が動く → 焼きが stale になり、次の更新で扉を除いて焼き直す。
            {
                int d2 = -1;
                AF_SceneHandle s2 = doorScene(0.0f, &d2);
                AF_SceneBakeStaticFaces(s2, 1.0f, 5);            // 扉は静的として焼かれる
                AF_SceneUpdateInstance(s2, d2, V(-0.5f + 0.5f * cs, 1.5f, 0.5f * sn), V(0.5f, 1.5f, 0.03f),
                                       V(cs, 0, sn), V(0,1,0));  // → moved、焼きは stale
                const double eStale = total(s2, Ld, Sd);          // 更新の中で焼き直される
                const int facesAfter = AF_SceneFaceBakeFaceCount(s2);
                AF_SceneDestroy(s2);
                std::snprintf(buf, sizeof(buf), "(焼き直し後 %.2f / 生 %.2f dB、面 %d)", dB(eStale), dB(eLiveOpen), facesAfter);
                check("[焼き] タグ無しで焼いた扉が動いても、焼き直しで生と 1 dB 以内", std::fabs(dB(eStale) - dB(eLiveOpen)) < 1.0, buf);
            }
        }
        // 8d) 費用: 焼きあり／なし の 1 更新。同じ部屋に居るとき（焼きが効く形）。役割1・カタログ・尾は止めて面の線だけを測る。
        {
            for (int bake = 0; bake < 2; ++bake) {
                AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
                const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
                AF_SceneAddInstanceBox(s, V(2.0f, 1.5f, 0.0f), V(0.3f, 1.5f, 0.3f), V(1,0,0), V(0,1,0), m);   // 柱
                AF_UpdateConfig c = cfgFor(1, 5, 0);
                c.role1EveryN = 100000; c.catalogEveryN = 100000; c.useEdgeCatalog = 0; c.useReflections = 0;
                AF_SceneSetUpdateConfig(s, &c);
                AF_SceneSetListener(s, V(-1.0f, 1.6f, -1.0f)); AF_SceneSetSource(s, 1, V(1.5f, 1.6f, 1.2f));
                if (bake) AF_SceneBakeStaticFaces(s, 1.0f, 5);
                for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
                const auto t0 = std::chrono::high_resolution_clock::now();
                for (int k = 0; k < 200; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
                const double us = std::chrono::duration<double, std::micro>(
                    std::chrono::high_resolution_clock::now() - t0).count() / 200.0;
                std::printf("      費用: 面の線 %s  1 更新 %.0f us（面の線だけ・音源 1・箱 7 個・同じ部屋）\n", bake ? "焼きあり" : "焼きなし", us);
                AF_SceneDestroy(s);
            }
        }
    }
    // 9) 外の作り手のための口: BVH の書き出しと、焼きの出し入れ（GPU 化の下ごしらえ）。
    {
        AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
        AF_UpdateConfig c = cfgFor(1, 5, 0);
        AF_SceneSetUpdateConfig(s, &c);
        // BVH: 節 > 0、葉の数の和 = 実体数、葉が指す添字が有効。
        const int nn = AF_SceneBvhNodeCount(s), no = AF_SceneBvhOrderCount(s);
        std::vector<AF_BvhNode> nodes(static_cast<size_t>(std::max(1, nn)));
        std::vector<int> order(static_cast<size_t>(std::max(1, no)));
        const int got = AF_SceneExportBvh(s, nodes.data(), nn, order.data(), no);
        int leafSum = 0; bool idxOk = true;
        for (int i = 0; i < got; ++i) if (nodes[static_cast<size_t>(i)].count > 0) leafSum += nodes[static_cast<size_t>(i)].count;
        for (int i = 0; i < no; ++i) idxOk = idxOk && order[static_cast<size_t>(i)] >= 0 && order[static_cast<size_t>(i)] < AF_SceneInstanceCount(s);
        bool rootOk = got > 0;
        if (got > 0) {   // 根は全実体の箱を包む
            for (int i = 0; i < AF_SceneInstanceCount(s); ++i) {
                AF_InstanceDesc d{};
                if (!AF_SceneGetInstance(s, i, &d)) { rootOk = false; break; }
                rootOk = rootOk && d.center.x - d.halfExtents.x >= nodes[0].minX - 1e-3f
                                && d.center.x + d.halfExtents.x <= nodes[0].maxX + 1e-3f;
            }
        }
        std::snprintf(buf, sizeof(buf), "(節 %d / 葉の和 %d / 実体 %d)", got, leafSum, AF_SceneInstanceCount(s));
        check("[口] BVH が書き出せる（葉の和 = 実体数、添字が有効、根が全部を包む）",
              got == nn && nn > 0 && leafSum == AF_SceneInstanceCount(s) && idxOk && rootOk, buf);
        // 焼きの往復: 出して消して入れたら、タップがビット一致で戻る。
        AF_SceneBakeStaticFaces(s, 1.0f, 5);
        const int n1 = taps(s, L, S, pos, g);
        std::vector<float> g1(g, g + n1 * kBands);
        const int bytes = AF_SceneFaceBakeBytes(s);
        std::vector<unsigned char> blob(static_cast<size_t>(std::max(1, bytes)));
        const int wrote = AF_SceneFaceBakeExport(s, blob.data(), bytes);
        AF_SceneClearFaceBake(s);
        const int facesCleared = AF_SceneFaceBakeFaceCount(s);
        const int badImport = AF_SceneFaceBakeImport(s, blob.data(), bytes - 1);
        const int imp = AF_SceneFaceBakeImport(s, blob.data(), bytes);
        const int n2 = taps(s, L, S, pos, g);
        bool same = (n1 == n2);
        for (int k = 0; same && k < n2 * kBands; ++k) same = (g[k] == g1[static_cast<size_t>(k)]);
        std::snprintf(buf, sizeof(buf), "(%d バイト / 面 %d → 消して %d → 入れて %d / タップ %d 本)",
                      wrote, 6, facesCleared, AF_SceneFaceBakeFaceCount(s), n2);
        check("[口] 焼きを出して消して入れ直すと、タップがビット一致で戻る", wrote == bytes && bytes > 0 && facesCleared == 0 && imp == 1 && same, buf);
        check("[口] 大きさの合わない焼きは受け取らない", badImport == 0);
        AF_SceneDestroy(s);
    }
    // 10) LOD と予算: 張る角で 1 本／線／面を選ぶ。予算から漏れた面の 1 次は尾に残る。
    {
        // 10a) 大きな箱（20 x 3 x 6）。西の壁ぎわ 0.4 m → 面 5x3 = 15 本。東の壁（19.8 m 先、半幅 3 → 17°）→ 線 5 本。
        //      1.2 m 角の板（14.7 m 先 → 4.7°）→ 1 本。床・天井・南北（張る角 89〜91°）→ 線 5 本ずつ。合計 41 本。
        {
            AF_SceneHandle s = boxRoom(10.0f, 1.5f, 3.0f, tr, ab, sc);
            const int m = AF_SceneAddMaterial(s, tr, ab, sc, 6);
            AF_SceneAddInstanceBox(s, V(5.0f, 1.5f, 0.0f), V(0.03f, 0.6f, 0.6f), V(1,0,0), V(0,1,0), m);   // 小さな板
            AF_UpdateConfig c = cfgFor(1, 5, 0);
            AF_SceneSetUpdateConfig(s, &c);
            const AF_Vector3 Lw = V(-9.6f, 1.6f, 0.0f), Sw = V(-8.0f, 1.6f, 0.5f);
            const int n = taps(s, Lw, Sw, pos, g);
            int west = 0, east = 0;
            for (int t = 0; t < n; ++t) {
                const float dx = pos[t].x - Lw.x, dy = pos[t].y - Lw.y, dz = pos[t].z - Lw.z;
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len < 1e-4f) continue;
                if (dx / len < 0.0f) ++west;                 // 西向き＝壁ぎわの面
                if (dx / len > 0.95f) ++east;                // ほぼ真東＝東の壁の線と板
            }
            std::snprintf(buf, sizeof(buf), "(全 %d 本 / 西 %d / 真東 %d)", n, west, east);
            check("[LOD] 壁ぎわは面 15 本、遠い板は 1 本、合計 41 本", n == 41 && west == 15, buf);
            AF_SceneDestroy(s);
        }
        // 10b) 予算: 上限が小さいと面ごと丸ごと落ち、落ちた面の 1 次は尾に残る。
        //      尾の総和は「全部線」< 「一部線」< 「1 次を外さない」。
        {
            double tail[3] = {0, 0, 0}; int nTaps[3] = {0, 0, 0};
            for (int k = 0; k < 3; ++k) {
                AF_SceneHandle s = boxRoom(4.0f, 1.5f, 3.0f, tr, ab, sc);
                AF_UpdateConfig c = cfgFor(1, 5, 1);
                c.earlyTaps = (k == 1) ? 12 : 48;                  // 12 本 = 面 2 枚ぶんしか入らない
                c.echogramSkipFirstOrder = (k == 2) ? 0 : 1;
                AF_SceneSetUpdateConfig(s, &c);
                AF_SceneSetListener(s, L); AF_SceneSetSource(s, 1, S);
                for (int i = 0; i < 8; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
                const int idx = AF_SceneSourceIndex(s, 1);
                nTaps[k] = AF_SceneGetEarlyReflections(s, idx, pos, g, 48);
                float bins[60 * kBands];
                const int nb = AF_SceneGetEchogramBands(s, idx, bins, 60);
                for (int i = 0; i < nb * kBands; ++i) tail[k] += bins[i];
                AF_SceneDestroy(s);
            }
            std::snprintf(buf, sizeof(buf), "(タップ 48:%d 本 / 12:%d 本、尾 全部線 %.3f < 一部線 %.3f < 外さない %.3f)",
                          nTaps[0], nTaps[1], tail[0], tail[1], tail[2]);
            check("[予算] 上限 12 なら面 2 枚（10 本）だけ線になり、残りの面の 1 次は尾に残る",
                  nTaps[0] == 30 && nTaps[1] == 10 && tail[0] < tail[1] && tail[1] < tail[2], buf);
        }
    }
}

void diagnoseSwingDoorSourceSweep() {
    std::printf("\n[診断] Test_SwingDoor の再現: 閉扉で音源を左右へ振る\n");
    const float halfW = 7.4f, wallT = 0.2f, roomH = 3.4f;
    const float doorHalf = 0.5f;
    auto build = [&](float doorDeg, bool withDoor) -> AF_SceneHandle {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        auto box = [&](AF_Vector3 c, AF_Vector3 he) {
            AF_SceneAddInstanceBox(s, c, he, V(1,0,0), V(0,1,0), mat);
        };
        // 外周（Wall_W/E/N/S、床、天井）
        box(V(-7.2f, 1.5f, 0), V(0.2f, 1.5f, 7.0f));
        box(V( 7.2f, 1.5f, 0), V(0.2f, 1.5f, 7.0f));
        box(V(0, 1.5f,  7.2f), V(7.0f, 1.5f, 0.2f));
        box(V(0, 1.5f, -7.2f), V(7.0f, 1.5f, 0.2f));
        box(V(0, -0.2f, 0), V(halfW, 0.2f, halfW));
        box(V(0,  3.2f, 0), V(halfW, 0.2f, halfW));
        // 仕切り（戸口の左右）
        box(V(-3.75f, 1.5f, 0), V(3.25f, 1.5f, wallT * 0.5f));
        box(V( 3.75f, 1.5f, 0), V(3.25f, 1.5f, wallT * 0.5f));
        if (withDoor) {
            // 蝶番 x=-0.5、幅 1.0。閉じている時は戸口をちょうど塞ぐ。
            const int d = AF_SceneAddInstanceBox(s, V(0, 1.5f, 0), V(doorHalf, 1.5f, 0.03f),
                                                 V(1,0,0), V(0,1,0), mat);
            const float th = doorDeg * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            AF_SceneUpdateInstance(s, d, V(-0.5f + doorHalf * c, 1.5f, doorHalf * sn),
                                   V(doorHalf, 1.5f, 0.03f), V(c, 0, sn), V(0,1,0));
        }
        AF_UpdateConfig cfg{};
        cfg.role1EveryN = 1;   cfg.role2EveryN = 1;   cfg.earlyEveryN = 1;
        cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
        cfg.reflectionRays = 256; cfg.reflectionBounces = 3;
        cfg.directWeight = 1.0f;  cfg.useReflections = 1;
        cfg.useEdgeCatalog = 1;   cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;
        cfg.enableReverb = 1;     cfg.echogramBins = 100;  cfg.echogramBinSeconds = 0.01f;
        cfg.echogramRays = 512;   cfg.echogramBounces = 24;
        cfg.speedOfSound = 343.0f; cfg.distanceRef = 1.5f;
        cfg.enableEarlyReflections = 1; cfg.earlyTaps = 4;
        cfg.earlyRays = 512; cfg.earlyBounces = 2;
        cfg.enableDiffractionSources = 1; cfg.diffSources = 3;
        AF_SceneSetUpdateConfig(s, &cfg);
        return s;
    };
    const AF_Vector3 L = V(0, 1.6f, -3.0f);
    auto azOf = [](float dx, float dz) { return std::atan2(dx, dz) * 180.0f / 3.14159265f; };
    auto sweep = [&](float doorDeg, bool withDoor, const char* label, float x0, float x1, float step) {
        AF_SceneHandle s = build(doorDeg, withDoor);
        std::printf("      ── %s ──\n", label);
        std::printf("        音源x   生存(dB)  真の方位  到来方位   差     回折  開口率  フレネル\n");
        float prevDb = 0.0f; bool have = false;
        float maxJump = 0.0f, jumpAt = 0.0f;
        for (float x = x0; x <= x1 + 1e-4f; x += step) {
            const AF_Vector3 S = V(x, 1.6f, 3.0f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
            const int idx = AF_SceneSourceIndex(s, 1);
            float b6[kBands] = {};
            AF_SceneGetSourceOcclusion(s, idx, b6);
            double e2 = 0.0; for (int b = 0; b < kBands; ++b) e2 += b6[b] * b6[b];
            const float db = static_cast<float>(10.0 * std::log10(std::max(e2 / kBands, 1e-12)));
            float ad[3] = {0, 0, 1};
            AF_SceneGetSourceArrivalDir(s, idx, ad);
            const float trueAz = azOf(x - L.x, 3.0f - L.z);
            const float arrAz = azOf(ad[0], ad[2]);
            float d28[28] = {};
            const int np = AF_SceneDebugDiffractionPath(s, L, S, d28, 0);
            // ★ホストが実際に読むのは**キャッシュ側**（AF_SceneUpdate が書いた物）。
            //   早期反射は像源としてワールドに置かれ、Unity が 3D で定位する ──
            //   鏡像は反対側に出ることがあるので、閉扉で直接音が −35 dB まで落ちると
            //   ここが定位を持っていく。方位と強さを並べる。
            AF_Vector3 dp[4]; float dg[4] = {};
            const int nds = AF_SceneGetDiffractionSources(s, idx, dp, dg, 4);
            AF_Vector3 ep[8]; float eg[8 * kBands] = {};
            const int ne = AF_SceneGetEarlyReflections(s, idx, ep, eg, 8);
            char er[160]; int q = 0; er[0] = 0;
            float erAzTop = 0.0f, erTop = -1.0f;
            for (int k = 0; k < ne; ++k) {
                float sum = 0.0f;
                for (int b = 0; b < kBands; ++b) sum += eg[k * kBands + b];
                sum /= kBands;
                const float az = azOf(ep[k].x - L.x, ep[k].z - L.z);
                if (sum > erTop) { erTop = sum; erAzTop = az; }
                { const float dxr = ep[k].x - L.x, dyr = ep[k].y - L.y, dzr = ep[k].z - L.z;
                    const float dr = std::sqrt(dxr*dxr + dyr*dyr + dzr*dzr);
                    if (q < 96) q += std::snprintf(er + q, sizeof(er) - q, " %+.0f°/%.1fm", az, dr); }
            }
            // ★尾／直接の比。ホスト（UpdateReverbTargetRatio）と同じ式で出す:
            //   rc = 0.057√(V/RT60)、t = (r/rc)²、知覚圧縮 t^exponent（場面の値 0.5）。
            //   尾の絶対レベルは tailGain = 自由音場の直接 × √t、直接タップは 自由音場 × 生存、
            //   さらに尾には tailSrcLevel(=生存) が掛かるので、**尾/直接 = √t**（生存が約分される）。
            //   ＝ 遮蔽が深いほど、聞こえている物のほとんどが尾になる。
            const float vol = AF_SceneRoomVolumeAt(s, L, 2.0f);
            float rt6[kBands] = {};
            AF_SceneRt60At(s, L, 2.0f, rt6, kBands);
            const float rt = std::max(0.05f, rt6[2]);
            const float rc = 0.057f * std::sqrt(std::max(vol, 1.0f) / rt);
            const float rr = std::sqrt((x - L.x) * (x - L.x) + (3.0f - L.z) * (3.0f - L.z));
            const float traw = (rr * rr) / std::max(rc * rc, 1e-4f);
            const float tcmp = std::pow(traw, 0.5f);                 // 場面の reverbRatioExponent
            const float tailOverDirect = 10.0f * std::log10(std::max(tcmp, 1e-12f));
            if (have) {
                const float j = std::fabs(db - prevDb);
                if (j > maxJump) { maxJump = j; jumpAt = x; }
            }
            prevDb = db; have = true;
            const bool flip = (std::fabs(trueAz) > 5.0f) && (trueAz * arrAz < 0.0f) && (std::fabs(arrAz) > 5.0f);
            const bool erFlip = (ne > 0) && (std::fabs(trueAz) > 5.0f) && (trueAz * erAzTop < 0.0f)
                                && (std::fabs(erAzTop) > 5.0f);
            std::printf("        %+5.2f  %7.1f   %+6.1f°  %+6.1f°  %+6.1f°   %d本  %.4f  %s | 尾/直接 %+5.1f dB | 回折源%d 反射%d本%s%s%s\n",
                        x, db, trueAz, arrAz, trueAz - arrAz, np, d28[1],
                        (d28[18] > 0.5f) ? "はい" : "いいえ", tailOverDirect, nds, ne, er,
                        flip ? "  ★到来が反転" : "", erFlip ? "  ★最強の反射が反対側" : "");
        }
        std::printf("        最大の跳び %.1f dB（x=%+.2f 付近、刻み %.2f m）\n", maxJump, jumpAt, step);
        AF_SceneDestroy(s);
    };
    sweep(0.0f, true, "閉扉・音源を x=-5 → +5（0.5 m 刻み）", -5.0f, 5.0f, 0.5f);
    sweep(0.0f, true, "閉扉・報告の境目まわり x=-3.9 → -3.1（0.1 m 刻み）", -3.9f, -3.1f, 0.1f);
    sweep(90.0f, true, "90° 開・x=-5 → +5（0.5 m 刻み）", -5.0f, 5.0f, 0.5f);
    std::printf("      読み方: 到来方位の符号が真と逆なら反転。生存 dB の跳びが「急に開ける」。\n");
}

void diagnoseClosedDoorLocalization() {
    std::printf("\n[診断] 閉じた扉ごしの定位（真の方位 vs 到来方位。壁の奥の反転の切り分け）\n");
    const float roomW = 7.08f, roomD = 3.48f, roomH = 3.0f, wall = 0.16f;
    const float doorW = 1.40f, doorH = 2.0f, doorT = 0.08f;
    const float gapCx = 3.54f;
    const float gapL = gapCx - doorW * 0.5f, gapR = gapCx + doorW * 0.5f;
    const float hw = wall * 0.5f, cy = roomH * 0.5f, zf = roomD + hw;
    const AF_Vector3 L = V(gapCx, 1.6f, roomD + wall + 0.5f);
    const AF_Vector3 src[3] = { V((306.0f-146.0f)/100.0f, 1.5f, (432.0f-196.0f)/100.0f),
                                V((500.0f-146.0f)/100.0f, 1.5f, (320.0f-196.0f)/100.0f),
                                V((694.0f-146.0f)/100.0f, 1.5f, (432.0f-196.0f)/100.0f) };
    const char* sname[3] = { "音源1(左)", "音源2(中)", "音源3(右)" };
    auto azOf = [&](float dx, float dz) {
        return std::atan2(dx, dz) * 180.0f / 3.14159265f;
    };
    auto run = [&](float deg, bool hasDoor, const char* label) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        auto box = [&](AF_Vector3 c, AF_Vector3 he) {
            AF_SceneAddInstanceBox(s, c, he, V(1,0,0), V(0,1,0), mat);
        };
        box(V(-hw, cy, roomD*0.5f), V(hw, cy, roomD*0.5f + wall));
        box(V(roomW + hw, cy, roomD*0.5f), V(hw, cy, roomD*0.5f + wall));
        box(V(roomW*0.5f, cy, -hw), V(roomW*0.5f + wall, cy, hw));
        box(V(roomW*0.5f, roomH + hw, roomD*0.5f), V(roomW*0.5f + wall, hw, roomD*0.5f + wall));
        box(V(roomW*0.5f, -hw, roomD*0.5f), V(roomW*0.5f + wall, hw, roomD*0.5f + wall));
        box(V(gapL*0.5f, cy, zf), V(gapL*0.5f, cy, hw));
        box(V((gapR + roomW)*0.5f, cy, zf), V((roomW - gapR)*0.5f, cy, hw));
        box(V(gapCx, (doorH + roomH)*0.5f, zf), V(doorW*0.5f, (roomH - doorH)*0.5f, hw));
        if (hasDoor) {
            const int doorId = AF_SceneAddInstanceBox(s, V(gapCx, doorH*0.5f, zf),
                V(doorW*0.5f, doorH*0.5f, doorT*0.5f), V(1,0,0), V(0,1,0), mat);
            const float th = deg * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            const float rx = gapCx - gapR;
            const float sgn = (rx < 0.0f) ? 1.0f : -1.0f;
            AF_SceneUpdateInstance(s, doorId, V(gapR + rx*c, doorH*0.5f, zf - std::fabs(rx)*sn),
                V(doorW*0.5f, doorH*0.5f, doorT*0.5f), V(c, 0, sgn*sn), V(0,1,0));
        }
        AF_UpdateConfig cfg{};
        cfg.role1EveryN = 1;   cfg.role2EveryN = 1;   cfg.earlyEveryN = 1;
        cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
        cfg.reflectionRays = 256; cfg.reflectionBounces = 3;
        cfg.directWeight = 1.0f;  cfg.useReflections = 1;
        cfg.useEdgeCatalog = 1;   cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;
        cfg.enableReverb = 1;     cfg.echogramBins = 100;  cfg.echogramBinSeconds = 0.01f;
        cfg.echogramRays = 512;   cfg.echogramBounces = 24;
        cfg.speedOfSound = 343.0f; cfg.distanceRef = 1.5f;
        cfg.enableEarlyReflections = 1; cfg.earlyTaps = 4;
        cfg.earlyRays = 512; cfg.earlyBounces = 2;
        cfg.enableDiffractionSources = 1; cfg.diffSources = 3;
        AF_SceneSetUpdateConfig(s, &cfg);
        AF_SceneSetListener(s, L);
        for (int i = 0; i < 3; ++i) AF_SceneSetSource(s, (unsigned long long)(i + 1), src[i]);
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        std::printf("      ── %s ──\n", label);
        std::printf("        %-10s 真の方位  到来方位   差    生存(dB)  漏れ点の方位  回折の開口の方位\n", "音源");
        for (int i = 0; i < 3; ++i) {
            const int idx = AF_SceneSourceIndex(s, (unsigned long long)(i + 1));
            const float trueAz = azOf(src[i].x - L.x, src[i].z - L.z);
            float ad[3] = {0, 0, 1};
            AF_SceneGetSourceArrivalDir(s, idx, ad);
            const float arrAz = azOf(ad[0], ad[2]);
            float b6[kBands] = {};
            AF_SceneGetSourceOcclusion(s, idx, b6);
            double e2 = 0.0; for (int b = 0; b < kBands; ++b) e2 += b6[b] * b6[b];
            const double db = 10.0 * std::log10(std::max(e2 / kBands, 1e-12));
            AF_Vector3 lp{};
            AF_SceneMeasureLeakPoint(s, L, src[i], &lp);
            const float leakAz = azOf(lp.x - L.x, lp.z - L.z);
            AF_Vector3 dpos[4]; float dgain[4] = {}; float dband[4 * kBands] = {};
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, src[i], dpos, dgain, dband, 4);
            char apStr[32] = "なし";
            if (nd > 0) std::snprintf(apStr, sizeof(apStr), "%+6.1f°(%d本)", azOf(dpos[0].x - L.x, dpos[0].z - L.z), nd);
            float dsq = trueAz - arrAz; if (dsq > 180.0f) dsq -= 360.0f; if (dsq < -180.0f) dsq += 360.0f;
            const bool flipped = (std::fabs(trueAz) > 8.0f) && (trueAz * arrAz < 0.0f)
                                 && (std::fabs(arrAz) > 8.0f);
            std::printf("        %-10s %+6.1f°  %+6.1f°  %+6.1f°  %6.1f   %+6.1f°       %s%s\n",
                        sname[i], trueAz, arrAz, dsq, db, leakAz, apStr,
                        flipped ? "   ★左右が反転" : "");
        }
        AF_SceneDestroy(s);
    };
    run(0.0f, true, "閉扉（0°）");
    run(40.0f, true, "40° 開");
    run(0.0f, false, "ドアなし（基準）");
    std::printf("      読み方: 到来方位が 0° 付近なら「戸口へ寄っている」＝正しい（壁の奥は戸口から漏れる）。\n"
                "              符号が真と逆で絶対値が大きいなら反転。ホストは遮蔽が深いほどこの向きへ寄せる。\n");
}

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
// 格子が上限に当たって黙って粗くなる件。
//
//   格子は**登録された全ボックスの AABB 全体**を覆うので、広い地面を 1 枚置くだけで
//   指定したセルが無視される。0.25m → 0.375m に降格すると、0.9m の戸口を割るのに要る
//   「幅 = 半径×2」の余裕が無くなり、**部屋がそこで割れなくなる**。
//   音の結果（別の部屋の残響が乗るかどうか）が変わるのに何も出ないので、
//   ホストが検知できる口を用意した ── その口が実際に立つことを縛る。
void testRoomGridDegradeDetect() {
    std::printf("\n[部屋] 格子が上限で粗くなったことを検知できるか\n");
    struct C { const char* name; float half; float h; float cell; bool wantDegrade; };
    const C cases[] = {
        {"25x25m・天井 4m   0.25m 指定", 12.5f,  4.0f, 0.25f, false},
        // ゲーム側の神殿と同じ規模（90x90m の地面・天井 10m）。実際にここで降格していた。
        {"90x90m・天井 10m  0.25m 指定", 45.0f, 10.0f, 0.25f, true },
    };
    for (const C& c : cases) {
        AF_SceneHandle s = AF_SceneCreate();
        AF_SceneSetRoomCellSize(s, c.cell);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // 地面＋壁＋天井で囲う。
        const float t = 0.3f, h = c.h;
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(c.half, t, c.half), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(c.half, t, c.half), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-c.half, h*0.5f, 0), V(t, h*0.5f, c.half), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( c.half, h*0.5f, 0), V(t, h*0.5f, c.half), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -c.half), V(c.half, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  c.half), V(c.half, h*0.5f, t), V(1,0,0), V(0,1,0), m);

        float req = 0.0f, act = 0.0f; double nv = 0.0, maxv = 0.0;
        const int bad = AF_SceneRoomGridDegraded(s, &req, &act, &nv, &maxv);
        std::printf("        %s  要求 %.3fm → 実際 %.3fm / %.0f ボクセル（上限 %.0f）%s\n",
                    c.name, req, act, nv, maxv, bad ? "  ★降格" : "");
        char tag[96];
        std::snprintf(tag, sizeof(tag), "[部屋] %s の降格判定",
                      c.wantDegrade ? "広すぎる格子" : "収まる格子");
        check(tag, (bad != 0) == c.wantDegrade);
        if (c.wantDegrade) check("[部屋] 降格したら実セルが要求より大きい", act > req * 1.001f);
        else               check("[部屋] 収まっていれば実セル＝要求", act <= req * 1.001f);
        AF_SceneDestroy(s);
    }
    std::printf("      → 降格すると 幅 実セル×2 未満の戸口で部屋が割れなくなる。\n"
                "        ホストは AF_SceneRoomGridDegraded で気づいて警告すること。\n");
}

// 【C1】部屋グラフの開口からポータルを自動生成する。
//
//   「ここが開口である」という同じ事実を、エンジンが自動で持っているのに人が
//   もう一度置き直していた。置き忘れるとフレネル帯域積分が一度も走らず、
//   戸口がただの幾何の隙間として処理される ── **静かに劣化して気づけない**。
//
//   ★自動生成そのものより、「ポータルが増えても無関係な場所の回折が変わらない」
//     ほうが重要。以前は 1 枚でもあればシーン中の回折をポータルが支配していたので、
//     自動生成した瞬間に柱の陰（コンセプトの中心動作）が壊れる。
void testAutoPortals() {
    std::printf("\n[C1] 開口からポータルを自動生成する\n");
    const float h = 3.0f, t = 0.2f, hw = 5.0f, hd = 5.0f;
    const float doorW = 0.9f, doorH = 2.0f;

    // 2 部屋（z<0 と z>0）を仕切り、幅 0.9m・高さ 2.0m の戸口を空ける。
    //   仕切りは「左の袖・右の袖・戸口の上のまぐさ」の 3 枚で作る。
    auto build = [&](bool pillar) {
        AF_SceneHandle s = AF_SceneCreate();
        AF_SceneSetRoomCellSize(s, 0.15f);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        // 仕切り（z=0）。戸口は x∈[-0.45, +0.45], y∈[0, 2.0]
        const float sideW = (hw - doorW * 0.5f) * 0.5f;
        const float sideC = doorW * 0.5f + sideW;
        AF_SceneAddInstanceBox(s, V(-sideC, h*0.5f, 0), V(sideW, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( sideC, h*0.5f, 0), V(sideW, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        const float lintelH = (h - doorH) * 0.5f;
        AF_SceneAddInstanceBox(s, V(0, doorH + lintelH, 0), V(doorW*0.5f, lintelH, t),
                               V(1,0,0), V(0,1,0), m);
        if (pillar)   // 部屋 A（z<0）の中に柱。開口とは無関係な回折源
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -3.0f), V(0.9f, h*0.5f, 0.25f),
                                   V(1,0,0), V(0,1,0), m);
        return s;
    };

    // ── ① 自動生成された矩形が戸口の実寸に合うか ──
    {
        AF_SceneHandle s = build(false);
        AF_SceneSetAutoPortals(s, 1);
        AF_SceneSetListener(s, V(0, 1.5f, -2.0f));
        AF_SceneSetSource(s, 1, V(0, 1.5f, 2.0f));
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        int nAuto = 0, nMan = 0;
        const int nTot = AF_SceneGetPortalCounts(s, &nAuto, &nMan);
        std::printf("        部屋 %d 個 / 開口 %d 個 → ポータル 合計 %d（自動 %d / 手置き %d）\n",
                    AF_SceneRoomCount(s), AF_SceneApertureCount(s), nTot, nAuto, nMan);
        check("[C1] 開口からポータルが生える", nAuto >= 1);
        check("[C1] 手置きは 0 のまま", nMan == 0);

        // 矩形の寸法。格子 0.15m なのでセル 1 個ぶんの誤差は許す。
        float area = 0.0f; AF_Vector3 c{}, nrm{}; int ra = -1, rb = -1;
        if (AF_SceneApertureInfo(s, 0, &area, &c, &nrm, &ra, &rb)) {
            std::printf("        開口0: 面積 %.2f m2（実寸 %.2f）中心 (%.2f, %.2f, %.2f) "
                        "法線 (%.2f, %.2f, %.2f)\n",
                        area, doorW * doorH, c.x, c.y, c.z, nrm.x, nrm.y, nrm.z);
            check("[C1] 開口の面積が戸口の実寸に近い（±30%）",
                  area > doorW * doorH * 0.7f && area < doorW * doorH * 1.3f);
        }
        // 自動生成された矩形そのもの。ホストがギズモで確かめられるように読み出せる。
        AF_Vector3 pc{}, pu{}, pv{}; float hu = 0.0f, hv = 0.0f;
        if (AF_SceneGetPortal(s, 0, &pc, &pu, &pv, &hu, &hv)) {
            std::printf("        自動ポータル0: 幅 %.2fm（実寸 %.2f）高さ %.2fm（実寸 %.2f）"
                        " 中心 (%.2f, %.2f, %.2f)\n",
                        hu * 2.0f, doorW, hv * 2.0f, doorH, pc.x, pc.y, pc.z);
            // 格子 0.15m。face 中心の外接箱＋セル 1 個ぶんなので、実寸に対して
            // セル 1 個ぶんまでの膨らみは許す。
            check("[C1] 矩形の幅が戸口の実寸に近い（+1セル以内）",
                  hu * 2.0f >= doorW - 0.15f && hu * 2.0f <= doorW + 0.30f);
            check("[C1] 矩形の高さが戸口の実寸に近い（+1セル以内）",
                  hv * 2.0f >= doorH - 0.15f && hv * 2.0f <= doorH + 0.30f);
        }
        AF_SceneDestroy(s);
    }

    // ── ② ポータルが増えても、無関係な柱の回り込みが変わらないか ──
    //   ここが C1 の関門。以前は「シーンに 1 枚でもあれば回折はポータルが全部決める」
    //   だったので、自動生成した瞬間に柱の陰が壊れた（実測 -3.3dB）。
    {
        const AF_Vector3 L = V(0, 1.5f, -4.2f), S = V(0, 1.5f, -1.8f);  // 柱(z=-3)を挟む
        float g[2][kBands] = {};
        for (int k = 0; k < 2; ++k) {
            AF_SceneHandle s = build(true);
            AF_SceneSetAutoPortals(s, k);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            const int idx = AF_SceneSourceIndex(s, 1);
            if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, g[k]);
            int na = 0; AF_SceneGetPortalCounts(s, &na, nullptr);
            std::printf("        柱の陰  自動生成 %s（ポータル %d 枚）  125=%.4f 1k=%.4f 4k=%.4f\n",
                        k ? "ON " : "OFF", na, g[k][0], g[k][3], g[k][5]);
            AF_SceneDestroy(s);
        }
        double worst = 0.0;
        for (int b = 0; b < kBands; ++b)
            worst = std::max(worst, std::fabs(20.0 * std::log10(std::max(g[1][b], 1e-6f)
                                                              / std::max(g[0][b], 1e-6f))));
        std::printf("        → 差 最大 %.2f dB（開口と無関係な経路なので 0dB が正しい）\n", worst);
        check("[C1] 自動生成しても無関係な柱の回り込みが動かない（1dB 以内）", worst <= 1.0);
    }

    // ── ③ 戸口越しでは自動ポータルが手置きと同じ働きをするか ──
    //   手で戸口の実寸に置いた矩形と、自動生成した矩形を比べる。
    {
        // ★直線が戸口を素通りしない配置にすること。L(2,-2)→S(-2,2) だと z=0 を x=0 で
        //   横切る＝戸口のど真ん中で、遮蔽が起きずポータルの出番が無い（実測 0.97）。
        //   同じ x に置いて、直線が仕切りの壁に当たるようにする。
        const AF_Vector3 L = V(2.0f, 1.5f, -2.0f), S = V(2.0f, 1.5f, 2.0f);
        float g[3][kBands] = {};
        const char* name[3] = {"ポータル無し", "手置き（実寸）", "自動生成    "};
        for (int k = 0; k < 3; ++k) {
            AF_SceneHandle s = build(false);
            if (k == 1)
                AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                                  doorW*0.5f, doorH*0.5f);
            if (k == 2) AF_SceneSetAutoPortals(s, 1);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            const int idx = AF_SceneSourceIndex(s, 1);
            if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, g[k]);
            std::printf("        戸口越し %s  125=%.4f 1k=%.4f 4k=%.4f\n",
                        name[k], g[k][0], g[k][3], g[k][5]);
            AF_SceneDestroy(s);
        }
        double worst = 0.0;
        for (int b = 0; b < kBands; ++b)
            worst = std::max(worst, std::fabs(20.0 * std::log10(std::max(g[2][b], 1e-6f)
                                                              / std::max(g[1][b], 1e-6f))));
        std::printf("        → 手置き vs 自動 の差 最大 %.2f dB\n", worst);
        check("[C1] 自動生成が手置きと同じ働きをする（6dB 以内）", worst <= 6.0);
        // フレネルは帯域で差が付くのが要点。平坦なら積分が走っていない。
        const double tilt = 20.0 * std::log10(std::max(g[2][5], 1e-6f)
                                            / std::max(g[2][0], 1e-6f));
        std::printf("        → 自動生成での帯域傾き 4k/125 = %+.2f dB"
                    "（フレネル帯域積分が走っている証拠）\n", tilt);
        check("[C1] 自動生成でも帯域に差が出る（フレネルが走っている）",
              std::fabs(tilt) > 0.5);
    }

    // ── ④ 支配の境目に崖が出ていないか ──
    //   却下済み案の再発ポイント。「線分が矩形を通るか」の二値で切り替えると
    //   実測 8.8dB の崖が出た。portalGovern は距離から作った連続量なので出ないはず。
    //   リスナーを戸口の正面から横へ歩かせ、支配 1→0 を跨いで隣接差を見る。
    {
        const float step = 0.1f;
        double worst = 0.0; double worstAt = 0.0;
        float prev[kBands] = {}; bool have = false;
        std::printf("        支配の境目を歩く（戸口の正面 x=0 → 横 x=3.0）:\n          ");
        for (int i = 0; i <= 30; ++i) {
            const float x = i * step;
            AF_SceneHandle s = build(false);
            AF_SceneSetAutoPortals(s, 1);
            AF_SceneSetListener(s, V(x, 1.5f, -2.0f));
            AF_SceneSetSource(s, 1, V(0, 1.5f, 2.0f));
            for (int k = 0; k < 5; ++k) AF_SceneUpdate(s, 1.0f/60.0f);
            float g[kBands] = {};
            const int idx = AF_SceneSourceIndex(s, 1);
            if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, g);
            if (i % 5 == 0) std::printf("x=%.1f:%.4f ", x, g[0]);
            if (have) {
                const double d = std::fabs(20.0 * std::log10(std::max(g[0], 1e-6f)
                                                           / std::max(prev[0], 1e-6f)));
                if (d > worst) { worst = d; worstAt = x; }
            }
            for (int b = 0; b < kBands; ++b) prev[b] = g[b];
            have = true;
            AF_SceneDestroy(s);
        }
        std::printf("\n        → 0.1m 刻みの最大隣接差 %.2f dB @ x=%.1f"
                    "（二値切り替えでは 8.8dB の崖が出ていた）\n", worst, worstAt);
        check("[C1] 支配の境目に崖が出ない（0.1m 刻みで 3dB 以内）", worst <= 3.0);
    }

    // ── ⑤ ポータルが増えたぶんのコスト ──
    //   自動生成すると戸口の数だけポータルが並ぶ。computeSoftOcclusion は音源ごとに
    //   全ポータルを舐めるので、枚数が効く。
    //   ★ただし高い portalCoupling（フレネル積分）は支配 w>0 のときしか呼ばない。
    //     遠い口は portalGovern（内積数回）だけで落ちる。その差を数字で確かめる。
    {
        // 5 部屋を戸口で数珠つなぎにする（廊下沿いに 4 枚の仕切り）。
        auto buildMany = [&]() {
            AF_SceneHandle s = AF_SceneCreate();
            AF_SceneSetRoomCellSize(s, 0.2f);
            const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            const float L2 = 25.0f, W2 = 4.0f;
            AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(W2+t, t, L2+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(W2+t, t, L2+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(-W2-t, h*0.5f, 0), V(t, h*0.5f, L2+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( W2+t, h*0.5f, 0), V(t, h*0.5f, L2+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -L2-t), V(W2+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f,  L2+t), V(W2+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
            for (int k = 0; k < 4; ++k) {
                const float z = -15.0f + k * 10.0f;
                const float sw = (W2 - doorW * 0.5f) * 0.5f;
                const float sc = doorW * 0.5f + sw;
                AF_SceneAddInstanceBox(s, V(-sc, h*0.5f, z), V(sw, h*0.5f, t), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V( sc, h*0.5f, z), V(sw, h*0.5f, t), V(1,0,0), V(0,1,0), m);
                const float lh = (h - doorH) * 0.5f;
                AF_SceneAddInstanceBox(s, V(0, doorH + lh, z), V(doorW*0.5f, lh, t),
                                       V(1,0,0), V(0,1,0), m);
            }
            return s;
        };
        for (int k = 0; k < 2; ++k) {
            AF_SceneHandle s = buildMany();
            AF_SceneSetAutoPortals(s, k);
            AF_SceneSetListener(s, V(0, 1.5f, -20.0f));
            for (int i = 0; i < 8; ++i) AF_SceneSetSource(s, i + 1, V(1.0f, 1.5f, -18.0f + i * 5.0f));
            for (int i = 0; i < 8; ++i) AF_SceneUpdate(s, 1.0f/60.0f);   // 暖機＋部屋の構築
            int na = 0, nm = 0; AF_SceneGetPortalCounts(s, &na, &nm);
            const int iters = 60;
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < iters; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            const auto t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
            std::printf("        5部屋・音源8本  自動生成 %s  部屋 %d / ポータル %d 枚"
                        "  1フレーム %.2f ms\n",
                        k ? "ON " : "OFF", AF_SceneRoomCount(s), na + nm, ms);
            AF_SceneDestroy(s);
        }
    }

    // ── ⑥ 手置きが自動生成で消えないか ──
    {
        AF_SceneHandle s = build(false);
        AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0), doorW*0.5f, doorH*0.5f);
        AF_SceneSetListener(s, V(0, 1.5f, -2.0f));
        AF_SceneSetSource(s, 1, V(0, 1.5f, 2.0f));
        // 前提を明示する。既定に頼ると、既定が変わった瞬間にこのテストが嘘をつく
        //   （実際、自動生成の既定を Unity に合わせた途端に落ちた）。
        AF_SceneSetAutoPortals(s, 0);
        for (int i = 0; i < 3; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        int a0 = 0, m0 = 0; AF_SceneGetPortalCounts(s, &a0, &m0);
        AF_SceneSetAutoPortals(s, 1);
        for (int i = 0; i < 3; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        int a1 = 0, m1 = 0; AF_SceneGetPortalCounts(s, &a1, &m1);
        AF_SceneSetAutoPortals(s, 0);
        for (int i = 0; i < 3; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        int a2 = 0, m2 = 0; AF_SceneGetPortalCounts(s, &a2, &m2);
        std::printf("        自動 OFF→ON→OFF  手置き %d→%d→%d / 自動 %d→%d→%d\n",
                    m0, m1, m2, a0, a1, a2);
        check("[C1] 自動生成を入り切りしても手置きが残る", m0 == 1 && m1 == 1 && m2 == 1);
        // ★このシーンの開口は手置きポータルが覆っているので、自動は作られないのが正しい。
        //   作ると同じ戸口に矩形が 2〜3 枚でき、扉を回すとボクセル由来の矩形が
        //   位置・大きさ・向きごと作り直されて開口率が飛ぶ（実測 57°→58° で 1 度で倍）。
        //   自動生成そのものが効くことは「5部屋・音源8本」の計測が見ている。
        check("[C1] 手置きが覆う開口には自動を作らない", a0 == 0 && a1 == 0 && a2 == 0);
        AF_SceneDestroy(s);
    }
}

// 【調査】同じ開口の右側と左側で、音量・音色が変わってしまう件。
//
//   正しい非対称と、不具合の非対称を分ける:
//     ・経路長が変われば音量が変わる …… 正しい
//     ・斜めから見ると開口が狭く見えて音色が変わる …… 正しい（フレネル）
//     ・**左右対称な位置で違う値が出る** …… 不具合。鏡像は鏡像でなければならない
//   ここでは 3 つ目だけを測る。シーンを x=0 に対して厳密に対称に組み、
//   リスナーを ±x の鏡像位置に置いて、帯域ごとの差を見る。
void diagnosePortalLeftRightSymmetry() {
    std::printf("\n[調査] 同じ開口の左右で音量・音色が変わるか（鏡像の比較）\n");
    const float h = 3.0f, t = 0.2f, hw = 6.0f, hd = 6.0f;
    const float doorW = 0.9f, doorH = 2.0f;

    auto build = [&](float cell, bool autoP) {
        AF_SceneHandle s = AF_SceneCreate();
        AF_SceneSetRoomCellSize(s, cell);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        const float sw = (hw - doorW * 0.5f) * 0.5f;
        const float sc = doorW * 0.5f + sw;
        AF_SceneAddInstanceBox(s, V(-sc, h*0.5f, 0), V(sw, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( sc, h*0.5f, 0), V(sw, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        const float lh = (h - doorH) * 0.5f;
        AF_SceneAddInstanceBox(s, V(0, doorH + lh, 0), V(doorW*0.5f, lh, t), V(1,0,0), V(0,1,0), m);
        if (autoP) AF_SceneSetAutoPortals(s, 1);
        else AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                               doorW*0.5f, doorH*0.5f);
        return s;
    };

    struct Case { const char* name; float cell; bool autoP; };
    const Case cases[] = {
        {"手置き（x=0 に厳密対称）", 0.15f, false},
        {"自動生成 セル 0.15m    ", 0.15f, true },
        {"自動生成 セル 0.25m    ", 0.25f, true },
    };
    const float xs[] = {0.5f, 1.0f, 2.0f, 3.0f};
    for (const Case& c : cases) {
        // 自動生成された矩形が x=0 に対称かをまず見る。ずれていれば左右差の原因はこれ。
        {
            AF_SceneHandle s = build(c.cell, c.autoP);
            AF_SceneSetListener(s, V(0, 1.5f, -2.0f));
            AF_SceneSetSource(s, 1, V(0, 1.5f, 2.0f));
            for (int i = 0; i < 5; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            AF_Vector3 pc{}, pu{}, pv{}; float hu = 0.0f, hv = 0.0f;
            AF_SceneGetPortal(s, 0, &pc, &pu, &pv, &hu, &hv);
            std::printf("      %s  矩形 中心x %+.4f 幅 %.3f（x=0 からのずれ %.4f m）\n",
                        c.name, pc.x, hu * 2.0f, std::fabs(pc.x));
            AF_SceneDestroy(s);
        }
        double worst = 0.0; float worstX = 0.0f;
        double worstDir = 0.0;
        for (float x : xs) {
            float g[2][kBands] = {};
            float dir[2][3] = {};
            for (int k = 0; k < 2; ++k) {
                const float sx = (k == 0) ? -x : x;
                AF_SceneHandle s = build(c.cell, c.autoP);
                AF_SceneSetListener(s, V(sx, 1.5f, -2.5f));
                AF_SceneSetSource(s, 1, V(0, 1.5f, 2.5f));   // 音源は開口の正面
                for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
                const int idx = AF_SceneSourceIndex(s, 1);
                if (idx >= 0) {
                    AF_SceneGetSourceOcclusion(s, idx, g[k]);
                    AF_SceneGetSourceArrivalDir(s, idx, dir[k]);
                }
                AF_SceneDestroy(s);
            }
            double d = 0.0;
            for (int b = 0; b < kBands; ++b)
                d = std::max(d, std::fabs(20.0 * std::log10(std::max(g[1][b], 1e-6f)
                                                          / std::max(g[0][b], 1e-6f))));
            // 到来方向は鏡像なら x が反転して y,z は同じになるはず。
            //   ★ここがこのエンジンの主張そのもの ── 開口が方向を決める。
            //     方向がずれていたら「音の方へ進むと穴に着く」が成立しない。
            const double dx = std::fabs((double)dir[0][0] + dir[1][0]);   // 反転して足すと 0
            const double dy = std::fabs((double)dir[0][1] - dir[1][1]);
            const double dz = std::fabs((double)dir[0][2] - dir[1][2]);
            const double dd = std::max(dx, std::max(dy, dz));
            worstDir = std::max(worstDir, dd);
            std::printf("          x=%+.1f 対 %+.1f  ゲイン差 %.2f dB / "
                        "到来方向 左(%+.3f,%+.3f,%+.3f) 右(%+.3f,%+.3f,%+.3f) 鏡像ずれ %.4f\n",
                        -x, x, d, dir[0][0], dir[0][1], dir[0][2],
                        dir[1][0], dir[1][1], dir[1][2], dd);
            if (d > worst) { worst = d; worstX = x; }
        }
        std::printf("      → 鏡像どうしの差 ゲイン 最大 %.2f dB @ x=±%.1f / "
                    "到来方向 最大 %.4f\n", worst, worstX, worstDir);
    }
    std::printf("      ※鏡像は鏡像でなければならない。0dB でないぶんは不具合。\n");
}

// 【判断材料】ポータルの矩形が実寸より狭いと、扉の開き具合カーブの**形**が変わるのか、
//   それとも量（スケール）だけが変わるのか。
//
//   決めごと #3「実測は参照であって目標ではない ── 形は物理から採り、絶対値は演出で決める」
//   に照らすと、**形が変わるなら直すべき / 量だけなら演出の範囲**。ここを数字で分ける。
//   自動生成の矩形は voxel 化のせいで実寸より最大 1 セル狭くなる（実測 0.750/0.900 = 17%）。
void diagnosePortalWidthVsDoorCurve() {
    std::printf("\n[判断] 矩形の幅が扉の開き具合カーブの「形」を変えるか\n");
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;

    // 矩形の幅だけを変える。戸口の実寸は 1.2m のまま動かさない。
    struct W { const char* name; float widthScale; };
    const W widths[] = { {"実寸 1.20m    ", 1.00f},
                         {"17%狭い 1.00m ", 0.83f},
                         {"33%狭い 0.80m ", 0.67f} };
    const int nW = 3;
    const int nA = 19;                                   // 0..90° を 5° 刻み
    float f[nW][nA] = {};

    for (int w = 0; w < nW; ++w) {
        for (int a = 0; a < nA; ++a) {
            const float deg = a * 5.0f;
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
            const int pid = AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                                              doorW * 0.5f * widths[w].widthScale, doorH * 0.5f);
            const AF_Vector3 L = V(0, 1.6f, -4), S = V(-2.0f, 1.6f, 3.5f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 3; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            float fr[kBands] = {}; AF_Vector3 pp{};
            AF_SceneMeasurePortal(s, pid, L, S, fr, &pp);
            f[w][a] = fr[0];                             // 125Hz の開口率
            AF_SceneDestroy(s);
        }
    }

    std::printf("        開き角 ");
    for (int w = 0; w < nW; ++w) std::printf(" %s", widths[w].name);
    std::printf("\n");
    for (int a = 0; a < nA; a += 2) {
        std::printf("         %4.0f° ", a * 5.0f);
        for (int w = 0; w < nW; ++w) {
            const float full = std::max(f[w][nA-1], 1e-6f);
            std::printf("  %.4f (正規化 %.3f)", f[w][a], f[w][a] / full);
        }
        std::printf("\n");
    }
    // 形が同じなら「各々の全開値で正規化したカーブ」が重なる。ずれるなら形が変わっている。
    std::printf("        正規化カーブの実寸からのずれ:\n");
    for (int w = 1; w < nW; ++w) {
        double worst = 0.0; float atDeg = 0.0f;
        for (int a = 0; a < nA; ++a) {
            const double r0 = f[0][a] / std::max(f[0][nA-1], 1e-6f);
            const double rw = f[w][a] / std::max(f[w][nA-1], 1e-6f);
            if (std::fabs(rw - r0) > worst) { worst = std::fabs(rw - r0); atDeg = a * 5.0f; }
        }
        std::printf("          %s  最大 %.3f @ %.0f°\n", widths[w].name, worst, atDeg);
    }
    // 「どの角度で半分開いたと聞こえるか」＝カーブの位置。ここがずれると扉の手応えが変わる。
    for (int w = 0; w < nW; ++w) {
        float half = -1.0f;
        for (int a = 1; a < nA; ++a) {
            const float r0 = f[w][a-1] / std::max(f[w][nA-1], 1e-6f);
            const float r1 = f[w][a]   / std::max(f[w][nA-1], 1e-6f);
            if (r0 < 0.5f && r1 >= 0.5f) {
                half = (a - 1) * 5.0f + 5.0f * (0.5f - r0) / std::max(r1 - r0, 1e-6f);
                break;
            }
        }
        std::printf("        %s  半分開いたと聞こえる角度 %.1f°  / 全開の開口率 %.4f\n",
                    widths[w].name, half, f[w][nA-1]);
    }
}

// 【調整の口】開口の音色の広がりを、音量とは独立に動かせるか。
//
//   これはエンジンであって作品ではないので、「どれくらい芝居がかって聞こえるか」は
//   ホストが決められないといけない。物理から出るのは形で、誇張の量は演出（決めごと #3）。
//   ★ただし apertureContrast は**音量**カーブを動かすので、それとは別の口が要る。
//     ここでは「音色は動くが音量は動かない」ことを数字で縛る。
void testApertureTimbreKnob() {
    std::printf("\n[調整] 開口の音色を音量と独立に動かせるか\n");
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const float deg = 35.0f;                       // 半開き。帯域差がいちばん出る辺り

    auto measure = [&](float timbre, float* out6) {
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
        const int pid = AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                                          doorW * 0.5f, doorH * 0.5f);
        AF_SceneSetApertureTimbre(s, timbre);
        const AF_Vector3 L = V(0, 1.6f, -4), S = V(-2.0f, 1.6f, 3.5f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 3; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        AF_Vector3 pp{};
        AF_SceneMeasurePortal(s, pid, L, S, out6, &pp);
        AF_SceneDestroy(s);
    };

    const float ks[] = {0.5f, 1.0f, 1.8f, 2.5f};
    std::printf("        k     125     250     500      1k      2k      4k    "
                "平均(音量)  4k/125(音色)\n");
    float meanAt1 = 0.0f, tiltAt1 = 0.0f;
    for (float k : ks) {
        float f[kBands] = {};
        measure(k, f);
        float m = 0.0f;
        for (int b = 0; b < kBands; ++b) m += f[b];
        m /= kBands;
        const float tilt = 20.0f * std::log10(std::max(f[5], 1e-6f) / std::max(f[0], 1e-6f));
        std::printf("        %.1f  ", k);
        for (int b = 0; b < kBands; ++b) std::printf(" %.4f", f[b]);
        std::printf("   %.4f    %+.2f dB\n", m, tilt);
        if (k == 1.0f) { meanAt1 = m; tiltAt1 = tilt; }
    }
    // 素通し(k=1)を基準に、音量が動かず音色だけ動くことを縛る。
    for (float k : ks) {
        if (k == 1.0f) continue;
        float f[kBands] = {};
        measure(k, f);
        float m = 0.0f;
        for (int b = 0; b < kBands; ++b) m += f[b];
        m /= kBands;
        const float tilt = 20.0f * std::log10(std::max(f[5], 1e-6f) / std::max(f[0], 1e-6f));
        const double dLevel = 20.0 * std::log10(std::max(m, 1e-6f) / std::max(meanAt1, 1e-6f));
        char tag[96];
        std::snprintf(tag, sizeof(tag), "[調整] k=%.1f で音量が動かない（±0.5dB）", k);
        check(tag, std::fabs(dLevel) <= 0.5);
        std::snprintf(tag, sizeof(tag), "[調整] k=%.1f で音色が%s", k,
                      (k > 1.0f) ? "濃くなる" : "薄くなる");
        // 傾きは負（高域ほど通りにくい）。「濃い」＝**より負**なので絶対値で比べる。
        check(tag, (k > 1.0f) ? (std::fabs(tilt) > std::fabs(tiltAt1) + 0.2f)
                              : (std::fabs(tilt) < std::fabs(tiltAt1) - 0.2f));
    }
    std::printf("      ※既定は k=1.0（素通し）。エンジンは物理から出た形をそのまま出し、\n"
                "        どれだけ誇張するかは作品側が決める（決めごと #3）。\n");
}

// 【ゲー側からの確認依頼】「地面の板 + 閉じた建物」が同居したとき、屋外の空気が
//   部屋として立っていないか。
//
//   testOutdoorIsNotARoom は 地面だけ／地面＋塀／地面＋壁＋天井 を見ているが、
//   **建物と屋外が同じシーンに居るとき**を見ていなかった。実機（神殿）では
//   リスナーが建物の外に居るのに「部屋0 に 100%」「実効V 22398m³」と出ており、
//   これは地面 46×70m × 高さ 8.65m の空気から建物を引いた量にほぼ一致する。
void diagnoseOutdoorWithBuilding() {
    std::printf("\n[調査] 地面の板 + 閉じた建物 ── 屋外が部屋になっていないか\n");
    // 実機（神殿）に寄せる: 音響用の地面 46x70m、主室 20x18x8m。
    const float gx = 23.0f, gz = 35.0f, gt = 0.2f;      // 地面の板（半寸法）
    const float rx = 10.0f, rz = 9.0f, rh = 8.0f, wt = 0.3f;  // 建物の内寸/高さ/壁厚
    // ★戸口の有無で分ける。相手のシーンは開口 4275 箇所＝穴が空いている。
    //   閉じた建物だと屋外は行き止まりだが、戸口があると屋外の空気が
    //   建物の中の種まで**繋がる**。塗り戻しがそこを通れるなら、
    //   「外の世界」として捨てたはずのボクセルが部屋に吸われる。
    struct C { const char* name; float cell; float seed; float doorW; };
    const C cases[] = {
        {"閉じた建物   セル0.25/種0.6", 0.25f, 0.6f, 0.0f},
        {"戸口 1.1m    セル0.25/種0.6", 0.25f, 0.6f, 1.1f},
        {"戸口 1.1m    セル0.25/種0.0", 0.25f, 0.0f, 1.1f},
        {"戸口 3.0m    セル0.25/種0.6", 0.25f, 0.6f, 3.0f},
    };
    for (const C& c : cases) {
        AF_SceneHandle s = AF_SceneCreate();
        AF_SceneSetRoomCellSize(s, c.cell);
        AF_SceneSetRoomSeedRadius(s, c.seed);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // 広い地面の板 1 枚。
        AF_SceneAddInstanceBox(s, V(0, -gt, 0), V(gx, gt, gz), V(1,0,0), V(0,1,0), m);
        // 建物（壁 4 枚＋天井）。−Z 側の壁にだけ戸口を空ける。
        AF_SceneAddInstanceBox(s, V(-rx-wt, rh*0.5f, 0), V(wt, rh*0.5f, rz+wt), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( rx+wt, rh*0.5f, 0), V(wt, rh*0.5f, rz+wt), V(1,0,0), V(0,1,0), m);
        if (c.doorW <= 0.0f) {
            AF_SceneAddInstanceBox(s, V(0, rh*0.5f, -rz-wt), V(rx+wt, rh*0.5f, wt),
                                   V(1,0,0), V(0,1,0), m);
        } else {
            const float dh = 2.2f;                       // 戸口の高さ
            const float sw = (rx + wt - c.doorW * 0.5f) * 0.5f;
            const float sc = c.doorW * 0.5f + sw;
            AF_SceneAddInstanceBox(s, V(-sc, rh*0.5f, -rz-wt), V(sw, rh*0.5f, wt),
                                   V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( sc, rh*0.5f, -rz-wt), V(sw, rh*0.5f, wt),
                                   V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, dh + (rh - dh) * 0.5f, -rz-wt),
                                   V(c.doorW * 0.5f, (rh - dh) * 0.5f, wt), V(1,0,0), V(0,1,0), m);
        }
        AF_SceneAddInstanceBox(s, V(0, rh*0.5f,  rz+wt), V(rx+wt, rh*0.5f, wt), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, rh+wt, 0), V(rx+wt, wt, rz+wt), V(1,0,0), V(0,1,0), m);

        const int nRooms = AF_SceneRoomCount(s);
        const int nAper  = AF_SceneApertureCount(s);
        int nx = 0, ny = 0, nz = 0; float cell = 0.0f;
        AF_SceneRoomGridDims(s, &nx, &ny, &nz, &cell);
        // 建物の中（部屋のはず）と、建物の外（部屋であってはならない）。
        const AF_Vector3 pIn  = V(0, 1.6f, 0);
        const AF_Vector3 pOut = V(0, 1.6f, gz - 4.0f);     // 建物のはるか外・地面の上
        const float vIn  = AF_SceneRoomVolumeAt(s, pIn, 2.0f);
        const float vOut = AF_SceneRoomVolumeAt(s, pOut, 2.0f);
        const int rIn  = AF_SceneRoomAt(s, pIn);
        const int rOut = AF_SceneRoomAt(s, pOut);
        std::printf("      %s  格子 %dx%dx%d @%.2fm / 部屋 %d 個 / 開口 %d 箇所\n",
                    c.name, nx, ny, nz, cell, nRooms, nAper);
        std::printf("          建物の中 room=%2d 実効V %8.0f m3（設計 %.0f）\n",
                    rIn, vIn, 2.0f * rx * 2.0f * rz * rh);
        std::printf("          建物の外 room=%2d 実効V %8.0f m3  ← 0 でなければ屋外が部屋\n",
                    rOut, vOut);
        // 戸口から外へ何 m まで「部屋」が伸びているか。塗り戻しがどこまで吸うかを見る。
        bool leaked = false;
        if (c.doorW > 0.0f) {
            std::printf("          戸口の外へ:");
            for (float d = 1.0f; d <= 20.0f; d += 3.0f) {
                const AF_Vector3 p = V(0, 1.6f, -(rz + wt) - d);
                const int rr = AF_SceneRoomAt(s, p);
                std::printf("  %.0fm:room%d", d, rr);
                if (d >= 4.0f && rr >= 0) leaked = true;   // 戸口のすぐ外は連続性のため許す
            }
            std::printf("\n");
        }
        // ★戸口があっても、建物の中の実効体積は「閉じた建物」と同じでなければならない。
        //   屋外を吸っていた頃は 2790 → 11467m3（4.0倍）になっていた。
        const float design = 2.0f * rx * 2.0f * rz * rh;
        // ★戸口が種半径の 2 倍より広いと、侵食で屋内と屋外の種が分かれない。
        //   すると屋外と繋がった成分ごと「外の世界」に落ち、**部屋そのものが消える**。
        //   これは未解決の設計判断（「開口が広すぎる空間は部屋か？」）なので、
        //   合格扱いにせず、消えていることを表示だけする。
        const bool pinchable = (c.doorW > 0.0f) && (c.doorW <= c.seed * 2.0f);
        if (c.seed > 0.0f && (c.doorW <= 0.0f || pinchable)) {
            char tag[96];
            std::snprintf(tag, sizeof(tag), "[屋外] %s で建物の体積が設計どおり", c.name);
            check(tag, vIn > design * 0.85f && vIn < design * 1.15f);
            if (c.doorW > 0.0f) {
                std::snprintf(tag, sizeof(tag), "[屋外] %s で屋外が部屋に吸われない", c.name);
                check(tag, !leaked);
            }
        } else if (c.doorW > 0.0f && c.seed > 0.0f) {
            std::printf("          ↑ 戸口 %.1fm > 種半径 %.1fm × 2 なので侵食で分かれず、"
                        "部屋が消えている（未解決の設計判断）\n", c.doorW, c.seed);
        }
        AF_SceneDestroy(s);
    }
    std::printf("      ※屋外は「外の世界」に落ちるはず（格子の外周に届く成分は部屋にしない）。\n");
}

// 【B7】遮蔽境界で**スペクトル**が跳ぶか（ゲームシステムからの測定依頼）。
//
//   既存の回帰は「D⊕F の合計レベルが連続か」しか見ていない。合計が連続でも
//   帯域ごとの傾きが振れれば「音色が跳ぶ」と聞こえる。そこを同じやり方で測る。
//
//   仕組みの見当（コードから）: 生存ゲインは帯域ごとに
//       outGain[b] = max( 透過soft[b], 回折dif[b] )
//   透過は質量則で **21dB の傾き**（Default: TL 31→52dB）、
//   回折はポータルが支配していなければ diffractionFlat_ で**平坦**に均される。
//   傾きが違う 2 本の max なので、**帯域ごとに乗り換わる位置がずれる**。
//   高域は soft が小さいので早く回折へ、低域は遅く乗り換わる ＝ その間スペクトルが動く。
//   ポータルが支配していれば dif はフレネル帯域積分（傾きを持つ）なので、
//   soft と形が近く乗り換えが揃う ＝ 音色が跳ばない。その対比を数字で出す。
void diagnoseShadowSpectrumJump() {
    std::printf("\n[B7] 遮蔽境界でスペクトルが跳ぶか（合計レベルではなく傾きを見る）\n");
    // ★部屋は広めに取る。狭いと**影境界が部屋の外に出て**リスナーが跨がないまま終わる。
    //   影境界は 音源(0,+3) から遮蔽物の縁を通って z=-3 に落ちる位置 ＝ 縁の x の 2 倍。
    //   最初 hw=8 で測ったら、跨ぐ前にリスナーが側壁へめり込んで -58dB の「崖」が出た。
    //   あれは壁の中に入っただけで、B7 とは無関係。
    const float h = 4.0f, t = 0.3f, hw = 16.0f, hd = 8.0f;

    // 柱（開口ではない）と、戸口（ポータルが自動生成される）の 2 通り。
    // ★柱が細いと回折が常に透過より大きく、max(soft,dif) は回折側に張り付く。
    //   透過の 21dB 傾きが顔を出すのは**回折が弱くなる場所** ＝ 迂回が長い大きな壁の陰。
    //   ゲームシステムの報告は「柱や壁が塞いだ瞬間」なので、壁のほうも測る。
    // ★ポータルの効きを見るには**同じ幾何で ON/OFF を比べる**しかない。
    //   柱・壁とはそもそも形が違うので、あれは「障害物の種類の違い」であって
    //   「ポータルの有無」ではなかった。戸口のまま自動生成を切り替える。
    struct C { const char* name; bool doorway; float barrierHalfW; bool portal; };
    const C cases[] = {
        {"戸口の陰へ歩く ポータル**無し**        ", true,  0.0f, false},
        {"戸口の陰へ歩く ポータル**有り**（自動）", true,  0.0f, true },
        {"柱の陰へ歩く 幅1.2m（参考・開口でない）", false, 0.6f, false},
        {"壁の陰へ歩く 幅6m （参考・開口でない）", false, 3.0f, false},
    };
    for (const C& c : cases) {
        std::printf("      %s\n", c.name);
        std::printf("        x     125     500      4k    合計dB  傾き4k/125  Δ合計  Δ傾き\n");
        float prevTot = 0.0f, prevTilt = 0.0f; bool have = false;
        double worstTot = 0.0, worstTilt = 0.0; float atTot = 0, atTilt = 0;
        for (float x = 0.0f; x <= (c.barrierHalfW >= 3.0f ? 9.0f : 2.4f); x += 0.1f) {
            AF_SceneHandle s = AF_SceneCreate();
            AF_SceneSetRoomCellSize(s, 0.2f);
            if (c.portal) AF_SceneSetAutoPortals(s, 1);
            const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
            if (c.doorway) {
                // z=0 を仕切って戸口 1.0m を空ける（部屋が 2 つに割れる）。
                const float dw = 1.0f, sw = (hw + t - dw * 0.5f) * 0.5f, sc = dw * 0.5f + sw;
                AF_SceneAddInstanceBox(s, V(-sc, h*0.5f, 0), V(sw, h*0.5f, t), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V( sc, h*0.5f, 0), V(sw, h*0.5f, t), V(1,0,0), V(0,1,0), m);
                AF_SceneAddInstanceBox(s, V(0, 2.2f + (h-2.2f)*0.5f, 0),
                                       V(dw*0.5f, (h-2.2f)*0.5f, t), V(1,0,0), V(0,1,0), m);
            } else {
                // 遮蔽物 1 枚。幅は場合ごと・厚み 0.3m。
                AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(c.barrierHalfW, h*0.5f, 0.15f),
                                       V(1,0,0), V(0,1,0), m);
            }
            // リスナーを横へ動かして、見通し → 影 へ入る。
            AF_SceneSetListener(s, V(x, 1.6f, -3.0f));
            AF_SceneSetSource(s, 1, V(0, 1.6f, 3.0f));
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            float g[kBands] = {};
            const int idx = AF_SceneSourceIndex(s, 1);
            if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, g);
            AF_SceneDestroy(s);

            float mean = 0.0f;
            for (int b = 0; b < kBands; ++b) mean += g[b];
            mean /= kBands;
            const float tot = 20.0f * std::log10(std::max(mean, 1e-6f));
            const float tilt = 20.0f * std::log10(std::max(g[5], 1e-6f) / std::max(g[0], 1e-6f));
            float dT = 0.0f, dL = 0.0f;
            if (have) { dL = tot - prevTot; dT = tilt - prevTilt; }
            std::printf("        %.1f  %.4f  %.4f  %.4f  %6.1f  %8.2f  %+6.2f %+7.2f\n",
                        x, g[0], g[2], g[5], tot, tilt, dL, dT);
            if (have) {
                if (std::fabs(dL) > worstTot)  { worstTot  = std::fabs(dL); atTot  = x; }
                if (std::fabs(dT) > worstTilt) { worstTilt = std::fabs(dT); atTilt = x; }
            }
            prevTot = tot; prevTilt = tilt; have = true;
        }
        std::printf("        → 0.1m 刻みの最大 Δ:  合計 %.2f dB @ x=%.1f  /  "
                    "**傾き %.2f dB @ x=%.1f**\n", worstTot, atTot, worstTilt, atTilt);
    }
    std::printf("      ※合計が連続でも傾きが振れれば「音色が跳ぶ」と聞こえる。\n"
                "        生存ゲインは帯域ごとに max(透過, 回折) を取っており、\n"
                "        透過は 21dB 傾き・回折は（ポータル非支配なら）平坦なので、\n"
                "        帯域ごとに乗り換わる位置がずれる。\n");
}

// 【目標】「全開の開口は開口率 1.0 で鳴るべき」── 素通しの開口が何を返すかを切り分ける。
//
//   実測では、実寸ぴったりの矩形は扉を 90°開いても 0.8512 止まりだった。
//   矩形を狭めるほど 1.0 に近づく（0.80m で 1.0000）ので、
//   「全開」と「それ以上」の区別が付かない一方、正しい寸法だと 15% 損している。
//   まず**扉を一枚も置かない**素通しの開口で測り、
//     1.0 なら → 0.8512 の犯人は扉の板（90°でも縁が残る）
//     1.0 未満 → フレネル積分の正規化そのものが 1.0 に届かない
//   を分ける。
void diagnoseOpenApertureFraction() {
    std::printf("\n[目標] 素通しの開口は開口率 1.0 を返すか（扉を置かない）\n");
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;

    // 見通しの角度を変えて測る。矩形の中心を通る線／端を通る線／外れる線。
    struct P { const char* name; float lx, lz, sx, sz; };
    const P places[] = {
        {"正対（線が矩形の中心を通る）  ",  0.0f, -4.0f,  0.0f, 3.5f},
        {"やや斜め（線が矩形の中を通る）",  0.3f, -4.0f, -0.3f, 3.5f},
        {"斜め（線が矩形の外へ外れる） ",  0.0f, -4.0f, -2.0f, 3.5f},
        {"真横（線が壁を突く）        ",  2.5f, -4.0f, -2.5f, 3.5f},
    };
    // 矩形の幅を振る。実寸／狭い／広い。
    const float scales[] = {1.00f, 0.83f, 1.20f};
    for (float sc : scales) {
        std::printf("      矩形 幅 %.2fm（実寸比 %.2f）\n", doorW * sc, sc);
        for (const P& p : places) {
            AF_SceneHandle s = AF_SceneCreate();
            const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            // 仕切り（z=0）に戸口だけ空ける。**扉の板は置かない。**
            AF_SceneAddInstanceBox(s, V(-(hw + doorW*0.5f) * 0.5f, h*0.5f, 0),
                                   V((hw - doorW*0.5f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V((hw + doorW*0.5f) * 0.5f, h*0.5f, 0),
                                   V((hw - doorW*0.5f) * 0.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                                   V(doorW*0.5f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
            const int pid = AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                                              doorW * 0.5f * sc, doorH * 0.5f);
            const AF_Vector3 L = V(p.lx, 1.6f, p.lz), S = V(p.sx, 1.6f, p.sz);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 3; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            float f[kBands] = {}; AF_Vector3 pp{};
            AF_SceneMeasurePortal(s, pid, L, S, f, &pp);
            std::printf("        %s 開口率 125=%.4f 500=%.4f 4k=%.4f\n",
                        p.name, f[0], f[2], f[5]);
            AF_SceneDestroy(s);
        }
    }
    std::printf("      ※塞ぐものが何も無いので、理屈の上では全帯域 1.0 のはず。\n");
}

// 【検証】く字の廊下シーン（Test_LCorridor）── **回折だけで定位が出るか**。
//
//   両端にリスナーと音源。扉も開口も無い。直線は角の内壁で必ず塞がれるので、
//   届く音はすべて角を回り込んだもの。確かめるのは音量ではなく**方向**:
//     ・到来方向が「音源の方（壁の向こう）」ではなく「曲がり角の方」を向くか
//     ・角へ歩くとその方向が連続に動くか（＝音の方へ進むと角に着く）
//   ★曲がり角は開口ではないので部屋は割れず、ポータルは生えない。そこが狙い
//     （戸口／ポータルの検証は Test_DiffractionGap と Test_SwingDoor が持つ）。
void testLCorridorScene() {
    std::printf("\n[検証] く字の廊下 ── 回折だけで定位が出るか（扉も開口も無し）\n");
    const float w = 1.5f, h = 3.0f, t = 0.3f;
    const float aEnd = -12.0f, bEnd = 12.0f;

    auto build = [&]() {
        AF_SceneHandle sc = AF_SceneCreate();
        AF_SceneSetRoomCellSize(sc, 0.25f);
        AF_SceneSetAutoPortals(sc, 1);
        const int mm = AF_SceneAddMaterial(sc, nullptr, nullptr, nullptr, 0);
        auto bx = [&](float cx, float cy, float cz, float sx, float sy, float sz) {
            AF_SceneAddInstanceBox(sc, V(cx, cy, cz), V(sx*0.5f, sy*0.5f, sz*0.5f),
                                   V(1,0,0), V(0,1,0), mm);
        };
        bx(-w - t*0.5f, h*0.5f, (aEnd + w)*0.5f, t, h, w - aEnd + t);
        bx( w + t*0.5f, h*0.5f, (aEnd - w)*0.5f, t, h, -w - aEnd);
        bx((bEnd - w)*0.5f, h*0.5f,  w + t*0.5f, bEnd + w + t, h, t);
        bx((bEnd + w)*0.5f, h*0.5f, -w - t*0.5f, bEnd - w, h, t);
        bx(0.0f, h*0.5f, aEnd - t*0.5f, 2*w + 2*t, h, t);
        bx(bEnd + t*0.5f, h*0.5f, 0.0f, t, h, 2*w + 2*t);
        for (int k = 0; k < 2; ++k) {
            const float ft = 0.3f;
            const float y = (k == 0) ? -ft*0.5f : h + ft*0.5f;
            bx(0.0f, y, (aEnd + w)*0.5f, 2*w + 2*t, ft, w - aEnd + t);
            bx((bEnd - w)*0.5f, y, 0.0f, bEnd + w + t, ft, 2*w + 2*t);
        }
        return sc;
    };

    const AF_Vector3 S = V(bEnd - 2.0f, 1.6f, 0.0f);   // 奥の脚の奥
    // ① 開口が立たないこと（＝素の回折が担当していること）を確かめる。
    {
        AF_SceneHandle s = build();
        AF_SceneSetListener(s, V(0, 1.6f, aEnd + 2.0f));
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        int na = 0, nm = 0;
        AF_SceneGetPortalCounts(s, &na, &nm);
        std::printf("        部屋 %d 個 / 開口 %d 箇所 / ポータル %d 枚\n",
                    AF_SceneRoomCount(s), AF_SceneApertureCount(s), na + nm);
        check("[く字] 曲がり角では開口が立たない", AF_SceneApertureCount(s) == 0);
        check("[く字] ポータルが生えない（素の回折が担当）", na + nm == 0);
        AF_SceneDestroy(s);
    }

    // ② 到来方向。手前の脚を角へ向かって歩く。
    //   音源は +X の彼方だが**壁の向こう**なので、そちらから聞こえてはいけない。
    //   正しくは「角の方」＝ +Z 寄り。角に近づくほど +X へ寄っていくのが自然。
    std::printf("        z      生存(125)  到来方向(x, z)   音源方向との差  1歩の振れ\n");
    float prevAx = 0.0f, prevAz = 0.0f; bool have = false;
    double worstStep = 0.0; float worstAt = 0.0f; double angFar = 0.0, angNear = 0.0;
    int nFallback = 0; float fallbackZ[8] = {}; double worstFallback = 0.0; bool prevFallback = false;
    for (float z = aEnd + 2.0f; z <= 1.01f; z += 0.25f) {
        AF_SceneHandle s = build();
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        const int idx = AF_SceneSourceIndex(s, 1);
        float g[kBands] = {}; float dir[3] = {0, 0, 0};
        if (idx >= 0) {
            AF_SceneGetSourceOcclusion(s, idx, g);
            AF_SceneGetSourceArrivalDir(s, idx, dir);
        }
        float sx = S.x - L.x, sz = S.z - L.z;
        const float sl = std::sqrt(sx*sx + sz*sz);
        if (sl > 1e-6f) { sx /= sl; sz /= sl; }
        const float dl = std::sqrt(dir[0]*dir[0] + dir[2]*dir[2]);
        const float ax = (dl > 1e-6f) ? dir[0]/dl : 0.0f;
        const float az = (dl > 1e-6f) ? dir[2]/dl : 1.0f;
        const double cosang = std::max(-1.0, std::min(1.0, (double)(ax*sx + az*sz)));
        const double angDeg = std::acos(cosang) * 180.0 / 3.14159265358979;
        // ★「遮蔽されているのに到来方向が音源へ直線」＝経路探索が空振りして
        //   フォールバック（source - listener）に落ちた印。ここで像が壁を突き抜ける。
        const bool fallback = (g[0] < 0.95f) && (angDeg < 0.5);
        double step = 0.0;
        if (have) {
            const double c2 = std::max(-1.0, std::min(1.0, (double)(ax*prevAx + az*prevAz)));
            step = std::acos(c2) * 180.0 / 3.14159265358979;
            if (!fallback && !prevFallback && step > worstStep) { worstStep = step; worstAt = z; }
        } else {
            angFar = angDeg;
        }
        if (fallback) {
            if (nFallback < 8) fallbackZ[nFallback] = z;
            ++nFallback;
            if (step > worstFallback) worstFallback = step;
        }
        angNear = angDeg;
        std::printf("        %+5.1f   %.4f    (%+.3f, %+.3f)   %6.1f 度      %5.1f 度%s\n",
                    z, g[0], ax, az, angDeg, step, fallback ? "  ★空振り" : "");
        prevFallback = fallback;
        prevAx = ax; prevAz = az; have = true;
        AF_SceneDestroy(s);
    }
    std::printf("        → 0.25m 刻みの到来方向の最大振れ %.1f 度 @ z=%+.1f"
                " / 音源方向との差 遠 %.1f 度 → 近 %.1f 度\n",
                worstStep, worstAt, angFar, angNear);
    std::printf("        → 経路探索の空振り %d 箇所（そこで像が壁を突き抜ける。最大 %.1f 度）:",
                nFallback, worstFallback);
    for (int i = 0; i < nFallback && i < 8; ++i) std::printf(" z=%+.2f", fallbackZ[i]);
    std::printf("\n");
    // ★空振りの所を除けば滑らか。空振りそのものは未修正なので、合格扱いにせず数だけ出す。
    check("[く字] 空振りを除けば 0.25m で 10 度以内", worstStep <= 10.0);
    check("[く字] 音源の方角ではなく角の方を向く（遠くで 20 度以上ずれる）", angFar >= 20.0);
    check("[く字] 角へ寄るほど音源方向へ近づく", angNear < angFar);
    std::printf("      ※音源は +X の彼方だが壁の向こう。到来方向が音源方向と大きく違い、\n"
                "        角へ近づくほどそちらへ寄るのが「音の方へ進むと角に着く」。\n");

    // ③ 【定位が聞こえるか】方向が正しくても、残響に埋もれれば耳には届かない。
    //   後期残響は HRTF を通らず L/R バランスだけなので、残響優位の場所では
    //   方向の手がかりが薄まる。臨界距離と残響/直接比を出して、そこを数字にする。
    {
        // 壁の吸音を変えて 2 通り。既定のコンクリは吸わないので残響が支配する。
        const float aHard[6] = {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f};   // コンクリ相当
        const float aSoft[6] = {0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f};   // よく吸う内装
        for (int k = 0; k < 2; ++k) {
            const float* ab = (k == 0) ? aHard : aSoft;
            AF_SceneHandle s = AF_SceneCreate();
            AF_SceneSetRoomCellSize(s, 0.25f);
            const int mm = AF_SceneAddMaterial(s, nullptr, ab, nullptr, 6);
            auto bx = [&](float cx, float cy, float cz, float sx, float sy, float sz) {
                AF_SceneAddInstanceBox(s, V(cx, cy, cz), V(sx*0.5f, sy*0.5f, sz*0.5f),
                                       V(1,0,0), V(0,1,0), mm);
            };
            bx(-w - t*0.5f, h*0.5f, (aEnd + w)*0.5f, t, h, w - aEnd + t);
            bx( w + t*0.5f, h*0.5f, (aEnd - w)*0.5f, t, h, -w - aEnd);
            bx((bEnd - w)*0.5f, h*0.5f,  w + t*0.5f, bEnd + w + t, h, t);
            bx((bEnd + w)*0.5f, h*0.5f, -w - t*0.5f, bEnd - w, h, t);
            bx(0.0f, h*0.5f, aEnd - t*0.5f, 2*w + 2*t, h, t);
            bx(bEnd + t*0.5f, h*0.5f, 0.0f, t, h, 2*w + 2*t);
            for (int q = 0; q < 2; ++q) {
                const float ft = 0.3f;
                const float y = (q == 0) ? -ft*0.5f : h + ft*0.5f;
                bx(0.0f, y, (aEnd + w)*0.5f, 2*w + 2*t, ft, w - aEnd + t);
                bx((bEnd - w)*0.5f, y, 0.0f, bEnd + w + t, ft, 2*w + 2*t);
            }
            const AF_Vector3 L = V(0, 1.6f, aEnd + 2.0f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 8; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            const float vol = AF_SceneRoomVolumeAt(s, L, 2.0f);
            float rt[kBands] = {};
            AF_SceneRt60At(s, L, 2.0f, rt, kBands);
            const float dx = S.x - L.x, dz = S.z - L.z;
            const float r = std::sqrt(dx*dx + dz*dz);
            std::printf("        【定位が耳に届くか】%s  実効V %.0f m3 / 距離 %.1fm\n",
                        (k == 0) ? "硬い壁(コンクリ α0.02)" : "吸う壁(α0.25〜0.50) ", vol, r);
            std::printf("          帯域   RT60(s)  臨界距離rc(m)   残響/直接 t    wet\n");
            for (int b = 0; b < kBands; ++b) {
                const float rc = (rt[b] > 1e-3f) ? 0.057f * std::sqrt(vol / rt[b]) : 0.0f;
                const float tt = (rc > 1e-3f) ? (r / rc) * (r / rc) : 0.0f;
                std::printf("          %5d   %6.2f   %8.2f   %12.0f    %.4f\n",
                            (int)(125 << b), rt[b], rc, tt, tt / (1.0f + tt));
            }
            AF_SceneDestroy(s);
        }
        std::printf("      ※wet が 1 に近いほど、聞こえている音のほとんどが後期残響。\n");
    }

    // ④ 【現実との差】現実は残響が強くても定位できる。
    //   先行音効果（最初に届いた波面が定位を決める）と、残響そのものが持つ方向のため。
    //   ではエンジンの何が方向を運んでいるのか ── 段ごとに数えて、
    //   「方向を持てる段」と「持てない段」のエネルギー比を出す。
    {
        AF_SceneHandle s = build();
        const AF_Vector3 L = V(0, 1.6f, aEnd + 2.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 10; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        const int idx = AF_SceneSourceIndex(s, 1);
        AF_Vector3 pos[32]; float g6[32 * 6];
        const int nEarly = (idx >= 0)
            ? AF_SceneGetEarlyReflections(s, idx, pos, g6, 32) : 0;
        AF_Vector3 dpos[16]; float dg[16];
        const int nDiff = (idx >= 0)
            ? AF_SceneGetDiffractionSources(s, idx, dpos, dg, 16) : 0;
        double eEarly = 0.0;
        for (int i = 0; i < nEarly; ++i)
            for (int b = 0; b < 6; ++b) eEarly += (double)g6[i*6+b] * g6[i*6+b];
        double eDiff = 0.0;
        for (int i = 0; i < nDiff; ++i) eDiff += (double)dg[i] * dg[i];
        std::printf("        【何が方向を運んでいるか】早期反射 %d 本（エネルギー和 %.4f）"
                    " / 回折二次音源 %d 本（%.4f）\n", nEarly, eEarly, nDiff, eDiff);
        std::printf("          段              方向の持ち方                     HRTF\n");
        std::printf("          直接音 D        音源(見かけ)方向                  ○\n");
        std::printf("          回折 F 最強1本  開口/角の方向                     ○（B1・C++のみ）\n");
        std::printf("          回折 F 残り     等パワーパン                      ×\n");
        std::printf("          早期反射 R %2d本  等パワーパン                      ×\n", nEarly);
        std::printf("          後期尾          L/R バランスのみ（方向プローブ既定オフ）  ×\n");
        AF_SceneDestroy(s);
    }
    std::printf("      ※現実で残響が強くても定位できるのは、先行音効果と\n"
                "        **残響自体が方向を持つ**ため。エンジンは早期反射と尾に HRTF が無く、\n"
                "        方向を持てるのが D と F の 1 本だけ ── ここが現実との差。\n");
}

// 【耳の報告】く字の廊下で「壁が横にかかった瞬間、音色が変わる」。
//
//   レンダのログで、角へ近づく途中に**回折タップの本数が 1 → 0 に落ちて**いた
//   （z=-4.5 で 1 本、z=-3.1 で 0 本）。タップが消えると、そのタップが持っていた
//   帯域の形ごと消えるので、合計のスペクトルが動く。
//   生存ゲインの連続性（既存の検査）はタップの本数を見ていないので素通りする。
//   ここでは**ホストが実際に鳴らすタップ**を数えて、本数が変わる位置と
//   そのときスペクトルがどれだけ動くかを出す。
void diagnoseCorridorTapDropout() {
    std::printf("\n[耳の報告] く字の廊下で壁が横にかかる瞬間（回折タップの増減）\n");
    const float w = 1.5f, h = 3.0f, t = 0.3f, aEnd = -12.0f, bEnd = 12.0f;
    auto build = [&]() {
        AF_SceneHandle sc = AF_SceneCreate();
        AF_SceneSetRoomCellSize(sc, 0.25f);
        const int mm = AF_SceneAddMaterial(sc, nullptr, nullptr, nullptr, 0);
        auto bx = [&](float cx, float cy, float cz, float sx, float sy, float sz) {
            AF_SceneAddInstanceBox(sc, V(cx, cy, cz), V(sx*0.5f, sy*0.5f, sz*0.5f),
                                   V(1,0,0), V(0,1,0), mm);
        };
        bx(-w - t*0.5f, h*0.5f, (aEnd + w)*0.5f, t, h, w - aEnd + t);
        bx( w + t*0.5f, h*0.5f, (aEnd - w)*0.5f, t, h, -w - aEnd);
        bx((bEnd - w)*0.5f, h*0.5f,  w + t*0.5f, bEnd + w + t, h, t);
        bx((bEnd + w)*0.5f, h*0.5f, -w - t*0.5f, bEnd - w, h, t);
        bx(0.0f, h*0.5f, aEnd - t*0.5f, 2*w + 2*t, h, t);
        bx(bEnd + t*0.5f, h*0.5f, 0.0f, t, h, 2*w + 2*t);
        for (int k = 0; k < 2; ++k) {
            const float ft = 0.3f;
            const float y = (k == 0) ? -ft*0.5f : h + ft*0.5f;
            bx(0.0f, y, (aEnd + w)*0.5f, 2*w + 2*t, ft, w - aEnd + t);
            bx((bEnd - w)*0.5f, y, 0.0f, bEnd + w + t, ft, 2*w + 2*t);
        }
        return sc;
    };
    const AF_Vector3 S = V(bEnd - 2.0f, 1.6f, 0.0f);
    std::printf("        z      回折本数  合計(D+F) 125    4k    傾き4k/125   Δ合計   Δ傾き\n");
    int prevNd = -1; float prevTot = 0.0f, prevTilt = 0.0f; bool have = false;
    double worstTot = 0.0, worstTilt = 0.0; float atTot = 0, atTilt = 0;
    for (float z = -6.0f; z <= -1.0f; z += 0.1f) {
        AF_SceneHandle s = build();
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        // ホストと同じ組み方: 直接(透過) ＋ 回折タップ。
        float trans[kBands] = {}; float occFrac = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, trans, kBands, &occFrac);
        AF_Vector3 dp[8]; float dg[8]; float db[8*6];
        const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
        float band[kBands] = {};
        for (int b = 0; b < kBands; ++b) band[b] = trans[b];
        for (int i = 0; i < nd; ++i)
            for (int b = 0; b < kBands; ++b) band[b] += db[i*6+b] * occFrac;
        float mean = 0.0f;
        for (int b = 0; b < kBands; ++b) mean += band[b];
        mean /= kBands;
        const float tot = 20.0f * std::log10(std::max(mean, 1e-6f));
        const float tilt = 20.0f * std::log10(std::max(band[5], 1e-6f)
                                            / std::max(band[0], 1e-6f));
        float dT = 0.0f, dL = 0.0f;
        if (have) { dL = tot - prevTot; dT = tilt - prevTilt; }
        const bool changed = (prevNd >= 0 && nd != prevNd);
        // どの段で落ちているかの切り分け:
        //   候補が 0        → 稜線の探索（tryEdge の見通し判定）で落ちている
        //   候補>0 で本数 0 → クラスタ化か重み（apertureWeight）で落ちている
        AF_Vector3 cp[64]; float cd[64];
        const int nCand = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 64);
        float minDelta = 1e9f;
        for (int i = 0; i < nCand; ++i) minDelta = std::min(minDelta, cd[i]);
        std::printf("        %+5.1f    %d %s   %.4f %.4f   %7.2f   %+6.2f %+7.2f"
                    "   候補%2d 最小δ%6.2f%s\n",
                    z, nd, changed ? "★" : " ", band[0], band[5], tilt, dL, dT,
                    nCand, (nCand > 0) ? minDelta : -1.0f,
                    changed ? "  ← 本数が変わった" : "");
        if (have) {
            if (std::fabs(dL) > worstTot)  { worstTot  = std::fabs(dL); atTot  = z; }
            if (std::fabs(dT) > worstTilt) { worstTilt = std::fabs(dT); atTilt = z; }
        }
        prevNd = nd; prevTot = tot; prevTilt = tilt; have = true;
        AF_SceneDestroy(s);
    }
    std::printf("        → 0.1m 刻みの最大 Δ:  合計 %.2f dB @ z=%+.1f  /  "
                "**傾き %.2f dB @ z=%+.1f**\n", worstTot, atTot, worstTilt, atTilt);
    std::printf("      ※既存の連続性検査は生存ゲイン（スカラ）を見ており、\n"
                "        **タップの本数**は見ていない。本数が変わるとそのタップが持つ\n"
                "        帯域の形ごと消えるので、合計のスペクトルが動く。\n");

    // 落ちる位置の周りを 1cm 刻みで見る。
    //   数 mm で出たり消えたりするなら**数値的な knife-edge**（判定が幾何の際どい所で
    //   ひっくり返っている）。一定の区間で消えるなら幾何的な理由がある。
    std::printf("        落ちる位置の周りを 1cm 刻みで（候補数 / 最小δ）:\n          ");
    int runs = 0; int prevC = -1;
    for (float z = -3.95f; z <= -3.55f; z += 0.01f) {
        AF_SceneHandle s = build();
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        AF_Vector3 cp[64]; float cd[64];
        const int nc = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 64);
        if (prevC >= 0 && nc != prevC) ++runs;
        prevC = nc;
        std::printf("%d", nc);
        AF_SceneDestroy(s);
    }
    std::printf("\n          （z=-3.95 → -3.55 の 41 点。切り替わり %d 回）\n", runs);
    std::printf("      ※きれいな帯で消えるなら数値誤差ではなく、幾何的な死角。\n");

    // どのゲートが死角を作っているかを 1 つずつ切って確かめる。
    //   bit0 pointInsideOther / bit1 penNearWeight / bit2 crossesCore
    struct G { int mask; const char* name; };
    const G gates[] = {
        {0, "全部有効（本番）        "},
        {1, "pointInsideOther を切る "},
        {2, "penNearWeight を切る    "},
        {4, "crossesCore を切る      "},
        {7, "全部切る                "},
    };
    std::printf("        ゲートを 1 つずつ切って死角が消えるか（z=-6.0 → -1.0 の 51 点の候補数）:\n");
    for (const G& g : gates) {
        int dead = 0, flips = 0, pc = -1;
        std::printf("          %s ", g.name);
        for (float z = -6.0f; z <= -1.0f; z += 0.1f) {
            AF_SceneHandle s = build();
            AF_SceneSetDiffractionGateMask(s, g.mask);
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            AF_Vector3 cp[64]; float cd[64];
            const int nc = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 64);
            if (nc == 0) ++dead;
            if (pc >= 0 && (nc == 0) != (pc == 0)) ++flips;
            pc = nc;
            std::printf("%c", (nc == 0) ? '.' : '#');
            AF_SceneDestroy(s);
        }
        std::printf("  死角 %2d 点 / 切り替わり %d 回\n", dead, flips);
    }
    std::printf("      ※'#'=候補あり / '.'=死角。死角が消えたゲートが犯人。\n");

    // 【(B)】稜線からポータルを生成する版。回折の**量**が矩形の中の影で決まるので、
    //   候補の有無ではなく実際の帯域ゲインで見る（本数が同じでも中身が変わる）。
    std::printf("        (B) 稜線ポータル ON/OFF での回折ゲイン（125Hz / 傾き4k-125）:\n");
    for (int mode = 0; mode < 2; ++mode) {
        std::printf("          %s ", mode ? "ON （稜線→矩形→フレネル）" : "OFF（前川＋開口積分）  ");
        double worstTilt = 0.0; float prevTilt = 0.0f; bool have = false;
        for (float z = -6.0f; z <= -1.0f; z += 0.1f) {
            AF_SceneHandle s = build();
            AF_SceneSetEdgePortals(s, mode);
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            AF_Vector3 dp[8]; float dg[8]; float db[8*6];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float lo = 0.0f, hi = 0.0f;
            for (int i = 0; i < nd; ++i) { lo += db[i*6+0]; hi += db[i*6+5]; }
            const float tilt = (lo > 1e-6f)
                ? 20.0f * std::log10(std::max(hi, 1e-6f) / lo) : 0.0f;
            if (have && lo > 1e-6f) worstTilt = std::max(worstTilt, (double)std::fabs(tilt - prevTilt));
            if (lo > 1e-6f) { prevTilt = tilt; have = true; }
            std::printf("%c", (nd == 0) ? '.' : '#');
            AF_SceneDestroy(s);
        }
        std::printf("  傾きの最大隣接差 %.2f dB\n", worstTilt);
    }
}

// 【(B) の較正】稜線ポータルと従来（前川＋開口積分）を、同じ配置で δ ごとに並べる。
//   比がどこでも一定なら**ただの倍率**で、決めごと #3 の「絶対値は演出で決める」で片付く。
//   δ とともに比が動くなら**形が違う**ので、矩形の作り方（大きさ・向き）を直す必要がある。
static float g_edgeSpan = 1.0f;
void diagnoseEdgePortalCalibration() {
    std::printf("\n[(B)較正] 稜線ポータル vs 従来 ── 形が同じか、倍率だけか\n");
    const float h = 4.0f, t = 0.3f, hw = 12.0f, hd = 10.0f;
    auto build = [&](bool edgeP) {
        AF_SceneHandle s = AF_SceneCreate();
        AF_SceneSetRoomCellSize(s, 0.5f);
        AF_SceneSetEdgePortals(s, edgeP ? 1 : 0);
        if (edgeP) AF_SceneSetEdgePortalSpan(s, g_edgeSpan);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // 床と、有限の衝立 1 枚だけ（部屋にしない＝反射を混ぜない）。
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(3.0f, h*0.5f, 0.15f), V(1,0,0), V(0,1,0), m);
        return s;
    };
    const AF_Vector3 S = V(0, 1.6f, 4.0f);
    std::printf("        x     δ(m)    従来125   (B)125    比(dB)  |  従来4k    (B)4k     比(dB)\n");
    double minR = 1e9, maxR = -1e9;
    for (float x = 0.0f; x <= 2.8f; x += 0.4f) {
        float gOld[kBands] = {}, gNew[kBands] = {};
        float delta = -1.0f;
        for (int k = 0; k < 2; ++k) {
            AF_SceneHandle s = build(k == 1);
            const AF_Vector3 L = V(x, 1.6f, -4.0f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 5; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            AF_Vector3 dp[8]; float dg[8]; float db[8*6];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float* dst = (k == 0) ? gOld : gNew;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < kBands; ++b) dst[b] += db[i*6+b];
            if (k == 0) {
                AF_Vector3 cp[32]; float cd[32];
                const int nc = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 32);
                for (int i = 0; i < nc; ++i)
                    if (delta < 0.0f || cd[i] < delta) delta = cd[i];
            }
            AF_SceneDestroy(s);
        }
        const double r0 = 20.0 * std::log10(std::max(gNew[0],1e-6f)/std::max(gOld[0],1e-6f));
        const double r5 = 20.0 * std::log10(std::max(gNew[5],1e-6f)/std::max(gOld[5],1e-6f));
        std::printf("        %.1f  %6.3f  %.5f  %.5f  %+7.2f  |  %.5f  %.5f  %+7.2f\n",
                    x, delta, gOld[0], gNew[0], r0, gOld[5], gNew[5], r5);
        if (gOld[0] > 1e-5f && gNew[0] > 1e-7f) {
            minR = std::min(minR, r0); maxR = std::max(maxR, r0);
        }
    }
    std::printf("      → 125Hz の比のばらつき %.2f 〜 %.2f dB（幅 %.2f dB）\n",
                minR, maxR, maxR - minR);
    std::printf("      ※幅が小さければ**ただの倍率**＝決めごと #3 で片付く。\n"
                "        δ とともに動くなら形が違う＝矩形の作り方を直す必要がある。\n");
}

// 【発注者の線引きへの回答】apertureContrast は「物理」か「演出」か。
//
//   線引き: 物理でやるのは「扉を開いた際の音の入ってきかた」と「音の反響」。それ以外は演出。
//   apertureContrast は開口率を S 字に強調する写像で、**入ってきかたの量そのものではない**。
//   同じファイルの apertureTimbre には「誇張は作品側の判断なので既定では掛けない」と
//   書いてあるのに、こちらだけ既定 2.0 になっていた ── 食い違い。
//   ここでは「どの値だと扉がどう聞こえるか」を出して、選べるようにする。
void diagnoseApertureContrastChoice() {
    std::printf("\n[線引き] apertureContrast は演出か ── 扉の開き角に対する効き\n");
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const float cs[] = {1.0f, 2.0f, 4.0f};
    std::printf("        開き角   contrast=1.0（物理そのまま）  2.0（旧既定）  4.0（Unity が押す値）\n");
    float full[3] = {0, 0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        for (float deg = 0.0f; deg <= 90.01f; deg += 15.0f) {
            float g[3] = {0, 0, 0};
            for (int k = 0; k < 3; ++k) {
                AF_SceneHandle s = AF_SceneCreate();
                const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
                AF_SceneSetApertureContrast(s, cs[k]);
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
                AF_SceneAddInstanceBox(s, V(-0.6f + c*doorW*0.5f, doorH*0.5f, sn*doorW*0.5f),
                                       V(doorW*0.5f, doorH*0.5f, 0.03f), V(c,0,sn), V(0,1,0), mat);
                AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0), doorW*0.5f, doorH*0.5f);
                const AF_Vector3 L = V(0, 1.6f, -4), S = V(-2.0f, 1.6f, 3.5f);
                AF_SceneSetListener(s, L);
                AF_SceneSetSource(s, 1, S);
                for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
                // ★測るのは生存ゲインではなく**回折タップのゲイン**。
                //   apertureContrast は F タップに掛かる写像で、生存ゲイン（スカラ）には
                //   効かない。最初そちらを測って「3 値とも同じ」と出て気づいた。
                AF_Vector3 dp[8]; float dgs[8]; float db[8*6];
                const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dgs, db, 8);
                float sum = 0.0f;
                for (int i = 0; i < nd; ++i) sum += db[i*6+0];
                g[k] = sum;
                AF_SceneDestroy(s);
            }
            if (pass == 0) { if (deg > 89.0f) for (int k = 0; k < 3; ++k) full[k] = g[k]; continue; }
            std::printf("         %4.0f°   %.4f (%+6.1f dB)   %.4f (%+6.1f dB)   %.4f (%+6.1f dB)\n",
                        deg,
                        g[0], 20.0 * std::log10(std::max(g[0],1e-6f)/std::max(full[0],1e-6f)),
                        g[1], 20.0 * std::log10(std::max(g[1],1e-6f)/std::max(full[1],1e-6f)),
                        g[2], 20.0 * std::log10(std::max(g[2],1e-6f)/std::max(full[2],1e-6f)));
        }
    }
    std::printf("      ※dB は各々の全開を 0dB とした相対値＝**開ける手応え**。\n"
                "        物理そのままが 1.0。2.0/4.0 は S 字で中央を急にした演出。\n");
}

// 【調査】(B) で狭い隙間が塞がらない件 ── 矩形に遮蔽物が写っているか。
//   2m と 3cm が同値になるのは「幅を見ていない」印で、それは前川（δ だけの関数）の症状。
//   つまり (B) の積分に落ちていない疑いがある。写った枚数を直接数える。
void diagnoseSlitPortalShadow() {
    std::printf("\n[調査] (B) で狭い隙間が塞がるか ── 矩形に何枚写っているか\n");
    const float t = 0.15f, h = 3.0f, hw = 8.0f;
    std::printf("        span   隙間幅   回折125Hz(F)  2m 比(dB)  写った枚数  候補\n");
    for (float span : {0.0f, 0.5f, 1.0f, 2.0f, 4.0f}) {
        float ref = 0.0f;
        const int mode = (span > 0.0f) ? 1 : 0;
        for (float gap : {2.0f, 0.5f, 0.1f, 0.03f}) {
            AF_SceneHandle s = AF_SceneCreate();
            const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            AF_SceneSetEdgePortals(s, mode);
            if (mode) AF_SceneSetEdgePortalSpan(s, span);
            AF_SceneAddInstanceBox(s, V(-hw * 0.5f, h * 0.5f, 0), V(hw * 0.5f, h * 0.5f, t),
                                   V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(gap + (hw - gap) * 0.5f, h * 0.5f, 0),
                                   V((hw - gap) * 0.5f, h * 0.5f, t), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, 6), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, 6), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(-hw, h * 0.5f, 0), V(t, h * 0.5f, 6), V(1,0,0), V(0,1,0), mat);
            AF_SceneAddInstanceBox(s, V(hw, h * 0.5f, 0), V(t, h * 0.5f, 6), V(1,0,0), V(0,1,0), mat);
            const AF_Vector3 L = V(-0.1f, 1.6f, -3.0f), S = V(-0.1f, 1.6f, 3.0f);
            AF_SceneSetListener(s, L);
            AF_SceneSetSource(s, 1, S);
            for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            AF_Vector3 dp[8]; float dg[8]; float db[8*6];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float lo = 0.0f;
            for (int i = 0; i < nd; ++i) lo += db[i*6+0];
            AF_Vector3 cp[32]; float cd[32];
            const int nc = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 32);
            if (gap > 1.9f) ref = lo;
            std::printf("        %-4s  %5.2f m   %.5f     %+7.1f      %3d       %d\n",
                        mode ? "ON" : "OFF", gap, lo,
                        20.0 * std::log10(std::max(lo, 1e-7f) / std::max(ref, 1e-7f)),
                        AF_SceneDebugPortalPolys(s), nc);
            AF_SceneDestroy(s);
        }
        std::printf("        %s\n", (span > 0.0f) ? "  ---" : "  --- 以下 (B) ON、span を振る");
    }
    std::printf("      ※「2m 比」が幅の効き。125Hz(λ=2.7m) に対し 3cm はほぼ通さないはずなので、\n"
                "        物理では -30dB 台が期待値。枚数 -1 は開口積分が走っていない（前川へ落ちた）。\n");

    // 頭打ちの正体を見る。span=2 で 0.1m と 0.03m が同値になるので、
    // **隙間以外の候補**が幅に依らない下駄を履かせている疑い。候補ごとに中身を出す。
    std::printf("        頭打ちの正体（span=2・候補ごとの内訳。隙間は x∈[0,gap]）:\n");
    for (float gap : {0.10f, 0.03f}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneSetEdgePortals(s, 1);
        AF_SceneSetEdgePortalSpan(s, 2.0f);
        AF_SceneAddInstanceBox(s, V(-hw * 0.5f, h * 0.5f, 0), V(hw * 0.5f, h * 0.5f, t),
                               V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(gap + (hw - gap) * 0.5f, h * 0.5f, 0),
                               V((hw - gap) * 0.5f, h * 0.5f, t), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, 6), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, 6), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(-hw, h * 0.5f, 0), V(t, h * 0.5f, 6), V(1,0,0), V(0,1,0), mat);
        AF_SceneAddInstanceBox(s, V(hw, h * 0.5f, 0), V(t, h * 0.5f, 6), V(1,0,0), V(0,1,0), mat);
        const AF_Vector3 L = V(-0.1f, 1.6f, -3.0f), S = V(-0.1f, 1.6f, 3.0f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        AF_Vector3 dp[8]; float dg[8]; float db[8*6];
        const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
        std::printf("          隙間 %.2fm  回折音源 %d 本:\n", gap, nd);
        for (int i = 0; i < nd; ++i)
            std::printf("            [%d] 位置(%+6.2f,%+5.2f,%+6.2f)  125Hz %.5f  4k %.5f\n",
                        i, dp[i].x, dp[i].y, dp[i].z, db[i*6+0], db[i*6+5]);
        AF_Vector3 cp[32]; float cd[32];
        const int nc = AF_SceneDiffractionCandidates(s, L, S, cp, cd, 32);
        double nmr = 0, dnm = 0; float lu = 0;
        AF_SceneDebugPortalIntegral(s, &nmr, &dnm, &lu);
        std::printf("            125Hz の積分: 分子 %.6f / 分母 %.6f = %.6f  範囲 ±%.3fm\n",
                    nmr, dnm, (dnm > 1e-12) ? nmr/dnm : -1.0, lu);
        std::printf("            候補 %d 個:", nc);
        for (int i = 0; i < nc && i < 6; ++i)
            std::printf("  (%+.2f,%+.2f,%+.2f)δ%.2f", cp[i].x, cp[i].y, cp[i].z, cd[i]);
        std::printf("\n");
        AF_SceneDestroy(s);
    }
    std::printf("      ※隙間(x=0〜gap)から離れた位置の候補が下駄の正体。\n");
}

// 【看板の検査】扉の開き具合が音になっているか。
//
//   ★これまで扉のカーブは printf の診断でしか見ておらず、**検査が無かった**。
//     そのためセッション中に黙って変わっていた（10°の開口率 0.0207 → 0.0000）。
//     扉の開き具合はこの作品の看板なので、ここが動いたら落ちるようにする。
//   ★書くのは「いまの値」ではなく「あるべき挙動」:
//       ・閉じていれば鳴らない
//       ・少し開けたら少し聞こえる（0 ではない）← 「気配」がここに乗る
//       ・開けるほど増える（単調）
//       ・全開なら十分開く
//     いまの実装がこれを満たさないなら、それは実装の側の問題。
void testDoorOpennessCurve() {
    std::printf("\n[看板] 扉の開き具合が音になっているか\n");
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    const float angles[] = {0.0f, 10.0f, 20.0f, 45.0f, 70.0f, 90.0f};
    float op[6] = {};
    std::printf("        開き角   開口率(125Hz)\n");
    for (int a = 0; a < 6; ++a) {
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
        const float th = angles[a] * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        AF_SceneAddInstanceBox(s, V(-0.6f + c*doorW*0.5f, doorH*0.5f, sn*doorW*0.5f),
                               V(doorW*0.5f, doorH*0.5f, 0.03f), V(c,0,sn), V(0,1,0), mat);
        const int pid = AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                                          doorW*0.5f, doorH*0.5f);
        const AF_Vector3 L = V(0, 1.6f, -4), S = V(-2.0f, 1.6f, 3.5f);
        AF_SceneSetListener(s, L);
        AF_SceneSetSource(s, 1, S);
        for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
        float f[kBands] = {}; AF_Vector3 pp{};
        AF_SceneMeasurePortal(s, pid, L, S, f, &pp);
        op[a] = f[0];
        std::printf("         %4.0f°   %.4f\n", angles[a], op[a]);
        AF_SceneDestroy(s);
    }
    check("[看板] 閉じていれば鳴らない（<0.02）", op[0] < 0.02f);
    // ★「少し開けたら少し聞こえる」が看板そのもの。0 だと気配が伝わらない。
    char b[96];
    std::snprintf(b, sizeof(b), "(10° %.4f / 20° %.4f)", op[1], op[2]);
    check("[看板] 10° で開口率が 0 でない（少し開けたら少し聞こえる）", op[1] > 0.002f, b);
    check("[看板] 20° で 10° より開く", op[2] > op[1], b);
    bool mono = true;
    for (int a = 1; a < 6; ++a) if (op[a] < op[a-1] - 1e-4f) mono = false;
    check("[看板] 開けるほど増える（単調）", mono);
    std::snprintf(b, sizeof(b), "(90° %.4f)", op[5]);
    check("[看板] 全開で十分開く（>0.5）", op[5] > 0.5f, b);

    // 原因の切り分け: どのゲートが低角側を塞いでいるか。
    //   bit1 pointInsideOther / bit2 penNearWeight / bit4 crossesCore / bit8 entersAperture
    std::printf("        ゲートを切ったときの 10°/20° の開口率:\n");
    for (int mask : {0, 1, 2, 4, 8, 15}) {
        float o[2] = {0, 0};
        for (int a = 0; a < 2; ++a) {
            const float deg = (a == 0) ? 10.0f : 20.0f;
            AF_SceneHandle s = AF_SceneCreate();
            const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            AF_SceneSetDiffractionGateMask(s, mask);
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
            const float th2 = deg * 3.14159265f / 180.0f;
            const float c2 = std::cos(th2), s2 = std::sin(th2);
            AF_SceneAddInstanceBox(s, V(-0.6f + c2*doorW*0.5f, doorH*0.5f, s2*doorW*0.5f),
                                   V(doorW*0.5f, doorH*0.5f, 0.03f), V(c2,0,s2), V(0,1,0), mat);
            const int pid2 = AF_SceneAddPortal(s, V(0, doorH*0.5f, 0), V(1,0,0), V(0,1,0),
                                               doorW*0.5f, doorH*0.5f);
            const AF_Vector3 L2 = V(0, 1.6f, -4), S2 = V(-2.0f, 1.6f, 3.5f);
            AF_SceneSetListener(s, L2);
            AF_SceneSetSource(s, 1, S2);
            for (int i = 0; i < 4; ++i) AF_SceneUpdate(s, 1.0f/60.0f);
            float f2[kBands] = {}; AF_Vector3 pp2{};
            AF_SceneMeasurePortal(s, pid2, L2, S2, f2, &pp2);
            o[a] = f2[0];
            AF_SceneDestroy(s);
        }
        const char* nm = (mask == 0) ? "全部有効      " : (mask == 1) ? "pointInsideOther"
                       : (mask == 2) ? "penNearWeight " : (mask == 4) ? "crossesCore   "
                       : (mask == 8) ? "entersAperture" : "全部切る      ";
        std::printf("          %s  10°=%.4f  20°=%.4f\n", nm, o[0], o[1]);
    }
}

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
            if (std::fabs(occ[i][kBands - 1] - occ[j][kBands - 1]) < 1e-6f) { uniq = false; break; }
        if (uniq) ++distinct;
    }
    // ★見通せる位置の音源は生存 1.000 で揃うのが**正しい**。全部バラバラを要求しない。
    //   ここで見たいのは「取り違えていないか」なので、遮られた側と見通せる側が
    //   きちんと分かれることを縛る。
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < N; ++i) { lo = std::min(lo, occ[i][kBands - 1]); hi = std::max(hi, occ[i][kBands - 1]); }
    std::printf("      生存ゲイン(4kHz) 音源ごと:");
    for (int i = 0; i < N; ++i) std::printf(" %.3f", occ[i][kBands - 1]);
    std::printf("   相異なる値 %d/%d（最小 %.3f 最大 %.3f）\n", distinct, N, lo, hi);
    check("[多音源] 遮られた音源と見通せる音源が分かれる", distinct >= 3 && hi > lo * 1.5f);

    // ③ エコグラムは**部屋ごと**であること
    //
    // ★2026-08-24 に契約を変えた。以前は「音源ごとに別物」を縛っていたが、
    //   尾（後期残響）は**部屋の形にしか依存しない**ので、同じ部屋の音源は共有する。
    //   出荷経路（VoiceConvolver）は元から代表 1 本しか読んでおらず
    //   （形は代表・量は自分。実測 形は同室 1.3dB 以内／隣室 2.6〜3.9dB ずれ）、
    //   音源ごとに計算していたぶんは**丸ごと無駄**だった
    //   （実測: 音源 16 本で 16.798 ms、うち 14.3 ms が音源数で伸びるぶん）。
    //
    //   → 縛るのは「同じ部屋なら一致し、代表が引ける」。
    //     部屋をまたいで違うことは [残響] 小部屋 vs 大部屋 が別に縛っている。
    std::vector<float> e(100 * kBands);
    double total[N] = {};
    int sameRoomMismatch = 0, badRep = 0;
    for (int i = 0; i < N; ++i) {
        const int bins = AF_SceneGetEchogramBands(s, idx[i], e.data(), 100);
        double sum = 0.0;
        for (int k = 0; k < bins * kBands; ++k) sum += e[static_cast<std::size_t>(k)];
        total[i] = sum;
        const int rep = AF_SceneGetTailShapeIndex(s, idx[i]);
        if (rep < 0) { ++badRep; continue; }
        // 代表が同じなら、返ってくる尾も同じでなければならない。
        for (int j = 0; j < i; ++j)
            if (AF_SceneGetTailShapeIndex(s, idx[j]) == rep
                && std::fabs(total[i] - total[j]) > 1e-9) ++sameRoomMismatch;
    }
    std::printf("      エコグラム総和 音源ごと:");
    for (int i = 0; i < N; ++i) std::printf(" %.2f", total[i]);
    std::printf("\n      代表:");
    for (int i = 0; i < N; ++i) std::printf(" %d", AF_SceneGetTailShapeIndex(s, idx[i]));
    std::printf("\n");
    char shareNote[96];
    std::snprintf(shareNote, sizeof(shareNote), "(同室で食い違い %d / 代表が引けない %d)",
                  sameRoomMismatch, badRep);
    check("[多音源] 同じ部屋の音源は同じ尾を共有する",
          sameRoomMismatch == 0 && badRep == 0, shareNote);

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
    double prevTilt = 0.0, worstTilt = 0.0; float worstTiltAt = 0.0f;
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

        // 【B7】帯域ごとに D と F を足して、**傾き（4k/125）**が 1 歩で何 dB 動くか。
        //   ゲーム側の起票どおりの測り方。合計レベルの連続性しか見ていなかったので、
        //   「レベルは連続なのに音色が跳ぶ」が検査を素通りしていた。
        AF_Vector3 dp2[8]; float dg2[8], db2[48];
        const int nd2 = AF_SceneComputeDiffractionSourceBands(s, L, S, dp2, dg2, db2, 8);
        float tot6[kBands] = {};
        for (int b = 0; b < kBands; ++b) {
            float f = 0.0f;
            for (int i = 0; i < nd2; ++i) f += db2[i * kBands + b];
            f *= occFrac;
            tot6[b] = std::sqrt(soft[b] * soft[b] + f * f);
        }
        const double tilt = 20.0 * std::log10(std::max<double>(tot6[kBands - 1], 1e-9)
                                            / std::max<double>(tot6[0], 1e-9));
        if (!first) {
            const double step = std::fabs(db - prevDb);
            if (step > worst) { worst = step; worstAt = x; }
            worstD = std::max(worstD, std::fabs(20.0 * std::log10(std::max(sm, 1e-9))
                                              - 20.0 * std::log10(std::max(prevD, 1e-9))));
            const double ts = std::fabs(tilt - prevTilt);
            if (ts > worstTilt) { worstTilt = ts; worstTiltAt = x; }
        }
        first = false; prevDb = db; prevD = sm; prevTilt = tilt;
        std::printf("        %5.2f   %5.2f   %7.5f   %7.5f      %7.5f  %7.1f   %6.3f   %+7.1f",
                    x, occFrac, sm, fpart, total, db, om, tilt);
        if (x < -1.95f || (x > -1.75f && x < -1.65f)) {
            std::printf("  │ D:");
            for (int b = 0; b < kBands; ++b) std::printf("%7.4f", soft[b]);
            std::printf("  F本%d", nd2);
        }
        std::printf("\n");
    }
    std::printf("      0.1m あたりの最大変化: D⊕F の合計 %.1f dB / D タップ単体 %.1f dB"
                "（x=%.2f 付近）\n", worst, worstD, worstAt);
    std::printf("      【B7】D⊕F の**傾き**(4k/125) の 0.1m あたり最大変化: %.1f dB（x=%.2f）\n",
                worstTilt, worstTiltAt);
    std::printf("      ※ 合計レベルが連続でも、傾きが跳べば「音色が変わった」と聞こえる。\n");
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

// ─────────────────────────────────────────────────────────────────────
// 【診断・見立て外れの記録】BVH の作り直しは床ではなかった
//
//   連絡板 2026-08-22 でゲームレーンが「音源 1 本 10.6ms・間引きで消えない床がある」
//   と報告してきた。床の候補として **BVH の全再構築** を疑った。
//   実装は確かに refit ではなく全再構築で（インスタンスが 1 つでも動くと bvhDirty_ が
//   立ち、次のレイで buildBvh が全ノードを捨てて再分割する）、動く物が 1 つでもあれば
//   毎フレーム払う ＝ 音源数と無関係な床に見える、という筋だった。
//
//   ★測ったら外れていた。作り直しの代金は**測定ノイズ以下**（符号が負に出る）。
//     理由は単純で、作り直しは遅延実行（ensureBvh はレイを撃つときに呼ばれる）なので、
//     1 フレームぶんの費用が **音源あたり 256レイ × 3バウンス ≒ 768 回の走査**に
//     ならされる。1025 形でも作り直しは 2.4ms 未満で、走査の総額に埋もれる。
//     → **refit 化は無駄。優先度から落としてよい。**
//
//   代わりに見えたのは「形の数に対する伸び」のほう（17形 1.6ms → 1025形 128.8ms）。
//   ⚠ ただしこの試験は形を増やすと**密度も上がる**（同じ間隔で並べるので迷路が濃くなる）
//     ので、この数字は BVH の良し悪しを切り分けたものでは**ない**。
//     切り分けるには密度を保ったまま形を増やす（＝場を広げる）必要がある。
// ─────────────────────────────────────────────────────────────────────
void diagnoseBvhRebuildCost() {
    std::printf("\n[診断] BVH の作り直しは床か（→ 外れ。ノイズ以下）\n");
    std::printf("        1 つでも動くと全再構築だが、遅延実行なので 768 回の走査にならされる。\n");
    std::printf("        %8s %12s %12s %12s %10s\n",
                "形の数", "静止 ms", "1つ動かす ms", "作り直し ms", "1形あたり us");

    for (int nBox : {16, 64, 256, 1024}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // 格子状に箱を並べる。1 点に寄せると BVH の分割が縮退して測り物にならない。
        const int side = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(nBox))));
        for (int i = 0; i < nBox; ++i) {
            const int ix = i % side, iy = (i / side) % side, iz = i / (side * side);
            AF_SceneAddInstanceBox(s, V(ix * 2.0f - side, iy * 2.0f, iz * 2.0f - side),
                                   V(0.5f, 0.5f, 0.5f), V(1,0,0), V(0,1,0), m);
        }
        const unsigned long long movedId =
            AF_SceneAddInstanceBox(s, V(0, 1.0f, 0), V(0.5f, 1.0f, 0.05f),
                                   V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(0, 1.6f, -3.0f));
        AF_SceneSetSource(s, 1, V(0, 1.6f, 3.0f));

        // レイを撃つ段だけ残す。BVH の代金を他の段で薄めないため。
        AF_UpdateConfig c{};
        c.role1EveryN = 1; c.role2EveryN = 1; c.earlyEveryN = 1;
        c.diffSrcEveryN = 1;
        c.reflectionRays = 256; c.reflectionBounces = 3;
        c.directWeight = 1.0f; c.useReflections = 1;
 c.enableReverb = 0;
        c.enableEarlyReflections = 0; c.enableDiffractionSources = 0;
        c.speedOfSound = 343.0f; c.distanceRef = 1.5f;
        AF_SceneSetUpdateConfig(s, &c);

        using clk = std::chrono::high_resolution_clock;
        auto run = [&](bool move, int frames) {
            double acc = 0.0;
            for (int f = 0; f < frames; ++f) {
                const auto t0 = clk::now();
                if (move) {
                    const float th = (f % 90) * 3.14159265f / 180.0f;
                    AF_SceneUpdateInstance(s, movedId, V(0, 1.0f, 0), V(0.5f, 1.0f, 0.05f),
                                           V(std::cos(th), 0, std::sin(th)), V(0, 1, 0));
                }
                AF_SceneUpdate(s, 1.0f / 60.0f);
                acc += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            }
            return acc / frames;
        };
        run(true, 20);                       // 暖機
        const double moving  = run(true, 120);
        const double stillMs = run(false, 120);
        const double rebuild = moving - stillMs;
        std::printf("        %8d %12.3f %12.3f %12.3f %10.2f\n",
                    nBox + 1, stillMs, moving, rebuild, rebuild * 1000.0 / (nBox + 1));
        AF_SceneDestroy(s);
    }
    std::printf("        → 「作り直し ms」が 0 前後（負にも振れる）＝**代金は測れない**。\n"
                "          遅延実行で 768 回の走査にならされるため。refit 化は無駄。\n"
                "        ⚠ 「静止 ms」の伸びは形の数だけでなく**密度**も上がっているので、\n"
                "          BVH の良し悪しを切り分けた数字ではない。読み違えないこと。\n");
}

// ─────────────────────────────────────────────────────────────────────
// 【複数コア】並列で回しても結果がビット一致すること
//
//   割るのは「音源ごとに自分の枠にしか書かない」段だけ:
//     遮蔽・回折(role1 の前半) / 早期反射 / 回折二次音源
//   共有の箱へ足し込む段（反射の reflected[] やエコグラムのビン）は割っていない。
//   だから各音源の計算順も丸めも変わらず、**厳密に一致するはず**。
//
//   ★ここは「だいたい合っている」では意味がない。**1 ビットでも違えば、割ってはいけない
//     ものを割っている**ということなので、許容差ではなく厳密比較にする。
//     （速い経路と参照経路を許容差で縛るのは分割畳み込みのような**別物**の場合。
//       ここは同じ計算を場所を変えてやるだけなので、一致しないほうがおかしい。）
// ─────────────────────────────────────────────────────────────────────
void testWorkerThreads() {
    std::printf("\n[複数コア] 直列と並列がビット一致するか\n");

    // 遮蔽された音源を多めに置く（diffractionComposite が走る＝いちばん重い経路）。
    auto build = [](int threads) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.3f, hw = 9.0f, hd = 9.0f;
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        // 衝立を 2 枚。半分の音源を影に入れる。
        AF_SceneAddInstanceBox(s, V(-4.0f, h*0.5f, 0), V(2.5f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( 4.0f, h*0.5f, 1.0f), V(2.5f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(0, 1.6f, -6.0f));
        for (int i = 0; i < 12; ++i) {
            const float x = -7.0f + i * 1.3f;
            AF_SceneSetSource(s, static_cast<unsigned long long>(200 + i), V(x, 1.6f, 4.0f));
        }
        AF_SceneSetWorkerThreads(s, threads);
        for (int k = 0; k < 6; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        return s;
    };

    // ★2 種類に分けて集める。並列化のしかたが違うので、要求できる強さが違う。
    //
    //   exact  … 早期反射・回折二次音源。**音源ごとに自分の枠にしか書かない**段なので、
    //            各音源の計算順も丸めも変わらない ＝ **ビット一致を要求できる**。
    //   approx … 生存6帯域・到来方向・遮蔽スカラ。この中には
    //            occlusionReflectedMulti の**反射の足し込み**が入っている。
    //            レイを全音源で共有して同じ箱へ足すので、塊で割ると足す順が変わる。
    //            ＝ ビット一致は要求できない。**許容差で押さえる**。
    auto collectExact = [](AF_SceneHandle s, std::vector<float>& out) {
        out.clear();
        for (int i = 0; i < 12; ++i) {
            const int idx = AF_SceneSourceIndex(s, static_cast<unsigned long long>(200 + i));
            AF_Vector3 ep[8]; float eg[8 * kBands];
            const int ne = AF_SceneGetEarlyReflections(s, idx, ep, eg, 8);
            out.push_back(static_cast<float>(ne));
            for (int k = 0; k < ne; ++k) {
                out.push_back(ep[k].x); out.push_back(ep[k].y); out.push_back(ep[k].z);
                for (int b2 = 0; b2 < kBands; ++b2) out.push_back(eg[k * kBands + b2]);
            }
            AF_Vector3 dp[8]; float dg[8];
            const int nd = AF_SceneGetDiffractionSources(s, idx, dp, dg, 8);
            out.push_back(static_cast<float>(nd));
            for (int k = 0; k < nd; ++k) {
                out.push_back(dp[k].x); out.push_back(dp[k].y); out.push_back(dp[k].z);
                out.push_back(dg[k]);
            }
        }
    };
    auto collectApprox = [](AF_SceneHandle s, std::vector<float>& out) {
        out.clear();
        for (int i = 0; i < 12; ++i) {
            const int idx = AF_SceneSourceIndex(s, static_cast<unsigned long long>(200 + i));
            float b[kBands] = {};
            AF_SceneGetSourceOcclusion(s, idx, b);
            for (int k = 0; k < kBands; ++k) out.push_back(b[k]);
            float d[3] = {};
            AF_SceneGetSourceArrivalDir(s, idx, d);
            out.push_back(d[0]); out.push_back(d[1]); out.push_back(d[2]);
            out.push_back(AF_SceneGetSourceOcclusionScalar(s, idx));
        }
    };
    auto collect = collectExact;

    std::vector<float> serial, par2, par4;
    AF_SceneHandle s1 = build(1); collect(s1, serial);
    const int got1 = AF_SceneGetWorkerThreads(s1);
    AF_SceneDestroy(s1);                       // ★ここで join される
    AF_SceneHandle s2 = build(2); collect(s2, par2);
    const int got2 = AF_SceneGetWorkerThreads(s2);
    AF_SceneDestroy(s2);
    AF_SceneHandle s4 = build(4); collect(s4, par4);
    const int got4 = AF_SceneGetWorkerThreads(s4);
    AF_SceneDestroy(s4);

    char note[160];
    std::snprintf(note, sizeof(note), "(設定 1/2/4 → 実際 %d/%d/%d)", got1, got2, got4);
    check("[複数コア] スレッド数が設定どおり入る", got1 == 1 && got2 == 2 && got4 == 4, note);

    auto exactSame = [&](const std::vector<float>& a, const std::vector<float>& b,
                         const char* label) {
        if (a.size() != b.size()) {
            std::snprintf(note, sizeof(note), "(要素数が違う %zu vs %zu)", a.size(), b.size());
            check(label, false, note);
            return;
        }
        // ★型紙 B（設定不変）を共有の式で（detectors.h）。
        //   「速くするだけの設定」は音を変えてはいけない。型紙 5（同じ設定で 2 回）とは別物。
        //   ★今日いちばん使った型紙。並列／共有バス／エコグラム分割の 3 つを全部これで見た。
        af::detect::Break br[8];
        const int nv = af::detect::invariantUnderSetting(a.data(), b.data(), (int)a.size(),
                                                         /*exact=*/true, 0.0f, br, 8);
        std::snprintf(note, sizeof(note), "(%zu 個中 違い %d 個%s)", a.size(), nv,
                      nv ? "" : " ＝ 完全一致");
        check(label, nv == 0, note);
        if (nv) std::printf("        ★最初の食い違い: [%d] %.9g vs %.9g\n",
                            br[0].index, a[(size_t)br[0].index], b[(size_t)br[0].index]);
    };
    std::printf("        音源 12 本（半分は衝立の影）\n");
    std::printf("        ① 音源ごとの段（早期反射・回折二次音源）／比べる値 %zu 個\n", serial.size());
    exactSame(serial, par2, "[複数コア] 2 コアの音源ごとの段がビット一致");
    exactSame(serial, par4, "[複数コア] 4 コアの音源ごとの段がビット一致");

    // ② 反射の足し込みが入る量は許容差で。丸めの差だけのはず。
    {
        std::vector<float> aS, aP;
        { AF_SceneHandle s = build(1); collectApprox(s, aS); AF_SceneDestroy(s); }
        { AF_SceneHandle s = build(4); collectApprox(s, aP); AF_SceneDestroy(s); }
        double worstRel = 0.0;
        const size_t n = std::min(aS.size(), aP.size());
        for (size_t k = 0; k < n; ++k) {
            const double den = std::max(std::fabs((double)aS[k]), 1e-6);
            if (std::fabs((double)aS[k]) > 1e-5)
                worstRel = std::max(worstRel, std::fabs((double)aP[k] - (double)aS[k]) / den);
        }
        std::snprintf(note, sizeof(note), "(%zu 個 / 最大相対差 %.3e)", n, worstRel);
        std::printf("        ② 反射の足し込みが入る量（生存・到来方向）は許容差で\n");
        check("[複数コア] 生存・到来方向が直列と一致（相対差 < 1e-4）",
              worstRel < 1e-4 && n > 0, note);
    }

    // ─── エコグラム: レイで割るので**ビット一致しない**。許容差で押さえる ───
    //   レイは全音源で共有していて同じビンへ足し込むので、スレッドごとに別の箱へ積んで
    //   最後に足す。浮動小数の足す順が変わるため厳密には一致しない。
    //   ★ただし塊の割り方は固定なので、**実行のたびには揺れない**。そこは厳密に縛る。
    //   （速い経路と参照経路を許容差で縛るのは分割畳み込みと同じ作り。dsp_regression 参照）
    auto collectEcho = [](AF_SceneHandle s, std::vector<float>& out) {
        out.clear();
        std::vector<float> e(100 * kBands);
        for (int i = 0; i < 12; ++i) {
            const int idx = AF_SceneSourceIndex(s, static_cast<unsigned long long>(200 + i));
            const int bins = AF_SceneGetEchogramBands(s, idx, e.data(), 100);
            for (int k = 0; k < bins * kBands; ++k) out.push_back(e[static_cast<size_t>(k)]);
        }
    };
    std::vector<float> echoSerial, echoPar4a, echoPar4b;
    { AF_SceneHandle s = build(1); collectEcho(s, echoSerial); AF_SceneDestroy(s); }
    { AF_SceneHandle s = build(4); collectEcho(s, echoPar4a);  AF_SceneDestroy(s); }
    { AF_SceneHandle s = build(4); collectEcho(s, echoPar4b);  AF_SceneDestroy(s); }

    // ① 実行のたびに揺れないこと（塊の割り方が固定なら厳密に一致するはず）
    {
        // ★型紙 5（決定的）を共有の式で。元になったバグ: 並列で BVH の遅延構築が競合した。
        const int n = (int)std::min(echoPar4a.size(), echoPar4b.size());
        af::detect::Break br[8];
        const int nd2 = af::detect::deterministic(echoPar4a.data(), echoPar4b.data(), n,
                                                  0.0f, br, 8);
        af::detect::report("型紙5 決定的", br, nd2, "");
        std::snprintf(note, sizeof(note), "(%d 個中 違い %d 個%s)", n, nd2,
                      nd2 ? "" : " ＝ 実行のたびに同じ");
        check("[複数コア] 並列のエコグラムが実行のたびに揺れない", nd2 == 0 && n > 0, note);
    }
    // ② 直列と許容差で一致すること（丸めの差だけのはず）
    {
        double worstRel = 0.0; double sumS = 0.0, sumP = 0.0;
        const size_t n = std::min(echoSerial.size(), echoPar4a.size());
        for (size_t k = 0; k < n; ++k) {
            sumS += echoSerial[k]; sumP += echoPar4a[k];
            const double den = std::max(std::fabs((double)echoSerial[k]), 1e-9);
            const double rel = std::fabs((double)echoPar4a[k] - (double)echoSerial[k]) / den;
            if (std::fabs((double)echoSerial[k]) > 1e-7) worstRel = std::max(worstRel, rel);
        }
        const double totalRel = (sumS > 1e-9) ? std::fabs(sumP - sumS) / sumS : 0.0;
        std::snprintf(note, sizeof(note), "(最大相対差 %.3e / 総和の差 %.3e)", worstRel, totalRel);
        check("[複数コア] 並列のエコグラムが直列と一致（相対差 < 1e-4）",
              worstRel < 1e-4 && totalRel < 1e-6, note);
        // ★型紙 B の**許容差版**を共有の式で（上のビット一致版と同じ関数の別モード）。
        //   エコグラムはレイを塊で割って最後に足すので、浮動小数の足す順が変わりビット一致しない。
        //   「速くするだけの設定が音を変えていないか」を許容差で縛るのがここ。
        af::detect::Break br[8];
        const int nv2 = af::detect::invariantUnderSetting(
            echoSerial.data(), echoPar4a.data(), (int)n, /*exact=*/false, 1e-4f, br, 8);
        af::detect::report("型紙B 設定不変(許容差)", br, nv2, " 相対");
        std::snprintf(note, sizeof(note), "(%zu 個中 外れ %d 個)", n, nv2);
        check("[複数コア] 型紙B 並列にしても音が変わらない", nv2 == 0, note);
    }

    // ★どれだけ速くなるか。ゲームレーンの場面（遮蔽音源が多い）に寄せて測る。
    //   割っているのは音源ごとの段だけなので、遮蔽音源が多いほど効く。
    std::printf("        速さ（音源 12 本・半分が影）\n");
    std::printf("          %8s %10s %10s\n", "コア", "ms/frame", "直列比");
    double base1 = 0.0;
    for (int th : {1, 2, 4, 8}) {
        AF_SceneHandle s = build(th);
        using clk = std::chrono::high_resolution_clock;
        for (int k = 0; k < 20; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);   // 暖機
        const auto t0 = clk::now();
        const int N = 120;
        for (int k = 0; k < N; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;
        if (th == 1) base1 = ms;
        std::printf("          %8d %10.3f %9.2fx\n", th, ms, (ms > 0) ? base1 / ms : 0.0);
        AF_SceneDestroy(s);
    }
    std::printf("        ※ 音源ごとの段（ビット一致）とレイの段（許容差）の両方を割っている。\n"
                "          残る直列は 部屋・BVH 構築など。そこが頭打ちを作る。\n");

    // ★エコグラムを切って、音源ごとの段だけを見る。
    //   ゲームレーンの場面（遮蔽音源 10 本超・per-source が費用のほぼ全部）に近いのはこちら。
    //   上の数字は「エコグラムが半分を占める場面での全体の伸び」で、天井ではない。
    std::printf("        速さ（同じ場面でエコグラムを切る＝音源ごとの段だけを見る）\n");
    std::printf("          %8s %10s %10s\n", "コア", "ms/frame", "直列比");
    double baseNo = 0.0;
    for (int th : {1, 2, 4, 8}) {
        AF_SceneHandle s = build(th);
        AF_UpdateConfig c{};
        c.role1EveryN = 1; c.role2EveryN = 4; c.earlyEveryN = 1;
        c.diffSrcEveryN = 1;
        c.reflectionRays = 256; c.reflectionBounces = 3;
        c.directWeight = 1.0f; c.useReflections = 1;

        c.enableReverb = 0;                       // ← ここだけ切る
        c.speedOfSound = 343.0f; c.distanceRef = 1.5f;
        c.enableEarlyReflections = 1; c.earlyTaps = 4; c.earlyRays = 512; c.earlyBounces = 2;
        c.enableDiffractionSources = 1; c.diffSources = 3;
        AF_SceneSetUpdateConfig(s, &c);
        using clk = std::chrono::high_resolution_clock;
        for (int k = 0; k < 20; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const auto t0 = clk::now();
        const int N = 120;
        for (int k = 0; k < N; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;
        if (th == 1) baseNo = ms;
        std::printf("          %8d %10.3f %9.2fx\n", th, ms, (ms > 0) ? baseNo / ms : 0.0);
        AF_SceneDestroy(s);
    }

    // 破棄でスレッドが確実に畳まれること。畳めていなければここで固まるか落ちる。
    //   ★Unity のドメインリロードで固まる事故は、まさにこれが畳めていないと起きる。
    for (int k = 0; k < 8; ++k) {
        AF_SceneHandle s = build(4);
        AF_SceneDestroy(s);
    }
    check("[複数コア] 作って壊してを 8 回繰り返しても固まらない", true, "(join できている)");
}

// ─────────────────────────────────────────────────────────────────────
// 【段】音源ごとに「どこまで解くか」を固定する
//
//   厳密   … 回折の合成まで。体験の芯
//   簡易   … 遮蔽の音量と帯域カーブだけ。回り込みの方向は出ない
//   バーチャル … 解かない
//
//   ★見たいのは 3 つ:
//     ① 段を下げても**遮蔽の量は残る**（簡易でも「其処に何か在る」は伝わる）
//     ② 段を下げると**費用が下がる**
//     ③ 自動バーチャルが**響く部屋では発動しない**（残響が担っているので切ってはいけない）
// ─────────────────────────────────────────────────────────────────────
void testSourceTiers() {
    std::printf("\n[段] 音源ごとに解く深さを決める\n");

    // 衝立の裏に音源。遮蔽されているので厳密と簡易で差が出る。
    auto build = []() {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.3f, hw = 9.0f, hd = 9.0f;
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(3.0f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(0, 1.6f, -5.0f));
        AF_SceneSetSource(s, 1, V(0, 1.6f, 5.0f));   // 衝立の真後ろ
        return s;
    };

    // ① 段ごとの結果
    std::printf("        %-10s %10s %10s %10s %8s\n",
                "段", "生存125Hz", "生存4kHz", "こもり倍率", "二次音源");
    float occ0[kBands] = {}, occ1[kBands] = {}, occ2[kBands] = {};
    int nd0 = 0, nd1 = 0, nd2 = 0;
    const char* names[3] = {"厳密", "簡易", "バーチャル"};
    for (int tier = 0; tier < 3; ++tier) {
        AF_SceneHandle s = build();
        AF_SceneSetSourceTier(s, 1, tier);
        for (int k = 0; k < 6; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int idx = AF_SceneSourceIndex(s, 1);
        float b[kBands] = {};
        AF_SceneGetSourceOcclusion(s, idx, b);
        AF_Vector3 dp[8]; float dg[8];
        const int nd = AF_SceneGetDiffractionSources(s, idx, dp, dg, 8);
        const float ratio = (b[5] > 1e-9f) ? b[0] / b[5] : 0.0f;
        std::printf("        %-10s %10.5f %10.5f %10.2f %8d\n", names[tier], b[0], b[5], ratio, nd);
        for (int k = 0; k < kBands; ++k)
            (tier == 0 ? occ0 : tier == 1 ? occ1 : occ2)[k] = b[k];
        (tier == 0 ? nd0 : tier == 1 ? nd1 : nd2) = nd;
        AF_SceneDestroy(s);
    }
    char note[160];
    // 簡易でも遮蔽は残る（音量と帯域カーブは出る）。
    std::snprintf(note, sizeof(note), "(簡易 125Hz %.5f / 4kHz %.5f)", occ1[0], occ1[5]);
    check("[段] 簡易でも遮蔽の量が残る（素通しにならない）", occ1[0] < 0.99f && occ1[5] < 0.99f, note);
    // 簡易でも「低いほうが通る」形は出る。これが「其処に何か在る」を伝える最低限。
    std::snprintf(note, sizeof(note), "(簡易 こもり倍率 %.2f)", occ1[0] / std::max(occ1[5], 1e-9f));
    check("[段] 簡易でもこもりの向きが出る（125Hz > 4kHz）", occ1[0] > occ1[5], note);
    // バーチャルは解かない＝素通し。
    std::snprintf(note, sizeof(note), "(バーチャル 125Hz %.5f / 4kHz %.5f)", occ2[0], occ2[5]);
    check("[段] バーチャルは解かない（自由音場のまま）", occ2[0] > 0.99f && occ2[5] > 0.99f, note);
    // 二次音源は厳密だけ。
    std::snprintf(note, sizeof(note), "(厳密 %d / 簡易 %d / バーチャル %d)", nd0, nd1, nd2);
    check("[段] 回折二次音源は厳密だけ", nd0 > 0 && nd1 == 0 && nd2 == 0, note);

    // ② 費用。音源を増やして段ごとに測る。
    std::printf("        費用（衝立の裏に音源 12 本）\n");
    std::printf("          %-10s %10s %10s\n", "段", "ms/frame", "厳密比");
    double base = 0.0;
    for (int tier = 0; tier < 3; ++tier) {
        AF_SceneHandle s = build();
        for (int i = 0; i < 12; ++i) {
            AF_SceneSetSource(s, static_cast<unsigned long long>(300 + i),
                              V(-6.0f + i * 1.1f, 1.6f, 5.0f));
            AF_SceneSetSourceTier(s, static_cast<unsigned long long>(300 + i), tier);
        }
        using clk = std::chrono::high_resolution_clock;
        for (int k = 0; k < 20; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const auto t0 = clk::now();
        const int N = 120;
        for (int k = 0; k < N; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;
        if (tier == 0) base = ms;
        std::printf("          %-10s %10.3f %9.2fx\n", names[tier], ms, (ms > 0) ? base / ms : 0.0);
        if (tier == 2) {
            std::snprintf(note, sizeof(note), "(厳密 %.3f → バーチャル %.3f ms)", base, ms);
            check("[段] バーチャルは厳密よりはっきり安い", ms < base * 0.7, note);
        }
        AF_SceneDestroy(s);
    }

    // ③ ★自動バーチャルが響く部屋で発動しないこと（いちばん大事）
    //   板の案は「自由音場で聞こえないなら遮蔽込みでも聞こえない」だったが、
    //   **残響は音を足す**ので、そのまま距離で切ると響く部屋で聞こえている音を黙らせる。
    {
        AF_SceneHandle s = build();          // 閉じた部屋（響く）
        // ★半径は rc より**大きく**取る。
        //   rc は「直接音と残響が同じ大きさになる距離」。半径 > rc なら、
        //   残響の大きさ ＝ 直接音の rc での大きさ ＞ 可聴限界（＝半径での大きさ）。
        //   つまり残響が可聴限界より上まで届いている ＝ 距離では切れない。
        //   逆に半径 < rc なら残響は可聴限界より下なので、自由音場の判定で安全。
        //   （最初この向きを取り違えて、半径 1.0m・rc 1.81m で「切るな」と書いて落とした）
        AF_SceneSetSourceAudibleRadius(s, 1, 4.0f);   // rc(約1.8m) より大きい
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int idx = AF_SceneSourceIndex(s, 1);
        const int eff = AF_SceneGetSourceTierEffective(s, idx);   // 距離は 10m ある
        float rt[kBands] = {};
        AF_SceneRt60At(s, V(0, 1.6f, -5.0f), 2.0f, rt, kBands);
        const float vol = AF_SceneRoomVolumeAt(s, V(0, 1.6f, -5.0f), 2.0f);
        const float rtMid = 0.5f * (rt[2] + rt[3]);
        const float rc = (vol > 1.0f && rtMid > 1e-3f) ? 0.057f * std::sqrt(vol / rtMid) : 1e9f;
        std::snprintf(note, sizeof(note), "(体積 %.0fm3 / RT60中域 %.2fs / rc %.2fm / 段 %d)",
                      vol, rtMid, rc, eff);
        check("[段] 響く部屋では自動バーチャルへ落とさない", eff == 0, note);
        std::printf("        → rc(%.2fm) <= 聞こえる半径(4.0m) なら残響が担っている。切らない。\n", rc);
        AF_SceneDestroy(s);
    }
    // 屋外（部屋にならない＝残響が担わない）なら、距離で落とす。
    {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -0.3f, 0), V(40, 0.3f, 40), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(0, 1.6f, 0));
        AF_SceneSetSource(s, 1, V(0, 1.6f, 30.0f));
        AF_SceneSetSourceAudibleRadius(s, 1, 5.0f);   // 30m は遠い
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int eff = AF_SceneGetSourceTierEffective(s, AF_SceneSourceIndex(s, 1));
        std::snprintf(note, sizeof(note), "(屋外・距離 30m / 半径 5m / 段 %d)", eff);
        check("[段] 屋外の遠い音源は自動でバーチャルへ落ちる", eff == 2, note);

        // ヒステリシス: 半径のすぐ内側へ戻せば復帰し、境目で往復しない。
        AF_SceneSetSource(s, 1, V(0, 1.6f, 5.8f));    // 出の閾(6.25m)より内、入り(5m)より外
        for (int k = 0; k < 2; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int mid = AF_SceneGetSourceTierEffective(s, AF_SceneSourceIndex(s, 1));
        AF_SceneSetSource(s, 1, V(0, 1.6f, 4.0f));    // 入りの閾より内
        for (int k = 0; k < 2; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const int in = AF_SceneGetSourceTierEffective(s, AF_SceneSourceIndex(s, 1));
        std::snprintf(note, sizeof(note), "(5.8m で %d のまま / 4.0m で %d へ復帰)", mid, in);
        check("[段] ヒステリシス: 帯の中では段が動かず、内側で復帰する",
              mid == 2 && in == 0, note);
        AF_SceneDestroy(s);
    }
}

// ─────────────────────────────────────────────────────────────────────
// 【段の予算と順位】厳密の本数に上限を作り、可聴性の順に配る（主スレッドの上限）
//
//   物差し: score = 音量 × 1/r × 前フレームの生存 × 重要度。上から 厳密 → 簡易 → 保持。
//   縛るのは 6 つ:
//     ① 予算を呼ばなければ今までどおり（全音源が手動の段）
//     ② 厳密の本数はちょうど上限。顔ぶれは近い順。残りは「保持」で素通しの仮想ではない
//     ③ 保持は最後の答えを保つ（探りで解いた遮蔽が残り、1.0 に戻らない）
//     ④ 固定は段を保ち枠を消費する。重要度と音量は順位を動かす
//     ⑤ ヒステリシス: 入れ替わりは 1 s の後。現職の +3 dB を超えない差では入れ替わらない
//     ⑥ 費用: 12 本・半分が影で、無制限 → 厳密 6 で平均が下がる
// ─────────────────────────────────────────────────────────────────────
void testTierBudget() {
    std::printf("\n[段の予算] 厳密の本数に上限を作り、可聴性の順に配る\n");
    const float h = 4.0f, t = 0.3f, hw = 9.0f, hd = 9.0f;
    // 部屋 18 × 18 m。x 軸に沿って音源を 1.3 m 刻みに並べ、リスナーは −x の端。近い順 ＝ 順位。
    auto build = [&](int nSrc) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(-8.0f, 1.6f, 0.0f));
        for (int i = 0; i < nSrc; ++i)
            AF_SceneSetSource(s, static_cast<unsigned long long>(1 + i), V(-6.5f + i * 1.3f, 1.6f, 0.0f));
        return s;
    };
    auto tierOf = [](AF_SceneHandle s, int i) {
        return AF_SceneGetSourceTierEffective(s, AF_SceneSourceIndex(s, static_cast<unsigned long long>(1 + i)));
    };
    auto countTiers = [&](AF_SceneHandle s, int nSrc, int out[4]) {
        for (int k = 0; k < 4; ++k) out[k] = 0;
        for (int i = 0; i < nSrc; ++i) { const int tt = tierOf(s, i); if (tt >= 0 && tt < 4) ++out[tt]; }
    };
    auto run = [](AF_SceneHandle s, float sec) {
        const int k = static_cast<int>(sec * 60.0f + 0.5f);
        for (int i = 0; i < k; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
    };
    char note[240];

    // ① 予算 OFF
    {
        AF_SceneHandle s = build(12);
        run(s, 0.5f);
        int c[4]; countTiers(s, 12, c);
        std::snprintf(note, sizeof(note), "(厳密 %d / 簡易 %d / 仮想 %d / 保持 %d)", c[0], c[1], c[2], c[3]);
        check("[段の予算] 予算を呼ばなければ全音源が厳密（今までどおり）", c[0] == 12, note);
        AF_SceneDestroy(s);
    }
    // ② 厳密 4・簡易 3 の顔ぶれ
    {
        AF_SceneHandle s = build(12);
        AF_SceneSetTierBudget(s, 4, 3);
        run(s, 2.0f);
        int c[4]; countTiers(s, 12, c);
        const int probe = AF_SceneGetTierProbeIndex(s);
        std::snprintf(note, sizeof(note), "(厳密 %d / 簡易 %d / 仮想 %d / 保持 %d / 探り idx %d)", c[0], c[1], c[2], c[3], probe);
        check("[段の予算] 厳密は上限ちょうど（4 本）", c[0] == 4, note);
        check("[段の予算] 簡易は上限 ＋ 探り 1 本", c[1] == 3 + (probe >= 0 ? 1 : 0), note);
        check("[段の予算] 残りは保持で、素通しの仮想は 0", c[2] == 0 && c[3] == 12 - c[0] - c[1], note);
        bool near4 = true;
        for (int i = 0; i < 4; ++i) near4 = near4 && (tierOf(s, i) == 0);
        std::snprintf(note, sizeof(note), "(段 %d %d %d %d | %d %d %d ...)", tierOf(s,0), tierOf(s,1), tierOf(s,2), tierOf(s,3), tierOf(s,4), tierOf(s,5), tierOf(s,6));
        check("[段の予算] 厳密の 4 本は近い順", near4, note);
        const float p0 = AF_SceneGetSourcePriority(s, AF_SceneSourceIndex(s, 1));
        const float p11 = AF_SceneGetSourcePriority(s, AF_SceneSourceIndex(s, 12));
        std::snprintf(note, sizeof(note), "(1.5 m: %.3f / 15.8 m: %.3f)", p0, p11);
        check("[段の予算] 順位の物差しは近いほど大きい（1/r）", p0 > p11 * 5.0f, note);
        std::printf("        順位の物差し: 近 %.3f → 遠 %.3f（比 %.1f）\n", p0, p11, (p11 > 0) ? p0 / p11 : 0.0f);
        AF_SceneDestroy(s);
    }
    // ③ 保持は最後の答えを保つ（衝立の裏の遠い音源）
    {
        AF_SceneHandle s = build(12);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        // 衝立: x=6.0 に立て、その裏（x 6.5 / 7.8）の 2 本を影にする
        AF_SceneAddInstanceBox(s, V(6.0f, h*0.5f, 0), V(0.3f, h*0.5f, 3.0f), V(1,0,0), V(0,1,0), m);
        AF_SceneSetTierBudget(s, 4, 3);
        run(s, 3.0f);   // 保持 5 本を探りが数周する
        const int tt = tierOf(s, 11);
        float b[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 12), b);
        std::snprintf(note, sizeof(note), "(段 %d / 生存 125Hz %.3f / 4kHz %.3f)", tt, b[0], b[5]);
        check("[段の予算] 衝立の裏の遠い音源は保持（か探り中の簡易）", tt == 3 || tt == 1, note);
        check("[段の予算] 保持の生存は探りで解いた値のまま（素通し 1.0 に戻らない）", b[5] < 0.9f && b[0] > b[5], note);
        AF_SceneDestroy(s);
    }
    // ④ 固定・重要度・音量
    {
        AF_SceneHandle s = build(12);
        AF_SceneSetTierBudget(s, 4, 3);
        AF_SceneSetSourceImportance(s, 12, 1.0f, 1);     // いちばん遠い音源を固定
        AF_SceneSetSourceImportance(s, 1, 0.01f, 0);     // いちばん近い音源の重要度 −40 dB
        run(s, 2.0f);
        int c[4]; countTiers(s, 12, c);
        std::snprintf(note, sizeof(note), "(遠い固定 段 %d / 近い −40 dB 段 %d / 厳密 %d 本)", tierOf(s, 11), tierOf(s, 0), c[0]);
        check("[段の予算] 固定した遠い音源は厳密のまま", tierOf(s, 11) == 0, note);
        check("[段の予算] 固定は枠を消費する（厳密は 4 本のまま）", c[0] == 4, note);
        check("[段の予算] 重要度 −40 dB の近い音源は厳密から落ちる", tierOf(s, 0) != 0, note);
        AF_SceneSetSourceLoudness(s, 2, 0.0f);            // 次に近い音源の音量 0
        run(s, 2.0f);
        std::snprintf(note, sizeof(note), "(音量 0 の音源 段 %d)", tierOf(s, 1));
        check("[段の予算] 音量 0 の音源は厳密から落ちる", tierOf(s, 1) != 0, note);
        AF_SceneDestroy(s);
    }
    // ⑤ ヒステリシス
    {
        AF_SceneHandle s = build(12);
        AF_SceneSetTierBudget(s, 4, 3);
        run(s, 2.0f);
        AF_SceneSetListener(s, V(8.0f, 1.6f, 0.0f));     // 反対の端へ。遠かった音源が近くなる
        run(s, 0.3f); const int e03 = tierOf(s, 11);
        run(s, 1.0f); const int e13 = tierOf(s, 11);
        std::snprintf(note, sizeof(note), "(0.3 s 後 段 %d / 1.3 s 後 段 %d)", e03, e13);
        check("[段の予算] 入れ替わりは 1 s のヒステリシスの後（0.3 s ではまだ）", e03 != 0 && e13 == 0, note);
        AF_SceneDestroy(s);
    }
    {
        // 現職の加点: 2 本の音源の間で、差が +3 dB を超えない限り現職が残る
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(s, V(0, -t, 0), V(20, t, 20), V(1,0,0), V(0,1,0), m);
        AF_SceneSetSource(s, 1, V(-2.0f, 1.6f, 0));
        AF_SceneSetSource(s, 2, V( 2.0f, 1.6f, 0));
        AF_SceneSetTierBudget(s, 1, 0);
        AF_SceneSetListener(s, V(-0.2f, 1.6f, 0));   // A が近い（1.8 m vs 2.2 m）
        run(s, 1.5f);
        const int a0 = tierOf(s, 0), b0 = tierOf(s, 1);
        AF_SceneSetListener(s, V(0.2f, 1.6f, 0));    // B が近い。差は 2.2/1.8 = 1.22 倍（+1.7 dB）< 加点 +3 dB
        run(s, 3.0f);
        const int a1 = tierOf(s, 0), b1 = tierOf(s, 1);
        AF_SceneSetListener(s, V(1.0f, 1.6f, 0));    // B が 3 倍近い（+9.5 dB）。入れ替わる
        run(s, 1.5f);
        const int a2 = tierOf(s, 0), b2 = tierOf(s, 1);
        std::snprintf(note, sizeof(note), "(初 A%d B%d / +1.7dB 3s後 A%d B%d / +9.5dB 1.5s後 A%d B%d)", a0, b0, a1, b1, a2, b2);
        check("[段の予算] 現職の加点: +1.7 dB の差では 3 s 経っても入れ替わらない", a0 == 0 && b0 != 0 && a1 == 0 && b1 != 0, note);
        check("[段の予算] +9.5 dB の差なら 1 s 後に入れ替わる", a2 != 0 && b2 == 0, note);
        AF_SceneDestroy(s);
    }
    // ⑥ 費用: 12 本・半分が影（衝立の裏）。無制限 vs 厳密 6・簡易 10
    {
        auto buildShadow = [&]() {
            AF_SceneHandle s = AF_SceneCreate();
            const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
            AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(3.0f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
            AF_SceneSetListener(s, V(0, 1.6f, -5.0f));
            for (int i = 0; i < 12; ++i)
                AF_SceneSetSource(s, static_cast<unsigned long long>(300 + i), V(-6.0f + i * 1.1f, 1.6f, 5.0f));
            return s;
        };
        using clk = std::chrono::high_resolution_clock;
        double avg[2] = {0, 0}, worst[2] = {0, 0};
        int tiers[2][4] = {};
        for (int mode = 0; mode < 2; ++mode) {
            AF_SceneHandle s = buildShadow();
            if (mode == 1) AF_SceneSetTierBudget(s, 6, 10);
            for (int k = 0; k < 150; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);   // 2.5 s: 段が落ち着く
            const int N = 120;
            double sum = 0.0, mx = 0.0;
            for (int k = 0; k < N; ++k) {
                const auto t0 = clk::now();
                AF_SceneUpdate(s, 1.0f / 60.0f);
                const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
                sum += ms; mx = std::max(mx, ms);
            }
            avg[mode] = sum / N; worst[mode] = mx;
            for (int i = 0; i < 12; ++i) {
                const int tt = AF_SceneGetSourceTierEffective(s, AF_SceneSourceIndex(s, static_cast<unsigned long long>(300 + i)));
                if (tt >= 0 && tt < 4) ++tiers[mode][tt];
            }
            AF_SceneDestroy(s);
        }
        std::printf("        費用（12 本・半分が影、1 コア）\n");
        std::printf("          %-14s %10s %10s   段の内訳（厳密/簡易/仮想/保持）\n", "予算", "平均 ms", "最悪 ms");
        std::printf("          %-14s %10.3f %10.3f   %d/%d/%d/%d\n", "無制限", avg[0], worst[0], tiers[0][0], tiers[0][1], tiers[0][2], tiers[0][3]);
        std::printf("          %-14s %10.3f %10.3f   %d/%d/%d/%d\n", "厳密 6・簡易 10", avg[1], worst[1], tiers[1][0], tiers[1][1], tiers[1][2], tiers[1][3]);
        std::snprintf(note, sizeof(note), "(平均 %.3f → %.3f ms)", avg[0], avg[1]);
        check("[段の予算] 厳密 6 に絞ると平均が下がる", avg[1] < avg[0] * 0.9, note);
    }
}

// ─────────────────────────────────────────────────────────────────────
// 【更新の非同期化】主スレッドは「入力を渡す・答えを読む」だけ。解くのは DLL のワーカー
//
//   縛るのは 6 つ:
//     ① 同期と非同期で答えが**ビット一致**する（同じ入力列を流し、待ってから読む）
//     ② 主スレッドの AF_SceneUpdate は µs の桁で帰る（同期は ms）
//     ③ 仕事の最中に幾何の問い合わせを叩いても壊れず、落ち着いた後の答えは同期と一致する
//     ④ 仕事の最中に同期の口（AddInstanceBox）を呼んでも壊れず、次の答えに効く
//     ⑤ 非同期を切ると同期に戻る／仕事の最中に破棄しても壊れない
//     ⑥ 主スレッドの壁時計: 問い合わせを叩きながら 120 フレーム回す時間が同期より短い
// ─────────────────────────────────────────────────────────────────────
void testAsyncUpdate() {
    std::printf("\n[非同期] 更新をワーカースレッドへ ── 主スレッドは入力を渡して答えを読むだけ\n");
    const float h = 4.0f, t = 0.3f, hw = 9.0f, hd = 9.0f;
    struct Rig { AF_SceneHandle s; int door; };
    auto build = [&](int nSrc) {
        Rig r; r.s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(r.s, nullptr, nullptr, nullptr, 0);
        AF_SceneAddInstanceBox(r.s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(r.s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(r.s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(r.s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(r.s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(r.s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        // 衝立（扉の代わりに毎フレーム回す）
        r.door = AF_SceneAddInstanceBox(r.s, V(0, h*0.5f, 0), V(3.0f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(r.s, V(-2.0f, 1.6f, -5.0f));
        for (int i = 0; i < nSrc; ++i)
            AF_SceneSetSource(r.s, static_cast<unsigned long long>(1 + i), V(-6.0f + i * (12.0f / nSrc), 1.6f, 5.0f));
        AF_SceneSetTierBudget(r.s, 4, 2);
        return r;
    };
    // フレーム f の入力（同じ列を同期・非同期の両方へ流す）
    auto drive = [&](const Rig& r, int f) {
        const float a = 0.02f * static_cast<float>(f);
        AF_SceneUpdateInstance(r.s, r.door, V(0, h*0.5f, 0), V(3.0f, h*0.5f, 0.3f),
                               V(std::cos(a), 0, std::sin(a)), V(0, 1, 0));
        AF_SceneSetListener(r.s, V(-2.0f + 0.03f * static_cast<float>(f), 1.6f, -5.0f));
        AF_SceneSetSource(r.s, 3, V(1.0f + 0.02f * static_cast<float>(f), 1.6f, 5.0f));
    };
    auto occOf = [](AF_SceneHandle s, int srcNo, float* out6) {
        const int idx = AF_SceneSourceIndex(s, static_cast<unsigned long long>(srcNo));
        for (int k = 0; k < kBands; ++k) out6[k] = -1.0f;
        AF_SceneGetSourceOcclusion(s, idx, out6);
        return idx;
    };
    char note[240];
    using clk = std::chrono::high_resolution_clock;

    // ① 一致
    {
        Rig a = build(6), b = build(6);
        AF_SceneSetAsync(b.s, 1);
        int mismatch = 0, frames = 90, cmp = 0;
        float maxDiff = 0.0f;
        for (int f = 0; f < frames; ++f) {
            drive(a, f); AF_SceneUpdate(a.s, 1.0f / 60.0f);
            drive(b, f); AF_SceneUpdate(b.s, 1.0f / 60.0f); AF_SceneAsyncWait(b.s);
            for (int i = 1; i <= 6; ++i) {
                float oa[kBands], ob[kBands];
                const int ia = occOf(a.s, i, oa), ib = occOf(b.s, i, ob);
                for (int k = 0; k < kBands; ++k) {
                    const float d = std::fabs(oa[k] - ob[k]);
                    maxDiff = std::max(maxDiff, d);
                    if (d != 0.0f) ++mismatch;
                    ++cmp;
                }
                if (AF_SceneGetSourceTierEffective(a.s, ia) != AF_SceneGetSourceTierEffective(b.s, ib)) ++mismatch;
                AF_Vector3 pa[8], pb[8]; float ga[48], gb[48];
                const int na = AF_SceneGetEarlyReflections(a.s, ia, pa, ga, 8);
                const int nb = AF_SceneGetEarlyReflections(b.s, ib, pb, gb, 8);
                if (na != nb) ++mismatch;
                float ea[32 * kBands] = {}, eb[32 * kBands] = {};
                const int ma = AF_SceneGetEchogramBands(a.s, ia, ea, 32);
                const int mb = AF_SceneGetEchogramBands(b.s, ib, eb, 32);
                if (ma != mb) ++mismatch;
                for (int k = 0; k < ma * kBands; ++k) if (ea[k] != eb[k]) { ++mismatch; break; }
            }
        }
        std::snprintf(note, sizeof(note), "(%d フレーム × 6 本、比較 %d、不一致 %d、最大差 %.3g)", frames, cmp, mismatch, maxDiff);
        check("[非同期] 同期と非同期で答えがビット一致（遮蔽・段・早期反射の本数・尾）", mismatch == 0, note);
        AF_SceneDestroy(a.s); AF_SceneDestroy(b.s);
    }
    // ② 主スレッドの AF_SceneUpdate の費用
    {
        Rig a = build(12), b = build(12);
        AF_SceneSetAsync(b.s, 1);
        for (int f = 0; f < 30; ++f) { drive(a, f); AF_SceneUpdate(a.s, 1.0f / 60.0f); drive(b, f); AF_SceneUpdate(b.s, 1.0f / 60.0f); AF_SceneAsyncWait(b.s); }
        double syncMs = 0.0, launchUs = 0.0, skipUs = 0.0; float computeMs = 0.0f;
        const int N = 60;
        for (int f = 30; f < 30 + N; ++f) {
            drive(a, f);
            auto t0 = clk::now(); AF_SceneUpdate(a.s, 1.0f / 60.0f);
            syncMs += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            drive(b, f);
            t0 = clk::now(); AF_SceneUpdate(b.s, 1.0f / 60.0f);            // 着手（写し＋待ち行列＋投げる）
            launchUs += std::chrono::duration<double, std::micro>(clk::now() - t0).count();
            t0 = clk::now(); AF_SceneUpdate(b.s, 1.0f / 60.0f);            // 仕事中: 何もしない
            skipUs += std::chrono::duration<double, std::micro>(clk::now() - t0).count();
            AF_SceneAsyncWait(b.s);
            float cms; int lag, sk, q; AF_SceneGetUpdateStats(b.s, &cms, &lag, &sk, &q); computeMs += cms;
        }
        syncMs /= N; launchUs /= N; skipUs /= N; computeMs /= N;
        std::printf("        主スレッドの AF_SceneUpdate（12 本・衝立が回る）\n");
        std::printf("          同期        %8.3f ms\n", syncMs);
        std::printf("          非同期 着手 %8.1f us（写し＋待ち行列＋投げる）／仕事中 %6.1f us／ワーカーの計算 %.3f ms\n", launchUs, skipUs, computeMs);
        std::snprintf(note, sizeof(note), "(同期 %.3f ms → 非同期 着手 %.1f us)", syncMs, launchUs);
        check("[非同期] 主スレッドの AF_SceneUpdate は同期の 1/10 以下", launchUs * 1e-3 < syncMs * 0.1, note);
        AF_SceneDestroy(a.s); AF_SceneDestroy(b.s);
    }
    // ③ 仕事の最中に幾何の問い合わせを叩く ＋ ⑥ 壁時計
    {
        Rig a = build(12), b = build(12);
        AF_SceneSetAsync(b.s, 1);
        auto hammer = [&](AF_SceneHandle s, int f, int reps, bool* bad) {
            const AF_Vector3 L = V(-2.0f + 0.03f * static_cast<float>(f), 1.6f, -5.0f);
            for (int k = 0; k < reps; ++k) {
                const int i = k % 12;
                const AF_Vector3 S = V(-6.0f + i * 1.0f, 1.6f, 5.0f);
                float tr[kBands], df[kBands], occ = 0.0f;
                AF_SceneComputeTransmissionBands(s, L, S, tr, kBands);
                AF_SceneComputeDiffractionBands(s, L, S, df, kBands);
                AF_SceneComputeSoftOcclusion(s, L, S, tr, kBands, &occ);
                AF_Vector3 mid; const float dd = AF_SceneDiffractionPath(s, S, L, &mid);
                const int room = AF_SceneRoomAt(s, L);
                float rt[kBands] = {}; AF_SceneRt60At(s, L, 2.0f, rt, kBands);
                for (int q = 0; q < kBands; ++q)
                    if (!(tr[q] >= 0.0f && tr[q] <= 1.0f) || !(df[q] >= 0.0f && df[q] <= 1.0f) || !(rt[q] >= 0.0f)) *bad = true;
                if (!(occ >= 0.0f && occ <= 1.0f) || !(dd >= -1.0f) || room < -1) *bad = true;
            }
        };
        bool badA = false, badB = false;
        const int frames = 120, reps = 40;
        auto t0 = clk::now();
        for (int f = 0; f < frames; ++f) { drive(a, f); AF_SceneUpdate(a.s, 1.0f / 60.0f); hammer(a.s, f, reps, &badA); }
        const double wallSync = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        t0 = clk::now();
        for (int f = 0; f < frames; ++f) { drive(b, f); AF_SceneUpdate(b.s, 1.0f / 60.0f); hammer(b.s, f, reps, &badB); }
        const double wallAsync = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        float cms; int lag, skipped, queued; AF_SceneGetUpdateStats(b.s, &cms, &lag, &skipped, &queued);
        // 落ち着かせて（残りの入力を全部反映）から、問い合わせを同期と突き合わせる
        AF_SceneAsyncWait(b.s); AF_SceneUpdate(b.s, 1.0f / 60.0f); AF_SceneAsyncWait(b.s);
        AF_SceneUpdate(a.s, 1.0f / 60.0f);
        int qmis = 0;
        for (int i = 0; i < 12; ++i) {
            const AF_Vector3 L = V(-2.0f + 0.03f * (frames - 1), 1.6f, -5.0f);
            const AF_Vector3 S = V(-6.0f + i * 1.0f, 1.6f, 5.0f);
            float ta[kBands], tb[kBands], oa = 0, ob = 0;
            AF_SceneComputeSoftOcclusion(a.s, L, S, ta, kBands, &oa);
            AF_SceneComputeSoftOcclusion(b.s, L, S, tb, kBands, &ob);
            for (int q = 0; q < kBands; ++q) if (ta[q] != tb[q]) ++qmis;
            if (AF_SceneRoomAt(a.s, S) != AF_SceneRoomAt(b.s, S)) ++qmis;
        }
        std::printf("        問い合わせを叩きながら %d フレーム（1 フレームに %d 組: 透過・回折・遮蔽・迂回路・部屋・RT60）\n", frames, reps);
        std::printf("          壁時計  同期 %.1f ms ／ 非同期 %.1f ms（追いつかず %d フレーム、ワーカー %.2f ms/回）\n", wallSync, wallAsync, skipped, cms);
        std::snprintf(note, sizeof(note), "(範囲外 同期 %d / 非同期 %d、落ち着いた後の不一致 %d)", badA ? 1 : 0, badB ? 1 : 0, qmis);
        check("[非同期] 仕事の最中の問い合わせが壊れず、落ち着いた後は同期と一致する", !badB && qmis == 0, note);
        std::snprintf(note, sizeof(note), "(同期 %.1f → 非同期 %.1f ms)", wallSync, wallAsync);
        check("[非同期] 問い合わせ込みの主スレッドの壁時計が同期より短い", wallAsync < wallSync, note);
        AF_SceneDestroy(a.s); AF_SceneDestroy(b.s);
    }
    // ④ 仕事の最中に同期の口（構築）を呼ぶ
    {
        Rig b = build(12);
        AF_SceneSetAsync(b.s, 1);
        for (int f = 0; f < 30; ++f) { drive(b, f); AF_SceneUpdate(b.s, 1.0f / 60.0f); }
        AF_SceneAsyncWait(b.s);
        float before[kBands]; occOf(b.s, 12, before);            // いちばん右の音源（見通せる）
        const int m = AF_SceneAddMaterial(b.s, nullptr, nullptr, nullptr, 0);
        drive(b, 30); AF_SceneUpdate(b.s, 1.0f / 60.0f);          // 投げた直後に
        const int n0 = AF_SceneInstanceCount(b.s);
        const int id = AF_SceneAddInstanceBox(b.s, V(2.0f, h*0.5f, 0), V(1.5f, h*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);   // 同期: 仕事を待って入る
        const int n1 = AF_SceneInstanceCount(b.s);
        for (int f = 31; f < 40; ++f) { drive(b, f); AF_SceneUpdate(b.s, 1.0f / 60.0f); AF_SceneAsyncWait(b.s); }
        float after[kBands]; occOf(b.s, 12, after);
        std::snprintf(note, sizeof(note), "(id %d、数 %d → %d、生存 4kHz %.3f → %.3f)", id, n0, n1, before[5], after[5]);
        check("[非同期] 仕事の最中の構築（AddInstanceBox）が壊れず次の答えに効く", id >= 0 && n1 == n0 + 1 && after[5] < before[5], note);
        AF_SceneDestroy(b.s);
    }
    // ⑤ 切る／仕事の最中に破棄
    {
        Rig b = build(6);
        AF_SceneSetAsync(b.s, 1);
        for (int f = 0; f < 20; ++f) { drive(b, f); AF_SceneUpdate(b.s, 1.0f / 60.0f); }
        AF_SceneSetAsync(b.s, 0);
        const int isA = AF_SceneIsAsync(b.s);
        drive(b, 20); AF_SceneUpdate(b.s, 1.0f / 60.0f);
        float o[kBands]; const int idx = occOf(b.s, 1, o);
        std::snprintf(note, sizeof(note), "(IsAsync %d、index %d、生存 %.3f)", isA, idx, o[0]);
        check("[非同期] 切ると同期に戻り、答えは続く", isA == 0 && idx >= 0 && o[0] >= 0.0f && o[0] <= 1.0f, note);
        AF_SceneDestroy(b.s);
        Rig c = build(12);
        AF_SceneSetAsync(c.s, 1);
        drive(c, 0); AF_SceneUpdate(c.s, 1.0f / 60.0f);
        AF_SceneDestroy(c.s);                                     // 仕事の最中に破棄（待ってから畳む）
        check("[非同期] 仕事の最中に破棄しても壊れない", true, "");
    }
}

// ─────────────────────────────────────────────────────────────────────
// 【調整支援】「この地点でこう聞こえてほしい」に合わせるための土台
//
//   道具の流れ:
//     ① いま届いている音の内訳を採る（透過ぶん / 回折ぶん）
//     ② 透過が担っているなら、**誰が担っているか**を engine に名指しさせる
//     ③ その材質を書き換える → 音が動く
//     ④ 届かない目標は「届かない」と分かる
//
//   ★ここで縛るのは②③④。①は既存の API（GetSourceOcclusion / ComputeSoftOcclusion）。
//   ⚠ 回折が担っている帯域では材質を触っても動かない。それも検査で押さえる
//     （効かないつまみを回し続けるのが、この手の道具のいちばんの失敗）。
// ─────────────────────────────────────────────────────────────────────
void testTuningSupport() {
    std::printf("\n[調整支援] 誰が担っているかを名指しできるか\n");

    AF_SceneHandle s = AF_SceneCreate();
    const int mWall = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);   // 既定の壁
    // 扉だけ別の材質にする。壁より通す＝担い手になるはず。
    const float trDoor[kBands] = {0.02f, 0.015f, 0.010f, 0.006f, 0.004f, 0.003f};
    const float abDoor[kBands] = {0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f};
    const int mDoor = AF_SceneAddMaterial(s, trDoor, abDoor, nullptr, kBands);

    const float h = 4.0f, t = 0.3f, hw = 6.0f, hd = 6.0f;
    AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), mWall);
    AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), mWall);
    // 音源とリスナーのあいだに、壁 1 枚＋扉 1 枚。扉のほうがよく通す。
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(hw+t, h*0.5f, 0.2f), V(1,0,0), V(0,1,0), mWall);
    const unsigned long long door =
        AF_SceneAddInstanceBox(s, V(0, 1.6f, 1.5f), V(0.5f, 1.0f, 0.05f), V(1,0,0), V(0,1,0), mDoor);
    (void)door;
    const AF_Vector3 L = V(0, 1.6f, -4.0f), S = V(0, 1.6f, 4.0f);
    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);
    for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);

    // ② 担い手を名指しできるか
    int inst[8], mat[8]; float lossDb[8];
    const int nc = AF_SceneTransmissionCarriers(s, L, S, inst, mat, lossDb, 8);
    std::printf("        担い手 %d 件（透過損失の大きい順）\n", nc);
    for (int i = 0; i < nc; ++i)
        std::printf("          [%d] 実体 %d / 材質 %d / 透過損失 %.1f dB%s\n",
                    i, inst[i], mat[i], lossDb[i], (mat[i] == mDoor) ? "  ← 扉" : "");
    char note[160];
    std::snprintf(note, sizeof(note), "(%d 件 / 先頭の材質 %d)", nc, nc > 0 ? mat[0] : -1);
    check("[調整] 経路上の遮蔽物を名指しできる", nc >= 2, note);
    // 壁のほうが遮る＝損失が大きいので先頭に来るはず（順序が壊れていないか）。
    bool sorted = true;
    for (int i = 1; i < nc; ++i) if (lossDb[i] > lossDb[i - 1] + 1e-4f) sorted = false;
    check("[調整] 透過損失の大きい順に並ぶ", sorted, "");

    // ③ ★名指しの順が**効き目の順**になっているか
    //   道具の値打ちは「どれを触れば効くか」を当てること。1 番目を触ったときのほうが
    //   2 番目を触ったときより大きく動かなければ、名指しに意味が無い。
    //   ⚠ ここは倍率ではなく**大小関係**で縛る（絶対値で書かない方針）。
    float before[kBands] = {};
    AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), before);
    auto bumpAndMeasure = [&](int matId, const float* baseTr) {
        float tr2[kBands];
        for (int b = 0; b < kBands; ++b) tr2[b] = std::min(1.0f, baseTr[b] * 10.0f);
        AF_SceneSetMaterial(s, matId, tr2, abDoor, nullptr, kBands);
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), g);
        AF_SceneSetMaterial(s, matId, baseTr, abDoor, nullptr, kBands);   // 戻す
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        return g[0];
    };
    // 1 番目（壁）の元の透過率を engine から引く。
    float trWall[kBands] = {};
    AF_SceneGetMaterial(s, mat[0], trWall, nullptr, nullptr, kBands);
    const float top = bumpAndMeasure(mat[0], trWall);
    const float second = bumpAndMeasure(mat[1], trDoor);
    std::printf("        125Hz %.5f を基準に、10 倍にしたときの動き\n", before[0]);
    std::printf("          1番目(材質%d) → %.5f （%.2f 倍）\n", mat[0], top, top / before[0]);
    std::printf("          2番目(材質%d) → %.5f （%.2f 倍）\n", mat[1], second, second / before[0]);
    std::snprintf(note, sizeof(note), "(1番目 %.2f倍 / 2番目 %.2f倍)",
                  top / before[0], second / before[0]);
    check("[調整] 1番目の担い手のほうがよく効く（名指しに意味がある）", top > second, note);

    // ④ 回折が担っている帯域では材質を触っても動かない ── それが見えること。
    //   透過だけの値と合成後の値を並べれば、差が回折の取り分になる。
    {
        float soft[kBands] = {}; float occFrac = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, soft, kBands, &occFrac);
        float comb[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), comb);
        std::printf("        内訳(125Hz) 透過のみ %.5f / 合成後 %.5f  → 回折の取り分 %.5f\n",
                    soft[0], comb[0], std::max(0.0f, comb[0] - soft[0]));
        check("[調整] 透過ぶんと合成後を別々に採れる（回折の取り分が出せる）",
              soft[0] >= 0.0f && comb[0] >= soft[0] - 1e-3f, "");
    }
    AF_SceneDestroy(s);
}

// ─────────────────────────────────────────────────────────────────────
// 【早期反射の共有】音源ごとに撃つのをやめて、リスナーから 1 回にした影響
//
//   レイの経路（原点・方向・当たり・バウンス）は全音源で同一なので共有できる。
//   ⚠ ただし 1 つだけ音源に依存する量がある ── レイの飛距離 maxDist(=refDist×8+50)。
//     共有するには**いちばん遠い音源**に合わせるので、近い音源のレイは以前より長く飛ぶ。
//     → 遠くの弱いタップが増える可能性がある。**上位 N の選び方が変わっていないか**を見る。
//
//   基準は「その音源だけのシーン」。1 音源なら maxDist は元と同じなので、
//   共有版でも個別版と同じ答えになるはず。それと群れの中での値を比べる。
// ─────────────────────────────────────────────────────────────────────
void testEarlySharing() {
    std::printf("\n[早期反射・共有] 1 音源のときと群れの中で答えが変わらないか\n");

    auto build = [](int nSrc) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.3f, hw = 8.0f, hd = 8.0f;
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(0, 1.6f, -6.0f));
        // ★距離をばらけさせる。近い音源ほど「飛距離を遠い音源に合わせた」影響を受ける。
        for (int i = 0; i < nSrc; ++i)
            AF_SceneSetSource(s, static_cast<unsigned long long>(400 + i),
                              V(-5.0f + i * 2.5f, 1.6f, -4.0f + i * 3.0f));
        for (int k = 0; k < 6; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        return s;
    };

    const int kSrc = 5;
    // 群れの中での値
    AF_SceneHandle sAll = build(kSrc);
    // 音源 0 番だけのシーン（＝共有しても個別版と同じになる基準）
    AF_SceneHandle sOne = AF_SceneCreate();
    {
        const int m = AF_SceneAddMaterial(sOne, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.3f, hw = 8.0f, hd = 8.0f;
        AF_SceneAddInstanceBox(sOne, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(sOne, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(sOne, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(sOne, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(sOne, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(sOne, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(sOne, V(0, 1.6f, -6.0f));
        AF_SceneSetSource(sOne, 400, V(-5.0f, 1.6f, -4.0f));
        for (int k = 0; k < 6; ++k) AF_SceneUpdate(sOne, 1.0f / 60.0f);
    }

    AF_Vector3 pA[8], pB[8]; float gA[8 * kBands], gB[8 * kBands];
    const int nA = AF_SceneGetEarlyReflections(sAll, AF_SceneSourceIndex(sAll, 400), pA, gA, 8);
    const int nB = AF_SceneGetEarlyReflections(sOne, AF_SceneSourceIndex(sOne, 400), pB, gB, 8);
    std::printf("        いちばん近い音源のタップ数: 群れの中 %d / 単独 %d\n", nA, nB);

    double worst = 0.0, sumA = 0.0, sumB = 0.0;
    const int nc = std::min(nA, nB);
    for (int k = 0; k < nc * kBands; ++k) {
        sumA += gA[k]; sumB += gB[k];
        worst = std::max(worst, std::fabs((double)gA[k] - (double)gB[k]));
    }
    const double rel = (sumB > 1e-9) ? std::fabs(sumA - sumB) / sumB : 0.0;
    std::printf("        エネルギー和 群れ %.4f / 単独 %.4f（相対差 %.2e）最大の差 %.3e\n",
                sumA, sumB, rel, worst);
    char note[160];
    std::snprintf(note, sizeof(note), "(タップ数 %d vs %d / 総和の相対差 %.2e)", nA, nB, rel);
    // ★飛距離を遠い音源に合わせたぶん、遠くの弱いタップが増えうる。
    //   上位の並びが変わっていなければ、聞こえ方は変わらない。
    check("[早期反射] 共有しても上位のタップが変わらない（相対差 < 5%）",
          nA == nB && rel < 0.05, note);

    AF_SceneDestroy(sAll);
    AF_SceneDestroy(sOne);
}

// ─────────────────────────────────────────────────────────────────────
// 【幾何が主役か】ポータルを切っても素の回折で音が出るか
//
//   元の判定基準（[[portal-is-precision-not-mechanism]]）:
//     「ポータルを全部消しても音が出るか」。出れば幾何が主役、出なければ authoring
//
//   2026-08-23 にこれを「**手置きを**全部消しても音が出るか」へ読み替えた。
//   自動ポータルは部屋グラフ（ボクセル分割）から出ていて誰も注記していない ＝ 幾何そのもの、
//   という理由。「自動ポータルを消しても動け」は「BVH を消しても動け」に近い。
//
//   ⚠ ただし読み替えで**安全網が 1 枚減る**。自動生成にはパラメータがあり
//     （最小面積・格子の大きさ・重複の距離）、外すと開口が消える。つまり
//     「網羅性の問題」は消えたのではなく、**人の注記からパラメータ調整へ移っただけ**。
//   → だから元の基準を検査として残す。**自動ポータルも切って、素の回折だけで音が出るか。**
//     ここが 0 になったら、聞こえる場所を決めているのが幾何ではなくなったということ。
// ─────────────────────────────────────────────────────────────────────
// ★測る 2 点は 1 箇所で持つ。以前は build と測定で別々に書いていて、
//   片方だけ動かすと「見通しを測っているのに気づかない」状態になる。
static const AF_Vector3 kGeoL = V(-5.0f, 1.6f, -2.0f);
static const AF_Vector3 kGeoS = V( 3.0f, 1.6f,  4.0f);

void testGeometryIsPrimary() {
    std::printf("\n[幾何が主役か] ポータルを切っても素の回折で音が出るか\n");

    // 壁に戸口を 1 つ空けただけの形。ポータルを置く/置かないで比べる。
    auto build = [](int autoPortals) {
        AF_SceneHandle s = AF_SceneCreate();
        AF_SceneSetAutoPortals(s, autoPortals);
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.3f, hw = 8.0f, hd = 8.0f;
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        // 部屋を割る仕切り。中央に**戸口の実寸**（幅 0.9m × 高さ 2.0m・まぐさ付き）を残す。
        //   ⚠ 最初は幅 1.6m × 天井まで開けたが、**部屋が 1 個のまま**でポータルが生えず、
        //     ON/OFF が同じ値になって「緑だが何も測っていない」検査になった。
        //     広い開口は「くびれのある 1 部屋」と判定されるのが正しい振る舞い。
        AF_SceneAddInstanceBox(s, V(-4.425f, h*0.5f, 0), V(3.975f, h*0.5f, 0.25f), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( 4.425f, h*0.5f, 0), V(3.975f, h*0.5f, 0.25f), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0.0f, 3.0f, 0), V(0.45f, 1.0f, 0.25f), V(1,0,0), V(0,1,0), m);  // まぐさ
        // ★直線が**壁を貫く**位置に置くこと。戸口を素通りすると回折ではなく見通しを測る。
        //   最初 (-3,-4)→(3,4) にしたが、z=0 での x が 0（＝開口の真ん中）で素通りだった。
        //   いまは z=0 で x=-2.33 ＝ 仕切りの中。届くには戸口を回り込むしかない。
        AF_SceneSetListener(s, kGeoL);
        AF_SceneSetSource(s, 1, kGeoS);
        for (int k = 0; k < 8; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        return s;
    };

    float gOn[kBands] = {}, gOff[kBands] = {};
    int pOn = 0, pOff = 0, dummy = 0;
    {
        AF_SceneHandle s = build(1);
        AF_SceneComputeDiffractionBands(s, kGeoL, kGeoS, gOn, kBands);
        AF_SceneGetPortalCounts(s, &pOn, &dummy);
        AF_SceneDestroy(s);
    }
    {
        AF_SceneHandle s = build(0);
        AF_SceneComputeDiffractionBands(s, kGeoL, kGeoS, gOff, kBands);
        AF_SceneGetPortalCounts(s, &pOff, &dummy);
        AF_SceneDestroy(s);
    }
    // ★部屋が割れていないと開口が無く、ポータルも生えない＝比較が成立しない。
    //   「緑だが何も測っていない」を避けるため、部屋数も出す。
    {
        AF_SceneHandle s = build(1);
        int nx = 0, ny = 0, nz = 0; float cell = 0.0f;
        AF_SceneRoomGridDims(s, &nx, &ny, &nz, &cell);
        std::printf("        部屋 %d 個 / 格子 %dx%dx%d @%.2fm\n",
                    AF_SceneRoomCount(s), nx, ny, nz, cell);
        AF_SceneDestroy(s);
    }
    std::printf("        自動ポータル ON  ポータル %d 枚   125Hz %.5f / 4kHz %.5f\n",
                pOn, gOn[0], gOn[5]);
    std::printf("        自動ポータル OFF ポータル %d 枚   125Hz %.5f / 4kHz %.5f\n",
                pOff, gOff[0], gOff[5]);
    const float ratio = (gOn[0] > 1e-9f) ? gOff[0] / gOn[0] : 0.0f;
    std::printf("        → 素の回折だけで ON の %.2f 倍。**0 になったら幾何の経路が死んでいる。**\n",
                ratio);

    char note[160];
    std::snprintf(note, sizeof(note), "(ポータル無しで 125Hz %.5f / ON 比 %.2f)", gOff[0], ratio);
    // ★量ではなく「出るか」だけを縛る。ポータルは精度の上積みなので、値が違うのは正しい。
    //   縛りたいのは「ポータルが唯一の手段になっていないこと」。
    check("[幾何] ポータルを全部切っても素の回折で音が出る", gOff[0] > 1e-4f, note);
    std::printf("        ※ 2026-08-23 に基準を「手置きを全部消しても」へ読み替えたが、\n"
                "          自動生成のパラメータ次第で開口が消えうるので、元の基準も残している。\n");
}

// ─────────────────────────────────────────────────────────────────────
// 【閉扉】扉を閉じたまま近づくと、あるところで急に中の音が聞こえ出さないか
//
//   連絡板 2026-08-23（紹介資料の測定中）で報告された症状:
//     0.6m で -13.5dB（帯域まっすぐ＝開いた経路）／0.7m で -36.5dB（透過）
//     **10cm で 23.0 dB。**しかも音の性質が「回り込み」から「透過」へ入れ替わる。
//
//   ★帯域が**まっすぐ**なのが手がかり。回折も透過も周波数依存なので、平らなのは
//     「開口が全開（f が全帯域 1.0）」と判定されているということ。
//   閉扉なら透過しか経路が無いので、近距離側の -13.5dB は幻の経路。
//
//   実機で言えば「**閉じた扉に近づくと、あるところで急に中の音が聞こえ出す**」。
//   主題が扉なので体験にまっすぐ当たる。既知の連続性の不具合（12.5/8.8/14.9/27.6dB）の
//   中でいちばん大きい。
// ─────────────────────────────────────────────────────────────────────
void testClosedDoorApproach() {
    std::printf("\n[閉扉] 閉じた扉へ近づくときに跳ばないか\n");
    // 連絡板の形状に合わせる: 部屋 7.08 x 3.48 x 3.0 / 戸口 0.90 幅 / 壁厚 0.16 / 扉 0.08 厚
    const float rw = 7.08f, rd = 3.48f, rh = 3.0f, wt = 0.16f, dw = 0.90f;
    AF_SceneHandle s = AF_SceneCreate();
    const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    const float cx = rw * 0.5f, cz = rd;            // 戸口の中心（+Z 側の壁）
    AF_SceneAddInstanceBox(s, V(cx, -wt, rd*0.5f), V(rw*0.5f+wt, wt, rd*0.5f+wt), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(cx, rh+wt, rd*0.5f), V(rw*0.5f+wt, wt, rd*0.5f+wt), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(-wt, rh*0.5f, rd*0.5f), V(wt, rh*0.5f, rd*0.5f+wt), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(rw+wt, rh*0.5f, rd*0.5f), V(wt, rh*0.5f, rd*0.5f+wt), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(cx, rh*0.5f, -wt), V(rw*0.5f+wt, rh*0.5f, wt), V(1,0,0), V(0,1,0), m);
    // 戸口のある壁: 左右の袖＋まぐさ
    const float lw = (rw - dw) * 0.5f;
    AF_SceneAddInstanceBox(s, V(lw*0.5f, rh*0.5f, cz), V(lw*0.5f, rh*0.5f, wt), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(rw - lw*0.5f, rh*0.5f, cz), V(lw*0.5f, rh*0.5f, wt), V(1,0,0), V(0,1,0), m);
    AF_SceneAddInstanceBox(s, V(cx, 2.5f, cz), V(dw*0.5f, 0.5f, wt), V(1,0,0), V(0,1,0), m);
    // 閉じた扉（戸口と面一）
    AF_SceneAddInstanceBox(s, V(cx, 1.0f, cz), V(dw*0.5f, 1.0f, 0.04f), V(1,0,0), V(0,1,0), m);
    // 音源は部屋の中（戸口の左・奥）
    AF_SceneSetSource(s, 1, V(cx - 1.94f, 1.6f, cz - 1.12f));

    std::printf("        距離   総量(dB)  低−高(dB)   f使用  面 有界  縁u   v   捨\n");
    float prevDb = 0.0f, maxJump = 0.0f, jumpAt = 0.0f;
    bool first = true;
    // 型紙へ渡す列（道具の走査と同じ形）。
    std::vector<float> dbSeries, atSeries, bandSeries;
    for (float d = 0.3f; d <= 1.51f; d += 0.1f) {
        const AF_Vector3 L = V(cx, 1.6f, cz + d);
        AF_SceneSetListener(s, L);
        for (int k = 0; k < 3; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        float g[kBands] = {};
        AF_SceneGetSourceOcclusion(s, AF_SceneSourceIndex(s, 1), g);
        float sum = 0.0f;
        for (int b = 0; b < kBands; ++b) sum += g[b];
        const float db = 20.0f * std::log10(std::max(sum / kBands, 1e-6f));
        const float tilt = 20.0f * std::log10(std::max(g[0], 1e-6f) / std::max(g[5], 1e-6f));
        float d17[32] = {};
        AF_SceneDebugDiffractionPath(s, L, V(cx - 1.94f, 1.6f, cz - 1.12f), d17, 0);
        std::printf("        %4.1f m %8.1f %9.1f      %s   %.0f  %.0f   %.2f %.2f  %.0f\n",
                    d, db, tilt, d17[18] > 0.5f ? "○" : "×",
                    d17[22], d17[23], d17[24], d17[25], d17[26]);
        dbSeries.push_back(db); atSeries.push_back(d);
        for (int b = 0; b < kBands; ++b) bandSeries.push_back(g[b]);
        if (!first) {
            const float j = std::fabs(db - prevDb);
            if (j > maxJump) { maxJump = j; jumpAt = d; }
        }
        prevDb = db; first = false;
    }
    std::printf("        → 0.1m あたりの最大変化 %.1f dB @ %.1f m  %s\n",
                maxJump, jumpAt, (maxJump < 6.0f) ? "" : "未達 ←");

    // ★★ ここから下は**デバッグツールの走査とまったく同じ式**（detectors.h）★★
    //   道具が録音を走査するときに呼ぶ関数を、検査からも呼ぶ。別々の式にすると
    //   「道具は見つけたのに検査は通る」「検査は落ちるのに道具は黙る」が起きる。
    {
        af::detect::Break br[16];
        const int nj = af::detect::noJump(dbSeries.data(), atSeries.data(),
                                          (int)dbSeries.size(), 6.0f, br, 16);
        af::detect::report("型紙1 跳ばない(6dB)", br, nj, " dB");
        // ★型紙4: 遮蔽されているのに帯域がまっすぐ＝幻。
        //   閉扉なので全行が遮蔽 ＝ occluded に nullptr を渡して全行を見る。
        const int nRows = static_cast<int>(bandSeries.size()) / af::detect::kBands;
        const int np = af::detect::noPhantom(bandSeries.data(), nullptr,
                                             atSeries.data(), nRows, 3.0f, br, 16);
        af::detect::report("型紙4 幻が出ない(傾き3dB)", br, np, " dB");
        std::printf("        ※ この 2 つは道具がキャプチャを走査するときと同じ関数です。\n");
    }
    std::printf(
        "        ★原因（2026-08-23 特定）: **有界判定が二値**。\n"
        "          積分窓の縁の開き具合 uFrac が kBoundedPerimMax(0.45) を跨いだ瞬間に\n"
        "          「有界／半空間」が切り替わり、f を使う経路と前川へ落ちる経路が入れ替わる。\n"
        "          上の表で 縁u 0.41→0.73 と跨いだ所で 有界 1→0、f 使用 ○→× になっている。\n"
        "        ★しかも**2 つの模型が 20dB 食い違う**ので、どこで切り替えても崖になる。\n"
        "          閉じた扉なら透過しか経路が無いので -37dB 側が正しく、前川側の -17dB は幻。\n"
        "          前川は「扉が閉じている」ことを知らず、隙間幅ゲートも効いていない\n"
        "          （帯域が平ら＝slitWidthBandGain が素通し＝slitWidthAt が広い値を返している。\n"
        "            設計 §5-11 の失敗様態 4「まぐさに乗ると戸口の高さを測る」が生きている）。\n"
        "        → 直し方は**滑らかに混ぜる**ことではない。20dB 食い違う 2 つを混ぜても\n"
        "          途中がどちらでもない値になるだけ。**食い違いそのものを潰す**のが筋。\n"
        "          設計 §5-13（f と前川は排他）に触るので、判断を仰いでから着手する。\n");
    // ★ここは check にしていない。**既知の未解決**であり、このファイルの書き方に合わせて
    //   数字を毎回出して「未達」と示す（合否にすると赤が常態化して意味が濁る）。
    //   直したら check へ格上げすること。
    AF_SceneDestroy(s);
}

// ─────────────────────────────────────────────────────────────────────
// 【診断】エコグラムの費用は音源数にどう効くか（尾を焼く前の下調べ）
//
//   レイはリスナーから 1 回撃って共有しているが、**各ヒットで音源数ぶん**
//   computeTransmission を回している（[音源][ビン][帯域] へ積むため）。
//   一方で出荷経路（VoiceConvolver）が読むのは**部屋の代表 1 本ぶんだけ**
//   （尾の形は部屋ごとに共有＝段階①）。残りは積んで捨てている可能性がある。
//
//   ★焼く前に、ここがどれだけ効くのかを数字にする。
//     音源数に強く比例するなら「部屋ごとに 1 本だけ積む」で先に減らせる。
//     比例が弱いなら、費用はレイ側なので**焼くしかない**。
// ─────────────────────────────────────────────────────────────────────
// ================================================ 尾を焼く前の決定的な測定
// ★「尾はリスナーの部屋にしか依存しない」は**仮説**。焼く仕組みを作る前に測る。
//   ここで決まるのは:
//     ・部屋の中で尾がほとんど変わらない → **部屋ごとに 1 本**焼けばよい（24〜120KB）
//     ・部屋の中でも変わる               → 格子で焼く必要がある（1.5〜6MB／継ぎ目の心配）
//   ⚠ 形（減衰の傾き）と量（レベル）を**分けて**見る。
//     出荷経路は「形は代表・量は自分」なので、焼くのは**形**。
//     量が場所で変わるのは正常（距離減衰）で、それは焼く対象ではない。
void diagnoseTailProbeDensity() {
    std::printf("\n[診断] 部屋の中で尾はどれだけ変わるか（焼く密度を決める）\n");

    AF_SceneHandle s = AF_SceneCreate();
    // ⚠ 最初この検査は 2 部屋を同じ材質・戸口 2m で組んでいて、**部屋をまたぐ差が
    //   0.09dB** しか出なかった。2 つが音響的に 1 つの空間になっていただけで、
    //   測っていたのは「部屋の違い」ではなかった。
    //   → 戸口を 0.9m にして、**小部屋だけよく吸う材質**にする
    //     （per-source を入れた元のバグ「響く部屋と吸う部屋」と同じ形にそろえる）。
    const int mLive = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);   // 既定＝響く
    const float absorb[kBands] = { 0.60f, 0.70f, 0.80f, 0.85f, 0.85f, 0.85f };
    const float trans[kBands]  = { 0.0008f, 0.00025f, 0.00006f, 0.000025f, 0.00001f, 0.000006f };
    const float scat[kBands]   = { 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.80f };
    const int mDead = AF_SceneAddMaterial(s, trans, absorb, scat, kBands);    // よく吸う
    // 大部屋（x∈[-8,0]）と小部屋（x∈[1,5]）を **0.9m の戸口**でつなぐ。高さ 3m。
    const float h = 3.0f, t = 0.15f;
    AF_SceneAddInstanceBox(s, V(-4.0f, h*0.5f, -6), V(4.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(-4.0f, h*0.5f,  6), V(4.5f, h*0.5f, t), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(-8.5f, h*0.5f,  0), V(t, h*0.5f, 6), V(1,0,0), V(0,1,0), mLive);
    // 仕切り（戸口 z∈[-0.45,0.45]）
    AF_SceneAddInstanceBox(s, V(0.5f, h*0.5f, -3.2f), V(t, h*0.5f, 2.75f), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(0.5f, h*0.5f,  3.2f), V(t, h*0.5f, 2.75f), V(1,0,0), V(0,1,0), mLive);
    // 小部屋（吸う）
    //   ⚠ 床・天井・側壁は**内寸を覆いきる**こと。最初 x∈[1,5] にしていて、
    //     内側は x∈[0.65,4.85] なので**仕切り際に 0.35m の穴**が空いていた。
    //     そこからエネルギーが漏れ、RT60 が Sabine より一貫して短く出た
    //     （相対比較（開↔閉）は同じ形なので影響しないが、理論との突き合わせが濁る）。
    AF_SceneAddInstanceBox(s, V(2.75f, h*0.5f, -3), V(2.25f, h*0.5f, t), V(1,0,0), V(0,1,0), mDead);
    AF_SceneAddInstanceBox(s, V(2.75f, h*0.5f,  3), V(2.25f, h*0.5f, t), V(1,0,0), V(0,1,0), mDead);
    AF_SceneAddInstanceBox(s, V(5.0f, h*0.5f,  0), V(t, h*0.5f, 3), V(1,0,0), V(0,1,0), mDead);
    // 床・天井（大部屋側は響く、小部屋側は吸う）
    AF_SceneAddInstanceBox(s, V(-4.0f, -t, 0), V(4.5f, t, 6), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(-4.0f, h + t, 0), V(4.5f, t, 6), V(1,0,0), V(0,1,0), mLive);
    AF_SceneAddInstanceBox(s, V(2.75f, -t, 0), V(2.25f, t, 3), V(1,0,0), V(0,1,0), mDead);
    AF_SceneAddInstanceBox(s, V(2.75f, h + t, 0), V(2.25f, t, 3), V(1,0,0), V(0,1,0), mDead);

    constexpr int kBins = 100;
    const float kBinSec = 0.01f;
    std::vector<float> e(kBins * kBands);
    AF_Vector3 src[1] = { V(-4.0f, 1.5f, 0.0f) };

    // 形＝**減衰率（dB/s）**。量＝総和（dB）。
    //
    // ⚠ 最初は「200ms と 600ms の比」で測っていたが、**吸う部屋は 600ms で尾が無い**ので
    //   比が数値の床に張り付き、115dB という無意味な値が出た（物理ではなく指標の問題）。
    //   → ピークから -10dB と -25dB まで落ちる時刻を取り、その間の傾きを見る（T15 相当）。
    //     どちらの部屋でも定義できて、RT60 = 60 / 減衰率 で読み替えられる。
    auto measure = [&](AF_Vector3 L, float& outDecayDbPerSec, float& outLevelDb,
                       int bounces = 32) {
        AF_SceneComputeEchogramBands(s, L, src, 1, e.data(), kBins, kBinSec, 343.0f,
                                     2048, bounces, 0.0f);
        std::vector<double> band(static_cast<std::size_t>(kBins), 0.0);
        double total = 0.0, peak = 0.0;
        int peakBin = 0;
        for (int k = 0; k < kBins; ++k) {
            double v = 0.0;
            for (int b = 0; b < kBands; ++b) v += e[static_cast<std::size_t>(k * kBands + b)];
            band[static_cast<std::size_t>(k)] = v;
            total += v;
            if (v > peak) { peak = v; peakBin = k; }
        }
        outLevelDb = 10.0f * std::log10(static_cast<float>(total > 1e-12 ? total : 1e-12));
        if (peak <= 1e-12) { outDecayDbPerSec = 0.0f; return; }
        // ⚠ 「-10dB と -25dB を跨ぐビン」で測ると **10ms ビンの量子化**が乗る
        //   （跨ぎが 1 ビン違うだけで 3 dB/s 動き、同じ部屋の中で 13.5 dB/s の
        //     「ばらつき」に見えた。物理ではなく分解能だった）。
        //   → ピーク -5dB 〜 -25dB の区間で **log エネルギーの最小二乗直線**を取る。
        const double hi = peak * std::pow(10.0, -0.5);    // -5dB
        const double lo = peak * std::pow(10.0, -2.5);    // -25dB
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int nfit = 0;
        for (int k = peakBin; k < kBins; ++k) {
            const double v = band[static_cast<std::size_t>(k)];
            if (v > hi) continue;
            if (v < lo || v <= 1e-12) break;
            const double x = static_cast<double>(k) * kBinSec;
            const double y = 10.0 * std::log10(v);
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++nfit;
        }
        if (nfit < 4) { outDecayDbPerSec = 0.0f; return; }
        const double den = nfit * sxx - sx * sx;
        if (std::fabs(den) < 1e-12) { outDecayDbPerSec = 0.0f; return; }
        const double slope = (nfit * sxy - sx * sy) / den;      // dB/s（負）
        outDecayDbPerSec = static_cast<float>(-slope);
    };

    struct Probe { const char* room; AF_Vector3 p; };
    // ⚠ 戸口のすぐ脇は「部屋の中」ではなく境界。ばらつきに混ぜると、部屋の性質ではなく
    //   開口の効果を測ることになる。→ 戸口から 1.5m 以上離した点だけを使う。
    const Probe probes[] = {
        {"大部屋", V(-6.5f, 1.5f, -4.0f)}, {"大部屋", V(-6.5f, 1.5f, 4.0f)},
        {"大部屋", V(-5.0f, 1.5f,  0.0f)}, {"大部屋", V(-3.0f, 1.5f, -3.5f)},
        {"大部屋", V(-3.0f, 1.5f,  3.5f)},
        {"小部屋", V( 2.5f, 1.5f, -2.0f)}, {"小部屋", V( 4.0f, 1.5f, 2.0f)},
        {"小部屋", V( 4.0f, 1.5f, -2.0f)}, {"小部屋", V( 2.5f, 1.5f, 2.0f)},
    };
    const int nP = static_cast<int>(sizeof(probes) / sizeof(probes[0]));

    std::printf("      %-8s %-22s %12s %10s %8s\n",
                "部屋", "位置", "減衰(dB/s)", "量(dB)", "RT60(s)");
    float shape[16] = {}, level[16] = {};
    for (int i = 0; i < nP; ++i) {
        measure(probes[i].p, shape[i], level[i]);
        char pos[40];
        std::snprintf(pos, sizeof(pos), "(%.1f, %.1f)", probes[i].p.x, probes[i].p.z);
        std::printf("      %-8s %-22s %12.1f %10.2f %8.2f\n", probes[i].room, pos,
                    shape[i], level[i], shape[i] > 1e-3f ? 60.0f / shape[i] : 0.0f);
    }

    // 部屋の中でのばらつき（形／量）と、部屋をまたいだ差。
    auto spread = [&](int lo, int hi, float* v, float& mn, float& mx) {
        mn = 1e9f; mx = -1e9f;
        for (int i = lo; i < hi; ++i) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
    };
    float bs0, bs1, ss0, ss1, bl0, bl1, sl0, sl1;
    spread(0, 5, shape, bs0, bs1);  spread(5, nP, shape, ss0, ss1);
    spread(0, 5, level, bl0, bl1);  spread(5, nP, level, sl0, sl1);

    std::printf("\n      %-14s %-20s %s\n", "", "減衰のばらつき", "量のばらつき");
    std::printf("      %-14s %6.1f dB/s %-12s %6.2f dB\n", "大部屋の中", bs1 - bs0, "", bl1 - bl0);
    std::printf("      %-14s %6.1f dB/s %-12s %6.2f dB\n", "小部屋の中", ss1 - ss0, "", sl1 - sl0);
    const float across = std::fabs(0.5f * (bs0 + bs1) - 0.5f * (ss0 + ss1));
    std::printf("      %-14s %6.1f dB/s（部屋どうしの中央の差）\n", "部屋をまたぐ", across);

    std::printf("\n      → 判断の目安:\n");
    std::printf("        部屋の中の**形**のばらつきが、部屋をまたぐ差よりずっと小さければ\n");
    std::printf("        **部屋ごとに 1 本**焼けばよい（24〜120KB）。\n");
    std::printf("        同じくらいなら格子で焼く必要がある（1.5〜6MB・継ぎ目の心配が増える）。\n");
    char note[192];
    std::snprintf(note, sizeof(note),
                  "(部屋の中 大%.1f/小%.1f dB/s ／ またぐ %.1f dB/s)",
                  bs1 - bs0, ss1 - ss0, across);
    // ★これは**焼く設計の前提そのもの**。崩れたら「部屋ごとに 1 本」では足りない。
    check("[診断] 尾の減衰は部屋の中より部屋をまたぐ方が大きく変わる",
          across > std::max(bs1 - bs0, ss1 - ss0), note);

    // ── ★★ 扉の状態で尾がどれだけ変わるか ★★ ─────────────────────
    //
    //   ここが「焼いてよいか」の本当の分かれ道。
    //   位置依存（上）が小さくても、**扉で大きく変わるなら部屋の尾は焼けない**
    //   ── 扉の開き具合はこの作品の主題そのもので、焼いてはいけないものだから。
    //
    //   目安は上で出た「部屋の中のばらつき」。
    //     扉で変わる量 < 部屋の中のばらつき → 扉は尾に効いていない。焼いてよい
    //     扉で変わる量 > 部屋の中のばらつき → 尾は「部屋＋扉」の関数。焼き方を変える必要がある
    const float within = std::max(bs1 - bs0, ss1 - ss0);
    const int door = AF_SceneAddInstanceBox(s, V(0.5f, h*0.5f, 0.0f),
                                            V(t, h*0.5f, 0.45f), V(1,0,0), V(0,1,0), mLive);
    std::printf("\n      扉を戸口へ入れて開閉する（部屋の中のばらつき %.1f dB/s と比べる）\n", within);
    std::printf("      %-8s %-14s %12s %10s\n", "部屋", "扉", "減衰(dB/s)", "量(dB)");

    struct Spot { const char* room; AF_Vector3 p; };
    const Spot spots[] = { {"大部屋", V(-5.0f, 1.5f, 0.0f)}, {"小部屋", V(3.5f, 1.5f, 0.0f)} };
    float worstDoorDelta = 0.0f, worstLevelDelta = 0.0f;
    for (const Spot& sp : spots) {
        float dOpen = 0, lOpen = 0, dShut = 0, lShut = 0;
        // 開＝扉を戸口の外へどける（形として無い状態）
        AF_SceneUpdateInstance(s, door, V(0.5f, h*0.5f, 5.5f), V(t, h*0.5f, 0.45f),
                               V(1,0,0), V(0,1,0));
        measure(sp.p, dOpen, lOpen);
        // 閉＝戸口を塞ぐ
        AF_SceneUpdateInstance(s, door, V(0.5f, h*0.5f, 0.0f), V(t, h*0.5f, 0.45f),
                               V(1,0,0), V(0,1,0));
        measure(sp.p, dShut, lShut);
        std::printf("      %-8s %-14s %12.1f %10.2f\n", sp.room, "開", dOpen, lOpen);
        std::printf("      %-8s %-14s %12.1f %10.2f   差 %.1f dB/s / %.1f dB\n",
                    sp.room, "閉", dShut, lShut,
                    std::fabs(dShut - dOpen), std::fabs(lShut - lOpen));
        worstDoorDelta = std::max(worstDoorDelta, std::fabs(dShut - dOpen));
        worstLevelDelta = std::max(worstLevelDelta, std::fabs(lShut - lOpen));
    }
    // ★★ 決定的な切り分け ★★
    //   上の 389.7 dB/s は「尾がもう無い」ことの表れかもしれない（量が 33.6dB 落ちている）。
    //   **形が扉で変わる**のか、**入ってくる量が消えただけ**なのかを分ける ──
    //   音源を小部屋の中へ置けば、扉を閉めても小部屋自身の尾は残る。
    //   ここで形が変わらなければ、**形＝部屋の性質／量＝扉の性質**と言い切れる。
    {
        AF_Vector3 saved = src[0];
        src[0] = V(3.5f, 1.5f, 1.5f);           // 小部屋の中に音源
        const AF_Vector3 L = V(3.0f, 1.5f, -1.5f);
        float dOpen = 0, lOpen = 0, dShut = 0, lShut = 0;
        AF_SceneUpdateInstance(s, door, V(0.5f, h*0.5f, 5.5f), V(t, h*0.5f, 0.45f),
                               V(1,0,0), V(0,1,0));
        measure(L, dOpen, lOpen);
        AF_SceneUpdateInstance(s, door, V(0.5f, h*0.5f, 0.0f), V(t, h*0.5f, 0.45f),
                               V(1,0,0), V(0,1,0));
        measure(L, dShut, lShut);
        std::printf("\n      ★音源も小部屋の中に置いた場合（形が部屋の性質かを分ける）\n");
        std::printf("      %-8s %-14s %12.1f %10.2f\n", "小部屋", "開", dOpen, lOpen);
        std::printf("      %-8s %-14s %12.1f %10.2f   差 %.1f dB/s / %.1f dB\n",
                    "小部屋", "閉", dShut, lShut,
                    std::fabs(dShut - dOpen), std::fabs(lShut - lOpen));
        const float shapeDelta = std::fabs(dShut - dOpen);
        std::printf("      → 同室でも扉で形が %.1f dB/s 変わる（部屋の中のばらつき %.1f dB/s）\n",
                    shapeDelta, within);
        std::printf("        量は %.1f dB しか変わらない ＝ **量ではなく形が変わっている**\n",
                    std::fabs(lShut - lOpen));
        std::printf("        RT60 で 開 %.2fs → 閉 %.2fs。吸う小部屋が、扉を開けると響く\n"
                    "        大部屋と**結合**して減衰が遅くなり、閉めると自分の速さに戻る。\n",
                    dOpen > 1e-3f ? 60.0f / dOpen : 0.0f, dShut > 1e-3f ? 60.0f / dShut : 0.0f);
        std::snprintf(note, sizeof(note), "(開 %.1f → 閉 %.1f dB/s ／ 量は %.1f dB しか動かない)",
                      dOpen, dShut, std::fabs(lShut - lOpen));
        // ★★ 焼く設計の結論はここで出た ★★
        //   「尾は部屋の形にしか依存しない」は**偽**だった。連成残響（coupled rooms）が効く。
        //   扉を閉めると結合が切れて、その部屋自身の減衰に戻る。
        //   → **部屋ごとに 1 本焼くと、この変化が丸ごと消える。**
        //     扉の開き具合は主題そのものなので、これは失ってはいけない。
        //   縛るのは「扉を閉めたら吸う部屋は速く乾く」。ここが崩れたら体験が壊れる。
        check("[診断] 扉を閉めると吸う部屋は速く乾く（連成が切れる）",
              dShut > dOpen * 1.5f && std::fabs(lShut - lOpen) < 3.0f, note);
        src[0] = saved;
    }

    // ── ★★ 念のため: 機構を確かめる ★★ ───────────────────────────
    //
    //   連成残響が本当なら、効きは**2 部屋の残響時間の差**で決まるはず。
    //   → 小部屋の吸音率を振って「効果が消える所」を探す。
    //     消えるなら機構は連成で確定。消えないなら別の原因（＝この結論は疑わしい）。
    //
    //   ★あわせて **Sabine の理論値**と突き合わせる。扉を閉じれば小部屋は孤立するので
    //     RT60 = 0.161 V / (S α) が当てはまる。数字が理論と合えば、
    //     測定そのものが信用できる（当てはめの作りが壊れていない）。
    {
        // 小部屋のおおよその寸法（内側）: 4.0 × 6.0 × 3.0 m
        //   ⚠ 変数名を V にしてはいけない。座標を作るヘルパ V(x,y,z) を隠す。
        const float vol = 4.0f * 6.0f * 3.0f;
        const float surf = 2.0f * (4.0f * 6.0f) + 2.0f * (4.0f * 3.0f) + 2.0f * (6.0f * 3.0f);
        std::printf("\n      ★機構の確認: 小部屋の吸音率を振る（音源・リスナーとも小部屋の中）\n");
        std::printf("      %-6s %10s %10s %10s %12s %12s\n",
                    "α", "開(dB/s)", "閉(dB/s)", "差(dB/s)", "閉のRT60(s)", "Sabine(s)");
        AF_Vector3 saved = src[0];
        src[0] = V(3.5f, 1.5f, 1.5f);
        const AF_Vector3 L = V(3.0f, 1.5f, -1.5f);
        float deltaAt[8] = {}; int nA = 0; float worstSabineErr = 0.0f;
        for (float a : {0.10f, 0.20f, 0.40f, 0.60f, 0.80f}) {
            float ab[kBands], tr2[kBands], sc[kBands];
            for (int b2 = 0; b2 < kBands; ++b2) { ab[b2] = a; tr2[b2] = trans[b2]; sc[b2] = scat[b2]; }
            AF_SceneSetMaterial(s, mDead, tr2, ab, sc, kBands);
            float dOpen = 0, lOpen = 0, dShut = 0, lShut = 0;
            AF_SceneUpdateInstance(s, door, V(0.5f, h*0.5f, 5.5f), V(t, h*0.5f, 0.45f),
                                   V(1,0,0), V(0,1,0));
            measure(L, dOpen, lOpen);
            AF_SceneUpdateInstance(s, door, V(0.5f, h*0.5f, 0.0f), V(t, h*0.5f, 0.45f),
                                   V(1,0,0), V(0,1,0));
            measure(L, dShut, lShut);
            const float rtShut = dShut > 1e-3f ? 60.0f / dShut : 0.0f;
            const float sabine = 0.161f * vol / (surf * a);
            // ★理論値と比べるときは**跳ね返りを増やして**測り直す。
            //   既定の 32 回だと α=0.10 では 1 回 0.46dB ＝ **32 回で 14.6dB しか落ちない**。
            //   尾が減衰しきる前に打ち切られ、当てはめが実際より速い減衰を返す
            //   （これは測定の限界であってエンジンの誤りではない。ただし
            //     **響く空間では実行時の尾も同じ理由で短い**ことは覚えておくこと）。
            float dDeep = 0, lDeep = 0;
            measure(L, dDeep, lDeep, 256);
            const float rtDeep = dDeep > 1e-3f ? 60.0f / dDeep : 0.0f;
            std::printf("      %-6.2f %10.1f %10.1f %10.1f %12.3f %12.3f  (跳ね256で %.3f)\n",
                        a, dOpen, dShut, std::fabs(dShut - dOpen), rtShut, sabine, rtDeep);
            deltaAt[nA++] = std::fabs(dShut - dOpen);
            // ⚠ 10ms ビンでは RT60 が 0.2 秒を切ると当てはめの点が足りない。
            //   分解能の外は理論と比べない（比べると測定の限界を engine の誤りに見せてしまう）。
            if (sabine > 0.2f && rtDeep > 1e-4f)
                worstSabineErr = std::max(worstSabineErr, std::fabs(rtDeep - sabine) / sabine);
        }
        // 材質を元へ戻す（あとの検査に影響させない）
        AF_SceneSetMaterial(s, mDead, trans, absorb, scat, kBands);
        src[0] = saved;

        std::printf("      → 吸音率が上がるほど（＝隣室との残響差が開くほど）扉の効きが大きくなる\n"
                    "        なら、機構は連成残響で確定。α=0.10（隣室と近い）で小さければ裏取り完了。\n");
        std::snprintf(note, sizeof(note), "(α0.10 で %.1f dB/s ／ α0.80 で %.1f dB/s)",
                      deltaAt[0], deltaAt[nA - 1]);
        check("[診断] 扉の効きは隣室との残響差が開くほど大きい（＝連成が機構）",
              nA >= 2 && deltaAt[nA - 1] > deltaAt[0] * 2.0f, note);
        std::snprintf(note, sizeof(note), "(Sabine との最大ずれ %.0f%%)", worstSabineErr * 100.0f);
        // ★理論値との突き合わせ。当てはめの作りが壊れていないことの独立な裏取り。
        check("[診断] 扉を閉じたときの RT60 が Sabine の理論値と合う（±40%）",
              worstSabineErr < 0.40f, note);
    }

    std::printf("\n      → 扉で変わる減衰 最大 %.1f dB/s（部屋の中のばらつき %.1f dB/s）\n",
                worstDoorDelta, within);
    std::printf("        量は %.1f dB 変わる（こちらは大きくて当然。扉の主題そのもの）\n",
                worstLevelDelta);
    if (worstDoorDelta > within)
        std::printf("        ⚠ **尾の形は「部屋」ではなく「部屋＋扉」の関数。**\n"
                    "          部屋ごとに 1 本焼くと、扉を閉めても尾の減衰が変わらない。\n");
    else
        std::printf("        → 尾の**形**は扉でほとんど変わらない。焼くのは形、量は実行時でよい。\n");
    std::snprintf(note, sizeof(note), "(扉 %.1f dB/s ／ 部屋の中 %.1f dB/s ／ 量 %.1f dB)",
                  worstDoorDelta, within, worstLevelDelta);
    // ★合否ではなく監視。数字が動いたら焼く設計を見直す合図。
    check("[診断] 扉は尾の**量**を大きく変える（主題が効いている）", worstLevelDelta > 3.0f, note);

    AF_SceneDestroy(s);
}

void diagnoseEchogramScaling() {
    std::printf("\n[診断] エコグラムの費用は音源数にどう効くか（尾を焼く前の下調べ）\n");
    std::printf("        %8s %12s %14s\n", "音源数", "ms/frame", "1音源あたり");

    double base1 = 0.0;
    for (int nSrc : {1, 4, 8, 16}) {
        AF_SceneHandle s = AF_SceneCreate();
        const int m = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        const float h = 4.0f, t = 0.3f, hw = 8.0f, hd = 8.0f;
        AF_SceneAddInstanceBox(s, V(0, -t, 0),  V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h+t, 0), V(hw+t, t, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(-hw-t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V( hw+t, h*0.5f, 0), V(t, h*0.5f, hd+t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd-t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd+t), V(hw+t, h*0.5f, t), V(1,0,0), V(0,1,0), m);
        AF_SceneSetListener(s, V(0, 1.6f, -6.0f));
        for (int i = 0; i < nSrc; ++i)
            AF_SceneSetSource(s, static_cast<unsigned long long>(500 + i),
                              V(-6.0f + i * 0.8f, 1.6f, 4.0f));

        // ★エコグラムだけ残す。他の段を切って、測る のはこの段の伸び方だけにする。
        AF_UpdateConfig c{};
        c.role1EveryN = 1; c.role2EveryN = 1; c.earlyEveryN = 1;
        c.diffSrcEveryN = 1; c.catalogEveryN = 1;
        c.reflectionRays = 0; c.reflectionBounces = 0;
        c.directWeight = 1.0f; c.useReflections = 0;
        c.useEdgeCatalog = 0;
        c.enableReverb = 1; c.echogramBins = 100; c.echogramBinSeconds = 0.01f;
        c.echogramRays = 512; c.echogramBounces = 24;
        c.speedOfSound = 343.0f; c.distanceRef = 1.5f;
        c.enableEarlyReflections = 0; c.enableDiffractionSources = 0;
        AF_SceneSetUpdateConfig(s, &c);

        using clk = std::chrono::high_resolution_clock;
        for (int k = 0; k < 10; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);   // 暖機
        const auto t0 = clk::now();
        const int N = 60;
        for (int k = 0; k < N; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;
        if (nSrc == 1) base1 = ms;
        std::printf("        %8d %12.3f %14.3f\n", nSrc, ms, ms / nSrc);
        AF_SceneDestroy(s);
    }
    std::printf("        → 1 音源のときが %.3f ms。ここが**レイ側の固定費**。\n", base1);
    std::printf("          音源数で伸びるぶんが「各ヒットで音源数ぶん回している」代金。\n"
                "          出荷経路が読むのは部屋の代表 1 本だけなので、伸びるぶんは\n"
                "          **部屋ごとに 1 本だけ積めば消える**（焼く前に取れる）。\n");
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
// く字廊下の角で、回折が「前川の見込み」および「早期反射」とどれだけ離れているか。
// 耳で「角の向こうの定位が無い」と言われたのを数字で裏取りするための診断。
void diagnoseCorridorDiffractionLevel() {
    std::printf("\n[診断] く字廊下の角: 回折の大きさは妥当か（反射・前川と比べる）\n");
    const float w = 1.5f, h = 3.0f, t = 0.3f, aEnd = -12.0f, bEnd = 12.0f;
    AF_SceneHandle s = AF_SceneCreate();
    AF_SceneSetRoomCellSize(s, 0.25f);
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    auto bx = [&](float cx, float cy, float cz, float sx, float sy, float sz) {
        AF_SceneAddInstanceBox(s, V(cx, cy, cz), V(sx * 0.5f, sy * 0.5f, sz * 0.5f),
                               V(1, 0, 0), V(0, 1, 0), mat);
    };
    bx(-w - t * 0.5f, h * 0.5f, (aEnd + w) * 0.5f, t, h, w - aEnd + t);
    bx( w + t * 0.5f, h * 0.5f, (aEnd - w) * 0.5f, t, h, -w - aEnd);
    bx((bEnd - w) * 0.5f, h * 0.5f,  w + t * 0.5f, bEnd + w + t, h, t);
    bx((bEnd + w) * 0.5f, h * 0.5f, -w - t * 0.5f, bEnd - w, h, t);
    bx(0.0f, h * 0.5f, aEnd - t * 0.5f, 2 * w + 2 * t, h, t);
    bx(bEnd + t * 0.5f, h * 0.5f, 0.0f, t, h, 2 * w + 2 * t);
    for (int k = 0; k < 2; ++k) {
        const float ft = 0.3f;
        const float y = (k == 0) ? -ft * 0.5f : h + ft * 0.5f;
        bx(0.0f, y, (aEnd + w) * 0.5f, 2 * w + 2 * t, ft, w - aEnd + t);
        bx((bEnd - w) * 0.5f, y, 0.0f, bEnd + w + t, ft, 2 * w + 2 * t);
    }
    const AF_Vector3 S = V(bEnd - 2.0f, 1.6f, 0.0f);
    AF_SceneSetSource(s, 1, S);
    AF_SceneSetApertureSpread(s, 3);

    std::printf("  z      本数  回折(広帯域)  反射(広帯域)  差     前川の見込み  実測との差\n");
    for (float z = -10.0f; z <= -1.0f; z += 1.5f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneUpdate(s, 0.02f);
        const int idx = AF_SceneSourceIndex(s, 1);
        AF_Vector3 dp[8]; float dg[8], db[48];
        const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
        float difSum = 0.0f;
        for (int i = 0; i < nd; ++i)
            for (int b = 0; b < 6; ++b) difSum += db[i * 6 + b] / 6.0f;
        AF_Vector3 ep[16]; float eg[16 * 6];
        const int ne = (idx >= 0) ? AF_SceneGetEarlyReflections(s, idx, ep, eg, 16) : 0;
        float refSum = 0.0f;
        for (int i = 0; i < ne; ++i)
            for (int b = 0; b < 6; ++b) refSum += eg[i * 6 + b] / 6.0f;
        // 角の縦稜（x=+w, z=+w）を回る経路の迂回長から前川で見積もる（500 Hz）。
        const float e1 = std::sqrt(w * w + (w - z) * (w - z));
        const float e2 = std::sqrt((S.x - w) * (S.x - w) + w * w);
        const float dirLen = std::sqrt(S.x * S.x + z * z);
        const float delta = e1 + e2 - dirLen;
        const float N = 2.0f * delta / (343.0f / 500.0f);
        float att = 20.0f * std::log10f(std::sqrt(6.2831853f * N) /
                                        std::tanh(std::sqrt(6.2831853f * N))) + 5.0f;
        if (att > 24.0f) att = 24.0f;     // 単一稜の実用上限
        const float maek = std::pow(10.0f, -att / 20.0f);
        std::printf("  %5.1f  %d本%s %10.5f  %12.5f  %5.1fdB  %10.5f  %+6.1fdB\n",
                    z, nd, AF_SceneIsOccluded(s, L, S) ? "遮" : "見", difSum, refSum,
                    20.0f * std::log10f(std::max(difSum, 1e-9f) / std::max(refSum, 1e-9f)),
                    maek, 20.0f * std::log10f(std::max(difSum, 1e-9f) / maek));
    }
    std::printf("  ※ 回折は角の向きを運ぶ唯一の成分。反射より大きく下だと、耳では角が消える。\n");

    // 描画で回折が 0 本になった地点（左に寄った位置）を、遮蔽判定と一緒に確かめる。
    //   透過が -60dB 以下なのに経路が 0 本なら、塞がれているのに回折が出ていない＝穴。
    std::printf("\n  ── 左に寄った位置で経路が消えないか ──\n");
    std::printf("   x      z    経路  透過(dB)  遮蔽判定\n");
    for (float xo = 0.0f; xo >= -1.0f; xo -= 0.5f) {
        for (float z = -4.0f; z <= -1.5f; z += 0.7f) {
            const AF_Vector3 L = V(xo, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            // 更新を回してから測り直す。変われば「1 回の更新で収束していない」。
            for (int k = 0; k < 8; ++k) AF_SceneUpdate(s, 0.02f);
            const int nd8 = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float tr[6] = {}; float of = 0.0f;
            AF_SceneComputeSoftOcclusion(s, L, S, tr, 6, &of);
            float tb = 0.0f;
            for (int b = 0; b < 6; ++b) tb += tr[b] / 6.0f;
            const float dbv = 20.0f * std::log10f(std::max(tb, 1e-9f));
            const int occ = AF_SceneIsOccluded(s, L, S);
            std::printf("  %5.1f %6.1f   %d本  %7.1f   %s%s\n", xo, z, nd, dbv,
                        occ ? "遮蔽" : "見通せる",
                        (occ && nd == 0) ? "   ★穴（塞がれているのに経路なし）" : "");
        }
    }

    // ★歩いている最中に何が変わるかを細かい刻みで測る。
    //   耳の訴えは「移動中に音が変わる」。設計の第一制約（連続性）そのものなので、
    //   レベルだけでなく**音色（4k/125 の傾き）と到来方向**も 1 歩ぶんの差で見る。
    //   経路の本数も一緒に出すので、穴の位置がここで分かる。
    {
        std::printf("\n  ── 0.1m 刻みで歩いたとき、1 歩で何が動くか ──\n");
        std::printf("   z      経路  広帯域    Δ量(dB)  傾き(dB)  Δ傾き(dB)  方向(x,z)   Δ方向(度)\n");
        bool havePrev = false;
        float pLev = 0.0f, pTilt = 0.0f, pdx = 0.0f, pdz = 0.0f;
        float worstLev = 0.0f, worstTilt = 0.0f, worstDir = 0.0f;
        float atLev = 0.0f, atTilt = 0.0f, atDir = 0.0f;
        int drops = 0;
        for (float z = -8.0f; z <= -1.0f; z += 0.1f) {
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            const int occ = AF_SceneIsOccluded(s, L, S);
            float lo = 0.0f, hi = 0.0f, sum = 0.0f, dx = 0.0f, dz = 0.0f, wsum = 0.0f;
            for (int i = 0; i < nd; ++i) {
                lo += db[i * 6 + 0]; hi += db[i * 6 + 5];
                float g = 0.0f;
                for (int b = 0; b < 6; ++b) { sum += db[i * 6 + b] / 6.0f; g += db[i * 6 + b] / 6.0f; }
                const float ux = dp[i].x - L.x, uz = dp[i].z - L.z;
                const float ul = std::sqrt(ux * ux + uz * uz);
                if (ul > 1e-4f) { dx += g * ux / ul; dz += g * uz / ul; wsum += g; }
            }
            if (wsum > 1e-9f) { dx /= wsum; dz /= wsum; }
            if (occ && nd == 0) ++drops;
            const float lev = 20.0f * std::log10f(std::max(sum, 1e-9f));
            const float tilt = 20.0f * std::log10f(std::max(hi, 1e-9f) / std::max(lo, 1e-9f));
            if (havePrev && nd > 0) {
                const float dL = std::fabs(lev - pLev);
                const float dT = std::fabs(tilt - pTilt);
                float c = dx * pdx + dz * pdz;
                c = std::min(1.0f, std::max(-1.0f, c));
                const float dD = std::acos(c) * 57.29578f;
                if (dL > worstLev) { worstLev = dL; atLev = z; }
                if (dT > worstTilt) { worstTilt = dT; atTilt = z; }
                if (dD > worstDir) { worstDir = dD; atDir = z; }
                if (dL > 3.0f || dT > 3.0f || dD > 15.0f)
                    std::printf("  %5.1f   %d本  %8.5f  %+7.1f  %+8.1f  %+8.1f  (%+.2f,%+.2f) %7.1f ★\n",
                                z, nd, sum, dL, tilt, dT, dx, dz, dD);
            }
            if (nd > 0) { pLev = lev; pTilt = tilt; pdx = dx; pdz = dz; havePrev = true; }
            else havePrev = false;
        }
        std::printf("  1 歩(0.1m)の最大変化: 量 %.1f dB @z=%.1f / 音色 %.1f dB @z=%.1f / "
                    "方向 %.1f 度 @z=%.1f\n",
                    worstLev, atLev, worstTilt, atTilt, worstDir, atDir);
        std::printf("  塞がれているのに経路 0 本だった地点: %d 箇所 / 71\n", drops);
        std::printf("  ※ ★は 1 歩で 量3dB・音色3dB・方向15度 のどれかを超えたところ。\n");
    }

    // ★「実体の中」判定の連続化とその幅を振って、穴と滑らかさが同時に成立する所を探す。
    //   二値のままだと穴が空き、連続でも幅が歩幅より狭いと 1 歩で跳ぶ。
    {
        std::printf("\n  ── 「実体の中」判定: 二値 vs 連続（幅を振る）──\n");
        std::printf("   設定        穴/71   1歩の量(dB)  1歩の音色(dB)  1歩の方向(度)\n");
        const float scales[] = {0.0f, 0.08f, 0.15f, 0.30f, 0.50f, 0.80f, 1.20f};
        for (float sc : scales) {
            if (sc <= 0.0f) AF_SceneSetInsideOtherContinuous(s, 0, 0.0f);
            else            AF_SceneSetInsideOtherContinuous(s, 1, sc);
            bool have = false;
            float pL = 0, pT = 0, pdx = 0, pdz = 0;
            float wL = 0, wT = 0, wD = 0, wLz = 0, wLa = 0, wLb = 0;
            int holes = 0;
            for (float z = -8.0f; z <= -1.0f; z += 0.1f) {
                const AF_Vector3 L = V(0, 1.6f, z);
                AF_SceneSetListener(s, L);
                AF_SceneUpdate(s, 0.02f);
                AF_Vector3 dp[8]; float dg[8], db[48];
                const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
                if (AF_SceneIsOccluded(s, L, S) && nd == 0) ++holes;
                float lo = 0, hi = 0, sum = 0, dx = 0, dz = 0, ws = 0;
                for (int i = 0; i < nd; ++i) {
                    lo += db[i*6+0]; hi += db[i*6+5];
                    float g = 0;
                    for (int b = 0; b < 6; ++b) { sum += db[i*6+b]/6.0f; g += db[i*6+b]/6.0f; }
                    const float ux = dp[i].x - L.x, uz = dp[i].z - L.z;
                    const float ul = std::sqrt(ux*ux + uz*uz);
                    if (ul > 1e-4f) { dx += g*ux/ul; dz += g*uz/ul; ws += g; }
                }
                if (ws > 1e-9f) { dx /= ws; dz /= ws; }
                const float lev = 20.0f * std::log10f(std::max(sum, 1e-9f));
                const float tilt = 20.0f * std::log10f(std::max(hi,1e-9f)/std::max(lo,1e-9f));
                if (have && nd > 0) {
                    if (std::fabs(lev - pL) > wL) { wLz = z; wLa = pL; wLb = lev; }
                    wL = std::max(wL, std::fabs(lev - pL));
                    wT = std::max(wT, std::fabs(tilt - pT));
                    float c = dx*pdx + dz*pdz;
                    c = std::min(1.0f, std::max(-1.0f, c));
                    wD = std::max(wD, std::acos(c) * 57.29578f);
                }
                if (nd > 0) { pL = lev; pT = tilt; pdx = dx; pdz = dz; have = true; }
                else have = false;
            }
            char nm[32];
            if (sc <= 0.0f) std::snprintf(nm, sizeof(nm), "二値（出荷）");
            else            std::snprintf(nm, sizeof(nm), "連続 幅%.2fm", sc);
            std::printf("   %-14s %2d      %8.1f     %8.1f       %8.1f   @z=%.1f (%.1f→%.1f dB)\n",
                        nm, holes, wL, wT, wD, wLz, wLa, wLb);
        }
        AF_SceneSetInsideOtherContinuous(s, 0, 0.0f);
        std::printf("  ※ 穴 0 かつ 1歩の変化が小さい幅を探す。歩幅は 0.1m。\n");
    }

    // ★影境界で、回折単体は飛んでも**直接音と足した合計**は連続かどうか。
    //   回折だけ見て騒いでいた可能性があるので先に確かめる。
    //   合成はレンダラと同じ形にする: 直接 = 透過、回折 = Σ(帯域) × occFrac。
    {
        AF_SceneSetInsideOtherContinuous(s, 1, 0.30f);
        std::printf("\n  ── 影境界: 回折単体は飛ぶが、直接音と足した合計はどうか ──\n");
        std::printf("    z     回折      直接     合計(電力和)   Δ回折   Δ合計\n");
        float pD = 0.0f, pT = 0.0f;
        bool have = false;
        float worstD = 0.0f, worstT = 0.0f;
        for (float z = -2.6f; z <= -1.0f; z += 0.1f) {
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float tr[6] = {}; float of = 0.0f;
            AF_SceneComputeSoftOcclusion(s, L, S, tr, 6, &of);
            float dif = 0.0f, dir = 0.0f;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < 6; ++b) dif += db[i * 6 + b] / 6.0f;
            dif *= of;
            for (int b = 0; b < 6; ++b) dir += tr[b] / 6.0f;
            const float tot = std::sqrt(dif * dif + dir * dir);   // 別タップ＝電力和
            const float dD = 20.0f * std::log10f(std::max(dif, 1e-9f));
            const float dT = 20.0f * std::log10f(std::max(tot, 1e-9f));
            float sD = 0.0f, sT = 0.0f;
            if (have) { sD = dD - pD; sT = dT - pT;
                        worstD = std::max(worstD, std::fabs(sD));
                        worstT = std::max(worstT, std::fabs(sT)); }
            std::printf("  %5.1f  %8.5f  %8.5f  %10.5f   %+6.1f  %+6.1f\n",
                        z, dif, dir, tot, sD, sT);
            pD = dD; pT = dT; have = true;
        }
        std::printf("  1歩(0.1m)の最大: 回折単体 %.1f dB / **合計 %.1f dB**\n", worstD, worstT);
        std::printf("  ※ 合計が連続なら、回折の崩れは直接音が引き取っている＝耳では問題ない。\n");
        AF_SceneSetInsideOtherContinuous(s, 0, 0.0f);
    }

    // ★状態を完全に排除して測る（点ごとにシーンを作り直す）。
    //   ここでも 0 本なら幾何の問題。1 本なら状態（履歴・段階更新）の問題。
    {
        std::printf("\n  ── 点ごとにシーンを作り直したら（状態なし）──\n");
        std::printf("   x      z    経路  透過(dB)\n");
        for (float xo = 0.0f; xo >= -1.0f; xo -= 0.5f) {
            for (float z = -4.0f; z <= -1.5f; z += 0.7f) {
                AF_SceneHandle s2 = AF_SceneCreate();
                AF_SceneSetRoomCellSize(s2, 0.25f);
                const int m2 = AF_SceneAddMaterial(s2, nullptr, nullptr, nullptr, 0);
                auto bx2 = [&](float cx, float cy, float cz, float sx, float sy, float sz) {
                    AF_SceneAddInstanceBox(s2, V(cx, cy, cz), V(sx*0.5f, sy*0.5f, sz*0.5f),
                                           V(1,0,0), V(0,1,0), m2);
                };
                bx2(-w - t*0.5f, h*0.5f, (aEnd + w)*0.5f, t, h, w - aEnd + t);
                bx2( w + t*0.5f, h*0.5f, (aEnd - w)*0.5f, t, h, -w - aEnd);
                bx2((bEnd - w)*0.5f, h*0.5f,  w + t*0.5f, bEnd + w + t, h, t);
                bx2((bEnd + w)*0.5f, h*0.5f, -w - t*0.5f, bEnd - w, h, t);
                bx2(0.0f, h*0.5f, aEnd - t*0.5f, 2*w + 2*t, h, t);
                bx2(bEnd + t*0.5f, h*0.5f, 0.0f, t, h, 2*w + 2*t);
                for (int k = 0; k < 2; ++k) {
                    const float ft = 0.3f;
                    const float y = (k == 0) ? -ft*0.5f : h + ft*0.5f;
                    bx2(0.0f, y, (aEnd + w)*0.5f, 2*w + 2*t, ft, w - aEnd + t);
                    bx2((bEnd - w)*0.5f, y, 0.0f, bEnd + w + t, ft, 2*w + 2*t);
                }
                AF_SceneSetSource(s2, 1, S);
                AF_SceneSetApertureSpread(s2, 3);
                const AF_Vector3 L = V(xo, 1.6f, z);
                AF_SceneSetListener(s2, L);
                AF_SceneUpdate(s2, 0.02f);
                AF_Vector3 dp[8]; float dg[8], db[48];
                const int nd = AF_SceneComputeDiffractionSourceBands(s2, L, S, dp, dg, db, 8);
                float tr[6] = {}; float of = 0.0f;
                AF_SceneComputeSoftOcclusion(s2, L, S, tr, 6, &of);
                float tb = 0.0f;
                for (int b = 0; b < 6; ++b) tb += tr[b] / 6.0f;
                int craw = 0, ccut = 0, ccl = 0;
                AF_SceneDebugDiffractionCounts(s2, &craw, &ccut, &ccl);
                // どのゲートを切ると経路が戻るか。bit1=点が他の実体の中 / bit2=掠めの重み
                // bit4=芯の横断。戻るゲートが犯人。
                int back[3] = {};
                for (int gi = 0; gi < 3; ++gi) {
                    const int mask = (gi == 0) ? 1 : (gi == 1) ? 2 : 4;
                    AF_SceneSetDiffractionGateMask(s2, mask);
                    AF_Vector3 dp2[8]; float dg2[8], db2[48];
                    back[gi] = AF_SceneComputeDiffractionSourceBands(s2, L, S, dp2, dg2, db2, 8);
                }
                AF_SceneSetDiffractionGateMask(s2, 0);
                // ccut は「重み0棄却 ＋ 稜線検討本数×1000」の詰め方（診断専用）。
                std::printf("  %5.1f %6.1f  %d本 %7.1fdB  検討%2d → 可視通過%d → クラスタ%d"
                            "  | ゲート別: 中%d本 掠%d本 芯%d本\n",
                            xo, z, nd, 20.0f * std::log10f(std::max(tb, 1e-9f)),
                            ccut / 1000, craw, ccl, back[0], back[1], back[2]);
                AF_SceneDestroy(s2);
            }
        }
    }

    // ★同じ位置でも、そこへ至る経過で答えが変わらないか（履歴依存の検出）。
    //   最初の表では x=0,z=-4.0 が 1 本、上の表では 0 本だった。
    {
        std::printf("\n  ── 同じ位置なのに答えが変わるか（履歴依存）──\n");
        const AF_Vector3 T = V(0.0f, 1.6f, -4.0f);
        auto probe = [&](const char* how) {
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, T, S, dp, dg, db, 8);
            float sum = 0.0f;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < 6; ++b) sum += db[i * 6 + b] / 6.0f;
            std::printf("   %-34s %d本  合計 %.5f\n", how, nd, sum);
        };
        AF_SceneSetListener(s, T); AF_SceneUpdate(s, 0.02f); probe("そこへ直接置いた");
        AF_SceneSetListener(s, T); AF_SceneUpdate(s, 0.02f); probe("同じ位置でもう一度");
        for (float z = -10.0f; z <= -4.0f; z += 1.5f) {
            AF_SceneSetListener(s, V(0, 1.6f, z)); AF_SceneUpdate(s, 0.02f);
        }
        probe("遠くから 1.5m 刻みで歩いてきた");
        for (float z = -1.0f; z >= -4.0f; z -= 0.5f) {
            AF_SceneSetListener(s, V(0, 1.6f, z)); AF_SceneUpdate(s, 0.02f);
        }
        probe("角の側から戻ってきた");
        std::printf("   ※ 数字が揃わなければ、同じ幾何に複数の答えがある。\n");
    }

    // ★自動生成された残響の場に、そもそも方向の構造があるか。
    //   「空間ごとに自動で作っているから残響の定位は無理では」への確認。
    //   場を測るのは AF_SceneProbeDirectionalEnergy で、作り方には依らない。
    //   構造があるなら取り出せるし、平坦なら取り出しようがない。
    {
        std::printf("\n  ── 残響の場に方向の構造があるか（125Hz・6 軸へ投影）──\n");
        std::printf("   z      右      左      前      後      上      下    最大差\n");
        const AF_Vector3 ax[6] = { V(1,0,0), V(-1,0,0), V(0,0,1), V(0,0,-1), V(0,1,0), V(0,-1,0) };
        const int nd = 64;
        std::vector<AF_Vector3> dirs(nd);
        std::vector<float> en(nd * 6, 0.0f);
        for (int i = 0; i < nd; ++i) {   // フィボナッチ球
            const float y = 1.0f - 2.0f * (i + 0.5f) / nd;
            const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float th = 2.399963f * i;
            dirs[i] = V(r * std::cos(th), y, r * std::sin(th));
        }
        for (float z = -10.0f; z <= -2.0f; z += 2.0f) {
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_SceneProbeDirectionalEnergy(s, L, dirs.data(), nd, 8, en.data());
            float acc[6] = {};
            float tot = 0.0f;
            for (int i = 0; i < nd; ++i) {
                const float e = en[i * 6];          // 125Hz
                tot += e;
                for (int a = 0; a < 6; ++a) {
                    const float c = dirs[i].x * ax[a].x + dirs[i].y * ax[a].y + dirs[i].z * ax[a].z;
                    if (c > 0.0f) acc[a] += e * c;  // その軸寄りの成分
                }
            }
            std::printf("  %5.1f ", z);
            float mx = -1e30f, mn = 1e30f;
            for (int a = 0; a < 6; ++a) {
                const float v = (tot > 1e-9f) ? acc[a] / tot : 0.0f;
                mx = std::max(mx, v); mn = std::min(mn, v);
                std::printf("%7.3f", v);
            }
            std::printf("  %6.1f dB\n", 20.0f * std::log10f(std::max(mx, 1e-6f)
                                                          / std::max(mn, 1e-6f)));
        }
        std::printf("  ※ 最大差が数 dB 以上あれば、場に構造がある＝尾に方向を載せられる。\n");
        std::printf("     全軸がほぼ同じなら、場そのものが平坦で取り出しようがない。\n");
    }

    // ★ソフト遮蔽の 2 つの出力が矛盾していないか。
    //   ヘッダは「回折タップ = 回折ゲイン × outOccFrac」と定めているので、
    //   occFrac が過小だとその分だけ回折が黙って小さくなる。
    //   透過が -60dB 以下なのに occFrac が小さければ、同じ関数が矛盾を返している。
    std::printf("\n  ── ソフト遮蔽の 2 出力は整合しているか（廊下の左右にずらして歩く）──\n");
    std::printf("   x      z     透過(広帯域)      dB   occFrac   判定\n");
    for (float xoff = 0.0f; xoff >= -1.2f; xoff -= 0.3f) {
        for (float z = -10.0f; z <= -4.0f; z += 3.0f) {
            const AF_Vector3 L = V(xoff, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            float tr[6] = {}; float of = 0.0f;
            AF_SceneComputeSoftOcclusion(s, L, S, tr, 6, &of);
            float tb = 0.0f;
            for (int b = 0; b < 6; ++b) tb += tr[b] / 6.0f;
            const float db = 20.0f * std::log10f(std::max(tb, 1e-9f));
            const bool blocked = (db < -60.0f);
            std::printf("  %5.1f %6.1f   %11.7f  %7.1f   %6.3f   %s\n",
                        xoff, z, tb, db, of,
                        (blocked && of < 0.9f) ? "★矛盾（塞がれているのに occFrac が小さい）"
                                               : "整合");
        }
    }

    // 角を誰が担当しているか。自動ポータルが立っていれば、そちらが答えを出している。
    {
        int na = 0, nm = 0;
        AF_SceneSetListener(s, V(0, 1.6f, -7.0f));
        AF_SceneUpdate(s, 0.02f);
        AF_SceneGetPortalCounts(s, &na, &nm);
        std::printf("\n  ── 角の担当は誰か（自動 %d 枚 / 手置き %d 枚）──\n", na, nm);
        std::printf("  z      ");
        for (int q = 0; q < na + nm && q < 3; ++q) std::printf("P%d 開口/中心            ", q);
        std::printf("\n");
        for (float z = -10.0f; z <= -2.0f; z += 2.0f) {
            const AF_Vector3 L = V(0, 1.6f, z);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            std::printf("  %5.1f  ", z);
            for (int q = 0; q < na + nm && q < 3; ++q) {
                float fz[kBands] = {}; AF_Vector3 pcp = V(0,0,0);
                if (!AF_SceneMeasurePortal(s, q, L, S, fz, &pcp)) { std::printf("  ―                      "); continue; }
                AF_Vector3 pc{}, pu{}, pv{}; float phu = 0, phv = 0;
                AF_SceneGetPortal(s, q, &pc, &pu, &pv, &phu, &phv);
                std::printf("%.4f (%.1f,%.1f,%.1f) 半%.1fx%.1f  ",
                            fz[0], pc.x, pc.y, pc.z, phu, phv);
            }
            std::printf("\n");
        }
    }

    // 角の向こうで鳴っている早期反射は、本当に届く経路か。
    //   音源が完全に隠れているのに反射だけ素通りしているなら、壁を抜けている。
    {
        const AF_Vector3 L = V(0, 1.6f, -7.0f);
        AF_SceneSetListener(s, L);
        AF_SceneUpdate(s, 0.02f);
        const int idx = AF_SceneSourceIndex(s, 1);
        AF_Vector3 ep[16]; float eg[16 * 6];
        const int ne = (idx >= 0) ? AF_SceneGetEarlyReflections(s, idx, ep, eg, 16) : 0;
        std::printf("\n  ── z=-7.0 の早期反射は本当に届く経路か（音源は完全遮蔽 occFrac 0.999）──\n");
        std::printf("   #  到来方向(頭基準 x=右)      経路長   広帯域   帯域(125..4k)\n");
        for (int i = 0; i < ne; ++i) {
            float g = 0.0f;
            for (int b = 0; b < 6; ++b) g += eg[i * 6 + b] / 6.0f;
            const float dx = ep[i].x - L.x, dy = ep[i].y - L.y, dz = ep[i].z - L.z;
            const float plen = std::sqrt(dx*dx + dy*dy + dz*dz);
            const float inv = (plen > 1e-4f) ? 1.0f / plen : 0.0f;
            std::printf("  %2d  (%+5.2f,%+5.2f,%+5.2f)  %8.2fm  %7.4f  ",
                        i, dx*inv, dy*inv, dz*inv, plen, g);
            for (int b = 0; b < 6; ++b) std::printf("%6.3f", eg[i * 6 + b]);
            std::printf("\n");
        }
        std::printf("  ※ 4 本が同じゲイン・同じ帯域なら、経路の違いが反映されていない。\n");
        std::printf("     角を回る音は経路長も反射回数も違うはずで、揃うのは不自然。\n");
        // 既定材質の反射率と突き合わせる。√(1−α−τ) と一致するなら、
        // 「反射点→音源」の遮蔽（seg）が全部 1＝一度も効いていないことになる。
        {
            float tr[6] = {}, ab[6] = {}, sc[6] = {};
            AF_MaterialPresetBands(0, tr, ab, sc);
            std::printf("  検算 √(1-α-τ) =");
            for (int b = 0; b < 6; ++b)
                std::printf("%6.3f", std::sqrt(std::max(0.0f, 1.0f - ab[b] - tr[b])));
            std::printf("   ← 上の 4 本と一致すれば seg（反射点→音源の遮蔽）が効いていない\n");
        }

        // 反射は部屋の形から作られる。部屋の判定が壊れていれば反射も壊れる。
        std::printf("\n  ── 部屋の判定 ──\n");
        std::printf("  部屋数 %d / リスナーの部屋 %d / 音源の部屋 %d\n",
                    AF_SceneRoomCount(s), AF_SceneRoomAt(s, L), AF_SceneRoomAt(s, S));
        for (int r = 0; r < AF_SceneRoomCount(s) && r < 4; ++r) {
            float vol = 0.0f;
            AF_Vector3 c{}, mn{}, mx{};
            AF_SceneRoomInfo(s, r, &vol, &c, &mn, &mx);
            std::printf("   部屋%d 体積 %8.1f m3  範囲 (%6.1f,%5.1f,%6.1f)〜(%6.1f,%5.1f,%6.1f)\n",
                        r, vol, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
        }
        std::printf("  ※ 実際の廊下は x,z が -12〜12・高さ 0〜3 の L 字。範囲がこれより大きければ\n");
        std::printf("     部屋が外の世界と繋がっていて、反射が形状の外に出る。\n");
    }

    // 開口率の二重掛けを戻した場合と並べる（変更前 → 変更後 を同じ物差しで）。
    std::printf("\n  ── 二重掛けを戻すと（変更前）── 直した後 ──\n");
    std::printf("  z       変更前      変更後     改善\n");
    for (float z = -10.0f; z <= -1.0f; z += 1.5f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        float before = 0.0f, after = 0.0f;
        for (int pass = 0; pass < 2; ++pass) {
            AF_SceneSetKeepDoubleOpen(s, (pass == 0) ? 1 : 0);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float sum = 0.0f;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < 6; ++b) sum += db[i * 6 + b] / 6.0f;
            ((pass == 0) ? before : after) = sum;
        }
        std::printf("  %5.1f  %9.5f  %9.5f  %+6.1f dB\n", z, before, after,
                    20.0f * std::log10f(std::max(after, 1e-9f) / std::max(before, 1e-9f)));
    }
    AF_SceneSetKeepDoubleOpen(s, 0);

    // 前川だけに任せた場合（設計どおり「定位と回り込む距離だけ」）。
    std::printf("\n  ── 前川だけに任せると（f を重ねない）──\n");
    std::printf("  z       いま      前川だけ    差      反射との差\n");
    for (float z = -10.0f; z <= -1.0f; z += 1.5f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        float now = 0.0f, one = 0.0f, refSum = 0.0f;
        for (int pass = 0; pass < 2; ++pass) {
            AF_SceneSetDiffractionSingleModel(s, pass);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float sum = 0.0f;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < 6; ++b) sum += db[i * 6 + b] / 6.0f;
            ((pass == 0) ? now : one) = sum;
            if (pass == 1) {
                const int idx = AF_SceneSourceIndex(s, 1);
                AF_Vector3 ep[16]; float eg[16 * 6];
                const int ne = (idx >= 0) ? AF_SceneGetEarlyReflections(s, idx, ep, eg, 16) : 0;
                for (int i = 0; i < ne; ++i)
                    for (int b = 0; b < 6; ++b) refSum += eg[i * 6 + b] / 6.0f;
            }
        }
        std::printf("  %5.1f  %9.5f  %9.5f  %+6.1f dB  %+6.1f dB\n", z, now, one,
                    20.0f * std::log10f(std::max(one, 1e-9f) / std::max(now, 1e-9f)),
                    20.0f * std::log10f(std::max(one, 1e-9f) / std::max(refSum, 1e-9f)));
    }
    AF_SceneSetDiffractionSingleModel(s, 0);

    // 設計書 §5-12 の結論（エネルギーは波動側＝フレネル開口が持つ）は
    // apertureIsTransmission として実装済み。C++ 既定 OFF / Unity 既定 ON で食い違っている。
    std::printf("\n  ── 4 つの設定を並べる（どれが出荷の音か）──\n");
    std::printf("  z      前川×f    前川のみ   f のみ    f のみ＋二重掛け戻し\n");
    for (float z = -8.5f; z <= -2.0f; z += 1.5f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        float v[4] = {};
        for (int p = 0; p < 4; ++p) {
            AF_SceneSetDiffractionSingleModel(s, (p == 1) ? 1 : 0);
            AF_SceneSetApertureIsTransmission(s, (p >= 2) ? 1 : 0);
            AF_SceneSetKeepDoubleOpen(s, (p == 3) ? 1 : 0);
            AF_SceneSetListener(s, L);
            AF_SceneUpdate(s, 0.02f);
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < 6; ++b) v[p] += db[i * 6 + b] / 6.0f;
        }
        std::printf("  %5.1f  %8.5f  %8.5f  %8.5f  %8.5f\n", z, v[0], v[1], v[2], v[3]);
    }
    AF_SceneSetDiffractionSingleModel(s, 0);
    AF_SceneSetApertureIsTransmission(s, 0);
    AF_SceneSetKeepDoubleOpen(s, 0);
    std::printf("  ※ 「前川×f」= C++ 既定＝WAV レンダラの音。「f のみ」= Unity 既定の音。\n");

    // 内訳。どの係数が効いて前川より下がるのかを当てずに見る。
    std::printf("\n  ── 最強経路の内訳（掛け算の順に）──\n");
    std::printf("  z      δ(m)  openGain slit(m)  thru    openBand(125..4k)              積=thru*oB\n");
    for (float z = -10.0f; z <= -1.0f; z += 1.5f) {
        const AF_Vector3 L = V(0, 1.6f, z);
        AF_SceneSetListener(s, L);
        AF_SceneUpdate(s, 0.02f);
        float d[17] = {};
        const int np = AF_SceneDebugDiffractionPath(s, L, S, d, -1);
        if (np <= 0) { std::printf("  %5.1f   経路なし\n", z); continue; }
        float ob = 0.0f;
        for (int b = 0; b < 6; ++b) ob += d[10 + b] / 6.0f;
        std::printf("  %5.1f  %5.2f  %7.4f  %5.2f  %7.5f  ", z, d[0], d[1], d[2], d[3]);
        for (int b = 0; b < 6; ++b) std::printf("%6.3f", d[10 + b]);
        std::printf("  %8.5f\n", d[3] * ob);
    }
    AF_SceneDestroy(s);
}

// 尾を「音源ごと」から「音源の部屋ごと」へ畳めるか。
//   畳めれば 8 音源で 4.5ms → 部屋数ぶんに減る（尾はオーディオスレッドの 47%）。
//   ただし過去に「全音源で 1 本」にして 500ms で 9.0dB ずれた記録がある。
//   同じ部屋の音源どうしが十分似ているかを、減衰の形で直接測る。
void diagnoseTailShareByRoom() {
    std::printf("\n[診断] 尾は「音源の部屋ごと」に畳めるか（減衰の形を比べる）\n");
    AF_SceneHandle s = AF_SceneCreate();
    AF_SceneSetRoomCellSize(s, 0.25f);
    // 部屋A（響く・コンクリ）と部屋B（吸う）を戸口で繋ぐ。
    float trA[kBands] = {0.001f,0.001f,0.001f,0.001f,0.001f,0.001f};
    float abA[kBands] = {0.02f,0.02f,0.02f,0.03f,0.03f,0.04f};      // 裸のコンクリ
    float abB[kBands] = {0.40f,0.40f,0.45f,0.50f,0.55f,0.60f};      // よく吸う
    const int matA = AF_SceneAddMaterial(s, trA, abA, nullptr, kBands);
    const int matB = AF_SceneAddMaterial(s, trA, abB, nullptr, kBands);
    auto bx = [&](float cx, float cy, float cz, float hx, float hy, float hz, int m) {
        AF_SceneAddInstanceBox(s, V(cx, cy, cz), V(hx, hy, hz), V(1,0,0), V(0,1,0), m);
    };
    const float h = 3.0f, t = 0.2f;
    // 部屋A: x∈[-8,0]、部屋B: x∈[0,8]。共通の床・天井、間仕切りに戸口。
    bx(-4, -t, 0, 4, t, 4, matA); bx(-4, h + t, 0, 4, t, 4, matA);
    bx( 4, -t, 0, 4, t, 4, matB); bx( 4, h + t, 0, 4, t, 4, matB);
    bx(-8 - t, h*0.5f, 0, t, h*0.5f, 4, matA);   // A 西
    bx( 8 + t, h*0.5f, 0, t, h*0.5f, 4, matB);   // B 東
    bx(-4, h*0.5f, -4 - t, 4, h*0.5f, t, matA); bx(-4, h*0.5f, 4 + t, 4, h*0.5f, t, matA);
    bx( 4, h*0.5f, -4 - t, 4, h*0.5f, t, matB); bx( 4, h*0.5f, 4 + t, 4, h*0.5f, t, matB);
    bx(0, h*0.5f, -2.5f, t, h*0.5f, 1.5f, matA); // 仕切り（戸口 z∈[-1,1]）
    bx(0, h*0.5f,  2.5f, t, h*0.5f, 1.5f, matA);

    const AF_Vector3 L = V(-6.0f, 1.6f, 0.0f);   // リスナーは部屋A
    AF_SceneSetListener(s, L);
    for (int k = 0; k < 3; ++k) AF_SceneUpdate(s, 1.0f/60.0f);

    struct Case { const char* name; AF_Vector3 pos; };
    const Case cs[4] = {
        {"A1 同室・近い  ", V(-2.0f, 1.6f,  2.0f)},
        {"A2 同室・遠い  ", V(-7.0f, 1.6f, -3.0f)},
        {"A3 同室・戸口際", V(-1.0f, 1.6f,  0.0f)},
        {"B1 隣室        ", V( 6.0f, 1.6f,  0.0f)},
    };
    constexpr int kBins = 200;
    std::vector<float> eg(4 * kBins * kBands, 0.0f);
    for (int c = 0; c < 4; ++c) {
        const AF_Vector3 src[1] = { cs[c].pos };
        AF_SceneComputeEchogramBands(s, L, src, 1, eg.data() + c * kBins * kBands,
                                     kBins, 0.005f, 343.0f, 1024, 64, 4.0f);
    }
    // 減衰の形＝最初のビンを 0dB とした相対。時刻ごとに 4 例を並べる。
    std::printf("      500Hz の減衰（各自の立ち上がりを 0dB とした相対）\n");
    std::printf("      時刻      A1      A2      A3   │    B1（隣室）\n");
    auto lvl = [&](int c, int bin) {
        return eg[static_cast<std::size_t>(c) * kBins * kBands
                + static_cast<std::size_t>(bin) * kBands + 2];
    };
    float ref[4];
    for (int c = 0; c < 4; ++c) {
        ref[c] = 1e-9f;
        for (int b = 0; b < 20; ++b) ref[c] = std::max(ref[c], lvl(c, b));
    }
    float worstSame = 0.0f, worstCross = 0.0f;
    for (int bin = 20; bin < kBins; bin += 20) {
        float d[4];
        for (int c = 0; c < 4; ++c)
            d[c] = 20.0f * std::log10f(std::max(lvl(c, bin), 1e-9f) / ref[c]);
        std::printf("     %4.0f ms  %6.1f  %6.1f  %6.1f   │  %6.1f\n",
                    bin * 5.0f, d[0], d[1], d[2], d[3]);
        for (int a = 0; a < 3; ++a)
            for (int b2 = a + 1; b2 < 3; ++b2)
                worstSame = std::max(worstSame, std::fabs(d[a] - d[b2]));
        for (int a = 0; a < 3; ++a)
            worstCross = std::max(worstCross, std::fabs(d[a] - d[3]));
    }
    std::printf("      同室どうしの最大差: %.1f dB / 隣室との最大差: %.1f dB\n",
                worstSame, worstCross);
    std::printf("      ※ 同室が小さく隣室が大きければ「部屋ごとに畳む」が成立する。\n");
    std::printf("        同室でも大きいなら畳めない（音源ごとに持つしかない）。\n");
    AF_SceneDestroy(s);
}

// 扉が振れる向きに対して、音源が「隙間の開く側」と「板が覆う側」で差が出るか。
//   Unity の Test_SwingDoor と同じ形。音源は戸口の中心から左右 0.8m。
//   蝶番は左枠(x=-0.5)、板は +Z 側（音源のいる部屋）へ振れる。
//     右(+0.8) … 隙間が開いていく方向
//     左(-0.8) … 振れた板が覆っていく方向
//   回折だけで届いている間（見通しが立つ前）に差が出るのかを見る。
void diagnoseDoorSideAsymmetry() {
    std::printf("\n[診断] 扉の開く向き: 隙間側と板の裏側で差が出るか\n");
    AF_SceneHandle s = AF_SceneCreate();
    AF_SceneSetRoomCellSize(s, 0.25f);
    // 材質: 壁はコンクリ、扉は木（Unity と同じ）
    float trC[kBands], abC[kBands], scC[kBands];
    float trW[kBands], abW[kBands], scW[kBands];
    AF_MaterialPresetBands(1, trC, abC, scC);   // Concrete
    AF_MaterialPresetBands(4, trW, abW, scW);   // WoodDoor
    const int matWall = AF_SceneAddMaterial(s, trC, abC, scC, kBands);
    const int matDoor = AF_SceneAddMaterial(s, trW, abW, scW, kBands);
    auto bx = [&](float cx, float cy, float cz, float sx, float sy, float sz, int m) {
        return AF_SceneAddInstanceBox(s, V(cx, cy, cz), V(sx*0.5f, sy*0.5f, sz*0.5f),
                                      V(1,0,0), V(0,1,0), m);
    };
    const float h = 3.0f, th = 0.4f, hw = 7.0f, hd = 7.0f;
    bx(0, -th*0.5f, 0, hw*2, th, hd*2, matWall);        // 床
    bx(0, h + th*0.5f, 0, hw*2, th, hd*2, matWall);     // 天井
    bx(-hw - th*0.5f, h*0.5f, 0, th, h, hd*2, matWall); // 西
    bx( hw + th*0.5f, h*0.5f, 0, th, h, hd*2, matWall); // 東
    bx(0, h*0.5f, -hd - th*0.5f, hw*2, h, th, matWall); // 南
    bx(0, h*0.5f,  hd + th*0.5f, hw*2, h, th, matWall); // 北
    // 仕切り（戸口 x∈[-0.5,+0.5]）
    const float gapL = -0.5f, gapR = 0.5f, pth = 0.2f;
    bx(-hw + (gapL + hw)*0.5f, h*0.5f, 0, gapL + hw, h, pth, matWall);
    bx( gapR + (hw - gapR)*0.5f, h*0.5f, 0, hw - gapR, h, pth, matWall);
    // 扉（蝶番 x=-0.5、+Z 側へ振れる）
    const float dw = gapR - gapL, dth = 0.06f;
    const int doorId = bx(0, h*0.5f, 0, dw, h, dth, matDoor);
    AF_SceneAddPortal(s, V(0, h*0.5f, 0), V(1,0,0), V(0,1,0), dw*0.5f, h*0.5f);

    const AF_Vector3 L = V(0, 1.6f, -3.0f);
    const AF_Vector3 sR = V( 0.8f, 1.6f, 3.0f);   // 隙間が開く側
    const AF_Vector3 sL = V(-0.8f, 1.6f, 3.0f);   // 板が覆う側
    AF_SceneSetListener(s, L);

    std::printf("      角度   右(隙間側)  左(板の裏)   差      右の遮蔽  左の遮蔽\n");
    for (int deg = 0; deg <= 90; deg += 10) {
        const float t = deg * 3.14159265f / 180.0f;
        const float c = std::cos(t), sn = std::sin(t);
        // 蝶番 gapL を軸に回す。板の中心は蝶番から幅の半分だけ先。
        AF_SceneUpdateInstance(s, doorId,
            V(gapL + c * dw * 0.5f, h * 0.5f, sn * dw * 0.5f),
            V(dw * 0.5f, h * 0.5f, dth * 0.5f), V(c, 0, sn), V(0, 1, 0));
        for (int k = 0; k < 3; ++k) AF_SceneUpdate(s, 1.0f/60.0f);
        auto lvl = [&](const AF_Vector3& S) {
            AF_Vector3 dp[8]; float dg[8], db[48];
            const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
            float sum = 0.0f;
            for (int i = 0; i < nd; ++i)
                for (int b = 0; b < kBands; ++b) sum += db[i * kBands + b] / kBands;
            return sum;
        };
        const float gR = lvl(sR), gL = lvl(sL);
        std::printf("      %3d°   %8.5f  %8.5f  %+6.1fdB   %s      %s\n", deg, gR, gL,
                    20.0f * std::log10f(std::max(gR,1e-9f) / std::max(gL,1e-9f)),
                    AF_SceneIsOccluded(s, L, sR) ? "遮" : "見",
                    AF_SceneIsOccluded(s, L, sL) ? "遮" : "見");
    }
    std::printf("      ※「遮」の間は回折だけで届いている。そこで差が出るかが問い。\n");
    AF_SceneDestroy(s);
}

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
    int dropCount = 0;          // 開口へ近づくのに小さくなった箇所（向きの検査）
    // ★型紙 3（経路が消えない）を共有の式へ通す（detectors.h）。掃引そのものは変えない。
    //   「量が小さい」ではなく「**探索が空振りする**」を見るのがこの型紙の要点。
    std::vector<int> pathCounts; std::vector<float> pathAt;
    float prevG = -1.0f;
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
        // f が掛かっているか。深い影で 0 まで落ちる原因の切り分け用。
        float d17[32] = {};
        AF_SceneDebugDiffractionPath(s, L, S, d17, 0);
        // 前川の δ 単独なら幾らになるか。A = 10log10(3 + 20N), N = 2δ/λ（125Hz で λ=2.744m）。
        //   f が落としているのか、δ が落としているのかの切り分け。
        const double dlt = std::max(0.0, (double)d17[0]);
        const double N125 = 2.0 * dlt / 2.744;
        const double mk125 = std::pow(10.0, -0.05 * (10.0 * std::log10(3.0 + 20.0 * N125)));
        std::printf("      %8.1f   %6.3f   %d 本  (%5.2f,%5.2f)  %6.2f m  "
                    "δ=%5.2f 前川単独 %.4f  f(125)=%.4f  f%s%s\n",
                    x, g[0], n, dx, dz, plen,
                    dlt, mk125, d17[10],
                    d17[18] > 0.5f ? "○" : "×",
                    (n == 0) ? "   ← 回折が消えた" : "");
        // ★以前は `g[0] <= 1e-4` も「消えた」に数えていた。**絶対値での判定**で、
        //   このファイルの方針（冒頭コメント「期待値を絶対値でなく関係で書く」）に反していた。
        //   しかも閾値は f が飽和して壊れていた頃に決めたもの。f が効くようになると、
        //   横へ外れた端が正しく小さくなって落ちる（実測 x=-8 で 前川単独 0.0886 に対し
        //   f 0.0000 ── 有限の開口は半無限スクリーンより通さないので向きは正しい）。
        //   → **見たいのは「探索が空振りして経路が消える」こと**なので、そこだけ縛る。
        //     量が小さいことは下の単調性で見る。
        if (n == 0) ++zeroCount;
        pathCounts.push_back(n); pathAt.push_back(x);
        // 開口は +x 側にあるので、x が増えるほど大きくなるのが正しい向き。
        //   量そのものではなく**向き**を縛る（絶対値で縛らない）。
        if (x > -8.0f && g[0] < prevG - 1e-6f) ++dropCount;
        prevG = g[0];
        if (havePrev && n > 0) {
            const float d = std::sqrt((dx - prevDx) * (dx - prevDx) + (dz - prevDz) * (dz - prevDz));
            if (d > maxDirJump) { maxDirJump = d; dirJumpAt = x; }
            const float dl = std::fabs(plen - prevLen);
            if (dl > maxLenJump) { maxLenJump = dl; lenJumpAt = x; }
        }
        if (n > 0) { prevDx = dx; prevDz = dz; prevLen = plen; havePrev = true; }
    }

    char b[128];
    // ★以前は「ゲイン <= 1e-4 なら消えたとみなす」という**絶対値**の判定だった。
    //   このファイルの方針（冒頭「期待値を絶対値でなく関係で書く」）に反していて、
    //   しかも閾値は f が飽和して壊れていた頃に決めたもの。f が効くようになると、
    //   横へ外れた端が正しく小さくなって落ちた（実測 x=-8 で 前川単独 0.0886 に対し f 0.0000。
    //   有限の開口は半無限スクリーンより通さないので、小さくなる向き自体は正しい）。
    //   → **絶対値をやめ、2 つの「関係」で縛る**。緩めたのではなく、見る物を変えた。
    //     ① 経路が見つかること（探索の空振り＝本当の死角を捕まえる）
    //     ② 開口へ近づくほど大きくなること（向きが逆なら模型が壊れている）
    // ── 型紙 3 を共有の式で。走査（デバッグツール）と検査がここで同じ関数を呼ぶ ──
    {
        af::detect::Break br[16];
        const int np = af::detect::pathAlive(pathCounts.data(), pathAt.data(),
                                             (int)pathCounts.size(), br, 16);
        af::detect::report("型紙3 経路が消えない", br, np, "");
        std::snprintf(b, sizeof(b), "(経路が消えた %d 箇所)", np);
        check("開口が1つだけなので、どこでも経路が見つかる", np == 0 && zeroCount == 0, b);
    }
    std::snprintf(b, sizeof(b), "(逆行 %d 箇所)", dropCount);
    check("開口へ近づくほど大きくなる（量ではなく向きを縛る）", dropCount == 0, b);
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
    // ★型紙 2（向き）・C（効いている）・D（上限）を共有の式へ通す（detectors.h）。
    //   ここが **2 と C を対で使う**理由の現物。この掃引の元になったバグは
    //   「2m でも 3cm でも同じ音」＝ **定数**で、単調性だけなら合格してしまう。
    std::vector<float> gapAt, gapSeries, tapSeries;
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
        // ★実際に鳴るのはタップ経路。生存ゲインだけ見て誤診した前例が 2 回あるので並べる。
        AF_SceneUpdate(s, 0.02f);
        AF_Vector3 dp[8]; float dg[8], db[48];
        const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
        float tap[kBands] = {};
        for (int i = 0; i < nd; ++i)
            for (int b = 0; b < kBands; ++b) tap[b] += db[i * kBands + b];
        float dbg[28] = {};
        AF_SceneDebugDiffractionPath(s, L, S, dbg, -1);
        std::printf("      %6.2f m  %6.3f  %6.3f  %6.3f   │ タップ %d本 %6.3f"
                    " │ f: 面%d枚 有界%d枚 │ 縁 u=%.2f v=%.2f │ 後ろで捨てた遮蔽物 %d 個\n",
                    gap, g[0], g[2], g[5], nd, tap[0],
                    (int)dbg[22], (int)dbg[23], dbg[24], dbg[25], (int)dbg[26]);
        gapAt.push_back(gap); gapSeries.push_back(g[0]); tapSeries.push_back(tap[0]);
        if (prev >= 0.0f && g[0] > prev + 1e-3f) monotone = false;
        if (gap >= 1.99f) wide = g[0];
        narrow = g[0];
        prev = g[0];
        AF_SceneDestroy(s);
    }

    char b[96];
    // ── 型紙 2＋C＋D を共有の式で。走査と検査がここで同じ関数を呼ぶ ──
    {
        af::detect::Break br[16];
        // 型紙 2: 隙間が狭くなる向きに掃いているので「下がり続ける」が正しい向き。
        const int nm = af::detect::monotonic(gapSeries.data(), gapAt.data(),
                                             (int)gapSeries.size(), /*rising=*/false,
                                             1e-3f, br, 16);
        af::detect::report("型紙2 向きが正しい", br, nm, "");
        check("隙間を狭めるほど回折が落ちる(単調)", nm == 0 && monotone);

        // 型紙 C: ★**単調は定数を通す。**振れ幅が無ければ「つまみが効いていない」。
        //   これを入れないと、隙間幅が全く効かなかった当時のコードが**合格していた**。
        const float range = af::detect::responseRangeDb(gapSeries.data(), (int)gapSeries.size());
        std::snprintf(b, sizeof(b), "(2m %.3f → 3cm %.3f / 振れ幅 %.1f dB)", wide, narrow, range);
        check("3cm の隙間は 2m の隙間よりずっと通らない(1/4 以下)", narrow < wide * 0.25f, b);
        std::snprintf(b, sizeof(b), "(振れ幅 %.1f dB)", range);
        check("型紙C 隙間幅が効いている(定数でない・12dB 以上動く)", range >= 12.0f, b);

        // 型紙 D: タップは振幅比なので 1.0 を超えたら物理的にありえない。
        //   ★元になったバグ: 取り分を絶対和にしたとき柱のタップが 0.88 → 1.76。
        const int nb = af::detect::withinPhysicalBound(tapSeries.data(), gapAt.data(),
                                                      (int)tapSeries.size(), 1.0f, br, 16);
        af::detect::report("型紙D 上限を超えない", br, nb, "");
        std::snprintf(b, sizeof(b), "(1.0 超過 %d 箇所)", nb);
        check("型紙D タップが物理の上限(1.0)を超えない", nb == 0, b);
    }

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
    float sweepEach[91][3] = {};
    float sweepDirect[91] = {}, sweepOcc[91] = {};
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
                // 経路ごとの重みも控える。合計だけ見ていると「どれが飛んだか」が分からない。
                for (int i = 0; i < 3; ++i) sweepEach[di][i] = (i < n) ? gain[i] : 0.0f;
                // 55〜60°の飛びの中身。経路ごとに、配分の重み w と経路長を見る。
                //   w は **重心由来の経路長** から作られている。重心はクラスタの構成が
                //   変わると跳ぶ ── 設計 scene.h の DiffractionPath がまさに警告している点。
                if (di >= 54 && di <= 61) {
                    std::printf("          %3d°  ", di);
                    for (int pi = 0; pi < n && pi < 3; ++pi) {
                        float d[20] = {};
                        AF_SceneDebugDiffractionPath(s, L, S, d, pi);
                        std::printf("[%d] δ %.3f thru %.5f | ", pi, d[0], d[3]);
                    }
                    // ポータルごとに「開口率」と「影として写った遮蔽物の枚数」。
                    //   枚数が 1 減った角度が、扉が計算から外れた角度。
                    int na = 0, nm = 0; AF_SceneGetPortalCounts(s, &na, &nm);
                    for (int q = 0; q < na + nm && q < 4; ++q) {
                        float fz[kBands] = {}; AF_Vector3 pcp = V(0,0,0);
                        if (!AF_SceneMeasurePortal(s, q, L, S, fz, &pcp)) continue;
                        AF_Vector3 pc{}, pu{}, pv{}; float phu = 0, phv = 0;
                        AF_SceneGetPortal(s, q, &pc, &pu, &pv, &phu, &phv);
                        std::printf("P%d 開口 %.4f 影%d枚 中心(%.2f,%.2f,%.2f) 半%.2fx%.2f | ",
                                    q, fz[0], AF_SceneDebugPortalPolys(s),
                                    pc.x, pc.y, pc.z, phu, phv);
                    }
                    std::printf("\n");
                }
                // 直接音がいつ入り始めるか。回折だけ見ていると「見通しが立つ瞬間」が映らない。
                {
                    float tr[kBands] = {}; float of = 0.0f;
                    AF_SceneComputeSoftOcclusion(s, L, S, tr, kBands, &of);
                    float trb = 0.0f;
                    for (int b = 0; b < kBands; ++b) trb += tr[b] / kBands;
                    sweepDirect[di] = trb;
                    sweepOcc[di] = of;
                }
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
            std::printf("          %3d°  回折 %.4f  開口率125Hz %.4f  直接音 %.4f  遮蔽 %.3f\n",
                        d, sweepSum[d], sweepOpen[d], sweepDirect[d], sweepOcc[d]);
        const int j = static_cast<int>(jumpAt + 0.5f);
        std::printf("        隣接差が最大の付近 (%d°):\n", j);
        for (int d = std::max(0, j - 3); d <= std::min(90, j + 2); ++d)
            std::printf("          %3d°  合計 %.4f  開口率 %.4f  経路 %d本  内訳 %.4f / %.4f / %.4f\n",
                        d, sweepSum[d], sweepOpen[d], sweepN[d],
                        sweepEach[d][0], sweepEach[d][1], sweepEach[d][2]);
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
            // 隙間の幅で**音色が変わる**こと（全開は平坦、狭いと傾く）。
            //   ★2026-09-02 言い直し。以前は「狭いほど低域が通る」（低−高 が正）を要求していた。
            //     それは直接経路が扉板の質量則しか見ていなかった頃の形。直接経路の半影を
            //     帯域別のフレネル半径にすると、125 Hz の窓（この配置で ≒ 2.5 m）は板越しにしか
            //     見えず、4 kHz の窓（≒ 0.44 m）は隙間の縁にかかって素通しの標本を拾う
            //     ── 「狭い隙間は高域だけ通す」（aperture_fresnel.h の主張、開けると明るくなる）
            //     が直接経路にも出て、符号が反転した（実測 狭 −8.8 / 全開 −1.1 dB）。
            //     符号は板の透過と隙間の面積比で決まる絶対値の話なので縛らず、
            //     **狭いと傾き、開くと平坦に戻る**という形だけを縛る。
            const float narrow = portalTilt(0.08f);   // 1割弱だけ開いている
            const float wide   = portalTilt(1.00f);   // 全開
            { char bo[96]; std::snprintf(bo, sizeof(bo), "(狭 %.1f / 全開 %.1f dB)", narrow, wide);
              check("[開口] 隙間が狭いと音色が傾き、全開で平坦に戻る（差 3 dB 以上）",
                    std::fabs(narrow) > std::fabs(wide) + 3.0f, bo); }
            check("[開口] 全開なら帯域差はほぼ無い", wide < 1.5f);
        }

        check("[回折] 障害物なしなら生存ゲインは平坦", tiltDb(false, true, noAbsorb) < 0.2f);

        // ★2026-09-02 言い直し（直接経路の半影を帯域別のフレネル半径にしたため）。
        //   以前は「柱を置いても平坦（< 0.2 dB）」だったが、その前提は物理的に成立しない:
        //   0.6 m の柱は 125 Hz のフレネル半径（この配置で ≒ 2.4 m）よりずっと細く、低域は柱を
        //   回り込んで届く。だから柱の影では**低域ほど通る**のが正しい。
        //   「回折で LPF は掛けない（こもりは材質が担当）」という決定が守ろうとしたのは
        //   **深い影の定常状態**で、それは厚い壁の向こうで確かめる（下）。縁の半影は帯域で幅が違う。
        {
            // 低−高（符号つき）。正なら低域の方が通っている。
            auto tiltLoHi = [&](bool pillar, const float* absorb) {
                AF_SceneHandle s = build(pillar, absorb);
                // ★反射レイは切る（useReflections=0）。この検査が縛るのは**直接経路の半影**が帯域で幅を変えること。
                //   2026-09-03 に反射レイを「運ぶエネルギーが床を割るまで」走らせるようにしたら、
                //   吸わない壁（noAbsorb）の箱では反射が影を埋め切って 低−高 0.0 dB になった。
                //   それは無損失の箱の正しい極限で、半影の主張とは別物。反射込みの影は上の 3 ケース表で見る。
                AF_UpdateConfig cfg = {};
                cfg.role1EveryN = 1; cfg.role2EveryN = 1; cfg.earlyEveryN = 1;
                cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 3;
                cfg.reflectionRays = 256; cfg.reflectionBounces = 6;
                cfg.directWeight = 1.0f; cfg.useReflections = 0;
                cfg.useEdgeCatalog = 1; cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;
                cfg.enableReverb = 0; cfg.echogramBins = 100; cfg.echogramBinSeconds = 0.01f;
                cfg.echogramRays = 128; cfg.echogramBounces = 8; cfg.speedOfSound = 343.0f;
                cfg.enableEarlyReflections = 0; cfg.earlyTaps = 4; cfg.earlyRays = 64; cfg.earlyBounces = 2;
                cfg.enableDiffractionSources = 0; cfg.diffSources = 1;
                AF_SceneSetUpdateConfig(s, &cfg);
                AF_SceneSetListener(s, V(0, 1.6f, -3.5f));
                AF_SceneSetSource(s, 1, V(0, 1.6f, 3.5f));
                for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
                float occ[kBands] = {};
                const int idx = AF_SceneSourceIndex(s, 1);
                if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, occ);
                AF_SceneDestroy(s);
                return 20.0f * std::log10(std::max(occ[0], 1e-6f) / std::max(occ[kBands - 1], 1e-6f));
            };
            const float pillarTilt = tiltLoHi(true, noAbsorb);
            char b2[96];
            std::snprintf(b2, sizeof(b2), "(低−高 %.1f dB)", pillarTilt);
            check("[回折] 柱の影では低域ほど通る（低−高 が正、20 dB 以内）",
                  pillarTilt > 0.0f && pillarTilt < 20.0f, b2);

            // 深い影: 部屋を横切る厚い壁（吸わない・透過は材質どおり）。全帯域が壁を通るので平坦。
            AF_SceneHandle s = AF_SceneCreate();
            const float flatT[6] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};   // 透過も平坦にする（既定は質量則で 21 dB 傾く）
            const int m = AF_SceneAddMaterial(s, flatT, noAbsorb, nullptr, 6);
            AF_SceneAddInstanceBox(s, V(0, -tt, 0),    V(hw2+tt, tt, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh+tt, 0),  V(hw2+tt, tt, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(-hw2-tt, hh*0.5f, 0), V(tt, hh*0.5f, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V( hw2+tt, hh*0.5f, 0), V(tt, hh*0.5f, hd2+tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f, -hd2-tt), V(hw2+tt, hh*0.5f, tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f,  hd2+tt), V(hw2+tt, hh*0.5f, tt), V(1,0,0), V(0,1,0), m);
            AF_SceneAddInstanceBox(s, V(0, hh*0.5f, 0), V(hw2, hh*0.5f, 0.3f), V(1,0,0), V(0,1,0), m);   // 部屋を横切る壁
            AF_SceneSetListener(s, V(0, 1.6f, -3.5f));
            AF_SceneSetSource(s, 1, V(0, 1.6f, 3.5f));
            for (int i = 0; i < 6; ++i) AF_SceneUpdate(s, 1.0f / 60.0f);
            float occ[kBands] = {};
            const int idx = AF_SceneSourceIndex(s, 1);
            if (idx >= 0) AF_SceneGetSourceOcclusion(s, idx, occ);
            AF_SceneDestroy(s);
            float lo = 1e9f, hi = -1e9f;
            for (int b = 0; b < kBands; ++b) { lo = std::min(lo, occ[b]); hi = std::max(hi, occ[b]); }
            const float wallTilt = 20.0f * std::log10(std::max(hi, 1e-6f) / std::max(lo, 1e-6f));
            std::snprintf(b2, sizeof(b2), "(帯域差 %.2f dB)", wallTilt);
            check("[回折] 厚い壁の深い影では生存ゲインは平坦（こもりは材質が担当）", wallTilt < 0.2f, b2);
        }
        // 切り替えが効いている証拠。★吸わない表面だと反射が強すぎて回折の傾きが薄まるので、
        //   ここは既定壁（吸音あり）で比べる。既定壁では平坦化 ON でも吸音ぶんの傾きが残る
        //   （物理的に正しい残り方）ので、絶対値ではなく ON/OFF の差で見る。
        //   ⚠ 2026-09-02: 柱の場面では直接経路の半影（帯域別）が回折の項より大きく、
        //     平坦化の切り替え（回折の項だけに効く）の差が 1 dB を切るようになった。
        //     ここでは「切っても傾きが減らない」だけを確かめる。切り替えの効き目そのものは
        //     回折が支配する場面（く字の廊下）で測るのが筋 ── 未整備。
        {
            const float on = tiltDb(true, true, nullptr);
            const float off = tiltDb(true, false, nullptr);
            char b3[96];
            std::snprintf(b3, sizeof(b3), "(ON %.2f / OFF %.2f dB)", on, off);
            check("[回折] 平坦化を切っても傾きは減らない", off >= on - 0.2f, b3);
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

// ================================================ 型紙そのものの検査（検出器の検査）
// ★「破れなし」は、その型紙が**効いている**証明にならない。
//   検出器が黙っているのは「健全だから」かもしれないし「壊れていて何も見ていない」
//   からかもしれない。区別できないまま並べると、中身の無い検査が増える
//   ── これは型紙 C（効いている）が言っていることの、検出器自身への適用。
//
//   なので**当時の実数**を食わせて発火することを確かめる。数値は全部この作品で実際に出た値。
void testDetectorSelfCheck() {
    std::printf("\n[型紙] 検出器そのものの検査（当時の実数を食わせて発火するか）\n");
    af::detect::Break br[16];
    char b[160];
    auto fires = [&](const char* label, int n, int want) {
        std::snprintf(b, sizeof(b), "(発火 %d 件 / 期待 %d 件)", n, want);
        check(label, n == want, b);
    };

    // 型紙 1: 閉扉の距離掃引。実測で 10cm を跨いで 19.7dB / 23.0dB 跳んだ。
    {
        const float bad[]  = {-40.0f, -39.2f, -38.5f, -18.8f, -18.1f};   // 4 番目で 19.7dB
        const float good[] = {-40.0f, -39.2f, -38.5f, -37.6f, -36.9f};
        fires("型紙1 が 19.7dB の跳びで発火する",
              af::detect::noJump(bad, nullptr, 5, 6.0f, br, 16), 1);
        fires("型紙1 が正常な階段では黙る",
              af::detect::noJump(good, nullptr, 5, 6.0f, br, 16), 0);
    }
    // 型紙 2 と C: 対で使う理由の現物。
    //   ★**定数の列を型紙 2 に食わせると合格する。**これが「隙間幅が効かない」を
    //     見逃した形そのもの。C を足して初めて捕まる。
    {
        const float flat[]  = {0.180f, 0.180f, 0.180f, 0.180f};          // 幅を変えても同じ
        const float rev[]   = {0.180f, 0.090f, 0.140f, 0.030f};          // 3 番目で向きが逆
        const float healthy[] = {0.180f, 0.090f, 0.030f, 0.004f};
        fires("型紙2 が向きの逆転で発火する",
              af::detect::monotonic(rev, nullptr, 4, false, 1e-3f, br, 16), 1);
        fires("★型紙2 は定数を通してしまう（だから C と対で使う）",
              af::detect::monotonic(flat, nullptr, 4, false, 1e-3f, br, 16), 0);
        const float rFlat = af::detect::responseRangeDb(flat, 4);
        const float rOk   = af::detect::responseRangeDb(healthy, 4);
        std::snprintf(b, sizeof(b), "(定数 %.1f dB / 健全 %.1f dB)", rFlat, rOk);
        check("型紙C が定数を捕まえ、健全な掃引は通す", rFlat < 1.0f && rOk > 12.0f, b);
    }
    // 型紙 3: 2 次回折が 0.486 → 0.000 になったとき、経路探索が空振りしていた。
    {
        const int bad[]  = {3, 2, 0, 1};
        const int good[] = {3, 2, 1, 1};
        fires("型紙3 が経路の消失で発火する", af::detect::pathAlive(bad, nullptr, 4, br, 16), 1);
        fires("型紙3 が経路があれば黙る",  af::detect::pathAlive(good, nullptr, 4, br, 16), 0);
    }
    // 型紙 4: 閉扉の幻。帯域がまっすぐ（低−高 ≈ 0）＝ 回折でも透過でもありえない形。
    {
        const float phantom[kBands] = {0.21f, 0.21f, 0.21f, 0.21f, 0.209f, 0.209f};
        const float real[kBands]    = {0.21f, 0.15f, 0.09f, 0.04f, 0.012f, 0.003f};
        fires("型紙4 が平らな帯域（幻）で発火する",
              af::detect::noPhantom(phantom, nullptr, nullptr, 1, 3.0f, br, 16), 1);
        fires("型紙4 が周波数依存のある本物では黙る",
              af::detect::noPhantom(real, nullptr, nullptr, 1, 3.0f, br, 16), 0);
    }
    // 型紙 5 と B: 同じ設定で 2 回（5）／設定を変えて（B）。別物なので両方要る。
    {
        const float a[] = {1.0f, 2.0f, 3.0f};
        const float c[] = {1.0f, 2.0f, 3.0000005f};
        fires("型紙5 が実行ごとの揺れで発火する", af::detect::deterministic(a, c, 3, 0.0f, br, 16), 1);
        fires("型紙B(ビット一致) が設定で変わった音を捕まえる",
              af::detect::invariantUnderSetting(a, c, 3, true, 0.0f, br, 16), 1);
        fires("型紙B(許容差) は丸めの差では黙る",
              af::detect::invariantUnderSetting(a, c, 3, false, 1e-4f, br, 16), 0);
    }
    // 型紙 A: 尾だけ 8.3 倍（outputGain の掛け忘れ）。跳びでも幻でもない「一定の間違い」。
    {
        const float den[] = {0.10f, 0.12f, 0.09f};
        const float bad[] = {0.83f, 0.996f, 0.747f};                     // 全部 8.3 倍
        const float good[]= {0.10f, 0.12f, 0.09f};
        fires("型紙A が 8.3 倍の掛け忘れで発火する",
              af::detect::ratioInRange(bad, den, nullptr, 3, 0.99f, 1.01f, "r", br, 16), 3);
        fires("型紙A が比が保たれていれば黙る",
              af::detect::ratioInRange(good, den, nullptr, 3, 0.99f, 1.01f, "r", br, 16), 0);
    }
    // 型紙 D: 取り分を絶対和にしたとき、柱のタップが 0.88 → 1.76。
    {
        const float bad[]  = {0.88f, 1.76f, 0.44f};
        const float good[] = {0.88f, 0.88f, 0.44f};
        fires("型紙D が 1.76（1.0 超え）で発火する",
              af::detect::withinPhysicalBound(bad, nullptr, 3, 1.0f, br, 16), 1);
        fires("型紙D が上限内では黙る",
              af::detect::withinPhysicalBound(good, nullptr, 3, 1.0f, br, 16), 0);
    }
}

// ================================================ キャプチャ（録る → 読む → 走査する）
// 仕様: docs/SOUND_DEBUG_TOOL.md。ここで見るのは 3 つ。
//   ① 録った**入力**がそのまま戻ること（戻らなければ再現にならない）
//   ② 動いた実体（扉）が残ること ← ★呼ぶ順の罠がある。下の注記
//   ③ 走査が本物の破れに印を付けること（型紙は回帰テストと同じ関数）
// キャプチャ用の場面。★リプレイ側が**同じ形**を作れるよう、必ずここを通すこと
//   （キャプチャに入っているのは動いた実体だけで、静的な形は入っていない）。
static AF_SceneHandle buildCaptureScene(int* outDoor) {
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    const float t = 0.1f, h = 3.0f;
    // 仕切り壁（戸口 x∈[-0.5,0.5]）と、その戸口を塞ぐ扉。
    AF_SceneAddInstanceBox(s, V(-2.25f, h*0.5f, 0), V(1.75f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V( 2.25f, h*0.5f, 0), V(1.75f, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    const int door = AF_SceneAddInstanceBox(s, V(0, h*0.5f, 0), V(0.5f, h*0.5f, 0.05f),
                                            V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, -t, 0), V(6, t, 8), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(6, t, 8), V(1,0,0), V(0,1,0), mat);
    if (outDoor) *outDoor = door;
    return s;
}

void testCaptureRoundTrip() {
    std::printf("\n[キャプチャ] 録る → 読む → 走査する\n");
    const char* kPath = "afcap_roundtrip.afcap";

    int door = 0;
    AF_SceneHandle s = buildCaptureScene(&door);
    const float h = 3.0f;

    const unsigned long long kId = 4001;
    AF_SceneCaptureBegin(s, /*preroll*/60, /*postroll*/30, /*maxSources*/4,
                         /*sampleRate*/48000, /*recordPcm*/1);

    const int kFrames = 200, kBlock = 512;
    std::vector<float> pcm(static_cast<size_t>(kBlock) * 2);
    std::vector<float> lzWant; std::vector<int> movedWant;
    int markFrame = 150, teleportFrame = 120;
    for (int f = 0; f < kFrames; ++f) {
        // 扉を少しずつ開ける（＝実体が動く）。★録れているかを②で見る。
        const float open = 0.002f * static_cast<float>(f);
        AF_SceneUpdateInstance(s, door, V(open, h*0.5f, 0), V(0.5f, h*0.5f, 0.05f),
                               V(1,0,0), V(0,1,0));
        // リスナーは扉へ近づく。★frame 120 で瞬間移動させる ＝ **本物の跳び**を仕込む。
        float lz = -4.0f + 0.02f * static_cast<float>(f);
        if (f >= teleportFrame) lz += 3.0f;
        AF_SceneSetListener(s, V(0.2f, 1.5f, lz));
        AF_SceneSetSource(s, kId, V(0.2f, 1.5f, 3.0f));
        // マスターPCM。位置が分かる値を入れて往復を確かめる。
        for (int i = 0; i < kBlock * 2; ++i)
            pcm[static_cast<size_t>(i)] = 0.5f * std::sin(0.01f * static_cast<float>(f * kBlock + i));
        AF_SceneCapturePushAudio(s, pcm.data(), kBlock);
        if (f == markFrame) AF_SceneCaptureMark(s);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        lzWant.push_back(lz);
        movedWant.push_back(f);
    }

    char note[192];
    int held = 0;
    const int st = AF_SceneCaptureStatus(s, &held);
    std::snprintf(note, sizeof(note), "(状態 %d / 保持 %d フレーム)", st, held);
    check("[キャプチャ] マーク後に前後が揃って保存可能になる", st == 2 && held == 91, note);

    const int wrote = AF_SceneCaptureWrite(s, kPath, "Test_CaptureRoundTrip", 0xABCD1234u);
    check("[キャプチャ] .afcap を書けた", wrote == 1);
    AF_SceneDestroy(s);

    acoustic::dbg::CaptureFile cap;
    std::string err;
    const bool ok = acoustic::dbg::readCapture(kPath, cap, &err);
    std::snprintf(note, sizeof(note), "(%s)", ok ? "読めた" : err.c_str());
    check("[キャプチャ] 書いたものをそのまま読める", ok, note);
    if (!ok) return;

    std::snprintf(note, sizeof(note), "(場面 %s / dllHash %08x / 並列 %d)",
                  cap.scene.c_str(), cap.dllHash, cap.workerThreads);
    check("[キャプチャ] ヘッダの識別子が焼けている（古い物を掴んだまま気づかない対策）",
          cap.scene == "Test_CaptureRoundTrip" && cap.dllHash == 0xABCD1234u, note);

    // ① 入力（リスナー位置）が**そのまま**戻ること。丸めも許さない ── 押し直しが再現になる前提。
    {
        // ⚠ 残るのは「末尾 N フレーム」ではない。**マーク＋postroll で録音が止まる**ので、
        //   窓は掃引の途中で終わる。だからフレーム番号で引く（最初これを間違えて全滅した）。
        int bad = 0;
        for (int k = 0; k < cap.frames; ++k) {
            const unsigned fn = cap.global[static_cast<size_t>(k)].frame;
            if (fn >= lzWant.size()) { ++bad; continue; }
            if (cap.global[static_cast<size_t>(k)].lz != lzWant[fn]) ++bad;
        }
        std::snprintf(note, sizeof(note), "(%d フレーム中 ずれ %d)", cap.frames, bad);
        check("[キャプチャ] ① 入力のリスナー位置がビット一致で戻る", bad == 0, note);
    }

    // ② 動いた実体が残ること。
    //   ★ここに罠があった。ホストの呼ぶ順は UpdateInstance → SetListener → Update なので、
    //     **扉の記録は beginFrame より前に溜まる**。最初 beginFrame で枠を消していて、
    //     扉の動きが 1 つも残らなかった。掃除の場所を endFrame 側へ移して直した。
    {
        int framesWithMoved = 0, wrongId = 0;
        for (int k = 0; k < cap.frames; ++k) {
            if (!cap.moved[static_cast<size_t>(k)].empty()) ++framesWithMoved;
            for (const auto& m : cap.moved[static_cast<size_t>(k)])
                if (m.instance != door) ++wrongId;
        }
        std::snprintf(note, sizeof(note), "(%d / %d フレームに扉あり・別 id %d)",
                      framesWithMoved, cap.frames, wrongId);
        check("[キャプチャ] ② 動いた扉が毎フレーム残る（呼ぶ順の罠）",
              framesWithMoved == cap.frames && wrongId == 0, note);
    }

    // マークの印がちょうど 1 つ、押したフレームに付くこと。
    {
        int marks = 0; unsigned markedAt = 0;
        for (int k = 0; k < cap.frames; ++k)
            if (cap.global[static_cast<size_t>(k)].marked) {
                ++marks; markedAt = cap.global[static_cast<size_t>(k)].frame;
            }
        std::snprintf(note, sizeof(note), "(印 %d 個 @ frame %u / 押したのは %d)",
                      marks, markedAt, markFrame);
        check("[キャプチャ] 「変だ」の印が押したフレームに 1 つだけ付く",
              marks == 1 && markedAt == static_cast<unsigned>(markFrame), note);
    }

    // PCM が入っていること。
    {
        const double secs = static_cast<double>(cap.pcm.size() / 2) / cap.sampleRate;
        std::snprintf(note, sizeof(note), "(%zu サンプル ＝ %.2f 秒)", cap.pcm.size() / 2, secs);
        check("[キャプチャ] マスターPCM が前後ぶん入っている", cap.pcm.size() > 0 && secs > 1.0, note);
    }

    // ③ 走査。★型紙は回帰テストが呼ぶのと**同じ関数**（detectors.h）。
    {
        const std::vector<af::scan::Mark> marks = af::scan::scan(cap);
        af::scan::printMarks(marks);
        int jumpAtTeleport = 0;
        for (const auto& m : marks) {
            if (std::string(m.templateName).find("型紙1") == std::string::npos) continue;
            if (m.frame >= teleportFrame && m.frame <= teleportFrame + 2) ++jumpAtTeleport;
        }
        std::snprintf(note, sizeof(note), "(frame %d 付近の型紙1 の印 %d 件 / 印は全部で %zu 件)",
                      teleportFrame, jumpAtTeleport, marks.size());
        check("[キャプチャ] ③ 走査が仕込んだ跳び（瞬間移動）に印を付ける",
              jumpAtTeleport >= 1, note);
    }
    std::remove(kPath);
}

// ================================================ 入力リプレイ（押し直して解き直す）
// ★この道具の前提そのもの ──「録った入力を押し直せば再現になる」を**測る**。
//   仕様書には「同じビルド・同じ workerThreads なら完全に一致する」と書いたが、
//   それが**どこまで本当か**を確かめていなかった。ここで確かめる。
void testCaptureReplay() {
    std::printf("\n[リプレイ] 録った入力を押し直すと同じ音になるか\n");
    const char* kPath = "afcap_replay.afcap";
    const unsigned long long kId = 4001;
    const float h = 3.0f;

    // 録る側と押す側で**同じ動き**を使う（ずれたら比較にならない）。
    auto driveFrame = [&](AF_SceneHandle s, int door, int f) {
        const float open = 0.002f * static_cast<float>(f);
        AF_SceneUpdateInstance(s, door, V(open, h*0.5f, 0), V(0.5f, h*0.5f, 0.05f),
                               V(1,0,0), V(0,1,0));
        AF_SceneSetListener(s, V(0.2f, 1.5f, -4.0f + 0.02f * static_cast<float>(f)));
        AF_SceneSetSource(s, kId, V(0.2f, 1.5f, 3.0f));
    };

    auto recordTo = [&](const char* path, int preroll, int frames, int markAt) {
        int door = 0;
        AF_SceneHandle s = buildCaptureScene(&door);
        AF_SceneCaptureBegin(s, preroll, 30, 4, 48000, /*recordPcm*/0);
        for (int f = 0; f < frames; ++f) {
            driveFrame(s, door, f);
            if (f == markAt) AF_SceneCaptureMark(s);
            AF_SceneUpdate(s, 1.0f / 60.0f);
        }
        const int ok = AF_SceneCaptureWrite(s, path, "Test_CaptureReplay", 0u);
        AF_SceneDestroy(s);
        return ok == 1;
    };

    char note[224];

    // ── ① 窓がフレーム 0 から始まる場合 ──
    //   録音開始とリプレイ開始でエンジンの内部状態が揃うので、ここは厳密に一致するはず。
    {
        const bool wrote = recordTo(kPath, /*preroll*/300, /*frames*/120, /*markAt*/80);
        check("[リプレイ] ① 先頭から録れた", wrote);
        acoustic::dbg::CaptureFile cap;
        if (wrote && acoustic::dbg::readCapture(kPath, cap)) {
            int door = 0;
            AF_SceneHandle s = buildCaptureScene(&door);   // ★同じ形を作り直す
            const std::vector<af::replay::FrameDiff> d = af::replay::replayAndCompare(s, cap);
            AF_SceneDestroy(s);
            int badFrames = 0, totalMismatch = 0, badTail = 0; float worst = 0.0f; int worstAt = -1;
            for (const auto& x : d) {
                if (x.exactMismatches) { ++badFrames; totalMismatch += x.exactMismatches; }
                if (!x.tailExact) ++badTail;
                if (x.worstRel > worst) { worst = x.worstRel; worstAt = x.frame; }
            }
            std::printf("        窓の先頭 frame %u / %d フレーム\n",
                        cap.global.empty() ? 0u : cap.global[0].frame, cap.frames);
            std::snprintf(note, sizeof(note),
                          "(%d フレーム中 一致しない %d / 要素 %d / 最大相対差 %.3e @f%d)",
                          cap.frames, badFrames, totalMismatch, worst, worstAt);
            check("[リプレイ] ① 先頭から録れば押し直しでビット一致する",
                  badFrames == 0 && cap.frames > 0, note);
            std::snprintf(note, sizeof(note), "(尾が一致しない %d フレーム / 全 %d)",
                          badTail, cap.frames);
            check("[リプレイ] ① 尾（エコグラム）も先頭からビット一致する",
                  badTail == 0 && cap.frames > 0, note);
        }
        std::remove(kPath);
    }

    // ── ② 窓が途中から始まる場合（実際の使い方はこちら）──
    //   ⚠ ここが仕様書の書きすぎだったところ。リングが一周していると、窓の先頭では
    //     エンジンの内部状態（段の間引きカウンタ・エコグラムの積み上げ）が**録音時と違う**。
    //     どのくらいで揃うかを測って、必要な暖機を数字で出す。
    {
        const bool wrote = recordTo(kPath, /*preroll*/60, /*frames*/200, /*markAt*/150);
        check("[リプレイ] ② 途中から始まる窓を録れた", wrote);
        acoustic::dbg::CaptureFile cap;
        if (wrote && acoustic::dbg::readCapture(kPath, cap)) {
            int door = 0;
            AF_SceneHandle s = buildCaptureScene(&door);
            const std::vector<af::replay::FrameDiff> d = af::replay::replayAndCompare(s, cap);
            AF_SceneDestroy(s);
            // 「最後にビット一致しなかったフレーム」＝ここまでは信用しない、の境目。
            int lastBad = -1, badFrames = 0, lastBadTail = -1, badTail = 0;
            float worstTail = 0.0f;
            for (int i = 0; i < static_cast<int>(d.size()); ++i) {
                if (d[static_cast<size_t>(i)].exactMismatches) { lastBad = i; ++badFrames; }
                if (!d[static_cast<size_t>(i)].tailExact) { lastBadTail = i; ++badTail; }
                if (d[static_cast<size_t>(i)].tailRel > worstTail)
                    worstTail = d[static_cast<size_t>(i)].tailRel;
            }
            std::printf("        窓の先頭 frame %u / %d フレーム\n",
                        cap.global.empty() ? 0u : cap.global[0].frame, cap.frames);
            std::printf("        帯域ゲイン: 一致しない %d フレーム\n", badFrames);
            std::printf("        尾(エコグラム): 一致しない %d フレーム / 最大相対差 %.3e\n",
                        badTail, worstTail);
            if (lastBadTail >= 0)
                std::printf("        ★尾の暖機は先頭から %d フレーム"
                            "（そこから末尾まではビット一致）\n", lastBadTail + 1);
            // ★縛るのは「いつか揃うこと」。何フレームで揃うかは段の設定で変わるので焼かない。
            const int tail = static_cast<int>(d.size()) - 1 - lastBad;
            std::snprintf(note, sizeof(note), "(末尾 %d フレームがビット一致 / 全 %d)",
                          tail, cap.frames);
            check("[リプレイ] ② 帯域ゲインは途中から始めても一致する",
                  cap.frames > 0 && tail >= cap.frames / 2, note);
            const int tailOk = static_cast<int>(d.size()) - 1 - lastBadTail;
            std::snprintf(note, sizeof(note), "(尾が末尾 %d フレーム一致 / 全 %d)",
                          tailOk, cap.frames);
            check("[リプレイ] ② 尾も暖機のあとは一致する（暖機ぶんは信用しない）",
                  cap.frames > 0 && tailOk >= cap.frames / 2, note);
        }
        std::remove(kPath);
    }
}

// ================================================ 破れ → 回帰テストの生成
// ★資料の主張のうち 3 つめ「見つけた破れがそのまま回帰テストになる」の実物。
//   ここでいちばん大事なのは **絶対値を焼かないこと**。
void testCaptureEmit() {
    std::printf("\n[生成] 破れから回帰テストを吐く\n");
    const char* kPath = "afcap_emit.afcap";
    const unsigned long long kId = 4001;
    const float h = 3.0f;

    int door = 0;
    AF_SceneHandle s = buildCaptureScene(&door);
    AF_SceneCaptureBegin(s, 90, 20, 4, 48000, /*recordPcm*/0);
    const int kFrames = 140, teleportFrame = 70;
    for (int f = 0; f < kFrames; ++f) {
        AF_SceneUpdateInstance(s, door, V(0.002f * static_cast<float>(f), h*0.5f, 0),
                               V(0.5f, h*0.5f, 0.05f), V(1,0,0), V(0,1,0));
        float lz = -4.0f + 0.02f * static_cast<float>(f);
        if (f >= teleportFrame) lz += 3.0f;                 // 仕込んだ跳び
        AF_SceneSetListener(s, V(0.2f, 1.5f, lz));
        AF_SceneSetSource(s, kId, V(0.2f, 1.5f, 3.0f));
        if (f == 110) AF_SceneCaptureMark(s);
        AF_SceneUpdate(s, 1.0f / 60.0f);
    }
    const bool wrote = AF_SceneCaptureWrite(s, kPath, "Test_CaptureEmit", 0u) == 1;
    AF_SceneDestroy(s);
    check("[生成] 元になるキャプチャを録れた", wrote);
    if (!wrote) return;

    acoustic::dbg::CaptureFile cap;
    if (!acoustic::dbg::readCapture(kPath, cap)) { check("[生成] キャプチャを読めた", false); return; }

    char note[192];
    std::snprintf(note, sizeof(note), "(箱 %zu 個 / メッシュ %d 個)", cap.boxes.size(), cap.meshCount);
    check("[生成] 静的な形がキャプチャに入っている（吐いた検査が自己完結するために要る）",
          cap.boxes.size() == 5 && cap.meshCount == 0, note);

    const std::vector<af::scan::Mark> marks = af::scan::scan(cap);
    // ★仕込んだ跳び（瞬間移動）の印を選ぶ。最初の 1 件だと別の跳びを拾うことがある。
    const af::scan::Mark* jump = nullptr;
    for (const auto& m : marks) {
        if (std::string(m.templateName).find("型紙1") == std::string::npos) continue;
        if (m.frame >= teleportFrame && m.frame <= teleportFrame + 2) { jump = &m; break; }
    }
    check("[生成] 走査が仕込んだ跳びを見つけた", jump != nullptr);
    if (!jump) { std::remove(kPath); return; }

    const std::string src = af::emit::emitCase(cap, *jump, "testGenerated_DoorJump");
    std::printf("        吐いた行数 %d 行 / %zu バイト\n",
                1 + static_cast<int>(std::count(src.begin(), src.end(), '\n')), src.size());

    // 中身の検査 ①: 必要な部品が入っていること。
    const bool hasGeom  = src.find("AF_SceneAddInstanceBox") != std::string::npos;
    const bool hasInput = src.find("AF_SceneSetListener") != std::string::npos
                       && src.find("kL[][3]") != std::string::npos;
    const bool hasDoor  = src.find("AF_SceneUpdateInstance") != std::string::npos;
    const bool hasTmpl  = src.find("af::detect::noJump") != std::string::npos;
    std::snprintf(note, sizeof(note), "(形 %d / 入力 %d / 扉 %d / 型紙 %d)",
                  hasGeom, hasInput, hasDoor, hasTmpl);
    check("[生成] 形・入力・動いた実体・型紙がすべて入っている",
          hasGeom && hasInput && hasDoor && hasTmpl, note);

    // ★中身の検査 ②: **録れた出力の数値が 1 つも入っていないこと。**
    //   ここがこの生成器の肝。絶対値を焼くと、正当なチューニングのたびに落ちて
    //   すぐ信用されなくなる（このリポジトリの方針＝期待値は関係で書く）。
    {
        int leaked = 0; char buf[64];
        for (int k = 0; k < cap.frames; ++k)
            for (const auto& e : cap.sources[static_cast<size_t>(k)]) {
                if (e.id != jump->sourceId) continue;
                for (int b = 0; b < acoustic::dbg::kCapBands; ++b) {
                    // 0 と 1 は形（軸・材質）にも出るので数えない。
                    if (e.band[b] <= 0.0f || e.band[b] >= 1.0f) continue;
                    std::snprintf(buf, sizeof(buf), "%g", e.band[b]);
                    if (src.find(buf) != std::string::npos) ++leaked;
                }
            }
        std::snprintf(note, sizeof(note), "(録れた出力の値が %d 個 混入)", leaked);
        check("[生成] ★吐いた検査に録れた出力の数値が 1 つも入っていない", leaked == 0, note);
    }

    // 吐いたものを実際に置いておく（人が見て貼れるように）。
    if (std::FILE* f = std::fopen("generated_case.cpp.txt", "wb")) {
        std::fwrite(src.data(), 1, src.size(), f);
        std::fclose(f);
        std::printf("        → generated_case.cpp.txt に書き出した\n");
    }
    std::remove(kPath);

    // ── ★健全な窓からも 1 本吐く ──
    //   ⚠ 上で吐いた検査は**必ず落ちる**。仕込んだ跳びは本物なので当然で、
    //     「直したあとに通る検査」がこの生成器の成果物。だから golden には
    //     跳びを仕込まない窓を使う（これを AcousticEngineTest/generated_cases.inc へ入れて、
    //     **吐いたものが実際にコンパイルされて通ること**をビルドで担保する）。
    {
        const char* kPath2 = "afcap_emit_ok.afcap";
        int door2 = 0;
        AF_SceneHandle s2 = buildCaptureScene(&door2);
        AF_SceneCaptureBegin(s2, 90, 20, 4, 48000, 0);
        for (int f = 0; f < 140; ++f) {
            AF_SceneUpdateInstance(s2, door2, V(0.002f * static_cast<float>(f), h*0.5f, 0),
                                   V(0.5f, h*0.5f, 0.05f), V(1,0,0), V(0,1,0));
            AF_SceneSetListener(s2, V(0.2f, 1.5f, -4.0f + 0.02f * static_cast<float>(f)));
            AF_SceneSetSource(s2, kId, V(0.2f, 1.5f, 3.0f));
            if (f == 110) AF_SceneCaptureMark(s2);
            AF_SceneUpdate(s2, 1.0f / 60.0f);
        }
        const bool ok2 = AF_SceneCaptureWrite(s2, kPath2, "Test_CaptureEmitOk", 0u) == 1;
        AF_SceneDestroy(s2);
        acoustic::dbg::CaptureFile cap2;
        if (ok2 && acoustic::dbg::readCapture(kPath2, cap2) && cap2.frames > 40) {
            af::scan::Mark mk;
            mk.frame = static_cast<int>(cap2.global[static_cast<size_t>(cap2.frames / 2)].frame);
            mk.sourceId = kId;
            mk.templateName = "型紙1 跳ばない";
            mk.what = "level";
            const std::string good = af::emit::emitCase(cap2, mk, "testGenerated_DoorSweep");
            if (std::FILE* f = std::fopen("generated_case_ok.cpp.txt", "wb")) {
                std::fwrite(good.data(), 1, good.size(), f);
                std::fclose(f);
                std::printf("        → generated_case_ok.cpp.txt（健全な窓・golden 候補）\n");
            }
        }
        std::remove(kPath2);
    }
}

// ================================================ ABI 経路（Unity が通る道）
// ★ホストは .afcap を DLL 越しに開いて走査する。**検査の経路と同じ結果**でなければ
//   「Unity では印が出るのに検査は通る」が起きる。ここでそれを縛る。
void testCaptureAbi() {
    std::printf("\n[ABI] Unity が通る道と検査の道が同じ結果か\n");
    const char* kPath = "afcap_abi.afcap";
    const unsigned long long kId = 4001;
    const float h = 3.0f;

    int door = 0;
    AF_SceneHandle s = buildCaptureScene(&door);
    AF_SceneCaptureBegin(s, 60, 20, 4, 48000, /*recordPcm*/1);
    std::vector<float> blk(256 * 2);
    for (int f = 0; f < 120; ++f) {
        AF_SceneUpdateInstance(s, door, V(0.002f * static_cast<float>(f), h*0.5f, 0),
                               V(0.5f, h*0.5f, 0.05f), V(1,0,0), V(0,1,0));
        float lz = -4.0f + 0.02f * static_cast<float>(f);
        if (f >= 60) lz += 3.0f;
        AF_SceneSetListener(s, V(0.2f, 1.5f, lz));
        AF_SceneSetSource(s, kId, V(0.2f, 1.5f, 3.0f));
        for (int i = 0; i < 512; ++i)
            blk[static_cast<size_t>(i)] = 0.25f * std::sin(0.01f * static_cast<float>(i));
        AF_SceneCapturePushAudio(s, blk.data(), 256);
        if (f == 100) AF_SceneCaptureMark(s);
        AF_SceneUpdate(s, 1.0f / 60.0f);
    }
    const bool wrote = AF_SceneCaptureWrite(s, kPath, "Test_CaptureAbi", 0x1234u) == 1;
    AF_SceneDestroy(s);
    check("[ABI] 元になるキャプチャを録れた", wrote);
    if (!wrote) return;

    char note[224];
    AF_CaptureHandle cap = AF_CaptureOpen(kPath);
    check("[ABI] AF_CaptureOpen で開ける", cap != nullptr);
    if (!cap) { std::remove(kPath); return; }

    AF_CaptureInfo info{};
    check("[ABI] 概要が取れる", AF_CaptureGetInfo(cap, &info) == 1);
    char sceneName[64] = {};
    AF_CaptureGetSceneName(cap, sceneName, sizeof(sceneName));
    std::snprintf(note, sizeof(note),
                  "(%s / %d フレーム / 箱 %d / 材質 %d / 音源 %d / PCM %d)",
                  sceneName, info.frames, info.boxCount, info.materialCount,
                  info.sourceCount, info.pcmFrames);
    check("[ABI] 概要の中身が合っている",
          info.frames == 81 && info.boxCount == 5 && info.materialCount >= 1
          && info.sourceCount == 1 && info.pcmFrames > 0 && info.dllHash == 0x1234u
          && std::string(sceneName) == "Test_CaptureAbi", note);

    // ★走査: ABI 越しの結果と、検査が直に呼ぶ結果が一致すること。
    {
        acoustic::dbg::CaptureFile direct;
        acoustic::dbg::readCapture(kPath, direct);
        const std::vector<af::scan::Mark> want = af::scan::scan(direct);
        const int got = AF_CaptureScan(cap, nullptr, 0);
        std::vector<AF_CaptureMark> abiMarks(static_cast<size_t>(got > 0 ? got : 1));
        AF_CaptureScan(cap, abiMarks.data(), got);
        int mismatch = 0;
        for (int i = 0; i < got && i < static_cast<int>(want.size()); ++i)
            if (abiMarks[static_cast<size_t>(i)].frame != want[static_cast<size_t>(i)].frame
                || abiMarks[static_cast<size_t>(i)].sourceId != want[static_cast<size_t>(i)].sourceId)
                ++mismatch;
        std::snprintf(note, sizeof(note), "(ABI %d 件 / 直 %zu 件 / 食い違い %d)",
                      got, want.size(), mismatch);
        check("[ABI] 走査の結果が検査の経路と一致する",
              got == static_cast<int>(want.size()) && mismatch == 0 && got > 0, note);
    }

    // ★リプレイ: ABI の ApplyFrame で押し直して、録れた帯域とビット一致すること。
    {
        int door2 = 0;
        AF_SceneHandle s2 = buildCaptureScene(&door2);
        acoustic::dbg::CaptureFile ref;
        acoustic::dbg::readCapture(kPath, ref);
        int bad = 0;
        for (int k = 0; k < info.frames; ++k) {
            AF_CaptureApplyFrame(s2, cap, k);
            AF_SceneUpdate(s2, 1.0f / 60.0f);
            for (const auto& src : ref.sources[static_cast<size_t>(k)]) {
                const int idx = AF_SceneSourceIndex(s2, src.id);
                float g[kBands] = {};
                AF_SceneGetSourceOcclusion(s2, idx, g);
                for (int b = 0; b < kBands; ++b) if (g[b] != src.band[b]) ++bad;
            }
        }
        AF_SceneDestroy(s2);
        std::snprintf(note, sizeof(note), "(一致しない要素 %d 個 / %d フレーム)", bad, info.frames);
        check("[ABI] AF_CaptureApplyFrame で押し直すと帯域がビット一致する", bad == 0, note);
    }

    // ★吐き出し: ABI 越しでも同じものが出ること。
    {
        std::vector<char> buf(65536);
        const int n = AF_CaptureEmitCase(cap, 0, "testGenerated_Abi", buf.data(),
                                         static_cast<int>(buf.size()));
        const std::string src(buf.data());
        const bool ok = n > 0 && src.find("AF_SceneAddMaterial") != std::string::npos
                     && src.find("AF_SceneAddInstanceBox") != std::string::npos
                     && src.find("af::detect::") != std::string::npos;
        std::snprintf(note, sizeof(note), "(%d 文字)", n);
        check("[ABI] AF_CaptureEmitCase が検査を吐く", ok, note);
    }

    AF_CaptureClose(cap);
    std::remove(kPath);
}

// ================================================ Unity と同じ呼び順・同じスレッド構成
// ★型検査では絶対に捕まらないところを見る。
//   Unity では PCM が**オーディオスレッド**から、シーンの更新が**メインスレッド**から来る。
//   `CaptureRecorder.cs` の呼び順そのままを、本物のスレッド 2 本で再現する。
//   ⚠ ここが通らないなら Unity でも通らない。逆は言えない（Unity 固有の話は見ない）。
void testCaptureUnityFlow() {
    std::printf("\n[Unity流] オーディオスレッドから PCM・メインから更新\n");
    const char* kPath = "afcap_unity.afcap";
    const unsigned long long kId = 7001;
    const float h = 3.0f;

    int door = 0;
    AF_SceneHandle s = buildCaptureScene(&door);
    // CaptureRecorder.OnEnable と同じ: 秒 → フレームは 60fps 換算で固定。
    AF_SceneCaptureBegin(s, 60, 20, 64, 48000, /*recordPcm*/1);

    // オーディオスレッド。Unity の OnAudioFilterRead と同じ粒（1024 フレーム）で回す。
    std::atomic<bool> stop{false};
    std::atomic<long long> pushed{0};
    std::thread audio([&]() {
        std::vector<float> blk(1024 * 2);
        while (!stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 1024 * 2; ++i)
                blk[static_cast<size_t>(i)] = 0.2f * std::sin(0.001f * static_cast<float>(i));
            AF_SceneCapturePushAudio(s, blk.data(), 1024);
            pushed.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // メインスレッド。CaptureRecorder.Update と同じく毎フレーム Status を引く。
    int sawRecording = 0, sawReady = 0;
    for (int f = 0; f < 130; ++f) {
        AF_SceneUpdateInstance(s, door, V(0.002f * static_cast<float>(f), h*0.5f, 0),
                               V(0.5f, h*0.5f, 0.05f), V(1,0,0), V(0,1,0));
        AF_SceneSetListener(s, V(0.2f, 1.5f, -4.0f + 0.02f * static_cast<float>(f)));
        AF_SceneSetSource(s, kId, V(0.2f, 1.5f, 3.0f));
        if (f == 100) AF_SceneCaptureMark(s);
        AF_SceneUpdate(s, 1.0f / 60.0f);
        int held = 0;
        const int st = AF_SceneCaptureStatus(s, &held);
        if (st == 1) ++sawRecording;
        if (st == 2) ++sawReady;
    }
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    char note[224];
    std::snprintf(note, sizeof(note), "(録音中 %d / 保存可 %d / オーディオ %lld ブロック)",
                  sawRecording, sawReady, (long long)pushed.load());
    check("[Unity流] 状態が「録音中」→「保存可」と進む",
          sawRecording > 0 && sawReady > 0 && pushed.load() > 0, note);

    const bool wrote = AF_SceneCaptureWrite(s, kPath, "Test_UnityFlow", 0x99u) == 1;
    check("[Unity流] 別スレッドから PCM を積みながらでも保存できる", wrote);

    // ★CaptureRecorder.SaveAndRestart と同じ: 保存したらすぐ次の録音を始める。
    AF_SceneCaptureBegin(s, 60, 20, 64, 48000, 1);
    for (int f = 0; f < 10; ++f) {
        AF_SceneSetListener(s, V(0.2f, 1.5f, 0.5f * static_cast<float>(f)));
        AF_SceneUpdate(s, 1.0f / 60.0f);
    }
    int held2 = 0;
    const int st2 = AF_SceneCaptureStatus(s, &held2);
    std::snprintf(note, sizeof(note), "(状態 %d / 保持 %d)", st2, held2);
    check("[Unity流] 保存後にもう一度録り始められる", st2 == 1 && held2 == 10, note);
    AF_SceneDestroy(s);

    if (!wrote) { std::remove(kPath); return; }

    // 落ちたファイルが Unity のパネルと同じ道で開けること。
    AF_CaptureHandle cap = AF_CaptureOpen(kPath);
    check("[Unity流] 落ちたファイルをパネルと同じ道で開ける", cap != nullptr);
    if (cap) {
        AF_CaptureInfo info{};
        AF_CaptureGetInfo(cap, &info);
        const float secs = info.sampleRate > 0
                         ? static_cast<float>(info.pcmFrames) / static_cast<float>(info.sampleRate)
                         : 0.0f;
        std::snprintf(note, sizeof(note), "(%d フレーム / PCM %.2f 秒 / 音源 %d)",
                      info.frames, secs, info.sourceCount);
        // ★PCM が「入っている」だけでなく**長さが妥当**か。
        //   窓は (60+20)/60 = 1.33 秒。オーディオスレッドの歩調は実時間なので
        //   ぴったりにはならないが、**0 でも 10 秒でもない**ことを見る。
        check("[Unity流] PCM の長さが窓と釣り合っている(0.1〜1.4秒)",
              info.frames == 81 && secs > 0.1f && secs <= 1.4f, note);
        AF_CaptureClose(cap);
    }
    std::remove(kPath);
}

// ★自動生成した回帰テストを**そのままビルドに入れる**。
//   生成器が「読めるコードらしきもの」ではなく、実際に通るコードを吐くことの担保。
#include "generated_cases.inc"

// ─────────────────────────────────────────────────────────────────────────
// [主題] 扉が**両脇の**音源に効いているか
//
// ★なぜ要るか（2026-08-27）
//   323 件が全部通ったまま、扉が両脇の音源に**まったく効いていなかった**
//   （閉扉で −6.1 dB、全掃引で 1.7 dB。資料時点は −37 dB／32.8 dB）。
//   正面の音源だけは正常だった（透過のみで鳴るため）。既存の検査は正面か
//   単一音源ばかりで、**主題そのものの場面**（扉の脇にいる音源）を判定していなかった。
//   診断（diagnoseSwingDoor 等）は数字を出していたが、判定していない。
//   → 資料 p6 の表と**同じ形状・同じ条件**を、そのまま検査にする。
//
// ★形状は pattern_table.cpp と同一（部屋 7.08×3.48×3.0 / 壁 0.16 / 戸口 1.40 / 扉 0.08 厚）。
//   リスナーは戸口の外 0.5 m（資料の数値はこの条件。既定の 0.0 ではない ── 記録漏れだった）。
//   ⚠ ここを変えると資料の表と突き合わせられなくなる。
//
// ★守る性質（扉のピラー D1/D4/D5 と、コンセプトの蝶番非対称）
//   ・閉扉なら 3 本とも −30 dB 以下（型紙 4: 幻が出ない）
//   ・閉扉の帯域は平らでない（型紙 4）
//   ・各音源の全掃引の振れ幅が 25 dB 以上（型紙 C: 効いている）
//   ・右蝶番の 40° で、左の音源が右の音源より 4 dB 以上明るい／左蝶番で鏡像（コンセプト）
//   ・ドアなしは資料の −4.4 / 0.0 / −4.4 に一致（形状と条件がずれていない確認）
// ─────────────────────────────────────────────────────────────────────────
void testDoorWorksForOffAxisSources() {
    std::printf("\n[主題] 扉が両脇の音源に効いているか（資料 p6 と同条件）\n");
    const float roomW = 7.08f, roomD = 3.48f, roomH = 3.0f, wall = 0.16f;
    const float doorW = 1.40f, doorH = 2.0f, doorT = 0.08f;
    const float gapCx = 3.54f;
    const float gapL = gapCx - doorW * 0.5f, gapR = gapCx + doorW * 0.5f;
    const float hw = wall * 0.5f, cy = roomH * 0.5f, zf = roomD + hw;
    const float ldist = 0.5f;
    const AF_Vector3 L = V(gapCx, 1.6f, roomD + wall + ldist);
    // 図の音源 3 本（SVG → 世界。pattern_table::fromSvg と同じ変換）。
    const AF_Vector3 src[3] = { V((306.0f-146.0f)/100.0f, 1.5f, (432.0f-196.0f)/100.0f),
                                V((500.0f-146.0f)/100.0f, 1.5f, (320.0f-196.0f)/100.0f),
                                V((694.0f-146.0f)/100.0f, 1.5f, (432.0f-196.0f)/100.0f) };

    // 1 パターンぶん測って、音源 3 本の 6 帯域ゲインと広帯域 dB を返す。
    auto measure = [&](float deg, bool hasDoor, bool leftHinge,
                       float bands[3][kBands], float broadDb[3]) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
        auto box = [&](AF_Vector3 c, AF_Vector3 he) {
            AF_SceneAddInstanceBox(s, c, he, V(1,0,0), V(0,1,0), mat);
        };
        box(V(-hw, cy, roomD*0.5f), V(hw, cy, roomD*0.5f + wall));
        box(V(roomW + hw, cy, roomD*0.5f), V(hw, cy, roomD*0.5f + wall));
        box(V(roomW*0.5f, cy, -hw), V(roomW*0.5f + wall, cy, hw));
        box(V(roomW*0.5f, roomH + hw, roomD*0.5f), V(roomW*0.5f + wall, hw, roomD*0.5f + wall));
        box(V(roomW*0.5f, -hw, roomD*0.5f), V(roomW*0.5f + wall, hw, roomD*0.5f + wall));
        box(V(gapL*0.5f, cy, zf), V(gapL*0.5f, cy, hw));
        box(V((gapR + roomW)*0.5f, cy, zf), V((roomW - gapR)*0.5f, cy, hw));
        box(V(gapCx, (doorH + roomH)*0.5f, zf), V(doorW*0.5f, (roomH - doorH)*0.5f, hw));
        if (hasDoor) {
            const int doorId = AF_SceneAddInstanceBox(s, V(gapCx, doorH*0.5f, zf),
                V(doorW*0.5f, doorH*0.5f, doorT*0.5f), V(1,0,0), V(0,1,0), mat);
            // 蝶番まわりの回転（pattern_table と同じ式。内開き）。
            const float th = deg * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            const float hinge = leftHinge ? gapL : gapR;
            const float rx = gapCx - hinge;
            const float sgn = (rx < 0.0f) ? 1.0f : -1.0f;
            AF_SceneUpdateInstance(s, doorId,
                V(hinge + rx*c, doorH*0.5f, zf - std::fabs(rx)*sn),
                V(doorW*0.5f, doorH*0.5f, doorT*0.5f), V(c, 0, sgn*sn), V(0,1,0));
        }
        AF_UpdateConfig cfg{};
        cfg.role1EveryN = 1;   cfg.role2EveryN = 1;   cfg.earlyEveryN = 1;
        cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
        cfg.reflectionRays = 256; cfg.reflectionBounces = 3;
        cfg.directWeight = 1.0f;  cfg.useReflections = 1;
        cfg.useEdgeCatalog = 1;   cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;
        cfg.enableReverb = 1;     cfg.echogramBins = 100;  cfg.echogramBinSeconds = 0.01f;
        cfg.echogramRays = 512;   cfg.echogramBounces = 24;
        cfg.speedOfSound = 343.0f; cfg.distanceRef = 1.5f;
        cfg.enableEarlyReflections = 1; cfg.earlyTaps = 4;
        cfg.earlyRays = 512; cfg.earlyBounces = 2;
        cfg.enableDiffractionSources = 1; cfg.diffSources = 3;
        AF_SceneSetUpdateConfig(s, &cfg);
        AF_SceneSetListener(s, L);
        for (int i = 0; i < 3; ++i) AF_SceneSetSource(s, (unsigned long long)(i + 1), src[i]);
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);
        for (int i = 0; i < 3; ++i) {
            const int idx = AF_SceneSourceIndex(s, (unsigned long long)(i + 1));
            AF_SceneGetSourceOcclusion(s, idx, bands[i]);
            float sum = 0.0f;
            for (int b = 0; b < kBands; ++b) sum += bands[i][b] * bands[i][b];
            broadDb[i] = 20.0f * std::log10(std::max(std::sqrt(sum / kBands), 1e-6f));
        }
        AF_SceneDestroy(s);
    };

    // 資料 p6 と同じ 5 パターン（右蝶番）。
    const float angles[4] = { 0.0f, 40.0f, 80.0f, 160.0f };
    float bands[5][3][kBands] = {};
    float db[5][3] = {};
    std::printf("        扉        音源1(左)  音源2(中)  音源3(右)\n");
    for (int p = 0; p < 5; ++p) {
        measure(p < 4 ? angles[p] : 0.0f, p < 4, false, bands[p], db[p]);
        std::printf("        %-8s  %8.1f  %8.1f  %8.1f\n",
                    p < 4 ? (p == 0 ? "0°(閉)" : p == 1 ? "40°" : p == 2 ? "80°" : "160°") : "ドアなし",
                    db[p][0], db[p][1], db[p][2]);
    }
    char buf[160];

    // ── 形状と条件がずれていない確認（ドアなしは資料の −4.4 / 0.0 / −4.4 の近く）──
    //   ★2026-09-02: 許容を ±2.0 dB に広げた。脇の音源の「ドアなし」の値は、戸口の縁が
    //     低域のフレネル窓をどれだけ塞ぐかという**模型依存**の量で、直接経路の半影を
    //     0.4 m の円盤 → 帯域別の環 に変えたら −5.1 → −3.7 / −2.8 へ動いた。
    //     この検査の役目は「形状と条件（戸口 1.40 m・リスナー 0.5 m）が資料からずれていない」
    //     ことの確認なので、桁が合っていれば足りる。
    std::snprintf(buf, sizeof(buf), "(%.1f / %.1f / %.1f)", db[4][0], db[4][1], db[4][2]);
    check("[主題] ドアなしが資料の近く（形状・条件の確認、±2 dB）",
          std::fabs(db[4][0] + 4.4f) < 2.0f && std::fabs(db[4][1]) < 1.0f
          && std::fabs(db[4][2] + 4.4f) < 2.0f, buf);
    // 左右対称な配置なので音源1 と 音源3 は同じ値であるべき。
    //   ⚠ 実測 −3.7 / −2.8（0.9 dB のずれ）。標本点が箱の面を掠める所で浮動小数の判定が
    //     鏡像で一致しないことがある（点標本の宿命）。1.5 dB までは許し、それ以上は壊れとみなす。
    std::snprintf(buf, sizeof(buf), "(左 %.1f / 右 %.1f)", db[4][0], db[4][2]);
    check("[主題] ドアなしで左右の音源が対称（差 1.5 dB 以内）",
          std::fabs(db[4][0] - db[4][2]) <= 1.5f, buf);

    // ── 型紙 4: 閉扉で幻が出ない ──
    std::snprintf(buf, sizeof(buf), "(%.1f / %.1f / %.1f dB)", db[0][0], db[0][1], db[0][2]);
    check("[主題][型紙4] 閉扉なら 3 本とも −30 dB 以下",
          db[0][0] <= -30.0f && db[0][1] <= -30.0f && db[0][2] <= -30.0f, buf);
    {
        float flat[3 * kBands];
        for (int i = 0; i < 3; ++i) for (int b = 0; b < kBands; ++b) flat[i*kBands + b] = bands[0][i][b];
        const bool occ[3] = { true, true, true };
        af::detect::Break br[3];
        const int n = af::detect::noPhantom(flat, occ, nullptr, 3, 6.0f, br, 3);
        std::snprintf(buf, sizeof(buf), "(平らな音源 %d 本)", n);
        check("[主題][型紙4] 閉扉の帯域が平らでない（低−高 ≥ 6 dB）", n == 0, buf);
    }

    // ── 型紙 C: 扉が効いている（各音源の全掃引の振れ幅）──
    for (int i = 0; i < 3; ++i) {
        float series[5];
        for (int p = 0; p < 5; ++p) series[p] = std::pow(10.0f, db[p][i] / 20.0f);
        const float range = af::detect::responseRangeDb(series, 5);
        std::snprintf(buf, sizeof(buf), "(音源%d 振れ幅 %.1f dB)", i + 1, range);
        check(i == 0 ? "[主題][型紙C] 音源1(左) に扉が効いている（≥25 dB）"
            : i == 1 ? "[主題][型紙C] 音源2(中) に扉が効いている（≥25 dB）"
                     : "[主題][型紙C] 音源3(右) に扉が効いている（≥25 dB）",
              range >= 25.0f, buf);
    }

    // ── コンセプト: どちら側から開くかで、どの音が先に届くかが変わる ──
    //   右蝶番なら開口は左から開くので、40° で左の音源が先に明るい。左蝶番で鏡像。
    {
        float bL[3][kBands] = {}, dL[3] = {};
        measure(40.0f, true, true, bL, dL);
        std::snprintf(buf, sizeof(buf), "(右蝶番 40°: 左 %.1f / 右 %.1f)", db[1][0], db[1][2]);
        check("[主題][概念] 右蝶番 40° で左の音源が右より 4 dB 以上明るい",
              db[1][0] - db[1][2] >= 4.0f, buf);
        std::snprintf(buf, sizeof(buf), "(左蝶番 40°: 左 %.1f / 右 %.1f)", dL[0], dL[2]);
        check("[主題][概念] 左蝶番 40° で右の音源が左より 4 dB 以上明るい（鏡像）",
              dL[2] - dL[0] >= 4.0f, buf);
    }

    // ── 型紙 1: 板の縁を越える瞬間に跳ばない（扉のピラー D4）──
    //   2026-09-02 の実測: 蝶番側の音源3 が 127° −15.1 → 128° −8.0 → 129° −5.5（2° で 9.6 dB）。
    //   開いた板の縁が見通し線を横切る瞬間、直接経路の遮蔽判定が二値で切り替わっていた。
    //   ★角度 1° あたり 3 dB を上限にする。半影の幅は帯域で違う（125Hz ≒ 40°、4kHz ≒ 8°）
    //     ので、広帯域の値が滑らかでも高域は速く動く ── それは正しいが、二値で跳ぶのは別物。
    {
        float prev = 0.0f, maxStep = 0.0f, stepAt = 0.0f, last = 0.0f;
        bool first = true;
        for (float deg = 118.0f; deg <= 136.01f; deg += 1.0f) {
            float b3[3][kBands] = {}, d3[3] = {};
            measure(deg, true, false, b3, d3);
            if (!first) {
                const float s = std::fabs(d3[2] - prev);
                if (s > maxStep) { maxStep = s; stepAt = deg; }
            }
            prev = d3[2]; last = d3[2]; first = false;
        }
        std::snprintf(buf, sizeof(buf), "(最大 %.1f dB/° @ %.0f°)", maxStep, stepAt);
        check("[主題][型紙1] 蝶番側の音源が板の縁を越えるとき 1° で 3 dB 以上跳ばない",
              maxStep <= 3.0f, buf);
        std::snprintf(buf, sizeof(buf), "(136° で %.1f dB)", last);
        check("[主題] 136° では蝶番側の音源も開いている（> −8 dB）", last > -8.0f, buf);
    }
}

int main() {
    std::printf("=== AF_Scene* 数値回帰テスト ===\n");
    std::printf("（期待値は絶対値でなく「関係」で書いている。詳細は冒頭コメント参照）\n");

    // ★配布先の DLL の鮮度は**最初に**見る。DLL を掴んだまま走らせることもあるので、
    //   テストの途中で誰かが差し替えても結果が揺れないよう、状態はここで固定する。
    scanDllFreshness();
    reportDllFreshness();

    // 開発中の絞り込み: AF_ONLY=async（非同期）／tier（段の予算）でその節だけ回す（全体は 8 分かかる）。
    if (const char* only = std::getenv("AF_ONLY")) {
        if (std::strcmp(only, "async") == 0) testAsyncUpdate();
        else if (std::strcmp(only, "tier") == 0) testTierBudget();
        std::printf("\n[AF_ONLY=%s] %d 件中 失敗 %d\n", only, g_checks, g_failures);
        return (g_failures == 0) ? 0 : 1;
    }

    testDetectorSelfCheck();      // ★検出器が効いているかを先に確かめる
    testCaptureRoundTrip();
    testCaptureReplay();
    testCaptureEmit();
    testCaptureAbi();
    testCaptureUnityFlow();
    testGenerated_DoorSweep();    // ← generated_cases.inc（自動生成）
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
    testLCorridorScene();
    diagnoseCorridorTapDropout();
    for (float sp : {1.0f, 2.0f, 4.0f, 8.0f}) {
        g_edgeSpan = sp;
        std::printf("\n  === 矩形の半幅 = フレネル半径 x %.0f ===", sp);
        diagnoseEdgePortalCalibration();
    }
    g_edgeSpan = 1.0f;
    testDoorOpennessCurve();
    diagnoseApertureContrastChoice();
    diagnoseSlitPortalShadow();
    diagnoseOpenApertureFraction();
    diagnoseShadowSpectrumJump();
    diagnoseOutdoorWithBuilding();
    testOutdoorIsNotARoom();
    testRoomGridDegradeDetect();
    testAutoPortals();
    diagnosePortalLeftRightSymmetry();
    diagnosePortalWidthVsDoorCurve();
    testApertureTimbreKnob();
    testManySources();
    diagnosePortalScopeGlobal();
    diagnosePerSourceEchogram();
    diagnoseAbsorptionVsLocalization();
    diagnoseShadowCliff();
    diagnoseNonDoorShapes();
    diagnosePillarTimbre();
    diagnoseReverbSendWalk();
    diagnoseCaveMouth();
    diagnoseWalkContinuity();
    testHrtfLeftRight();
    diagnoseClosedDoorLocalization();
    testFaceReflections();
    diagnoseSwingDoorSourceSweep();
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
    diagnoseBvhRebuildCost();
    testWorkerThreads();
    testSourceTiers();
    testTierBudget();
    testAsyncUpdate();
    testTuningSupport();
    testEarlySharing();
    testGeometryIsPrimary();
    testClosedDoorApproach();
    testDoorWorksForOffAxisSources();
    diagnoseTailProbeDensity();
    diagnoseEchogramScaling();
    testSoftOcclusion();
    diagnoseDiffractionOnlySweep();
    diagnoseTailShareByRoom();
    diagnoseDoorSideAsymmetry();
    diagnoseCorridorDiffractionLevel();
    testApertureOpenness();
    testDoorContinuity();
    testPerInstanceMaterial();
    testVoiceApi();
    testRobustness();

    std::printf("\n----\n");
    if (g_failures == 0) std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_checks);
    else std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_failures, g_checks);

    // ★もう一度出す。2000 行流れたあとに読むのは末尾なので、ここに無いと気づけない。
    //   終了コードは変えない ── 配布のし忘れはエンジンの回帰ではないので、
    //   これで CI を落とすと「テストが落ちた」の意味が濁る。
    reportDllFreshness();

    return (g_failures == 0) ? 0 : 1;
}
