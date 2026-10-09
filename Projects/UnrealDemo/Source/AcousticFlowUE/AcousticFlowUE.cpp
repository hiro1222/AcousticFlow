// AcousticFlowUE.cpp ── ゲームモジュール。起動時に自前のエンジン（AcousticEngine.dll）を読むだけ。
//
// ■ なぜ起動時に読むか
//   Wwise の自前プラグイン（AcousticFlow.dll）は、音のスレッドで GetModuleHandle("AcousticEngine.dll") を使ってエンジンを探す。
//   ゲームが同じ DLL を先に読んでおけば、プラグインとゲームが同じ台帳（acoustic_host.h）を見る。
//   遅延読み込みに任せると、最初にエンジンの関数を呼んだ時まで読まれず、その前に鳴り始めた音は素通しになる。
// ■ 壊れる所
//   ・DLL を解放しない。Wwise の音のスレッドが終了の直前まで使っているので、ゲーム側から先に外すと落ちる（プロセスの終わりに任せる）。
#include "Modules/ModuleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAcousticFlow, Log, All);

class FAcousticFlowUEModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/Win64/AcousticEngine.dll")));
		void* Handle = FPlatformProcess::GetDllHandle(*Path);
		// ★UE_LOG は中に if を持つマクロなので、波括弧なしの if … else にすると else の相手がずれる。
		if (Handle)
		{
			UE_LOG(LogAcousticFlow, Log, TEXT("AcousticEngine.dll を読んだ: %s"), *Path);
		}
		else
		{
			UE_LOG(LogAcousticFlow, Error, TEXT("AcousticEngine.dll が読めない: %s（build/bin/Release でエンジンをビルドしてから、このプロジェクトをビルドし直す）"), *Path);
		}
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FAcousticFlowUEModule, AcousticFlowUE, "AcousticFlowUE");
