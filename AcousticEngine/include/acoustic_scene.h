/* acoustic_scene.h ── Scene ベースの幾何/音響クエリ C API
 *
 *   - 幾何 = インスタンス（geomId + OBB transform + materialId）
 *   - 出力 = 6帯域（1スカラに潰さない）
 *
 * ★2026-09-03: 旧 acoustic_engine.h（AddBox/IsOccluded 系と Wwise 風の再生 API）を削除した。
 *   DLL 側は既定でスタブ Adapter だったので、あの再生 API は何も鳴らしていなかった。
 *   ここが C の唯一の窓口になったので、エクスポートのマクロと AF_Vector3 もここで定義する。
 *
 * ハンドルは Scene 実体への不透明ポインタ。C# 側は IntPtr。
 */
#ifndef ACOUSTIC_SCENE_H
#define ACOUSTIC_SCENE_H

/* DLL ビルド側（ACOUSTICENGINE_EXPORTS）でだけ dllexport。利用側は dllimport。 */
#ifdef _WIN32
  #ifdef ACOUSTICENGINE_EXPORTS
    #define ACOUSTIC_API __declspec(dllexport)
  #else
    #define ACOUSTIC_API __declspec(dllimport)
  #endif
#else
  #define ACOUSTIC_API   /* 非 Windows では不要 */
#endif

/* 3 次元ベクトル。C# 側の AFVector3（float×3, blittable）と並びを揃える。 */
typedef struct AF_Vector3 {
    float x;
    float y;
    float z;
} AF_Vector3;

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

/* 【材質プリセットの唯一の出どころ】preset 番号の 6 帯域値を書く。書けた帯域数を返す。
 *   preset: 0=Default / 1=Concrete / 2=Glass / 3=Opaque / 4=WoodDoor
 *   各 out は 6 要素以上、または null（要らないものは null でよい）。
 *
 * ★ホスト側がプリセット値を自前で持つと必ずずれる。実際に C++ と C# で
 *   Default と Concrete の透過が 6〜8dB 食い違っていた（C++ 側だけ意図的に上げて、
 *   C# が置き去りになった）。回帰テストが守る遮音量と出荷する音が違う状態で、
 *   決めごと #1「同じ問いに 2 つの答えを持たせない」そのもの。
 * preset: 0 Default（内壁）/ 1 Concrete / 2 Glass / 3 Opaque（検証用）/ 4 WoodDoor /
 *         5 WoodRoom（板張りの部屋）/ 6 Stone（石積み）/ 7 Cave（岩肌）/ 8 Snow（新雪）。範囲外は 0。
 *   5〜8 は「部屋の響きのプリセット」（2026-09-08）。部屋の RT60 は Sabine で形と材質から出るので、
 *   響きのプリセットは材質として持つ（RT60 の表を別に持たない）。
 *   ホストはこの関数から引くこと。 */
ACOUSTIC_API int AF_MaterialPresetBands(int preset, float* outTransmission,
                                        float* outAbsorption, float* outScattering);

/* 材質をテーブルに追加し materialId を返す。
 *   transmission/absorption/scattering : 各帯域(0..1)配列。null なら既定壁の該当値。
 *   numBands : 配列要素数。内部帯域数(6)未満なら不足分は既定壁で補う。<=0 は全て既定。
 * 各配列は独立に反映（null の配列だけ既定のまま）。 */
ACOUSTIC_API int AF_SceneAddMaterial(AF_SceneHandle scene,
                                     const float* transmission,
                                     const float* absorption,
                                     const float* scattering,
                                     int numBands);

/* 材質の現在値を読む。書けた帯域数を返す（不要な出力は NULL 可）。
 *   調整の道具が「いま幾つか」を出すのに要る。値が見えないと追い込めない。 */
ACOUSTIC_API int AF_SceneGetMaterial(AF_SceneHandle scene, int materialId,
                                     float* outTransmission, float* outAbsorption,
                                     float* outScattering, int count);

/* 既存の材質の中身を書き換える。成功で 1。引数の意味は AF_SceneAddMaterial と同じ。
 * その材質を使っているインスタンスが**一斉に**変わる（「部屋をコンクリからガラスへ」）。
 * 材質はクエリのたびにテーブルを引き直しているので、次のフレームから効く。
 * BVH は形状だけなので再構築も起きない。 */
ACOUSTIC_API int AF_SceneSetMaterial(AF_SceneHandle scene, int materialId,
                                     const float* transmission,
                                     const float* absorption,
                                     const float* scattering,
                                     int numBands);

/* インスタンスの材質を付け替える。成功で 1。「この扉だけ木、この窓だけガラス」用。
 * 材質を1つ共有していると、薄い扉と厚いコンクリ壁が同じ透過率になってしまう。 */
ACOUSTIC_API int AF_SceneSetInstanceMaterial(AF_SceneHandle scene, int instanceId, int materialId);

/* インスタンスの現在の materialId。範囲外は -1。 */
ACOUSTIC_API int AF_SceneGetInstanceMaterial(AF_SceneHandle scene, int instanceId);

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

/* 【形状(BLAS)】三角形メッシュを登録し geomId を返す。失敗は -1。
 *   verticesXYZ: 頂点 x,y,z の連続配列（vertexCount 個ぶん = 3*vertexCount 要素）
 *   indices    : 三角形インデックス（indexCount 要素、3つで1三角形）。範囲外の三角形は捨てる。
 *   outLocalCenter / outLocalHalfExtents: 正規化に使ったローカル AABB（null 可）。
 *
 * 形状は「ローカル AABB が単位箱 [-1,1]^3」になるよう正規化して保持する。したがって
 * インスタンスの OBB がそのまま local→world の変換になり、同時にワールド境界ボックスも兼ねる。
 * ホストは outLocal* を使って OBB を作ること:
 *     center      = transform.TransformPoint(outLocalCenter)
 *     halfExtents = outLocalHalfExtents * lossyScale   （成分ごと。非一様スケール可）
 *     right/up    = transform の軸
 *
 * 実行時に呼べる。破壊やプロシージャル生成で形状が増えても、構築はこの1個ぶんだけで、
 * レベル全体の再計算は発生しない。 */
ACOUSTIC_API int AF_SceneAddMesh(AF_SceneHandle scene,
                                 const float* verticesXYZ, int vertexCount,
                                 const int* indices, int indexCount,
                                 AF_Vector3* outLocalCenter,
                                 AF_Vector3* outLocalHalfExtents);

/* 形状から抽出された回折稜線の本数（診断用）。無効な geomId は -1。
 * 二面角が平坦な稜線は候補にならないので、この本数は**テッセレーションに依存しない**
 * （壁を10倍細分しても本数は変わらない）。その確認に使う。 */
ACOUSTIC_API int AF_SceneGetMeshEdgeCount(AF_SceneHandle scene, int geomId);

/* 形状を解放する。参照していたインスタンスは箱（境界ボックス）扱いに落ちる。
 * 他の geomId は無効化されない（スロットを詰め直さないため）。 */
ACOUSTIC_API void AF_SceneRemoveMesh(AF_SceneHandle scene, int geomId);

/* メッシュ形状のインスタンスを追加し instanceId を返す。geomId は AF_SceneAddMesh の戻り値。
 * center/halfExtents/right/up は AF_SceneAddInstanceBox と同じ意味で、
 * メッシュではこれが変換行列と境界ボックスを兼ねる（上記の作り方に従うこと）。
 * geomId が無効なら箱として追加される。 */
