// project.h — 開発環境が開いたり保存したりする「プロジェクト」。
//
// ★オーサリングツールの形の芯はここです。
//   窓やメニューは飾りで、**開いて・直して・保存できる物がある**ことが形の中身。
//
// ★2026-08-30、バンクという器を廃してここへ畳みました。
//   プロジェクトが音源とフォルダを直接持ちます。詳しくは `library.h` の頭。
//
// ⚠ 中身が空の機能を並べない、を守っています。
//   Wwise を写経して sound id / バス / RTPC の枠だけ作ると、
//   「観測する対象そのものが存在しない」項目が並びます（`SOUND_DEBUG_TOOL.md` §1）。

#pragma once

#include "events.h"
#include "library.h"

#include <string>

namespace af {

struct Project {
    // ── 中身（いまはこれだけ。増やすときは「本当に効くか」を先に確かめる）
    std::string name       = "名称未設定";
    float       toneHz     = 440.0f;
    float       toneLevel  = 0.10f;    // 線形。0.1 ≒ -20 dBFS
    bool        toneOnOpen = false;

    // 音源の台帳。**プロジェクトが直接持ちます**（別ファイルにはしない）。
    SoundLibrary library;

    // イベント。**ゲーム側からの発火点**。
    EventList events;

    // ── 状態
    std::wstring path;              // 空 = まだ保存していない
    bool         dirty = false;

    bool save(const std::wstring& to, std::string* err);
    bool load(const std::wstring& from, std::string* err);

    // プロジェクトが置かれているフォルダ。素材の相対パスの基準。
    // 未保存なら空（そのあいだ素材は絶対パスで持つ）。
    std::wstring dir() const;

    // 画面の見出しに出す名前。保存していなければ「名称未設定」。
    std::string titleLine() const;

    // 台帳を直したときに呼ぶ（未保存の印を立てる）。
    void touch() { dirty = true; }
};

}  // namespace af
