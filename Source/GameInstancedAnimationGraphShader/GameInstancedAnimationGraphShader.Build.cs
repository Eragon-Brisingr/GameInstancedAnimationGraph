// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GameInstancedAnimationGraphShader : ModuleRules
{
	public GameInstancedAnimationGraphShader(ReadOnlyTargetRules Target) : base(Target)
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
				"DerivedDataCache",
				"RenderCore",
				"RHI",
				"Renderer",
				"Projects",
				"Engine",
				"HierarchyTableRuntime",
				"HierarchyTableAnimationRuntime",
			});

		// Needed for FShaderSerializeContext (RenderCore internal header).
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/RenderCore/Internal"));
	}
}

