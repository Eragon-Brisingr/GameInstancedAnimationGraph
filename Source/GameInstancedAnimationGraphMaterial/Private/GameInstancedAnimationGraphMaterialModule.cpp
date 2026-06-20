#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInterface.h"
#include "MaterialCachedData.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "GIAG_MaterialExpressionParameter.h"

namespace
{
	bool HasPrimitiveDataParameter(const FMaterialCachedExpressionData& CachedData)
	{
		auto HasIndex = [](const TArray<int32>& Indices)
		{
			for (const int32 Idx : Indices)
			{
				if (Idx != INDEX_NONE)
				{
					return true;
				}
			}
			return false;
		};

		return HasIndex(CachedData.ScalarPrimitiveDataIndexValues)
			|| HasIndex(CachedData.VectorPrimitiveDataIndexValues);
	}

	bool PatchMaterialPerInstanceCustomDataFlag(UMaterialInterface* MaterialInterface)
	{
		if (!MaterialInterface)
		{
			return false;
		}

		const FMaterialCachedExpressionData& CachedData = MaterialInterface->GetCachedExpressionData();
		if (&CachedData == &FMaterialCachedExpressionData::EmptyData
			|| CachedData.bHasPerInstanceCustomData
			|| !HasPrimitiveDataParameter(CachedData))
		{
			return false;
		}

		// GIAG material expressions compile to PerInstanceCustomData, but UE's cached-data
		// scanner only marks native PerInstanceCustomData expression classes.
		const_cast<FMaterialCachedExpressionData&>(CachedData).bHasPerInstanceCustomData = true;
		return true;
	}
}

class FGameInstancedAnimationGraphMaterialModule : public IModuleInterface
{
public:
	void StartupModule() override
	{
		PackageLoadCompletedHandle = FCoreUObjectDelegates::OnPackageLoadCompleted.AddRaw(
			this, &FGameInstancedAnimationGraphMaterialModule::OnPackageLoadCompleted);
		PatchLoadedMaterials();

#if WITH_EDITOR
		OnCompilationFinishedHandle = UMaterial::OnMaterialCompilationFinished().AddStatic(
			&FGameInstancedAnimationGraphMaterialModule::OnMaterialCompiled);
#endif
	}

	void ShutdownModule() override
	{
		if (PackageLoadCompletedHandle.IsValid())
		{
			FCoreUObjectDelegates::OnPackageLoadCompleted.Remove(PackageLoadCompletedHandle);
			PackageLoadCompletedHandle.Reset();
		}

#if WITH_EDITOR
		UMaterial::OnMaterialCompilationFinished().Remove(OnCompilationFinishedHandle);
#endif
	}

private:
	FDelegateHandle PackageLoadCompletedHandle;

	static void PatchLoadedMaterials()
	{
		for (TObjectIterator<UMaterialInterface> It; It; ++It)
		{
			PatchMaterialPerInstanceCustomDataFlag(*It);
		}
	}

	void OnPackageLoadCompleted(UPackage* Package)
	{
		if (!Package)
		{
			return;
		}

		TArray<UObject*> ObjectsInPackage;
		GetObjectsWithPackage(Package, ObjectsInPackage, EGetObjectsFlags::None, RF_NoFlags, EInternalObjectFlags::Garbage);
		for (UObject* Object : ObjectsInPackage)
		{
			if (UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Object))
			{
				PatchMaterialPerInstanceCustomDataFlag(MaterialInterface);
			}
		}
	}

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
				PatchMaterialPerInstanceCustomDataFlag(Material);
				return;
			}
		}
	}
#endif
};

IMPLEMENT_MODULE(FGameInstancedAnimationGraphMaterialModule, GameInstancedAnimationGraphMaterial)
