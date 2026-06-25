/* acoustic_engine.h
 * AcousticFlow の C API 窓口（公開ヘッダ）。
 *
 * 設計ルール：
 *   - C# / 他言語から呼べるよう extern "C" で名前マングリングを避ける。
 *   - 公開するのはプリミティブ型と POD 構造体のみ。STL / C++ クラスは出さない。
 *     （C++ のクラスはコンパイラ依存のメモリ配置を持つため境界を越えさせない）
 *   - インスタンスはハンドル（intptr_t 相当）で受け渡す（今後追加）。
 *
 * 現段階では境界疎通の確認用に関数を 1 つだけ公開している。
 */
#ifndef ACOUSTIC_ENGINE_H
#define ACOUSTIC_ENGINE_H

/* --- DLL エクスポート/インポートの切り替え ---
 * Windows では DLL の関数に「外から呼べる」印を付ける必要がある。
 *   DLL をビルドする側  : __declspec(dllexport)  … 関数を公開する
 *   DLL を利用する側    : __declspec(dllimport)  … 公開関数を取り込む
 * 同じヘッダを両者が使うので、ビルド側でだけ定義される
 * ACOUSTICENGINE_EXPORTS を見てマクロを切り替える。
 */
#ifdef _WIN32
  #ifdef ACOUSTICENGINE_EXPORTS
    #define ACOUSTIC_API __declspec(dllexport)
  #else
    #define ACOUSTIC_API __declspec(dllimport)
  #endif
#else
  #define ACOUSTIC_API   /* 非 Windows では不要 */
#endif

