#include "GIAG_AnimGraphGpuRunner.h"

#include "GIAG_AnimGraphShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Animation/Skeleton.h"
#include "HAL/IConsoleManager.h"
#include "StructUtils/StructView.h"

static TAutoConsoleVariable<int32> CVar_GIAG_DebugUploadStats(
	TEXT("GameInstancedAnim.DebugUploadStats"),
	0,
	TEXT("Log per-frame CPU->GPU upload volume for GameInstancedAnim AnimGraph runner. 0=off, 1=on."),
	ECVF_Default);

namespace
{
	static FRDGBufferRef CreateOrRegisterExternalBuffer(
		FRDGBuilder& GraphBuilder,
		TRefCountPtr<FRDGPooledBuffer>& External,
		const FRDGBufferDesc& Desc,
		const TCHAR* Name)
	{
		if (External.IsValid() && External->Desc == Desc)
		{
			return GraphBuilder.RegisterExternalBuffer(External, Name);
		}
		FRDGBufferRef NewBuf = GraphBuilder.CreateBuffer(Desc, Name);

		// Preserve per-slot persistent state across grow: when stride is unchanged we copy the
		// old prefix into the new (larger or smaller) buffer. Without this, slot-indexed buffers
		// such as ComponentToWorldBySlot/WorldToComponentBySlot/NodeParams retain garbage for any
		// slot whose owner did not happen to produce a dirty upload this frame, corrupting GPU
		// pose evaluation for stable instances after a bucket grow.
		if (External.IsValid() && External->Desc.BytesPerElement == Desc.BytesPerElement)
		{
			const uint64 OldNumBytes = (uint64)External->Desc.BytesPerElement * (uint64)External->Desc.NumElements;
			const uint64 NewNumBytes = (uint64)Desc.BytesPerElement * (uint64)Desc.NumElements;
			const uint64 CopyBytes = FMath::Min(OldNumBytes, NewNumBytes);
			if (CopyBytes > 0)
			{
				FRDGBufferRef OldBuf = GraphBuilder.RegisterExternalBuffer(External, TEXT("GIAG_AG_PrevBuffer"));
				AddCopyBufferPass(GraphBuilder, NewBuf, 0, OldBuf, 0, CopyBytes);
			}
		}

		External = GraphBuilder.ConvertToExternalBuffer(NewBuf);
		return NewBuf;
	}

	static void UploadStructuredBuffer(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef Dst,
		uint64 DstOffsetBytes,
		const TCHAR* UploadName,
		uint32 BytesPerElement,
		const void* Data,
		uint32 NumElements)
	{
		if (!Dst || !Data || NumElements == 0)
		{
			return;
		}
		const uint64 NumBytes = (uint64)BytesPerElement * (uint64)NumElements;
		FRDGBufferRef Upload = CreateStructuredBuffer(
			GraphBuilder,
			UploadName,
			BytesPerElement,
			NumElements,
			Data,
			NumBytes);
		AddCopyBufferPass(GraphBuilder, Dst, DstOffsetBytes, Upload, 0, NumBytes);
	}

	// AnimLibrary buffers are now cached/owned by the SceneExtension instance (per scene).
	// Runner only consumes buffers passed in by caller.

	static FRDGBufferRef CreateOrRegisterExternalBuffer_FromCache(
		FRDGBuilder& GraphBuilder,
		FGIAG_AnimResourceCache& Cache,
		const FGIAG_AnimResourceKey& ShareKey,
		const FRDGBufferDesc& Desc,
		const TCHAR* Name)
	{
		TRefCountPtr<FRDGPooledBuffer>* Found = Cache.Buffers.Find(ShareKey);
		if (Found && Found->IsValid() && (*Found)->Desc == Desc)
		{
			return GraphBuilder.RegisterExternalBuffer(*Found, Name);
		}

		FRDGBufferRef NewBuf = GraphBuilder.CreateBuffer(Desc, Name);
		Cache.Buffers.Add(ShareKey, GraphBuilder.ConvertToExternalBuffer(NewBuf));
		return NewBuf;
	}
}

