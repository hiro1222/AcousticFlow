#include "events.h"

#include <cstdlib>

namespace af {

int EventList::add(const std::string& baseName) {
    // ★名前が重ならないようにする。
    //   ゲーム側は名前で呼ぶので、同じ名前が 2 つあると
    //   **どちらが鳴るか分からない**という一番たちの悪い形になります。
    std::string name = baseName;
    for (int n = 2; n < 10000; ++n) {
        bool dup = false;
        for (const Event& e : events_)
            if (e.name == name) { dup = true; break; }
        if (!dup) break;
        name = baseName + " " + std::to_string(n);
    }

    Event e;
    e.name = name;
    events_.push_back(e);
    return static_cast<int>(events_.size()) - 1;
}

void EventList::remove(std::size_t index) {
    if (index >= events_.size()) return;
    events_.erase(events_.begin() + static_cast<long>(index));
}

void EventList::rename(std::size_t index, const std::string& newName) {
    if (index >= events_.size() || newName.empty()) return;
    events_[index].name = newName;
}

void EventList::addAction(std::size_t index, ActionType type, unsigned soundId) {
    if (index >= events_.size()) return;
    EventAction a;
    a.type    = type;
    a.soundId = soundId;
    events_[index].actions.push_back(a);
}

void EventList::removeAction(std::size_t index, std::size_t actionIndex) {
    if (index >= events_.size()) return;
    auto& v = events_[index].actions;
    if (actionIndex >= v.size()) return;
    v.erase(v.begin() + static_cast<long>(actionIndex));
}

void EventList::setActionType(std::size_t index, std::size_t actionIndex, ActionType type) {
    if (index >= events_.size()) return;
    auto& v = events_[index].actions;
    if (actionIndex >= v.size()) return;
    v[actionIndex].type = type;
}

void EventList::setActionSound(std::size_t index, std::size_t actionIndex, unsigned soundId) {
    if (index >= events_.size()) return;
    auto& v = events_[index].actions;
    if (actionIndex >= v.size()) return;
    v[actionIndex].soundId = soundId;
}

void EventList::writeTo(std::FILE* f) const {
    std::fprintf(f, "events.count = %d\n", static_cast<int>(events_.size()));
    for (std::size_t i = 0; i < events_.size(); ++i) {
        std::fprintf(f, "events.%d.name = %s\n", static_cast<int>(i), events_[i].name.c_str());
        std::fprintf(f, "events.%d.actions = %d\n", static_cast<int>(i),
                     static_cast<int>(events_[i].actions.size()));
        for (std::size_t k = 0; k < events_[i].actions.size(); ++k) {
            const EventAction& a = events_[i].actions[k];
            // 1 行 1 動作。"種類:音源番号" と書く（差分が読めるように短く）。
            std::fprintf(f, "events.%d.action.%d = %d:%u\n", static_cast<int>(i),
                         static_cast<int>(k), static_cast<int>(a.type), a.soundId);
        }
    }
}

bool EventList::readLine(const std::string& key, const std::string& value) {
    if (key == "events.count") {
        const int n = std::atoi(value.c_str());
        // ★正気度を見る。壊れた数で確保しない。
        events_.assign(static_cast<std::size_t>(n > 0 && n < 8192 ? n : 0), Event{});
        return true;
    }
    if (key.rfind("events.", 0) != 0) return false;

    const std::string rest = key.substr(7);
    const std::size_t dot = rest.find('.');
    if (dot == std::string::npos) return false;
    const std::size_t idx = static_cast<std::size_t>(std::atoi(rest.substr(0, dot).c_str()));
    if (idx >= events_.size()) return false;

    const std::string what = rest.substr(dot + 1);
    if (what == "name")    { events_[idx].name = value; return true; }
    if (what == "actions") { events_[idx].actions.clear(); return true; }

    if (what.rfind("action.", 0) == 0) {
        // "種類:音源番号"
        const std::size_t colon = value.find(':');
        if (colon == std::string::npos) return false;
        EventAction a;
        a.type = (std::atoi(value.substr(0, colon).c_str()) == 1) ? ActionType::Stop
                                                                  : ActionType::Play;
        a.soundId = static_cast<unsigned>(std::atoi(value.substr(colon + 1).c_str()));
        events_[idx].actions.push_back(a);
        return true;
    }
    return false;
}

}  // namespace af
