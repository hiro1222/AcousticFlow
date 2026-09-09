// voice_api.cpp — acoustic_voice.h の実装。C API と Dsp/VoiceRenderer の橋渡し。
//
// ここでやることは3つだけ:
//   ・ハンドル（void*）と C++ オブジェクトの相互変換
//   ・null / 範囲外の防御（呼び出し側の言語が何であっても落とさない）
//   ・AF_VoiceTap ⇄ EarlyReflectConv::Tap の詰め替え
// 音の処理そのものは Dsp/ 側にあり、ここには一切書かない。
#include "acoustic_voice.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "Dsp/hrtf_set.h"
#include "Dsp/voice_renderer.h"

namespace {

af::dsp::HrtfSet* asHrtf(AF_HrtfHandle h) { return static_cast<af::dsp::HrtfSet*>(h); }
af::dsp::VoiceRenderer* asVoice(AF_VoiceHandle h) {
    return static_cast<af::dsp::VoiceRenderer*>(h);
}
af::dsp::DirectionBus* asDirBus(AF_DirectionBusHandle h) {
    return static_cast<af::dsp::DirectionBus*>(h);
}

// タップ詰め替え用の一時領域。制御スレッドからしか触らないので静的で足りる。
//   毎回 vector を作らないのは、setTaps が毎フレーム呼ばれうるため。
std::vector<af::dsp::EarlyReflectConv::Tap>& tapScratch() {
    static std::vector<af::dsp::EarlyReflectConv::Tap> s;
    return s;
}

float orDefault(float v, float def) { return (v > 0.0f) ? v : def; }
int orDefault(int v, int def) { return (v > 0) ? v : def; }

}  // namespace

// ================================================================ ABI

int AF_AbiVersion(void) { return AF_ABI_VERSION; }

// ================================================================ HRTF

AF_HrtfHandle AF_HrtfLoadFile(const char* path) {
    if (!path) return nullptr;
    auto* set = new (std::nothrow) af::dsp::HrtfSet();
    if (!set) return nullptr;
    if (!af::dsp::HrtfSet::loadFromFile(path, *set)) { delete set; return nullptr; }
    return set;
}

AF_HrtfHandle AF_HrtfCreateSynthetic(int sampleRate) {
    auto* set = new (std::nothrow) af::dsp::HrtfSet(
        af::dsp::HrtfSet::createSynthetic(orDefault(sampleRate, 48000)));
    return set;
}

void AF_HrtfDestroy(AF_HrtfHandle hrtf) { delete asHrtf(hrtf); }

int AF_HrtfDirectionCount(AF_HrtfHandle hrtf) {
    af::dsp::HrtfSet* s = asHrtf(hrtf);
    return s ? s->directionCount() : 0;
}

void AF_HrtfGetName(AF_HrtfHandle hrtf, char* outName, int maxLen) {
    if (!outName || maxLen <= 0) return;
    af::dsp::HrtfSet* s = asHrtf(hrtf);
    const char* n = s ? s->name().c_str() : "(none)";
    const int len = std::min(maxLen - 1, static_cast<int>(std::strlen(n)));
    std::memcpy(outName, n, static_cast<std::size_t>(len));
    outName[len] = '\0';
}

// ================================================================ 音源

AF_VoiceHandle AF_VoiceCreate(const AF_VoiceConfig* cfg) {
    af::dsp::VoiceRenderer::Config c;
    if (cfg) {
        c.sampleRate = orDefault(cfg->sampleRate, 48000);
        c.maxFrames = orDefault(cfg->maxFrames, 4096);
        c.tailSeconds = orDefault(cfg->tailSeconds, 1.0f);
        c.tapCrossfadeMs = orDefault(cfg->tapCrossfadeMs, 30.0f);
        c.hrtfCrossfadeMs = orDefault(cfg->hrtfCrossfadeMs, 12.0f);
        c.hrtfCrossoverHz = orDefault(cfg->hrtfCrossoverHz, 700.0f);
        c.tailFirstBlock = orDefault(cfg->tailFirstBlock, 64);
        c.tailCapBlock = orDefault(cfg->tailCapBlock, 8192);
    }
    return new (std::nothrow) af::dsp::VoiceRenderer(c);
}

void AF_VoiceDestroy(AF_VoiceHandle voice) { delete asVoice(voice); }

