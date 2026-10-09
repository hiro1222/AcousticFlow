// host_util.h — コンソール版と GUI 版で共有する小物。
//
// ★同じことを 2 箇所に書かないため。既定の探索候補が 2 つあると、
//   「片方でだけ見つかる」という追いにくい食い違いになる。

#pragma once

#include <windows.h>

#include <string>

namespace af {

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

inline std::wstring absolutePath(const std::wstring& rel) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetFullPathNameW(rel.c_str(), MAX_PATH * 2, buf, nullptr);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf) : rel;
}

inline bool fileExists(const std::wstring& p) {
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 引数が無いときに順に試す既定の候補。
// リポジトリのどこから起動されても当たるように、相対で数段さかのぼる。
inline std::wstring findDefaultModule() {
    static const wchar_t* kCandidates[] = {
        L"AcousticEngine.dll",
        L"..\\..\\..\\..\\build2\\bin\\Release\\AcousticEngine.dll",
        L"..\\..\\..\\..\\UnityDemo\\Assets\\Plugins\\x86_64\\AcousticEngine.dll",
        L"..\\..\\..\\build2\\bin\\Release\\AcousticEngine.dll",
        L"..\\..\\..\\UnityDemo\\Assets\\Plugins\\x86_64\\AcousticEngine.dll",
        L"..\\..\\build2\\bin\\Release\\AcousticEngine.dll",
        L"..\\..\\UnityDemo\\Assets\\Plugins\\x86_64\\AcousticEngine.dll",
        L"build2\\bin\\Release\\AcousticEngine.dll",
        L"UnityDemo\\Assets\\Plugins\\x86_64\\AcousticEngine.dll",
    };
    for (const wchar_t* c : kCandidates) {
        const std::wstring p = absolutePath(c);
        if (fileExists(p)) return p;
    }
    return {};
}

}  // namespace af
