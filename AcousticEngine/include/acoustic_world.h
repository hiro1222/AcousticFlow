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
/* 実行中の材質の調整（2026-09-12）。箱の材質を差し替える／材質の中身を書き換える（プリセット番号でも可）。
 * 静的な箱に効く変更は次の Update で部屋グラフ（RT60）と ISM の面を組み直す（1 回、数百 ms）。
 * 動く箱（扉の板）だけに効く変更は組み直さない（透過・吸音・散乱はそのフレームから効く）。
 * 戻り値は 1 で書けた（範囲外なら 0）。★調整で毎フレーム AddMaterial を呼ぶと表が伸び続けるので、必ずこちらで。 */
ACOUSTIC_API void AF_WorldSetBoxMaterial(AF_WorldHandle w, int box, int material);
ACOUSTIC_API int  AF_WorldUpdateMaterial(AF_WorldHandle w, int material, const float* transmission6, const float* absorption6, const float* scattering6);
ACOUSTIC_API int  AF_WorldUpdateMaterialPreset(AF_WorldHandle w, int material, int preset);
ACOUSTIC_API int  AF_WorldMaterialCount(AF_WorldHandle w);

/* ── 面（箱）。axisX/axisY は正規直交、axisZ は外積で出す。dynamic=1 は動く物（部屋グラフと焼きから外す） ── */
ACOUSTIC_API int  AF_WorldAddBox(AF_WorldHandle w, AF_Vector3 center, AF_Vector3 halfExtents,
                                 AF_Vector3 axisX, AF_Vector3 axisY, int material, int dynamic);
ACOUSTIC_API void AF_WorldSetBoxTransform(AF_WorldHandle w, int box, AF_Vector3 center, AF_Vector3 axisX, AF_Vector3 axisY);
ACOUSTIC_API void AF_WorldSetBoxActive(AF_WorldHandle w, int box, int active);

/* ── 部屋（部屋グラフから自動。build は Update が必要時に呼ぶが、明示もできる） ── */
/* 部屋グラフのボクセル一辺(m)。AF_WorldBuild の**前**に置く。既定 0.25。
 *   ★戸口の幅を数ボクセルで割れる大きさにすること。粗いと戸口で部屋が割れず、
 *     2 部屋が 1 部屋に潰れる（扉を閉めても響きが変わらなくなる）。 */
ACOUSTIC_API void AF_WorldSetRoomCell(AF_WorldHandle w, float meters);
/* 要求した一辺と、実際に使われた一辺、総ボクセル数と上限。NULL 可。
 *   ★要求 ≠ 実際 なら、上限に収めるために粗くされている＝音の結果が変わっている。 */
ACOUSTIC_API void AF_WorldRoomCellInfo(AF_WorldHandle w, float* outRequested, float* outEffective,
                                       double* outVoxels, double* outMaxVoxels);

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
/* 予算（段 8）。totalRays = 0 で無制限（音源ごとに raysPerEmitter）。
 * fullSlots / lightSlots は 厳密 / 簡易 の枠。漏れた音源は「保持」（最後の答えを保つ。素通しではない）。
 * 昇格 0.5 s / 降格 1.0 s のヒステリシスと、保持を順繰りに解き直す探り（既定 1 本/フレーム）は中で持つ。 */
ACOUSTIC_API void AF_WorldSetBudget(AF_WorldHandle w, int totalRays, int fullSlots, int lightSlots, int probesPerFrame);
/* レイ更新のフレーム分散（設計文書 Ⅶ）。rayGroups 個の組に分け、毎フレーム 1 組だけ飛ばす（1 で分散なし、上限 8）。
 * 組ごとの結果は幾何が同じなら同じなので、静止していれば合計は一定＝揺れない。動けば rayGroups フレームで入れ替わる。 */
ACOUSTIC_API void AF_WorldSetRayGroups(AF_WorldHandle w, int rayGroups);
/* 1 音源の本数の上限（既定 512）。予算がいくら余っていても、これより多くは飛ばさない。
 * ★GPU（AF_WorldSetGpuTrace）で本数を桁で増やすときに上げる。raysPerEmitter だけ上げても、ここで頭打ちになる。
 * 下限は簡易の下限（32）、上限は 65536（1 回のディスパッチの出力が 4 GB を超えないため）。 */
