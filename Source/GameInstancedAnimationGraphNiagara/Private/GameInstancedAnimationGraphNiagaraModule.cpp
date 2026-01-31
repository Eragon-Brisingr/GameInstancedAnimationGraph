#include "GameInstancedAnimationGraphNiagaraModule.h"

#include "NiagaraDataInterfaceGIAGAttach.h"
#include "NiagaraTypeRegistry.h"

#define LOCTEXT_NAMESPACE "GameInstancedAnimationGraphNiagara"

void FGameInstancedAnimationGraphNiagaraModule::StartupModule()
{
	// Register our DI type so it appears in Niagara's allowed parameter type list.
	// Niagara parameter menus are driven by FNiagaraTypeRegistry::GetRegisteredParameterTypes().
	{
		const ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(UNiagaraDataInterfaceGIAGAttach::StaticClass(), Flags);
	}
}

void FGameInstancedAnimationGraphNiagaraModule::ShutdownModule()
{
    
}

#undef LOCTEXT_NAMESPACE
    
IMPLEMENT_MODULE(FGameInstancedAnimationGraphNiagaraModule, GameInstancedAnimationGraphNiagara)