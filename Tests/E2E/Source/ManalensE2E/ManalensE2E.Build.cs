using UnrealBuildTool;

public class ManalensE2E : ModuleRules
{
	public ManalensE2E(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Manalens" });
	}
}
