// AcousticFlowGameMode.cpp ── 仕組みの説明は AcousticFlowGameMode.h の頭書き。
#include "AcousticFlowGameMode.h"
#include "AcousticFlowSpace.h"
#include "AcousticFlowWorld.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "AcousticFlowWalker.h"
#include "GameFramework/PlayerStart.h"

using namespace AcousticFlowSpace;

AAcousticFlowGameMode::AAcousticFlowGameMode()
{
	DefaultPawnClass = AAcousticFlowWalker::StaticClass();   // Unity の FlowWalker と同じ歩き（1.4 m/s・右ドラッグで向く）
}

/// 遊びを始める。場面は組まない（レベルに置いた物が正）。司令塔が無い地図なら、置き忘れを知らせるだけ。
void AAcousticFlowGameMode::StartPlay()
{
	if (!AAcousticFlowWorld::Find(GetWorld()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[AcousticFlow] この地図に司令塔（AcousticFlowWorld）が無い。レベルに 1 つ置くと鳴る"));
		if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Yellow, TEXT("AcousticFlow: no AcousticFlowWorld in this level (place one)"));
	}
	Super::StartPlay();
}

/// プレイヤーを出発点（PlayerStart）の位置と向きにそのまま置く。無ければ手前の部屋、戸口の中心から 3 m、目の高さ 1.6 m で戸口を向く。
///   ★普通の置き方（Super）は当たりを避けて位置をずらすことがある。耳の位置が検査の場面とずれないように、そのまま置く。
APawn* AAcousticFlowGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	APlayerStart* Start = Cast<APlayerStart>(StartSpot);
	if (!Start) for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It) { Start = *It; break; }
	const FTransform T = Start ? FTransform(Start->GetActorRotation(), Start->GetActorLocation())
	                           : FTransform(FRotator::ZeroRotator, ToUnreal(0.0f, 1.6f, -3.0f));
	return SpawnDefaultPawnAtTransform(NewPlayer, T);
}
