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
};