// ── 尾の共有バス（段階②）──
namespace {
af::dsp::TailBus* asBus(AF_TailBusHandle h) { return static_cast<af::dsp::TailBus*>(h); }
}  // namespace

AF_TailBusHandle AF_TailBusCreate(int sampleRate, float tailSeconds,
                                  int firstBlock, int capBlock, int maxFrames) {
    const int sr = orDefault(sampleRate, 48000);
    const float sec = orDefault(tailSeconds, 1.0f);
    const int tailSamples = static_cast<int>(sec * static_cast<float>(sr));
    return new (std::nothrow) af::dsp::TailBus(tailSamples,
                                               orDefault(firstBlock, 64),
                                               orDefault(capBlock, 8192),
                                               orDefault(maxFrames, 4096), sr);
}

void AF_TailBusDestroy(AF_TailBusHandle bus) { delete asBus(bus); }

void AF_TailBusSetCrossfadeMs(AF_TailBusHandle bus, float ms) {
    if (af::dsp::TailBus* b = asBus(bus)) b->setCrossfadeMs(ms);
}

void AF_TailBusRender(AF_TailBusHandle bus, int frames, float* outL, float* outR) {
    if (af::dsp::TailBus* b = asBus(bus)) b->render(frames, outL, outR);
}

float AF_TailBusRms(AF_TailBusHandle bus) {
    af::dsp::TailBus* b = asBus(bus);
    return b ? b->rms() : 0.0f;
}

int AF_TailBusHasIr(AF_TailBusHandle bus) {
    af::dsp::TailBus* b = asBus(bus);
    return (b && b->hasIr()) ? 1 : 0;
}

int AF_TailBusLastMono(AF_TailBusHandle bus, float* out, int frames) {
    af::dsp::TailBus* b = asBus(bus);
    return b ? b->lastMono(out, frames) : 0;
}

void AF_VoiceSetTailBus(AF_VoiceHandle voice, AF_TailBusHandle bus, int isOwner) {
    if (af::dsp::VoiceRenderer* v = asVoice(voice))
        v->setTailBus(asBus(bus), isOwner != 0);
}

// ── 尾の FDN（docs/TAIL_FDN_PLAN.md 手順 4、2026-09-09）──
namespace {
af::dsp::FdnRoomMix* asFdn(AF_FdnMixHandle h) { return static_cast<af::dsp::FdnRoomMix*>(h); }
}  // namespace

AF_FdnMixHandle AF_FdnMixCreate(int sampleRate, int maxFrames, float diffusion) {
    return new (std::nothrow) af::dsp::FdnRoomMix(orDefault(sampleRate, 48000), orDefault(maxFrames, 4096),
                                                  orDefault(diffusion, 0.6f));
}

void AF_FdnMixDestroy(AF_FdnMixHandle mix) { delete asFdn(mix); }

int AF_FdnMixAddRoom(AF_FdnMixHandle mix, float lineScale, const float* rt60_6, int colour) {
    af::dsp::FdnRoomMix* m = asFdn(mix);
    return m ? m->addRoom((lineScale > 0.0f) ? lineScale : 1.0f, rt60_6, colour != 0) : -1;
}

int AF_FdnMixRoomCount(AF_FdnMixHandle mix) {
    af::dsp::FdnRoomMix* m = asFdn(mix);
    return m ? m->roomCount() : 0;
}

void AF_FdnMixSetRoomRt60(AF_FdnMixHandle mix, int room, const float* rt60_6, int colour) {
    if (af::dsp::FdnRoomMix* m = asFdn(mix)) m->setRoomRt60(room, rt60_6, colour != 0);
}

void AF_FdnMixSetListenerWeight(AF_FdnMixHandle mix, int room, const float* w6) {
    if (af::dsp::FdnRoomMix* m = asFdn(mix)) m->setListenerWeight(room, w6);
}

void AF_FdnMixRender(AF_FdnMixHandle mix, int frames, float* outL, float* outR) {
    if (af::dsp::FdnRoomMix* m = asFdn(mix)) m->render(frames, outL, outR);
}

float AF_FdnMixRms(AF_FdnMixHandle mix) {
    af::dsp::FdnRoomMix* m = asFdn(mix);
    return m ? m->rms() : 0.0f;
}

void AF_VoiceSetFdnMix(AF_VoiceHandle voice, AF_FdnMixHandle mix) {
    if (af::dsp::VoiceRenderer* v = asVoice(voice)) v->setFdnMix(asFdn(mix));
}

