// voice_bus.h — 試聴の音を**同時に数本**鳴らすための束ね。
//
// ★なぜ要るのか
//   イベントは「動作の並び」を持ちます。1 つの発火で「A を鳴らして B も鳴らす」が
//   できないと、名前を付けただけの別名になってイベントである意味がありません。
//   ⚠ 2026-09-01 までここが 1 本しかなく、**動作を 2 つ持つイベントは
//     最後の 1 つしか鳴っていませんでした**（しかもログは「再生 2」と出していた）。
//
// ★これは「ボイス管理」ではありません。
//   優先度・スティール・上限といった仕組みは、**いまは**持ちません。
//   ⚠ 2026-09-01、行き先が「ゲームで使えるミドルウェア」に変わったので、
//     **ここは踏み台であって完成形ではありません**（本物のボイス管理に置き換わる）。
//   ここにあるのは **試聴に足りるだけの固定本数**で、埋まったら一番古いものを
//   使い回すだけです。ゲームの再生機構ではなく、道具の試聴機構です。
//
// ★デバイスは開けっぱなしにします。
//   1 本だった頃は鳴らすたびに閉じて開き直していて、そのぶん途切れていました。
//   束ねが常に載っているので、鳴らす／止めるは**この中で**済みます。
//
// スレッド規約:
//   render()  … 実時間スレッドから。**確保しない・待たない**
//   それ以外  … 画面から。値は atomic で渡す

#pragma once

#include "block_source.h"

#include <vector>

namespace af {

class VoiceBus : public BlockSource {
public:
    // 試聴に足りるだけ。増やす理由が出てからでないと増やしません。
    static constexpr int kVoices = 8;

    VoiceBus() { mix_.resize(4096 * 8, 0.0f); }

    // ★実時間スレッドが触る配列を先に確保しておく。
    //   render の中で伸ばすと、そこで確保が起きます。
    void prepare(int maxFrames, int channels) {
        const std::size_t need = static_cast<std::size_t>(maxFrames) *
                                 static_cast<std::size_t>(channels > 0 ? channels : 2);
        if (mix_.size() < need) mix_.resize(need, 0.0f);
    }

    void render(float* out, int frames, int channels) override {
        const std::size_t n = static_cast<std::size_t>(frames) *
                              static_cast<std::size_t>(channels);
        // ★まず 0 で埋める。足し込む形なので、前のブロックの残骸が残ると耳に出ます。
        for (std::size_t i = 0; i < n; ++i) out[i] = 0.0f;

        if (mix_.size() < n) return;   // ⚠ ここで伸ばさない（確保しない）

        for (int v = 0; v < kVoices; ++v) {
            if (v_[v].empty()) continue;
            v_[v].render(mix_.data(), frames, channels);
            for (std::size_t i = 0; i < n; ++i) out[i] += mix_[i];
        }

        // ★足して溢れたら挟む。
        //   ⚠ 挟まないと、重ねた瞬間に耳とスピーカーを痛めます。
        //     歪んで聞こえるのは正しい ── 「重ね過ぎ」がそのまま分かります。
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] >  1.0f) out[i] =  1.0f;
            if (out[i] < -1.0f) out[i] = -1.0f;
        }
    }

    const char* name() const override { return "取り込んだ音"; }
    void setActive(bool on) override {
        if (!on) stopAll();
    }

    // ── 画面から

    // その音源に割り当てられている本を返す。無ければ nullptr。
    ClipSource* find(int entry) {
        if (entry < 0) return nullptr;
        for (int v = 0; v < kVoices; ++v)
            if (entry_[v] == entry && !v_[v].empty()) return &v_[v];
        return nullptr;
    }

    // その音源に本を割り当てる。★同じ音源が既に載っていれば、それを使い回します
    //   （載せ直すと頭からになるので、続きから鳴らせなくなる）。
    ClipSource& bind(int entry) {
        if (ClipSource* c = find(entry)) { age_[indexOf(c)] = ++clock_; return *c; }

        int pick = -1;
        for (int v = 0; v < kVoices; ++v)
            if (v_[v].empty()) { pick = v; break; }
        if (pick < 0) {
            // ★埋まっていたら一番古いものを使い回す。優先度は見ません。
            pick = 0;
            for (int v = 1; v < kVoices; ++v)
                if (age_[v] < age_[pick]) pick = v;
        }
        entry_[pick] = entry;
        age_[pick]   = ++clock_;
        return v_[pick];
    }

    void stopEntry(int entry) {
        if (ClipSource* c = find(entry)) c->stop();
    }
    void stopAll() {
        for (int v = 0; v < kVoices; ++v) v_[v].stop();
    }
    // 台帳が変わったら、指している先が別物になっている。全部手放す。
    void releaseAll() {
        for (int v = 0; v < kVoices; ++v) {
            v_[v].setClip(std::vector<float>(), 0, 0);
            entry_[v] = -1;
        }
    }

    // 何本が鳴っているか（画面と検査が読む）。
    int playingCount() const {
        int n = 0;
        for (int v = 0; v < kVoices; ++v)
            if (!v_[v].empty() && v_[v].state() == ClipSource::State::Playing) ++n;
        return n;
    }
    int loadedCount() const {
        int n = 0;
        for (int v = 0; v < kVoices; ++v)
            if (!v_[v].empty()) ++n;
        return n;
    }

    // ★どれも選ばれていないときに返す空の本。
    //   ⚠ nullptr を返さないこと。呼ぶ側が毎回 null を確かめる形にすると、
    //     1 箇所忘れただけで落ちます。空の本なら「何も載っていない」と答えます。
    ClipSource& idle() { return idle_; }

    ClipSource& at(int v) { return v_[v]; }
    int entryAt(int v) const { return entry_[v]; }

private:
    int indexOf(const ClipSource* c) const {
        for (int v = 0; v < kVoices; ++v)
            if (&v_[v] == c) return v;
        return 0;
    }

    ClipSource         v_[kVoices];
    int                entry_[kVoices] = {-1, -1, -1, -1, -1, -1, -1, -1};
    unsigned long long age_[kVoices]   = {};
    unsigned long long clock_ = 0;
    ClipSource         idle_;
    std::vector<float> mix_;
};

}  // namespace af
