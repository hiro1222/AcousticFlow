using UnrealBuildTool;

public class AcousticFlowUETarget : TargetRules
{
	public AcousticFlowUETarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("AcousticFlowUE");
	}
}
