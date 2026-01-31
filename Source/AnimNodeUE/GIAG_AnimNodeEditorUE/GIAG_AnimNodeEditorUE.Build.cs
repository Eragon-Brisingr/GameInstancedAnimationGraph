// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GIAG_AnimNodeEditorUE : ModuleRules
{
	public GIAG_AnimNodeEditorUE(ReadOnlyTargetRules Target) : base(Target)
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
				"UnrealEd",
				"BlueprintGraph",
				"KismetCompiler",
				"AnimGraph",
				
				"GameInstancedAnimationGraph",
				"GIAG_AnimNodeUE",
			});
	}
}

