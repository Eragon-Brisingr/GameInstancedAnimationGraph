#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialCachedData.h"
#include "GIAG_MaterialExpressionParameter.h"

class FGameInstancedAnimationGraphMaterialModule : public IModuleInterface
{
public:
	void StartupModule() override
	{
#if WITH_EDITOR
		OnCompilationFinishedHandle = UMaterial::OnMaterialCompilationFinished().AddStatic(
			&FGameInstancedAnimationGraphMaterialModule::OnMaterialCompiled);
#endif
	}

	void ShutdownModule() override
	{
#if WITH_EDITOR
		UMaterial::OnMaterialCompilationFinished().Remove(OnCompilationFinishedHandle);
#endif
	}

private:
#if WITH_EDITOR
	FDelegateHandle OnCompilationFinishedHandle;

	static void OnMaterialCompiled(UMaterialInterface* MaterialInterface)
	{
		UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
		if (!Material)
		{
			return;
		}
		if (Material->GetCachedExpressionData().bHasPerInstanceCustomData)
		{
			return;
		}

		for (UMaterialExpression* Expr : Material->GetExpressions())
		{
			if (Expr && Expr->IsA<UGIAG_MaterialExpressionParameterBase>())
			{
				UGIAG_MaterialExpressionParameterBase::PatchPerInstanceCustomDataFlag(Material);
				return;
			}
		}
	}
#endif
};

IMPLEMENT_MODULE(FGameInstancedAnimationGraphMaterialModule, GameInstancedAnimationGraphMaterial)
