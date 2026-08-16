// btm.h — 有限の楔による回折を、稜線に沿った積分で解く。
//         Biot–Tolstoy–Medwin / Svensson, Fred & Vanderkooy (1999) の二次音源モデル。
//
// なぜこれを入れるか（前川＋開口積分で行き詰まった経緯）:
//   前川の式は**半無限スクリーンの実験式**で、エネルギー保存から導かれていない。
//   δ（迂回の余剰長）しか見ないので、1.8cm の隙間でも 1.8m の開口でも同じ値を返す。
//   そこで「開口がどれだけ開いているか」を別の項として持とうとしたが、
//   面の上で測る方式は「面のうちどこまでを見るか」という問いから逃れられなかった:
//     ・直線と平面の交点に窓を置く → 音源が正面に無いと窓が壁の中に沈む
//     ・稜線由来の開口点に置く     → 点が跳ぶ（実測 3m）
//     ・開いている領域の重心に置く → 部屋の外まで「開いている」と数える（実測 閉扉で 0.517）
//   δ 重みで範囲を絞ると、その δ が前川の減衰と二重に掛かる（実測 1kHz で 4倍の差）。
//
//   BTM は**面ではなく稜線**の上を積分するので、この問いが発生しない。
//   戸口は稜線 4 本の和として扱えばよく、開口という概念を別に持たなくて済む。
//   扉も「稜線を持つただの物体」なので、特別視が入り込む余地がない。
//
// 何を返すか:
//   インパルス応答を組まずに、**6 帯域の複素応答を直接評価**する。
//     H(ω) = ∫ dz · (−ν/4π) · β(z)/(m·l) · exp(−iω(m+l)/c)
//   帯域ごとに 1 つの周波数で評価すればゲインが出るので、今のタップ模型
//   （遅延＋6帯域ゲイン）にそのまま載る。畳み込み経路を変えずに済む。
//
// 記号:
//   稜線を z 軸とする円筒座標で、音源 (r_S, θ_S, z_S) / 受音点 (r_R, θ_R, z_R)。
//   θ は楔の一方の面から、**空気側を通って**測る。楔の開き角 θ_w に対し ν = π/θ_w。
//     薄い衝立   θ_w = 2π    → ν = 0.5
//     箱の凸稜線 θ_w = 3π/2  → ν = 2/3
//
// ★式は記憶から書いている。信じずに、成り立つはずの性質で検証すること:
//   ・相反性         音源と受音点を入れ替えても同じ
//   ・影境界で半分   薄い衝立の影境界では回折場が直接音のちょうど 1/2（−6dB）
//   ・深い影で前川   十分大きい衝立の深い影では前川の値に近づく
//   これらは btm_regression 側で確認している。
#pragma once

#include <algorithm>
#include <cmath>

#include "vec3.h"

