// AcousticFlowWorldAreas.cpp ── 司令塔のエリアの表示。仕組みは AcousticFlowWorld.h の頭書き「エリアの表示」。
//
//   エディタ（Play していない）: 印の付いた箱から下見用のエンジンの世界（Preview）を組み、部屋グラフを描く。
//   Play 中: 本番の世界（World）の部屋グラフを 8 キーで描く。描く物はどちらも同じ（DrawAreas と DrawAreaLabels）。
//   ★描くのはエンジンが実際に使っている物（AF_WorldBoxInfo・AF_WorldApertureInfo・AF_WorldRoomAt）。
//     Unreal の見た目ではなくエンジンの答えを描くので、座標の橋（AcousticFlowSpace）の間違いもここで見える。
#include "AcousticFlowWorld.h"
#include "AcousticFlowEmitter.h"
#include "AcousticFlowPlaceables.h"
#include "AcousticFlowSpace.h"
#include "Components/StaticMeshComponent.h"
#include "Debug/DebugDrawService.h"
#include "DrawDebugHelpers.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/StaticMesh.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "UObject/UObjectIterator.h"

using namespace AcousticFlowSpace;

namespace
{
	const FColor kRoomColors[] = { FColor(70, 160, 255), FColor(255, 130, 70), FColor(110, 215, 110),
	                               FColor(215, 120, 255), FColor(255, 205, 70), FColor(70, 225, 215) };
	FColor RoomColor(int32 R, uint8 A) { FColor C = kRoomColors[((R % 6) + 6) % 6]; C.A = A; return C; }
	const FColor kDoorwayColor(90, 230, 255);
	constexpr uint8 kWorldDepth = 0, kForeground = 1;   // SDPG_World / SDPG_Foreground（前面は壁の向こうでも見える）
}

/// エディタの下見。0.25 秒ごとに壁の形を見て、変わっていて 0.3 秒動いていなければ組み直す。扉は位置だけ送る。
void AAcousticFlowWorld::TickEditorPreview()
{
	if (!bShowAreasInEditor) { if (Preview) ReleasePreview(); return; }
	EnsureLabelDraw();
	const double Now = FPlatformTime::Seconds();
	if (Now >= PreviewNextCheck)
	{
		PreviewNextCheck = Now + 0.25;
		const uint32 H = WallHash();
		if (!Preview) RebuildPreview(H);
		else if (H != PreviewHash)
		{
			// ★壁を引きずっている間に毎回組むと、1 回数百 ms の組み直しでエディタが引っかかる。止まってから組む。
			if (H != PendingHash) { PendingHash = H; PendingSince = Now; }
			else if (Now - PendingSince >= 0.3) RebuildPreview(H);
		}
	}
	if (!Preview) return;

	// 扉（動く箱）は位置だけ送る（部屋グラフに入っていないので組み直し不要）。動いたら 1 回解いて戸口の開き具合を出し直す。
	bool bMoved = false;
	for (FBoxEntry& B : PreviewBoxes)
	{
		if (!B.bDynamic || !B.Comp.IsValid() || !B.Comp->GetStaticMesh()) continue;
		const FTransform T = B.Comp->GetComponentTransform();
		if (T.Equals(B.Last, 0.01)) continue;
		B.Last = T;
		const FBox Local = B.Comp->GetStaticMesh()->GetBoundingBox();
		const FQuat Q = T.GetRotation();
		AF_WorldSetBoxTransform(Preview, B.Id, ToEngine(T.TransformPosition(Local.GetCenter())), DirToEngine(Q.GetAxisY()), DirToEngine(Q.GetAxisZ()));
		bMoved = true;
	}
	if (bMoved) AF_WorldUpdate(Preview, 1.0f / 60.0f);
	DrawAreas(Preview);
}

