/* capture_scan.h ── 録った .afcap に**型紙を当てる**（走査）。
 *
 * ★ここが道具の心臓。ただし中身は薄い。判定式は全部 detectors.h にあり、
 *   **回帰テストが呼ぶのと同じ関数**を呼ぶだけ。
 *   別々の式にすると「道具は見つけたのに検査は通る」が起きる（docs/SOUND_DEBUG_TOOL.md §5.1）。
 *
 * 置き場所について（2026-08-24 に一度変えた）:
 *   最初はツール側（AcousticEngineTest/）に置いていた。Unity のキャプチャ一覧から
 *   走査を呼びたくなったので、DLL に入るここへ移した。
 *   ★**理由は変わっていない**: 判定するのは「録音後」であって実時間ではない。
 *     だから検出器を後で良くしても**昔のキャプチャに付け直せる**（§1 判断2）。
 *     変えたのは置き場所だけで、回帰テストが呼ぶ関数は同じまま。
 *   ⚠ ここは AF_SceneUpdate の経路からは**呼ばれない**。呼んだらこの利点が消える。
 */
#ifndef ACOUSTICFLOW_TEST_CAPTURE_SCAN_H
#define ACOUSTICFLOW_TEST_CAPTURE_SCAN_H

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "detectors.h"
#include "capture.h"

