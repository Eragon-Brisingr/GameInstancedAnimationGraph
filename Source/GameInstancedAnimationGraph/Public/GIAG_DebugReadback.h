#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimCommon.h"

/**
 * Debug readback result: Final LocalPoseTRS for one SlotIndex (one instance) from GPU evaluation.
 * Layout: one FGIAG_BoneTRS per bone.
 */
struct FGIAG_LocalPoseReadbackResult
{
	int32 RecordIndex = INDEX_NONE;
	int32 SerialNumber = INDEX_NONE;
	uint64 CpuRequestFrame = 0;
	uint32 NumBones = 0;
	TArray<FGIAG_BoneTRS> LocalPoseTRS;
};

/** Debug readback result: one Niagara attach FxTransform element decoded as FGIAG_Transform (world space). */
struct FGIAG_AttachFxTransformReadbackResult
{
	uint32 BucketId = 0;
	uint32 OutputIndex = 0;
	uint64 CpuRequestFrame = 0;
	FGIAG_Transform FxTransform = FGIAG_Transform::Identity;
};

/** Debug readback result: one native attach instance buffer element (Origin + 3x rows) decoded to world-space origin + basis rows. */
struct FGIAG_AttachInstanceBuffersReadbackResult
{
	uint32 BucketId = 0;
	uint32 OutputIndex = 0;
	uint64 CpuRequestFrame = 0;
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f Row0 = FVector3f::ZeroVector;
	FVector3f Row1 = FVector3f::ZeroVector;
	FVector3f Row2 = FVector3f::ZeroVector;
};

/**
 * Debug readback result: per-slot NeedNodeBits words for one SlotIndex.
 * Layout matches GPU cull mask: WordsPerSlot = ceil(NumNodes/32).
 */
struct FGIAG_NeedNodeBitsReadbackResult
{
	int32 RecordIndex = INDEX_NONE;
	int32 SerialNumber = INDEX_NONE;
	uint64 CpuRequestFrame = 0;
	uint32 SlotIndex = 0;
	uint32 NumNodes = 0;
	uint32 WordsPerSlot = 0;
	TArray<uint32> Words; // size == WordsPerSlot
};

/**
 * Thread-safe debug bus between RT (producer) and GT (consumer).
 * Latest-only caching is owned by UGameInstancedAnimationGraphSubsystem; this bus is just a queue.
 */
namespace GIAG::DebugReadback
{
	GAMEINSTANCEDANIMATIONGRAPH_API void EnqueueLocalPose(FGIAG_LocalPoseReadbackResult&& Result);
	GAMEINSTANCEDANIMATIONGRAPH_API bool DequeueLocalPose(FGIAG_LocalPoseReadbackResult& OutResult);

	GAMEINSTANCEDANIMATIONGRAPH_API void EnqueueAttachFxTransform(FGIAG_AttachFxTransformReadbackResult&& Result);
	GAMEINSTANCEDANIMATIONGRAPH_API bool DequeueAttachFxTransform(FGIAG_AttachFxTransformReadbackResult& OutResult);

	GAMEINSTANCEDANIMATIONGRAPH_API void EnqueueAttachInstanceBuffers(FGIAG_AttachInstanceBuffersReadbackResult&& Result);
	GAMEINSTANCEDANIMATIONGRAPH_API bool DequeueAttachInstanceBuffers(FGIAG_AttachInstanceBuffersReadbackResult& OutResult);

	GAMEINSTANCEDANIMATIONGRAPH_API void EnqueueNeedNodeBits(FGIAG_NeedNodeBitsReadbackResult&& Result);
	GAMEINSTANCEDANIMATIONGRAPH_API bool DequeueNeedNodeBits(FGIAG_NeedNodeBitsReadbackResult& OutResult);
}
