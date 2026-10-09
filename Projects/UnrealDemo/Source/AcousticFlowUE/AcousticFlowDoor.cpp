// AcousticFlowDoor.cpp ── 仕組みの説明は AcousticFlowDoor.h の頭書き。
#include "AcousticFlowDoor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ConstructorHelpers.h"

AAcousticFlowDoor::AAcousticFlowDoor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;   // 世界（TG_PostPhysics）が読む前に動かす
	Hinge = CreateDefaultSubobject<USceneComponent>(TEXT("Hinge"));
	RootComponent = Hinge;
	Hinge->SetMobility(EComponentMobility::Movable);
	Leaf = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Leaf"));
	Leaf->SetupAttachment(Hinge);
	Leaf->SetMobility(EComponentMobility::Movable);
	Leaf->ComponentTags.Add(TEXT("AF_Door"));     // 世界が「動く箱・木の扉」として拾う印
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded()) Leaf->SetStaticMesh(Cube.Object);
}

void AAcousticFlowDoor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Apply();
}

/// 角度を進める。5 キーで開く・6 キーで閉じる（押している間だけ）。0〜120° に丸めてから板を置き直す。
void AAcousticFlowDoor::Tick(float Dt)
{
	Super::Tick(Dt);
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (PC->IsInputKeyDown(EKeys::Five)) AngleDeg += SpeedDegPerSec * Dt;
		if (PC->IsInputKeyDown(EKeys::Six)) AngleDeg -= SpeedDegPerSec * Dt;
	}
	AngleDeg = FMath::Clamp(AngleDeg, 0.0f, 120.0f);
	Apply();
}

/// 角度から板の位置・向き・寸法を作る（engine が読むのはこの結果だけ）。
///   エンジンの座標の along = (cosθ, 0, sinθ) は Unreal では (X = sinθ, Y = cosθ)。
///   板の立方体（100 cm）のローカル Y を along に向けるには、ヨーを −θ にする（ローカル Y は (−sinψ, cosψ)）。
void AAcousticFlowDoor::Apply()
{
	if (!Leaf) return;
	const float Th = FMath::DegreesToRadians(AngleDeg);
	const float W = FMath::Max(0.01f, WidthM - ClearanceM);
	const float H = FMath::Max(0.01f, HeightM - 2.0f * ClearanceM);
	// 蝶番（Actor）の水平の向き（Yaw）で回す（2026-10-04）。回していなければ、閉じた板は +Y へ伸び +X 側へ開く（今まで）。
	//   ★Yaw だけ使う（傾けて置いても板は立ったまま）。前は Actor の向きを見ず、Y に沿った壁にしか付けられなかった。
	const float Yaw = GetActorRotation().Yaw;
	const FVector AlongU = FRotator(0.0f, Yaw, 0.0f).RotateVector(FVector(FMath::Sin(Th), FMath::Cos(Th), 0.0));
	const FVector Center = GetActorLocation() + AlongU * (W * 0.5f * 100.0f);
	Leaf->SetWorldLocationAndRotation(Center, FRotator(0.0f, Yaw - AngleDeg, 0.0f));
	Leaf->SetWorldScale3D(FVector(ThicknessM, W, H));   // 立方体は 1 m ＝ 拡大率 1（X 厚み・Y 幅・Z 高さ）
}
