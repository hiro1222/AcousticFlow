#include "library.h"

#include "host_util.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace af {
namespace {

std::wstring fileNameOf(const std::wstring& p) {
    const std::size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

std::string stemUtf8(const std::wstring& p) {
    std::wstring n = fileNameOf(p);
    const std::size_t dot = n.find_last_of(L'.');
    if (dot != std::wstring::npos) n = n.substr(0, dot);
    return wideToUtf8(n);
}

bool isAbsolute(const std::wstring& p) {
    return (p.size() >= 2 && p[1] == L':') || (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
}

std::wstring trimSlash(std::wstring d) {
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

// base の下にあれば相対にする。無理なら絶対のまま。
//
// ★相対にしたいのは、プロジェクトごと別の機械へ移せるようにするため。
//   絶対パスで持つと、渡した先で全部「見つかりません」になります。
std::wstring makeRelative(const std::wstring& abs, const std::wstring& base) {
    if (base.empty()) return abs;
    const std::wstring dir = trimSlash(base);
    if (abs.size() > dir.size() + 1 &&
        ::CompareStringOrdinal(abs.c_str(), static_cast<int>(dir.size()),
                               dir.c_str(), static_cast<int>(dir.size()), TRUE) == CSTR_EQUAL &&
        (abs[dir.size()] == L'\\' || abs[dir.size()] == L'/')) {
        return abs.substr(dir.size() + 1);
    }
    return abs;
}

// "足音" と "足音/石" の親子を見る。名前の一致だけで見ると子が迷子になる。
bool isUnder(const std::string& s, const std::string& p) {
    return s == p ||
           (s.size() > p.size() && s.compare(0, p.size(), p) == 0 && s[p.size()] == '/');
}

}  // namespace

std::wstring SoundLibrary::absolutePathOf(std::size_t index) const {
    if (index >= entries_.size()) return {};
    const std::wstring& p = entries_[index].path;
    if (isAbsolute(p)) return p;
    if (base_.empty()) return absolutePath(p);
    return trimSlash(base_) + L"\\" + p;
}

bool SoundLibrary::add(const std::wstring& wavPath, const std::string& folder, std::string* err) {
    const std::wstring abs = absolutePath(wavPath);

    // ★足す前に読めることを確かめる。
    //   読めないものを台帳に載せると、鳴らそうとした時まで気づけません。
    WavInfo info;
    if (!readWavInfo(abs, &info, err)) return false;

    SoundEntry e;
    e.id       = nextId_++;
    e.name     = stemUtf8(abs);
    e.path     = makeRelative(abs, base_);
    e.folder   = folder;
    e.info     = info;
    e.resolved = true;
    entries_.push_back(e);
    if (!folder.empty()) addFolder(folder);
    if (err) err->clear();
    return true;
}

int SoundLibrary::indexOfId(unsigned id) const {
    if (id == 0) return -1;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].id == id) return static_cast<int>(i);
    return -1;
}

void SoundLibrary::remove(std::size_t index) {
    if (index >= entries_.size()) return;
    entries_.erase(entries_.begin() + static_cast<long>(index));
}

void SoundLibrary::rename(std::size_t index, const std::string& newName) {
    if (index >= entries_.size()) return;
    entries_[index].name = newName;
}

void SoundLibrary::setGain(std::size_t index, float db) {
    if (index >= entries_.size()) return;
    entries_[index].gainDb = db;
}

void SoundLibrary::setLoop(std::size_t index, bool on) {
    if (index >= entries_.size()) return;
    entries_[index].loop = on;
}

void SoundLibrary::setPitch(std::size_t index, float semitones) {
    if (index >= entries_.size()) return;
    if (semitones < -24.0f) semitones = -24.0f;
    if (semitones >  24.0f) semitones =  24.0f;
    entries_[index].pitch = semitones;
}

void SoundLibrary::setLpf(std::size_t index, float hz) {
    if (index >= entries_.size()) return;
    if (hz <    20.0f) hz =    20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    entries_[index].lpfHz = hz;
}

void SoundLibrary::setHpf(std::size_t index, float hz) {
    if (index >= entries_.size()) return;
    if (hz <    20.0f) hz =    20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    entries_[index].hpfHz = hz;
}

void SoundLibrary::setFolder(std::size_t index, const std::string& folder) {
    if (index >= entries_.size()) return;
    entries_[index].folder = folder;
    if (!folder.empty()) addFolder(folder);
}

void SoundLibrary::addFolder(const std::string& p) {
    if (p.empty()) return;
    for (const std::string& f : folders_)
        if (f == p) return;
    folders_.push_back(p);
    std::sort(folders_.begin(), folders_.end());
}

void SoundLibrary::renameFolder(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return;

    // ★入れ子ごと連れて行く。"足音" を直したら "足音/石" も付いてくる。
    for (std::string& f : folders_)
        if (isUnder(f, from)) f = to + f.substr(from.size());
    for (SoundEntry& e : entries_)
        if (isUnder(e.folder, from)) e.folder = to + e.folder.substr(from.size());

    std::sort(folders_.begin(), folders_.end());
    folders_.erase(std::unique(folders_.begin(), folders_.end()), folders_.end());
}

void SoundLibrary::removeFolder(const std::string& p) {
    if (p.empty()) return;

    const std::size_t slash = p.find_last_of('/');
    const std::string up = (slash == std::string::npos) ? std::string() : p.substr(0, slash);

    // ★中身は消さずに、ひとつ上へ移す。
    //   フォルダは並べ方でしかないので、消したら素材まで消えるのは筋が違う。
    for (SoundEntry& e : entries_)
        if (isUnder(e.folder, p)) e.folder = up;

    folders_.erase(std::remove_if(folders_.begin(), folders_.end(),
                                  [&](const std::string& s) { return isUnder(s, p); }),
                   folders_.end());
}

std::vector<std::string> SoundLibrary::childFolders(const std::string& prefix) const {
    std::vector<std::string> out;

    // 明示的なフォルダと、項目が指しているフォルダの両方から集める
    // （読み込んだファイルに folders の行が無くても、迷子を出さないため）。
    auto consider = [&](const std::string& full) {
        if (full.empty()) return;
        if (prefix.empty()) {
            const std::size_t s = full.find('/');
            out.push_back(s == std::string::npos ? full : full.substr(0, s));
        } else {
            if (full.size() <= prefix.size()) return;
            if (full.compare(0, prefix.size(), prefix) != 0 || full[prefix.size()] != '/') return;
            const std::string rest = full.substr(prefix.size() + 1);
            const std::size_t s = rest.find('/');
            out.push_back(prefix + "/" + (s == std::string::npos ? rest : rest.substr(0, s)));
        }
    };
    for (const std::string& f : folders_)  consider(f);
    for (const SoundEntry& e : entries_)   consider(e.folder);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void SoundLibrary::refresh() {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        SoundEntry& e = entries_[i];
        std::string err;
        e.resolved = readWavInfo(absolutePathOf(i), &e.info, &err);
        e.problem  = e.resolved ? std::string() : err;
    }
}

void SoundLibrary::writeTo(std::FILE* f, const std::wstring& newBase) const {
    // ★保存先が変わると相対の基準も変わる。
    //   いまの基準で絶対へ直してから、新しい基準で相対に取り直す。
    //   これを忘れると、別のフォルダへ保存した瞬間に全部見失います。
    std::fprintf(f, "folders.count = %d\n", static_cast<int>(folders_.size()));
    for (std::size_t i = 0; i < folders_.size(); ++i)
        std::fprintf(f, "folders.%d = %s\n", static_cast<int>(i), folders_[i].c_str());

    std::fprintf(f, "sounds.count = %d\n", static_cast<int>(entries_.size()));
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const std::wstring rel = makeRelative(absolutePathOf(i), newBase);
        // ★番号を最初に書く。イベントがここを指しています。
        std::fprintf(f, "sounds.%d.id = %u\n", static_cast<int>(i), entries_[i].id);
        std::fprintf(f, "sounds.%d.name = %s\n", static_cast<int>(i), entries_[i].name.c_str());
        std::fprintf(f, "sounds.%d.path = %s\n", static_cast<int>(i), wideToUtf8(rel).c_str());
        // 既定のままの値は書かない。差分を読みたいので、余計な行を増やさない。
        if (!entries_[i].folder.empty())
            std::fprintf(f, "sounds.%d.folder = %s\n", static_cast<int>(i),
                         entries_[i].folder.c_str());
        if (entries_[i].gainDb != 0.0f)
            std::fprintf(f, "sounds.%d.gainDb = %g\n", static_cast<int>(i), entries_[i].gainDb);
        if (entries_[i].pitch != 0.0f)
            std::fprintf(f, "sounds.%d.pitch = %g\n", static_cast<int>(i), entries_[i].pitch);
        if (entries_[i].lpfHz < 20000.0f)
            std::fprintf(f, "sounds.%d.lpfHz = %g\n", static_cast<int>(i), entries_[i].lpfHz);
        if (entries_[i].hpfHz > 20.0f)
            std::fprintf(f, "sounds.%d.hpfHz = %g\n", static_cast<int>(i), entries_[i].hpfHz);
        if (entries_[i].loop)
            std::fprintf(f, "sounds.%d.loop = 1\n", static_cast<int>(i));
    }
}

