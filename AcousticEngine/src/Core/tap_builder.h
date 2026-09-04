// tap_builder.h ── タップの組み立て（2026-09-05）。ホストの BuildTapsForSource（C# 170 行）を DLL へ移した物。
//
//   何を解くか: 「同じ問いに 2 つの答え」の最大の残り。ホストは直接タップの透過を **別の関数**
//   （AF_SceneComputeSoftOcclusion ＝ 音源まわりの円盤 32 点）で引き直していて、役割1 が出す生存
//   （computeDirectSoft ＝ 窓の走査線積分、09-05）と模型が違った。1/r・空気吸収・パン・尾の比・平滑・
//   HRTF に載せる回折タップの選び方も全部ホストにあり、別のホストへ持っていくと全部書き直しになる。
//   → 規則はここに 1 つ。ホストは AF_SceneGetVoiceProgram で受け取って畳み込み器へ写すだけ。
//
//   入る物（音源ごと）: 生存 6 帯域（反射込み）／到来方向／直接の半影 soft（振幅、役割1 の合成前）／
//     回折 dif（振幅、ポータル混合後）／面の線の R タップ／二次音源の F タップ／段／尾の代表。
//   出る物（AF_VoiceProgram）: タップ（遅延 ms・6 帯域ゲイン・パン・向き・種類・HRTF 重み）、直接音の向き、
//     HRTF に載せる回折タップ、ITDG、生存の平均、自由音場の直接レベル、尾の代表、尾の比（★音源ごと。#4）、
//     mixing time、部屋の中である割合。
//   ★ホストにあった規則をそのまま持ち込んだ物（値はホストの既定と同じ。変えていない）:
//     ・直接タップ ＝ soft × 傾き（transmissionTilt／HighCut／Gain）× 空気吸収 × 1/r。時間方向は dB で平滑（tapSmoothTime）
//     ・R タップ ＝ 面の線のゲイン × 空気吸収 × 1/r、遅延 = (経路長 − 直接距離)/c
//     ・F タップ ＝ 回折の配分 × 遮蔽割合 occFrac × diffractionGain、distanceOnly なら帯域は平坦。枠は maxTaps の末尾 8 本
//     ・直接音の向き ＝ 真の向きと到来方向を遮蔽の深さ（steerThreshold）で slerp → SmoothDamp（directionSmoothTime）
//     ・HRTF の回折タップ ＝ いちばん強い F。前フレームの開口（1.5 m 以内）を marginDb 上回るまで乗り換えない
//     ・尾の比 ＝ (r/rc)²、rc = 0.057√(V/RT60)、指数で圧縮、ソフトニー 50→ceiling、部屋の中である割合で縮める（#5）
//   ★変えた所（1 つだけ）: 直接タップの透過が computeSoftOcclusion（円盤 32 点）から役割1 の soft（走査線）へ。
//     遮蔽割合 occFrac は「低域の減衰量」（1 − soft[125 Hz]）で、旧の定義（1 − √g[0] の平均）と同じ量。
//   ⚠ 尾の比は音源ごと（不具合 #4）。ホストは音源 0 の距離だけで全音源の比を決めていた（2 m と 10 m で 25 倍ずれる）。
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "Core/vec3.h"

