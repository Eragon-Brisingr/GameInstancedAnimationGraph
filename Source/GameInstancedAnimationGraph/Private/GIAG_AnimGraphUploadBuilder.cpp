#include "GIAG_AnimGraphUploadBuilder.h"

FGIAG_AnimGraphUploads FGIAG_AnimGraphUploadBuilder::BuildUploads_GameThread(
	const FGIAG_AnimGraphCompiledData& CompiledData,
	TConstArrayView<uint8*> NodeData,
	TConstArrayView<uint32> NodeStrideBytes,
	int32 SlotCapacity,
	const FGIAG_AnimGraphRunParams& Params,
	TArray<int32>& InOutDirtyNodeIndices,
	TBitArray<>& InOutDirtyNodeMask,
	TArray<TBitArray<>>& InOutNodeParamDirtyBitsByNode,
	TArray<TArray<uint32>>& InOutDirtyNodeParamSlotsByNode,
	bool bUploadSkeletonStatic,
	const TArray<int32>& ParentIndices,
	const TArray<FGIAG_BoneTRS>& InverseRefPoseTRS,
	int32 SlotOffset)
{
	FGIAG_AnimGraphUploads Uploads;

	checkf(CompiledData.NumNodes > 0, TEXT("GIAG: invalid compiled graph (no nodes)."));
	checkf(NodeData.Num() == CompiledData.NumNodes, TEXT("GIAG: NodeData mismatch (Got=%d Expected=%d)."), NodeData.Num(), CompiledData.NumNodes);
	checkf(NodeStrideBytes.Num() == CompiledData.NumNodes, TEXT("GIAG: NodeStrideBytes mismatch (Got=%d Expected=%d)."), NodeStrideBytes.Num(), CompiledData.NumNodes);
	checkf(SlotCapacity > 0, TEXT("GIAG: invalid SlotCapacity."));

	if (bUploadSkeletonStatic)
	{
		Uploads.ParentIndices = ParentIndices;
		Uploads.InverseRefPoseTRS = InverseRefPoseTRS;
		Uploads.bUploadSkeleton = true;
	}

	// Initialized once (no runtime resizing/Reset).
	checkf(NodeParamStrideBytesByNode.Num() == CompiledData.NumNodes, TEXT("GIAG: UploadBuilder not initialized for this compiled graph."));
	checkf(InOutNodeParamDirtyBitsByNode.Num() == CompiledData.NumNodes, TEXT("GIAG: NodeParamDirtyBitsByNode size mismatch."));
	checkf(InOutDirtyNodeParamSlotsByNode.Num() == CompiledData.NumNodes, TEXT("GIAG: DirtyNodeParamSlotsByNode size mismatch."));
	checkf(InOutDirtyNodeMask.Num() == CompiledData.NumNodes, TEXT("GIAG: DirtyNodeMask size mismatch."));

	Uploads.NodeParamStrideBytesByNode.SetNumZeroed(CompiledData.NumNodes);

	// Event-driven param uploads: only iterate dirty nodes & slots (no full scan).
	for (const int32 NodeIdx : InOutDirtyNodeIndices)
	{
		check(NodeIdx >= 0 && NodeIdx < CompiledData.NumNodes);
		const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
		if (!Node.NodeMeta)
		{
			continue;
		}
		check(NodeData[NodeIdx] != nullptr);
		check(NodeStrideBytes[NodeIdx] > 0);

		TArray<uint32>& DirtySlots = InOutDirtyNodeParamSlotsByNode[NodeIdx];
		if (DirtySlots.Num() == 0)
		{
			continue;
		}

		// Determine stride lazily from a constructed instance.
		uint32& StrideBytes = NodeParamStrideBytesByNode[NodeIdx];
		if (StrideBytes == 0)
		{
			uint32 Stride = 0;
			const int32 FirstDirtySlot = (int32)DirtySlots[0];
			check(FirstDirtySlot >= 0 && FirstDirtySlot < SlotCapacity);
			void* NodeDataPtr = NodeData[NodeIdx] + (int64)NodeStrideBytes[NodeIdx] * (int64)FirstDirtySlot;
			Node.NodeMeta->GatherUploadsGPU(NodeDataPtr, Stride);
			StrideBytes = FMath::Max<uint32>(Stride, 1u);
		}
		Uploads.NodeParamStrideBytesByNode[NodeIdx] = StrideBytes;

		// Build a scatter upload: indices + packed blobs (StrideBytes per index).
		FGIAG_AnimGraphNodeUploadRun Run;
		Run.NodeIndex = NodeIdx;
		Run.StrideBytes = StrideBytes;
		Run.InstanceIndices.Reserve(DirtySlots.Num());
		Run.Bytes.SetNumUninitialized((int32)((uint64)StrideBytes * (uint64)DirtySlots.Num()));

		uint8* DestinationPtr = Run.Bytes.GetData();
		for (const uint32 SlotU : DirtySlots)
		{
			const int32 SlotIndex = (int32)SlotU;
			checkf(SlotIndex >= 0 && SlotIndex < SlotCapacity, TEXT("GIAG: invalid dirty slot %d (Cap=%d)."), SlotIndex, SlotCapacity);

			// Clear dirty bit as we consume (event-driven).
			TBitArray<>& Bits = InOutNodeParamDirtyBitsByNode[NodeIdx];
			check(Bits.IsValidIndex(SlotIndex));
			Bits[SlotIndex] = false;

			uint8* NodePtr = NodeData[NodeIdx] + (int64)NodeStrideBytes[NodeIdx] * (int64)SlotIndex;
			uint32 Stride = 0;
			const void* Blob = Node.NodeMeta->GatherUploadsGPU(NodePtr, Stride);
			checkf(Blob != nullptr, TEXT("GIAG: GatherUploadsGPU returned null for dirty node (Node=%d Slot=%d)."), NodeIdx, SlotIndex);
			checkf(Stride == 0 || Stride == StrideBytes, TEXT("GIAG: GatherUploadsGPU stride mismatch (Node=%d Slot=%d Gather=%u Expected=%u)."), NodeIdx, SlotIndex, Stride, StrideBytes);

			Run.InstanceIndices.Add(SlotU + (uint32)SlotOffset);
			FMemory::Memcpy(DestinationPtr, Blob, StrideBytes);
			DestinationPtr += StrideBytes;
		}
		DirtySlots.Reset();
		Uploads.NodeRuns.Add(MoveTemp(Run));
	}

	// Clear dirty node list/mask after consumption.
	for (const int32 NodeIdx : InOutDirtyNodeIndices)
	{
		check(InOutDirtyNodeMask.IsValidIndex(NodeIdx));
		InOutDirtyNodeMask[NodeIdx] = false;
	}
	InOutDirtyNodeIndices.Reset();

	return Uploads;
}

