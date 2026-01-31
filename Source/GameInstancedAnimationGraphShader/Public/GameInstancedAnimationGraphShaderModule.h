#pragma once

#include "Modules/ModuleManager.h"

class FGameInstancedAnimationGraphShaderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

