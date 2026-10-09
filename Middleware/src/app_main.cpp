// app_main.cpp — Windows アプリケーション版のホスト（AfHost）。
//
// ★これは「システムを載せていない状態のミドルウェア」です。
//   音響エンジンにリンクしていないし、include もしていません。
//   エンジンはこのアプリにとって **依存先ではなく、実行時に読み込む対象**です。
//
// ★なぜ Dear ImGui なのか
//   Unity 側の監視窓は全部イミディエイトモード（`OnGUI` / `GUILayout`）で書かれています
//   ── `AcousticToolsWindow.cs` に 102 箇所、`StatusMonitorWindow.cs` に 58 箇所。
//   同じ書き方のまま移せるので、10 枚の窓を運ぶ費用がいちばん安くなります。
//   メータや波形のように毎フレーム描き替えるものとも相性が良い。
//
//   ⚠ 資料の「外部ライブラリなし」は**エンジンの主張**です。ここは道具なので
//     既製品を使い、浮いた時間を音に回します。エンジン側は 0 本のまま。

#include "audio_device.h"
#include "block_source.h"
#include "voice_bus.h"
#include "host_util.h"
#include "module.h"
#include "project.h"
#include "selftest.h"
#include "undo.h"

#include <windows.h>

#include <d3d11.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder（既定の窓配置を組むため）
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <functional>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ── D3D11 の最小限の面倒（ImGui を出すだけなので、深いことはしない）
ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_context     = nullptr;
IDXGISwapChain*         g_swapChain   = nullptr;
ID3D11RenderTargetView* g_backBuffer  = nullptr;
bool                    g_needResize  = false;
UINT                    g_resizeW = 0, g_resizeH = 0;

void createBackBuffer() {
    ID3D11Texture2D* tex = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&tex));
    if (!tex) return;
    g_device->CreateRenderTargetView(tex, nullptr, &g_backBuffer);
    tex->Release();
}

void releaseBackBuffer() {
    if (g_backBuffer) { g_backBuffer->Release(); g_backBuffer = nullptr; }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = {};

    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &sd, &g_swapChain, &g_device, &got, &g_context);

    // ハードウェアが無い環境（リモート・仮想機）でも立ち上がるようにする。
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
            &sd, &g_swapChain, &g_device, &got, &g_context);

    if (FAILED(hr)) return false;
    createBackBuffer();
    return true;
}

void destroyDevice() {
    releaseBackBuffer();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

// ★自分の描いた絵を自分で書き出す。
//
//   画面キャプチャに頼らないのは、**他人の窓を巻き込むから**です。
//   実際、前面化が効かずに無関係な窓を撮ってしまいました。
//   ここならバックバッファそのものなので、写るのはこのアプリだけです。
//   資料の図もこれで作れます（毎回同じ絵が出る）。
bool saveBackBufferPng(const std::wstring& path, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!g_swapChain || !g_device || !g_context) return fail("D3D がまだ用意できていません");

    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
        return fail("バックバッファを取れません");

    D3D11_TEXTURE2D_DESC d = {};
    back->GetDesc(&d);

    // CPU から読める複製を作って、そこへ写す（バックバッファは直接読めない）。
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;

    ID3D11Texture2D* stage = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &stage)) || !stage) {
        back->Release();
        return fail("読み出し用の複製を作れません");
    }
    g_context->CopyResource(stage, back);
    back->Release();

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_context->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        stage->Release();
        return fail("複製を読み出せません");
    }

    // 行ごとに詰め直す（RowPitch は幅 × 4 とは限らない）。
    //
    // ★同時に R と B を入れ替える。
    //   スワップチェーンは DXGI_FORMAT_R8G8B8A8_UNORM（並びは R,G,B,A）だが、
    //   WIC の PNG 符号器が受けるのは 32bppBGRA。ここを揃えないと**青が橙になる**
    //   （最初の書き出しで実際にそうなった。見出しの青 0.16,0.29,0.48 が
    //     0.48,0.29,0.16 の茶色で出た）。
    const UINT w = d.Width, h = d.Height;
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const unsigned char* src =
            static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        unsigned char* dst = px.data() + static_cast<size_t>(y) * w * 4;
        for (UINT x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];   // B
            dst[x * 4 + 1] = src[x * 4 + 1];   // G
            dst[x * 4 + 2] = src[x * 4 + 0];   // R
            dst[x * 4 + 3] = src[x * 4 + 3];   // A
        }
    }
    g_context->Unmap(stage, 0);
    stage->Release();

    // WIC で PNG にする。OS が持っているので外部ライブラリは増えない。
    IWICImagingFactory* wic = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic))))
        return fail("WIC を作れません");

    IWICBitmapEncoder*     enc    = nullptr;
    IWICBitmapFrameEncode* frame  = nullptr;
    IWICStream*            stream = nullptr;
    bool ok = false;

    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(px.size()), px.data())) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
            ok = true;
    }

    if (frame)  frame->Release();
    if (enc)    enc->Release();
    if (stream) stream->Release();
    wic->Release();

    if (!ok) return fail("PNG を書き出せません");
    if (err) err->clear();
    return true;
}

// エクスプローラから放り込まれたファイル。窓の手続きが積み、画面側が引き取る。
// ★ここで取り込みまでやらない。窓の手続きは**受け取るだけ**にして、
//   ファイルを読む・鳴らすといった時間の掛かることは主ループでやる。
std::vector<std::wstring> g_dropped;

