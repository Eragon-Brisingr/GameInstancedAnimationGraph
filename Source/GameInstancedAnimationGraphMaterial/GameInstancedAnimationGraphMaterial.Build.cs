using UnrealBuildTool;

public class GameInstancedAnimationGraphMaterial : ModuleRules
{
	public GameInstancedAnimationGraphMaterial(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
			});
	}
}