ACOUSTIC_API int AF_SceneAddInstanceMesh(AF_SceneHandle scene,
                                         int geomId,
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

/* 【開口の開き具合】回折の**音量**を決める。稜線探索は定位だけを担当する。
 *
 * 開口まわりに経路と垂直な円盤を張り、listener→点→source が通るサンプルの割合を数える。
 * 閉まっていれば 0、開くにつれて連続に増える。扉が回っても戸口の枠は動かないので、
 * 前川の式(δ)や「開口点まわりの差し渡し」では開き具合を捉えられなかった。
 *
 *   ref     : これだけ開いていれば素通しとみなす割合。既定 0.30。
 *             円盤は戸口の周りの壁も含むので、全開の戸口でも 1 にはならない。
 *   power   : カーブ。既定 1.0（割合に比例）。小さいほど開き始めで立ち上がる。
 *   radius  : 断面を見る円盤の半径(m)。戸口を覆う大きさにする。既定 1.0。
 *   samples : 円盤のサンプル数。既定 12。多いほど滑らかで重い。0 で無効化。 */
ACOUSTIC_API void AF_SceneSetApertureOpen(AF_SceneHandle scene, float ref, float power,
                                          float radius, int samples);
ACOUSTIC_API void AF_SceneGetApertureOpen(AF_SceneHandle scene, float* outRef, float* outPower,
                                          float* outRadius, int* outSamples);

/* 【診断】フレネルゾーンのうちどれだけ開いているかを帯域別に返す（outFrac6 は6要素）。
 * 戻り値 1 = 遮蔽物があって制限が掛かる / 0 = 遮るものが無く制限しない。 */
ACOUSTIC_API int AF_SceneMeasureApertureFresnel(AF_SceneHandle scene, AF_Vector3 listener,
                                                AF_Vector3 source, float* outFrac6);

/* 【回折・帯域別】開口ごとに 6 帯域のゲインを返す（outBand6 は maxSrc*6 要素）。
 * 隙間が狭いほど高域だけが通る＝**開くと音色が開く**。実測（現実の扉の開閉を収録）で
 * 開−閉の差が 125Hz +2.6dB / 1kHz +10.3dB と、低域はほとんど変わらず中高域だけが
 * 大きく増えることを確認した。低域は閉じていても壁を抜けるので増える余地が無く、
 * 中高域は壁で止まっているぶん開いた分だけ丸ごと増える。 */
ACOUSTIC_API int AF_SceneComputeDiffractionSourceBands(AF_SceneHandle scene,
                                                       AF_Vector3 listener, AF_Vector3 source,
                                                       AF_Vector3* outPos, float* outGain,
                                                       float* outBand6, int maxSrc);

/* 隙間のハイパスの傾き。1.0 = 6dB/oct。小さくすると音色の変化が穏やかになる。
 * 実測に合わせる値ではなく、**聞かせたい音**に合わせて決めてよいチューニング値。 */
/* ============================ ポータル（開口の矩形） ============================
 * ホストが「ここが戸口」と矩形を置く。**開き具合は渡さない** ── エンジンが毎フレーム
 * 実際の形状から測る。だから扉が板で部分的に覆っていることも、音源が戸口の正面に
 * あるかどうかも、そのまま結果に出る（実測: 同じ扉角度で音源位置により 22.3dB と 2.9dB）。
 *
 * なぜ矩形を置くのか: 開口の測り方は「面のうちどこまで積分するか」に答えが無く、
 * 4通り試して全部失敗した。ポータルはその答えそのもの（積分範囲＝この矩形）。
 * エリアのトポロジはホスト、音響的な状態はエンジン、という役割分担。 */
ACOUSTIC_API int AF_SceneAddPortal(AF_SceneHandle scene, AF_Vector3 center,
                                   AF_Vector3 axisU, AF_Vector3 axisV,
                                   float halfU, float halfV);
ACOUSTIC_API void AF_SceneUpdatePortal(AF_SceneHandle scene, int id, AF_Vector3 center,
                                       AF_Vector3 axisU, AF_Vector3 axisV,
                                       float halfU, float halfV);

/* 【自動生成】部屋グラフの開口からポータルを作る。
 *
 * 「ここが開口である」という同じ事実を、エンジンが自動で持っているのに人がもう一度
 * 置き直していた。置き忘れるとフレネル帯域積分が一度も走らず、戸口がただの幾何の
 * 隙間として処理される ── **静かに劣化して気づく手段が無い**のがいちばん悪い。
 *
 * 手置きのポータルは消えない。自動生成ぶんだけがリストの末尾で入れ替わる。
 * 幾何が変わって部屋グラフが作り直されると自動で追随する。 */
ACOUSTIC_API void AF_SceneSetAutoPortals(AF_SceneHandle scene, int enable);

/* 音源ごとの段（遮蔽・回折／早期反射／回折二次音源）を何コアで回すか。1 以下＝直列。
 *   既定は 1（直列）。並列にしても**結果はビット一致**（音源ごとに自分の枠にしか
 *   書かない段だけを割るので、各音源の計算順も丸めも変わらない）。
 *   ⚠ スレッドはシーンが持ち、AF_SceneDestroy で join する。
 *      **シーンを破棄する前に再生を止めること。**
 *   ⚠ 診断カウンタは thread_local。並列時は呼び出し元スレッドのぶんしか読めない。 */
ACOUSTIC_API void AF_SceneSetWorkerThreads(AF_SceneHandle scene, int threads);
ACOUSTIC_API int  AF_SceneGetWorkerThreads(AF_SceneHandle scene);

/* ── 音源の段 ─────────────────────────────────────────────────────────
 * どこまで解くかを**音源ごとに固定**する（オーサリング）。
 *   0 = 厳密   … 回折の合成まで解く。体験の芯（扉の奥の音・探しているベル）
 *   1 = 簡易   … 遮蔽の音量と帯域カーブだけ。回り込みの方向は出ない
 *   2 = バーチャル … 解かない。ホストは再生位置だけ進める
 * 既定は 0（厳密）＝ 既存のホストの音は変わらない。
 *
 * ⚠ 距離で自動に切り替えないこと。歩くだけで段が変わり、切り替わりが聞こえる
 *   （同じ音源が場面によって別の仕組みで鳴る＝決めごと #1）。
 *
 * ★2D の音（UI・音楽・ナレーション）に段はない。**音源として登録しない**。
 *   エンジンが返せる答えが存在しないので、「何もしない段」を作るとホストの判断が
 *   エンジン側へ漏れる。 */
/* 【調整支援】リスナー→音源の直線の透過損失を**誰が担っているか**を大きい順に返す。
 *   outLossDb は帯域平均の透過損失(dB, 正)。大きいほどよく遮っている＝担い手。
 *   戻り値は書けた数。
 *
 * ★「この地点でこう聞こえてほしい」に合わせるとき、いちばん困るのは
 *   **どの材質を触れば効くのか分からない**こと。それを engine が名指しする。
 * ⚠ 診断であって、音の経路を増やす物ではない。鳴るのは合成後の 1 つのまま（決めごと #1）。
 * ⚠ 透過だけを見る。回り込み（回折）が担っている帯域では、ここを触っても動かない。
 *   透過ぶんは AF_SceneComputeSoftOcclusion、合成後は AF_SceneGetSourceOcclusion で採れるので、
 *   その差が回折の取り分になる。**触る前にどちらが担っているかを確かめること。** */
ACOUSTIC_API int AF_SceneTransmissionCarriers(AF_SceneHandle scene,
                                              AF_Vector3 listener, AF_Vector3 source,
                                              int* outInstance, int* outMaterial,
                                              float* outLossDb, int maxCount);

/* 音源の段（0 厳密／1 簡易／2 バーチャル）。予算（AF_SceneSetTierBudget）を使うときは**上限**になる
 * （簡易にした音源は予算が余っても厳密にならない）。既定 0。 */
ACOUSTIC_API void AF_SceneSetSourceTier(AF_SceneHandle scene, unsigned long long id, int tier);

/* 自由音場で聞こえなくなる距離(m)。0 以下＝自動でバーチャルへ落とさない（既定）。
 *
 * ⚠ 距離だけでは判定していない。臨界距離 rc = 0.057√(V/RT60) より遠くでは
 *   **残響が直接音を上回り、しかも距離でほとんど減らない**。だから
 *   「rc <= この半径」の部屋（＝響く部屋）では自動バーチャルを使わない。
 *   自由音場の直接音だけで切ると、響く部屋で聞こえている音を黙らせる。
 * 出入りの閾は分けてある（入り = 半径 / 出 = 半径 × 1.25）。 */
ACOUSTIC_API void AF_SceneSetSourceAudibleRadius(AF_SceneHandle scene,
                                                 unsigned long long id, float metres);

/* いま実際に使われている段（自動バーチャルと予算の結果を含む）。index は AF_SceneSourceIndex。
 *   0 厳密／1 簡易／2 バーチャル（可聴限界の外＝素通し。ホストは尾も止めてよい）／
 *   3 保持（予算から漏れた。最後の答えを保っている。**聞こえている音源なので止めないこと**）。
 * 見つからなければ -1。 */
ACOUSTIC_API int AF_SceneGetSourceTierEffective(AF_SceneHandle scene, int index);

/* ── 段の予算と順位（2026-09-04）── 主スレッドの上限。
 * 主スレッドの費用は「厳密の本数」で決まる（1 本 0.3 ms の桁。簡易 0.05 ms、保持 0）。予算を超えたぶんは
 * 可聴性の見積もり score = 音量 × 1/r × 前フレームの生存 × 重要度 の低い順に落とす。
 *   exactMax / simpleMax … 厳密・簡易の本数。0 以下＝その段は無制限（既定。呼ばなければ今までどおり手動の段）。
 *   落ちた音源は「保持」（AF_SceneGetSourceTierEffective が 3）: 役割1 を飛ばして最後の答えを保ち、面の線と
 *   二次音源は 0 本、尾は部屋の代表の物をそのまま。素通しにはしない。保持の音源は毎フレーム 1 本ずつ
 *   （いちばん古い物から）簡易で探り直すので、扉が開いて聞こえるようになれば順位が上がる。
 * ヒステリシス: 現職に段 1 つあたり +3 dB の加点、昇格は 0.5 s・降格は 1 s 続いてから（登録直後は待たない）。
 * ⚠ 厳密の上限は硬い。簡易は降格の通過点として最大 1 s だけ超えることがある。 */
ACOUSTIC_API void  AF_SceneSetTierBudget(AF_SceneHandle scene, int exactMax, int simpleMax);
/* 順位の物差しの「音量」（線形、既定 1）。AudioSource.volume などを毎フレーム押してよい。 */
ACOUSTIC_API void  AF_SceneSetSourceLoudness(AF_SceneHandle scene, unsigned long long id, float gainLinear);
/* 重要度（倍率、既定 1。2 = +6 dB ぶん落ちにくい）と固定（pinned != 0: 予算に関わらず手動の段のまま。
 * 枠は消費する。探しているベルのような体験の芯だけに）。デザイナに渡すつまみ。 */
ACOUSTIC_API void  AF_SceneSetSourceImportance(AF_SceneHandle scene, unsigned long long id,
                                               float importance, int pinned);
/* 直近の順位の物差し（診断・HUD）。範囲外は -1。 */
ACOUSTIC_API float AF_SceneGetSourcePriority(AF_SceneHandle scene, int index);
/* このフレームの探り（保持 → 簡易で解き直し）に選ばれた音源の index。無ければ -1。診断用。 */
ACOUSTIC_API int   AF_SceneGetTierProbeIndex(AF_SceneHandle scene);

/* この音源の尾（後期残響）を担っている代表音源の index。範囲外は -1。
 *
 * ★尾は**部屋の形にしか依存しない**ので、同じ部屋の音源は 1 本を共有する。
 *   代表は「同じ部屋のいちばん若い index」で決め打ち。位置で選ぶと音源が動くたび
 *   代表が入れ替わり、尾の形が乗り換わって**段差**になる。
 *   部屋が取れない（屋外）音源は自分自身が代表。
 *
 * ⚠ ホスト側で同じ規則を持たないこと。2 箇所にあると、片方だけ変えたときに
 *   「エンジンが計算した代表」と「ホストが読む代表」がずれる。
 * ★AF_SceneGetEchogramBands はどの index でも部屋の尾を返すので、
 *   尾を引くだけならこれを呼ぶ必要はない。IR の作り直しを
 *   「代表が変わったときだけ」に絞りたいホスト向け。 */
ACOUSTIC_API int AF_SceneGetTailShapeIndex(AF_SceneHandle scene, int index);

/* ============================================================================
 * キャプチャ ── 「入力」と「音響の出力」を常時録っておき、あとから走査する。
 *   仕様: docs/SOUND_DEBUG_TOOL.md
 *
 * 使い方:
 *   起動時に AF_SceneCaptureBegin。以後は何もしなくてよい（Update の中で溜まる）。
 *   「いま変だった」と思ったら AF_SceneCaptureMark。押した時点の**前** preroll と
 *   **後** postroll が揃うと Status が 2 になり、AF_SceneCaptureWrite で保存できる。
 *
 * ★録るのは結果ではなく**入力**なので、押し直せばそれが再現になる。
 *   録音後に走査して破れを見つけ、そこから回帰テストを生やす（型紙は detectors.h）。
 * ========================================================================== */

/* 録音を始める。0 以下を渡した項目は既定値（preroll 21秒・postroll 5秒・64音源・48kHz）。 */
ACOUSTIC_API void AF_SceneCaptureBegin(AF_SceneHandle scene, int prerollFrames,
                                       int postrollFrames, int maxSources,
                                       int sampleRate, int recordPcm);

/* 録音を止める。溜めた分は保持される。 */
ACOUSTIC_API void AF_SceneCaptureEnd(AF_SceneHandle scene);

/* 段の間引きの位相（8要素）。
 *   [0]catalog [1]diffSrc [2]early [3]echoRaySlice [4]staggered [5]echoPrimed
 * ★入力リプレイで**尾を再現する**ために要る。これを戻さないと、エコグラムの
 *   レイを撃つフレームが録音時とずれ、扉が動き続けるかぎり永久に一致しない
 *   （実測: 91 フレーム全部が不一致・最大 15.5% ずれ）。 */
ACOUSTIC_API void AF_SceneDebugGetStagePhase(AF_SceneHandle scene, int* out8);
ACOUSTIC_API void AF_SceneDebugSetStagePhase(AF_SceneHandle scene, const int* in8);

/* ============================================================================
 * 録った .afcap を**開いて調べる**（ホスト側の道具用）
 *
 * ★走査（型紙を当てる）は DLL の中で回す。
 *   理由: 回帰テストが呼ぶのと**同じ関数**でなければ意味がないから
 *   （「道具は見つけたのに検査は通る」が起きる）。C# へ写経すると必ずずれる。
 * ⚠ ただし AF_SceneUpdate の経路からは呼ばない。判定は**録音後**であること自体が
 *   設計（検出器を後で良くしたら昔のキャプチャに付け直せる）。
 * ========================================================================== */

typedef void* AF_CaptureHandle;

typedef struct AF_CaptureInfo {
    int frames;
    int sampleRate;
    int maxSources;
    int preroll;
    int postroll;
    int workerThreads;
    int markedFrame;
    unsigned int dllHash;
    int boxCount;
    int meshCount;        /* >0 なら C++ の検査には吐けない場面 */
    int materialCount;
    int pcmFrames;
    int sourceCount;      /* キャプチャに出てくる音源の種類数 */
} AF_CaptureInfo;

typedef struct AF_CaptureMark {
    unsigned long long sourceId;
    int   frame;
    float seconds;
    float amount;
    int   reserved;
    /* ⚠ 中身は **UTF-8**。C# 側で ByValTStr（既定 Ansi）で受けると化ける。
     *   byte[] で受けて自分で UTF-8 として読むこと（Native.Utf8）。 */
    char  templateName[40];   /* 例: "型紙1 跳ばない" */
    char  what[24];           /* 例: "level" */
} AF_CaptureMark;

/* 開く。失敗で NULL。使い終わったら必ず AF_CaptureClose。 */
ACOUSTIC_API AF_CaptureHandle AF_CaptureOpen(const char* path);
ACOUSTIC_API void AF_CaptureClose(AF_CaptureHandle cap);

/* 概要。成功で 1。 */
ACOUSTIC_API int AF_CaptureGetInfo(AF_CaptureHandle cap, AF_CaptureInfo* out);
/* 録音時の場面名。書けた文字数を返す。 */
ACOUSTIC_API int AF_CaptureGetSceneName(AF_CaptureHandle cap, char* buf, int bufSize);
/* 音源 id の一覧。書けた数を返す。 */
ACOUSTIC_API int AF_CaptureGetSourceIds(AF_CaptureHandle cap,
                                        unsigned long long* out, int maxOut);

/* 型紙を当てて印を集める。見つかった数を返す（out が足りなければ maxOut まで）。 */
ACOUSTIC_API int AF_CaptureScan(AF_CaptureHandle cap, AF_CaptureMark* out, int maxOut);

/* 連続した同じ印をひとまとめにしたもの。
 * ★実機のキャプチャで要ると分かった: 閉扉の幻は**何秒も続く**ので、
 *   生のままだと 382 件並んで読めない（まとめると 5 か所だった）。
 * ⚠ count が 1 か 100 かは**意味が違う**（瞬きか、続いているか）。必ず長さも見ること。 */
typedef struct AF_CaptureRun {
    unsigned long long sourceId;
    int   firstFrame;
    int   lastFrame;
    int   count;          /* 続いたフレーム数 */
    int   reserved;
    float firstSeconds;
    float lastSeconds;
    float worst;          /* 続いたあいだの最大の破れ量 */
    float reserved2;
    char  templateName[40];   /* UTF-8 */
    char  what[24];           /* UTF-8 */
} AF_CaptureRun;

/* まとめた印。まとまり方は AfCapScan（コマンドライン）とまったく同じ。 */
ACOUSTIC_API int AF_CaptureScanRuns(AF_CaptureHandle cap, AF_CaptureRun* out, int maxOut);

/* マスターPCM（interleaved stereo）。書けたフレーム数を返す。 */
ACOUSTIC_API int AF_CaptureGetPcm(AF_CaptureHandle cap, float* outInterleaved, int maxFrames);

/* 入力リプレイ: frameIndex のフレームの**入力**を scene へ押す。
 *   ⚠ 形（静的なジオメトリ）は呼び手が用意しておくこと。この関数は形に触らない。
 *   ⚠ frameIndex == 0 のときに段の間引きの位相も戻す。**必ず 0 から順に呼ぶこと**。
 *      飛ばして呼ぶと尾が再現しない（docs/SOUND_DEBUG_TOOL.md §3.1）。 */
ACOUSTIC_API int AF_CaptureApplyFrame(AF_SceneHandle scene, AF_CaptureHandle cap,
                                      int frameIndex);

/* 印 1 つ → C++ の回帰テストを吐く。書けた文字数を返す（0 なら吐けない場面）。
 *   ★吐くのは「入力」と「どの性質を縛るか」だけ。**録れた出力の数値は入らない**。 */
ACOUSTIC_API int AF_CaptureEmitCase(AF_CaptureHandle cap, int markIndex,
                                    const char* testName, char* buf, int bufSize);

/* 「いま変だった」を押す。2 回目以降は前後が揃うまで無視される。 */
ACOUSTIC_API void AF_SceneCaptureMark(AF_SceneHandle scene);

/* 0=止まっている 1=録っている 2=前後が揃った（保存できる）。
   outFramesHeld に保持中のフレーム数を書く（NULL 可）。 */
ACOUSTIC_API int AF_SceneCaptureStatus(AF_SceneHandle scene, int* outFramesHeld);

/* マスターPCM を渡す（interleaved stereo）。
   ⚠ オーディオスレッドから呼ぶこと。内部でロックも確保もしない。 */
ACOUSTIC_API void AF_SceneCapturePushAudio(AF_SceneHandle scene,
                                           const float* interleavedStereo, int frames);

/* .afcap として保存する。成功で 1。
   sceneName / dllHash はヘッダに焼く識別子（開いたときに現物と照合するため）。 */
ACOUSTIC_API int AF_SceneCaptureWrite(AF_SceneHandle scene, const char* path,
                                      const char* sceneName, unsigned int dllHash);
/* これ未満の断面積(m2)の開口はポータルにしない。格子の量子化ノイズで
 * ありもしない戸口が並ぶのを防ぐ。既定 0.25 m2。 */
ACOUSTIC_API void AF_SceneSetAutoPortalMinArea(AF_SceneHandle scene, float m2);
/* outAuto / outManual に内訳を書く（NULL 可）。戻り値は合計。
 * 並びは [手置き ... , 自動生成 ...]。自動生成ぶんは末尾に固まっている。 */
ACOUSTIC_API int  AF_SceneGetPortalCounts(AF_SceneHandle scene, int* outAuto, int* outManual);

/* ポータル 1 枚の矩形を読み出す。戻り値 0 で範囲外。
 * 自動生成が戸口を正しく見つけたかは**見れば分かる**ようにしておきたいので、
 * ホストがギズモを描けるようにする（音で気づくのは難しい）。 */
ACOUSTIC_API int  AF_SceneGetPortal(AF_SceneHandle scene, int id, AF_Vector3* outCenter,
                                    AF_Vector3* outAxisU, AF_Vector3* outAxisV,
                                    float* outHalfU, float* outHalfV);

/* ポータルが繋いでいる部屋（自動生成のときだけ入る。手置きは -1/-1）。
 * outToOutside=1 なら片側が「外の世界」の口（洞窟の口・屋外へ開く戸口。roomA=-1）。 */
ACOUSTIC_API int  AF_SceneGetPortalRooms(AF_SceneHandle scene, int id,
                                         int* outRoomA, int* outRoomB, int* outToOutside);

/* 外の世界へ開く口も開口（自動ポータル）にするか。既定 0。
 * ★1 にしても回折・エコグラムの経路生成はその口を使わない（従来の数値を変えないため）。
 *   使うのは「扉の定点」（隣の空間の響きを戸口の位置から鳴らす仕組み）だけ。
 *   ON にすると、その部屋の Sabine の境界面積に口が吸音率 1 で入るので RT60 が少し短くなる。 */
ACOUSTIC_API void AF_SceneSetOutsideApertures(AF_SceneHandle scene, int on);

/* 【扉の定点】隣の空間の**拡散した響き**がこの口を通る割合（帯域別 0..1、6 要素）。
 * 音源に依存しない。口の面に垂直な短い線分を 9×3 並べて透過を測り、面積で平均する。
 *   開いている所 1.0 ／ 板（閉じた扉）に覆われた所 その材質の質量則
 * ＝ τ_eff = τ_板·(覆われた割合) + (開いた割合)。戻り値 0 で範囲外。 */
ACOUSTIC_API int  AF_ScenePortalDiffuseCoupling(AF_SceneHandle scene, int id, float* out6);

/* ポータルの「支配が及ぶ距離」(m)。矩形と経路の距離がこれを超えたら、
 * 回折は一般の稜線探索に完全に戻る。あいだは滑らかに混ざる。
 *
 * ★以前は「シーンに 1 枚でもポータルがあれば回折はポータルが全部決める」だった。
 *   自動生成すると全シーンがその状態になり、部屋の中央の柱の回り込みまで
 *   ポータルが答えることになる（実測 -3.3dB）。既定 1.0m。 */
/* 【計測用】回折の可視判定のゲートを個別に切る。0 = 全部有効（本番）。
 * bit0 pointInsideOther / bit1 penNearWeight / bit2 crossesCore
 * どのゲートが死角を作っているかを切り分けるためだけの口。 */
/* 【(B)】稜線からポータルを生成してフレネル積分する。既定 0（従来の前川＋開口積分）。 */
/* 【計測用】直近の開口積分で矩形に写った遮蔽物の枚数。 */
ACOUSTIC_API int AF_SceneDebugPortalPolys(AF_SceneHandle scene);
/* 【計測用】直近の開口積分の 125Hz の分子・分母・積分範囲。 */
ACOUSTIC_API void AF_SceneDebugPortalIntegral(AF_SceneHandle scene, double* numer, double* denom, float* limU);

/* 【診断】最強の回折経路の内訳。out17 は 17 要素。戻り値は経路数（0 なら遮蔽なし／経路なし）。
 *   [0]=δ / [1]=openGain / [2]=slitWidth / [3]=thru(低域加重の広帯域)
 *   [4..9]=gain[6]（前川×openGain 済み）/ [10..15]=openBand[6] / [16]=nSpread */
/* 回折の減衰を前川だけに任せる（フレネル積分 f を重ねて掛けない）。既定 OFF。
 *   前川は f の近似なので、両方掛けるのは同じ物理を二度数えること。
 *   ON にすると回折は「開口への定位」と「回り込むぶんの距離」だけを運ぶ。 */
ACOUSTIC_API void AF_SceneSetDiffractionSingleModel(AF_SceneHandle scene, int on);

ACOUSTIC_API void AF_SceneDebugDiffractionCounts(AF_SceneHandle scene, int* raw, int* cut, int* clusters);

ACOUSTIC_API void AF_SceneSetInsideOtherContinuous(AF_SceneHandle scene, int on, float scale);

ACOUSTIC_API void AF_SceneSetKeepDoubleOpen(AF_SceneHandle scene, int on);
ACOUSTIC_API int AF_SceneDebugDiffractionPath(AF_SceneHandle scene, AF_Vector3 listener,
                                              AF_Vector3 source, float* out17, int which);

ACOUSTIC_API void AF_SceneSetEdgePortals(AF_SceneHandle scene, int enable);
ACOUSTIC_API void AF_SceneSetEdgePortalSpan(AF_SceneHandle scene, float k);

ACOUSTIC_API void AF_SceneSetDiffractionGateMask(AF_SceneHandle scene, int mask);

ACOUSTIC_API void AF_SceneSetPortalGovernRange(AF_SceneHandle scene, float meters);

/* ポータルがどれだけ開いているかを帯域別に測る。
 *   outFrac6 : 帯域ごとに通る割合(0..1)。完全に塞がれれば 0、素通しなら 1。
 *   outPoint : 開いている部分の重み付き重心（定位に使う）。
 *              エネルギーと方向が同じ積分から出るので、両者がずれて飛ばない。 */
ACOUSTIC_API int AF_SceneMeasurePortal(AF_SceneHandle scene, int id,
                                       AF_Vector3 listener, AF_Vector3 source,
                                       float* outFrac6, AF_Vector3* outPoint);

/* 開口を「透過の一部」として扱う。0=従来（前川×開口率） / 1=合成透過率の f の項。既定 0。
 * ON では開口タップが前川の δ 減衰を払わず f をそのまま持つ（δ の効果は f の重みに既に入って
 * いるので、掛けると二重になる）。開口を通る成分は開口の方向から届くので、直接音ではなく
 * 開口のタップへ入る＝定位は壊れない。 */
ACOUSTIC_API void AF_SceneSetApertureIsTransmission(AF_SceneHandle scene, int on);

/* ── 部屋の検出（Rooms & Portals の土台）───────────────────────────
 * 静的な形状だけをボクセル化し、空きの連結成分を「部屋」とする。
 * 一度でも変換が更新されたインスタンス（＝扉のように動くもの）は静的に含めない。
 * 含めると閉扉時に戸口が塞がって「そこに開口がある」という情報が幾何から消えるため。
 * 幾何が変わったときだけ再計算される（毎フレーム呼んでも中で弾かれる）。 */
ACOUSTIC_API int  AF_SceneRoomCount(AF_SceneHandle scene);
/* 点がどの部屋にいるか。-1 は部屋の外／実体の中。 */
ACOUSTIC_API int  AF_SceneRoomAt(AF_SceneHandle scene, AF_Vector3 p);
/* 部屋の体積(m3 相当)・重心・境界。out は null 可。 */
ACOUSTIC_API int  AF_SceneRoomInfo(AF_SceneHandle scene, int room, float* outVolume,
                                   AF_Vector3* outCentroid,
                                   AF_Vector3* outMin, AF_Vector3* outMax);
/* ボクセル一辺(m)。戸口の幅を数ボクセルで割れる大きさにすること（既定 0.25）。 */
ACOUSTIC_API void AF_SceneSetRoomCellSize(AF_SceneHandle scene, float meters);
/* 検出に使った格子の寸法（診断用）。 */
/* 格子が上限に当たって粗くなっていないかを調べる。戻り値 1 で降格あり。
 *
 * 格子は**登録された全ボックスの AABB 全体**を覆う。総ボクセル数が上限
 * （既定 400 万）を超えると、収まるまでセルを 1.5 倍ずつ粗くする。
 * つまり広い地面を 1 枚置くだけで AF_SceneSetRoomCellSize が黙って無視される。
 * 0.25m → 0.375m に降格すると 0.9m の戸口で部屋が割れなくなるので、
 * **音の結果が変わるのに何も出ない**。ホストはこれを見て警告すること。
 *   outRequested : 要求したセル(m)
 *   outActual    : 実際に使われたセル(m)
 *   outVoxels    : 実際の総ボクセル数
 *   outMaxVoxels : 上限
 * いずれも NULL 可。 */
ACOUSTIC_API int AF_SceneRoomGridDegraded(AF_SceneHandle scene, float* outRequested,
                                          float* outActual, double* outVoxels,
                                          double* outMaxVoxels);

ACOUSTIC_API void AF_SceneRoomGridDims(AF_SceneHandle scene, int* nx, int* ny, int* nz,
                                       float* cell);
/* 構築の段別所要時間(ms)と、直近の更新で塗り直したブロック数。
 * どこを削るべきかを推測でなく数字で決めるため。null 可。
 * dirtyBricks が 0 なら全再構築、>0 なら差分更新。 */
ACOUSTIC_API void AF_SceneRoomBuildTimes(AF_SceneHandle scene, float* alloc, float* fill,
                                         float* dist, float* label, float* merge,
                                         float* grow,
                                         int* dirtyBricks, int* totalBricks);

/* 【部屋を戸口で割る半径(m)】既定 0.6。⚠ 種 ≤ 戸口の半幅だと部屋が戸口から外へ漏れて 0 個になる（実測 2026-09-05:
 * 0.6 は戸口 1.0 m まで、0.8 は 1.4 m まで、1.0 でも 2.0 m は漏れる）。場面ごとに**いちばん広い戸口の半幅 ＋ 1 セル**より大きく、
 * 狭い所の半分より小さく（0.8 にすると 1.0 m の廊下が消え、回帰の [焼き] も落ちた。既定を上げるのは見送った）。0 個なら AF_SceneRoomCount で分かる。
 * 自由空間をこの半径ぶん侵食してから連結成分を取る。
 * 素の連結成分だと戸口で繋がった空間が全部ひとつの部屋になり、扉の向こうも同じ部屋に
 * なってしまう（残響を切り替える土台にならない）。幅がこの 2 倍に満たないくびれが
 * 千切れるので、そこで部屋が分かれる。落とした殻は最寄りの部屋へ塗り戻すので、
 * 全ボクセルに部屋が付く（戸口に立っていても必ずどちらかになる）。
 * 0 で侵食なし＝素の連結成分。目安: 戸口の幅の半分 < 値 < 部屋の狭い所の半分。 */
ACOUSTIC_API void AF_SceneSetRoomSeedRadius(AF_SceneHandle scene, float meters);

/* 【回折を平坦にする】既定 ON(1)。回折が持つ情報を「開口の方向」と「回り込んだぶんの
 * 距離減衰」に限り、周波数依存のこもりは**透過**だけに担当させる。
 * OFF にすると前川の帯域依存が生存ゲインに乗る ── 直線上に 0.6m 角の柱を 1 本置くだけで
 * 生存ゲインが 1.000 全帯域 → 0.895/0.883/0.834/0.774/0.679/0.584（傾き -3.7dB）になり、
 * 障害物があるだけで音色が変わる。 */
ACOUSTIC_API void AF_SceneSetDiffractionFlat(AF_SceneHandle scene, int flat);

/* 距離の近似（1=3-4-5 の13近傍・既定 / 0=市街地距離の3近傍）。
 * 0 は 0.15m 以下の細かい格子でだけ使うこと（粗いと戸口の幅の差が丸まって消える）。 */
ACOUSTIC_API void AF_SceneSetRoomChamferFull(AF_SceneHandle scene, int full);

/* 【開口】部屋どうしを繋ぐくびれ（戸口・窓・壊れた壁の穴）の数。面積の大きい順。
 * 壁で隔てられているだけの所には出ない（繋がっている所だけ）。 */
/* 【点のまわりの部屋の占め方】半径 radius(m) の球の中で各部屋が占める割合を返す。
 * outRooms/outWeights に大きい順に書き、書けた数を返す（割合の合計は 1）。
 * ★部屋を音に使うときは必ずこれを通すこと。部屋番号そのもので切り替えると、
 *   プレイヤーが必ず通る戸口のど真ん中に不連続を置くことになる。割合なら
 *   部屋の真ん中で 100:0、戸口で 50:50 と連続に変わる。
 *   radius は戸口の幅の 1〜2 倍が目安（そのぶんの距離をかけて入れ替わる）。 */
ACOUSTIC_API int  AF_SceneRoomWeights(AF_SceneHandle scene, AF_Vector3 p, float radius,
                                      int* outRooms, float* outWeights, int maxOut);

/* 【点のまわりの部屋の占め方・空間版】上と同じ球で測るが、「外の世界」も分母に入れる。
 * 合計は 1 以下で、残りが屋外の分。屋外との境目で連続に混ぜたい量（扉の定点の (1−w)、
 * 外へ出るときの残響の量）はこちらを使うこと。
 * ★上の AF_SceneRoomWeights は部屋どうしの割合（合計 1）なので、外へ開く戸口では
 *   球が部屋に触れた瞬間に 0→1 と跳ぶ（実測: 戸口の 1.8 m 手前で 1 歩に 0→1.00）。 */
/* 【部屋の中である割合】点のまわりの空間のうち、どこかの部屋の中である割合（0..1）。
 * 外へ出るときの**残響の量**はこれで縮めること（2026-09-05、不具合 #5）。
 * ⚠ V と RT60 を空間版で混ぜても量は連続にならない: 両方が同じ割合で縮むので臨界距離 rc = 0.057√(V/RT60)
 *   が動かず、球が最後の部屋を離れる 2 m 外で V が一気に 0 になる。V・RT60 は部屋どうしの割合のまま使い、
 *   量だけをこの割合で付ける（戸口の 1.3 m 手前 0.98 → 戸口 0.64 → 0.7 m 外 0.15 → 2 m 外 0）。 */
ACOUSTIC_API float AF_SceneRoomShareTotalAt(AF_SceneHandle scene, AF_Vector3 p, float radius);

ACOUSTIC_API int  AF_SceneRoomShareAt(AF_SceneHandle scene, AF_Vector3 p, float radius,
                                      int* outRooms, float* outWeights, int maxOut);

/* 【診断】生存ゲインの合成前の 2 つの担い手（6 帯域）。outSoft6 = 直接の半影（振幅）、
 * outDif6 = 回折（ポータル混合後）。生存 = 帯域ごとの max。歩行の走査で「どちらが跳んだか」を分ける用。 */
ACOUSTIC_API int  AF_SceneDebugSurvivalParts(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source,
                                             float* outSoft6, float* outDif6);

/* 点における「実効的な部屋の体積」(m3)。上の割合で混ぜたもの。0 なら部屋の外。
 * 残響量の土台（臨界距離 rc = 0.057√(V/RT60)）にそのまま入れられる。
 * レベル全体の外形箱を V に使うと、狭い部屋でも広間でも同じ値になって残響量が合わない。 */
ACOUSTIC_API float AF_SceneRoomVolumeAt(AF_SceneHandle scene, AF_Vector3 p, float radius);

/* 点における帯域別の残響時間(s)。out は 6 要素以上。書けた帯域数を返す。
 * 部屋ごとの Sabine 値（RT60 = 0.161 V / A）を上の占め方で混ぜたもの。
 * ★エコグラムから測ると (a) レイのばらつきが乗る (b)「-60dB を超える最後のビン」という
 *   離散インデックスになる (c) その床が直接音のピーク基準なので遮蔽で床ごと動く ──
 *   どれも位置に対して不連続。形と材質から出せば部屋ごとの定数になる。 */
ACOUSTIC_API int AF_SceneRt60At(AF_SceneHandle scene, AF_Vector3 p, float radius,
                                float* out, int count);

/* 部屋 room の音響量。各出力は null 可。成功で 1。
 *   outSurface : 境界の面積(m2)。開口を含む
 *   outOpenArea: そのうち開口ぶん(m2)
 *   outAbsorb6 : 平均吸音率（帯域別）
 *   outRt60_6  : 残響時間(s)（帯域別）
 * ★開口は吸音率 1 として数えている。そこから出た音はこの部屋に戻らないので、
 *   音響的には穴＝完全吸音。部屋どうしの結合が開口の面積として自動的に効く。 */
ACOUSTIC_API int AF_SceneRoomAcoustics(AF_SceneHandle scene, int room,
                                       float* outSurface, float* outOpenArea,
                                       float* outAbsorb6, float* outRt60_6);

ACOUSTIC_API int  AF_SceneApertureCount(AF_SceneHandle scene);

/* 開口 index の情報。各出力は null 可。成功で 1。
 *   outArea   : 断面積(m2)
 *   outCenter : 断面の重心
 *   outNormal : 面の向き（roomA → roomB が正）
 *   outRoomA / outRoomB : 繋いでいる部屋番号（roomA < roomB）
 * ★ここに出るのは**戸口**（開口の器）であって、扉の開き具合ではない。扉は動くものとして
 *   静的な塗り分けから外してある。開き具合は回折・透過の経路が連続量で出しているので、
 *   残響の結合ではそちらと組むこと（部屋番号で切り替えると境界で音が跳ねる）。 */
ACOUSTIC_API int  AF_SceneApertureInfo(AF_SceneHandle scene, int index,
                                       float* outArea,
                                       AF_Vector3* outCenter,
                                       AF_Vector3* outNormal,
                                       int* outRoomA, int* outRoomB);

/* 差分更新の粒度（ブロック一辺のボクセル数）。既定 16。変えると全再構築が走る。
 * 小さいほど更新は局所的になるが、走査線の区間が切り詰められて全再構築が遅くなる。 */
ACOUSTIC_API void AF_SceneSetRoomBrick(AF_SceneHandle scene, int voxels);

/* ★試して却下した案の記録: 稜線探索で見つけた開口を「開口率最大のポータル」として
 *   評価する（ゲイン = 開口の広さだけ。前川の δ も開口積分も掛けない）。
 *   隣室へ歩く境界の段差は 5.6dB → 2.7dB と良くなったが、**衝立で破綻した** ──
 *   幅の法則は「壁に空いた穴」の量で、有限の遮蔽物には定義されない。
 *   実測で影の中の回折が全部 0.000 になり、回帰が 2/151 FAIL。
 *   前川の δ は穴でも衝立でも定義されるので、そちらを土台に残すのが正しい。 */

/* 開口率の**幅**を開く指数（コントラスト）。1.0=素通し（既定）。大きいほど開閉の差が開く。
 * 開口率の形（1−cosθ のクレッシェンド、高域ほど大きく開く）は物理から出ているが、
 * 量が足りない（実測で扉の全掃引が 1.3dB）。形を保ったまま幅だけを開くための演出用。
 * 開口という一般の量への写像なので、扉を特別視しない。 */
ACOUSTIC_API void AF_SceneSetApertureContrast(AF_SceneHandle scene, float p);

/* 開口の**音色の広がり**だけを開く指数。1.0=素通し（既定）。
 *
 * AF_SceneSetApertureContrast が「開閉で**音量**がどれだけ動くか」なのに対し、
 * こちらは「開閉で**音色**がどれだけ動くか」。帯域平均を保つので音量には効かない。
 *
 *   f'[b] = m·(f[b]/m)^k   （m = 帯域平均。掛けたあと平均を元に戻す）
 *
 * これはエンジンであって作品ではないので、どれくらい芝居がかって聞こえるかは
 * ホストが決められないといけない。物理から出るのは形で、どれだけ誇張するかは演出。
 * 既定を 1.0（素通し）にしてあるのはそのため ── エンジンは形をそのまま出す。
 *   ホラー寄りに濃くしたいなら 1.5〜2.5 あたり。平らにしたいなら 1 未満。
 * 全帯域が同じ値のときは動かないので、「完全に閉じている／全開」は k で変わらない。 */
ACOUSTIC_API void AF_SceneSetApertureTimbre(AF_SceneHandle scene, float k);

/* BTM（有限楔の稜線積分）で回折の帯域ゲインを出す。0=従来（前川＋開口積分） / 1=BTM。既定 0。
 * ON では前川の δ 減衰も開口率も使わない（まとめて置き換わる。両方掛けると二重になる）。 */
ACOUSTIC_API void AF_SceneSetUseBtm(AF_SceneHandle scene, int on);

/* 直接経路の半影の作り方。
 *   3 = 走査線 ＋ **透過の空間の重みを帯域に依らせない**（既定、2026-09-08）。
 *       板は面として再放射するので、どの帯域も同じ面が担う。帯域ごとの核で重み付けると、
 *       低域のフレネル帯が幅 1 m の扉からはみ出して周りの壁を拾い、閉扉が材質より明るくなる
 *       （実測 5.6 dB 対 材質 12.0 dB）。開口（素通しの区間）は帯域ごとのままなので、
 *       「狭い隙間は高域から通る」は残る。
 *   2 = 窓の走査線積分（09-05。影の区間を解析的に積むので扉の隙間で跳ばない）
 *   1 = 帯域ごとのフレネル半径の環に標本点（09-02。4 点の環が階段になる）
 *   0 = 従来（音源まわり 0.4 m・8 点・帯域共通）
 * 同じビルドで聞き比べるための切り替え。採用が固まったら下側ごと消す予定。 */
ACOUSTIC_API void AF_SceneSetDirectPenumbra(AF_SceneHandle scene, int on);

/* 開口の法則。0 = 戸口の面への射影（既定）／
 * 1 = 戸口の面から**出ていった**遮蔽物は、出ていったぶん「蓋」でなくなる（帯域ごとのフレネル半径）／
 * 2 = 出ていった遮蔽物の**自由端と枠の間の弦**を開口幅とする（2026-09-08。帯域に依らない）。
 *   射影 (1−cosθ) は角度の 2 乗でしか開かないが、弦 2sin(θ/2) は角度に比例する。
 *   開いた瞬間が一番大きく、あとがなだらかになる形。閉じた扉では 0 と厳密に同じ、
 *   自由端が枠の幅まで離れた所（幅 1 m なら 60 度）から先は射影と一致する。
 *   ★1 を既定にしていたが、試聴で「高音域が抜けすぎ」となり 2026-09-08 に 0 へ差し戻した。
 *   狙い（開き始めの数度を効かせる）は達成していたが、同じ +10 dB が 8〜45 度の全域に
 *   乗り続け、扉が実際に居る角度の音色を支配していた。1 を活かすには高域の量を落とす必要がある。
 *   開いた扉は蝶番を軸に奥へ出ていくので、射影で測ると開口が角度の 2 乗でしか広がらず、
 *   最初の数度がまったく効かない（実測: 2 度で 0.6 mm）。1 では奥行きを帯域ごとのフレネル半径で
 *   量り、**高域から先に抜ける**。前提「隙間から高域だけが漏れる」を波長から出す形。
 *   実測（幅 1 m の扉・戸口の正面）: 8 度の開口 4 kHz が 0.016 → 0.053（+10 dB）、125 Hz はほぼ据え置き。
 *   閉じた扉（奥行き 0）では従来と厳密に同じ。 */
ACOUSTIC_API void AF_SceneSetApertureLaw(AF_SceneHandle scene, int law);

/* 開口の積分で δ（遠回り）をどれだけ効かせるか。1=そのまま / 0=効かせない。既定 1。
 * δ 減衰は前川の式として回折タップに既に掛かっているので、ここでも掛けると二重になる。
 * 切り分けの実測用。 */
ACOUSTIC_API void AF_SceneSetApertureDeltaWeight(AF_SceneHandle scene, float w);

ACOUSTIC_API void AF_SceneSetSlitBandSlope(AF_SceneHandle scene, float slope);

/* 【開口を面として鳴らす】開口を何点の二次音源に散らすか（1=点／2〜3=面）。既定 1。
 * 点1つだと戸口がピンポイントに聞こえて「直線的」になる。開口の広がりに沿って散らすと、
 * 点ごとに経路長と方向が違うので遅延とパンがばらけ、戸口サイズの面として聞こえる
 * （ホイヘンスの原理）。定位は全点が同じ開口の上にあるので失われない。 */
ACOUSTIC_API void AF_SceneSetApertureSpread(AF_SceneHandle scene, int points);
ACOUSTIC_API int AF_SceneGetApertureSpread(AF_SceneHandle scene);

/* 【診断】最有力の回折経路における実測の隙間幅(m)。回折の音量はこれで決まる。 */
ACOUSTIC_API float AF_SceneMeasureSlitWidth(AF_SceneHandle scene, AF_Vector3 listener, AF_Vector3 source);

/* 開口率(0..1)をそのまま測る（比較用。既定は無効）。 */
ACOUSTIC_API float AF_SceneMeasureApertureOpenness(AF_SceneHandle scene, AF_Vector3 listener,
                                                   AF_Vector3 source);

/* 【ソフト遮蔽】直接経路の透過(振幅・6帯域)と「どれだけ遮られているか(0..1)」を別々に返す。
 * outTrans(6要素以上) と outOccFrac(null可)。書き込んだ帯域数を返す。
 *
 * 音源まわりの円盤をサンプルするので、掠める位置では「一部だけ遮られる」状態がそのまま
 * 数値になる。単一レイの透過判定と違い、透過も遮蔽割合も**連続に動く**。
 * これを使うと「見通せているか」の二値分岐なしに直接音と回折を配分できる:
 *     直接タップ = outTrans          （見通しで 1.0 / 境界で約 0.5 / 影で材質の透過）
 *     回折タップ = 回折ゲイン × outOccFrac（見通しで 0 なので二重計上しない） */
ACOUSTIC_API int AF_SceneComputeSoftOcclusion(AF_SceneHandle scene,
                                              AF_Vector3 listener, AF_Vector3 source,
                                              float* outTrans, int count,
                                              float* outOccFrac);

/* 【診断】どこから最も多く漏れているか（透過で重み付けた遮蔽面上の重心）。
 * 壁より弱い扉があればそちらへ寄る。定位をここへ向けると「扉から漏れて聞こえる」になる。 */
ACOUSTIC_API void AF_SceneMeasureLeakPoint(AF_SceneHandle scene,
                                           AF_Vector3 listener, AF_Vector3 source,
                                           AF_Vector3* outPoint);

/* 【回折・キルヒホッフ版】開口の「大きさ」を実測してフレネル・キルヒホッフの解析解に渡す。
 * outGains(6要素以上) に帯域別ゲイン、outAperture に開口中心、outPathLength に実経路長。
 * 開口が見つかれば 1、面全体が塞がっていれば 0（呼び出し側で透過のみとする）。
 *
 * 前川の式は δ だけの関数なので開口の幅に反応できない（扉が回っても戸口の枠は動かないため
 * δ が変わらず、開き具合が piecewise constant になる）。こちらは面上の開いている範囲を
 * 直接測るので、開口幅・扉の開き具合・周波数依存がすべて同じ式から出る。 */
ACOUSTIC_API int AF_SceneComputeDiffractionKirchhoff(AF_SceneHandle scene,
                                                     AF_Vector3 listener, AF_Vector3 source,
                                                     float* outGains, int count,
                                                     AF_Vector3* outAperture,
                                                     float* outPathLength);

/* 【方向プローブ】origin から各方向へレイを飛ばし、「その方向からどれだけ残響が返るか」を
 * 帯域別に返す。outEnergy は dirCount*6 要素（方向ごとに 6 帯域が連続）。
 *
 * 後期残響は拡散なので時間構造はエコーグラムが持てばよく、足りないのは方向分布だけ。
 * これは音源に依存せずリスナー位置だけで決まるので、音源数が増えてもコストが増えない。
 *
 * 値は方向間の相対分布として使うこと（絶対値に意味は無い）。
 * 何にも当たらない方向は 0 になる（開けている＝残響を返さない）。 */
ACOUSTIC_API void AF_SceneProbeDirectionalEnergy(AF_SceneHandle scene,
                                                 AF_Vector3 origin,
                                                 const AF_Vector3* dirs, int dirCount,
                                                 int maxBounces,
                                                 float* outEnergy);

/* 【可視化】origin から dir 方向へ鏡面反射で maxBounces 回まで追った経路（通過点）を
 * outPoints に書き、その点数を返す。outPoints[0]=origin/以降=反射点/最後=終端。
 * outPoints は maxPoints 個以上（最低 maxBounces+2）。反響経路の線描画用。 */
ACOUSTIC_API int AF_SceneTraceReflectionPath(AF_SceneHandle scene,
                                             AF_Vector3 origin, AF_Vector3 dir,
                                             float maxDist, int maxBounces,
                                             AF_Vector3* outPoints, int maxPoints);

/* 【B: キューブマップ エッジカタログ】リスナー中心に res²×6面のレイを撒き、深度不連続で
 * シルエット稜線を拾ってカタログ化する。1回撒けば全音源で共有（リスナー係留）。 */
ACOUSTIC_API void AF_SceneBuildEdgeCatalog(AF_SceneHandle scene, AF_Vector3 listener,
                                           int res, float maxDist);
ACOUSTIC_API int AF_SceneEdgeCatalogCount(AF_SceneHandle scene);
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
/* リスナーの向き（前・上、世界座標）。タップの組み立てが到来方向をリスナー座標（+x 右／+y 上／+z 前）へ落とすのに要る。
 * 呼ばなければ +z 前・+y 上。 */
ACOUSTIC_API void AF_SceneSetListenerOrientation(AF_SceneHandle scene, AF_Vector3 forward, AF_Vector3 up);

/* ── タップの組み立て（2026-09-05、docs/TAP_BUILDER.md）──
 * ホストの BuildTapsForSource（1/r・空気吸収・パン・尾の比・平滑・HRTF の回折タップの選び方）を DLL へ移した。
 * ホストはつまみ（AF_TapParams）とリスナーの向きを渡し、更新のあと AF_SceneGetVoiceProgram で受け取って
 * 畳み込み器へ写すだけ。直接タップの透過は役割1 の半影（窓の走査線積分）＝生存と同じ模型（同じ問いに答えは 1 つ）。
 * ★尾の比は音源ごと（不具合 #4）。 */
typedef struct AF_TapParams {
    float distanceRef;              /* 1/r の基準距離(m)。0 で距離減衰なし。既定 1.5 */
    float diffractionDistanceRef;   /* 回折タップの基準距離。0 = distanceRef */
    float diffractionDistancePower; /* 回折タップの減衰の指数。既定 1 */
    float airAbsorptionScale;       /* 空気吸収の倍率。既定 1 */
    float transmissionTilt;         /* 透過のこもり（125 Hz 基準の傾きの指数）。既定 1 */
    float transmissionHighCutDb;    /* 透過の高域カット(dB @4 kHz)。既定 0 */
    float transmissionGainDb;       /* 透過の音量(dB)。既定 0 */
    float diffractionGainDb;        /* 回折タップの音量(dB)。既定 6 */
    int   diffractionDistanceOnly;  /* 1: 回折タップは方向と距離だけ（帯域は平坦）。既定 1 */
    float diffractionHighCutDb;     /* 回折の高域カット(dB @4 kHz)。既定 0 */
    float tapSmoothTime;            /* 透過・遮蔽割合の平滑（秒、dB 領域）。既定 0.08 */
    float directionSmoothTime;      /* 直接音の向きの平滑（秒）。既定 0.18 */
    float steerThreshold;           /* 遮蔽の深さがこれを超えたら向きを到来方向へ寄せ始める。既定 0.2 */
    int   diffractionHrtf;          /* 1: HRTF に回折タップを 1 本載せる。既定 1 */
    float diffractionHrtfMarginDb;  /* 乗り換えのヒステリシス(dB)。既定 2 */
    float reverbRatioExponent;      /* 尾の比の知覚圧縮（1 = 物理）。既定 0.5 */
    float reverbRatioCeiling;       /* 尾の比のソフトニーの天井（膝は 50）。既定 150 */
    float roomBlendRadius;          /* 部屋の混ぜ半径(m)。既定 2 */
    int   reverbShareFade;          /* 1: 外へ出るとき量を部屋の中である割合で縮める。既定 1 */
    float fallbackRt60;             /* 部屋が取れないときの RT60(s)。既定 0.5 */
    int   maxTaps;                  /* 音源 1 本のタップ上限（≤ 64）。既定 64 */
    int   diffractionTapReserve;    /* 回折タップのために末尾に空けておく数。既定 8 */
} AF_TapParams;
ACOUSTIC_API void AF_SceneSetTapParams(AF_SceneHandle scene, const AF_TapParams* params);

#define AF_PROGRAM_MAX_TAPS 64
typedef struct AF_ProgramTap {
    float delayMs;              /* 直接音を 0 とした相対遅延 */
    float gain6[6];             /* 6 帯域の振幅（1/r・空気吸収込み） */
    float panL, panR;           /* 等パワーのパン */
    float dirX, dirY, dirZ;     /* 到来方向（リスナー座標） */
    float arrX, arrY, arrZ;     /* 到来点（世界） */
    int   type;                 /* 0 直接／1 反射／2 回折 */
    float hrtfWeight;           /* HRTF に載せる割合（回折 1 本だけ 1） */
} AF_ProgramTap;
typedef struct AF_VoiceProgram {
    int   count;
    AF_ProgramTap taps[AF_PROGRAM_MAX_TAPS];
    float directDirX, directDirY, directDirZ;   /* 直接音の向き（リスナー座標、平滑後） */
    int   hrtfTapIndex;                         /* -1 = 無し */
    float hrtfDirX, hrtfDirY, hrtfDirZ;
    float itdgMs;                               /* 直接以外の最小遅延 */
    float sourceLevel;                          /* 生存の平均（反射込み）。尾の送出量 */
    float freeFieldDirect;                      /* 自由音場の直接レベル（遮蔽なし）。尾の校正基準 */
    int   tailShapeIndex;                       /* 尾の形の代表（AF_SceneGetTailShapeIndex と同じ） */
    float tailRatio;                            /* 尾の比（圧縮・天井・割合込み）★音源ごと */
    float tailRatioPhysical;                    /* 圧縮前 (r/rc)² */
    float mixingTimeMs;                         /* √V（5〜500） */
    float roomShare;                            /* 部屋の中である割合 */
    int   tier;                                 /* 段（0 厳密／1 簡易／2 バーチャル／3 保持） */
} AF_VoiceProgram;
/* 更新の答え。index は AF_SceneSourceIndex。非同期なら写し（1 フレーム前の入力）。範囲外は 0。 */
ACOUSTIC_API int AF_SceneGetVoiceProgram(AF_SceneHandle scene, int index, AF_VoiceProgram* out);

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

    /* 2026-09-03 早期反射の模型。0 = 像源をレイで拾う（旧）／1 = 面ごとの線音源（既定）。
     * earlyFaceSubTaps = 面 1 枚あたりの下位タップ数（1..8）。
     * echogramSkipFirstOrder = 1 で尾のエコグラムから 1 次反射を外す（早期反射のタップと二重に鳴らさない）。 */
    int   earlyModel;
    int   earlyFaceSubTaps;
    int   echogramSkipFirstOrder;
} AF_UpdateConfig;

