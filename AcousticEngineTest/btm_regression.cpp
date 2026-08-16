// btm_regression.cpp — BTM（有限楔の稜線積分）が正しく実装できているかを、
//                      **式を信じずに、成り立つはずの性質で**確かめる。
//
// 式は記憶から書いているので、値そのものを期待値と突き合わせても意味がない
// （期待値も同じ記憶から作ることになる）。代わりに、実装がどうであれ物理として
// 満たさなければならない性質を並べて、そこから外れないことを見る。
//
//   ① 相反性       音源と受音点を入れ替えても同じ値
//   ② 影境界で半分 薄い衝立の影境界では、回折場が直接音のちょうど 1/2（−6dB）
//   ③ 連続性       受音点を影境界を跨いで動かしても飛ばない
//   ④ 深い影の傾向 影が深いほど小さく、低域ほど大きい（前川と同じ向き）
//   ⑤ 有限性       稜線を短くすると寄与が減る（半無限のスクリーンとの違い）
//
// ② が一番効く。正規化の規約（1/4πd を使っているか等）が違えば必ずここで落ちる。
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../AcousticEngine/src/Core/btm.h"

using namespace acoustic;

namespace {

int g_pass = 0, g_fail = 0;

void check(const char* name, bool ok, const char* note = nullptr) {
    if (ok) { ++g_pass; std::printf("  [OK] %-46s %s\n", name, note ? note : ""); }
    else    { ++g_fail; std::printf("  [FAIL] %-44s %s\n", name, note ? note : ""); }
}

constexpr double kC = 343.0;
constexpr int kBands = 6;
const double kBandHz[kBands] = {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0};

// 薄い衝立の上端。板は **z=0 平面の y<0 側**、稜線は x 方向で y=0,z=0。
//   面方向は両面とも「板が伸びる向き」= (0,-1,0)。厚みが無いので開き角は 2π。
//   ★音源・受音点は板の面から外して置くこと（z≠0）。z=0 に置くと板の中になる。
btm::Edge thinScreenEdge(float halfLen = 50.0f) {
    btm::Edge e;
    e.a = Vec3(-halfLen, 0.0f, 0.0f);
    e.b = Vec3( halfLen, 0.0f, 0.0f);
    e.faceDir0 = Vec3(0, -1, 0);
    e.faceDir1 = Vec3(0, -1, 0);
    return e;
}

}  // namespace

