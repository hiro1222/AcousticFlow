// AcousticFlowWorldCapture.cpp ── 録画（起動の引数 -AFCapture）のときに、エンジンへ渡した物を記録する。仕組みの説明は AcousticFlowWorld.h の頭書き。
//
// ■ 何を書くか（Saved/Wwise/AFCapture_scene.txt、1 行 1 件、空白区切り、座標はエンジンの m）
//   rate <標本化周波数>
//   room <部屋を割る半径> <外への口 0|1>
//   box <id> <中心 3> <半分の大きさ 3> <右の軸 3> <上の軸 3> <材質の番号> <動く 0|1>
//   knobs <直接 初期 後期 回折 透過> <隣の部屋の後期> <初期反射の出し方> <閉じ込め> <先着 dB> <響きの配り方 p> <レイ>   … 変わったときだけ
//   emitter <時刻> <id> <位置 3> <半径> <出力の音量>                                                                     … 登録したとき
//   f <時刻> <dt> <耳の位置 3> <前 3> <上 3>                                                                              … 毎フレーム
//   e <id> <位置 3> <半径> <操作中 0|1> <重要度>                                                                         … 毎フレーム、音源ごと
//   b <id> <中心 3> <右の軸 3> <上の軸 3>                                                                                 … 毎フレーム、動く箱ごと
//   時刻は録画の合図（AFCapture_info.txt の qpc）からの秒。
// ■ 誰が読むか
//   AfReplayWav（AcousticEngineTest/replay_capture.cpp）が同じ順で同じ API を呼び直して音にする。録画の道具が画面と合わせる。
//   ★出力の機器が止まっていても（2026-10-04 に Yamaha AG03 の時計が止まって Wwise が無音になった）、同じ歩き方の音が作れる。
#include "AcousticFlowWorld.h"
#include "AcousticFlowEmitter.h"
#include "AcousticFlowSpace.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using namespace AcousticFlowSpace;

namespace
{
	FString V3(const AF_Vector3& V) { return FString::Printf(TEXT("%.5f %.5f %.5f"), V.x, V.y, V.z); }
	FString A3(const float* V) { return FString::Printf(TEXT("%.5f %.5f %.5f"), V[0], V[1], V[2]); }
}

/// 1 行書く（UTF-8）。
void AAcousticFlowWorld::CaptureLine(const FString& Line)
{
	if (!CaptureLog) return;
	const FTCHARToUTF8 U(*(Line + TEXT("\n")));
	CaptureLog->Serialize(const_cast<ANSICHAR*>(U.Get()), U.Length());
}

/// 録画を始める: 合図（時刻）と場面の記録を開き、部屋の割り方・箱・摘みを書く。世界を組んだ直後、音源を拾う前に呼ぶ。
void AAcousticFlowWorld::BeginCapture()
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Wwise"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	CaptureLog = IFileManager::Get().CreateFileWriter(*(Dir / TEXT("AFCapture_scene.txt")));
	if (!CaptureLog) return;
	CaptureQpc = FPlatformTime::Cycles64();
	CaptureLine(FString::Printf(TEXT("rate %d"), SampleRate));
	CaptureLine(FString::Printf(TEXT("room %.4f %d"), RoomSeedRadiusM, bOutsideMouth ? 1 : 0));
	for (const FBoxEntry& B : Boxes)
	{
		AF_BoxInfo I{};
		if (!AF_WorldBoxInfo(World, B.Id, &I)) continue;
		CaptureLine(FString::Printf(TEXT("box %d %s %s %s %s %d %d"), B.Id, *A3(I.center), *A3(I.halfExtents), *A3(I.axisX), *A3(I.axisY), B.Preset, B.bDynamic ? 1 : 0));
	}
	// 合図は最後に（録画の道具はこれを見て撮り始める。時刻 0 は CaptureQpc）
	const FString Info = FString::Printf(TEXT("qpc=%llu\nfreq=%.0f\n"), CaptureQpc, 1.0 / FPlatformTime::GetSecondsPerCycle64());
	FFileHelper::SaveStringToFile(Info, *(Dir / TEXT("AFCapture_info.txt")));
	UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 録画の記録を始めた: %s"), *(Dir / TEXT("AFCapture_scene.txt")));
}

float AAcousticFlowWorld::CaptureTime() const
{
	return static_cast<float>(static_cast<double>(FPlatformTime::Cycles64() - CaptureQpc) * FPlatformTime::GetSecondsPerCycle64());
}

/// 音源を登録したときに書く。
void AAcousticFlowWorld::CaptureEmitter(const UAcousticFlowEmitter* E, int32 Id)
{
	if (!CaptureLog || !E || !E->GetOwner()) return;
	CaptureLine(FString::Printf(TEXT("emitter %.5f %d %s %.4f %.4f"), CaptureTime(), Id, *V3(ToEngine(E->GetOwner()->GetActorLocation())), E->RadiusM, E->OutputGain));
}

/// 毎フレーム、AF_WorldUpdate の直前に書く（耳・音源・動く箱、摘みは変わったときだけ）。
void AAcousticFlowWorld::CaptureFrame(float Dt)
{
	if (!CaptureLog) return;
	const FString Knobs = FString::Printf(TEXT("knobs %.5f %.5f %.5f %.5f %.5f %.5f %d %.4f %.3f %.3f %d %.3f"),
		WeightDirect, WeightEarly, WeightLate, WeightDiffract, WeightTransmit, WeightLateAdjacent, EarlyModel, AdjacentContain, PrecedenceDb, LateDistancePow, RaysPerEmitter, ShadowMuffleDb);
	if (Knobs != CaptureKnobs) { CaptureLine(Knobs); CaptureKnobs = Knobs; }
	CaptureLine(FString::Printf(TEXT("f %.5f %.6f %s %s %s"), CaptureTime(), Dt, *V3(ListenerPos), *V3(ListenerFwd), *V3(ListenerUp)));
	for (auto& E : Emitters)
		if (E.IsValid() && E->EmitterId >= 0)
			CaptureLine(FString::Printf(TEXT("e %d %s %.4f %d %.4f"), E->EmitterId, *V3(ToEngine(E->GetOwner()->GetActorLocation())), E->RadiusM, E->bOperated ? 1 : 0, E->Loudness));
	for (const FBoxEntry& B : Boxes)
	{
		if (!B.bDynamic) continue;
		AF_BoxInfo I{};
		if (AF_WorldBoxInfo(World, B.Id, &I)) CaptureLine(FString::Printf(TEXT("b %d %s %s %s"), B.Id, *A3(I.center), *A3(I.axisX), *A3(I.axisY)));
	}
}

/// 録画を終える（記録を閉じる）。
void AAcousticFlowWorld::EndCapture()
{
	if (!CaptureLog) return;
	CaptureLog->Close();
	delete CaptureLog;
	CaptureLog = nullptr;
	UE_LOG(LogTemp, Log, TEXT("[AcousticFlow] 録画の記録を閉じた"));
}
