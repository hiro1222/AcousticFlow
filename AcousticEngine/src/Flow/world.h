/* Flow/world.h ── 世界: 更新の 1 サイクル（段 4。段 5 で開口と動く箱）
 *
 * ■ 役割
 *   段 1〜3 の部品を束ねて「1 フレーム」を回す。ホストから見える唯一の物。
 *   持つ物: 決まり（rules）、面（surfaces）、部屋グラフとプローブと戸口、リスナー、音源とその配分の状態、FDN の器への結び。
 *
 * ■ 中の仕組み
 *   build():  面のうち静的な箱を部屋グラフに渡し、部屋ごとにプローブ、戸口の矩形（rooms::Aperture）を控える。
 *   update(dt): 1) リスナーと音源の beginFrame（変化量）
 *               2) 戸口ごとに、動く箱（扉）の覆い（aperture.openingCoverage）→ OpeningState → プローブの RT60（applyOpenings）
 *                  同じ a が距離・部屋の結合にも使われる（段 5 では RT60 だけ。結合は「最後の反射の部屋」で後日）
 *               3) 音源ごとに: 部屋を引く → レイ（種 = 音源 id）→ 見通し（aperture.discVisibility、幅は見込み角）→ 配分 → Mix
 *               4) FDN: 部屋ごとに RT60 と listenerWeight を置く（段 4 と同じ式）
 *   applyToVoice(): 橋（mix_to_voice.h）で VoiceRenderer へ。
 *   境 mixingSec = 3 × 平均自由行程 / c。
 *
 * ■ 繋がり
 *   受ける: ホスト（C API world_api.cpp）から 箱・材質・リスナー・音源・FDN の器・設定・動く箱の位置。
 *   渡す:   Mix（帳簿は情報タブへ）、VoiceRenderer への適用、FdnRoomMix への部屋と重み。
 *
 * ■ 退けた書き方
 *   ・非同期ワーカーを段 4 で入れる: 「一周を鳴らす」が先。非同期は段 8（負荷）で旧 scene_async.h の形を流用する。
 *   ・音源ごとに FDN の器を持つ: 設計文書「FDN は部屋ごと。音源数に比例しない」。器は 1 つ、部屋 = プローブ。
 *   ・扉の覆いを音源ごとの視点で出す: 部屋の性質（RT60）が音源で変わる。正射影で 1 つ（aperture.h）。
 *
 * ■ 壊れる所
 *   ・bindFdn を build の前に呼ぶと部屋が無い。順序: build → bindFdn。
 *   ・部屋グラフを作り直したら器を「古い」印にする（FdnRoomMix は部屋を消せない。同じ器に足すと重複する）。
 *   ・listenerWeight を置き忘れると FdnRoomMix の既定 0 で無音。update が毎フレーム置く。
 *   ・動く箱を部屋グラフに入れると、閉じた扉で戸口が消える（部屋が割れる）。dynamic は build から外す。
 */
#ifndef ACOUSTICFLOW_FLOW_WORLD_H
#define ACOUSTICFLOW_FLOW_WORLD_H

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "Core/room_graph.h"
#include "Core/worker_pool.h"
#include "Flow/budget.h"
#include "Dsp/fdn_room_mix.h"
#include "Dsp/voice_renderer.h"
#include "Flow/aperture.h"
#include "Flow/distribute.h"
#include "Flow/emitter.h"
#include "Flow/energy_trace.h"
#include "Flow/image_sources.h"
#include "Flow/mix.h"
#include "Flow/mix_to_voice.h"
#include "Flow/probe.h"
#include "Flow/response.h"
#include "Flow/surfaces.h"
#include "Flow/trace_scene.h"
#include "Gpu/trace_gpu.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

class World {
public:
    WorldRules rules;
    Surfaces   surfaces;
    Response   response;
    Budget     budget;              // 段 8（cfg.totalRays = 0 で無制限＝raysPerEmitter をそのまま）
    int   raysPerEmitter = 256;     // 予算が無制限のときの 1 音源の本数
    int   rayGroups = 4;            // レイをこの数の組に分け、毎フレーム 1 組だけ飛ばす（1 で分散なし）
    int   maxBounces = 40;
    float headCm = 57.0f;

    // ── 面 ──
    int addBox(const Obb& obb, int material, bool dynamic) { dirty_ = true; return surfaces.add(obb, material, dynamic); }
    void setBoxTransform(int box, const Obb& obb) {
        if (box < 0 || box >= surfaces.count()) return;
        Surface& s = surfaces.at(box);
        s.obb = obb;
        if (!s.dynamic) dirty_ = true;          // 静的な物を動かしたら部屋グラフを作り直す
    }
    // ── 実行中の材質の調整（2026-09-12）──
    //   部屋の RT60 は build で材質から出し（Sabine）、ISM の面の反射率も build で写す。だから静的な箱に効く変更は
    //   dirty にして次の update で組み直す（部屋グラフの作り直し込みで数百 ms。調整のときだけなので許容）。
    //   レイの場面（透過・吸音・散乱）と扉の板の覆いは毎フレーム材質を読むので、そちらはそのフレームから効く。
    //   ★退けた書き方: 部屋グラフを作り直さずに RT60 だけ出し直す。部屋ごとの面積の内訳を部屋グラフが持っていないので、
    //     出し直しにもボクセルの走査が要る。作り直しと大差ないので分けなかった。
    /// 箱の材質を差し替える。静的な箱なら次の update で部屋を組み直す。同じ材質なら何もしない。
    void setBoxMaterial(int box, int material) {
        if (box < 0 || box >= surfaces.count() || material < 0 || material >= rules.materials.count()) return;
        Surface& s = surfaces.at(box);
        if (s.material == material) return;
        s.material = material;
        if (!s.dynamic) dirty_ = true;
    }
    /// 材質の中身を書き換える。その材質を使う静的な箱があれば次の update で部屋を組み直す。範囲外なら false。
    bool updateMaterial(int material, const AcousticMaterial& m) {
        if (!rules.materials.set(material, m)) return false;
        for (int i = 0; i < surfaces.count(); ++i) {
            const Surface& s = surfaces.at(i);
            if (s.material == material && !s.dynamic && s.active) { dirty_ = true; break; }
        }
        return true;
    }

