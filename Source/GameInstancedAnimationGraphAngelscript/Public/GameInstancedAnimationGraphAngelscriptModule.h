#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FGameInstancedAnimationGraphAngelscriptModule : public IModuleInterface
{
public:
    void StartupModule() override;
    void ShutdownModule() override;
};
