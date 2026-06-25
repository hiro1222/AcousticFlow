/* Core/acoustic_world.h
 * 音響計算の対象となる「世界」を保持する Core 層のクラス。
 *
 * 現段階では障害物(AABB)の集合を持ち、2点間の可視性(遮蔽の有無)を判定する。
 * 今後ここにレイキャスト・反射・部屋(Room)などが加わっていく。
 *
 * 設計: このクラスは Wwise にも C API にも依存しない純粋な計算層。
 *       std::vector など STL を内部で自由に使ってよい（境界を越えないため）。
 */
#ifndef ACOUSTICFLOW_CORE_ACOUSTIC_WORLD_H
#define ACOUSTICFLOW_CORE_ACOUSTIC_WORLD_H

#include <vector>

#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/ray.h"
#include "Core/vec3.h"

namespace acoustic {

class AcousticWorld {
public:
    // 障害物ボックスを中心+halfExtentsで追加する（既定マテリアル）。
    void addBox(const Vec3& center, const Vec3& halfExtents);

    // マテリアルを指定して障害物ボックスを追加する。
    void addBox(const Vec3& center, const Vec3& halfExtents,
                const AcousticMaterial& material);

    // 登録済みの障害物をすべて消す（シーン再構築用）。
    void clearGeometry();

    // 2点 from->to の間が障害物で遮られているか。
    //   true  = 遮蔽あり（直線で見通せない）
    //   false = 見通せる
    bool isOccluded(const Vec3& from, const Vec3& to) const;

    // origin から dir 方向へレイを飛ばし、最も近い障害物との交差を返す。
    // dir は内部で正規化する。maxDist までに何も無ければ hit=false。
    // レイキャスト本体・反射計算の土台となる中核処理。
    RayHit raycastClosest(const Vec3& origin, const Vec3& dir, float maxDist) const;

    // 反射経路をトレースする。origin から dir 方向にレイを飛ばし、壁に当たる
    // たびに鏡面反射させながら最大 maxBounces 回まで追う。
    // 通過点（始点・各反射点・終端）を outPoints に書き込み、その個数を返す。
    //   outPoints[0]      = origin
    //   outPoints[1..]    = 反射点（壁にぶつかった点）
    //   最後の点          = 終端（最後の反射点 or 開放空間での到達点）
    // maxPoints は outPoints の容量。最低でも maxBounces+2 を渡すこと。
    int traceReflectionPath(const Vec3& origin, const Vec3& dir, float maxDist,
                            int maxBounces, Vec3* outPoints, int maxPoints) const;

    // 直接線分 from->to が通る障害物の透過率を、周波数帯ごとに掛け合わせる。
    // outGain[band] に「各帯域でどれだけ生き残ったか(0..1)」を書き込む。
    //   壁が無ければ全帯域 1.0、壁を通るほど（特に高域が）小さくなる。
    // これが「材質ベースのこもり」の素。
    void computeTransmission(const Vec3& from, const Vec3& to,
                             float outGain[kNumBands]) const;

    // 透過の広帯域平均から求めた遮蔽スカラー(0..1, 1=ほぼ遮断)。
    // 既存の Wwise Occlusion ノブ（1値）に渡す用の簡易指標。
    // ※ 直線1本のみ＝壁が直接を塞ぐと 1 付近に張り付く（回り込みを無視）。
    float occlusionScalar(const Vec3& from, const Vec3& to) const;

    // レイ積分による遮蔽スカラー(0..1)。occlusionScalar の上位版。
    // リスナー起点で numRays 本のレイを飛ばし、各バウンス点から音源へ「つなぐ」
    // （next-event 推定）ことで、壁で反射して回り込む成分も数える。
    //   → 直接が完全に塞がれても、反射経路があれば 1.0 に張り付かず緩む。
    // 直接経路（透過）＋反射経路（吸収で減衰）を帯域別に足し、広帯域平均から occ 化。
    // numRays/maxBounces で精度とコストを調整（音響更新レートで毎回呼ぶ想定）。
    // ※ エネルギーの正規化・反射の重みは暫定 placeholder（聴感で要調整）。
    float occlusionScalarRayIntegrated(const Vec3& source, const Vec3& listener,
                                       int numRays, int maxBounces) const;

    // 複数音源版（リスナーベースの共有パス）。リスナー起点のレイを numRays 本だけ
    // 撒き（raycast＝反射トレースは音源数に依存しない＝1回ぶん）、各バウンス点から
    // 全音源へ next-event でつなぐ。outOcc[j] に音源 j の遮蔽スカラ(0..1)を書く。
    //   sources : count 個の音源位置 / outOcc : count 個以上確保
    // 多音源でも raycast コストは増えない（next-event のみ音源数ぶん）。実ゲームの
    // 大量音源向けの土台。単音源版と同じエネルギー積分ロジックを共有する。
    void occlusionScalarMultiSource(const Vec3& listener,
                                    const Vec3* sources, int count,
                                    float* outOcc, int numRays, int maxBounces) const;

    // リスナーに届くエネルギーを「到達時間」でビン分けしたエコグラム（残響可視化用）。
    // listener 起点のレイを numRays 本撒き（共有）、各バウンス点から sources[] へ
    // next-event でつなぎ、各寄与を 到達時間=経路長/speedOfSound に応じて outBins へ
    // 積算する（広帯域＝帯域平均）。直接音（直線）も該当ビンに加える。
    //   outBins[k] : 時間 [k*binSeconds, (k+1)*binSeconds) に届く合計エネルギー
    //   binSeconds×numBins が窓幅。窓外の寄与は捨てる。
    // 直接音の大ピーク → 初期反射 → 指数減衰の尾、という残響の形が出る土台。
    void computeEchogram(const Vec3& listener, const Vec3* sources, int count,
                         int numRays, int maxBounces,
                         float* outBins, int numBins,
                         float binSeconds, float speedOfSound) const;

    // 音源面（中心・法線・面内 right 軸・半幅/半高・形状）を cols×rows の
    // グリッドに切り、各セル上の点 → リスナーへの「直接到達量」(0..1) を
    // outGrid に row-major(r*cols+c) で書き込む。1=完全に届く / 0=壁で遮断。
    //   shape: 0=点 / 1=円(単位円の外は -1=面外) / 2=矩形
    //   right は面内に射影・正規化し、up は normal×right で内部生成する。
    // 面音源の「どこが届くか」可視化用。書き込んだセル数(cols*rows)を返す。
    int computeFaceReachability(const Vec3& center, const Vec3& normal,
                                const Vec3& rightAxis, float halfW, float halfH,
                                int shape, const Vec3& listener,
                                float* outGrid, int cols, int rows) const;

    // 登録済み障害物の数（デバッグ用）。
    int boxCount() const { return static_cast<int>(occluders_.size()); }

private:
    // 障害物 = 形状(AABB) + 材質。
    struct Occluder {
        Aabb box;
        AcousticMaterial material;
    };
    std::vector<Occluder> occluders_;
};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_ACOUSTIC_WORLD_H