/// 下見の世界を組み直す（Play の世界と同じ拾い方・同じ材質）。部屋グラフを組んで 1 回解き、床の升目を引く。
void AAcousticFlowWorld::RebuildPreview(uint32 Hash)
{
	if (Preview) AF_WorldDestroy(Preview);
	Preview = AF_WorldCreate();
	PreviewHash = Hash;
	PendingHash = Hash;
	if (!Preview) return;
	CollectBoxesInto(Preview, PreviewBoxes);
	ApplyRoomSettings(Preview);
	AF_WorldSetWorkers(Preview, 1);
	AF_WorldBuild(Preview);
	AF_WorldUpdate(Preview, 1.0f / 60.0f);   // 戸口の開き具合（板の覆い）は解いてから読める
	ComputeAreas(Preview);
	UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 下見: 箱 %d / 部屋 %d / 戸口 %d を組み直した"),
		PreviewBoxes.Num(), AF_WorldRoomCount(Preview), AF_WorldApertureCount(Preview));
}

/// 壁の形の指紋。壁（AF_Wall）は位置・向き・大きさ・材質、扉（AF_Door）は大きさだけ（位置は動かしても組み直さない）。部屋の割り方の摘みも。
uint32 AAcousticFlowWorld::WallHash() const
{
	uint32 H = 0x9E3779B9u;
	int32 N = 0;
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		const UStaticMeshComponent* C = *It;
		if (C->GetWorld() != GetWorld() || !C->GetStaticMesh() || !IsValid(C)) continue;
		const bool bWall = C->ComponentHasTag(TEXT("AF_Wall")), bDoor = C->ComponentHasTag(TEXT("AF_Door"));
		if (!bWall && !bDoor) continue;
		const FTransform T = C->GetComponentTransform();
		if (bWall)
		{
			H = HashCombineFast(H, GetTypeHash(T.GetLocation()));
			H = HashCombineFast(H, GetTypeHash(T.GetRotation().Euler()));
		}
		H = HashCombineFast(H, GetTypeHash(T.GetScale3D()));
		H = HashCombineFast(H, GetTypeHash(C->GetStaticMesh()->GetFName()));
		if (const AAcousticFlowWallBox* WB = Cast<AAcousticFlowWallBox>(C->GetOwner())) H = HashCombineFast(H, GetTypeHash(static_cast<uint8>(WB->Material)));
		++N;
	}
	// 部屋の割り方（司令塔の摘み）も指紋に入れる。変えたら組み直す。
	H = HashCombineFast(H, GetTypeHash(RoomSeedRadiusM));
	H = HashCombineFast(H, GetTypeHash(bOutsideMouth));
	return HashCombineFast(H, GetTypeHash(N));
}

