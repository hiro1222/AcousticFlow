/* Adapter/wwise_adapter.cpp
 * Wwise 連携の「本物」の実装。
 * ★このプロジェクトで唯一 Wwise SDK を include するファイル★
 * CMake が Wwise SDK を見つけたときだけビルドされる。
 *
 * ここでは最小構成として、サウンドエンジンの起動と終了だけを行う。
 * （バンク読み込みや実際の発音はまだ。まず「起動できる」ことを確証する）
 */
#include "Adapter/wwise_adapter.h"

// --- Wwise SDK ---
#include <AK/SoundEngine/Common/AkMemoryMgrModule.h>  // メモリマネージャ + 既定設定
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>  // ストリームマネージャ
#include <AK/SoundEngine/Common/IAkStreamMgr.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>      // サウンドエンジン本体
#include <AK/MusicEngine/Common/AkMusicEngine.h>      // 音楽エンジン

// ソースプラグインの登録。これを include すると AK_STATIC_LINK_PLUGIN により
// Tone Generator がサウンドエンジンに登録される（対応する .lib のリンクも必要）。
// これが無いと、イベント再生は成功するのに音が生成されず無音になる。
#include <AK/Plugin/AkToneSourceFactory.h>            // Wwise Tone Generator
// Vorbis コーデックのデコーダ。バンクの音源を Vorbis 形式にした場合、これを include + .lib
// リンクしないと「バンク読込もイベント再生も成功するのにデコードできず無音」になる。
#include <AK/Plugin/AkVorbisDecoderFactory.h>         // Vorbis デコーダ
// Wwise RoomVerb（残響エフェクト）。これを include + .lib リンクしないと、
// RoomVerb を使うバンクの読込/再生が失敗して無音になる。
#include <AK/Plugin/AkRoomVerbFXFactory.h>            // Wwise RoomVerb
// Wwise Reflect（イメージソース早期反射）と Spatial Audio。これを include + .lib
// リンクしないと SetImageSource による方向つき反射(A)が使えない。
#include <AK/Plugin/AkReflectFXFactory.h>             // Wwise Reflect
#include <AK/SpatialAudio/Common/AkSpatialAudio.h>    // AK::SpatialAudio::Init / SetImageSource

// Wwise サンプルの Low-Level I/O（バンクをディスクから読むための実装）。
// これがないと LoadBank がファイルを開けない。
#include "AkFilePackageLowLevelIODeferred.h"

// 出力レベル可視化：マスターバスの RMS メータリングを取るのに使う。
#include <AK/SoundEngine/Common/AkCommonDefs.h>  // AK::IAkMetering / SpeakerVolumes / AkMeteringFlags
#include <AK/SoundEngine/Common/AkCallback.h>     // AkBusMeteringCallbackFunc
#include <atomic>

// UTF-8 -> ワイド文字変換（SetBasePath/LoadBank は AkOSChar=wchar_t を要求）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace acoustic {
namespace adapter {

namespace {
// ファイル I/O フックの実体（バンク読込の土台）。プロセスに1つ。
CAkFilePackageLowLevelIODeferred g_lowLevelIO;

// UTF-8 の C 文字列をワイド文字へ変換する小さなヘルパ。
bool toWide(const char* utf8, wchar_t* out, int outCount) {
    if (!utf8 || !out || outCount <= 0) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outCount);
    return n > 0;
}

// 出力(マスターバス)の左右レベル(RMS, 線形)。音声スレッドのメータリング
// コールバックが書き、ゲーム側が getOutputLevels で読む（atomic で安全に受け渡し）。
std::atomic<float> g_levelL{0.0f};
std::atomic<float> g_levelR{0.0f};

// マスターバスのメータリングコールバック（音声スレッドで毎フレーム呼ばれる）。
void BusMeteringCallback(AkBusMeteringCallbackInfo* in_pInfo) {
    if (!in_pInfo || !in_pInfo->pMetering) return;
    const AkReal32* rms = in_pInfo->pMetering->rms;  // AK_EnableBusMeter_RMS で有効・線形値
    if (!rms) return;
    const AkUInt32 n = in_pInfo->channelConfig.uNumChannels;
    const float l = (n > 0) ? rms[0] : 0.0f;
    const float r = (n > 1) ? rms[1] : l;
    g_levelL.store(l, std::memory_order_relaxed);
    g_levelR.store(r, std::memory_order_relaxed);
}
}  // namespace

bool isWwiseAvailable() {
    return true;  // 本物の実装
}

