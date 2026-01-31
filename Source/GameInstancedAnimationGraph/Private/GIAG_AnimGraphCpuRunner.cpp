#include "GIAG_AnimGraphCpuRunner.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

namespace
{
	static void EnsurePoseResourceSize(
		TArray<FGIAG_BoneTRS>& InOut,
		int32 SlotCapacity,
		int32 NumBones)
	{
		const int64 Needed = (int64)SlotCapacity * (int64)NumBones;
		if (InOut.Num() != Needed)
		{
			InOut.SetNumZeroed(Needed);
		}
	}
}

void FGIAG_AnimGraphCpuRunner::EnsureResourceCache(
	const FGIAG_AnimGraphCompiledData& CompiledData,
	USkeleton* Skeleton)
{
	if (CachedCompiled == &CompiledData && CachedSkeleton.Get() == Skeleton)
	{
		return;
	}

	CachedCompiled = &CompiledData;
	CachedSkeleton = Skeleton;

	ResourcesByKey.Reset();
	OptionalKeyByNodeBySlot.Reset();
	MaxOptionalSlot = -1;

	check(Skeleton);

	// Compute optional resource keys per node (mirrors UploadBuilder logic but stores CPU bytes).
	OptionalKeyByNodeBySlot.SetNum(CompiledData.NumNodes);

	// Collect unique sources (ShareKey -> request + source meta/settings).
	struct FUniqueResourceSource
	{
		FGIAG_AnimResourceRequest Request;
		const IGIAG_AnimNodeMeta* Meta = nullptr;
		FConstStructView Settings;
	};
	TMap<FGIAG_AnimResourceKey, FUniqueResourceSource> UniqueResources;

	for (int32 NodeIdx = 0; NodeIdx < CompiledData.NumNodes; ++NodeIdx)
	{
		const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
		if (!Node.NodeMeta)
		{
			continue;
		}

		TArray<FGIAG_AnimResourceRequest> Requests;
		Node.NodeMeta->EnumerateResourceRequests(Node.Settings, Skeleton, EGIAG_AnimResourceTarget::CPU, Requests);
		if (Requests.Num() == 0)
		{
			continue;
		}

		for (const FGIAG_AnimResourceRequest& Request : Requests)
		{
			if (Request.ShareKey.IsNone() || Request.Layout.Kind != EGIAG_AnimResourceKind::Buffer || Request.Layout.StrideBytes == 0 || Request.Layout.NumElements == 0)
			{
				continue;
			}

			MaxOptionalSlot = FMath::Max(MaxOptionalSlot, (int32)Request.Slot);

			if (!UniqueResources.Contains(Request.ShareKey))
			{
				FUniqueResourceSource Src;
				Src.Request = Request;
				Src.Meta = Node.NodeMeta;
				Src.Settings = Node.Settings;
				UniqueResources.Add(Request.ShareKey, Src);
			}
		}
	}

	if (MaxOptionalSlot >= 0)
	{
		for (int32 NodeIdx = 0; NodeIdx < CompiledData.NumNodes; ++NodeIdx)
		{
			OptionalKeyByNodeBySlot[NodeIdx].SetNumZeroed(MaxOptionalSlot + 1);
			const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
			if (!Node.NodeMeta)
			{
				continue;
			}

			TArray<FGIAG_AnimResourceRequest> Requests;
		Node.NodeMeta->EnumerateResourceRequests(Node.Settings, Skeleton, EGIAG_AnimResourceTarget::CPU, Requests);
			for (const FGIAG_AnimResourceRequest& Request : Requests)
			{
				if ((int32)Request.Slot <= MaxOptionalSlot)
				{
					OptionalKeyByNodeBySlot[NodeIdx][(int32)Request.Slot] = Request.ShareKey;
				}
			}
		}
	}

	// Build CPU bytes for all unique resources now (deterministic).
	for (const TPair<FGIAG_AnimResourceKey, FUniqueResourceSource>& KV : UniqueResources)
	{
		const FGIAG_AnimResourceKey& ShareKey = KV.Key;
		const FUniqueResourceSource& Src = KV.Value;
		if (!Src.Meta)
		{
			continue;
		}

		TSharedPtr<void> Resource;
		const bool bOk = Src.Meta->BuildResourceForCPU(Src.Request, Src.Settings, Skeleton, Resource);
		checkf(bOk && Resource.IsValid(),
			TEXT("GIAG CPU: failed to build CPU resource for optional ShareKey."));
		ResourcesByKey.Add(ShareKey, MoveTemp(Resource));
	}
}

