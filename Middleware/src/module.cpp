#include "module.h"

#include <windows.h>

#include <string>

namespace af {
namespace {

std::string lastErrorText() {
    const DWORD e = ::GetLastError();
    if (e == 0) return "（エラーコードなし）";
    char* buf = nullptr;
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string s = (n && buf) ? std::string(buf, n) : ("コード " + std::to_string(e));
    if (buf) ::LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// %TEMP% に重複しない名前を作る。プロセス ID と連番を入れているので、
// ホストを 2 つ立ち上げても衝突しない。
std::wstring makeShadowPath(const std::wstring& original) {
    wchar_t tmpDir[MAX_PATH + 1] = {};
    const DWORD n = ::GetTempPathW(MAX_PATH, tmpDir);
    if (n == 0 || n > MAX_PATH) return L"";

    // 元のファイル名を残す。デバッガや Process Explorer で追えるようにするため。
    std::wstring base = original;
    const size_t slash = base.find_last_of(L"\\/");
    if (slash != std::wstring::npos) base = base.substr(slash + 1);

    static unsigned serial = 0;
    ++serial;

    std::wstring dir = std::wstring(tmpDir) + L"afhost\\";
    ::CreateDirectoryW(dir.c_str(), nullptr);   // 既にあってもよい

    return dir + std::to_wstring(::GetCurrentProcessId()) + L"_" +
           std::to_wstring(serial) + L"_" + base;
}

}  // namespace

Module::~Module() { unload(); }

bool Module::load(const std::wstring& path, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    if (handle_) unload();

    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail("ファイルが見つかりません: " + lastErrorText());

    const std::wstring shadow = makeShadowPath(path);
    if (shadow.empty()) return fail("一時フォルダのパスを作れませんでした");

    // ★複製してから読み込む。ここが「原本を掴まない」の全部。
    if (!::CopyFileW(path.c_str(), shadow.c_str(), FALSE))
        return fail("影武者を作れませんでした: " + lastErrorText());

    // 依存 DLL は原本のフォルダから探させる。影武者は %TEMP% にいるので、
    // これを指定しないと同じフォルダに置いた依存 DLL が見つからない。
    HMODULE h = ::LoadLibraryExW(shadow.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        const std::string why = lastErrorText();
        ::DeleteFileW(shadow.c_str());
        return fail("読み込めませんでした: " + why);
    }

    handle_   = h;
    original_ = path;
    shadow_   = shadow;
    if (err) err->clear();
    return true;
}

void Module::unload() {
    if (!handle_) return;
    ::FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;

    // 解放した直後は、まだ OS がファイルを離していないことがある。
    // 消せなくても致命ではない（%TEMP% なので後で掃除される）ので、数回だけ試す。
    for (int i = 0; i < 5 && !shadow_.empty(); ++i) {
        if (::DeleteFileW(shadow_.c_str())) break;
        ::Sleep(20);
    }
    shadow_.clear();
}

bool Module::reload(std::string* err) {
    if (original_.empty()) {
        if (err) *err = "まだ何も読み込んでいません";
        return false;
    }
    const std::wstring keep = original_;
    unload();
    return load(keep, err);
}

void* Module::symbol(const char* name) const {
    if (!handle_) return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle_), name));
}

bool Module::directLoadLocksOriginal(const std::wstring& path) {
    HMODULE h = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) return false;                       // 読めないなら比較にならない
    const bool locked = !originalIsWritable(path);
    ::FreeLibrary(h);
    return locked;
}

bool Module::originalIsWritable(const std::wstring& path) {
    // 共有を一切許さずに書き込みで開く。誰かが掴んでいれば必ず失敗する。
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

}  // namespace af
