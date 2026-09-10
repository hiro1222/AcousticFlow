// render_wav_flow.cpp — **新コア（AF_World）だけ**で音を作って WAV に書き出す。Unity 不要。
//
// ■ 役割
//   旧コアの AfRenderWav と**同じ場面・同じ信号・同じ長さ**を、新コアの C API だけで鳴らす。
//   出た 2 本の WAV は AfAnalyzeWav でそのまま突き合わせられる ＝ 新旧の違いを数字で出す土台。
//   段 10（旧コアの削除）の前に「新コアでも数字で確かめられる」状態を作るのが目的。
//
// ■ 中の仕組み
//   1) 場面: 密閉した 2 部屋を戸口で繋ぎ、蝶番が x=-0.6 の扉を置く。リスナーは手前、音源は奥。
//      箱の寸法・位置・材質・リスナー・音源は render_wav.cpp の既定と 1 対 1 で同じ。
//   2) 配線: World → FdnRoomMix → DirectionBus。Unity の AcousticWorld.cs と同じ順番で繋ぐ
//      （声を鳴らす → FDN を鳴らす → バスを鳴らす。バスは AudioListener で 1 回ぶん）。
//   3) 扉を 0°→90°→0° の三角波で回しながら 12 秒ぶん回す。
//      ★旧コアはホスト側でタップを組んでいたが、新コアは AF_WorldApplyVoice が全部やる。
//        ここが新旧のいちばん大きな作りの違いで、この道具の短さがそれを示している。
//   4) 節目ごとに数字を出す: 角度・見通しの割合・五成分（dB）・段・レイ・虚像の数・部屋。
//      耳で聴く前に、鳴っている中身が読める。
//
// ■ 繋がり
//   受ける: acoustic_world.h（新コア）と acoustic_voice.h（DSP）。旧コアの入口は 1 つも呼ばない。
//   渡す:   16bit ステレオ WAV。AfAnalyzeWav が読む。
//
// ■ 退けた書き方
//   ・AfRenderWav に --core flow を足す: 1 つの翻訳単位に新旧両方の入口が入る。
//     旧コアを消す段 10 で、この道具ごと壊れるか、#ifdef だらけになる。別ファイルなら旧を消すだけで済む。
//   ・場面を新コアに合わせて作り直す: 比べる意味が無くなる。**寸法は旧に合わせる**のが要点。
//   ・合成 HRTF でなく実測 kemar を既定にする: 旧コアの既定（扉モード）が合成なので揃えた。
//
// ■ 壊れる所
//   ・AF_WorldBindFdn の前に AF_WorldUpdate を呼ぶと、器に部屋が無いまま送りが作られる。
//   ・FdnStale を見ずに回すと、部屋が増えたときに古い器を指したままになる。
//   ・DirectionBus を繋いだら FDN の出力はバス経由で出る。AF_FdnMixRender の戻りだけ足すと無音に近くなる。
//
// 使い方:
//   AfFlowWav [出力パス] [音源x]        既定 flow_door_sweep.wav / -2.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "acoustic_world.h"
#include "acoustic_voice.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 512;

AF_Vector3 V(float x, float y, float z) { AF_Vector3 v{x, y, z}; return v; }

// 16bit PCM ステレオ WAV を書く（render_wav.cpp と同じ形式）。
bool writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int sampleRate) {
    const int n = static_cast<int>(l.size());
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const std::int32_t dataBytes = n * 2 * 2;
    const std::int32_t riffSize = 36 + dataBytes;
    const std::int16_t ch = 2, bits = 16, fmt = 1;
    const std::int32_t byteRate = sampleRate * ch * bits / 8;
    const std::int16_t blockAlign = static_cast<std::int16_t>(ch * bits / 8);
    const std::int32_t fmtSize = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVE", 1, 4, f); std::fwrite("fmt ", 1, 4, f);
    std::fwrite(&fmtSize, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&sampleRate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            float v = (c == 0) ? l[static_cast<std::size_t>(i)] : r[static_cast<std::size_t>(i)];
            v = std::min(1.0f, std::max(-1.0f, v));
            const std::int16_t s = static_cast<std::int16_t>(v * 32767.0f);
            std::fwrite(&s, 2, 1, f);
        }
    }
    std::fclose(f);
    return true;
}

