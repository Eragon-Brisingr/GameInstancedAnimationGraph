#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "GIAG_SkeletalMeshUserData.generated.h"

UCLASS(BlueprintType, CollapseCategories)
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_SkeletalMeshUserData : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, meta=(ClampMin="0.0001", UIMin="0.0001"))
	float AnimationMinScreenSize = 0.0001f;

	/** Per-instance custom data float count for material parameters.
	 *  -1 = auto-detect from material's ScalarPrimitiveDataIndex / VectorPrimitiveDataIndex.
	 *  0  = no custom data.
	 *  >0 = explicit float count (clamped to FCustomPrimitiveData::NumCustomPrimitiveDataFloats). */
	UPROPERTY(EditAnywhere, meta=(ClampMin="-1", ClampMax="36"))
	int32 NumMaterialDataFloats = -1;
};

