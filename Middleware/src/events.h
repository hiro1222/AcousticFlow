// events.h — イベント。**ゲーム側からの発火点**。
//
// ★これは「ゲームが名前で呼ぶ口」です。
//
//   ゲーム側のコードは音源のファイル名も、音量も、フォルダも知りません。
//   知っているのは `足音_石` のような**イベントの名前**だけ。
//   何を鳴らすかを差し替えても、ゲーム側のコードは 1 行も変わりません。
//   これが Wwise のイベントと同じ狙いで、**音の作り手と実装者を切り離す**ところです。
//
// ★イベントは「動作の並び」を持ちます。
//   1 つの発火で「A を鳴らして B を止める」ができないと、名前を付けただけの
//   別名になってしまい、イベントである意味がありません。
//
// ★対象は**音源の番号**で指します（名前ではなく）。
//   名前で指すと、改名した瞬間にイベントが壊れます。
//
// ⚠ いま持っている動作は **再生 / 停止** の 2 つだけです。
//   遅延・フェード・確率・切り替えは持ちません。**まだ効かせる先が無いから**で、
//   枠だけ作ると中身の空いた機能になります（`SOUND_DEBUG_TOOL.md` §1）。

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace af {

enum class ActionType {
    Play = 0,   // 鳴らす
    Stop = 1,   // 止める
};

inline const char* actionTypeName(ActionType t) {
    return t == ActionType::Play ? "再生" : "停止";
}

struct EventAction {
    ActionType type    = ActionType::Play;
    unsigned   soundId = 0;      // SoundEntry::id。0 = 指していない
};

struct Event {
    std::string              name = "新しいイベント";
    std::vector<EventAction> actions;
};

class EventList {
public:
    const std::vector<Event>& all() const { return events_; }
    std::size_t size() const { return events_.size(); }
    void clear() { events_.clear(); }

    // 名前が重ならないように連番を振って足す。作った位置を返す。
    int  add(const std::string& baseName);
    void remove(std::size_t index);
    void rename(std::size_t index, const std::string& newName);

    void addAction(std::size_t index, ActionType type, unsigned soundId);
    void removeAction(std::size_t index, std::size_t actionIndex);
    void setActionType(std::size_t index, std::size_t actionIndex, ActionType type);
    void setActionSound(std::size_t index, std::size_t actionIndex, unsigned soundId);

    // ── 保存と読み込み（プロジェクトのファイルの一部として書かれる）
    void writeTo(std::FILE* f) const;
    bool readLine(const std::string& key, const std::string& value);

private:
    std::vector<Event> events_;
};

}  // namespace af