void AF_VoiceSetFdnSends(AF_VoiceHandle voice, const int* rooms, const float* gains, int n) {
    if (af::dsp::VoiceRenderer* v = asVoice(voice)) v->setFdnSends(rooms, gains, n);
}

void AF_VoiceSetTailAmount(AF_VoiceHandle voice, float directGain, float targetRatio) {
    if (af::dsp::VoiceRenderer* v = asVoice(voice)) v->setTailAmount(directGain, targetRatio);
}

void AF_VoiceSetFdnOnsetMs(AF_VoiceHandle voice, float ms) {
    if (af::dsp::VoiceRenderer* v = asVoice(voice)) v->setFdnOnsetMs(ms);
}

void AF_FdnMixSetDirectionBus(AF_FdnMixHandle mix, AF_DirectionBusHandle bus, float headCircumferenceCm) {
    if (af::dsp::FdnRoomMix* m = asFdn(mix))
        m->setDirectionBus(static_cast<af::dsp::DirectionBus*>(bus), (headCircumferenceCm > 0.0f) ? headCircumferenceCm : 57.0f);
}

void AF_FdnMixSetListenerDirection(AF_FdnMixHandle mix, int room, float dx, float dy, float dz, float spread) {
    if (af::dsp::FdnRoomMix* m = asFdn(mix)) { const float d[3] = { dx, dy, dz }; m->setListenerDirection(room, d, spread); }
}

AF_DirectionBusHandle AF_DirectionBusCreate(int sampleRate, int lanes, int maxFrames) {
    return new (std::nothrow) af::dsp::DirectionBus(orDefault(sampleRate, 48000), orDefault(lanes, 8),
                                                    orDefault(maxFrames, 1024));
}

AF_DirectionBusHandle AF_DirectionBusCreateEx(int sampleRate, int lanes, int maxFrames, int firstBlock, int capBlock) {
    return new (std::nothrow) af::dsp::DirectionBus(orDefault(sampleRate, 48000), orDefault(lanes, 8),
                                                    orDefault(maxFrames, 1024), orDefault(firstBlock, 128), orDefault(capBlock, 1024));
}

void AF_DirectionBusDestroy(AF_DirectionBusHandle bus) { delete asDirBus(bus); }

void AF_DirectionBusSetHrtf(AF_DirectionBusHandle bus, AF_HrtfHandle hrtf, float headCircumferenceCm) {
    if (af::dsp::DirectionBus* b = asDirBus(bus)) b->setHrtfSet(asHrtf(hrtf), orDefault(headCircumferenceCm, 57.0f));
}

int AF_DirectionBusHasHrtf(AF_DirectionBusHandle bus) {
    af::dsp::DirectionBus* b = asDirBus(bus);
    return (b && b->hasHrtf()) ? 1 : 0;
}

int AF_DirectionBusLanes(AF_DirectionBusHandle bus) {
    af::dsp::DirectionBus* b = asDirBus(bus);
    return b ? b->lanes() : 0;
}

void AF_DirectionBusRender(AF_DirectionBusHandle bus, int frames, float* outL, float* outR) {
    if (af::dsp::DirectionBus* b = asDirBus(bus)) b->render(frames, outL, outR);
}

float AF_DirectionBusRms(AF_DirectionBusHandle bus) {
    af::dsp::DirectionBus* b = asDirBus(bus);
    return b ? b->rms() : 0.0f;
}

void AF_VoiceSetDirectionBus(AF_VoiceHandle voice, AF_DirectionBusHandle bus) {
    if (af::dsp::VoiceRenderer* v = asVoice(voice)) v->setDirectionBus(asDirBus(bus));
}

void AF_VoiceSetTaps(AF_VoiceHandle voice, const AF_VoiceTap* taps, int count) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (!v) return;
    if (!taps || count <= 0) { v->setTaps(nullptr, 0); return; }

    auto& s = tapScratch();
    s.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        af::dsp::EarlyReflectConv::Tap& d = s[static_cast<std::size_t>(i)];
        const AF_VoiceTap& src = taps[i];
        d.delaySamples = std::max(0, src.delaySamples);
        for (int b = 0; b < 6; ++b) d.g[b] = src.gain6[b];
        d.panL = src.panL;
        d.panR = src.panR;
        // 未設定（両方 0）なら鏡面 100% として扱う。呼び出し側が散乱を使わない場合の既定。
        if (src.gSpec == 0.0f && src.gDiff == 0.0f) { d.gSpec = 1.0f; d.gDiff = 0.0f; }
        else { d.gSpec = src.gSpec; d.gDiff = src.gDiff; }
        d.hrtfWeight = (src.hrtfWeight < 0.0f) ? 0.0f
                     : (src.hrtfWeight > 1.0f) ? 1.0f : src.hrtfWeight;
        // 到来方向。全て 0 なら方向なし（従来どおりパンで鳴る）。
        d.dir[0] = src.dirX; d.dir[1] = src.dirY; d.dir[2] = src.dirZ;
        d.dirValid = (src.dirX * src.dirX + src.dirY * src.dirY + src.dirZ * src.dirZ) > 1e-8f;
    }
    v->setTaps(s.data(), count);
}