double dB(double e) { return 10.0 * std::log10(std::max(e, 1e-30)); }

}  // namespace

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "flow_door_sweep.wav";
    const float srcX = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : -2.0f;

    std::printf("=== 新コア（AF_World）単体レンダリング: 扉の開閉 ===\n");
    std::printf("  音源 x = %.1f （0=戸口に正対 / -2=外れた位置）\n", srcX);

    // ── 場面（寸法は render_wav.cpp の扉モードと同じ）──
    AF_WorldHandle w = AF_WorldCreate();
    const int mat = AF_WorldAddMaterial(w, nullptr, nullptr, nullptr);   // 既定の壁
    const float t = 0.15f, h = 4.0f, doorH = 2.4f;
    const float hw = 6.0f, hd = 8.0f;
    // 戸口の半幅。既定 0.6（＝幅 1.2 m。旧コアの扉モードと同じ）。
    //   ★部屋グラフが「くびれ」で割るかどうかはここで決まる。割れないと戸口が 0 個になり、
    //     扉で響きを変える仕組み（applyOpenings）がまるごと効かない。境目を測るための摘み。
    float doorHalf = 0.6f;
    if (const char* dh = std::getenv("AF_DOOR_HALF")) doorHalf = static_cast<float>(std::atof(dh));
    const float doorW = doorHalf * 2.0f;
    auto box = [&](AF_Vector3 c, AF_Vector3 he, AF_Vector3 ax, int dyn) {
        return AF_WorldAddBox(w, c, he, ax, V(0, 1, 0), mat, dyn);
    };
    // 仕切り（z=0）: 戸口 x∈[-0.6,0.6], y∈[0,2.4]
    box(V(-(hw + doorHalf) * 0.5f, h * 0.5f, 0), V((hw - doorHalf) * 0.5f, h * 0.5f, t), V(1, 0, 0), 0);
    box(V( (hw + doorHalf) * 0.5f, h * 0.5f, 0), V((hw - doorHalf) * 0.5f, h * 0.5f, t), V(1, 0, 0), 0);
    box(V(0, (doorH + h) * 0.5f, 0), V(doorHalf, (h - doorH) * 0.5f, t), V(1, 0, 0), 0);
    // 外周（密閉）
    box(V(0, -t, 0),          V(hw, t, hd),           V(1, 0, 0), 0);
    box(V(0, h + t, 0),       V(hw, t, hd),           V(1, 0, 0), 0);
    box(V(-hw, h * 0.5f, 0),  V(t, h * 0.5f, hd),     V(1, 0, 0), 0);
    box(V( hw, h * 0.5f, 0),  V(t, h * 0.5f, hd),     V(1, 0, 0), 0);
    box(V(0, h * 0.5f, -hd),  V(hw, h * 0.5f, t),     V(1, 0, 0), 0);
    box(V(0, h * 0.5f,  hd),  V(hw, h * 0.5f, t),     V(1, 0, 0), 0);
    // 扉（蝶番 x=-0.6、+z 側へ振れる）。★動く箱なので dynamic=1（部屋グラフから外れる）
    const int door = box(V(0, doorH * 0.5f, 0), V(doorW * 0.5f, doorH * 0.5f, 0.03f), V(1, 0, 0), 1);

    const AF_Vector3 L = V(0, 1.6f, -4.0f);
    const AF_Vector3 S = V(srcX, 1.6f, 3.5f);
    AF_WorldSetListener(w, L, V(0, 0, 1), V(0, 1, 0));
    const int emitter = AF_WorldAddEmitter(w, S, 0.2f);
    // 部屋の粒度。戸口は 1.2 m なので、割るには一辺がその 1/4 以下ほしい。
    if (const char* rc = std::getenv("AF_ROOM_CELL")) AF_WorldSetRoomCell(w, static_cast<float>(std::atof(rc)));
    // 五成分の重み（直接・初期・後期・回折・透過）。AF_WEIGHTS="1,1,0,1,1" で後期を止める、など。\n    //   ★色がどこから来ているかを切り分けるための摘み。既定は全部 1。\n    if (const char* wv = std::getenv("AF_WEIGHTS")) {\n        float w5[5] = {1, 1, 1, 1, 1}; int k = 0;\n        for (const char* q = wv; *q && k < 5; ) {\n            w5[k++] = static_cast<float>(std::atof(q));\n            const char* c = std::strchr(q, ','); if (!c) break; q = c + 1;\n        }\n        AF_WorldSetWeights(w, w5);\n        std::printf("  成分の重み: 直接 %.2f 初期 %.2f 後期 %.2f 回折 %.2f 透過 %.2f\n", w5[0], w5[1], w5[2], w5[3], w5[4]);\n    }\n    AF_WorldBuild(w);
    {
        float req = 0.0f, eff = 0.0f; double vox = 0.0, maxVox = 0.0;
        AF_WorldRoomCellInfo(w, &req, &eff, &vox, &maxVox);
        std::printf("  部屋 %d、リスナーの部屋 %d、音源の部屋 %d\n",
                    AF_WorldRoomCount(w), AF_WorldRoomAt(w, L), AF_WorldRoomAt(w, S));
        std::printf("  ボクセル一辺: 要求 %.3f m / 実際 %.3f m（%.0f 個 / 上限 %.0f）%s\n",
                    req, eff, vox, maxVox, (eff > req * 1.001f) ? "  ★上限に収めるため粗くされています" : "");
        std::printf("  戸口（rooms::Aperture）%d 個\n", AF_WorldApertureCount(w));
    }

    // ── DSP: 声 → FDN → 方向バス（Unity の AcousticWorld.cs と同じ配線）──
    AF_VoiceConfig vc{};
    vc.sampleRate = kSampleRate;
    vc.maxFrames = kBlock;
    vc.tailSeconds = 1.2f;
    AF_VoiceHandle voice = AF_VoiceCreate(&vc);
    AF_VoiceSetOutputGain(voice, 0.6f);
    AF_HrtfHandle hrtf = AF_HrtfCreateSynthetic(kSampleRate);
    AF_VoiceSetHrtf(voice, hrtf);
    AF_VoiceSetHrtfEnabled(voice, 1);

    AF_FdnMixHandle fdn = AF_FdnMixCreate(kSampleRate, kBlock, 0.6f);
    AF_WorldBindFdn(w, fdn);                       // ★部屋を器へ入れるのは World の仕事
    AF_VoiceSetFdnMix(voice, fdn);

    AF_DirectionBusHandle bus = AF_DirectionBusCreate(kSampleRate, 8, kBlock);
    AF_DirectionBusSetHrtf(bus, hrtf, 57.0f);
    AF_VoiceSetDirectionBus(voice, bus);
    AF_FdnMixSetDirectionBus(fdn, bus, 57.0f);

    const float seconds = 12.0f;
    const int total = static_cast<int>(seconds * kSampleRate);
    std::vector<float> outL(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> outR(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> dry(kBlock), bl(kBlock), br(kBlock), fl(kBlock), fr(kBlock);
    unsigned int rs = 22222u;                      // ★旧と同じ種・同じ式（信号を揃える）
    float pink = 0.0f;

    std::printf("  扉を 0°→90°→0° と動かしながら %.0f 秒ぶん鳴らします\n", seconds);
    std::printf("    %6s %8s %9s %9s %9s %9s %9s %5s %6s %6s | %s\n",
                "角度", "見通し", "直接", "初期", "後期", "回折", "透過", "段", "レイ", "虚像",
                "回折: 箱/稜線 δ 隙間 重み 影の数");

    int pos = 0, nextReport = 0;
    while (pos < total) {
        const int n = std::min(kBlock, total - pos);
        const float u = static_cast<float>(pos) / static_cast<float>(total);
        const float deg = (u < 0.5f) ? (u * 2.0f * 90.0f) : ((1.0f - u) * 2.0f * 90.0f);

        // 扉を回す（蝶番 x=-0.6）。中心と軸を置き直すだけ。
        const float th = deg * 3.14159265f / 180.0f;
        const float c = std::cos(th), sn = std::sin(th);
        AF_WorldSetBoxTransform(w, door, V(-doorHalf + c * doorW * 0.5f, doorH * 0.5f, sn * doorW * 0.5f),
                                V(c, 0, sn), V(0, 1, 0));
        AF_WorldUpdate(w, static_cast<float>(n) / kSampleRate);
        if (AF_WorldFdnStale(w) != 0) { AF_WorldBindFdn(w, fdn); AF_VoiceSetFdnMix(voice, fdn); }
        AF_WorldApplyVoice(w, emitter, voice, kSampleRate);

        for (int i = 0; i < n; ++i) {
            rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
            const float wn = static_cast<int>(rs) * (1.0f / 2147483648.0f);
            pink += 0.03f * (wn - pink);
            dry[static_cast<std::size_t>(i)] = (pink * 3.0f + wn * 0.15f) * 0.5f;
        }
        AF_VoiceRender(voice, dry.data(), n, bl.data(), br.data(), nullptr);
        std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
        AF_FdnMixRender(fdn, n, fl.data(), fr.data());
        for (int i = 0; i < n; ++i) {
            outL[static_cast<std::size_t>(pos + i)] = bl[static_cast<std::size_t>(i)] + fl[static_cast<std::size_t>(i)];
            outR[static_cast<std::size_t>(pos + i)] = br[static_cast<std::size_t>(i)] + fr[static_cast<std::size_t>(i)];
        }
        std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
        AF_DirectionBusRender(bus, n, fl.data(), fr.data());   // ★Unity では AudioListener で 1 回
        for (int i = 0; i < n; ++i) {
            outL[static_cast<std::size_t>(pos + i)] += fl[static_cast<std::size_t>(i)];
            outR[static_cast<std::size_t>(pos + i)] += fr[static_cast<std::size_t>(i)];
        }

        if (pos >= nextReport) {
            AF_MixInfo mi{};
            AF_WorldMixInfo(w, emitter, &mi);
            double cm[5] = {};
            for (int k = 0; k < 5; ++k) for (int b = 0; b < 6; ++b) cm[k] += mi.component6[k][b];
            AF_DiffractionInfo di{};
            AF_WorldDiffractionInfo(w, emitter, &di);
            std::printf("    %6.1f %8.4f %9.2f %9.2f %9.2f %9.2f %9.2f %5d %6d %6d | %d/%d d=%.3f a=%.3f w=%.2f 影%d\n",
                        deg, mi.visibleFraction, dB(cm[0]), dB(cm[1]), dB(cm[2]), dB(cm[3]), dB(cm[4]),
                        AF_WorldEmitterTier(w, emitter), AF_WorldEmitterRays(w, emitter), mi.imageCount,
                        di.valid ? di.box : -1, di.valid ? di.edge : -1, di.delta, di.gapWidth, di.weight, mi.shadowers);
            nextReport += total / 12;
        }
        pos += n;
    }

    if (!writeWav(outPath, outL, outR, kSampleRate)) {
        std::printf("  書き出しに失敗: %s\n", outPath);
        return 1;
    }
    std::printf("  書き出し: %s（%.1f 秒）\n", outPath, seconds);

    AF_VoiceSetDirectionBus(voice, nullptr);
    AF_VoiceSetFdnMix(voice, nullptr);
    AF_WorldBindFdn(w, nullptr);
    AF_DirectionBusDestroy(bus);
    AF_FdnMixDestroy(fdn);
    AF_VoiceDestroy(voice);
    AF_HrtfDestroy(hrtf);
    AF_WorldDestroy(w);
    return 0;
}