void FGIAG_AnimGraphUploadBuilder::MergeUploads(FGIAG_AnimGraphUploads& InOut, FGIAG_AnimGraphUploads&& Other)
{
	if (Other.bUploadSkeleton && !InOut.bUploadSkeleton)
	{
		InOut.ParentIndices = MoveTemp(Other.ParentIndices);
		InOut.InverseRefPoseTRS = MoveTemp(Other.InverseRefPoseTRS);
		InOut.bUploadSkeleton = true;
	}

	if (InOut.NodeParamStrideBytesByNode.Num() == 0 && Other.NodeParamStrideBytesByNode.Num() > 0)
	{
		InOut.NodeParamStrideBytesByNode = MoveTemp(Other.NodeParamStrideBytesByNode);
	}
	else if (Other.NodeParamStrideBytesByNode.Num() > 0)
	{
		for (int32 i = 0; i < Other.NodeParamStrideBytesByNode.Num() && i < InOut.NodeParamStrideBytesByNode.Num(); ++i)
		{
			if (InOut.NodeParamStrideBytesByNode[i] == 0)
			{
				InOut.NodeParamStrideBytesByNode[i] = Other.NodeParamStrideBytesByNode[i];
			}
		}
	}

	InOut.NodeRuns.Append(MoveTemp(Other.NodeRuns));
	InOut.ResourceRuns.Append(MoveTemp(Other.ResourceRuns));

	if (Other.MaxOptionalSRVSlot > InOut.MaxOptionalSRVSlot)
	{
		InOut.MaxOptionalSRVSlot = Other.MaxOptionalSRVSlot;
		InOut.OptionalSRVKeyByNodeBySlot = MoveTemp(Other.OptionalSRVKeyByNodeBySlot);
	}
}


