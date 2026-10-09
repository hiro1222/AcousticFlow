// AcousticFlowUE.Build.cs ── Unreal の試聴台のゲームモジュール。自前のエンジン（AcousticEngine.dll）と Wwise の Unreal プラグインに繋ぐ。
//
// ■ 繋ぎ方
//   ・エンジンのヘッダはリポジトリの AcousticEngine/include をそのまま読む（写さない。答えを 2 つにしない）。
//   ・エンジンは DLL。.lib で繋ぎ、遅延読み込みにして、モジュールの起動時に Binaries/Win64 の物を名前付きで読む。
//     ★Wwise の自前プラグイン（AcousticFlow.dll）はエンジンを GetModuleHandle("AcousticEngine.dll") で探すので、
//       ゲームが先に読んでおく必要がある（同じ DLL を共有しないと、台帳が別物になって何も引けない）。
//   ・DLL はビルドのたびに build/bin/Release から Binaries/Win64 へ写す（RuntimeDependencies）。
using System.IO;
using UnrealBuildTool;

public class AcousticFlowUE : ModuleRules
{
	public AcousticFlowUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "AkAudio" });

		// Source/AcousticFlowUE → Source → UnrealDemo → Projects → リポジトリの根
		string RepoRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
		string EngineInclude = Path.Combine(RepoRoot, "AcousticEngine", "include");
		string EngineBin = Path.Combine(RepoRoot, "build", "bin", "Release");

		PublicIncludePaths.Add(EngineInclude);
		PublicAdditionalLibraries.Add(Path.Combine(EngineBin, "AcousticEngine.lib"));
		PublicDelayLoadDLLs.Add("AcousticEngine.dll");
		RuntimeDependencies.Add("$(BinaryOutputDir)/AcousticEngine.dll", Path.Combine(EngineBin, "AcousticEngine.dll"));
	}
}
