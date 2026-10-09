// AcousticFlowMaterial.h ── 音の材質の選び方（Unity の AcousticMaterialPreset と同じ名前・同じ番号）。
//
// ■ 役割
//   壁の箱（AAcousticFlowWallBox）がどの材質かを選ぶ。番号はエンジンの AF_WorldAddMaterialPreset にそのまま渡す。
//   値（透過・吸音・散乱の帯域ごとの表）はエンジンの material.cpp だけが持つ（Unreal にも Unity にも同じ表を置かない）。
// ■ 繋がり
//   司令塔 AAcousticFlowWorld::CollectBoxesInto が、箱ごとにこの番号で材質を作る（同じ番号は 1 つを共有）。
// ■ 壊れる所
//   ・番号の並びを変えるとエンジン・Unity とずれる。足すときは末尾に、エンジンの表と同時に。
#pragma once

#include "CoreMinimal.h"
#include "AcousticFlowMaterial.generated.h"

UENUM(BlueprintType)
enum class EAcousticFlowMaterial : uint8
{
	Default  = 0 UMETA(DisplayName = "Default（内壁）"),
	Concrete = 1 UMETA(DisplayName = "Concrete（コンクリート）"),
	Glass    = 2 UMETA(DisplayName = "Glass（ガラス）"),
	Opaque   = 3 UMETA(DisplayName = "Opaque（不透過）"),
	WoodDoor = 4 UMETA(DisplayName = "WoodDoor（木の扉）"),
	WoodRoom = 5 UMETA(DisplayName = "WoodRoom（板張り）"),
	Stone    = 6 UMETA(DisplayName = "Stone（石積み）"),
	Cave     = 7 UMETA(DisplayName = "Cave（岩肌）"),
	Snow     = 8 UMETA(DisplayName = "Snow（新雪）"),
};
