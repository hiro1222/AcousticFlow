/* capture_emit.h ── 見つけた破れを、そのまま C++ の回帰テストにする。
 *
 * ★★ 絶対値を焼いてはいけない ★★
 *   このリポジトリの方針は「期待値を**絶対値でなく関係**で書く」
 *   （scene_regression.cpp 冒頭）。フレーム 412 の 0.0837 を焼くと、
 *   **正当なチューニングのたびに落ちて、すぐ信用されなくなる**。
 *   → 吐くのは「入力（形・位置の列）」と「どの性質を縛るか（型紙）」だけ。
 *      期待値は型紙が持つ。だから吐いた検査には**録れた出力の数値が 1 つも入らない**。
 *
 * 何を吐くか:
 *   1. 静的な形（箱）      ← キャプチャのヘッダから
 *   2. 入力の列（リスナー / 音源 / 動いた実体）← フレーム記録から
 *   3. 型紙の呼び出し 1 行  ← 走査で鳴った型紙
 *
 * ⚠ メッシュを含む場面は吐けない（docs/SOUND_DEBUG_TOOL.md §5.2）。
 *   本番ステージは 8,102 三角形。→ 形はデータファイルで持つ形が要る（未着手）。
 */
#ifndef ACOUSTICFLOW_TEST_CAPTURE_EMIT_H
#define ACOUSTICFLOW_TEST_CAPTURE_EMIT_H

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "capture_scan.h"

namespace af {
namespace emit {

using acoustic::dbg::CaptureFile;

struct EmitOptions {
    int windowBefore = 30;   // 印の前後どれだけを検査にするか
    int windowAfter  = 30;
    /* ★窓の先頭は捨てる。段の位相を戻しても**溜まりかけのエコグラム**が残るため
     *   （実測 6 フレーム。docs §3.1）。ここを入れると生成した検査が不安定になる。 */
    int warmupFrames = 6;
};

/* 吐けるか。吐けないなら理由を返す（空文字なら吐ける）。 */
inline std::string emitBlocker(const CaptureFile& cap) {
    if (cap.meshCount > 0) return "メッシュを含む場面は C++ に吐けない（docs §5.2）";
    if (cap.boxes.empty()) return "形が入っていない（古い .afcap か、箱が 0 個）";
    if (cap.frames <= 0) return "フレームが無い";
    return std::string();
}

inline void appendf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

/* float リテラルを吐く。★"%g" の結果に f を付けるだけだと 0 → "0f" になり**コンパイルが通らない**。
 *   （実際にそれで生成物が壊れていた。生成器は「吐いたものが通る」まで見ないと意味がない） */
inline std::string flt(float v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));
    std::string s(b);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
        && s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    s += "f";
    return s;
}

