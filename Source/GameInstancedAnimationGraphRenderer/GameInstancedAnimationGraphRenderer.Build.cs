// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GameInstancedAnimationGraphRenderer : ModuleRules
{
	public GameInstancedAnimationGraphRenderer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"GameInstancedAnimationGraph",
				"GameInstancedAnimationGraphShader",
				"RenderCore",
				"RHI",
				"Renderer",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
			});

		// We intentionally localize Renderer private header dependency to this module only.
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Renderer", "Private"));
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Renderer", "Internal"));
	}
}

