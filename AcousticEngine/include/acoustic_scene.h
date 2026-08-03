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

/* 【回折】from→to の帯域別回折ゲイン(0..1)を outGains(6要素以上)に書く。書き込んだ帯域数を返す。
 * モデルは前川の式。符号付き δ（影で正・境界で 0・照射側で負）だけの関数なので、
 * 稜線が乗り換わってもゲインが飛ばない（δ の min は連続）。高域ほど強く減衰するので、
 * 6帯域ゲインをそのまま適用すれば回折によるローパスになる。 */
ACOUSTIC_API int AF_SceneComputeDiffractionBands(AF_SceneHandle scene,
                                                 AF_Vector3 from, AF_Vector3 to,
                                                 float* outGains, int count);

/* 【回折・UTD版(比較検証用)】上と同じものを Kouyoumjian-Pathak の UTD で計算する。
 * 厳密解だが回折点の 3D 幾何に依存するため、稜線が乗り換わる位置でゲインが不連続に飛ぶ。
 * 音声経路には使わない。回帰テストで前川版と並べ、この選択の根拠を数値で残すためのもの。 */
ACOUSTIC_API int AF_SceneComputeDiffractionBandsUtd(AF_SceneHandle scene,
                                                    AF_Vector3 from, AF_Vector3 to,
                                                    float* outGains, int count);

/* 【回折の経路可視化】from→to が遮蔽されているとき、稜線を回る最短迂回の余剰経路長 δ(m)を
 * 返し、その迂回点(source→P→listener の P)を outMidPoint に書く。遮蔽なし/迂回路なしは -1。
 * outMidPoint は null 可。Unity で回折の「経路補完」を線で描くために使う。 */
ACOUSTIC_API float AF_SceneDiffractionPath(AF_SceneHandle scene,
                                           AF_Vector3 from, AF_Vector3 to,
                                           AF_Vector3* outMidPoint);

/* 【可視化】遮蔽時の回折候補の迂回点 P と余剰経路δを最大 maxCount 個 outPoints/outDeltas に書き、
 * 書いた個数を返す。全候補（回り込み経路）を線で描画し、最短δを色分けするデバッグ用。遮蔽なしは 0。
 *   outPoints : AF_Vector3 × maxCount（各候補の掠める迂回点 P）
 *   outDeltas : float × maxCount（各候補の余剰経路長 δ=迂回長−直線長, m） */
ACOUSTIC_API int AF_SceneDiffractionCandidates(AF_SceneHandle scene,
                                               AF_Vector3 from, AF_Vector3 to,
                                               AF_Vector3* outPoints, float* outDeltas, int maxCount);

/* 【回折を二次音源として鳴らす（GTD/ホイヘンス）】遮蔽時、回り込みエッジを「エッジ＝二次音源」として
 * 最大 maxN 個の方向つき仮想音源に束ねて返す（近い方向はクラスタ統合）。両側開口なら左右に分かれ、
 * リスナー移動で各ゲインが滑らかに変化する。書いた音源数を返す。遮蔽なし/迂回なしは 0。
 *   outPos  : AF_Vector3 × maxN（二次音源のワールド位置 = listener + 方向×音源距離）
 *   outGain : float × maxN（相対ゲイン, 合計で正規化・短い迂回ほど大） */
ACOUSTIC_API int AF_SceneComputeDiffractionSources(AF_SceneHandle scene,
                                                   AF_Vector3 listener, AF_Vector3 source,
                                                   AF_Vector3* outPos, float* outGain, int maxN);

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
/*   outDir : null でなければ j*3 に「エネルギーが届く支配方向(単位ベクトル)」を書く。
 *            遮蔽時は反射/回折が支配し音源真方向でなく“回り込んで届く方向”になる。
 *   directWeight : ステアの直接項の重み係数（大=音源方向に定位が張り付く / 小=反射方向へ開く）。
 *                  直接項は directGain×(直接距離/実効経路)²×directWeight。実効経路=直接距離+回折δ。 */
ACOUSTIC_API void AF_SceneOcclusionReflectedMulti(AF_SceneHandle scene,
                                                  AF_Vector3 listener,
                                                  const AF_Vector3* sources, int count,
                                                  float* outOcc, float* outBands, float* outDir,
                                                  float directWeight, int numRays, int maxBounces);

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

