// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameInstancedAnimationGraphModule.h"

#include "GIAG_AnimNodeMetaManager.h"

#define LOCTEXT_NAMESPACE "GameInstancedAnimationGraph"

void FGameInstancedAnimationGraphModule::StartupModule()
{
	FGIAG_AnimNodeMetaManager::Get().InitManager();
}

void FGameInstancedAnimationGraphModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGameInstancedAnimationGraphModule, GameInstancedAnimationGraph)