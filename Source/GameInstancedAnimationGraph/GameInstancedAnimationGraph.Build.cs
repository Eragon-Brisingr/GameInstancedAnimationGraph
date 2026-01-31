// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GameInstancedAnimationGraph : ModuleRules
{
	public GameInstancedAnimationGraph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bValidateExperimentalApi = false;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"DeveloperSettings",
			});
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Niagara",
				"NiagaraCore",
				"RHI",
				"RenderCore",
				"Renderer",
				"Projects",
				
				"GameInstancedAnimationGraphShader",
			});

	}
}
