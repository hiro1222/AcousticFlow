/* Flow/mix_to_voice.h ── 配分から DSP への橋（段 4）
 *
 * ■ 役割
 *   Mix（エネルギー、秒、リスナー座標）を VoiceRenderer の言葉（振幅、サンプル、タップ、送り）に写す。
 *   ★√ を取るのはここ 1 か所。Flow の中はエネルギー、DSP は振幅（mix.h の規約）。
 *   DSP 側（Dsp/voice_renderer.h）は変更しない。既存の入口 setTaps / setFdnSends / setFdnOnsetMs / setDirection に流す。
 *
 * ■ 中の仕組み
 *   1) 振幅の尺度: g = √(E · 4π · d_ref²)、d_ref = 1 m。直接音は E = 1/(4πd²) なので g = 1/d、1 m で 1.0。
 *      五成分と FDN の送りに同じ K = 4π d_ref² を掛けるので、成分どうしの比はエネルギーのまま保たれる。
 *      絶対値はホストの outputGain（音量つまみ）。「実測は参照、絶対値は耳」。
 *   2) タップの並び: index 0 は必ず直接音（VoiceRenderer の規約: フル HRTF は index 0 だけ）。
 *      遅延は**直接音からの相対**（直接は 0）。到達の絶対時刻をそのまま遅延にすると 3.5 m で 10 ms の待ちが
 *      音の出だしに乗る。ゲームでは直接音を遅らせない。
 *   3) 方向: dirLocal が単位なら dirValid。広がり spread は gSpec = √(1−s)、gDiff = √s に写す
 *      （EarlyReflectConv の散乱の規約と同じ。拡散ぶんは方向を失う）。方向なし（初期の仮タップ）は等パワーのパン。
 *   4) FDN の送り: 部屋ごとに 1 本、振幅 a = √(mean_b E_b · K)。帯域の形は世界が部屋ごとの listenerWeight に載せる
 *      （world.h。送りはモノラルという DSP の制約）。setTailAmount(1, 1) で DSP 側の尾の倍率を素通しにする
 *      ── 旧実装では tailGain = 直接 × √比 で量を作っていたが、いまは量は配分が持つ。
 *   5) 尾の開始: onsetSec − directSec（直接音からの相対、ms）。
 *
 * ■ 繋がり
 *   受ける: Mix、部屋番号 → FDN の部屋番号の対応、サンプルレート、頭囲。
 *   渡す:   VoiceRenderer（残す側）。
 *
 * ■ 退けた書き方
 *   ・VoiceRenderer に setMix を足す: DSP が Flow の型を知ることになり、段 10 で消す側と残す側が絡む。
 *     橋を Flow に置けば DSP は無傷。
 *   ・遅延を絶対時刻にする: 上記。
 *   ・送りに帯域を持たせるために DSP を改造: 段 4 は「一周を鳴らす」。帯域の形は listenerWeight で等価に出せる
 *     （N 音源でも Σ が線形なので厳密。world.h の注記）。
 *
 * ■ 壊れる所
 *   ・√ を Flow の中でも取ると二重（壁越しが二乗ぶん小さい）。ここだけ。
 *   ・直接音のタップが index 0 に無いと HRTF が反射に掛かり、直接音が方向を失う。並べ替えてから渡す。
 *   ・setTailAmount(1, 1) を忘れると tailGain_ が 0 のままで FDN が鳴らない（無音で気づきにくい）。
 */
#ifndef ACOUSTICFLOW_FLOW_MIX_TO_VOICE_H
#define ACOUSTICFLOW_FLOW_MIX_TO_VOICE_H

#include <algorithm>
#include <cmath>
#include "Dsp/early_reflect_conv.h"
#include "Dsp/voice_renderer.h"
#include "Flow/mix.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

constexpr float kRefDistM = 1.0f;
constexpr float kEnergyToAmp = 4.0f * 3.14159265f * kRefDistM * kRefDistM;   // K

inline float ampFromEnergy(float e) { return std::sqrt(std::max(0.0f, e) * kEnergyToAmp); }