    // ── 部屋 ──
    void build() {
        std::vector<rooms::SolidBox> statics;
        for (int i = 0; i < surfaces.count(); ++i) {
            const Surface& s = surfaces.at(i);
            if (!s.active || s.dynamic) continue;              // 動く物は部屋グラフに入れない（地図 8 章）
            rooms::SolidBox sb; sb.obb = s.obb;
            const AcousticMaterial& m = rules.materials.get(s.material);
            for (int b = 0; b < kNumBands; ++b) sb.absorption[b] = m.absorption[b];
            statics.push_back(sb);
        }
        builder_.invalidateAll();
        const rooms::Result& res = builder_.build(statics);
        probes_.clear();
        for (const rooms::Room& rm : res.rooms) probes_.push_back(probeFromRoom(rm, res.grid.cell));
        apertures_ = res.apertures;
        // 部屋の格子をレイの場面へ（段 2-f）。build のときだけ変わる（buildTraceScene は毎フレームだが、ここは触らない）。
        {
            const rooms::Grid& g = res.grid;
            const std::vector<std::int16_t>& rv = builder_.roomOfVoxel();
            traceScene_.roomOrigin = g.origin; traceScene_.roomCell = g.cell;
            traceScene_.roomNx = g.nx; traceScene_.roomNy = g.ny; traceScene_.roomNz = g.nz;
            traceScene_.roomVox.assign(rv.begin(), rv.end());
            ++traceScene_.roomVersion;
        }
        collectFaces(surfaces, rules.materials, faces_);          // ISM の面（静的な箱の 6 面）
        dirty_ = false;
        ++buildCount_;
        if (fdn_) { fdn_ = nullptr; fdnRoomOf_.assign(probes_.size(), -1); fdnStale_ = true; }
    }
    /// 閉じた扉から漏れる回折の扱い。**既定 1（案A）**。2026-09-10 決定（同日 2 → 1 に訂正）。
    ///   0 旧          脚の貫通を固定の 7 cm と比べる。厚み 6 cm の板は通り抜けても重み 0.19 が残り、
    ///                 **実際の隙間の大きさと無関係に**一定の漏れが出る（透過の +11.6 dB）＝ 作り物
    ///   1 案A（既定） 貫通をその箱自身の薄さと比べる。板を通り抜けたら 0、角を掠めたら 1。
    ///                 閉じている間だけ効き、**10° 以降は 0 と 1 ビット違わない**（実測）。
    ///                 ★「本物の 4 mm の隙間まで消える」と一度書いたが誤り。中を見ると、
    ///                   閉扉で立っていた候補は箱 7（枠）の稜線で a=0.201・w=0.19 ＝
    ///                   **板を厚みぶん貫く経路**だった。4 mm の隙間は経路として最初から
    ///                   見つかっていない。案A が消したのは実在しない経路だけ
    ///   2 案B         回折のエネルギーに戸口の openFrac を掛ける。閉扉の作り物は 22 dB 下がるが
    ///                 **半開きの正しい回折まで下がる**。戸口の方向の手がかりが消える
    ///                 （回折の取り分 10° で 19.1% → 0.23%、60° で 5.2% → 2.5%。試聴で判明）
    ///   3 A と B の両方
    ///   ★どれも角度に二値を置かない。1 は経路の幾何、2 は口の帳簿。
    ///   ★2 は戸口（rooms::Aperture）が要る。部屋が割れていない場面では何も起きない。
    ///     そこは別途「密閉の旗」で埋める案がある（docs/CORE_DIFF.md）。
    int leakModel = 1;
    /// 戸口越しの後期に向きを付ける（段 2-f、2026-09-11）。**既定 1。**0 で旧（後期を丸ごと耳の部屋の FDN へ一様に）。
    ///   試聴の指摘「同じ部屋は LEV でいいが、隣の部屋では扉からの指向性が強いはず」から。
    ///   レイの後期を「放射した面が耳と同じ部屋に面しているか」で分け、戸口越しの分を**音源の部屋の FDN** へ
    ///   向き付きで送る。向きと集まり具合はレイが測った物（戸口の中心から 1〜2°、R 0.97。docs/CORE_DIFF.md ④）。
    ///   ★同じ部屋の音源では何も変わらない（戸口越しの分は耳の部屋へ畳む）。部屋が割れない場面（CORE_DIFF ①）でも変わらない。
    ///   ★退けた書き方: 旧コアの式（戸口の立体角 × α²）。戸口が要るうえ、戸口から 3 m で戸口越しをレイの約 3 倍に見積もる。
    ///   ── 段 2-g（2026-09-12）で 3 段にした。**既定 2。** ──
    ///   0 旧（後期を丸ごと耳の部屋の FDN へ一様に）／1 案1（戸口越しの分を音源の部屋の FDN へ、レーンの点で）
    ///   2 戸口の線音源（音源の部屋の尾を戸口の横幅に並べた 5 点から HRTF で。耳の部屋の分のうち戸口から入った分は
    ///     戸口の線音源から耳の部屋の FDN へ流す）。試聴の指摘「向こうの部屋の残響が全体から聞こえすぎ」から。
    ///   ★2 でも、耳の部屋と戸口で繋がっていない部屋（部屋が割れない 1.2 m の戸口など）は 1 の形に落ちる。
    int lateThrough = 2;
    /// 戸口寄せ（2026-09-12）。lateThrough=2 のとき、耳の部屋へ流す分のうちこの割合を戸口の線音源から直接鳴らす。
    ///   0 ＝ レイの割合のまま（既定）。1 ＝ 戸口から入った分を全部戸口から鳴らす（自分の部屋の響きは戸口を通らなかった分だけ）。
    ///   総量は変えない。実行中に動かしてよい（distribute の平滑した値に掛かるので段にならない）。
    float doorPull = 0.0f;
    /// 戸口の線音源の低域の相関の境（Hz、2026-09-12）。この境より下は 5 点が同じ波形、上は点ごとに別の波形。0 で旧（全帯域を別々に）。
    ///   FdnRoomMix::setPortalCoherence に毎フレーム置く。**既定 3000。**IACC の探り（AF_ONLY=doorcoh、戸口の正面 0.5〜3 m）:
    ///     0（旧）0.34〜0.54 ／ 500: 0.34〜0.53（低域はもともと相関が高く、ほぼ動かない）／ 1500: 0.43〜0.54 ／
    ///     3000: 0.59〜0.62（距離によらず、旧の 3 m と同じ）／ 6000: 0.79〜0.90 ／ 全帯域: 0.78〜0.95（点に寄りすぎて幅が消える）
    ///   物理の目安（点の間隔 0.2 m で 850 Hz 以上は無相関）より上だが、ここは耳で決める所。
    float doorCoherenceHz = 3000.0f;
    /// 先着の重み（2026-09-12）。最初の到達（直接の到達時刻）から遅れる到来ほど出口の量を下げる（帳簿は物理のまま）。
    ///   precedenceDb: 十分遅い到来の下げ幅（dB）。0 で今までと 1 ビットも同じ。precedenceSec: 窓（先行音効果の 40 ms が目安）。
    ///   発注者の指示「ゲームだから完全物理でなく、聞こえ方がいい物を」。直接・透過は変わらず、部屋の響きが下がり、戸口の線音源は中間。
    ///   ★エンジンの既定は 0（物理のまま。検査と AfFlowWav は「出口の和 ＝ 帳簿」を見張る）。演出の既定は載る側（Unity は 6 dB）が持つ。
    float precedenceDb = 0.0f;
    float precedenceSec = 0.04f;
    /// 壁越しの反射（2026-09-12）。**既定 0 ＝ 通さない。**1 で旧（壁を横切った影の線も τ を掛けて初期・後期に数える）。
    ///   試聴「壁の向こうの透過音がダブる。反射や残響は壁を抜けないから、透過は直接だけにしてほしい」。
    ///   壁を抜けるのは解析で出す透過の直接音（distribute の kTransmit）だけになる。開いた戸口を通る分は変わらない。
    int wallReflect = 0;
    /// 尾のレーンの作り（FdnRoomMix::setLaneModel、2026-09-12）。**既定 1。**実行中に切り替えてよい（試聴の A/B）。
    ///   0 耳ごとの行（88d5f0b〜）: 点の向きでも左右が別の波形。ITD が効かず、向きが変わると尾の波形が入れ替わる
    ///   1 点と拡散を分ける: 点は 1 本の波形＋点の向きの ITD。自室（広がり 1）は 0 と 1 ビットも同じ
    ///   ★効くのはレーンを通る尾だけ。lateThrough=2 で戸口の線音源になった部屋はレーンを通らない。
    int laneModel = 1;
    /// 虚像の次数（1..3）。既定 2。
    ///   ★3 にすると壁際で「詰まった連続反射」が出る。同じ壁を繰り返し使う経路が 3 次で初めて現れるため。
    ///     2 次までだと、近づいた壁が絡む虚像だけが前へ寄り、群としては詰まらない
    ///     （実測: 広がりが 1 m より近くで 4.8 ms から縮まず 5.1 ms へ戻る）。
    ///   ★費用は候補の数で効く（箱 6 面なら 1 次 6・2 次 30・3 次 120）。厳密の段だけに掛ける。
    int imageOrder = 2;
    /// レイを GPU で解くか（0 切／1 入。**既定 0**）。2026-09-10。
    ///   ★音は作らない。GPU が出すのは幾何と統計（レイの当たりと帳簿）だけで、
    ///     音にするのは今までどおりエンジン（CPU）。決めごと（音の計算はエンジンだけ）と当たらない。
    ///   ★ホスト（Unity）のデバイスは借りない。エンジンが自前で持つので、
    ///     検査と AfFlowWav が Unity 無しで回せる。
    ///   ★CPU とビット一致はしない（丸めも sin/cos も違う）。実測で初期・後期とも ±0.05 dB。
    ///     静止していれば毎フレーム同じ、という性質は種の式が同じなので保てる。
    ///   ★GPU が無い機械や、シェーダが積めない場合は黙って CPU のまま動く。
    ///   ★入れている間はワーカーを使わない（器が 1 つなので、複数スレッドから同時に流せない）。
    int gpuTrace = 0;
    /// GPU の道が実際に使えているか（診断）。gpuTrace=1 でも false なら CPU で回っている。
    bool gpuActive() const { return gpuReady_ && gpuTrace != 0; }
    const std::string& gpuAdapter() const { return gpuTracer_.adapterName(); }
    const std::string& gpuError() const { return gpuTracer_.error(); }
    bool fdnStale() const { return fdnStale_; }
    /// 部屋グラフのボクセル一辺(m)。**戸口の幅を数ボクセルで割れる大きさ**にすること。
    ///   粗いと戸口で部屋が割れず、2 部屋が 1 部屋に潰れる（＝扉を閉めても響きが変わらない）。
    ///   build() の前に置く。
    void setRoomCell(float meters) { builder_.setCell(meters); dirty_ = true; }
    float roomCellRequested() const { return builder_.cell(); }
    /// 実際に使われた一辺。総ボクセル数が上限を超えると 1.5 倍ずつ粗くなるので、要求と食い違うことがある。
    ///   ★食い違ったら音の結果が変わっている。呼び出し側が 2 つを比べて気づけるように分けてある。
    float roomCellEffective() const { return builder_.result().grid.cell; }
    double roomVoxels() const {
        const rooms::Grid& g = builder_.result().grid;
        return static_cast<double>(g.nx) * g.ny * g.nz;
    }
    double roomMaxVoxels() const { return static_cast<double>(builder_.maxVoxels()); }
    int  roomCount() const { return static_cast<int>(probes_.size()); }
    const Probe& probe(int r) const { return probes_[static_cast<std::size_t>(r)]; }
    int  apertureCount() const { return static_cast<int>(apertures_.size()); }
    const rooms::Aperture& aperture(int i) const { return apertures_[static_cast<std::size_t>(i)]; }
    /// 戸口 i の素通しの割合（1 − 板の覆い）。update の後に読む。
    float apertureOpenFrac(int i) const { return (i >= 0 && i < static_cast<int>(openFrac_.size())) ? openFrac_[static_cast<std::size_t>(i)] : 1.0f; }
    int  buildCount() const { return buildCount_; }
    int  roomAt(const Vec3& p) const {
        const rooms::Grid& g = builder_.result().grid;
        if (g.v.empty()) return -1;
        return builder_.roomAtVoxel(static_cast<int>((p.x - g.origin.x) / g.cell),
                                    static_cast<int>((p.y - g.origin.y) / g.cell),
                                    static_cast<int>((p.z - g.origin.z) / g.cell));
    }

