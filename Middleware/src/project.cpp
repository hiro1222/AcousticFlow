#include "project.h"

#include "host_util.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace af {
namespace {

// ★書式は「1 行 1 項目の key = value」。
//   バイナリにしないのは、**差分が git で読めるほうが、道具の設定には大事**だから。
//   資産（焼いた音響データ）を持つようになったらそちらは別書式になります。
constexpr const char* kMagic   = "# AfHost project";
constexpr int         kVersion = 2;    // 2 = バンクを畳んで sounds.* を直接持つ

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

std::wstring dirOf(const std::wstring& p) {
    const std::size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

}  // namespace

std::wstring Project::dir() const { return path.empty() ? std::wstring() : dirOf(path); }

bool Project::save(const std::wstring& to, std::string* err) {
    FILE* f = nullptr;
    if (::_wfopen_s(&f, to.c_str(), L"wb") != 0 || !f) {
        if (err) *err = "書き込めません";
        return false;
    }

    std::fprintf(f, "%s\n", kMagic);
    std::fprintf(f, "version = %d\n", kVersion);
    std::fprintf(f, "name = %s\n", name.c_str());
    std::fprintf(f, "tone.hz = %g\n", toneHz);
    std::fprintf(f, "tone.level = %g\n", toneLevel);
    std::fprintf(f, "tone.onOpen = %d\n", toneOnOpen ? 1 : 0);

    // ★保存先が変わると素材の相対パスの基準も変わるので、新しい基準を渡す。
    const std::wstring newBase = dirOf(to);
    library.writeTo(f, newBase);
    events.writeTo(f);
    std::fclose(f);

    path = to;
    // ★書いただけでは足りない。**メモリ側の相対パスも新しい基準へ付け替える。**
    //   ここを忘れると、別のフォルダへ保存した瞬間に実体を見失います。
    library.rebase(newBase);
    dirty = false;
    if (err) err->clear();
    return true;
}

bool Project::load(const std::wstring& from, std::string* err) {
    FILE* f = nullptr;
    if (::_wfopen_s(&f, from.c_str(), L"rb") != 0 || !f) {
        if (err) *err = "開けません";
        return false;
    }

    // 読み込みは一時の入れ物へ。途中で失敗したときに、いま開いている物を壊さないため。
    Project tmp;
    bool sawMagic = false;
    bool first    = true;
    char line[2048];

    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);

        // ★先頭の UTF-8 BOM を捨てる。
        //   このリポジトリは「PowerShell の .ps1 は BOM 付きで保存すること」という
        //   決まりがあるくらいで、BOM 付きのファイルが普通に出てきます。
        if (first) {
            first = false;
            if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
                static_cast<unsigned char>(s[1]) == 0xBB &&
                static_cast<unsigned char>(s[2]) == 0xBF)
                s.erase(0, 3);
        }
        if (s.empty()) continue;
        if (s.rfind(kMagic, 0) == 0) { sawMagic = true; continue; }
        if (s[0] == '#') continue;

        const std::size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(s.substr(0, eq));
        const std::string v = trim(s.substr(eq + 1));

        if      (k == "name")        tmp.name       = v;
        else if (k == "tone.hz")     tmp.toneHz     = static_cast<float>(std::atof(v.c_str()));
        else if (k == "tone.level")  tmp.toneLevel  = static_cast<float>(std::atof(v.c_str()));
        else if (k == "tone.onOpen") tmp.toneOnOpen = (std::atoi(v.c_str()) != 0);
        else if (k == "version")     continue;
        else if (k.rfind("events.", 0) == 0) tmp.events.readLine(k, v);
        else                                 tmp.library.readLine(k, v);
    }
    std::fclose(f);

    if (!sawMagic) {
        if (err) *err = "このファイルは AfHost のプロジェクトではありません";
        return false;
    }

    // ★値の正気度を見てから採る。壊れた設定で実時間スレッドを動かさない。
    if (!(tmp.toneHz > 20.0f && tmp.toneHz < 20000.0f))    tmp.toneHz    = 440.0f;
    if (!(tmp.toneLevel >= 0.0f && tmp.toneLevel <= 1.0f)) tmp.toneLevel = 0.10f;

    name       = tmp.name;
    toneHz     = tmp.toneHz;
    toneLevel  = tmp.toneLevel;
    toneOnOpen = tmp.toneOnOpen;
    library    = tmp.library;
    events     = tmp.events;
    path       = from;

    library.finishLoad(dirOf(from));

    dirty = false;
    if (err) err->clear();
    return true;
}

std::string Project::titleLine() const {
    std::string s = name.empty() ? "名称未設定" : name;
    if (dirty) s += " *";
    return s;
}

}  // namespace af
