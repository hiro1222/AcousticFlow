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
#include "Flow/image_surface.h"
#include "Flow/receiver.h"
#include "Flow/mix.h"
#include "Flow/response.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

/// 受取面の「それ以降の当たり」のタップの遅れを均す時間（秒）。1 回目のタップは Response::statSec。
constexpr float kFaceLaterDelaySec = 0.3f;

struct DistributeInput {
    const TraceResult* trace = nullptr;
    const Visibility* visibility = nullptr;   // nullptr なら見通し 1（検査用）
    const Diffraction* diffraction = nullptr; // nullptr か valid=false なら回折 0
    const ImageSet* images = nullptr;         // nullptr か count=0 なら初期は方向なしの 1 本
    // 受取面（2026-09-13、World::earlyModel 1 / 2 / 3）。あれば初期はこのタップで鳴らす（2・3 では虚像のタップも入る。素性は imageTapId）。
    //   ★量の総量は trace->early6（World が受取面の合計に書き戻した物）。ここは取り分・遅れ・向き・広がりだけを使う。
    const FaceTapSet* faceTaps = nullptr;
    // 虚像の面音源（2026-09-14、World::earlyModel 4）。images と同じ並び。あれば虚像のタップの向き・幅・出口の重みをここから取る。
    const ImageSurface* surfaces = nullptr;
    // 虚像の面音源の隣の部屋の閉じ込め。surfaces[i].toDoor の割合を戸口の 1 本（素性 FaceTapSet::kIdDoor）へ移す。
    const FaceContain* imageContain = nullptr;
    Vec3 sourcePos{0, 0, 0};
    const Listener* listener = nullptr;
    int listenerRoom = -1;                 // −1 なら FDN の送りを作らない
    int sourceRoom = -1;                   // 段 2-f。耳と違う部屋なら、戸口越しの後期をこの部屋の FDN へ向き付きで送る
    bool  doorSource = false;              // 段 2-g。音源の部屋と耳の部屋が戸口で繋がり、戸口の線音源で鳴らすか
    float doorFeed = 0.0f;                 // 段 2-g。耳の部屋の分のうち戸口から入った割合（＝その戸口の開き具合 0..1）
    float doorPull = 0.0f;                 // 戸口寄せ（2026-09-12）。耳の部屋へ流す分のうち、戸口の線音源から直接鳴らす側へ移す割合 0..1
    // 隣の部屋の閉じ込め（2026-09-14）。音源が耳と別の部屋にいるとき、耳の部屋で響かせる分（耳の部屋の FDN への送りと、
    //   戸口の線音源から耳の部屋へ流す分）をこの割合だけ戸口から直接鳴らす側へ移す。0 で今までと 1 ビットも同じ。総量は変えない。
    //   ★発注者「隣の部屋の残響・反射は今いる部屋では反響させず、ドアから鳴る音が絶対に支配的になるように」。演出の摘み。
    float adjacentContain = 0.0f;
    // 先着の重み（2026-09-12）。到来が最初の到達（直接の到達時刻）から遅れるほど下げる。出口だけで、帳簿（component6）は物理のまま。
    //   重み = 10^(−precedenceDb/10 · (1 − e^(−Δ/precedenceSec)))。直接・透過は Δ=0 で変わらず、部屋の響きは大きく下がる。0 dB で今までと同じ。
    //   ★ゲームなので完全な物理でなく聞こえ方を優先する（発注者の指示）。先行音効果の窓 40 ms が τ の目安。
    float precedenceDb = 0.0f;
    float precedenceSec = 0.04f;
    const WorldWeights* weights = nullptr; // nullptr なら全部 1
    const Response* response = nullptr;    // nullptr なら既定
    float dt = 1.0f / 60.0f;
};

