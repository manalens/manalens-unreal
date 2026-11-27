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
			// Steam Link: the generic online interfaces only (no Steam module). The
			// Steam Online Subsystem is used at run time when the game enables it.
			"OnlineSubsystem",
		});
	}
}
