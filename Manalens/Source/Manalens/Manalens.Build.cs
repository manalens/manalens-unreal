using UnrealBuildTool;

public class Manalens : ModuleRules
{
	public Manalens(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"Json",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",
		});
	}
}