ACOUSTIC_API void AF_WorldSetMaxRaysPerEmitter(AF_WorldHandle w, int maxRays);
/* 音源ごとのループを複数コアへ。workers <= 1 で直列（既定）。★Unity は既に全コアを使うので明示のときだけ。 */
ACOUSTIC_API void AF_WorldSetWorkers(AF_WorldHandle w, int workers);
/* 今フレームに実際に飛ばしたレイの総数（費用の目安）。 */
ACOUSTIC_API int  AF_WorldSpentRays(AF_WorldHandle w);
/* 音源の段（0 厳密 / 1 簡易 / 2 保持）と本数。 */
ACOUSTIC_API int  AF_WorldEmitterTier(AF_WorldHandle w, int emitter);
ACOUSTIC_API int  AF_WorldEmitterRays(AF_WorldHandle w, int emitter);
ACOUSTIC_API void AF_WorldSetWeights(AF_WorldHandle w, const float* w5);          /* 直接・初期・後期・回折・透過。NULL で全部 1 */
ACOUSTIC_API void AF_WorldSetResponse(AF_WorldHandle w, float levelSec, float colourSec, float statSec, float directionSec);
ACOUSTIC_API void AF_WorldSetHeadCm(AF_WorldHandle w, float headCircumferenceCm);
/* 閉じた扉から漏れる回折の扱い。**既定 1（案A）**。
 *   0 旧（作り物の漏れが出る）/ 1 案A（既定。閉扉だけ効き、10° 以降は 0 と同じ）
 *   2 案B（半開きの回折まで下がり、戸口の方向が消える）/ 3 両方
 *   ★どちらも角度に二値を置かない。1 は経路の幾何、2 は口の帳簿。2 は戸口が要る。 */
ACOUSTIC_API void AF_WorldSetLeakModel(AF_WorldHandle w, int model);
/* 隣の部屋の後期の鳴らし方（段 2-f / 2-g、既定 2）。実行中に切り替えられる（試聴の A/B）。
 *   0 旧（後期を丸ごと耳の部屋の FDN へ一様に）
 *   1 戸口越しの面から来た分を音源の部屋の FDN へ送り、その FDN を戸口の向き（レーンの点）で聞く
 *   2 戸口の線音源: 音源の部屋の尾を戸口の横幅に並べた 5 点から HRTF で鳴らし、耳の部屋の分のうち
 *     戸口から入った分（× 戸口の開き具合）を戸口の線音源から耳の部屋の FDN へ流す
 * 同じ部屋の音源はどれでも変わらない。2 でも戸口で繋がっていない部屋は 1 の形になる。 */
ACOUSTIC_API void AF_WorldSetLateThrough(AF_WorldHandle w, int on);
ACOUSTIC_API int  AF_WorldLateThrough(AF_WorldHandle w);
/* 戸口寄せ（0..1、既定 0）。lateThrough=2 のとき、耳の部屋へ流す分のうちこの割合を戸口の線音源から直接鳴らす。総量は変えない。
 * 実行中に動かしてよい。 */
ACOUSTIC_API void  AF_WorldSetDoorPull(AF_WorldHandle w, float pull);
ACOUSTIC_API float AF_WorldDoorPull(AF_WorldHandle w);
/* 戸口の線音源の低域の相関の境（Hz、既定 3000）。この境より下は 5 点が同じ波形、上は点ごとに別の波形。0 で旧（全帯域を別々に）。
 * 近づいて 5 点が広い角度に散ったときの定位のため。実行中に動かしてよい。 */
ACOUSTIC_API void  AF_WorldSetDoorCoherence(AF_WorldHandle w, float hz);
ACOUSTIC_API float AF_WorldDoorCoherence(AF_WorldHandle w);
/* 先着の重み（既定 6 dB / 窓 0.04 s）。最初の到達（直接の到達時刻）から遅れる到来ほど出口の量を下げる。帳簿は物理のまま。
 * 重み = 10^(−db/10 · (1 − e^(−Δ/sec)))。0 dB で今までと同じ。実行中に動かしてよい。 */
ACOUSTIC_API void AF_WorldSetPrecedence(AF_WorldHandle w, float db, float sec);
/* 壁越しの反射（既定 0 ＝ 通さない）。1 で旧: 壁を横切った反射と残響も透過率で薄めて届ける。
 * 0 では壁を抜けるのは透過の直接音だけ。開いた戸口を通る分はどちらでも同じ。実行中に切り替えてよい。 */
ACOUSTIC_API void AF_WorldSetWallReflect(AF_WorldHandle w, int on);
/* 初期反射の出し方（既定 3）。実行中に切り替えてよい。
 *   0 虚像（ISM）／1 壁の受取面（面ごとのタップ）／2 虚像を面でつなぐ（虚像の面が受けたレイの量を虚像へ）
 *   3 虚像の網（受け取りごとの見かけの音源の点を、虚像を結んだ網で受けて角の虚像へ配る） */
