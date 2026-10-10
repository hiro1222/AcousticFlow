// AcousticFlowWorld.cpp ── 仕組みの説明は AcousticFlowWorld.h の頭書き。ここは関数ごとの「何を・どうやって」。
#include "AcousticFlowWorld.h"
#include "AcousticFlowEmitter.h"
#include "AcousticFlowPlaceables.h"
#include "AcousticFlowSpace.h"
#include "acoustic_host.h"
#include "AkComponent.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Debug/DebugDrawService.h"
#include "GameFramework/PlayerController.h"

using namespace AcousticFlowSpace;

AAcousticFlowWorld::AAcousticFlowWorld()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;   // 扉（TG_PrePhysics）が動いた後で解く
}

AAcousticFlowWorld* AAcousticFlowWorld::Find(UWorld* InWorld)
{
	if (!InWorld) return nullptr;
	for (TActorIterator<AAcousticFlowWorld> It(InWorld); It; ++It) return *It;
	return nullptr;
}

/// 世界を作る。順番に意味がある（Unity の AcousticWorld.OnEnable と同じ）。
///   1) 材質（コンクリート・木の扉）→ 2) 印の付いた箱を全部渡す → 3) 部屋グラフ（数百 ms、ここだけ）
///   → 4) 部屋ごとの FDN と方向バス → 5) もう立っている音源を拾う。
void AAcousticFlowWorld::BeginPlay()
{
	Super::BeginPlay();
	World = AF_WorldCreate();
	if (!World) { UE_LOG(LogTemp, Error, TEXT("[AcousticFlow] 世界を作れない（AcousticEngine.dll）")); return; }
	CollectBoxes();
	ApplyRoomSettings(World);
	PushKnobs();
	AF_WorldSetWorkers(World, 1);
	AF_WorldBuild(World);
	CreateShared();
	AF_HostSetUnitsPerMeter(100.0f);             // 台帳は Unreal の cm で書く
	// 録画（起動の引数 -AFCapture のときだけ）: エンジンへ渡した物を記録し、撮り始めの合図を書く（AcousticFlowWorldCapture.cpp）。
	//   音は後で AfReplayWav が同じ API を呼び直して作る。Wwise の録音（StartOutputCapture）は -game で 32〜64 KB しか書かれず、
	//   出力の機器が止まっていると何も録れない（2026-10-04）。
	if (FParse::Param(FCommandLine::Get(), TEXT("AFCapture"))) BeginCapture();
	for (TObjectIterator<UAcousticFlowEmitter> It; It; ++It)
		if (It->GetWorld() == GetWorld() && It->VoiceHandle()) Register(*It);
	UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 世界: 箱 %d / 音源 %d"), Boxes.Num(), Emitters.Num());
	// 部屋ごとの大きさと RT60（6 帯域）をログへ。「部屋が 2 つに分かれているか」「響きの長さ」を試聴の前に確かめる。
	for (int32 r = 0; r < AF_WorldRoomCount(World); ++r)
	{
		float Rt[6] = {}, V = 0, S = 0, Mfp = 0;
		if (AF_WorldRoomInfo(World, r, Rt, &V, &S, &Mfp))
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 部屋 %d: 体積 %.1f m3 / 表面 %.1f m2 / RT60 %.2f %.2f %.2f %.2f %.2f %.2f s"), r, V, S, Rt[0], Rt[1], Rt[2], Rt[3], Rt[4], Rt[5]);
	}
	ComputeAreas(World);   // Play 中のエリアの表示（8 キー）

}

/// 片付け。★順番: 音源を外す → 世界を壊す → 器（FDN・方向バス・HRTF）。音源の Voice は各自が台帳に預ける。
void AAcousticFlowWorld::EndPlay(const EEndPlayReason::Type Reason)
{
	EndCapture();
	for (auto& E : Emitters) if (E.IsValid()) E->Detach();
	Emitters.Empty();
	AF_HostBegin(); AF_HostCommit();            // 台帳を空にして、プラグインが古い Voice を引かないように
	if (LabelDrawHandle.IsValid()) { UDebugDrawService::Unregister(LabelDrawHandle); LabelDrawHandle.Reset(); }
	if (World) { AF_WorldDestroy(World); World = nullptr; }
	// 器はプラグインが写しの中で握っているかもしれない。1 ブロック（約 21 ms）より十分長く待てないので、
	// ここでは壊さずに残す（プロセスの終わりに片付く。再生を繰り返しても数個の器が残るだけ）。
	Super::EndPlay(Reason);
}

