// AcousticFlowWalker.cpp ── 仕組みの説明は AcousticFlowWalker.h の頭書き。
#include "AcousticFlowWalker.h"
#include "Components/SceneComponent.h"
#include "GameFramework/PlayerController.h"

AAcousticFlowWalker::AAcousticFlowWalker()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;   // 世界（TG_PostPhysics）が聞き手を読む前に動かす
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Ear"));
	BaseEyeHeight = 0.0f;                         // Actor の位置がそのまま耳（出発点の高さ 1.6 m）
	SetActorEnableCollision(false);
}

/// 開始時の向きを yaw/pitch に取り込む（出発点の向きのまま始める）。
void AAcousticFlowWalker::BeginPlay()
{
	Super::BeginPlay();
	Yaw = GetActorRotation().Yaw;
	Pitch = 0.0f;
	if (AController* C = GetController()) C->SetControlRotation(FRotator(Pitch, Yaw, 0.0f));
}

/// 毎フレームの視線と移動（Unity の FlowWalker.Update と同じ順）。
///   視線: 右ボタン中だけ、マウスの移動量 × LookDegPerCount を yaw/pitch に足す（pitch は ±80° で頭打ち）。
///   移動: WASD/QE を足した向きを正規化し、yaw だけの回転を掛けて水平面の向きに直してから、速さ × 時間を足す。
void AAcousticFlowWalker::Tick(float Dt)
{
	Super::Tick(Dt);
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return;
	if (PC->IsInputKeyDown(EKeys::RightMouseButton))
	{
		float DX = 0.0f, DY = 0.0f;
		PC->GetInputMouseDelta(DX, DY);
		Yaw += DX * LookDegPerCount;
		Pitch = FMath::Clamp(Pitch + DY * LookDegPerCount, -80.0f, 80.0f);
	}
	PC->SetControlRotation(FRotator(Pitch, Yaw, 0.0f));

	const float Speed = (PC->IsInputKeyDown(EKeys::LeftShift) ? RunSpeed : WalkSpeed) * 100.0f;   // cm/s
	FVector D = FVector::ZeroVector;
	if (PC->IsInputKeyDown(EKeys::W)) D.X += 1.0;
	if (PC->IsInputKeyDown(EKeys::S)) D.X -= 1.0;
	if (PC->IsInputKeyDown(EKeys::D)) D.Y += 1.0;
	if (PC->IsInputKeyDown(EKeys::A)) D.Y -= 1.0;
	if (PC->IsInputKeyDown(EKeys::E)) D.Z += 1.0;
	if (PC->IsInputKeyDown(EKeys::Q)) D.Z -= 1.0;
	if (!D.IsNearlyZero())
	{
		const FVector Flat = FRotator(0.0f, Yaw, 0.0f).RotateVector(D.GetSafeNormal());
		SetActorLocation(GetActorLocation() + Flat * Speed * Dt);
	}
}
