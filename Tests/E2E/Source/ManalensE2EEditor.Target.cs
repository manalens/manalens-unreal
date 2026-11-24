using UnrealBuildTool;

public class ManalensE2EEditorTarget : TargetRules
{
	public ManalensE2EEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ManalensE2E");
	}
}