/// 床の升目で部屋を引き、横一列の塊（AreaRuns）にまとめる。部屋ごとの床の高さと札（番号・体積・RT60）も作る。
///   ★断面は耳の高さ（AreaSampleHeightM）。部屋は升目の塊なので、耳の高さで切った形がそのエリア。
void AAcousticFlowWorld::ComputeAreas(AF_WorldHandle W)
{
	AreaRuns.Reset();
	RoomFloorY.Reset();
	AreaLabels.Reset();
	if (!W || !GetWorld()) return;

	// 動かない箱の外接範囲（エンジンの座標、x と z）
	float MinX = FLT_MAX, MaxX = -FLT_MAX, MinZ = FLT_MAX, MaxZ = -FLT_MAX;
	const int32 NB = AF_WorldBoxCount(W);
	for (int32 i = 0; i < NB; ++i)
	{
		AF_BoxInfo B{};
		if (!AF_WorldBoxInfo(W, i, &B) || B.dynamic) continue;
		float E[3];
		for (int k = 0; k < 3; ++k)
			E[k] = FMath::Abs(B.axisX[k]) * B.halfExtents[0] + FMath::Abs(B.axisY[k]) * B.halfExtents[1] + FMath::Abs(B.axisZ[k]) * B.halfExtents[2];
		MinX = FMath::Min(MinX, B.center[0] - E[0]); MaxX = FMath::Max(MaxX, B.center[0] + E[0]);
		MinZ = FMath::Min(MinZ, B.center[2] - E[2]); MaxZ = FMath::Max(MaxZ, B.center[2] + E[2]);
	}
	if (MinX > MaxX) return;

	float Step = FMath::Max(0.05f, AreaCellM);
	while (((MaxX - MinX) / Step) * ((MaxZ - MinZ) / Step) > 250000.0f) Step *= 2.0f;   // 引く回数の上限（広い地図で固まらない）
	AreaCellUsed = Step;
	const int32 NX = FMath::CeilToInt((MaxX - MinX) / Step), NZ = FMath::CeilToInt((MaxZ - MinZ) / Step);
	const int32 NR = AF_WorldRoomCount(W);
	TArray<double> SumX, SumZ, Cnt;
	TArray<FVector2f> First;
	SumX.Init(0.0, NR); SumZ.Init(0.0, NR); Cnt.Init(0.0, NR); First.Init(FVector2f(0, 0), NR);

	for (int32 iz = 0; iz < NZ; ++iz)
	{
		const float Z = MinZ + (iz + 0.5f) * Step;
		int32 Cur = -2;
		float X0 = MinX;
		for (int32 ix = 0; ix <= NX; ++ix)
		{
			const float X = MinX + (ix + 0.5f) * Step;
			const int32 R = (ix < NX) ? AF_WorldRoomAt(W, AF_Vector3{ X, AreaSampleHeightM, Z }) : -3;
			if (R != Cur)
			{
				if (Cur >= 0) AreaRuns.Add(FAreaRun{ Cur, X0, X - 0.5f * Step, Z });
				Cur = R;
				X0 = X - 0.5f * Step;
			}
			if (R >= 0 && R < NR)
			{
				if (Cnt[R] == 0.0) First[R] = FVector2f(X, Z);
				SumX[R] += X; SumZ[R] += Z; Cnt[R] += 1.0;
			}
		}
	}

	// 部屋ごとの床の高さ: 最初の升目から真下へ Unreal の当たりで引く（当たらなければ断面の 1.6 m 下）
	RoomFloorY.Init(AreaSampleHeightM - 1.6f, NR);
	for (int32 R = 0; R < NR; ++R)
	{
		if (Cnt[R] == 0.0) continue;
		FHitResult Hit;
		const FVector A = ToUnreal(First[R].X, AreaSampleHeightM, First[R].Y);
		const FVector B = ToUnreal(First[R].X, AreaSampleHeightM - 30.0f, First[R].Y);
		if (GetWorld()->LineTraceSingleByChannel(Hit, A, B, ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(AcousticFlowArea), false)))
			RoomFloorY[R] = float(Hit.ImpactPoint.Z * 0.01);
	}

	// 部屋の札: 番号・体積・表面積・RT60（125 Hz / 1 kHz / 4 kHz）・耳の高さの床面積
	for (int32 R = 0; R < NR; ++R)
	{
		if (Cnt[R] == 0.0) continue;
		float Rt[6] = {}, V = 0, S = 0, Mfp = 0;
		AF_WorldRoomInfo(W, R, Rt, &V, &S, &Mfp);
		FAreaLabel L;
		L.Pos = ToUnreal(float(SumX[R] / Cnt[R]), RoomFloorY[R] + 0.1f, float(SumZ[R] / Cnt[R]));
		L.Color = RoomColor(R, 255);
		L.Lines = { FString::Printf(TEXT("ROOM %d"), R),
		            FString::Printf(TEXT("V %.0f m3 / S %.0f m2 / floor %.1f m2"), V, S, Cnt[R] * Step * Step),
		            FString::Printf(TEXT("RT60 %.2f / %.2f / %.2f s  (125 / 1k / 4k Hz)"), Rt[0], Rt[3], Rt[5]) };
		AreaLabels.Add(MoveTemp(L));
	}
}

