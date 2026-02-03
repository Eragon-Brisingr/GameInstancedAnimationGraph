#pragma once

#include "CoreMinimal.h"
#include "GameInstancedAnimationGraphHandle.h"
#include "Animation/AnimNodeBase.h"
#include "AnimNode_GIAG_PoseFromHandle.generated.h"

/**
 * Source node: outputs GIAG CPU pose for a given handle.
 * - Preferred path: read pose from Subsystem's per-frame CPU cache.
 * - Fallback: if cache missing, evaluate this handle on CPU (single instance) and output.
 */
USTRUCT(BlueprintInternalUseOnly)
struct GIAG_ANIMNODEUE_API FAnimNode_GIAG_PoseFromHandle : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GIAG", meta=(PinShownByDefault))
	FGameInstancedAnimationGraphHandle Handle;
	
	UPROPERTY(Transient)
	TObjectPtr<UGameInstancedAnimationGraphSubsystem> Subsystem;

	// FAnimNode_Base
	void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	void Evaluate_AnyThread(FPoseContext& Output) override;
	void GatherDebugData(FNodeDebugData& DebugData) override;
};

