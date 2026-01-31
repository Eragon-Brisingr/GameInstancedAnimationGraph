#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"

#include "GIAG_AnimSequenceUserData.generated.h"

UCLASS(BlueprintType, CollapseCategories)
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_AnimSequenceUserData : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, meta=(ClampMin="0.001", UIMin="0.001"))
	float SecondsPerFrame = 1.0f / 30.0f;
};