/* 設定を渡す（変わったときだけでよい）。 */
ACOUSTIC_API void AF_SceneSetUpdateConfig(AF_SceneHandle scene, const AF_UpdateConfig* cfg);

/* 毎フレーム 1 回。内部レートに従って各役割を実行し、結果を内部バッファへ書く。 */
ACOUSTIC_API void AF_SceneUpdate(AF_SceneHandle scene, float dt);

/* ── 更新の非同期化（2026-09-04、docs/ASYNC_UPDATE.md）──
 * AF_SceneSetAsync(1) で AF_SceneUpdate は解かずに帰る（数 µs）。解くのは DLL のワーカースレッド。
 *   ・設定（SetListener / SetSource / UpdateInstance / SetUpdateConfig …）は待ち行列に入り、次の着手で順に効く。
 *   ・結果（GetSourceOcclusion / GetEarlyReflections / GetEchogramBands / 段 …）は写しから返る。
 *     写しは仕事が終わった次の AF_SceneUpdate で取り替わる ＝ **1 フレーム前の入力に対する答え**。
 *   ・幾何の問い合わせ（ComputeSoftOcclusion / RoomAt / DiffractionPath …）は解く段と並走する
 *     （作り直しの間だけ待つ）。
 *   ・構築・焼き・書き出し・キャプチャ操作は仕事を待ってから走る（毎フレーム呼ぶ物ではない）。
 * 仕事が 1 フレームに収まらないときは、その AF_SceneUpdate は何もしない（入力は溜まり、答えは古いまま）。
 * ⚠ ホストはシーンを破棄してから終わること（破棄でワーカーを待って畳む）。 */
