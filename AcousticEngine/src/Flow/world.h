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
    /// 虚像の次数（1..3）。既定 2。
    ///   ★3 にすると壁際で「詰まった連続反射」が出る。同じ壁を繰り返し使う経路が 3 次で初めて現れるため。
    ///     2 次までだと、近づいた壁が絡む虚像だけが前へ寄り、群としては詰まらない
    ///     （実測: 広がりが 1 m より近くで 4.8 ms から縮まず 5.1 ms へ戻る）。
    ///   ★費用は候補の数で効く（箱 6 面なら 1 次 6・2 次 30・3 次 120）。厳密の段だけに掛ける。
    int imageOrder = 2;
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
        listener_.beginFrame(dt);
        updateOpenings();
        now_ += dt;
        const int lroom = roomAt(listener_.pos);
        const float mixingSec = mixingSecFor(lroom);

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

        // 音源ごとの解き（互いに独立で、自分の枠にしか書かない → 並列化できる。既定は直列）。
        auto solve = [&](int k) {
            Slot& s = slots_[static_cast<std::size_t>(liveIdx_[static_cast<std::size_t>(k)])];
            const BudgetSlot& bs = budgetSlots_[static_cast<std::size_t>(k)];
            s.tier = static_cast<int>(bs.tier);
            s.rays = bs.rays;
            if (bs.rays <= 0) return;                     // 保持: 最後の答えを保つ（Mix を触らない）
            const bool light = (bs.tier != Tier::Full);
            TraceParams prm;
            prm.rays = bs.rays; prm.maxBounces = light ? std::min(maxBounces, 12) : maxBounces; prm.mixingSec = mixingSec;
            prm.seed = static_cast<std::uint32_t>(s.em.id + 1) * 0x9E3779B1u;    // 音源ごとに固定
            // フレーム分散（設計文書 Ⅶ）: 組を 1 つだけ飛ばし、残りは前回の結果を使う。
            //   本数が変わったら組を全部作り直す（重み 1/rays が変わるので混ぜられない）。
            const int G = std::max(1, std::min(rayGroups, TraceGroups::kMax));
            prm.groups = G;
            prm.group = s.groupNext % G;
            if (s.groupRays != bs.rays || s.groupCount != G) {
                for (int k = 0; k < G; ++k) s.parts[k] = tracer_.run(surfaces, rules.materials, s.em.pos, listener_.pos, TraceParams{prm.rays, prm.maxBounces, prm.mixingSec, prm.seed, G, k});
                s.groupRays = bs.rays; s.groupCount = G;
            } else {
                s.parts[prm.group] = tracer_.run(surfaces, rules.materials, s.em.pos, listener_.pos, prm);
            }
            s.groupNext = (s.groupNext + 1) % G;
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
            s.mixer.run(in, s.mix);
        };
        if (pool_ && pool_->size() > 1 && n > 1) pool_->parallelFor(n, solve);
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
        for (std::size_t r = 0; r < probes_.size(); ++r)
            if (fdnRoomOf_[r] >= 0) fdn_->setRoomRt60(fdnRoomOf_[r], probes_[r].rt60, false);
        std::vector<float> sum(probes_.size() * kNumBands, 0.0f);
        for (const Slot& s : slots_) {
            if (!s.used) continue;
            for (int k = 0; k < s.mix.sendCount; ++k) {
                const FdnSend& sd = s.mix.sends[k];
                if (sd.room < 0 || sd.room >= roomCount()) continue;
                for (int b = 0; b < kNumBands; ++b) sum[static_cast<std::size_t>(sd.room) * kNumBands + b] += sd.e6[b];
            }
        }
        for (std::size_t r = 0; r < probes_.size(); ++r) {
            if (fdnRoomOf_[r] < 0) continue;
            float mean = 0.0f; for (int b = 0; b < kNumBands; ++b) mean += sum[r * kNumBands + b]; mean /= kNumBands;
            float w[kNumBands];
            for (int b = 0; b < kNumBands; ++b) w[b] = (mean > 1e-20f) ? std::sqrt(sum[r * kNumBands + b] / mean) : 0.0f;
            fdn_->setListenerWeight(fdnRoomOf_[r], w);
        }
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
