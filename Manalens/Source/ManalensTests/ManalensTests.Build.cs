using UnrealBuildTool;

// Automation tests of the Manalens client. Editor only: run them with
// UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests Manalens; Quit".
public class ManalensTests : ModuleRules
{
	public ManalensTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"Projects",
			"Manalens",
		});
	}
}
