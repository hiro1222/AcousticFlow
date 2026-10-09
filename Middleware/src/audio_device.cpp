#include "audio_device.h"

#include "block_source.h"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstdio>
#include <cstring>

namespace af {
namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT。ksmedia.h を引き込むと巻き添えが多いので、
// 値をここに置く（この GUID は仕様として固定されている）。
const GUID kSubFormatFloat =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// PKEY_Device_FriendlyName。
//
// ⚠ 素直に `initguid.h` ＋ `functiondiscoverykeys_devpkey.h` を include すると、
//   Windows SDK 10.0.26100 では `DEFINE_PROPERTYKEY` が二重に展開されて
//   100 個以上のエラーになる（実際になった）。値は仕様として固定なので、
//   ヘッダを引かずにここに置く。**依存も 1 本減る。**
const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string hrText(HRESULT hr) {
    char b[64];
    ::sprintf_s(b, "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

// COM の生ポインタを取りこぼさないための最小の入れ物。
// （このためだけに WRL/ATL を持ち込みたくない）
template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    void** putVoid() { return reinterpret_cast<void**>(&p); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

double perfFreq() {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

double nowMs(double freq) {
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

}  // namespace

AudioDevice::~AudioDevice() { stop(); }

int AudioDevice::copyScope(float* dst, int cap) const {
    const unsigned s0 = scopeSeq_.load(std::memory_order_acquire);
    if (s0 & 1u) return 0;                                  // 書いている最中
    int n = scopeCount_.load(std::memory_order_relaxed);
    if (n > cap) n = cap;
    if (n > 0) std::memcpy(dst, scope_, sizeof(float) * static_cast<size_t>(n));
    const unsigned s1 = scopeSeq_.load(std::memory_order_acquire);
    return (s0 == s1) ? n : 0;                              // 途中で書き換わったら捨てる
}

void AudioDevice::resetStats() {
    callbacks_.store(0, std::memory_order_relaxed);
    late_.store(0, std::memory_order_relaxed);
    worstMs_.store(0.0, std::memory_order_relaxed);
    lastMs_.store(0.0, std::memory_order_relaxed);
    worstLoad_.store(0.0, std::memory_order_relaxed);
}

bool AudioDevice::probeDefaultFormat(int* sampleRate, int* channels, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    // 呼び出し元のスレッドで一時的に COM を借りる。既に初期化済みなら
    // S_FALSE が返るので、その場合は解放しない（他人の初期化を壊さないため）。
    const HRESULT ci = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owned = (ci == S_OK);
    bool ok = false;
    std::string why;

    {
        Com<IMMDeviceEnumerator> enumr;
        Com<IMMDevice> dev;
        Com<IAudioClient> client;
        WAVEFORMATEX* mix = nullptr;

        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), enumr.putVoid());
        if (SUCCEEDED(hr)) hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, dev.put());
        if (SUCCEEDED(hr))
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);

        if (SUCCEEDED(hr) && mix) {
            if (sampleRate) *sampleRate = static_cast<int>(mix->nSamplesPerSec);
            if (channels)   *channels   = static_cast<int>(mix->nChannels);
            ok = true;
        } else {
            why = "既定の出力の形式を取れません: " + hrText(hr);
        }
        if (mix) ::CoTaskMemFree(mix);
    }

    if (owned) ::CoUninitialize();
    if (!ok) return fail(why);
    if (err) err->clear();
    return true;
}

bool AudioDevice::start(BlockSource* source, std::string* err) {
    if (running_.load()) { if (err) err->clear(); return true; }
    if (!source) { if (err) *err = "音源が指定されていません"; return false; }

    source_ = source;
    quit_.store(false);
    startOk_.store(false);
    startError_.clear();

    // スレッド側の初期化が済むまで待つ。素性（周波数・チャンネル数）を
    // 呼び出し側へ返してから戻りたいため。
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    startedEvent_ = done;

    thread_ = std::thread([this] { threadMain(); });

    ::WaitForSingleObject(done, 5000);

    const bool ok = startOk_.load();
    if (!ok) {
        quit_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    startedEvent_ = nullptr;
    ::CloseHandle(done);

    if (!ok) {
        if (err) *err = startError_.empty() ? "デバイスを開けませんでした" : startError_;
        return false;
    }

    running_.store(true);
    if (err) err->clear();
    return true;
}

void AudioDevice::stop() {
    if (!thread_.joinable()) { running_.store(false); return; }
    quit_.store(true);
    thread_.join();
    running_.store(false);
}

void AudioDevice::threadMain() {
    const double freq = perfFreq();

    // 起動の成否をちょうど 1 回だけ呼び出し元へ返す。
    bool signalled = false;
    auto signalStart = [&](bool ok, const std::string& why) {
        if (signalled) return;
        signalled = true;
        if (!ok) startError_ = why;
        startOk_.store(ok);
        if (startedEvent_) ::SetEvent(static_cast<HANDLE>(startedEvent_));
    };

    // ★COM の初期化はこのスレッドだけで完結させる。
    //   ホスト側（コンソール）に COM の都合を持ち込まないため。
    const HRESULT ci = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(ci);

    // 早期 return で抜けたいので中身をラムダに入れる（goto は初期化を跨げない）。
    auto run = [&]() {
        Com<IMMDeviceEnumerator> enumr;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), enumr.putVoid());
        if (FAILED(hr)) return signalStart(false, "デバイス列挙を作れません: " + hrText(hr));

        Com<IMMDevice> dev;
        hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, dev.put());
        if (FAILED(hr)) return signalStart(false, "既定の出力デバイスがありません: " + hrText(hr));

