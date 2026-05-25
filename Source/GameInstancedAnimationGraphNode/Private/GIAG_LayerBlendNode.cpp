#include "GIAG_LayerBlendNode.h"

#include "GIAG_AnimGraph.h"
#include "GIAG_AnimNodeMetaManager.h"
#include "Animation/Skeleton.h"
#include "GIAG_HierarchyTableMaskUtils.h"
#include "GIAG_RdgDispatchTiling.h"
#include "GlobalShader.h"
#include "HierarchyTable.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

#include "GIAG_LayerBlendNode.ispc.generated.h"
static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), "GIAG ISPC: FGIAG_BoneTRS layout mismatch.");
static_assert(sizeof(ispc::FGIAG_BlendLayerNode_ISPC) == sizeof(FGIAG_LayerBlendNode), "GIAG ISPC: FGIAG_BlendLayerNode layout mismatch.");

GIAG_REGISTER_ANIM_NODE(FGIAG_LayerBlendNode);

namespace
{
	class FGIAG_PoseBlendLayerCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseBlendLayerCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseBlendLayerCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, NodeIndex)
			SHADER_PARAMETER(uint32, NeedNodeWordsPerSlot)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, PoseA)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, PoseB)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Weights)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Alpha)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, NeedNodeBits)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_BoneTRS>, RW_OutPose)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
			OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
			OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseBlendLayerCS, "/GameInstancedAnimationGraphNode/GIAG_LayerBlendNode.usf", "Main", SF_Compute);

	static void AddPoseBlendLayerPass(
		FRDGBuilder& GraphBuilder,
		uint32 NumBones,
		uint32 NumInstances,
		FRDGBufferSRVRef ActiveInstanceIndices,
		FRDGBufferSRVRef NeedNodeBits,
		uint32 NeedNodeWordsPerSlot,
		uint32 NodeIndex,
		FRDGBufferSRVRef PoseA,
		FRDGBufferSRVRef PoseB,
		FRDGBufferSRVRef Weights,
		FRDGBufferSRVRef Alpha,
		FRDGBufferUAVRef RW_OutPose)
	{
		FGIAG_PoseBlendLayerCS::FParameters* BaseShaderParams = GraphBuilder.AllocParameters<FGIAG_PoseBlendLayerCS::FParameters>();
		BaseShaderParams->NumBones = NumBones;
		BaseShaderParams->NumInstances = NumInstances;
		BaseShaderParams->NodeIndex = NodeIndex;
		BaseShaderParams->NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;
		BaseShaderParams->ActiveInstanceIndices = ActiveInstanceIndices;
		BaseShaderParams->NeedNodeBits = NeedNodeBits;
		BaseShaderParams->PoseA = PoseA;
		BaseShaderParams->PoseB = PoseB;
		BaseShaderParams->Weights = Weights;
		BaseShaderParams->Alpha = Alpha;
		BaseShaderParams->RW_OutPose = RW_OutPose;

		TShaderMapRef<FGIAG_PoseBlendLayerCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)NumBones * (int64)NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
			{
				FGIAG_PoseBlendLayerCS::FParameters* ChunkShaderParams = GraphBuilder.AllocParameters<FGIAG_PoseBlendLayerCS::FParameters>();
				*ChunkShaderParams = *BaseShaderParams;
				ChunkShaderParams->DispatchGroupCountX = (uint32)GroupCount.X;
				ChunkShaderParams->DispatchGroupCountY = (uint32)GroupCount.Y;
				ChunkShaderParams->DispatchGroupOffset = (uint32)GroupOffset1D;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GIAG_LayerBlend"),
					ChunkShaderParams,
					ERDGPassFlags::Compute,
					[ChunkShaderParams, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *ChunkShaderParams, GroupCount);
					});
			});
	}
}

uint32 FGIAG_LayerBlendNode::ComputeCullNeedMaskCPU(uint32 NumInputs) const
{
	return (Alpha > 0.0f) ? GIAG::AllInputsMask(NumInputs) : 1u;
}

void FGIAG_LayerBlendNode::EmitCullNeedMaskHlslBody(FString& Out, const TCHAR*& OutHlslElementType, const TCHAR*& OutMemberName)
{
	OutHlslElementType = TEXT("float");
	OutMemberName = TEXT("Alpha");
	Out += TEXT("\tfloat Alpha = NodeData[SlotIndex];\n");
	Out += TEXT("\treturn (Alpha > 0.0f) ? GIAG_AllInputsMask(NumInputs) : 1u;\n");
}

