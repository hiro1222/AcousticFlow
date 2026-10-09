/* acoustic_host.h ── 外の器（Wwise のプラグインなど）が、音のスレッドから自前の DSP を回すための口（2026-09-29）
 *
 * ■ 全体の中の位置
 *   ゲームのスレッド（Unity の AcousticWorld / WorldVoice）→ **台帳**（ここ）→ 音のスレッド（Wwise のオブジェクト処理プラグイン）。
 *   今まで Unity の OnAudioFilterRead が呼んでいた「Voice で音にする」「共有の器を 1 回回す」を、外の器からも呼べるようにする。
 *   ★計算は増やさない。音源ごとの配分は今までどおりゲームのスレッドで Voice に置かれ、ここは「どの Voice が今どこか」を渡すだけ。
 *
 * ■ 役割
 *   ・ゲームのスレッドが毎フレーム、音源ごとの（鍵・Voice・位置）と、聞き手・共有の器（FDN・方向バス）を書いて公開する。
 *   ・音のスレッドは 1 ブロックに 1 回、写しを取る。届いた音の位置からいちばん近い Voice を引いて鳴らす。
 *   ・壊した Voice はすぐには消さず、1 秒おいてから壊す（音のスレッドが写しの中で握っている間は消せない）。
 *
 * ■ 中の仕組み
 *   公開は seqlock（書き手が番号を奇数にして書き、偶数に戻す。読み手は番号が偶数で前後一致したときだけ採る）。
 *   ★音のスレッドは待たない・確保しない・ロックしない。取れなかったブロックは前の写しをそのまま使う。
 *   位置は 2 通り持つ: 世界の座標と、聞き手から見た座標（右・上・前）。Wwise のオブジェクトの位置がどちらの座標で
 *   届くかを実機で確かめるため、読み手は両方で近さを比べ、当たった方を AF_HostReport で返す。
 *
 * ■ 壊れる所
 *   ・Voice を AF_VoiceDestroy で直に壊すと、音のスレッドが写しの中の古い手を使って落ちる。必ず AF_HostRetireVoice を通す。
 *   ・AF_FdnMixRender / AF_DirectionBusRender は 1 ブロックに 1 回だけ（Unity 側の TailBusRenderer と二重に回すと 2 倍鳴る）。
 */
#ifndef ACOUSTICFLOW_ACOUSTIC_HOST_H
#define ACOUSTICFLOW_ACOUSTIC_HOST_H

#include "acoustic_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AF_HOST_MAX_VOICES 256

/* 音源 1 本ぶん（写しの中身）。 */
typedef struct AF_HostVoice {
    int            key;          /* ゲーム側の鍵（emitter 番号など） */
    AF_VoiceHandle voice;
    float          x, y, z;      /* 世界の座標 */
    float          lx, ly, lz;   /* 聞き手から見た座標（+x 右 / +y 上 / +z 前） */
} AF_HostVoice;

/* 共有の器。どちらも NULL 可。 */
typedef struct AF_HostShared {
    AF_FdnMixHandle       fdn;
    AF_DirectionBusHandle bus;
    int                   sampleRate;   /* Voice を作った標本化周波数（外の器と合っているかの確かめ用） */
} AF_HostShared;

/* ── ゲームのスレッド ── */
/* 1 フレームの書き始め（台帳を空にする）。 */
ACOUSTIC_API void AF_HostBegin(void);
/* 音源を 1 本書く（世界の座標）。 */
ACOUSTIC_API void AF_HostPutVoice(int key, AF_VoiceHandle voice, float x, float y, float z);
/* 聞き手（位置・前・上）。聞き手から見た座標はこれで作る。 */
ACOUSTIC_API void AF_HostSetListener(float px, float py, float pz, float fx, float fy, float fz, float ux, float uy, float uz);
/* 共有の器。 */
ACOUSTIC_API void AF_HostSetShared(AF_FdnMixHandle fdn, AF_DirectionBusHandle bus, int sampleRate);
/* 位置の単位（1 m が何単位か）。Unity は 1、Unreal は 100（cm）。既定 1。
 * ★外の器（Wwise）は、ゲームが渡した座標をそのまま持つ。台帳にも同じ座標を書くので、「同じ音源とみなす距離」だけを
 *   この単位で換算する（Unreal で m のつもりの 0.5 を使うと 0.5 cm になり、ほぼ全部外れる）。
 * ★構造体（AF_HostShared）に足さず別の関数にしたのは、古いプラグインと大きさが食い違って書き越すのを避けるため。 */
