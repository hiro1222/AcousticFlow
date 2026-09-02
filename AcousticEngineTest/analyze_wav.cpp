// analyze_wav.cpp — 録音した WAV を「エンジンと同じ土俵」の数字に落とす。
//
// 目的:
//   現実の扉の開閉を収録して、**時間ごとの6帯域レベル**を出す。
//   エンジンの帯域（125/250/500/1k/2k/4kHz）とクロスオーバーを揃えてあるので、
//   実測のカーブとエンジンのカーブを直接重ねて比べられる。
//
//   確かめたいのは2つ:
//     ① 音量の変化の**形**（クレッシェンドか、跳ぶか）
//     ② 開くにつれて**高域が入ってくる**か（仮説。否定されるかもしれない）
//
// 使い方:
//   AfAnalyzeWav <入力.wav> [窓ms]      既定の窓 100ms
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kNumBands = 6;
// エンジン(Dsp/early_reflect_conv.h)と同じ境目。隣接帯域の幾何平均。
const float kCrossHz[kNumBands - 1] = {177.0f, 354.0f, 707.0f, 1414.0f, 2828.0f};

// RBJ バイカッド（直接形II転置）。エンジンと同型。
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
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// 16bit PCM の WAV を読む（モノラル化）。チャンク走査なので LIST 等が挟まっても平気。
bool readWav(const char* path, std::vector<float>& out, int& sampleRate) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char riff[12];
    if (std::fread(riff, 1, 12, f) != 12 ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(f); return false;
    }
    int channels = 0, bits = 0;
    sampleRate = 0;
    while (true) {
        char id[4];
        std::uint32_t size = 0;
        if (std::fread(id, 1, 4, f) != 4) break;
        if (std::fread(&size, 4, 1, f) != 1) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            if (std::fread(fmt.data(), 1, size, f) != size) break;
            channels = *reinterpret_cast<std::uint16_t*>(&fmt[2]);
            sampleRate = *reinterpret_cast<std::uint32_t*>(&fmt[4]);
            bits = *reinterpret_cast<std::uint16_t*>(&fmt[14]);
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (channels <= 0 || bits != 16) { std::fclose(f); return false; }
            const std::size_t frames = size / (2u * static_cast<unsigned>(channels));
            out.resize(frames);
            std::vector<std::int16_t> buf(static_cast<std::size_t>(channels));
            for (std::size_t i = 0; i < frames; ++i) {
                if (std::fread(buf.data(), 2, static_cast<std::size_t>(channels), f)
                    != static_cast<std::size_t>(channels)) { out.resize(i); break; }
                int acc = 0;
                for (int c = 0; c < channels; ++c) acc += buf[static_cast<std::size_t>(c)];
                out[i] = static_cast<float>(acc) / (channels * 32768.0f);
            }
            std::fclose(f);
            return !out.empty();
        } else {
            std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR);
        }
    }
    std::fclose(f);
    return false;
}

float toDb(double v) { return 20.0f * std::log10(static_cast<float>(std::max(v, 1e-12))); }

// 左右をそのまま読む（定位の確認用。モノラル化してしまうと両耳差が消える）。
bool readWavStereo(const char* path, std::vector<float>& l, std::vector<float>& r, int& sampleRate) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char riff[12];
    if (std::fread(riff, 1, 12, f) != 12 ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(f); return false;
    }
    int channels = 0, bits = 0;
    sampleRate = 0;
    while (true) {
        char id[4];
        std::uint32_t size = 0;
        if (std::fread(id, 1, 4, f) != 4) break;
        if (std::fread(&size, 4, 1, f) != 1) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            if (std::fread(fmt.data(), 1, size, f) != size) break;
            channels = *reinterpret_cast<std::uint16_t*>(&fmt[2]);
            sampleRate = *reinterpret_cast<std::uint32_t*>(&fmt[4]);
            bits = *reinterpret_cast<std::uint16_t*>(&fmt[14]);
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (channels != 2 || bits != 16) { std::fclose(f); return false; }
            const std::size_t frames = size / 4u;
            l.resize(frames); r.resize(frames);
            std::int16_t buf[2];
            for (std::size_t i = 0; i < frames; ++i) {
                if (std::fread(buf, 2, 2, f) != 2) { l.resize(i); r.resize(i); break; }
                l[i] = buf[0] / 32768.0f;
                r[i] = buf[1] / 32768.0f;
            }
            std::fclose(f);
            return !l.empty();
        } else {
            std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR);
        }
    }
    std::fclose(f);
    return false;
}