namespace acoustic {

/// ホストから渡す規則の値（デザイナのつまみ）。既定はホスト（AcousticFlowSceneDemo）の既定と同じ。
struct TapParams {
    float distanceRef = 1.5f;              // 1/r の基準距離(m)。0 で距離減衰なし
    float diffractionDistanceRef = 0.0f;   // F タップの基準距離。0 = distanceRef
    float diffractionDistancePower = 1.0f; // F タップの減衰の指数
    float airAbsorptionScale = 1.0f;       // 空気吸収の倍率
    float transmissionTilt = 1.0f;         // 透過のこもり（125 Hz 基準の傾きの指数）
    float transmissionHighCutDb = 0.0f;    // 透過の高域カット(dB @4 kHz)
    float transmissionGainDb = 0.0f;       // 透過の音量(dB)
    float diffractionGainDb = 6.0f;        // 回折タップの音量(dB)
    bool  diffractionDistanceOnly = true;  // 回折タップは方向と距離だけ（帯域は平坦）
    float diffractionHighCutDb = 0.0f;     // 回折の高域カット(dB @4 kHz)
    float tapSmoothTime = 0.08f;           // 透過・遮蔽割合の平滑（秒、dB 領域）
    float directionSmoothTime = 0.18f;     // 直接音の向きの平滑（秒、SmoothDamp）
    float steerThreshold = 0.2f;           // 遮蔽の深さがこれを超えたら向きを到来方向へ寄せ始める
    bool  diffractionHrtf = true;          // HRTF に回折タップを 1 本載せる
    float diffractionHrtfMarginDb = 2.0f;  // 乗り換えのヒステリシス(dB)
    float reverbRatioExponent = 0.5f;      // 尾の比の知覚圧縮（1 = 物理）
    float reverbRatioCeiling = 150.0f;     // 尾の比のソフトニーの天井（膝は 50）
    float roomBlendRadius = 2.0f;          // 部屋の混ぜ半径(m)
    bool  reverbShareFade = true;          // 外へ出るとき量を部屋の中である割合で縮める（#5）
    float fallbackRt60 = 0.5f;             // 部屋が取れないときの RT60（ホストのエコグラム由来）
    int   maxTaps = 64;                    // 音源 1 本のタップ上限（≤ 64）
    int   diffractionTapReserve = 8;       // F タップのために末尾に空けておく数
};

struct ProgramTap {
    float delayMs;        // 直接音を 0 とした相対遅延
    float gain[6];        // 6 帯域の振幅（1/r・空気吸収込み）
    float panL, panR;     // 等パワーのパン（到来点とリスナーの右）
    float dir[3];         // 到来方向（リスナー座標: +x 右／+y 上／+z 前）
    float arr[3];         // 到来点（世界）
    int   type;           // 0 直接／1 反射／2 回折
    float hrtfWeight;     // HRTF に載せる割合（回折 1 本だけ 1）
};

struct VoiceProgram {
    static constexpr int kMaxTaps = 64;
    int count = 0;
    ProgramTap taps[kMaxTaps] = {};
    float directDir[3] = {0.0f, 0.0f, 1.0f};   // 直接音の向き（リスナー座標、平滑後）
    int   hrtfTapIndex = -1;
    float hrtfDir[3] = {0.0f, 0.0f, 1.0f};
    float itdgMs = 0.0f;                        // 直接以外の最小遅延
    float sourceLevel = 1.0f;                   // 生存の平均（反射込み）。尾の送出量
    float freeFieldDirect = 1.0f;               // 自由音場の直接レベル（空気吸収 × 1/r、遮蔽なし）
    int   tailShapeIndex = -1;                  // 尾の形の代表
    float tailRatio = 0.0f;                     // 尾の比（圧縮・天井・割合込み）★音源ごと
    float tailRatioPhysical = 0.0f;             // 圧縮前 (r/rc)²
    float mixingTimeMs = 25.0f;                 // √V
    float roomShare = 1.0f;                     // 部屋の中である割合
    int   tier = 0;
};

class TapBuilder {
public:
    struct SourceIn {
        unsigned long long id = 0;
        Vec3 pos{};
        int tier = 0;
        const float* bands = nullptr;        // 6: 生存（反射込み）
        const float* arrivalDir = nullptr;   // 3: 到来方向（世界）
        const float* softAmp = nullptr;      // 6: 直接の半影（振幅）
        const float* dif = nullptr;          // 6: 回折（振幅）
        const Vec3* earlyPos = nullptr; const float* earlyGain = nullptr; int earlyCount = 0;
        const Vec3* diffPos = nullptr;  const float* diffGain = nullptr;  int diffCount = 0;
        int tailShapeIndex = -1;
    };
    struct SceneIn {
        Vec3 listener{}, fwd{0, 0, 1}, up{0, 1, 0}, right{1, 0, 0};
        float dt = 1.0f / 60.0f;
        float speedOfSound = 343.0f;
        bool  hasRoom = false;
        float roomVol = 0.0f, roomRt60 = 0.0f, roomShare = 1.0f;
        float fallbackVol = 0.0f;            // 部屋が無いときの外形箱の体積（0 = 何も無い → 尾の比 0）
    };