ACOUSTIC_API void  AF_HostSetUnitsPerMeter(float unitsPerMeter);
ACOUSTIC_API float AF_HostUnitsPerMeter(void);
/* 書いた物を公開する（音のスレッドから見えるようになる）。 */
ACOUSTIC_API void AF_HostCommit(void);
/* Voice を引退させる（1 秒後に AF_VoiceDestroy）。AF_HostTick が壊す。 */
ACOUSTIC_API void AF_HostRetireVoice(AF_VoiceHandle voice);
/* HRTF も同じく 1 秒後に壊す（Voice が握っているので Voice より先に壊してはいけない。同じ Tick では Voice を先に壊す）。 */
ACOUSTIC_API void AF_HostRetireHrtf(AF_HrtfHandle hrtf);
/* 引退した Voice のうち 1 秒経った物を壊す。毎フレーム呼ぶ。 */
ACOUSTIC_API void AF_HostTick(void);
/* 外の器が鳴らしているか（音のスレッドから AF_HostReport が最近 0.5 秒以内に届いたか）。 */
ACOUSTIC_API int  AF_HostActive(void);

/* ── 音のスレッド ── */
/* 写しを取る。書いた数を返す（取れなければ前の写しの数を返さず -1。そのときは前の写しを使うこと）。 */
ACOUSTIC_API int  AF_HostSnapshot(AF_HostVoice* out, int maxOut, AF_HostShared* outShared);
/* 対応付けの結果を返す（表示用）。space: 0 世界 / 1 聞き手から見た座標。meanErr は当たった物の距離の平均（m）。 */
ACOUSTIC_API void AF_HostReport(int matched, int missed, int space, float meanErr);
/* 音の大きさの計器（2026-09-30、試聴で「対応は取れているのに鳴らない」を切り分けるため）。
 * instances: 生きているプラグインの実体の数。inRms: 受けた音（Wwise の音量を掛ける前、モノラル）。
 * gain: Wwise の音量（最初の音源の cumulativeGain）。outRms: 出した音（左右の平均）。どれも回した実体の最後のブロック。 */
ACOUSTIC_API void AF_HostReportLevels(int instances, float inRms, float gain, float outRms);

/* ── 表示 ── */
ACOUSTIC_API void AF_HostStats(int* outMatched, int* outMissed, int* outSpace, float* outMeanErr, int* outBlocks);
ACOUSTIC_API void AF_HostLevels(int* outInstances, float* outInRms, float* outGain, float* outOutRms);
/* 対応付けの累計（2026-10-01）。AF_HostStats は最後のブロックだけなので、途中で外れたブロックが見えない。
 * 報告のたびに足し込む。ゲームは前回との差で「この 1 秒に何ブロック外れたか」を出す。maxErr は前回読んでからの最大（読むと 0 に戻す）。 */
ACOUSTIC_API void AF_HostTotals(long long* outMatched, long long* outMissed, float* outMaxErr);

/* ── 左右の計器（2026-10-05）──
 *   音のスレッド（Wwise のプラグイン）が 1 ブロックごとに、左右の RMS とピークを段ごとに書く。別の窓（tools/af_meter.py）が読んでグラフにする。
 *   段: 0 全体（出口）／1 直接と反射（Voice の和）／2 戸口の響き（FDN の直出し）／3 方向バス（反射・響きのレーン）。
 *   置き場は名前付きの共有メモリ "Local\AcousticFlowMeterV1"（AF_METER_SHM_NAME）。輪になった AF_METER_CAPACITY 個の欄に、
 *   書いた数 writeCount（64 bit）を最後に進めて公開する。読み手は writeCount から新しい欄だけ読む。
 *   ★共有メモリはゲームのスレッドの AF_HostCommit で作る（最初の 1 回だけ）。音のスレッドは作らない・待たない・確保しない。
 *     作られる前の AF_HostMeterWrite は何もしない。
 *   形（ビューアと同じ並び。変えたら版を上げる）:
 *     先頭: magic "AFMETER1"(8) / version u32 / capacity u32 / stages u32 / reserved u32 / writeCount i64
 *     欄:   timeSec f64（steady_clock の秒）/ frames i32 / sampleRate i32 / rmsL[4] rmsR[4] peakL[4] peakR[4]（f32）＝ 80 バイト */
#define AF_METER_STAGES   4
#define AF_METER_CAPACITY 2048
#define AF_METER_SHM_NAME "Local\\AcousticFlowMeterV1"
typedef struct AF_HostMeterBlock {
    int   frames, sampleRate;
    float rmsL[AF_METER_STAGES], rmsR[AF_METER_STAGES];
    float peakL[AF_METER_STAGES], peakR[AF_METER_STAGES];
} AF_HostMeterBlock;
ACOUSTIC_API void AF_HostMeterWrite(const AF_HostMeterBlock* block);
/* 共有メモリが作られているか（0/1）。検査と診断用。 */
ACOUSTIC_API int  AF_HostMeterReady(void);

#ifdef __cplusplus
}
#endif

#endif  /* ACOUSTICFLOW_ACOUSTIC_HOST_H */
