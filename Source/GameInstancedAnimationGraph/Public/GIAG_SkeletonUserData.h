#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"

#include "GIAG_SkeletonUserData.generated.h"

UCLASS(BlueprintType, CollapseCategories)
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_SkeletonUserData : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="GameInstancedAnim")
	FRotator RootRotationOffset = FRotator::ZeroRotator;
};