/// 描く（1 フレームぶん。毎フレーム呼ぶ）。床の色タイル・戸口の枠・エンジンの箱の輪郭・音源の幅。
void AAcousticFlowWorld::DrawAreas(AF_WorldHandle W) const
{
	UWorld* UW = GetWorld();
	if (!UW || !W) return;

	// 床の色タイル（部屋ごと）。横一列の塊を 1 枚の薄い板で。
	const double Half = AreaCellUsed * 0.5;
	for (const FAreaRun& R : AreaRuns)
	{
		const float FY = RoomFloorY.IsValidIndex(R.Room) ? RoomFloorY[R.Room] : 0.0f;
		const FVector C = ToUnreal(0.5f * (R.X0 + R.X1), FY + 0.02f, R.Z);
		const FVector Ext(Half * 100.0 * 0.92, (R.X1 - R.X0) * 50.0, 0.5);   // 列の間に細い隙間（升目の粗さが見えるように）
		DrawDebugSolidBox(UW, C, Ext, RoomColor(R.Room, 80), false, -1.0f, kWorldDepth);
	}

	// 戸口の枠（部屋と部屋の境目。開き具合は札に出す）
	const int32 NA = AF_WorldApertureCount(W);
	for (int32 a = 0; a < NA; ++a)
	{
		AF_ApertureInfo A{};
		if (!AF_WorldApertureInfo(W, a, &A)) continue;
		const FVector C = ToUnreal(A.center);
		const FVector U = DirToUnreal(A.axisU) * (A.halfU * 100.0), V = DirToUnreal(A.axisV) * (A.halfV * 100.0);
		const FVector P[4] = { C - U - V, C + U - V, C + U + V, C - U + V };
		for (int k = 0; k < 4; ++k) DrawDebugLine(UW, P[k], P[(k + 1) % 4], kDoorwayColor, false, -1.0f, kForeground, 3.0f);
	}

	// エンジンが見ている箱の輪郭（壁は灰、扉は橙で前面に）。1 cm 膨らませて面と重ならないように。
	const int32 NB = AF_WorldBoxCount(W);
	for (int32 i = 0; i < NB; ++i)
	{
		AF_BoxInfo B{};
		if (!AF_WorldBoxInfo(W, i, &B) || !B.active) continue;
		const FQuat Q = FMatrix(DirToUnreal(B.axisZ), DirToUnreal(B.axisX), DirToUnreal(B.axisY), FVector::ZeroVector).ToQuat();
		const FVector Ext(B.halfExtents[2] * 100.0 + 1.0, B.halfExtents[0] * 100.0 + 1.0, B.halfExtents[1] * 100.0 + 1.0);
		const bool bDyn = B.dynamic != 0;
		DrawDebugBox(UW, ToUnreal(B.center), Ext, Q, bDyn ? FColor(255, 150, 40) : FColor(190, 190, 205), false, -1.0f,
		             bDyn ? kForeground : kWorldDepth, bDyn ? 2.5f : 1.0f);
	}

	// 音源の幅（RadiusM の球、前面に）
	for (TObjectIterator<UAcousticFlowEmitter> It; It; ++It)
	{
		if (It->GetWorld() != UW || !It->GetOwner()) continue;
		DrawDebugSphere(UW, It->GetOwner()->GetActorLocation(), FMath::Max(5.0f, It->RadiusM * 100.0f), 16,
		                FColor(255, 170, 40), false, -1.0f, kForeground, 1.5f);
	}
}

