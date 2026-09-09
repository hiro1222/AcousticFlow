/* Flow/distribute.h ── 配分（段 3。段 5 で直接音を見通しの割合に）
 *
 * ■ 役割
 *   レイの集計（TraceResult）と開口の解析（Visibility）を五成分に配り、タップと FDN の送りの形（Mix）にする。
 *   設計文書 Ⅴ「全体エネルギーからの配分」。
 *   段 5 の形: 直接 = 自由音場 × 見通しの割合、透過 = 自由音場 × (1−割合) × 遮っている物の τ、初期・後期はレイ。
 *   回折は 0（段 6 で (1−割合) × 前川）、初期は方向なし（段 7 で ISM が割る）。
 *
 * ■ 中の仕組み
 *   1) 受け取った物を 1 回ずつ出す:
 *        直接   → タップ（到達 directSec、方向は音源の向き、点）。量 = free × visible
 *        透過   → タップ（到達 directSec、方向は音源の向き、広がり 0.5）。量 = free × (1−visible) × τ_shadow
 *                 ★遮られた分だけが板を通る。旧実装（中心の直線の τ）だと幅を持つ音源が半分隠れたとき、
 *                   隠れた半分が素通しになった。
 *        初期   → タップ 1 本（到達 firstReflectSec、方向なし、広がり 1）。段 7 で虚像の数に割れる
 *        後期   → FDN の送り（リスナーの部屋）。段 5 で「最後の反射の部屋」ごとの送りに割れる
 *   2) 帳簿: energy6 = 受け取った物の和、component6 = 出した物の内訳。どちらも**生**（平滑と重みの前）。
 *      conserves() は「受け取った物を全部 1 回ずつ出したか」の検査。物理の保存則はレイ側（段 2）。
 *   3) 速度: 成分ごとに Follower6（response.h）。量は即時、形は colourSec。レイ由来（初期・後期）は量にも statSec。
 *      ★直接と透過は解析なので平滑しない（量は levelSec=0 で即時）。扉の立ち上がりはここから来る。
 *   4) 世界の重み: 出口で 1 回だけ掛ける。帳簿には掛けない。
 *   5) 尾の開始 onsetSec = 最初の反射の到達。段 7 で最初の虚像の到達に置き換わる。
 *
 * ■ 繋がり
 *   受ける: TraceResult、Visibility、音源とリスナーの位置、リスナーの部屋番号、WorldWeights、Response、dt。
 *   渡す:   Mix を段 4 の橋へ（√ を取って VoiceRenderer へ）。
 *
 * ■ 退けた書き方
 *   ・後期の量を「部屋の結合の式」で別に減らす: 旧実装の穴そのもの。減らすのは 1 か所（レイ）。
 *   ・平滑してから帳簿に書く: 成分ごとに時定数が違うので和が総量に一致しなくなり、検査が意味を失う。
 *   ・見通しの割合を直接音だけに掛けて透過に触らない: 板の向こうの音源が「見えない ＝ 直接 0」で終わり、透過が消える。
 *
 * ■ 壊れる所
 *   ・重みを帳簿にも掛けると conserves が通ったまま二重に効く。帳簿は生。
 *   ・visible を平滑すると扉の立ち上がりが鈍る。解析値は毎フレームそのまま。
 *   ・listenerRoom が −1（外）のとき送りを作ると FdnRoomMix が範囲外を引く。送りは部屋があるときだけ。
 */
#ifndef ACOUSTICFLOW_FLOW_DISTRIBUTE_H
#define ACOUSTICFLOW_FLOW_DISTRIBUTE_H

#include <algorithm>
#include <cmath>
#include "Core/vec3.h"
#include "Flow/aperture.h"
#include "Flow/emitter.h"
#include "Flow/energy_trace.h"
#include "Flow/mix.h"
#include "Flow/response.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

struct DistributeInput {
    const TraceResult* trace = nullptr;
    const Visibility* visibility = nullptr;   // nullptr なら見通し 1（検査用）
    Vec3 sourcePos{0, 0, 0};
    const Listener* listener = nullptr;
    int listenerRoom = -1;                 // −1 なら FDN の送りを作らない
    const WorldWeights* weights = nullptr; // nullptr なら全部 1
    const Response* response = nullptr;    // nullptr なら既定
    float dt = 1.0f / 60.0f;
};

