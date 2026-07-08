/* Adapter/wwise_adapter.h
 * Wwise 連携層の「Wwise を含まない」インターフェース。
 *
 * ★重要な設計境界★
 *   このヘッダは Wwise SDK を一切 include しない。
 *   よって Export 層や Core 層はこのヘッダだけを見れば良く、
 *   Wwise の存在から完全に切り離される。
 *   実体は wwise_adapter.cpp（本物）か null_adapter.cpp（スタブ）が提供し、
 *   どちらをビルドするかは CMake が Wwise SDK の有無で切り替える。
 */
#ifndef ACOUSTICFLOW_ADAPTER_WWISE_ADAPTER_H
#define ACOUSTICFLOW_ADAPTER_WWISE_ADAPTER_H

namespace acoustic {
namespace adapter {

// Wwise サウンドエンジンを初期化する。成功なら true。
// （MemoryMgr -> StreamMgr -> SoundEngine -> MusicEngine の順で起動）
bool initAudio();

// 初期化済みかどうか。
bool isAudioInitialized();

// Wwise サウンドエンジンを終了する（初期化と逆順）。
void shutdownAudio();

// この実装が本物の Wwise 連携かどうか（true=本物 / false=スタブ）。
// Wwise 未導入ビルドかを呼び出し側が知るためのフラグ。
bool isWwiseAvailable();

// ===== 再生系（バンク読込・ゲームオブジェクト・イベント） =====
// 引数で Wwise 型を露出させないため、ID は unsigned long long、
// ベクトルは float で受け渡す（このヘッダを Wwise 非依存に保つ）。

// サウンドバンクを探す基準フォルダ（UTF-8パス）を設定する。
bool setBankPath(const char* utf8Path);

// バンクを名前で読み込む（例 "Init.bnk" / "Main.bnk"）。
bool loadBank(const char* bankName);

// ゲームオブジェクト（音源やリスナーの実体）を登録/解除する。
void registerGameObject(unsigned long long id, const char* name);
void unregisterGameObject(unsigned long long id);

// ゲームオブジェクトの位置と向き（前方/上方ベクトル）を設定する。
void setGameObjectPosition(unsigned long long id,
                           float px, float py, float pz,
                           float fx, float fy, float fz,
                           float tx, float ty, float tz);

// 既定のリスナーを設定する。
void setDefaultListener(unsigned long long id);

// イベントを名前で再生する。戻り値は playing ID（0=失敗）。
unsigned int postEvent(const char* eventName, unsigned long long gameObjectId);

// イベントに Stop/Pause/Resume 等を実行する（actionType: Stop=0/Pause=1/Resume=2）。
void executeActionOnEvent(const char* eventName, int actionType,
                          unsigned long long gameObjectId);

// 遮蔽(occlusion)・障害(obstruction) を 0..1 で設定する。
void setObstructionOcclusion(unsigned long long emitterId,
                             unsigned long long listenerId,
                             float obstruction, float occlusion);

// 音源→リスナー間の出力バス音量(線形ゲイン)を設定する。
// 指向性の「向きによる音量」を当てるのに使う（occlusion とは別経路）。
//   volume 1.0 = 等倍（無指向と同じ）/ 0 に近いほど小さい。
void setEmitterListenerVolume(unsigned long long emitterId,
                              unsigned long long listenerId,
                              float volume);

// Wwise の State を設定する（State グループ名・State 名で指定、UTF-8）。
// 空間化の切替（HRTF↔パンニング）などに使う。Wwise 側で State により
// 出力経路（HRTF バス / パンニング バス）を切り替える前提のグローバル状態。
void setState(const char* stateGroup, const char* state);

// RTPC（リアルタイムパラメータ）をグローバルに設定する。
// 残響の wet量・減衰時間などを、エンジンの計算値で Wwise エフェクトに当てるのに使う。
void setRTPCValue(const char* name, float value);

// RTPC をゲームオブジェクト単位で設定する。
// 音源ごとに別々の値を当てたい用途（帯域別EQの Gain を音源ごとに駆動するなど）。
void setRTPCValueOnObject(const char* name, float value, unsigned long long gameObjectId);

// 【早期反射(A)】emitterId の音源に、早期反射のイメージソースを AkReflect（auxBusName の
// aux バス）へ設定する。count 個の像源＝positions[i*3]=x,y,z（ワールド位置）＋levels[i]=線形ゲイン。
// 毎フレーム呼ぶ想定（先に既存を全消し→設定し直す）。auxBusName 空/未検出なら authoring 既定の
// reflections aux バスを使う。AK::SpatialAudio::SetImageSource 経由。
void setEarlyReflections(unsigned long long emitterId, const char* auxBusName,
                         const float* positions, const float* levels, int count);

// 出力(マスターバス)の左右レベル(RMS, 線形)を取得する（メーター可視化用）。
// メータリング未対応/未初期化なら 0 を返す。
void getOutputLevels(float* outLeft, float* outRight);

// 毎フレーム呼ぶ。キューに溜まったイベント等を処理する。
void renderAudio();

}  // namespace adapter
}  // namespace acoustic

#endif  // ACOUSTICFLOW_ADAPTER_WWISE_ADAPTER_H