/// Mix を VoiceRenderer に写す。fdnRoomOfProbe[probe] = FDN 側の部屋番号（無ければ −1）。
inline void applyMixToVoice(const Mix& m, af::dsp::VoiceRenderer& v, int sampleRate, float headCm,
                            const int* fdnRoomOfProbe, int probeCount) {
    // ── タップ: 直接を index 0 に ──
    af::dsp::EarlyReflectConv::Tap taps[Mix::kMaxTaps];
    int n = 0;
    float directSec = 0.0f;
    float dirDirect[3] = {0.0f, 0.0f, 1.0f};
    for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind == TapKind::Direct) { directSec = m.taps[i].delaySec; break; }
    auto put = [&](const MixTap& t) {
        if (n >= Mix::kMaxTaps) return;
        af::dsp::EarlyReflectConv::Tap& d = taps[n++];
        d = af::dsp::EarlyReflectConv::Tap{};
        d.id = t.id;                        // 素性をそのまま渡す（DSP はこれでフレームをまたいで繋ぐ）
        d.delaySamples = std::max(0, static_cast<int>(std::lround((t.delaySec - directSec) * sampleRate)));
        for (int b = 0; b < kNumBands; ++b) d.g[b] = ampFromEnergy(t.e6[b]);
        const float len = length(t.dirLocal);
        if (len > 0.5f) {
            d.dir[0] = t.dirLocal.x / len; d.dir[1] = t.dirLocal.y / len; d.dir[2] = t.dirLocal.z / len;
            d.dirValid = true;
        }
        const float s = std::min(1.0f, std::max(0.0f, t.spread));
        d.gSpec = std::sqrt(1.0f - s); d.gDiff = std::sqrt(s);
        d.panL = 0.70710678f; d.panR = 0.70710678f;
        d.width = std::max(0.0f, t.width);          // 面音源の幅（方向バスのレーンへ幅で配るのは VoiceRenderer::setTaps）
        d.hrtfWeight = 0.0f;
    };
    for (int i = 0; i < m.tapCount; ++i)
        if (m.taps[i].kind == TapKind::Direct) { put(m.taps[i]); dirDirect[0] = taps[0].dir[0]; dirDirect[1] = taps[0].dir[1]; dirDirect[2] = taps[0].dir[2]; break; }
    if (n == 0) { af::dsp::EarlyReflectConv::Tap zero{}; taps[n++] = zero; }   // 直接が無くても index 0 は空けておく
    for (int i = 0; i < m.tapCount; ++i) if (m.taps[i].kind != TapKind::Direct) put(m.taps[i]);
    v.setDirection(dirDirect, headCm);
    v.setTaps(taps, n);

    // ── FDN の送り ──
    int rooms[af::dsp::VoiceRenderer::kMaxFdnSends]; float gains[af::dsp::VoiceRenderer::kMaxFdnSends]; int ns = 0;
    for (int i = 0; i < m.sendCount && ns < af::dsp::VoiceRenderer::kMaxFdnSends; ++i) {
        const FdnSend& s = m.sends[i];
        if (s.room < 0 || s.room >= probeCount || !fdnRoomOfProbe || fdnRoomOfProbe[s.room] < 0) continue;
        float mean = 0.0f; for (int b = 0; b < kNumBands; ++b) mean += s.e6[b]; mean /= kNumBands;
        rooms[ns] = fdnRoomOfProbe[s.room]; gains[ns] = ampFromEnergy(mean); ++ns;
    }
    v.setTailAmount(1.0f, 1.0f);                       // 尾の倍率は素通し（量は配分が持つ）
    v.setFdnSends(rooms, gains, ns);
    // ★尾の開始は**サンプルに丸めて**渡す。VoiceRenderer の遅延線は小数遅延を線形補間で読むので、
    //   小数部 f があるとインパルスが 2 サンプルに割れて (1−f)² + f² に減る（f=0.5 で −3 dB。Nyquist で |1−2f| の高域損失）。
    //   段 4 の橋の検査で FDN だけ −1.74 dB 足りず、器を直接測って −0.09 dB だったので送りの経路と分かった。
    //   1 サンプル（21 µs）の精度は尾の開始には要らない。DSP 側を良い補間（allpass / Lagrange）にするのは段 10 の候補。
    const float onsetSamples = std::round(std::max(0.0f, m.onsetSec - directSec) * static_cast<float>(sampleRate));
    v.setFdnOnsetMs(onsetSamples * 1000.0f / static_cast<float>(sampleRate));
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_MIX_TO_VOICE_H
