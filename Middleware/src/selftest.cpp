#include "selftest.h"

#include "audio_device.h"
#include "block_source.h"
#include "voice_bus.h"
#include "voice_slots.h"
#include "command_queue.h"
#include "voice_pool.h"
#include "host_util.h"
#include "module.h"
#include "project.h"
#include "undo.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <utility>
#include <cstdio>

namespace af {
namespace {

// 検査用に小さな WAV を書く。
// ★読む側の検査に、書く側の道具を使い回さない ── ここは検査のためだけの最小実装。
//   （本物の書き出しが要るのは資産を焼く段で、その時は別に作る）
bool writeTestWav(const std::wstring& path, int rate, int channels, int frames) {
    FILE* f = nullptr;
    if (::_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;

    const int bits = 16;
    const int blockAlign = channels * bits / 8;
    const int dataBytes  = frames * blockAlign;

    auto u32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16);
    u16(1); u16(static_cast<unsigned short>(channels));
    u32(static_cast<unsigned>(rate));
    u32(static_cast<unsigned>(rate * blockAlign));
    u16(static_cast<unsigned short>(blockAlign)); u16(static_cast<unsigned short>(bits));
    std::fwrite("data", 1, 4, f); u32(static_cast<unsigned>(dataBytes));

    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const short s = static_cast<short>(std::sin(6.283185307179586 * 440.0 * t) * 12000);
        for (int c = 0; c < channels; ++c) std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

}  // namespace

SelfTestReport runSelfTest(const std::wstring& target) {
    SelfTestReport r;
    auto check = [&](bool ok, const char* what, const std::string& detail) {
        r.checks.push_back({ok, what, detail});
        if (!ok) ++r.failed;
    };

    char d[256];

    // ① 形式を見る（デバイスを開かずに）
    int rate = 0, ch = 0;
    std::string perr;
    const bool probed = AudioDevice::probeDefaultFormat(&rate, &ch, &perr);
    if (probed) ::sprintf_s(d, "%d Hz / %d ch", rate, ch);
    else        ::sprintf_s(d, "%s", perr.c_str());
    check(probed, "既定の出力の形式を見る（開かずに）", d);
    if (!probed) rate = 48000;

    // ② デバイスを開いて実時間スレッドを回す
    AudioDevice device;
    ToneSource  tone(static_cast<double>(rate));
    std::string err;
    const bool opened = device.start(&tone, &err);
    if (opened)
        ::sprintf_s(d, "%s / 輪 %d サンプル", device.deviceName().c_str(), device.bufferFrames());
    else
        ::sprintf_s(d, "%s", err.c_str());
    check(opened, "出力デバイスを開く", d);

    if (opened) {
        // ③ 1 秒鳴らして、ブロックが実際に回ったかを数える
        tone.setActive(true);
        ::Sleep(1000);
        tone.setActive(false);
        ::Sleep(100);

        const unsigned long long n = device.callbacks();
        const int fpc = device.framesPerCallback();
        ::sprintf_s(d, "%llu 回 / 1 回 %d サンプル（%.2f ms）/ 最悪 %.4f ms＝持ち時間の %.3f %% / "
                       "間に合わなかった回 %llu",
                    n, fpc, fpc > 0 ? 1000.0 * fpc / device.sampleRate() : 0.0,
                    device.worstBlockMs(), device.worstLoadPercent(), device.lateBlocks());
        check(n > 0, "実時間スレッドがブロックを回した", d);
        check(device.lateBlocks() == 0, "全ブロックが持ち時間内に収まった", "");
        check(tone.settled(), "止めたあと傾斜が下り切った（無音に達した）", "");
    }

    // ④ プロジェクトの往復（保存 → 読み直しで同じ値になるか）
    //
    // ★オーサリングツールの芯は「開いて・直して・保存できる」ことなので、
    //   ここが壊れたら道具として成立していません。UI を通さずに確かめられます。
    {
        wchar_t dir[MAX_PATH + 1] = {};
        ::GetTempPathW(MAX_PATH, dir);
        const std::wstring p = std::wstring(dir) + L"afhost_selftest.afproj";

        Project a;
        a.name       = "検査用プロジェクト";
        a.toneHz     = 311.13f;
        a.toneLevel  = 0.234f;
        a.toneOnOpen = true;

        std::string e;
        const bool saved = a.save(p, &e);

        Project b;
        const bool loaded = saved && b.load(p, &e);
        const bool same = loaded && b.name == a.name && b.toneOnOpen == a.toneOnOpen &&
                          std::fabs(b.toneHz - a.toneHz) < 0.01f &&
                          std::fabs(b.toneLevel - a.toneLevel) < 0.0001f;

        if (same) ::sprintf_s(d, "%s / %.2f Hz / %.3f", b.name.c_str(), b.toneHz, b.toneLevel);
        else      ::sprintf_s(d, "%s", e.empty() ? "値が一致しません" : e.c_str());
        check(same, "プロジェクトを保存して読み直すと同じ値になる", d);

        // ★別物のファイルを掴まされたら断ること。
        //   黙って既定値で開くと、直したはずの設定が消えたように見える。
        {
            const std::wstring bad = std::wstring(dir) + L"afhost_selftest_bad.txt";
            FILE* f = nullptr;
            if (::_wfopen_s(&f, bad.c_str(), L"wb") == 0 && f) {
                std::fputs("これはプロジェクトではありません\n", f);
                std::fclose(f);
                Project c;
                check(!c.load(bad, &e), "プロジェクトでないファイルは断る", "");
                ::DeleteFileW(bad.c_str());
            }
        }

        ::DeleteFileW(p.c_str());
    }


    // ⑧ 札の寿命（ハンドルを扱うコードのバグの大半がここから出る）
    {
        afr::VoiceSlots slots;
        slots.reset(3);

        const AFR_Voice a = slots.acquire();
        const AFR_Voice b = slots.acquire();
        const AFR_Voice c = slots.acquire();
        const AFR_Voice none = slots.acquire();      // 枠は 3 つしかない
        ::sprintf_s(d, "3 枠すべて配った／4 本目は無効／生きている %d 本",
                    slots.liveCount());
        check(a && b && c && none == AFR_VOICE_NONE && slots.liveCount() == 3,
              "枠のぶんだけ札を配り、足りなければ無効を返す", d);

        // ★★ ここが肝 ── 手放した枠を配り直したとき、古い札が弾かれること。
        //   ⚠ 世代を持たないと、古い札で Stop を呼んで**無関係な音が止まります**。
        const int   idxA = slots.indexOf(a);
        const bool  freed = slots.release(a);
        const AFR_Voice a2 = slots.acquire();        // 同じ枠が返ってくるはず
        const bool sameSlot = slots.indexOf(a2) == idxA;
        const bool oldDead  = !slots.valid(a);
        const bool newAlive =  slots.valid(a2);
        ::sprintf_s(d, "枠 %d を配り直した／古い札は無効 %s／新しい札は有効 %s",
                    idxA, oldDead ? "○" : "✕", newAlive ? "○" : "✕");
        check(freed && sameSlot && oldDead && newAlive,
              "使い回した枠で、古い札は弾かれる（世代で見分ける）", d);

        // 二度手放しをその場で止めること（数え違いの元）。
        const bool twice = slots.release(a);
        ::sprintf_s(d, "生きている %d 本（3 のまま）", slots.liveCount());
        check(!twice && slots.liveCount() == 3, "同じ札で二度手放せない", d);

        // ⚠ 世代を 0 から始めると、枠 0 の最初の札が 0 ＝「無効」と同じ値になる。
        afr::VoiceSlots s2;
        s2.reset(1);
        const AFR_Voice first = s2.acquire();
        ::sprintf_s(d, "枠 0 の最初の札 = %llu（0 なら「無効」と区別が付かない）",
                    static_cast<unsigned long long>(first));
        check(first != AFR_VOICE_NONE && s2.valid(first),
              "枠 0 の最初の札が「無効」と同じ値にならない", d);
    }

    // ⑨ 指示の輪（ゲーム → オーディオ）
    {
        afr::CommandQueue q;
        q.reset(8);
        afr::Command c;

        // 積んだ順に出ること（FIFO）。
        for (unsigned i = 0; i < 5; ++i) {
            afr::Command in;
            in.type = afr::Cmd::Play;
            in.u    = i;
            q.push(in);
        }
        bool inOrder = true;
        for (unsigned i = 0; i < 5; ++i)
            if (!q.pop(&c, 0) || c.u != i) { inOrder = false; break; }
        ::sprintf_s(d, "5 個を積んで 5 個とも同じ順で出た");
        check(inOrder && !q.pop(&c, 0), "積んだ順に取り出せる（空になったら false）", d);

        // ★満杯では**新しいほうを捨てる**。古いほうを上書きしない。
        //   ⚠ 上書きすると、いちばん古い Play（ループを始めた指示）が消えて
        //     **止められないループ音**が残ります。
        afr::CommandQueue q2;
        q2.reset(4);
        const int cap = q2.capacity();                  // ★指定以上（2 のべき乗 -1）
        for (unsigned i = 0; i < 10; ++i) {
            afr::Command in;
            in.type = afr::Cmd::Play;
            in.u    = i;
            q2.push(in);
        }
        const bool oldestKept = q2.pop(&c, 0) && c.u == 0;   // いちばん古いが生きている
        ::sprintf_s(d, "10 個積んで %d 個入り／積めなかった %llu 回／先頭は %u（0 なら古いほうが生きている）",
                    cap, q2.failedPushes(), c.u);
        check(cap >= 4 && oldestKept &&
              q2.failedPushes() == static_cast<unsigned long long>(10 - cap),
              "満杯では新しいほうを捨て、数える（古いほうを上書きしない）", d);

        // 時刻が来ていないものは出てこないこと。
        afr::CommandQueue q3;
        q3.reset(4);
        afr::Command later;
        later.type  = afr::Cmd::Stop;
        later.frame = 1000;
        q3.push(later);
        const bool notYet = !q3.pop(&c, 999);
        const bool nowOk  =  q3.pop(&c, 1000);
        ::sprintf_s(d, "999 では出ず、1000 で出た");
        check(notYet && nowOk, "時刻が来るまで取り出さない", d);
    }

    // ⑩ 輪を 2 スレッドで回す（ここが本番の使い方）
    //   ⚠ 1 スレッドで積んで取り出しても、**メモリ順序の間違いは出ません**。
    //     release/acquire を relaxed にしても 1 スレッドなら通ってしまう。
    //     実際に別スレッドで回して、落ちも順序の乱れも無いことを見ます。
    {
        afr::CommandQueue q;
        q.reset(256);
        const unsigned kTotal = 50000;

        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (unsigned i = 0; i < kTotal; ++i) {
                afr::Command in;
                in.type = afr::Cmd::Play;
                in.u    = i;
                in.a    = static_cast<float>(i);   // 中身も一緒に見る
                while (!q.push(in)) {}             // 満杯なら空くまで粘る（積む側は待ってよい）
            }
        });

