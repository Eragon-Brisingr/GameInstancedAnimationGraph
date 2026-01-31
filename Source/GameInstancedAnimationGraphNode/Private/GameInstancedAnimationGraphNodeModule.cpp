#include "GameInstancedAnimationGraphNodeModule.h"

#include "ShaderCore.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "GameInstancedAnimationGraph"

void FGameInstancedAnimationGraphNodeModule::StartupModule()
{
	// Make our plugin shader directory visible to the shader compiler.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GameInstancedAnimationGraph"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString ShaderDir = Plugin->GetBaseDir() / TEXT("Shaders") / TEXT("Nodes");
	AddShaderSourceDirectoryMapping(TEXT("/GameInstancedAnimationGraphNode"), ShaderDir);
}

void FGameInstancedAnimationGraphNodeModule::ShutdownModule()
{
}


#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGameInstancedAnimationGraphNodeModule, GameInstancedAnimationGraphNode)

