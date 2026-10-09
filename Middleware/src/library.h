// library.h — プロジェクトが持つ音源の台帳。
//
// ★2026-08-30、**バンクという器を廃しました。**
//
//   バンクを別ファイル（`.afbank`）にしていましたが、容れ物としての働きが
//   フォルダと完全に重複していました。バンクが余分に持っていたのは
//   「別ファイルであること」だけで、**それを何にも使っていませんでした**。
//
//   実際のミドルウェアでバンクが独立しているのは**焼いて配る単位**だからです。
//   焼く道具がまだ無い以上、名前だけ借りた空の器になります
//   （「中身の空いた機能を並べない」に反する）。
//   焼く段になったら、そのとき本当に要る単位を改めて決めます。
//
// ★ここで持たないもの（決定済み・蒸し返さない）
//   ピッチ・フィルタ・ランダム化・切り替え・バス・RTPC。
//   持つのは在り処と、**試聴の鳴らし方**（音量・ループ）だけです。

#pragma once

#include "wav.h"

#include <cstdio>
#include <string>
#include <vector>

namespace af {

struct SoundEntry {
    // ★イベントから指すための番号。**名前ではなく番号で指す。**
    //   名前で指すと、改名した瞬間にイベントが壊れます。番号なら改名しても
    //   並べ替えても外さない限り繋がったままです（Wwise が GUID でやっているのと同じ狙い）。
    unsigned     id = 0;

    std::string  name;    // 表示名。既定はファイル名（拡張子なし）
    std::wstring path;    // プロジェクトからの相対。外にあるものは絶対

    // どのフォルダに入れているか。空 = 直下。入れ子は "足音/石" のように書く。
    // ★フォルダは**並べ方**であって、音には何も影響しません。
    std::string  folder;

    // ── 試聴のための調整（プロパティで触る）
    //
    // ★どれも「鳴らし方」で、素材そのものは書き換えません。
    //   焼いた資産にこの値が乗るかどうかは、**焼く道具を作る段の別の決めごと**です。
    float gainDb = 0.0f;    // -60..+12
    bool  loop   = false;
    // ⚠ ピッチは**読む速さ**なので、時間も一緒に伸び縮みします（長さは保たれない）。
    float pitch  = 0.0f;    // 半音。-24..+24
    // ⚠ 端＝素通り（ClipSource::kLpOff / kHpOff と同じ値）。
    float lpfHz  = 20000.0f;
    float hpfHz  = 20.0f;
    // ── ここから下は毎回ファイルから引く。保存しない
    WavInfo     info;
    bool        resolved = false;   // ファイルが在って、読めた
    std::string problem;            // 読めない理由
};

// 音源とフォルダの台帳。プロジェクトが 1 つだけ持つ。
class SoundLibrary {
public:
    // 相対パスの基準（＝プロジェクトのフォルダ）。保存・読み込みのときに入る。
    void setBaseDir(const std::wstring& d) { base_ = d; }
    const std::wstring& baseDir() const { return base_; }

    // ── 中身
    const std::vector<SoundEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); folders_.clear(); }

    // WAV を 1 本足す。読めないファイルは**足しません**（err に理由）。
    bool add(const std::wstring& wavPath, const std::string& folder, std::string* err);

    // 番号で引く。無ければ -1。
    int  indexOfId(unsigned id) const;
    void remove(std::size_t index);
    void rename(std::size_t index, const std::string& newName);
    void setGain(std::size_t index, float db);
    void setLoop(std::size_t index, bool on);
    void setPitch(std::size_t index, float semitones);
    void setLpf(std::size_t index, float hz);
    void setHpf(std::size_t index, float hz);
    void setFolder(std::size_t index, const std::string& folder);

    std::wstring absolutePathOf(std::size_t index) const;

    // ── フォルダ（並べ方だけ。音には影響しない）
    //
    // ★空のフォルダも作れるように、**明示的に持ちます**。
    //   「項目が指しているから在る」だけにすると、先に仕切りを作れません。
    const std::vector<std::string>& folders() const { return folders_; }
    void addFolder(const std::string& path);
    void renameFolder(const std::string& from, const std::string& to);
    // 消したフォルダの中身は**ひとつ上へ移す**（黙って消さない）。
    void removeFolder(const std::string& path);
    // 直下の子フォルダ（表示用）。prefix が空なら一番上。
    std::vector<std::string> childFolders(const std::string& prefix) const;

    // 全項目をファイルから引き直す（開いた直後・保存先が変わった直後に呼ぶ）。
    void refresh();

    // ── 保存と読み込み（プロジェクトのファイルの一部として書かれる）
    void writeTo(std::FILE* f, const std::wstring& newBase) const;

    // ★保存先が変わったあとに呼ぶ。**メモリ側の相対パスも付け替える。**
    //   基準だけ新しくしてパスを古いままにすると、次に読み直すまで
    //   その場では正しく見えるのに、実体を見失います（検査で捕まえた）。
    void rebase(const std::wstring& newBase);
    // "sounds.*" / "folders.*" の 1 行を食わせる。読めたら true。
    bool readLine(const std::string& key, const std::string& value);
    // 読み終わったあとに呼ぶ（基準を入れて、素性を引き直す）。
    void finishLoad(const std::wstring& base);

private:
    std::vector<SoundEntry>  entries_;
    unsigned                 nextId_ = 1;   // 0 は「指していない」の意に使う
    std::vector<std::string> folders_;   // 明示的に持つ（空でも作れるように）
    std::wstring             base_;      // 相対パスの基準
};

}  // namespace af
