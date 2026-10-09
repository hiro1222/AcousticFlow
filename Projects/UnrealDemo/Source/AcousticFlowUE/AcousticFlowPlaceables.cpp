// AcousticFlowPlaceables.cpp ── 仕組みの説明は AcousticFlowPlaceables.h の頭書き。
#include "AcousticFlowPlaceables.h"
#include "AcousticFlowEmitter.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

AAcousticFlowWallBox::AAcousticFlowWallBox()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Base"));   // 足元（箱の底の真ん中）
	RootComponent->SetMobility(EComponentMobility::Movable);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	Box = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Box"));
	Box->SetupAttachment(RootComponent);
	Box->SetMobility(EComponentMobility::Movable);
	if (Cube.Succeeded()) Box->SetStaticMesh(Cube.Object);
	Box->ComponentTags.Add(TEXT("AF_Wall"));                                 // 司令塔が「動かない壁」として拾う印
}

/// 寸法から箱を伸ばし、底が Actor の位置に来るよう持ち上げる（立方体は 100 cm ＝ 拡大率 1、中心が原点）。
void AAcousticFlowWallBox::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	const FVector S(FMath::Max(0.01, SizeM.X), FMath::Max(0.01, SizeM.Y), FMath::Max(0.01, SizeM.Z));
	Box->SetRelativeScale3D(S);
	Box->SetRelativeLocation(FVector(0.0, 0.0, S.Z * 50.0));
}

AAcousticFlowSoundSource::AAcousticFlowSoundSource()
{
	PrimaryActorTick.bCanEverTick = false;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	Ball = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ball"));
	RootComponent = Ball;
	Ball->SetMobility(EComponentMobility::Movable);
	if (Sphere.Succeeded()) Ball->SetStaticMesh(Sphere.Object);
	Ball->SetRelativeScale3D(FVector(0.4));                                  // 球は直径 100 cm → 40 cm
	Ball->SetCollisionEnabled(ECollisionEnabled::NoCollision);               // 音源の球は壁にしない（印も付けない）
	Emitter = CreateDefaultSubobject<UAcousticFlowEmitter>(TEXT("AcousticFlowEmitter"));
}