ACOUSTIC_API void AF_WorldSetEarlyModel(AF_WorldHandle w, int model);
/* 音源ごとの上書き。初期反射の出し方（−1 で世界の設定、0/1/2/3）と、隣の部屋の閉じ込め（負で世界の設定、0..1）。 */
ACOUSTIC_API void AF_WorldSetEmitterEarlyModel(AF_WorldHandle w, int emitter, int model);
ACOUSTIC_API void AF_WorldSetEmitterAdjacentContain(AF_WorldHandle w, int emitter, float amount);
/* 隣の部屋の閉じ込め（0..1、既定 0）。音源が耳と別の部屋にいるとき、耳の部屋で響かせる分（後期の耳の部屋の FDN への送りと
 * 戸口から流す分、初期の耳の部屋の面の反射）をこの割合だけ戸口へ移す。総量は変えない。1 で隣の部屋の音は戸口からだけ鳴る。実行中に動かしてよい。 */
ACOUSTIC_API void  AF_WorldSetAdjacentContain(AF_WorldHandle w, float amount);
ACOUSTIC_API float AF_WorldAdjacentContain(AF_WorldHandle w);
ACOUSTIC_API int  AF_WorldEarlyModel(AF_WorldHandle w);
ACOUSTIC_API int  AF_WorldWallReflect(AF_WorldHandle w);
/* 尾のレーンの作り（既定 1）。実行中に切り替えられる（試聴の A/B）。
 *   0 耳ごとの行（点の向きでも左右が別の波形。ITD が効かず、向きが変わると尾の波形が入れ替わる）
 *   1 点と拡散を分ける（点は 1 本の波形＋点の向きの ITD。自室＝広がり 1 は 0 と 1 ビットも同じ）
 * 効くのはレーン（方向バス）を通る尾だけ。戸口の線音源（AF_WorldSetLateThrough の 2）の部屋は通らない。 */
ACOUSTIC_API void AF_WorldSetLaneModel(AF_WorldHandle w, int model);
ACOUSTIC_API int  AF_WorldLaneModel(AF_WorldHandle w);
/* レイを GPU で解くか（0 切／1 入。既定 0）。
 *   ★音は作らない。GPU が出すのは幾何と統計だけで、音にするのはエンジン（CPU）。
 *   ★ホストのデバイスは借りない。エンジンが自前で持つ。
 *   ★GPU が無い機械や積めない場合は黙って CPU のまま動く。実際に使えているかは
 *     AF_WorldGpuActive で見る（1 を置いても 0 が返ることがある）。 */
ACOUSTIC_API void AF_WorldSetGpuTrace(AF_WorldHandle w, int on);
ACOUSTIC_API int  AF_WorldGpuActive(AF_WorldHandle w);
/* GPU の名前（診断）。使えないときは失敗の理由が入る。戻り値は書いた文字数。 */
ACOUSTIC_API int  AF_WorldGpuInfo(AF_WorldHandle w, char* buf, int bufBytes);
ACOUSTIC_API int  AF_WorldLeakModel(AF_WorldHandle w);

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
    float visibleFraction;     /* 見通しの割合 0..1（段 5 aperture） */
    int   shadowers;           /* 影を落とした箱の数 */
    int   imageCount;          /* 有効な虚像の数（段 7 ISM） */
    int   imageCandidates;     /* 検討した虚像の数 */
} AF_MixInfo;
/* 戸口の数と、戸口 i の素通しの割合（1 − 板の覆い。Update の後に読む）。 */
ACOUSTIC_API int   AF_WorldApertureCount(AF_WorldHandle w);
ACOUSTIC_API float AF_WorldApertureOpenFrac(AF_WorldHandle w, int aperture);
ACOUSTIC_API int AF_WorldMixInfo(AF_WorldHandle w, int emitter, AF_MixInfo* out);

/* 聞こえている音の到来（AF ツールの配分タブ用、2026-09-12）。
 *   量は配分の出口（平滑 × 重み ＝ 耳に届く量）。帯域の平均エネルギーで、音源の出力 1 に対して 1/m²。
 *   向きはリスナー座標（+x 右 / +y 上 / +z 前）。全方向から来る分は向き 0・広がり 1。
 *   後期は FDN の送りを分けて出す: 耳の部屋の響き／戸口から直接（戸口の線音源の点）／戸口から流した響き／戸口の向きの点。
 *   書いた数を返す（maxOut で打ち切る）。 */
