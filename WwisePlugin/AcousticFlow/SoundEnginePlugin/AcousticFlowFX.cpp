/* AcousticFlowFX.cpp ── 仕組みの説明は AcousticFlowFX.h の頭書き。ここは関数ごとの「何を・どうやって」。 */
#include "AcousticFlowFX.h"
#include "../AcousticFlowConfig.h"

#include <AK/AkWwiseSDKVersion.h>
#include <AK/SoundEngine/Common/AkAudioObject.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

AK::IAkPlugin* CreateAcousticFlowFX(AK::IAkPluginMemAlloc* in_pAllocator)
{
    return AK_PLUGIN_NEW(in_pAllocator, AcousticFlowFX());
}

AK::IAkPluginParam* CreateAcousticFlowFXParams(AK::IAkPluginMemAlloc* in_pAllocator)
{
    return AK_PLUGIN_NEW(in_pAllocator, AcousticFlowFXParams());
}

AK_IMPLEMENT_PLUGIN_FACTORY(AcousticFlowFX, AkPluginTypeEffect, AcousticFlowConfig::CompanyID, AcousticFlowConfig::PluginID)

namespace {
// 位置の一致とみなす距離（m）。ゲームのスレッドと音のスレッドの 1 フレームのずれ（歩いて 2〜3 cm）より十分広く、
// 隣の音源（普通は 1 m 以上離れている）より狭く。
const float kMatchTol = 0.5f;
// エンジンが見つからないとき、何ブロックおきに探し直すか（Unity が DLL を読む前にこのプラグインが立つことがある）。
const int kRebindEvery = 50;
int s_rebindCountdown = 0;
// 1 ブロックに回すのは実体 1 つ。最後に回したブロック番号と、音を受けた実体が最後に回したブロック番号。
std::atomic<AkUInt32> s_renderedTick{0xFFFFFFFFu};
std::atomic<AkUInt32> s_voicedTick{0};
std::atomic<bool>     s_voicedEver{false};
std::atomic<int>      s_instances{0};   // 生きている実体の数（計器）
}

AcousticFlowFX::AcousticFlowFX()
    : m_pParams(nullptr)
    , m_pAllocator(nullptr)
    , m_pContext(nullptr)
    , m_mono(nullptr)
    , m_tmpL(nullptr)
    , m_tmpR(nullptr)
    , m_voices(nullptr)
    , m_voiceCount(0)
    , m_shared{nullptr, nullptr, 0}
    , m_space(1)
    , m_sampleRate(48000)
    , m_bindCount(0)
    , m_blockCount(0)
    , m_idleBlocks(0)
{
}

AcousticFlowFX::~AcousticFlowFX()
{
}

/// エンジンの関数を AcousticEngine.dll から名前で引く。
///   ★GetModuleHandle だけを使い、LoadLibrary はしない。Unity が既に読んでいる DLL と同じ物を使わないと、
///     台帳（グローバル）が別物になって何も引けない。無ければ素通しで鳴らし、Execute で時々探し直す。
void AcousticFlowFX::BindEngine()
{
    HMODULE h = ::GetModuleHandleW(L"AcousticEngine.dll");
    if (!h) { m_engine = Engine{}; return; }
    m_engine.snapshot    = reinterpret_cast<decltype(m_engine.snapshot)>(::GetProcAddress(h, "AF_HostSnapshot"));
    m_engine.report      = reinterpret_cast<decltype(m_engine.report)>(::GetProcAddress(h, "AF_HostReport"));
    m_engine.voiceRender = reinterpret_cast<decltype(m_engine.voiceRender)>(::GetProcAddress(h, "AF_VoiceRender"));
    m_engine.fdnRender   = reinterpret_cast<decltype(m_engine.fdnRender)>(::GetProcAddress(h, "AF_FdnMixRender"));
    m_engine.busRender   = reinterpret_cast<decltype(m_engine.busRender)>(::GetProcAddress(h, "AF_DirectionBusRender"));
    m_engine.unitsPerMeter = reinterpret_cast<decltype(m_engine.unitsPerMeter)>(::GetProcAddress(h, "AF_HostUnitsPerMeter"));
    m_engine.reportLevels = reinterpret_cast<decltype(m_engine.reportLevels)>(::GetProcAddress(h, "AF_HostReportLevels"));
    m_engine.meterWrite   = reinterpret_cast<decltype(m_engine.meterWrite)>(::GetProcAddress(h, "AF_HostMeterWrite"));
    if (!m_engine.ok()) m_engine = Engine{};
}

