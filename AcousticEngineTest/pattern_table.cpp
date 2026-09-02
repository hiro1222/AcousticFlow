// pattern_table.cpp — 資料の「扉の開き具合」の図に載せる数値を実測する。
//
// ★何のためにあるか
//   紹介資料の p5 に置く図（patterns_all.svg）は、部屋・戸口・扉・音源 3 本の平面図。
//   その右に並べる数字を**推測ではなく実測**で埋めるためのもの。
//   図と数字が同じ配置から出ていることを、コマンド 1 本で再現できる状態にしておく。
//
// ★図からの寸法の取り方（SVG 100 単位 = 1 m）
//   部屋の内側   x 146→854 / y 196→544  →  7.08 m × 3.48 m（高さは 3.0 m を与える）
//   壁の厚み     16 単位 → 0.16 m
//   戸口         x 455→545（90 単位 = 0.90 m 幅）を下側の壁に空ける。高さ 2.0 m
//   扉           0.90 × 2.0 × 0.08 m。蝶番は戸口の右端（SVG の rotate 中心 x=545）
//   音源1/2/3    (306,432) (500,320) (694,432) → 下の kSrc
//   ★リスナーは図に描かれていないので、**戸口の面**（壁の外側の面・床上 1.6 m）に置く。
//     ここを 0 にしてあるのは、扉が無いときに 3 本とも素通し（0.0 dB）になる位置だから。
//     外へ下がると戸口の縁が左右の音源を遮り、扉のせいでない減衰が混ざる。
//     ここを変えると数字は変わる。図に描き足すときは同じ位置にすること。
//
// 使い方:  AfPatternTable            全パターンの表を出す
//          AfPatternTable csv        CSV で出す（資料へ貼る用）
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <cstdlib>
#include <vector>
#include <algorithm>

#include "acoustic_scene.h"

namespace {

constexpr int kBands = 6;
const char* kBandName[kBands] = { "125", "250", "500", "1k", "2k", "4k" };

AF_Vector3 V(float x, float y, float z) { AF_Vector3 v{x, y, z}; return v; }

// SVG 座標 → 世界座標。x はそのまま、SVG の y は世界の z（奥行き）へ。
//   部屋の内側の左上 (146,196) を原点にする。
// 部屋の倍率（AF_ROOMSCALE、既定 1.0）。★扉の幅と壁厚は**実寸のまま**にする。
//   部屋だけ広げると、同じ扉が相対的に小さくなる ── 図の見た目を実物に寄せるため。
//   ★もう 1 つ狙いがある: 閉扉の「幻」は**積分の窓が部屋より大きい**ときに出る疑いがある。
//     部屋を広げて幻が消えるなら、その見立てが正しいことになる。
float roomScale() {
    static float s = -1.0f;
    if (s < 0.0f) {
        s = 1.0f;
        if (const char* e = std::getenv("AF_ROOMSCALE")) {
            const float v = static_cast<float>(std::atof(e));
            if (v >= 0.5f && v <= 8.0f) s = v;
        }
    }
    return s;
}

AF_Vector3 fromSvg(float sx, float sy, float height) {
    const float k = roomScale();
    return V((sx - 146.0f) / 100.0f * k, height, (sy - 196.0f) / 100.0f * k);
}

const float kRoomW = (854.0f - 146.0f) / 100.0f * roomScale();   // 既定 7.08
const float kRoomD = (544.0f - 196.0f) / 100.0f * roomScale();   // 既定 3.48
constexpr float kRoomH = 3.0f;
constexpr float kWall  = 0.16f;
// 戸口の幅は環境変数で振れる（AF_GAPW、既定 0.90 m）。扉板の幅も同じにする。
constexpr float kDoorH = 2.0f;
constexpr float kDoorT = 0.08f;

// 戸口の中心（世界 x）。SVG の 455〜545 の中点。
const float kGapCx = ((455.0f + 545.0f) * 0.5f - 146.0f) / 100.0f * roomScale();   // 既定 3.54

struct Pattern { const char* name; float deg; bool hasDoor; };
// ★資料 p6 の表と**同じ角度**にすること。ここがずれると
//   「この表は実行ファイル 1 本で再現できます」が嘘になる。
// ⚠ 100° を使わないのは、89°→90°→91° に不連続があり
//   その先の枝（90〜120° 台）の値が信用できないため（p18 の未解決課題）。
//   80° は 3 回連続で同値を確認済み。
const Pattern kPatFixed[] = {
    { "開口 0°（閉）", 0.0f,   true  },
    { "開口 40°",      40.0f,  true  },
    { "開口 80°",      80.0f,  true  },
    { "開口 160°",     160.0f, true  },
    { "ドアなし",      0.0f,   false },
};

// ★AF_SWEEP=1 で 0°〜180° を 10° 刻みに振る。
//   「角度が緩いのに音が減る」のが本当か、**曲線の形**で確かめるため。
//   飛び飛びの 4 点だけ見ていると、凹みが本物か測り方の綾か区別できない。
std::vector<Pattern> buildPatterns(std::vector<std::string>& names) {
    std::vector<Pattern> out;
    const char* sw = std::getenv("AF_SWEEP");
    if (!sw || std::string(sw) != "1") {
        for (const Pattern& p : kPatFixed) out.push_back(p);
        return out;
    }
    // 範囲と刻みは環境変数で絞れる（AF_SWEEPFROM / AF_SWEEPTO / AF_SWEEPSTEP）。
    //   ★段差の疑いがある所を**細かく**見るため。10° 刻みだけだと、
    //     幾何の変化なのか二値の判定が切り替わったのか区別できない。
    int from = 0, to = 180, step = 10;
    if (const char* e = std::getenv("AF_SWEEPFROM")) from = std::atoi(e);
    if (const char* e = std::getenv("AF_SWEEPTO"))   to   = std::atoi(e);
    if (const char* e = std::getenv("AF_SWEEPSTEP")) step = std::max(1, std::atoi(e));
    names.reserve(static_cast<std::size_t>((to - from) / step + 4));
    for (int d = from; d <= to; d += step) {
        names.push_back("開口 " + std::to_string(d) + "°");
        out.push_back(Pattern{ names.back().c_str(), static_cast<float>(d), true });
    }
    names.push_back("ドアなし");
    out.push_back(Pattern{ names.back().c_str(), 0.0f, false });
    return out;
}

// 図の音源 3 本（SVG 座標）。高さは床上 1.5 m。
struct Src { const char* name; float sx, sy; };
const Src kSrc[] = {
    { "音源1", 306.0f, 432.0f },
    { "音源2", 500.0f, 320.0f },
    { "音源3", 694.0f, 432.0f },
};

float toDb(float lin) { return 20.0f * std::log10(std::fmax(1e-6f, lin)); }

// 箱を 1 つ足す。center と halfExtent で指定する。
void addBox(AF_SceneHandle s, AF_Vector3 c, AF_Vector3 he, int mat) {
    AF_SceneAddInstanceBox(s, c, he, V(1, 0, 0), V(0, 1, 0), mat);
}

}  // namespace

