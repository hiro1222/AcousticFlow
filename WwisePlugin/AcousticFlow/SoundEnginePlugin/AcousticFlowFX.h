/* AcousticFlowFX.h ── Wwise のバスに載せる、自前のエンジンの鳴らし口（オブジェクト処理・場外型）
 *
 * ■ 全体の中の位置
 *   ゲーム（Unity の AcousticWorld / WorldVoice）が自前のエンジンで配分を解き、音源ごとの Voice に置く
 *     → 台帳（acoustic_host.h）に「どの Voice が今どこか」を書く
 *     → **ここ**: Wwise のバス「AcousticFlow」に届いた音を 1 本ずつ受け、位置のいちばん近い Voice で両耳の音にして足し、
 *        部屋ごとの FDN と方向バスを 1 回だけ回して足し、ステレオ 1 本で出す
 *     → Wwise のその先のバス（出力）へ。
 *   ★Unity の OnAudioFilterRead（WorldVoice と TailBusRenderer）が今やっている「音にする」所を、Wwise の中へ移した物。
 *     計算は増やしていない。配分はゲームのスレッドで今までどおり解かれている。
 *
 * ■ 役割
 *   Wwise を「鳴らす器」にする。何本受けても出すのは両耳の 2 ch だけ（自前の HRTF で立体化済み。Wwise に二重に立体化させない）。
 *
 * ■ 中の仕組み
 *   Init:    出力をステレオの普通の形に決める（io_rFormat.channelConfig）。こうすると出力のオブジェクトは Init の時点で
 *            決まった 1 本になり、Execute で作り直さずに済む。エンジンの関数は AcousticEngine.dll から名前で引く。
 *   Execute: 係か確かめる（1 ブロックに回すのは実体 1 つだけ）→ 台帳の写しを取る
 *            → 届いた音ごとに「モノラルに畳む → Voice を引く（覚えている結び付き、無ければ位置で）→ AF_VoiceRender → 足す」
 *            → FDN → 方向バス（この順。FDN は方向バスへ送るので先に回す）→ 対応付けの結果を台帳へ返す。
 *            段ごと（Voice の和・FDN の直出し・方向バス・全体）の左右の RMS とピークを、左右の計器（AF_HostMeterWrite）へ書く。
 *   係:      Wwise はバスの実体を聞き手の数だけ作る。どの実体も同じ Voice・FDN・方向バス（ゲームに 1 組）を回すので、
 *            Wwise のブロック番号（GetBufferTick）で見張り、1 ブロックに 1 つだけが回す。音を受けている実体が優先
 *            （受けていない実体は、誰も音を受けていないときだけ回して響きの尾を進める）。
 *
 * ■ 繋がり
 *   受ける: Wwise のバスに届いた音（AkAudioObjects。位置は AkAudioObject::positioning.threeD.xform）。
 *           自前のエンジンの台帳（AF_HostSnapshot）。
 *   渡す:   Wwise へステレオ 1 本。台帳へ対応付けの結果（AF_HostReport、AF ツールで見る）。
 *
 * ■ 退けた書き方
 *   ・エンジンの .lib をリンクする: Wwise の画面（Authoring）で試聴するときにエンジンの DLL が無いと、プラグインごと
 *     読み込めなくなる。名前で引いて、無ければ素通し（モノラルを左右へ）にする。
 *   ・Wwise の RTPC で音源の番号を渡す: Wwise 側の仕込みが要り、自前の仕組みに Wwise の役割が混ざる（発注者の指示）。
 *     位置は Wwise が必ず持っているので、位置で引けば Wwise 側に何も置かなくてよい。
 *
 *   ・毎ブロック位置で引き直す（2026-10-01 まで）: 位置は聞き手から見た座標で届き、ゲームの台帳と Wwise では数フレームずれる。
 *     頭を回す・速く歩くと 0.5 m を超えて外れ、そのブロックだけ素通し（こもりも減衰も無い生の音）で鳴って「飛ぶ」。
 *     Wwise の音の通し番号（AkAudioObject::key、音が鳴っている間は変わらない）で結び付きを覚え、最初の 1 回だけ位置で引く。
 *   ・実体ごとに全部回す（2026-09-30 まで）: Unreal のエディタが聞き手を 2 つ持っていてバスの実体が 2 つになり
 *     （呼ばれる回数 188 回/秒 ＝ 1 つの 2 倍）、同じ器が 1 ブロックに 2 回進んで反射が千切れ（ツブ・びりびり）、
 *     響きが重なり、Wwise の並列処理で 2 つが同時に同じ器を触って落ちた。
 *   ・最初に来た実体を固定の係にする（同日の 1 回目）: 無音になった。音を受けない実体（聞き手を絞る前のビューポート側）が
 *     係に居座り、音を受けた実体は係でないので黙っていた。係はブロックごとに決め、音を受けている実体を優先する。
 *
 * ■ 壊れる所
 *   ・回さない実体は無音を出す。音源が 2 人の聞き手に届くと、ブロックごとにどちらかの実体から出る（足せば連続）。
 *     2 人の聞き手の先の処理（音量など）が違うと、ブロックごとに行き来して千切れる。聞き手はゲームで 1 人に絞る。
 *   ・別々の聞き手にだけ届く音源が 2 本あると、そのブロックで回らない方の音源は鳴らない（1 ブロックに回すのは 1 つ）。
 *   ・同じバスに 2 段（直列）挿すと、2 段目は「このブロックはもう回った」で無音を出し、1 段目の音を消す（2026-09-30、
 *     tools/wwise_setup.py が枠を付け足していて実際に起きた）。見張りが無かった頃は 2 段目が器をもう 1 回回して千切れていた。
 *     バスの枠は 1 つ。ゲームの画面の「plugin instances」が 1 であること。
 *   ・音が 5 秒届かない実体は「もう鳴らない」（AK_NoMoreData）を返して片付けてもらう。ずっと「鳴っている」と返すと、
 *     Play を止めても前の実体が残り、2 回目の Play で回す権利を取り合って無音になった（2026-10-03、ClaimBlock の注記）。
 *   ・Unity 側の TailBusRenderer も回していると同じく二重になる（AcousticWorld の出口を Wwise にしたら止まる作り）。
 *   ・Voice を作った標本化周波数が Wwise と違うと、遅れと ITD がずれる（AF_HostShared.sampleRate で確かめる）。
 */
