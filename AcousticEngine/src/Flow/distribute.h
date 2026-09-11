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
#include "Flow/diffraction.h"
#include "Flow/emitter.h"
#include "Flow/energy_trace.h"
#include "Flow/image_sources.h"
#include "Flow/mix.h"
#include "Flow/response.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

struct DistributeInput {
    const TraceResult* trace = nullptr;
    const Visibility* visibility = nullptr;   // nullptr なら見通し 1（検査用）
    const Diffraction* diffraction = nullptr; // nullptr か valid=false なら回折 0
    const ImageSet* images = nullptr;         // nullptr か count=0 なら初期は方向なしの 1 本
    Vec3 sourcePos{0, 0, 0};
    const Listener* listener = nullptr;
    int listenerRoom = -1;                 // −1 なら FDN の送りを作らない
    int sourceRoom = -1;                   // 段 2-f。耳と違う部屋なら、戸口越しの後期をこの部屋の FDN へ向き付きで送る
    const WorldWeights* weights = nullptr; // nullptr なら全部 1
    const Response* response = nullptr;    // nullptr なら既定
    float dt = 1.0f / 60.0f;
};

/// 音源 1 つぶんの配分の状態（成分ごとの追従）。世界が音源ごとに 1 つ持つ。
class EmitterMixer {
public:
    void reset() { for (int c = 0; c < kNumComponents; ++c) f_[c].reset(); fOther_.reset(); }

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
            // 回折: 遮られた分 (1−vis) が最寄りの稜線を回る。前川の帯域別の減衰 g²（追記 B「遮られた直接音の代わり」）
            raw[kDiffract][b] = (in.diffraction && in.diffraction->valid) ? T.freeDirect6[b] * (1.0f - vis) * in.diffraction->energy6[b] : 0.0f;
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
        auto emitTap = [&](TapKind kind, int comp, float delaySec, const Vec3& dir, float spread, int id) {
            MixTap* t = out.pushTap();
            if (!t) return;
            t->kind = kind; t->id = id; t->delaySec = delaySec; t->dirLocal = dir; t->spread = spread;
            for (int b = 0; b < kNumBands; ++b) {
                out.component6[comp][b] += raw[comp][b];
                t->e6[b] = sm[comp][b] * W.w[comp];
            }
        };
        emitTap(TapKind::Direct,   kDirect,   T.directSec, dirLocal, 0.0f, 1);
        emitTap(TapKind::Transmit, kTransmit, T.directSec, dirLocal, 0.5f, 2);
        // 初期: ISM の虚像へ配る（方向と正規化重み。段 7）。**方向の分かっている割合だけ**を虚像へ、
        //   残りは方向なしの 1 本へ回す。正規化重み（Σ=1）は「どの虚像へ」しか言えないので、
        //   全部が僅かにしか見えていないとき（妥当 0.0004）でも満額が虚像に乗り、扉が開いて
        //   虚像が 0→4 本に生えた瞬間に初期反射が丸ごと方向つきに切り替わっていた（実測 2.0 dB／55°）。
        //   広がりは 1 − 可視率（面の縁で半分隠れた虚像は半分ぼやける）。帳簿の初期は総量を 1 回だけ。
        const ImageSet* im = in.images;
        float dirFrac[kNumBands] = {};
        if (im && im->count > 0)
            for (int b = 0; b < kNumBands; ++b) dirFrac[b] = std::min(1.0f, std::max(0.0f, im->directional6[b]));
        // 方向なしの残り。虚像が無ければ丸ごとここ（＝従来の 1 本と同じ）。到達はレイの最初の反射で固定
        //   ── 虚像の出入りで到達が動くと、素性で繋いだこのタップの遅延が掃引される。
        {
            MixTap* t = out.pushTap();
            if (t) {
                t->kind = TapKind::Early; t->id = 4;
                t->delaySec = (T.firstReflectSec > 0.0f) ? T.firstReflectSec : T.directSec;
                t->dirLocal = Vec3(0, 0, 0); t->spread = 1.0f;
                for (int b = 0; b < kNumBands; ++b) t->e6[b] = sm[kEarly][b] * W.w[kEarly] * (1.0f - dirFrac[b]);
            }
        }
        if (im && im->count > 0) {
            for (int i = 0; i < im->count; ++i) {
                const ImageSource& src = im->img[i];
                MixTap* t = out.pushTap();
                if (!t) break;
                t->kind = TapKind::Early; t->delaySec = src.pathSec;
                // 面の組で決まる素性（並び順に依らない）。3 次まで入る形。
                //   面の番号が 126 を超える場面では衝突しうるので、そのときは素性を諦めて遅延で繋ぐ。
                {
                    const int f0 = src.face[0] + 1, f1 = src.face[1] + 1, f2 = src.face[2] + 1;
                    t->id = (f0 < 128 && f1 < 128 && f2 < 128) ? (16 + f0 + f1 * 128 + f2 * 16384) : -1;
                }
                t->dirLocal = in.listener->toLocal(src.pos - in.listener->pos);
                t->spread = 1.0f - src.validity;
                for (int b = 0; b < kNumBands; ++b) t->e6[b] = sm[kEarly][b] * W.w[kEarly] * dirFrac[b] * src.weight6[b];
            }
        }
        for (int b = 0; b < kNumBands; ++b) out.component6[kEarly][b] += raw[kEarly][b];   // 帳簿は 1 回だけ
        // ── 後期の送り（段 2-f で 2 本に割った）──
        //   耳の部屋へ:   耳の部屋の面から来た分。一様に聞く ＝ 同じ部屋の LEV。
        //   音源の部屋へ: 戸口越しの面から来た分。レイが測った向きと集まり具合を持たせ、World がその部屋の FDN を戸口の向きで聞く。
        //   ★割合は**平滑した値どうしの比**で取る。生の比を平滑済みの総量に掛けると、組の入れ替わりで割合だけが跳ぶ。
        //   ★音源が耳と同じ部屋なら割らない（戸口越しの分は耳の部屋へ畳む ＝ 今までと同じ）。
        //   ★帳簿（component6）は総量を 1 回だけ。割るのは出口だけ。
        const bool apart = (in.listenerRoom >= 0 && in.sourceRoom >= 0 && in.sourceRoom != in.listenerRoom);
        float thru[kNumBands] = {};
        {
            float rawOther[kNumBands] = {};
            if (apart) for (int b = 0; b < kNumBands; ++b) rawOther[b] = std::max(0.0f, std::min(T.lateOther6[b], T.late6[b]));
            float smOther[kNumBands];
            fOther_.update(rawOther, in.dt, statLevel, rs.colourSec, smOther);    // 割らないフレームも回す（戻るときに段にしない）
            for (int b = 0; b < kNumBands; ++b)
                thru[b] = (apart && sm[kLate][b] > 0.0f) ? std::min(1.0f, std::max(0.0f, smOther[b] / sm[kLate][b])) : 0.0f;
        }
        if (in.listenerRoom >= 0) {
            FdnSend* s = out.pushSend();
            if (s) {
                s->room = in.listenerRoom;
                for (int b = 0; b < kNumBands; ++b) { out.component6[kLate][b] += raw[kLate][b]; s->e6[b] = sm[kLate][b] * W.w[kLate] * (1.0f - thru[b]); }
            }
            float eThru = 0.0f;
            for (int b = 0; b < kNumBands; ++b) eThru += sm[kLate][b] * W.w[kLate] * thru[b];
            if (apart && eThru > 0.0f) {
                FdnSend* o = out.pushSend();
                if (o) {
                    o->room = in.sourceRoom;
                    for (int b = 0; b < kNumBands; ++b) o->e6[b] = sm[kLate][b] * W.w[kLate] * thru[b];
                    float mass = 0.0f;
                    for (int b = 0; b < kNumBands; ++b) mass += T.lateOther6[b];
                    const float len = std::sqrt(T.otherDir[0] * T.otherDir[0] + T.otherDir[1] * T.otherDir[1] + T.otherDir[2] * T.otherDir[2]);
                    if (len > 0.0f && mass > 0.0f) {
                        o->dir[0] = T.otherDir[0] / len; o->dir[1] = T.otherDir[1] / len; o->dir[2] = T.otherDir[2] / len;
                        o->focus = std::min(1.0f, len / mass);
                    }
                }
            }
        } else {
            for (int b = 0; b < kNumBands; ++b) out.component6[kLate][b] += raw[kLate][b];
        }
        if (in.diffraction && in.diffraction->valid)
            emitTap(TapKind::Diffract, kDiffract, in.diffraction->pathSec, in.diffraction->dirLocal, 0.0f, 3);   // 方向は稜線の点
        else
            for (int b = 0; b < kNumBands; ++b) out.component6[kDiffract][b] += raw[kDiffract][b];
        // 尾の開始 = 最初の虚像の到達（ITDG）。虚像が無ければレイの最初の反射、それも無ければ直接。
        out.onsetSec = (im && im->count > 0 && im->firstSec > 0.0f) ? im->firstSec
                     : (T.firstReflectSec > 0.0f) ? T.firstReflectSec : T.directSec;
    }

private:
    Follower6 f_[kNumComponents];
    Follower6 fOther_;   // 段 2-f。後期のうち戸口越しの分。後期と同じ速さで追い、比を取る
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_DISTRIBUTE_H