/* 【残響】上の帯域別版。outBins は numBins*6 要素で、outBins[k*6 + b] に
 * 時間ビン k・帯域 b(125/250/500/1k/2k/4kHz) のエネルギーを書く。
 * 実際の部屋は高域ほど速く減衰するため、実測された尾を IR 畳み込みで鳴らすには
 * 広帯域平均ではなくこの帯域別カーブが要る。
 *
 * distanceRef: 音源からの広がり損失の基準距離（0以下で無効＝旧APIと同じ相対の形）。
 *   減衰は atten = distanceRef / max(d, distanceRef)、エネルギーはその2乗。
 *   d は「音源→反射点」の区間長。リスナーまでの総経路長ではないことに注意
 *   （総経路長で掛けると尾に 1/t² の偽の減衰が乗り、広い部屋の中央で反響が痩せる）。 */
ACOUSTIC_API void AF_SceneComputeEchogramBands(AF_SceneHandle scene,
                                               AF_Vector3 listener,
                                               const AF_Vector3* sources, int count,
                                               float* outBins, int numBins,
                                               float binSeconds, float speedOfSound,
                                               int numRays, int maxBounces,
                                               float distanceRef);

/* 【可視化】origin から dir 方向へ鏡面反射で maxBounces 回まで追った経路（通過点）を
 * outPoints に書き、その点数を返す。outPoints[0]=origin/以降=反射点/最後=終端。
 * outPoints は maxPoints 個以上（最低 maxBounces+2）。反響経路の線描画用。 */
ACOUSTIC_API int AF_SceneTraceReflectionPath(AF_SceneHandle scene,
                                             AF_Vector3 origin, AF_Vector3 dir,
                                             float maxDist, int maxBounces,
                                             AF_Vector3* outPoints, int maxPoints);

/* 【B: キューブマップ エッジカタログ】リスナー中心に res²×6面のレイを撒き、深度不連続で
 * シルエット稜線を拾ってカタログ化する。以降 diffraction はこのカタログの稜線を優先し、
 * 有効な迂回が無ければ箱コーナー探索へフォールバック。1回撒けば全音源で共有（リスナー係留）。
 *   res=面解像度(例16〜32) / maxDist=レイ到達距離。毎フレーム or 低レートで呼ぶ。 */
ACOUSTIC_API void AF_SceneBuildEdgeCatalog(AF_SceneHandle scene, AF_Vector3 listener,
                                           int res, float maxDist);

/* カタログの稜線数（デバッグ用）。 */
ACOUSTIC_API int AF_SceneEdgeCatalogCount(AF_SceneHandle scene);

/* カタログを消す（箱コーナー探索に戻す）。 */
ACOUSTIC_API void AF_SceneClearEdgeCatalog(AF_SceneHandle scene);

/* 【早期反射タップ(A)】source→…→listener の主要な初期反射を最大 maxTaps 本抽出する。
 * 各タップ = imageSourcePos（= listener + 到来方向×経路長。定位/距離減衰用の像源位置）＋
 * 6帯域ゲイン。エネルギー強い順・近い方向はまとめる。書き込んだタップ数を返す。
 *   outImagePos : AF_Vector3 × maxTaps（像源のワールド位置）
 *   outGain     : float × maxTaps*6（タップごとの帯域ゲイン0..1）
 * これを Wwise Reflect の image source / 仮想エミッタで「方向つき反射音」として鳴らす。 */
ACOUSTIC_API int AF_SceneComputeEarlyReflections(AF_SceneHandle scene,
                                                 AF_Vector3 listener, AF_Vector3 source,
                                                 AF_Vector3* outImagePos, float* outGain,
                                                 int maxTaps, int numRays, int maxBounces);

/* 登録済みインスタンス数（デバッグ用）。 */
ACOUSTIC_API int AF_SceneInstanceCount(AF_SceneHandle scene);

/* ============================================================================
 * リスナー / 音源の登録（API移行 段1: docs/API_MIGRATION_PLAN.md）
 *
 * これまで listener/source はクエリごとに引数で渡していたが、SPEC §2 では
 * エンジンが保持し、内部で音源ループを回す。ここではまず「保持」だけを用意する。
 * 既存クエリは引数版のまま動くので、段1では挙動は変わらない。
 *
 * 使い方: セットアップで音源を登録し、毎フレーム AF_SceneSetListener /
 *         AF_SceneSetSource で位置だけ更新する（同じ id の再呼び出しは位置更新）。
 * ============================================================================ */

/* リスナー位置を設定する。 */
ACOUSTIC_API void AF_SceneSetListener(AF_SceneHandle scene, AF_Vector3 pos);

/* 音源を登録/更新する。既存の id なら位置を更新するだけ。
 *   id : ホスト側の音源識別子。Wwise の GameObject ID と揃えておくと配線が楽。 */