/// 部品のタグで箱を選ぶ。AF_Wall = 動かない壁（AAcousticFlowWallBox は自分の材質、他はコンクリート）、AF_Door = 動く板（木の扉）。
void AAcousticFlowWorld::CollectBoxes()
{
	CollectBoxesInto(World, Boxes);
}

/// 部屋の割り方を渡す（組む前。Play の世界と下見の世界で同じ）。
void AAcousticFlowWorld::ApplyRoomSettings(AF_WorldHandle W) const
{
	AF_WorldSetRoomSeedRadius(W, RoomSeedRadiusM);
	AF_WorldSetOutsideMouth(W, bOutsideMouth ? 1 : 0);
}

/// 印の付いた箱を W に足して Out に控える（Play の世界とエディタの下見の世界で同じ拾い方をする）。
///   ★大きさはメッシュの境界箱 × 拡大率。向きは部品の右（Y）と上（Z）を写す（3 本目の前はエンジンが作る）。
///   ★材質は番号（AF_WorldAddMaterialPreset）ごとに 1 つ作って共有する。Unity は面ごとに 1 つ作る（実行中に 1 面だけ
///     書き換えるため）が、Unreal には実行中に材質を変える口が無いので共有でよい。
void AAcousticFlowWorld::CollectBoxesInto(AF_WorldHandle W, TArray<FBoxEntry>& Out) const
{
	Out.Empty();
	TMap<int32, int32> MatOfPreset;
	auto Mat = [&](int32 Preset) -> int32 {
		if (const int32* Found = MatOfPreset.Find(Preset)) return *Found;
		return MatOfPreset.Add(Preset, AF_WorldAddMaterialPreset(W, Preset));
	};
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* C = *It;
		if (C->GetWorld() != GetWorld() || !C->GetStaticMesh() || !IsValid(C)) continue;
		const bool bWall = C->ComponentHasTag(TEXT("AF_Wall"));
		const bool bDoor = C->ComponentHasTag(TEXT("AF_Door"));
		if (!bWall && !bDoor) continue;
		const FBox Local = C->GetStaticMesh()->GetBoundingBox();
		const FTransform T = C->GetComponentTransform();
		const FVector Ext = Local.GetExtent() * T.GetScale3D().GetAbs();
		const FQuat Q = T.GetRotation();
		const AF_Vector3 Center = ToEngine(T.TransformPosition(Local.GetCenter()));
		const AF_Vector3 Half{ float(Ext.Y * 0.01), float(Ext.Z * 0.01), float(Ext.X * 0.01) };
		int32 Preset = bDoor ? 4 : 1;                                   // 木の扉／コンクリート
		if (const AAcousticFlowWallBox* WB = Cast<AAcousticFlowWallBox>(C->GetOwner())) if (!bDoor) Preset = static_cast<int32>(WB->Material);
		const int32 Id = AF_WorldAddBox(W, Center, Half, DirToEngine(Q.GetAxisY()), DirToEngine(Q.GetAxisZ()),
		                                Mat(Preset), bDoor ? 1 : 0);
		Out.Add(FBoxEntry{ C, Id, bDoor, T, Preset });
	}
}

/// 部屋ごとの FDN と方向バスを作って繋ぐ（Unity の TailBusRenderer と同じ順）。
///   クロスオーバー → HRTF → FDN の接続。FDN はバスを差した時点の HRTF でレーンの量を揃えるので、この順を崩さない。
void AAcousticFlowWorld::CreateShared()
{
	Bus = AF_DirectionBusCreateVertical(SampleRate, 8, 4096, 2);
	if (Bus)
	{
		AF_DirectionBusSetCrossover(Bus, 700.0f);
		BusHrtf = AF_HrtfCreateSynthetic(SampleRate);
		AF_DirectionBusSetHrtf(Bus, BusHrtf, 57.0f);
		AF_DirectionBusSetPanSplit(Bus, bLanePanSplit ? 1 : 0);
	}
	RebindFdn();
}

