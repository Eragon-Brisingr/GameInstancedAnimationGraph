// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GameInstancedAnimationGraphNode : ModuleRules
{
	public GameInstancedAnimationGraphNode(ReadOnlyTargetRules Target) : base(Target)
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
				"HierarchyTableRuntime",
				"HierarchyTableAnimationRuntime",
				"RenderCore",
				"RHI",
				"Projects",
				
				"GameInstancedAnimationGraph",
				"GameInstancedAnimationGraphShader",
			});

		// Make shader-side shared headers available to ISPC preprocessor (for Shared/GIAG_*.inl).
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "Shaders", "Common", "Shared"));
	}
}