/// 出力をステレオの普通の形に決め、作業用の配列を取る。
///   ★io_rFormat.channelConfig に普通の形（オブジェクトでない）を入れると、出力のオブジェクトは Wwise が Init の時点で
///     1 本作る。Execute で作り直さないので、out_objects をそのまま使ってよい（SDK の注記）。
AKRESULT AcousticFlowFX::Init(AK::IAkPluginMemAlloc* in_pAllocator, AK::IAkEffectPluginContext* in_pContext, AK::IAkPluginParam* in_pParams, AkAudioFormat& io_rFormat)
{
    m_pParams = (AcousticFlowFXParams*)in_pParams;
    m_pAllocator = in_pAllocator;
    m_pContext = in_pContext;
    m_sampleRate = io_rFormat.uSampleRate;
    io_rFormat.channelConfig.SetStandard(AK_SPEAKER_SETUP_STEREO);

    m_mono = (float*)AK_PLUGIN_ALLOC(in_pAllocator, sizeof(float) * kMaxFrames);
    m_tmpL = (float*)AK_PLUGIN_ALLOC(in_pAllocator, sizeof(float) * kMaxFrames);
    m_tmpR = (float*)AK_PLUGIN_ALLOC(in_pAllocator, sizeof(float) * kMaxFrames);
    m_voices = (AF_HostVoice*)AK_PLUGIN_ALLOC(in_pAllocator, sizeof(AF_HostVoice) * AF_HOST_MAX_VOICES);
    if (!m_mono || !m_tmpL || !m_tmpR || !m_voices) return AK_InsufficientMemory;
    m_voiceCount = 0;
    BindEngine();
    s_instances.fetch_add(1, std::memory_order_relaxed);
    return AK_Success;
}

AKRESULT AcousticFlowFX::Term(AK::IAkPluginMemAlloc* in_pAllocator)
{
    if (m_mono) s_instances.fetch_sub(1, std::memory_order_relaxed);   // Init が通った実体だけ数えている
    if (m_mono) AK_PLUGIN_FREE(in_pAllocator, m_mono);
    if (m_tmpL) AK_PLUGIN_FREE(in_pAllocator, m_tmpL);
    if (m_tmpR) AK_PLUGIN_FREE(in_pAllocator, m_tmpR);
    if (m_voices) AK_PLUGIN_FREE(in_pAllocator, m_voices);
    AK_PLUGIN_DELETE(in_pAllocator, this);
    return AK_Success;
}

AKRESULT AcousticFlowFX::Reset()
{
    m_voiceCount = 0;
    m_idleBlocks = 0;
    return AK_Success;
}

/// 場外型（受けた物と出す物が別）で、オブジェクトを受けられる、と Wwise に伝える。
AKRESULT AcousticFlowFX::GetPluginInfo(AkPluginInfo& out_rPluginInfo)
{
    out_rPluginInfo.eType = AkPluginTypeEffect;
    out_rPluginInfo.bIsInPlace = false;
    out_rPluginInfo.bCanProcessObjects = true;
    out_rPluginInfo.uBuildVersion = AK_WWISESDK_VERSION_COMBINED;
    return AK_Success;
}

/// このブロックを回してよいか（頭書きの「係」）。1 ブロックに回すのは実体 1 つ、音を受けている実体が優先。
///   ・音を受けている実体: 来たことをまず記録し（回せたかどうかに関係なく）、そのブロックで最初なら回す
///     （ブロック番号の取り合いは compare_exchange で 1 つだけ勝つ）。
///   ・音を受けていない実体: 音を受けた実体が直近 2 ブロック以内に**来て**いれば譲る。誰も音を受けていなければ回す
///     （音源が止まっても部屋の響きの尾を進めるため。止めると遅延線が止まり、次に鳴ったとき尾が飛ぶ）。
///   ★「最初に来た実体を固定の係にする」形（09-30 の 1 回目）は無音になった。音源の聞き手をカメラに絞る前に
///     ビューポートの聞き手の実体が係になり、絞った後は係に音が 1 本も届かず、音を受けた実体は係でないので黙っていた。
///   ★「音を受けた実体が直近に**回した**か」で譲る形（10-01〜10-03）は、2 回目の Play で無音になった。1 回目の Play の
///     音の無い実体が残っていて毎ブロック先に呼ばれ、記録が古いので回す権利を取り続け、音を受けた新しい実体は回せないので
///     記録も新しくならない（ずっと黙る）。回せたかでなく「来たか」で記録すると、次のブロックから必ず譲る（失うのは 1 ブロック）。
///   ★ブロックは順に処理される（ブロック t の全部のバスが済んでから t+1）ので、ブロックをまたいで 2 つが重なることは無い。
bool AcousticFlowFX::ClaimBlock(bool hasObjects)
{
    AK::IAkGlobalPluginContext* g = m_pContext ? m_pContext->GlobalContext() : nullptr;
    if (!g) return true;   // ブロック番号が取れない（起きないはず）なら見張らない
    const AkUInt32 tick = g->GetBufferTick();
    if (hasObjects) { s_voicedTick.store(tick, std::memory_order_release); s_voicedEver.store(true, std::memory_order_release); }
    else if (s_voicedEver.load(std::memory_order_acquire)
             && (AkUInt32)(tick - s_voicedTick.load(std::memory_order_acquire)) <= 2u)
        return false;
    AkUInt32 prev = s_renderedTick.load(std::memory_order_acquire);
    if (prev == tick || !s_renderedTick.compare_exchange_strong(prev, tick, std::memory_order_acq_rel)) return false;
    return true;
}

