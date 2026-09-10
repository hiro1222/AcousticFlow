/* Flow/budget.h ── 予算（段 8）
 *
 * ■ 役割
 *   総レイ数を固定して、音源ごとに何本使うかを決める。設計文書 Ⅶ「レイ更新のフレーム分散。総予算は固定」。
 *   優先度は Ⅶ の順そのまま: 1 プレイヤーの操作対象 → 2 距離 → 3 音量 → 4 変化量。
 *   ★ピラーと一致する所（Ⅶ の結び）: 行動による変化を主役にするなら、計算資源も行動が関わる音源へ寄せる。
 *
 * ■ 中の仕組み
 *   1) 点数 score = loudness / (1 + d/d_ref) × (1 + 3·操作対象) × (1 + change/2)
 *      距離は 1/(1+d/4)（逆二乗にすると遠い音源が 0 に落ちて「保持」の意味が消える）。
 *      操作対象は ×4（Ⅶ の 1 位。他の 3 つを合わせても越えられない差）。変化量は m/s を 2 で割って足す（歩行 1.4 → ×1.7）。
 *   2) 段（tier）: 点数の順に 厳密（full）→ 簡易（light）→ 保持（hold）。段の数はホストが決める（既定 6 / 10）。
 *      ★ヒステリシス: 昇格は 0.5 s、降格は 1.0 s 続けて条件を満たしたときだけ（地図 7 章、旧実装の実測）。
 *        履歴が無いと段の境で音源が行き来して、その音だけ質が上下するのが聞こえる。降格を長くするのは
 *        「落ちるほうが目立つ」ため（一度良くなった音が戻るのは気づかれる）。
 *   3) 本数: 厳密は満額、簡易は 1/4（下限 32 本）、保持は 0 本。
 *      ★保持は「素通し」ではない ── **最後の答えを保つ**（distribute は前の Mix を出し続ける）。
 *        旧実装のバーチャル（可聴限界の外で音も止める）とは別物。
 *   4) 探り: 保持のうちいちばん古い 1 本を毎フレーム簡易で解き直す（順繰り）。
 *      扉が開いても保持の音源が眠ったままにならない。保持 N 本なら N フレームで一周する。
 *
 * ■ 繋がり
 *   受ける: Emitter の並び（loudness・operated・change・位置）、リスナー、Budget の設定。
 *   渡す:   音源ごとの tier と rays を world.update へ。
 *
 * ■ 退けた書き方
 *   ・点数を「音量 × 1/r²」だけにする: 操作対象が遠いと落ちる。ピラーは行動が主役なので操作対象が最優先。
 *   ・段を音源ごとに固定（旧設計の最初の形）: 決める側が居ないと全音源が厳密のまま。予算はエンジンが持つ。
 *   ・毎フレーム点数だけで並べ替える: 段の境で行き来する（上記）。時間を持たせる。
 *   ・保持を「音を止める」にする: 部屋の向こうで鳴り続けている音が消える。最後の答えを保つ。
 *
 * ■ 壊れる所
 *   ・昇格と降格を同じ時間にすると、点数が境で震えている音源が周期的に段を往復する（ヒステリシスの意味が無い）。
 *   ・探りを 0 本にすると、保持の音源は扉が開いても更新されない（旧実装で「保持の遮蔽の更新は探りの周期」と書いた所）。
 *   ・総予算を音源数で割るだけにすると、1 本のときに 4096 本飛んで 46 ms 掛かる。上限（1 本あたり）も持つ。
 */
#ifndef ACOUSTICFLOW_FLOW_BUDGET_H
#define ACOUSTICFLOW_FLOW_BUDGET_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Flow/emitter.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

enum class Tier : int { Full = 0, Light = 1, Hold = 2 };

struct BudgetConfig {
    int   totalRays = 1536;        // 1 フレームの総レイ数（0 = 無制限。音源ごとに raysPerEmitter を使う）
    int   maxPerEmitter = 512;     // 1 音源の上限（1 本のときに払いすぎないため）
    int   minPerEmitter = 32;      // 簡易の下限
    int   fullSlots = 6;           // 厳密の枠（旧実装の実測。方向バスと対で「載る」条件）
    int   lightSlots = 10;         // 簡易の枠
    float promoteSec = 0.5f;       // 昇格に要る継続（旧実装の実測）
    float demoteSec = 1.0f;        // 降格に要る継続（落ちるほうが目立つので長い）
    float distRef = 4.0f;          // 点数の距離の基準（m）
    int   probesPerFrame = 1;      // 保持のうち毎フレーム解き直す本数（順繰り）
};

struct BudgetSlot {
    Tier  tier = Tier::Hold;
    int   rays = 0;
    float score = 0.0f;
    int   wantSlot = -1;           // 点数の順位（0 が最上位）
    float promoteTimer = 0.0f, demoteTimer = 0.0f;
    float lastSolvedSec = -1e9f;   // 最後に解いた時刻（探りの順繰り）
    bool  probing = false;         // 今フレームは探りで解く
    bool  primed = false;          // 一度でも段を決めたか（最初のフレームはヒステリシスを通さない）
};

class Budget {
public:
    BudgetConfig cfg;