FGIAG_AnimGraphCpuRunner::FOutputs FGIAG_AnimGraphCpuRunner::Evaluate(
	const FGIAG_AnimGraphCompiledData& CompiledData,
	const FGIAG_AnimGraphCpuRunParams& Params)
{
	check(Params.SkeletalMesh);
	check(Params.Skeleton);
	check(Params.SkeletalMesh->GetSkeleton() == Params.Skeleton);
	check(Params.Skeleton == Params.Skeleton); // keep style consistent with GPU runner (no defensive null toleration)
	check(Params.NumBones > 0);
	check(Params.SlotCapacity > 0);
	check(Params.NumInstances > 0);
	check(Params.ActiveInstanceIndices.Num() == Params.NumInstances);
	check(Params.ParentIndices.Num() == Params.NumBones);
	check(Params.ComponentToWorldBySlot.Num() == Params.SlotCapacity);
	check(Params.NodeData.Num() == CompiledData.NumNodes);
	check(Params.NodeStrideBytes.Num() == CompiledData.NumNodes);

	EnsureResourceCache(CompiledData, Params.Skeleton);

	// Pose resources (slot-indexed).
	if (PoseResources.Num() != CompiledData.NumPoseResources ||
		PoseResourcesSlotCapacity != Params.SlotCapacity ||
		PoseResourcesNumBones != Params.NumBones)
	{
		PoseResources.SetNum(CompiledData.NumPoseResources);
		for (int32 PoseIdx = 0; PoseIdx < PoseResources.Num(); ++PoseIdx)
		{
			EnsurePoseResourceSize(PoseResources[PoseIdx], Params.SlotCapacity, Params.NumBones);
		}
		PoseResourcesSlotCapacity = Params.SlotCapacity;
		PoseResourcesNumBones = Params.NumBones;
	}

	const bool bShouldCullNodes = (CompiledData.bEnableNodeCull && CompiledData.FinalPoseOutput.NodeIndex >= 0);

	// Bitset layout (node-indexed):
	// - Pack 32 node flags into one uint32 word.
	// - WordIndex = NodeIndex / 32  == (NodeIndex >> 5)
	// - BitIndex  = NodeIndex % 32  == (NodeIndex & 31)
	// - Mask      = 1u << BitIndex
	const uint32 WordsPerSlot = bShouldCullNodes ? FMath::DivideAndRoundUp<uint32>((uint32)CompiledData.NumNodes, 32u) : 0u;
	TArray<uint32, TInlineAllocator<8>> AnyNodeWords;
	if (bShouldCullNodes)
	{
		AnyNodeWords.SetNumZeroed((int32)FMath::Max(1u, WordsPerSlot));

		auto AllInputsMask = [](uint32 NumInputs) -> uint32
		{
			// bit i => input pin i is needed
			if (NumInputs >= 32u) { return 0xFFFFFFFFu; }
			return (NumInputs == 0u) ? 0u : ((1u << NumInputs) - 1u);
		};

		auto SetNodeBit = [](TArray<uint32, TInlineAllocator<8>>& Words, int32 NodeIdx)
		{
			const uint32 U = (uint32)NodeIdx;
			Words[(int32)(U >> 5)] |= (1u << (U & 31u));
		};
		auto TestNodeBit = [](const TArray<uint32, TInlineAllocator<8>>& Words, int32 NodeIdx) -> bool
		{
			const uint32 U = (uint32)NodeIdx;
			return (Words[(int32)(U >> 5)] & (1u << (U & 31u))) != 0u;
		};

		TArray<uint32, TInlineAllocator<8>> LocalWords;
		LocalWords.SetNumZeroed((int32)FMath::Max(1u, WordsPerSlot));

		for (const uint32 SlotU : Params.ActiveInstanceIndices)
		{
			const int32 SlotIndex = (int32)SlotU;
			check(SlotIndex >= 0 && SlotIndex < Params.SlotCapacity);

			// Start from the final node for this slot, then propagate dependencies backwards.
			for (int32 w = 0; w < (int32)WordsPerSlot; ++w) { LocalWords[w] = 0u; }
			SetNodeBit(LocalWords, CompiledData.FinalPoseOutput.NodeIndex);

			// Reverse topological order: when a node is needed, mark its needed inputs as needed.
			for (int32 i = CompiledData.ExecOrder.Num() - 1; i >= 0; --i)
			{
				const int32 NodeIdx = CompiledData.ExecOrder[i];
				check(NodeIdx >= 0 && NodeIdx < CompiledData.NumNodes);

				if (!TestNodeBit(LocalWords, NodeIdx))
				{
					continue;
				}

				const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
				const IGIAG_AnimNodeMeta* Meta = Node.NodeMeta;
				check(Meta != nullptr);

				const uint32 NumInputs = (uint32)Node.NumInputPins;
				uint32 NeedMask = AllInputsMask(NumInputs);

				// If this node provides cull logic, it may skip some inputs based on per-slot node state.
				if (Meta->HasCullLogic())
				{
					check(Params.NodeData[NodeIdx] != nullptr);
					const uint8* NodePtr = Params.NodeData[NodeIdx] + (int64)Params.NodeStrideBytes[NodeIdx] * (int64)SlotIndex;
					NeedMask = Meta->ComputeCullNeedMaskCPU(NumInputs, NodePtr);
				}

				check(Node.InputSources.Num() == Node.NumInputPins);
				for (uint32 Pin = 0; Pin < NumInputs; ++Pin)
				{
					if ((NeedMask & (1u << Pin)) == 0u)
					{
						continue;
					}

					const int32 SrcNode = Node.InputSources[(int32)Pin].NodeIndex;
					if (SrcNode >= 0)
					{
						SetNodeBit(LocalWords, SrcNode);
					}
				}
			}

			for (int32 w = 0; w < (int32)WordsPerSlot; ++w)
			{
				AnyNodeWords[w] |= LocalWords[w];
			}
		}
	}

	auto IsNodeNeededAnySlot = [&](int32 NodeIdx) -> bool
	{
		if (!bShouldCullNodes)
		{
			return true;
		}
		const uint32 U = (uint32)NodeIdx;
		return (AnyNodeWords[(int32)(U >> 5)] & (1u << (U & 31u))) != 0u;
	};

	// Execute schedule (batch-based).
	for (const FGIAG_AnimDispatchBatch& Batch : CompiledData.DispatchSchedule)
	{
		if (Batch.NodeIndices.Num() == 0)
		{
			continue;
		}

		TConstArrayView<int32> NodeIndicesToRun = Batch.NodeIndices;
		TArray<int32, TInlineAllocator<8>> FilteredNodeIndices;

		if (bShouldCullNodes)
		{
			// Skip nodes that are not needed by any active slot.
			FilteredNodeIndices.Reserve(Batch.NodeIndices.Num());
			for (int32 NodeIdx : Batch.NodeIndices)
			{
				if (IsNodeNeededAnySlot(NodeIdx))
				{
					FilteredNodeIndices.Add(NodeIdx);
				}
			}
			if (FilteredNodeIndices.Num() == 0)
			{
				continue;
			}
			NodeIndicesToRun = FilteredNodeIndices;
		}

		const int32 FirstNodeIdx = NodeIndicesToRun[0];
		checkf(FirstNodeIdx >= 0 && FirstNodeIdx < CompiledData.Nodes.Num(), TEXT("GIAG CPU: invalid FirstNodeIdx=%d."), FirstNodeIdx);
		const IGIAG_AnimNodeMeta* NodeMeta = CompiledData.Nodes[FirstNodeIdx].NodeMeta;
		checkf(NodeMeta != nullptr, TEXT("GIAG CPU: missing NodeMeta for node %d."), FirstNodeIdx);

		// Build per-node context arrays (match GPU runner shape).
		TArray<FConstStructView, TInlineAllocator<8>> NodeSettingsViews;
		TArray<TArray<FGIAG_CPUPoseBufferView, TInlineAllocator<4>>, TInlineAllocator<8>> BatchInputPoses;
		TArray<TArray<FGIAG_CPUPoseBufferView, TInlineAllocator<4>>, TInlineAllocator<8>> BatchOutputPoses;

		NodeSettingsViews.Reserve(NodeIndicesToRun.Num());
		BatchInputPoses.Reserve(NodeIndicesToRun.Num());
		BatchOutputPoses.Reserve(NodeIndicesToRun.Num());

		for (int32 NodeIdx : NodeIndicesToRun)
		{
			const FGIAG_AnimCompiledNode& CompiledNode = CompiledData.Nodes[NodeIdx];
			NodeSettingsViews.Add(CompiledNode.Settings);

			// Input poses
			TArray<FGIAG_CPUPoseBufferView, TInlineAllocator<4>> InPoses;
			InPoses.SetNum(CompiledNode.NumInputPins);
			for (int32 InputPinIndex = 0; InputPinIndex < CompiledNode.NumInputPins; ++InputPinIndex)
			{
				if (CompiledNode.NodeMeta->GetInputPinType(InputPinIndex) != EGIAG_AnimPinType::Pose)
				{
					continue;
				}
				const int32 PoseResourceIndex = CompiledNode.InputPoseResources[InputPinIndex];
				if (PoseResourceIndex >= 0)
				{
					InPoses[InputPinIndex] =
					{
						.Data = PoseResources[PoseResourceIndex].GetData(),
						.NumBones = Params.NumBones,
						.SlotCapacity = Params.SlotCapacity,
					};
				}
			}
			BatchInputPoses.Add(MoveTemp(InPoses));

			// Output poses
			TArray<FGIAG_CPUPoseBufferView, TInlineAllocator<4>> OutPoses;
			OutPoses.SetNum(CompiledNode.NumOutputPins);
			for (int32 OutputPinIndex = 0; OutputPinIndex < CompiledNode.NumOutputPins; ++OutputPinIndex)
			{
				if (CompiledNode.NodeMeta->GetOutputPinType(OutputPinIndex) != EGIAG_AnimPinType::Pose)
				{
					continue;
				}
				const int32 PoseResourceIndex = CompiledNode.OutputPoseResources[OutputPinIndex];
				if (PoseResourceIndex >= 0)
				{
					OutPoses[OutputPinIndex] =
					{
						.Data = PoseResources[PoseResourceIndex].GetData(),
						.NumBones = Params.NumBones,
						.SlotCapacity = Params.SlotCapacity,
					};
				}
			}
			BatchOutputPoses.Add(MoveTemp(OutPoses));
		}

		TArray<TConstArrayView<FGIAG_CPUPoseBufferView>, TInlineAllocator<8>> InViews;
		TArray<TConstArrayView<FGIAG_CPUPoseBufferView>, TInlineAllocator<8>> OutViews;
		InViews.Reserve(BatchInputPoses.Num());
		OutViews.Reserve(BatchOutputPoses.Num());
		for (int32 i = 0; i < BatchInputPoses.Num(); ++i) { InViews.Add(BatchInputPoses[i]); }
		for (int32 i = 0; i < BatchOutputPoses.Num(); ++i) { OutViews.Add(BatchOutputPoses[i]); }

		// Optional buffers per node (by logical slot).
		TArray<TArray<const void*, TInlineAllocator<8>>, TInlineAllocator<4>> OptionalPtrsBySlot;
		TArray<TArray<uint32, TInlineAllocator<8>>, TInlineAllocator<4>> OptionalNumBytesBySlot;
		TArray<TConstArrayView<const void*>, TInlineAllocator<4>> OptionalPtrViewsBySlot;
		TArray<TConstArrayView<uint32>, TInlineAllocator<4>> OptionalNumBytesViewsBySlot;
		TArray<TArray<TSharedPtr<void>, TInlineAllocator<8>>, TInlineAllocator<4>> OptionalResourcesBySlot;
		TArray<TConstArrayView<TSharedPtr<void>>, TInlineAllocator<4>> OptionalResourceViewsBySlot;

		if (MaxOptionalSlot >= 0 && OptionalKeyByNodeBySlot.Num() == CompiledData.NumNodes)
		{
			const int32 NumSlots = MaxOptionalSlot + 1;
			OptionalPtrsBySlot.SetNum(NumSlots);
			OptionalNumBytesBySlot.SetNum(NumSlots);
			OptionalPtrViewsBySlot.SetNum(NumSlots);
			OptionalNumBytesViewsBySlot.SetNum(NumSlots);
			OptionalResourcesBySlot.SetNum(NumSlots);
			OptionalResourceViewsBySlot.SetNum(NumSlots);

			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				OptionalPtrsBySlot[Slot].SetNumZeroed(NodeIndicesToRun.Num());
				OptionalNumBytesBySlot[Slot].SetNumZeroed(NodeIndicesToRun.Num());
				OptionalResourcesBySlot[Slot].SetNumZeroed(NodeIndicesToRun.Num());
			}

			for (int32 NodeInBatch = 0; NodeInBatch < NodeIndicesToRun.Num(); ++NodeInBatch)
			{
				const int32 NodeIdx = NodeIndicesToRun[NodeInBatch];
				const TArray<FGIAG_AnimResourceKey>& KeysBySlot = OptionalKeyByNodeBySlot[NodeIdx];
				for (int32 Slot = 0; Slot < NumSlots; ++Slot)
				{
					const FGIAG_AnimResourceKey ShareKey = KeysBySlot.IsValidIndex(Slot) ? KeysBySlot[Slot] : FGIAG_AnimResourceKey();
					if (ShareKey.IsNone())
					{
						continue;
					}
					TSharedPtr<void>* Found = ResourcesByKey.Find(ShareKey);
					checkf(Found && Found->IsValid(),
						TEXT("GIAG CPU: missing optional resource for ShareKey (expected built in EnsureResourceCache)."));
					OptionalResourcesBySlot[Slot][NodeInBatch] = *Found;
				}
			}

			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				OptionalPtrViewsBySlot[Slot] = OptionalPtrsBySlot[Slot];
				OptionalNumBytesViewsBySlot[Slot] = OptionalNumBytesBySlot[Slot];
				OptionalResourceViewsBySlot[Slot] = OptionalResourcesBySlot[Slot];
			}
		}

		FGIAG_AnimNodeCpuDispatchContext DispatchContext
		{
			.CurrentTimeSeconds = Params.CurrentTimeSeconds,
			.NumInstances = Params.NumInstances,
			.SlotCapacity = Params.SlotCapacity,
			.NumBones = Params.NumBones,
			.SkeletalMesh = Params.SkeletalMesh,
			.ParentIndices = Params.ParentIndices,
			.RefPoseLocal = Params.RefPoseLocal,
			.ComponentToWorldBySlot = Params.ComponentToWorldBySlot,
			.ActiveInstanceIndices = Params.ActiveInstanceIndices,
			.AnimSequencesByClipIndex = Params.AnimSequencesByClipIndex,
			.Compiled = &CompiledData,
			.NodeData = Params.NodeData,
			.NodeStrideBytes = Params.NodeStrideBytes,
			.NodeIndices = NodeIndicesToRun,
			.NodeSettingsPerNode = NodeSettingsViews,
			.InputPosesPerNode = InViews,
			.OutputPosesPerNode = OutViews,
			.OptionalBufferPtrsPerNodeBySlot = OptionalPtrViewsBySlot,
			.OptionalBufferNumBytesPerNodeBySlot = OptionalNumBytesViewsBySlot,
			.OptionalResourcesPerNodeBySlot = OptionalResourceViewsBySlot,
		};

		NodeMeta->AddPassesCPU(DispatchContext);
	}

	FOutputs Outputs;
	if (CompiledData.FinalPoseOutput.NodeIndex >= 0)
	{
		const FGIAG_AnimCompiledNode& FinalNode = CompiledData.Nodes[CompiledData.FinalPoseOutput.NodeIndex];
		const int32 FinalPoseRes = FinalNode.OutputPoseResources[CompiledData.FinalPoseOutput.PinIndex];
		if (FinalPoseRes >= 0)
		{
			Outputs.FinalLocalPose =
			{
				.Data = PoseResources[FinalPoseRes].GetData(),
				.NumBones = Params.NumBones,
				.SlotCapacity = Params.SlotCapacity,
			};
		}
	}
	return Outputs;
}

