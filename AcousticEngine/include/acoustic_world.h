/* acoustic_world.h ── 新コア（Flow）の C API（段 4）
 *
 * 世界 1 つ = 面（箱）＋材質＋部屋（自動）＋リスナー＋音源。毎フレーム AF_WorldUpdate を 1 回。
 * 音は AF_Voice*（acoustic_voice.h、残す側）が作る。配分は AF_WorldApplyVoice で音源の Voice に写す。
 * 後期の残響は AF_FdnMix*（同）の器 1 つを AF_WorldBindFdn で結ぶ。器の Render は AudioListener 側で 1 回。
 *
 * 単位: 位置は m、角度は度、材質の係数はエネルギー比（0..1）。座標は Unity と同じ左手系。
 * ★C# は「音の面の操作」だけを持つ。数値（エネルギー・振幅）はこの API の内側で閉じる。
 */
#ifndef ACOUSTICFLOW_ACOUSTIC_WORLD_H
#define ACOUSTICFLOW_ACOUSTIC_WORLD_H

#include "acoustic_scene.h"   /* ACOUSTIC_API / AF_Vector3 */
#include "acoustic_voice.h"   /* AF_VoiceHandle / AF_FdnMixHandle */

#ifdef __cplusplus
extern "C" {
#endif

typedef void* AF_WorldHandle;

ACOUSTIC_API AF_WorldHandle AF_WorldCreate(void);
ACOUSTIC_API void AF_WorldDestroy(AF_WorldHandle w);

/* ── 材質（帯域 6 の配列。NULL なら既定の壁）。戻りは材質番号。 ── */
ACOUSTIC_API int AF_WorldAddMaterial(AF_WorldHandle w, const float* transmission6, const float* absorption6, const float* scattering6);
/* プリセット番号で足す（AF_MaterialPresetBands と同じ番号: 1 コンクリート 2 ガラス 3 不透過 4 木の扉 5 板張り 6 石 7 洞窟 8 雪。0 既定） */
ACOUSTIC_API int AF_WorldAddMaterialPreset(AF_WorldHandle w, int preset);

/* ── 面（箱）。axisX/axisY は正規直交、axisZ は外積で出す。dynamic=1 は動く物（部屋グラフと焼きから外す） ── */
ACOUSTIC_API int  AF_WorldAddBox(AF_WorldHandle w, AF_Vector3 center, AF_Vector3 halfExtents,
                                 AF_Vector3 axisX, AF_Vector3 axisY, int material, int dynamic);
ACOUSTIC_API void AF_WorldSetBoxTransform(AF_WorldHandle w, int box, AF_Vector3 center, AF_Vector3 axisX, AF_Vector3 axisY);
ACOUSTIC_API void AF_WorldSetBoxActive(AF_WorldHandle w, int box, int active);

/* ── 部屋（部屋グラフから自動。build は Update が必要時に呼ぶが、明示もできる） ── */
ACOUSTIC_API void AF_WorldBuild(AF_WorldHandle w);
ACOUSTIC_API int  AF_WorldRoomCount(AF_WorldHandle w);
ACOUSTIC_API int  AF_WorldBuildCount(AF_WorldHandle w);
ACOUSTIC_API int  AF_WorldRoomAt(AF_WorldHandle w, AF_Vector3 p);
/* 部屋のプローブ: RT60（6）、体積、表面積、平均自由行程。成功で 1 */
ACOUSTIC_API int  AF_WorldRoomInfo(AF_WorldHandle w, int room, float* outRt60_6, float* outVolume, float* outSurface, float* outMeanFreePath);

/* ── リスナーと音源 ── */
ACOUSTIC_API void AF_WorldSetListener(AF_WorldHandle w, AF_Vector3 pos, AF_Vector3 forward, AF_Vector3 up);
ACOUSTIC_API int  AF_WorldAddEmitter(AF_WorldHandle w, AF_Vector3 pos, float radius);
ACOUSTIC_API void AF_WorldRemoveEmitter(AF_WorldHandle w, int emitter);
ACOUSTIC_API void AF_WorldSetEmitter(AF_WorldHandle w, int emitter, AF_Vector3 pos, float radius, int operated, float loudness);

/* ── 設定 ── */
ACOUSTIC_API void AF_WorldSetRays(AF_WorldHandle w, int raysPerEmitter, int maxBounces);
ACOUSTIC_API void AF_WorldSetWeights(AF_WorldHandle w, const float* w5);          /* 直接・初期・後期・回折・透過。NULL で全部 1 */
ACOUSTIC_API void AF_WorldSetResponse(AF_WorldHandle w, float levelSec, float colourSec, float statSec, float directionSec);
ACOUSTIC_API void AF_WorldSetHeadCm(AF_WorldHandle w, float headCircumferenceCm);

/* ── 1 フレーム ── */
ACOUSTIC_API void AF_WorldUpdate(AF_WorldHandle w, float dt);

/* ── 音へ ── */
/* 後期の器を結ぶ。器は AF_FdnMixCreate で作り、AudioListener 側で AF_FdnMixRender する。部屋は世界が足す。 */
ACOUSTIC_API void AF_WorldBindFdn(AF_WorldHandle w, AF_FdnMixHandle fdn);
/* 部屋グラフを作り直したので器を結び直す必要がある（1）。ホストは新しい器を AF_FdnMixCreate で作り、
 * AF_WorldBindFdn と各 Voice の AF_VoiceSetFdnMix を新しい器へ向け、古い器は 1 秒おいて AF_FdnMixDestroy。 */
ACOUSTIC_API int  AF_WorldFdnStale(AF_WorldHandle w);
/* 音源の配分を Voice に写す（毎フレーム、Update の後）。Voice は AF_VoiceSetFdnMix で同じ器に繋いでおく。 */
ACOUSTIC_API void AF_WorldApplyVoice(AF_WorldHandle w, int emitter, AF_VoiceHandle voice, int sampleRate);

/* ── 情報（AF ツールの情報タブ用）。エネルギーは 音源の出力 1 に対する 1/m²。 ── */
typedef struct AF_MixInfo {
    float energy6[6];          /* 総量（生） */
    float component6[5][6];    /* 五成分の内訳（生）: 直接・初期・後期・回折・透過 */
    float onsetSec;            /* 尾の開始（絶対） */
    float directSec;           /* 直接音の到達 */
    int   tapCount, sendCount;
    int   room;                /* 音源の部屋（−1 外） */
    int   directCrossings;     /* 直線が横切った壁の枚数 */
    float firstReflectSec;
    int   raysTraced, hits;
} AF_MixInfo;
ACOUSTIC_API int AF_WorldMixInfo(AF_WorldHandle w, int emitter, AF_MixInfo* out);

#ifdef __cplusplus
}
#endif

#endif  /* ACOUSTICFLOW_ACOUSTIC_WORLD_H */
