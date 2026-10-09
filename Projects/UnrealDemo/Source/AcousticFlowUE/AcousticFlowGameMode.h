// AcousticFlowGameMode.h ── 試聴台のゲームモード。プレイヤーを出発点に置く。場面は組まない（レベルに置いた物が正）。
//
// ■ 役割
//   ・壁・扉・音源・司令塔は地図（/Game/Maps/Flow_SwingDoor など）に Actor として置いてある。ここは何も組まない。
//     地図に司令塔（AAcousticFlowWorld）が無ければ、画面とログに「置いて」と出すだけ。
//   ・プレイヤーは AAcousticFlowWalker（Unity の FlowWalker と同じ。WASD で 1.4 m/s、Shift で 3 m/s、右ドラッグで向く、Q/E で上下）。
//     出発点（PlayerStart）の位置と向きにそのまま置く。
//   扉は 5 キーで開く・6 キーで閉じる。
// ■ 退けた書き方
//   ・地図が空なら場面をコードで組む予備（2026-10-04 まで、AcousticFlowScene）: 構造物の答えがコードと地図の 2 つになり、
//     エディタで直した地図と食い違う。発注者「コードで用意するのでなく、レベルに配置する形に」。コードは _attic/ へ。
// ■ 壊れる所
//   ・司令塔を置き忘れた地図では音が鳴らない（音源は司令塔に登録して初めて鳴る）。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "AcousticFlowGameMode.generated.h"

UCLASS()
class ACOUSTICFLOWUE_API AAcousticFlowGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AAcousticFlowGameMode();
	virtual void StartPlay() override;
	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;
};