const void* FGIAG_LayerBlendNode::GatherUploadsGPU(uint32& OutUploadStrideBytes) const
{
	OutUploadStrideBytes = sizeof(Alpha);
	return &Alpha;
}

void FGIAG_LayerBlendNode::EnumerateResourceRequests(FConstStructView Settings, const USkeleton* Skeleton, EGIAG_AnimResourceTarget Target, TArray<FGIAG_AnimResourceRequest>& Out)
{
	if (!IsValid(Skeleton) || Skeleton->GetReferenceSkeleton().GetNum() <= 0)
	{
		return;
	}

	const FGIAG_BlendLayerSettings* BlendSettings = Settings.GetPtr<const FGIAG_BlendLayerSettings>();
	UHierarchyTable* Table = BlendSettings ? BlendSettings->BlendMaskTable.Get() : nullptr;

	FGIAG_AnimResourceRequest Req;
	Req.Slot = 0; // Slot 0 reserved for BlendLayer mask weights
	Req.ShareKey.Object = Skeleton;
	Req.ShareKey.SecondaryObject = Table;
	Req.ShareKey.AddonDesc = NAME_None;
	Req.Layout.Kind = EGIAG_AnimResourceKind::Buffer;
	Req.Layout.StrideBytes = sizeof(float);
	Req.Layout.NumElements = (uint32)Skeleton->GetReferenceSkeleton().GetNum();
	Req.Access = EGIAG_AnimResourceAccess::SRV;
	Out.Add(Req);
}

bool FGIAG_LayerBlendNode::BuildResourceForGPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TArray<uint8>& OutBytes)
{
	checkf(IsValid(Skeleton) && Skeleton->GetReferenceSkeleton().GetNum() > 0,
		TEXT("GIAG BlendLayer: invalid Skeleton."));
	checkf(Req.Slot == 0 && Req.Layout.Kind == EGIAG_AnimResourceKind::Buffer,
		TEXT("GIAG BlendLayer: invalid request slot/kind (Slot=%u Kind=%u)."), Req.Slot, (uint32)Req.Layout.Kind);
	checkf(Req.Layout.StrideBytes == sizeof(float),
		TEXT("GIAG BlendLayer: invalid request stride (Stride=%u)."), Req.Layout.StrideBytes);
	checkf(Req.Layout.NumElements == (uint32)Skeleton->GetReferenceSkeleton().GetNum(),
		TEXT("GIAG BlendLayer: invalid request num elements (Req=%u Skel=%d)."),
		Req.Layout.NumElements, Skeleton->GetReferenceSkeleton().GetNum());

	const FGIAG_BlendLayerSettings* BlendLayerSettings = Settings.GetPtr<const FGIAG_BlendLayerSettings>();
	checkf(BlendLayerSettings != nullptr, TEXT("GIAG BlendLayer: Settings must be provided."));
	const UHierarchyTable* Table = BlendLayerSettings->BlendMaskTable.Get();

	TArray<float> Weights;
	checkf(GIAG::HierarchyTableMaskUtils::BuildPerBoneMaskWeights(Skeleton, Table, Weights),
		TEXT("GIAG BlendLayer: failed to build mask weights."));
	checkf(Weights.Num() == (int32)Req.Layout.NumElements,
		TEXT("GIAG BlendLayer: weights size mismatch (Weights=%d Req=%u)."), Weights.Num(), Req.Layout.NumElements);

	OutBytes.SetNumUninitialized(sizeof(float) * Weights.Num());
	FMemory::Memcpy(OutBytes.GetData(), Weights.GetData(), (SIZE_T)OutBytes.Num());
	return true;
}