#ifdef __cplusplus
extern "C" {   /* C++ の名前マングリングを抑止し、C リンケージで公開する */
#endif

/* ===== 境界で受け渡す POD 構造体 ===== */

/* 3 次元ベクトル（境界用）。
 * float ×3 = 12 バイト、自然境界で詰まり(パディング)が入らない単純な配置。
 * C# 側は [StructLayout(LayoutKind.Sequential)] の float ×3 と一致させる。
 * Core 内部の acoustic::Vec3 とは意図的に別型にしている。
 */
typedef struct AF_Vector3 {
    float x;
    float y;
    float z;
} AF_Vector3;

/* エンジン実体への不透明ハンドル。
 * C++ クラスを境界に晒さず、ポインタだけを受け渡す。
 * C# 側は IntPtr として扱う。
 */
typedef void* AcousticEngineHandle;

/* ===== バージョン ===== */

/* エンジンのバージョン番号を返す（境界疎通の確認用）。
 * 形式: major * 10000 + minor * 100 + patch  例) 0.1.0 -> 100
 */
ACOUSTIC_API int AcousticEngine_GetVersion(void);

/* ===== エンジンの生成・破棄（ハンドル方式） ===== */

/* エンジン実体を生成し、そのハンドルを返す。失敗時は NULL。 */
ACOUSTIC_API AcousticEngineHandle AcousticEngine_Create(void);

/* エンジン実体を破棄する。生成したハンドルは必ずこれで解放する。 */
ACOUSTIC_API void AcousticEngine_Destroy(AcousticEngineHandle engine);

/* ===== ジオメトリ登録 & 可視性判定（ステップ3） ===== */

/* 障害物ボックスを中心+halfExtentsで追加する。 */
ACOUSTIC_API void AcousticEngine_AddBox(AcousticEngineHandle engine,
                                        AF_Vector3 center,
                                        AF_Vector3 halfExtents);

/* 登録済みの障害物をすべて消す。 */
ACOUSTIC_API void AcousticEngine_ClearGeometry(AcousticEngineHandle engine);

/* 2点 from->to が障害物で遮られているか。
 * 戻り値: 1 = 遮蔽あり / 0 = 見通せる
 *         (C には bool が無い文化なので int で返す)
 */
ACOUSTIC_API int AcousticEngine_IsOccluded(AcousticEngineHandle engine,
                                           AF_Vector3 from,
                                           AF_Vector3 to);

/* 材質ベースの遮蔽量を 0..1 で返す（1=ほぼ遮断 / 0=素通り）。
 * 直線上の壁の透過率（周波数帯別）の広帯域平均から算出。
 * 二値の IsOccluded と違い、壁の枚数・材質で連続的に変化する。
 */
ACOUSTIC_API float AcousticEngine_ComputeOcclusion(AcousticEngineHandle engine,
                                                   AF_Vector3 from,
                                                   AF_Vector3 to);

/* レイ積分による遮蔽量を 0..1 で返す（ComputeOcclusion の上位版）。
 * リスナー起点で numRays 本のレイを飛ばし、壁で反射して回り込む成分も数える。
 * 直線版と違い、直接が壁で塞がれても反射経路があれば 1.0 に張り付かず緩む。
 *   numRays    : 音用レイ本数（例 128〜256）。可視化レイとは別。
 *   maxBounces : 反射の最大回数（例 2〜3）。
 */
ACOUSTIC_API float AcousticEngine_ComputeOcclusionRayIntegrated(AcousticEngineHandle engine,
                                                                AF_Vector3 source,
                                                                AF_Vector3 listener,
                                                                int numRays,
                                                                int maxBounces);

/* 複数音源版の遮蔽（リスナーベースの共有パス）。リスナー起点のレイを numRays 本
 * だけ撒き（反射トレース＝raycast は音源数に依存しない）、各バウンス点から全音源へ
 * つなぐ。outOcc[j] に音源 j の遮蔽量(0..1)を書き込む。
 *   sources : 音源位置の配列（count 個）
 *   outOcc  : 出力（呼び出し側が count 個以上確保）
 * 多数音源でも raycast コストが増えない（next-event のみ音源数ぶん）。 */
ACOUSTIC_API void AcousticEngine_ComputeOcclusionMultiSource(AcousticEngineHandle engine,
                                                             AF_Vector3 listener,
                                                             const AF_Vector3* sources,
                                                             int count,
                                                             float* outOcc,
                                                             int numRays,
                                                             int maxBounces);

/* 【デバッグ/可視化用】from→to の直線が壁越しに各周波数帯をどれだけ通すか
 * （帯域別の透過ゲイン 0..1）を outGains に書き込む。
 *   帯域: 125, 250, 500, 1k, 2k, 4k Hz の順（計 6 個）。
 *   1=素通り / 0=完全遮断。occlusion スカラに潰す前の生の帯域値。
 * outGains は呼び出し側が 6 要素以上確保すること。count はその容量。
 * 実際に書き込んだ帯域数を返す（容量不足なら容量分だけ）。
 */
ACOUSTIC_API int AcousticEngine_ComputeTransmissionBands(AcousticEngineHandle engine,
                                                         AF_Vector3 from,
                                                         AF_Vector3 to,
                                                         float* outGains,
                                                         int count);

/* 【デバッグ/可視化用】origin から dir 方向へレイを飛ばし、最も近い障害物
 * までの距離を返す。ヒットしなければ -1 を返す。
 * dir は内部で正規化するので長さは任意でよい。
 * Unity でレイを線分描画してデバッグするのに使える。
 */
ACOUSTIC_API float AcousticEngine_DebugRaycast(AcousticEngineHandle engine,
                                               AF_Vector3 origin,
                                               AF_Vector3 dir,
                                               float maxDist);

/* origin から dir 方向にレイを飛ばし、壁で鏡面反射させながら最大 maxBounces
 * 回まで追った経路（通過点の列）を outPoints に書き込み、その個数を返す。
 *   outPoints は呼び出し側が確保した AF_Vector3 配列、maxPoints はその容量。
 *   容量は最低でも maxBounces+2 を渡すこと。
 * 反射経路の可視化（Unity）やレイトレの基盤に使う。
 */
ACOUSTIC_API int AcousticEngine_TraceReflectionPath(AcousticEngineHandle engine,
                                                    AF_Vector3 origin,
                                                    AF_Vector3 dir,
                                                    float maxDist,
                                                    int maxBounces,
                                                    AF_Vector3* outPoints,
                                                    int maxPoints);

/* リスナーに届くエネルギーを到達時間でビン分けしたエコグラム（残響可視化用）。
 * listener 起点のレイを numRays 本撒き、各バウンス点から sources[] へつなぎ、
 * 到達時間=経路長/speedOfSound に応じて outBins に広帯域エネルギーを積算する。
 *   outBins[k] : 時間 [k*binSeconds, (k+1)*binSeconds) に届く合計エネルギー
 *   outBins は numBins 個以上確保。直接音も該当ビンに含む。
 * 書き込んだビン数を返す（=numBins / 失敗時 0）。残響の尾の可視化に使う。 */
ACOUSTIC_API int AcousticEngine_ComputeEchogram(AcousticEngineHandle engine,
                                                AF_Vector3 listener,
                                                const AF_Vector3* sources,
                                                int count,
                                                int numRays,
                                                int maxBounces,
                                                float* outBins,
                                                int numBins,
                                                float binSeconds,
                                                float speedOfSound);

/* ===== 音源面（area source）の経路可視化 ===== */

/* 音源面の形状。Core 側の値（0=点/1=円/2=矩形）と一致させること。 */
typedef enum AF_SourceShape {
    AF_SOURCE_POINT = 0,  /* 点音源（面の広がり無し） */
    AF_SOURCE_DISK  = 1,  /* 円形の面（halfWidth=halfHeight=半径） */
    AF_SOURCE_RECT  = 2,  /* 矩形の面（halfWidth×halfHeight） */
} AF_SourceShape;

/* 音源面を cols×rows のグリッドに切り、各セル上の点 → リスナーへの
 * 「直接到達量」(0..1) を outGrid に row-major(r*cols+c) で書き込む。
 *   1=完全に届く / 0=壁で遮断 / -1=面の外（円形の角）。
 *   faceCenter : 面の中心
 *   faceNormal : 面の法線（音源の forward 相当。内部で正規化）
 *   faceRight  : 面内の横軸（音源の right 相当。内部で面内に射影・正規化）
 *   halfWidth/halfHeight : 面の半幅・半高（円なら両方とも半径）
 *   shape      : AF_SourceShape のいずれか
 * outGrid は呼び出し側が cols*rows 以上確保すること。書き込んだセル数を返す。
 * 「音源面のどこがリスナーに届くか」を別タブで可視化するために使う。 */
ACOUSTIC_API int AcousticEngine_ComputeFaceReachability(AcousticEngineHandle engine,
                                                        AF_Vector3 faceCenter,
                                                        AF_Vector3 faceNormal,
                                                        AF_Vector3 faceRight,
                                                        float halfWidth,
                                                        float halfHeight,
                                                        int shape,
                                                        AF_Vector3 listener,
                                                        float* outGrid,
                                                        int cols,
                                                        int rows);

/* ===== 音源指向性（directivity） ===== */

/* 指向性タイプ。Core 層の acoustic::DirectivityType と値を一致させること。
 * int として境界を越える（enum もコンパイラ間で int 互換なので安全）。
 */
typedef enum AF_DirectivityType {
    AF_DIRECTIVITY_OMNI          = 0,  /* 無指向 */
    AF_DIRECTIVITY_CARDIOID      = 1,  /* 前方主体 */
    AF_DIRECTIVITY_SUPERCARDIOID = 2,  /* 前方に鋭い */
    AF_DIRECTIVITY_BIDIRECTIONAL = 3,  /* 前後に放射 */
    AF_DIRECTIVITY_BEAM          = 4,  /* 強い前方ビーム */
} AF_DirectivityType;

/* 音源指向性の広帯域ゲイン(0..1)を返す（B プラン）。
 *   sourceForward : 音源の主放射方向（長さ任意。内部で正規化）
 *   directivityType: AF_DirectivityType のいずれか（範囲外は無指向扱い）
 *   outLowpass    : [出力] 背面/側面ほど大きい「こもり量」0..1。null 可。
 * エンジンハンドル不要（ジオメトリに依存しない純粋な幾何計算）。
 */
ACOUSTIC_API float AcousticEngine_ComputeDirectivity(AF_Vector3 sourcePos,
                                                     AF_Vector3 sourceForward,
                                                     AF_Vector3 listenerPos,
                                                     int directivityType,
                                                     float* outLowpass);

/* ===== 音声バックエンド(Wwise)のライフサイクル（ステップ4） =====
 * Wwise サウンドエンジンはグローバルなシングルトンなので、
 * エンジンハンドルとは独立したグローバル関数として公開する。
 */

/* このビルドが Wwise 連携を含むか。1 = 含む / 0 = スタブ。 */
ACOUSTIC_API int AcousticEngine_IsWwiseAvailable(void);

/* Wwise サウンドエンジンを初期化する。1 = 成功 / 0 = 失敗。 */
ACOUSTIC_API int AcousticEngine_InitAudio(void);

/* Wwise サウンドエンジンが初期化済みか。1 = 済 / 0 = 未。 */
ACOUSTIC_API int AcousticEngine_IsAudioInitialized(void);

/* Wwise サウンドエンジンを終了する。 */
ACOUSTIC_API void AcousticEngine_ShutdownAudio(void);

/* ===== 再生（バンク・ゲームオブジェクト・イベント・遮蔽） ===== */

/* サウンドバンクを探す基準フォルダ（UTF-8パス）を設定。1=成功/0=失敗。 */
ACOUSTIC_API int AcousticEngine_SetBankPath(const char* utf8Path);

/* バンクを名前で読み込む（例 "Init.bnk"）。1=成功/0=失敗。 */
ACOUSTIC_API int AcousticEngine_LoadBank(const char* bankName);

/* ゲームオブジェクト（音源/リスナーの実体）を登録/解除。 */
ACOUSTIC_API void AcousticEngine_RegisterGameObject(unsigned long long id, const char* name);
ACOUSTIC_API void AcousticEngine_UnregisterGameObject(unsigned long long id);

/* ゲームオブジェクトの位置と向き（前方/上方）を設定。 */
ACOUSTIC_API void AcousticEngine_SetGameObjectPosition(unsigned long long id,
                                                       AF_Vector3 position,
                                                       AF_Vector3 front,
                                                       AF_Vector3 top);

/* 既定のリスナーを設定。 */
ACOUSTIC_API void AcousticEngine_SetDefaultListener(unsigned long long id);

/* イベントを名前で再生。戻り値は playing ID（0=失敗）。 */
ACOUSTIC_API unsigned int AcousticEngine_PostEvent(const char* eventName,
                                                   unsigned long long gameObjectId);

/* イベントに Stop/Pause/Resume 等を実行（actionType: Stop=0 / Pause=1 / Resume=2）。
 * Pause/Resume は音源ボイスを止める/再開するが、バス側のリバーブの尾は鳴り続ける
 * ＝「音を止めて残響だけ残す」用途に使える。 */
ACOUSTIC_API void AcousticEngine_ExecuteActionOnEvent(const char* eventName, int actionType,
                                                      unsigned long long gameObjectId);

/* 遮蔽(occlusion)・障害(obstruction) を 0..1 で設定。 */
ACOUSTIC_API void AcousticEngine_SetObstructionOcclusion(unsigned long long emitterId,
                                                         unsigned long long listenerId,
                                                         float obstruction,
                                                         float occlusion);

/* 音源→リスナー間の出力バス音量(線形ゲイン)を設定。
 * 指向性の「向きによる音量」反映に使う（occlusion とは別経路）。 */
ACOUSTIC_API void AcousticEngine_SetEmitterListenerVolume(unsigned long long emitterId,
                                                          unsigned long long listenerId,
                                                          float volume);

/* Wwise の State を設定する（State グループ名・State 名、UTF-8）。
 * 空間化の切替（HRTF↔パンニング）などに使う。Wwise 側で State により出力
 * 経路（HRTF バス / パンニング バス）を切り替える前提のグローバル状態設定。 */
ACOUSTIC_API void AcousticEngine_SetState(const char* stateGroup, const char* state);

/* RTPC（リアルタイムパラメータ）をグローバルに設定する（UTF-8 名）。
 * 残響の wet量・減衰時間などをエンジンの計算値で Wwise に当てるのに使う。 */
ACOUSTIC_API void AcousticEngine_SetRTPCValue(const char* name, float value);

/* RTPC をゲームオブジェクト単位で設定する（UTF-8 名）。
 * 帯域別EQの Gain を音源ごとに独立駆動するなど、音源ごとに別値を当てる用途。 */
ACOUSTIC_API void AcousticEngine_SetRTPCValueOnObject(const char* name, float value,
                                                      unsigned long long gameObjectId);

/* 出力(マスターバス)の左右レベル(RMS, 線形 0..1程度)を取得する（メーター可視化用）。
 * outLeft / outRight に書き込む。未初期化・メータリング未対応なら 0。null 可。 */
ACOUSTIC_API void AcousticEngine_GetOutputLevels(float* outLeft, float* outRight);

/* 毎フレーム呼ぶ（溜まったイベント等を処理）。 */
ACOUSTIC_API void AcousticEngine_RenderAudio(void);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_ENGINE_H */