        go.store(true, std::memory_order_release);
        unsigned expect = 0;
        bool ok = true;
        afr::Command out;
        while (expect < kTotal) {
            if (!q.pop(&out, 0)) continue;
            // ★番号だけでなく中身も見る。番号が先に見えて中身が空、が典型の壊れ方。
            if (out.u != expect || out.a != static_cast<float>(expect)) { ok = false; break; }
            ++expect;
        }
        producer.join();
        ::sprintf_s(d, "%u 個を別スレッドから積んで、順序も中身も崩れなかった（積み直し %llu 回）",
                    kTotal, q.failedPushes());
        check(ok && expect == kTotal,
              "別スレッドから積んでも順序と中身が崩れない", d);
    }

    // ⑪ ボイスプール（上限・優先度・奪取・奪取時のフェード）
    {
        // ⚠ 0.5 にすると 3 本で 1.5 → 挟み込みで 1.0 になり、**2 本でも 3 本でも 1.0**。
        //   硬く切れていても通ってしまうので、溢れない高さにする。
        auto flat = [] { return std::vector<float>(441 * 2 * 20, 0.2f); };

        afr::VoicePool pool;
        pool.reset(3, 44100);
        pool.prepare(441, 2);

        // 上限まで配れること。
        const AFR_Voice v0 = pool.acquire(10);
        const AFR_Voice v1 = pool.acquire(10);
        const AFR_Voice v2 = pool.acquire(10);
        for (AFR_Voice v : {v0, v1, v2}) {
            af::ClipSource* s = pool.find(v);
            s->setClip(flat(), 2, 44100);
            s->play();
        }
        ::sprintf_s(d, "上限 %d に対して %d 本", pool.capacity(), pool.liveCount());
        check(v0 && v1 && v2 && pool.liveCount() == 3, "上限まで枠を配る", d);

        // ⚠ 先に鳴らしておくこと。包絡は 0 から立ち上がるので、
        //   **一度も render していない音を奪っても、落ちる高さがありません**
        //   （検査が「即 0 になった」と誤読します）。
        std::vector<float> b(441 * 2, 0.0f);
        pool.render(b.data(), 441, 2);
        pool.render(b.data(), 441, 2);

        // ★★ 満杯で奪う ── **枠は即座に空き、奪われた音は傾斜で下りる**。
        //   ⚠ ここが「待つ」か「プチッと鳴る」になるのが素直な書き方の結末。
        const AFR_Voice v3 = pool.acquire(10);      // いちばん古い v0 が奪われるはず
        const bool gotSlot = (v3 != AFR_VOICE_NONE);
        const bool oldDead = !pool.valid(v0);
        ::sprintf_s(d, "新しい札が即座に取れた %s／古い札は無効 %s／傾斜中 %d 本／硬く止めた %llu 回",
                    gotSlot ? "○" : "✕", oldDead ? "○" : "✕",
                    pool.fadingCount(), pool.hardStops());
        check(gotSlot && oldDead && pool.fadingCount() == 1 && pool.hardStops() == 0,
              "満杯でも待たずに奪える（奪われた音は傾斜で下りる）", d);

        // ★奪われた音が**即 0 になっていない**こと。
        //   ⚠ 残り 2 本も鳴っているので、奪った 1 本ぶんが乗っているかを見る。
        std::vector<float> after(441 * 2, 0.0f);
        pool.render(after.data(), 441, 2);
        ::sprintf_s(d, "奪った直後のブロック頭 %.4f（3 本ぶん 0.6 ／ 硬く切れていれば 0.4）",
                    after[0]);
        check(after[0] > 0.5f, "奪われた音は即 0 にならず傾斜で落ちる", d);

        // ⚠ 優先度が上のものは奪わないこと。
        //   ここを見ないと、遠くの環境音がセリフを殺します。
        afr::VoicePool p2;
        p2.reset(2, 44100);
        p2.prepare(441, 2);
        const AFR_Voice hi0 = p2.acquire(100);
        const AFR_Voice hi1 = p2.acquire(100);
        for (AFR_Voice v : {hi0, hi1}) { p2.find(v)->setClip(flat(), 2, 44100); p2.find(v)->play(); }
        const AFR_Voice low = p2.acquire(1);        // 自分より上しか居ない
        ::sprintf_s(d, "優先度 100 が 2 本埋めている所へ優先度 1 が来た → %s",
                    low == AFR_VOICE_NONE ? "断った" : "奪ってしまった");
        check(low == AFR_VOICE_NONE && p2.valid(hi0) && p2.valid(hi1),
              "自分より優先度が高いものは奪わない", d);

        // ⚠ 予備が尽きたら硬く止めて**数える**こと。
        //   数えないと「たまにプチッと鳴る」としか分かりません。
        afr::VoicePool p3;
        p3.reset(1, 44100);
        p3.prepare(441, 2);
        for (int i = 0; i < afr::VoicePool::kFadeReserve + 3; ++i) {
            const AFR_Voice v = p3.acquire(10);
            if (v == AFR_VOICE_NONE) break;
            p3.find(v)->setClip(flat(), 2, 44100);
            p3.find(v)->play();
        }
        ::sprintf_s(d, "予備 %d 本を使い切ったあと、硬く止めた %llu 回（数えている）",
                    afr::VoicePool::kFadeReserve, p3.hardStops());
        check(p3.hardStops() > 0, "予備が尽きたら硬く止めて、その回数を数える", d);
    }
    // ⑤ SoundLibrary（WAV を読む → 台帳に載る → 保存して読み直しても残る）
    {
        wchar_t dir[MAX_PATH + 1] = {};
        ::GetTempPathW(MAX_PATH, dir);
        const std::wstring wav  = std::wstring(dir) + L"afhost_selftest.wav";
        const std::wstring proj = std::wstring(dir) + L"afhost_selftest_bank.afproj";

        // 48000 Hz / 2ch / 0.25 秒
        const bool wrote = writeTestWav(wav, 48000, 2, 12000);
        check(wrote, "検査用の WAV を書く", "");

        if (wrote) {
            WavInfo info;
            std::string e;
            const bool read = readWavInfo(wav, &info, &e);
            const bool right = read && info.sampleRate == 48000 && info.channels == 2 &&
                               info.bits == 16 && info.frames == 12000;
            if (right) ::sprintf_s(d, "%d Hz / %d ch / %s / %.3f 秒",
                                   info.sampleRate, info.channels,
                                   info.formatLine().c_str(), info.seconds());
            else       ::sprintf_s(d, "%s", e.empty() ? "素性が食い違います" : e.c_str());
            check(right, "WAV の素性を読む", d);

            // ★台帳がプロジェクトの往復で残るか。
            //   素性は保存していないので、読み直したあとに**ファイルから引けているか**まで見る。
            {
                Project p;
                p.name = "台帳の検査";
                std::string pe;
                const bool added = p.library.add(wav, std::string(), &pe);
                check(added, "台帳に取り込む", added ? "" : pe);

                p.library.addFolder("性質");
                p.library.addFolder("性質/金属");
                p.library.setFolder(0, "性質/金属");
                p.library.setGain(0, -4.5f);
                p.library.setLoop(0, true);
                // ★調整は 4 つとも往復させること。1 つだけ見ていると、
                //   書き忘れた項目が黙って既定に戻ります。
                p.library.setPitch(0, -3.5f);
                p.library.setLpf(0, 4200.0f);
                p.library.setHpf(0, 180.0f);

                Project q;
                const bool ok = added && p.save(proj, &pe) && q.load(proj, &pe);
                const bool kept = ok && q.library.size() == 1 &&
                                  q.library.entries()[0].name == p.library.entries()[0].name &&
                                  q.library.entries()[0].folder == "性質/金属" &&
                                  q.library.entries()[0].loop &&
                                  q.library.entries()[0].gainDb == -4.5f &&
                                  q.library.entries()[0].pitch  == -3.5f &&
                                  q.library.entries()[0].lpfHz  == 4200.0f &&
                                  q.library.entries()[0].hpfHz  == 180.0f &&
                                  q.library.entries()[0].resolved &&
                                  q.library.entries()[0].info.sampleRate == 48000 &&
                                  q.library.folders().size() == 2;
                if (kept) ::sprintf_s(d,
                                      "%s / %s / %.1f dB / %+.1f 半音 / LPF %.0f / HPF %.0f Hz",
                                      q.library.entries()[0].name.c_str(),
                                      q.library.entries()[0].folder.c_str(),
                                      q.library.entries()[0].gainDb,
                                      q.library.entries()[0].pitch,
                                      q.library.entries()[0].lpfHz,
                                      q.library.entries()[0].hpfHz);
                else      ::sprintf_s(d, "%s", pe.empty() ? "台帳が残っていません" : pe.c_str());
                check(kept, "台帳をプロジェクトに保存して読み直す", d);

                // ★別のフォルダへ保存しても素材を見失わないこと。
                //   在り処はプロジェクトからの相対なので、**保存先が変わると基準も変わる**。
                //   ここを取り直していないと、別フォルダへ保存した瞬間に全部見失う。
                {
                    const std::wstring sub = std::wstring(dir) + L"afhost_selftest_sub";
                    ::CreateDirectoryW(sub.c_str(), nullptr);
                    const std::wstring p2 = sub + L"\\moved.afproj";

                    Project m = q;
                    const bool ms = m.save(p2, &pe);
                    m.library.refresh();
                    const bool mk = ms && m.library.size() == 1 &&
                                    m.library.entries()[0].resolved;
                    check(mk, "別のフォルダへ保存しても素材を見失わない",
                          mk ? wideToUtf8(m.library.entries()[0].path) : pe);

                    ::DeleteFileW(p2.c_str());
                    ::RemoveDirectoryW(sub.c_str());
                }

                // ★フォルダを消しても中身は消えない。ひとつ上へ移る。
                q.library.removeFolder("性質/金属");
                check(q.library.size() == 1 && q.library.entries()[0].folder == "性質",
                      "フォルダを消しても中身は残り、ひとつ上へ移る",
                      q.library.size() == 1 ? q.library.entries()[0].folder : "消えました");

                // ★入れ子ごと改名できること。
                // ⚠ 文字数ではなく**バイト数**で比べること（"材質" は UTF-8 で 6 バイト）。
                q.library.addFolder("性質/木");
                q.library.renameFolder("性質", "材質");
                bool allMoved = q.library.entries()[0].folder == "材質";
                for (const std::string& f : q.library.folders())
                    if (f.rfind("材質", 0) != 0) allMoved = false;
                check(allMoved, "フォルダを改名すると入れ子ごと付いてくる",
                      allMoved ? q.library.entries()[0].folder : "迷子が出ました");

                // ★イベント ── ゲーム側からの発火点。
                //   名前で呼ばれ、番号で音源を指す。**改名しても繋がったまま**が要。
                {
                    Project ep;
                    std::string ee;
                    ep.name = "イベントの検査";
                    ep.library.add(wav, std::string(), &ee);
                    const unsigned sid = ep.library.entries()[0].id;

                    const int ei = ep.events.add("足音_石");
                    ep.events.addAction(static_cast<std::size_t>(ei), ActionType::Play, sid);
                    ep.events.addAction(static_cast<std::size_t>(ei), ActionType::Stop, sid);

                    // 同じ名前を作らせない（ゲーム側は名前で呼ぶので、重なると
                    // どちらが鳴るか分からなくなる）。
                    ep.events.add("足音_石");
                    const bool uniq = ep.events.size() == 2 &&
                                      ep.events.all()[1].name != ep.events.all()[0].name;
                    check(uniq, "同じ名前のイベントを作らせない",
                          uniq ? ep.events.all()[1].name : "重なりました");

                    const std::wstring epath = std::wstring(dir) + L"afhost_selftest_ev.afproj";
                    Project eq;
                    const bool ok2 = ep.save(epath, &ee) && eq.load(epath, &ee);
                    const bool kept2 = ok2 && eq.events.size() == 2 &&
                                       eq.events.all()[0].name == "足音_石" &&
                                       eq.events.all()[0].actions.size() == 2 &&
                                       eq.events.all()[0].actions[0].type == ActionType::Play &&
                                       eq.events.all()[0].actions[1].type == ActionType::Stop &&
                                       eq.events.all()[0].actions[0].soundId == sid;
                    ::sprintf_s(d, "%s / 動作 %d 個",
                                eq.events.size() ? eq.events.all()[0].name.c_str() : "?",
                                eq.events.size() ? static_cast<int>(
                                    eq.events.all()[0].actions.size()) : 0);
                    check(kept2, "イベントを保存して読み直す", kept2 ? d : ee);

                    // ★音源を改名しても、イベントの指す先は外れない（番号で指しているため）。
                    eq.library.rename(0, "別の名前にした");
                    const int found = eq.library.indexOfId(eq.events.all()[0].actions[0].soundId);
                    check(found == 0, "音源を改名してもイベントの指す先は外れない",
                          found == 0 ? eq.library.entries()[0].name : "外れました");

                    // ★対象を外したら「見つかりません」と分かること（黙って無音にしない）。
                    eq.library.remove(0);
                    check(eq.library.indexOfId(eq.events.all()[0].actions[0].soundId) < 0,
                          "対象を外すと、イベントから見て「見つからない」になる", "");

                    ::DeleteFileW(epath.c_str());
                }

                // ★元に戻す／やり直す。
                //   台帳とイベントをまとめた写しを積む形なので、
                //   **戻し方を操作ごとに書く必要がない**。
                //   その代わり、写しが本当に元の姿へ戻るかをここで確かめます。
                {
                    UndoStack u;
                    SoundLibrary& L = q.library;
                    EventList&    E = q.events;
                    const std::string before = L.entries()[0].folder;

                    // ⚠ この段の土台は自分で作ること。
                    //   イベントの検査は別のプロジェクトでやっているので、
                    //   ここの `q.events` は空です。空の先頭を触って一度落としました。
                    E.add("検査用イベント");
                    E.addAction(0, ActionType::Play, L.entries()[0].id);

                    u.push("移動", L, E);
                    L.setFolder(0, "どこか別の場所");
                    const bool changed = L.entries()[0].folder == "どこか別の場所";

                    const bool undone = u.undo(&L, &E) && L.entries()[0].folder == before;
                    const bool redone = u.redo(&L, &E) && L.entries()[0].folder == "どこか別の場所";

                    ::sprintf_s(d, "%s → 変更 → 戻す → やり直す", before.c_str());
                    check(changed && undone && redone, "元に戻す・やり直すで姿が往復する", d);

                    // ★イベントも同じ 1 段で戻ること。
                    //   ここが台帳と別だった頃は、イベントを消すと戻せませんでした。
                    const std::size_t evBefore = E.size();
                    u.push("イベントを消す", L, E);
                    E.remove(0);
                    const bool evGone = E.size() + 1 == evBefore;
                    const bool evBack = u.undo(&L, &E) && E.size() == evBefore;
                    ::sprintf_s(d, "%zu 個 → 消す → 戻す → %zu 個", evBefore, E.size());
                    check(evGone && evBack, "イベントも元に戻せる", d);

                    // ★★片方だけ戻らないこと。ここが T26 の肝です。
                    //   イベントは音源を**番号**で指しているので、台帳だけ戻すと
                    //   戻した先に居ない音源を指した状態になり、
                    //   **「見つかりません」が勝手に生えます**。1 段で両方動くのを確かめます。
                    {
                        const std::size_t libBefore = L.size();
                        const std::size_t actBefore = E.all()[0].actions.size();

                        u.push("両方を変える", L, E);
                        L.remove(0);                              // 台帳から 1 つ外し
                        E.addAction(0, ActionType::Play, 12345);  // 同時にイベントへ動作を足す
                        const bool bothChanged = L.size() + 1 == libBefore &&
                                                 E.all()[0].actions.size() == actBefore + 1;

                        const bool bothBack = u.undo(&L, &E) &&
                                              L.size() == libBefore &&
                                              E.all()[0].actions.size() == actBefore;

                        ::sprintf_s(d, "台帳 %zu 件・動作 %zu 個が揃って戻った",
                                    L.size(), E.all()[0].actions.size());
                        check(bothChanged && bothBack,
                              "台帳とイベントが同じ 1 段で戻る（片方だけ戻らない）", d);
                    }

                    // ★新しく変えたら、やり直しの先は捨てること。
                    //   残すと「戻して・別のことをして・やり直す」で辻褄が合わなくなる。
                    u.undo(&L, &E);
                    u.push("外す", L, E);
                    check(!u.canRedo(), "戻したあとに別の操作をすると、やり直しの先は捨てられる",
                          u.canRedo() ? "残っています" : "");

                    // 段の上限を超えても壊れないこと（古いほうから捨てる）。
                    UndoStack u2;
                    for (std::size_t i = 0; i < UndoStack::kMax + 10; ++i) u2.push("反復", L, E);
                    ::sprintf_s(d, "%zu 段（上限 %zu）", u2.depth(), UndoStack::kMax);
                    check(u2.depth() == UndoStack::kMax, "段が上限を超えたら古いほうから捨てる", d);

                    // 検査で動かしたぶんを戻しておく（この下の項目が前提にしている）。
                    L.setFolder(0, before);
                    E.clear();
                }

                // ★素材が消えたら黙って空欄にせず、読めないと分かること。
                ::DeleteFileW(wav.c_str());
                q.library.refresh();
                check(!q.library.entries()[0].resolved && !q.library.entries()[0].problem.empty(),
                      "素材が消えたら、消えたと分かる", q.library.entries()[0].problem);

                ::DeleteFileW(proj.c_str());
            }

            // ★試聴の経路をデバイス抜きで通す。
            //   画面を押さないと確かめられない機能は、結局誰も確かめません。
            {
                WavInfo wi;
                std::vector<float> pcm;
                std::string e3;
                // 上で消してしまったので書き直す
                writeTestWav(wav, 48000, 2, 12000);
                const bool got = readWav(wav, &wi, &pcm, &e3);

                // 44100 / 2ch のデバイスに合わせる（周波数が違う＝変換が要る側）
                std::vector<float> conf = conformToDevice(pcm, wi, 44100, 2);
                const std::size_t want = static_cast<std::size_t>(12000.0 * 44100 / 48000) * 2;
                const bool sized = got && conf.size() >= want - 4 && conf.size() <= want + 4;
                ::sprintf_s(d, "48000→44100 / %zu サンプル（想定 %zu）", conf.size(), want);
                check(sized, "デバイスの周波数に合わせ直す", d);

                // ClipSource に入れて、実際にブロックを取り出す。
                ClipSource cs;
                const std::size_t n = conf.size();
                cs.setClip(std::move(conf), 2, 44100);
                cs.setActive(true);

                std::vector<float> blk(441 * 2, 0.0f);
                float peak = 0.0f;
                int blocks = 0;
                while (!cs.finished() && blocks < 200) {
                    cs.render(blk.data(), 441, 2);
                    for (float v : blk) { const float a = v < 0 ? -v : v; if (a > peak) peak = a; }
                    ++blocks;
                }
                ::sprintf_s(d, "%d ブロック取り出して山 %.3f / %zu サンプルを鳴らし切った",
                            blocks, peak, n);
                check(blocks > 0 && peak > 0.05f && cs.finished(),
                      "取り込んだ音をブロックで取り出せる", d);

                // ★調整が**実際に音を変えているか**を測る。
                //   つまみが動くだけでは意味がありません。
                //   ⚠ 素材は 1kHz の正弦（この上で作っている検査用 WAV）なので、
                //     そこを跨ぐように切って、通る／通らないを見ます。
                {
                    // ピッチ ── 読む速さが変わるので、鳴り切るまでのブロック数が変わる。
                    auto blocksToFinish = [&](float semi, float lpf, float hpf) {
                        std::vector<float> c2 = conformToDevice(pcm, wi, 44100, 2);
                        ClipSource s;
                        s.setClip(std::move(c2), 2, 44100);
                        s.setPitchSemitones(semi);
                        s.setLowPassHz(lpf);
                        s.setHighPassHz(hpf);
                        s.setActive(true);
                        std::vector<float> b(441 * 2, 0.0f);
                        int k = 0;
                        double sum = 0.0;
                        std::size_t cnt = 0;
                        while (!s.finished() && k < 400) {
                            s.render(b.data(), 441, 2);
                            for (float v : b) { sum += static_cast<double>(v) * v; ++cnt; }
                            ++k;
                        }
                        const double rms = cnt ? std::sqrt(sum / static_cast<double>(cnt)) : 0.0;
                        return std::pair<int, double>(k, rms);
                    };

                    const auto plain = blocksToFinish(0.0f,  ClipSource::kLpOff, ClipSource::kHpOff);
                    const auto up    = blocksToFinish(12.0f, ClipSource::kLpOff, ClipSource::kHpOff);
                    // 1 オクターブ上 = 倍の速さ = およそ半分の時間。
                    const bool halved = up.first > 0 && plain.first > 0 &&
                                        up.first <= plain.first * 0.6 + 2 &&
                                        up.first >= plain.first * 0.4 - 2;
                    ::sprintf_s(d, "±0 で %d ブロック → +12 半音で %d ブロック（およそ半分）",
                                plain.first, up.first);
                    check(halved, "ピッチを上げると読む速さが変わる", d);

                    // 低域通過を 1kHz より下へ置けば、1kHz の素材は目に見えて痩せる。
                    const auto lp = blocksToFinish(0.0f, 200.0f, ClipSource::kHpOff);
                    const bool cutHigh = plain.second > 0.0 && lp.second < plain.second * 0.35;
                    ::sprintf_s(d, "素通り %.4f → 200Hz で切って %.4f", plain.second, lp.second);
                    check(cutHigh, "低域通過が高い方を落とす", d);

                    // 高域通過も同じく、1kHz より上へ置けば痩せる。
                    const auto hp = blocksToFinish(0.0f, ClipSource::kLpOff, 6000.0f);
                    const bool cutLow = plain.second > 0.0 && hp.second < plain.second * 0.35;
                    ::sprintf_s(d, "素通り %.4f → 6kHz で切って %.4f", plain.second, hp.second);
                    check(cutLow, "高域通過が低い方を落とす", d);

                    // ⚠ 端に置いたら**素通り**であること。
                    //   掛けっぱなしだと、切っているつもりで位相だけ回ります。
                    const auto off = blocksToFinish(0.0f, ClipSource::kLpOff, ClipSource::kHpOff);
                    const double diff = plain.second > 0.0
                                            ? std::fabs(off.second - plain.second) / plain.second
                                            : 1.0;
                    ::sprintf_s(d, "端では %.6f の差（＝掛かっていない）", diff);
                    check(diff < 1e-6, "つまみを端に置くとフィルタは素通りする", d);
                }

                // ★片耳ずつ黙らせるのが**本当に片側だけ**に効くか。
                //   ⚠ 両方黙ったり、切ったつもりで鳴っていたりすると、
                //     聴き比べの道具として意味がありません。
                {
                    std::vector<float> c3 = conformToDevice(pcm, wi, 44100, 2);
                    ClipSource s;
                    s.setClip(std::move(c3), 2, 44100);
                    s.setChannelMute(0, true);          // 左だけ黙らせる
                    s.setActive(true);

                    std::vector<float> b(441 * 2, 0.0f);
                    double sl = 0.0, sr = 0.0;
                    std::size_t cnt = 0;
                    int k = 0;
                    // ★傾斜のぶん（約 5ms）は数えない。落ち切る前を混ぜると
                    //   「黙っていない」と読めてしまいます。
                    while (!s.finished() && k < 200) {
                        s.render(b.data(), 441, 2);
                        if (k >= 2) {
                            for (int i = 0; i < 441; ++i) {
                                sl += static_cast<double>(b[i * 2 + 0]) * b[i * 2 + 0];
                                sr += static_cast<double>(b[i * 2 + 1]) * b[i * 2 + 1];
                                ++cnt;
                            }
                        }
                        ++k;
                    }
                    const double rl = cnt ? std::sqrt(sl / static_cast<double>(cnt)) : 0.0;
                    const double rr = cnt ? std::sqrt(sr / static_cast<double>(cnt)) : 0.0;
                    ::sprintf_s(d, "左 %.8f ／ 右 %.4f（実効値）", rl, rr);
                    check(rl < 1e-6 && rr > 0.05, "片側を黙らせても、もう片方は鳴っている", d);

                    // 傾斜で落ちること（1 ブロック目にいきなり 0 にしていない）。
                    ClipSource s2;
                    std::vector<float> c4 = conformToDevice(pcm, wi, 44100, 2);
                    s2.setClip(std::move(c4), 2, 44100);
                    s2.setActive(true);
                    s2.render(b.data(), 441, 2);        // まず普通に 1 ブロック
                    s2.setChannelMute(0, true);
                    s2.render(b.data(), 441, 2);        // 切った直後のブロック
                    bool zeroAtOnce = true;
                    for (int i = 0; i < 8; ++i)
                        if (b[i * 2] != 0.0f) { zeroAtOnce = false; break; }
                    ::sprintf_s(d, "切った直後も先頭 8 サンプルは 0 ではない（傾斜で落ちる）");
                    check(!zeroAtOnce, "黙らせるとき一気に 0 にせず傾斜で落とす", d);
                }

                // ★打ち込みは範囲の外も来る（画面が数字入力になったため）。
                //   ⚠ 挟まずに渡すと、20kHz を超える切り位置や
                //     半音 999 のような値がそのまま実時間スレッドへ行きます。
                {
                    SoundLibrary L2;
                    std::string e2;
                    L2.add(wav, std::string(), &e2);
                    L2.setPitch(0,  999.0f);
                    L2.setLpf(0,  99999.0f);
                    L2.setHpf(0,      -5.0f);
                    const bool hi = L2.entries()[0].pitch == 24.0f &&
                                    L2.entries()[0].lpfHz == 20000.0f &&
                                    L2.entries()[0].hpfHz == 20.0f;
                    L2.setPitch(0, -999.0f);
                    const bool lo = L2.entries()[0].pitch == -24.0f;
                    ::sprintf_s(d, "999→%.0f 半音 / 99999→%.0f Hz / -5→%.0f Hz",
                                L2.entries()[0].pitch, L2.entries()[0].lpfHz,
                                L2.entries()[0].hpfHz);
                    check(hi && lo, "範囲の外を打ち込んでも挟んで受け取る", d);
                }

                // ★同時に数本鳴らせること。イベントが動作を 2 つ持てる以上、
                //   ここが 1 本だと**イベントは名前を付けただけの枠**になります。
                //   ⚠ 2026-09-01 まで 1 本しか無く、動作 2 つのイベントは
                //     最後の 1 つしか鳴っていませんでした（ログは「再生 2」と出していた）。
                {
                    VoiceBus vb;
                    vb.prepare(441, 2);

                    auto load = [&](int entry) {
                        std::vector<float> c = conformToDevice(pcm, wi, 44100, 2);
                        ClipSource& v = vb.bind(entry);
                        v.setClip(std::move(c), 2, 44100);
                        v.play();
                    };
                    load(0);
                    load(1);
                    check(vb.playingCount() == 2, "本を 2 つ載せると 2 つとも鳴る",
                          vb.playingCount() == 2 ? "2 本" : "1 本しか鳴っていません");

                    // 重ねたぶん大きくなること（＝混ざっている。片方に置き換わっていない）。
                    std::vector<float> b(441 * 2, 0.0f);
                    double two = 0.0;
                    for (int k = 0; k < 5; ++k) {
                        vb.render(b.data(), 441, 2);
                        for (float x : b) two += static_cast<double>(x) * x;
                    }
                    vb.stopEntry(1);
                    // ★止めたのは 1 番だけ。0 番は鳴り続けていること。
                    const bool oneLeft = vb.playingCount() == 1;

                    VoiceBus vb2;
                    vb2.prepare(441, 2);
                    {
                        std::vector<float> c = conformToDevice(pcm, wi, 44100, 2);
                        ClipSource& v = vb2.bind(0);
                        v.setClip(std::move(c), 2, 44100);
                        v.play();
                    }
                    double one = 0.0;
                    for (int k = 0; k < 5; ++k) {
                        vb2.render(b.data(), 441, 2);
                        for (float x : b) one += static_cast<double>(x) * x;
                    }
                    ::sprintf_s(d, "1 本 %.3f → 2 本 %.3f", std::sqrt(one), std::sqrt(two));
                    check(two > one * 1.5, "重ねると本当に足し合わされる", d);

                    check(oneLeft, "止めたのは指した本だけ（もう 1 本は鳴り続ける）",
                          oneLeft ? "1 本残った" : "巻き添えで止まりました");
                }

                // ═══ ここから 2 項目は「発注者が実装する」ぶん ═══
                //
                // ★先に落ちる検査として置いてあります。**緑になったら完成**です。
                //   実装は `ClipSource` の中。⚠ 同じ形が既にあります ──
                //   ミュートの傾斜（`muteGain_` / `kMuteStep`）が手本です。
                //
                // ⚠ 素材は**一定値 0.5**を使います。信号自体が動くと、
                //   段差が音量のせいか波形のせいか区別が付きません。
                {
                    auto flatClip = [] {
                        return std::vector<float>(441 * 2 * 8, 0.5f);
                    };
                    std::vector<float> b(441 * 2, 0.0f);

                    auto maxStep = [&](const std::vector<float>& v) {
                        float m = 0.0f;
                        for (int i = 1; i < 441; ++i) {
                            const float d = std::fabs(v[i * 2] - v[(i - 1) * 2]);
                            if (d > m) m = d;
                        }
                        return m;
                    };

                    // ── ① 音量は傾斜で変わること（階段にならない）
                    //   いまは 1 ブロックごとに読むだけなので、10ms 刻みで階段になります。
                    ClipSource g1;
                    g1.setClip(flatClip(), 2, 44100);
                    g1.setGainDb(0.0f);
                    g1.setActive(true);
                    g1.render(b.data(), 441, 2);        // 0.5 で安定させる
                    g1.setGainDb(-40.0f);               // 大きく下げる
                    g1.render(b.data(), 441, 2);

                    const float head1 = b[0];
                    const float step1 = maxStep(b);
                    ::sprintf_s(d, "ブロック頭 %.4f（0.5 のまま始まる）／ 隣との最大差 %.5f",
                                head1, step1);
                    check(head1 > 0.4f && step1 < 0.01f,
                          "音量は傾斜で変わる（階段にならない）", d);

                    // ── ② 止めるときも傾斜で落ちること
                    //   いまは state を Stopped にするだけなので、波形の途中で 0 に落ちます。
                    //   ⚠ 頭出しを 0 に戻すのは**傾斜が終わってから**にすること
                    //     （先に戻すと、鳴らしながら位置だけ飛びます）。
                    ClipSource g2;
                    g2.setClip(flatClip(), 2, 44100);
                    g2.setActive(true);
                    g2.render(b.data(), 441, 2);
                    g2.stop();
                    g2.render(b.data(), 441, 2);

                    const float head2 = b[0];
                    const float step2 = maxStep(b);
                    ::sprintf_s(d, "ブロック頭 %.4f（まだ鳴っている）／ 隣との最大差 %.5f",
                                head2, step2);
                    check(head2 > 0.1f && step2 < 0.01f,
                          "止めるときは傾斜で落とす", d);
                }
            }

            // WAV でないものを取り込ませない。
            {
                const std::wstring bad = std::wstring(dir) + L"afhost_selftest_bad.wav";
                FILE* bf = nullptr;
                if (::_wfopen_s(&bf, bad.c_str(), L"wb") == 0 && bf) {
                    std::fputs("RIFFxxxxNOPE", bf);
                    std::fclose(bf);
                    SoundLibrary r2;
                    check(!r2.add(bad, std::string(), &e), "WAV でないものは取り込まない", e);
                    ::DeleteFileW(bad.c_str());
                }
            }
        }

        ::DeleteFileW(proj.c_str());
        ::DeleteFileW(wav.c_str());
    }