    void build(const TapParams& p, const SceneIn& sc, const SourceIn& s, VoiceProgram& out) {
        State& st = stateFor(s.id);
        out.count = 0;
        out.tier = s.tier;
        out.tailShapeIndex = s.tailShapeIndex;
        const Vec3 L = sc.listener, S = s.pos;
        const float directDist = length(S - L);
        const float toMs = 1000.0f / std::max(sc.speedOfSound, 1.0f);
        const float diffGainLin = (std::fabs(p.diffractionGainDb) > 0.01f) ? std::pow(10.0f, p.diffractionGainDb / 20.0f) : 1.0f;
        const int maxTaps = std::max(1, std::min(p.maxTaps, VoiceProgram::kMaxTaps));
        const int reserve = std::max(0, std::min(p.diffractionTapReserve, maxTaps - 1));

        // ── 直接タップ: 役割1 の半影（振幅）→ 傾き・音量 → dB で平滑 ──
        float soft[6]; float occFrac = 0.0f;
        for (int b = 0; b < 6; ++b) soft[b] = s.softAmp ? s.softAmp[b] : 1.0f;
        occFrac = clamp01(1.0f - soft[0]);   // 遮蔽割合は「低域の減衰量」（旧 computeSoftOcclusion と同じ量）
        applyTilt(soft, p.transmissionTilt, p.transmissionHighCutDb);
        if (std::fabs(p.transmissionGainDb) > 0.01f) {
            const float tg = std::pow(10.0f, p.transmissionGainDb / 20.0f);
            for (int b = 0; b < 6; ++b) soft[b] = std::min(soft[b] * tg, 1.0f);
        }
        {
            const float a = (p.tapSmoothTime > 1e-4f && sc.dt > 0.0f) ? 1.0f - std::exp(-sc.dt / p.tapSmoothTime) : 1.0f;
            if (!st.init) {
                st.init = true; st.occSm = occFrac;
                for (int b = 0; b < 6; ++b) st.transSm[b] = soft[b];
            } else {
                st.occSm += (occFrac - st.occSm) * a;
                for (int b = 0; b < 6; ++b) st.transSm[b] = smoothLog(st.transSm[b], soft[b], a);
            }
            occFrac = st.occSm;
            for (int b = 0; b < 6; ++b) soft[b] = st.transSm[b];
        }
        int n = 0;
        writeTap(p, sc, out, n++, soft, directDist, S, 0, 0.0f);

        // ── R タップ（面の線）: 全経路長 → 遅延、ゲイン × 空気吸収 × 1/r。F の枠を残す ──
        for (int t = 0; t < s.earlyCount && n < maxTaps - reserve; ++t) {
            const float pl = length(s.earlyPos[t] - L);
            const float rel = std::max(0.0f, (pl - directDist) * toMs);
            writeTap(p, sc, out, n++, s.earlyGain + t * 6, pl, s.earlyPos[t], 1, rel);
        }
        // ── F タップ（二次音源）: 配分 × 遮蔽割合 × 回折の音量。見通せていれば 0 本 ──
        for (int t = 0; t < s.diffCount && n < maxTaps; ++t) {
            if (occFrac <= 1e-4f) break;
            const float pl = length(s.diffPos[t] - L);
            const float rel = std::max(0.0f, (pl - directDist) * toMs);
            const float share = s.diffGain[t] * occFrac * diffGainLin;
            float g[6];
            for (int b = 0; b < 6; ++b) g[b] = p.diffractionDistanceOnly ? share : (s.dif ? s.dif[b] : 1.0f) * share;
            if (p.diffractionHighCutDb > 0.01f) applyTilt(g, 1.0f, p.diffractionHighCutDb);
            writeTap(p, sc, out, n++, g, pl, s.diffPos[t], 2, rel);
        }
        out.count = n;

        // ITDG
        float best = 1e30f;
        for (int i = 0; i < n; ++i) if (out.taps[i].type != 0 && out.taps[i].delayMs < best) best = out.taps[i].delayMs;
        out.itdgMs = (best >= 1e29f) ? 0.0f : best;

        // 生存の平均（反射込み）＝尾の送出量
        {
            float m = 0.0f;
            for (int b = 0; b < 6; ++b) m += s.bands ? s.bands[b] : 1.0f;
            out.sourceLevel = m / 6.0f;
        }

        // ── 直接音の向き: 真の向き ↔ 到来方向を遮蔽の深さで slerp → SmoothDamp ──
        {
            float wsum = 0.0f, gsum = 0.0f;
            for (int b = 0; b < 6; ++b) { gsum += kBandWeights[b] * (s.bands ? s.bands[b] : 1.0f); wsum += kBandWeights[b]; }
            const float occ = clamp01(1.0f - (wsum > 0.0f ? gsum / wsum : 0.0f));
            const float steer = (p.steerThreshold < 1.0f) ? clamp01((occ - p.steerThreshold) / (1.0f - p.steerThreshold)) : 0.0f;
            Vec3 trueDir = S - L;
            trueDir = (length(trueDir) > 1e-8f) ? normalized(trueDir) : sc.fwd;
            Vec3 engDir = s.arrivalDir ? Vec3(s.arrivalDir[0], s.arrivalDir[1], s.arrivalDir[2]) : trueDir;
            engDir = (length(engDir) > 1e-8f) ? normalized(engDir) : trueDir;
            const Vec3 target = slerp(trueDir, engDir, steer);
            if (length(st.apparent) < 1e-8f) { st.apparent = target; st.apparentVel = Vec3(0, 0, 0); }
            else {
                st.apparent = smoothDamp(st.apparent, target, st.apparentVel, std::max(0.01f, p.directionSmoothTime), sc.dt);
                if (length(st.apparent) > 1e-8f) st.apparent = normalized(st.apparent);
            }
            toLocal(sc, st.apparent, out.directDir);
        }

        // ── HRTF に載せる回折タップ（ヒステリシス）──
        out.hrtfTapIndex = -1;
        out.hrtfDir[0] = 0; out.hrtfDir[1] = 0; out.hrtfDir[2] = 1;
        if (p.diffractionHrtf) {
            int bestI = -1; float bestG = 0.0f;
            int keep = -1; float keepG = 0.0f;
            const float kSame = 1.5f * 1.5f;
            float nearest = kSame;
            for (int i = 0; i < n; ++i) {
                if (out.taps[i].type != 2) continue;
                float g = 0.0f; for (int b = 0; b < 6; ++b) g += out.taps[i].gain[b];
                g /= 6.0f;
                if (g <= 0.0f) continue;
                if (g > bestG) { bestG = g; bestI = i; }
                if (st.hrtfHasPrev) {
                    const Vec3 a(out.taps[i].arr[0], out.taps[i].arr[1], out.taps[i].arr[2]);
                    const float d2 = length2(a - st.hrtfPrev);
                    if (d2 < nearest) { nearest = d2; keep = i; keepG = g; }
                }
            }
            if (bestI < 0) { st.hrtfHasPrev = false; }
            else {
                int pick = bestI;
                if (keep >= 0 && keep != bestI && p.diffractionHrtfMarginDb > 0.0f) {
                    const float marginLin = std::pow(10.0f, p.diffractionHrtfMarginDb / 20.0f);
                    if (bestG < keepG * marginLin) pick = keep;
                }
                out.hrtfTapIndex = pick;
                st.hrtfPrev = Vec3(out.taps[pick].arr[0], out.taps[pick].arr[1], out.taps[pick].arr[2]);
                st.hrtfHasPrev = true;
                for (int k = 0; k < 3; ++k) out.hrtfDir[k] = out.taps[pick].dir[k];
                out.taps[pick].hrtfWeight = 1.0f;
            }
        } else {
            st.hrtfHasPrev = false;
        }

        // ── 尾の比（★音源ごと。#4）と mixing time ──
        {
            float vol, rt;
            if (sc.hasRoom) { vol = sc.roomVol; rt = std::max(0.05f, sc.roomRt60); }
            else            { vol = std::max(1.0f, sc.fallbackVol); rt = std::max(0.05f, p.fallbackRt60); }
            out.roomShare = sc.hasRoom ? clamp01(sc.roomShare) : 1.0f;
            if (!sc.hasRoom && sc.fallbackVol <= 0.0f) {
                out.tailRatio = 0.0f; out.tailRatioPhysical = 0.0f; out.mixingTimeMs = 25.0f;
            } else {
                const float r = std::max(0.1f, directDist);
                const float rc = 0.057f * std::sqrt(vol / rt);
                float t = (r * r) / std::max(rc * rc, 1e-4f);
                out.tailRatioPhysical = t;
                t = std::pow(t, p.reverbRatioExponent);
                const float share = (p.reverbShareFade && sc.hasRoom) ? out.roomShare : 1.0f;
                out.tailRatio = softCeiling(t, 50.0f, p.reverbRatioCeiling) * share;
                out.mixingTimeMs = std::min(std::max(std::sqrt(vol), 5.0f), 500.0f);
            }
        }
    }