/// 台帳の写しの中から、鍵で Voice を引く（無ければ -1）。
int AcousticFlowFX::VoiceIndexOfKey(int voiceKey) const
{
    for (int i = 0; i < m_voiceCount; ++i) if (m_voices[i].key == voiceKey) return i;
    return -1;
}

/// 位置からいちばん近い Voice を引く（台帳の写しの中から総当たり。音源は多くて数十本なので線形で足りる）。
int AcousticFlowFX::FindVoice(float x, float y, float z, int space, float& outDist) const
{
    int best = -1;
    float bestD2 = 1e30f;
    for (int i = 0; i < m_voiceCount; ++i)
    {
        const AF_HostVoice& v = m_voices[i];
        const float dx = (space == 0 ? v.x : v.lx) - x;
        const float dy = (space == 0 ? v.y : v.ly) - y;
        const float dz = (space == 0 ? v.z : v.lz) - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    outDist = (best >= 0) ? std::sqrt(bestD2) : 1e30f;
    return best;
}

/// 1 ブロックぶん。受けた音を 1 本ずつ自前の Voice で両耳の音にして足し、共有の器を 1 回回して足す。
namespace {
// 左右の計器: 1 ブロックの左右の RMS とピークを段 s に入れる（AF_HostMeterWrite へ渡す形）。
void MeasureStage(AF_HostMeterBlock& m, int s, const float* l, const float* r, AkUInt32 frames)
{
    double el = 0.0, er = 0.0; float pl = 0.0f, pr = 0.0f;
    for (AkUInt32 k = 0; k < frames; ++k)
    {
        el += (double)l[k] * l[k]; er += (double)r[k] * r[k];
        const float al = std::fabs(l[k]), ar = std::fabs(r[k]);
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;
    }
    const double nf = (double)(frames > 0 ? frames : 1);
    m.rmsL[s] = (float)std::sqrt(el / nf); m.rmsR[s] = (float)std::sqrt(er / nf);
    m.peakL[s] = pl; m.peakR[s] = pr;
}
}  // namespace

void AcousticFlowFX::Execute(const AkAudioObjects& in_objects, const AkAudioObjects& out_objects)
{
    if (out_objects.uNumObjects == 0) return;
    AkAudioBuffer* out = out_objects.ppObjectBuffers[0];
    AkUInt32 frames = out->MaxFrames();
    if (frames > (AkUInt32)kMaxFrames) frames = kMaxFrames;
    float* outL = (float*)out->GetChannel(0);
    float* outR = (out->NumChannels() > 1) ? (float*)out->GetChannel(1) : outL;
    std::memset(outL, 0, sizeof(float) * frames);
    if (outR != outL) std::memset(outR, 0, sizeof(float) * frames);

    // 音が 5 秒届いていない実体は「もう鳴らない」と返して、Wwise に片付けてもらう（2026-10-03）。
    //   ★ずっと「鳴っている」と返していた頃は、Play を止めても前の Play の実体が残り続け、2 回目の Play で溜まっていった
    //     （実体 4 つ）。残った実体は回す権利を取り合う相手にもなる（ClaimBlock の注記）。5 秒あれば部屋の響きの尾も消えている。
    if (in_objects.uNumObjects > 0) m_idleBlocks = 0;
    else if (m_idleBlocks < 0x7FFFFFFF) ++m_idleBlocks;
    if ((double)m_idleBlocks * (double)frames > 5.0 * (double)m_sampleRate)
    {
        out->uValidFrames = (AkUInt16)frames;
        out->eState = AK_NoMoreData;
        return;
    }

    // 係でなければ無音で返す（同じ器を 1 ブロックに 2 回回さない・2 つのスレッドで同時に触らない）
    if (!ClaimBlock(in_objects.uNumObjects > 0))
    {
        out->uValidFrames = (AkUInt16)frames;
        out->eState = AK_DataReady;
        return;
    }

    // エンジンが見つかっていなければ、時々探し直す（Unity が DLL を読む前に立ったとき）
    if (!m_engine.ok() && --s_rebindCountdown <= 0) { BindEngine(); s_rebindCountdown = kRebindEvery; }

    // 台帳の写し。取れなかったブロック（書いている最中）は前の写しをそのまま使う ── 待たない
    if (m_engine.ok())
    {
        AF_HostShared sh;
        const int n = m_engine.snapshot(m_voices, AF_HOST_MAX_VOICES, &sh);
        if (n >= 0) { m_voiceCount = n; m_shared = sh; }
    }

    // 「同じ音源とみなす距離」を、ゲームの座標の単位で（Unity は m、Unreal は cm）。
    const float upm = (m_engine.unitsPerMeter ? m_engine.unitsPerMeter() : 1.0f);
    const float tol = kMatchTol * (upm > 1e-6f ? upm : 1.0f);
    int matched = 0, missed = 0;
    double errSum = 0.0;
    double inSq = 0.0;          // 計器: 受けた音（音量を掛ける前）の二乗和
    float firstGain = -1.0f;    // 計器: 最初の音源の Wwise の音量
    for (AkUInt32 i = 0; i < in_objects.uNumObjects; ++i)
    {
        AkAudioBuffer* buf = in_objects.ppObjectBuffers[i];
        const AkAudioObject* obj = in_objects.ppObjects[i];
        const AkUInt32 valid = (buf->uValidFrames < frames) ? buf->uValidFrames : frames;
        const AkUInt32 nch = buf->NumChannels();
        if (valid == 0 || nch == 0) continue;

        // 1) モノラルに畳む。Wwise の音量（cumulativeGain: 前のブロックの値 → このブロックの値）をなめらかに掛ける。
        //    ★これを掛けないと、Wwise でデザイナが触った音量（RTPC を含む）が効かない。
        const float g0 = obj->cumulativeGain.fPrev, g1 = obj->cumulativeGain.fNext;
        if (firstGain < 0.0f) firstGain = g1;
        const float inv = 1.0f / (float)nch;
        for (AkUInt32 k = 0; k < valid; ++k)
        {
            float s = 0.0f;
            for (AkUInt32 c = 0; c < nch; ++c) s += ((const float*)buf->GetChannel(c))[k];
            inSq += (double)(s * inv) * (s * inv);
            const float g = g0 + (g1 - g0) * ((float)k / (float)frames);
            m_mono[k] = s * inv * g;
        }
        for (AkUInt32 k = valid; k < frames; ++k) m_mono[k] = 0.0f;

        // 2) Voice を引く。まず覚えている結び付き（この音の通し番号 → Voice の鍵）。無ければ位置で引いて覚える。
        //    ★位置で引くのは最初の 1 回だけ。位置は聞き手から見た座標で、頭を回す・歩くとゲームと Wwise で数フレームずれ、
        //      毎ブロック引き直すと許容 0.5 m を超えたブロックだけ素通しになって「飛ぶ」（頭書きの退けた書き方）。
        int vi = -1;
        float dist = 1e30f;
        const AkVector& p = obj->positioning.threeD.xform.Position();
        if (m_engine.ok() && m_voiceCount > 0)
        {
            int bi = -1;
            for (int b = 0; b < m_bindCount; ++b) if (m_binds[b].obj == obj->key) { bi = b; break; }
            if (bi >= 0)
            {
                vi = VoiceIndexOfKey(m_binds[bi].voiceKey);
                if (vi >= 0)
                {
                    m_binds[bi].lastBlock = m_blockCount;
                    const AF_HostVoice& v = m_voices[vi];   // 距離は表示用（ゲームと Wwise のずれの大きさ）
                    const float dx = (m_space == 0 ? v.x : v.lx) - p.X, dy = (m_space == 0 ? v.y : v.ly) - p.Y, dz = (m_space == 0 ? v.z : v.lz) - p.Z;
                    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                else m_binds[bi] = m_binds[--m_bindCount];   // Voice が台帳から消えた ── 結び付きを捨てて位置で引き直す
            }
            if (vi < 0)
            {
                // 今の座標系で遠ければもう片方も試し、近い方に乗り換える（Unity と Unreal で届く座標系が違うため）
                vi = FindVoice(p.X, p.Y, p.Z, m_space, dist);
                if (dist > tol)
                {
                    float d2 = 1e30f;
                    const int vj = FindVoice(p.X, p.Y, p.Z, 1 - m_space, d2);
                    if (d2 < dist) { vi = vj; dist = d2; if (d2 <= tol) m_space = 1 - m_space; }
                }
                if (dist > tol) vi = -1;
                if (vi >= 0 && m_bindCount < kMaxBinds) m_binds[m_bindCount++] = Bind{ obj->key, m_voices[vi].key, m_blockCount };
            }
        }

        // 3) 鳴らす。見つかれば自前の Voice（直接音・反射・回折・透過・尾への送り）、無ければ素通し（左右へ半分ずつ）。
        if (vi >= 0)
        {
            m_engine.voiceRender(m_voices[vi].voice, m_mono, (int)frames, m_tmpL, m_tmpR, nullptr);
            for (AkUInt32 k = 0; k < frames; ++k) { outL[k] += m_tmpL[k]; outR[k] += m_tmpR[k]; }
            ++matched;
            errSum += dist / (upm > 1e-6f ? upm : 1.0f);   // 表示は m で
        }
        else
        {
            for (AkUInt32 k = 0; k < frames; ++k) { const float s = m_mono[k] * 0.7071f; outL[k] += s; outR[k] += s; }
            ++missed;
        }
    }

    // 左右の計器（AF_HostMeterWrite）: 段 1 ＝ ここまでの和（Voice の直接・反射・回折・透過と素通し）
    AF_HostMeterBlock meter{};
    meter.frames = (int)frames; meter.sampleRate = (int)m_sampleRate;
    const bool metering = (m_engine.meterWrite != nullptr);
    if (metering) MeasureStage(meter, 1, outL, outR, frames);

    // 4) 共有の器を 1 回だけ回す。FDN → 方向バスの順（FDN は方向バスのレーンへ送るので先）。
    //    ★音源が鳴っていないブロックでも回す（遅延線を進めないと、次の送りで尾が飛ぶ）。
    if (m_engine.ok())
    {
        if (m_shared.fdn)
        {
            std::memset(m_tmpL, 0, sizeof(float) * frames); std::memset(m_tmpR, 0, sizeof(float) * frames);
            m_engine.fdnRender(m_shared.fdn, (int)frames, m_tmpL, m_tmpR);
            if (metering) MeasureStage(meter, 2, m_tmpL, m_tmpR, frames);   // 段 2 ＝ 戸口の響き（FDN の直出し）
            for (AkUInt32 k = 0; k < frames; ++k) { outL[k] += m_tmpL[k]; outR[k] += m_tmpR[k]; }
        }
        if (m_shared.bus)
        {
            std::memset(m_tmpL, 0, sizeof(float) * frames); std::memset(m_tmpR, 0, sizeof(float) * frames);
            m_engine.busRender(m_shared.bus, (int)frames, m_tmpL, m_tmpR);
            if (metering) MeasureStage(meter, 3, m_tmpL, m_tmpR, frames);   // 段 3 ＝ 方向バス（反射・響きのレーン）
            for (AkUInt32 k = 0; k < frames; ++k) { outL[k] += m_tmpL[k]; outR[k] += m_tmpR[k]; }
        }
        if (metering) { MeasureStage(meter, 0, outL, outR, frames); m_engine.meterWrite(&meter); }   // 段 0 ＝ 全体
        m_engine.report(matched, missed, m_space, matched > 0 ? (float)(errSum / matched) : 0.0f);
        if (m_engine.reportLevels)
        {
            double outSq = 0.0;
            for (AkUInt32 k = 0; k < frames; ++k) outSq += 0.5 * ((double)outL[k] * outL[k] + (double)outR[k] * outR[k]);
            const double nf = (double)(frames > 0 ? frames : 1);
            m_engine.reportLevels(s_instances.load(std::memory_order_relaxed), (float)std::sqrt(inSq / nf),
                                  firstGain < 0.0f ? 0.0f : firstGain, (float)std::sqrt(outSq / nf));
        }
    }

    // 結び付きの掃除: 2 秒（約 190 ブロック）届かなかった音の分は捨てる（音が止まった・別の音に替わった）
    ++m_blockCount;
    for (int b = m_bindCount - 1; b >= 0; --b)
        if (m_blockCount - m_binds[b].lastBlock > 190) m_binds[b] = m_binds[--m_bindCount];

    // 5) 出力は毎ブロック「鳴っている」。音源が止まっても部屋の響きは尾を引くので、ここで止めない。
    out->uValidFrames = (AkUInt16)frames;
    out->eState = AK_DataReady;
}
