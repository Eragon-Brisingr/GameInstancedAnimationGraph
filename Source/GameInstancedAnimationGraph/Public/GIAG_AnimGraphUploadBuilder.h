#pragma once

#include "CoreMinimal.h"

#include "GIAG_AnimGraphGpuRunner.h"

/**
 * Game-thread-only upload builder.
 */
struct FGIAG_AnimGraphUploadBuilder
{
	/** Per-node param buffer stride (GT-derived; used to size RT buffers). Size = CompiledData.NumNodes. */
	TArray<uint32> NodeParamStrideBytesByNode;

public:
	/** One-time init: size per-node mirrors. (Optional shared resources are handled by Subsystem/SceneExtension.) */
	void Initialize(const FGIAG_AnimGraphCompiledData& CompiledData)
	{
		NodeParamStrideBytesByNode.SetNumZeroed(CompiledData.NumNodes);
		for (uint32& Stride : NodeParamStrideBytesByNode)
		{
			Stride = 0;
		}
	}

	FGIAG_AnimGraphUploads BuildUploads_GameThread(
		const FGIAG_AnimGraphCompiledData& CompiledData,
		TConstArrayView<uint8*> NodeData,
		TConstArrayView<uint32> NodeStrideBytes,
		int32 SlotCapacity,
		const FGIAG_AnimGraphRunParams& Params,
		/** Dirty node indices (unique) and mask (deduped). */
		TArray<int32>& InOutDirtyNodeIndices,
		TBitArray<>& InOutDirtyNodeMask,
		/** NodeIdx -> dirty slot bits/list (deduped). */
		TArray<TBitArray<>>& InOutNodeParamDirtyBitsByNode,
		TArray<TArray<uint32>>& InOutDirtyNodeParamSlotsByNode,
		bool bUploadSkeletonStatic,
		const TArray<int32>& ParentIndices,
		const TArray<FGIAG_BoneTRS>& InverseRefPoseTRS,
		int32 SlotOffset = 0);

	/** Merge another uploads result into an existing one (appends NodeRuns/ResourceRuns). */
	static void MergeUploads(FGIAG_AnimGraphUploads& InOut, FGIAG_AnimGraphUploads&& Other);
};