    /// 消えた音源の状態を捨てる（id の集合を渡す）。
    void retain(const unsigned long long* ids, int n) {
        for (std::size_t i = 0; i < states_.size();) {
            bool keep = false;
            for (int k = 0; k < n; ++k) if (ids[k] == states_[i].id) { keep = true; break; }
            if (keep) ++i; else { states_[i] = states_.back(); states_.pop_back(); }
        }
    }

private:
    struct State {
        unsigned long long id = 0;
        bool init = false;
        float transSm[6] = {1, 1, 1, 1, 1, 1};
        float occSm = 0.0f;
        Vec3 apparent{0, 0, 0};
        Vec3 apparentVel{0, 0, 0};
        bool hrtfHasPrev = false;
        Vec3 hrtfPrev{0, 0, 0};
    };
    std::vector<State> states_;
    static constexpr float kAirAbsDbPerM[6] = {0.0003f, 0.0008f, 0.0017f, 0.003f, 0.0085f, 0.025f};
    static constexpr float kBandWeights[6] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};

    State& stateFor(unsigned long long id) {
        for (State& s : states_) if (s.id == id) return s;
        states_.push_back(State{});
        states_.back().id = id;
        return states_.back();
    }
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    static float length2(const Vec3& v) { return v.x * v.x + v.y * v.y + v.z * v.z; }

