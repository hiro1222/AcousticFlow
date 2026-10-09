// module.h — DLL を「原本を掴まずに」読み込む。
//
// ★この道具が存在する理由そのもの。
//
//   Unity はネイティブ DLL を LoadLibrary したまま離しません。だから
//   **エンジンをビルドし直すたびに Unity を閉じる**必要がありました
//   （2026-08-19 には実際 `Device or resource busy` で差し替えが止まっている）。
//
//   ここでは読み込む前に**影武者（一時ファイル）を作って、そちらを LoadLibrary します。**
//   原本は誰にも掴まれないので、ホストを走らせたままエンジンをビルドし直せます。
//
// ⚠ これはミドルウェア側の一般的な仕掛けで、AcousticFlow のことは何も知りません。
//   どの DLL でも同じように読み込めます（実際、初回の確認は engine 以外でも通ります）。

#pragma once

#include <string>

namespace af {

class Module {
public:
    Module() = default;
    ~Module();

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // 原本を影武者へ複製してから読み込む。失敗したら err に理由が入る。
    bool load(const std::wstring& path, std::string* err);

    // 影武者を解放して消す。読み込んでいなければ何もしない。
    void unload();

    // 解放 → 読み込みを続けて行う。ビルドし直した DLL を掴み直す用。
    bool reload(std::string* err);

    bool loaded() const { return handle_ != nullptr; }

    // 関数を引く。無ければ nullptr。
    void* symbol(const char* name) const;

    const std::wstring& originalPath() const { return original_; }
    const std::wstring& shadowPath() const { return shadow_; }

    // ★原本が本当に掴まれていないかを、実際に書き込みで開いて確かめる。
    //   「掴んでいないはず」を主張ではなく検査にするための関数
    //   （この作品では、看板の性質は必ず検査にする決まり）。
    static bool originalIsWritable(const std::wstring& path);

    // ★比較用。影武者を使わずに原本を直接 LoadLibrary したとき、原本が掴まれるか。
    //
    //   これが無いと originalIsWritable() の ✔ が空振りでないか判定できません
    //   （そもそも OS が掴まないなら、影武者の仕掛けは何もしていないことになる）。
    //   Unity で起きているのはこちらの経路です。
    //   true = 掴まれた（＝影武者に意味がある）。
    static bool directLoadLocksOriginal(const std::wstring& path);

private:
    void* handle_ = nullptr;      // HMODULE。windows.h をヘッダに持ち込まないため void*
    std::wstring original_;
    std::wstring shadow_;
};

}  // namespace af
