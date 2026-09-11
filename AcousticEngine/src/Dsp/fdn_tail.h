// fdn_tail.h ── 後期残響の尾を FDN（帰還遅延網）で生成する。2026-09-09。
//
// ■ なぜこれか（docs/TAIL_FDN_PLAN.md）
//   尾は mixing time より後の拡散音場で、耳が聞き分けるのは帯域別の減衰・量・方向という統計。
//   幾何の細部（到達時刻・面の形・扉の開口の色）は mixing time より前を面の線音源が持つ。
//   だから尾は「測って再生する」（実測エコグラム → IR → 畳み込み）でなく
//   「特徴を数値にして生成する」で足りる。生成器なら
//     ・変わる物が全部ゲインか係数で、状態（遅延線の中身）は入れ替わらない → 連続性が構造で出る
//     ・扉は隣室の FDN への配線のゲインになる → 状態を数え上げて焼く必要が無い
//     ・費用が固定で RT60 の長さに依らない
//
// ■ 作り（帯域ごとに独立した Jot 型 FDN を 6 本）
//   入力 → 4 段の直列 allpass（拡散。粒感・金属感を消す）
//        → 直列クロスオーバーで 6 帯域に分ける（EarlyReflectConv / ReverbTailIr と同じ 177/354/707/1414/2828 Hz）
//        → 帯域 b の信号は FDN_b へ: 16 本の遅延線（互いに素っぽい長さ。C# 旧版で詰めた値）
//           各線の出力に**スカラ**の減衰 g[b][i] → 高速 Walsh–Hadamard × 1/√N（無損失の混合）→ 各線の入口へ
//        → 出力は 6 本の FDN の読み出しの和 × 1/√N。
//
//   ★入口のクロスオーバーは 4 次（Linkwitz–Riley、2 次 Butterworth を 2 段）。EarlyReflectConv の
//     2 次より急にしてある。理由: 2 次だと 1414 Hz の境で 2 kHz の成分の 1/4 が下の輪（RT60 が長い）に
//     入り、聞こえる 2 kHz の減衰が長くなる（実測: 目標 0.45 s に対し 2 次の分析で 0.73 s）。
//     4 次にすると境の 1 オクターブ上で −24 dB → 漏れは 1/16。切る周波数は同じ。
//   ★退けた書き方: 1 本の FDN の輪の中で各線の出力を 6 帯域に分けて帯域ごとの減衰を掛ける。
//     クロスオーバー（2 次 lowpass、12 dB/oct）は帯域が重なるので、たとえば 2 kHz の成分の一部が
//     隣の帯域の減衰で回る。**輪の中で分けると漏れが 1 周ごとに積み重なり、減衰の遅い隣の帯域が
//     時間とともに勝つ**。実測: 高域ほど短い RT60（1.2/1.0/0.8/0.6/0.45/0.3 s）を与えて
//     T60 が +1.5 / +18 / +31 / +49 / +62 / +34 % 長く出た（EDC 当てはめ）。
//     帯域ごとに輪を分ければ漏れは入口の 1 回だけで、各輪の減衰は厳密に g_b になる。
//     費用は遅延線が 6 倍になるが、輪の中のフィルタ（16 本 × 5 段の biquad）が消えるので同程度。
//
// ■ 入力の拡散（allpass）は部屋なりに
//   allpass 1 段は係数 g で鳴り続け、T60_ap = −3·L/(fs·log10 g)。g = 0.6・L = 15 ms で **0.2 秒**もある。
//   乾いた小部屋（RT60 0.12 s）に入れると、部屋より拡散器のほうが長く鳴り、閉めた小部屋の減衰が
//   +80% 長く出た（実測、手順 2 の検査）。そこで
//     ・長さは線の倍率に合わせる（小部屋ほど短い）
//     ・係数は T60_ap ≤ RT60（1 kHz）/4 になるよう頭打ちにする（setRt60 で更新。係数なので実行時に変えてよい）
//   響く部屋では 0.6 のまま、乾いた部屋では自然に軽くなる。
//
// ■ 減衰の係数（Jot）
//   線 i（長さ L_i サンプル）が 1 周するごとに g = 10^(−3·L_i / (fs·T60_b))。T60_b 秒で 60 dB 落ちる。
//   線ごとに長さに比例した減衰を与えるので、混合後の全モードが同じ速さで減る（Jot の条件）。
//   ★「高域が先に減る」は係数で捏造しない。T60_b は部屋グラフの Sabine（材質＋空気吸収）から来る。
//
// ■ 量の正規化 ── 解析式 ＋ 作るときに自分で測った補正
//   狙いは「帯域ごとにインパルス応答のエネルギーが 1」。これで尾のスペクトルは入力のまま、RT60 の違いは
//   減衰の速さにだけ出る。ReverbTailIr と同じ規約なので尾の量の規則 freeFieldDirect × √tailRatio × tailSrcLevel
//   がそのまま載る。
//   ★解析式 √(1 − ḡ²) だけでは足りなかった（実測、平らな RT60 で 0.15 → 2.4 s を振って）:
//       拡散 0.6 で +1.9〜+3.0 dB、拡散なしで −3.3〜+5.8 dB。T60 と拡散で大きく動く。
//     式は「エネルギーが線の長さに比例して分布し、混合がそれを保つ」を仮定するが、16 本・長さ 5 倍の
//     網では成り立たない。入口の重みを √(L_i/L̄) にして分布を揃えても −0.4 dB しか動かなかった
//     （退けた書き方。混合後の分布は入口では決まらない）。
//   → 作るときに自分で測る。平らな T60 を 7 点（0.1〜6.4 s、対数等間隔）置き、それぞれインパルスを
//     入れて線の最長の 3 周まで回し、残りは設計の減衰で外挿してエネルギーを出す。ずれを dB で表に持ち、
//     setRt60 では帯域ごとの T60 で表を引いて（対数で線形補間）入力ゲインに掛ける。
//     費用は 1 本あたり数十 ms（部屋を作るときだけ）。拡散の係数は T60 で頭打ちになるので、表は
//     その帯域の T60 で決まる拡散を含む（1 kHz 以外の帯域では拡散が 1 kHz の T60 で決まるぶん、
//     ±1 dB 程度の残りがある。検査で ±2 dB）。
//   ★測る出力は L/R（Hadamard の行 1・2 ＝ 実際に聞く出力）。M（全部足す行）で測ると +2.3 dB 違う
//     （線どうしが帰還で相関しているので、符号を混ぜた行と全部足す行で量が違う。2026-09-09 の実測）。
//     目標は片耳 0.5 ＝ 両耳の合計で 1。ReverbTailIr が「全チャンネル合計でエネルギー 1」なので同じ規約
//     （直接音は等パワーのパンで片耳 0.5 なので、片耳どうしの比が tailRatio になる）。
//
// ■ 連続性
//   setRt60 は目標を置くだけ。render が 1 ブロックの中で係数を線形に目標へ寄せる。
//   状態は触らないので、いま溜まっているエネルギーが次のサンプルから新しい速さで減るだけ。
//   ★遅延線の長さは実行時に変えない（変えると音程が動く）。部屋の違いは係数・量・開始時刻で出す。
//
// ■ 部屋ごとの種（variant、2026-09-11）
//   同じ lineScale の器は遅延線の長さも符号も同じで、同じ入力に**同じ波形**を返していた（相関 1.000）。
//   FdnRoomMix は部屋の出力を「無相関だからエネルギーで足す」前提で配線しているので、1 つの音源を
//   2 部屋へ √(1−t)・√t で割って送ると振幅が揃って足され、量が (√(1−t)+√t)² 倍になった
//   （段 2-f で戸口越しの後期を割ったときに表に出た。t=0.3 で +2.8 dB）。
//   種が 0 でなければ、線と allpass の長さに種から決めた ±3% の揺らぎを掛ける。長さの比が変わると
//   モードの並びが変わり、出力が無相関になる。種 0 は今までと 1 サンプルも同じ。
//   ★±3% の根拠: 隣り合う線の長さの間隔は 5.6〜18%。これより大きく揺らすと 2 本がほぼ同じ長さに寄って
//     モードが重なる（金属感）。小さすぎると低い帯域（周期 8 ms）で最初の数周の相関が抜けない。
//   ★退けた書き方: 注ぎ込みの符号を部屋ごとに変える。線の長さが違う網では直交が保たれず、相関が残る。
//     部屋ごとに Hadamard の別の行を読む: 16 行のうち 15 行を 8 レーン × 2 耳で使い切っていて、2 部屋目に回す行が無い。
//
// ■ スレッド規約
//   setRt60 … 制御スレッド（目標を書いて版を上げる）
//   render  … オーディオスレッド（確保・ロックなし。版が変わっていれば目標を取り込む）
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace af {
namespace dsp {

class FdnTail {
public:
    static constexpr int kNumBands = 6;
    static constexpr int kLines = 16;
    static constexpr int kAllpass = 4;

    /// diffusion : 入力 allpass の係数（0 で無効。0.5〜0.7 が定番）
    /// lineScale : 遅延線の長さの倍率（0.3〜2.5）。部屋ごとの器を作るときに平均自由行程 4V/S なりに決める。
    ///             基準（1.0）は 15〜78 ms で、平均自由行程 12 ms（14 m 角・高 3 m の部屋）に当たる。
    ///             小さい乾いた部屋で長いままだと、混ざり切る（線の最長の約 3 周）前に減衰が終わる。
    ///             ★実行時には変えない（変えると音程が動く）。
    /// variant   : 部屋ごとの種（上の■）。0 で今までと同じ。FdnRoomMix が部屋の番号を渡す。
    explicit FdnTail(int sampleRate, float diffusion = 0.6f, float lineScale = 1.0f, std::uint32_t variant = 0)
        : fs_(std::max(8000, sampleRate)), diffusion_(clampf(diffusion, 0.0f, 0.95f)),
          lineScale_(clampf(lineScale, 0.3f, 2.5f)) {
        // 種 0 はちょうど 1.0 を返す（掛けても 1 ビットも変わらない）。
        auto jitter = [variant](int i, std::uint32_t salt) -> float {
            if (variant == 0u) return 1.0f;
            std::uint32_t h = (variant * 2654435761u) ^ (static_cast<std::uint32_t>(i + 1) * 2246822519u) ^ (salt * 3266489917u);
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
            const float u = static_cast<float>(h) / 4294967295.0f * 2.0f - 1.0f;     // −1..1
            return 1.0f + 0.03f * u;
        };
        // 遅延線の長さ（ms）。C# 旧版（IrConvolver）で 4 ＝ 金属的／8／16 と詰めた値。互いに素っぽく広く分散。
        static const float kLineMs[kLines] = {
            15.3f, 18.1f, 21.7f, 25.3f, 29.1f, 33.7f, 37.9f, 42.3f,
            46.7f, 51.1f, 55.9f, 60.3f, 65.1f, 69.7f, 73.9f, 78.3f };
        static const float kApMs[kAllpass] = { 7.3f, 9.9f, 12.7f, 15.1f };
        static const float kCrossHz[kNumBands - 1] = { 177.0f, 354.0f, 707.0f, 1414.0f, 2828.0f };

        std::size_t total = 0; double lenSum = 0.0;
        for (int i = 0; i < kLines; ++i) {
            lineLen_[i] = std::max(1, static_cast<int>(std::lround(kLineMs[i] * lineScale_ * jitter(i, 0u) * 0.001f * fs_)));
            lineOff_[i] = total; total += static_cast<std::size_t>(lineLen_[i]);
            lenSum += static_cast<double>(lineLen_[i]);
        }
        bandStride_ = total;                                   // 帯域ごとに同じ並びの遅延線
        for (int i = 0; i < kLines; ++i) injW_[i] = static_cast<float>(std::sqrt(static_cast<double>(lineLen_[i]) / (lenSum / kLines)));
        lineBuf_.assign(bandStride_ * kNumBands, 0.0f);
        for (int b = 0; b < kNumBands; ++b)
            for (int i = 0; i < kLines; ++i) linePos_[b][i] = 0;
        // Hadamard の符号表（行 r・線 i）。行 0 = 全部 +1（M）。
        for (int r = 0; r < kLines; ++r)
            for (int i = 0; i < kLines; ++i) signTab_[r][i] = parity(i & r) ? -1.0f : 1.0f;

        total = 0;
        for (int k = 0; k < kAllpass; ++k) {
            apLen_[k] = std::max(1, static_cast<int>(std::lround(kApMs[k] * lineScale_ * jitter(k, 1u) * 0.001f * fs_)));
            apOff_[k] = total; total += static_cast<std::size_t>(apLen_[k]);
            apPos_[k] = 0;
        }
        apBuf_.assign(total, 0.0f);
        for (int c = 0; c < kNumBands - 1; ++c)
            for (int s = 0; s < 2; ++s) split_[c][s].setLowpass(kCrossHz[c], static_cast<float>(fs_));

        calibrate();                                           // 量の校正（数十 ms。部屋を作るときだけ）
        float rt[kNumBands] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        computeGains(rt, gainCur_, inGainCur_);
        diffCur_ = diffTgt_ = pendDiff_ = diffusionFor(rt);
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) gainTgt_[b][i] = gainCur_[b][i];
            inGainTgt_[b] = inGainCur_[b];
            rt60_[b] = rt[b];
        }
        version_.store(0, std::memory_order_release);
        seen_ = 0;
    }

    int sampleRate() const { return fs_; }
    float lineScale() const { return lineScale_; }
    /// 遅延線の最長（サンプル）。混ざり切るまでの目安はこの約 3 倍。
    int longestLine() const { int m = 0; for (int i = 0; i < kLines; ++i) m = std::max(m, lineLen_[i]); return m; }

    // ── 制御スレッド ──

    /// 帯域別の残響時間(s)。部屋グラフの Sabine から毎フレーム渡してよい（目標を置くだけ）。
    ///   bandScale6: 帯域ごとの入力の倍率（振幅）。null で 1。エネルギー 1 の正規化の**上に**掛かる
    ///   （部屋の色。FdnRoomMix が √(RT60_b / 基準) を渡す。校正の表は触らない）。
    void setRt60(const float* rt60Sec6, const float* bandScale6 = nullptr) {
        if (!rt60Sec6) return;
        float g[kNumBands][kLines]; float ig[kNumBands];
        computeGains(rt60Sec6, g, ig);
        // 目標を書いてから版を上げる。render は版を見て取り込む。
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) pendGain_[b][i] = g[b][i];
            pendInGain_[b] = ig[b] * ((bandScale6 && bandScale6[b] > 0.0f) ? bandScale6[b] : 1.0f);
            rt60_[b] = rt60Sec6[b];
        }
        pendDiff_ = diffusionFor(rt60Sec6);
        version_.fetch_add(1, std::memory_order_release);
    }

    /// 状態を捨てる（場面の切り替え用。通常は呼ばない ── 連続性が消える）。
    void reset() {
        std::fill(lineBuf_.begin(), lineBuf_.end(), 0.0f);
        std::fill(apBuf_.begin(), apBuf_.end(), 0.0f);
        for (int c = 0; c < kNumBands - 1; ++c) { split_[c][0].clear(); split_[c][1].clear(); }
    }

    /// 診断: 帯域 b の入力ゲインと、帯域 b・線 i の 1 周あたりの減衰。
    float inputGain(int band = 3) const { return (band >= 0 && band < kNumBands) ? inGainCur_[band] : 0.0f; }
    float gain(int band, int line) const {
        if (line < 0 || line >= kLines || band < 0 || band >= kNumBands) return 0.0f;
        return gainCur_[band][line];
    }
    float rt60(int band) const { return (band >= 0 && band < kNumBands) ? rt60_[band] : 0.0f; }
    float diffusion() const { return diffCur_; }
    /// 診断: 量の校正の表（解析式のままだと何 dB ずれるか）。
    int   calibrationPoints() const { return kCal; }
    float calibrationT60(int k) const { return (k >= 0 && k < kCal) ? calT60_[k] : 0.0f; }
    float calibrationDb(int k) const { return (k >= 0 && k < kCal) ? calDb_[k] : 0.0f; }

    // ── オーディオスレッド ──

    /// in をモノラルで frames サンプル入れ、尾を out に**上書き**する。in と out は別の配列。
    ///   係数は 1 ブロックの中で目標へ線形に寄る（連続性の担保はここ）。
    void render(const float* in, int frames, float* out) { renderImpl(in, frames, out, nullptr, nullptr, 0, nullptr); }
    /// 診断: 帯域ごとの輪の出力を別々に書く（outBands[b][n]）。減衰の検査が輪を直接測るのに使う。
    void renderBands(const float* in, int frames, float* const* outBands) { renderImpl(in, frames, nullptr, outBands, nullptr, 0, nullptr); }
    /// 帯域ごとに M/L/R の 3 本を書く（部屋ごとの配線用。FdnRoomMix が使う）。
    ///   M = 全線の和（Hadamard の行 0）、L/R = 別の行（行 1 = bit0 / 行 2 = bit1）の符号で足した 2 本。
    ///   M・L・R は互いに直交する符号なので拡散状態では無相関＝尾が広がる（畳み込みの尾の L/R 独立ノイズと同じ性質）。
    ///   エネルギーは 3 本とも同じ（各行の符号は ±1 が 16 個）。どれも nullptr 可。
    void renderBandsMLR(const float* in, int frames, float* const* outM, float* const* outL, float* const* outR) {
        const int rows[3] = { 0, 1, 2 };
        float* ptr[3 * kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
            ptr[b] = outM ? outM[b] : nullptr;
            ptr[kNumBands + b] = outL ? outL[b] : nullptr;
            ptr[2 * kNumBands + b] = outR ? outR[b] : nullptr;
        }
        renderImpl(in, frames, nullptr, nullptr, rows, 3, ptr);
    }
    /// 【手順 6】Hadamard の任意の行を帯域ごとに書く（方向バスのレーン用）。行 r の線 i の符号 = popcount(i & r) の偶奇。
    ///   行どうしは直交するので拡散状態では無相関（レーンごとに独立した尾＝方向ごとに別のノイズ）。
    ///   outRows[r * kNumBands + b] に frames サンプル（nullptr 可）。行は 0..15（16 本の線）。
    void renderBandsRows(const float* in, int frames, const int* rows, int nRows, float* const* outRows) {
        renderImpl(in, frames, nullptr, nullptr, rows, nRows, outRows);
    }

