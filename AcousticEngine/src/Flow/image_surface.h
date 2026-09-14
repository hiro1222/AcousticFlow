/* Flow/image_surface.h ── 虚像の面音源: 虚像どうしをつないで面にし、向き・幅（ASW）・奥行きの重みを出す（2026-09-14）
 *
 * ■ 役割
 *   World::earlyModel = 4。ISM の虚像（図 9 の鏡像の分布）を点で鳴らさず、耳から見て近い虚像どうしをつないだ「面音源」として鳴らす。
 *   発注者の狙い: ①つなぐので動いても滑らか ②面の幅（ASW）で部屋の広さ感 ③虚像が集まった所・経路が短い所を重くして奥行き感。
 *   量はレイの初期の総量のまま（配分で解く）。虚像ごとの取り分は ISM の幾何の重み（earlyModel 0 と同じ）で、ここはその形を変える。
 *
 * ■ 中の仕組み（虚像ごと、毎フレーム）
 *   1) 虚像そのものの幅 α_i: 音源の大きさの見込み atan(r / d) ＋ 散乱の幅 s_i · roughRad。
 *      s_i = 1 − Π(1 − 経路の面の散乱率)。凹凸のある壁の虚像は 1 点でなく群になる（図 8 の仮想音源群）ので、その広がりを幅で持つ。
 *   2) つなぎの強さ k_ij = exp(−(角度_ij / (connectRad + α_i + α_j))²) · exp(−((t_i − t_j) / connectSec)²)。
 *      耳から見た向きと到達が近いほど強い。しきい値で切らない（虚像が出入りしても可視率で重さが連続に変わる）。
 *   3) 面: 自分と、つながった虚像を量 × つなぎで重みを付けて束ねる。
 *        向き = 量 × 向きの和の向き（面の重心）
 *        幅   = √3 · 角度の標準偏差（虚像どうしの開き ＋ 各虚像の幅 α²/3。一様な弧の半角に揃える）
 *      遅れは各虚像の正確な到達のまま（面は向きと幅を持ち、時間には点で並ぶ）。
 *   4) 奥行きの重み（演出。出口だけ）: 集まり ρ_i = 1 + Σ k_ij · 可視率_j、近さ n_i = 直接の距離 / 虚像の距離（≤ 1）。
 *      G_i = ρ_i^densityPow · n_i^nearPow を、Σ 量_i · G_i = Σ 量_i になるように揃える（初期の総量は変えない）。
 *      densityPow = nearPow = 0 なら G = 1 で、量は earlyModel 0 と 1 ビットも同じ。
 *
 * ■ 繋がり
 *   受ける: その音源の ImageSet（位置・到達・可視率・正規化した重み・面の組）、ISM の面（散乱率）、耳と音源の位置、音源の幅、SurfaceParams。
 *   渡す:   ImageSurface（向き・幅・出口の重み）を distribute の虚像の道へ。幅は MixTap::width → DSP の Tap::width（方向バスのレーンへ幅で配る）。
 *
 * ■ 退けた書き方
 *   ・レイの受け取りから仮想音源群を作る（earlyModel 3 の散乱の点）: 群はできるが、量と形がレイの統計に乗り、表現に直結しない。
 *     発注者「レイでやるよりも面音源化するのがいい。作りたい表現に直結している」。
 *   ・隣り合う虚像を三角形に結んで面を塗る: 虚像が出入りするたびに三角形の組み方が変わる（どこかで二値の判定が要る）。
 *     重み付きの束ねなら組み方を持たずに同じ「つながった面」が出る。
 *   ・広がり（spread）で幅を出す: 今の広がりは量を方向の無い拡散の道へ回すだけで、ある向きの幅にならない。幅は別に持つ。
 *
 * ■ 壊れる所
 *   ・connectRad を大きくすると、遠く離れた虚像まで 1 枚の面になり、向きが全部部屋の重心へ寄って方向が消える。
 *   ・方向バスのレーンは方位角だけ（上下を畳む）。天井・床の虚像の幅は方位角の幅として鳴る。
 *   ・虚像の数は ImageSet の上限（48 本）まで。3 次まで入れると上限で落ちる虚像が増え、集まりの重みがそこで変わる。
 */
#ifndef ACOUSTICFLOW_FLOW_IMAGE_SURFACE_H
#define ACOUSTICFLOW_FLOW_IMAGE_SURFACE_H

#include <algorithm>
#include <cmath>
#include <vector>
#include "Core/vec3.h"
#include "Flow/image_sources.h"

