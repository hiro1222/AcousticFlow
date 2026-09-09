/* Flow/response.h ── 速度の表（段 3）
 *
 * ■ 役割
 *   「何がどの速さで追うか」の時定数を 1 か所に置く。設計文書 Ⅸ「鮮明さを作る五要素（それぞれ別の速度で動く）」と
 *   追記 C の表の実体。デザイナが触る場所（地図 6 章の決めごと 5「速度は一か所」）。
 *
 * ■ 中の仕組み
 *   1) Response: 時定数の表。0 は「平滑なし＝毎フレームそのまま」。
 *        levelSec   量（ラウドネス）。0。★「ラウドネスは開き始めた瞬間に立ち上がる」（Ⅸ）
 *        colourSec  形（帯域の色）。40 ms。旧実装の実測 26〜54 ms の中
 *        statSec    レイの統計（初期・後期の総量）。50 ms。種を固定した相関標本の残りの揺れを均す。遅れの閾値 70 ms の下
 *        directionSec 方向（方向バスのレーン）。60 ms。旧 FDN のレーンで 1 ブロックだと 4.3 dB 跳んだ
 *        onsetJumpFactor 尾の開始の渡りは跳びの 4 倍（DSP 側の VoiceRenderer が使う。ここは値を持つだけ）
 *   2) Follower6: 帯域ベクトル e6 を「量」と「形」に分けて別々の速さで追う。
 *        量 L = Σ_b e_b、形 s_b = e_b / L。L は levelSec、s は colourSec で一次遅れ。出力 = L·s。
 *      ★量と形を分けるのが要点。1 つの時定数で e6 を追うと、扉を開けた瞬間の立ち上がり（量）まで鈍る。
 *        量だけ即時にして色をゆっくり追わせると、「音は一気に大きくなり、鮮明さが追いついてくる」になる（Ⅸ）。
 *      一次遅れ: y += (x − y)·(1 − exp(−dt/τ))。τ=0 なら y = x。
 *
 * ■ 繋がり
 *   受ける: 設定（ホストの Inspector、または既定値）。
 *   渡す:   distribute が Follower6 を成分ごとに持って使う。onsetJumpFactor は段 4 の橋が DSP へ渡す。
 *
 * ■ 退けた書き方
 *   ・成分ごとに時定数を書く（直接 τ、初期 τ、…）: 「量」と「形」という軸のほうが、Ⅸ の言葉（ラウドネス／鮮明さ）と
 *     一対一になる。成分による差は statSec（統計かどうか）だけで足りる。
 *   ・時定数を各ファイルの定数にする: 旧実装で追従の定数が 4 ファイルに散り、どれを触れば扉のラグが変わるか
 *     分からなくなった（docs/DOOR_LAG.md）。
 *
 * ■ 壊れる所
 *   ・levelSec を 0 以外にすると、扉を開けた瞬間の立ち上がりが鈍り、「開けたのに音が遅れて来る」になる。
 *   ・L が 0 のとき s を作ると 0 除算。L ≤ 1e-12 なら形は前の値を保つ（音が消えている間に色が壊れない）。
 *   ・τ が dt より短いと 1 − exp(−dt/τ) ≈ 1 で平滑が消えるだけ（害は無い）。負の τ は 0 と同じに扱う。
 */
#ifndef ACOUSTICFLOW_FLOW_RESPONSE_H
#define ACOUSTICFLOW_FLOW_RESPONSE_H

#include <algorithm>
#include <cmath>
#include "Core/material.h"

namespace acoustic {
namespace flow {

struct Response {
    float levelSec = 0.0f;          // 量: 毎フレーム（Ⅸ ラウドネスは瞬間に立ち上がる）
    float colourSec = 0.04f;        // 形: 40 ms（実測 26〜54 ms）
    float statSec = 0.05f;          // レイの統計: 50 ms（遅れの閾値 70 ms の下）
    float directionSec = 0.06f;     // 方向: 60 ms
    float onsetJumpFactor = 4.0f;   // 尾の開始の渡り: 跳びの 4 倍
};

/// 一次遅れの係数。τ ≤ 0 なら 1（そのまま）。
inline float followCoef(float dt, float tauSec) {
    if (tauSec <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-dt / tauSec);
}

/// 帯域ベクトルを「量」と「形」で別々に追う。
struct Follower6 {
    float level = 0.0f;
    float shape[kNumBands] = {};
    bool  primed = false;

    void reset() { level = 0.0f; primed = false; for (int b = 0; b < kNumBands; ++b) shape[b] = 0.0f; }

    /// x6 を目標に 1 フレーム進め、out6 に現在値を書く。
    void update(const float* x6, float dt, float levelSec, float shapeSec, float* out6) {
        float L = 0.0f; for (int b = 0; b < kNumBands; ++b) L += std::max(0.0f, x6[b]);
        float s[kNumBands];
        const bool hasShape = L > 1e-12f;
        for (int b = 0; b < kNumBands; ++b) s[b] = hasShape ? std::max(0.0f, x6[b]) / L : shape[b];
        if (!primed) {                       // 最初のフレームは目標をそのまま（0 からの立ち上がりで色が壊れない）
            level = L; for (int b = 0; b < kNumBands; ++b) shape[b] = s[b];
            primed = true;
        } else {
            const float kL = followCoef(dt, levelSec), kS = followCoef(dt, shapeSec);
            level += (L - level) * kL;
            if (hasShape) for (int b = 0; b < kNumBands; ++b) shape[b] += (s[b] - shape[b]) * kS;
        }
        for (int b = 0; b < kNumBands; ++b) out6[b] = level * shape[b];
    }
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_RESPONSE_H