    /// 段 2-g: 戸口の線音源の診断（update の後に読む。FDN が繋がっていないと空）。
    struct PortalDiag {
        int   room = -1, aperture = -1;
        float spanDeg = 0.0f;                 // 戸口の縁どうしの見込み角（横幅）
        float directGain = 0.0f, feedGain = 0.0f, visible = 0.0f;
        float pointGain[af::dsp::FdnRoomMix::kPortalPoints] = {};
        float pointAz[af::dsp::FdnRoomMix::kPortalPoints] = {};   // 点の方位（度、+ が右）
        float pointPos[af::dsp::FdnRoomMix::kPortalPoints * 3] = {};   // 点の位置（world。配分タブの地図用）
    };
    const std::vector<PortalDiag>& portalDiag() const { return portalDiag_; }

    /// レイが見る平らな場面（検査用。update の後に読む）。
    const TraceScene& traceScene() const { return traceScene_; }
    /// 部屋 r の尾が来る向き（リスナー座標）と広がり（0 点〜1 一様）。updateFdn が置いた物（段 2-f）。無ければ false。
    bool fdnDirection(int r, Vec3& dirLocal, float& spread) const {
        if (r < 0 || static_cast<std::size_t>(r) * 4 + 3 >= fdnDir_.size()) return false;
        const std::size_t i = static_cast<std::size_t>(r) * 4;
        dirLocal = Vec3(fdnDir_[i], fdnDir_[i + 1], fdnDir_[i + 2]);
        spread = fdnDir_[i + 3];
        return true;
    }

