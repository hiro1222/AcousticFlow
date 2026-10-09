// replay_capture.cpp — 録画の記録（Unreal の -AFCapture が書く AFCapture_scene.txt）を、同じエンジンの同じ API で呼び直して音にする。
//
// ■ 全体の中の位置
//   tools/record_unreal_demo.ps1: Unreal を自動で歩かせて録画 → **ここ**で記録から音を作る → 録画の道具（AfCapture）が画面と合わせて MP4。
//   発注者「動画撮って送ってほしい」（2026-10-04）。出力の機器（Yamaha AG03）の時計が止まって Wwise が無音になった日に作った。
//
// ■ 中の仕組み
//   1) 記録の頭: 部屋の割り方・箱（材質の番号つき）・摘み → Unreal の AAcousticFlowWorld::BeginPlay / PushKnobs と同じ順・同じ値で呼ぶ。
//   2) 器: Unreal の CreateShared と同じ（方向バス 8 レーン＋上下・クロスオーバー 700 Hz・合成 HRTF、FDN 0.6）。
//      音源の Voice は UAcousticFlowEmitter::BeginPlay と同じ設定。
//   3) 時間: 音は 512 標本ずつ。各ブロックの頭までに起きたフレーム（f 行）を順に当てる
//      （耳・音源・動く箱・摘み → AF_WorldUpdate(dt) → 器が古ければ繋ぎ直す → AF_WorldApplyVoice）。
//      ★ゲームのスレッドが毎フレーム配分を解き、音のスレッドがブロックごとに鳴らす ── Unreal と同じ関係。
//   4) 音: Wwise の自前プラグインと同じ並び（各 Voice → FDN → 方向バス）。入力は試験の音（ループ、2 ch をモノラルの平均に）を
//      音源を登録した時刻から鳴らす。
//
// ■ 退けた書き方
//   ・場面をここに書き直す: 地図（レベル）が正（発注者の指示）。記録の箱をそのまま使えば、地図を直しても書き直さなくてよい。
//   ・Wwise の録音を使う: -game で 0.2 秒しか書かれなかった。
//
// ■ 壊れる所
//   ・Wwise 側の音量（RTPC・減衰）はここには無い（試験の音は減衰なし・音量 1 で鳴らしている）。Wwise で音量を触ったら合わない。
//   ・音源の追加・削除のうち削除は記録していない（試験の地図では起きない）。
//
// 使い方:
//   AfReplayWav <AFCapture_scene.txt> <入力の WAV> <出力の WAV>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "acoustic_world.h"
#include "acoustic_voice.h"

namespace {

constexpr int kBlock = 512;

AF_Vector3 V(float x, float y, float z) { AF_Vector3 v{x, y, z}; return v; }

struct Knobs {
    float w5[5] = {1, 1, 0.126f, 1, 1};
    float adj = 1.0f, contain = 1.0f, precedence = 6.0f, latePow = 1.5f;
    int early = 4, rays = 256;
    float shadow = 0.0f;   // 影のこもり（dB、2026-10-06 から記録の末尾に載る。それより前の記録には無いので 0 ＝ 当時の音）
};

struct EmitterUpd { int id; AF_Vector3 p; float r; int op; float loud; };
struct BoxUpd { int id; AF_Vector3 c, ax, ay; };
struct Frame {
    double t = 0.0; float dt = 0.0f;
    AF_Vector3 l{}, fwd{}, up{};
    bool hasKnobs = false; Knobs knobs;
    std::vector<EmitterUpd> emitters;
    std::vector<BoxUpd> boxes;
};
struct EmitterDecl { double t; int id; AF_Vector3 p; float r, gain; };
struct BoxDecl { int id; AF_Vector3 c, h, ax, ay; int preset, dyn; };

/// 入力の WAV を読む（PCM 16 / float 32、何 ch でも平均してモノラル）。
bool ReadWavMono(const char* path, std::vector<float>& mono, int& rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    size_t pos = 12, off = 0, len = 0; int tag = 0, ch = 0, bits = 0; rate = 0;
    while (pos + 8 <= b.size()) {
        const uint32_t n = u32(pos + 4);
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0) { tag = static_cast<int>(u16(pos + 8)); ch = static_cast<int>(u16(pos + 10)); rate = static_cast<int>(u32(pos + 12)); bits = static_cast<int>(u16(pos + 22)); }
        else if (std::memcmp(b.data() + pos, "data", 4) == 0) { off = pos + 8; len = std::min<size_t>(n, b.size() - off); break; }
        pos += 8 + n + (n & 1);
    }
    if (!off || ch <= 0 || bits <= 0) return false;
    const int bps = bits / 8;
    const size_t frames = len / static_cast<size_t>(bps * ch);
    mono.assign(frames, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (int c = 0; c < ch; ++c) {
            const char* p = b.data() + off + (i * ch + c) * bps;
            if (tag == 3 && bps == 4) { float v; std::memcpy(&v, p, 4); s += v; }
            else if (bps == 2) { int16_t v; std::memcpy(&v, p, 2); s += v / 32768.0f; }
        }
        mono[i] = s / static_cast<float>(ch);
    }
    return true;
}