bool initAudio() {
    if (AK::SoundEngine::IsInitialized()) {
        return true;  // 二重初期化を防ぐ
    }

    // 1) メモリマネージャ。Wwise の全モジュールがこれに依存するので最初に起動。
    AkMemSettings memSettings;
    AK::MemoryMgr::GetDefaultSettings(memSettings);
    if (AK::MemoryMgr::Init(&memSettings) != AK_Success) {
        return false;
    }

    // 2) ストリームマネージャ。バンク等のファイル読み込みの土台。
    //    今は実ファイルを読まないので、ストリーミング「デバイス」は作らず
    //    マネージャ本体だけ生成する（最小構成）。
    AkStreamMgrSettings stmSettings;
    AK::StreamMgr::GetDefaultSettings(stmSettings);
    if (AK::StreamMgr::Create(stmSettings) == nullptr) {
        AK::MemoryMgr::Term();
        return false;
    }

    // 2.5) ストリーミングデバイス + Low-Level I/O。
    //      これでディスクからバンクを読めるようになる（最小初期化では省いていた）。
    AkDeviceSettings deviceSettings;
    AK::StreamMgr::GetDefaultDeviceSettings(deviceSettings);
    if (g_lowLevelIO.Init(deviceSettings) != AK_Success) {
        if (AK::IAkStreamMgr::Get()) AK::IAkStreamMgr::Get()->Destroy();
        AK::MemoryMgr::Term();
        return false;
    }

    // 3) サウンドエンジン本体。
    AkInitSettings initSettings;
    AkPlatformInitSettings platformInitSettings;
    AK::SoundEngine::GetDefaultInitSettings(initSettings);
    AK::SoundEngine::GetDefaultPlatformInitSettings(platformInitSettings);
    if (AK::SoundEngine::Init(&initSettings, &platformInitSettings) != AK_Success) {
        g_lowLevelIO.Term();
        if (AK::IAkStreamMgr::Get()) {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        AK::MemoryMgr::Term();
        return false;
    }

    // 4) 音楽エンジン（インタラクティブミュージック用。初期化しておくのが定石）。
    AkMusicSettings musicSettings;
    AK::MusicEngine::GetDefaultInitSettings(musicSettings);
    if (AK::MusicEngine::Init(&musicSettings) != AK_Success) {
        AK::SoundEngine::Term();
        g_lowLevelIO.Term();
        if (AK::IAkStreamMgr::Get()) {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        AK::MemoryMgr::Term();
        return false;
    }

    // 5) スペーシャルオーディオ（早期反射=AkReflect のイメージソースに必要）。
    //    失敗しても致命ではない（早期反射だけ無効になる）ので、init 全体は成功のまま続行。
    AkSpatialAudioInitSettings spatialSettings;  // 既定でよい
    AK::SpatialAudio::Init(spatialSettings);

    return true;
}

bool isAudioInitialized() {
    return AK::SoundEngine::IsInitialized();
}

void shutdownAudio() {
    if (!AK::SoundEngine::IsInitialized()) {
        return;
    }
    // 初期化と逆順で終了する。
    AK::MusicEngine::Term();
    AK::SoundEngine::Term();
    g_lowLevelIO.Term();
    if (AK::IAkStreamMgr::Get()) {
        AK::IAkStreamMgr::Get()->Destroy();
    }
    AK::MemoryMgr::Term();
}

// ===== 再生系 =====

bool setBankPath(const char* utf8Path) {
    wchar_t wpath[1024];
    if (!toWide(utf8Path, wpath, 1024)) return false;
    return g_lowLevelIO.SetBasePath(wpath) == AK_Success;
}

bool loadBank(const char* bankName) {
    wchar_t wname[260];
    if (!toWide(bankName, wname, 260)) return false;
    AkBankID bankID = 0;
    const bool ok = AK::SoundEngine::LoadBank(wname, bankID) == AK_Success;
    if (ok) {
        // バンク（特に Init）読込後＝マスターバスが存在する状態でメータリング登録。
        // initAudio 時点ではバスが無くてバインドされないので、ここで一度だけ登録する。
        static bool s_meteringRegistered = false;
        if (!s_meteringRegistered) {
            AK::SoundEngine::RegisterBusMeteringCallback(
                AK::SoundEngine::GetIDFromString("Master Audio Bus"),
                BusMeteringCallback, AK_EnableBusMeter_RMS);
            s_meteringRegistered = true;
        }
    }
    return ok;
}

void registerGameObject(unsigned long long id, const char* name) {
    AK::SoundEngine::RegisterGameObj(static_cast<AkGameObjectID>(id), name ? name : "");
}

void unregisterGameObject(unsigned long long id) {
    AK::SoundEngine::UnregisterGameObj(static_cast<AkGameObjectID>(id));
}

void setGameObjectPosition(unsigned long long id,
                           float px, float py, float pz,
                           float fx, float fy, float fz,
                           float tx, float ty, float tz) {
    AkSoundPosition pos;
    pos.Set(px, py, pz, fx, fy, fz, tx, ty, tz);
    AK::SoundEngine::SetPosition(static_cast<AkGameObjectID>(id), pos);
}

void setDefaultListener(unsigned long long id) {
    const AkGameObjectID listener = static_cast<AkGameObjectID>(id);
    AK::SoundEngine::SetDefaultListeners(&listener, 1);
    // 早期反射(AkReflect)は Spatial Audio のリスナー基準で像源を空間化する。
    // 既定リスナー1つなら自動選択されるが、明示登録しておくと確実。
    AK::SpatialAudio::RegisterListener(listener);
}

unsigned int postEvent(const char* eventName, unsigned long long gameObjectId) {
    if (!eventName) return 0;
    return static_cast<unsigned int>(
        AK::SoundEngine::PostEvent(eventName, static_cast<AkGameObjectID>(gameObjectId)));
}

// イベントに対する Stop/Pause/Resume 等を実行する。
// actionType は AkActionOnEventType（Stop=0 / Pause=1 / Resume=2）。
// Pause/Resume は音源ボイスを止める/再開するが、リバーブ等のバス側の尾は
// 鳴り続ける＝「音を止めて残響だけ残す」デバッグ/演出に使える。
void executeActionOnEvent(const char* eventName, int actionType,
                          unsigned long long gameObjectId) {
    if (!eventName) return;
    AK::SoundEngine::ExecuteActionOnEvent(
        eventName,
        static_cast<AK::SoundEngine::AkActionOnEventType>(actionType),
        static_cast<AkGameObjectID>(gameObjectId));
}

void setObstructionOcclusion(unsigned long long emitterId,
                             unsigned long long listenerId,
                             float obstruction, float occlusion) {
    AK::SoundEngine::SetObjectObstructionAndOcclusion(
        static_cast<AkGameObjectID>(emitterId),
        static_cast<AkGameObjectID>(listenerId),
        obstruction, occlusion);
}

void setEmitterListenerVolume(unsigned long long emitterId,
                              unsigned long long listenerId,
                              float volume) {
    // 音源(emitter)→リスナー間の出力バス音量を線形ゲインで設定する。
    // RTPC を新設せずに「向きによる音量差」を当てられる軽い経路。
    AK::SoundEngine::SetGameObjectOutputBusVolume(
        static_cast<AkGameObjectID>(emitterId),
        static_cast<AkGameObjectID>(listenerId),
        volume);
}

void setState(const char* stateGroup, const char* state) {
    if (!stateGroup || !state) return;
    // 文字列版 SetState（名前を内部でハッシュ）。PostEvent と同じく非最適化ビルドで使える。
    AK::SoundEngine::SetState(stateGroup, state);
}

void getOutputLevels(float* outLeft, float* outRight) {
    if (outLeft) *outLeft = g_levelL.load(std::memory_order_relaxed);
    if (outRight) *outRight = g_levelR.load(std::memory_order_relaxed);
}

void setRTPCValue(const char* name, float value) {
    if (!name) return;
    // 文字列版・ゲームオブジェクト省略＝グローバル RTPC。SetState と同じく非最適化で使える。
    AK::SoundEngine::SetRTPCValue(name, static_cast<AkRtpcValue>(value));
}

void setRTPCValueOnObject(const char* name, float value, unsigned long long gameObjectId) {
    if (!name) return;
    // ゲームオブジェクトIDを指定＝その音源のボイスにだけ効く RTPC。
    // 帯域別EQの Gain を音源ごとに独立駆動するのに使う。
    AK::SoundEngine::SetRTPCValue(name, static_cast<AkRtpcValue>(value),
                                  static_cast<AkGameObjectID>(gameObjectId));
}

void setEarlyReflections(unsigned long long emitterId, const char* auxBusName,
                         const float* positions, const float* levels, int count) {
    const AkGameObjectID emitter = static_cast<AkGameObjectID>(emitterId);
    // auxBusName が指定されていればそのバス、空/未指定なら authoring 既定の reflections バス。
    AkUniqueID auxBusID = AK_INVALID_AUX_ID;
    if (auxBusName && auxBusName[0] != '\0') {
        auxBusID = AK::SoundEngine::GetIDFromString(auxBusName);
    }
    // 先に既存の像源を全消し（毎フレーム作り直す前提。emitter×auxBus 単位で消す）。
    AK::SpatialAudio::ClearImageSources(auxBusID, emitter);
    if (!positions || !levels || count <= 0) return;

    for (int i = 0; i < count; ++i) {
        const AkVector64 pos{ positions[i * 3 + 0],
                              positions[i * 3 + 1],
                              positions[i * 3 + 2] };
        // 距離スケール 1.0（Core が像源位置に距離減衰を織り込み済み）＋タップの線形レベル。
        AkImageSourceSettings info(pos, 1.0f, levels[i]);
        // 像源ID はタップ番号。フレーム間で同じ番号に上書き更新される。
        AK::SpatialAudio::SetImageSource(static_cast<AkImageSourceID>(i), info, "ER",
                                         auxBusID, emitter);
    }
}

void renderAudio() {
    AK::SoundEngine::RenderAudio();
}

}  // namespace adapter
}  // namespace acoustic