    // 透過のこもり: 125 Hz を基準に傾きの指数、＋帯域で対数等間隔の高域カット
    static void applyTilt(float* g, float tilt, float cutDb) {
        const bool doTilt = std::fabs(tilt - 1.0f) > 1e-3f, doCut = cutDb > 1e-3f;
        if (!doTilt && !doCut) return;
        const float lo = std::max(g[0], 1e-6f);
        for (int b = 0; b < 6; ++b) {
            float v = g[b];
            if (doTilt) { const float rel = clamp01(v / lo); v = lo * std::pow(rel, tilt); }
            if (doCut)  { const float f = static_cast<float>(b) / 5.0f; v *= std::pow(10.0f, -(cutDb * f) / 20.0f); }
            g[b] = v;
        }
    }
    // dB（対数）で補間。振幅で補間すると大きい側へ速く小さい側へ遅く動く
    static float smoothLog(float cur, float target, float a) {
        const float floorAmp = 1e-5f;
        const float c = std::log(std::max(cur, floorAmp)), t = std::log(std::max(target, floorAmp));
        const float v = std::exp(c + (t - c) * a);
        return (v <= floorAmp * 1.01f && target <= 0.0f) ? 0.0f : v;
    }
    static float softCeiling(float x, float knee, float ceiling) {
        if (x <= knee) return x;
        const float span = ceiling - knee;
        if (span <= 0.0f) return knee;
        return knee + span * (1.0f - std::exp(-(x - knee) / span));
    }
    static Vec3 slerp(const Vec3& a, const Vec3& b, float t) {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        const float d = std::max(-1.0f, std::min(1.0f, dot(a, b)));
        const float th = std::acos(d);
        if (th < 1e-4f) return normalized(a + (b - a) * t);
        const float s = std::sin(th);
        return a * (std::sin((1.0f - t) * th) / s) + b * (std::sin(t * th) / s);
    }
    // Unity の Vector3.SmoothDamp と同じ式（臨界減衰ばね、maxSpeed 無限）
    static Vec3 smoothDamp(const Vec3& current, const Vec3& target0, Vec3& velocity, float smoothTime, float dt) {
        smoothTime = std::max(0.0001f, smoothTime);
        const float omega = 2.0f / smoothTime;
        const float x = omega * dt;
        const float ex = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const Vec3 change = current - target0;
        const Vec3 target = current - change;
        const Vec3 temp = (velocity + change * omega) * dt;
        velocity = (velocity - temp * omega) * ex;
        Vec3 output = target + (change + temp) * ex;
        if (dot(target0 - current, output - target0) > 0.0f) {
            output = target0;
            velocity = (dt > 0.0f) ? (output - target0) * (1.0f / dt) : Vec3(0, 0, 0);
        }
        return output;
    }
    static void toLocal(const SceneIn& sc, const Vec3& w, float out[3]) {
        out[0] = dot(w, sc.right); out[1] = dot(w, sc.up); out[2] = dot(w, sc.fwd);
    }
    void writeTap(const TapParams& p, const SceneIn& sc, VoiceProgram& out, int n,
                  const float* g, float pathLen, const Vec3& arrival, int type, float delayMs) const {
        ProgramTap& t = out.taps[n];
        float air[6];
        for (int b = 0; b < 6; ++b) air[b] = std::pow(10.0f, -(kAirAbsDbPerM[b] * pathLen * p.airAbsorptionScale) / 20.0f);
        float refDist = p.distanceRef, power = 1.0f;
        if (type == 2) { if (p.diffractionDistanceRef > 0.0f) refDist = p.diffractionDistanceRef; power = p.diffractionDistancePower; }
        float da = 1.0f;
        if (refDist > 0.0f) { const float a = refDist / std::max(pathLen, refDist); da = (power == 1.0f) ? a : std::pow(a, power); }
        for (int b = 0; b < 6; ++b) t.gain[b] = g[b] * air[b] * da;
        if (type == 0) {
            float e = 0.0f;
            for (int b = 0; b < 6; ++b) e += (air[b] * da) * (air[b] * da);
            out.freeFieldDirect = std::sqrt(e / 6.0f);
        }
        // パン（到来点とリスナーの右。等パワー）
        Vec3 d = arrival - sc.listener;
        const float xr = (length2(d) > 1e-8f) ? dot(normalized(d), sc.right) : 0.0f;
        const float pt = (xr + 1.0f) * 0.5f;
        t.panL = std::sqrt(std::max(0.0f, 1.0f - pt)); t.panR = std::sqrt(std::max(0.0f, pt));
        t.type = type; t.delayMs = delayMs; t.hrtfWeight = 0.0f;
        t.arr[0] = arrival.x; t.arr[1] = arrival.y; t.arr[2] = arrival.z;
        if (length2(d) > 1e-8f) toLocal(sc, normalized(d), t.dir);
        else { t.dir[0] = 0; t.dir[1] = 0; t.dir[2] = 1; }
    }
};

}  // namespace acoustic