bool WriteWav(const char* path, const std::vector<float>& lr, int rate) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int32_t n = static_cast<int32_t>(lr.size() / 2), data = n * 4, riff = 36 + data, fmtSize = 16, byteRate = rate * 4;
    const int16_t fmt = 1, ch = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (float v : lr) { const int16_t s = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f)); std::fwrite(&s, 2, 1, f); }
    std::fclose(f);
    return true;
}

/// Unreal の PushKnobs と同じ呼び方（値は記録から、それ以外は PushKnobs に書いてある定数）。
void PushKnobs(AF_WorldHandle w, const Knobs& k) {
    AF_WorldSetWeights(w, k.w5);
    AF_WorldSetAdjacentLateWeight(w, k.adj);
    AF_WorldSetShadowMuffle(w, k.shadow);
    AF_WorldSetResponse(w, 0.0f, 0.04f, 0.05f, 0.06f);
    AF_WorldSetRays(w, k.rays, 40);
    AF_WorldSetLeakModel(w, 1);
    AF_WorldSetLateThrough(w, 2);
    AF_WorldSetDoorPull(w, 0.0f);
    AF_WorldSetWallReflect(w, 0);
    AF_WorldSetEarlyModel(w, k.early);
    AF_WorldSetImageSurface(w, 20.0f, 15.0f, 5.0f, 1.0f, 1.0f);
    AF_WorldSetAdjacentContain(w, k.contain);
    AF_WorldSetDoorCoherence(w, 3000.0f);
    AF_WorldSetPrecedence(w, k.precedence, 0.04f);
    AF_WorldSetLateDistanceShape(w, k.latePow);
    AF_WorldSetLaneModel(w, 1);
    AF_WorldSetHeadCm(w, 57.0f);
    AF_WorldSetBudget(w, 1536, 6, 10, 1);
    AF_WorldSetRayGroups(w, 4);
    AF_WorldSetMaxRaysPerEmitter(w, 512);
}