int main() {
    std::printf("=== BTM（有限楔の稜線積分）の性質チェック ===\n");
    std::printf("  式は記憶から書いている。値ではなく性質で検証する。\n\n");

    // ---------------------------------------------------------------- ① 相反性
    std::printf("[①] 相反性（音源と受音点を入れ替えても同じ）\n");
    {
        const btm::Edge e = thinScreenEdge();
        const Vec3 S(0.0f, -2.0f, -4.0f);   // 板の裏、稜線より下
        const Vec3 R(0.0f, -1.0f,  5.0f);   // 板の表、稜線より下＝影の中
        float g1[kBands] = {}, g2[kBands] = {}, d1 = 0, d2 = 0;
        const bool ok1 = btm::edgeBands(e, S, R, kBandHz, kBands, kC, g1, &d1);
        const bool ok2 = btm::edgeBands(e, R, S, kBandHz, kBands, kC, g2, &d2);
        check("両方向とも計算できる", ok1 && ok2);
        double worst = 0.0;
        for (int b = 0; b < kBands; ++b) {
            const double rel = std::fabs(g1[b] - g2[b])
                             / std::max(static_cast<double>(g1[b]), 1e-9);
            worst = std::max(worst, rel);
        }
        char note[96];
        std::snprintf(note, sizeof(note), "(最大相対差 %.2e / 遅延 %.5f vs %.5f)", worst, d1, d2);
        check("入れ替えても同じ値（相対差 < 1e-4）", worst < 1e-4, note);
    }

    // ------------------------------------------------ ② 総和が影境界を跨いで連続か
    // ★これが本命の検証。回折**単体**が境界で 0.5 になるわけではない。
    //   参照実装 (EDtoolbox) は境界で特異になる項を積分から除外し、代わりに
    //   「直接音を半分にせよ」と呼び出し側へ通知する。つまり −6dB は幾何側から出る。
    //   だから見るべきは「直接音 + 回折」が境界を跨いで連続かどうか。
    //   位相を捨てて絶対値で足すと打ち消しが起きないので、**複素数で足す**。
    std::printf("\n[②] 直接音＋回折の総和が影境界を跨いで連続か（本命）\n");
    {
        const btm::Edge e = thinScreenEdge();
        const Vec3 S(0.0f, -2.0f, -4.0f);
        // 影境界は S=(0,-2,-4) から稜線(0,0,0) を延ばした先。z=6 の面では y=3。
        const int bi = 3;   // 1kHz で見る
        float prevTot = -1.0f, maxJump = 0.0f, at = 0.0f;
        std::printf("        y      直接  回折(実部,虚部)      総和\n");
        for (float y = 1.0f; y <= 5.0f; y += 0.02f) {
            const Vec3 R(0.0f, y, 6.0f);
            float g[kBands] = {};
            double re[kBands] = {}, im[kBands] = {};
            const bool notOnBoundary = btm::edgeBands(e, S, R, kBandHz, kBands, kC,
                                                      g, nullptr, 96, re, im);
            // 直接音の可視性。稜線より上（y>3 側の見通し）なら 1、影なら 0、境界なら 0.5。
            const double shadowY = 3.0;
            double direct = (y > shadowY) ? 1.0 : 0.0;
            if (!notOnBoundary) direct = 0.5;          // 境界上は半分（EDtoolbox の規約）
            const double tr = direct + re[bi], ti = im[bi];
            const float tot = static_cast<float>(std::sqrt(tr * tr + ti * ti));
            if (prevTot >= 0.0f) {
                const float j = std::fabs(tot - prevTot);
                if (j > maxJump) { maxJump = j; at = y; }
            }
            if (y > 2.9f && y < 3.11f)
                std::printf("        %.2f   %.1f   (%+.4f,%+.4f)   %.4f\n",
                            y, direct, re[bi], im[bi], tot);
            prevTot = tot;
        }
        char note[112];
        std::snprintf(note, sizeof(note), "(1kHz 最大隣接差 %.4f @ y=%.2f)", maxJump, at);
        check("総和が飛ばない（0.02m 刻みで隣接差 < 0.15）", maxJump < 0.15f, note);
    }

    // ------------------------------------------------------------ ③ 回折の大きさ
    std::printf("\n[③] 影境界のすぐ両側で回折が直接音の半分あたりか\n");
    {
        const btm::Edge e = thinScreenEdge();
        const Vec3 S(0.0f, -2.0f, -4.0f);
        float gIn[kBands] = {}, gOut[kBands] = {};
        btm::edgeBands(e, S, Vec3(0.0f, 2.9f, 6.0f), kBandHz, kBands, kC, gIn, nullptr);
        btm::edgeBands(e, S, Vec3(0.0f, 3.1f, 6.0f), kBandHz, kBands, kC, gOut, nullptr);
        char note[112];
        std::snprintf(note, sizeof(note), "(影側 %.4f / 明側 %.4f, 1kHz。目安 0.5)",
                      gIn[3], gOut[3]);
        check("境界のすぐ両側で 0.5 前後（0.25〜0.8）",
              gIn[3] > 0.25f && gIn[3] < 0.8f && gOut[3] > 0.25f && gOut[3] < 0.8f, note);
    }

    // ------------------------------------------------------------ ④ 深い影の傾向
    std::printf("\n[④] 影が深いほど小さく、低域ほど大きい（前川と同じ向き）\n");
    {
        const btm::Edge e = thinScreenEdge();
        const Vec3 S(0.0f, -2.0f, -4.0f);
        float gShallow[kBands] = {}, gDeep[kBands] = {};
        btm::edgeBands(e, S, Vec3(0.0f, -0.3f, 6.0f), kBandHz, kBands, kC, gShallow, nullptr);
        btm::edgeBands(e, S, Vec3(0.0f, -3.0f, 6.0f), kBandHz, kBands, kC, gDeep, nullptr);
        char n1[96];
        std::snprintf(n1, sizeof(n1), "(浅い %.4f → 深い %.4f, 1kHz)", gShallow[3], gDeep[3]);
        check("影が深いほど小さい", gDeep[3] < gShallow[3], n1);
        char n2[96];
        std::snprintf(n2, sizeof(n2), "(125Hz %.4f / 4kHz %.4f)", gDeep[0], gDeep[5]);
        check("深い影では低域ほど大きい", gDeep[0] > gDeep[5], n2);
    }

    // -------------------------------------------------------------- ⑤ 稜線の有限性
    // ここが前川との本質的な違い。前川は半無限を仮定するので稜線の長さを知らない。
    std::printf("\n[⑤] 稜線が短いほど寄与が小さい（半無限仮定との違い）\n");
    {
        const Vec3 S(0.0f, -2.0f, -4.0f);
        const Vec3 R(0.0f, -1.0f,  6.0f);
        float gLong[kBands] = {}, gShort[kBands] = {};
        btm::edgeBands(thinScreenEdge(50.0f), S, R, kBandHz, kBands, kC, gLong, nullptr);
        btm::edgeBands(thinScreenEdge(0.15f), S, R, kBandHz, kBands, kC, gShort, nullptr);
        char note[112];
        std::snprintf(note, sizeof(note), "(長い稜線 %.5f → 短い稜線 %.5f, 1kHz)",
                      gLong[3], gShort[3]);
        check("短い稜線のほうが小さい", gShort[3] < gLong[3], note);
    }

    std::printf("\n----\n");
    if (g_fail == 0) std::printf("[OK] %d 件のチェックすべてに合格しました。\n", g_pass);
    else std::printf("[FAIL] %d / %d 件のチェックに失敗しました。\n", g_fail, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
