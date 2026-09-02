/* detectors.h ── 「破れの型紙」。**検出器と回帰テストが同じ式**であるための共有ヘッダ。
 *
 * ★なぜ 1 箇所に置くのか
 *   サウンドデバッグツール（docs/SOUND_DEBUG_TOOL.md）は、録ったキャプチャを走査して
 *   破れを見つけ、そこから回帰テストを生やす。このとき
 *   **走査に使う式と、生えた検査が使う式が別物だと意味がない**
 *   （「道具は見つけたのに検査は通る」「検査は落ちるのに道具は黙る」が起きる）。
 *   → 型紙をここに置き、走査も検査も同じ関数を呼ぶ。
 *
 * ★型紙は「思いついたもの」ではなく **実際に出たバグから逆算したもの**だけを置く。
 *   汎用の項目を並べると、中身の無い検査が増える。各型紙に**元になったバグ**を書くこと。
 *
 * 単位の規約:
 *   - gain は振幅比(0..1)。dB へ直すのは 20log10。
 *   - 帯域は 125/250/500/1k/2k/4k の 6 つ。
 */
#ifndef ACOUSTICFLOW_TEST_DETECTORS_H
#define ACOUSTICFLOW_TEST_DETECTORS_H

#include <cmath>
#include <cstdio>
#include <vector>