int main(int argc, char** argv) {
    const bool csv = (argc > 1 && std::string(argv[1]) == "csv");

    // 戸口の幅は環境変数で振れる（AF_GAPW、既定 0.90 m）。扉板の幅も同じにする。
    //   広げると、戸口の縁が左右の音源を遮るぶんが減って「ドアなし」が 0 dB に近づく。
    float kDoorW = 0.90f;
    if (const char* g = std::getenv("AF_GAPW")) {
        const float v = static_cast<float>(std::atof(g));
        if (v >= 0.2f && v <= 5.0f) kDoorW = v;
    }
    const float gapCx = kGapCx;
    const float kGapL = gapCx - kDoorW * 0.5f;
    const float kGapR = gapCx + kDoorW * 0.5f;
    // リスナーの距離は環境変数で振れる（AF_LDIST、既定 0.0 m ＝ 戸口の面）。
    //   図に載せる数字がどの立ち位置のものかを、後から変えられるようにしてある。
    float ldist = 0.0f;
    if (const char* d = std::getenv("AF_LDIST")) { const float v = (float)std::atof(d); if (v >= 0.0f && v <= 20.0f) ldist = v; }
    const AF_Vector3 L = V(gapCx, 1.6f, kRoomD + kWall + ldist);

    if (!csv) {
        std::printf("\n扉の開き具合 → 音源ごとの帯域別ゲイン（資料 p5 の図に対応）\n");
        std::printf("  部屋 %.2f × %.2f × %.1f m ／ 戸口 %.2f m 幅 ／ 壁厚 %.2f m\n",
                    kRoomW, kRoomD, kRoomH, kDoorW, kWall);
        std::printf("  リスナー (%.2f, %.2f, %.2f) ＝ 戸口の正面・外側 %.2f m\n\n",
                    L.x, L.y, L.z, ldist);
    } else {
        std::printf("pattern,source");
        for (int b = 0; b < kBands; ++b) std::printf(",%sHz_dB", kBandName[b]);
        std::printf(",broadband_dB\n");
    }

    std::vector<std::string> sweepNames;
    const std::vector<Pattern> kPat = buildPatterns(sweepNames);
    for (const Pattern& p : kPat) {
        AF_SceneHandle s = AF_SceneCreate();
        const int mat = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);

        const float cy = kRoomH * 0.5f;
        const float hw = kWall * 0.5f;

        // 左右の壁
        addBox(s, V(-hw, cy, kRoomD * 0.5f), V(hw, cy, kRoomD * 0.5f + kWall), mat);
        addBox(s, V(kRoomW + hw, cy, kRoomD * 0.5f), V(hw, cy, kRoomD * 0.5f + kWall), mat);
        // 奥の壁（z=0 側）
        addBox(s, V(kRoomW * 0.5f, cy, -hw), V(kRoomW * 0.5f + kWall, cy, hw), mat);
        // 天井と床（部屋として抽出させるために要る）
        addBox(s, V(kRoomW * 0.5f, kRoomH + hw, kRoomD * 0.5f),
               V(kRoomW * 0.5f + kWall, hw, kRoomD * 0.5f + kWall), mat);
        addBox(s, V(kRoomW * 0.5f, -hw, kRoomD * 0.5f),
               V(kRoomW * 0.5f + kWall, hw, kRoomD * 0.5f + kWall), mat);

        // 手前の壁（z=kRoomD 側）を、戸口の左右と上に分けて置く
        const float zf = kRoomD + hw;
        addBox(s, V(kGapL * 0.5f, cy, zf), V(kGapL * 0.5f, cy, hw), mat);
        addBox(s, V((kGapR + kRoomW) * 0.5f, cy, zf),
               V((kRoomW - kGapR) * 0.5f, cy, hw), mat);
        addBox(s, V(gapCx, (kDoorH + kRoomH) * 0.5f, zf),
               V(kDoorW * 0.5f, (kRoomH - kDoorH) * 0.5f, hw), mat);

        // 扉。蝶番は既定で戸口の右端（AF_HINGE=left で左端）。閉じているとき戸口を塞ぐ。
        //   ★左右反転しても表が鏡像にならなければ、実装の非対称＝疑わしい。
        int doorId = -1;
        if (p.hasDoor) {
            doorId = AF_SceneAddInstanceBox(s, V(gapCx, kDoorH * 0.5f, zf),
                                            V(kDoorW * 0.5f, kDoorH * 0.5f, kDoorT * 0.5f),
                                            V(1, 0, 0), V(0, 1, 0), mat);
            const float th = p.deg * 3.14159265f / 180.0f;
            const float c = std::cos(th), sn = std::sin(th);
            // 蝶番まわりに回した中心と、回した右方向ベクトル。
            const char* hs = std::getenv("AF_HINGE");
            const bool leftHinge = (hs && std::string(hs) == "left");
            const float hinge = leftHinge ? kGapL : kGapR;
            const float rx = gapCx - hinge;               // 右蝶番なら負・左蝶番なら正
            // 扉は既定で部屋の内側へ開く（図の弧に合わせる）。z が減る向き。
            //   蝶番の左右にかかわらず内開きになるよう、z は fabs(rx) で引く。
            // ★AF_SWING=out で外開き（リスナー側へ開く）。
            //   内開きだと扉板は**音源の側**に立つので、角度によって音源を隠す。
            //   外開きだと**リスナーの側**に立つので、隠すものが変わる。
            //   「なぜ内／外で数字が変わるのか」を実測で示すために振れるようにした。
            const char* sw = std::getenv("AF_SWING");
            const bool outward = (sw && std::string(sw) == "out");
            const float zdir = outward ? +1.0f : -1.0f;
            const float sgn = ((rx < 0.0f) ? 1.0f : -1.0f) * (outward ? -1.0f : 1.0f);
            const AF_Vector3 nc = V(hinge + rx * c, kDoorH * 0.5f,
                                    zf + zdir * std::fabs(rx) * sn);
            AF_SceneUpdateInstance(s, doorId, nc,
                                   V(kDoorW * 0.5f, kDoorH * 0.5f, kDoorT * 0.5f),
                                   V(c, 0, sgn * sn), V(0, 1, 0));
        }

        // ★ハーネス（diagnoseFrameCost）と同じ設定にする。
        //   これを与えないと config がゼロ埋めのままで、遮蔽も回折も計算されない。
        AF_UpdateConfig cfg{};
        cfg.role1EveryN = 1;   cfg.role2EveryN = 1;   cfg.earlyEveryN = 1;
        cfg.diffSrcEveryN = 1; cfg.catalogEveryN = 1;
        cfg.reflectionRays = 256; cfg.reflectionBounces = 3;
        cfg.directWeight = 1.0f;  cfg.useReflections = 1;
        // ★2026-08-23: サウンドシステム側の一時的な削除実験でこの 3 行が消えていました。
        //   戻しました。無いと catalog 無効で測ることになり、他の測定と条件が揃いません。
        cfg.useEdgeCatalog = 1;   cfg.edgeCatalogRes = 16; cfg.edgeCatalogMaxDist = 40.0f;

        cfg.enableReverb = 1;     cfg.echogramBins = 100;  cfg.echogramBinSeconds = 0.01f;
        cfg.echogramRays = 512;   cfg.echogramBounces = 24;
        cfg.speedOfSound = 343.0f; cfg.distanceRef = 1.5f;
        cfg.enableEarlyReflections = 1; cfg.earlyTaps = 4;
        cfg.earlyRays = 512; cfg.earlyBounces = 2;
        cfg.enableDiffractionSources = 1; cfg.diffSources = 3;
        AF_SceneSetUpdateConfig(s, &cfg);

        // ★成分の切り分け用。AF_REFL=0 で反射の回り込みを、AF_DIFFSRC=0 で回折二次音源を落とす。
        //   閉扉なのに漏れる、が**どの成分から来ているか**を測るために足した。
        //   ⚠ 既定は変えない。無指定なら従来の数値。
        if (const char* r = std::getenv("AF_REFL")) {
            if (std::atoi(r) == 0) { cfg.useReflections = 0; AF_SceneSetUpdateConfig(s, &cfg); }
        }
        if (const char* d2 = std::getenv("AF_DIFFSRC")) {
            if (std::atoi(d2) == 0) { cfg.enableDiffractionSources = 0; AF_SceneSetUpdateConfig(s, &cfg); }
        }

        // ★AF_BTM=1 で回折を BTM 経路に切り替える（既定 OFF ＝ 前川＋フレネル開口積分）。
        //   焼く模型を決めるための比較用。BTM は稜線を**選ばず**複素で足すので、
        //   「開口」という量を持たない ── 90° の段差と閉扉の幻が、模型由来かどうかを切り分ける。
        //   ⚠ 既定を変えない。無指定なら従来と同じ数値が出る（資料の再現性のため）。
        if (const char* b = std::getenv("AF_BTM")) {
            if (std::atoi(b) != 0) AF_SceneSetUseBtm(s, 1);
        }

        AF_SceneSetListener(s, L);
        for (int i = 0; i < 3; ++i) {
            AF_SceneSetSource(s, static_cast<unsigned long long>(i + 1),
                              fromSvg(kSrc[i].sx, kSrc[i].sy, 1.5f));
        }
        // 回折の二次音源は数フレームに 1 回しか更新されない（測定時の罠）。
        for (int k = 0; k < 4; ++k) AF_SceneUpdate(s, 1.0f / 60.0f);

        if (!csv) std::printf("── %s ──\n", p.name);

        for (int i = 0; i < 3; ++i) {
            // ★AF_SceneGetSourceOcclusion は名前と違い **帯域別の生存ゲイン**を返す
            //   （ヘッダ: 「透過⊕回折⊕反射の結果」）。これがそのまま「届く量」。
            const int idx = AF_SceneSourceIndex(s, static_cast<unsigned long long>(i + 1));
            float band[kBands] = {};
            AF_SceneGetSourceOcclusion(s, idx, band);

            float sum = 0.0f;
            for (int b = 0; b < kBands; ++b) sum += band[b] * band[b];
            const float broad = std::sqrt(sum / kBands);

            // ★AF_DIAG=1 で回折の内訳を出す。
            //   知りたいのは **openBandIsFresnel**（[18]）── フレネル開口の経路を通ったか、
            //   有界と判定できずに**前川へ落ちた**か。前川は δ しか見ないので扉を知らない。
            //   両脇の音源だけ扉が効かない原因が、この分岐かどうかを確かめるために足した。
            if (std::getenv("AF_DIAG")) {
                float d28[28] = {};
                const int np = AF_SceneDebugDiffractionPath(
                    s, L, fromSvg(kSrc[i].sx, kSrc[i].sy, 1.5f), d28, 0);
                std::printf("   [診断] %s 経路%d  フレネル=%s  面%d 有界%d 落%d  "
                            "openGain %.4f  δ %.3f\n",
                            kSrc[i].name, np,
                            (d28[18] > 0.5f) ? "はい" : "★いいえ(前川)",
                            (int)d28[22], (int)d28[23], (int)d28[26], d28[1], d28[0]);
            }

            if (csv) {
                std::printf("%s,%s", p.name, kSrc[i].name);
                for (int b = 0; b < kBands; ++b) std::printf(",%.1f", toDb(band[b]));
                std::printf(",%.1f\n", toDb(broad));
            } else {
                std::printf("   %s  ", kSrc[i].name);
                for (int b = 0; b < kBands; ++b)
                    std::printf("%s %6.1f  ", kBandName[b], toDb(band[b]));
                std::printf("│ 総 %6.1f dB  低−高 %+5.1f\n",
                            toDb(broad), toDb(band[0]) - toDb(band[5]));
            }
        }
        if (!csv) std::printf("\n");
        AF_SceneDestroy(s);
    }
    return 0;
}