/// 音源 1 つぶんの配分の状態（成分ごとの追従）。世界が音源ごとに 1 つ持つ。
class EmitterMixer {
public:
    void reset() { for (int c = 0; c < kNumComponents; ++c) f_[c].reset(); }

    void run(const DistributeInput& in, Mix& out) {
        out.clear();
        const TraceResult& T = *in.trace;
        static const Response kDefault;
        const Response& rs = in.response ? *in.response : kDefault;
        WorldWeights ident;
        const WorldWeights& W = in.weights ? *in.weights : ident;
        static const Visibility kAllVisible;
        const Visibility& Vis = in.visibility ? *in.visibility : kAllVisible;
        const float vis = std::min(1.0f, std::max(0.0f, Vis.visible));

        // ── 帳簿（生）: 受け取った物 ──
        float raw[kNumComponents][kNumBands] = {};
        for (int b = 0; b < kNumBands; ++b) {
            raw[kDirect][b]   = T.freeDirect6[b] * vis;
            raw[kTransmit][b] = T.freeDirect6[b] * (1.0f - vis) * Vis.shadowTau6[b];
            raw[kEarly][b]    = T.early6[b];
            raw[kLate][b]     = T.late6[b];
            raw[kDiffract][b] = 0.0f;                          // 段 6
            out.energy6[b] = raw[kDirect][b] + raw[kEarly][b] + raw[kLate][b] + raw[kTransmit][b] + raw[kDiffract][b];
        }

        // ── 速度: 成分ごとに追う（量は即時、形は colourSec。レイ由来は量にも statSec）──
        float sm[kNumComponents][kNumBands];
        const float statLevel = std::max(rs.levelSec, rs.statSec);
        f_[kDirect].update(raw[kDirect], in.dt, rs.levelSec, rs.colourSec, sm[kDirect]);
        f_[kTransmit].update(raw[kTransmit], in.dt, rs.levelSec, rs.colourSec, sm[kTransmit]);
        f_[kEarly].update(raw[kEarly], in.dt, statLevel, rs.colourSec, sm[kEarly]);
        f_[kLate].update(raw[kLate], in.dt, statLevel, rs.colourSec, sm[kLate]);
        f_[kDiffract].update(raw[kDiffract], in.dt, rs.levelSec, rs.colourSec, sm[kDiffract]);

        // ── 出す: 受け取った物を 1 回ずつ。帳簿の内訳は生、出口は 平滑 × 重み ──
        const Vec3 toSrc = in.sourcePos - in.listener->pos;
        const Vec3 dirLocal = (length(toSrc) > kEps) ? in.listener->toLocal(toSrc) : Vec3(0, 0, 0);
        auto emitTap = [&](TapKind kind, int comp, float delaySec, const Vec3& dir, float spread) {
            MixTap* t = out.pushTap();
            if (!t) return;
            t->kind = kind; t->delaySec = delaySec; t->dirLocal = dir; t->spread = spread;
            for (int b = 0; b < kNumBands; ++b) {
                out.component6[comp][b] += raw[comp][b];
                t->e6[b] = sm[comp][b] * W.w[comp];
            }
        };
        emitTap(TapKind::Direct,   kDirect,   T.directSec, dirLocal, 0.0f);
        emitTap(TapKind::Transmit, kTransmit, T.directSec, dirLocal, 0.5f);
        emitTap(TapKind::Early,    kEarly,    (T.firstReflectSec > 0.0f) ? T.firstReflectSec : T.directSec, Vec3(0, 0, 0), 1.0f);
        if (in.listenerRoom >= 0) {
            FdnSend* s = out.pushSend();
            if (s) {
                s->room = in.listenerRoom;
                for (int b = 0; b < kNumBands; ++b) { out.component6[kLate][b] += raw[kLate][b]; s->e6[b] = sm[kLate][b] * W.w[kLate]; }
            }
        } else {
            for (int b = 0; b < kNumBands; ++b) out.component6[kLate][b] += raw[kLate][b];
        }
        for (int b = 0; b < kNumBands; ++b) out.component6[kDiffract][b] += raw[kDiffract][b];
        out.onsetSec = (T.firstReflectSec > 0.0f) ? T.firstReflectSec : T.directSec;
    }

private:
    Follower6 f_[kNumComponents];
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_DISTRIBUTE_H
