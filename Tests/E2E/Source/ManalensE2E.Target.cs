using UnrealBuildTool;

public class ManalensE2ETarget : TargetRules
{
	public ManalensE2ETarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ManalensE2E");
	}
}