enum {
    AF_ARRIVAL_DIRECT = 0,          /* 直接 */
    AF_ARRIVAL_EARLY = 1,           /* 初期（虚像。向きあり） */
    AF_ARRIVAL_EARLY_DIFFUSE = 2,   /* 初期（方向なしの残り） */
    AF_ARRIVAL_DIFFRACT = 3,        /* 回折 */
    AF_ARRIVAL_TRANSMIT = 4,        /* 透過 */
    AF_ARRIVAL_LATE_ROOM = 5,       /* 後期・耳の部屋の響き（全方向） */
    AF_ARRIVAL_LATE_DOOR = 6,       /* 後期・戸口から直接（戸口の線音源の点。段 2-g） */
    AF_ARRIVAL_LATE_DOOR_FEED = 7,  /* 後期・戸口から流した響き（耳の部屋で全方向に鳴る。戸口の音から立ち上がる） */
    AF_ARRIVAL_LATE_POINT = 8       /* 後期・戸口の向きの点（案1。段 2-f） */
};
typedef struct AF_Arrival {
    int   kind;          /* 上の AF_ARRIVAL_* */
    int   emitter;
    float dirLocal[3];   /* リスナー座標。全方向なら 0 */
    float spread;        /* 0 点 … 1 一様 */
    float energy;        /* 耳に届く量（帯域の平均エネルギー） */
    float delaySec;      /* 到達（絶対。後期は尾の開始） */
    float origin[3];     /* 出どころ（world、地図用）: 直接・透過は音源、回折は稜線の点、初期（虚像）は耳の側の壁の反射点、
                            戸口から直接は戸口の点、戸口の向きの点（案1）は戸口の中心 */
    int   hasOrigin;     /* origin が意味を持つか（全方向の分は 0） */
    int   box;           /* 初期（虚像）が耳の側で返った箱の番号（無ければ −1）。地図で壁を色付けする */
} AF_Arrival;
ACOUSTIC_API int AF_WorldArrivals(AF_WorldHandle w, int emitter, AF_Arrival* out, int maxOut);

/* 地図用の形（AF ツールの配分タブ、2026-09-12）。エンジンが使っている箱と戸口をそのまま出す。 */
typedef struct AF_BoxInfo {
    float center[3], halfExtents[3], axisX[3], axisY[3], axisZ[3];
    int   dynamic, active;
} AF_BoxInfo;
ACOUSTIC_API int AF_WorldBoxCount(AF_WorldHandle w);
ACOUSTIC_API int AF_WorldBoxInfo(AF_WorldHandle w, int box, AF_BoxInfo* out);
typedef struct AF_ApertureInfo {
    float center[3], axisU[3], axisV[3];   /* 外接矩形の中心と 2 軸 */
    float halfU, halfV, openFrac;           /* 半幅と素通しの割合 */
    int   roomA, roomB;
} AF_ApertureInfo;
ACOUSTIC_API int AF_WorldApertureInfo(AF_WorldHandle w, int aperture, AF_ApertureInfo* out);

/* 回折の中身（診断）。旧コアの AF_SceneDebugDiffractionPath にあたる。
 *   ★valid が 0 になる原因は 3 つあって、区別できないと追えない:
 *     ① 見通しが 1（遮られていない ＝ 回折は不要）
 *     ② 影を落とす箱が無い（候補が空）
 *     ③ 候補はあったが脚が他の箱に潰されて重み 0（扉の板が経路を塞いだ、など）
 *   weight と shadowers を並べて読めば、どれかが分かる。 */
typedef struct AF_DiffractionInfo {
    int   valid;
    int   box, edge;           /* 回った箱と稜線の番号。−1 は無し */
    float delta;               /* 迂回長 δ(m) */
    float gapWidth;            /* 隙間の幅 a(m)。段 9-a のフレネル開口 */
    float weight;              /* 脚の貫通による重み 0..1（1 = 脚が完全に通る） */
    float pathSec;             /* 到達（秒） */
    float energy6[6];          /* 帯域別のエネルギー比（前川 × 隙間の通り） */
    float gapOpen6[6];         /* 隙間の通り 0..1（帯域別） */
    float point[3];            /* 回折点（world） */
    float dirLocal[3];         /* リスナー座標の到来方向 */
} AF_DiffractionInfo;
ACOUSTIC_API int AF_WorldDiffractionInfo(AF_WorldHandle w, int emitter, AF_DiffractionInfo* out);

#ifdef __cplusplus
}
#endif

#endif  /* ACOUSTICFLOW_ACOUSTIC_WORLD_H */