    // ── リスナーと音源 ──
    void setListener(const Vec3& pos, const Vec3& forward, const Vec3& up) { listener_.pos = pos; listener_.forward = forward; listener_.up = up; }
    const Listener& listener() const { return listener_; }
    int addEmitter(const Vec3& pos, float radius) {
        for (std::size_t i = 0; i < slots_.size(); ++i) if (!slots_[i].used) return reuse(static_cast<int>(i), pos, radius);
        slots_.emplace_back();
        return reuse(static_cast<int>(slots_.size()) - 1, pos, radius);
    }
    void removeEmitter(int id) { if (valid(id)) { slots_[static_cast<std::size_t>(id)].used = false; slots_[static_cast<std::size_t>(id)].mix.clear(); } }
    void setEmitter(int id, const Vec3& pos, float radius, bool operated, float loudness) {
        if (!valid(id)) return;
        Emitter& e = slots_[static_cast<std::size_t>(id)].em;
        e.pos = pos; e.radius = radius; e.operated = operated; e.loudness = std::min(1.0f, std::max(0.0f, loudness));
    }
    const Emitter* emitter(int id) const { return valid(id) ? &slots_[static_cast<std::size_t>(id)].em : nullptr; }
    const Mix* mix(int id) const { return valid(id) ? &slots_[static_cast<std::size_t>(id)].mix : nullptr; }
    const TraceResult* trace(int id) const { return valid(id) ? &slots_[static_cast<std::size_t>(id)].trace : nullptr; }
    const Visibility* visibility(int id) const { return valid(id) ? &slots_[static_cast<std::size_t>(id)].vis : nullptr; }
    const Diffraction* diffraction(int id) const { return valid(id) ? &slots_[static_cast<std::size_t>(id)].diff : nullptr; }
    const ImageSet* images(int id) const { return valid(id) ? &slots_[static_cast<std::size_t>(id)].images : nullptr; }
    /// ISM の面（虚像の face 番号が指す物）。配分タブの地図が壁の上の反射点を出すのに使う。
    const std::vector<Face>& faces() const { return faces_; }
    int faceCount() const { return static_cast<int>(faces_.size()); }

    // ── FDN の器 ──
    void bindFdn(af::dsp::FdnRoomMix* fdn) {
        fdn_ = fdn;
        fdnRoomOf_.assign(probes_.size(), -1);
        fdnStale_ = false;
        if (!fdn_) return;
        for (std::size_t r = 0; r < probes_.size(); ++r)
            fdnRoomOf_[r] = fdn_->addRoom(fdnLineScale(probes_[r]), probes_[r].rt60, false);
    }
    const int* fdnRoomOfProbe() const { return fdnRoomOf_.empty() ? nullptr : fdnRoomOf_.data(); }

    // ── 1 フレーム ──
    void update(float dt) {
        if (dirty_) build();
        // ★当たり判定の木を毎フレーム作り直す。動く箱があるので、作った木をそのまま持つと
        //   包みが古くなり、当たるべき箱を枝刈りで捨てる。数十個なら作り直しでも数 us。
        //   木があれば nearest / transmittance が木を歩く。答えは総当たりと 1 ビットも変わらない（検査で担保）。
        surfaces.rebuildBvh();
        // ★レイが見る場面を平らな配列にして 1 フレームに 1 回だけ組む。
        //   音源ごとに組み直すと箱の数 × 音源の数だけ無駄が出る。GPU へ送るのもこの 1 つ。
        buildTraceScene(surfaces, rules.materials, traceScene_);
        // GPU の道（既定は切）。1 回だけ積んで、以後は毎フレーム場面を送るだけ。
        if (gpuTrace != 0 && !gpuTried_) { gpuTried_ = true; gpuReady_ = gpuTracer_.init(); }
        if (gpuActive() && !gpuTracer_.upload(traceScene_)) gpuReady_ = false;   // 送れなくなったら CPU へ戻る
        listener_.beginFrame(dt);
        updateOpenings();
        now_ += dt;
        const int lroom = roomAt(listener_.pos);
        const float mixingSec = mixingSecFor(lroom);
        lroom_ = lroom;
        // 段 2-g: 耳の部屋と戸口で繋がる部屋ごとに、使う戸口を 1 つ選ぶ（面積 × 開き具合がいちばん大きい物）。
        //   ★閉じた扉でも選ぶ（開き具合は下限 1e-3 で並べるだけ）。量のほうが開き具合で 0 へ寄るので段にならない。
        doorOf_.assign(probes_.size(), -1);
        if (lateThrough == 2 && lroom >= 0) {
            std::vector<float> bestScore(probes_.size(), 0.0f);
            for (std::size_t a = 0; a < apertures_.size(); ++a) {
                const rooms::Aperture& ap = apertures_[a];
                const int other = (ap.roomA == lroom) ? ap.roomB : ((ap.roomB == lroom) ? ap.roomA : -1);
                if (other < 0 || other >= roomCount()) continue;
                const float score = ap.area * std::max(openFrac_[a], 1e-3f);
                if (score > bestScore[static_cast<std::size_t>(other)]) {
                    bestScore[static_cast<std::size_t>(other)] = score;
                    doorOf_[static_cast<std::size_t>(other)] = static_cast<int>(a);
                }
            }
        }

        // 生きている音源を集め、予算で段と本数を決める（段 8）。
        live_.clear(); liveIdx_.clear();
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (!s.used || !s.em.active) continue;
            s.em.beginFrame(dt);
            s.em.room = roomAt(s.em.pos);
            live_.push_back(&s.em); liveIdx_.push_back(static_cast<int>(i));
        }
        budget.update(live_, listener_.pos, dt, now_, raysPerEmitter, budgetSlots_);
        const int n = static_cast<int>(live_.size());

