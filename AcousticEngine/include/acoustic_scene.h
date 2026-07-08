/* acoustic_scene.h ── 新アーキ(2026-07) Phase 1：Scene ベースの幾何/音響クエリ C API
 *
 * 旧 acoustic_engine.h の AddBox/IsOccluded/ComputeOcclusion 系（スカラ遮蔽・箱コーナー回折）
 * を置き換える、インスタンス方式 Scene のための新しい C 窓口。
 *   - 幾何 = インスタンス（geomId + OBB transform + materialId）
 *   - 出力 = 6帯域（1スカラに潰さない）
 * Wwise 音声のライフサイクル（Init/LoadBank/PostEvent/RTPC 等）は acoustic_engine.h の
 * 既存 ABI を引き続き使う（音声バックエンドは新アーキでも再利用）。
 *
 * ハンドルは Scene 実体への不透明ポインタ。C# 側は IntPtr。
 */
#ifndef ACOUSTIC_SCENE_H
#define ACOUSTIC_SCENE_H

#include "acoustic_engine.h"  /* AF_Vector3 / ACOUSTIC_API を共有 */

#ifdef __cplusplus
extern "C" {
#endif

/* Scene 実体への不透明ハンドル。 */
typedef void* AF_SceneHandle;

/* ===== 生成・破棄 ===== */

/* Scene を生成しハンドルを返す。失敗時 NULL。 */
ACOUSTIC_API AF_SceneHandle AF_SceneCreate(void);

/* Scene を破棄する。 */
ACOUSTIC_API void AF_SceneDestroy(AF_SceneHandle scene);

/* ===== 材質テーブル ===== */

/* 材質をテーブルに追加し materialId を返す。
 *   transmission/absorption/scattering : 各帯域(0..1)配列。null なら既定壁の該当値。
 *   numBands : 配列要素数。内部帯域数(6)未満なら不足分は既定壁で補う。<=0 は全て既定。
 * 各配列は独立に反映（null の配列だけ既定のまま）。 */
ACOUSTIC_API int AF_SceneAddMaterial(AF_SceneHandle scene,
                                     const float* transmission,
                                     const float* absorption,
                                     const float* scattering,
                                     int numBands);

/* ===== インスタンス（占有物） ===== */

/* OBB インスタンスを追加し instanceId を返す。失敗時 -1。
 *   center/halfExtents : ワールドの中心と半サイズ。
 *   right/up           : ローカル軸のワールド向き（内部で正規直交化。Unity の
 *                        transform.right/up をそのまま渡せる）。
 *   materialId         : AF_SceneAddMaterial の戻り値（範囲外は 0）。 */
ACOUSTIC_API int AF_SceneAddInstanceBox(AF_SceneHandle scene,
                                        AF_Vector3 center,
                                        AF_Vector3 halfExtents,
                                        AF_Vector3 right,
                                        AF_Vector3 up,
                                        int materialId);

/* 既存インスタンスの transform を更新する（動いた分だけ）。範囲外 id は無視。 */
ACOUSTIC_API void AF_SceneUpdateInstance(AF_SceneHandle scene,
                                         int instanceId,
                                         AF_Vector3 center,
                                         AF_Vector3 halfExtents,
                                         AF_Vector3 right,
                                         AF_Vector3 up);

/* インスタンスの有効/無効（active: 1=有効 / 0=無効）。BVH/走査から外すだけ＝軽い。 */
ACOUSTIC_API void AF_SceneSetInstanceActive(AF_SceneHandle scene,
                                            int instanceId, int active);

/* 全インスタンスを消す（材質テーブルは保持）。毎フレーム作り直す用途。 */
ACOUSTIC_API void AF_SceneClearInstances(AF_SceneHandle scene);

/* ===== クエリ ===== */

/* from→to の直線が通る壁の帯域別透過ゲイン(0..1)を outGains に書く。
 *   壁なし → 全帯域 1.0 / 壁を通るほど（材質次第で高域が）小さくなる。
 *   帯域: 125,250,500,1k,2k,4k Hz の順（計 6）。outGains は 6 要素以上（count=容量）。
 * 実際に書き込んだ帯域数を返す。役割1「材質ベースのこもり」の 6帯域出力。 */
ACOUSTIC_API int AF_SceneComputeTransmissionBands(AF_SceneHandle scene,
                                                  AF_Vector3 from,
                                                  AF_Vector3 to,
                                                  float* outGains,
                                                  int count);

/* 2点間が遮られているか（二値）。1=遮蔽 / 0=見通せる。 */
ACOUSTIC_API int AF_SceneIsOccluded(AF_SceneHandle scene,
                                    AF_Vector3 from, AF_Vector3 to);

/* origin から dir 方向の最近傍ヒットまでの距離を返す。無ヒットは -1。
 * dir は内部で正規化。デバッグ/可視化用。 */
ACOUSTIC_API float AF_SceneRaycast(AF_SceneHandle scene,
                                   AF_Vector3 origin, AF_Vector3 dir, float maxDist);

/* 【回折(Phase1.5 暫定)】from→to の帯域別回折ゲイン(0..1)を outGains(6要素以上)に書く。
 *   遮蔽なし → 全帯域 1.0 / 迂回路あり → Maekawa（低域ほど大きい）/ 迂回路なし → 0。
 * 直接が壁で塞がれても、稜線を回り込む成分を周波数別に与える。書き込んだ帯域数を返す。
 *   ※ 本実装 Phase4（エッジカタログ+UTD）で置き換える前味。 */
ACOUSTIC_API int AF_SceneComputeDiffractionBands(AF_SceneHandle scene,
                                                 AF_Vector3 from, AF_Vector3 to,
                                                 float* outGains, int count);

/* 【回折の経路可視化】from→to が遮蔽されているとき、稜線を回る最短迂回の余剰経路長 δ(m)を
 * 返し、その迂回点(source→P→listener の P)を outMidPoint に書く。遮蔽なし/迂回路なしは -1。
 * outMidPoint は null 可。Unity で回折の「経路補完」を線で描くために使う。 */
ACOUSTIC_API float AF_SceneDiffractionPath(AF_SceneHandle scene,
                                           AF_Vector3 from, AF_Vector3 to,
                                           AF_Vector3* outMidPoint);

/* 【役割2：反射込み遮蔽】リスナー起点で numRays 本のレイを撒き、壁で反射させながら音源へ
 * next-event でつなぐ。直接(透過⊕回折)＋反射で回り込む成分から遮蔽量(0..1)を返す。
 * 反射経路があるので壁裏でも 1.0 に張り付かない（＝実際の部屋の「回り込み」）。
 *   outBands6 : null でなければ 直接⊕回折⊕反射 の帯域別生存(0..1) を書く（6要素）。
 *   numRays   : リスナーレイ本数（例 256〜1024）。maxBounces : 反射回数（例 2〜3）。 */
ACOUSTIC_API float AF_SceneOcclusionReflected(AF_SceneHandle scene,
                                              AF_Vector3 source, AF_Vector3 listener,
                                              float* outBands6, int numRays, int maxBounces);

/* 【役割2・複数音源（リスナーベース共有）】リスナーレイを numRays 本だけ撒き（raycast は
 * 音源数に依存しない）、各バウンス点から全音源へ next-event でつなぐ。
 *   sources  : 音源位置（count 個） / outOcc : 音源ごとの遮蔽量(0..1)（count 個以上, null 可）
 *   outBands : null でなければ j*6+b に 直接⊕回折⊕反射 の帯域別生存(count*6)を書く
 * 大量音源でも raycast コストが増えない。 */
ACOUSTIC_API void AF_SceneOcclusionReflectedMulti(AF_SceneHandle scene,
                                                  AF_Vector3 listener,
                                                  const AF_Vector3* sources, int count,
                                                  float* outOcc, float* outBands,
                                                  int numRays, int maxBounces);

/* 【残響(Phase6)：エコグラム】リスナーに届くエネルギーを到達時間ビンに積む。
 * 直接音＋反射（共有レイ・多バウンス）。outBins[k]=時間[k*binSeconds,(k+1)*binSeconds)の合計。
 *   sources : 音源位置(count個) / outBins : numBins 個以上確保 / speedOfSound : 音速(m/s, 例343)
 * 直接音の大ピーク→初期反射→指数減衰の尾。RT60/wet 算出（＝Wwise残響駆動）の土台。 */
ACOUSTIC_API void AF_SceneComputeEchogram(AF_SceneHandle scene,
                                          AF_Vector3 listener,
                                          const AF_Vector3* sources, int count,
                                          float* outBins, int numBins,
                                          float binSeconds, float speedOfSound,
                                          int numRays, int maxBounces);

/* 登録済みインスタンス数（デバッグ用）。 */
ACOUSTIC_API int AF_SceneInstanceCount(AF_SceneHandle scene);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_SCENE_H */
