/* Flow/world.h ── 世界: 更新の 1 サイクル（段 4）
 *
 * ■ 役割
 *   段 1〜3 の部品を束ねて「1 フレーム」を回す。ホストから見える唯一の物。
 *   持つ物: 決まり（rules）、面（surfaces）、部屋グラフとプローブ、リスナー、音源とその配分の状態、FDN の器への結び。
 *
 * ■ 中の仕組み
 *   build():  面のうち静的な箱を部屋グラフに渡し、部屋ごとにプローブを作る。FDN が結ばれていれば部屋を足す
 *             （colour=false: 減衰の形は RT60、帯域の量は配分が listenerWeight で載せる）。
 *   update(dt): 1) リスナーと音源の beginFrame（変化量）
 *               2) 音源ごとに: 部屋を引く → レイ（種 = 音源 id、境 = 3 × 平均自由行程 / c）→ 配分 → Mix
 *               3) FDN: 部屋ごとに RT60（開口の板込み。段 5 まで板は無い）と listenerWeight を置く。
 *                  listenerWeight_b = √(Σ_i E_i,b / mean_b Σ_i E_i,b)、送りは a_i = √(mean_b E_i,b · K)。
 *                  ★N 音源でも厳密: Σ_i a_i² · w_b² = K Σ_i E_i,b（mean は線形なので約分できる）。
 *   applyToVoice(): 橋（mix_to_voice.h）で VoiceRenderer へ。
 *   境 mixingSec = 3 × 平均自由行程 / c: ISM の次数 3 が埋める範囲（設計文書「FDN のエコー密度が十分になるまでを ISM が埋める」）。
 *     7 × 14 × 3 m で 30 ms。部屋の外（−1）は 30 ms 固定。
 *
 * ■ 繋がり
 *   受ける: ホスト（C API world_api.cpp）から 箱・材質・リスナー・音源・FDN の器・設定。
 *   渡す:   Mix（帳簿は情報タブへ）、VoiceRenderer への適用、FdnRoomMix への部屋と重み。
 *
 * ■ 退けた書き方
 *   ・非同期ワーカーを段 4 で入れる: 「一周を鳴らす」が先。1 音源 256 本で 3 ms なので、まず同期で聴く。
 *     非同期は段 8（負荷）で旧 scene_async.h の形を流用する。
 *   ・音源ごとに FDN の器を持つ: 設計文書「FDN は部屋ごと。音源数に比例しない」。器は 1 つ、部屋 = プローブ。
 *   ・listenerWeight を「リスナーの部屋 1 / 他 0」で置く: 隣室の尾（レイが扉を通って集計した後期）が鳴らない。
 *     配分の送り先（段 5 で「最後の反射の部屋」）が決めるべき物。
 *
 * ■ 壊れる所
 *   ・bindFdn を build の前に呼ぶと部屋が無い。順序: build → bindFdn（または bindFdn 済みなら build が足す）。
 *   ・FDN の器を差し替えたとき fdnRoomOf_ を作り直さないと、古い部屋番号へ送って範囲外。
 *   ・listenerWeight を置き忘れると FdnRoomMix の既定 0 で無音（旧実装で 1 回踏んだ）。update が毎フレーム置く。
 */
#ifndef ACOUSTICFLOW_FLOW_WORLD_H
#define ACOUSTICFLOW_FLOW_WORLD_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Core/room_graph.h"
#include "Dsp/fdn_room_mix.h"
#include "Dsp/voice_renderer.h"
#include "Flow/distribute.h"
#include "Flow/emitter.h"
#include "Flow/energy_trace.h"
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
    int   raysPerEmitter = 256;     // 段 8 で予算が決める
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
        dirty_ = false;
        ++buildCount_;
        // ★器に部屋を足し直さない。FdnRoomMix は部屋を消せない（オーディオが回っている前提の設計）ので、
        //   同じ器に足すと重複して 16 本の上限をすぐ使い切る。器を古い印にして、ホストが新しい器を作って結び直す
        //   （旧ホストの ReplaceFdnMix と同じ流儀。古い器は 1 秒おいて壊す）。印が立っている間は送り先が無い＝尾は鳴らない。
        if (fdn_) { fdn_ = nullptr; fdnRoomOf_.assign(probes_.size(), -1); fdnStale_ = true; }
    }
    /// 部屋グラフを作り直したので FDN の器を結び直す必要がある（bindFdn で消える）。
    bool fdnStale() const { return fdnStale_; }
    int  roomCount() const { return static_cast<int>(probes_.size()); }
    const Probe& probe(int r) const { return probes_[static_cast<std::size_t>(r)]; }
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
        const int lroom = roomAt(listener_.pos);
        const float mixingSec = mixingSecFor(lroom);
        EnergyTrace tracer;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (!s.used || !s.em.active) continue;
            s.em.beginFrame(dt);
            s.em.room = roomAt(s.em.pos);
            TraceParams prm;
            prm.rays = raysPerEmitter; prm.maxBounces = maxBounces; prm.mixingSec = mixingSec;
            prm.seed = static_cast<std::uint32_t>(s.em.id + 1) * 0x9E3779B1u;    // 音源ごとに固定
            s.trace = tracer.run(surfaces, rules.materials, s.em.pos, listener_.pos, prm);
            DistributeInput in;
            in.trace = &s.trace; in.sourcePos = s.em.pos; in.listener = &listener_;
            in.listenerRoom = lroom; in.weights = &rules.weights; in.response = &response; in.dt = dt;
            s.mixer.run(in, s.mix);
        }
        updateFdn();
    }

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
    void updateFdn() {
        if (!fdn_ || probes_.empty()) return;
        // 部屋ごとの生きた RT60（段 5 で開口の板が入る。いまは全開）
        for (std::size_t r = 0; r < probes_.size(); ++r) {
            applyOpenings(probes_[r], static_cast<int>(r), openings_.data(), static_cast<int>(openings_.size()));
            if (fdnRoomOf_[r] >= 0) fdn_->setRoomRt60(fdnRoomOf_[r], probes_[r].rt60, false);
        }
        // 部屋ごとの帯域の形 = 全音源の送りの和の形
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
    std::vector<OpeningState> openings_;     // 段 5 で扉から作る
    Listener listener_;
    std::vector<Slot> slots_;
    af::dsp::FdnRoomMix* fdn_ = nullptr;
    std::vector<int> fdnRoomOf_;
    bool dirty_ = true;
    bool fdnStale_ = false;
    int  buildCount_ = 0;
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_WORLD_H