AF_Vector3 Read3(std::istringstream& in) { float x = 0, y = 0, z = 0; in >> x >> y >> z; return V(x, y, z); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: AfReplayWav <AFCapture_scene.txt> <input.wav> <output.wav>\n"); return 2; }
    // ── 記録を読む ──
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "記録を開けない: %s\n", argv[1]); return 3; }
    int rate = 48000; float seed = 0.6f; int outside = 0;
    std::vector<BoxDecl> boxes; std::vector<EmitterDecl> emitters; std::vector<Frame> frames;
    Knobs knobs0; bool haveKnobs0 = false; Knobs pending; bool pendingKnobs = false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string k; s >> k;
        if (k == "rate") s >> rate;
        else if (k == "room") s >> seed >> outside;
        else if (k == "box") { BoxDecl b; s >> b.id; b.c = Read3(s); b.h = Read3(s); b.ax = Read3(s); b.ay = Read3(s); s >> b.preset >> b.dyn; boxes.push_back(b); }
        else if (k == "knobs") {
            Knobs kn; for (float& x : kn.w5) s >> x;
            s >> kn.adj >> kn.early >> kn.contain >> kn.precedence >> kn.latePow >> kn.rays;
            if (!(s >> kn.shadow)) kn.shadow = 0.0f;   // 古い記録（末尾に無い）
            if (!haveKnobs0 && frames.empty()) { knobs0 = kn; haveKnobs0 = true; } else { pending = kn; pendingKnobs = true; }
        }
        else if (k == "emitter") { EmitterDecl e; s >> e.t >> e.id; e.p = Read3(s); s >> e.r >> e.gain; emitters.push_back(e); }
        else if (k == "f") {
            Frame f; s >> f.t >> f.dt; f.l = Read3(s); f.fwd = Read3(s); f.up = Read3(s);
            if (pendingKnobs) { f.hasKnobs = true; f.knobs = pending; pendingKnobs = false; }
            frames.push_back(f);
        }
        else if (k == "e" && !frames.empty()) { EmitterUpd u; s >> u.id; u.p = Read3(s); s >> u.r >> u.op >> u.loud; frames.back().emitters.push_back(u); }
        else if (k == "b" && !frames.empty()) { BoxUpd u; s >> u.id; u.c = Read3(s); u.ax = Read3(s); u.ay = Read3(s); frames.back().boxes.push_back(u); }
    }
    if (frames.empty()) { std::fprintf(stderr, "記録にフレームが無い\n"); return 4; }
    std::vector<float> src; int srcRate = 0;
    if (!ReadWavMono(argv[2], src, srcRate) || src.empty()) { std::fprintf(stderr, "入力の WAV を読めない: %s\n", argv[2]); return 5; }
    std::printf("記録: 箱 %zu・音源 %zu・フレーム %zu（%.2f 秒）、入力 %.1f 秒 %d Hz\n", boxes.size(), emitters.size(), frames.size(),
                frames.back().t, static_cast<double>(src.size()) / srcRate, srcRate);

    // ── 世界（Unreal の BeginPlay と同じ順）──
    AF_WorldHandle w = AF_WorldCreate();
    std::map<int, int> matOfPreset;
    for (const BoxDecl& b : boxes) {
        if (!matOfPreset.count(b.preset)) matOfPreset[b.preset] = AF_WorldAddMaterialPreset(w, b.preset);
        const int id = AF_WorldAddBox(w, b.c, b.h, b.ax, b.ay, matOfPreset[b.preset], b.dyn);
        if (id != b.id) std::printf("  ★箱の番号がずれた: 記録 %d → %d\n", b.id, id);
    }
    AF_WorldSetRoomSeedRadius(w, seed);
    AF_WorldSetOutsideMouth(w, outside);
    PushKnobs(w, knobs0);
    AF_WorldSetWorkers(w, 1);
    AF_WorldBuild(w);
    std::printf("部屋 %d・戸口 %d\n", AF_WorldRoomCount(w), AF_WorldApertureCount(w));
    // 器（Unreal の CreateShared / RebindFdn と同じ）
    AF_DirectionBusHandle bus = AF_DirectionBusCreateVertical(rate, 8, 4096, 2);
    AF_DirectionBusSetCrossover(bus, 700.0f);
    AF_HrtfHandle busHrtf = AF_HrtfCreateSynthetic(rate);
    AF_DirectionBusSetHrtf(bus, busHrtf, 57.0f);
    AF_DirectionBusSetPanSplit(bus, 0);
    std::vector<AF_FdnMixHandle> fdns;
    auto rebind = [&]() {
        AF_FdnMixHandle f = AF_FdnMixCreate(rate, 4096, 0.6f);
        AF_FdnMixSetDirectionBus(f, bus, 57.0f);
        fdns.push_back(f);
        AF_WorldBindFdn(w, f);
        return f;
    };
    AF_FdnMixHandle fdn = rebind();
    // 音源（UAcousticFlowEmitter::BeginPlay と同じ）。登録の時刻になったら世界へ足す。
    struct Live { int recId; int worldId; AF_VoiceHandle voice; AF_HrtfHandle hrtf; long long startSample; };
    std::vector<Live> live;
    std::map<int, int> worldOfRec;
    size_t nextEmitter = 0;
    auto addDue = [&](double t) {
        while (nextEmitter < emitters.size() && emitters[nextEmitter].t <= t) {
            const EmitterDecl& e = emitters[nextEmitter++];
            AF_VoiceConfig cfg{};
            cfg.sampleRate = rate; cfg.maxFrames = 4096; cfg.tailSeconds = 1.0f;
            cfg.tapCrossfadeMs = 30.0f; cfg.hrtfCrossfadeMs = 12.0f; cfg.hrtfCrossoverHz = 700.0f; cfg.tailFirstBlock = 64; cfg.tailCapBlock = 8192;
            Live L{e.id, AF_WorldAddEmitter(w, e.p, e.r), AF_VoiceCreate(&cfg), AF_HrtfCreateSynthetic(rate), std::llround(e.t * rate)};
            AF_VoiceSetHrtf(L.voice, L.hrtf); AF_VoiceSetHrtfEnabled(L.voice, 1);
            AF_VoiceSetOutputGain(L.voice, e.gain); AF_VoiceSetTailLevel(L.voice, 1.0f);
            AF_VoiceSetFdnMix(L.voice, fdn); AF_VoiceSetDirectionBus(L.voice, bus);
            worldOfRec[e.id] = L.worldId;
            live.push_back(L);
        }
    };

    // ── 鳴らす ──
    const long long total = std::llround((frames.back().t + 0.5) * rate);
    std::vector<float> out(static_cast<size_t>(total) * 2, 0.0f);
    std::vector<float> mono(kBlock), l(kBlock), r(kBlock), fl(kBlock), fr(kBlock);
    size_t fi = 0;
    double peak = 0.0;
    for (long long s0 = 0; s0 < total; s0 += kBlock) {
        const int n = static_cast<int>(std::min<long long>(kBlock, total - s0));
        const double tb = static_cast<double>(s0) / rate;
        // このブロックの頭までに起きたフレームを当てる（ゲームのスレッドの仕事）
        while (fi < frames.size() && frames[fi].t <= tb) {
            const Frame& f = frames[fi++];
            addDue(f.t);
            for (const BoxUpd& b : f.boxes) AF_WorldSetBoxTransform(w, b.id, b.c, b.ax, b.ay);
            AF_WorldSetListener(w, f.l, f.fwd, f.up);
            if (f.hasKnobs) PushKnobs(w, f.knobs);
            for (const EmitterUpd& u : f.emitters) {
                auto it = worldOfRec.find(u.id);
                if (it != worldOfRec.end()) AF_WorldSetEmitter(w, it->second, u.p, u.r, u.op, u.loud);
            }
            AF_WorldUpdate(w, std::max(1e-4f, f.dt));
            if (AF_WorldFdnStale(w)) { fdn = rebind(); for (Live& L : live) AF_VoiceSetFdnMix(L.voice, fdn); }
            for (Live& L : live) AF_WorldApplyVoice(w, L.worldId, L.voice, rate);
        }
        // 音のスレッドの仕事（Wwise の自前プラグインと同じ並び: 各 Voice → FDN → 方向バス）
        float* o = out.data() + s0 * 2;
        for (Live& L : live) {
            for (int i = 0; i < n; ++i) {
                const long long k = s0 + i - L.startSample;
                if (k < 0) { mono[static_cast<size_t>(i)] = 0.0f; continue; }
                // 入力の周波数から 48 kHz へ（直線補間、ループ）。Wwise も試験の音（44.1 kHz）を出力の周波数へ変えて鳴らしている。
                const double pos = static_cast<double>(k) * srcRate / rate;
                const long long j = static_cast<long long>(pos);
                const float fr2 = static_cast<float>(pos - static_cast<double>(j));
                const size_t a = static_cast<size_t>(j % static_cast<long long>(src.size())), b = static_cast<size_t>((j + 1) % static_cast<long long>(src.size()));
                mono[static_cast<size_t>(i)] = src[a] * (1.0f - fr2) + src[b] * fr2;
            }
            AF_VoiceRender(L.voice, mono.data(), n, l.data(), r.data(), nullptr);
            for (int i = 0; i < n; ++i) { o[i * 2] += l[static_cast<size_t>(i)]; o[i * 2 + 1] += r[static_cast<size_t>(i)]; }
        }
        std::fill(fl.begin(), fl.end(), 0.0f); std::fill(fr.begin(), fr.end(), 0.0f);
        AF_FdnMixRender(fdn, n, fl.data(), fr.data());
        AF_DirectionBusRender(bus, n, fl.data(), fr.data());
        for (int i = 0; i < n; ++i) {
            o[i * 2] += fl[static_cast<size_t>(i)]; o[i * 2 + 1] += fr[static_cast<size_t>(i)];
            peak = std::max(peak, static_cast<double>(std::max(std::fabs(o[i * 2]), std::fabs(o[i * 2 + 1]))));
        }
    }
    if (!WriteWav(argv[3], out, rate)) { std::fprintf(stderr, "書けない: %s\n", argv[3]); return 6; }
    std::printf("書いた: %s（%.2f 秒、最大 %.1f dBFS）\n", argv[3], static_cast<double>(total) / rate, 20.0 * std::log10(std::max(peak, 1e-9)));
    for (Live& L : live) AF_VoiceDestroy(L.voice);
    AF_WorldBindFdn(w, nullptr);
    AF_WorldDestroy(w);
    return 0;
}
