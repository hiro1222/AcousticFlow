// undo.h — 元に戻す／やり直す。
//
// ★台帳とイベントを**まとめて 1 つの写し**にして積みます（操作の逆再生ではなく）。
//
//   逆再生（コマンド方式）は、操作ごとに「戻し方」を書く必要があり、
//   **1 つ書き忘れると、そこだけ静かに戻らない**道具になります。
//   台帳は数百項目の見込みなので、まるごと写しても安く済みます
//   （1 段が数十 KB。64 段で数 MB）。安さと引き換えに、取りこぼしが起きません。
//
// ★片方だけ戻してはいけません。
//   イベントは音源を**番号**で指しています。台帳だけ戻すと、
//   戻した先に居ない音源をイベントが指した状態になり、
//   **「見つかりません」が勝手に生えます**。逆に、イベントだけ戻すと
//   消したはずの動作が復活します。だから両方を 1 段に入れて、必ず一緒に動かします。
//
// ★写すのは**変える前**です。`edit()` を通して必ず 1 段積んでから変えます。
//   入口を 1 つにしてあるので、新しい操作を足しても積み忘れが起きません。
//
// ⚠ 対象は**台帳とイベントだけ**です。
//   プロジェクトの名前や試験信号の設定は対象外（打ち直せば済むため）。

#pragma once

#include "events.h"
#include "library.h"

#include <string>
#include <vector>

namespace af {

class UndoStack {
public:
    // 何段まで持つか。これを超えたら古いほうから捨てる。
    static constexpr std::size_t kMax = 64;

    // 開いた直後・新規の直後に呼ぶ。積んだ段を全部捨てる。
    void reset() { undo_.clear(); redo_.clear(); }

    // 変える**前**の姿を積む。label は「元に戻す：移動」のように出る。
    void push(const std::string& label, const SoundLibrary& lib, const EventList& events) {
        undo_.push_back({label, lib, events});
        if (undo_.size() > kMax) undo_.erase(undo_.begin());
        // ★新しく変えたら、やり直しの先は捨てる。
        //   残すと「戻して・別のことをして・やり直す」で辻褄が合わなくなります。
        redo_.clear();
    }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    const std::string& undoLabel() const { return undo_.back().label; }
    const std::string& redoLabel() const { return redo_.back().label; }

    bool undo(SoundLibrary* lib, EventList* events) {
        if (undo_.empty() || !lib || !events) return false;
        redo_.push_back({undo_.back().label, *lib, *events});
        *lib    = undo_.back().lib;
        *events = undo_.back().events;
        undo_.pop_back();
        return true;
    }

    bool redo(SoundLibrary* lib, EventList* events) {
        if (redo_.empty() || !lib || !events) return false;
        undo_.push_back({redo_.back().label, *lib, *events});
        *lib    = redo_.back().lib;
        *events = redo_.back().events;
        redo_.pop_back();
        return true;
    }

    std::size_t depth() const { return undo_.size(); }

private:
    struct Step {
        std::string  label;
        SoundLibrary lib;
        EventList    events;
    };
    std::vector<Step> undo_;
    std::vector<Step> redo_;
};

}  // namespace af
