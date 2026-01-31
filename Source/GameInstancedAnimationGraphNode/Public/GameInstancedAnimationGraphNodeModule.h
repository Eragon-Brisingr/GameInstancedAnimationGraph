#pragma once

#include "Modules/ModuleManager.h"

class FGameInstancedAnimationGraphNodeModule : public IModuleInterface
{
public:
	void StartupModule() override;
	void ShutdownModule() override;
};

