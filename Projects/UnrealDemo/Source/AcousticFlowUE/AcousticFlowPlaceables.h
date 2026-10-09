// AcousticFlowPlaceables.h ── 地図に置く部品（エディタの「アクタを配置」でドラッグして置く）。構造物はコードで組まず、これをレベルに置く。
//
// ■ 全体の中の位置
//   エディタ（ここを置く）→ 司令塔 AAcousticFlowWorld が印の付いた箱と音源の部品を拾う → engine。
//   Play しなくても司令塔の下見がエリア・戸口・箱を描くので、置きながら部屋の形を確かめられる。
// ■ 役割
//   AAcousticFlowWallBox   : 動かない壁の箱（AF_Wall の印付き）。寸法は SizeM（m）、材質は Material（既定 コンクリート）。足元が床に乗る置き方。
//   AAcousticFlowSoundSource: 音源（0.4 m の球 ＋ UAcousticFlowEmitter）。置けば Play で鳴る。球の中心が音の位置。
//   扉は AAcousticFlowDoor をそのまま置く（蝶番が Actor の位置。壁 2 枚の隙間に、壁の厚みの真ん中で置く）。
// ■ 繋がり
//   壁: 司令塔の CollectBoxesInto が部品のタグ AF_Wall で拾う。音源: UAcousticFlowEmitter が自分で司令塔に登録し、Wwise の Event を鳴らす。
// ■ 退けた書き方
//   ・ただの StaticMeshActor に印を手で付けてもらう: 付け忘れると壁が音に効かない（見えるのに音が抜ける）。印を最初から持たせる。
//   ・壁の寸法を Actor の拡大で変える: 足元の位置がずれる（箱の中心が原点のため）。寸法を数値で持ち、箱だけを伸ばす。
// ■ 壊れる所
//   ・音源の球を床に埋めたまま（Z = 0）にすると、音の位置も床の中になる。高さ（目の高さなら 160 cm）を入れること。
//   ・材質を選べるのはこの箱だけ。印 AF_Wall を手で付けた他の部品はコンクリートになる（Unity の AcousticSurface に当たる物は無い）。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AcousticFlowMaterial.h"
#include "AcousticFlowPlaceables.generated.h"

class UStaticMeshComponent;
class UAcousticFlowEmitter;

UCLASS(meta = (DisplayName = "AcousticFlow Wall Box"))
class ACOUSTICFLOWUE_API AAcousticFlowWallBox : public AActor
{
	GENERATED_BODY()

public:
	AAcousticFlowWallBox();

	/// 寸法（m）: X ＝ 厚み、Y ＝ 幅、Z ＝ 高さ。Actor の位置は箱の底の真ん中（床に置けばそのまま立つ）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") FVector SizeM = FVector(0.2, 4.0, 3.0);
	/// 音の材質（Unity の AcousticSurface.material と同じ番号）。洞窟の岩は Cave。値はエンジンの表が正。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") EAcousticFlowMaterial Material = EAcousticFlowMaterial::Concrete;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, Category = "AcousticFlow") TObjectPtr<UStaticMeshComponent> Box;
};

UCLASS(meta = (DisplayName = "AcousticFlow Sound Source"))
class ACOUSTICFLOWUE_API AAcousticFlowSoundSource : public AActor
{
	GENERATED_BODY()

public:
	AAcousticFlowSoundSource();

protected:
	UPROPERTY(VisibleAnywhere, Category = "AcousticFlow") TObjectPtr<UStaticMeshComponent> Ball;
	UPROPERTY(VisibleAnywhere, Category = "AcousticFlow") TObjectPtr<UAcousticFlowEmitter> Emitter;
};