// 窓ごとに ILD（左右の音量差）と ITD（相互相関のずれ）を出す。
// 定位が入っているなら、歩くにつれてこの二つが連続的に動くはず。
int analyzeLr(const char* path, float winMs) {
    std::vector<float> l, r;
    int sr = 0;
    if (!readWavStereo(path, l, r, sr)) {
        std::printf("[FAIL] ステレオ 16bit PCM の WAV のみ対応: %s\n", path);
        return 1;
    }
    const int n = static_cast<int>(l.size());
    const int win = std::max(1, static_cast<int>(sr * winMs * 0.001f));
    const int maxLag = static_cast<int>(sr * 0.0012f);  // ±1.2 ms＝人の頭の幅ぶん
    std::printf("=== 両耳差の解析 ===\n");
    std::printf("  %s\n", path);
    std::printf("  %d Hz / %.2f 秒 / 窓 %.0f ms\n\n", sr, static_cast<float>(n) / sr, winMs);
    std::printf("  時刻     全体     ILD      ITD    定位の向き\n");
    std::printf("   (s)     (dB)     (dB)     (us)\n");
    double ildAbsSum = 0.0, itdAbsSum = 0.0;
    int rows = 0;
    for (int s = 0; s + win <= n; s += win) {
        double el = 0.0, er = 0.0;
        for (int i = 0; i < win; ++i) { el += l[s + i] * l[s + i]; er += r[s + i] * r[s + i]; }
        const double rmsL = std::sqrt(el / win), rmsR = std::sqrt(er / win);
        const double all = std::sqrt((el + er) / (2.0 * win));
        if (all < 1e-5) continue;  // 無音の窓は飛ばす
        const float ild = toDb(rmsR) - toDb(rmsL);   // ＋なら右が大きい
        // 相互相関で左右のずれを探す。＋なら右が先（右から来ている）。
        int bestLag = 0; double best = -1e30;
        for (int lag = -maxLag; lag <= maxLag; ++lag) {
            double acc = 0.0;
            for (int i = maxLag; i < win - maxLag; ++i) acc += l[s + i] * r[s + i + lag];
            if (acc > best) { best = acc; bestLag = lag; }
        }
        const float itdUs = bestLag * 1e6f / sr;
        const char* side = (ild > 1.0f) ? "右" : (ild < -1.0f) ? "左" : "正面";
        std::printf("  %5.2f   %6.1f   %+6.1f   %+7.0f    %s\n",
                    static_cast<float>(s) / sr, toDb(all), ild, itdUs, side);
        ildAbsSum += std::fabs(ild); itdAbsSum += std::fabs(itdUs); ++rows;
    }
    if (rows > 0) {
        std::printf("\n  平均 |ILD| = %.2f dB / 平均 |ITD| = %.0f us（%d 窓）\n",
                    ildAbsSum / rows, itdAbsSum / rows, rows);
        std::printf("  ※ 定位が入っていれば |ILD| が数 dB、|ITD| が数百 us 出る。\n");
        std::printf("     どちらもほぼ 0 なら、その音は左右同じ＝頭の中で鳴っている。\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("使い方: AfAnalyzeWav <入力.wav> [窓ms]\n");
        std::printf("        AfAnalyzeWav lr <入力.wav> [窓ms]   左右差（定位）を見る\n");
        return 1;
    }
    if (std::string(argv[1]) == "lr") {
        if (argc < 3) { std::printf("使い方: AfAnalyzeWav lr <入力.wav> [窓ms]\n"); return 1; }
        return analyzeLr(argv[2], (argc > 3) ? static_cast<float>(std::atof(argv[3])) : 250.0f);
    }
    const float winMs = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 100.0f;

    std::vector<float> x;
    int sr = 0;
    if (!readWav(argv[1], x, sr)) {
        std::printf("[FAIL] 読めません（16bit PCM の WAV のみ対応）: %s\n", argv[1]);
        return 1;
    }
    const int n = static_cast<int>(x.size());
    std::printf("=== 録音の解析 ===\n");
    std::printf("  %s\n", argv[1]);
    std::printf("  %d Hz / %.2f 秒 / 窓 %.0f ms\n", sr, static_cast<float>(n) / sr, winMs);

    // 6帯域に分ける（エンジンと同じ直列クロスオーバー。全部足すと元に戻る）。
    std::vector<std::vector<float>> band(kNumBands, std::vector<float>(static_cast<std::size_t>(n)));
    {
        Biquad lp[kNumBands - 1];
        for (int b = 0; b < kNumBands - 1; ++b) lp[b].setLowpass(kCrossHz[b], static_cast<float>(sr));
        for (int i = 0; i < n; ++i) {
            float rest = x[static_cast<std::size_t>(i)];
            for (int b = 0; b < kNumBands - 1; ++b) {
                const float lo = lp[b].process(rest);
                band[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)] = lo;
                rest -= lo;
            }
            band[kNumBands - 1][static_cast<std::size_t>(i)] = rest;
        }
    }

    const int win = std::max(1, static_cast<int>(winMs * 0.001f * sr));
    const int steps = n / win;

    // まず全体の最大レベルを取り、それを 0dB として相対で出す（絶対値に意味は無いので）。
    double peakRms = 0.0;
    std::vector<double> total(static_cast<std::size_t>(steps), 0.0);
    for (int s = 0; s < steps; ++s) {
        double acc = 0.0;
        for (int i = s * win; i < (s + 1) * win; ++i)
            acc += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
        total[static_cast<std::size_t>(s)] = std::sqrt(acc / win);
        peakRms = std::max(peakRms, total[static_cast<std::size_t>(s)]);
    }
    if (peakRms <= 0.0) { std::printf("[FAIL] 無音です\n"); return 1; }

    std::printf("\n  時刻      全体   ─────────── 帯域別(最大を0dBとした相対 dB) ───────────\n");
    std::printf("   (s)      (dB)    125Hz   250Hz   500Hz    1kHz    2kHz    4kHz\n");
    for (int s = 0; s < steps; ++s) {
        std::printf("  %6.2f  %7.1f", (s * win) / static_cast<float>(sr),
                    toDb(total[static_cast<std::size_t>(s)] / peakRms));
        for (int b = 0; b < kNumBands; ++b) {
            double acc = 0.0;
            for (int i = s * win; i < (s + 1) * win; ++i) {
                const double v = band[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)];
                acc += v * v;
            }
            std::printf("  %6.1f", toDb(std::sqrt(acc / win) / peakRms));
        }
        std::printf("\n");
    }

    // 要約：閉／開を代表するフレームを選んで比べる。
    //   ★最大・最小をそのまま使ってはいけない。扉のラッチ音・きしみが1フレームだけ
    //     突き抜けるので、そこが「全開」の基準になってしまう（実測でそうなった。
    //     ラッチ音は低域が強い衝撃音なので、帯域別の差が丸ごと嘘になる）。
    //     順位で下から/上から 15% の位置を代表値に採れば、単発の外れ値に引っ張られない。
    auto rmsAt = [&](int b, int s) {
        double acc = 0.0;
        for (int i = s * win; i < (s + 1) * win; ++i) {
            const double v = band[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)];
            acc += v * v;
        }
        return std::sqrt(acc / win);
    };
    std::vector<int> order(static_cast<std::size_t>(steps));
    for (int s = 0; s < steps; ++s) order[static_cast<std::size_t>(s)] = s;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return total[static_cast<std::size_t>(a)]
                                       < total[static_cast<std::size_t>(b)]; });
    const int lo = order[static_cast<std::size_t>(steps * 15 / 100)];          // 閉じている代表
    const int hi = order[static_cast<std::size_t>(steps - 1 - steps * 15 / 100)]; // 開いている代表

    std::printf("\n  ── 要約（外れ値に強い代表フレームで比較）──\n");
    std::printf("  閉 %.2fs (%.1f dB) / 開 %.2fs (%.1f dB) / 差 %.1f dB\n",
                (lo * win) / static_cast<float>(sr), toDb(total[static_cast<std::size_t>(lo)] / peakRms),
                (hi * win) / static_cast<float>(sr), toDb(total[static_cast<std::size_t>(hi)] / peakRms),
                toDb(total[static_cast<std::size_t>(hi)] / total[static_cast<std::size_t>(lo)]));
    std::printf("  帯域ごとの 開−閉 差:\n");
    for (int b = 0; b < kNumBands; ++b)
        std::printf("    %5.0f Hz : %5.1f dB\n", 125.0 * std::pow(2.0, b),
                    toDb(rmsAt(b, hi) / std::max(rmsAt(b, lo), 1e-12)));
    std::printf("  （高域ほど差が大きければ「開くと明るくなる」）\n");
    return 0;
}