namespace af {
namespace scan {

using acoustic::dbg::CaptureFile;

/* 走査で付いた印 1 つ。★時刻だけでなく「どの音源の何が動いたか」を必ず持つ。 */
struct Mark {
    int frame = 0;
    float seconds = 0.0f;
    unsigned long long sourceId = 0;
    const char* templateName = "";   // どの型紙が鳴ったか
    const char* what = "";           // 何の量が動いたか
    float amount = 0.0f;
};

struct ScanOptions {
    float maxStepDb = 6.0f;      // 型紙1。正常な階段（段の間引き）より上に置く
    float minTiltDb = 3.0f;      // 型紙4。遮蔽されているのに平ら＝幻
    float occludedAbove = 0.5f;  // 遮蔽スカラがこれ以上
    /* ★★ 型紙4 の前提。実機のキャプチャで分かったこと（2026-08-24）★★
     *   最初は「遮蔽スカラ >= 0.5 なら周波数依存が出るはず」としていた。
     *   実機（Test_Full・1141 フレーム）に当てたら **816 件**が光って使い物にならなかった。
     *
     *   原因: `occ` は**幾何的にどれだけ遮られたか**であって、
     *   「透過と回折でしか届いていない」ではない。開いた戸口の向こうの音は
     *   直線が塞がれていても**開口を素通り**して届くので、平らで**正しい**。
     *
     *   元になったバグ（閉扉の幻）の本質は「**ほとんど通っていないのに平ら**」だった
     *   （-13.5dB で低−高 0.0dB）。ほとんど通っていないなら、残っているのは
     *   透過か回折しかなく、どちらも強く周波数に依存する。
     *   → **量の条件を足す。** 平均生存がこの値以下の行だけを見る。
     *     -9dB（0.35）。閉扉の実測 0.21（-13.5dB）は捕まえ、素通しは見ない。 */
    float phantomMaxLevel = 0.35f;
    float tailRatioLo = 0.0f;    // 型紙A。0 なら見ない（場面ごとに妥当値が違うため）
    float tailRatioHi = 0.0f;
};

/* 音源 1 本ぶんの列を切り出して型紙を当てる。
 *   ★音源ごとに見るのが要点。全音源を混ぜた 1 本の列にすると、
 *     「どれかが跳んだ」までしか分からず原因に辿り着けない。 */
inline void scanSource(const CaptureFile& cap, unsigned long long id,
                       const ScanOptions& opt, std::vector<Mark>& out) {
    const int n = cap.frames;
    std::vector<float> lvlDb, at, bands, tail, direct;
    std::vector<int> paths, frameNo;
    std::vector<unsigned char> occluded;
    lvlDb.reserve((size_t)n);

    for (int k = 0; k < n; ++k) {
        const auto& row = cap.sources[(size_t)k];
        for (const auto& s : row) {
            if (s.id != id) continue;
            float sum = 0.0f;
            for (int b = 0; b < acoustic::dbg::kCapBands; ++b) {
                bands.push_back(s.band[b]);
                sum += s.band[b];
            }
            lvlDb.push_back(af::detect::db(sum / acoustic::dbg::kCapBands));
            at.push_back((float)cap.global[(size_t)k].audioSample / (float)cap.sampleRate);
            frameNo.push_back((int)cap.global[(size_t)k].frame);
            // 型紙3 が見るのは「経路の本数」。★量ではなく**見つかったか**。
            //   直接音が通っている（遮蔽が弱い）行は経路 0 でも正常なので +1 しておく。
            paths.push_back((int)s.earlyTaps + (int)s.diffSrcs
                            + ((s.occ < opt.occludedAbove) ? 1 : 0));
            // ★この旗の意味は「遮蔽されている」ではなく
            //   **「この行は周波数依存が出ていなければおかしい」**。
            //   ＝ 遮蔽されていて、かつ実際にほとんど通っていない行。
            const float mean = sum / (float)acoustic::dbg::kCapBands;
            occluded.push_back((s.occ >= opt.occludedAbove && mean <= opt.phantomMaxLevel)
                               ? 1u : 0u);
            tail.push_back(s.tailLevel);
            direct.push_back(sum);
            break;
        }
    }
    const int m = (int)lvlDb.size();
    if (m < 2) return;

    std::vector<af::detect::Break> br((size_t)m + 8);
    auto push = [&](const char* tmpl, int cnt) {
        for (int i = 0; i < cnt; ++i) {
            Mark mk;
            const int j = br[(size_t)i].index;
            mk.frame = (j >= 0 && j < m) ? frameNo[(size_t)j] : 0;
            mk.seconds = br[(size_t)i].at;
            mk.sourceId = id;
            mk.templateName = tmpl;
            mk.what = br[(size_t)i].what;
            mk.amount = br[(size_t)i].amount;
            out.push_back(mk);
        }
    };

    push("型紙1 跳ばない",
         af::detect::noJump(lvlDb.data(), at.data(), m, opt.maxStepDb,
                            br.data(), (int)br.size()));

    // 型紙4 は「遮蔽されている行」だけ見る。素通しなら平らで正しい。
    //   ⚠ std::vector<bool> はビット詰めでポインタが取れないので、素の配列を使う。
    std::unique_ptr<bool[]> occFlags(new bool[(size_t)m]);
    for (int i = 0; i < m; ++i) occFlags[(size_t)i] = (occluded[(size_t)i] != 0);
    push("型紙4 幻が出ない",
         af::detect::noPhantom(bands.data(), occFlags.get(), at.data(), m, opt.minTiltDb,
                               br.data(), (int)br.size()));

    push("型紙3 経路が消えない",
         af::detect::pathAlive(paths.data(), at.data(), m, br.data(), (int)br.size()));

    if (opt.tailRatioHi > opt.tailRatioLo)
        push("型紙A 比が保たれる",
             af::detect::ratioInRange(tail.data(), direct.data(), at.data(), m,
                                      opt.tailRatioLo, opt.tailRatioHi,
                                      "tail/direct", br.data(), (int)br.size()));

    // 型紙D: 生存ゲインは振幅比。1.0 を超えたら物理的にありえない。
    push("型紙D 上限を超えない",
         af::detect::withinPhysicalBound(bands.data(), nullptr,
                                         m * acoustic::dbg::kCapBands, 1.0f,
                                         br.data(), (int)br.size()));
}

/* キャプチャ全体を走査する。音源ごとに型紙を当てて印を集める。 */
inline std::vector<Mark> scan(const CaptureFile& cap, const ScanOptions& opt = ScanOptions{}) {
    std::vector<Mark> out;
    std::vector<unsigned long long> ids;
    for (const auto& row : cap.sources)
        for (const auto& s : row) {
            bool seen = false;
            for (auto id : ids) if (id == s.id) { seen = true; break; }
            if (!seen) ids.push_back(s.id);
        }
    for (auto id : ids) scanSource(cap, id, opt, out);
    return out;
}

/* 連続した同じ印をひとまとめにする。
 *   ★実機のキャプチャで要ると分かった（2026-08-24）。閉扉の幻は**何秒も続く**ので、
 *     生のままだと 381 件並んで読めない。1 件に見えると「1 フレームの瞬き」と
 *     誤解するので、**続いた長さ**を持たせること（そこが重要な情報）。
 *   ⚠ まとめるのは表示だけ。検査（回帰テスト）は生の列を見る。 */
struct MarkRun {
    Mark first;          // 先頭の印（型紙・音源・何が動いたか）
    int  lastFrame = 0;
    float lastSeconds = 0.0f;
    int  count = 1;
    float worst = 0.0f;  // 続いたあいだの最大の破れ量
};

inline std::vector<MarkRun> groupRuns(const std::vector<Mark>& marks) {
    std::vector<MarkRun> out;
    for (const Mark& m : marks) {
        bool merged = false;
        for (MarkRun& r : out) {
            if (r.first.sourceId != m.sourceId) continue;
            if (std::strcmp(r.first.templateName, m.templateName) != 0) continue;
            if (m.frame != r.lastFrame + 1) continue;      // 連続していなければ別の run
            r.lastFrame = m.frame;
            r.lastSeconds = m.seconds;
            ++r.count;
            if (m.amount > r.worst) r.worst = m.amount;
            merged = true;
            break;
        }
        if (!merged) {
            MarkRun r;
            r.first = m; r.lastFrame = m.frame; r.lastSeconds = m.seconds;
            r.count = 1; r.worst = m.amount;
            out.push_back(r);
        }
    }
    return out;
}

/* 人が読める形で出す。★時刻だけでなく「何が動いたか」を必ず出す。 */
inline void printMarks(const std::vector<Mark>& marks, int maxLines = 8) {
    if (marks.empty()) { std::printf("        印なし\n"); return; }
    std::printf("        印 %d 件\n", (int)marks.size());
    const int n = (int)marks.size() < maxLines ? (int)marks.size() : maxLines;
    for (int i = 0; i < n; ++i)
        std::printf("          f%-5d t=%6.2fs  音源 %llu  %s  %s %.3f\n",
                    marks[(size_t)i].frame, marks[(size_t)i].seconds,
                    (unsigned long long)marks[(size_t)i].sourceId,
                    marks[(size_t)i].templateName, marks[(size_t)i].what,
                    marks[(size_t)i].amount);
    if ((int)marks.size() > n) std::printf("          … 他 %d 件\n", (int)marks.size() - n);
}

}  // namespace scan
}  // namespace af

#endif  // ACOUSTICFLOW_TEST_CAPTURE_SCAN_H