ACOUSTIC_API void AF_SceneSetAsync(AF_SceneHandle scene, int enable);
ACOUSTIC_API int  AF_SceneIsAsync(AF_SceneHandle scene);
/* 走っている仕事を待ち、答えを写す（検査・「このフレームの答えが要る」ホスト用）。同期なら何もしない。 */
ACOUSTIC_API void AF_SceneAsyncWait(AF_SceneHandle scene);
/* 統計: 直近の仕事の計算時間(ms)、写しの古さ(フレーム)、追いつかず投げられなかったフレーム数、待ち行列の長さ。 */
ACOUSTIC_API void AF_SceneGetUpdateStats(AF_SceneHandle scene, float* outComputeMs, int* outLagFrames,
                                         int* outSkippedFrames, int* outQueued);

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

/* 焼く層（2026-09-03、3 層構造の第 1 段）: 静的な面のリストとセルごとの見通しを焼く。
 *   戻り値は焼いたセル数（0 なら焼けていない＝部屋グラフが無い／面が無い）。
 *   実行時は 焼いた見通し × 動いた物の遮蔽 × 今の位置と重み で面の線を出す。呼ばなければ生で解く。 */
ACOUSTIC_API int AF_SceneBakeStaticFaces(AF_SceneHandle scene, float cellSize, int subTaps);
ACOUSTIC_API void AF_SceneClearFaceBake(AF_SceneHandle scene);
ACOUSTIC_API int AF_SceneFaceBakeFaceCount(AF_SceneHandle scene);
/* 音響的に動く物（扉・門・車両）のタグ。焼く層に入れず、最初から実行時の遮蔽として扱う。焼き済みなら焼き直す。 */
ACOUSTIC_API void AF_SceneSetInstanceDynamic(AF_SceneHandle scene, int instanceId, int dynamic);

