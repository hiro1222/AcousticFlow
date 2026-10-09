// AcousticFlowEmitter.cpp ── 仕組みの説明は AcousticFlowEmitter.h の頭書き。
#include "AcousticFlowEmitter.h"
#include "AcousticFlowWorld.h"
#include "AkAudioDevice.h"
#include "AkAudioEvent.h"
#include "AkComponent.h"
#include "AkGameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"
#include "acoustic_host.h"

UAcousticFlowEmitter::UAcousticFlowEmitter()
{
	PrimaryComponentTick.bCanEverTick = false;
	Event = TSoftObjectPtr<UAkAudioEvent>(FSoftObjectPath(TEXT("/Game/WwiseAudio/Events/Default_Work_Unit/Play_AF_Test.Play_AF_Test")));
}

/// Voice を作り、世界に登録し、Wwise の Event を鳴らす。
///   ★Voice は Wwise の周波数と大きめのブロック（4096）で作る（Wwise の 1 ブロックは既定 1024）。
///   ★世界がまだ立っていなければ、世界の BeginPlay が後で拾う（どちらが先でも同じ形になる）。
void UAcousticFlowEmitter::BeginPlay()
{
	Super::BeginPlay();
	AAcousticFlowWorld* World = AAcousticFlowWorld::Find(GetWorld());
	const int32 SampleRate = World ? World->SampleRate : 48000;

	AF_VoiceConfig Cfg{};
	Cfg.sampleRate = SampleRate; Cfg.maxFrames = 4096; Cfg.tailSeconds = 1.0f;
	Cfg.tapCrossfadeMs = 30.0f; Cfg.hrtfCrossfadeMs = 12.0f; Cfg.hrtfCrossoverHz = 700.0f; Cfg.tailFirstBlock = 64; Cfg.tailCapBlock = 8192;
	Voice = AF_VoiceCreate(&Cfg);
	Hrtf = AF_HrtfCreateSynthetic(SampleRate);
	if (Voice)
	{
		AF_VoiceSetHrtf(Voice, Hrtf);
		AF_VoiceSetHrtfEnabled(Voice, 1);
		AF_VoiceSetOutputGain(Voice, OutputGain);
		AF_VoiceSetTailLevel(Voice, 1.0f);
	}
	if (World) World->Register(this);

	if (UAkAudioEvent* Ev = Event.LoadSynchronous())
	{
		// 鳴らす前に AkComponent を用意して整える（PostEvent は同じ物＝ Actor の根に付いた物を使う）。
		//   ★Wwise の自前の遮蔽を切る（既定は 0.2 秒ごとに見通し線を 1 本飛ばし、当たれば occlusion 1 で直接音も響きも絞る）。
		//     影・回折・透過はエンジンだけが持つ。
		//   ★聞き手をプレイヤーの 1 人に絞ってから鳴らす。鳴らした後で絞ると、最初の数ブロックだけ既定の聞き手
		//     （ビューポートを含む 2 人）へ届いて、バスの実体が 2 つ立ち上がる。
		if (FAkAudioDevice* Dev = FAkAudioDevice::Get())
			if (UAkComponent* Ak = Dev->GetAkComponent(GetOwner()->GetRootComponent(), FName(), nullptr, EAttachLocation::KeepRelativeOffset))
			{
				Ak->OcclusionRefreshInterval = 0.0f;
				if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
					BindListener(Cam->FindComponentByClass<UAkComponent>());
			}
		UAkGameplayStatics::PostEvent(Ev, GetOwner(), 0, FOnAkPostEventCallback(), true);
	}
	else
		UE_LOG(LogTemp, Warning, TEXT("[AcousticFlow] %s: Event が読めない（tools/wwise_setup.py unreal → WwiseReconcile を先に）"), *GetOwner()->GetName());
}

/// 世界から外し、Voice と HRTF は台帳に預けて 1 秒後に壊してもらう。
void UAcousticFlowEmitter::EndPlay(const EEndPlayReason::Type Reason)
{
	if (AAcousticFlowWorld* World = AAcousticFlowWorld::Find(GetWorld())) World->Unregister(this);
	if (Voice) { AF_HostRetireVoice(Voice); Voice = nullptr; }
	if (Hrtf) { AF_HostRetireHrtf(Hrtf); Hrtf = nullptr; }
	Super::EndPlay(Reason);
}

/// 聞き手をプレイヤーの 1 人だけにする。Event が作った AkComponent（Actor の根に付いている）に効かせる。
void UAcousticFlowEmitter::BindListener(UAkComponent* Listener)
{
	if (!Listener || BoundListener.Get() == Listener || !GetOwner()) return;
	UAkComponent* Ak = GetOwner()->FindComponentByClass<UAkComponent>();
	if (!Ak || Ak == Listener) return;
	Ak->SetListeners(TArray<UAkComponent*>{ Listener });
	BoundListener = Listener;
	UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] %s: 聞き手を %s の 1 人に絞った"), *GetOwner()->GetName(), *Listener->GetOwner()->GetName());
}

void UAcousticFlowEmitter::UseDefaultListeners()
{
	FAkAudioDevice* Dev = FAkAudioDevice::Get();
	UAkComponent* Ak = GetOwner() ? GetOwner()->FindComponentByClass<UAkComponent>() : nullptr;
	if (!Dev || !Ak) return;
	TArray<UAkComponent*> All;
	for (const TWeakObjectPtr<UAkComponent>& W : Dev->GetDefaultListeners()) if (W.IsValid()) All.Add(W.Get());
	Ak->SetListeners(All);
	BoundListener = nullptr;
	UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] %s: 聞き手を既定の %d 人に戻した"), *GetOwner()->GetName(), All.Num());
}

void UAcousticFlowEmitter::Attach(int32 InEmitterId, AF_FdnMixHandle Fdn, AF_DirectionBusHandle Bus)
{
	EmitterId = InEmitterId;
	SetFdn(Fdn);
	if (Voice) AF_VoiceSetDirectionBus(Voice, Bus);
}

void UAcousticFlowEmitter::SetFdn(AF_FdnMixHandle Fdn)
{
	if (Voice) AF_VoiceSetFdnMix(Voice, Fdn);
}

void UAcousticFlowEmitter::Detach()
{
	EmitterId = -1;
	if (Voice) { AF_VoiceSetFdnMix(Voice, nullptr); AF_VoiceSetDirectionBus(Voice, nullptr); }
}