namespace acoustic {
namespace flow {

/// 面音源の摘み。角度はラジアン。
struct SurfaceParams {
    float roughRad = 20.0f * 3.14159265f / 180.0f;     // 散乱率 1 の面の虚像が持つ幅（半角）
    float connectRad = 15.0f * 3.14159265f / 180.0f;   // つなぐ角度の尺度
    float connectSec = 0.005f;                          // つなぐ到達の尺度
    float densityPow = 0.0f;                            // 集まりの重み（演出。0 で重みなし）
    float nearPow = 0.0f;                               // 近さの重み（演出。0 で重みなし）
};

/// 経路の長さの打ち切りの手前で、つなぎの重さを落とす幅（秒）。2 ms ＝ 0.69 m。
constexpr float kSurfaceFadeSec = 0.002f;

struct ImageSurface {
    Vec3  dir{0, 0, 1};      // 面の向き（world、単位。耳から）
    float width = 0.0f;      // 面の幅（半角、ラジアン）
    float gain = 1.0f;       // 出口の重み（量に掛ける。Σ 量 · gain = Σ 量）
    float density = 1.0f;    // 集まり ρ（情報）
    float toDoor = 0.0f;     // 隣の部屋の閉じ込めで戸口の 1 本へ移す割合（World が書く。0 で移さない）
};

inline float angleBetween(const Vec3& a, const Vec3& b) {
    return std::acos(std::min(1.0f, std::max(-1.0f, dot(a, b))));
}

/// 虚像ごとの面音源を out[0..im.count) に書く。
inline void buildImageSurfaces(const ImageSet& im, const std::vector<Face>& faces, const Vec3& listener, const Vec3& source,
                               float sourceRadius, const SurfaceParams& p, ImageSurface* out, float maxPathSec = 1e9f) {
    const int n = im.count;
    if (n <= 0 || !out) return;
    const float directDist = std::max(1e-3f, length(source - listener));
    Vec3  u[ImageSet::kMaxImages];
    float e[ImageSet::kMaxImages], alpha[ImageSet::kMaxImages], d[ImageSet::kMaxImages], fade[ImageSet::kMaxImages];
    for (int i = 0; i < n; ++i) {
        const ImageSource& s = im.img[i];
        const Vec3 v = s.pos - listener;
        d[i] = std::max(1e-3f, length(v));
        u[i] = v * (1.0f / d[i]);
        double m = 0.0; for (int b = 0; b < kNumBands; ++b) m += s.weight6[b];
        e[i] = static_cast<float>(m / kNumBands);
        double keep = 1.0;
        for (int k = 0; k < s.order && k < ImageSource::kMaxOrder; ++k) {
            const int f = s.face[k];
            if (f >= 0 && f < static_cast<int>(faces.size())) keep *= 1.0 - std::min(1.0f, std::max(0.0f, faces[static_cast<std::size_t>(f)].scatter1k));
        }
        const float sc = static_cast<float>(1.0 - keep);
        // 経路の長さの打ち切り（buildImages の maxPathSec）の手前 kSurfaceFadeSec で、つなぎの相手としての重さを 0 へ落とす。
        //   ★打ち切りで虚像が 1 本消えると、つながっていた面の重心が 1 フレームで跳ぶ（耳を 1 cm 動かして 3.9°）。
        fade[i] = std::min(1.0f, std::max(0.0f, (maxPathSec - s.pathSec) / kSurfaceFadeSec));
        alpha[i] = std::atan(std::max(0.15f, sourceRadius) / d[i]) + sc * p.roughRad;
    }
    double sumE = 0.0, sumEG = 0.0;
    for (int i = 0; i < n; ++i) {
        double N = e[i];
        Vec3 c = u[i] * e[i];
        double rho = 1.0;
        for (int j = 0; j < n; ++j) {
            if (j == i) continue;
            const float ang = angleBetween(u[i], u[j]);
            const float sa = std::max(1e-4f, p.connectRad + alpha[i] + alpha[j]);
            const float dt = im.img[i].pathSec - im.img[j].pathSec;
            const double k = std::exp(-static_cast<double>(ang * ang) / (sa * sa)) *
                             std::exp(-static_cast<double>(dt * dt) / std::max(1e-12, static_cast<double>(p.connectSec) * p.connectSec));
            if (!(k > 1e-6)) continue;
            N += k * e[j] * fade[j];
            c = c + u[j] * static_cast<float>(k * e[j] * fade[j]);
            rho += k * fade[j] * std::min(1.0f, std::max(0.0f, im.img[j].validity));
        }
        const float lc = length(c);
        const Vec3 dir = (lc > 1e-6f) ? c * (1.0f / lc) : u[i];
        // 幅: 面の重心まわりの角度の分散（各虚像の開き ＋ 各虚像自身の幅 α²/3）
        double var = 0.0;
        if (N > 0.0) {
            {
                const float a = angleBetween(u[i], dir);
                var += e[i] * (a * a + alpha[i] * alpha[i] / 3.0f);
            }
            for (int j = 0; j < n; ++j) {
                if (j == i) continue;
                const float ang = angleBetween(u[i], u[j]);
                const float sa = std::max(1e-4f, p.connectRad + alpha[i] + alpha[j]);
                const float dt = im.img[i].pathSec - im.img[j].pathSec;
                const double k = std::exp(-static_cast<double>(ang * ang) / (sa * sa)) *
                                 std::exp(-static_cast<double>(dt * dt) / std::max(1e-12, static_cast<double>(p.connectSec) * p.connectSec));
                if (!(k > 1e-6)) continue;
                const float a = angleBetween(u[j], dir);
                var += k * e[j] * fade[j] * (a * a + alpha[j] * alpha[j] / 3.0f);
            }
            var /= N;
        } else {
            var = alpha[i] * alpha[i] / 3.0f;
        }
        ImageSurface& o = out[i];
        o.toDoor = 0.0f;
        o.dir = dir;
        o.width = std::min(1.5707963f, static_cast<float>(std::sqrt(3.0 * std::max(0.0, var))));
        o.density = static_cast<float>(rho);
        const double nearF = std::min(1.0, static_cast<double>(directDist) / d[i]);
        const double G = ((p.densityPow != 0.0f) ? std::pow(rho, static_cast<double>(p.densityPow)) : 1.0) *
                         ((p.nearPow != 0.0f) ? std::pow(nearF, static_cast<double>(p.nearPow)) : 1.0);
        o.gain = static_cast<float>(G);
        sumE += e[i]; sumEG += e[i] * G;
    }
    // 初期の総量を変えない（重みなしなら scale はちょうど 1）
    if (sumEG > 0.0 && (p.densityPow != 0.0f || p.nearPow != 0.0f)) {
        const float scale = static_cast<float>(sumE / sumEG);
        for (int i = 0; i < n; ++i) out[i].gain *= scale;
    }
}

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_IMAGE_SURFACE_H
