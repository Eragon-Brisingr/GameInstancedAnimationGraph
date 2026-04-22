#include "GIAG_AdditiveNode.h"

#include "GIAG_AnimGraph.h"
#include "GIAG_AnimNodeMetaManager.h"
#include "GIAG_RdgDispatchTiling.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

#include "GIAG_AdditiveNode.ispc.generated.h"
static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), "GIAG ISPC: FGIAG_BoneTRS layout mismatch.");
static_assert(sizeof(ispc::FGIAG_AdditiveNode_ISPC) == sizeof(FGIAG_AdditiveNode), "GIAG ISPC: FGIAG_AdditiveNode layout mismatch.");

GIAG_REGISTER_ANIM_NODE(FGIAG_AdditiveNode);

namespace
{
	class FGIAG_PoseAdditiveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseAdditiveCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseAdditiveCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, NodeIndex)
			SHADER_PARAMETER(uint32, NeedNodeWordsPerSlot)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, BasePose)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, AdditivePose)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Alpha) // per-slot
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

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseAdditiveCS, "/GameInstancedAnimationGraphNode/GIAG_AdditiveNode.usf", "Main", SF_Compute);

	static void AddPoseAdditivePass(
		FRDGBuilder& GraphBuilder,
		uint32 NumBones,
		uint32 NumInstances,
		FRDGBufferSRVRef ActiveInstanceIndices,
		FRDGBufferSRVRef NeedNodeBits,
		uint32 NeedNodeWordsPerSlot,
		uint32 NodeIndex,
		FRDGBufferSRVRef BasePose,
		FRDGBufferSRVRef AdditivePose,
		FRDGBufferSRVRef Alpha,
		FRDGBufferUAVRef RW_OutPose)
	{
		FGIAG_PoseAdditiveCS::FParameters* BaseP = GraphBuilder.AllocParameters<FGIAG_PoseAdditiveCS::FParameters>();
		BaseP->NumBones = NumBones;
		BaseP->NumInstances = NumInstances;
		BaseP->NodeIndex = NodeIndex;
		BaseP->NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;
		BaseP->ActiveInstanceIndices = ActiveInstanceIndices;
		BaseP->NeedNodeBits = NeedNodeBits;
		BaseP->BasePose = BasePose;
		BaseP->AdditivePose = AdditivePose;
		BaseP->Alpha = Alpha;
		BaseP->RW_OutPose = RW_OutPose;

		TShaderMapRef<FGIAG_PoseAdditiveCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)NumBones * (int64)NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
			{
				FGIAG_PoseAdditiveCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_PoseAdditiveCS::FParameters>();
				*P = *BaseP;
				P->DispatchGroupCountX = (uint32)GroupCount.X;
				P->DispatchGroupCountY = (uint32)GroupCount.Y;
				P->DispatchGroupOffset = (uint32)GroupOffset1D;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GIAG_Additive"),
					P,
					ERDGPassFlags::Compute,
					[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
					});
			});
	}
}

uint32 FGIAG_AdditiveNode::ComputeCullNeedMaskCPU(uint32 NumInputs) const
{
	return (Alpha > 0.0f) ? GIAG::AllInputsMask(NumInputs) : 1u;
}

void FGIAG_AdditiveNode::EmitCullNeedMaskHlslBody(FString& Out, const TCHAR*& OutHlslElementType, const TCHAR*& OutMemberName)
{
	OutHlslElementType = TEXT("float");
	OutMemberName = TEXT("Alpha");
	Out += TEXT("\tfloat Alpha = NodeData[SlotIndex];\n");
	Out += TEXT("\treturn (Alpha > 0.0f) ? GIAG_AllInputsMask(NumInputs) : 1u;\n");
}

const void* FGIAG_AdditiveNode::GatherUploadsGPU(uint32& OutUploadStrideBytes) const
{
	OutUploadStrideBytes = sizeof(Alpha);
	return &Alpha;
}

void FGIAG_AdditiveNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		AddPoseAdditivePass(
			Context.GraphBuilder,
			(uint32)Context.NumBones,
			(uint32)Context.NumInstances,
			Context.ActiveInstanceIndicesSRV,
			Context.NeedNodeBitsSRV,
			Context.NeedNodeWordsPerSlot,
			(uint32)Context.NodeIndices[NodeIndexInBatch],
			Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base].SRV,
			Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Additive].SRV,
			Context.NodeParamSRVsPerNode[NodeIndexInBatch],      // Alpha
			Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out].UAV);
	}
}

void FGIAG_AdditiveNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	check(Context.Compiled);
	check(Context.InputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.OutputPosesPerNode.Num() == Context.NodeIndices.Num());

	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const int32 NodeIdx = Context.NodeIndices[NodeIndexInBatch];
		check(NodeIdx >= 0 && NodeIdx < Context.Compiled->Nodes.Num());

		const FGIAG_CPUPoseBufferView BasePose = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base];
		const FGIAG_CPUPoseBufferView AdditivePose = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Additive];
		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(BasePose.IsValid() && AdditivePose.IsValid() && OutPose.IsValid());

		ispc::GIAG_ApplyAdditiveTRS(
			Context.NumBones,
			Context.NumInstances,
			Context.SlotCapacity,
			(const uint32*)Context.ActiveInstanceIndices.GetData(),
			(const ispc::FGIAG_AdditiveNode_ISPC*)Context.NodeData[NodeIdx],
			(const ispc::FGIAG_BoneTRS*)BasePose.Data,
			(const ispc::FGIAG_BoneTRS*)AdditivePose.Data,
			(ispc::FGIAG_BoneTRS*)OutPose.Data);
	}
}

