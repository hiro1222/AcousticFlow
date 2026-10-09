// AcousticFlowWalker.h ── 試聴のための歩き（Unity の FlowWalker と同じ）。WASD で歩く、右ドラッグで向く、Q/E で上下、Shift で走る。
//
// ■ 全体の中の位置
//   ゲーム（ここ）→ AAcousticFlowWorld（プレイヤーのカメラを聞き手として engine へ流す）→ 音の計算 → Wwise で鳴らす。
//   この層は「耳をどこへ運ぶか」だけを決める。音の話は 1 行も入っていない。
// ■ 役割
//   歩く速さと向きの回し方を Unity の FlowWalker と揃える（1.4 m/s ＝ 回帰の「歩行の連続性」と同じ速さ）。
//   耳で聞く段差と、Unity で聞いた段差・回帰で測った段差が同じ条件になる。
// ■ 中の仕組み
//   Tick で 3 つ。1) 右ボタンを押している間だけ視線を回す 2) キーの向きを yaw だけの回転で水平に直す 3) 速さを掛けて進む。
//   目の高さは 0（Actor の位置がそのまま耳）。当たりは持たない（壁際で押し戻されて耳が跳ばないように）。
// ■ 退けた書き方
//   ・Unreal の DefaultPawn（2026-10-01 まで）: 12 m/s で進み、マウスで常に向きが回る。Unity の 8.6 倍の速さで、
//     1 フレームの向きの変化も大きく、両耳の差（ITD）の追従が間に合わず「飛ぶ」。Unity と聞き比べる前提が崩れていた。
// ■ 壊れる所
//   ・WalkSpeed を 1.4 m/s から離すと、耳で聞く段差と回帰の数字が別物になる。
//   ・LookSpeed を上げすぎると 1 フレームの向きの変化が大きくなり、ITD の追従が間に合わず「飛ぶ」。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AcousticFlowWalker.generated.h"

UCLASS()
class ACOUSTICFLOWUE_API AAcousticFlowWalker : public APawn
{
	GENERATED_BODY()

public:
	AAcousticFlowWalker();

	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float WalkSpeed = 1.4f;   // m/s（Unity と同じ）
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float RunSpeed = 3.0f;    // Shift
	/// マウスの移動量 1 あたりの角度（度）。Unity の lookSpeed 0.15 × 10 × 入力の既定の感度 0.1 と同じ。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float LookDegPerCount = 0.15f;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	float Yaw = 0.0f, Pitch = 0.0f;
};
