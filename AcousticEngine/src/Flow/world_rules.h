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
    /// 隣の部屋の響き（2026-10-01）。音源が耳と別の部屋にいるときの後期だけに、w[kLate] へさらに掛ける（エネルギー比）。
    ///   戸口から抜けてくる響き（戸口の線音源）と、耳の部屋で響かせる分の両方。音源と耳が同じ部屋なら掛けない。
    ///   発注者「隣の部屋から抜ける反射音が支配的過ぎて、角度による音色の変化がはっきりしない・隣の部屋で反響が大きすぎる」。
    ///   手前の部屋では 10〜60° の間、響きが角度で変わる音（直接・回折・透過・初期）より 10〜14 dB 大きかった（AF_ONLY=shadowexit）。
    ///   形（RT60・向き・遅れ）は変えず量だけ（S2: 量は耳で決める）。既定 1 ＝ 今までと 1 ビットも同じ。
    float lateAdjacent = 1.0f;
    /// 影のこもり（2026-10-06、dB、0..24、既定 0）。遮られた直接の道（透過・回折）の出口にだけ、高域を余分に落とす。
    ///   帯域 b（125 Hz … 4 kHz）で −S·b/5 dB ＝ オクターブあたり S/5 dB の傾き。125 Hz は 0、4 kHz で −S。
    ///   発注者「ドアの影の時のこもりをもう少し大きくしたい」。扉の部屋では響きが帳簿の 3〜8 割を占め、
    ///   透過・回折のこもり（閉 +20 dB）が響きに薄められていた（全部込みで +14 dB、10° で +6 dB）。
    ///   ★傾きにしたのは、物理の 2 つ（板の質量則 ≈ 6 dB/oct、前川の回折 ≈ 3 dB/oct）がどちらも傾きだから。
    ///     棚（500 Hz から上だけ）にすると角ができ、「板が厚くなった」ではなく「フィルタを掛けた」に聞こえる。
    ///   ★見通しの割合で連続に効く: 透過も回折も量が (1 − 見通し) に比例するので、影から出れば掛かる量ごと 0 へ下りる（二値の切り替えなし）。
    ///   演出の摘み（先着の重みと同じ形）: エンジンの既定は 0（物理どおり）、聞かせたい値は載る側が持つ。
    float shadowMuffleDb = 0.0f;
    /// 影のこもりが一番深くなる周波数（2026-10-10、Hz、250..4000、既定 4000）。発注者「扉の開閉によるこもりがわかりにくい、調整できるように」。
    ///   125 Hz を 0 にして、この周波数で −shadowMuffleDb に届き、それより上はその深さのまま（間はオクターブに比例）。
    ///   既定 4000 ＝ 今までと 1 ビットも同じ（4 kHz で −S）。高域の少ない素材（試験のフリー音源は 2 kHz より上が 0.4%）だと
    ///   4 kHz で深くしても聞こえないので、1000 Hz などに下げると中域からこもる。
    float shadowMuffleFullHz = 4000.0f;
    /// 影のこもりを帯域ごとの係数（エネルギー比）にする。S = 0 なら厳密に 1（今までと 1 ビットも同じ）。
    void shadowMuffle6(float g6[kNumBands]) const {
        const float s = std::fmin(24.0f, std::fmax(0.0f, shadowMuffleDb));
        const float full = std::isfinite(shadowMuffleFullHz) ? std::fmin(4000.0f, std::fmax(250.0f, shadowMuffleFullHz)) : 4000.0f;
        if (full >= 4000.0f) {
            //既定: 今までと同じ式（帯域 b で −S·b/5）
            for (int b = 0; b < kNumBands; ++b)
                g6[b] = (s > 0.0f) ? std::pow(10.0f, -0.1f * s * static_cast<float>(b) / static_cast<float>(kNumBands - 1)) : 1.0f;
            return;
        }
        //125 Hz から full までのオクターブ数で割って、そこから上は一番深いまま
        const float span = std::log2(full / kBandHz[0]);
        for (int b = 0; b < kNumBands; ++b) {
            const float frac = std::fmin(1.0f, static_cast<float>(b) / span);
            g6[b] = (s > 0.0f) ? std::pow(10.0f, -0.1f * s * frac) : 1.0f;
        }
    }
    bool isIdentity() const {
        for (int i = 0; i < kNumComponents; ++i) if (std::fabs(w[i] - 1.0f) > 1e-6f) return false;
        return std::fabs(lateAdjacent - 1.0f) <= 1e-6f && shadowMuffleDb <= 0.0f;
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
    /// 中身を書き換える（実行中の調整用、2026-09-12）。0 番（既定の壁）も書き換えてよい。範囲外なら false。
    ///   ★毎フレーム add で増やすと表が伸び続けるので、調整は必ずこちらで。
    bool set(int id, const AcousticMaterial& m) {
        if (id < 0 || id >= static_cast<int>(materials_.size())) return false;
        materials_[static_cast<std::size_t>(id)] = m;
        return true;
    }
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
