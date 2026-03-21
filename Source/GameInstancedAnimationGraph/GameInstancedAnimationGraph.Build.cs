// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
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

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Make shader-side shared headers available to ISPC preprocessor (for Shared/GIAG_*.ush).
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "Shaders", "Common", "Shared"));

	}
}
