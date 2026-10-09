/* afr_api.h — ゲームが呼ぶ口。ミドルウェアのランタイム側の C API。
 *
 * ★ここは**ゲームに載る側**です。道具（AfHost）ではありません。
 *   台帳もプロジェクトファイルも知りません（`AfRuntime` は `AfAuthoring` を link しない）。
 *
 * ★接頭辞が `AFR_` なのは、音響エンジンの `AF_` と**衝突するから**です。
 *   Bridge は両方を link します。同じ接頭辞のまま `AF_VoicePlay` のような名前を
 *   足すと、**リンクの段で初めて気づく**か、最悪気づかないまま片方が使われます。
 *   `AF_` ＝ 音響エンジン（AcousticFlow）／ `AFR_` ＝ そのランタイム（Runtime）。
 *
 * ── スレッド規約（関数ごとに書いてあります）
 *   [audio] … オーディオコールバックから。**確保しない・待たない・例外を投げない**
 *   [game]  … ゲームスレッドから。**積むだけで即戻る**
 *   [read]  … どこからでも読める。積まない
 *
 * ⚠ [game] の関数は**すぐには効きません**。次のブロックの頭で効きます。
 *   即座に効かせようとすると、オーディオスレッドと同じデータを同時に触ることになり、
 *   ロックが要る＝実時間制約を破ります。
 */
#ifndef AFR_API_H
#define AFR_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* ABI の版。**構造体を変えたら必ず上げること。**
 *
 * ⚠ 音響エンジン側で実際に事故りました ── DLL だけ古いまま C# を更新したときに
 *   構造体の長さが食い違い（44→48 バイト）、マーシャラが別の刻み幅で書き込んで
 *   **例外も出さずに中身が化けた**。原因に辿り着けません。
 *   1 : 最初の版
 */
#define AFR_ABI_VERSION 1
int AFR_AbiVersion(void);

/* ── 型 ───────────────────────────────────────────────────────── */

typedef struct AFR_System* AFR_SystemHandle;

/* 鳴っている音 1 本を指す札。0 は「無効」。
 *
 * ★中身は **枠の番号（下位 32 ビット）＋ 世代（上位 32 ビット）**です。
 *   ⚠ 枠の番号だけにすると、**使い回した枠を古い札が指します**。
 *     足音が鳴り終わって枠が別の音に回ったあと、古い札で Stop を呼ぶと
 *     **無関係な音が止まります**。世代を持てば、古い札はその場で弾けます。
 *   これは「オブジェクト破棄時のループ音が止まらない」の裏返しの事故で、
 *   ハンドルを扱うコードのバグの大半がここから出ます。
 */
typedef unsigned long long AFR_Voice;
#define AFR_VOICE_NONE 0ull

typedef struct AFR_Vector3 { float x, y, z; } AFR_Vector3;

typedef struct AFR_Config {
    int sampleRate;       /* 0 で 48000 */
    int channels;         /* 0 で 2 */
    int maxFrames;        /* 1 回の Render で来る最大サンプル数。0 で 4096 */
    int maxVoices;        /* 同時発音の枠。0 で 64 */
    int commandCapacity;  /* 積める指示の数。0 で 1024 */
} AFR_Config;

/* ── 生成と破棄 ───────────────────────────────────────────────── */

/* [game] 枠もキューも**ここで全部確保します**。
 *   ⚠ 以後 Render の中では 1 バイトも確保しません。それが実時間制約の担保です。 */
AFR_SystemHandle AFR_Create(const AFR_Config* cfg);
void             AFR_Destroy(AFR_SystemHandle sys);

/* ── 音を出す ─────────────────────────────────────────────────── */

/* [audio] 1 ブロックぶん書く。**必ず全部書きます**（前の残骸が残ると耳に出ます）。 */
void AFR_Render(AFR_SystemHandle sys, float* interleaved, int frames);

/* ── ゲームからの指示（積むだけ・即戻る）───────────────────────── */

/* [game] 鳴らす。返るのは札。⚠ **この時点ではまだ鳴っていません**（次のブロックから）。
 *   fadeInMs が 0 でも、内部で最低 5ms は掛かります ── 0ms で立ち上げると
 *   波形の途中から始まって必ずプチッと鳴るためです。 */
AFR_Voice AFR_Play(AFR_SystemHandle sys, unsigned soundId, float fadeInMs);

/* [game] 止める。⚠ **即停止はクリックの原因**なので、時間を指定します。
 *   0 を渡しても最低 5ms は掛かります。 */