#ifndef AcousticFlowFX_H
#define AcousticFlowFX_H

#include "AcousticFlowFXParams.h"
#include "acoustic_host.h"

class AcousticFlowFX
    : public AK::IAkOutOfPlaceObjectPlugin
{
public:
    AcousticFlowFX();
    ~AcousticFlowFX();

    AKRESULT Init(AK::IAkPluginMemAlloc* in_pAllocator, AK::IAkEffectPluginContext* in_pContext, AK::IAkPluginParam* in_pParams, AkAudioFormat& io_rFormat) override;
    AKRESULT Term(AK::IAkPluginMemAlloc* in_pAllocator) override;
    AKRESULT Reset() override;
    AKRESULT GetPluginInfo(AkPluginInfo& out_rPluginInfo) override;
    void Execute(const AkAudioObjects& in_objects, const AkAudioObjects& out_objects) override;

private:
    // エンジンの関数（AcousticEngine.dll から名前で引く。無ければ nullptr ＝ 素通し）
    struct Engine {
        decltype(&AF_HostSnapshot)       snapshot = nullptr;
        decltype(&AF_HostReport)         report = nullptr;
        decltype(&AF_VoiceRender)        voiceRender = nullptr;
        decltype(&AF_FdnMixRender)       fdnRender = nullptr;
        decltype(&AF_DirectionBusRender) busRender = nullptr;
        decltype(&AF_HostUnitsPerMeter)  unitsPerMeter = nullptr;   // 無くてもよい（古いエンジン）。無ければ 1 m ＝ 1
        decltype(&AF_HostReportLevels)   reportLevels = nullptr;    // 無くてもよい。音の大きさの計器
        decltype(&AF_HostMeterWrite)     meterWrite = nullptr;      // 無くてもよい。左右の計器（別の窓 tools/af_meter.py が読む）
        bool ok() const { return snapshot && report && voiceRender && fdnRender && busRender; }
    };
    void BindEngine();
    // このブロックを回してよいか（1 ブロックに 1 つだけ true。音を受けている実体が優先）。
    bool ClaimBlock(bool hasObjects);
    // 位置からいちばん近い Voice を引く。space: 0 世界 / 1 聞き手から見た座標。見つからなければ -1。
    int FindVoice(float x, float y, float z, int space, float& outDist) const;
    // 覚えている結び付き（Wwise の音の通し番号 → 台帳の Voice の鍵）。一度位置で当たったら、あとは位置で引き直さない。
    int VoiceIndexOfKey(int voiceKey) const;
    struct Bind { AkAudioObjectID obj; int voiceKey; AkUInt64 lastBlock; };
    static const int kMaxBinds = 64;
    Bind m_binds[kMaxBinds];
    int m_bindCount;
    AkUInt64 m_blockCount;                       // この実体が回したブロックの数（結び付きの掃除に使う）
    int m_idleBlocks;                            // 音が届かなかったブロックが続いた数（5 秒で Wwise に片付けてもらう）

    AcousticFlowFXParams* m_pParams;
    AK::IAkPluginMemAlloc* m_pAllocator;
    AK::IAkEffectPluginContext* m_pContext;
    Engine m_engine;

    static const int kMaxFrames = 4096;          // Wwise の 1 ブロック（既定 1024）より十分大きく
    float* m_mono;                               // 受けた音をモノラルに畳む
    float* m_tmpL;                               // Voice・FDN・方向バスの出力を一旦受ける
    float* m_tmpR;
    AF_HostVoice* m_voices;                      // 台帳の写し（AF_HOST_MAX_VOICES 本）
    int m_voiceCount;
    AF_HostShared m_shared;
    int m_space;                                 // いま当たっている座標系（0 世界 / 1 聞き手）。最初は 1 を試す
    AkUInt32 m_sampleRate;
};

#endif // AcousticFlowFX_H