        // ── レイの注文を先に全部集める（段 2-e）──
        //   ★「何を解くか」を決める所と「解く」所を分けた。分けないと GPU へ束ねて出せない。
        //     決め方は前と 1 文字も変えていないので、CPU の答えも前と同じ。
        jobs_.clear(); jobRange_.assign(static_cast<std::size_t>(n), JobRange{});
        for (int k = 0; k < n; ++k) {
            Slot& s = slots_[static_cast<std::size_t>(liveIdx_[static_cast<std::size_t>(k)])];
            const BudgetSlot& bs = budgetSlots_[static_cast<std::size_t>(k)];
            s.tier = static_cast<int>(bs.tier);
            s.rays = bs.rays;
            if (bs.rays <= 0) continue;                   // 保持: 最後の答えを保つ（Mix を触らない）
            const bool light = (bs.tier != Tier::Full);
            TraceParams prm;
            prm.rays = bs.rays; prm.maxBounces = light ? std::min(maxBounces, 12) : maxBounces; prm.mixingSec = mixingSec;
            prm.seed = static_cast<std::uint32_t>(s.em.id + 1) * 0x9E3779B1u;    // 音源ごとに固定
            prm.listenerRoom = (lateThrough != 0) ? lroom : -1;                  // 段 2-f。-1 なら出どころを分けない（今までと 1 ビットも同じ）
            prm.wallReflect = wallReflect;
            // フレーム分散（設計文書 Ⅶ）: 組を 1 つだけ飛ばし、残りは前回の結果を使う。
            //   本数が変わったら組を全部作り直す（重み 1/rays が変わるので混ぜられない）。
            const int G = std::max(1, std::min(rayGroups, TraceGroups::kMax));
            prm.groups = G;
            prm.group = s.groupNext % G;
            JobRange& jr = jobRange_[static_cast<std::size_t>(k)];
            jr.first = static_cast<int>(jobs_.size());
            if (s.groupRays != bs.rays || s.groupCount != G) {
                for (int g = 0; g < G; ++g) {
                    TraceParams pg = prm; pg.group = g;
                    jobs_.push_back(TraceJob{s.em.pos, pg, &s.parts[g]});
                }
                s.groupRays = bs.rays; s.groupCount = G;
            } else {
                jobs_.push_back(TraceJob{s.em.pos, prm, &s.parts[prm.group]});
            }
            jr.count = static_cast<int>(jobs_.size()) - jr.first;
            s.groupNext = (s.groupNext + 1) % G;
        }
        // ── GPU なら 1 回で全部流す。CPU なら音源ごとの解きの中で（並列のまま）解く。──
        const bool batched = gpuActive() && runTracesBatched();

        // 音源ごとの解き（互いに独立で、自分の枠にしか書かない → 並列化できる。既定は直列）。
        auto solve = [&](int k) {
            Slot& s = slots_[static_cast<std::size_t>(liveIdx_[static_cast<std::size_t>(k)])];
            const BudgetSlot& bs = budgetSlots_[static_cast<std::size_t>(k)];
            if (bs.rays <= 0) return;                     // 保持: 最後の答えを保つ（Mix を触らない）
            const bool light = (bs.tier != Tier::Full);
            const JobRange jr = jobRange_[static_cast<std::size_t>(k)];
            if (!batched)
                for (int j = jr.first; j < jr.first + jr.count; ++j)
                    *jobs_[static_cast<std::size_t>(j)].out =
                        runTrace(jobs_[static_cast<std::size_t>(j)].src, jobs_[static_cast<std::size_t>(j)].prm);
            const int G = std::max(1, s.groupCount);
            TraceResult::sumGroups(s.parts, G, s.trace);
            // 見通し（解析）。幅は見込み角で点へ寄せた物。
            //   ★段に関わらず**円盤のまま**解く（2026-09-10）。以前は簡易・保持を点に落としていたが、
            //     点だと扉が横切る瞬間に見通しが 0↔1 で切り替わり、半影にならない。
            //     音源 17 本で 1 フレーム 11.8 dB、24 本で 13.1 dB の跳びが出ていた（実測）。
            //     厳密の枠が 6 なので、6 本までは起きず**枠を超えた分から跳ぶ**のが症状の形だった。
            //   ★費用は遮りがある配置で 0.006 ms／音源。17 本で 0.1 ms、更新の中央 5.25 ms に対して 2%。
            //     同じ場面のレイ 512 本が 5.4 ms なので、削るなら統計（レイ・虚像）の側を削る。
            //     解析で出している物は削らない ── そこが段になると耳に付く。
            const float rEff = s.em.effectiveRadius(s.trace.directDist, 1.0f);
            s.vis = discVisibility(surfaces, rules.materials, listener_.pos, s.em.pos, rEff);
            // 回折（段 6）: 遮られた分が最寄りの稜線を回る。見通しが 1 なら要らない。
            s.diff = edgeDiffraction(surfaces, listener_, s.em.pos, s.vis, leakModel);
            // 案B: 回折の量に戸口の空き具合を掛ける。回折点にいちばん近い戸口を使う。
            //   ★量にだけ掛ける（候補の選び方は変えない）。選び方まで変えると案A と混ざって切り分けが効かない。
            if ((leakModel == 2 || leakModel == 3) && s.diff.valid) {
                float openF = 1.0f, bestD2 = 1e9f;
                for (std::size_t a = 0; a < apertures_.size(); ++a) {
                    const Vec3 d = s.diff.point - apertures_[a].rectCenter;
                    const float d2 = dot(d, d);
                    if (d2 < bestD2) { bestD2 = d2; openF = apertureOpenFrac(static_cast<int>(a)); }
                }
                for (int b = 0; b < kNumBands; ++b) s.diff.energy6[b] *= openF;
            }
            // 虚像（段 7）: 初期の方向と正規化重み。簡易は作らない（方向なしの 1 本に落ちる）。
            if (light) s.images.count = 0;
            else buildImages(surfaces, faces_, listener_, s.em.pos, rEff, mixingSec + 3.0f / kSpeedOfSound, s.images, imageOrder);
            DistributeInput in;
            in.trace = &s.trace; in.visibility = &s.vis; in.diffraction = &s.diff; in.images = &s.images;
            in.sourcePos = s.em.pos; in.listener = &listener_;
            in.listenerRoom = lroom; in.weights = &rules.weights; in.response = &response; in.dt = dt;
            in.sourceRoom = (lateThrough != 0) ? s.em.room : -1;
            in.precedenceDb = precedenceDb; in.precedenceSec = precedenceSec;
            {
                const int er = s.em.room;
                const int door = (lateThrough == 2 && er >= 0 && er < static_cast<int>(doorOf_.size())) ? doorOf_[static_cast<std::size_t>(er)] : -1;
                in.doorSource = (door >= 0);
                in.doorFeed = (door >= 0) ? openFrac_[static_cast<std::size_t>(door)] : 0.0f;
                in.doorPull = doorPull;
            }
            s.mixer.run(in, s.mix);
        };
        // ★GPU が入っているときは並列にしない。計算の器が 1 つしかないので、
        //   複数のスレッドから同時に流すと出力バッファを取り合う。
        //   そもそも GPU 側が桁で速いので、主スレッドから順に流しても足りる。
        if (pool_ && pool_->size() > 1 && n > 1 && !gpuActive()) pool_->parallelFor(n, solve);
        else for (int k = 0; k < n; ++k) solve(k);
        spentRays_ = Budget::spentRays(budgetSlots_, n);
        updateFdn();
    }

    /// 音源ごとのループを複数コアへ（0/1 で直列。既定は直列。★Unity は既に全コアを使っているので黙って増やさない）。
    void setWorkers(int workers) {
        if (workers <= 1) { pool_.reset(); return; }
        pool_ = std::make_unique<WorkerPool>(workers);
    }
    int  workers() const { return pool_ ? pool_->size() : 1; }
    int  spentRays() const { return spentRays_; }
    int  tierOf(int id) const { return valid(id) ? slots_[static_cast<std::size_t>(id)].tier : 2; }
    int  raysOf(int id) const { return valid(id) ? slots_[static_cast<std::size_t>(id)].rays : 0; }

    /// 配分を VoiceRenderer へ（橋）。
    void applyToVoice(int id, af::dsp::VoiceRenderer& v, int sampleRate) const {
        if (!valid(id)) return;
        applyMixToVoice(slots_[static_cast<std::size_t>(id)].mix, v, sampleRate, headCm, fdnRoomOfProbe(), roomCount());
    }