    /// 音源ごとの段と本数を決める。emitters は「有効な音源」の並び（id は添字と別でよい）。
    ///   now は経過時間（秒）。slots_ は音源の id で引く（世界が持つ）。
    void update(const std::vector<const Emitter*>& ems, const Vec3& listenerPos, float dt, float now,
                int fallbackRays, std::vector<BudgetSlot>& slots) {
        const int n = static_cast<int>(ems.size());
        if (static_cast<int>(slots.size()) < n) slots.resize(static_cast<std::size_t>(n));
        if (n == 0) return;
        // 1) 点数
        std::vector<int> order(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const Emitter& e = *ems[static_cast<std::size_t>(i)];
            const float d = length(e.pos - listenerPos);
            const float far = 1.0f / (1.0f + d / std::max(0.5f, cfg.distRef));
            const float op = e.operated ? 4.0f : 1.0f;
            const float ch = 1.0f + std::min(4.0f, e.change * 0.5f);
            slots[static_cast<std::size_t>(i)].score = std::max(1e-6f, e.loudness) * far * op * ch;
            order[static_cast<std::size_t>(i)] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return slots[static_cast<std::size_t>(a)].score > slots[static_cast<std::size_t>(b)].score; });
        for (int k = 0; k < n; ++k) slots[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])].wantSlot = k;

        // 2) 段（ヒステリシス）
        for (int i = 0; i < n; ++i) {
            BudgetSlot& s = slots[static_cast<std::size_t>(i)];
            const Tier want = (s.wantSlot < cfg.fullSlots) ? Tier::Full
                            : (s.wantSlot < cfg.fullSlots + cfg.lightSlots) ? Tier::Light : Tier::Hold;
            // ★最初のフレームはヒステリシスを通さず、いきなりその段にする。
            //   通すと新しく足した音源が「保持」から始まり、昇格の 0.5 s のあいだ**無音**になる
            //   （保持は最後の答えを保つ形だが、最初は答えが無い）。段 8 の検査で踏んだ。
            //   ヒステリシスは「段の変化」を鈍らせる物で、初期化を鈍らせる物ではない（Follower6 の primed と同じ）。
            if (!s.primed) { s.tier = want; s.primed = true; s.promoteTimer = 0.0f; s.demoteTimer = 0.0f; continue; }
            if (want == s.tier) { s.promoteTimer = 0.0f; s.demoteTimer = 0.0f; continue; }
            const bool up = static_cast<int>(want) < static_cast<int>(s.tier);
            if (up) {
                s.demoteTimer = 0.0f; s.promoteTimer += dt;
                if (s.promoteTimer >= cfg.promoteSec) { s.tier = want; s.promoteTimer = 0.0f; }
            } else {
                s.promoteTimer = 0.0f; s.demoteTimer += dt;
                if (s.demoteTimer >= cfg.demoteSec) { s.tier = want; s.demoteTimer = 0.0f; }
            }
        }

        // 3) 探り: 保持のうち最後に解いてからいちばん時間が経った物（順繰り）
        for (int i = 0; i < n; ++i) slots[static_cast<std::size_t>(i)].probing = false;
        for (int p = 0; p < cfg.probesPerFrame; ++p) {
            int pick = -1; float oldest = 1e30f;
            for (int i = 0; i < n; ++i) {
                const BudgetSlot& s = slots[static_cast<std::size_t>(i)];
                if (s.tier != Tier::Hold || s.probing) continue;
                if (s.lastSolvedSec < oldest) { oldest = s.lastSolvedSec; pick = i; }
            }
            if (pick < 0) break;
            slots[static_cast<std::size_t>(pick)].probing = true;
        }

        // 4) 本数。総予算を「厳密の数 + 簡易と探りの 1/4」の重みで割る。
        int nFull = 0, nLight = 0;
        for (int i = 0; i < n; ++i) {
            const BudgetSlot& s = slots[static_cast<std::size_t>(i)];
            if (s.tier == Tier::Full) ++nFull;
            else if (s.tier == Tier::Light || s.probing) ++nLight;
        }
        int perFull = fallbackRays;
        if (cfg.totalRays > 0) {
            const float weight = static_cast<float>(nFull) + 0.25f * static_cast<float>(nLight);
            perFull = (weight > 0.0f) ? static_cast<int>(cfg.totalRays / weight) : cfg.totalRays;
        }
        perFull = std::min(std::max(perFull, cfg.minPerEmitter), cfg.maxPerEmitter);
        const int perLight = std::max(cfg.minPerEmitter, perFull / 4);
        for (int i = 0; i < n; ++i) {
            BudgetSlot& s = slots[static_cast<std::size_t>(i)];
            if (s.tier == Tier::Full) s.rays = perFull;
            else if (s.tier == Tier::Light || s.probing) s.rays = perLight;
            else s.rays = 0;                                  // 保持: 解かない（最後の答えを保つ）
            if (s.rays > 0) s.lastSolvedSec = now;
        }
    }

    /// 実際に飛ばした本数の合計（費用の目安）。
    static int spentRays(const std::vector<BudgetSlot>& slots, int n) {
        int t = 0; for (int i = 0; i < n && i < static_cast<int>(slots.size()); ++i) t += slots[static_cast<std::size_t>(i)].rays;
        return t;
    }
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_BUDGET_H
