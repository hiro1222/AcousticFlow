/* Core/maekawa.h ── 前川の回折減衰（音声経路で実際に使う回折モデル）
 *
 * 理論: 前川純一 (1968) "Noise reduction by screens"。フレネル数
 *     N = 2δ/λ   （δ = 迂回経路長 − 直線経路長, λ = 波長）
 *   だけで減衰量を与える実験式。
 *
 * ── なぜ UTD ではなくこちらを音声経路に使うか ────────────────────────
 *   UTD(Core/utd.h) は厳密解だが、回折点 P の 3D 幾何（入射角φ'・回折角φ・
 *   ウェッジ角 n・Kellerコーン角β0）に依存する。複数の稜線が候補になる形状では
 *   どの稜線が最短かが位置によって入れ替わり、そのたびに幾何が不連続に変わるため
 *   ゲインが飛ぶ。実測で最大 7.9dB の段差が出た（下記の回帰テスト参照）。
 *   重み付き平均・複素和・方向クラスタ＋エネルギー加算をいずれも試したが、
 *   UTD は各稜線を「孤立した無限ウェッジ」として評価するので、
 *   正しく重ね合わせようとするほど過大計上になり悪化した。
 *
 *   一方 δ は「候補すべての最小値」を取っても連続である（min は連続関数を保つ。
 *   argmin が入れ替わっても値そのものは飛ばない）。したがって δ だけに依存する
 *   このモデルは、平滑化を一切せずに連続性が構造的に保証される。
 *
 *   加えて N = 2δ/λ は波長で割るので、高域ほど N が大きく減衰が強い。
 *   6帯域ゲインとして返せば、そのままローパス（回折によるハイ落ち）になる。
 *
 *   厳密さより連続性を優先するのは実際の商用実装と同じ判断（Wwise は回折角→
 *   減衰カーブ、Project Acoustics はオフラインの波動解を焼き込み）。
 *   UTD は検証・比較用に Core/utd.h に残してある。
 *
 * δ は符号付きで渡すこと:
 *     影の側     δ > 0
 *     影境界     δ = 0   → 両側とも 5dB 減衰に一致するので連続
 *     照らされた側 δ < 0
 *   影境界では直線が稜線を掠めるので δ→0 になり、符号だけが反転する。
 */
#ifndef ACOUSTICFLOW_CORE_MAEKAWA_H
#define ACOUSTICFLOW_CORE_MAEKAWA_H

#include <cmath>

#include "Core/material.h"  // kNumBands

namespace acoustic {
namespace maekawa {

constexpr float kPi = 3.14159265358979f;
constexpr float kSpeed = 343.0f;

// 実測曲線の上限。前川の式は N→∞ で無限に減衰するが、実際の遮蔽体では
// 24dB 程度で頭打ちになる（透過・二次反射が支配的になるため）。
constexpr float kMaxAttenDb = 24.0f;

// フレネル数 N に対する減衰量(dB)。N は符号付き（影で正、照射側で負）。
//     N ≥ 0   : A = 5 + 20log10( √(2πN) / tanh√(2πN) )
//     N <  0  : A = 5 + 20log10( √(2π|N|) / tan √(2π|N|) )   （√ が虚数になり tanh→tan）
//     N < -0.2: A = 0   （影境界から十分離れた照射側。回折の影響なし）
//   N→0 では √(2πN)/tanh√(2πN) → 1 なので両側とも A=5dB に収束し、連続。
//   cap=false は「開口どうしの優劣を付ける」用。
//     上限で頭打ちにすると遠い開口が全部 24dB で同点になり、支配開口が薄まって
//     **定位がぼやける**（本作で優先するのは遮蔽量の精度より回折点への定位）。
//     出力ゲインには cap=true、重み付けには cap=false を使い分ける。
inline float attenuationDb(float N, bool cap = true) {
    if (N < -0.2f) return 0.0f;
    const float x = std::sqrt(2.0f * kPi * std::fabs(N));
    if (x < 1e-4f) return 5.0f;  // N≒0。下の式は 0/0 になるので極限値を直接返す。
    const float ratio = (N >= 0.0f) ? (x / std::tanh(x)) : (x / std::tan(x));
    if (!(ratio > 1e-6f)) return 0.0f;  // 照射側で tan が負/発散する領域の保険
    const float a = 5.0f + 20.0f * std::log10(ratio);
    if (a < 0.0f) return 0.0f;
    if (!cap) return a;
    return (a > kMaxAttenDb) ? kMaxAttenDb : a;
}

// 符号付き δ(m) から 6帯域の相対ゲイン(0..1)を返す。
//   directGain が非 null なら、照射側で「直接音がまだ届いている分」との合成に使わず、
//   純粋に回折による減衰率として返す（呼び出し側で透過などと合成する）。
inline void gainBands(float deltaSigned, float outGain[kNumBands], bool cap = true) {
    static const float kBandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
    for (int b = 0; b < kNumBands; ++b) {
        const float lambda = kSpeed / kBandFreq[b];
        const float N = 2.0f * deltaSigned / lambda;
        outGain[b] = std::pow(10.0f, -attenuationDb(N, cap) / 20.0f);
    }
}

// 開口どうしの優劣づけ用の重み（上限で頭打ちにしないので、遠い開口が同点にならない）。
// 低域加重なのは、回折で回り込むのは低域だから。
inline float apertureWeight(float delta) {
    float g[kNumBands];
    gainBands(delta, g, /*cap=*/false);
    const float w[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
    float gs = 0.0f, ws = 0.0f;
    for (int b = 0; b < kNumBands; ++b) { gs += w[b] * g[b]; ws += w[b]; }
    return ws > 0.0f ? gs / ws : 0.0f;
}

}  // namespace maekawa
}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_MAEKAWA_H
