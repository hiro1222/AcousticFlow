using UnrealBuildTool;

public class AcousticFlowUEEditorTarget : TargetRules
{
	public AcousticFlowUEEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("AcousticFlowUE");
	}
}