ACOUSTIC_API void AF_SceneSetSource(AF_SceneHandle scene,
                                    unsigned long long id, AF_Vector3 pos);

/* 音源を削除する。存在しない id は無視。 */
ACOUSTIC_API void AF_SceneRemoveSource(AF_SceneHandle scene, unsigned long long id);

/* 登録済み音源をすべて削除する。 */
ACOUSTIC_API void AF_SceneClearSources(AF_SceneHandle scene);

/* 登録済み音源の数。 */
ACOUSTIC_API int AF_SceneSourceCount(AF_SceneHandle scene);

/* ============================================================================
 * バッチ更新（API移行 段2: docs/API_MIGRATION_PLAN.md）
 *
 * SPEC §4 のフレーム内パイプライン。ホストは毎フレーム AF_SceneUpdate を 1 回呼び、
 * 結果を AF_SceneGet* で読む。「どの計算をいつ走らせるか」はエンジンが内部レートで
 * 管理する（ホストがカウンタを持たない＝移植時に書き直す部分が減る）。
 *
 * 結果は「直前の AF_SceneUpdate ぶん」。段5でワーカースレッド化すると 1 フレーム遅延になる。
 * ============================================================================ */

/* 更新設定。0 以下の間隔は 1 として扱う。 */
typedef struct AF_UpdateConfig {
    int   role1EveryN;          /* 遮蔽・回折の更新間隔(フレーム) */
    int   role2EveryN;          /* 残響(エコグラム)の更新間隔 */
    int   earlyEveryN;          /* 早期反射の更新間隔 */
    int   diffSrcEveryN;        /* 回折二次音源の更新間隔 */
    int   catalogEveryN;        /* エッジカタログの更新間隔 */

    int   reflectionRays;       /* 役割1: 反射込み遮蔽のレイ数 */
    int   reflectionBounces;
    float directWeight;
    int   useReflections;       /* 0/1 */

    int   useEdgeCatalog;       /* 0/1 */
    int   edgeCatalogRes;
    float edgeCatalogMaxDist;

    int   enableReverb;         /* 0/1 */
    int   echogramBins;
    float echogramBinSeconds;
    int   echogramRays;
    int   echogramBounces;
    float speedOfSound;
    float distanceRef;          /* 音源からの広がり損失の基準距離。0=無効 */

    int   enableEarlyReflections; /* 0/1 */
    int   earlyTaps;
    int   earlyRays;
    int   earlyBounces;
    int   enableDiffractionSources; /* 0/1 */
    int   diffSources;
} AF_UpdateConfig;

/* 設定を渡す（変わったときだけでよい）。 */
ACOUSTIC_API void AF_SceneSetUpdateConfig(AF_SceneHandle scene, const AF_UpdateConfig* cfg);

/* 毎フレーム 1 回。内部レートに従って各役割を実行し、結果を内部バッファへ書く。 */
ACOUSTIC_API void AF_SceneUpdate(AF_SceneHandle scene, float dt);

/* --- 結果取得（index は登録順。AF_SceneSourceIndex で id から引く）--- */

/* 音源 id → index。見つからなければ -1。 */
ACOUSTIC_API int AF_SceneSourceIndex(AF_SceneHandle scene, unsigned long long id);

/* 帯域別の生存ゲイン(6要素)。透過⊕回折⊕反射の結果。 */
ACOUSTIC_API void AF_SceneGetSourceOcclusion(AF_SceneHandle scene, int index, float* out6);

/* 遮蔽スカラ(0..1)。1=完全遮蔽。 */
ACOUSTIC_API float AF_SceneGetSourceOcclusionScalar(AF_SceneHandle scene, int index);

/* エネルギーが届く支配方向(単位ベクトル・3要素)。 */
ACOUSTIC_API void AF_SceneGetSourceArrivalDir(AF_SceneHandle scene, int index, float* out3);

/* 早期反射タップ。像源位置と6帯域ゲインを書き、本数を返す。 */
ACOUSTIC_API int AF_SceneGetEarlyReflections(AF_SceneHandle scene, int index,
                                             AF_Vector3* outPos, float* outGain6, int maxTaps);

/* 回折二次音源。位置とゲインを書き、本数を返す。 */
ACOUSTIC_API int AF_SceneGetDiffractionSources(AF_SceneHandle scene, int index,
                                               AF_Vector3* outPos, float* outGain, int maxSrc);

/* 帯域別エコグラム。outBins[k*6 + b] に書き、書けたビン数を返す。 */
ACOUSTIC_API int AF_SceneGetEchogramBands(AF_SceneHandle scene, float* outBins, int numBins);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_SCENE_H */
