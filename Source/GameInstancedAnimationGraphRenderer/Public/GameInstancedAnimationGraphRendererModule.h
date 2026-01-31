#pragma once

#include "Modules/ModuleManager.h"

class FGameInstancedAnimationGraphRendererModule : public IModuleInterface
{
public:
	void StartupModule() override;
	void ShutdownModule() override;
};