namespace af {
namespace detect {

constexpr int kBands = 6;

inline float db(float lin) { return 20.0f * std::log10(lin > 1e-6f ? lin : 1e-6f); }

/// 破れ 1 件。時刻/位置と「何が動いたか」を持つ。
///   ★時刻だけでは足りない。今日、原因を特定できたのは毎回**どの量が動いたか**が
///     見えたからで（f使用 ○→× ／ 有界 1→0 ／ 縁u 0.41→0.73）、
///     時刻だけの印なら特定できていない。
struct Break {
    int index = -1;        // 掃引の何番目 / フレーム番号
    float at = 0.0f;       // 位置 or 秒
    float amount = 0.0f;   // 破れの大きさ（dB / 比 / 度 など、型紙ごと）
    const char* what = ""; // 何の量か
};

// ─────────────────────────────────────────────────────────────────────
// 型紙 1: 跳ばない
//   元になったバグ: 閉扉で距離 10cm を跨いで 23.0dB / 19.7dB
//   ⚠ 閾値の注意: 段の更新間隔が違うので**正常でも規則的に階段になる**
//     （エコグラム 4 / 早期反射 3 / 二次音源 2 フレーム）。素朴な隣接差だと毎フレーム光る。
//     実際の本物は 19.7dB と 23.0dB だったので 6dB 前後が妥当。
// ─────────────────────────────────────────────────────────────────────
inline int noJump(const float* seriesDb, const float* at, int n, float maxStepDb,
                  Break* out, int maxOut) {
    int m = 0;
    for (int i = 1; i < n && m < maxOut; ++i) {
        const float d = std::fabs(seriesDb[i] - seriesDb[i - 1]);
        if (d > maxStepDb) out[m++] = Break{i, at ? at[i] : (float)i, d, "level"};
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 2: 向きが正しい（掃くと単調に動く）
//   元になったバグ: 開口へ近づくのに小さくなる（向きの逆転）
//   ⚠ **これ単体では定数を通してしまう。**必ず型紙 C と対で使うこと。
//   rising=true なら「index が増えるほど大きい」を期待する。
// ─────────────────────────────────────────────────────────────────────
inline int monotonic(const float* series, const float* at, int n, bool rising,
                     float slack, Break* out, int maxOut) {
    int m = 0;
    for (int i = 1; i < n && m < maxOut; ++i) {
        const float d = series[i] - series[i - 1];
        const bool bad = rising ? (d < -slack) : (d > slack);
        if (bad) out[m++] = Break{i, at ? at[i] : (float)i, std::fabs(d), "direction"};
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 3: 経路が消えない
//   元になったバグ: 2 次回折が 0.486 → 0.000／経路探索の空振り
//   ★「量が小さい」ではなく「**経路が見つからない**」を見る。量は型紙 2/C の担当。
//     絶対値で縛ると、正当に小さくなっただけで落ちる（実際にそれで一度落とした）。
// ─────────────────────────────────────────────────────────────────────
inline int pathAlive(const int* pathCount, const float* at, int n, Break* out, int maxOut) {
    int m = 0;
    for (int i = 0; i < n && m < maxOut; ++i)
        if (pathCount[i] <= 0) out[m++] = Break{i, at ? at[i] : (float)i, 0.0f, "path lost"};
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 4: 幻が出ない
//   元になったバグ: 閉扉で -13.5dB・**帯域がまっすぐ**（低−高 0.0dB）
//   ★回折も透過も周波数依存なので、**遮蔽されているのに平ら**なら模型が破れている。
//     これは汎用のプロファイラには書けない。自分の模型の破れ方を知っている人しか書けない。
//   bands は [i*kBands + b]。occluded[i] が false の行は見ない（素通しなら平らで正しい）。
// ─────────────────────────────────────────────────────────────────────
inline int noPhantom(const float* bands, const bool* occluded, const float* at, int n,
                     float minTiltDb, Break* out, int maxOut) {
    int m = 0;
    for (int i = 0; i < n && m < maxOut; ++i) {
        if (occluded && !occluded[i]) continue;
        const float lo = bands[i * kBands + 0], hi = bands[i * kBands + kBands - 1];
        if (lo < 1e-6f && hi < 1e-6f) continue;      // 無音は対象外
        const float tilt = std::fabs(db(lo) - db(hi));
        if (tilt < minTiltDb) out[m++] = Break{i, at ? at[i] : (float)i, tilt, "flat spectrum"};
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 5: 決定的（同じ入力で 2 回走らせて一致）
//   元になったバグ: 並列化で BVH が競合（遅延構築を複数スレッドが同時に走らせた）
// ─────────────────────────────────────────────────────────────────────
inline int deterministic(const float* a, const float* b, int n, float tol,
                         Break* out, int maxOut) {
    int m = 0;
    for (int i = 0; i < n && m < maxOut; ++i) {
        const float d = std::fabs(a[i] - b[i]);
        if (d > tol) out[m++] = Break{i, (float)i, d, "nondeterministic"};
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 A: 比が保たれる
//   元になったバグ: 尾だけ 8.3 倍大きかった（outputGain の掛け忘れ）。
//     **跳びでも幻でもない ── 一定して間違っている**ので、他の型紙では捕まらない。
//   num/den の比が [lo, hi] に収まることを見る。
// ─────────────────────────────────────────────────────────────────────
inline int ratioInRange(const float* num, const float* den, const float* at, int n,
                        float lo, float hi, const char* what, Break* out, int maxOut) {
    int m = 0;
    for (int i = 0; i < n && m < maxOut; ++i) {
        if (den[i] < 1e-9f) continue;
        const float r = num[i] / den[i];
        if (r < lo || r > hi) out[m++] = Break{i, at ? at[i] : (float)i, r, what};
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 B: 設定不変（速くするだけの設定を変えても音が変わらない）★今日いちばん使った
//   元になったバグ:
//     ・並列と直列がビット一致するか（音源ごとの段は一致すべき）
//     ・共有バスと個別畳み込みが一致するか（-123.5dB まで確認）
//     ・エコグラムをフレーム分割しても同じか（直接音を分割数ぶん重ねていた +18.00）
//   ★「速くするだけ」の変更は音を変えてはいけない。型紙 5 とは別物
//     （あちらは同じ設定で 2 回、こちらは**設定を変えて**）。
//   exact=true ならビット一致を、false なら相対差 tol を要求する。
// ─────────────────────────────────────────────────────────────────────
inline int invariantUnderSetting(const float* base, const float* alt, int n,
                                 bool exact, float relTol, Break* out, int maxOut) {
    int m = 0;
    for (int i = 0; i < n && m < maxOut; ++i) {
        if (exact) {
            if (base[i] != alt[i])
                out[m++] = Break{i, (float)i, std::fabs(base[i] - alt[i]), "not bit-identical"};
        } else {
            const float d = std::fabs(base[i] - alt[i]);
            const float ref = std::fabs(base[i]) > 1e-9f ? std::fabs(base[i]) : 1e-9f;
            if (d / ref > relTol) out[m++] = Break{i, (float)i, d / ref, "setting changed sound"};
        }
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 C: 効いている（掃いたときに出力がちゃんと動く）
//   元になったバグ: 隙間の幅が効かない（2m でも 0.1m でも同じ音）。
//   ★**型紙 2（単調）は定数を通してしまう。**必ず対で使うこと。
//     「単調かつ、ちゃんと動いている」で初めて「そのつまみが効いている」と言える。
//   戻り値: 実際の振れ幅(dB)。呼び出し側が minRangeDb と比べる。
// ─────────────────────────────────────────────────────────────────────
inline float responseRangeDb(const float* series, int n) {
    if (n <= 0) return 0.0f;
    float lo = series[0], hi = series[0];
    for (int i = 1; i < n; ++i) { lo = series[i] < lo ? series[i] : lo;
                                  hi = series[i] > hi ? series[i] : hi; }
    return db(hi > 1e-6f ? hi : 1e-6f) - db(lo > 1e-6f ? lo : 1e-6f);
}

// ─────────────────────────────────────────────────────────────────────
// 型紙 D: 物理の上限を超えない
//   元になったバグ: 取り分を絶対和にしたとき、柱のタップが 0.88 → **1.76**（1.0 超え）。
//   ★「配分」を「絶対量」に変えたときに踏む定番。エネルギーが増えていないかを見る。
// ─────────────────────────────────────────────────────────────────────
inline int withinPhysicalBound(const float* gains, const float* at, int n, float maxGain,
                               Break* out, int maxOut) {
    int m = 0;
    for (int i = 0; i < n && m < maxOut; ++i)
        if (gains[i] > maxGain) out[m++] = Break{i, at ? at[i] : (float)i, gains[i], "gain > bound"};
    return m;
}

/// 破れを人が読める形で出す。**時刻だけでなく「何が動いたか」を必ず出す。**
inline void report(const char* label, const Break* b, int n, const char* unit = "") {
    if (n <= 0) { std::printf("        %-28s 破れなし\n", label); return; }
    std::printf("        %-28s 破れ %d 件\n", label, n);
    for (int i = 0; i < n && i < 5; ++i)
        std::printf("          [%d] @ %.2f  %s %.3f%s\n", b[i].index, b[i].at, b[i].what,
                    b[i].amount, unit);
    if (n > 5) std::printf("          … 他 %d 件\n", n - 5);
}

}  // namespace detect
}  // namespace af

#endif  // ACOUSTICFLOW_TEST_DETECTORS_H