    // ⑥ DLL を原本を掴まずに読み込む
    if (target.empty()) {
        check(false, "読み込む対象の DLL", "見つかりません（引数でパスを渡せます）");
    } else {
        // ★先に「直接読んだら掴まれる」を確かめる。
        //   これが false だと、この下の ✔ は空振り（OS がそもそも掴まないだけ）になる。
        const bool locksWhenDirect = Module::directLoadLocksOriginal(target);
        check(locksWhenDirect, "影武者なしで直接読むと、原本が掴まれる（比較の土台）",
              locksWhenDirect ? "＝この後の項目に意味がある"
                              : "★掴まれなかった。影武者の仕掛けは何も稼いでいない");

        Module mod;
        std::string e;
        const bool loaded = mod.load(target, &e);
        check(loaded, "DLL を読み込む", loaded ? wideToUtf8(mod.shadowPath()) : e);

        if (loaded) {
            // ★これがこの道具の存在理由。読み込んだまま原本を書き込みで開く。
            check(Module::originalIsWritable(mod.originalPath()),
                  "影武者ごしなら、読み込んだまま原本を書き込みで開ける", "");

            const bool re = mod.reload(&e);
            check(re, "掴み直す（ビルドし直した DLL を載せ替える）", re ? "" : e);

            mod.unload();
            check(!mod.loaded(), "解放する", "");
        }
    }

    device.stop();
    return r;
}

}  // namespace af
