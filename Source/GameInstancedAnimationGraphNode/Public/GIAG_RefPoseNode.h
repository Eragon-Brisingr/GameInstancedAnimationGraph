#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "GIAG_RefPoseNode.generated.h"

/**
 * RefPose node instance (0 inputs, 1 pose output).
 * Outputs local RefPose (ReferenceSkeleton::GetRefBonePose) in skeleton bone order.
 */
USTRUCT(BlueprintType)
struct alignas(16) GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_RefPoseNode final : public FGIAG_AnimNodeBase
{
	GENERATED_BODY()
public:
	using FNodeMeta = TGIAG_AnimNodeMeta<FGIAG_RefPoseNode>;

protected:
	friend FNodeMeta;

	/** No per-instance uploads. */
	const void* GatherUploadsGPU(uint32& OutUploadStrideBytes) const
	{
		OutUploadStrideBytes = 0;
		return nullptr;
	}

	static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context);
	static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context);
};