/// 音源 1 つぶんの配分の状態（成分ごとの追従）。世界が音源ごとに 1 つ持つ。
class EmitterMixer {
public:
    void reset() { for (int c = 0; c < kNumComponents; ++c) f_[c].reset(); fOther_.reset(); faceSm_.clear(); onsetSm_ = -1.0f; }

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
        // 先着の重み（DistributeInput::precedenceDb）: 最初の到達（直接の到達時刻）からの遅れ Δ で出口の量を下げる。エネルギーの係数。
        auto pre = [&](float delaySec) {
            if (in.precedenceDb <= 0.0f) return 1.0f;
            const float d = std::max(0.0f, delaySec - T.directSec);
            const float k = 1.0f - std::exp(-d / std::max(1e-3f, in.precedenceSec));
            return std::pow(10.0f, -in.precedenceDb * 0.1f * k);
        };
        auto emitTap = [&](TapKind kind, int comp, float delaySec, const Vec3& dir, float spread, int id) {
            MixTap* t = out.pushTap();
            if (!t) return;
            t->kind = kind; t->id = id; t->delaySec = delaySec; t->dirLocal = dir; t->spread = spread;
            const float pw = pre(delaySec);
            for (int b = 0; b < kNumBands; ++b) {
                out.component6[comp][b] += raw[comp][b];
                t->e6[b] = sm[comp][b] * W.w[comp] * pw;
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
        const FaceTapSet* ft = (in.faceTaps && in.faceTaps->count > 0) ? in.faceTaps : nullptr;
        if (ft) {
            // 受取面: 面ごとのタップへ、面が受けた量の取り分で配る（帯域ごとに Σ = 1）。
            //   ★取り分・遅れ・向き・広がりは**追従で均してから**使う（2026-09-13）。レイの組が入れ替わると、面のタップの
            //     取り分と遅れがそのフレームで跳ぶ（扉 29.8° で天井のタップが 1 回目 18%・2.1 ms とそれ以降 17%・5.8 ms に割れ、
            //     扉の板のタップの遅れが 3.1 → 3.8 ms）。総量は statSec で均しているのに形だけ生だったので、鳴らすと段になった
            //     （clicks の扉だけ 虚像 0.75 → 受取面 1.81 dB）。取り分と遅れは statSec、向きと広がりは directionSec。
            //   ★新しく出たタップは取り分 0 から、消えたタップは 0 へ落としてから外す。取り分は帯域ごとに和を 1 に揃え直す（帳簿は変えない）。
            const float aStat = (rs.statSec > 1e-4f) ? 1.0f - std::exp(-in.dt / rs.statSec) : 1.0f;
            const float aDir = (rs.directionSec > 1e-4f) ? 1.0f - std::exp(-in.dt / rs.directionSec) : 1.0f;
            for (FaceSmooth& z : faceSm_) z.seen = false;
            for (int i = 0; i < ft->count; ++i) {
                const FaceTap& f = ft->tap[i];
                FaceSmooth* z = nullptr;
                for (FaceSmooth& q : faceSm_) if (q.id == f.id) { z = &q; break; }
                const bool hasDir = length(f.dirWorld) > 0.5f;
                if (!z) {
                    faceSm_.push_back(FaceSmooth{});
                    z = &faceSm_.back();
                    z->id = f.id; z->delay = f.delaySec; z->dir = hasDir ? f.dirWorld : Vec3(0, 0, 0); z->spread = hasDir ? f.spread : 1.0f;
                }
                z->seen = true;
                for (int b = 0; b < kNumBands; ++b) {
                    const float target = (ft->total6[b] > 0.0f) ? f.e6[b] / ft->total6[b] : 0.0f;
                    z->share6[b] += aStat * (target - z->share6[b]);
                }
                // 虚像をつないだタップ（earlyModel 2、素性は imageTapId）: 遅れ・向き・広がりは虚像の値を生のまま使う（ISM の道と同じ）。
                //   虚像の遅れと向きは幾何で連続に動くので、均すと耳が動いたとき遅れが追いつかず、鏡の点の位置がずれて聞こえる。均すのは取り分だけ。
                if (f.id >= 16 && f.id < FaceTapSet::kIdNear) {
                    z->delay = f.delaySec; z->dir = hasDir ? f.dirWorld : Vec3(0, 0, 0); z->spread = hasDir ? f.spread : 1.0f;
                    continue;
                }
                {
                    // それ以降の当たりのタップは遅れを長く均す（kFaceLaterDelaySec）。1 回目は statSec のまま（壁際の近さの手がかり）。
                    //   ★それ以降のタップは多くの経路をまとめた物で、扉が開くと量の重心が一気に移り、目標の遅れが数 ms 跳ぶ。
                    //     50 ms で追うと強いタップの遅れが 1 フレームに 1 ms ずつ掃かれ（扉の板のタップ 11.7 → 7.5 ms）、
                    //     鳴らすと段になった（clicks の歩き＋扉 虚像 1.20 → 受取面 1.66 dB。0.3 s で 1.41 dB、1 s でも 1.41 dB）。
                    const bool later = (f.id == FaceTapSet::kIdRest) || (f.id >= FaceTapSet::kIdBase && ((f.id - FaceTapSet::kIdBase) % 2) == 1);
                    const float aD = later ? (1.0f - std::exp(-in.dt / kFaceLaterDelaySec)) : aStat;
                    z->delay += aD * (f.delaySec - z->delay);
                }
                if (hasDir) {
                    Vec3 d = (length(z->dir) > 0.5f) ? z->dir + (f.dirWorld - z->dir) * aDir : f.dirWorld;
                    const float ld = length(d);
                    z->dir = (ld > 1e-6f) ? d * (1.0f / ld) : f.dirWorld;
                    z->spread += aDir * (f.spread - z->spread);
                } else {
                    z->spread += aDir * (1.0f - z->spread);
                }
            }
            // 見えなくなったタップは 0 へ落とし、十分小さくなったら外す
            for (FaceSmooth& z : faceSm_) if (!z.seen) for (int b = 0; b < kNumBands; ++b) z.share6[b] *= (1.0f - aStat);
            faceSm_.erase(std::remove_if(faceSm_.begin(), faceSm_.end(), [](const FaceSmooth& z) {
                if (z.seen) return false;
                for (int b = 0; b < kNumBands; ++b) if (z.share6[b] > 1e-5f) return false;
                return true;
            }), faceSm_.end());
            float sumShare[kNumBands] = {};
            for (const FaceSmooth& z : faceSm_) for (int b = 0; b < kNumBands; ++b) sumShare[b] += z.share6[b];
            for (const FaceSmooth& z : faceSm_) {
                MixTap* t = out.pushTap();
                if (!t) break;
                t->kind = TapKind::Early; t->id = z.id; t->delaySec = z.delay;
                const bool hasDir = length(z.dir) > 0.5f;
                t->dirLocal = hasDir ? in.listener->toLocal(z.dir) : Vec3(0, 0, 0);
                t->spread = hasDir ? std::min(1.0f, std::max(0.0f, z.spread)) : 1.0f;
                const float pw = pre(z.delay);
                for (int b = 0; b < kNumBands; ++b) {
                    const float share = (sumShare[b] > 0.0f) ? z.share6[b] / sumShare[b] : 0.0f;
                    t->e6[b] = sm[kEarly][b] * W.w[kEarly] * share * pw;
                }
            }
            if (ft->firstSec > 0.0f) onsetSm_ = (onsetSm_ < 0.0f) ? ft->firstSec : onsetSm_ + aStat * (ft->firstSec - onsetSm_);
        } else {
            faceSm_.clear(); onsetSm_ = -1.0f;
        }
        float dirFrac[kNumBands] = {};
        if (!ft && im && im->count > 0)
            for (int b = 0; b < kNumBands; ++b) dirFrac[b] = std::min(1.0f, std::max(0.0f, im->directional6[b]));
        // 方向なしの残り。虚像が無ければ丸ごとここ（＝従来の 1 本と同じ）。到達はレイの最初の反射で固定
        //   ── 虚像の出入りで到達が動くと、素性で繋いだこのタップの遅延が掃引される。受取面のときは出さない。
        if (!ft) {
            MixTap* t = out.pushTap();
            if (t) {
                t->kind = TapKind::Early; t->id = 4;
                t->delaySec = (T.firstReflectSec > 0.0f) ? T.firstReflectSec : T.directSec;
                t->dirLocal = Vec3(0, 0, 0); t->spread = 1.0f;
                const float pw = pre(t->delaySec);
                for (int b = 0; b < kNumBands; ++b) t->e6[b] = sm[kEarly][b] * W.w[kEarly] * (1.0f - dirFrac[b]) * pw;
            }
        }
        if (!ft && im && im->count > 0) {
            float doorE6[kNumBands] = {};
            bool toDoor = false;
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
                // 虚像の面音源: 向きは面の重心、幅は面の角度の広がり、量に出口の重み（集まり・近さ。Σ は変えない）
                float sg = 1.0f, keep = 1.0f;
                if (in.surfaces) {
                    t->dirLocal = in.listener->toLocal(in.surfaces[i].dir);
                    t->width = in.surfaces[i].width;
                    sg = in.surfaces[i].gain;
                    if (in.imageContain && in.surfaces[i].toDoor > 0.0f) {
                        const float c = std::min(1.0f, in.surfaces[i].toDoor);
                        keep = 1.0f - c;
                        for (int b = 0; b < kNumBands; ++b) doorE6[b] += sm[kEarly][b] * W.w[kEarly] * dirFrac[b] * src.weight6[b] * sg * c;
                        toDoor = true;
                    }
                }
                const float pw = pre(src.pathSec);
                for (int b = 0; b < kNumBands; ++b) t->e6[b] = sm[kEarly][b] * W.w[kEarly] * dirFrac[b] * src.weight6[b] * sg * pw * keep;
            }
            if (toDoor) {
                // 隣の部屋の閉じ込めの戸口の 1 本: 向きは戸口の中心、遅れは音源 → 戸口 → 耳、幅は戸口の見込みの半角
                const FaceContain& fc = *in.imageContain;
                MixTap* t = out.pushTap();
                if (t) {
                    t->kind = TapKind::Early; t->id = FaceTapSet::kIdDoor; t->delaySec = fc.doorDelaySec;
                    const Vec3 dd = fc.doorPoint - in.listener->pos;
                    t->dirLocal = (length(dd) > 1e-4f) ? in.listener->toLocal(dd) : Vec3(0, 0, 0);
                    t->spread = 0.0f;
                    t->width = std::min(1.0f, std::max(0.0f, fc.doorSpread)) * 1.5707963f;
                    const float pw = pre(fc.doorDelaySec);
                    for (int b = 0; b < kNumBands; ++b) t->e6[b] = doorE6[b] * pw;
                }
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
        // 尾の開始 = 最初の虚像の到達（ITDG）。虚像が無ければレイの最初の反射、それも無ければ直接。（先着の重みの遅れにも使う）
        const float onsetSec = (ft && onsetSm_ > 0.0f) ? onsetSm_
                             : (im && im->count > 0 && im->firstSec > 0.0f) ? im->firstSec
                             : (T.firstReflectSec > 0.0f) ? T.firstReflectSec : T.directSec;
        // 先着の重みで見る尾の遅れの目安: 戸口から直接の分 ＝ 尾の開始 ＋ 尾の器の最短の線（15 ms）、
        //   部屋の響き（耳の部屋の尾、戸口から流した分）＝ さらに部屋を渡って戻る 30 ms。目安であって測った値ではない。
        const float preDoor = pre(onsetSec + 0.015f);
        const float preRoom = pre(onsetSec + 0.045f);
        float thru[kNumBands] = {};
        {
            float rawOther[kNumBands] = {};
            if (apart) for (int b = 0; b < kNumBands; ++b) rawOther[b] = std::max(0.0f, std::min(T.lateOther6[b], T.late6[b]));
            float smOther[kNumBands];
            fOther_.update(rawOther, in.dt, statLevel, rs.colourSec, smOther);    // 割らないフレームも回す（戻るときに段にしない）
            for (int b = 0; b < kNumBands; ++b)
                thru[b] = (apart && sm[kLate][b] > 0.0f) ? std::min(1.0f, std::max(0.0f, smOther[b] / sm[kLate][b])) : 0.0f;
        }
        // 段 2-g: 戸口の線音源で鳴らすときは、耳の部屋の分のうち戸口から入った分（× 戸口の開き具合）も
        //   音源の部屋の送りへ回す。そこから戸口の線音源が耳の部屋の FDN へ流す（戸口の音から鳴り始める）。
        //   thru6 は戸口から耳へ直接出す分。★量の合計は変えない（耳の部屋へ直接 ＋ 戸口へ ＝ 後期）。
        //   ★開き具合で割るのは、閉じた扉ごしの分（板と壁の透過）まで戸口から鳴らさないため。開ききれば 1。
        const float feed = (apart && in.doorSource) ? std::min(1.0f, std::max(0.0f, in.doorFeed)) : 0.0f;
        // 隣の部屋の閉じ込め: 耳の部屋で響かせる分（own ＝ (1−thru)(1−feed) と、流す分 fp·(1−pull)）を c だけ戸口から直接へ。
        //   c = 0 のときは下の式を今までと同じ並びで書く（1 ビットも同じ。寄せの検査 ⑦ が総量の一致をビットで見ている）。
        const float contain = apart ? std::min(1.0f, std::max(0.0f, in.adjacentContain)) : 0.0f;
        if (in.listenerRoom >= 0) {
            FdnSend* s = out.pushSend();
            if (s) {
                s->room = in.listenerRoom;
                for (int b = 0; b < kNumBands; ++b) {
                    out.component6[kLate][b] += raw[kLate][b];
                    s->e6[b] = (contain > 0.0f)
                        ? sm[kLate][b] * W.w[kLate] * (1.0f - thru[b]) * (1.0f - feed) * (1.0f - contain) * preRoom
                        : sm[kLate][b] * W.w[kLate] * (1.0f - thru[b]) * (1.0f - feed) * preRoom;
                }
            }
            float eThru = 0.0f;
            for (int b = 0; b < kNumBands; ++b)
                eThru += sm[kLate][b] * W.w[kLate] * (thru[b] + (1.0f - thru[b]) * feed + (1.0f - thru[b]) * (1.0f - feed) * contain);
            if (apart && eThru > 0.0f) {
                FdnSend* o = out.pushSend();
                if (o) {
                    o->room = in.sourceRoom;
                    // 戸口寄せ: 耳の部屋へ流す分（(1−thru)·feed）のうち pull を、戸口から直接鳴らす側（thru6）へ移す。
                    //   ★総量（e6）は変えない。レイの割合から離れる調整なので、量は耳で決める（既定 0 ＝ 物理の割合のまま）。
                    //     根拠: 耳は先に届いた音で定位を決める（先行音効果）。壁の陰では戸口から直接の分が総量の 1% 台になり、
                    //     自分の部屋の響き（全方向）に −19 dB で負けていた（試聴 2026-09-12、配分タブ）。
                    const float pull = std::min(1.0f, std::max(0.0f, in.doorPull));
                    for (int b = 0; b < kNumBands; ++b) {
                        // 戸口から直接の分（thru6）は戸口の遅れ、耳の部屋へ流す分は部屋の遅れで重み付け。
                        //   ★e6 は「thru·preDoor ＋ fp·(preRoom ＋ pull·(preDoor − preRoom))」の並びで書く。先着の重みが無いとき
                        //     (preDoor = preRoom = 1) は pull によらず同じ式になり、寄せで総量が 1 ビットも変わらない（検査 ⑦）。
                        const float base = sm[kLate][b] * W.w[kLate];
                        const float fp = (1.0f - thru[b]) * feed;
                        if (contain > 0.0f) {
                            // 閉じ込め: 戸口から直接 ＝ thru ＋ fp·(pull ＋ (1−pull)·c) ＋ own·c、耳の部屋へ流す ＝ fp·(1−pull)·(1−c)
                            const float own = (1.0f - thru[b]) * (1.0f - feed);
                            o->thru6[b] = base * (thru[b] + fp * (pull + (1.0f - pull) * contain) + own * contain) * preDoor;
                            o->e6[b] = o->thru6[b] + base * fp * (1.0f - pull) * (1.0f - contain) * preRoom;
                        } else {
                            o->thru6[b] = base * (thru[b] + fp * pull) * preDoor;
                            o->e6[b] = base * (thru[b] * preDoor + fp * (preRoom + pull * (preDoor - preRoom)));
                        }
                    }
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
        out.onsetSec = onsetSec;
    }

private:
    // 受取面のタップの追従（素性ごと）。取り分・遅れは statSec、向き・広がりは directionSec。
    struct FaceSmooth {
        int   id = -1;
        float share6[kNumBands] = {};
        float delay = 0.0f;
        Vec3  dir{0, 0, 0};
        float spread = 1.0f;
        bool  seen = false;
    };
    std::vector<FaceSmooth> faceSm_;
    float onsetSm_ = -1.0f;
    Follower6 f_[kNumComponents];
    Follower6 fOther_;   // 段 2-f。後期のうち戸口越しの分。後期と同じ速さで追い、比を取る
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_DISTRIBUTE_H