private:
    static int parity(int x) { x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return x & 1; }

    void renderImpl(const float* in, int frames, float* out, float* const* outBands,
                    const int* rows, int nRows, float* const* outRows) {
        if (!rows || !outRows) nRows = 0;
        if (nRows > kLines) nRows = kLines;
        if ((!out && !outBands && nRows <= 0) || frames <= 0) return;
        // 版が変わっていれば目標を取り込む（浮動小数の書きは版の release より前、読みは acquire の後）。
        const int v = version_.load(std::memory_order_acquire);
        if (v != seen_) {
            for (int b = 0; b < kNumBands; ++b) {
                for (int i = 0; i < kLines; ++i) gainTgt_[b][i] = pendGain_[b][i];
                inGainTgt_[b] = pendInGain_[b];
            }
            diffTgt_ = pendDiff_;
            seen_ = v;
        }
        // ブロック内の線形ランプ。
        const float inv = 1.0f / static_cast<float>(frames);
        float gStep[kNumBands][kLines]; float igStep[kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) gStep[b][i] = (gainTgt_[b][i] - gainCur_[b][i]) * inv;
            igStep[b] = (inGainTgt_[b] - inGainCur_[b]) * inv;
        }
        const float oNorm = 1.0f / std::sqrt(static_cast<float>(kLines));
        const float dStep = (diffTgt_ - diffCur_) * inv;

        for (int n = 0; n < frames; ++n) {
            for (int b = 0; b < kNumBands; ++b) {
                for (int i = 0; i < kLines; ++i) gainCur_[b][i] += gStep[b][i];
                inGainCur_[b] += igStep[b];
            }
            diffCur_ += dStep;

            // 入力の拡散（直列 allpass）→ 6 帯域へ。
            float x = in ? in[n] : 0.0f;
            if (diffCur_ > 1e-4f)
                for (int k = 0; k < kAllpass; ++k) x = allpass(k, x, diffCur_);
            float xb[kNumBands];
            {
                float rest = x;
                for (int c = 0; c < kNumBands - 1; ++c) {
                    const float lo = split_[c][1].process(split_[c][0].process(rest));
                    xb[c] = lo; rest -= lo;
                }
                xb[kNumBands - 1] = rest;
            }

            float sum = 0.0f;
            for (int b = 0; b < kNumBands; ++b) {
                float* base = lineBuf_.data() + bandStride_ * static_cast<std::size_t>(b);
                float f[kLines]; float bsum = 0.0f;
                float rsum[kLines] = {};                 // 行ごとの符号付きの和（nRows ≤ kLines）
                for (int i = 0; i < kLines; ++i) {
                    const float y = base[lineOff_[i] + static_cast<std::size_t>(linePos_[b][i])];
                    bsum += y;
                    for (int r = 0; r < nRows; ++r) rsum[r] += y * signTab_[rows[r] & (kLines - 1)][i];
                    f[i] = y * gainCur_[b][i];
                }
                sum += bsum;
                if (outBands) outBands[b][n] = bsum * oNorm;
                for (int r = 0; r < nRows; ++r)
                    if (outRows[r * kNumBands + b]) outRows[r * kNumBands + b][n] = rsum[r] * oNorm;
                // 高速 Walsh–Hadamard（無損失の混合）。
                for (int len = 1; len < kLines; len <<= 1)
                    for (int j = 0; j < kLines; j += len << 1)
                        for (int k = j; k < j + len; ++k) {
                            const float a = f[k], c = f[k + len];
                            f[k] = a + c; f[k + len] = a - c;
                        }
                const float xin = xb[b] * inGainCur_[b];
                for (int i = 0; i < kLines; ++i) {
                    base[lineOff_[i] + static_cast<std::size_t>(linePos_[b][i])] = xin * injW_[i] + f[i] * oNorm;
                    if (++linePos_[b][i] >= lineLen_[i]) linePos_[b][i] = 0;
                }
            }
            if (out) out[n] = sum * oNorm;
        }
        // 丸め誤差の蓄積を止める（目標に着いたことにする）。
        for (int b = 0; b < kNumBands; ++b) {
            for (int i = 0; i < kLines; ++i) gainCur_[b][i] = gainTgt_[b][i];
            inGainCur_[b] = inGainTgt_[b];
        }
        diffCur_ = diffTgt_;
    }

    // RBJ バイカッド（直接形II転置）。EarlyReflectConv / ReverbTailIr と同じ物。
    //   ★帯域の切り方を系全体で 1 つにするため、定数も式も揃えてある。変えるときは 3 か所とも。
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void setLowpass(float fc, float fs) {
            const float w0 = 2.0f * 3.14159265358979323846f * fc / fs;
            const float cw = std::cos(w0), sw = std::sin(w0);
            const float alpha = sw / (2.0f * 0.70710678f);
            b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; b2 = (1.0f - cw) * 0.5f;
            const float a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
            b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
            z1 = 0.0f; z2 = 0.0f;
        }
        void clear() { z1 = 0.0f; z2 = 0.0f; }
        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // 拡散の係数を RT60 で頭打ちにする: 最長の allpass が RT60(1 kHz)/4 で 60 dB 落ちる g より大きくしない。
    float diffusionFor(const float* rt60) const {
        const double t60 = std::min(30.0, std::max(0.05, static_cast<double>(rt60[3])));
        int lmax = 1; for (int k = 0; k < kAllpass; ++k) lmax = std::max(lmax, apLen_[k]);
        const double gCap = std::pow(10.0, -3.0 * static_cast<double>(lmax) / (static_cast<double>(fs_) * t60 * 0.25));
        return static_cast<float>(std::min(static_cast<double>(diffusion_), gCap));
    }

    // Schroeder allpass 1 段。位相だけ撹拌して振幅特性は平坦。
    float allpass(int k, float x, float g) {
        float* buf = apBuf_.data() + apOff_[k];
        const float d = buf[apPos_[k]];
        const float v = x - g * d;
        buf[apPos_[k]] = v;
        if (++apPos_[k] >= apLen_[k]) apPos_[k] = 0;
        return d + g * v;
    }

    // Jot の減衰係数と、帯域ごとのエネルギー 1 の入力ゲイン。
    void computeGains(const float* rt60, float (*g)[kLines], float* inGain) const {
        for (int b = 0; b < kNumBands; ++b) {
            // RT60 は 0.05〜30 秒に丸める。短すぎると係数が 0 に張り付き、長すぎると無損失に近づく。
            const double t60 = std::min(30.0, std::max(0.05, static_cast<double>(rt60[b])));
            double sumG2 = 0.0;
            for (int i = 0; i < kLines; ++i) {
                const double Li = static_cast<double>(lineLen_[i]);
                double gv = std::pow(10.0, -3.0 * Li / (static_cast<double>(fs_) * t60));
                if (gv > 0.9999) gv = 0.9999;          // 無損失の輪を作らない
                g[b][i] = static_cast<float>(gv);
                sumG2 += gv * gv;
            }
            const double meanG2 = sumG2 / static_cast<double>(kLines);
            double ig = std::sqrt(std::max(1.0 - meanG2, 1e-3));
            if (calibrated_) ig *= std::pow(10.0, -corrDbFor(static_cast<float>(t60)) / 20.0);
            inGain[b] = static_cast<float>(ig);
        }
    }

    // 校正の表を T60 で引く（対数で線形補間、端は端の値）。
    float corrDbFor(float t60) const {
        if (!calibrated_) return 0.0f;
        const float x = std::log(std::max(t60, 1e-3f));
        if (x <= std::log(calT60_[0])) return calDb_[0];
        if (x >= std::log(calT60_[kCal - 1])) return calDb_[kCal - 1];
        for (int k = 1; k < kCal; ++k) {
            const float x0 = std::log(calT60_[k - 1]), x1 = std::log(calT60_[k]);
            if (x <= x1) { const float u = (x - x0) / (x1 - x0); return calDb_[k - 1] + (calDb_[k] - calDb_[k - 1]) * u; }
        }
        return calDb_[kCal - 1];
    }

    // 作るときに自分で測る。平らな T60 でインパルス応答のエネルギーを出し、解析式とのずれを表に置く。
    //   線の最長の 3 周まで実際に回し、残りは設計の減衰 10^(−6t/T60) で外挿（最後の 20 ms の平均パワー × T60·fs/13.8）。
    void calibrate() {
        calibrated_ = false;
        const int lmax = longestLine();
        const int nRun = std::max(lmax * 3, fs_ / 10);            // 3 周か 100 ms の長いほう
        const int nAvg = std::max(1, fs_ / 50);                    // 最後の 20 ms
        std::vector<float> in(static_cast<std::size_t>(nRun), 0.0f);
        std::vector<float> bufL(static_cast<std::size_t>(nRun) * kNumBands, 0.0f), bufR(static_cast<std::size_t>(nRun) * kNumBands, 0.0f);
        float* pl[kNumBands]; float* pr[kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
            pl[b] = bufL.data() + static_cast<std::size_t>(b) * nRun;
            pr[b] = bufR.data() + static_cast<std::size_t>(b) * nRun;
        }
        in[0] = 1.0f;
        for (int k = 0; k < kCal; ++k) {
            calT60_[k] = 0.1f * std::pow(2.0f, static_cast<float>(k));   // 0.1, 0.2, … 6.4 s
            float rt[kNumBands]; for (int b = 0; b < kNumBands; ++b) rt[b] = calT60_[k];
            reset();
            float g[kNumBands][kLines]; float ig[kNumBands];
            computeGains(rt, g, ig);                                // 表は未完成なので解析式そのまま
            for (int b = 0; b < kNumBands; ++b) { for (int i = 0; i < kLines; ++i) { gainCur_[b][i] = g[b][i]; gainTgt_[b][i] = g[b][i]; } inGainCur_[b] = ig[b]; inGainTgt_[b] = ig[b]; }
            diffCur_ = diffTgt_ = diffusionFor(rt);
            // 実際に聞く出力（L/R）で測る。M は線どうしの相関で量が違う（上の■）。
            {
                const int rows[2] = { 1, 2 };
                float* ptr[2 * kNumBands];
                for (int b = 0; b < kNumBands; ++b) { ptr[b] = pl[b]; ptr[kNumBands + b] = pr[b]; }
                renderImpl(in.data(), nRun, nullptr, nullptr, rows, 2, ptr);
            }
            double e = 0.0, tail = 0.0;
            for (int i = 0; i < nRun; ++i) {
                float l = 0.0f, r = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { l += pl[b][static_cast<std::size_t>(i)]; r += pr[b][static_cast<std::size_t>(i)]; }
                const double p = 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);   // 片耳の平均パワー
                e += p;
                if (i >= nRun - nAvg) tail += p;
            }
            const double pEnd = tail / nAvg;                          // 最後の 20 ms の平均パワー（1 サンプルあたり）
            const double tau = static_cast<double>(calT60_[k]) * fs_ / 13.8155;   // ∫10^(−6t/T60)dt（サンプル）
            e += pEnd * tau;
            // 表は「解析式のままだと目標より何 dB 大きいか」。目標は片耳 0.5（両耳の合計で 1）。
            calDb_[k] = static_cast<float>(10.0 * std::log10(std::max(e, 1e-30) / 0.5));
        }
        reset();
        calibrated_ = true;
    }

    const int fs_;
    const float diffusion_;
    const float lineScale_;

    std::vector<float> lineBuf_;          // [帯域][線][時間]
    std::size_t bandStride_ = 0;
    std::size_t lineOff_[kLines] = {};
    int lineLen_[kLines] = {};
    float injW_[kLines] = {};
    int linePos_[kNumBands][kLines] = {};

    std::vector<float> apBuf_;
    std::size_t apOff_[kAllpass] = {};
    int apLen_[kAllpass] = {};
    int apPos_[kAllpass] = {};

    Biquad split_[kNumBands - 1][2];      // 入口の直列クロスオーバー（LR4 ＝ 2 段。1 組だけ）

    float gainCur_[kNumBands][kLines] = {};
    float gainTgt_[kNumBands][kLines] = {};
    float pendGain_[kNumBands][kLines] = {};
    float inGainCur_[kNumBands] = {}, inGainTgt_[kNumBands] = {}, pendInGain_[kNumBands] = {};
    float rt60_[kNumBands] = {};
    float signTab_[kLines][kLines] = {};   // Hadamard の符号（行 × 線）
    float diffCur_ = 0.6f, diffTgt_ = 0.6f, pendDiff_ = 0.6f;
    static constexpr int kCal = 7;
    float calT60_[kCal] = {};          // 校正点の T60（s、対数等間隔）
    float calDb_[kCal] = {};           // その T60 で解析式のままだと何 dB ずれるか（正なら大きすぎる）
    bool  calibrated_ = false;
    std::atomic<int> version_{0};
    int seen_ = 0;
};

}  // namespace dsp
}  // namespace af