// エクスプローラでそのファイルを選んだ状態で開く。
void revealInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_DROPFILES: {
        HDROP h = reinterpret_cast<HDROP>(wp);
        const UINT n = ::DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH * 2] = {};
            if (::DragQueryFileW(h, i, buf, MAX_PATH * 2)) g_dropped.push_back(buf);
        }
        ::DragFinish(h);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && g_device) {
            g_needResize = true;
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // Alt でメニューを開かない
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 画面に出す小物 ─────────────────────────────────

void labelValue(const char* k, const char* fmt, ...) {
    ImGui::TextUnformatted(k);
    ImGui::SameLine(160.0f);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

// 目盛り付きの横棒。0..1 を色で塗る。
void bar(float v01, const ImVec4& col, const char* overlay) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(v01, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::PopStyleColor();
}


// ── ファイルを選ぶ（OS の物を使う。依存を増やさない）
bool pickFile(HWND owner, bool forSave, std::wstring* out) {
    wchar_t buf[MAX_PATH * 2] = {};
    if (out && !out->empty() && out->size() < MAX_PATH)
        ::wcscpy_s(buf, out->c_str());

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"AfHost プロジェクト (*.afproj)\0*.afproj\0すべてのファイル (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH * 2;
    ofn.lpstrDefExt = L"afproj";
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                      (forSave ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    const BOOL ok = forSave ? ::GetSaveFileNameW(&ofn) : ::GetOpenFileNameW(&ofn);
    if (!ok) return false;
    *out = buf;
    return true;
}

// WAV を選ぶ。複数まとめて選べる。
//
// ⚠ 複数選択の戻り値は「フォルダ\0名前1\0名前2\0\0」という並びで、1 本だけのときは
//   「フルパス\0\0」になる。**同じ配列で 2 つの形が返る**ので、両方を読む。
bool pickWavs(HWND owner, std::vector<std::wstring>* out) {
    std::vector<wchar_t> buf(32768, 0);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"WAV (*.wav)\0*.wav\0すべてのファイル (*.*)\0*.*\0";
    ofn.lpstrFile   = buf.data();
    ofn.nMaxFile    = static_cast<DWORD>(buf.size());
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR |
                      OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (!::GetOpenFileNameW(&ofn)) return false;

    out->clear();
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;

    if (*p == L'\0') {                 // 1 本だけ
        out->push_back(first);
    } else {                           // フォルダ＋名前が続く
        while (*p) {
            std::wstring name = p;
            p += name.size() + 1;
            out->push_back(first + L"\\" + name);
        }
    }
    return !out->empty();
}

// ── 操作盤の絵 ───────────────────────────────────
//
// ★字ではなく図形で描いている。
//   ■ ❚❚ ▶ は日本語フォントに無いことがあり、豆腐になると
//   「押せない」のか「字が無い」のか区別が付かない（✔ で一度踏んだ）。
//   自分で描けばフォントに依らないし、押せる／押せないも色で出せる。

enum class Glyph { Stop, Pause, Play };

bool transportButton(const char* id, Glyph g, bool enabled, bool lit, float size = 42.0f) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 box(size, size);

    ImGui::InvisibleButton("##b", box);
    const bool pressed = enabled && ImGui::IsItemClicked();
    const bool hover   = enabled && ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 col;
    if (!enabled)   col = IM_COL32(110, 116, 126, 255);
    else if (lit)   col = IM_COL32(120, 230, 190, 255);
    else if (hover) col = IM_COL32(255, 255, 255, 255);
    else            col = IM_COL32(215, 222, 232, 255);

    const float th = 2.4f;                 // 線の太さ
    const float m  = size * 0.22f;         // 余白
    const ImVec2 a(p.x + m, p.y + m), b(p.x + size - m, p.y + size - m);

    switch (g) {
    case Glyph::Stop:
        dl->AddRect(a, b, col, 0.0f, 0, th);
        break;
    case Glyph::Pause: {
        const float w = (b.x - a.x) * 0.30f;
        dl->AddRect(a, ImVec2(a.x + w, b.y), col, 0.0f, 0, th);
        dl->AddRect(ImVec2(b.x - w, a.y), b, col, 0.0f, 0, th);
        break;
    }
    case Glyph::Play:
        dl->AddTriangle(ImVec2(a.x, a.y), ImVec2(b.x, (a.y + b.y) * 0.5f),
                        ImVec2(a.x, b.y), col, th);
        break;
    }

    ImGui::PopID();
    return pressed;
}

// 長さを「1:23.4」の形に。
std::string durationText(double sec) {
    char b[32];
    const int m = static_cast<int>(sec) / 60;
    ::sprintf_s(b, "%d:%04.1f", m, sec - m * 60);
    return b;
}

std::string sizeText(std::uint64_t bytes) {
    char b[32];
    if (bytes >= 1024ull * 1024) ::sprintf_s(b, "%.1f MB", bytes / (1024.0 * 1024.0));
    else                         ::sprintf_s(b, "%.0f KB", bytes / 1024.0);
    return b;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // WIC（PNG 書き出し）と WASAPI が COM を使う。音声スレッド側は自前で初期化するので、
    // ここで初期化しても二重にはならない（入れ子は S_FALSE が返るだけ）。
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ── 引数
    std::wstring argProject, shotPath, focusPanel;
    int  shotFrames    = 40;      // 字体と配置が落ち着くまで少し回す
    bool startWithTone = false;   // 起動と同時に試験信号を出す（絵を撮る用）
    int  selectIndex   = -1;      // 起動と同時に台帳の N 行目を選ぶ（絵を撮る用）
    bool startPlaying  = false;   // その音を鳴らし始める（同上）
    bool startRenaming = false;   // 名前の変更を開いた状態にする（同上）
    bool showEmptyMenu = false;   // 空き場所の右クリックを開いた状態にする（同上）
    bool evEditShot    = false;   // イベントを 1 つ作って編集メニューを開く（同上・T26 の証拠用）
    bool infoShot      = false;   // プロパティを「情報」側にして撮る（同上）
    int  fireIndex     = -1;      // N 番目のイベントを発火する（同上）
    {
        int argc = 0;
        wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--shot" && i + 1 < argc)        shotPath = af::absolutePath(argv[++i]);
            else if (a == L"--frames" && i + 1 < argc) shotFrames = _wtoi(argv[++i]);
            else if (a == L"--tone")                  startWithTone = true;
            // 資料の図を撮るとき、見せたいパネルを前に出す。
            else if (a == L"--panel" && i + 1 < argc) focusPanel = argv[++i];
            else if (a == L"--select" && i + 1 < argc) selectIndex = _wtoi(argv[++i]);
            else if (a == L"--play")                  startPlaying = true;
            else if (a == L"--rename")                startRenaming = true;
            else if (a == L"--menu")                  showEmptyMenu = true;
            else if (a == L"--evedit")                evEditShot = true;
            else if (a == L"--info")                  infoShot = true;
            else if (a == L"--fire" && i + 1 < argc)  fireIndex = _wtoi(argv[++i]);
            else if (!a.empty() && a[0] != L'-' && argProject.empty()) argProject = a;
        }
        if (argv) ::LocalFree(argv);
    }

    // ── 窓
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = inst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AfHostWindow";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"AfHost", WS_OVERLAPPEDWINDOW,
                                80, 60, 1180, 760, nullptr, nullptr, inst, nullptr);
    if (!createDevice(hwnd)) {
        ::MessageBoxW(nullptr, L"Direct3D 11 を初期化できませんでした。", L"AfHost", MB_ICONERROR);
        destroyDevice();
        ::UnregisterClassW(wc.lpszClassName, inst);
        return 1;
    }

    // エクスプローラからの放り込みを受け取る（WM_DROPFILES）。
    ::DragAcceptFiles(hwnd, TRUE);

    ::ShowWindow(hwnd, shotPath.empty() ? SW_SHOWDEFAULT : SW_SHOWNOACTIVATE);
    ::UpdateWindow(hwnd);

    // ── ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // ★配置は覚える。道具は毎回並べ直すものではない。
    //   ただし初回だけは既定の配置を組む（何も無いと窓が全部重なって出る）。
    io.IniFilename = "AfHost.ini";
    const bool hadIni = af::fileExists(L"AfHost.ini");

    // ★日本語の字が要る。既定のフォントには入っていないので OS のものを読む。
    //   JIS の記号（★ ■ ● ▲）も使うので、仮名漢字だけの範囲では足りない。
    {
        static ImVector<ImWchar> ranges;
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        for (ImWchar c = 0x2190; c <= 0x266F; ++c) b.AddChar(c);   // 矢印・図形・記号
        b.BuildRanges(&ranges);

        static const char* kFonts[] = {
            "C:\\Windows\\Fonts\\YuGothM.ttc",
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\msgothic.ttc",
        };
        for (const char* f : kFonts)
            if (io.Fonts->AddFontFromFileTTF(f, 17.0f, nullptr, ranges.Data)) break;
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding  = 3.0f;
    style.GrabRounding   = 3.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // ── 中身
    std::vector<std::string> log;
    auto say = [&](const std::string& s) {
        log.push_back(s);
        if (log.size() > 400) log.erase(log.begin());
    };

    af::Project proj;

    // ★音源を作る前に形式を見る（開いてから作り直すと実時間スレッドと競合する）。
    int probeRate = 48000, probeCh = 2;
    std::string perr;
    if (!af::AudioDevice::probeDefaultFormat(&probeRate, &probeCh, &perr))
        say("形式を見られませんでした（" + perr + "）。48000 Hz として続けます");

    af::ToneSource  tone(static_cast<double>(probeRate), proj.toneHz, proj.toneLevel);
    af::AudioDevice device;
    af::VoiceBus    bus;
    af::Module      mod;

    int  selected     = -1;   // SoundLibrary で選んでいる音源（-1 = 選んでいない）
    std::string selFolder;    // ツリーで選んでいるフォルダ
    bool scrubWasPlaying = false;   // 波形を掴む前に鳴っていたか

    // ★画面が触っている 1 本。**選んでいる音源に割り当てられた本**を指します。
    //   ⚠ nullptr は返しません。何も載っていなければ空の本を返すので、
    //     呼ぶ側は毎回 null を確かめなくて済みます（1 箇所忘れると落ちるため）。
    auto cur = [&]() -> af::ClipSource& {
        af::ClipSource* c = bus.find(selected);
        return c ? *c : bus.idle();
    };

    // いまデバイスに繋いでいるのはどちらか。
    // ★試験信号と試聴は混ぜません。試聴どうしは**同時に数本**鳴ります（イベントのため）。
    enum class Playing { Tone, Clip };
    Playing wired = Playing::Tone;

    bool clipPlaying  = false;
    std::string firedEvent;         // 直近に発火したイベントの名前（操作盤の見出し用）

    // プロジェクトの値を音源へ流す（開いた直後と、いじった直後に呼ぶ）。
    auto applyProject = [&] {
        tone.setFrequency(proj.toneHz);
        tone.setLevel(proj.toneLevel);
    };

    if (!argProject.empty()) {
        std::string e;
        if (proj.load(af::absolutePath(argProject), &e)) {
            applyProject();
            say("開きました : " + af::wideToUtf8(proj.path));
        } else {
            say("開けません ── " + e);
        }
    }

    std::string err;
    if (device.start(&tone, &err)) say("出力 : " + device.deviceName());
    else                           say("出力デバイスを開けませんでした ── " + err);

    bool playing = startWithTone || proj.toneOnOpen;
    tone.setActive(playing);

    // 自己検査は 1.1 秒かかるので別スレッドで回す（窓が固まらないように）。
    std::thread        testThread;
    std::atomic<bool>  testRunning{false};
    af::SelfTestReport report;
    bool               haveReport = false;

    // 表示する窓（表示メニューから出し入れする）
    bool showProject = true, showLibrary = true, showControl = true;
    bool showProps = true, showTone = true, showEvents = true;
    bool showOutput = true, showLog = true;
    bool showSelfTest = false, showModule = false;

    // ★試聴する音を差し替える。デバイスを止めてから入れて、開け直す。
    //   実時間スレッドが読んでいる配列を入れ替えないための唯一確実な手。
    //   止まるのは 100ms 前後で、試聴の道具にはそれが許される。
    // 1 本ぶんを載せて鳴らす。★イベントからも試聴ボタンからもここを通します。
    //   ⚠ デバイスは開けっぱなし。1 本しか無かった頃は鳴らすたびに閉じて開き直していて、
    //     そのぶん途切れていました。束ねが常に載っているので中で済みます。
    auto startVoice = [&](int index) -> bool {
        af::SoundLibrary& lib = proj.library;
        if (index < 0 || static_cast<std::size_t>(index) >= lib.size()) return false;
        const auto& e = lib.entries()[static_cast<std::size_t>(index)];
        if (!e.resolved) { say("鳴らせません ── " + e.problem); return false; }

        const int rate = device.running() ? device.sampleRate() : probeRate;
        const int ch   = device.running() ? device.channels()   : probeCh;

        // ★同じ音が既に載っていれば読み直さない。読み直すと頭からになります。
        af::ClipSource* had = bus.find(index);
        af::ClipSource& v   = bus.bind(index);
        if (!had) {
            af::WavInfo info;
            std::vector<float> pcm;
            std::string e2;
            if (!af::readWav(lib.absolutePathOf(static_cast<std::size_t>(index)),
                             &info, &pcm, &e2)) {
                say("読めません ── " + e2);
                return false;
            }
            v.setClip(af::conformToDevice(pcm, info, rate, ch), ch, rate);
        }
        v.setGainDb(e.gainDb);
        v.setPitchSemitones(e.pitch);
        v.setLowPassHz(e.lpfHz);
        v.setHighPassHz(e.hpfHz);
        v.setLoop(e.loop);
        v.play();

        // 試験信号が載っていたら、束ねへ差し替える（ここだけデバイスを開け直す）。
        if (wired != Playing::Clip || !device.running()) {
            device.stop();
            bus.prepare(4096, ch);
            std::string se;
            if (!device.start(&bus, &se)) { say("開き直せません ── " + se); return false; }
            wired = Playing::Clip;
        }
        clipPlaying = true;
        playing = false;
        return true;
    };

    auto auditionClip = [&](int index) {
        if (!startVoice(index)) return;
        say("試聴 : " + proj.library.entries()[static_cast<std::size_t>(index)].name);
    };


    // 選んでいる音を鳴らす。★▶ ボタンとスペースが**同じここを呼びます**。
    //   別々に書くと、片方だけ直して食い違います（決めごと #1）。
    auto transportPlay = [&] {
        af::SoundLibrary& lib = proj.library;
        const bool hasSel = selected >= 0 && static_cast<std::size_t>(selected) < lib.size();
        if (!hasSel || !lib.entries()[static_cast<std::size_t>(selected)].resolved) return;

        // ★同じ音が既に載っていれば、載せ直さずに続きから鳴らす。
        //   ⚠ 載っているかは束ねに聞きます（本が複数あるので、
        //     「最後に載せたのは誰か」を別に覚えると必ずずれます）。
        if (bus.find(selected)) cur().play();
        else                    auditionClip(selected);
    };

    // スペース ── 送りの入切。
    //
    // ★鳴っていたら**一時停止**／鳴っていなければ鳴らす、の 2 通りだけ。
    //   ⚠ 止める（頭へ戻す）のではありません。聴きながら止めて、
    //     **同じ所から聴き直す**のが調整の手順なので、位置を捨てると使えません。
    //     頭へ戻したいときは ■ を押します。
    //   キー 1 つで 3 通りを回すと、押す前に何が起きるか分からなくなるので、
    //   一時停止と停止はキーとボタンで分けています。
    auto transportToggle = [&] {
        const bool loaded = (wired == Playing::Clip) && !cur().empty();
        if (loaded && cur().state() == af::ClipSource::State::Playing) cur().pause();
        else                                                          transportPlay();
    };

    // 試験信号のほうへ戻す。
    auto auditionTone = [&](bool on) {
        if (wired != Playing::Tone) {
            device.stop();
            bus.stopAll();          // ★束ねごと止める（1 本だけ止めても他が鳴り続ける）
            std::string se;
            if (!device.start(&tone, &se)) { say("開き直せません ── " + se); return; }
            wired = Playing::Tone;
            clipPlaying = false;
        }
        playing = on;
        tone.setActive(on);
    };

    // ★イベントを発火する。ゲーム側が名前で呼ぶのと同じ道を、道具からも通す。
    //
    //   ここを作らないと、イベントは「名前を書いただけの枠」になります。
    //   組んだそばから鳴らして確かめられることが、イベントが機能である条件です。
    //
    // ★2026-09-01、**動作のぶんだけ同時に鳴る**ようになりました。
    //   それまでは鳴らせるのが 1 本きりで、最後の「再生」しか残らず、
    //   しかもログは「再生 2」と出していました（数えるだけ数えて鳴っていない）。
    auto fireEvent = [&](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= proj.events.size()) return;
        const af::Event& e = proj.events.all()[static_cast<std::size_t>(index)];
        if (e.actions.empty()) { say("動作がありません : " + e.name); return; }

        int played = 0, stopped = 0, missing = 0;
        for (const af::EventAction& a : e.actions) {
            const int si = proj.library.indexOfId(a.soundId);
            if (si < 0) { ++missing; continue; }

            if (a.type == af::ActionType::Play) {
                // ★1 本ずつ別の本に載せる。⚠ 2026-09-01 まで 1 本しか無く、
                //   動作を 2 つ持つイベントは**最後の 1 つしか鳴っていませんでした**
                //   （それでもログは「再生 2」と出していた）。
                if (startVoice(si)) ++played;
            } else {
                // ★止めるのは**その動作が指している音**だけ。
                //   ⚠ 以前は鳴っているものを無差別に止めていて、
                //     A を止めるつもりで B が黙っていました。
                bus.stopEntry(si);
                ++stopped;
            }
        }
        // 操作盤に出すのは、鳴らしたうちの最後の 1 本。
        for (const af::EventAction& a : e.actions)
            if (a.type == af::ActionType::Play) {
                const int si = proj.library.indexOfId(a.soundId);
                if (si >= 0) selected = si;
            }
        firedEvent = e.name;
        char b[96];
        if (missing > 0) ::sprintf_s(b, "（再生 %d / 停止 %d / 見つからない %d）",
                                     played, stopped, missing);
        else             ::sprintf_s(b, "（再生 %d / 停止 %d）", played, stopped);
        say("発火 : " + e.name + b);
    };

    // ★配置の版。窓の増減や置き場所を変えたら、ここを上げること。
    //   ⚠ 上げないと、前の配置を覚えている ini のせいで**消えた窓を探すはめ**になります
    //     （2026-09-01 に「波形」を畳んで「プロパティ」を真ん中へ動かした回で必要になった）。
    //   覚えた配置は捨てますが、それは一度だけです。
    constexpr int kLayoutVersion = 5;   // 5 = AudioBank → SoundLibrary（窓の名前が変わった）
    const std::wstring layoutStamp = L"AfHost.layout";
    bool layoutStale = true;
    if (std::FILE* lf = nullptr; ::_wfopen_s(&lf, layoutStamp.c_str(), L"rb") == 0 && lf) {
        int v = 0;
        if (std::fscanf(lf, "%d", &v) == 1 && v == kLayoutVersion) layoutStale = false;
        std::fclose(lf);
    }
    if (layoutStale) {
        if (std::FILE* lf = nullptr; ::_wfopen_s(&lf, layoutStamp.c_str(), L"wb") == 0 && lf) {
            std::fprintf(lf, "%d\n", kLayoutVersion);
            std::fclose(lf);
        }
    }

    bool layoutDone  = hadIni && !layoutStale;
    bool resetLayout = false;

    float meter = 0.0f;
    std::vector<float> scope(af::AudioDevice::kScopeMax, 0.0f);
    int scopeN = 0;

    // ★選んでいる音源の「音データ」の姿。列ごとの谷と山を線 1 本にして描きます。
    //
    // ⚠ 出力の波形（オシロ）とは別物です。
    //   あれは実時間スレッドが**いま出している 1 ブロック**（441 サンプル＝10ms）。
    //   作業中に見たいのは**ファイルそのものの姿**のほうだと分かったので、
    //   真ん中はこちらに入れ替え、オシロは出力パネルへ寄せました。
    //
    // ⚠ 行番号ではなく **id で持ちます**。並びが変わると同じ番号が別の音源を指すので、
    //   番号で持つと古い絵が残ります。
    static constexpr int kWaveCols = 1024;
    unsigned waveId = 0;                    // どの音源のものを持っているか（0 = 無し）
    int      waveCh = 0;                    // ★チャンネルごとに 1 段ずつ持つ
    std::vector<float> waveMin, waveMax;    // [ch * kWaveCols + col]
    std::string waveNote;                   // 読めなかったときの理由

    // 選んだ音源のファイルを読み、**チャンネルごとに**列の谷と山にまとめる。
    //
    // ★左右を混ぜません。混ぜると、片側だけ入っている素材が
    //   「両方に入っている」ように見えます（ミュートで聴き比べる意味も消えます）。
    // ⚠ 選ぶたびにファイルを読み直します。試聴に足りるだけの短い音を前提にしているので
    //   これで足りますが、長い素材を扱うようになったら**ここが引っかかります**
    //   （そのときは焼いた素材の側に山谷を持たせるのが筋）。
    auto buildWave = [&](int index) {
        waveMin.clear();
        waveMax.clear();
        waveNote.clear();
        waveId = 0;
        waveCh = 0;

        af::SoundLibrary& lib = proj.library;
        if (index < 0 || static_cast<std::size_t>(index) >= lib.size()) return;
        const auto& e = lib.entries()[static_cast<std::size_t>(index)];
        waveId = e.id;
        if (!e.resolved) { waveNote = e.problem; return; }

        af::WavInfo info;
        std::vector<float> pcm;
        std::string err;
        if (!af::readWav(lib.absolutePathOf(static_cast<std::size_t>(index)),
                         &info, &pcm, &err)) {
            waveNote = err;
            return;
        }
        const int ch = info.channels > 0 ? info.channels : 1;
        const std::size_t frames = pcm.size() / static_cast<std::size_t>(ch);
        if (frames == 0) { waveNote = "中身がありません"; return; }

        waveCh = ch;
        waveMin.assign(static_cast<std::size_t>(ch) * kWaveCols, 0.0f);
        waveMax.assign(static_cast<std::size_t>(ch) * kWaveCols, 0.0f);
        for (int k = 0; k < ch; ++k) {
            for (int c = 0; c < kWaveCols; ++c) {
                const std::size_t a = frames * static_cast<std::size_t>(c) / kWaveCols;
                std::size_t b = frames * static_cast<std::size_t>(c + 1) / kWaveCols;
                if (b <= a) b = a + 1;
                float lo = 0.0f, hi = 0.0f;
                bool first = true;
                for (std::size_t f = a; f < b && f < frames; ++f) {
                    const float v = pcm[f * static_cast<std::size_t>(ch) +
                                        static_cast<std::size_t>(k)];
                    if (first) { lo = hi = v; first = false; }
                    else if (v < lo) lo = v;
                    else if (v > hi) hi = v;
                }
                const std::size_t o = static_cast<std::size_t>(k) * kWaveCols +
                                      static_cast<std::size_t>(c);
                waveMin[o] = lo;
                waveMax[o] = hi;
            }
        }
    };

    // 音データを描く。★掴んで頭出しもここでやります。
    //
    // ★2026-09-01、頭出しはコントロールパネルの帯からここへ移しました。
    //   **見ている場所と掴む場所が同じ**ほうが、どこへ飛ぶか分かるためです。
    //   ⚠ 帯は消しました。残すと同じ操作が 2 箇所になります（決めごと #1）。
    auto drawWave = [&](float h) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float  w  = ImGui::GetContentRegionAvail().x;
        if (w < 4.0f || h < 4.0f) return;
        const ImVec2 p1(p0.x + w, p0.y + h);

        const int lanes = waveCh > 0 ? waveCh : 1;
        const float lh  = h / static_cast<float>(lanes);

        dl->AddRectFilled(p0, p1, IM_COL32(16, 24, 36, 255));

        // ★この行に本が割り当たっているときだけ、位置の線を出す。
        const bool loaded = (wired == Playing::Clip) && bus.find(selected) != nullptr;

        for (int k = 0; k < lanes; ++k) {
            const float top = p0.y + lh * static_cast<float>(k);
            const float mid = top + lh * 0.5f;
            dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), IM_COL32(64, 84, 104, 255));

            // ★黙らせている側は暗く描く。目と耳で同じことが起きているのが分かるように。
            const bool muted = cur().channelMuted(k);
            const ImU32 col = muted ? IM_COL32(70, 90, 84, 255) : IM_COL32(96, 196, 156, 255);

            if (!waveMax.empty() && k < waveCh) {
                const int px = static_cast<int>(w);
                for (int x = 0; x < px; ++x) {
                    const int c = kWaveCols * x / (px > 0 ? px : 1);
                    const std::size_t o = static_cast<std::size_t>(k) * kWaveCols +
                                          static_cast<std::size_t>(c);
                    const float hi = mid - waveMax[o] * (lh * 0.44f);
                    const float lo = mid - waveMin[o] * (lh * 0.44f);
                    dl->AddLine(ImVec2(p0.x + x + 0.5f, hi),
                                ImVec2(p0.x + x + 0.5f, lo + 1.0f), col);
                }
            }
            // 段の名前（1ch なら「モノラル」）
            const char* nm = lanes == 1 ? "モノラル" : (k == 0 ? "L" : (k == 1 ? "R" : nullptr));
            char num[8];
            if (!nm) { ::sprintf_s(num, "%d", k + 1); nm = num; }
            dl->AddText(ImVec2(p0.x + 4, top + 2), IM_COL32(150, 165, 185, 255), nm);
            if (k > 0)
                dl->AddLine(ImVec2(p0.x, top), ImVec2(p1.x, top), IM_COL32(44, 56, 72, 255));
        }

        // ★掴んで頭出し。⚠ 掴んでいる間は鳴らさない。
        //   短い音を掴んだまま動かすと、その場所から鳴り続けて、
        //   何を聴いているのか分からなくなります。離したら、掴む前に
        //   鳴っていた場合だけ鳴らし直します。
        ImGui::InvisibleButton("##wave", ImVec2(w, h));
        const bool held = ImGui::IsItemActive();
        if (ImGui::IsItemActivated() && loaded) {
            scrubWasPlaying = (cur().state() == af::ClipSource::State::Playing);
            cur().pause();
        }
        if (held && loaded) {
            float t = (ImGui::GetIO().MousePos.x - p0.x) / w;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            cur().seekSeconds(cur().durationSeconds() * static_cast<double>(t));
        }
        if (ImGui::IsItemDeactivated() && scrubWasPlaying) {
            cur().play();
            scrubWasPlaying = false;
        }

        // 再生位置。★いま鳴らしている音源のときだけ出す ──
        //   別の行を眺めている間に線が走ると、その音の位置だと勘違いします。
        if (loaded) {
            const float x = p0.x + w * cur().progress();
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(240, 190, 90, 255), 1.5f);
        }
        dl->AddRect(p0, p1, IM_COL32(58, 72, 92, 255));
    };

    std::wstring lastTitle;

    // ── 台帳の操作
    //
    // ★右クリックからも上のボタンからも**同じものを呼ぶ**。
    //   同じ操作を 2 通り書くと、片方だけ直して食い違います（決めごと #1）。
    af::UndoStack undo;

    // ★台帳とイベントを触るときは**必ずここを通す**。
    //   入口を 1 つにしてあるので、
    //     ① 元に戻すための写しを積む
    //     ② 未保存の印を立てる
    //   の 2 つを書き忘れようがありません。新しい操作を足すときもここを通すこと。
    //
    // ★台帳とイベントで分けていません。
    //   イベントは音源を番号で指しているので、**片方だけ戻すと辻褄が合いません**
    //   （台帳だけ戻すと「見つかりません」が生え、イベントだけ戻すと消した動作が復活する）。
    //   1 段に両方入れて、必ず一緒に動かします。
    auto edit = [&](const char* label, auto&& fn) {
        undo.push(label, proj.library, proj.events);
        fn();
        proj.touch();
    };

    char renameBuf[128] = {};
    int  renameEntry = -1;          // 音源の改名（-1 = していない）
    std::string renameFolderPath;   // フォルダの改名（空 = していない）
    bool renameFocus = false;

    // ★いま何を選んでいるか。
    //   音源・フォルダ・イベント・動作は同時に「選ばれている」状態になり得るので、
    //   **最後に押したものがどれか**を 1 つ持たないと、下の詳細が何を出すか決まりません。
    enum class Sel { None, Sound, Folder, Event, Action };
    Sel selKind = Sel::None;

    int selectedEvent  = -1;  // イベントの一覧で選んでいる行
    int selectedAction = -1;  // その中で選んでいる動作

    // ★波形の下に出すもの。★1 枚に両方並べると、どちらも幅が足りませんでした
    //   （「16 bit PCM」が切れ、つまみも短い）。切り替えにして横幅を丸ごと渡します。
    enum class PropView { Adjust, Info };
    PropView propView = PropView::Adjust;
    int renameEvent   = -1;   // イベントの改名（-1 = していない）

    auto startRenameEvent = [&](int i, const std::string& cur) {
        renameEvent = i;
        renameEntry = -1;
        renameFolderPath.clear();
        renameFocus = true;
        ::strncpy_s(renameBuf, cur.c_str(), sizeof(renameBuf) - 1);
    };

    // 元に戻す／やり直したあとの後始末。
    // ★選んでいた行や、改名中の対象が消えていることがある。
    //   放っておくと、無い行を指したまま操作できてしまいます。
    //
    // ⚠ イベントも戻るようになったので、**イベント側の選択も詰めます**。
    //   音源だけ見ていると、消えたイベントを選んだままプロパティが開き、
    //   そこで打った文字が別のイベントに入ります。
    auto afterUndo = [&] {
        // ★戻すと行番号が別の音源を指します。束ねが古い番号を掴んだままだと、
        //   別の音の上に位置の線が出ます。全部手放すのが安全。
        bus.releaseAll();
        if (selected >= 0 && static_cast<std::size_t>(selected) >= proj.library.size())
            selected = -1;
        if (selectedEvent >= 0 && static_cast<std::size_t>(selectedEvent) >= proj.events.size()) {
            selectedEvent  = -1;
            selectedAction = -1;
        }
        if (selectedEvent >= 0 && selectedAction >= 0) {
            const auto& acts = proj.events.all()[static_cast<std::size_t>(selectedEvent)].actions;
            if (static_cast<std::size_t>(selectedAction) >= acts.size()) selectedAction = -1;
        }
        // 指す先が消えたなら、下の詳細も何も出さない状態に戻す。
        if (selected < 0 && selectedEvent < 0) selKind = Sel::None;
        renameEntry = -1;
        renameFolderPath.clear();
        selFolder.clear();
        proj.touch();
    };

    auto startRename = [&](int ei, const std::string& cur) {
        renameEntry = ei;
        renameFolderPath.clear();
        renameFocus = true;
        ::strncpy_s(renameBuf, cur.c_str(), sizeof(renameBuf) - 1);
    };

    // フォルダの改名。持つのは**末尾の名前だけ**（親は動かさない）。
    auto startRenameFolder = [&](const std::string& full) {
        renameEntry = -1;
        renameFolderPath = full;
        renameFocus = true;
        const std::size_t s = full.find_last_of('/');
        ::strncpy_s(renameBuf, (s == std::string::npos ? full : full.substr(s + 1)).c_str(),
                    sizeof(renameBuf) - 1);
    };

    // 取り込み先のフォルダも受ける（右クリックした場所へ入る）。
    auto importInto = [&](const std::string& folder) {
        std::vector<std::wstring> picked;
        if (!pickWavs(hwnd, &picked)) return;
        int ok = 0;
        edit("取り込み", [&] {
            for (const auto& w : picked) {
                std::string e;
                if (proj.library.add(w, folder, &e)) ++ok;
                else say("取り込めません（" + af::wideToUtf8(w) + "）── " + e);
            }
        });
        if (ok > 0) say("取り込みました : " + std::to_string(ok) + " 本");
    };

    // 親の下に「新しいフォルダ」を作る。名前が重なったら連番。
    auto newFolder = [&](const std::string& parent) {
        const std::string base = parent.empty() ? "新しいフォルダ" : parent + "/新しいフォルダ";
        std::string p = base;
        for (int n = 2; n < 1000; ++n) {
            bool dup = false;
            for (const std::string& f : proj.library.folders())
                if (f == p) { dup = true; break; }
            if (!dup) break;
            p = base + " " + std::to_string(n);
        }
        edit("フォルダを作る", [&] { proj.library.addFolder(p); });
        startRenameFolder(p);
    };

    // ── 掴んで運ぶ（ドラッグ）
    //
    // ★運ぶものは 1 種類の荷札にまとめる。音源とフォルダで別の荷札にすると、
    //   落とす側が両方に対応することになって、条件が二重になります。
    //   entry >= 0 なら音源、entry < 0 ならフォルダ（folder に道筋）。
    struct DragItem {
        int  entry;
        char folder[256];
    };

    auto dropInto = [&](const std::string& dstFolder, const DragItem& it) {
        if (it.entry >= 0) {
            if (static_cast<std::size_t>(it.entry) >= proj.library.size()) return;
            edit("移動", [&] {
                proj.library.setFolder(static_cast<std::size_t>(it.entry), dstFolder);
            });
            return;
        }

        const std::string from = it.folder;
        if (from.empty()) return;

        // 自分自身・自分の子の中へは落とせない（入れ子が自分を含んでしまう）。
        if (dstFolder == from ||
            (dstFolder.size() > from.size() &&
             dstFolder.compare(0, from.size(), from) == 0 && dstFolder[from.size()] == '/')) {
            say("そのフォルダの中へは移せません");
            return;
        }

        // ★移動は「親を差し替えた改名」で済む。renameFolder が入れ子ごと連れて行く。
        //   同じことを 2 通り書かないため、専用の移動関数は作っていません。
        const std::size_t s = from.find_last_of('/');
        const std::string leaf = (s == std::string::npos) ? from : from.substr(s + 1);
        const std::string to = dstFolder.empty() ? leaf : dstFolder + "/" + leaf;
        edit("フォルダの移動", [&] { proj.library.renameFolder(from, to); });
        selFolder = to;
    };

    auto acceptDrop = [&](const std::string& dstFolder) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AF_ITEM")) {
            DragItem it{};
            std::memcpy(&it, p->Data, sizeof(it));
            dropInto(dstFolder, it);
        }
        ImGui::EndDragDropTarget();
    };

    // ── ツリーの中身（フォルダと音源）を描く。フォルダは入れ子になるので再帰。
    //
    // ★フォルダは**並べ方だけ**で、音には何も影響しません。
    //   性質で分けたいときの仕切りです。だから消しても中身は消えず、ひとつ上へ移ります。
    std::function<void(const std::string&)> drawContents = [&](const std::string& prefix) {
        af::SoundLibrary& lib = proj.library;

        // ── 子フォルダ
        for (const std::string& f : lib.childFolders(prefix)) {
            const std::size_t s = f.find_last_of('/');
            const std::string leaf = (s == std::string::npos) ? f : f.substr(s + 1);
            const bool renamingF = (renameFolderPath == f);

            ImGui::PushID(f.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            ImGuiTreeNodeFlags ff = ImGuiTreeNodeFlags_OpenOnArrow |
                                    ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                    ImGuiTreeNodeFlags_SpanAllColumns |
                                    ImGuiTreeNodeFlags_DefaultOpen;
            if (selected < 0 && selFolder == f) ff |= ImGuiTreeNodeFlags_Selected;

            // ★節点は必ず作る。名前を変えている最中は見出しを空にして横に入力欄。
            //   「変更中は作らない」にすると TreePop() と対にならず ImGui に怒られる。
            const bool fopen = ImGui::TreeNodeEx("##fd", ff, "%s",
                                                 renamingF ? "" : ("[" + leaf + "]").c_str());

            if (renamingF) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                if (renameFocus) { ImGui::SetKeyboardFocusHere(); renameFocus = false; }
                if (ImGui::InputText("##rnf", renameBuf, sizeof(renameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
                    ImGui::IsItemDeactivated()) {
                    if (renameBuf[0]) {
                        const std::string parent =
                            (s == std::string::npos) ? std::string() : f.substr(0, s);
                        edit("フォルダの名前", [&] {
                            lib.renameFolder(f, parent.empty() ? std::string(renameBuf)
                                                               : parent + "/" + renameBuf);
                        });
                    }
                    renameFolderPath.clear();
                }
            } else {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                    DragItem it{-1, {}};
                    ::strncpy_s(it.folder, f.c_str(), sizeof(it.folder) - 1);
                    ImGui::SetDragDropPayload("AF_ITEM", &it, sizeof(it));
                    ImGui::Text("[%s]", leaf.c_str());
                    ImGui::EndDragDropSource();
                }
                acceptDrop(f);

                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    selected = -1;
                    selFolder = f;
                    selKind = Sel::Folder;
                }
                if (ImGui::BeginPopupContextItem("fdmenu")) {
                    selFolder = f;
                    if (ImGui::MenuItem("フォルダを作る（この中に）")) newFolder(f);
                    if (ImGui::MenuItem("WAV を取り込む…")) importInto(f);
                    ImGui::Separator();
                    if (ImGui::MenuItem("名前の変更", "F2")) startRenameFolder(f);
                    if (ImGui::MenuItem("削除（中身はひとつ上へ）")) {
                        edit("フォルダの削除", [&] { lib.removeFolder(f); });
                        selFolder.clear();
                    }
                    ImGui::EndPopup();
                }
            }

            if (fopen) { drawContents(f); ImGui::TreePop(); }
            ImGui::PopID();
        }

        // ── このフォルダ直下の音源
        for (int ei = 0; ei < static_cast<int>(lib.size()); ++ei) {
            const auto& e = lib.entries()[static_cast<std::size_t>(ei)];
            if (e.folder != prefix) continue;

            const bool renamingEntry = (renameEntry == ei);
            ImGui::PushID(ei);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            ImGuiTreeNodeFlags lf = ImGuiTreeNodeFlags_Leaf |
                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                    ImGuiTreeNodeFlags_SpanAllColumns;
            if (selected == ei) lf |= ImGuiTreeNodeFlags_Selected;

            // 読めない素材は名前を赤で出す（列は名前だけなので、ここが唯一の合図）。
            if (!e.resolved)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            ImGui::TreeNodeEx("##e", lf, "%s%s", e.resolved ? "" : "× ",
                              renamingEntry ? "" : e.name.c_str());
            if (!e.resolved) ImGui::PopStyleColor();

            if (renamingEntry) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                if (renameFocus) { ImGui::SetKeyboardFocusHere(); renameFocus = false; }
                if (ImGui::InputText("##rne", renameBuf, sizeof(renameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
                    ImGui::IsItemDeactivated()) {
                    if (renameBuf[0])
                        edit("名前の変更", [&] { lib.rename(static_cast<std::size_t>(ei), renameBuf); });
                    renameEntry = -1;
                }
            } else {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                    DragItem it{ei, {}};
                    ImGui::SetDragDropPayload("AF_ITEM", &it, sizeof(it));
                    ImGui::TextUnformatted(e.name.c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::IsItemClicked()) {
                    selected = ei; selFolder.clear(); selKind = Sel::Sound;
                }
                if (ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    selected = ei;
                    firedEvent.clear();   // イベント経由ではないので見出しを戻す
                    auditionClip(ei);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s%s%s", af::wideToUtf8(e.path).c_str(),
                                      e.resolved ? "" : "\n",
                                      e.resolved ? "" : e.problem.c_str());

                if (ImGui::BeginPopupContextItem("entmenu")) {
                    selected = ei;
                    if (ImGui::MenuItem("試聴", nullptr, false, e.resolved)) {
                        firedEvent.clear();
                        auditionClip(ei);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("名前の変更", "F2")) startRename(ei, e.name);
                    if (ImGui::BeginMenu("フォルダへ移動")) {
                        if (ImGui::MenuItem("（一番上）", nullptr, e.folder.empty()))
                            edit("移動", [&] {
                                lib.setFolder(static_cast<std::size_t>(ei), std::string());
                            });
                        for (const std::string& f : lib.folders())
                            if (ImGui::MenuItem(f.c_str(), nullptr, e.folder == f))
                                edit("移動", [&] { lib.setFolder(static_cast<std::size_t>(ei), f); });
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("エクスプローラで表示", nullptr, false, e.resolved))
                        revealInExplorer(lib.absolutePathOf(static_cast<std::size_t>(ei)));
                    if (ImGui::MenuItem("引き直す")) lib.refresh();
                    ImGui::Separator();
                    if (ImGui::MenuItem("台帳から外す", "Del")) {
                        edit("外す", [&] { lib.remove(static_cast<std::size_t>(ei)); });
                        selected = -1;
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
    };

    // ── ファイル操作
    //
    // ★保存はファイル 1 つ。台帳もこの中に入っています。
    //   バンクを別ファイルにしていた頃は「バンクだけ保存し忘れる」が起き得ましたが、
    //   畳んだので、その心配ごと自体が消えました。
    auto saveProjectTo = [&](const std::wstring& p) -> bool {
        std::string e;
        if (!proj.save(p, &e)) { say("保存できません ── " + e); return false; }
        char b[64];
        ::sprintf_s(b, "（音源 %d 本）", static_cast<int>(proj.library.size()));
        say("保存しました : " + af::wideToUtf8(p) + b);
        return true;
    };

    auto doSaveAs = [&]() -> bool {
        std::wstring p = proj.path;
        if (!pickFile(hwnd, true, &p)) return false;
        return saveProjectTo(p);
    };
    auto doSave = [&]() -> bool {
        if (proj.path.empty()) return doSaveAs();
        return saveProjectTo(proj.path);
    };
    // 保存していない変更があれば聞く。続けてよければ true。
    auto confirmDiscard = [&]() -> bool {
        if (!proj.dirty) return true;
        const int r = ::MessageBoxW(hwnd, L"変更が保存されていません。保存しますか？",
                                    L"AfHost", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES)    return doSave();
        return true;
    };
    auto doNew = [&] {
        if (!confirmDiscard()) return;
        auditionTone(false);          // 取り込んだ音を鳴らしたままにしない
        selected = -1;
        proj = af::Project{};
        bus.releaseAll();      // ★台帳が変わると行番号が別物を指す。全部手放す
        undo.reset();          // 前のプロジェクトの段は持ち越さない
        applyProject();
        say("新しいプロジェクトを作りました");
    };
    auto doOpen = [&] {
        if (!confirmDiscard()) return;
        std::wstring p;
        if (!pickFile(hwnd, false, &p)) return;
        std::string e;
        if (!proj.load(p, &e)) { say("開けません ── " + e); return; }
        bus.releaseAll();      // ★同上
        undo.reset();          // 開いた直後を土台にする
        applyProject();
        selected = -1;
        auditionTone(proj.toneOnOpen);
        char b[64];
        ::sprintf_s(b, "（音源 %d 本）", static_cast<int>(proj.library.size()));
        say("開きました : " + af::wideToUtf8(p) + b);
    };

    // ── 主ループ
    bool done  = false;
    int  frame = 0;
    while (!done) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_needResize) {
            releaseBackBuffer();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            createBackBuffer();
            g_needResize = false;
        }

        // ★終わった検査スレッドは、ここで回収する。
        //   回収を「回すボタン」より後に置くと、joinable なままの std::thread へ
        //   代入することになり std::terminate で落ちる。順序に意味がある。
        if (!testRunning.load() && testThread.joinable()) {
            testThread.join();
            haveReport = true;
            std::string e;
            if (!device.start(&tone, &e)) say("検査のあと開き直せません ── " + e);
            tone.setActive(playing);
        }

        // 題名は「プロジェクト名 — AfHost」。変わったときだけ差し替える。
        {
            const std::wstring t = af::utf8ToWide(proj.titleLine()) + L" — AfHost";
            if (t != lastTitle) { ::SetWindowTextW(hwnd, t.c_str()); lastTitle = t; }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ── メニューバー
        if (ImGui::BeginMainMenuBar()) {
            // 資料の図を撮るとき、編集メニューを押さずに開く。
            // ★狙いは「イベントも戻せる」ことの**目に見える証拠**です。
            //   仕組みが動くかの検査は通っても、**辿り着けるか**は別で、
            //   過去 2 回そこで漏らしています。
            if (evEditShot && frame == 3) ImGui::OpenPopup("編集");
            if (ImGui::BeginMenu("ファイル")) {
                if (ImGui::MenuItem("新規"))              doNew();
                if (ImGui::MenuItem("開く…"))             doOpen();
                ImGui::Separator();
                if (ImGui::MenuItem("保存"))              doSave();
                if (ImGui::MenuItem("名前を付けて保存…")) doSaveAs();
                ImGui::Separator();
                if (ImGui::MenuItem("終了")) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                ImGui::EndMenu();
            }
            // ── 編集（元に戻す／やり直す）
            if (ImGui::BeginMenu("編集")) {
                const std::string ul =
                    undo.canUndo() ? "元に戻す：" + undo.undoLabel() : std::string("元に戻す");
                const std::string rl =
                    undo.canRedo() ? "やり直す：" + undo.redoLabel() : std::string("やり直す");
                if (ImGui::MenuItem(ul.c_str(), "Ctrl+Z", false, undo.canUndo())) {
                    if (undo.undo(&proj.library, &proj.events)) { afterUndo(); say("元に戻しました"); }
                }
                if (ImGui::MenuItem(rl.c_str(), "Ctrl+Y", false, undo.canRedo())) {
                    if (undo.redo(&proj.library, &proj.events)) { afterUndo(); say("やり直しました"); }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("表示")) {
                ImGui::MenuItem("プロジェクト", nullptr, &showProject);
                ImGui::MenuItem("SoundLibrary", nullptr, &showLibrary);
                ImGui::MenuItem("コントロールパネル", nullptr, &showControl);
                ImGui::MenuItem("イベント",     nullptr, &showEvents);
                ImGui::MenuItem("プロパティ",   nullptr, &showProps);
                ImGui::MenuItem("試験信号",     nullptr, &showTone);
                ImGui::MenuItem("出力",         nullptr, &showOutput);
                ImGui::MenuItem("ログ",         nullptr, &showLog);
                ImGui::Separator();
                ImGui::MenuItem("自己検査",     nullptr, &showSelfTest);
                ImGui::MenuItem("モジュール",   nullptr, &showModule);
                ImGui::Separator();
                if (ImGui::MenuItem("配置を既定に戻す")) resetLayout = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("ヘルプ")) {
                ImGui::TextUnformatted(
                    "AcousticFlow の開発環境です。\n"
                    "音響エンジンは載っていません ── このアプリはエンジンに\n"
                    "リンクしておらず、エンジン抜きで単体で立ちます。");
                ImGui::EndMenu();
            }

            // 右端に状態を出す
            const char* state = playing ? "試聴中" : "停止";
            const float w = ImGui::CalcTextSize(state).x + 24.0f;
            ImGui::SameLine(ImGui::GetWindowWidth() - w);
            ImGui::TextColored(playing ? ImVec4(0.4f, 0.9f, 0.6f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                               "%s", state);
            ImGui::EndMainMenuBar();
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();

        // ── ツールバー（試聴の操作。ここが作業の入口）
        {
            const float h = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
            if (ImGui::BeginViewportSideBar("##toolbar", vp, ImGuiDir_Up, h,
                                            ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoSavedSettings)) {
                if (wired == Playing::Clip) {
                    // 取り込んだ音を鳴らしている最中。ここは止めるだけ。
                    if (ImGui::Button("停止", ImVec2(70, 0))) auditionTone(false);
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    ImGui::TextUnformatted("取り込んだ音を試聴中");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(200);
                    bar(cur().progress(), ImVec4(0.55f, 0.75f, 0.45f, 1.0f), "");
                } else {
                    if (ImGui::Button(playing ? "停止" : "試聴", ImVec2(70, 0)))
                        auditionTone(!playing);
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();

                    // ★つまみはコントロールパネルにだけ置く。
                    //   同じ値を 2 箇所から動かせると、どちらが正か分からなくなる
                    //   （決めごと #1）。ここは「いまの値」を読むだけ。
                    ImGui::Text("試験信号 %.0f Hz / %.3f", proj.toneHz, proj.toneLevel);

                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    ImGui::TextUnformatted("山");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(170);
                    bar(meter, ImVec4(0.30f, 0.72f, 0.95f, 1.0f), "");
                }
            }
            ImGui::End();
        }

        // ── ステータスバー（いつも見えている数字はここへ集める）
        {
            // 中身は文字 1 行だけ。窓の余白ぶんを足さないと下が切れる（切れた）。
            const float h = ImGui::GetTextLineHeight() + style.WindowPadding.y * 2.0f;
            if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, h,
                                            ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoSavedSettings)) {
                if (device.running()) {
                    const int fpc = device.framesPerCallback();
                    ImGui::Text("%s  |  %d Hz / %d ch  |  1 回 %d サンプル（%.2f ms）"
                                "  |  最悪 %.4f ms ＝ 持ち時間の %.3f %%",
                                device.deviceName().c_str(), device.sampleRate(),
                                device.channels(), fpc,
                                fpc > 0 ? 1000.0 * fpc / device.sampleRate() : 0.0,
                                device.worstBlockMs(), device.worstLoadPercent());
                    ImGui::SameLine();
                    const unsigned long long late = device.lateBlocks();
                    ImGui::TextColored(late == 0 ? ImVec4(0.4f, 0.9f, 0.6f, 1)
                                                 : ImVec4(1.0f, 0.5f, 0.4f, 1),
                                       "  |  間に合わなかった回 %llu", late);
                } else {
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "出力が開いていません");
                }
            }
            ImGui::End();
        }

        const ImGuiID dockId = ImGui::GetID("AfHostDockSpace");
        ImGui::DockSpaceOverViewport(dockId, vp);

        // ── 既定の配置（初回、または「配置を既定に戻す」）
        if (!layoutDone || resetLayout) {
            // ⚠ 覚えた配置を捨てるときは黙ってやらないこと。
            //   自分で組んだ配置が消えた側からは、**壊れたのか作り直されたのか
            //   区別が付きません**。理由を言ってから捨てます。
            if (hadIni && layoutStale) {
                say("配置を既定に組み直しました ── 窓の構成が変わったためです"
                    "（自分で組んだ配置は、この 1 回だけ消えます）");
                layoutStale = false;
            }
            layoutDone  = true;
            resetLayout = false;

            // ⚠ 分割の戻り値と「反対側」の両方を受け取ること。
            //   反対側に nullptr を渡すと元の id が親になり、そこへ窓を置いても効かない。
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);

            // 左＝台帳と操作、真ん中＝選んだものの中身（音データを含む）、右＝機器、下＝記録。
            // ★見る場所（左上）／触る場所（左下）／整える場所（真ん中）／機器（右）で分けている。
            ImGuiID center = dockId;
            ImGuiID leftTop = 0;
            const ImGuiID left    = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.30f,
                                                                nullptr, &center);
            const ImGuiID right   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f,
                                                                nullptr, &center);
            // ★下段は 0.40 → 0.26 に詰めた。記録と診断しか無くなったので、
            //   真ん中（音データ＋調整）に縦を回すほうが効きます。数字入力を足して
            //   1 項目が 2 行になったので 0.26 → 0.20 へ。
            const ImGuiID bottom  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.18f,
                                                                nullptr, &center);
            const ImGuiID leftLow = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f,
                                                                nullptr, &leftTop);

            ImGui::DockBuilderDockWindow("SoundLibrary", leftTop);
            ImGui::DockBuilderDockWindow("イベント",     leftTop);
            ImGui::DockBuilderDockWindow("プロジェクト", leftTop);
            ImGui::DockBuilderDockWindow("コントロールパネル", leftLow);
            ImGui::DockBuilderDockWindow("試験信号",    right);
            ImGui::DockBuilderDockWindow("出力",        right);
            // ★真ん中は「選んでいるものを見て直す場所」。音データもここに出る。
            ImGui::DockBuilderDockWindow("プロパティ",   center);
            // 下段は記録と診断。
            ImGui::DockBuilderDockWindow("ログ",         bottom);
            ImGui::DockBuilderDockWindow("自己検査",     bottom);
            ImGui::DockBuilderDockWindow("モジュール",   bottom);
            ImGui::DockBuilderFinish(dockId);
        }


        // ★鳴り切っても試験信号へは戻しません。
        //   戻すとデバイスを開け直すことになり、操作盤の帯と残り秒数が消えます。
        //   ClipSource が自分で止まるので、載せたままにしておくのが正しい
        //   （もう一度 ▶ を押せば、そのまま頭から鳴ります）。

        // ── キーボード（既存のオーサリングツールと同じ割り当て）
        // ⚠ 文字を打っている最中は効かせない。名前の変更で "d" を打った瞬間に
        //   消えたら困る。
        // 元に戻す／やり直す。文字を打っている最中は効かせない。
        if (!ImGui::GetIO().WantTextInput) {
            const bool ctrl = ImGui::GetIO().KeyCtrl;
            const bool shift = ImGui::GetIO().KeyShift;
            if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (undo.undo(&proj.library, &proj.events)) { afterUndo(); say("元に戻しました"); }
            }
            if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) ||
                         (shift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
                if (undo.redo(&proj.library, &proj.events)) { afterUndo(); say("やり直しました"); }
            }

            // スペース ── 送りの入切（DAW や Wwise と同じ割り当て）。
            //
            // ⚠ 文字を打っている最中は効かせない。**名前に空白が入れられなくなります**
            //   （上の `WantTextInput` の中に置いてあるのはそのため）。
            // ⚠ つまみや帯を掴んでいる間も効かせない。掴んだまま押すと、
            //   動かしている最中に止まって、掴み直すことになります。
            if (!ctrl && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Space))
                transportToggle();
        }

        if (!ImGui::GetIO().WantTextInput && renameEntry < 0 && renameFolderPath.empty()) {
            af::SoundLibrary& kb = proj.library;
            const bool hasEntry = selected >= 0 && static_cast<std::size_t>(selected) < kb.size();

            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                if (hasEntry)
                    startRename(selected, kb.entries()[static_cast<std::size_t>(selected)].name);
                else if (!selFolder.empty())
                    startRenameFolder(selFolder);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && hasEntry) {
                edit("外す", [&] { kb.remove(static_cast<std::size_t>(selected)); });
                selected = -1;
                say("台帳から外しました（ファイルは消していません）");
            }
        }

        // 実時間スレッドが公開した波形と山を受け取る
        {
            const int n = device.copyScope(scope.data(), static_cast<int>(scope.size()));
            if (n > 0) scopeN = n;
            const float p = device.peak();
            if (p > meter) meter = p;
            else           meter *= 0.90f;      // 1 フレームあたり約 -0.9 dB で落とす
        }

        // ── プロジェクト
        if (showProject) {
            if (ImGui::Begin("プロジェクト", &showProject)) {
                char buf[128] = {};
                ::strncpy_s(buf, proj.name.c_str(), sizeof(buf) - 1);
                ImGui::TextUnformatted("名前");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##name", buf, sizeof(buf))) {
                    proj.name = buf;
                    proj.dirty = true;
                }

                ImGui::Spacing();
                ImGui::TextUnformatted("保存先");
                ImGui::TextWrapped("%s", proj.path.empty()
                                             ? "（未保存）"
                                             : af::wideToUtf8(proj.path).c_str());

                ImGui::Separator();
                ImGui::TextUnformatted("音源");
                ImGui::BulletText("%d 本 ／ フォルダ %d 個",
                                  static_cast<int>(proj.library.size()),
                                  static_cast<int>(proj.library.folders().size()));
                ImGui::TextDisabled("台帳は SoundLibrary、試聴の操作はコントロールパネル");

                ImGui::Separator();
                ImGui::TextWrapped(
                    "★いま持っている設定は、本当に効くものだけです。"
                    "バスや RTPC のような枠を先に並べると、観測する対象が無いまま"
                    "中身の空いた機能が増えます。焼いた資産を持つのは、"
                    "焼く道具が出来てからです。");
            }
            ImGui::End();
        }

        // ── SoundLibrary（プロジェクトが持つ音源の台帳）
        //
        // ★2026-08-30、バンクという器を廃してここへ畳みました。
        //   容れ物としての働きがフォルダと完全に重複していて、バンクが余分に
        //   持っていたのは「別ファイルであること」だけだったためです。
        //   触り方は既存のオーサリングツールに合わせています
        //   ── ツリー ＋ 右クリック ＋ F2 ＋ Del ＋ 掴んで運ぶ ＋ 放り込み。
        if (showLibrary) {
            if (ImGui::Begin("SoundLibrary", &showLibrary)) {
                // 落としこまれた WAV を引き取る（窓の手続きが積んでいる）。
                if (!g_dropped.empty()) {
                    int ok = 0;
                    edit("取り込み", [&] {
                        for (const auto& w : g_dropped) {
                            std::string e;
                            // 選んでいるフォルダがあればそこへ、無ければ一番上へ。
                            if (proj.library.add(w, selFolder, &e)) ++ok;
                            else say("取り込めません（" + af::wideToUtf8(w) + "）── " + e);
                        }
                    });
                    if (ok > 0)
                        say("放り込まれた " + std::to_string(ok) + " 本を取り込みました");
                    g_dropped.clear();
                }

                // ★表に ScrollY を付けない。
                //   付けると表が**別の窓**になり、その中で右クリックしても
                //   親の窓のメニューが出ません（空き場所の右クリックが効かなくなる）。
                //   代わりに SoundLibrary の窓ごと縦に流れます。
                const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("tree", 1, tf)) {
                    // ★列は名前だけ。素性は右の Audio タブが持つ。
                    //   同じ数字を 2 箇所に出すと、片方だけ直して食い違う（決めごと #1）。
                    ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch);
                    drawContents(std::string());
                    ImGui::EndTable();
                }

                // 一番上（フォルダの外）へ落とすための受け皿。
                // ★表の外側にも落とし先が要る。無いと、いったんフォルダへ入れた音源を
                //   一番上へ戻す手が右クリックしか無くなります。
                acceptDrop(std::string());

                if (proj.library.size() == 0 && proj.library.folders().empty()) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("まだ何も入っていません。");
                    ImGui::TextDisabled("空いている所で右クリック、または WAV を放り込んでください。");
                }

                // ── 空いている所の右クリック
                //
                // ★上のボタンは無くしました。操作の入口を右クリックに一本化しています
                //   （ボタンの列だと、項目が増えたときに置き場所が無くなる）。
                //
                // ⚠ ここを埋めてはいけません。`Dummy` などで場所を取ると、それ自体が
                //   「項目」になり、`NoOpenOverItems` に引っかかって**逆に出なくなります**。
                //   窓の空白は、何も置かないままで当たり判定になります。
                // 資料の図を撮るとき、右クリックを押さずにメニューを開く。
                if (showEmptyMenu && frame >= 3) {
                    ImGui::OpenPopup("emptymenu");
                    showEmptyMenu = false;
                }

                if (ImGui::BeginPopupContextWindow("emptymenu",
                                                   ImGuiPopupFlags_MouseButtonRight |
                                                   ImGuiPopupFlags_NoOpenOverItems)) {
                    selFolder.clear();
                    if (ImGui::MenuItem("WAV を取り込む…")) importInto(std::string());
                    if (ImGui::MenuItem("フォルダを作る"))   newFolder(std::string());
                    ImGui::EndPopup();
                }

                ImGui::TextDisabled("右クリックで操作 ／ 掴んで運べます ／ "
                                    "F2 名前の変更 ／ Del 外す ／ 二度押しで試聴");
            }
            ImGui::End();
        }

        // ── コントロールパネル（選んだものを操作する場所）
        //
        // ★操作はここに集める。台帳（SoundLibrary）は見る場所、ここは触る場所。
        //   ⚠ ここも「音の加工」は持ちません。持つのは**鳴らし方**だけです。
        if (showControl) {
            if (ImGui::Begin("コントロールパネル", &showControl)) {
                af::SoundLibrary& lib = proj.library;
                const bool hasSel =
                    selected >= 0 && static_cast<std::size_t>(selected) < lib.size();

                // ── 操作盤
                //
                // ★何を鳴らしているかを、いちばん上に大きく出す。
                //   ボタンだけ並んでいても「どれを触っているか」が分からない。
                {
                    const bool loaded = (wired == Playing::Clip) && !cur().empty();
                    const auto st = cur().state();

                    // 見出し: イベント名（発火したとき）／プロジェクト名 › 音源名
                    if (hasSel) {
                        const auto& e = lib.entries()[static_cast<std::size_t>(selected)];
                        // ★スケッチにあった「イベント名」の枠が、ここで埋まりました。
                        //   イベントから鳴らしたときはその名前、直接鳴らしたときは
                        //   プロジェクト名を出します。
                        if (!firedEvent.empty())
                            ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s",
                                               firedEvent.c_str());
                        else
                            ImGui::TextDisabled("%s", proj.name.c_str());
                        // 音源名はここで変えられる（名前を変える口はここ 1 つ）。
                        char nb[128] = {};
                        ::strncpy_s(nb, e.name.c_str(), sizeof(nb) - 1);
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##entryname", nb, sizeof(nb)))
                            lib.rename(static_cast<std::size_t>(selected), nb);
                    } else {
                        ImGui::TextDisabled("（未選択）");
                        ImGui::TextDisabled("SoundLibrary で 1 つ選んでください");
                    }

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // 停止・一時停止・再生。中央に寄せる。
                    const float bw = 42.0f, gap = 22.0f;
                    const float row = bw * 3 + gap * 2;
                    const float avail = ImGui::GetContentRegionAvail().x;
                    if (avail > row) ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                                          (avail - row) * 0.5f);

                    const bool canStop  = loaded && st != af::ClipSource::State::Stopped;
                    const bool canPause = loaded && st == af::ClipSource::State::Playing;
                    const bool canPlay  = hasSel &&
                                          lib.entries()[static_cast<std::size_t>(selected)]
                                              .resolved;

                    if (transportButton("stop", Glyph::Stop, canStop, false, bw) && canStop)
                        cur().stop();
                    ImGui::SameLine(0.0f, gap);
                    if (transportButton("pause", Glyph::Pause, canPause,
                                        st == af::ClipSource::State::Paused, bw) && canPause)
                        cur().pause();
                    ImGui::SameLine(0.0f, gap);
                    if (transportButton("play", Glyph::Play, canPlay,
                                        st == af::ClipSource::State::Playing, bw) && canPlay)
                        transportPlay();   // ★スペースと同じものを呼ぶ

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // ★頭出しの帯は 2026-09-01 に廃しました。
                    //   プロパティの**音データを直接掴む**形に移しています ──
                    //   見ている場所と掴む場所が同じほうが、どこへ飛ぶか分かるため。
                    //   ⚠ 両方に置くと同じ操作が 2 箇所になります（決めごと #1）。
                    ImGui::TextDisabled("%s / %s",
                                        durationText(loaded ? cur().positionSeconds() : 0.0).c_str(),
                                        durationText(loaded ? cur().durationSeconds() : 0.0).c_str());

                    // ★キーは押してみないと分かりません。ここに出しておきます。
                    //   仕組みが動くことと、辿り着けることは別です。
                    ImGui::TextDisabled("スペースで 再生／一時停止（■ で頭へ戻す）");
                    ImGui::TextDisabled("頭出しは音データを掴んで");

                    // ★素性はここに出さない。プロパティが持つ。
                    //   同じ数字を 2 箇所に出すと、片方だけ直して食い違います。
                    //   ここは読めない時だけ、鳴らせない理由を出す。
                    if (hasSel) {
                        const auto& e = lib.entries()[static_cast<std::size_t>(selected)];
                        if (!e.resolved)
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", e.problem.c_str());
                    }
                }
            }
            ImGui::End();
        }

        // ── イベント（ゲーム側からの発火点）
        //
        // ★ゲーム側は `足音_石` のような**名前**だけを知っていて、
        //   どの音源が鳴るか・音量がいくつかは知りません。差し替えても
        //   ゲーム側のコードは 1 行も変わらない ── これがイベントの意味です。
        if (showEvents) {
            if (ImGui::Begin("イベント", &showEvents)) {
                af::EventList& ev = proj.events;

                const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("evtree", 1, tf)) {
                    ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch);

                    for (int i = 0; i < static_cast<int>(ev.size()); ++i) {
                        const af::Event& e = ev.all()[static_cast<std::size_t>(i)];
                        const bool renaming = (renameEvent == i);

                        ImGui::PushID(i);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);

                        ImGuiTreeNodeFlags nf = ImGuiTreeNodeFlags_OpenOnArrow |
                                                ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                ImGuiTreeNodeFlags_SpanAllColumns |
                                                ImGuiTreeNodeFlags_DefaultOpen;
                        if (selectedEvent == i) nf |= ImGuiTreeNodeFlags_Selected;

                        char lbl[192] = {};
                        if (!renaming)
                            ::sprintf_s(lbl, "%s（%d）", e.name.c_str(),
                                        static_cast<int>(e.actions.size()));
                        const bool open = ImGui::TreeNodeEx("##ev", nf, "%s", lbl);

                        if (renaming) {
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(-1);
                            if (renameFocus) { ImGui::SetKeyboardFocusHere(); renameFocus = false; }
                            if (ImGui::InputText("##rnev", renameBuf, sizeof(renameBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue) ||
                                ImGui::IsItemDeactivated()) {
                                if (renameBuf[0])
                                    edit("イベントの名前", [&] {
                                        ev.rename(static_cast<std::size_t>(i), renameBuf);
                                    });
                                renameEvent = -1;
                            }
                        } else {
                            // ★SoundLibrary から音源を掴んで落とすと「再生」の動作が付く。
                            //   一覧を行き来せずに組めるので、ここがいちばん速い道。
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* p =
                                        ImGui::AcceptDragDropPayload("AF_ITEM")) {
                                    DragItem it{};
                                    std::memcpy(&it, p->Data, sizeof(it));
                                    if (it.entry >= 0 &&
                                        static_cast<std::size_t>(it.entry) < proj.library.size()) {
                                        const unsigned sid =
                                            proj.library.entries()[
                                                static_cast<std::size_t>(it.entry)].id;
                                        edit("動作を足す", [&] {
                                            ev.addAction(static_cast<std::size_t>(i),
                                                         af::ActionType::Play, sid);
                                        });
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }

                            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                                selectedEvent = i;
                                selectedAction = -1;
                                selKind = Sel::Event;
                            }
                            if (ImGui::IsItemHovered() &&
                                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                selectedEvent = i;
                                fireEvent(i);
                            }

                            if (ImGui::BeginPopupContextItem("evmenu")) {
                                selectedEvent = i;
                                if (ImGui::MenuItem("発火", nullptr, false, !e.actions.empty()))
                                    fireEvent(i);
                                ImGui::Separator();
                                if (ImGui::MenuItem("名前の変更", "F2"))
                                    startRenameEvent(i, e.name);
                                if (ImGui::BeginMenu("動作を足す")) {
                                    for (int t = 0; t < 2; ++t) {
                                        const af::ActionType at = (t == 0) ? af::ActionType::Play
                                                                           : af::ActionType::Stop;
                                        if (ImGui::BeginMenu(af::actionTypeName(at))) {
                                            for (std::size_t s = 0; s < proj.library.size(); ++s) {
                                                const auto& se = proj.library.entries()[s];
                                                if (ImGui::MenuItem(se.name.c_str())) {
                                                    const unsigned sid = se.id;
                                                    edit("動作を足す", [&] {
                                                        ev.addAction(
                                                            static_cast<std::size_t>(i), at, sid);
                                                    });
                                                }
                                            }
                                            if (proj.library.size() == 0)
                                                ImGui::TextDisabled("音源がありません");
                                            ImGui::EndMenu();
                                        }
                                    }
                                    ImGui::EndMenu();
                                }
                                ImGui::Separator();
                                if (ImGui::MenuItem("イベントを消す", "Del")) {
                                    edit("イベントを消す", [&] {
                                        ev.remove(static_cast<std::size_t>(i));
                                    });
                                    selectedEvent = -1;
                                }
                                ImGui::EndPopup();
                            }
                        }

                        if (open) {
                            for (int k = 0; k < static_cast<int>(e.actions.size()); ++k) {
                                const af::EventAction& a = e.actions[static_cast<std::size_t>(k)];
                                const int si = proj.library.indexOfId(a.soundId);
                                const bool lost = (si < 0);

                                ImGui::PushID(k + 1000);
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);

                                ImGuiTreeNodeFlags af_ = ImGuiTreeNodeFlags_Leaf |
                                                         ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                         ImGuiTreeNodeFlags_SpanAllColumns;
                                if (selKind == Sel::Action && selectedEvent == i &&
                                    selectedAction == k)
                                    af_ |= ImGuiTreeNodeFlags_Selected;

                                if (lost)
                                    ImGui::PushStyleColor(ImGuiCol_Text,
                                                          ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                                ImGui::TreeNodeEx(
                                    "##a", af_,
                                    "%s → %s", af::actionTypeName(a.type),
                                    lost ? "× 見つかりません"
                                         : proj.library.entries()[static_cast<std::size_t>(si)]
                                               .name.c_str());
                                if (lost) ImGui::PopStyleColor();

                                if (ImGui::IsItemClicked()) {
                                    selectedEvent  = i;
                                    selectedAction = k;
                                    selKind = Sel::Action;
                                }

                                if (ImGui::BeginPopupContextItem("acmenu")) {
                                    if (ImGui::BeginMenu("種類")) {
                                        for (int t = 0; t < 2; ++t) {
                                            const af::ActionType at =
                                                (t == 0) ? af::ActionType::Play
                                                         : af::ActionType::Stop;
                                            if (ImGui::MenuItem(af::actionTypeName(at), nullptr,
                                                                a.type == at))
                                                edit("動作の種類", [&] {
                                                    ev.setActionType(static_cast<std::size_t>(i),
                                                                     static_cast<std::size_t>(k),
                                                                     at);
                                                });
                                        }
                                        ImGui::EndMenu();
                                    }
                                    if (ImGui::BeginMenu("対象")) {
                                        for (std::size_t s = 0; s < proj.library.size(); ++s) {
                                            const auto& se = proj.library.entries()[s];
                                            if (ImGui::MenuItem(se.name.c_str(), nullptr,
                                                                se.id == a.soundId)) {
                                                const unsigned sid = se.id;
                                                edit("動作の対象", [&] {
                                                    ev.setActionSound(
                                                        static_cast<std::size_t>(i),
                                                        static_cast<std::size_t>(k), sid);
                                                });
                                            }
                                        }
                                        ImGui::EndMenu();
                                    }
                                    ImGui::Separator();
                                    if (ImGui::MenuItem("動作を外す"))
                                        edit("動作を外す", [&] {
                                            ev.removeAction(static_cast<std::size_t>(i),
                                                            static_cast<std::size_t>(k));
                                        });
                                    ImGui::EndPopup();
                                }
                            ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                if (ev.size() == 0) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("まだイベントがありません。");
                    ImGui::TextDisabled("空いている所で右クリックして作ってください。");
                }

                // ⚠ ここを埋めないこと（項目になると右クリックが出なくなる）。
                if (ImGui::BeginPopupContextWindow("evempty",
                                                   ImGuiPopupFlags_MouseButtonRight |
                                                   ImGuiPopupFlags_NoOpenOverItems)) {
                    if (ImGui::MenuItem("イベントを作る")) {
                        int made = -1;
                        edit("イベントを作る",
                                   [&] { made = ev.add("新しいイベント"); });
                        if (made >= 0) {
                            selectedEvent = made;
                            startRenameEvent(made, ev.all()[static_cast<std::size_t>(made)].name);
                        }
                    }
                    ImGui::EndPopup();
                }

                ImGui::TextDisabled("右クリックで操作 ／ 二度押しで発火 ／ "
                                    "SoundLibrary から掴んで落とすと「再生」が付きます");
            }
            ImGui::End();
        }

        // ── プロパティ（選んでいるものを見て直す場所）
        //
        // ★**1 枚で全部を受け持ちます。**音源・フォルダ・イベント・動作で
        //   別々のパネルを作ると、種類が増えるたびにタブが増え、
        //   しかも同じ数字が 2 箇所に出ます（決めごと #1）。
        //   選んだものに応じて中身が入れ替わる、1 枚に寄せました。
        //
        // ★以前あった「Audio」タブはここへ畳みました（音源を選んだときの中身が それ）。
        if (showProps) {
            if (ImGui::Begin("プロパティ", &showProps)) {
                af::SoundLibrary& lib = proj.library;
                af::EventList&    ev  = proj.events;

                const bool hasSound = selKind == Sel::Sound && selected >= 0 &&
                                      static_cast<std::size_t>(selected) < lib.size();
                const bool hasEvent = (selKind == Sel::Event || selKind == Sel::Action) &&
                                      selectedEvent >= 0 &&
                                      static_cast<std::size_t>(selectedEvent) < ev.size();

                if (hasSound) {
                    // ── 音源
                    const std::size_t si = static_cast<std::size_t>(selected);
                    const auto& e = lib.entries()[si];

                    ImGui::TextDisabled("音源");
                    ImGui::TextUnformatted(e.name.c_str());
                    ImGui::Separator();

                    // ── 音データ
                    // ★選んだものが変わったときだけ読み直します（毎フレームは読まない）。
                    //   ⚠ 番号ではなく id で見比べること。並びが変わると同じ番号が
                    //     別の音源を指すので、番号だと古い絵が残ります。
                    if (waveId != e.id) buildWave(selected);

                    if (!waveNote.empty())
                        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", waveNote.c_str());
                    else
                        // ★調整のときは波形を少し低く。つまみが 4 本×2 行あるので、
                        //   全部を出すには縦が要ります（はみ出すと下のボタンが押せない）。
                        drawWave(propView == PropView::Adjust ? 112.0f : 150.0f);

                    if (e.resolved) {
                        // ★片耳ずつ黙らせる。**出口のチャンネル**に効きます。
                        //   モノラルの素材でも左右へ複製して出しているので、
                        //   片方を切れば片鳴りになります（＝経路の確かめに使える）。
                        // ⚠ これは聴き比べの道具で、音源の設定ではありません。
                        //   だからプロジェクトには保存しません ── 保存すると、
                        //   黙らせたまま閉じて、次に開いたとき原因の分からない片鳴りになります。
                        const char* side[2] = {"L", "R"};
                        for (int k = 0; k < 2; ++k) {
                            ImGui::PushID(k);
                            const bool m = cur().channelMuted(k);
                            if (m) ImGui::PushStyleColor(ImGuiCol_Button,
                                                         ImVec4(0.62f, 0.28f, 0.24f, 1.0f));
                            if (ImGui::Button(m ? (k == 0 ? "L 消" : "R 消")
                                                : (k == 0 ? "L"    : "R"), ImVec2(44, 0)))
                                cur().setChannelMute(k, !m);
                            if (m) ImGui::PopStyleColor();
                            ImGui::PopID();
                            ImGui::SameLine();
                        }
                        ImGui::BeginDisabled(!cur().channelMuted(0) && !cur().channelMuted(1));
                        if (ImGui::Button("両方鳴らす")) cur().clearChannelMutes();
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("｜ %s ／ 掴んで頭出し",
                                            durationText(e.info.seconds()).c_str());
                    }

                    ImGui::Spacing();

                    // ── 波形の下：調整と情報の切り替え
                    // ★1 枚に両方並べると、どちらも幅が足りませんでした
                    //   （「16 bit PCM」が切れ、つまみも短い）。横幅は丸ごと片方に渡します。
                    {
                        const ImVec4 lit(0.20f, 0.42f, 0.62f, 1.0f);
                        const struct { const char* name; PropView v; } tabs[2] = {
                            {"調整", PropView::Adjust}, {"情報", PropView::Info}};
                        for (int k = 0; k < 2; ++k) {
                            const bool on = propView == tabs[k].v;
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button, lit);
                            if (ImGui::Button(tabs[k].name, ImVec2(80, 0))) propView = tabs[k].v;
                            if (on) ImGui::PopStyleColor();
                            if (k == 0) ImGui::SameLine();
                        }
                        ImGui::Separator();
                    }

                    if (propView == PropView::Info) {
                        if (e.resolved) {
                            labelValue("形式",       "%s", e.info.formatLine().c_str());
                            labelValue("チャンネル", "%d ch", e.info.channels);
                            labelValue("周波数",     "%d Hz", e.info.sampleRate);
                            labelValue("長さ",       "%s",
                                       durationText(e.info.seconds()).c_str());
                            labelValue("大きさ",     "%s", sizeText(e.info.fileBytes).c_str());
                            labelValue("サンプル数", "%llu",
                                       static_cast<unsigned long long>(e.info.frames));
                        } else {
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", e.problem.c_str());
                        }
                        ImGui::Spacing();
                        ImGui::TextDisabled("在り処");
                        ImGui::TextWrapped("%s", af::wideToUtf8(e.path).c_str());
                        ImGui::Spacing();
                        if (ImGui::Button("エクスプローラで表示", ImVec2(180, 0)))
                            revealInExplorer(lib.absolutePathOf(si));
                    } else {
                        // ★上に数字、下にスライダ（既存のオーサリングツールと同じ形）。
                        //   数字は**打ち込めます** ── つまみだけだと「-4.5 dB ちょうど」に
                        //   合わせられず、狙った値を再現できません。
                        // ⚠ 4 本とも同じ形なので、1 本ずつ書かずにここでまとめます。
                        //   1 本ずつ書くと、Undo を積み忘れた 1 本が生まれます。
                        auto knob = [&](const char* label, float* v, float lo, float hi,
                                        const char* fmt, const char* unit, bool off,
                                        bool log, auto&& apply) {
                            ImGui::PushID(label);
                            // ★数字とスライダは 1 組。間を詰めて、組であることを見せる。
                            //   （離れていると別々の項目に見えます）
                            const ImVec2 sp = ImGui::GetStyle().ItemSpacing;
                            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp.x, 2.0f));

                            // 上段 ── 名前（切っているなら「切」）と、右端に数字。
                            ImGui::TextUnformatted(label);
                            if (off) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("（切）");
                            }
                            const float inW = 96.0f;
                            const float unitW = (unit && *unit)
                                                    ? ImGui::CalcTextSize(unit).x + 6.0f
                                                    : 0.0f;
                            ImGui::SameLine();
                            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x
                                                 - inW - unitW);
                            ImGui::SetNextItemWidth(inW);
                            // ⚠ 打ち込みは**確定してから**効かせる。1 文字ごとに効かせると、
                            //   「1200」と打つ途中の「1」で耳を殴られます。
                            bool typed = ImGui::InputFloat("##n", v, 0.0f, 0.0f, fmt,
                                                           ImGuiInputTextFlags_EnterReturnsTrue);
                            if (ImGui::IsItemActivated())
                                undo.push(label, proj.library, proj.events);
                            if (ImGui::IsItemDeactivatedAfterEdit()) typed = true;
                            if (unit && *unit) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", unit);
                            }

                            // 下段 ── スライダ。数字は上に出ているので目盛りは出さない。
                            ImGui::SetNextItemWidth(-1);
                            const bool slid = ImGui::SliderFloat("##s", v, lo, hi, "",
                                                                 log ? ImGuiSliderFlags_Logarithmic
                                                                     : 0);
                            if (ImGui::IsItemActivated())
                                undo.push(label, proj.library, proj.events);

                            if (typed || slid) {
                                // ⚠ 打ち込みは範囲の外も来ます。挟んでから渡すこと。
                                if (*v < lo) *v = lo;
                                if (*v > hi) *v = hi;
                                apply(*v);
                                proj.touch();
                            }
                            ImGui::PopStyleVar();
                            ImGui::PopID();
                            ImGui::Spacing();
                        };

                        float db = e.gainDb;
                        knob("音量", &db, -60.0f, 12.0f, "%.1f", "dB", false, false,
                             [&](float x) {
                                 lib.setGain(si, x);
                                 cur().setGainDb(x);
                             });

                        // ⚠ 読む速さを変えているので、**長さも一緒に伸び縮みします**。
                        float pt = e.pitch;
                        knob("ピッチ", &pt, -24.0f, 24.0f, "%+.1f", "半音", false, false,
                             [&](float x) {
                                 lib.setPitch(si, x);
                                 cur().setPitchSemitones(x);
                             });

                        // ⚠ 端に置いたら素通り。数字だけだと 20000 Hz で切っているのか
                        //   掛かっているのか区別が付かないので、名前の隣に「切」と出す。
                        float lpv = e.lpfHz;
                        knob("低域通過", &lpv, 20.0f, af::ClipSource::kLpOff, "%.0f", "Hz",
                             e.lpfHz >= af::ClipSource::kLpOff, true,
                             [&](float x) {
                                 lib.setLpf(si, x);
                                 cur().setLowPassHz(x);
                             });

                        float hpv = e.hpfHz;
                        knob("高域通過", &hpv, af::ClipSource::kHpOff, 20000.0f, "%.0f", "Hz",
                             e.hpfHz <= af::ClipSource::kHpOff, true,
                             [&](float x) {
                                 lib.setHpf(si, x);
                                 cur().setHighPassHz(x);
                             });

                        bool lp = e.loop;
                        if (ImGui::Checkbox("ループ", &lp)) {
                            edit("ループ", [&] { lib.setLoop(si, lp); });
                            cur().setLoop(lp);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("既定に戻す")) {
                            edit("調整を戻す", [&] {
                                lib.setGain(si, 0.0f);
                                lib.setPitch(si, 0.0f);
                                lib.setLpf(si, af::ClipSource::kLpOff);
                                lib.setHpf(si, af::ClipSource::kHpOff);
                            });
                            cur().setGainDb(0.0f);
                            cur().setPitchSemitones(0.0f);
                            cur().setLowPassHz(af::ClipSource::kLpOff);
                            cur().setHighPassHz(af::ClipSource::kHpOff);
                        }

                        ImGui::SameLine();
                        ImGui::BeginDisabled(!e.resolved);
                        // ⚠ ここは▶やスペースと違い、**必ず頭から**鳴らします。
                        //   （鳴らす入口が 3 つあるのは畳む余地あり。板の T30 に記録済み）
                        if (ImGui::Button("頭から試聴", ImVec2(110, 0))) {
                            firedEvent.clear();
                            auditionClip(selected);
                        }
                        ImGui::EndDisabled();
                        // ★説明文は置きません。1 度読めば済むものを常駐させると、
                        //   毎回それだけ縦を食って、下のボタンが見切れます
                        //   （実際に「試聴」が隠れました）。README に書いてあります。
                    }

                } else if (hasEvent) {
                    // ── イベント（と、その中の動作）
                    const std::size_t ei = static_cast<std::size_t>(selectedEvent);
                    const af::Event& e = ev.all()[ei];

                    ImGui::TextDisabled("イベント");
                    char nb[128] = {};
                    ::strncpy_s(nb, e.name.c_str(), sizeof(nb) - 1);
                    ImGui::SetNextItemWidth(280);
                    if (ImGui::InputText("##evname", nb, sizeof(nb)) && nb[0])
                        edit("イベントの名前", [&] { ev.rename(ei, nb); });
                    ImGui::SameLine();
                    ImGui::BeginDisabled(e.actions.empty());
                    if (ImGui::Button("発火", ImVec2(90, 0))) fireEvent(selectedEvent);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("ゲーム側はこの名前で呼びます");

                    ImGui::Separator();
                    ImGui::TextUnformatted("動作");

                    if (e.actions.empty()) {
                        ImGui::TextDisabled("まだありません。SoundLibrary から音源を掴んで"
                                            "落とすか、右クリックから足してください。");
                    } else {
                        const ImGuiTableFlags atf = ImGuiTableFlags_Borders |
                                                    ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_SizingStretchProp;
                        if (ImGui::BeginTable("acts", 3, atf)) {
                            ImGui::TableSetupColumn("種類",  ImGuiTableColumnFlags_WidthStretch, 0.6f);
                            ImGui::TableSetupColumn("対象",  ImGuiTableColumnFlags_WidthStretch, 1.8f);
                            ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthStretch, 0.4f);
                            ImGui::TableHeadersRow();

                            for (int k = 0; k < static_cast<int>(e.actions.size()); ++k) {
                                const af::EventAction& a = e.actions[static_cast<std::size_t>(k)];
                                const int si2 = lib.indexOfId(a.soundId);
                                ImGui::PushID(k);
                                ImGui::TableNextRow();

                                // 種類
                                ImGui::TableSetColumnIndex(0);
                                ImGui::SetNextItemWidth(-1);
                                if (ImGui::BeginCombo("##t", af::actionTypeName(a.type))) {
                                    for (int t = 0; t < 2; ++t) {
                                        const af::ActionType at = (t == 0) ? af::ActionType::Play
                                                                           : af::ActionType::Stop;
                                        if (ImGui::Selectable(af::actionTypeName(at),
                                                              a.type == at))
                                            edit("動作の種類", [&] {
                                                ev.setActionType(ei,
                                                                 static_cast<std::size_t>(k), at);
                                            });
                                    }
                                    ImGui::EndCombo();
                                }

                                // 対象
                                ImGui::TableSetColumnIndex(1);
                                ImGui::SetNextItemWidth(-1);
                                const char* cur = (si2 >= 0)
                                    ? lib.entries()[static_cast<std::size_t>(si2)].name.c_str()
                                    : "× 見つかりません";
                                if (si2 < 0)
                                    ImGui::PushStyleColor(ImGuiCol_Text,
                                                          ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                                const bool combo = ImGui::BeginCombo("##s", cur);
                                if (si2 < 0) ImGui::PopStyleColor();
                                if (combo) {
                                    for (std::size_t s = 0; s < lib.size(); ++s) {
                                        const auto& se = lib.entries()[s];
                                        if (ImGui::Selectable(se.name.c_str(),
                                                              se.id == a.soundId)) {
                                            const unsigned sid = se.id;
                                            edit("動作の対象", [&] {
                                                ev.setActionSound(
                                                    ei, static_cast<std::size_t>(k), sid);
                                            });
                                        }
                                    }
                                    ImGui::EndCombo();
                                }

                                // 外す
                                ImGui::TableSetColumnIndex(2);
                                if (ImGui::SmallButton("外す"))
                                    edit("動作を外す", [&] {
                                        ev.removeAction(ei, static_cast<std::size_t>(k));
                                    });
                            ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    }

                    ImGui::Spacing();
                    if (ImGui::Button("動作を足す") && lib.size() > 0)
                        edit("動作を足す", [&] {
                            ev.addAction(ei, af::ActionType::Play, lib.entries()[0].id);
                        });
                    ImGui::SameLine();
                    ImGui::TextDisabled("足してから、上の表で種類と対象を選びます");

                } else if (selKind == Sel::Folder && !selFolder.empty()) {
                    // ── フォルダ
                    int count = 0;
                    for (const auto& s : lib.entries())
                        if (s.folder == selFolder) ++count;

                    ImGui::TextDisabled("フォルダ");
                    ImGui::TextUnformatted(selFolder.c_str());
                    ImGui::Separator();
                    labelValue("直下の音源", "%d 本", count);
                    ImGui::TextDisabled("フォルダは並べ方だけで、音には何も影響しません。");
                    ImGui::TextDisabled("名前の変更・削除は SoundLibrary の右クリックから。");

                } else {
                    ImGui::TextDisabled("SoundLibrary かイベントで 1 つ選んでください。");
                    ImGui::TextDisabled("選んだものに応じて、ここの中身が入れ替わります。");
                }
            }
            ImGui::End();
        }


        // ── 試験信号（コントロールパネルから分けた）
        //
        // ★取り込んだ音とは別物なので、操作する場所も分ける。
        //   同じパネルに置いていると、いま何を触っているのか分かりにくい。
        if (showTone) {
            if (ImGui::Begin("試験信号", &showTone)) {
                ImGui::TextDisabled("デバイスまでの経路を耳で確かめるための音です。");
                ImGui::Separator();

                ImGui::BeginDisabled(wired == Playing::Clip);
                if (ImGui::Button(playing ? "停止" : "鳴らす", ImVec2(90, 0)))
                    auditionTone(!playing);

                ImGui::SetNextItemWidth(-90);
                float hz = proj.toneHz;
                if (ImGui::SliderFloat("周波数", &hz, 40.0f, 4000.0f, "%.0f Hz",
                                       ImGuiSliderFlags_Logarithmic)) {
                    proj.toneHz = hz; proj.dirty = true; applyProject();
                }
                ImGui::SetNextItemWidth(-90);
                float lv = proj.toneLevel;
                if (ImGui::SliderFloat("大きさ", &lv, 0.0f, 0.5f, "%.3f")) {
                    proj.toneLevel = lv; proj.dirty = true; applyProject();
                }
                if (ImGui::Checkbox("開いたら鳴らす", &proj.toneOnOpen)) proj.dirty = true;
                ImGui::EndDisabled();

                if (wired == Playing::Clip)
                    ImGui::TextDisabled("取り込んだ音を鳴らしている間は触れません。");

                ImGui::Separator();
                ImGui::TextDisabled("20ms の傾斜で出入りします。"
                                    "道具の側がプツッと鳴らすと、エンジンの不連続と"
                                    "区別が付かなくなるためです。");
            }
            ImGui::End();
        }

        // ── 波形
        // ── 出力
        if (showOutput) {
            if (ImGui::Begin("出力", &showOutput)) {
                if (device.running()) {
                    const int fpc = device.framesPerCallback();
                    labelValue("デバイス",   "%s", device.deviceName().c_str());
                    labelValue("形式",       "%d Hz / %d ch", device.sampleRate(),
                               device.channels());
                    labelValue("輪の大きさ", "%d サンプル", device.bufferFrames());
                    labelValue("1 回あたり", "%d サンプル（%.2f ms）", fpc,
                               fpc > 0 ? 1000.0 * fpc / device.sampleRate() : 0.0);
                    ImGui::Separator();
                    labelValue("回った回数", "%llu", device.callbacks());
                    // ★何本鳴っているかを出す。
                    //   ⚠ ここが 1 本しか無かった頃、ログは「再生 2」と出しながら
                    //     実際は 1 本でした。**数えた数と鳴っている数を別々に出す**ので、
                    //     食い違えばここで分かります。
                    labelValue("鳴っている本", "%d / %d（載せている %d）",
                               bus.playingCount(), af::VoiceBus::kVoices, bus.loadedCount());
                    labelValue("直近",       "%.4f ms", device.lastBlockMs());
                    labelValue("最悪",       "%.4f ms", device.worstBlockMs());
                    const double load = device.worstLoadPercent();
                    labelValue("持ち時間の", "%.3f %%", load);
                    bar(static_cast<float>(load / 100.0),
                        load < 50.0 ? ImVec4(0.24f, 0.80f, 0.72f, 1.0f)
                                    : ImVec4(0.90f, 0.45f, 0.25f, 1.0f), "");
                    if (ImGui::Button("実測値をリセット")) device.resetStats();

                    // ★オシロはここへ寄せました（元は真ん中の「波形」パネル）。
                    //   真ん中は音データを出す場所になったので、**同じ「波形」という
                    //   名前のものが 2 つ並ぶ**のを避けています。
                    //   消さなかったのは、これが**実時間スレッドが本当に回っている
                    //   目に見える証拠**だからで、上の数字と対になっています。
                    ImGui::Separator();
                    ImGui::TextUnformatted("直近の 1 ブロック");
                    if (scopeN > 0)
                        ImGui::PlotLines("##scope", scope.data(), scopeN, 0, nullptr,
                                         -0.55f, 0.55f, ImVec2(-1, 60));
                    else
                        ImGui::TextDisabled("まだ何も届いていません");
                    ImGui::TextDisabled("実時間スレッドが順番号つきで置いたものを、"
                                        "待たずに写しています。");
                } else {
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "デバイスが開いていません");
                }
            }
            ImGui::End();
        }

        // ── ログ
        if (showLog) {
            if (ImGui::Begin("ログ", &showLog)) {
                for (const auto& s : log) ImGui::TextWrapped("%s", s.c_str());
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::End();
        }

        // ── 自己検査（表示メニューから出す）
        if (showSelfTest) {
            if (ImGui::Begin("自己検査", &showSelfTest)) {
                const bool busy = testRunning.load() || testThread.joinable();
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("回す（約 1.1 秒 鳴ります）")) {
                    device.stop();          // 検査は自前でデバイスを開く
                    testRunning.store(true);
                    haveReport = false;
                    testThread = std::thread([&] {
                        report = af::runSelfTest(std::wstring());
                        testRunning.store(false);
                    });
                }
                ImGui::EndDisabled();
                if (busy) ImGui::TextDisabled("実行中…");

                if (haveReport) {
                    ImGui::Separator();
                    for (const auto& c : report.checks) {
                        // ★字は OK / NG にしてある。✔ は日本語フォントに無いことがあり、
                        //   豆腐になると「落ちた」のか「字が無い」のか見分けが付かない。
                        ImGui::TextColored(c.ok ? ImVec4(0.4f, 0.9f, 0.6f, 1)
                                                : ImVec4(1, 0.5f, 0.4f, 1),
                                           c.ok ? "OK" : "NG");
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s%s%s", c.what.c_str(),
                                           c.detail.empty() ? "" : " ── ", c.detail.c_str());
                    }
                    ImGui::Separator();
                    if (report.allPassed())
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1), "全部通りました");
                    else
                        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "落ちた項目 %d", report.failed);
                }
            }
            ImGui::End();
        }

        // ── モジュール（既定では出さない。載せる／付け替えは今の主題ではない）
        if (showModule) {
            if (ImGui::Begin("モジュール", &showModule)) {
                ImGui::TextWrapped("DLL を原本を掴まずに読み込む仕掛けの確認用です。"
                                   "エンジンを載せる段になったら使います。");
                ImGui::Separator();
                const std::wstring t = af::findDefaultModule();
                ImGui::TextWrapped("%s", t.empty() ? "（対象なし）"
                                                   : af::wideToUtf8(t).c_str());
                ImGui::BeginDisabled(mod.loaded() || t.empty());
                if (ImGui::Button("読み込む")) {
                    std::string e;
                    say(mod.load(t, &e) ? "読み込みました" : "読み込めません ── " + e);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!mod.loaded());
                if (ImGui::Button("解放する")) { mod.unload(); say("解放しました"); }
                ImGui::EndDisabled();
                if (mod.loaded()) {
                    const bool free1 = af::Module::originalIsWritable(mod.originalPath());
                    ImGui::TextColored(free1 ? ImVec4(0.4f, 0.9f, 0.6f, 1)
                                             : ImVec4(1, 0.5f, 0.4f, 1),
                                       free1 ? "読み込んだまま原本を書き込みで開けます"
                                             : "原本が掴まれています");
                }
            }
            ImGui::End();
        }

        // 資料の図を撮るとき、イベントを 1 つ作っておく（1 回だけ）。
        // ★ふつうの経路（edit）で作ること。ここだけ別経路にすると、
        //   撮れた図が「実際に使える」ことの証拠になりません。
        if (evEditShot && frame == 1) {
            int made = -1;
            edit("イベントを作る", [&] { made = proj.events.add("新しいイベント"); });
            if (made >= 0) { selectedEvent = made; selKind = Sel::Event; }
        }

        // 資料の図を撮るとき、プロパティを「情報」側にしておく。
        if (infoShot) { propView = PropView::Info; infoShot = false; }

        // 資料の図を撮るとき、行を選んで鳴らしておく（1 回だけ）。
        if (selectIndex >= 0 && frame >= 1) {
            selected = selectIndex;
            selKind  = Sel::Sound;
            if (startPlaying) auditionClip(selectIndex);
            selectIndex = -1;
        }

        // 資料の図を撮るとき、イベントを発火しておく（1 回だけ）。
        if (fireIndex >= 0 && frame >= 1) {
            selectedEvent = fireIndex;
            selKind = Sel::Event;
            fireEvent(fireIndex);
            fireIndex = -1;
        }

        // 資料の図を撮るとき、名前の変更を開いた状態にする（1 回だけ）。
        if (startRenaming && frame >= 2) {
            if (selected >= 0 && static_cast<std::size_t>(selected) < proj.library.size())
                startRename(selected,
                            proj.library.entries()[static_cast<std::size_t>(selected)].name);
            else if (!selFolder.empty())
                startRenameFolder(selFolder);
            startRenaming = false;
        }

        // 資料の図を撮るとき、見せたいパネルを前に出す。
        // ⚠ **窓を出したあとで**呼ぶこと。配置を組んだ直後だと、その窓はまだ
        //   この回で作られていないので効かない（最初それで空振りした）。
        if (!focusPanel.empty() && frame >= 2) {
            ImGui::SetWindowFocus(af::wideToUtf8(focusPanel).c_str());
            focusPanel.clear();
        }

        // ── 描く
        ImGui::Render();
        const float clear[4] = {0.07f, 0.08f, 0.10f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_backBuffer, nullptr);
        g_context->ClearRenderTargetView(g_backBuffer, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);   // 垂直同期。道具が CPU を食い荒らさないように

        if (!shotPath.empty() && ++frame >= shotFrames) {
            std::string e;
            saveBackBufferPng(shotPath, &e);
            done = true;
        }
    }

    if (testThread.joinable()) testThread.join();

    // ★止める順序に意味がある。
    //   先にデバイスを止めないと、実時間スレッドが音源を触っている最中に音源が消える。
    device.stop();
    mod.unload();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, inst);
    ::CoUninitialize();
    return 0;
}