bool FGIAG_LayerBlendNode::BuildResourceForCPU(
	const FGIAG_AnimResourceRequest& Req,
	FConstStructView Settings,
	const USkeleton* Skeleton,
	TSharedPtr<void>& OutResource)
{
	checkf(IsValid(Skeleton) && Skeleton->GetReferenceSkeleton().GetNum() > 0,
		TEXT("GIAG BlendLayer: invalid Skeleton."));
	checkf(Req.Slot == 0 && Req.Layout.Kind == EGIAG_AnimResourceKind::Buffer,
		TEXT("GIAG BlendLayer: invalid request slot/kind (Slot=%u Kind=%u)."), Req.Slot, (uint32)Req.Layout.Kind);
	checkf(Req.Layout.StrideBytes == sizeof(float),
		TEXT("GIAG BlendLayer: invalid request stride (Stride=%u)."), Req.Layout.StrideBytes);
	checkf(Req.Layout.NumElements == (uint32)Skeleton->GetReferenceSkeleton().GetNum(),
		TEXT("GIAG BlendLayer: invalid request num elements (Req=%u Skel=%d)."),
		Req.Layout.NumElements, Skeleton->GetReferenceSkeleton().GetNum());

	const FGIAG_BlendLayerSettings* BlendLayerSettings = Settings.GetPtr<const FGIAG_BlendLayerSettings>();
	checkf(BlendLayerSettings != nullptr, TEXT("GIAG BlendLayer: Settings must be provided."));
	const UHierarchyTable* Table = BlendLayerSettings->BlendMaskTable.Get();

	TArray<float> Weights;
	checkf(GIAG::HierarchyTableMaskUtils::BuildPerBoneMaskWeights(Skeleton, Table, Weights),
		TEXT("GIAG BlendLayer: failed to build mask weights."));
	checkf(Weights.Num() == (int32)Req.Layout.NumElements,
		TEXT("GIAG BlendLayer: weights size mismatch (Weights=%d Req=%u)."), Weights.Num(), Req.Layout.NumElements);

	TSharedPtr<TArray<float>> SharedWeights = MakeShared<TArray<float>>(MoveTemp(Weights));
	OutResource = SharedWeights;
	return true;
}

void FGIAG_LayerBlendNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const FRDGBufferSRVRef WeightsSRV =
			(Context.OptionalBufferSRVsPerNodeBySlot.Num() > 0
				&& Context.OptionalBufferSRVsPerNodeBySlot[0].Num() > NodeIndexInBatch)
			? Context.OptionalBufferSRVsPerNodeBySlot[0][NodeIndexInBatch]
			: nullptr;

		AddPoseBlendLayerPass(
			Context.GraphBuilder,
			(uint32)Context.NumBones,
			(uint32)Context.NumInstances,
			Context.ActiveInstanceIndicesSRV,
			Context.NeedNodeBitsSRV,
			Context.NeedNodeWordsPerSlot,
			(uint32)Context.NodeIndices[NodeIndexInBatch],
			Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base].SRV,
			Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Layer].SRV,
			WeightsSRV,
			Context.NodeParamSRVsPerNode[NodeIndexInBatch],      // Alpha (StructuredBuffer<float>)
			Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out].UAV);
	}
}

void FGIAG_LayerBlendNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	check(Context.Compiled);
	check(Context.InputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.OutputPosesPerNode.Num() == Context.NodeIndices.Num());

	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const int32 NodeIdx = Context.NodeIndices[NodeIndexInBatch];
		check(NodeIdx >= 0 && NodeIdx < Context.Compiled->Nodes.Num());

		const FGIAG_CPUPoseBufferView PoseA = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base];
		const FGIAG_CPUPoseBufferView PoseB = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Layer];
		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(PoseA.IsValid() && PoseB.IsValid() && OutPose.IsValid());

		// Per-node optional weights (slot 0) is node-in-batch indexed.
		const TArray<float>* PerBoneWeightsArray = Context.GetOptionalResourcePtr<TArray<float>>(0, NodeIndexInBatch);
		const float* PerBoneWeights = (PerBoneWeightsArray && PerBoneWeightsArray->Num() > 0) ? PerBoneWeightsArray->GetData() : nullptr;

		ispc::GIAG_BlendLayerTRS(
			Context.NumBones,
			Context.NumInstances,
			Context.SlotCapacity,
			(const uint32*)Context.ActiveInstanceIndices.GetData(),
			PerBoneWeights,
			(const ispc::FGIAG_BlendLayerNode_ISPC*)Context.NodeData[NodeIdx],
			(const ispc::FGIAG_BoneTRS*)PoseA.Data,
			(const ispc::FGIAG_BoneTRS*)PoseB.Data,
			(ispc::FGIAG_BoneTRS*)OutPose.Data);
	}
}