private:
    struct Slot {
        Emitter      em;
        EmitterMixer mixer;
        Mix          mix;
        TraceResult  trace;
        Visibility   vis;
        Diffraction  diff;
        ImageSet     images;
        int          tier = 2, rays = 0;
        // フレーム分散（段 8）: 組ごとの結果を持ち、毎フレーム 1 組だけ更新して足し合わせる
        TraceResult  parts[TraceGroups::kMax];
        int          groupNext = 0, groupRays = -1, groupCount = 0;
        bool         used = false;
    };
    bool valid(int id) const { return id >= 0 && id < static_cast<int>(slots_.size()) && slots_[static_cast<std::size_t>(id)].used; }
    int reuse(int id, const Vec3& pos, float radius) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        s = Slot{};
        s.used = true; s.em.id = id; s.em.pos = pos; s.em.prevPos = pos; s.em.radius = radius;
        s.mixer.reset(); s.mix.clear();
        return id;
    }
    float mixingSecFor(int room) const {
        if (room < 0 || room >= roomCount()) return 0.03f;
        const float mfp = probes_[static_cast<std::size_t>(room)].meanFreePath;
        return std::max(0.01f, std::min(0.12f, 3.0f * mfp / kSpeedOfSound));
    }
    /// 戸口ごとに動く箱の覆いを出し、OpeningState を組み、プローブの RT60 を出し直す。
    void updateOpenings() {
        openings_.clear();
        openFrac_.assign(apertures_.size(), 1.0f);
        for (std::size_t a = 0; a < apertures_.size(); ++a) {
            const rooms::Aperture& ap = apertures_[a];
            float cover = 0.0f; int leafMat = -1; float best = 0.0f;
            for (int i = 0; i < surfaces.count(); ++i) {
                const Surface& s = surfaces.at(i);
                if (!s.active || !s.dynamic) continue;
                const float c = openingCoverage(ap, s.obb);
                if (c <= 0.0f) continue;
                cover += c;
                if (c > best) { best = c; leafMat = s.material; }
            }
            cover = std::min(1.0f, cover);
            openFrac_[a] = 1.0f - cover;
            if (leafMat < 0) continue;                        // 板が無い口は穴のまま（プローブの既定）
            const AcousticMaterial& m = rules.materials.get(leafMat);
            for (int side = 0; side < 2; ++side) {
                const int room = (side == 0) ? ap.roomA : ap.roomB;
                if (room < 0) continue;
                OpeningState o; o.room = room; o.area = ap.area; o.openFrac = openFrac_[a];
                for (int b = 0; b < kNumBands; ++b) o.leafAbsorb[b] = std::min(1.0f, m.absorption[b] + m.transmission[b]);
                openings_.push_back(o);
            }
        }
        for (std::size_t r = 0; r < probes_.size(); ++r)
            applyOpenings(probes_[r], static_cast<int>(r), openings_.data(), static_cast<int>(openings_.size()));
    }
    void updateFdn() {
        if (!fdn_ || probes_.empty()) return;
        fdn_->setLaneModel(laneModel);                                     // 尾のレーンの作り（実行中に切り替えてよい）
        for (std::size_t r = 0; r < probes_.size(); ++r)
            if (fdnRoomOf_[r] >= 0) fdn_->setRoomRt60(fdnRoomOf_[r], probes_[r].rt60, false);
        fdn_->setPortalCoherence(doorCoherenceHz);
        std::vector<float> sum(probes_.size() * kNumBands, 0.0f);
        // 段 2-f: 部屋ごとの尾の向き。送りのエネルギー（帯域の平均）× 集まり具合 × 向き を足し、長さ ÷ 重さ がその部屋の集まり具合。
        //   ★耳の部屋へ行く分は集まり 0 なので重さだけ増える ＝ 同じ部屋に戸口越しと自室が混ざれば、その分だけ広がる。
        std::vector<double> dirSum(probes_.size() * 3, 0.0), dirMass(probes_.size(), 0.0);
        std::vector<double> portalT(probes_.size(), 0.0), portalE(probes_.size(), 0.0);    // 段 2-g
        for (const Slot& s : slots_) {
            if (!s.used) continue;
            for (int k = 0; k < s.mix.sendCount; ++k) {
                const FdnSend& sd = s.mix.sends[k];
                if (sd.room < 0 || sd.room >= roomCount()) continue;
                for (int b = 0; b < kNumBands; ++b) sum[static_cast<std::size_t>(sd.room) * kNumBands + b] += sd.e6[b];
                double mean = 0.0;
                for (int b = 0; b < kNumBands; ++b) mean += sd.e6[b];
                mean /= kNumBands;
                const std::size_t ri = static_cast<std::size_t>(sd.room);
                dirMass[ri] += mean;
                for (int q = 0; q < 3; ++q) dirSum[ri * 3 + q] += mean * sd.focus * sd.dir[q];
                // 段 2-g: 戸口の線音源の量。thru6 は戸口から耳へ直接出す分、残り（e6 − thru6）は耳の部屋の FDN へ流す分。
                //   流す分は耳の部屋の帯域の形にも入れる（耳の部屋の FDN の出口の形は、入る物の合計で決める）。
                if (lateThrough == 2 && ri < doorOf_.size() && doorOf_[ri] >= 0) {
                    double t = 0.0;
                    for (int b = 0; b < kNumBands; ++b) t += sd.thru6[b];
                    portalT[ri] += t / kNumBands;
                    portalE[ri] += mean;
                    if (lroom_ >= 0 && lroom_ < roomCount())
                        for (int b = 0; b < kNumBands; ++b)
                            sum[static_cast<std::size_t>(lroom_) * kNumBands + b] += std::max(0.0f, sd.e6[b] - sd.thru6[b]);
                }
            }
        }
        for (std::size_t r = 0; r < probes_.size(); ++r) {
            if (fdnRoomOf_[r] < 0) continue;
            float mean = 0.0f; for (int b = 0; b < kNumBands; ++b) mean += sum[r * kNumBands + b]; mean /= kNumBands;
            float w[kNumBands];
            for (int b = 0; b < kNumBands; ++b) w[b] = (mean > 1e-20f) ? std::sqrt(sum[r * kNumBands + b] / mean) : 0.0f;
            fdn_->setListenerWeight(fdnRoomOf_[r], w);
            // 段 2-f: 向きと広がり。戸口越しの送りが無い部屋は一様（広がり 1）。
            //   向きはワールドで束ねて、置く直前にリスナー座標へ写す（組を回している間に振り向いても古い向きにならない）。
            float dirLocal[3] = {0.0f, 0.0f, 1.0f};
            float spread = 1.0f;
            const double dx = dirSum[r * 3], dy = dirSum[r * 3 + 1], dz = dirSum[r * 3 + 2];
            const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dirMass[r] > 1e-20 && len > 1e-20) {
                const Vec3 dl = listener_.toLocal(Vec3(static_cast<float>(dx / len), static_cast<float>(dy / len), static_cast<float>(dz / len)));
                dirLocal[0] = dl.x; dirLocal[1] = dl.y; dirLocal[2] = dl.z;
                spread = 1.0f - static_cast<float>(std::min(1.0, len / dirMass[r]));
            }
            if (fdnDir_.size() != probes_.size() * 4) fdnDir_.assign(probes_.size() * 4, 0.0f);
            fdnDir_[r * 4] = dirLocal[0]; fdnDir_[r * 4 + 1] = dirLocal[1]; fdnDir_[r * 4 + 2] = dirLocal[2]; fdnDir_[r * 4 + 3] = spread;
            if (lateThrough != 0) fdn_->setListenerDirection(fdnRoomOf_[r], dirLocal, spread);   // 0 のときは触らない（旧と 1 ビット同じ）
        }
        // ── 段 2-g: 戸口の線音源 ──
        //   戸口の横幅に 5 点（幅の −0.8, −0.4, 0, +0.4, +0.8）。点ごとに向き（リスナー座標）と見通しを出し、
        //   見通しで量を配る（Σ² = 1）。見通しは戸口の面の点から 1 m 奥まで伸ばして測る ── 向こうへ開いた扉の板は
        //   戸口の面より奥にあるので、面の点までだと隠れない。
        //   ★幅は枠の外接矩形の半幅そのもの（1.0 m の戸口で 0.49 m）。端の点は幅の 0.8 に置くので枠の箱には沈まない。
        //     最初は「外接矩形はセル 1 個ぶんの厚みを含む」の注記から半セルを引いたが、それは厚みの向きの話で、
        //     横幅が 0.36 m に縮んで見込み角が 19° → 13.8° になった（検査 [戸口の線音源] ① で発覚）。
        portalDiag_.clear();
        const int K = af::dsp::FdnRoomMix::kPortalPoints;
        int slot = 0;
        const bool listenerHasFdn = lroom_ >= 0 && lroom_ < roomCount() && fdnRoomOf_[static_cast<std::size_t>(lroom_)] >= 0;
        if (lateThrough == 2 && listenerHasFdn) {
            for (std::size_t r = 0; r < probes_.size() && slot < af::dsp::FdnRoomMix::kMaxPortals; ++r) {
                if (r >= doorOf_.size() || doorOf_[r] < 0 || fdnRoomOf_[r] < 0 || portalE[r] <= 1e-30) continue;
                const rooms::Aperture& ap = apertures_[static_cast<std::size_t>(doorOf_[r])];
                const bool uIsWidth = std::fabs(ap.axisU.y) <= std::fabs(ap.axisV.y);
                const Vec3 wAxis = uIsWidth ? ap.axisU : ap.axisV;
                const float halfW = std::max(0.05f, uIsWidth ? ap.halfU : ap.halfV);
                // 戸口の中に立ったとき（点が耳に近すぎるとき）の向き: 耳の部屋から向こうの部屋へ向かう法線
                const Vec3 toOther = (ap.roomB == static_cast<int>(r)) ? ap.normal : ap.normal * -1.0f;
                float dirs[af::dsp::FdnRoomMix::kPortalPoints * 3], gains[af::dsp::FdnRoomMix::kPortalPoints], vis[af::dsp::FdnRoomMix::kPortalPoints];
                PortalDiag dg;
                dg.room = static_cast<int>(r); dg.aperture = doorOf_[r];
                double vsum = 0.0;
                for (int k = 0; k < K; ++k) {
                    const float off = (K > 1) ? (-0.8f + 1.6f * static_cast<float>(k) / static_cast<float>(K - 1)) : 0.0f;
                    const Vec3 pt = ap.rectCenter + wAxis * (off * halfW);
                    Vec3 d = pt - listener_.pos;
                    float len = length(d);
                    const Vec3 u = (len > 0.05f) ? d * (1.0f / len) : toOther;
                    const Vec3 dl = listener_.toLocal(u);
                    dirs[k * 3] = dl.x; dirs[k * 3 + 1] = dl.y; dirs[k * 3 + 2] = dl.z;
                    float tau[kNumBands];
                    surfaces.transmittance(listener_.pos, pt + u * 1.0f, -1, rules.materials, tau);
                    float v = 0.0f;
                    for (int b = 0; b < kNumBands; ++b) v += tau[b];
                    vis[k] = v / kNumBands;
                    vsum += vis[k];
                    dg.pointAz[k] = std::atan2(dl.x, dl.z) * 180.0f / 3.14159265f;
                    dg.pointPos[k * 3] = pt.x; dg.pointPos[k * 3 + 1] = pt.y; dg.pointPos[k * 3 + 2] = pt.z;
                }
                for (int k = 0; k < K; ++k)
                    gains[k] = (vsum > 1e-4) ? static_cast<float>(std::sqrt(vis[k] / vsum)) : static_cast<float>(std::sqrt(1.0 / K));
                const double E = portalE[r], T = std::min(portalT[r], E);
                const float directG = static_cast<float>(std::sqrt(T / E));
                const float feedG = static_cast<float>(std::sqrt(std::max(0.0, E - T) / E));
                fdn_->setPortal(slot, fdnRoomOf_[r], fdnRoomOf_[static_cast<std::size_t>(lroom_)], dirs, gains, K, directG, feedG, headCm);
                // 診断: 戸口の縁どうしの見込み角（横幅）
                {
                    Vec3 a0 = (ap.rectCenter - wAxis * halfW) - listener_.pos, a1 = (ap.rectCenter + wAxis * halfW) - listener_.pos;
                    const float l0 = length(a0), l1 = length(a1);
                    const float c = (l0 > 1e-4f && l1 > 1e-4f) ? dot(a0, a1) / (l0 * l1) : -1.0f;
                    dg.spanDeg = std::acos(std::min(1.0f, std::max(-1.0f, c))) * 180.0f / 3.14159265f;
                }
                dg.directGain = directG; dg.feedGain = feedG; dg.visible = static_cast<float>(vsum / K);
                for (int k = 0; k < K; ++k) dg.pointGain[k] = gains[k];
                portalDiag_.push_back(dg);
                ++slot;
            }
        }
        for (; slot < af::dsp::FdnRoomMix::kMaxPortals; ++slot) fdn_->setPortal(slot, -1, -1, nullptr, nullptr, 0, 0.0f, 0.0f);
    }

    rooms::Builder builder_;
    std::vector<Probe> probes_;
    std::vector<rooms::Aperture> apertures_;
    std::vector<Face> faces_;
    std::vector<OpeningState> openings_;
    std::vector<float> openFrac_;
    Listener listener_;
    std::vector<Slot> slots_;
    af::dsp::FdnRoomMix* fdn_ = nullptr;
    std::vector<int> fdnRoomOf_;
    bool dirty_ = true;
    bool fdnStale_ = false;
    TraceScene   traceScene_;   // レイが見る平らな場面（毎フレーム組み直す）
    std::vector<float> fdnDir_; // 段 2-f。部屋ごとの尾の向き（リスナー座標 xyz ＋ 広がり）。診断用
    int lroom_ = -1;                // 段 2-g。このフレームの耳の部屋（updateFdn が使う）
    std::vector<int> doorOf_;       // 段 2-g。部屋 → 耳の部屋と繋ぐ戸口の番号（-1 なし）
    std::vector<PortalDiag> portalDiag_;
    mutable gpu::GpuTracer gpuTracer_;
    bool gpuTried_ = false, gpuReady_ = false;

    /// レイの注文（どの音源の、どの組を、何本）。
    struct TraceJob { Vec3 src; TraceParams prm; TraceResult* out; };
    struct JobRange { int first = 0, count = 0; };
    mutable std::vector<TraceJob>  jobs_;
    std::vector<JobRange>  jobRange_;
    mutable std::vector<gpu::BatchJob> gpuJobs_;

    /// 集めた注文を **1 回のディスパッチ**で解く。流せなければ false（呼び出し側が CPU で解き直す）。
    ///   ★直接音はここでも CPU で出す（決定的なので GPU へ持っていく理由が無い）。
    bool runTracesBatched() const {
        if (jobs_.empty()) return true;
        gpuJobs_.clear(); gpuJobs_.reserve(jobs_.size());
        for (std::size_t j = 0; j < jobs_.size(); ++j) {
            *jobs_[j].out = TraceResult{};
            fillDirect(traceScene_, jobs_[j].src, listener_.pos, *jobs_[j].out);
            gpu::BatchJob g; g.source = jobs_[j].src; g.prm = jobs_[j].prm; g.out = jobs_[j].out;
            gpuJobs_.push_back(g);
        }
        return gpuTracer_.runBatch(traceScene_, listener_.pos, gpuJobs_.data(), static_cast<int>(gpuJobs_.size()));
    }

    /// レイを 1 音源ぶん解く。GPU が入っていれば GPU、そうでなければ CPU。
    TraceResult runTrace(const Vec3& src, const TraceParams& prm) const {
        if (!gpuActive()) return tracer_.run(traceScene_, src, listener_.pos, prm);
        TraceResult R;
        fillDirect(traceScene_, src, listener_.pos, R);
        if (!gpuTracer_.run(traceScene_, src, listener_.pos, prm, R))
            return tracer_.run(traceScene_, src, listener_.pos, prm);   // 流せなければ CPU で出し直す
        return R;
    }
    int  buildCount_ = 0;
    // 段 8
    EnergyTrace tracer_;
    std::vector<const Emitter*> live_;
    std::vector<int> liveIdx_;
    std::vector<BudgetSlot> budgetSlots_;
    std::unique_ptr<WorkerPool> pool_;
    float now_ = 0.0f;
    int   spentRays_ = 0;
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_WORLD_H
