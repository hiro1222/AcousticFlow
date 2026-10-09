// selftest.h — 「立ち上がる」を主張ではなく検査にする。
//
// ★GUI 版とコンソール版の両方から同じものを呼ぶ。
//   走査の答えを 2 つ持たせないため（`AfCapScan` が Unity のパネルと同じ関数を
//   呼んでいるのと同じ考え方）。

#pragma once

#include <string>
#include <vector>

namespace af {

struct CheckResult {
    bool        ok = false;
    std::string what;      // 何を確かめたか
    std::string detail;    // 実測値・失敗理由
};

struct SelfTestReport {
    std::vector<CheckResult> checks;
    int failed = 0;
    bool allPassed() const { return failed == 0; }
};

// 一通り動かして結果を返す。target が空なら DLL の項目は飛ばす。
// ⚠ 中で 1.1 秒ほど音を鳴らすので、UI スレッドから直接呼ぶと固まる。
SelfTestReport runSelfTest(const std::wstring& target);

}  // namespace af