/// FDN の器を作り直して、世界・方向バス・音源の全部を向け直す（部屋グラフが変わったとき）。古い器は 1 秒後に壊す。
void AAcousticFlowWorld::RebindFdn()
{
	AF_FdnMixHandle New = AF_FdnMixCreate(SampleRate, 4096, 0.6f);
	if (!New) return;
	if (Bus) AF_FdnMixSetDirectionBus(New, Bus, 57.0f);
	if (Fdn) RetiredFdn.Add(TPair<double, AF_FdnMixHandle>(FPlatformTime::Seconds(), Fdn));
	Fdn = New;
	AF_WorldBindFdn(World, Fdn);
	for (auto& E : Emitters) if (E.IsValid()) E->SetFdn(Fdn);
}

/// 摘みを押す（Unity の AcousticWorld の既定と同じ値）。値が変わったかを見ずに毎回押す（数 µs）。
void AAcousticFlowWorld::PushKnobs()
{
	const float W5[5] = { WeightDirect, WeightEarly, WeightLate, WeightDiffract, WeightTransmit };   // Unity の _w5 と同じ並び
	AF_WorldSetWeights(World, W5);
	AF_WorldSetAdjacentLateWeight(World, WeightLateAdjacent);   // 隣の部屋の響き（Unity の weightLateAdjacent と同じ）
	AF_WorldSetShadowMuffle(World, ShadowMuffleDb);             // 影のこもり（Unity の shadowMuffleDb と同じ）
	AF_WorldSetShadowMuffleFullHz(World, ShadowMuffleFullHz);   // 影のこもりが一番深くなる周波数（Unity の shadowMuffleFullHz と同じ）
	AF_WorldSetResponse(World, 0.0f, 0.04f, 0.05f, 0.06f);
	AF_WorldSetRays(World, RaysPerEmitter, 40);
	AF_WorldSetLeakModel(World, 1);
	AF_WorldSetLateThrough(World, 2);
	AF_WorldSetDoorPull(World, 0.0f);
	AF_WorldSetWallReflect(World, 0);
	AF_WorldSetEarlyModel(World, EarlyModel);
	AF_WorldSetImageSurface(World, 20.0f, 15.0f, 5.0f, 1.0f, 1.0f);
	AF_WorldSetAdjacentContain(World, AdjacentContain);
	AF_WorldSetDoorCoherence(World, 3000.0f);
	AF_WorldSetPrecedence(World, PrecedenceDb, 0.04f);
	AF_WorldSetLateDistanceShape(World, LateDistancePow);
	AF_WorldSetLaneModel(World, 1);
	AF_WorldSetHeadCm(World, 57.0f);
	AF_WorldSetBudget(World, 1536, 6, 10, 1);
	AF_WorldSetRayGroups(World, 4);
	AF_WorldSetMaxRaysPerEmitter(World, 512);
}

void AAcousticFlowWorld::Register(UAcousticFlowEmitter* E)
{
	if (!World || !E || !E->VoiceHandle() || Emitters.Contains(E)) return;
	const int32 Id = AF_WorldAddEmitter(World, ToEngine(E->GetOwner()->GetActorLocation()), E->RadiusM);
	if (Id < 0) return;
	Emitters.Add(E);
	E->Attach(Id, Fdn, Bus);
	CaptureEmitter(E, Id);
}

void AAcousticFlowWorld::Unregister(UAcousticFlowEmitter* E)
{
	if (!E || Emitters.Remove(E) == 0) return;
	if (World && E->EmitterId >= 0) AF_WorldRemoveEmitter(World, E->EmitterId);
	E->Detach();
}