void AF_VoiceScatterSplit(float delayMs, float mixingTimeMs, float scatterAmount,
                          float timeGrowth, float* outSpec, float* outDiff) {
    float sp = 1.0f, df = 0.0f;
    af::dsp::EarlyReflectConv::scatterSplit(delayMs, mixingTimeMs, scatterAmount,
                                            timeGrowth, sp, df);
    if (outSpec) *outSpec = sp;
    if (outDiff) *outDiff = df;
}

void AF_VoiceSetHrtf(AF_VoiceHandle voice, AF_HrtfHandle hrtf) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setHrtfSet(asHrtf(hrtf));
}

void AF_VoiceSetHrtfEnabled(AF_VoiceHandle voice, int enabled) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setHrtfEnabled(enabled != 0);
}

void AF_VoiceSetDirection(AF_VoiceHandle voice, AF_Vector3 dir, float headCircumferenceCm) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (!v) return;
    const float d[3] = {dir.x, dir.y, dir.z};
    v->setDirection(d, headCircumferenceCm);
}

void AF_VoiceSetEarCues(AF_VoiceHandle voice, int enabled) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setEarCuesEnabled(enabled != 0);
}

void AF_VoiceSetDiffractionDirection(AF_VoiceHandle voice, AF_Vector3 dir,
                                     float headCircumferenceCm) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (!v) return;
    const float d[3] = {dir.x, dir.y, dir.z};
    v->setDiffractionDirection(d, headCircumferenceCm);
}

float AF_VoiceRebuildTail(AF_VoiceHandle voice,
                          const float* echoBands, int binCount, float binMs,
                          float startMs, float fadeMs,
                          float smoothMs, float smoothGrowth, float envAlpha,
                          float directGain, float targetRatio,
                          const float* earBandGain, int earBandGainLen) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (!v) return 0.0f;
    return v->rebuildTail(echoBands, binCount, binMs, startMs, fadeMs,
                          smoothMs, smoothGrowth, envAlpha, directGain, targetRatio,
                          earBandGain, earBandGainLen);
}

void AF_VoiceSetOutputGain(AF_VoiceHandle voice, float gain) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setOutputGain(gain);
}

void AF_VoiceSetTailLevel(AF_VoiceHandle voice, float level) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setTailLevel(level);
}

void AF_VoiceSetTailCrossfadeMs(AF_VoiceHandle voice, float ms) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setTailCrossfadeMs(ms);
}

void AF_VoiceSetScatterDiffusion(AF_VoiceHandle voice, float g) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setScatterDiffusion(g);
}

void AF_VoiceSetTailEnvelope(AF_VoiceHandle voice, float wet, float srcLevel) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (v) v->setTailEnvelope(wet, srcLevel);
}

int AF_VoiceTailPartitions(AF_VoiceHandle voice) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    return v ? v->tailPartitions() : 0;
}

int AF_VoiceTailLatency(AF_VoiceHandle voice) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    return v ? v->tailLatency() : 0;
}

void AF_VoiceRender(AF_VoiceHandle voice, const float* input, int frames,
                    float* outL, float* outR, AF_VoiceMetering* outMetering) {
    af::dsp::VoiceRenderer* v = asVoice(voice);
    if (!v) return;
    af::dsp::VoiceRenderer::Metering m;
    v->render(input, frames, outL, outR, &m);
    if (outMetering) {
        outMetering->rmsDirect = m.rmsDirect;
        outMetering->rmsEarly = m.rmsEarly;
        outMetering->rmsScatter = m.rmsScatter;
        outMetering->rmsTail = m.rmsTail;
        outMetering->rmsOut = m.rmsOut;
    }
}
