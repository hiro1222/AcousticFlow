/* Flow/world_rules.h ── 世界の決まり（段 1）
 *
 * ■ 役割
 *   このエンジンが「音の世界」として持つ決まりを 1 か所に置く。
 *   帯域の定義、音速、材質の表、そして「配分への世界ごとの重み」。
 *   設計文書 Ⅱ「世界の音の決まりは、ゲームに応じて設定できる」の実体（追記 F）。
 *
 * ■ 中の仕組み
 *   1) 帯域: 6 帯域の中心周波数 kBandHz（125…4k）、その境目 kCrossHz（隣り合う中心の相乗平均）、
 *      各帯域の幅 bandWidthHz。境目が相乗平均なのは、オクターブ帯域が対数軸で等間隔だから。
 *      幅は「帯域幅で重み付けした平均」に使う（白色入力のエネルギーが帯域に幅なりに散るため。
 *      旧 FDN の色の基準で、500 Hz 1 点を基準にすると同じ部屋で −2.0 dB ずれ、幅平均で −0.3 dB に収まった）。
 *   2) 材質の表: AcousticMaterial（Core/material.h。帯域別の吸音・透過・散乱、単位はエネルギー比）を
 *      番号で引く。番号は足した順。0 番は既定の壁。
 *   3) 世界の重み WorldWeights: 五成分それぞれに掛ける 1 つの係数。既定は全部 1（＝物理どおり）。
 *      デザイナが「この世界は残響が濃い」「透過は薄い」を決める場所。帯域別にはしない ──
 *      帯域の形は材質が持つ（答えは 1 つ）。
 *   4) splitAt: 面に当たったエネルギーが 反射・透過・吸収 に分かれる比。和は 1。
 *      反射 = 1 − α − τ。energy_trace はこの 3 つだけで面を処理する。
 *
 * ■ 繋がり
 *   受ける: ホストの設定（材質の登録、重み）。
 *   渡す:   帯域表は energy_trace / diffraction / distribute / probe が引く。材質は energy_trace と probe。
 *           重みは distribute だけが掛ける（他で掛けると二重になる）。
 *
 * ■ 退けた書き方
 *   ・帯域表を各ファイルに書く: 旧 scene.h には kBandHz が 3 か所あった。値が同じでも「同じ物」と
 *     読めないし、1 か所だけ直すと食い違う。
 *   ・重みを帯域別にする: 材質と役割が重なり、どちらを触れば色が変わるか分からなくなる。
 *   ・音速を設定可能にする: 変える理由が無い（水中などは別の作品）。定数のほうが「なぜ 343」を聞かれて答えやすい。
 *
 * ■ 壊れる所
 *   ・kCrossHz を変えると DSP 側のクロスオーバー（FdnTail / VoiceRenderer）と食い違い、帯域の色が
 *     ずれる。境目は必ず相乗平均から出すこと（この表を手で書き換えない）。検査「境目 ＝ 相乗平均」で守る。
 *   ・WorldWeights を distribute 以外で掛けると二乗で効く（残響が 2 倍濃くなる）。
 *   ・splitAt で α + τ > 1 の材質を素通しすると反射が負になり、レイのエネルギーが増える（発散）。
 *     τ を 1 − α で頭打ちにしてある。
 */
#ifndef ACOUSTICFLOW_FLOW_WORLD_RULES_H
#define ACOUSTICFLOW_FLOW_WORLD_RULES_H

#include <cmath>
#include <vector>
#include "Core/material.h"

namespace acoustic {
namespace flow {

// ── 帯域 ──
constexpr float kSpeedOfSound = 343.0f;                                   // m/s、20 ℃ の空気
constexpr float kBandHz[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
// 境目 = 隣り合う中心の相乗平均。√(125·250) = 176.8 … √(2000·4000) = 2828.4
constexpr float kCrossHz[kNumBands - 1] = {176.78f, 353.55f, 707.11f, 1414.21f, 2828.43f};

/// 帯域 b の幅（Hz）。最下は 0 から、最上は fs/2 まで（白色入力のエネルギーがどう散るかの重み）。
inline float bandWidthHz(int b, float sampleRate) {
    const float lo = (b == 0) ? 0.0f : kCrossHz[b - 1];
    const float hi = (b == kNumBands - 1) ? sampleRate * 0.5f : kCrossHz[b];
    return hi - lo;
}

/// 五成分の並び。配分の内訳（Mix::component6）と世界の重みの添字。
enum Component : int { kDirect = 0, kEarly = 1, kLate = 2, kDiffract = 3, kTransmit = 4, kNumComponents = 5 };

// ── 世界の重み（追記 F）──
//   五成分へ掛ける係数（エネルギー比）。既定 1 = 物理どおり。
//   ★掛けるのは distribute の出口 1 か所だけ。
struct WorldWeights {
    float w[kNumComponents] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool isIdentity() const {
        for (int i = 0; i < kNumComponents; ++i) if (std::fabs(w[i] - 1.0f) > 1e-6f) return false;
        return true;
    }
};

// ── 材質の表 ──
class MaterialTable {
public:
    MaterialTable() { materials_.push_back(AcousticMaterial::defaultWall()); }   // 0 番は既定の壁
    int add(const AcousticMaterial& m) { materials_.push_back(m); return static_cast<int>(materials_.size()) - 1; }
    const AcousticMaterial& get(int id) const {
        if (id < 0 || id >= static_cast<int>(materials_.size())) return materials_[0];
        return materials_[static_cast<std::size_t>(id)];
    }
    int count() const { return static_cast<int>(materials_.size()); }
private:
    std::vector<AcousticMaterial> materials_;
};

/// 面で分かれる 3 つのエネルギー比（反射・透過・吸収）。和は 1。
struct SurfaceSplit { float reflect, transmit, absorb; };
inline SurfaceSplit splitAt(const AcousticMaterial& m, int band) {
    const float a = std::fmin(1.0f, std::fmax(0.0f, m.absorption[band]));
    const float t = std::fmin(1.0f - a, std::fmax(0.0f, m.transmission[band]));   // τ は 1 − α で頭打ち
    return SurfaceSplit{1.0f - a - t, t, a};
}

/// 世界の決まり一式。world.h がこれを 1 つ持つ。
struct WorldRules {
    MaterialTable materials;
    WorldWeights weights;
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_WORLD_RULES_H
