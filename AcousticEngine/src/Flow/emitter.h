/* Flow/emitter.h ── 音源とリスナー（段 1）
 *
 * ■ 役割
 *   「誰がどこで鳴っていて、誰がどこで聞いているか」の状態。それだけ。
 *   音源は幅を持つ（設計文書 Ⅵ「音源（幅を持つ／遠距離・低優先度で点にクロスフェード）」）。
 *   前フレームからの動きを覚えて「変化量」を出す（Ⅶ 優先度の 4 番目。Ⅹ の空欄「変化量の検出方法」）。
 *
 * ■ 中の仕組み
 *   1) Listener: 位置と向き（forward / up）。到来方向をリスナー座標に直す toLocal を持つ。
 *      右 = cross(up, forward)。Unity と同じ左手系で +x 右・+y 上・+z 前（旧 DirectionBus と同じ規約）。
 *   2) Emitter: 位置、幅 radius（m、0 で点）、操作対象か（operated）、聞こえの大きさ loudness（ホストが渡す 0..1）。
 *      effectiveRadius(dist, detail): 幅は「見込み角」で点に落とす。
 *          θ = atan(radius / dist)。θ ≥ 2° で幅のまま、θ ≤ 0.5° で点。間は線形。
 *      0.5° は水平の定位の弁別閾（JND）の下限あたり、2° はそれより十分大きい。見込み角がこれより
 *      小さい幅は耳に区別できないので、幅の計算（開口の走査線積分）を払う理由が無い。
 *      detail（0..1）は予算（budget.h）が渡す「この音源にどれだけ手を掛けるか」。低優先度なら
 *      予算が detail を下げ、幅が点に寄る。★幅を落とすのは計算量のためで、音の意味はない。
 *   3) 変化量 change: beginFrame(dt) で「自分の移動速度（m/s）」を入れる。動いた物が近くを通ったぶんは
 *      世界側が addChange で足す（段 8）。budget が優先度に使い、フレームの終わりに消える。
 *
 * ■ 繋がり
 *   受ける: ホストから位置・向き・幅・操作対象・loudness。世界から room（現在の部屋）と addChange。
 *   渡す:   位置と幅を energy_trace / aperture / image_sources へ。change と operated を budget へ。
 *           toLocal を distribute へ（方向をリスナー座標にする所は 1 つ）。
 *
 * ■ 退けた書き方
 *   ・幅を距離のしきい値で切る（8 m まで幅あり、など）: 大きい音源と小さい音源で意味が変わる。
 *     見込み角なら 0.3 m の箱も 3 m の滝も同じ規則で点になる。
 *   ・変化量を位置差の絶対値にする: フレームレートで値が変わる。速度（m/s）にして時間で正規化。
 *   ・リスナー座標への変換を各段でやる: 符号の規約（右がどっちか）がずれる事故が旧実装で 1 回あった。
 *     ここ 1 か所。
 *
 * ■ 壊れる所
 *   ・forward と up が直交していないと right が縮む。normalize してから cross を取ること。
 *   ・beginFrame を呼び忘れると prevPos が更新されず、静止しているのに change が出続ける
 *     （予算が「動いている」と誤解して優先度を上げ続ける）。検査「静止で 0」がこれを捕まえる。
 *   ・θ の境（0.5°/2°）を 1 点にすると、境をまたぐたびに幅が 0 と radius を行き来して、
 *     開口の積分結果が跳ぶ。線形の帯にしてあるのはそのため。
 */
#ifndef ACOUSTICFLOW_FLOW_EMITTER_H
#define ACOUSTICFLOW_FLOW_EMITTER_H

#include <algorithm>
#include <cmath>
#include "Core/vec3.h"

namespace acoustic {
namespace flow {

struct Listener {
    Vec3 pos{0, 0, 0};
    Vec3 forward{0, 0, 1};
    Vec3 up{0, 1, 0};
    Vec3 prevPos{0, 0, 0};
    float speed = 0.0f;               // m/s（今フレーム）

    void beginFrame(float dt) {
        speed = (dt > 1e-6f) ? length(pos - prevPos) / dt : 0.0f;
        prevPos = pos;
    }
    /// 到来方向（world、正規化不要）をリスナー座標へ。+x 右・+y 上・+z 前。
    Vec3 toLocal(const Vec3& dirWorld) const {
        const Vec3 f = normalized(forward);
        const Vec3 u = normalized(up);
        const Vec3 r = normalized(cross(u, f));          // 左手系: 右 = up × forward
        const Vec3 d = normalized(dirWorld);
        return Vec3(dot(d, r), dot(d, u), dot(d, f));
    }
};

struct Emitter {
    int   id = -1;
    bool  active = true;
    Vec3  pos{0, 0, 0};
    float radius = 0.0f;              // 幅（m）。0 = 点
    bool  operated = false;           // プレイヤーの操作対象（Ⅶ 優先度 1 位）
    float loudness = 1.0f;            // 聞こえの大きさ 0..1（ホストが渡す。Ⅶ 優先度 3 位）
    int   room = -1;                  // 現在の部屋（世界が入れる。-1 = 外）

    Vec3  prevPos{0, 0, 0};
    float speed = 0.0f;               // m/s
    float change = 0.0f;              // 変化量（今フレーム）。自分の速度 + 動いた物の接触

    void beginFrame(float dt) {
        speed = (dt > 1e-6f) ? length(pos - prevPos) / dt : 0.0f;
        prevPos = pos;
        change = speed;
    }
    /// 動いた物が近くを通った、扉が動いた、などを世界が足す（m/s 相当の値）。
    void addChange(float v) { change += std::max(0.0f, v); }

    /// 見込み角で点に寄せた実効の幅。detail は予算からの 0..1（1 = 手を掛ける）。
    float effectiveRadius(float dist, float detail = 1.0f) const {
        if (radius <= 0.0f || dist <= 1e-3f) return 0.0f;
        const float theta = std::atan(radius / dist);                       // 見込み角（rad）
        constexpr float kPointDeg = 0.5f, kFullDeg = 2.0f;
        const float lo = kPointDeg * 3.14159265f / 180.0f, hi = kFullDeg * 3.14159265f / 180.0f;
        const float w = std::min(1.0f, std::max(0.0f, (theta - lo) / (hi - lo)));
        return radius * w * std::min(1.0f, std::max(0.0f, detail));
    }
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_EMITTER_H
