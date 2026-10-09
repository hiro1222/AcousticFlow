// AcousticFlowThirdPersonGameMode.h ── 三人称の地図（Cave_ThirdPerson ほか）のゲームモード。Unreal の三人称テンプレートのキャラクターで始める。
//
// ■ 役割
//   プレイヤーをテンプレートの BP_ThirdPersonCharacter（マネキン・アニメーション・Enhanced Input の WASD／マウス／Space）、
//   コントローラーを BP_ThirdPersonPlayerController（入力の割り当て IMC_Default / IMC_MouseLook を足す）にする。場面は組まない。
//   地図の World Settings の GameMode Override にこれを入れてある（新しい地図ではエディタで選ぶ）。
//   中身（/Game/ThirdPerson・/Game/Characters・/Game/Input・/Game/LevelPrototyping）はエンジンの三人称テンプレート（UE 5.6）から
//   写した物で、エディタの「機能またはコンテンツパックを追加 → 三人称」と同じ（2026-10-04、発注者「既定テンプレートと同じ三人称キャラクターに」）。
// ■ 録画の自動の歩き（起動の引数 -AFDemoWalk のときだけ。普段の Play には無い）
//   2 秒立つ → 出発点の向きへ AFDemoWalkM（既定 25 m）を 1.4 m/s で歩く → 2 秒止まる → 見回す（左右 75°、6 秒）→ 2 秒止まる → ゲームを閉じる。
//   キャラクターの入力は止め（録画中に触れても動かない）、移動の入力を力ずくで足す。速さはテンプレートの歩き（5 m/s）に入力の大きさを掛けて
//   1.4 m/s にする（回帰の「歩行の連続性」と同じ速さ）。tools/record_unreal_demo.ps1 が使う。
// ■ 退けた書き方
//   ・自前のキャラクター（AAcousticFlowThirdPerson、円柱と円錐の見た目、2026-10-03〜10-04）: テンプレートと操作も見た目も違う。_attic へ。
//   ・テンプレートの BP_ThirdPersonGameMode を地図にそのまま入れる: 録画の自動の歩きを足す場所が無い。キャラクターとコントローラーだけ借りる。
// ■ 壊れる所
//   ・地図の GameMode Override を外すと、プロジェクトの既定（一人称の歩き）で始まる。
//   ・テンプレートの中身を消す・動かすと、キャラクターが既定の DefaultPawn（浮いて飛ぶ球）になる（起動のログに出る）。
//   ・耳はキャラクターの頭（司令塔の bListenerAtPawn、目の高さ ＝ カプセルの中心 ＋ BaseEyeHeight）。テンプレートは走りが 5 m/s。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "AcousticFlowThirdPersonGameMode.generated.h"

UCLASS()
class ACOUSTICFLOWUE_API AAcousticFlowThirdPersonGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AAcousticFlowThirdPersonGameMode();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool bDemoWalk = false;
	bool bDemoStarted = false;
	float DemoWalkM = 25.0f;     // 歩く距離（m、-AFDemoWalkM=）
	float DemoTime = 0.0f;       // 始まってからの時間（秒）
	float DemoPhaseTime = 0.0f;  // その段に入った時刻
	int32 DemoPhase = 0;         // 0 立つ / 1 歩く / 2 止まる / 3 見回す / 4 止まる / 5 閉じた
	float DemoYaw = 0.0f;        // 出発点の向き
	FVector DemoStart = FVector::ZeroVector;
};
