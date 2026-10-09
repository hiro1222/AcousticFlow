// AcousticFlowDoor.h ── 蝶番で開く扉（Unity の SwingDoor に当たる物）。角度を 1 つ持ち、板の位置・向き・寸法に落とす。
//
// ■ 全体の中の位置
//   操作（5 キーで開く / 6 キーで閉じる）→ **ここ**（角度 → 板）→ 板の部品（タグ AF_Door）→ AAcousticFlowWorld が
//   「動く箱」として毎フレーム engine へ渡す → engine が影・隙間・回折を解く。★ここは音に触れない。
// ■ 中の仕組み
//   蝶番→自由端の向き along = (cosθ, 0, sinθ)（エンジンの座標、振れる向きが前）。板の中心 = 蝶番 + along × 幅/2。
//   枠との隙間は自由端と上下だけに付け、蝶番側は密着させる（蝶番側に隙間があると閉じても漏れ続ける）。
// ■ 置き方
//   Actor の位置 ＝ 蝶番（戸口の片端、扉の高さの真ん中、壁の厚みの真ん中）。回していなければ閉じた板は +Y へ伸び、+X 側へ開く。
//   ほかの向きの壁には Actor を水平に回して置く（Yaw だけ見る。2026-10-04 から。Unity の SwingDoor の hinge も同じ）。
// ■ 壊れる所
//   ・Actor の位置（蝶番）の奥行きは壁の厚みの真ん中に置く（2026-10-01 の決定、Unity の SwingDoor と同じ）。
//     板が枠を抜けるまではこもったまま、すき間が開いた所で音色が変わる。面一に置くと 1° でこもりが抜け、扉の影が短くなる。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AcousticFlowDoor.generated.h"

class UStaticMeshComponent;

UCLASS()
class ACOUSTICFLOWUE_API AAcousticFlowDoor : public AActor
{
	GENERATED_BODY()

public:
	AAcousticFlowDoor();

	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float WidthM = 1.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float HeightM = 3.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float ThicknessM = 0.06f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float ClearanceM = 0.004f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow", meta = (ClampMin = "0", ClampMax = "120")) float AngleDeg = 0.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float SpeedDegPerSec = 45.0f;

	/// 角度から板を置き直す（エディタで角度を変えたときも呼ぶ）。
	void Apply();

protected:
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, Category = "AcousticFlow") TObjectPtr<USceneComponent> Hinge;
	UPROPERTY(VisibleAnywhere, Category = "AcousticFlow") TObjectPtr<UStaticMeshComponent> Leaf;
};
