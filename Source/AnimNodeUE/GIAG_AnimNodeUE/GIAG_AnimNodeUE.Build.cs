// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GIAG_AnimNodeUE : ModuleRules
{
	public GIAG_AnimNodeUE(ReadOnlyTargetRules Target) : base(Target)
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
				"AnimGraphRuntime",

				"GameInstancedAnimationGraph",
			});
	}
}