/// 毎フレームの本体。押す順は Unity の AcousticWorld.Update と同じ。
///   1) 動く箱の位置だけ送る → 2) 聞き手（プレイヤーのカメラ）→ 3) 摘み → 4) 音源 → 5) AF_WorldUpdate を 1 回
///   → 6) 器が古いと言われたら繋ぎ直す → 7) 各音源へ配分 → 8) 台帳へ公開 → 9) 引退した器・Voice の片付け。
void AAcousticFlowWorld::Tick(float Dt)
{
	Super::Tick(Dt);
	// エディタ（Play していない）: エリアの下見だけ。ゲームの処理は Play 中だけ。
	if (!GetWorld() || !GetWorld()->IsGameWorld()) { TickEditorPreview(); return; }
	if (!World) return;

	for (const FBoxEntry& B : Boxes)
	{
		if (!B.bDynamic || !B.Comp.IsValid()) continue;
		const FTransform T = B.Comp->GetComponentTransform();
		const FBox Local = B.Comp->GetStaticMesh()->GetBoundingBox();
		const FQuat Q = T.GetRotation();
		AF_WorldSetBoxTransform(World, B.Id, ToEngine(T.TransformPosition(Local.GetCenter())), DirToEngine(Q.GetAxisY()), DirToEngine(Q.GetAxisZ()));
	}
	if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		const FRotationMatrix R(Cam->GetCameraRotation());
		// 耳の位置: 一人称はカメラ。三人称（bListenerAtPawn）はキャラクターの頭（目の高さ）で、向きはカメラ（画面の左右と音の左右を揃える）。
		//   ★台帳（Wwise の聞き手）はカメラのまま（PublishToHost）。プラグインは音の通し番号で結び付きを覚えるので、耳とカメラが離れてよい。
		FVector Ear = Cam->GetCameraLocation();
		if (bListenerAtPawn)
			if (APawn* P = UGameplayStatics::GetPlayerPawn(this, 0)) Ear = P->GetPawnViewLocation();
		ListenerPos = ToEngine(Ear);
		ListenerFwd = DirToEngine(R.GetScaledAxis(EAxis::X));
		ListenerUp = DirToEngine(R.GetScaledAxis(EAxis::Z));
		AF_WorldSetListener(World, ListenerPos, ListenerFwd, ListenerUp);
		// Wwise の聞き手はカメラの管理役に付く（Wwise の統合が付ける）。音源をその 1 人だけに繋ぐ（替わったときだけ触る）。
		//   7 キー: 試聴の切り分け用に「既定の全員」と行き来する（どちらの経路で鳴るかを耳と計器で比べる）。
		APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
		if (PC && PC->WasInputKeyJustPressed(EKeys::Seven))
		{
			bListenerCameraOnly = !bListenerCameraOnly;
			if (!bListenerCameraOnly) for (auto& E : Emitters) if (E.IsValid()) E->UseDefaultListeners();
		}
		if (bListenerCameraOnly)
			if (UAkComponent* WwiseListener = Cam->FindComponentByClass<UAkComponent>())
				for (auto& E : Emitters) if (E.IsValid()) E->BindListener(WwiseListener);
	}
	PushKnobs();
	for (auto& E : Emitters)
		if (E.IsValid() && E->EmitterId >= 0)
			AF_WorldSetEmitter(World, E->EmitterId, ToEngine(E->GetOwner()->GetActorLocation()), E->RadiusM, E->bOperated ? 1 : 0, E->Loudness);
	CaptureFrame(Dt);   // 録画中だけ（記録は AF_WorldUpdate の直前の状態）

	AF_WorldUpdate(World, FMath::Max(1e-4f, Dt));
	if (AF_WorldFdnStale(World)) { RebindFdn(); ComputeAreas(World); }   // 部屋グラフが組み直された ── エリアも引き直す

	// エリアの表示（8 キーで出し入れ）。響きの量（9 キーで 0 → −3 → −6 → −9 → −12 dB → 0 と回す。耳で決めるための切り替え）
	if (APlayerController* PC8 = UGameplayStatics::GetPlayerController(this, 0))
	{
		if (PC8->WasInputKeyJustPressed(EKeys::Eight)) bShowAreasInGame = !bShowAreasInGame;
		// 0 → −3 → −6 → −9 → −12 dB → 0（Unity の AcousticWorld.NextStepDb と同じ回し方）
		auto NextStep = [](float W) { const float Db = 10.0f * FMath::LogX(10.0f, FMath::Max(W, 1e-6f)); return (Db <= -11.5f) ? 0.0f : FMath::RoundToFloat(Db / 3.0f) * 3.0f - 3.0f; };
		if (PC8->WasInputKeyJustPressed(EKeys::Nine))
		{
			const float Next = NextStep(WeightLate);
			WeightLate = FMath::Pow(10.0f, Next / 10.0f);
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 響き（後期）の重み %.3f（%+.0f dB）"), WeightLate, Next);
		}
		if (PC8->WasInputKeyJustPressed(EKeys::Zero))
		{
			const float Next = NextStep(WeightLateAdjacent);
			WeightLateAdjacent = FMath::Pow(10.0f, Next / 10.0f);
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 隣の部屋の響きの重み %.3f（%+.0f dB）"), WeightLateAdjacent, Next);
		}
		// − キー: 影のこもりを 0 → 3 → 6 → 9 → 12 → 18 → 24 dB → 0（Unity の AcousticWorld.NextShadowDb と同じ回し方。2026-10-10 に 24 まで）
		if (PC8->WasInputKeyJustPressed(EKeys::Hyphen))
		{
			static const float Steps[] = { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 18.0f, 24.0f };
			float Next = 0.0f;
			for (float S : Steps) if (S > ShadowMuffleDb + 0.5f) { Next = S; break; }
			ShadowMuffleDb = Next;
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 影のこもり %.0f dB（%.0f Hz から上で）"), ShadowMuffleDb, ShadowMuffleFullHz);
		}
		// = キー: 影のこもりが一番深くなる周波数を 4000 → 2000 → 1000 → 500 Hz → 4000（Unity の AcousticWorld.NextShadowFullHz と同じ）
		if (PC8->WasInputKeyJustPressed(EKeys::Equals))
		{
			ShadowMuffleFullHz = (ShadowMuffleFullHz <= 750.0f) ? 4000.0f : FMath::RoundToFloat(ShadowMuffleFullHz / 2.0f);
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 影のこもりが一番深くなる周波数 %.0f Hz（%.0f dB）"), ShadowMuffleFullHz, ShadowMuffleDb);
		}
	}
	if (bShowAreasInGame) { EnsureLabelDraw(); DrawAreas(World); }
	for (auto& E : Emitters)
		if (E.IsValid() && E->EmitterId >= 0 && E->VoiceHandle())
			AF_WorldApplyVoice(World, E->EmitterId, E->VoiceHandle(), SampleRate);
	PublishToHost();

	AF_HostTick();
	const double Now = FPlatformTime::Seconds();

	// 画面の隅に状態を出す（Unity の AF ツールの「鳴らす器」の行に当たる物）。0.5 秒ごと。
	//   ★英数字だけにする（Unreal の既定のフォントには日本語の字形が無く、四角になる）。
	//   2 行目は試聴の診断: 部屋の分かれ方、プラグインの回数（1 つなら 48000/512 = 93.75 回/秒。2 倍なら器が 2 回回っている）、
	//   先頭の音源の五成分（音源の出力 1 に対する量、6 帯域の平均を dB）。同じ物をログにも 1 秒ごとに書く（こちらで読むため）。
	if (GEngine && Now - LastStatusTime >= 0.5)
	{
		const double Span = Now - LastStatusTime;
		LastStatusTime = Now;
		int32 Hit = 0, Miss = 0, Space = -1, Blocks = 0; float Err = 0.0f;
		AF_HostStats(&Hit, &Miss, &Space, &Err, &Blocks);
		const float BlocksPerSec = (LastBlocks > 0 && Span > 0.0) ? float((Blocks - LastBlocks) / Span) : 0.0f;
		LastBlocks = Blocks;
		// 途中で外れたブロック（素通しで生の音が出た数）。最後のブロックだけ見ていると、外れが見えない。
		long long TotM = 0, TotX = 0; float MaxErr = 0.0f;
		AF_HostTotals(&TotM, &TotX, &MaxErr);
		const long long MissedNow = TotX - LastTotMissed, MatchedNow = TotM - LastTotMatched;
		LastTotMissed = TotX; LastTotMatched = TotM;
		const bool bActive = AF_HostActive() != 0;
		const FString Line1 = FString::Printf(TEXT("AcousticFlow via Wwise: %s | matched %d / missed %d | missed blocks %lld of %lld (0.5 s) | lag max %.3f m | space %s | plugin %.1f blk/s | emitters %d | boxes %d | door 5/6 | 8=areas"),
			bActive ? TEXT("RENDERING") : TEXT("NO REPORT FROM PLUGIN"), Hit, Miss, MissedNow, MissedNow + MatchedNow, MaxErr,
			Space == 0 ? TEXT("world") : Space == 1 ? TEXT("listener") : TEXT("-"), BlocksPerSec, Emitters.Num(), Boxes.Num());
		GEngine->AddOnScreenDebugMessage(0x4146, 0.6f, (bActive && MissedNow == 0) ? FColor::Green : FColor::Orange, Line1);

		auto Db = [](const float* Six) { double S = 0.0; for (int b = 0; b < 6; ++b) S += Six[b]; return 10.0 * FMath::LogX(10.0, FMath::Max(S / 6.0, 1e-12)); };
		FString Mix = TEXT("no emitter");
		for (auto& E : Emitters)
		{
			if (!E.IsValid() || E->EmitterId < 0) continue;
			AF_MixInfo MI{};
			if (AF_WorldMixInfo(World, E->EmitterId, &MI))
				Mix = FString::Printf(TEXT("src room %d | direct %.1f dB | early %.1f | late %.1f | diffract %.1f | transmit %.1f | visible %.0f%%"),
					MI.room, Db(MI.component6[0]), Db(MI.component6[1]), Db(MI.component6[2]), Db(MI.component6[3]), Db(MI.component6[4]), MI.visibleFraction * 100.0f);
			break;
		}
		const float Open = AF_WorldApertureCount(World) > 0 ? AF_WorldApertureOpenFrac(World, 0) : -1.0f;
		const FString Line2 = FString::Printf(TEXT("rooms %d | listener room %d | %s | doorway open %.0f%%"),
			AF_WorldRoomCount(World), AF_WorldRoomAt(World, ListenerPos), *Mix, Open * 100.0f);
		GEngine->AddOnScreenDebugMessage(0x4147, 0.6f, FColor::Cyan, Line2);
		// 3 行目（黄）: 音の大きさの計器。「対応は取れているのに鳴らない」を、受けた音・Wwise の音量・出した音で切り分ける。
		int32 Inst = 0; float InRms = 0, Gain = 0, OutRms = 0;
		AF_HostLevels(&Inst, &InRms, &Gain, &OutRms);
		auto ToDb = [](float R) { return 20.0f * FMath::LogX(10.0f, FMath::Max(R, 1e-6f)); };
		//   ★実体は 1 つのはず。2 つ以上なら、バスに AF_Renderer が 2 段挿さっている（直列で 2 段目が 1 段目の音を消す）か、
		//     聞き手・出力が複数。09-30 は前者だった（tools/wwise_setup.py が枠を付け足していた）。
		const FString Line3 = FString::Printf(TEXT("LATE (reverb) %+.0f dB (9) | ADJACENT ROOM LATE %+.0f dB (0) | SHADOW MUFFLE %.0f dB (-) @ %.0f Hz (=) | plugin instances %d%s | in %.1f dBFS | wwise gain %.3f | out %.1f dBFS | wwise listener: %s (7)"),
			10.0f * FMath::LogX(10.0f, FMath::Max(WeightLate, 1e-6f)), 10.0f * FMath::LogX(10.0f, FMath::Max(WeightLateAdjacent, 1e-6f)), ShadowMuffleDb, ShadowMuffleFullHz,
			Inst, Inst > 1 ? TEXT(" (!! expected 1: check the AcousticFlow bus has ONE AF_Renderer)") : TEXT(""),
			ToDb(InRms), Gain, ToDb(OutRms), bListenerCameraOnly ? TEXT("camera only") : TEXT("default (all)"));
		GEngine->AddOnScreenDebugMessage(0x4148, 0.6f, FColor::Yellow, Line3);
		static double LastLog = 0.0;
		if (Now - LastLog >= 1.0)
		{
			LastLog = Now;
			UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] %s | listener (%.2f, %.2f, %.2f) | %s | %s"), *Line1, ListenerPos.x, ListenerPos.y, ListenerPos.z, *Line2, *Line3);
		}
	}
	for (int32 i = RetiredFdn.Num() - 1; i >= 0; --i)
		if (Now - RetiredFdn[i].Key >= 1.0) { AF_FdnMixDestroy(RetiredFdn[i].Value); RetiredFdn.RemoveAt(i); }
}

/// 台帳に今フレームの「どの Voice が今どこか」と聞き手・共有の器を書く。
///   ★座標は Wwise が見ている物＝ Unreal の cm・Z 上のまま（エンジンへ渡す m・y 上とは別）。
void AAcousticFlowWorld::PublishToHost()
{
	AF_HostBegin();
	if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		const FVector P = Cam->GetCameraLocation();
		const FRotationMatrix R(Cam->GetCameraRotation());
		const FVector F = R.GetScaledAxis(EAxis::X), U = R.GetScaledAxis(EAxis::Z);
		AF_HostSetListener(P.X, P.Y, P.Z, F.X, F.Y, F.Z, U.X, U.Y, U.Z);
	}
	for (auto& E : Emitters)
	{
		if (!E.IsValid() || E->EmitterId < 0 || !E->VoiceHandle()) continue;
		const FVector Q = E->GetOwner()->GetActorLocation();
		AF_HostPutVoice(E->EmitterId, E->VoiceHandle(), Q.X, Q.Y, Q.Z);
	}
	AF_HostSetShared(Fdn, Bus, SampleRate);
	AF_HostCommit();
}
