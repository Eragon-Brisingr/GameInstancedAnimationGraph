#pragma once

#include "CoreMinimal.h"

#include "GIAG_AnimGraph.h"
#include "GIAG_AnimNodeBase.h"

class UAnimSequence;
class USkeleton;
class USkeletalMesh;

/**
 * CPU execution params for one evaluation.
 * CPU backend uses slot-indexed pose buffers, matching GIAG's compiled graph conventions.
 */
struct FGIAG_AnimGraphCpuRunParams
{
	int32 NumInstances = 0;  // active count
	int32 SlotCapacity = 0;  // slot capacity (for CPU we typically pack: SlotCapacity == NumInstances)
	int32 NumBones = 0;
	TConstArrayView<float> TimeSlots;

	USkeletalMesh* SkeletalMesh = nullptr;
	USkeleton* Skeleton = nullptr;

	/** Skeleton bone parent indices (bone -> parent). Size == NumBones. */
	TConstArrayView<int32> ParentIndices;

	/** Skeleton local RefPose (bone order == Skeleton reference skeleton). Size == NumBones. */
	TConstArrayView<FTransform> RefPoseLocal;

	/** ActiveIndex -> SlotIndex mapping (slot-indexed buffers). Size == NumInstances. */
	TConstArrayView<uint32> ActiveInstanceIndices;

	/** Per-slot TimeSlotIndex (SlotIndex -> TimeSlot index). Size == SlotCapacity. */
	TConstArrayView<uint8> TimeSlotIndexBySlot;

	/** Per-slot component transform. Size == SlotCapacity. */
	TConstArrayView<FTransform> ComponentToWorldBySlot;

	/** ClipIndex -> AnimSequence mapping. May be empty if graph does not use ClipPlayer. */
	TConstArrayView<const UAnimSequence*> AnimSequencesByClipIndex;

	/** Per-node AoS instance storage: NodeData[NodeIdx] points to SlotCapacity*StrideBytes bytes. */
	TConstArrayView<const uint8*> NodeData;
	TConstArrayView<uint32> NodeStrideBytes;

	/** Requested final CPU pose space. Default to ComponentPose to avoid extra conversion when caller does not need LocalPose. */
	EGIAG_AnimPinType RequestedFinalPoseType = EGIAG_AnimPinType::ComponentPose;
};

/**
 * CPU runner: executes a compiled AnimGraph on CPU, in the same batch order as the GPU runner.
 * Maintains per-graph persistent CPU buffers to avoid per-frame allocations.
 */
class GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimGraphCpuRunner
{
public:
	FGIAG_AnimGraphCpuRunner() = default;

	struct FOutputs
	{
		/** Slot-indexed final pose (NumBones transforms per slot). Space follows FinalPoseType. */
		FGIAG_CPUPoseBufferView FinalPose;
		EGIAG_AnimPinType FinalPoseType = EGIAG_AnimPinType::ComponentPose;
		/** Backward-compatible alias: set only when FinalPoseType == LocalPose. */
		FGIAG_CPUPoseBufferView FinalLocalPose;
	};

	/** Execute one evaluation on CPU. */
	FOutputs Evaluate(
		const FGIAG_AnimGraphCompiledData& CompiledData,
		const FGIAG_AnimGraphCpuRunParams& Params);

private:
	/** Ensure optional resource key mapping and CPU resource bytes cache are valid for (Compiled, Skeleton). */
	void EnsureResourceCache(
		const FGIAG_AnimGraphCompiledData& CompiledData,
		USkeleton* Skeleton);

private:
	// Cached graph identity.
	const FGIAG_AnimGraphCompiledData* CachedCompiled = nullptr;
	TWeakObjectPtr<USkeleton> CachedSkeleton;

	// Optional resources (shared) keyed by Request.ShareKey.
	TMap<FGIAG_AnimResourceKey, TSharedPtr<void>> ResourcesByKey;

	// [NodeIndex][Slot] -> ShareKey (IsNone = none)
	int32 MaxOptionalSlot = -1;
	TArray<TArray<FGIAG_AnimResourceKey>> OptionalKeyByNodeBySlot;

	// Persistent pose buffers: one per pose resource index.
	TArray<TArray<FGIAG_BoneTRS>> PoseResources;
	int32 PoseResourcesSlotCapacity = 0;
	int32 PoseResourcesNumBones = 0;

	// Scratch local-pose buffer used when the graph final pose is component-space.
	TArray<FGIAG_BoneTRS> FinalLocalScratch;
	int32 FinalLocalScratchSlotCapacity = 0;
	int32 FinalLocalScratchNumBones = 0;
};

