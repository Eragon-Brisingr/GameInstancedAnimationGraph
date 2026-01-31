#include "GameInstancedAnimationGraphShaderModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "GameInstancedAnimationGraph"

void FGameInstancedAnimationGraphShaderModule::StartupModule()
{
	// Make our plugin shader directory visible to the shader compiler.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GameInstancedAnimationGraph"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString ShaderDir = Plugin->GetBaseDir() / TEXT("Shaders") / TEXT("Common");
	AddShaderSourceDirectoryMapping(TEXT("/GameInstancedAnimationGraphShader"), ShaderDir);
}

void FGameInstancedAnimationGraphShaderModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGameInstancedAnimationGraphShaderModule, GameInstancedAnimationGraphShader)