/* ── 外の走査器・別の作り手のための口（2026-09-03、GPU 化の下ごしらえ）──
 *   BVH（インスタンスの broad-phase）を平らな配列で書き出す。節は AABB / leftFirst / count。
 *   count > 0 が葉で、order[leftFirst .. leftFirst+count) のインスタンス添字を指す（走査の規則は DLL 内と同じ）。
 *   メッシュの三角形はまだ出さない（箱の OBB だけ）。 */
typedef struct AF_BvhNode { float minX, minY, minZ, maxX, maxY, maxZ; int leftFirst; int count; } AF_BvhNode;
typedef struct AF_InstanceDesc {
    AF_Vector3 center, halfExtents, axisX, axisY, axisZ;
    int materialId, geomId, active, moved, dynamicTag;
} AF_InstanceDesc;
ACOUSTIC_API int AF_SceneBvhNodeCount(AF_SceneHandle scene);
ACOUSTIC_API int AF_SceneBvhOrderCount(AF_SceneHandle scene);
/* 戻り値は書いた節の数。outNodes/outOrder は NULL 可（数だけ知りたいとき）。 */
ACOUSTIC_API int AF_SceneExportBvh(AF_SceneHandle scene, AF_BvhNode* outNodes, int maxNodes,
                                   int* outOrder, int maxOrder);
