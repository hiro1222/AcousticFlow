// AcousticFlowEmitter.h ── 音源（Unity の WorldVoice に当たる物）。Actor に付けると、その場所から自前のエンジンで鳴る。
//
// ■ 全体の中の位置
//   場面（この部品）→ AAcousticFlowWorld（毎フレーム配分を解いて、この Voice に置く）
//     → Wwise の Event がこの Actor で鳴り、バス「AcousticFlow」の自前プラグインが位置でこの Voice を引いて両耳の音にする。
// ■ 役割
//   Voice（自前の DSP、音源 1 本ぶん）を持つことと、Wwise の Event を鳴らすこと。音を作る式はここに無い。
// ■ 中の仕組み
//   BeginPlay: Voice と合成 HRTF を作り（Wwise の周波数、大きめのブロック）、世界に登録し、Event を鳴らす。
//   EndPlay:   世界から外し、Voice と HRTF は台帳に預けて 1 秒後に壊してもらう（Wwise の音のスレッドがまだ握っている）。
// ■ 繋がり
//   受ける: AAcousticFlowWorld（emitter 番号・FDN・方向バス）。渡す: Wwise（Event）、台帳（位置と Voice、世界が書く）。
// ■ 退けた書き方
//   ・Voice を直に壊す: 音のスレッドが写しの中の古い手で鳴らして落ちる。
//   ・Wwise の遮蔽（AkComponent の既定 0.2 秒）を残す: 見通し線が何かに当たると occlusion 1 で丸ごと絞る。
//     エンジンの影（半影・回折・透過）の上に二値が掛かり、少しでも影に入ると無音になっていた（2026-09-30 試聴）。
// ■ 壊れる所
//   ・Event の音の立体化を None にすると、位置が届かず対応付けが全部外れる（素通しで鳴る）。
//   ・別の経路（Blueprint の PostEvent など）で同じ Actor に AkComponent を作ると、遮蔽の既定 0.2 秒が戻る。
//   ・聞き手を既定のままにすると、エディタのビューポートの聞き手のぶんバスの実体が増える（プラグインは 1 つしか回さないので、
//     回さない方の聞き手が出口に繋がっていると音が小さく／無音になりうる）。BindListener で 1 人に絞る。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "acoustic_voice.h"
#include "AcousticFlowEmitter.generated.h"

class UAkAudioEvent;
class UAkComponent;

UCLASS(ClassGroup = (AcousticFlow), meta = (BlueprintSpawnableComponent))
class ACOUSTICFLOWUE_API UAcousticFlowEmitter : public UActorComponent
{
	GENERATED_BODY()

public:
	UAcousticFlowEmitter();

	/// 鳴らす Wwise の Event（WwiseReconcile が作ったアセット）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow")
	TSoftObjectPtr<UAkAudioEvent> Event;

	/// 音源の幅（m）。0 で点。幅があると扉の影がこの円盤にどれだけ掛かるかで連続に変わる。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow", meta = (ClampMin = "0"))
	float RadiusM = 0.2f;

	/// 出力の音量（Voice の outputGain）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow", meta = (ClampMin = "0", ClampMax = "2"))
	float OutputGain = 0.6f;

	/// 予算の重要度（0..1）と、プレイヤーが操作している音か（優先度 1 位）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow", meta = (ClampMin = "0", ClampMax = "1"))
	float Loudness = 1.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow")
	bool bOperated = false;

	AF_VoiceHandle VoiceHandle() const { return Voice; }
	int32 EmitterId = -1;

	/// 世界が毎フレーム呼ぶ。この音源の Wwise の聞き手をプレイヤーの 1 人だけにする（替わったときだけ触る）。
	///   ★エディタはビューポートの聞き手を Play 中も残すので、既定の聞き手のままだとバスの実体が 2 つ作られる。
	void BindListener(UAkComponent* Listener);
	/// 試聴の切り分け用（7 キー）。聞き手を Wwise の既定の全員（ビューポートを含む）へ戻す。次の BindListener で 1 人に戻る。
	void UseDefaultListeners();

	/// 世界が呼ぶ。部屋の FDN と方向バスへ繋ぐ（器を作り直したときも呼ばれる）。
	void Attach(int32 InEmitterId, AF_FdnMixHandle Fdn, AF_DirectionBusHandle Bus);
	void SetFdn(AF_FdnMixHandle Fdn);
	void Detach();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	AF_VoiceHandle Voice = nullptr;
	AF_HrtfHandle Hrtf = nullptr;
	TWeakObjectPtr<UAkComponent> BoundListener;   // いま繋いでいる聞き手
};
