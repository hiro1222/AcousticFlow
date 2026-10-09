/* af_capture.cpp ── ゲームの窓と、パソコンから出ている音を撮って MP4 にする録画の道具（Windows の Media Foundation と WASAPI だけで動く）。
 *
 * ■ 全体の中の位置
 *   tools/record_unreal_demo.ps1 が、これと Unreal（-game -AFDemoWalk -AFCapture）を同時に立ち上げる。
 *     Unreal: 司令塔が遊びの始まりに Saved/Wwise/AFCapture_info.txt を書く（撮り始めの合図）。
 *             キャラクターが自動で歩き、終わったらゲームを閉じる。
 *     ここ:   合図が出たら窓と音を撮り始め、窓が閉じたら撮り終えて MP4 を書く。
 *   → 出来た MP4 をスマホへ送る（発注者「動画撮って送ってほしい」2026-10-04）。
 *
 * ■ 中の仕組み
 *   画面: 窓を最前面にし、画面からその窓の中身（クライアント領域）を BitBlt で 30 fps 取る。時刻は QueryPerformanceCounter。
 *         H.264 に Media Foundation の SinkWriter で詰める（動きながら詰めるので、生の絵は持たない）。
 *   音:   2 通り。どちらも 48 kHz・2 ch・16 bit にして AAC で詰める。
 *         --wav <WAV> … AfReplayWav が録画の記録から作った音（時刻 0 ＝ 合図の qpc）。撮り終えたあと、この WAV が出来るのを待って入れる。
 *                       出力の機器の状態に左右されない（既定。tools/record_unreal_demo.ps1 はこちら）。
 *         指定なし   … 有効な出力の機器を全部 WASAPI のループバックで録り、一番大きく鳴った機器を採る（実際に出ている音）。
 *                       パケットごとに頭の QPC 時刻が付くので、映像と同じ時計の上に並べる。
 *
 * ■ 退けた書き方
 *   ・ffmpeg で撮る: このパソコンに入っていない（入れるにはダウンロードが要る）。Media Foundation は Windows に最初からある。
 *   ・Wwise の録音（StartOutputCapture）: Unreal の -game では 32〜64 KB（0.2 秒）しか書かれなかった。止めてから 2.5 秒待っても同じ
 *     （2026-10-04 実測）。原因は出力の機器（Yamaha AG03）の時計が止まっていたこと（Wwise: Hardware audio subsystem stopped responding）。
 *     機器が止まるとループバックにも何も来ないので、既定は記録から作り直した音（--wav）にした。
 *   ・Unreal の一定刻み（-benchmark -fps）で絵を書き出す: 音は実時間で回るので、絵と音の時刻がずれる。
 *
 * ■ 壊れる所
 *   ・窓の上に別の最前面の窓（通知など）が重なると、それも写る（画面から取っているため）。画面がロックされていると黒くなる。
 *   ・パソコンで同時に鳴った音（通知・別のアプリ）も入る。出力の機器を途中で替えると、その後の音は入らない。
 *   ・窓の大きさは撮り始めの値で固定。途中で大きさが変わると、はみ出た分は切れる。
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Args {
    std::wstring title = L"AcousticFlowUE";   // 窓の題に含まれる文字（窓の種類は UnrealWindow）
    std::wstring out;                         // 出力の MP4
    std::wstring info;                        // 撮り始めの合図（AFCapture_info.txt。出たら撮り始める）
    std::wstring wav;                         // 音の WAV（AfReplayWav が作る。時刻 0 ＝ 合図の qpc）。空ならループバック
    int fps = 30;
    int bitrate = 6000000;
    double waitSec = 180.0;                   // 窓と合図を待つ上限
    double maxSec = 180.0;                    // 撮る長さの上限
};

struct FindCtx { const std::wstring* title; HWND found; };

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    FindCtx* c = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[128] = {}, ttl[512] = {};
    GetClassNameW(h, cls, 127);
    GetWindowTextW(h, ttl, 511);
    if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    if (!wcsstr(ttl, c->title->c_str())) return TRUE;
    RECT r{};
    GetClientRect(h, &r);
    if (r.right - r.left < 320 || r.bottom - r.top < 240) return TRUE;
    c->found = h;
    return FALSE;
}

HWND FindGameWindow(const std::wstring& title) {
    FindCtx c{&title, nullptr};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/// 合図（AFCapture_info.txt）の qpc を読む。Unreal の SaveStringToFile は UTF-16 か UTF-8 で書くので、数字だけ拾う。
bool ReadInfoQpc(const std::wstring& path, unsigned long long& qpc) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    std::string ascii;
    for (char c : raw) if (c != 0) ascii.push_back(c);   // UTF-16 の 0 を落とす
    const size_t p = ascii.find("qpc=");
    if (p == std::string::npos) return false;
    qpc = std::strtoull(ascii.c_str() + p + 4, nullptr, 10);
    return qpc != 0;
}

/// WAV を読んで 2 ch の float にする（PCM 16・float 32）。
bool ReadWavStereo(const std::wstring& path, std::vector<float>& lr, int& rate) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::vector<char> b;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    size_t pos = 12, off = 0, len = 0; int tag = 0, ch = 0, bits = 0; rate = 0;
    while (pos + 8 <= b.size()) {
        const uint32_t sz = u32(pos + 4);
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0) { tag = static_cast<int>(u16(pos + 8)); ch = static_cast<int>(u16(pos + 10)); rate = static_cast<int>(u32(pos + 12)); bits = static_cast<int>(u16(pos + 22)); }
        else if (std::memcmp(b.data() + pos, "data", 4) == 0) { off = pos + 8; len = std::min<size_t>(sz, b.size() - off); break; }
        pos += 8 + sz + (sz & 1);
    }
    if (!off || ch <= 0 || bits <= 0 || rate <= 0) return false;
    const int bps = bits / 8;
    const size_t frames = len / static_cast<size_t>(bps * ch);
    lr.assign(frames * 2, 0.0f);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c) {
            const char* q = b.data() + off + (i * ch + std::min(c, ch - 1)) * bps;
            float v = 0.0f;
            if (tag == 3 && bps == 4) std::memcpy(&v, q, 4);
            else if (bps == 2) { int16_t x; std::memcpy(&x, q, 2); v = x / 32768.0f; }
            lr[i * 2 + c] = v;
        }
    return true;
}

// ── 音: WASAPI のループバック ──
//   パケットを「頭の時刻（100 ns 単位の QPC）」付きで溜める。並べるのは撮り終えてから。
struct AudioPacket { UINT64 qpc100ns; std::vector<float> lr; };

class LoopbackRecorder {
public:
    /// 出力の機器 1 つでループバックを始める（機器は借りる。Stop で返す）。
    bool Start(IMMDevice* dev) {
        dev_ = dev; dev_->AddRef();
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev_->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
            PropVariantClear(&v);
            SafeRelease(ps);
        }
        HRESULT hr = S_OK;
        if (SUCCEEDED(hr)) hr = dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
        if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&fmt_);
        if (SUCCEEDED(hr)) hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, fmt_, nullptr);
        if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&cap_));
        if (FAILED(hr)) return false;
        isFloat_ = fmt_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (fmt_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        rate = static_cast<int>(fmt_->nSamplesPerSec);
        run_ = true;
        if (FAILED(client_->Start())) return false;
        th_ = std::thread([this] { Loop(); });
        return true;
    }
    void Stop() {
        run_ = false;
        if (th_.joinable()) th_.join();
        if (client_) client_->Stop();
        SafeRelease(cap_); SafeRelease(client_); SafeRelease(dev_);
        if (fmt_) { CoTaskMemFree(fmt_); fmt_ = nullptr; }
    }
    /// 録った音のエネルギー（どの機器でゲームが鳴ったかを選ぶ）。
    double Energy() const {
        double e = 0.0;
        for (const AudioPacket& p : packets) for (float v : p.lr) e += static_cast<double>(v) * v;
        return e;
    }
    std::vector<AudioPacket> packets;
    int rate = 48000;
    std::wstring name;

private:
    void Loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const int ch = fmt_->nChannels, bps = fmt_->wBitsPerSample / 8;
        while (run_) {
            Sleep(5);
            UINT32 next = 0;
            while (run_ && SUCCEEDED(cap_->GetNextPacketSize(&next)) && next > 0) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 devPos = 0, qpc = 0;
                if (FAILED(cap_->GetBuffer(&data, &frames, &flags, &devPos, &qpc))) break;
                AudioPacket p; p.qpc100ns = qpc; p.lr.assign(static_cast<size_t>(frames) * 2, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    for (UINT32 i = 0; i < frames; ++i)
                        for (int c = 0; c < 2; ++c) {
                            const BYTE* s = data + (static_cast<size_t>(i) * ch + std::min(c, ch - 1)) * bps;
                            float v = 0.0f;
                            if (isFloat_ && bps == 4) std::memcpy(&v, s, 4);
                            else if (bps == 2) { int16_t x; std::memcpy(&x, s, 2); v = x / 32768.0f; }
                            else if (bps == 4) { int32_t x; std::memcpy(&x, s, 4); v = x / 2147483648.0f; }
                            p.lr[static_cast<size_t>(i) * 2 + c] = v;
                        }
                cap_->ReleaseBuffer(frames);
                packets.push_back(std::move(p));
            }
        }
        CoUninitialize();
    }
    IMMDevice* dev_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* cap_ = nullptr;
    WAVEFORMATEX* fmt_ = nullptr;
    bool isFloat_ = false;
    std::atomic<bool> run_{false};
    std::thread th_;
};

HRESULT ConfigureWriter(IMFSinkWriter* w, int W, int H, int fps, int bitrate, DWORD& vIdx, DWORD& aIdx) {
    HRESULT hr = S_OK;
    IMFMediaType* t = nullptr;
    // 映像: H.264（出力）← RGB32 の上から下（入力）
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    t->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, static_cast<UINT32>(W), static_cast<UINT32>(H));
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = w->AddStream(t, &vIdx); SafeRelease(t); if (FAILED(hr)) return hr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(W * 4));   // 正 ＝ 上から下
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, static_cast<UINT32>(W), static_cast<UINT32>(H));
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = w->SetInputMediaType(vIdx, t, nullptr); SafeRelease(t); if (FAILED(hr)) return hr;
    // 音: AAC 192 kbps（出力）← 48 kHz・2 ch・16 bit（入力）
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    hr = w->AddStream(t, &aIdx); SafeRelease(t); if (FAILED(hr)) return hr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
    hr = w->SetInputMediaType(aIdx, t, nullptr); SafeRelease(t);
    return hr;
}

HRESULT WriteBytes(IMFSinkWriter* w, DWORD idx, const void* data, DWORD bytes, LONGLONG time, LONGLONG dur) {
    IMFMediaBuffer* buf = nullptr;
    IMFSample* s = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    BYTE* p = nullptr;
    if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
    if (SUCCEEDED(hr)) { std::memcpy(p, data, bytes); buf->Unlock(); hr = buf->SetCurrentLength(bytes); }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
    if (SUCCEEDED(hr)) hr = s->AddBuffer(buf);
    if (SUCCEEDED(hr)) hr = s->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = s->SetSampleDuration(dur);
    if (SUCCEEDED(hr)) hr = w->WriteSample(idx, s);
    SafeRelease(s); SafeRelease(buf);
    return hr;
}

bool ParseArgs(int argc, wchar_t** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring(); };
        if (k == L"--title") a.title = next();
        else if (k == L"--out") a.out = next();
        else if (k == L"--info") a.info = next();
        else if (k == L"--wav") a.wav = next();
        else if (k == L"--fps") a.fps = std::max(1, _wtoi(next().c_str()));
        else if (k == L"--bitrate") a.bitrate = std::max(500000, _wtoi(next().c_str()));
        else if (k == L"--wait") a.waitSec = _wtof(next().c_str());
        else if (k == L"--max") a.maxSec = _wtof(next().c_str());
    }
    return !a.out.empty() && !a.info.empty();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);   // 日本語をそのまま（既定のままだと最初の日本語で出力が止まる）
    _setmode(_fileno(stderr), _O_U8TEXT);
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        std::fwprintf(stderr, L"usage: af_capture --out <mp4> --info <AFCapture_info.txt> [--title AcousticFlowUE] [--fps 30] [--bitrate 6000000] [--wait 180] [--max 180]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // 画面の座標を実の画素で
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    auto now = []() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; };

    // ── 窓と撮り始めの合図を待つ ──
    const long long waitEnd = now() + static_cast<long long>(a.waitSec * qf.QuadPart);
    HWND hwnd = nullptr;
    while (now() < waitEnd) {
        if (!hwnd) hwnd = FindGameWindow(a.title);
        if (hwnd && FileExists(a.info)) break;
        Sleep(20);
    }
    if (!hwnd || !FileExists(a.info)) { std::fwprintf(stderr, L"[af_capture] 窓か撮り始めの合図が来なかった\n"); return 3; }
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // 上に何も重ならないように
    RECT cr{}; GetClientRect(hwnd, &cr);
    POINT org{0, 0}; ClientToScreen(hwnd, &org);
    const int W = (cr.right - cr.left) & ~1, H = (cr.bottom - cr.top) & ~1;   // H.264 は偶数
    std::wprintf(L"[af_capture] 窓 %dx%d @ (%ld, %ld) を撮る\n", W, H, org.x, org.y);

    MFStartup(MF_VERSION);
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);   // 映像を先に全部、音を後から書く（待たせない）
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFSinkWriter* writer = nullptr;
    HRESULT hr = MFCreateSinkWriterFromURL(a.out.c_str(), nullptr, attr, &writer);
    SafeRelease(attr);
    DWORD vIdx = 0, aIdx = 0;
    if (SUCCEEDED(hr)) hr = ConfigureWriter(writer, W, H, a.fps, a.bitrate, vIdx, aIdx);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) { std::fwprintf(stderr, L"[af_capture] 書き手を作れない 0x%08lx\n", static_cast<unsigned long>(hr)); return 4; }

    // 有効な出力の機器を全部録り、撮り終えてから一番大きく鳴った機器を採る（Wwise がどの機器へ出すかは環境で違う）。
    std::vector<LoopbackRecorder*> recs;
    if (a.wav.empty()) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0; col->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d))) continue;
                LoopbackRecorder* r = new LoopbackRecorder();
                if (r->Start(d)) recs.push_back(r); else { r->Stop(); delete r; }
                SafeRelease(d);
            }
        }
        SafeRelease(col); SafeRelease(en);
    }
    if (a.wav.empty() && recs.empty()) std::wprintf(L"[af_capture] 音の機器を開けなかった（無音で書く）\n");

    // ── 映像: 30 fps の刻みで画面から取る（遅れた刻みは飛ばす。時刻は刻みの番号から）──
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   // 負 ＝ 上から下
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    const LONGLONG frameDur = 10000000LL / a.fps;
    const long long t0 = now();
    long long lastIndex = -1, frames = 0, dropped = 0;
    const long long maxTicks = static_cast<long long>(a.maxSec * qf.QuadPart);
    while (IsWindow(hwnd) && IsWindowVisible(hwnd) && now() - t0 < maxTicks) {
        const long long idx = (now() - t0) * a.fps / qf.QuadPart;
        if (idx <= lastIndex) { Sleep(1); continue; }
        if (lastIndex >= 0 && idx > lastIndex + 1) dropped += idx - lastIndex - 1;
        lastIndex = idx;
        BitBlt(mem, 0, 0, W, H, screen, org.x, org.y, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        uint32_t* px = static_cast<uint32_t*>(bits);
        for (long long i = 0, n = static_cast<long long>(W) * H; i < n; ++i) px[i] |= 0xFF000000u;   // α を不透明に
        if (FAILED(WriteBytes(writer, vIdx, bits, static_cast<DWORD>(W * H * 4), idx * frameDur, frameDur))) break;
        ++frames;
    }
    LoopbackRecorder* best = nullptr;
    double bestE = 0.0;
    for (LoopbackRecorder* r : recs) {
        r->Stop();
        const double e = r->Energy();
        std::wprintf(L"[af_capture] 機器「%ls」: %zu パケット、%.1f dB\n", r->name.c_str(), r->packets.size(), 10.0 * std::log10(std::max(e, 1e-20)));
        if (e > bestE) { bestE = e; best = r; }
    }
    const bool haveAudio = best != nullptr;
    if (haveAudio) std::wprintf(L"[af_capture] 音は「%ls」から取る\n", best->name.c_str());
    LoopbackRecorder empty;
    LoopbackRecorder& rec = haveAudio ? *best : empty;   // 鳴った機器が無ければ無音
    const LONGLONG videoEnd = (lastIndex + 1) * frameDur;
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
    std::wprintf(L"[af_capture] 映像 %lld 枚（飛ばした刻み %lld）、%.2f 秒\n", frames, dropped, videoEnd / 1e7);

    // ── 音: パケットを頭の時刻で映像の時計の上に置き、48 kHz にする ──
    const long long outFrames = videoEnd * 48000 / 10000000;
    std::vector<float> tl;   // 機器の標本化周波数のままの時間軸（映像の頭 ＝ 0）
    const double t0Sec = static_cast<double>(t0) / qf.QuadPart;
    long long placed = 0;
    if (!a.wav.empty()) {
        // AfReplayWav が書き終えるのを待つ（大きさが 1 秒変わらなくなるまで、最大 180 秒）
        long long lastSize = -1; int stable = 0;
        for (int i = 0; i < 1800 && stable < 10; ++i) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            const long long size = GetFileAttributesExW(a.wav.c_str(), GetFileExInfoStandard, &fa)
                ? (static_cast<long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow : -1;
            stable = (size > 44 && size == lastSize) ? stable + 1 : 0;
            lastSize = size;
            Sleep(100);
        }
        unsigned long long infoQpc = 0;
        std::vector<float> wavLr; int wavRate = 0;
        if (ReadInfoQpc(a.info, infoQpc) && ReadWavStereo(a.wav, wavLr, wavRate)) {
            const double offsetSec = (static_cast<double>(infoQpc) - static_cast<double>(t0)) / qf.QuadPart;   // 合図は映像の頭より前（負）
            const long long n = static_cast<long long>(std::ceil(videoEnd / 1e7 * wavRate)) + 1;
            tl.assign(static_cast<size_t>(n) * 2, 0.0f);
            const long long m = static_cast<long long>(wavLr.size() / 2);
            for (long long k = 0; k < n; ++k) {
                const long long j = k + std::llround(-offsetSec * wavRate);   // 映像の時刻 k の音は、WAV の j 番目
                if (j < 0 || j >= m) continue;
                tl[static_cast<size_t>(k) * 2] = wavLr[static_cast<size_t>(j) * 2];
                tl[static_cast<size_t>(k) * 2 + 1] = wavLr[static_cast<size_t>(j) * 2 + 1];
            }
            rec.rate = wavRate;
            std::wprintf(L"[af_capture] 音は %ls（%.2f 秒、%d Hz）、合図は映像の頭から %+.3f 秒\n", a.wav.c_str(), static_cast<double>(m) / wavRate, wavRate, offsetSec);
        } else {
            std::wprintf(L"[af_capture] 音の WAV か合図を読めなかった（無音で書く）\n");
        }
    } else if (haveAudio) {
        const long long n = static_cast<long long>(std::ceil(videoEnd / 1e7 * rec.rate)) + 1;
        tl.assign(static_cast<size_t>(n) * 2, 0.0f);
        for (const AudioPacket& p : rec.packets) {
            const long long start = std::llround((p.qpc100ns / 1e7 - t0Sec) * rec.rate);
            const long long m = static_cast<long long>(p.lr.size() / 2);
            for (long long i = 0; i < m; ++i) {
                const long long k = start + i;
                if (k < 0 || k >= n) continue;
                tl[static_cast<size_t>(k) * 2] = p.lr[static_cast<size_t>(i) * 2];
                tl[static_cast<size_t>(k) * 2 + 1] = p.lr[static_cast<size_t>(i) * 2 + 1];
                ++placed;
            }
        }
        std::wprintf(L"[af_capture] 音 %zu パケット・%.2f 秒ぶん（%d Hz）を映像の時計に置いた\n", rec.packets.size(), static_cast<double>(placed) / rec.rate, rec.rate);
    }
    std::vector<int16_t> pcm(static_cast<size_t>(outFrames) * 2, 0);
    double peak = 0.0;
    if (!tl.empty()) {
        const long long n = static_cast<long long>(tl.size() / 2);
        for (long long i = 0; i < outFrames; ++i) {
            const double s = static_cast<double>(i) * rec.rate / 48000.0;
            const long long k = static_cast<long long>(s);
            if (k + 1 >= n) break;
            const float f = static_cast<float>(s - k);
            for (int c = 0; c < 2; ++c) {
                const float v = tl[static_cast<size_t>(k) * 2 + c] * (1.0f - f) + tl[static_cast<size_t>(k + 1) * 2 + c] * f;
                peak = std::max(peak, static_cast<double>(std::fabs(v)));
                pcm[static_cast<size_t>(i) * 2 + c] = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f));
            }
        }
        std::wprintf(L"[af_capture] 音の最大 %.1f dBFS\n", 20.0 * std::log10(std::max(peak, 1e-9)));
    }
    const long long chunk = 4800;   // 100 ms ずつ
    for (long long i = 0; i < outFrames; i += chunk) {
        const long long m = std::min(chunk, outFrames - i);
        WriteBytes(writer, aIdx, pcm.data() + i * 2, static_cast<DWORD>(m * 4), i * 10000000LL / 48000, m * 10000000LL / 48000);
    }
    hr = writer->Finalize();
    SafeRelease(writer);
    MFShutdown();
    CoUninitialize();
    timeEndPeriod(1);
    if (FAILED(hr)) { std::fwprintf(stderr, L"[af_capture] 書き終えられない 0x%08lx\n", static_cast<unsigned long>(hr)); return 5; }
    std::wprintf(L"[af_capture] 書いた: %ls\n", a.out.c_str());
    return 0;
}