void SoundLibrary::rebase(const std::wstring& newBase) {
    // ★先に全部を絶対へ直してから、新しい基準で相対に取り直す。
    //   1 本ずつ「絶対にして相対に」とやると、途中で base_ を差し替えることになり、
    //   残りの項目が新しい基準で解決されてしまいます。
    std::vector<std::wstring> abs(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) abs[i] = absolutePathOf(i);

    base_ = newBase;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        entries_[i].path = makeRelative(abs[i], base_);

    refresh();
}

bool SoundLibrary::readLine(const std::string& key, const std::string& value) {
    if (key == "folders.count") { folders_.clear(); return true; }
    if (key.rfind("folders.", 0) == 0) { folders_.push_back(value); return true; }

    if (key == "sounds.count") {
        const int n = std::atoi(value.c_str());
        // ★正気度を見る。壊れた数で確保しない。
        entries_.assign(static_cast<std::size_t>(n > 0 && n < 8192 ? n : 0), SoundEntry{});
        return true;
    }
    if (key.rfind("sounds.", 0) != 0) return false;

    const std::string rest = key.substr(7);
    const std::size_t dot = rest.find('.');
    if (dot == std::string::npos) return false;
    const std::size_t idx = static_cast<std::size_t>(std::atoi(rest.substr(0, dot).c_str()));
    if (idx >= entries_.size()) return false;

    const std::string what = rest.substr(dot + 1);
    if      (what == "id")     entries_[idx].id     = static_cast<unsigned>(std::atoi(value.c_str()));
    else if (what == "name")   entries_[idx].name   = value;
    else if (what == "path")   entries_[idx].path   = utf8ToWide(value);
    else if (what == "folder") entries_[idx].folder = value;
    else if (what == "gainDb") entries_[idx].gainDb = static_cast<float>(std::atof(value.c_str()));
    else if (what == "pitch")  entries_[idx].pitch  = static_cast<float>(std::atof(value.c_str()));
    else if (what == "lpfHz")  entries_[idx].lpfHz  = static_cast<float>(std::atof(value.c_str()));
    else if (what == "hpfHz")  entries_[idx].hpfHz  = static_cast<float>(std::atof(value.c_str()));
    else if (what == "loop")   entries_[idx].loop   = (std::atoi(value.c_str()) != 0);
    else return false;
    return true;
}

void SoundLibrary::finishLoad(const std::wstring& base) {
    base_ = base;

    // ★番号を持たない項目（古いファイル）には、ここで振り直す。
    //   0 のまま残すとイベントから指せません。
    nextId_ = 1;
    for (const SoundEntry& e : entries_)
        if (e.id >= nextId_) nextId_ = e.id + 1;
    for (SoundEntry& e : entries_)
        if (e.id == 0) e.id = nextId_++;

    std::sort(folders_.begin(), folders_.end());
    folders_.erase(std::unique(folders_.begin(), folders_.end()), folders_.end());
    // ★素性はここで初めてファイルから引く（台帳には持っていない）。
    refresh();
}

}  // namespace af
