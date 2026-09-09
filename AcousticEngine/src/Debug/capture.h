/* capture.h ── 「入力」と「音響の出力」を常時録っておく器（.afcap）。
 *
 * 仕様は docs/SOUND_DEBUG_TOOL.md。ここはその §2 の実装。
 *
 * ★この器の要点は「録るのは結果の再合成用ログではなく **入力**」であること。
 *   エンジンは毎フレーム、ホストから
 *     リスナー位置 / 音源位置 / 動いた実体の transform / Update
 *   を押されて動く。**その押している値を録って押し直せば、それが再現になる。**
 *   → イベントログからの再合成が要らない。RNG もボイススティールも無い。
 *
 * ★もうひとつの要点は「検知は録音後にやる」こと（§1 判断2）。
 *   ここは**溜めるだけ**で、破れの判定は一切しない。判定は detectors.h 側。
 *   こうしておくと **検出器を後で良くしたときに、昔のキャプチャへ付け直せる。**
 *   実時間で判定すると、録った瞬間の判定で固定されてしまう。
 *
 * スレッドの前提:
 *   - フレーム記録（pushFrame）は**制御スレッドから**のみ。AF_SceneUpdate の中。
 *   - PCM（pushAudio）は**オーディオスレッドから**のみ。ロックも確保もしない。
 *   - 保存（write）は制御スレッドから。⚠ 詳細は kPcmMarginSeconds の注記。
 */
