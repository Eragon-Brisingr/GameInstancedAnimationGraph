#include "GIAG_AnimGraphCpuRunner.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

#include "GIAG_PoseSpaceConvert.ispc.generated.h"
static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), "GIAG ISPC: FGIAG_BoneTRS layout mismatch.");

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

	static void ConvertLocalPoseToComponentPose(
		const FGIAG_BoneTRS* SourcePose,
		FGIAG_BoneTRS* DestinationPose,
		int32 NumBones,
		int32 NumInstances,
		int32 SlotCapacity,
		TConstArrayView<uint32> ActiveInstanceIndices,
		TConstArrayView<int32> ParentIndices)
	{
		ispc::GIAG_ConvertLocalToComponentPoseTRS(
			NumBones,
			NumInstances,
			SlotCapacity,
			(const uint32*)ActiveInstanceIndices.GetData(),
			(const int32*)ParentIndices.GetData(),
			(const ispc::FGIAG_BoneTRS*)SourcePose,
			(ispc::FGIAG_BoneTRS*)DestinationPose);
	}

	static void ConvertComponentPoseToLocalPose(
		const FGIAG_BoneTRS* SourcePose,
		FGIAG_BoneTRS* DestinationPose,
		int32 NumBones,
		int32 NumInstances,
		int32 SlotCapacity,
		TConstArrayView<uint32> ActiveInstanceIndices,
		TConstArrayView<int32> ParentIndices)
	{
		ispc::GIAG_ConvertComponentToLocalPoseTRS(
			NumBones,
			NumInstances,
			SlotCapacity,
			(const uint32*)ActiveInstanceIndices.GetData(),
			(const int32*)ParentIndices.GetData(),
			(const ispc::FGIAG_BoneTRS*)SourcePose,
			(ispc::FGIAG_BoneTRS*)DestinationPose);
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
				FUniqueResourceSource Source;
				Source.Request = Request;
				Source.Meta = Node.NodeMeta;
				Source.Settings = Node.Settings;
				UniqueResources.Add(Request.ShareKey, Source);
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
	for (const TPair<FGIAG_AnimResourceKey, FUniqueResourceSource>& Pair : UniqueResources)
	{
		const FGIAG_AnimResourceKey& ShareKey = Pair.Key;
		const FUniqueResourceSource& Source = Pair.Value;
		if (!Source.Meta)
		{
			continue;
		}

		TSharedPtr<void> Resource;
		const bool bOk = Source.Meta->BuildResourceForCPU(Source.Request, Source.Settings, Skeleton, Resource);
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
	check(CompiledData.PoseResourceTypes.Num() == CompiledData.NumPoseResources);
	checkf(
		GIAG_IsPosePinType(Params.RequestedFinalPoseType),
		TEXT("GIAG_AnimGraphCpuRunner: RequestedFinalPoseType must be pose-typed."));

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
	const bool bRequestFinalLocalPose = (Params.RequestedFinalPoseType == EGIAG_AnimPinType::LocalPose);
	int32 OriginalFinalPoseResource = INDEX_NONE;
	EGIAG_AnimPinType OriginalFinalPoseType = EGIAG_AnimPinType::LocalPose;
	bool bCanServeRequestedLocalPoseDirectly = false;
	if (CompiledData.FinalPoseOutput.NodeIndex >= 0)
	{
		check(CompiledData.Nodes.IsValidIndex(CompiledData.FinalPoseOutput.NodeIndex));
		const FGIAG_AnimCompiledNode& FinalOutputNode = CompiledData.Nodes[CompiledData.FinalPoseOutput.NodeIndex];
		check(FinalOutputNode.NodeMeta != nullptr);
		check(FinalOutputNode.OutputPoseResources.IsValidIndex(CompiledData.FinalPoseOutput.PinIndex));
		OriginalFinalPoseType = FinalOutputNode.NodeMeta->GetOutputPinType(CompiledData.FinalPoseOutput.PinIndex);
		checkf(
			GIAG_IsPosePinType(OriginalFinalPoseType),
			TEXT("GIAG_AnimGraphCpuRunner: FinalPoseOutput must be pose-typed."));
		OriginalFinalPoseResource = FinalOutputNode.OutputPoseResources[CompiledData.FinalPoseOutput.PinIndex];
		check(OriginalFinalPoseResource >= 0 && OriginalFinalPoseResource < PoseResources.Num());
		bCanServeRequestedLocalPoseDirectly = bRequestFinalLocalPose && (OriginalFinalPoseType == EGIAG_AnimPinType::LocalPose);
	}

	// Bitset layout (node-indexed):
	// - Pack 32 node flags into one uint32 word.
	// - Mask      = 1u << BitIndex
	const uint32 WordsPerSlot = bShouldCullNodes ? FMath::DivideAndRoundUp<uint32>((uint32)CompiledData.NumNodes, 32u) : 0u;
	TArray<uint32, TInlineAllocator<8>> AnyNodeWords;
	if (bShouldCullNodes)
	{
		AnyNodeWords.SetNumZeroed((int32)FMath::Max(1u, WordsPerSlot));

		auto SetNodeBit = [](TArray<uint32, TInlineAllocator<8>>& Words, uint32 NodeIdx)
		{
			Words[NodeIdx / 32] |= (1u << (NodeIdx % 32));
		};
		auto TestNodeBit = [](const TArray<uint32, TInlineAllocator<8>>& Words, uint32 NodeIdx) -> bool
		{
			return (Words[NodeIdx / 32] & (1u << (NodeIdx % 32))) != 0u;
		};

		TArray<uint32, TInlineAllocator<8>> LocalWords;
		LocalWords.SetNumZeroed(FMath::Max(1u, WordsPerSlot));

		// Batch-compute cull NeedMask for all cull-capable node types.
		// Uses precompiled CullDispatchSchedule/CullIndexByNode to avoid per-frame scans.
		check(CompiledData.NumInputPinsByNode.Num() == CompiledData.NumNodes);
		check(CompiledData.CullIndexByNode.Num() == CompiledData.NumNodes);

		TArray<uint32> CullNeedMasks;
		CullNeedMasks.SetNumUninitialized(FMath::Max(1, CompiledData.NumCullNodes * Params.NumInstances));

		if (CompiledData.NumCullNodes > 0)
		{
			FGIAG_AnimNodeCpuCullContext CullContext
			{
				.NumInstances = Params.NumInstances,
				.SlotCapacity = Params.SlotCapacity,
				.ActiveInstanceIndices = Params.ActiveInstanceIndices,
				.NumInputsByNode = CompiledData.NumInputPinsByNode,
				.NodeData = Params.NodeData,
				.NodeStrideBytes = Params.NodeStrideBytes,
			};

			for (const FGIAG_AnimDispatchBatch& Batch : CompiledData.CullDispatchSchedule)
			{
				check(Batch.NodeIndices.Num() > 0);
				const int32 FirstNodeIdx = Batch.NodeIndices[0];
				const IGIAG_AnimNodeMeta* Meta = CompiledData.Nodes[FirstNodeIdx].NodeMeta;
				check(Meta != nullptr && Meta->HasCullLogic());

				const int32 BaseCullIndex = CompiledData.CullIndexByNode[FirstNodeIdx];
				check(BaseCullIndex >= 0);

				const TConstArrayView<int32> NodeIndicesView = Batch.NodeIndices;
				TArrayView<uint32> OutView(
					CullNeedMasks.GetData() + (int64)BaseCullIndex * (int64)Params.NumInstances,
					(int32)((int64)Batch.NodeIndices.Num() * (int64)Params.NumInstances));
				Meta->ComputeCullNeedMasksCPU(CullContext, NodeIndicesView, OutView);
			}
		}

		for (int32 ActiveIndex = 0; ActiveIndex < Params.NumInstances; ++ActiveIndex)
		{
			const int32 SlotIndex = (int32)Params.ActiveInstanceIndices[ActiveIndex];
			check(SlotIndex >= 0 && SlotIndex < Params.SlotCapacity);

			// Start from the final node for this slot, then propagate dependencies backwards.
			for (int32 w = 0; w < (int32)WordsPerSlot; ++w) { LocalWords[w] = 0u; }
			SetNodeBit(LocalWords, CompiledData.FinalPoseOutput.NodeIndex);

			// Reverse topological order (batched by type): when a node is needed, mark its needed inputs as needed.
			// This is equivalent to iterating ExecOrder from back to front, but uses pre-built reverse batches.
			checkf(CompiledData.ReverseDispatchSchedule.Num() > 0, TEXT("GIAG CPU: ReverseDispatchSchedule must be built at compile time."));
			for (const FGIAG_AnimDispatchBatch& ReverseBatch : CompiledData.ReverseDispatchSchedule)
			{
				for (const int32 NodeIdx : ReverseBatch.NodeIndices)
				{
					check(NodeIdx >= 0 && NodeIdx < CompiledData.NumNodes);

					if (!TestNodeBit(LocalWords, NodeIdx))
					{
						continue;
					}

					const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
					check(Node.NodeMeta != nullptr);

					const uint32 NumInputs = (uint32)Node.NumInputPins;
					uint32 NeedMask = GIAG::AllInputsMask(NumInputs);

					// If this node type provides cull logic, look up the precomputed per-(node,active) need-mask.
					const int32 CullIdx = CompiledData.CullIndexByNode[NodeIdx];
					if (CullIdx != INDEX_NONE)
					{
						NeedMask = CullNeedMasks[(int64)CullIdx * (int64)Params.NumInstances + (int64)ActiveIndex];
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
							SetNodeBit(LocalWords, (uint32)SrcNode);
						}
					}
				}
			}

			for (int32 w = 0; w < (int32)WordsPerSlot; ++w)
			{
				AnyNodeWords[w] |= LocalWords[w];
			}
		}
	}

	auto IsNodeNeededAnySlot = [&](uint32 NodeIdx) -> bool
	{
		if (!bShouldCullNodes)
		{
			return true;
		}
		return (AnyNodeWords[NodeIdx / 32] & (1u << (NodeIdx % 32))) != 0u;
	};

	// Execute schedule (batch-based).
	for (const FGIAG_AnimDispatchBatch& Batch : CompiledData.DispatchSchedule)
	{
		if (Batch.Kind == EGIAG_AnimDispatchBatchKind::PoseSpaceConvert)
		{
			for (const int32 ConvertTaskIndex : Batch.ConvertTaskIndices)
			{
				check(CompiledData.PoseConvertTasks.IsValidIndex(ConvertTaskIndex));
				const FGIAG_AnimPoseConvertTask& ConvertTask = CompiledData.PoseConvertTasks[ConvertTaskIndex];
				if (bCanServeRequestedLocalPoseDirectly && ConvertTask.FirstConsumerNodeIndex == INDEX_NONE)
				{
					// Final-only convergence task is not needed when CPU caller explicitly requests local final pose.
					continue;
				}
				check(ConvertTask.SrcPoseResource >= 0 && ConvertTask.SrcPoseResource < PoseResources.Num());
				check(ConvertTask.DstPoseResource >= 0 && ConvertTask.DstPoseResource < PoseResources.Num());
				const TArray<FGIAG_BoneTRS>& SrcPose = PoseResources[ConvertTask.SrcPoseResource];
				TArray<FGIAG_BoneTRS>& DstPose = PoseResources[ConvertTask.DstPoseResource];

				if (ConvertTask.SrcPoseType == EGIAG_AnimPinType::LocalPose
					&& ConvertTask.DstPoseType == EGIAG_AnimPinType::ComponentPose)
				{
					ConvertLocalPoseToComponentPose(
						SrcPose.GetData(),
						DstPose.GetData(),
						Params.NumBones,
						Params.NumInstances,
						Params.SlotCapacity,
						Params.ActiveInstanceIndices,
						Params.ParentIndices);
				}
				else if (ConvertTask.SrcPoseType == EGIAG_AnimPinType::ComponentPose
					&& ConvertTask.DstPoseType == EGIAG_AnimPinType::LocalPose)
				{
					ConvertComponentPoseToLocalPose(
						SrcPose.GetData(),
						DstPose.GetData(),
						Params.NumBones,
						Params.NumInstances,
						Params.SlotCapacity,
						Params.ActiveInstanceIndices,
						Params.ParentIndices);
				}
				else
				{
					checkNoEntry();
				}
			}
			continue;
		}

		check(Batch.Kind == EGIAG_AnimDispatchBatchKind::Node);
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
				const EGIAG_AnimPinType InputType = CompiledNode.NodeMeta->GetInputPinType(InputPinIndex);
				if (!GIAG_IsPosePinType(InputType))
				{
					continue;
				}
				const int32 PoseResourceIndex = CompiledNode.InputPoseResources[InputPinIndex];
				if (PoseResourceIndex >= 0)
				{
					InPoses[InputPinIndex] =
					{
						.Data = PoseResources[PoseResourceIndex].GetData(),
						.PoseType = InputType,
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
				const EGIAG_AnimPinType OutputType = CompiledNode.NodeMeta->GetOutputPinType(OutputPinIndex);
				if (!GIAG_IsPosePinType(OutputType))
				{
					continue;
				}
				const int32 PoseResourceIndex = CompiledNode.OutputPoseResources[OutputPinIndex];
				if (PoseResourceIndex >= 0)
				{
					OutPoses[OutputPinIndex] =
					{
						.Data = PoseResources[PoseResourceIndex].GetData(),
						.PoseType = OutputType,
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
			.TimeSlots = Params.TimeSlots,
			.TimeSlotIndexBySlot = Params.TimeSlotIndexBySlot,
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
		if (bCanServeRequestedLocalPoseDirectly)
		{
			TArray<FGIAG_BoneTRS>& FinalLocalPose = PoseResources[OriginalFinalPoseResource];
			Outputs.FinalPose =
			{
				.Data = FinalLocalPose.GetData(),
				.PoseType = EGIAG_AnimPinType::LocalPose,
				.NumBones = Params.NumBones,
				.SlotCapacity = Params.SlotCapacity,
			};
			Outputs.FinalPoseType = EGIAG_AnimPinType::LocalPose;
			Outputs.FinalLocalPose = Outputs.FinalPose;
			return Outputs;
		}

		const EGIAG_AnimPinType FinalPoseType = CompiledData.FinalPoseType;
		const int32 FinalPoseRes = CompiledData.FinalPoseResource;
		checkf(
			FinalPoseType == EGIAG_AnimPinType::ComponentPose,
			TEXT("GIAG_AnimGraphCpuRunner: expected final pose type ComponentPose after compile-time convergence."));
		if (FinalPoseRes >= 0)
		{
			TArray<FGIAG_BoneTRS>& FinalComponentPose = PoseResources[FinalPoseRes];
			if (Params.RequestedFinalPoseType == EGIAG_AnimPinType::ComponentPose)
			{
				Outputs.FinalPose =
				{
					.Data = FinalComponentPose.GetData(),
					.PoseType = EGIAG_AnimPinType::ComponentPose,
					.NumBones = Params.NumBones,
					.SlotCapacity = Params.SlotCapacity,
				};
				Outputs.FinalPoseType = EGIAG_AnimPinType::ComponentPose;
				Outputs.FinalLocalPose = {};
			}
			else
			{
				checkf(
					Params.RequestedFinalPoseType == EGIAG_AnimPinType::LocalPose,
					TEXT("GIAG_AnimGraphCpuRunner: unsupported RequestedFinalPoseType=%u."),
					(uint32)Params.RequestedFinalPoseType);

				if (FinalLocalScratchSlotCapacity != Params.SlotCapacity || FinalLocalScratchNumBones != Params.NumBones)
				{
					EnsurePoseResourceSize(FinalLocalScratch, Params.SlotCapacity, Params.NumBones);
					FinalLocalScratchSlotCapacity = Params.SlotCapacity;
					FinalLocalScratchNumBones = Params.NumBones;
				}

				ConvertComponentPoseToLocalPose(
					FinalComponentPose.GetData(),
					FinalLocalScratch.GetData(),
					Params.NumBones,
					Params.NumInstances,
					Params.SlotCapacity,
					Params.ActiveInstanceIndices,
					Params.ParentIndices);

				Outputs.FinalPose =
				{
					.Data = FinalLocalScratch.GetData(),
					.PoseType = EGIAG_AnimPinType::LocalPose,
					.NumBones = Params.NumBones,
					.SlotCapacity = Params.SlotCapacity,
				};
				Outputs.FinalPoseType = EGIAG_AnimPinType::LocalPose;
				Outputs.FinalLocalPose = Outputs.FinalPose;
			}
		}
	}
	return Outputs;
}