/* 破れ 1 件 → C++ の検査 1 つ。 */
inline std::string emitCase(const CaptureFile& cap, const af::scan::Mark& mark,
                            const char* testName, const EmitOptions& opt = EmitOptions{}) {
    std::string out;
    const std::string blocker = emitBlocker(cap);
    if (!blocker.empty()) {
        appendf(out, "// 吐けません: %s\n", blocker.c_str());
        return out;
    }

    // 印の載っている行を探す。
    int center = 0;
    for (int k = 0; k < cap.frames; ++k)
        if (static_cast<int>(cap.global[static_cast<size_t>(k)].frame) == mark.frame) { center = k; break; }
    int lo = center - opt.windowBefore, hi = center + opt.windowAfter;
    if (lo < opt.warmupFrames) lo = opt.warmupFrames;
    if (hi > cap.frames - 1) hi = cap.frames - 1;
    if (lo >= hi) { appendf(out, "// 吐けません: 窓が取れない\n"); return out; }
    const int n = hi - lo + 1;

    appendf(out, "// ================================================================\n");
    appendf(out, "// %s ── キャプチャから自動生成（af::emit）\n", testName);
    appendf(out, "//   元: 場面 %s / frame %d / 音源 %llu\n",
            cap.scene.c_str(), mark.frame, (unsigned long long)mark.sourceId);
    appendf(out, "//   鳴った型紙: %s（%s %.3f）\n", mark.templateName, mark.what, mark.amount);
    appendf(out, "//\n");
    appendf(out, "// ★期待値は**絶対値でなく性質**で書いてある。録れた出力の数値は入っていない。\n");
    appendf(out, "//   絶対値を焼くと、正当なチューニングのたびに落ちて信用されなくなる。\n");
    appendf(out, "// ★窓の先頭 %d フレームは捨ててある（段の位相を戻しても溜まりかけの\n",
            opt.warmupFrames);
    appendf(out, "//   エコグラムが残るため。docs/SOUND_DEBUG_TOOL.md §3.1）。\n");
    appendf(out, "void %s() {\n", testName);
    appendf(out, "    std::printf(\"\\n[生成] %s\\n\");\n", testName);
    appendf(out, "    AF_SceneHandle s = AF_SceneCreate();\n");
    // ★材質も吐く。既定材質で代用すると**別の音を測る検査**になる
    //   （最初これを忘れていて、生成物は全部の箱に既定材質を使っていた）。
    appendf(out, "    // ── 材質（%d 個）。★録れた実物。既定で代用すると別の音を測ることになる ──\n",
            (int)cap.materials.size());
    if (cap.materials.empty()) {
        appendf(out, "    const int mat0 = AF_SceneAddMaterial(s, nullptr, nullptr, nullptr, 0);\n");
        appendf(out, "    (void)mat0;   // ⚠ 材質の入っていない古い .afcap から生成\n");
    }
    for (size_t i = 0; i < cap.materials.size(); ++i) {
        const auto& m = cap.materials[i];
        std::string t, a, sc;
        for (int b = 0; b < acoustic::dbg::kCapBands; ++b) {
            if (b) { t += ","; a += ","; sc += ","; }
            t += flt(m.transmission[b]); a += flt(m.absorption[b]); sc += flt(m.scattering[b]);
        }
        appendf(out, "    static const float kT%zu[6] = {%s};\n", i, t.c_str());
        appendf(out, "    static const float kA%zu[6] = {%s};\n", i, a.c_str());
        appendf(out, "    static const float kS%zu[6] = {%s};\n", i, sc.c_str());
        appendf(out, "    const int mat%zu = AF_SceneAddMaterial(s, kT%zu, kA%zu, kS%zu, 6);\n",
                i, i, i, i);
    }
    appendf(out, "    // ── 静的な形（箱 %d 個）──\n", (int)cap.boxes.size());
    for (const auto& b : cap.boxes) {
        char matRef[32];
        if (cap.materials.empty()) std::snprintf(matRef, sizeof(matRef), "mat0");
        else std::snprintf(matRef, sizeof(matRef), "mat%d",
                           (b.material >= 0 && b.material < (int)cap.materials.size())
                               ? b.material : 0);
        appendf(out, "    AF_SceneAddInstanceBox(s, V(%s,%s,%s), V(%s,%s,%s), "
                     "V(%s,%s,%s), V(%s,%s,%s), %s);\n",
                flt(b.cx).c_str(), flt(b.cy).c_str(), flt(b.cz).c_str(),
                flt(b.hx).c_str(), flt(b.hy).c_str(), flt(b.hz).c_str(),
                flt(b.rx).c_str(), flt(b.ry).c_str(), flt(b.rz).c_str(),
                flt(b.ux).c_str(), flt(b.uy).c_str(), flt(b.uz).c_str(), matRef);
    }

    appendf(out, "\n    // ── 入力の列（%d フレーム）──\n", n);
    appendf(out, "    static const float kL[][3] = {\n");
    for (int k = lo; k <= hi; ++k) {
        const auto& g = cap.global[static_cast<size_t>(k)];
        appendf(out, "        {%s,%s,%s},\n",
                flt(g.lx).c_str(), flt(g.ly).c_str(), flt(g.lz).c_str());
    }
    appendf(out, "    };\n");
    appendf(out, "    static const float kS[][3] = {\n");
    for (int k = lo; k <= hi; ++k) {
        float sx = 0, sy = 0, sz = 0;
        for (const auto& src : cap.sources[static_cast<size_t>(k)])
            if (src.id == mark.sourceId) { sx = src.sx; sy = src.sy; sz = src.sz; break; }
        appendf(out, "        {%s,%s,%s},\n", flt(sx).c_str(), flt(sy).c_str(), flt(sz).c_str());
    }
    appendf(out, "    };\n");

    // 動いた実体。★1 つに絞る（扉が複数動く場面は今は吐かない）。
    int movedInstance = -1;
    for (int k = lo; k <= hi && movedInstance < 0; ++k)
        for (const auto& m : cap.moved[static_cast<size_t>(k)]) { movedInstance = m.instance; break; }
    if (movedInstance >= 0) {
        appendf(out, "    static const float kMoved[][12] = {   // 実体 %d の transform\n",
                movedInstance);
        for (int k = lo; k <= hi; ++k) {
            const acoustic::dbg::CapMoved* found = nullptr;
            for (const auto& m : cap.moved[static_cast<size_t>(k)])
                if (m.instance == movedInstance) { found = &m; break; }
            if (found)
                appendf(out, "        {%s,%s,%s, %s,%s,%s, %s,%s,%s, %s,%s,%s},\n",
                        flt(found->cx).c_str(), flt(found->cy).c_str(), flt(found->cz).c_str(),
                        flt(found->hx).c_str(), flt(found->hy).c_str(), flt(found->hz).c_str(),
                        flt(found->rx).c_str(), flt(found->ry).c_str(), flt(found->rz).c_str(),
                        flt(found->ux).c_str(), flt(found->uy).c_str(), flt(found->uz).c_str());
            else
                appendf(out, "        {0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f, "
                             "0.0f,0.0f,0.0f},   // このフレームは動いていない\n");
        }
        appendf(out, "    };\n");
    }

    appendf(out, "\n    const int kN = %d;\n", n);
    appendf(out, "    std::vector<float> lvlDb, at, bands;\n");
    appendf(out, "    for (int k = 0; k < kN; ++k) {\n");
    if (movedInstance >= 0) {
        appendf(out, "        if (kMoved[k][3] != 0.0f || kMoved[k][4] != 0.0f)\n");
        appendf(out, "            AF_SceneUpdateInstance(s, %d,\n", movedInstance);
        appendf(out, "                V(kMoved[k][0],kMoved[k][1],kMoved[k][2]),\n");
        appendf(out, "                V(kMoved[k][3],kMoved[k][4],kMoved[k][5]),\n");
        appendf(out, "                V(kMoved[k][6],kMoved[k][7],kMoved[k][8]),\n");
        appendf(out, "                V(kMoved[k][9],kMoved[k][10],kMoved[k][11]));\n");
    }
    appendf(out, "        AF_SceneSetListener(s, V(kL[k][0], kL[k][1], kL[k][2]));\n");
    appendf(out, "        AF_SceneSetSource(s, %lluULL, V(kS[k][0], kS[k][1], kS[k][2]));\n",
            (unsigned long long)mark.sourceId);
    appendf(out, "        AF_SceneUpdate(s, 1.0f / 60.0f);\n");
    appendf(out, "        const int idx = AF_SceneSourceIndex(s, %lluULL);\n",
            (unsigned long long)mark.sourceId);
    appendf(out, "        float g[6] = {};\n");
    appendf(out, "        AF_SceneGetSourceOcclusion(s, idx, g);\n");
    appendf(out, "        float sum = 0.0f;\n");
    appendf(out, "        for (int b = 0; b < 6; ++b) { bands.push_back(g[b]); sum += g[b]; }\n");
    appendf(out, "        lvlDb.push_back(af::detect::db(sum / 6.0f));\n");
    appendf(out, "        at.push_back((float)k);\n");
    appendf(out, "    }\n");
    appendf(out, "    AF_SceneDestroy(s);\n\n");

    // ── 縛りは型紙 1 行 ──
    appendf(out, "    // ★縛るのは性質だけ。走査（デバッグツール）が呼ぶのと**同じ関数**。\n");
    appendf(out, "    af::detect::Break br[32];\n");
    const std::string tmpl(mark.templateName);
    if (tmpl.find("型紙1") != std::string::npos) {
        appendf(out, "    const int nb = af::detect::noJump(lvlDb.data(), at.data(), kN,\n");
        appendf(out, "                                     6.0f, br, 32);\n");
        appendf(out, "    af::detect::report(\"型紙1 跳ばない\", br, nb, \" dB\");\n");
        appendf(out, "    check(\"[生成] %s: 隣り合うフレームで 6dB を超えて跳ばない\", nb == 0);\n",
                testName);
    } else if (tmpl.find("型紙4") != std::string::npos) {
        appendf(out, "    const int nb = af::detect::noPhantom(bands.data(), nullptr, at.data(),\n");
        appendf(out, "                                        kN, 3.0f, br, 32);\n");
        appendf(out, "    af::detect::report(\"型紙4 幻が出ない\", br, nb, \" dB\");\n");
        appendf(out, "    check(\"[生成] %s: 遮蔽されているのに帯域がまっすぐ、が無い\", nb == 0);\n",
                testName);
    } else if (tmpl.find("型紙D") != std::string::npos) {
        appendf(out, "    const int nb = af::detect::withinPhysicalBound(bands.data(), nullptr,\n");
        appendf(out, "                                                  kN * 6, 1.0f, br, 32);\n");
        appendf(out, "    af::detect::report(\"型紙D 上限を超えない\", br, nb, \"\");\n");
        appendf(out, "    check(\"[生成] %s: 生存ゲインが 1.0 を超えない\", nb == 0);\n", testName);
    } else {
        appendf(out, "    // ⚠ この型紙の吐き出しは未実装: %s\n", mark.templateName);
        appendf(out, "    (void)br;\n");
    }
    appendf(out, "}\n");
    return out;
}

}  // namespace emit
}  // namespace af

#endif  // ACOUSTICFLOW_TEST_CAPTURE_EMIT_H