ACOUSTIC_API int AF_SceneGetInstance(AF_SceneHandle scene, int instanceId, AF_InstanceDesc* out);
/* 面の焼きの入れ物をバイト列で出し入れする（ファイル保存／別の作り手の結果の受け取り）。
 *   Bytes は 0 なら焼きが無い。Export は書いたバイト数（cap が足りなければ 0）。
 *   Import は 1=OK。版・大きさ・インスタンス数が合わなければ 0（焼いたときと実体の並びが違う）。 */
ACOUSTIC_API int AF_SceneFaceBakeBytes(AF_SceneHandle scene);
ACOUSTIC_API int AF_SceneFaceBakeExport(AF_SceneHandle scene, void* out, int cap);
ACOUSTIC_API int AF_SceneFaceBakeImport(AF_SceneHandle scene, const void* data, int size);

/* 回折二次音源。位置とゲインを書き、本数を返す。 */
ACOUSTIC_API int AF_SceneGetDiffractionSources(AF_SceneHandle scene, int index,
                                               AF_Vector3* outPos, float* outGain, int maxSrc);

/* 帯域別エコグラム。outBins[k*6 + b] に書き、書けたビン数を返す。 */
/* 帯域別エコグラム。音源 index のぶんを outBins[k*6 + b] に書き、書けたビン数を返す。
 * index に -1 を渡すと全音源の和（部屋全体の響きを見る用）。
 * ★音源ごとに持つ。以前は全音源を 1 本へ足していたので、響く部屋の音源と吸う部屋の
 *   音源が同じ尾で鳴っていた（実測: 減衰の形が 500ms で 9.0dB 違うのに 1 本へ潰れ、
 *   両方置くとどちらでもない中間になった）。レイ追跡はリスナーから 1 回で共有なので、
 *   分けても計算は増えない（増えるのはメモリだけ）。 */
ACOUSTIC_API int AF_SceneGetEchogramBands(AF_SceneHandle scene, int index,
                                          float* outBins, int numBins);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_SCENE_H */
