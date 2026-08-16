// render_wav.cpp — エンジンだけで音を作って WAV に書き出す（Unity 不要）。
//
// 目的:
//   段4で DSP が C++ 側へ移ったので、**Unity を開かずに耳で確かめられる**ようにする。
//   Unity はネイティブ DLL をロックするため、音の調整のたびに閉じる必要があって遅い。
//   ここなら数秒で回る。デモ動画の素材作りにも使える。
//
// 何を鳴らすか:
//   密閉した2部屋を戸口で繋ぎ、そこに開閉する扉を置く。音源は扉の向こう。
//   扉を 0°→90°→0° と動かしながらレンダリングするので、
//   **扉の開き具合が音にどう出るか**をそのまま聴ける（この作品の主張そのもの）。
//
// 使い方:
//   AfRenderWav [出力パス]      既定 door_sweep.wav
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "acoustic_scene.h"
#include "acoustic_voice.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 512;
constexpr float kSpeedOfSound = 343.0f;

AF_Vector3 V(float x, float y, float z) { AF_Vector3 v{x, y, z}; return v; }

// 16bit PCM ステレオ WAV を書く。
bool writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r,
              int sampleRate) {
    const int n = static_cast<int>(l.size());
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(n) * 2u * 2u;   // 2ch × 16bit
    const std::uint32_t fmtChunk = 16;
    const std::uint16_t fmtPcm = 1, channels = 2, bits = 16;
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate) * 2u * 2u;
    const std::uint16_t blockAlign = 4;
    const std::uint32_t riffSize = 36 + dataBytes;

    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtChunk, 4, 1, f);
    std::fwrite(&fmtPcm, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&sampleRate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);

    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            float v = (c == 0) ? l[static_cast<std::size_t>(i)] : r[static_cast<std::size_t>(i)];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            const std::int16_t s = static_cast<std::int16_t>(v * 32767.0f);
            std::fwrite(&s, 2, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

// 6帯域ゲインを低域加重で 1 スカラへ（距離減衰の基準に使う）。
float broadband(const float g[6]) {
    static const float w[6] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
    float gs = 0.0f, ws = 0.0f;
    for (int b = 0; b < 6; ++b) { gs += w[b] * g[b]; ws += w[b]; }
    return ws > 0.0f ? gs / ws : 0.0f;
}

}  // namespace

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "door_sweep.wav";
    // 第2引数に btm を渡すと回折を BTM（有限楔の稜線積分）で出す。鳴らし比べ用。
    const bool useBtm = (argc > 2) && std::string(argv[2]) == "btm";
    // どこかの引数に click があれば、ドライ信号を過渡音（クリック列）にする。
    //   ノイズはダブりを隠すので、離散的なエコーの有無を聞くときはこちら。
    bool useClick = false;
    for (int a = 1; a < argc; ++a) if (std::string(argv[a]) == "click") useClick = true;
    // 第2引数が数値ならコントラスト指数（開口率の幅を開く）。既定 1.0＝素通し。
    const float contrast = (argc > 2 && !useBtm)
                         ? static_cast<float>(std::atof(argv[2])) : 1.0f;
    std::printf("=== エンジン単体レンダリング（扉の開閉）===\n");

    // ── シーン: 密閉した2部屋を戸口で繋ぎ、扉を置く ──
    AF_SceneHandle s = AF_SceneCreate();
    const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);
    const float t = 0.15f, h = 4.0f, doorW = 1.2f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    // 仕切り（z=0）: 戸口 x∈[-0.6,0.6], y∈[0,2.4]
    AF_SceneAddInstanceBox(s, V(-(hw + 0.6f) * 0.5f, h * 0.5f, 0),
                           V((hw - 0.6f) * 0.5f, h * 0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V((hw + 0.6f) * 0.5f, h * 0.5f, 0),
                           V((hw - 0.6f) * 0.5f, h * 0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, (doorH + h) * 0.5f, 0),
                           V(0.6f, (h - doorH) * 0.5f, t), V(1,0,0), V(0,1,0), mat);
    // 外周（密閉）
    AF_SceneAddInstanceBox(s, V(0, -t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h + t, 0), V(hw, t, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(-hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V( hw, h*0.5f, 0), V(t, h*0.5f, hd), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f, -hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    AF_SceneAddInstanceBox(s, V(0, h*0.5f,  hd), V(hw, h*0.5f, t), V(1,0,0), V(0,1,0), mat);
    // 扉（蝶番 x=-0.6、+z 側へ振れる）
    const int door = AF_SceneAddInstanceBox(s, V(0, doorH * 0.5f, 0),
                                            V(doorW * 0.5f, doorH * 0.5f, 0.03f),
                                            V(1,0,0), V(0,1,0), mat);

    // 音源の x を第4引数で指定できる（既定 -2.0＝戸口から外れた位置）。
    //   ★この作品の主張は「扉の角度ではなく幾何が決める」なので、音源の位置で
    //     結果が変わることそのものが要点。0.0 を渡すと戸口に正対する。
    //     実測では 正対 0.056→1.000（25dB） / 大きく外れ 0.056→0.056（0dB）。
    const float srcX = (argc > 4) ? static_cast<float>(std::atof(argv[4])) : -2.0f;
    const AF_Vector3 L = V(0, 1.6f, -4.0f);     // リスナー（手前の部屋）
    const AF_Vector3 S = V(srcX, 1.6f, 3.5f);   // 音源（奥の部屋）
    std::printf("  音源 x = %.1f （0=戸口に正対 / -2=外れた位置）\n", srcX);
    AF_SceneSetListener(s, L);
    AF_SceneSetSource(s, 1, S);
    // 開口を「面」として鳴らす（ホイヘンス）。点1つだと戸口がピンポイントに聞こえる。
    AF_SceneSetApertureSpread(s, 3);
    // 戸口をポータルとして置く（x∈[-0.6,0.6], y∈[0,2.4], z=0）。
    //   開き具合は渡さない。エンジンが毎フレーム実形状から測る。
    AF_SceneAddPortal(s, V(0.0f, 1.2f, 0.0f), V(1, 0, 0), V(0, 1, 0), 0.6f, 1.2f);
    AF_SceneSetUseBtm(s, useBtm ? 1 : 0);
    if (contrast > 0.0f) AF_SceneSetApertureContrast(s, contrast);
    // 第3引数 tr で「開口を透過の一部として扱う」を有効化。
    const bool apTrans = (argc > 3) && std::string(argv[3]) == "tr";
    AF_SceneSetApertureIsTransmission(s, apTrans ? 1 : 0);
    if (apTrans) std::printf("  開口を透過として扱う: ON\n");
    std::printf("  回折の出し方: %s  コントラスト %.2f\n",
                useBtm ? "BTM(有限楔の稜線積分)" : "従来(前川＋開口積分)", contrast);

    // ── 音源（DSP）──
    AF_VoiceConfig cfg{};
    cfg.sampleRate = kSampleRate;
    cfg.maxFrames = kBlock;
    cfg.tailSeconds = 1.2f;
    AF_VoiceHandle voice = AF_VoiceCreate(&cfg);
    AF_VoiceSetOutputGain(voice, 0.6f);
    AF_HrtfHandle hrtf = AF_HrtfCreateSynthetic(kSampleRate);
    AF_VoiceSetHrtf(voice, hrtf);
    AF_VoiceSetHrtfEnabled(voice, 1);

    const float seconds = 12.0f;
    const int total = static_cast<int>(seconds * kSampleRate);
    std::vector<float> outL(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> outR(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> dry(kBlock), bl(kBlock), br(kBlock);

    // ドライ信号: ピンクっぽいノイズ（帯域の変化とレベル変化が聴き取りやすい）。
    unsigned int rs = 22222u;
    float pink = 0.0f;

    std::vector<float> echo(200 * 6, 0.0f);
    const float binMs = 5.0f;

    std::printf("  扉を 0°→90°→0° と動かしながら %.0f 秒ぶんレンダリングします\n", seconds);

    int pos = 0;
    int blockIndex = 0;
    while (pos < total) {
        const int n = std::min(kBlock, total - pos);
        const float tt = static_cast<float>(pos) / static_cast<float>(total);
        // 0→90→0 の三角波。ゆっくり開いてゆっくり閉じる。
        const float deg = (tt < 0.5f) ? (tt * 2.0f * 90.0f) : ((1.0f - tt) * 2.0f * 90.0f);

        // 扉を回す。
        const float th = deg * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        AF_SceneUpdateInstance(s, door,
                               V(-0.6f + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                               V(doorW * 0.5f, doorH * 0.5f, 0.03f), V(c, 0, sn), V(0, 1, 0));
        AF_SceneUpdate(s, static_cast<float>(n) / kSampleRate);

        // ── タップを組む ──
        AF_VoiceTap taps[8] = {};
        int nTaps = 0;

        // 0: 直接音（透過）。ソフト遮蔽で連続に。
        float trans[6] = {}; float occFrac = 0.0f;
        AF_SceneComputeSoftOcclusion(s, L, S, trans, 6, &occFrac);
        const float directDist =
            std::sqrt((S.x-L.x)*(S.x-L.x) + (S.y-L.y)*(S.y-L.y) + (S.z-L.z)*(S.z-L.z));
        const float distGain = 4.0f / std::max(directDist, 4.0f);   // 基準距離 4m の 1/r
        for (int b = 0; b < 6; ++b) taps[0].gain6[b] = trans[b] * distGain;
        taps[0].panL = taps[0].panR = 0.70710678f;
        taps[0].delaySamples = 0;
        nTaps = 1;

        // 1..: 回折（開口ごと）。距離減衰だけ掛ける方針（LPF は透過が担当）。
        // 帯域別で受け取る。隙間が狭いほど高域だけが通る＝開くと音色が開く。
        AF_Vector3 dp[8]; float dg[8]; float db[8 * 6];
        const int nd = AF_SceneComputeDiffractionSourceBands(s, L, S, dp, dg, db, 8);
        for (int i = 0; i < nd && nTaps < 8; ++i) {
            const float dx = dp[i].x - L.x, dy = dp[i].y - L.y, dz = dp[i].z - L.z;
            const float plen = std::sqrt(dx*dx + dy*dy + dz*dz);
            const float distAtten = 4.0f / std::max(plen, 4.0f);
            for (int b = 0; b < 6; ++b)
                taps[nTaps].gain6[b] = db[i * 6 + b] * occFrac * distAtten;
            // 到来方向を左右パンへ（等パワー）。
            const float inv = (plen > 1e-4f) ? 1.0f / plen : 0.0f;
            const float xr = (dx * inv + 1.0f) * 0.5f;
            taps[nTaps].panL = std::sqrt(1.0f - xr);
            taps[nTaps].panR = std::sqrt(xr);
            const float relMs = (plen - directDist) / kSpeedOfSound * 1000.0f;
            taps[nTaps].delaySamples =
                std::max(0, static_cast<int>(relMs * 0.001f * kSampleRate));
            AF_VoiceScatterSplit(relMs, 25.0f, 0.0f, 1.0f,
                                 &taps[nTaps].gSpec, &taps[nTaps].gDiff);
            ++nTaps;
        }
        AF_VoiceSetTaps(voice, taps, nTaps);

        // 直接音の到来方向で HRTF を回す（開口があればそちらを向く）。
        const AF_Vector3 arrive = (nd > 0) ? dp[0] : S;
        AF_VoiceSetDirection(voice, V(arrive.x - L.x, arrive.y - L.y, arrive.z - L.z), 57.0f);

        // ── 尾は数ブロックに1回だけ組み直す（重いので）──
        if ((blockIndex % 16) == 0) {
            const AF_Vector3 srcArr[1] = { S };
            AF_SceneComputeEchogramBands(s, L, srcArr, 1, echo.data(), 200,
                                         binMs * 0.001f, kSpeedOfSound, 512, 12, 4.0f);
            // ★尾の基準は「部屋へ入ってくるエネルギーの合計」。
            //   タップ0（透過）だけを基準にすると、扉を開けても透過は変わらないので
            //   **残響量が一切増えない**。現実で扉を開けたときの「ぐわっと広がる」感じは、
            //   向こうの部屋の響きが流れ込むことによる部分が大きい。
            //   回折タップが増えれば、その分だけ尾も膨らむようにする。
            float dGain = 0.0f;
            for (int i = 0; i < nTaps; ++i) dGain += broadband(taps[i].gain6);
            AF_VoiceRebuildTail(voice, echo.data(), 200, binMs, 25.0f, 8.0f,
                                30.0f, 0.0f, 0.6f, dGain, 0.6f, nullptr, 0);
        }

        // ── ドライ信号 ──
        //   ★ノイズは**ダブりを隠します**。離散的なエコーが混ざっていても
        //     連続音の中では聞き分けられない（実際それで判断できなかった）。
        //     クリック列なら2つ目のコピーがはっきり分かれて聞こえる。
        //     一方ノイズは帯域とレベルの変化が掴みやすいので、目的で使い分ける。
        for (int i = 0; i < n; ++i) {
            if (useClick) {
                // 0.4 秒ごとに短い立ち上がりの過渡音。反射やダブりの粒が見える。
                const int period = static_cast<int>(kSampleRate * 0.4f);
                const int ph = (pos + i) % period;
                dry[static_cast<std::size_t>(i)] =
                    (ph < 64) ? std::exp(-ph * 0.06f) * 0.9f : 0.0f;
            } else {
                rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
                const float w = static_cast<int>(rs) * (1.0f / 2147483648.0f);
                pink += 0.03f * (w - pink);          // 一次LPで低域寄りに
                dry[static_cast<std::size_t>(i)] = (pink * 3.0f + w * 0.15f) * 0.5f;
            }
        }

        AF_VoiceRender(voice, dry.data(), n, bl.data(), br.data(), nullptr);
        for (int i = 0; i < n; ++i) {
            outL[static_cast<std::size_t>(pos + i)] = bl[static_cast<std::size_t>(i)];
            outR[static_cast<std::size_t>(pos + i)] = br[static_cast<std::size_t>(i)];
        }

        if ((blockIndex % 128) == 0) {
            float tot = 0.0f;
            for (int i = 0; i < nTaps; ++i) tot += broadband(taps[i].gain6);
            std::printf("    %5.1f 秒  扉 %4.1f°  回折 %d本  到達エネルギー %.4f\n",
                        static_cast<float>(pos) / kSampleRate, deg, nd, tot);
        }

        pos += n;
        ++blockIndex;
    }

    AF_VoiceDestroy(voice);
    AF_HrtfDestroy(hrtf);
    AF_SceneDestroy(s);

    if (!writeWav(outPath, outL, outR, kSampleRate)) {
        std::printf("[FAIL] 書き出せませんでした: %s\n", outPath);
        return 1;
    }
    std::printf("[OK] 書き出しました: %s (%.1f 秒)\n", outPath, seconds);
    return 0;
}
