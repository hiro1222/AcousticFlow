// AcousticFlowThirdPersonGameMode.cpp ── 仕組みの説明は AcousticFlowThirdPersonGameMode.h の頭書き。
#include "AcousticFlowThirdPersonGameMode.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/ConstructorHelpers.h"

AAcousticFlowThirdPersonGameMode::AAcousticFlowThirdPersonGameMode()
{
	PrimaryActorTick.bCanEverTick = true;   // 録画の自動の歩き（-AFDemoWalk のときだけ働く）
	// Unreal の三人称テンプレートのキャラクターとコントローラー（BP_ThirdPersonGameMode と同じ組み合わせ）
	static ConstructorHelpers::FClassFinder<APawn> Pawn(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	static ConstructorHelpers::FClassFinder<APlayerController> Controller(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonPlayerController"));
	if (Pawn.Succeeded()) DefaultPawnClass = Pawn.Class;
	if (Controller.Succeeded()) PlayerControllerClass = Controller.Class;
}

void AAcousticFlowThirdPersonGameMode::BeginPlay()
{
	Super::BeginPlay();
	bDemoWalk = FParse::Param(FCommandLine::Get(), TEXT("AFDemoWalk"));
	FParse::Value(FCommandLine::Get(), TEXT("AFDemoWalkM="), DemoWalkM);
	if (bDemoWalk) UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 録画の自動の歩き: %.1f m"), DemoWalkM);
}

/// 録画の自動の歩き。段ごとの時間で進め、最後にゲームを閉じる（録画の道具が窓の閉じたのを見て書き終える）。
void AAcousticFlowThirdPersonGameMode::Tick(float Dt)
{
	Super::Tick(Dt);
	if (!bDemoWalk || DemoPhase >= 5) return;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* P = PC ? PC->GetPawn() : nullptr;
	if (!P) return;
	if (!bDemoStarted)
	{
		bDemoStarted = true;
		DemoStart = P->GetActorLocation();
		DemoYaw = P->GetActorRotation().Yaw;
		PC->SetIgnoreLookInput(true);   // 録画中はマウス・キーで動かない（向きと移動はここで決める）
		PC->SetIgnoreMoveInput(true);
		PC->SetControlRotation(FRotator(-10.0f, DemoYaw, 0.0f));
	}
	DemoTime += Dt;
	const float InPhase = DemoTime - DemoPhaseTime;
	auto Next = [&]() { ++DemoPhase; DemoPhaseTime = DemoTime; };
	switch (DemoPhase)
	{
	case 0: if (InPhase >= 2.0f) Next(); break;
	case 1:
	{
		// 1.4 m/s: テンプレートの歩きの最高速（MaxWalkSpeed）に入力の大きさを掛けて落とす。入力は止めてあるので力ずくで足す。
		float Scale = 1.0f;
		if (const ACharacter* C = Cast<ACharacter>(P))
			if (const UCharacterMovementComponent* M = C->GetCharacterMovement()) Scale = FMath::Clamp(140.0f / FMath::Max(1.0f, M->MaxWalkSpeed), 0.0f, 1.0f);
		P->AddMovementInput(FRotator(0.0f, DemoYaw, 0.0f).RotateVector(FVector::ForwardVector), Scale, true);
		if (FVector::Dist2D(P->GetActorLocation(), DemoStart) >= DemoWalkM * 100.0f) Next();
		break;
	}
	case 2: if (InPhase >= 2.0f) Next(); break;
	case 3:
		// 見回す: 左右 75° を 6 秒で 1 往復（正弦なので始めと終わりは止まっている）
		PC->SetControlRotation(FRotator(-10.0f, DemoYaw + 75.0f * FMath::Sin(2.0f * PI * InPhase / 6.0f), 0.0f));
		if (InPhase >= 6.0f) { PC->SetControlRotation(FRotator(-10.0f, DemoYaw, 0.0f)); Next(); }
		break;
	case 4:
		if (InPhase >= 2.0f)
		{
			Next();
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 録画の自動の歩き: 終わり（%.1f 秒）"), DemoTime);
			FGenericPlatformMisc::RequestExit(false);
		}
		break;
	default: break;
	}
}
