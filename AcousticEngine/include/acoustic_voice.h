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
typedef struct AF_VoiceTap {
    int   delaySamples;
    float gain6[6];
    float panL, panR;
    float gSpec, gDiff;
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