FGIAG_AnimGraphGpuRunner::FOutputs FGIAG_AnimGraphGpuRunner::AddPasses_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FGIAG_AnimGraphCompiledData& CompiledData,
	const FGIAG_AnimGraphRunParams& Params,
	FGIAG_AnimGraphUploads&& Uploads,
	const GIAG::FAnimLibraryBuffers& AnimLibraryBuffers,
	FGIAG_AnimResourceCache& AnimResourceCache)
{
	FOutputs Outputs;
	checkf(IsValid(Params.Skeleton), TEXT("GIAG: AddPassesGPU called with null Skeleton."));
	checkf(Params.NumInstances > 0 && Params.SlotCapacity > 0 && Params.NumBones > 0,
		TEXT("GIAG: AddPassesGPU invalid counts (NumInstances=%d SlotCapacity=%d NumBones=%d)."),
		Params.NumInstances, Params.SlotCapacity, Params.NumBones);
	checkf(Params.ActiveInstanceIndices.Num() == Params.NumInstances,
		TEXT("GIAG: AddPassesGPU ActiveInstanceIndices mismatch (Active=%d NumInstances=%d)."),
		Params.ActiveInstanceIndices.Num(), Params.NumInstances);
	checkf(
		CompiledData.PoseResourceTypes.Num() == CompiledData.NumPoseResources,
		TEXT("GIAG: PoseResourceTypes mismatch (Types=%d PoseResources=%d)."),
		CompiledData.PoseResourceTypes.Num(),
		CompiledData.NumPoseResources);

	// Ensure persistent arrays are sized for this compiled graph (RT-only).
	if (Resources.PoseOutputs.Num() != CompiledData.NumPoseResources)
	{
		Resources.PoseOutputs.SetNum(CompiledData.NumPoseResources);
	}
	if (Resources.NodeParams.Num() != CompiledData.NumNodes)
	{
		Resources.NodeParams.SetNum(CompiledData.NumNodes);
		for (FGIAG_AnimGraphPersistentResources::FNodeParamResource& NodeParamResource : Resources.NodeParams)
		{
			NodeParamResource.Buffer.SafeRelease();
			NodeParamResource.StrideBytes = 0;
			NodeParamResource.NumInstances = 0;
		}
	}

	checkf(Params.OutputTransformBuffer != nullptr, TEXT("GIAG: OutputTransformBuffer must be set."));

	checkf(AnimLibraryBuffers.ClipMetas.IsValid()
		&& AnimLibraryBuffers.AnimTRS.IsValid()
		&& AnimLibraryBuffers.RefPoseLocalTRS.IsValid()
		&& AnimLibraryBuffers.NumBones != 0,
		TEXT("GIAG: AnimLibrary buffers not ready (Clip=%d Anim=%d RefPose=%d NumBones=%u)."),
		AnimLibraryBuffers.ClipMetas.IsValid() ? 1 : 0,
		AnimLibraryBuffers.AnimTRS.IsValid() ? 1 : 0,
		AnimLibraryBuffers.RefPoseLocalTRS.IsValid() ? 1 : 0,
		AnimLibraryBuffers.NumBones);

	uint64 UploadBytes = 0;
	uint32 UploadOps = 0;
	auto CountUpload = [&UploadBytes, &UploadOps](uint64 Bytes)
	{
		UploadBytes += Bytes;
		UploadOps += 1;
	};

	// Optional resources are uploaded by the SceneExtension prepass (global per-scene cache).

	// Ensure static buffers.
	FRDGBufferRef ParentRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.ParentIndices,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), Params.NumBones),
		TEXT("GIAG_AG_ParentIndices"));
	FRDGBufferRef InvRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.InverseRefPoseTRS,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_BoneTRS), Params.NumBones),
		TEXT("GIAG_AG_InvRefPose"));
	FRDGBufferRef WorldToComponentRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.WorldToComponentBySlot,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), Params.SlotCapacity),
		TEXT("GIAG_AG_WorldToComponentBySlot"));
	FRDGBufferRef ComponentToWorldRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.ComponentToWorldBySlot,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), Params.SlotCapacity),
		TEXT("GIAG_AG_ComponentToWorldBySlot"));

	{
		if (Uploads.bUploadSkeleton)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Upload Graph Resource");

			CountUpload((uint64)sizeof(int32) * (uint64)Uploads.ParentIndices.Num());
			CountUpload((uint64)sizeof(FGIAG_BoneTRS) * (uint64)Uploads.InverseRefPoseTRS.Num());
			UploadStructuredBuffer(GraphBuilder, ParentRDG, 0, TEXT("GIAG_AG_UploadParent"), sizeof(int32), Uploads.ParentIndices.GetData(), Uploads.ParentIndices.Num());
			UploadStructuredBuffer(GraphBuilder, InvRDG, 0, TEXT("GIAG_AG_UploadInvRef"), sizeof(FGIAG_BoneTRS), Uploads.InverseRefPoseTRS.GetData(), Uploads.InverseRefPoseTRS.Num());
		}

		RDG_EVENT_SCOPE(GraphBuilder, "Upload Instance Data");

		// Per-slot transforms
		if (Params.TransformUpload.IsValid() && Params.TransformUpload->DirtySlots.Num() > 0)
		{
			const FGIAG_TransformUploadData& TransformUp = *Params.TransformUpload;
			checkf((int32)TransformUp.SlotCapacity == Params.SlotCapacity,
				TEXT("GIAG: TransformUpload SlotCapacity mismatch (Upload=%u Params=%d)."),
				TransformUp.SlotCapacity, Params.SlotCapacity);
			checkf(TransformUp.DirtySlots.Num() == TransformUp.DirtyComponentToWorld.Num(),
				TEXT("GIAG: TransformUpload arrays mismatch (Slots=%d Transforms=%d)."),
				TransformUp.DirtySlots.Num(), TransformUp.DirtyComponentToWorld.Num());

			const int32 NumDirty = TransformUp.DirtySlots.Num();
			check(NumDirty > 0);

			TArray<uint32> SlotIndices;
			TArray<FGIAG_Transform> ValuesC2W;
			TArray<FGIAG_Transform> ValuesW2C;
			SlotIndices.SetNumUninitialized(NumDirty);
			ValuesC2W.SetNumUninitialized(NumDirty);
			ValuesW2C.SetNumUninitialized(NumDirty);

			for (int32 Idx = 0; Idx < NumDirty; ++Idx)
			{
				const uint32 SlotU = TransformUp.DirtySlots[Idx];
				checkf(SlotU < (uint32)Params.SlotCapacity, TEXT("GIAG: invalid dirty slot %u (Cap=%d)."), SlotU, Params.SlotCapacity);

				const FTransform3f C2W = TransformUp.DirtyComponentToWorld[Idx];
				const FTransform3f W2C = C2W.Inverse();

				SlotIndices[Idx] = SlotU;
				ValuesC2W[Idx] = C2W;
				ValuesW2C[Idx] = W2C;
			}

			CountUpload(sizeof(uint32) * (uint64)NumDirty);
			CountUpload(sizeof(FGIAG_Transform) * (uint64)NumDirty);
			CountUpload(sizeof(FGIAG_Transform) * (uint64)NumDirty);

			FRDGBufferRef IndicesRDG = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1, NumDirty)),
				TEXT("GIAG_AG_ScatterTransformSlots"));
			FRDGBufferRef ValuesC2WRDG = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), FMath::Max(1, NumDirty)),
				TEXT("GIAG_AG_ScatterTransformValuesC2W"));
			FRDGBufferRef ValuesW2CRDG = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), FMath::Max(1, NumDirty)),
				TEXT("GIAG_AG_ScatterTransformValuesW2C"));

			GraphBuilder.QueueBufferUpload(IndicesRDG, SlotIndices.GetData(), sizeof(uint32) * NumDirty, ERDGInitialDataFlags::None);
			GraphBuilder.QueueBufferUpload(ValuesC2WRDG, ValuesC2W.GetData(), sizeof(FGIAG_Transform) * NumDirty, ERDGInitialDataFlags::None);
			GraphBuilder.QueueBufferUpload(ValuesW2CRDG, ValuesW2C.GetData(), sizeof(FGIAG_Transform) * NumDirty, ERDGInitialDataFlags::None);

			GIAG::FScatterWriteTransformsBySlotPassParams ScatterParams;
			ScatterParams.NumWrites = (uint32)NumDirty;
			ScatterParams.OutputIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndicesRDG, PF_R32_UINT));
			ScatterParams.ValuesComponentToWorld = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ValuesC2WRDG));
			ScatterParams.ValuesWorldToComponent = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ValuesW2CRDG));
			ScatterParams.RW_ComponentToWorldBySlot = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ComponentToWorldRDG));
			ScatterParams.RW_WorldToComponentBySlot = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(WorldToComponentRDG));
			GIAG::AddScatterWriteTransformsBySlotPasses(GraphBuilder, ScatterParams);
		}
	}

	// AnimLibrary buffers are owned by RT cache; runner only consumes them.
	FRDGBufferRef ClipRDG = GraphBuilder.RegisterExternalBuffer(AnimLibraryBuffers.ClipMetas, TEXT("GIAG_AG_ClipMetas_External"));
	FRDGBufferRef AnimRDG = GraphBuilder.RegisterExternalBuffer(AnimLibraryBuffers.AnimTRS, TEXT("GIAG_AG_AnimTRS_External"));
	FRDGBufferRef RefPoseRDG = GraphBuilder.RegisterExternalBuffer(AnimLibraryBuffers.RefPoseLocalTRS, TEXT("GIAG_AG_RefPoseLocalTRS_External"));

	// Ensure pose output buffers.
	TArray<FRDGBufferSRVRef> PoseSRVs;
	TArray<FRDGBufferUAVRef> PoseUAVs;
	TArray<FRDGBufferRef> PoseRDGs;
	PoseSRVs.SetNum(CompiledData.NumPoseResources);
	PoseUAVs.SetNum(CompiledData.NumPoseResources);
	PoseRDGs.SetNum(CompiledData.NumPoseResources);
	const uint32 PoseTransforms = (uint32)Params.SlotCapacity * (uint32)Params.NumBones;
	for (int32 PoseIdx = 0; PoseIdx < CompiledData.NumPoseResources; ++PoseIdx)
	{
		FGIAG_AnimGraphPersistentResources::FPoseResource& PoseResource = Resources.PoseOutputs[PoseIdx];
		FRDGBufferRef PoseRDG = CreateOrRegisterExternalBuffer(
			GraphBuilder,
			PoseResource.Buffer,
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_BoneTRS), PoseTransforms),
			TEXT("GIAG_AG_Pose"));
		PoseResource.NumTransforms = PoseTransforms;
		PoseResource.PoseType = CompiledData.PoseResourceTypes[PoseIdx];
		PoseRDGs[PoseIdx] = PoseRDG;
		PoseSRVs[PoseIdx] = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PoseRDG));
		PoseUAVs[PoseIdx] = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PoseRDG));
	}

	// Ensure node param buffers and SRVs.
	TArray<FRDGBufferSRVRef> NodeParamSRVs;
	NodeParamSRVs.SetNum(CompiledData.NumNodes);
	TArray<FRDGBufferRef> NodeParamRDGs;
	NodeParamRDGs.SetNum(CompiledData.NumNodes);

	for (int32 NodeIdx = 0; NodeIdx < CompiledData.NumNodes; ++NodeIdx)
	{
		FGIAG_AnimGraphPersistentResources::FNodeParamResource& NodeParamResource = Resources.NodeParams[NodeIdx];
		checkf((int32)Uploads.NodeParamStrideBytesByNode.Num() == CompiledData.NumNodes, TEXT("GIAG: NodeParamStrideBytesByNode size mismatch."));
		const uint32 UploadStride = Uploads.NodeParamStrideBytesByNode[NodeIdx];
		if (UploadStride != 0u)
		{
			NodeParamResource.StrideBytes = UploadStride;
		}

		// Ensure nodes have correct param stride even before any slot becomes dirty.
		const FGIAG_AnimCompiledNode& CompiledNode = CompiledData.Nodes[NodeIdx];
		const bool bNeedsCullParams = (CompiledNode.NodeMeta != nullptr) && CompiledNode.NodeMeta->HasCullLogic();
		if (bNeedsCullParams && NodeParamResource.StrideBytes == 0u)
		{
			NodeParamResource.StrideBytes = CompiledNode.GpuUploadStrideBytes;
		}

		const uint32 Stride = FMath::Max(1u, NodeParamResource.StrideBytes);
		const uint32 NewCapacity = (uint32)Params.SlotCapacity;

		// Node params are stored as a strong-typed StructuredBuffer with element stride = Stride bytes.
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(Stride, NewCapacity);
		FRDGBufferRef NodeRDG = CreateOrRegisterExternalBuffer(GraphBuilder, NodeParamResource.Buffer, Desc, TEXT("GIAG_AG_NodeParams"));
		NodeParamResource.NumInstances = NewCapacity;

		NodeParamRDGs[NodeIdx] = NodeRDG;
		NodeParamSRVs[NodeIdx] = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NodeRDG));
	}

	// Upload sparse node params by index (scatter packed bytes).
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Upload Node Paramaters");
		for (const FGIAG_AnimGraphNodeUploadRun& Run : Uploads.NodeRuns)
		{
			checkf(Run.NodeIndex >= 0 && Run.NodeIndex < NodeParamRDGs.Num(), TEXT("GIAG: invalid NodeIndex=%d in upload run."), Run.NodeIndex);
			checkf(Run.StrideBytes > 0, TEXT("GIAG: invalid StrideBytes in node upload (Node=%d)."), Run.NodeIndex);
			checkf((Run.StrideBytes % 4u) == 0u, TEXT("GIAG: node param stride must be multiple of 4 for scatter upload (Node=%d Stride=%u)."), Run.NodeIndex, Run.StrideBytes);
			checkf(Run.InstanceIndices.Num() > 0, TEXT("GIAG: empty InstanceIndices in node upload (Node=%d)."), Run.NodeIndex);
			checkf((uint64)Run.Bytes.Num() == (uint64)Run.StrideBytes * (uint64)Run.InstanceIndices.Num(),
				TEXT("GIAG: node upload byte size mismatch (Node=%d Bytes=%d Stride=%u Indices=%d)."),
				Run.NodeIndex, Run.Bytes.Num(), Run.StrideBytes, Run.InstanceIndices.Num());

			const uint32 NumWrites = (uint32)Run.InstanceIndices.Num();
			const uint64 TotalBytes = (uint64)Run.StrideBytes * (uint64)NumWrites;
			CountUpload((uint64)sizeof(uint32) * (uint64)NumWrites);
			CountUpload(TotalBytes);

			FRDGBufferRef IndicesRDG = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1u, NumWrites)),
				TEXT("GIAG_AG_NodeParamScatterIndices"));
			GraphBuilder.QueueBufferUpload(IndicesRDG, Run.InstanceIndices.GetData(), sizeof(uint32) * NumWrites, ERDGInitialDataFlags::None);

			const uint32 NumDwords = (uint32)((TotalBytes + 3ull) / 4ull);
			FRDGBufferDesc ValuesDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, NumDwords));
			ValuesDesc.Usage |= (BUF_ShaderResource | BUF_ByteAddressBuffer);
			FRDGBufferRef ValuesRDG = GraphBuilder.CreateBuffer(ValuesDesc, TEXT("GIAG_AG_NodeParamScatterValuesBytes"));
			GraphBuilder.QueueBufferUpload(ValuesRDG, Run.Bytes.GetData(), (uint32)TotalBytes, ERDGInitialDataFlags::None);

			GIAG::FScatterWriteBytesByIndexPassParams ScatterParams;
			ScatterParams.NumWrites = NumWrites;
			ScatterParams.StrideBytes = Run.StrideBytes;
			ScatterParams.OutputIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndicesRDG, PF_R32_UINT));
			ScatterParams.ValuesBytes = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ValuesRDG, PF_R32_UINT));
			ScatterParams.RW_DstBytes = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NodeParamRDGs[Run.NodeIndex], PF_R32_UINT));
			GIAG::AddScatterWriteBytesByIndexPasses(GraphBuilder, ScatterParams);
		}
	}

	// SRVs for shared inputs.
	const FRDGBufferSRVRef ParentSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ParentRDG, PF_R32_SINT));
	const FRDGBufferSRVRef InvSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InvRDG));
	const FRDGBufferSRVRef WorldToComponentSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(WorldToComponentRDG));
	const FRDGBufferSRVRef ComponentToWorldSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ComponentToWorldRDG));
	const FRDGBufferSRVRef ClipSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ClipRDG));
	const FRDGBufferSRVRef AnimSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(AnimRDG));
	const FRDGBufferSRVRef RefPoseSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RefPoseRDG));

	Outputs.ParentIndicesSRV = ParentSRV;
	Outputs.ComponentToWorldBySlotSRV = ComponentToWorldSRV;
	Outputs.InverseRefPoseSRV = InvSRV;

	// ActiveInstanceIndices is always-on (ActiveIndex -> SlotIndex).
	const uint32 NumActive = (uint32)Params.ActiveInstanceIndices.Num();
	const uint32 ActiveCapacity = FMath::Max<uint32>(1u, (uint32)Params.SlotCapacity);
	FRDGBufferRef ActiveRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.ActiveInstanceIndices,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ActiveCapacity),
		TEXT("GIAG_AG_ActiveInstanceIndices"));

	bool bUploadActive = false;
	if (Resources.ActiveInstanceIndicesNum != NumActive || (uint32)Resources.ActiveInstanceIndicesCPU.Num() != NumActive)
	{
		bUploadActive = true;
	}
	else if (NumActive > 0)
	{
		bUploadActive = FMemory::Memcmp(Resources.ActiveInstanceIndicesCPU.GetData(), Params.ActiveInstanceIndices.GetData(), (SIZE_T)sizeof(uint32) * (SIZE_T)NumActive) != 0;
	}

	if (bUploadActive)
	{
		Resources.ActiveInstanceIndicesNum = NumActive;
		Resources.ActiveInstanceIndicesCPU.SetNumUninitialized((int32)NumActive);
		if (NumActive > 0)
		{
			FMemory::Memcpy(Resources.ActiveInstanceIndicesCPU.GetData(), Params.ActiveInstanceIndices.GetData(), (SIZE_T)sizeof(uint32) * (SIZE_T)NumActive);
		}

		CountUpload((uint64)sizeof(uint32) * (uint64)NumActive);
		UploadStructuredBuffer(
			GraphBuilder,
			ActiveRDG,
			0,
			TEXT("GIAG_AG_UploadActiveInstanceIndices"),
			sizeof(uint32),
			Resources.ActiveInstanceIndicesCPU.GetData(),
			NumActive);
	}

	FRDGBufferSRVRef ActiveIndicesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ActiveRDG));
	Outputs.ActiveInstanceIndicesSRV = ActiveIndicesSRV;

	// TimeSlotIndices: per-slot static mapping (SlotIndex -> TimeSlotIndex).
	const uint32 SlotCapU = FMath::Max<uint32>(1u, (uint32)Params.SlotCapacity);
	FRDGBufferRef TimeSlotIdxRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.TimeSlotIndices,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SlotCapU),
		TEXT("GIAG_AG_TimeSlotIndices"));

	{
		const int32 SrcNum = Params.TimeSlotIndexBySlot.Num();
		bool bUploadTSI = Resources.TimeSlotIndicesCPU.Num() != SrcNum;
		if (!bUploadTSI && SrcNum > 0)
		{
			bUploadTSI = FMemory::Memcmp(Resources.TimeSlotIndicesCPU.GetData(), Params.TimeSlotIndexBySlot.GetData(), (SIZE_T)SrcNum) != 0;
		}
		if (bUploadTSI && SrcNum > 0)
		{
			Resources.TimeSlotIndicesCPU.SetNumUninitialized(SrcNum);
			FMemory::Memcpy(Resources.TimeSlotIndicesCPU.GetData(), Params.TimeSlotIndexBySlot.GetData(), (SIZE_T)SrcNum);

			TArray<uint32, TInlineAllocator<128>> Expanded;
			Expanded.SetNumUninitialized(SrcNum);
			for (int32 i = 0; i < SrcNum; ++i)
			{
				Expanded[i] = (uint32)Resources.TimeSlotIndicesCPU[i];
			}

			CountUpload((uint64)sizeof(uint32) * (uint64)SrcNum);
			UploadStructuredBuffer(
				GraphBuilder,
				TimeSlotIdxRDG,
				0,
				TEXT("GIAG_AG_UploadTimeSlotIndices"),
				sizeof(uint32),
				Expanded.GetData(),
				(uint32)SrcNum);
		}
	}

	FRDGBufferSRVRef TimeSlotIndicesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TimeSlotIdxRDG));

	// ---------------------------------------------------------------------
	// GPU node culling mask (v2): build per-slot node-needed bitset.
	// ---------------------------------------------------------------------
	const uint32 NeedNodeWordsPerSlot = FMath::DivideAndRoundUp<uint32>((uint32)CompiledData.NumNodes, 32u);
	const uint32 NeedNodeTotalWords = NeedNodeWordsPerSlot * (uint32)Params.SlotCapacity;
	FRDGBufferRef NeedNodeBitsRDG = CreateOrRegisterExternalBuffer(
		GraphBuilder,
		Resources.NeedNodeBits,
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1u, NeedNodeTotalWords)),
		TEXT("GIAG_AG_NeedNodeBits"));
	Resources.NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;
	Resources.NeedNodeBitsNumNodes = (uint32)CompiledData.NumNodes;
	Resources.NeedNodeBitsSlotCapacity = (uint32)Params.SlotCapacity;
	Outputs.NeedNodeBitsBuffer = NeedNodeBitsRDG;
	Outputs.NeedNodeNumNodes = (uint32)CompiledData.NumNodes;
	Outputs.NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;

	const bool bHasFinalNode = (CompiledData.FinalPoseOutput.NodeIndex >= 0);

	FRDGBufferSRVRef NeedNodeBitsSRV = nullptr;
	if (NeedNodeWordsPerSlot > 0 && NeedNodeBitsRDG)
	{
		NeedNodeBitsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NeedNodeBitsRDG));
	}

	// Run culling prepass (GPU). If there is no final node, disable culling by filling all ones.
	const bool bShouldRunGraphCull = bHasFinalNode && CompiledData.bEnableNodeCull && CompiledData.GraphCullShaderMap.IsValid();

	{
		if (bShouldRunGraphCull)
		{
			// Build cull param SRVs in the exact declaration order for this permutation.
			TArray<FRDGBufferSRVRef> CullParamsInDeclOrder;
			CullParamsInDeclOrder.SetNumZeroed(CompiledData.CullParamNodeIndices.Num());
			for (int32 i = 0; i < CompiledData.CullParamNodeIndices.Num(); ++i)
			{
				const int32 NodeIdx = CompiledData.CullParamNodeIndices[i];
				checkf(NodeIdx >= 0 && NodeIdx < CompiledData.NumNodes, TEXT("GIAG: invalid CullParamNodeIndex=%d."), NodeIdx);
				checkf(NodeParamRDGs.IsValidIndex(NodeIdx) && NodeParamRDGs[NodeIdx] != nullptr, TEXT("GIAG: missing NodeParamRDG for cull param node %d."), NodeIdx);
				CullParamsInDeclOrder[i] = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NodeParamRDGs[NodeIdx]));
			}

			GIAG::FGraphCullPassParams CullParams;
			CullParams.ShaderMap = CompiledData.GraphCullShaderMap;
			CullParams.NumNodes = (uint32)CompiledData.NumNodes;
			CullParams.NumInstances = (uint32)Params.NumInstances;
			CullParams.SlotCapacity = (uint32)Params.SlotCapacity;
			CullParams.WordsPerSlot = NeedNodeWordsPerSlot;
			CullParams.FinalNodeIndex = (uint32)CompiledData.FinalPoseOutput.NodeIndex;
			CullParams.ActiveInstanceIndices = ActiveIndicesSRV;
			CullParams.CullParams = CullParamsInDeclOrder;
			CullParams.RW_NeedNodeBits = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NeedNodeBitsRDG));
			GIAG::AddGraphCullPasses(GraphBuilder, CullParams);

		}
		else
		{
			// No GraphCull (disabled or no cull-capable nodes): mark all nodes needed.
			GIAG::FFillUintBufferPassParams Fill;
			Fill.NumDwords = FMath::Max(1u, NeedNodeTotalWords);
			Fill.Value = 0xFFFFFFFFu;
			Fill.RW_Out = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NeedNodeBitsRDG));
			GIAG::AddFillUintBufferPasses(GraphBuilder, Fill);
		}
	}

	// Execute schedule.
	for (const FGIAG_AnimDispatchBatch& Batch : CompiledData.DispatchSchedule)
	{
		if (Batch.Kind == EGIAG_AnimDispatchBatchKind::PoseSpaceConvert)
		{
			for (const int32 ConvertTaskIndex : Batch.ConvertTaskIndices)
			{
				check(CompiledData.PoseConvertTasks.IsValidIndex(ConvertTaskIndex));
				const FGIAG_AnimPoseConvertTask& ConvertTask = CompiledData.PoseConvertTasks[ConvertTaskIndex];
				check(ConvertTask.SrcPoseResource >= 0 && ConvertTask.SrcPoseResource < PoseSRVs.Num());
				check(ConvertTask.DstPoseResource >= 0 && ConvertTask.DstPoseResource < PoseUAVs.Num());

				GIAG::FPoseSpaceConvertPassParams ConvertParams;
				ConvertParams.NumBones = (uint32)Params.NumBones;
				ConvertParams.NumInstances = (uint32)Params.NumInstances;
				ConvertParams.SourcePoseType = (ConvertTask.SrcPoseType == EGIAG_AnimPinType::ComponentPose) ? 1u : 0u;
				ConvertParams.DestinationPoseType = (ConvertTask.DstPoseType == EGIAG_AnimPinType::ComponentPose) ? 1u : 0u;
				ConvertParams.ActiveInstanceIndices = ActiveIndicesSRV;
				ConvertParams.ParentIndices = ParentSRV;
				ConvertParams.SourcePoseTRS = PoseSRVs[ConvertTask.SrcPoseResource];
				ConvertParams.RW_DestinationPoseTRS = PoseUAVs[ConvertTask.DstPoseResource];
				GIAG::AddPoseSpaceConvertPasses(GraphBuilder, ConvertParams);
			}
			continue;
		}

		check(Batch.Kind == EGIAG_AnimDispatchBatchKind::Node);
		if (Batch.NodeIndices.Num() == 0)
		{
			continue;
		}

		const int32 FirstNodeIdx = Batch.NodeIndices[0];
		checkf(FirstNodeIdx >= 0 && FirstNodeIdx < CompiledData.Nodes.Num(), TEXT("GIAG: invalid FirstNodeIdx=%d."), FirstNodeIdx);
		const IGIAG_AnimNodeMeta* NodeMeta = CompiledData.Nodes[FirstNodeIdx].NodeMeta;
		checkf(NodeMeta != nullptr, TEXT("GIAG: missing NodeMeta for node %d."), FirstNodeIdx);

		// Build per-node context arrays.
		TArray<FConstStructView> NodeSettingsViews;
		TArray<FRDGBufferSRVRef> BatchNodeParamSRVs;
		TArray<TArray<FGIAG_RDGPoseBuffer>> BatchInputPoses;
		TArray<TArray<FGIAG_RDGPoseBuffer>> BatchOutputPoses;

		NodeSettingsViews.Reserve(Batch.NodeIndices.Num());
		BatchNodeParamSRVs.Reserve(Batch.NodeIndices.Num());
		BatchInputPoses.Reserve(Batch.NodeIndices.Num());
		BatchOutputPoses.Reserve(Batch.NodeIndices.Num());

		for (int32 NodeIdx : Batch.NodeIndices)
		{
			const FGIAG_AnimCompiledNode& CompiledNode = CompiledData.Nodes[NodeIdx];
			NodeSettingsViews.Add(CompiledNode.Settings);
			BatchNodeParamSRVs.Add(NodeParamSRVs[NodeIdx]);

			// Input poses
			TArray<FGIAG_RDGPoseBuffer> InPoses;
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
					InPoses[InputPinIndex].SRV = PoseSRVs[PoseResourceIndex];
					InPoses[InputPinIndex].PoseType = InputType;
				}
			}
			BatchInputPoses.Add(MoveTemp(InPoses));

			// Output poses
			TArray<FGIAG_RDGPoseBuffer> OutPoses;
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
					OutPoses[OutputPinIndex].UAV = PoseUAVs[PoseResourceIndex];
					OutPoses[OutputPinIndex].PoseType = OutputType;
				}
			}
			BatchOutputPoses.Add(MoveTemp(OutPoses));
		}

		TArray<TConstArrayView<FGIAG_RDGPoseBuffer>> InViews;
		TArray<TConstArrayView<FGIAG_RDGPoseBuffer>> OutViews;
		InViews.Reserve(BatchInputPoses.Num());
		OutViews.Reserve(BatchOutputPoses.Num());
		for (int32 i = 0; i < BatchInputPoses.Num(); ++i) { InViews.Add(BatchInputPoses[i]); }
		for (int32 i = 0; i < BatchOutputPoses.Num(); ++i) { OutViews.Add(BatchOutputPoses[i]); }

		// Optional resources per node (by logical slot).
		TArray<TArray<FRDGBufferSRVRef>> OptionalSRVsBySlot;
		TArray<TConstArrayView<FRDGBufferSRVRef>> OptionalSRVViewsBySlot;
		if (Uploads.MaxOptionalSRVSlot >= 0 && Uploads.OptionalSRVKeyByNodeBySlot.Num() == CompiledData.NumNodes)
		{
			const int32 NumSlots = Uploads.MaxOptionalSRVSlot + 1;
			OptionalSRVsBySlot.SetNum(NumSlots);
			OptionalSRVViewsBySlot.SetNum(NumSlots);

			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				OptionalSRVsBySlot[Slot].SetNumZeroed(Batch.NodeIndices.Num());
			}

			for (int32 NodeInBatch = 0; NodeInBatch < Batch.NodeIndices.Num(); ++NodeInBatch)
			{
				const int32 NodeIdx = Batch.NodeIndices[NodeInBatch];
				if (!Uploads.OptionalSRVKeyByNodeBySlot.IsValidIndex(NodeIdx))
				{
					continue;
				}
				const TArray<FGIAG_AnimResourceKey>& KeysBySlot = Uploads.OptionalSRVKeyByNodeBySlot[NodeIdx];
				for (int32 Slot = 0; Slot < NumSlots; ++Slot)
				{
					const FGIAG_AnimResourceKey ShareKey = KeysBySlot.IsValidIndex(Slot) ? KeysBySlot[Slot] : FGIAG_AnimResourceKey();
					if (ShareKey.IsNone())
					{
						continue;
					}

					TRefCountPtr<FRDGPooledBuffer>* Found = AnimResourceCache.Buffers.Find(ShareKey);
					checkf(Found && Found->IsValid(),
						TEXT("GIAG: missing optional resource for ShareKey (expected uploaded in SceneExtension prepass)."));

					FRDGBufferRef RDG = GraphBuilder.RegisterExternalBuffer(*Found, TEXT("GIAG_AG_OptionalResource_External"));
					OptionalSRVsBySlot[Slot][NodeInBatch] = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RDG));
				}
			}

			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				OptionalSRVViewsBySlot[Slot] = OptionalSRVsBySlot[Slot];
			}
		}

		FGIAG_AnimNodeDispatchContext DispatchContext
		{
			.GraphBuilder = GraphBuilder,
			.TimeSlots = MakeArrayView(Params.TimeSlots),
			.TimeSlotIndicesSRV = TimeSlotIndicesSRV,
			.NumInstances = Params.NumInstances,
			.NumBones = Params.NumBones,
			.ParentIndicesSRV = ParentSRV,
			.WorldToComponentBySlotSRV = WorldToComponentSRV,
			.ActiveInstanceIndicesSRV = ActiveIndicesSRV,
			.NeedNodeBitsSRV = NeedNodeBitsSRV,
			.NeedNodeWordsPerSlot = NeedNodeWordsPerSlot,
			.NodeParamSRVsPerNode = BatchNodeParamSRVs,
			.OptionalBufferSRVsPerNodeBySlot = OptionalSRVViewsBySlot,
			.ClipMetasSRV = ClipSRV,
			.AnimTRSSRV = AnimSRV,
			.NumClips = Params.NumClips,
			.RefPoseLocalTRSSRV = RefPoseSRV,
			.NodeIndices = Batch.NodeIndices,
			.NodeSettingsPerNode = NodeSettingsViews,
			.InputPosesPerNode = InViews,
			.OutputPosesPerNode = OutViews,
			.InputBoneWeightsPerNode = TConstArrayView<TConstArrayView<FGIAG_RDGBoneWeights>>(),
		};
		NodeMeta->AddPassesGPU(DispatchContext);
	}

	// Finalize:
	// - component pose -> UE TransformBuffer (RWByteAddressBuffer).
	if (CompiledData.FinalPoseOutput.NodeIndex >= 0)
	{
		const EGIAG_AnimPinType FinalPoseType = CompiledData.FinalPoseType;
		const int32 FinalPoseRes = CompiledData.FinalPoseResource;
		checkf(
			FinalPoseType == EGIAG_AnimPinType::ComponentPose,
			TEXT("GIAG_AnimGraphGpuRunner: expected final pose type ComponentPose after compile-time convergence."));

		if (FinalPoseRes >= 0)
		{
			Outputs.FinalPoseBuffer = PoseRDGs[FinalPoseRes];
			Outputs.FinalPoseType = FinalPoseType;
			const int32 FinalLocalRes = CompiledData.FinalLocalPoseResource;
			Outputs.FinalLocalPoseBuffer = (FinalLocalRes >= 0 && FinalLocalRes < PoseRDGs.Num())
				? PoseRDGs[FinalLocalRes] : nullptr;

			GIAG::FPoseToTransformBufferPassParams TBParams;
			TBParams.NumBones = (uint32)Params.NumBones;
			TBParams.NumInstances = (uint32)Params.NumInstances;
			TBParams.ActiveInstanceIndices = ActiveIndicesSRV;
			TBParams.InverseRefPoseTRS = InvSRV;
			TBParams.PoseTRS = PoseSRVs[FinalPoseRes];
			TBParams.TransformBuffer = Params.OutputTransformBuffer;

			TBParams.BaseTransformOffset = Params.BaseTransformOffset;
			TBParams.BasePreviousTransformOffset = Params.BasePreviousTransformOffset;
			TBParams.bWritePreviousTransforms = Params.bWritePreviousTransforms ? 1u : 0u;
			TBParams.MaxTransformCount = Params.MaxTransformCount;

			GIAG::AddPoseToTransformBufferPasses(GraphBuilder, TBParams);

			// Slots that just (re)entered GPU evaluation this frame: copy Current -> Previous so their
			// stale Previous region doesn't smear for one frame. Per-slot, runs only on switch/add frames.
			// Reuse the uploaded slot buffer for the follower prime pass (same slot indices).
			if (Params.ReenteredSlots.Num() > 0)
			{
				const uint32 NumReentered = (uint32)Params.ReenteredSlots.Num();
				FRDGBufferRef ReenteredRDG = CreateStructuredBuffer(
					GraphBuilder,
					TEXT("GIAG_ReenteredSlots"),
					sizeof(uint32),
					NumReentered,
					Params.ReenteredSlots.GetData(),
					(uint64)sizeof(uint32) * (uint64)NumReentered);
				FRDGBufferSRVRef ReenteredSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReenteredRDG));

				GIAG::FPrimePreviousTransformsPassParams PrimeParams;
				PrimeParams.NumSlots = NumReentered;
				PrimeParams.NumBones = (uint32)Params.NumBones;
				PrimeParams.MaxTransformCount = Params.MaxTransformCount;
				PrimeParams.BaseTransformOffset = Params.BaseTransformOffset;
				PrimeParams.BasePreviousTransformOffset = Params.BasePreviousTransformOffset;
				PrimeParams.SlotIndices = ReenteredSRV;
				PrimeParams.TransformBuffer = Params.OutputTransformBuffer;
				GIAG::AddPrimePreviousTransformsPasses(GraphBuilder, PrimeParams);

				Outputs.ReenteredSlotsSRV = ReenteredSRV;
				Outputs.NumReenteredSlots = NumReentered;
			}
		}
	}

	if (CVar_GIAG_DebugUploadStats.GetValueOnRenderThread() != 0)
	{
		UE_LOG(LogTemp, Log, TEXT("GIAG UploadStats: ops=%u bytes=%llu (Active=%d SlotCap=%d Bones=%d)"),
			UploadOps,
			(unsigned long long)UploadBytes,
			Params.NumInstances,
			Params.SlotCapacity,
			Params.NumBones);
	}

	return Outputs;
}