/// 札を画面に正対して描く（Unreal のデバッグ描画の仕組み。どの角度から見ても読める）。戸口の開き具合はその場で引く。
///   ★この呼び出しは全部の画面から来るので、自分の世界の画面にだけ描く（エディタの下見が Play の画面に出ないように）。
///   ★文字は英数字だけ（Unreal の既定のフォントには日本語の字形が無い）。
void AAcousticFlowWorld::DrawAreaLabels(UCanvas* Canvas, APlayerController* /*PC*/)
{
	UWorld* UW = GetWorld();
	if (!Canvas || !Canvas->SceneView || !UW) return;
	const FSceneViewFamily* Fam = Canvas->SceneView->Family;
	if (!Fam || !Fam->Scene || Fam->Scene->GetWorld() != UW) return;
	const bool bGame = UW->IsGameWorld();
	if (bGame ? !bShowAreasInGame : !bShowAreasInEditor) return;
	const AF_WorldHandle W = bGame ? World : Preview;
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font) return;
	const float LineH = Font->GetMaxCharHeight() + 1.0f;

	auto Put = [&](const FVector& Pos, const TArray<FString>& Lines, const FColor& Col)
	{
		const FVector S = Canvas->Project(Pos);
		if (S.Z <= 0.0) return;   // 目の後ろ
		float Y = float(S.Y) - LineH * Lines.Num() * 0.5f;
		for (const FString& Line : Lines)
		{
			float TW = 0.0f, TH = 0.0f;
			Canvas->TextSize(Font, Line, TW, TH);
			const float X = float(S.X) - TW * 0.5f;
			Canvas->SetDrawColor(FColor(0, 0, 0, 200));
			Canvas->DrawText(Font, Line, X + 1.0f, Y + 1.0f);   // 影（明るい床の上でも読めるように）
			Canvas->SetDrawColor(Col);
			Canvas->DrawText(Font, Line, X, Y);
			Y += LineH;
		}
	};
	for (const FAreaLabel& L : AreaLabels) Put(L.Pos, L.Lines, L.Color);
	if (!W) return;
	const int32 NA = AF_WorldApertureCount(W);
	for (int32 a = 0; a < NA; ++a)
	{
		AF_ApertureInfo A{};
		if (!AF_WorldApertureInfo(W, a, &A)) continue;
		const FVector Top = ToUnreal(A.center) + FVector(0, 0, FMath::Abs(DirToUnreal(A.axisV).Z) * A.halfV * 100.0 + 25.0);
		Put(Top, { (A.roomA < 0) ? FString::Printf(TEXT("MOUTH %d  (outside - room %d)"), a, A.roomB)   // 外への口（bOutsideMouth）
		                         : FString::Printf(TEXT("DOORWAY %d  (room %d - %d)"), a, A.roomA, A.roomB),
		           FString::Printf(TEXT("open %.0f%%"), A.openFrac * 100.0f) }, kDoorwayColor);
	}
}

/// 札の描画を画面に登録する（1 回だけ）。エディタの画面は "Editor"、Play の画面は "Game" の表示の区分で呼ばれる。
void AAcousticFlowWorld::EnsureLabelDraw()
{
	if (LabelDrawHandle.IsValid()) return;
	const TCHAR* Flag = (GetWorld() && GetWorld()->IsGameWorld()) ? TEXT("Game") : TEXT("Editor");
	LabelDrawHandle = UDebugDrawService::Register(Flag, FDebugDrawDelegate::CreateUObject(this, &AAcousticFlowWorld::DrawAreaLabels));
}

/// 下見の世界を片付ける。
void AAcousticFlowWorld::ReleasePreview()
{
	if (Preview) { AF_WorldDestroy(Preview); Preview = nullptr; }
	PreviewBoxes.Reset();
	PreviewHash = 0;
	PendingHash = 0;
}

/// 消えるとき: 札の登録を外し、下見の世界を壊す（エディタで地図を閉じたとき・Play の複製が消えるとき）。
void AAcousticFlowWorld::BeginDestroy()
{
	if (LabelDrawHandle.IsValid()) { UDebugDrawService::Unregister(LabelDrawHandle); LabelDrawHandle.Reset(); }
	ReleasePreview();
	Super::BeginDestroy();
}