        // 名前を取る（失敗しても致命ではない）。
        {
            Com<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                PROPVARIANT v;
                ::PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR)
                    deviceName_ = wideToUtf8(v.pwszVal);
                ::PropVariantClear(&v);
            }
        }

        Com<IAudioClient> client;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
        if (FAILED(hr)) return signalStart(false, "デバイスを開けません: " + hrText(hr));

        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) return signalStart(false, "混合形式を取れません: " + hrText(hr));

        // ★共有モードは「OS の混合形式」でしか開けない。変換は書かない。
        //   float32 以外だったら、黙って変な音を出すより開かないほうを採る
        //   （この作品では、静かに劣化するくらいなら止まるほうを選ぶ）。
        bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
            isFloat = (::memcmp(&ext->SubFormat, &kSubFormatFloat, sizeof(GUID)) == 0);
        }
        if (!isFloat || mix->wBitsPerSample != 32) {
            char b[192];
            ::sprintf_s(b, "混合形式が 32bit float ではありません（%u bit）。"
                           "変換は書いていないので開きません。", mix->wBitsPerSample);
            ::CoTaskMemFree(mix);
            return signalStart(false, b);
        }

        sampleRate_ = static_cast<int>(mix->nSamplesPerSec);
        channels_   = static_cast<int>(mix->nChannels);

        // 期間 0 ＝ デバイスの既定周期に任せる。試聴用なので詰めない。
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
        ::CoTaskMemFree(mix);
        if (FAILED(hr)) return signalStart(false, "初期化に失敗: " + hrText(hr));

        HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(ev);
        if (FAILED(hr)) {
            ::CloseHandle(ev);
            return signalStart(false, "通知を付けられません: " + hrText(hr));
        }

        UINT32 bufFrames = 0;
        client->GetBufferSize(&bufFrames);
        bufferFrames_ = static_cast<int>(bufFrames);

        Com<IAudioRenderClient> render;
        hr = client->GetService(__uuidof(IAudioRenderClient), render.putVoid());
        if (FAILED(hr)) {
            ::CloseHandle(ev);
            return signalStart(false, "描画口を取れません: " + hrText(hr));
        }

        // ★最初の 1 周ぶんを無音で埋めてから Start する。
        //   埋めずに始めると、デバイスが確保したままの領域（前に使ったアプリの残骸）が
        //   そのまま出ます。1 度きりですが、耳には確実に聞こえます。
        {
            BYTE* p = nullptr;
            if (SUCCEEDED(render->GetBuffer(bufFrames, &p)) && p) {
                std::memset(p, 0, static_cast<size_t>(bufFrames) * channels_ * sizeof(float));
                render->ReleaseBuffer(bufFrames, 0);
            }
        }

        // Pro Audio として登録する。無くても動くが、あるとスケジューラが優先する。
        DWORD taskIndex = 0;
        HANDLE mm = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        hr = client->Start();
        if (FAILED(hr)) {
            if (mm) ::AvRevertMmThreadCharacteristics(mm);
            ::CloseHandle(ev);
            return signalStart(false, "再生を開始できません: " + hrText(hr));
        }

        signalStart(true, {});

        const double budgetMs = 1000.0 * bufferFrames_ / sampleRate_;

        while (!quit_.load(std::memory_order_relaxed)) {
            // 2 周ぶん待って来なければデバイス側の異常。数えて回り続ける。
            const DWORD w = ::WaitForSingleObject(ev, static_cast<DWORD>(budgetMs * 2 + 100));
            if (w != WAIT_OBJECT_0) { late_.fetch_add(1, std::memory_order_relaxed); continue; }

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;

            const UINT32 avail = bufFrames - padding;
            if (avail == 0) continue;

            BYTE* p = nullptr;
            if (FAILED(render->GetBuffer(avail, &p)) || !p) break;

            const double t0 = nowMs(freq);
            source_->render(reinterpret_cast<float*>(p), static_cast<int>(avail), channels_);
            const double dt = nowMs(freq) - t0;

            // ★画面用に波形と山を公開する（ReleaseBuffer より前 ── 解放後の領域は読めない）。
            //   奇数 → 書く → 偶数。読み手は前後が一致したときだけ採用する。
            {
                const float* f = reinterpret_cast<const float*>(p);
                const int n = static_cast<int>(avail) < kScopeMax ? static_cast<int>(avail)
                                                                  : kScopeMax;
                scopeSeq_.fetch_add(1, std::memory_order_release);
                float pk = 0.0f;
                for (int i = 0; i < n; ++i) {
                    const float v = f[i * channels_];
                    scope_[i] = v;
                    const float a = (v < 0.0f) ? -v : v;
                    if (a > pk) pk = a;
                }
                scopeCount_.store(n, std::memory_order_relaxed);
                scopeSeq_.fetch_add(1, std::memory_order_release);
                peak_.store(pk, std::memory_order_relaxed);
            }

            render->ReleaseBuffer(avail, 0);

            lastMs_.store(dt, std::memory_order_relaxed);
            if (dt > worstMs_.load(std::memory_order_relaxed))
                worstMs_.store(dt, std::memory_order_relaxed);
            callbacks_.fetch_add(1, std::memory_order_relaxed);
            lastFrames_.store(static_cast<int>(avail), std::memory_order_relaxed);

            // ★持ち時間は「そのブロックの長さ」で決まる。輪の大きさではない。
            //   共有モードは輪 970 に対して 1 回あたり 441 前後しか渡さないので、
            //   輪で割ると 2 倍以上甘い数字が出る。
            const double thisBudgetMs = 1000.0 * avail / sampleRate_;
            const double loadPct = (thisBudgetMs > 0.0) ? 100.0 * dt / thisBudgetMs : 0.0;
            if (loadPct > worstLoad_.load(std::memory_order_relaxed))
                worstLoad_.store(loadPct, std::memory_order_relaxed);

            if (dt > thisBudgetMs)
                late_.fetch_add(1, std::memory_order_relaxed);
        }

        client->Stop();
        if (mm) ::AvRevertMmThreadCharacteristics(mm);
        ::CloseHandle(ev);
    };

    run();

    // どの経路で抜けても、待っている呼び出し元は必ず起こす。
    signalStart(false, startError_);

    if (comOwned) ::CoUninitialize();
}

}  // namespace af