#ifndef ACOUSTICFLOW_DEBUG_CAPTURE_H
#define ACOUSTICFLOW_DEBUG_CAPTURE_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace acoustic {
namespace dbg {

constexpr int kCapBands = 6;

/* ── 記録の粒 ─────────────────────────────────────────────── */

/* 全体（フレームに 1 つ）。時刻の基準は**フレーム番号とオーディオクロック**。 */
struct CapGlobal {
    std::uint32_t frame = 0;
    std::uint64_t audioSample = 0;      // このフレーム時点の累計サンプル数
    float lx = 0, ly = 0, lz = 0;       // リスナー位置（入力）
    std::uint16_t sourceCount = 0;
    std::uint16_t movedCount = 0;
    std::uint8_t  marked = 0;           // このフレームで「変だ」を押したか
    std::uint8_t  pad[3] = {0, 0, 0};
    /* ★★ 段の間引きの位相。update に入る**直前**の値 ★★
     *   [0]catalog [1]diffSrc [2]early [3]echoRaySlice [4]staggered [5]echoPrimed
     *
     *   これが無いとリプレイで尾が再現しない。実測（扉が毎フレーム動く 91 フレーム）:
     *   **91 フレーム全部が不一致・最大 15.5% ずれ**。しかも収束しない ──
     *   カウンタは録音側も再生側も 1 ずつ減るので、**位相のずれが永久に残る**。
     *   エコグラムはレイを 1/N ずつ撃って溜めるので、撃つフレームがずれると
     *   その瞬間の扉の角度が違い、出てくる尾が違う。
     *   → 位相を録って押し直す前に戻す。溜まりかけの中身までは戻さないが、
     *      それは 1 周（role2EveryN フレーム）で流れる。 */
    std::uint8_t  phase[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

/* 音源 1 本ぶん。位置＝入力、それ以外＝音響の出力（走査の対象）。 */
struct CapSource {
    std::uint64_t id = 0;
    float sx = 0, sy = 0, sz = 0;       // 入力
    float band[kCapBands] = {};         // 出力: 生存ゲイン 6 帯域
    float dx = 0, dy = 0, dz = 0;       // 出力: 到来方向
    float occ = 0;                      // 出力: 遮蔽スカラ
    float tailLevel = 0;                // 出力: 尾の量（エコグラム総和）
    std::uint16_t earlyTaps = 0;        // 出力: 早期反射の本数
    std::uint16_t diffSrcs = 0;         // 出力: 回折二次音源の本数（型紙3 が見る）
    std::uint8_t  tier = 0;             // 出力: 実際に使われた段
    std::uint8_t  pad[3] = {0, 0, 0};
};

/* 動いた実体（扉など）。★動くのは扉くらいなので、差分で録れば軽い。 */
struct CapMoved {
    std::int32_t instance = 0;
    float cx = 0, cy = 0, cz = 0;       // center
    float hx = 0, hy = 0, hz = 0;       // halfExtents
    float rx = 0, ry = 0, rz = 0;       // axisX (right)
    float ux = 0, uy = 0, uz = 0;       // axisY (up)
};

/* 静的な形（箱だけ）。★録音開始時に 1 回だけ写す。
 *   ★なぜ入れるか: これが無いと**生成した回帰テストが自己完結しない**
 *     （形は呼び手が作れ、では検査として使えない）。
 *   ⚠ メッシュは入れない。本番ステージは 8,102 三角形で、入れたら容量が破裂する。
 *     → メッシュを含む場面は「吐けない場面」として印を付ける（docs §5.2）。
 *     今日のバグは全部**箱の場面**で出ているので、箱から始めれば後回しにできる。 */
struct CapBox {
    std::int32_t instance = 0;
    std::int32_t material = 0;
    float cx = 0, cy = 0, cz = 0;
    float hx = 0, hy = 0, hz = 0;
    float rx = 1, ry = 0, rz = 0;
    float ux = 0, uy = 1, uz = 0;
};

/* 材質。★箱と一緒に録らないと、吐いた回帰テストが**別の音を測る**ことになる
 *   （最初これを入れ忘れていて、生成物は全部の箱に既定材質を使っていた）。 */
struct CapMaterial {
    float transmission[kCapBands] = {};
    float absorption[kCapBands]   = {};
    float scattering[kCapBands]   = {};
};

/* ── 設定 ─────────────────────────────────────────────────── */

struct CaptureConfig {
    /* ★21 秒なのは、**畳み込み器の遅延線が冷えている**ぶんを捨てるため。
     *   T-21s から流して T-20s から見せる。器の内部状態を保存する代わりに
     *   1 秒余分に録って暖める。 */
    int prerollFrames  = 21 * 60;
    int postrollFrames = 5 * 60;
    int maxSources     = 64;
    int sampleRate     = 48000;
    bool recordPcm     = true;
    /* 〔任意〕診断の列。⚠ workerThreads=1 でしか信用できない（§2.2）ので既定は切る。 */
    bool recordDiag    = false;
};

/* ⚠ PCM のリングは (preroll+postroll) より**この秒数ぶん長く**取る。
 *   保存はマーク＋postroll の時点で行うので、いちばん古い必要サンプルが
 *   上書きされるまでにこの余裕がある。コピーはミリ秒未満なので実用上これで足りる。
 *   ★「ロックを取らない代わりに余裕で守る」── オーディオスレッドを止めないため。 */
constexpr float kPcmMarginSeconds = 2.0f;

/* ── 本体 ─────────────────────────────────────────────────── */

class Capture {
public:
    void begin(const CaptureConfig& cfg) {
        // ★録ったまま begin し直す（保存 → 再開）と、オーディオスレッドが pushAudio で古い pcm_ に書いている最中に
        //   pcm_.assign で確保し直して落ちる（Unity のクラッシュ 2026-08-24 と 09-09、どちらも AF_SceneCapturePushAudio）。
        //   先に止めて、走っている 1 回ぶん（DSP 1 ブロック ≒ 21 ms）を待ってから確保する。再開は稀なので 30 ms は許す。
        if (active_) {
            active_ = false;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        cfg_ = cfg;
        if (cfg_.prerollFrames < 1) cfg_.prerollFrames = 1;
        if (cfg_.postrollFrames < 0) cfg_.postrollFrames = 0;
        if (cfg_.maxSources < 1) cfg_.maxSources = 1;
        capFrames_ = cfg_.prerollFrames + cfg_.postrollFrames + 1;
        /* ⚠★保持したい枚数より **1 枠多く**取る。
         *   次の枠は「動いた実体」を beginFrame より前に受けるので、先に空にしておく必要がある。
         *   ところが枠がちょうど保持枚数だと、その掃除が**いちばん古い保持フレームを消す**。
         *   実測: 扉が 91 フレーム中 90 フレームにしか残らなかった（先頭の 1 枚が消えていた）。
         *   → 書き込み中の枠を保持分と別に持つ。 */
        slots_ = capFrames_ + 1;
        gRing_.assign(static_cast<size_t>(slots_), CapGlobal{});
        sRing_.assign(static_cast<size_t>(slots_) * static_cast<size_t>(cfg_.maxSources),
                      CapSource{});
        mRing_.assign(static_cast<size_t>(slots_) * kMaxMovedPerFrame, CapMoved{});
        head_ = 0; filled_ = 0; frameNo_ = 0;
        markPending_ = false; markCountdown_ = 0; ready_ = false; markedFrame_ = 0;

        if (cfg_.recordPcm) {
            const double secs = (double)(cfg_.prerollFrames + cfg_.postrollFrames) / 60.0
                              + kPcmMarginSeconds;
            pcmCap_ = (size_t)(secs * cfg_.sampleRate) * 2u;      // stereo interleaved
            pcm_.assign(pcmCap_, 0);
            pcmWrite_.store(0, std::memory_order_relaxed);
        } else {
            pcmCap_ = 0; pcm_.clear();
        }
        active_ = true;
    }

    /* 静的な形を写す。録音開始の直後に 1 回だけ呼ぶ。
     *   meshCount > 0 なら「この場面は C++ に吐けない」印になる。 */
    void setGeometry(const std::vector<CapBox>& boxes, int meshCount,
                     const std::vector<CapMaterial>& materials) {
        boxes_ = boxes;
        meshCount_ = meshCount;
        materials_ = materials;
    }

    void end() { active_ = false; }
    bool active() const { return active_; }
    bool ready()  const { return ready_; }          // 保存できる状態か
    int  framesHeld() const { return filled_; }

    /* 「いま変だった」を押す。★押した時点の前 preroll と、押してから postroll を残す。 */
    void mark() {
        if (!active_ || markPending_) return;
        markPending_ = true;
        markCountdown_ = cfg_.postrollFrames;
        markedFrame_ = frameNo_;
        markAudio_ = pcmWrite_.load(std::memory_order_acquire);
    }

    /* ── 制御スレッド: 1 フレーム書く ───────────────────────── */
    /* ⚠★ここで枠を消してはいけない。
     *   ホストの呼ぶ順は  UpdateInstance（動いた実体） → SetListener/SetSource → Update
     *   なので、**このフレームの moved は beginFrame より前に溜まっている**。
     *   最初 `g = CapGlobal{}` で消していて、扉の動きが 1 つも残らなかった。
     *   → 枠の掃除は endFrame 側（次の枠へ移ったあと）で行う。 */
    CapGlobal* beginFrame(float lx, float ly, float lz, std::uint64_t audioSample) {
        if (!active_ || ready_) return nullptr;
        CapGlobal& g = gRing_[static_cast<size_t>(head_)];
        g.frame = frameNo_;
        g.audioSample = audioSample;
        g.lx = lx; g.ly = ly; g.lz = lz;
        g.marked = (markPending_ && markedFrame_ == frameNo_) ? 1u : 0u;
        return &g;
    }

    CapSource* sourceSlot(int i) {
        if (!active_ || ready_ || i < 0 || i >= cfg_.maxSources) return nullptr;
        return &sRing_[static_cast<size_t>(head_) * static_cast<size_t>(cfg_.maxSources)
                       + static_cast<size_t>(i)];
    }

    /* 実体が動いたことを記録する。★1 フレームの上限を超えたぶんは捨てて数だけ数える
     *   （毎フレーム全部作り直すホストで爆発しないように）。 */
    void noteMoved(const CapMoved& m) {
        if (!active_ || ready_) return;
        CapGlobal& g = gRing_[static_cast<size_t>(head_)];
        if (g.movedCount >= kMaxMovedPerFrame) { ++movedDropped_; return; }
        mRing_[static_cast<size_t>(head_) * kMaxMovedPerFrame + g.movedCount] = m;
        ++g.movedCount;
    }

    void endFrame(int sourceCount) {
        if (!active_ || ready_) return;
        CapGlobal& g = gRing_[static_cast<size_t>(head_)];
        g.sourceCount = static_cast<std::uint16_t>(
            sourceCount < cfg_.maxSources ? sourceCount : cfg_.maxSources);
        head_ = (head_ + 1) % slots_;
        if (filled_ < capFrames_) ++filled_;
        ++frameNo_;
        // 次の枠を空にしておく。★ここで掃除するのは、動いた実体が beginFrame より
        //   前に溜まるから（上の注記）。掃除の場所を間違えると扉の動きが消える。
        gRing_[static_cast<size_t>(head_)] = CapGlobal{};
        if (markPending_) {
            if (markCountdown_ <= 0) { ready_ = true; }   // 前後が揃った
            else --markCountdown_;
        }
    }

    /* ── オーディオスレッド: ロックも確保もしない ─────────────── */
    void pushAudio(const float* interleavedStereo, int frames) {
        if (!active_ || pcmCap_ == 0 || !interleavedStereo || frames <= 0) return;
        // 容量と先頭は 1 回だけ読む（begin が触るのは active_ を落として 30 ms 待った後だが、念のため途中で変わっても混ぜない）。
        const size_t cap = pcmCap_;
        std::int16_t* pcm = pcm_.data();
        if (cap == 0 || !pcm) return;
        size_t w = pcmWrite_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames * 2; ++i) {
            float v = interleavedStereo[i];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            pcm[w % cap] = static_cast<std::int16_t>(v * 32767.0f);
            ++w;
        }
        pcmWrite_.store(w, std::memory_order_release);
    }

    /* ── 保存 ───────────────────────────────────────────────── */
    /* ヘッダは ASCII の自己記述。★版が変わると並びが変わるので、並びをヘッダに書いておく。
     *   そうすれば**古いキャプチャも読める**。識別子（scene/dllHash）も焼いて、
     *   開いたときに現物と違えば呼び手が赤く言えるようにする。 */
    bool write(const char* path, const char* sceneName, std::uint32_t dllHash,
               int workerThreads, const char* rateLine) const {
        if (filled_ <= 0) return false;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return false;

        const int n = filled_;
        const int start = (head_ - n + slots_ * 2) % slots_;

        std::fprintf(f, "AFCAP 2\n");   /* 2: 段の位相 phase[8] を CapGlobal に追加 */
        std::fprintf(f, "sampleRate %d\n", cfg_.sampleRate);
        std::fprintf(f, "frames %d\n", n);
        std::fprintf(f, "maxSources %d\n", cfg_.maxSources);
        std::fprintf(f, "preroll %d postroll %d\n", cfg_.prerollFrames, cfg_.postrollFrames);
        std::fprintf(f, "markedFrame %u\n", markedFrame_);
        std::fprintf(f, "fieldsGlobal frame,audioSample,lx,ly,lz,sourceCount,movedCount,marked,"
                       "phase[catalog,diffSrc,early,echoRaySlice,staggered,echoPrimed]\n");
        std::fprintf(f, "fieldsSource id,sx,sy,sz,b125,b250,b500,b1k,b2k,b4k,"
                       "dx,dy,dz,occ,tailLvl,earlyTaps,diffSrcs,tier\n");
        std::fprintf(f, "fieldsMoved instance,cx,cy,cz,hx,hy,hz,rx,ry,rz,ux,uy,uz\n");
        std::fprintf(f, "workerThreads %d\n", workerThreads);
        std::fprintf(f, "%s\n", rateLine ? rateLine : "rates unknown");
        std::fprintf(f, "scene %s\n", (sceneName && *sceneName) ? sceneName : "unknown");
        std::fprintf(f, "dllHash %08x\n", dllHash);
        std::fprintf(f, "movedDropped %d\n", movedDropped_);
        /* 静的な形。★テキストで書く ── 生成した回帰テストにそのまま貼れる形にしたいので、
         *   人が読めて diff が取れることを優先する（箱は数十個なので容量は問題にならない）。 */
        std::fprintf(f, "materials %d\n", (int)materials_.size());
        for (const CapMaterial& m : materials_) {
            std::fprintf(f, "mat");
            for (int b = 0; b < kCapBands; ++b) std::fprintf(f, " %.9g", m.transmission[b]);
            for (int b = 0; b < kCapBands; ++b) std::fprintf(f, " %.9g", m.absorption[b]);
            for (int b = 0; b < kCapBands; ++b) std::fprintf(f, " %.9g", m.scattering[b]);
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "geomBoxes %d meshes %d\n", (int)boxes_.size(), meshCount_);
        for (const CapBox& b : boxes_)
            std::fprintf(f, "box %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
                         b.instance, b.material, b.cx, b.cy, b.cz, b.hx, b.hy, b.hz,
                         b.rx, b.ry, b.rz, b.ux, b.uy, b.uz);
        std::fprintf(f, "END_HEADER\n");

        for (int k = 0; k < n; ++k) {
            const int idx = (start + k) % slots_;
            const CapGlobal& g = gRing_[static_cast<size_t>(idx)];
            std::fwrite(&g, sizeof(CapGlobal), 1, f);
            const size_t sBase = static_cast<size_t>(idx) * static_cast<size_t>(cfg_.maxSources);
            if (g.sourceCount)
                std::fwrite(&sRing_[sBase], sizeof(CapSource), g.sourceCount, f);
            const size_t mBase = static_cast<size_t>(idx) * kMaxMovedPerFrame;
            if (g.movedCount)
                std::fwrite(&mRing_[mBase], sizeof(CapMoved), g.movedCount, f);
        }

        /* PCM。マーク時点から前 preroll・後 postroll ぶんを切り出す。 */
        std::vector<std::int16_t> out;
        if (pcmCap_) {
            const size_t w = pcmWrite_.load(std::memory_order_acquire);
            const double secs = (double)(cfg_.prerollFrames + cfg_.postrollFrames) / 60.0;
            size_t want = (size_t)(secs * cfg_.sampleRate) * 2u;
            if (want > pcmCap_) want = pcmCap_;
            const size_t have = (w < want) ? w : want;
            out.resize(have);
            for (size_t i = 0; i < have; ++i) out[i] = pcm_[(w - have + i) % pcmCap_];
        }
        std::fprintf(f, "PCM %d %d\n", (int)(out.size() / 2), 2);
        if (!out.empty()) std::fwrite(out.data(), sizeof(std::int16_t), out.size(), f);
        std::fclose(f);
        return true;
    }

    const CaptureConfig& config() const { return cfg_; }

    static constexpr int kMaxMovedPerFrame = 16;

private:
    CaptureConfig cfg_{};
    bool active_ = false;
    int  capFrames_ = 0, slots_ = 0, head_ = 0, filled_ = 0;
    std::uint32_t frameNo_ = 0, markedFrame_ = 0;
    bool markPending_ = false, ready_ = false;
    int  markCountdown_ = 0, movedDropped_ = 0;
    size_t markAudio_ = 0;

    std::vector<CapGlobal> gRing_;
    std::vector<CapSource> sRing_;
    std::vector<CapMoved>  mRing_;

    std::vector<CapBox> boxes_;
    std::vector<CapMaterial> materials_;
    int meshCount_ = 0;

    std::vector<std::int16_t> pcm_;
    size_t pcmCap_ = 0;
    std::atomic<size_t> pcmWrite_{0};
};

/* ── 読み出し（走査ツール側） ───────────────────────────────── */

struct CaptureFile {
    int sampleRate = 48000, frames = 0, maxSources = 0;
    int preroll = 0, postroll = 0, workerThreads = 1;
    std::uint32_t markedFrame = 0, dllHash = 0;
    std::string scene, rates, fieldsSource;
    std::vector<CapBox> boxes;
    std::vector<CapMaterial> materials;
    int meshCount = 0;              // >0 なら C++ に吐けない場面（docs §5.3）
    std::vector<CapGlobal> global;
    std::vector<std::vector<CapSource>> sources;   // frames × sourceCount
    std::vector<std::vector<CapMoved>>  moved;
    std::vector<std::int16_t> pcm;                 // interleaved stereo
};

inline bool readCapture(const char* path, CaptureFile& out, std::string* err = nullptr) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { if (err) *err = "開けない"; return false; }
    char line[512];
    bool magic = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "END_HEADER", 10) == 0) break;
        char key[64] = {};
        if (std::sscanf(line, "%63s", key) != 1) continue;
        if (std::strcmp(key, "AFCAP") == 0) { magic = true; continue; }
        if (std::strcmp(key, "sampleRate") == 0) std::sscanf(line, "%*s %d", &out.sampleRate);
        else if (std::strcmp(key, "frames") == 0) std::sscanf(line, "%*s %d", &out.frames);
        else if (std::strcmp(key, "maxSources") == 0) std::sscanf(line, "%*s %d", &out.maxSources);
        else if (std::strcmp(key, "preroll") == 0)
            std::sscanf(line, "%*s %d %*s %d", &out.preroll, &out.postroll);
        else if (std::strcmp(key, "markedFrame") == 0) {
            unsigned v = 0; std::sscanf(line, "%*s %u", &v); out.markedFrame = v;
        } else if (std::strcmp(key, "workerThreads") == 0)
            std::sscanf(line, "%*s %d", &out.workerThreads);
        else if (std::strcmp(key, "dllHash") == 0) {
            unsigned v = 0; std::sscanf(line, "%*s %x", &v); out.dllHash = v;
        } else if (std::strcmp(key, "scene") == 0) {
            char v[256] = {}; std::sscanf(line, "%*s %255s", v); out.scene = v;
        } else if (std::strcmp(key, "rates") == 0) {
            out.rates = line; if (!out.rates.empty() && out.rates.back() == '\n') out.rates.pop_back();
        } else if (std::strcmp(key, "fieldsSource") == 0) {
            char v[256] = {}; std::sscanf(line, "%*s %255s", v); out.fieldsSource = v;
        } else if (std::strcmp(key, "mat") == 0) {
            CapMaterial m;
            const char* p = line + 3;
            int okCount = 0;
            for (int i = 0; i < kCapBands * 3; ++i) {
                char* endp = nullptr;
                const double v = std::strtod(p, &endp);
                if (endp == p) break;
                float* dst = (i < kCapBands) ? m.transmission
                           : (i < kCapBands * 2) ? m.absorption : m.scattering;
                dst[i % kCapBands] = static_cast<float>(v);
                p = endp; ++okCount;
            }
            if (okCount == kCapBands * 3) out.materials.push_back(m);
        } else if (std::strcmp(key, "geomBoxes") == 0) {
            int nb = 0; std::sscanf(line, "%*s %d %*s %d", &nb, &out.meshCount);
            out.boxes.reserve(static_cast<size_t>(nb > 0 ? nb : 0));
        } else if (std::strcmp(key, "box") == 0) {
            CapBox b;
            if (std::sscanf(line, "%*s %d %d %f %f %f %f %f %f %f %f %f %f %f %f",
                            &b.instance, &b.material, &b.cx, &b.cy, &b.cz,
                            &b.hx, &b.hy, &b.hz, &b.rx, &b.ry, &b.rz,
                            &b.ux, &b.uy, &b.uz) == 14)
                out.boxes.push_back(b);
        }
    }
    if (!magic) { std::fclose(f); if (err) *err = "AFCAP ではない"; return false; }

    out.global.resize(static_cast<size_t>(out.frames));
    out.sources.resize(static_cast<size_t>(out.frames));
    out.moved.resize(static_cast<size_t>(out.frames));
    for (int k = 0; k < out.frames; ++k) {
        if (std::fread(&out.global[static_cast<size_t>(k)], sizeof(CapGlobal), 1, f) != 1) {
            std::fclose(f); if (err) *err = "フレーム記録が途中で切れている"; return false;
        }
        const CapGlobal& g = out.global[static_cast<size_t>(k)];
        out.sources[static_cast<size_t>(k)].resize(g.sourceCount);
        if (g.sourceCount)
            std::fread(out.sources[static_cast<size_t>(k)].data(), sizeof(CapSource),
                       g.sourceCount, f);
        out.moved[static_cast<size_t>(k)].resize(g.movedCount);
        if (g.movedCount)
            std::fread(out.moved[static_cast<size_t>(k)].data(), sizeof(CapMoved),
                       g.movedCount, f);
    }
    /* PCM の見出し行 */
    if (std::fgets(line, sizeof(line), f)) {
        int nf = 0, ch = 0;
        if (std::sscanf(line, "PCM %d %d", &nf, &ch) == 2 && nf > 0 && ch > 0) {
            out.pcm.resize(static_cast<size_t>(nf) * static_cast<size_t>(ch));
            std::fread(out.pcm.data(), sizeof(std::int16_t), out.pcm.size(), f);
        }
    }
    std::fclose(f);
    return true;
}

}  // namespace dbg
}  // namespace acoustic

#endif  // ACOUSTICFLOW_DEBUG_CAPTURE_H
