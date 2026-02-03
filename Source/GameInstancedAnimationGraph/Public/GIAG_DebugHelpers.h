#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameInstancedAnimationGraphHandle.h"
#include "GIAG_DebugHelpers.generated.h"

/**
 * Debug helpers for inspecting GIAG LocalPose readbacks.
 */
UCLASS()
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_DebugHelpers final : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Draw skeleton as parent-child line segments using a LocalPoseTRS readback.
	 * ParentIndices are resolved from subsystem via Handle.
	 */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug", meta = (WorldContext = WorldContextObject))
	static void DrawDebugSkeletonFromLocalPoseTRS(const UObject* WorldContextObject, const FTransform& ComponentToWorld, const FGameInstancedAnimationGraphHandle& Handle, const TArray<FTransform3f>& LocalPoseTRS, FLinearColor Color = FLinearColor::Green, float Duration = 0.f, float Thickness = 0.f);
};