void AFR_Stop  (AFR_SystemHandle sys, AFR_Voice v, float fadeOutMs);
void AFR_Pause (AFR_SystemHandle sys, AFR_Voice v, float fadeMs);
void AFR_Resume(AFR_SystemHandle sys, AFR_Voice v, float fadeMs);

/* [game] 全部止める。場面の切り替えで使います。 */
void AFR_StopAll(AFR_SystemHandle sys, float fadeOutMs);

/* ── パラメータ ───────────────────────────────────────────────── */

/* [game] ⚠ どれも**急変させません**。内部で数 ms ランプします
 *   （入れ忘れるとプチノイズが出る、というのは道具側で実際に踏みました）。 */
void AFR_SetGainDb(AFR_SystemHandle sys, AFR_Voice v, float db);
void AFR_SetPitch (AFR_SystemHandle sys, AFR_Voice v, float semitones);

/* ★★ 遮蔽は **6 帯域**で渡します（125/250/500/1k/2k/4kHz の減衰・dB・正の値）。
 *
 * ⚠ **ここをスカラー 1 個にすると、この作品の柱が死にます。**
 *   既存ミドルウェアの遮蔽は 0〜1 の 1 個で、角を回り込むときに**崖**ができます。
 *   実際の遮蔽は周波数で効き方が違う（低い音は回り込み、高い音は遮られる）ので、
 *   1 個の数では「扉がどれだけ開いているか」を表せません。
 *   音響エンジンのタップが `gain6[6]` を持っているのはそのためで、
 *   ここを合わせておかないと**エンジンの答えを受け取る器が無い**ことになります。
 *
 * ★ Push 型です。ミドルウェアはレイキャストを呼びません ──
 *   物理シーンへの問い合わせはスレッド安全でないことが多く、ロックと確保を伴うので、
 *   オーディオスレッドから呼ぶと実時間制約を破ります。**計算はゲーム側、こちらは受け取るだけ。**
 */
void AFR_SetOcclusionBands(AFR_SystemHandle sys, AFR_Voice v, const float bandsDb[6]);

/* [game] 位置と向き。距離減衰とパンニングに使います。 */
void AFR_SetVoicePosition(AFR_SystemHandle sys, AFR_Voice v, AFR_Vector3 p);
void AFR_SetListener(AFR_SystemHandle sys, AFR_Vector3 pos, AFR_Vector3 forward,
                     AFR_Vector3 up);

/* ── 状態を読む ───────────────────────────────────────────────── */

/* [read] その札がまだ生きているか。**古い札は 0 を返します**（世代で弾く）。
 *   ⚠ 「鳴り終わったか」を知る手段がないと、ハンドルを持ち続けて
 *     破棄時に止め忘れます。 */
int AFR_IsAlive(AFR_SystemHandle sys, AFR_Voice v);

/* [read] いま鳴っている本数と、枠の総数。診断用。 */
int AFR_PlayingCount(AFR_SystemHandle sys);
int AFR_VoiceCapacity(AFR_SystemHandle sys);

/* [read] 届かなかった指示の数。
 *
 * ★ここは**本当に失われた数**です。ゲームスレッドは止められないので、
 *   この口は積み直しません（1 回の失敗＝1 個の指示が届かなかった）。
 *   ⚠ 輪の側の `failedPushes()` とは意味が違います ── あちらは「積めなかった回数」で、
 *     積み直せば失われていません。**数える所と失われる所が違うので名前を分けています**
 *     （「再生 2」と出しながら 1 本しか鳴っていなかった件と同じ間違いを避けるため）。
 *
 * ⚠ 0 でないと「たまに音が出ない」という形でしか症状が出ません。だから外へ出します。 */
unsigned long long AFR_DroppedCommands(AFR_SystemHandle sys);

/* ══════════════════════════════════════════════════════════════════
 *  まだ無いもの（**枠だけ先に置かない**。中身が空いた口を並べると、
 *  呼べるのに効かない関数ができて、そちらのほうが害が大きい）
 *
 *    ・音の読み込みと参照カウント（`soundId` を誰が配るか）
 *    ・バス階層（SE / BGM / Voice → Master）と、尾の共有バスの受け口
 *    ・スペシャライザの挿し口（音響エンジンをここに挿す）
 *    ・RTPC / State / Switch
 *    ・コンテナ（Random / Switch / Sequence）とシード管理
 *    ・再生終了の通知（いまは AFR_IsAlive で問い合わせるだけ）
 *    ・ストリーミング
 * ══════════════════════════════════════════════════════════════════ */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AFR_API_H */
