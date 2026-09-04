/* acoustic_voice.h — 音源1本ぶんのレンダリング（DSP）の C API。
 *
 * 段4「DSP を C++ へ移行」で生えた窓口。それまで IR 畳み込みは Unity C# 側にあり、
 * エンジンは「幾何を計算してタップを返すだけ」だった。ここが入って初めて
 * **音を出すところまでエンジンの中で完結**する。
 *
 * 信号フロー（AcousticEngine/src/Dsp/voice_renderer.h）:
 *   dry(モノラル)
 *     ├─ 早期反射マルチタップ ──┬→ 鏡面 L/R
 *     │                          ├→ 直接音(モノ) → HRTF
 *     │                          └→ 拡散送り → allpass 拡散器
 *     └─ 後期尾(非一様分割畳み込み)
 *
 * スレッド規約:
 *   AF_VoiceRender()          … オーディオスレッドから。**確保・ロック・例外なし**
 *   それ以外                  … 制御スレッドから。差し替えは次のブロック境界で反映される
 */
#ifndef ACOUSTIC_VOICE_H
#define ACOUSTIC_VOICE_H

#include "acoustic_scene.h"   /* ACOUSTIC_API / AF_Vector3 */

#ifdef __cplusplus
extern "C" {
#endif

typedef void* AF_VoiceHandle;
typedef void* AF_HrtfHandle;

/* ── HRTF データセット（複数の音源で共有する）── */

/* .afhr を読み込む。失敗したら NULL。 */
ACOUSTIC_API AF_HrtfHandle AF_HrtfLoadFile(const char* path);
/* 実測データが無い環境向けの合成HRTF（球体頭モデル）。 */
ACOUSTIC_API AF_HrtfHandle AF_HrtfCreateSynthetic(int sampleRate);
ACOUSTIC_API void AF_HrtfDestroy(AF_HrtfHandle hrtf);
ACOUSTIC_API int AF_HrtfDirectionCount(AF_HrtfHandle hrtf);
/* 名前を outName へ書く（最大 maxLen-1 文字＋終端）。 */
ACOUSTIC_API void AF_HrtfGetName(AF_HrtfHandle hrtf, char* outName, int maxLen);

/* ── 音源 ── */

typedef struct AF_VoiceConfig {
    int   sampleRate;
    int   maxFrames;          /* 1回の Render で処理する最大サンプル数（0 で 4096） */
    float tailSeconds;        /* 後期尾の長さ（0 で 1.0） */
    float tapCrossfadeMs;     /* タップ差し替えのクロスフェード（0 で 30） */
    float hrtfCrossfadeMs;    /* 方向変化のクロスフェード（0 で 12） */
    float hrtfCrossoverHz;    /* これより下は HRIR を通さず ITD だけ（0 で 700） */
    int   tailFirstBlock;     /* 非一様分割の最小ブロック＝尾の遅延（0 で 64） */
    int   tailCapBlock;       /* 同・最大ブロック（0 で 8192） */
} AF_VoiceConfig;

/* 1タップ。エンジンが返すタップ（直接音／透過／回折／早期反射）をそのまま渡す。
 *   ★index 0 は必ず直接音にすること（HRTF はこれだけに掛かる）。
 *   gain6      : 125/250/500/1k/2k/4kHz の 6 帯域ゲイン
 *   panL/panR  : 等パワーパン（HRTF 有効時、index 0 では使われない）
 *   gSpec/gDiff: 鏡面 √(1-s) / 拡散 √s。AF_VoiceScatterSplit で作れる */
/* ABI の版。**AF_VoiceTap など C ABI の構造体を変えたら必ず上げること。**
 *
 * これが無いと、DLL だけ古いまま C# を更新したときに構造体の長さが食い違い、
 * マーシャラが別の刻み幅で書き込む（AF_VoiceTap は 44→48 バイトになった）。
 * 例外も出ずにタップの中身が化けるので、原因に辿り着けない。
 *   1 : hrtfWeight 追加 / AF_VoiceSetDiffractionDirection 追加（B1）
 *   2 : AF_SceneRoomGridDegraded 追加（格子の降格をホストが検知できるように）
 *   3 : ポータルの自動生成（AF_SceneSetAutoPortals ほか）／AF_SceneGetPortal 追加（C1）
  *   4 : AF_SceneSetApertureTimbre 追加（開口の音色を音量と独立に調整する口）
 *   5 : AF_VoiceTap に dir 追加（反射タップの軽量な両耳化）
 */
#define AF_ABI_VERSION 7   /* 2026-09-05: AF_TapParams / AF_VoiceProgram（タップの組み立てを DLL へ）。6 は AF_UpdateConfig の earlyModel 等 */
ACOUSTIC_API int AF_AbiVersion(void);

typedef struct AF_VoiceTap {
    int   delaySamples;
    float gain6[6];
    float panL, panR;
    float gSpec, gDiff;
    /* このタップを HRTF に載せる割合（0=パンのまま / 1=丸ごと HRTF）。
     * 到来方向は AF_VoiceSetDiffractionDirection で別に渡す。
     * 直接音(index 0)には効かない ── あちらは従来どおり AF_VoiceSetDirection の方向で
     * 常に HRTF を通る。★ホスト側の構造体（C# AFVoiceTap）と並びを合わせること。 */
    float hrtfWeight;
    /* 到来方向（リスナー座標系）。反射タップの**軽量な両耳化**（ITD ＋ 帯域別 ILD）に使う。
     * 全て 0 なら方向なしとみなし、従来どおり panL/panR で鳴る。
     * ★フル HRTF を通すのは直接音と hrtfWeight>0 のタップだけ。多数の反射に
     *   HRIR を畳み込むと重い（HRTF 1 本 +0.102ms/block に対し、こちらは 7 本で +0.043ms）。*/
    float dirX, dirY, dirZ;
} AF_VoiceTap;

/* 段別の計測（直前の Render ブロックの RMS）。内訳が読めないと調整できないので分けて返す。 */
typedef struct AF_VoiceMetering {
    float rmsDirect;
    float rmsEarly;
    float rmsScatter;
    float rmsTail;
    float rmsOut;
} AF_VoiceMetering;

ACOUSTIC_API AF_VoiceHandle AF_VoiceCreate(const AF_VoiceConfig* cfg);
ACOUSTIC_API void AF_VoiceDestroy(AF_VoiceHandle voice);

/* ── 尾の共有バス（段階②）─────────────────────────────────────────────
 * 後期残響の畳み込みを**音源で共有する**。
 *
 * ★近似ではない。畳み込みは線形なので、同じ IR に対して
 *     conv(IR, g1·x1) + conv(IR, g2·x2) = conv(IR, g1·x1 + g2·x2)
 *   が厳密に成り立つ。音源ごとのレベル g は足す前に掛けるので、
 *   **音源ごとの尾の量は保たれる**。減るのは畳み込みの回数だけ。
 *   実測: 音源 8 本で 1 ブロック 2.345 → 0.404 ms（**5.81 倍**）、出力の差は -123.7dB 下。
 *
 * ⚠ **同じ IR を使う音源だけ**を同じバスへ入れること（＝部屋ごとに 1 本）。
 *   違う部屋を混ぜると、片方の部屋の響きがもう片方に付く。
 * ⚠ 代表の音源（isOwner=1）を 1 本差すこと。1 本も差さないとバスに IR が入らず、
 *   **尾が丸ごと鳴らない**。全員を代表にすると、同じ IR で何度もクロスフェードが
 *   始まって 2 面ぶん畳むことになり、集約した意味が消える。
 *
 * ★呼ぶ場所（Unity）
 *   音源の OnAudioFilterRead が呼ばれる順は保証されないので、「全員が送ってから畳む」を
 *   素直に書けない。**AudioListener に付けたフィルタは全音源のミックス後に走る**ので、
 *   そこで AF_TailBusRender を 1 回呼ぶ。これなら遅延を足さずに順序が保証される。
 *   ⚠ 音源が 1 本も送っていないブロックでも呼ぶこと。畳み込み器の遅延線を進めないと、
 *     次に送りが来たときに尾が飛ぶ。 */
typedef void* AF_TailBusHandle;

ACOUSTIC_API AF_TailBusHandle AF_TailBusCreate(int sampleRate, float tailSeconds,
                                               int firstBlock, int capBlock, int maxFrames);
ACOUSTIC_API void AF_TailBusDestroy(AF_TailBusHandle bus);
ACOUSTIC_API void AF_TailBusSetCrossfadeMs(AF_TailBusHandle bus, float ms);
/* 溜まった送りを 1 回畳んで outL/outR へ**足す**（上書きしない）。 */
ACOUSTIC_API void AF_TailBusRender(AF_TailBusHandle bus, int frames, float* outL, float* outR);
/* 直前ブロックの尾の RMS（左）。バスに預けた音源の rmsTail は 0 になるので、計器はこちら。 */
ACOUSTIC_API float AF_TailBusRms(AF_TailBusHandle bus);
ACOUSTIC_API int AF_TailBusHasIr(AF_TailBusHandle bus);
/* 【オーディオスレッド】直近に Render したブロックの尾のモノラル（左右の平均）を out へ書く。
 * 扉の定点用: 隣の部屋のバスの出力を、戸口に置いた音源（別の AF_Voice）の入力にする。
 * Render と同じスレッドで、Render の後に呼ぶ。ホストは次のブロックで読めばよい
 * （1 ブロックの遅れは尾の立ち上がり 25ms より短い）。書けた数を返す。 */
ACOUSTIC_API int AF_TailBusLastMono(AF_TailBusHandle bus, float* out, int frames);

/* この音源の尾を共有バスへ預ける。bus=NULL で自前の畳み込みに戻る（既定）。 */
ACOUSTIC_API void AF_VoiceSetTailBus(AF_VoiceHandle voice, AF_TailBusHandle bus, int isOwner);

/* ── 方向バス（2026-09-04）──
 *   反射・回折のタップを 1 本ずつ両耳化せず、リスナー座標で固定した N 本のレーン（水平の環）へ隣り合う
 *   2 本の等パワーで振り、レーンごとに固定の HRIR で畳む。費用が O(タップ) → O(レーン) になり音源数に依らない。
 *   使い方は尾のバスと同じ: 音源は Render の中で送り、AudioListener のフィルタが Render を 1 回呼んで足す。
 *   lanes は 4〜16（8 = 45° 刻みが出発点、12 が反射の上限）。 */
typedef void* AF_DirectionBusHandle;
ACOUSTIC_API AF_DirectionBusHandle AF_DirectionBusCreate(int sampleRate, int lanes, int maxFrames);
/* 分割の最小ブロック（＝固有遅延）と最大ブロックを指定する版。既定は Create（64 / 1024）。 */
ACOUSTIC_API AF_DirectionBusHandle AF_DirectionBusCreateEx(int sampleRate, int lanes, int maxFrames, int firstBlock, int capBlock);
ACOUSTIC_API void AF_DirectionBusDestroy(AF_DirectionBusHandle bus);
/* レーンごとの固定 HRIR を焼く。hrtf は呼び手が生存を保証する。 */
ACOUSTIC_API void AF_DirectionBusSetHrtf(AF_DirectionBusHandle bus, AF_HrtfHandle hrtf, float headCircumferenceCm);
ACOUSTIC_API int  AF_DirectionBusHasHrtf(AF_DirectionBusHandle bus);
ACOUSTIC_API int  AF_DirectionBusLanes(AF_DirectionBusHandle bus);
/* 溜まった送りをレーンごとに畳んで outL/outR へ**足す**。音源が送っていないブロックでも呼ぶこと。 */
ACOUSTIC_API void AF_DirectionBusRender(AF_DirectionBusHandle bus, int frames, float* outL, float* outR);
ACOUSTIC_API float AF_DirectionBusRms(AF_DirectionBusHandle bus);
/* この音源の反射・回折タップを方向バスへ預ける。bus=NULL で自前の両耳化に戻る（既定）。次の SetTaps から効く。 */
ACOUSTIC_API void AF_VoiceSetDirectionBus(AF_VoiceHandle voice, AF_DirectionBusHandle bus);

/* タップ集合を差し替える。切替は次のブロックからクロスフェードして行われる。 */
ACOUSTIC_API void AF_VoiceSetTaps(AF_VoiceHandle voice, const AF_VoiceTap* taps, int count);

/* 遅延と mixing time から散乱率を鏡面/拡散へ振り分ける（タップを組むときの補助）。
 * ★mixing time は**頭打ち前の真の値**を渡すこと。畳み込みブロックの都合で下限を
 *   掛けた値を使うと、狭い部屋では実際の4倍以上に膨らんで散乱が効かなくなる。 */
ACOUSTIC_API void AF_VoiceScatterSplit(float delayMs, float mixingTimeMs, float scatterAmount,
                                       float timeGrowth, float* outSpec, float* outDiff);

/* HRTF。hrtf は voice より長生きさせること（共有前提で所有しない）。 */
ACOUSTIC_API void AF_VoiceSetHrtf(AF_VoiceHandle voice, AF_HrtfHandle hrtf);
ACOUSTIC_API void AF_VoiceSetHrtfEnabled(AF_VoiceHandle voice, int enabled);
/* 到来方向（リスナー座標系: +x=右, +y=上, +z=前）と頭囲(cm)。
 * ITD は頭のサイズにほぼ比例するので、ここが個人最適化で最も効く。 */
ACOUSTIC_API void AF_VoiceSetDirection(AF_VoiceHandle voice, AF_Vector3 dirListenerLocal,
                                       float headCircumferenceCm);

/* 回折バス（hrtfWeight>0 のタップ）の到来方向。直接音とは別方向を持つ。
 * 遮蔽されると直接音は材質の透過まで落ち、実エネルギーは開口を回り込んだ成分が運ぶ。
 * その成分に ITD と前後・上下の手がかりを載せるための口。 */
ACOUSTIC_API void AF_VoiceSetDiffractionDirection(AF_VoiceHandle voice,
                                                  AF_Vector3 dirListenerLocal,
                                                  float headCircumferenceCm);

/* 実測エコグラムから後期尾を組み直す。戻り値は 尾/直接 のエネルギー比（0 なら尾なし）。
 *   echoBands[k*6+b] : 時間ビン k・帯域 b のエネルギー
 *   startMs          : ここより前は早期反射が担当（二重計上を防ぐ）
 *   directGain       : 直接音タップの広帯域ゲイン（絶対レベルの基準）
 *   targetRatio      : 残響/直接エネルギーの目標比（呼び手が物理式から算出）
 *   earBandGain      : 耳ごと×6帯域のゲイン [c*6+b]（エネルギー比）。NULL で均一 */
ACOUSTIC_API float AF_VoiceRebuildTail(AF_VoiceHandle voice,
                                       const float* echoBands, int binCount, float binMs,
                                       float startMs, float fadeMs,
                                       float smoothMs, float smoothGrowth, float envAlpha,
                                       float directGain, float targetRatio,
                                       const float* earBandGain, int earBandGainLen);

/* 音量まわり。tailLevel は 1.0 が物理どおり（好みの微調整）。 */
ACOUSTIC_API void AF_VoiceSetOutputGain(AF_VoiceHandle voice, float gain);
ACOUSTIC_API void AF_VoiceSetTailLevel(AF_VoiceHandle voice, float level);
ACOUSTIC_API void AF_VoiceSetScatterDiffusion(AF_VoiceHandle voice, float g);
/* wet は開けた場所ほど小さく、srcLevel は壁裏で小さくなる（尾を絞る）。 */
ACOUSTIC_API void AF_VoiceSetTailEnvelope(AF_VoiceHandle voice, float wet, float srcLevel);

/* 反射タップの軽量な両耳化（ITD ＋ 帯域別 ILD）。既定 ON。
 * 切ると従来の等パワーパンに戻る（A/B 用）。
 * ★フル HRTF を通すのは直接音と hrtfWeight>0 のタップだけ。多数の反射に HRIR を
 *   畳み込むと重い（HRTF 1 本 +0.102ms/block に対し、こちらは 7 本で +0.043ms）。*/
ACOUSTIC_API void AF_VoiceSetEarCues(AF_VoiceHandle voice, int enabled);

/* 診断用。 */
ACOUSTIC_API int AF_VoiceTailPartitions(AF_VoiceHandle voice);
ACOUSTIC_API int AF_VoiceTailLatency(AF_VoiceHandle voice);

/* 【オーディオスレッド】モノラル入力 → ステレオ出力（上書き）。
 * outMetering は NULL 可。frames は内部で maxFrames に分割して回すので上限なし。 */
ACOUSTIC_API void AF_VoiceRender(AF_VoiceHandle voice, const float* input, int frames,
                                 float* outL, float* outR, AF_VoiceMetering* outMetering);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_VOICE_H */