namespace acoustic {
namespace btm {

constexpr double kPi = 3.14159265358979323846;

// 稜線 1 本ぶんの幾何。呼び出し側が組み立てて渡す。
struct Edge {
    Vec3 a{0, 0, 0};          // 稜線の端点（有限であることが BTM の要点）
    Vec3 b{0, 0, 0};
    // ★法線ではなく「**稜線から面に沿って外へ伸びる方向**」。
    //   ここを法線にすると薄い衝立で開き角が π と出て、応答が全部 0 になる（実測で踏んだ）。
    //   規約: ez=normalize(b−a) まわりに faceDir0 から**反時計回り**に faceDir1 まで回ると
    //         **物体の中**を通る。したがって空気側の角は 2π − その角。
    //   薄い衝立は両方が同じ方向（板が伸びる向き）＝ 角 0 → 空気側 2π。
    //   箱の凸稜線は互いに 90° ＝ 空気側 270°。
    Vec3 faceDir0{1, 0, 0};
    Vec3 faceDir1{0, 1, 0};
    // 開き角が既に分かっているならここに入れる（>0 なら faceDir1 より優先）。
    //   エッジカタログは UTD のウェッジ指数 n を持っているので、θ_w = n·π で直接渡せる。
    double openAngleRad = 0.0;
};

// 稜線に垂直な平面へ落として正規化する（退化時は適当な垂直軸）。
inline Vec3 flattenPerp(const Vec3& d, const Vec3& ez) {
    Vec3 f = d - ez * dot(d, ez);
    if (length(f) < 1e-9f) {
        Vec3 t = cross(ez, Vec3(0, 1, 0));
        if (length(t) < 1e-3f) t = cross(ez, Vec3(1, 0, 0));
        return normalized(t);
    }
    return normalized(f);
}

// 楔の開き角（空気側、ラジアン）。
inline double openAngle(const Edge& e) {
    if (e.openAngleRad > 1e-3) return e.openAngleRad;
    const Vec3 ez = normalized(e.b - e.a);
    const Vec3 f0 = flattenPerp(e.faceDir0, ez);
    const Vec3 f1 = flattenPerp(e.faceDir1, ez);
    const Vec3 g = cross(ez, f0);
    double solid = std::atan2(static_cast<double>(dot(f1, g)),
                              static_cast<double>(dot(f1, f0)));
    if (solid < 0.0) solid += 2.0 * kPi;
    const double open = 2.0 * kPi - solid;
    return (open > 1e-3) ? open : 2.0 * kPi;
}

// 稜線まわりの円筒座標へ落とす。
struct Cyl { double r, theta, z; };

inline Cyl toCylinder(const Edge& e, const Vec3& p) {
    const Vec3 ez = normalized(e.b - e.a);
    const Vec3 f0 = flattenPerp(e.faceDir0, ez);
    const Vec3 g = cross(ez, f0);

    const Vec3 d = p - e.a;
    Cyl c;
    c.z = dot(d, ez);
    const Vec3 perp = d - ez * static_cast<float>(c.z);
    c.r = length(perp);
    // ★θ は f0 から**空気側を通って**測る。物体側は反時計回りなので、逆回りで測る。
    //   これで θ(f0)=0、θ(f1)=θ_w となり、音源も受音点も 0..θ_w に収まる。
    double t = std::atan2(static_cast<double>(dot(perp, g)),
                          static_cast<double>(dot(perp, f0)));
    if (t < 0.0) t += 2.0 * kPi;
    double air = 2.0 * kPi - t;
    if (air >= 2.0 * kPi) air -= 2.0 * kPi;
    c.theta = air;
    return c;
}

// 稜線 1 本の回折応答。帯域ごとに |H| を返す（直接音の自由場 1/d に対する比）。
//
//   outGain[b] : 帯域 b のゲイン(0..)。直接音が同じ距離を素通りしたときとの比。
//   outDelay   : 代表遅延(秒)。稜線上で最も近い点（頂点）を通る経路長 ÷ c。
//
//   nz は稜線の分割数。頂点付近に寄せて取る（そこに可積分な特異点があるため）。
inline bool edgeBands(const Edge& e, const Vec3& src, const Vec3& rcv,
                      const double* bandHz, int nBands,
                      double c, float* outGain, float* outDelay, int nz = 96,
                      double* outRe = nullptr, double* outIm = nullptr,
                      bool* outOnBoundary = nullptr) {
    // ★戻り値は「計算できたか」だけ。境界上かどうかは outOnBoundary で返す。
    //   ここを兼用していたせいで、呼び出し側が失敗を境界と取り違えてゲイン 0 を採用し、
    //   扉の掃引が 0.0147 ⇄ 0.0413 と乱高下した。
    if (outOnBoundary) *outOnBoundary = false;
    const double len = static_cast<double>(length(e.b - e.a));
    if (len < 1e-6 || nBands <= 0 || !bandHz || !outGain) return false;

    const double thetaW = openAngle(e);
    if (thetaW < 1e-3) return false;
    const double nu = kPi / thetaW;

    const Cyl cs = toCylinder(e, src);
    const Cyl cr = toCylinder(e, rcv);
    if (cs.r < 1e-6 || cr.r < 1e-6) return false;   // 稜線上に乗っている

    const double directDist = std::max(static_cast<double>(length(rcv - src)), 1e-6);

    // 4 つの角の組み合わせ。影境界ではこのうち 1 つが cos(νφ)=1 に落ちて特異になる。
    const double phi[4] = {
        kPi + cs.theta + cr.theta,
        kPi + cs.theta - cr.theta,
        kPi - cs.theta + cr.theta,
        kPi - cs.theta - cr.theta,
    };
    // どの項が境界で特異になるか。νφ が 0 または 2π に落ちる項。
    //   ここが 1 つでも立っていたら、**呼び出し側は幾何側（直接音／鏡面反射）を
    //   半分にする必要がある**。これが境界での連続性を作る（EDtoolbox と同じ規約）。
    bool singular[4] = {false, false, false, false};
    bool anySingular = false;
    for (int k = 0; k < 4; ++k) {
        const double a = std::fabs(nu * phi[k]);
        if (a < 1e-9 || std::fabs(a - 2.0 * kPi) < 1e-9) { singular[k] = true; anySingular = true; }
    }

    // 頂点（経路長 m+l が最小になる z）。ここへ標本を寄せる。
    //   稜線に沿って音源と受音点の垂線の足を r で内分した位置が良い近似。
    //   ★稜線は有限なので、頂点が線分の外に出ることがある（扉自身の稜線など）。
    //     そのまま窓を置くと全標本が範囲外で捨てられ、**ゲインが 0 になる**。
    //     実測で扉の掃引が 0.0147 ⇄ 0.0413 と乱高下したのがこれ。線分内へ丸める。
    double zApex = (cs.z * cr.r + cr.z * cs.r) / std::max(cs.r + cr.r, 1e-9);
    zApex = std::min(std::max(zApex, 0.0), len);

    // 頂点を中心に、tanh で寄せた非一様標本を作る。刻みではなく**変数変換**なので、
    // 幾何が動いても標本の並びが連続に動く（段差を作らない）。
    const double halfSpan = 3.0;   // 変換後の積分範囲（±3 でほぼ全体を覆う）
    double acc_re[8] = {0}, acc_im[8] = {0};
    const int nb = std::min(nBands, 8);

    // ── 頂点近傍の「細い山」を捉えるための下ごしらえ ──
    //   境界の近くでは φ が小さくなり、頂点近傍で η ≒ a·|z−z_頂点| なので
    //     sin(νφ)/(cosh(νη)−cos(νφ)) ≒ (2/ν)·φ / (a²(z−z_頂点)² + φ²)
    //   となる。**幅 φ/a・高さ 1/φ の細い山**で、積分値は有限（π/a、φ に依らない）。
    //   発散しているのではなく、山が細すぎて刻みが捉えられていないだけ。
    //   φ が小さいほど山が細るので、扉が閉じているほど壊れる（実測で向きが反転した）。
    //   → 山の幅に合わせて内側を別に刻む。区間の端は解析的なので連続性は壊れない。
    auto etaAt = [&](double z) {
        const double dzs = z - cs.z, dzr = z - cr.z;
        const double m = std::sqrt(cs.r * cs.r + dzs * dzs);
        const double l = std::sqrt(cr.r * cr.r + dzr * dzr);
        double ch = (m * l + dzs * dzr) / (cs.r * cr.r);
        if (ch < 1.0) ch = 1.0;
        return std::acosh(ch);
    };
    double slopeA = 0.0;
    {
        const double h = 1e-3 * std::max(cs.r + cr.r, 1.0);
        slopeA = etaAt(zApex + h) / h;          // dη/dz（頂点では折れるので片側で見る）
    }
    double narrow = 1e30;                        // 最も細い山の幅（z 単位）
    for (int k = 0; k < 4; ++k) {
        if (singular[k]) continue;
        const double a = std::fabs(nu * phi[k]);
        const double w = (slopeA > 1e-12) ? (a / nu) / slopeA : 1e30;
        if (w > 1e-12 && w < narrow) narrow = w;
    }

    double bestPath = 1e30;
    int used = 0;
    const double broadScale = std::max(0.25 * (cs.r + cr.r), 1e-3);
    // 内側の窓。山の 8 倍幅を確保する。広い側より広くはしない。
    const double innerHalf = std::min(8.0 * narrow, broadScale);
    const bool needInner = (narrow < 1e29) && (innerHalf < 0.25 * broadScale);
    const int nOuter = needInner ? (nz / 2) : nz;
    const int nInner = needInner ? (nz - nOuter) : 0;

    // 内側（頂点まわりの細い山）を一様に刻む。端は厳密。
    for (int i = 0; i < nInner; ++i) {
        const double z = zApex - innerHalf + 2.0 * innerHalf * (i + 0.5) / nInner;
        const double dz = 2.0 * innerHalf / nInner;
        if (z < 0.0 || z > len) continue;
        const double dzs = z - cs.z, dzr = z - cr.z;
        const double m = std::sqrt(cs.r * cs.r + dzs * dzs);
        const double l = std::sqrt(cr.r * cr.r + dzr * dzr);
        if (m < 1e-9 || l < 1e-9) continue;
        const double path = m + l;
        if (path < bestPath) bestPath = path;
        double coshEta = (m * l + dzs * dzr) / (cs.r * cr.r);
        if (coshEta < 1.0) coshEta = 1.0;
        const double coshNuEta = std::cosh(nu * std::acosh(coshEta));
        double beta = 0.0;
        for (int k = 0; k < 4; ++k) {
            if (singular[k]) continue;
            const double np = nu * phi[k];
            const double den = coshNuEta - std::cos(np);
            if (std::fabs(den) < 1e-12) continue;
            beta += std::sin(np) / den;
        }
        ++used;
        const double amp = -(nu / (4.0 * kPi)) * beta / (m * l) * dz;
        for (int b = 0; b < nb; ++b) {
            const double ph = -2.0 * kPi * bandHz[b] * (path - directDist) / c;
            acc_re[b] += amp * std::cos(ph);
            acc_im[b] += amp * std::sin(ph);
        }
    }

    for (int i = 0; i < nOuter; ++i) {
        const double s = -halfSpan + 2.0 * halfSpan * (i + 0.5) / nOuter;
        const double th = std::tanh(s);
        // z へ写す。頂点まわりのスケールは音源・受音点の距離で決める。
        //   ★範囲と分解能はトレードオフ。標本数を据え置いて 4.0*(r_S+r_R) まで広げたら、
        //     頂点付近が粗くなって深い影が 0.078 → 0.334 に膨らみ、前川から外れた
        //     （周波数の向きも反転し、短い稜線が 0 になった）。
        //     0.25 のままなら深い影 1kHz で 0.078、前川の同条件 0.089 と 1.1倍で一致する。
        //     境界近傍だけは寄与が稜線に沿って遠くまで広がるので、この範囲では足りない
        //     ── そこは Svensson & Calamia (2006) の近似が要る領域。
        const double scale = broadScale;
        const double z = zApex + scale * th;
        // dz/ds = scale * sech²(s)
        const double sech2 = 1.0 - th * th;
        const double dz = scale * sech2 * (2.0 * halfSpan / nOuter);
        if (dz < 1e-12) continue;

        // 稜線は有限。範囲外は寄与しない（ここが半無限のスクリーンとの違い）。
        if (z < 0.0 || z > len) continue;
        // 内側で既に積んだ区間は飛ばす（二重に数えない）。
        if (needInner && std::fabs(z - zApex) < innerHalf) continue;

        const double dzs = z - cs.z, dzr = z - cr.z;
        const double m = std::sqrt(cs.r * cs.r + dzs * dzs);
        const double l = std::sqrt(cr.r * cr.r + dzr * dzr);
        if (m < 1e-9 || l < 1e-9) continue;
        const double path = m + l;
        if (path < bestPath) bestPath = path;

        // cosh(η) = (m·l + (z−z_S)(z−z_R)) / (r_S·r_R)
        double coshEta = (m * l + dzs * dzr) / (cs.r * cr.r);
        if (coshEta < 1.0) coshEta = 1.0;
        const double eta = std::acosh(coshEta);
        const double coshNuEta = std::cosh(nu * eta);

        double beta = 0.0;
        for (int k = 0; k < 4; ++k) {
            const double np = nu * phi[k];
            // ★特異項は**除外する**（クランプではなく）。
            //   影境界では φ のどれかが 0（または 2π）になり、頂点で η も 0 になるので
            //   sin(νφ)/(cosh(νη)−cos(νφ)) が 0/0 になる。
            //   参照実装 EDtoolbox (Svensson, BSD) も同じく除外していて、
            //   そのうえで**呼び出し側に「直接音または鏡面反射を半分にせよ」と通知**する。
            //   つまり境界での −6dB は回折積分からではなく、**幾何側の半減**から出る。
            //   ここを「消えた項が半分を作っているのだ」と誤読して丸一周した。
            if (singular[k]) continue;
            const double den = coshNuEta - std::cos(np);
            if (std::fabs(den) < 1e-12) continue;
            beta += std::sin(np) / den;
        }

        ++used;
        const double amp = -(nu / (4.0 * kPi)) * beta / (m * l) * dz;
        for (int b = 0; b < nb; ++b) {
            const double w = 2.0 * kPi * bandHz[b];
            const double ph = -w * (path - directDist) / c;
            acc_re[b] += amp * std::cos(ph);
            acc_im[b] += amp * std::sin(ph);
        }
    }

    if (used == 0) return false;   // 稜線に1点も乗らなかった＝計算できていない
    // ★自由場の規約。BTM の積分核は p_free = e^{-ikr}/r 側の規約で書かれているので、
    //   直接音との比は |H| · d（4π を掛けてはいけない）。
    //   4π を掛けていたときは深い影で 1kHz が 0.98 になり、前川の同条件 0.089 に対して
    //   約 11 倍＝ちょうど 4π ぶんずれていた。ここで規約違いに気づいた。
    const double norm = directDist;
    for (int b = 0; b < nb; ++b) {
        const double re = acc_re[b] * norm, im = acc_im[b] * norm;
        outGain[b] = static_cast<float>(std::sqrt(re * re + im * im));
        if (outRe) outRe[b] = re;
        if (outIm) outIm[b] = im;
    }
    for (int b = nb; b < nBands; ++b) outGain[b] = outGain[nb - 1];
    if (outDelay) *outDelay = static_cast<float>((bestPath < 1e29 ? bestPath : directDist) / c);
    // 境界上なら呼び出し側は幾何側（直接音／鏡面反射）を半分にする（EDtoolbox の規約）。
    if (outOnBoundary) *outOnBoundary = anySingular;
    return true;
}

// 境界上かどうかだけを知りたいとき（幾何側を半分にすべきか）。
inline bool onZoneBoundary(const Edge& e, const Vec3& src, const Vec3& rcv) {
    const double thetaW = openAngle(e);
    if (thetaW < 1e-3) return false;
    const double nu = kPi / thetaW;
    const Cyl cs = toCylinder(e, src), cr = toCylinder(e, rcv);
    const double phi[4] = {kPi + cs.theta + cr.theta, kPi + cs.theta - cr.theta,
                           kPi - cs.theta + cr.theta, kPi - cs.theta - cr.theta};
    for (int k = 0; k < 4; ++k) {
        const double a = std::fabs(nu * phi[k]);
        if (a < 1e-9 || std::fabs(a - 2.0 * kPi) < 1e-9) return true;
    }
    return false;
}

}  // namespace btm
}  // namespace acoustic
