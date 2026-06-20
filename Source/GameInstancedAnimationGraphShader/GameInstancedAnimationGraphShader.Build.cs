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
				"RenderCore",
				"RHI",
				"Renderer",
				"Projects",
				"Engine",
				"HierarchyTableRuntime",
				"HierarchyTableAnimationRuntime",
			});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("DerivedDataCache");
		}

		// Needed for FShaderSerializeContext (RenderCore internal header).
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/RenderCore/Internal"));
		// Needed for GetCompressedBoneTransform{SRV,UAV} helpers in Skinning/SkinningTransformProvider.h.
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
	}
}

