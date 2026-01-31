#include "GIAG_RefPoseNode.h"

#include "GIAG_AnimNodeMetaManager.h"
#include "GIAG_RdgDispatchTiling.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

GIAG_REGISTER_ANIM_NODE(FGIAG_RefPoseNode);

namespace
{
	class FGIAG_PoseRefPoseCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseRefPoseCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseRefPoseCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, NodeIndex)
			SHADER_PARAMETER(uint32, NeedNodeWordsPerSlot)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, RefPoseLocalTRS)
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

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseRefPoseCS, "/GameInstancedAnimationGraphNode/GIAG_RefPoseNode.usf", "Main", SF_Compute);

	static void AddPoseRefPosePass(
		FRDGBuilder& GraphBuilder,
		uint32 NumBones,
		uint32 NumInstances,
		FRDGBufferSRVRef ActiveInstanceIndices,
		FRDGBufferSRVRef NeedNodeBits,
		uint32 NeedNodeWordsPerSlot,
		uint32 NodeIndex,
		FRDGBufferSRVRef RefPoseLocalTRS,
		FRDGBufferUAVRef RW_OutPose)
	{
		FGIAG_PoseRefPoseCS::FParameters* BaseParameters = GraphBuilder.AllocParameters<FGIAG_PoseRefPoseCS::FParameters>();
		BaseParameters->NumBones = NumBones;
		BaseParameters->NumInstances = NumInstances;
		BaseParameters->NodeIndex = NodeIndex;
		BaseParameters->NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;
		BaseParameters->ActiveInstanceIndices = ActiveInstanceIndices;
		BaseParameters->RefPoseLocalTRS = RefPoseLocalTRS;
		BaseParameters->NeedNodeBits = NeedNodeBits;
		BaseParameters->RW_OutPose = RW_OutPose;

		TShaderMapRef<FGIAG_PoseRefPoseCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)NumBones * (int64)NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
			{
				FGIAG_PoseRefPoseCS::FParameters* ChunkParameters = GraphBuilder.AllocParameters<FGIAG_PoseRefPoseCS::FParameters>();
				*ChunkParameters = *BaseParameters;
				ChunkParameters->DispatchGroupCountX = (uint32)GroupCount.X;
				ChunkParameters->DispatchGroupCountY = (uint32)GroupCount.Y;
				ChunkParameters->DispatchGroupOffset = (uint32)GroupOffset1D;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GIAG_PoseRefPose_Chunk"),
					ChunkParameters,
					ERDGPassFlags::Compute,
					[ChunkParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *ChunkParameters, GroupCount);
					});
			});
	}
}

void FGIAG_RefPoseNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		AddPoseRefPosePass(
			Context.GraphBuilder,
			(uint32)Context.NumBones,
			(uint32)Context.NumInstances,
			Context.ActiveInstanceIndicesSRV,
			Context.NeedNodeBitsSRV,
			Context.NeedNodeWordsPerSlot,
			(uint32)Context.NodeIndices[NodeIndexInBatch],
			Context.RefPoseLocalTRSSRV,
			Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out].UAV);
	}
}

void FGIAG_RefPoseNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	check(Context.Compiled);
	check(Context.OutputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.RefPoseLocal.Num() == Context.NumBones);

	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(OutPose.IsValid());

		for (const int32 SlotIndex : Context.ActiveInstanceIndices)
		{
			check(SlotIndex >= 0 && SlotIndex < Context.SlotCapacity);
			for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
			{
				OutPose.At(SlotIndex, BoneIndex) = FTransform3f(Context.RefPoseLocal[BoneIndex]);
			}
		}
	}
}

