#include "GIAG_AnimGraphShaders.h"

#include "GIAG_RdgDispatchTiling.h"
#include "GIAG_GraphCullShaderMap.h"
#include "GIAG_GraphCullConstants.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "Serialization/MemoryImage.h"
#include "Misc/Crc.h"
#include "Skinning/SkinningTransformProvider.h"

namespace
{
	class FGIAG_PoseToTransformBufferCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseToTransformBufferCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseToTransformBufferCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, BaseTransformOffset)
			SHADER_PARAMETER(uint32, BasePreviousTransformOffset)
			SHADER_PARAMETER(uint32, bWritePreviousTransforms)
			SHADER_PARAMETER(uint32, MaxTransformCount)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, InverseRefPoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, PoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<FVector4f>, TransformBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_TransformBuffer)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 32);
			OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
			OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseToTransformBufferCS, "/GameInstancedAnimationGraphShader/GIAG_PoseToTransformBuffer_CS.usf", "Main", SF_Compute);

	class FGIAG_PoseSpaceConvertCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseSpaceConvertCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseSpaceConvertCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(uint32, SourcePoseType)
			SHADER_PARAMETER(uint32, DestinationPoseType)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ParentIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, SourcePoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_BoneTRS>, RW_DestinationPoseTRS)
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

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseSpaceConvertCS, "/GameInstancedAnimationGraphShader/GIAG_PoseSpaceConvert_CS.usf", "Main", SF_Compute);

	class FGIAG_FollowerPoseToTransformCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_FollowerPoseToTransformCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_FollowerPoseToTransformCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, SrcNumBones)
			SHADER_PARAMETER(uint32, NumSlots)
			SHADER_PARAMETER(uint32, NumDsts)
			SHADER_PARAMETER(uint32, MaxTransformCount)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, PoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, InverseRefPoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoneRemap)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, DstInfos)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, IsActiveBySlot)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<FVector4f>, TransformBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_TransformBuffer)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FGIAG_FollowerPoseToTransformCS, "/GameInstancedAnimationGraphShader/GIAG_FollowerPoseToTransform_CS.usf", "Main", SF_Compute);

	class FGIAG_BucketCompactionCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_BucketCompactionCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_BucketCompactionCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumMoves)
			SHADER_PARAMETER(uint32, MaxTransformCount)
			SHADER_PARAMETER(uint32, BaseSpanOffsetBytes)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, SlotMoves)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<FVector4f>, TransformBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_TransformBuffer)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FGIAG_BucketCompactionCS, "/GameInstancedAnimationGraphShader/GIAG_BucketCompaction_CS.usf", "Main", SF_Compute);

	class FGIAG_AttachToTransformBufferCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_AttachToTransformBufferCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_AttachToTransformBufferCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumAttachments)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, PoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, ComponentToWorldBySlot)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_AttachDescPacked>, AttachDescs)

			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_Transform>, RW_FxTransform)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_AttachToTransformBufferCS, "/GameInstancedAnimationGraphShader/GIAG_AttachToTransformBuffer_CS.usf", "Main", SF_Compute);

	class FGIAG_AttachToISMInstanceBuffersCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_AttachToISMInstanceBuffersCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_AttachToISMInstanceBuffersCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumAttachments)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, PoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, ComponentToWorldBySlot)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_AttachDescPacked>, AttachDescs)

			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_InstanceOrigin)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_InstanceTransform)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_AttachToISMInstanceBuffersCS, "/GameInstancedAnimationGraphShader/GIAG_AttachToISMInstanceBuffers_CS.usf", "Main", SF_Compute);

	class FGIAG_ScatterWriteFxTransformCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_ScatterWriteFxTransformCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_ScatterWriteFxTransformCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumWrites)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, OutputIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, ValuesTransform)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_Transform>, RW_FxTransform)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_ScatterWriteFxTransformCS, "/GameInstancedAnimationGraphShader/GIAG_ScatterWriteFxTransform_CS.usf", "Main", SF_Compute);

	class FGIAG_ScatterWriteInstanceBuffersCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_ScatterWriteInstanceBuffersCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_ScatterWriteInstanceBuffersCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumWrites)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, OutputIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, ValuesTransform)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_InstanceOrigin)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_InstanceTransform)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_ScatterWriteInstanceBuffersCS, "/GameInstancedAnimationGraphShader/GIAG_ScatterWriteInstanceBuffers_CS.usf", "Main", SF_Compute);

	class FGIAG_ScatterWriteTransformsBySlotCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_ScatterWriteTransformsBySlotCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_ScatterWriteTransformsBySlotCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumWrites)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, OutputIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, ValuesComponentToWorld)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, ValuesWorldToComponent)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_Transform>, RW_ComponentToWorldBySlot)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_Transform>, RW_WorldToComponentBySlot)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_ScatterWriteTransformsBySlotCS, "/GameInstancedAnimationGraphShader/GIAG_ScatterWriteTransformsBySlot_CS.usf", "Main", SF_Compute);

	class FGIAG_ScatterWriteBytesByIndexCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_ScatterWriteBytesByIndexCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_ScatterWriteBytesByIndexCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumWrites)
			SHADER_PARAMETER(uint32, StrideBytes)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, OutputIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, ValuesBytes)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RW_DstBytes)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_ScatterWriteBytesByIndexCS, "/GameInstancedAnimationGraphShader/GIAG_ScatterWriteBytesByIndex_CS.usf", "Main", SF_Compute);

	class FGIAG_FillUintBufferCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_FillUintBufferCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_FillUintBufferCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumDwords)
			SHADER_PARAMETER(uint32, Value)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_Out)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 256);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_FillUintBufferCS, "/GameInstancedAnimationGraphShader/GIAG_FillUintBuffer_CS.usf", "Main", SF_Compute);
}

namespace GIAG
{
	void AddPoseToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FPoseToTransformBufferPassParams& Params)
	{
		constexpr int32 ThreadsPerGroup = 32;

		TShaderMapRef<FGIAG_PoseToTransformBufferCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FRDGBufferSRVRef ActiveIndicesSRV = Params.ActiveInstanceIndices;

		if (!Params.TransformBuffer || !Params.InverseRefPoseTRS || !Params.PoseTRS || !ActiveIndicesSRV)
		{
			return;
		}
		if (Params.NumBones == 0 || Params.NumInstances == 0)
		{
			return;
		}

		const int64 TotalWorkItems = (int64)Params.NumBones * (int64)Params.NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			FGIAG_PoseToTransformBufferCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_PoseToTransformBufferCS::FParameters>();
			P->NumBones = Params.NumBones;
			P->NumInstances = Params.NumInstances;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->InverseRefPoseTRS = Params.InverseRefPoseTRS;
			P->PoseTRS = Params.PoseTRS;
			P->ActiveInstanceIndices = ActiveIndicesSRV;

			P->BaseTransformOffset = Params.BaseTransformOffset;
			P->BasePreviousTransformOffset = Params.BasePreviousTransformOffset;
			P->bWritePreviousTransforms = Params.bWritePreviousTransforms;
			P->MaxTransformCount = Params.MaxTransformCount;

			// UE 5.8 skinning transform buffer is float4-indexed; engine helpers handle the format.
			P->TransformBuffer = GetCompressedBoneTransformSRV(GraphBuilder, Params.TransformBuffer);
			P->RW_TransformBuffer = GetCompressedBoneTransformUAV(GraphBuilder, Params.TransformBuffer);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_PoseToTransformBuffer"),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddPoseSpaceConvertPasses(FRDGBuilder& GraphBuilder, const FPoseSpaceConvertPassParams& Params)
	{
		if (Params.NumBones == 0
			|| Params.NumInstances == 0
			|| Params.ActiveInstanceIndices == nullptr
			|| Params.ParentIndices == nullptr
			|| Params.SourcePoseTRS == nullptr
			|| Params.RW_DestinationPoseTRS == nullptr)
		{
			return;
		}
		if (Params.SourcePoseType == Params.DestinationPoseType)
		{
			return;
		}
		checkf(
			(Params.SourcePoseType <= 1u) && (Params.DestinationPoseType <= 1u),
			TEXT("GIAG: invalid pose convert types (Src=%u Dst=%u)."),
			Params.SourcePoseType,
			Params.DestinationPoseType);

		TShaderMapRef<FGIAG_PoseSpaceConvertCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumBones * (int64)Params.NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			FGIAG_PoseSpaceConvertCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_PoseSpaceConvertCS::FParameters>();
			P->NumBones = Params.NumBones;
			P->NumInstances = Params.NumInstances;
			P->SourcePoseType = Params.SourcePoseType;
			P->DestinationPoseType = Params.DestinationPoseType;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->ActiveInstanceIndices = Params.ActiveInstanceIndices;
			P->ParentIndices = Params.ParentIndices;
			P->SourcePoseTRS = Params.SourcePoseTRS;
			P->RW_DestinationPoseTRS = Params.RW_DestinationPoseTRS;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_PoseSpaceConvert"),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddFollowerPoseToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FFollowerPoseToTransformBufferPassParams& Params)
	{
		check(Params.NumBones > 0 && Params.NumDsts > 0 && Params.NumSlots > 0);
		check(Params.PoseTRS != nullptr && Params.InverseRefPoseTRS != nullptr);
		check(Params.DstInfos != nullptr && Params.TransformBuffer != nullptr);

		TShaderMapRef<FGIAG_FollowerPoseToTransformCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumDsts * (int64)Params.NumSlots * (int64)Params.NumBones;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_FollowerPoseToTransformCS::FParameters>();
			P->NumBones = Params.NumBones;
			P->SrcNumBones = Params.SrcNumBones;
			P->NumSlots = Params.NumSlots;
			P->NumDsts = Params.NumDsts;
			P->MaxTransformCount = Params.MaxTransformCount;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->PoseTRS = Params.PoseTRS;
			P->InverseRefPoseTRS = Params.InverseRefPoseTRS;
			P->BoneRemap = Params.BoneRemap;
			P->DstInfos = Params.DstInfos;
			P->IsActiveBySlot = Params.IsActiveBySlot;
			P->TransformBuffer = GetCompressedBoneTransformSRV(GraphBuilder, Params.TransformBuffer);
			P->RW_TransformBuffer = GetCompressedBoneTransformUAV(GraphBuilder, Params.TransformBuffer);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_FollowerPoseToTransform [%s %u]", *Params.DebugName.ToString(), Params.NumDsts),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddBucketCompactionPass(FRDGBuilder& GraphBuilder, const FBucketCompactionPassParams& Params)
	{
		if (Params.NumMoves == 0 || Params.MaxTransformCount == 0
			|| Params.SlotMoves == nullptr || Params.TransformBuffer == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_BucketCompactionCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TransformsPerMove = (int64)Params.MaxTransformCount * 2;
		const int64 TotalWorkItems = (int64)Params.NumMoves * TransformsPerMove;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_BucketCompactionCS::FParameters>();
			P->NumMoves            = Params.NumMoves;
			P->MaxTransformCount   = Params.MaxTransformCount;
			P->BaseSpanOffsetBytes = Params.BaseSpanOffsetBytes;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->SlotMoves           = Params.SlotMoves;
			P->TransformBuffer     = GetCompressedBoneTransformSRV(GraphBuilder, Params.TransformBuffer);
			P->RW_TransformBuffer  = GetCompressedBoneTransformUAV(GraphBuilder, Params.TransformBuffer);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_BucketCompaction (%u moves)", Params.NumMoves),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddAttachToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FAttachToTransformBufferPassParams& Params)
	{
		check(Params.NumBones > 0);
		check(Params.NumAttachments > 0);
		check(Params.PoseTRS != nullptr);
		check(Params.ComponentToWorldBySlot != nullptr);
		check(Params.AttachDescs != nullptr);
		check(Params.RW_FxTransform != nullptr);

		TShaderMapRef<FGIAG_AttachToTransformBufferCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumAttachments;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_AttachToTransformBufferCS::FParameters>();
			P->NumBones = Params.NumBones;
			P->NumAttachments = Params.NumAttachments;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->PoseTRS = Params.PoseTRS;
			P->ComponentToWorldBySlot = Params.ComponentToWorldBySlot;
			P->AttachDescs = Params.AttachDescs;
			P->RW_FxTransform = Params.RW_FxTransform;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_AttachToTransformBuffer(Num=%u)", Params.NumAttachments),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddAttachToISMInstanceBuffersPasses(FRDGBuilder& GraphBuilder, const FAttachToISMInstanceBuffersPassParams& Params)
	{
		check(Params.NumBones > 0);
		check(Params.NumAttachments > 0);
		check(Params.PoseTRS != nullptr);
		check(Params.ComponentToWorldBySlot != nullptr);
		check(Params.AttachDescs != nullptr);
		check(Params.RW_InstanceOrigin != nullptr);
		check(Params.RW_InstanceTransform != nullptr);

		TShaderMapRef<FGIAG_AttachToISMInstanceBuffersCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumAttachments;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_AttachToISMInstanceBuffersCS::FParameters>();
			P->NumBones = Params.NumBones;
			P->NumAttachments = Params.NumAttachments;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->PoseTRS = Params.PoseTRS;
			P->ComponentToWorldBySlot = Params.ComponentToWorldBySlot;
			P->AttachDescs = Params.AttachDescs;
			P->RW_InstanceOrigin = Params.RW_InstanceOrigin;
			P->RW_InstanceTransform = Params.RW_InstanceTransform;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_AttachToISMInstanceBuffers(Num=%u)", Params.NumAttachments),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddScatterWriteFxTransformPasses(FRDGBuilder& GraphBuilder, const FScatterWriteFxTransformPassParams& Params)
	{
		if (Params.NumWrites == 0 || Params.OutputIndices == nullptr || Params.ValuesTransform == nullptr || Params.RW_FxTransform == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_ScatterWriteFxTransformCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumWrites;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteFxTransformCS::FParameters>();
			P->NumWrites = Params.NumWrites;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->OutputIndices = Params.OutputIndices;
			P->ValuesTransform = Params.ValuesTransform;
			P->RW_FxTransform = Params.RW_FxTransform;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_ScatterWriteFxTransform"),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddScatterWriteInstanceBuffersPasses(FRDGBuilder& GraphBuilder, const FScatterWriteInstanceBuffersPassParams& Params)
	{
		if (Params.NumWrites == 0 || Params.OutputIndices == nullptr || Params.ValuesTransform == nullptr || Params.RW_InstanceOrigin == nullptr || Params.RW_InstanceTransform == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_ScatterWriteInstanceBuffersCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumWrites;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteInstanceBuffersCS::FParameters>();
			P->NumWrites = Params.NumWrites;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->OutputIndices = Params.OutputIndices;
			P->ValuesTransform = Params.ValuesTransform;
			P->RW_InstanceOrigin = Params.RW_InstanceOrigin;
			P->RW_InstanceTransform = Params.RW_InstanceTransform;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_ScatterWriteInstanceBuffers"),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddScatterWriteTransformsBySlotPasses(FRDGBuilder& GraphBuilder, const FScatterWriteTransformsBySlotPassParams& Params)
	{
		if (Params.NumWrites == 0
			|| Params.OutputIndices == nullptr
			|| Params.ValuesComponentToWorld == nullptr
			|| Params.ValuesWorldToComponent == nullptr
			|| Params.RW_ComponentToWorldBySlot == nullptr
			|| Params.RW_WorldToComponentBySlot == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_ScatterWriteTransformsBySlotCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumWrites;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteTransformsBySlotCS::FParameters>();
			P->NumWrites = Params.NumWrites;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->OutputIndices = Params.OutputIndices;
			P->ValuesComponentToWorld = Params.ValuesComponentToWorld;
			P->ValuesWorldToComponent = Params.ValuesWorldToComponent;
			P->RW_ComponentToWorldBySlot = Params.RW_ComponentToWorldBySlot;
			P->RW_WorldToComponentBySlot = Params.RW_WorldToComponentBySlot;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_ScatterWriteTransformsBySlot"),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddScatterWriteBytesByIndexPasses(FRDGBuilder& GraphBuilder, const FScatterWriteBytesByIndexPassParams& Params)
	{
		checkf((Params.StrideBytes % 4u) == 0u, TEXT("GIAG: ScatterWriteBytesByIndex requires StrideBytes multiple of 4 (StrideBytes=%u)."), Params.StrideBytes);

		if (Params.NumWrites == 0
			|| Params.StrideBytes == 0
			|| Params.OutputIndices == nullptr
			|| Params.ValuesBytes == nullptr
			|| Params.RW_DstBytes == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_ScatterWriteBytesByIndexCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)Params.NumWrites;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteBytesByIndexCS::FParameters>();
			P->NumWrites = Params.NumWrites;
			P->StrideBytes = Params.StrideBytes;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->OutputIndices = Params.OutputIndices;
			P->ValuesBytes = Params.ValuesBytes;
			P->RW_DstBytes = Params.RW_DstBytes;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_ScatterWriteBytesByIndex(Stride=%u)", Params.StrideBytes),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddFillUintBufferPasses(FRDGBuilder& GraphBuilder, const FFillUintBufferPassParams& Params)
	{
		if (Params.NumDwords == 0 || Params.RW_Out == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_FillUintBufferCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 256;
		const int64 TotalWorkItems = (int64)Params.NumDwords;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
		{
			auto* P = GraphBuilder.AllocParameters<FGIAG_FillUintBufferCS::FParameters>();
			P->NumDwords = Params.NumDwords;
			P->Value = Params.Value;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->RW_Out = Params.RW_Out;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_FillUintBuffer(Value=%u)", Params.Value),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FGIAG_GraphCullPassParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, RW_NeedNodeBits)
		// Used only for RDG lifetime/validation; actual shader binding is manual by fixed register indices.
		SHADER_PARAMETER_RDG_BUFFER_SRV_ARRAY(StructuredBuffer<uint32>, CullParams, [32])
	END_SHADER_PARAMETER_STRUCT()

	void AddGraphCullPasses(FRDGBuilder& GraphBuilder, const FGraphCullPassParams& Params)
	{
		if (Params.NumNodes == 0
			|| Params.NumInstances == 0
			|| Params.SlotCapacity == 0
			|| Params.WordsPerSlot == 0
			|| Params.ActiveInstanceIndices == nullptr
			|| Params.RW_NeedNodeBits == nullptr)
		{
			return;
		}

		if (Params.FinalNodeIndex == (uint32)INDEX_NONE)
		{
			GIAG::FFillUintBufferPassParams Fill;
			Fill.NumDwords = FMath::Max(1u, Params.SlotCapacity * Params.WordsPerSlot);
			Fill.Value = 0xFFFFFFFFu;
			Fill.RW_Out = Params.RW_NeedNodeBits;
			GIAG::AddFillUintBufferPasses(GraphBuilder, Fill);
			return;
		}

		check(Params.ShaderMap != nullptr);

		TShaderMapRef<FGIAG_GraphCullCS> CS(Params.ShaderMap);

		const uint32 ThreadsPerGroup = 64;
		const uint32 GroupCountX = FMath::DivideAndRoundUp<uint32>(Params.NumInstances, ThreadsPerGroup);

		FGIAG_GraphCullPassParameters* P = GraphBuilder.AllocParameters<FGIAG_GraphCullPassParameters>();
		P->ActiveInstanceIndices = Params.ActiveInstanceIndices;
		P->RW_NeedNodeBits = Params.RW_NeedNodeBits;
		for (uint32 i = 0; i < (uint32)GIAG::MaxGraphCullParamBuffers; ++i)
		{
			P->CullParams[i] = nullptr;
		}
		checkf(Params.CullParams.Num() <= (int32)GIAG::MaxGraphCullParamBuffers,
			TEXT("GIAG: GraphCull CullParams exceeds MaxGraphCullParamBuffers (%d > %u)."),
			Params.CullParams.Num(), (uint32)GIAG::MaxGraphCullParamBuffers);
		for (int32 i = 0; i < Params.CullParams.Num(); ++i)
		{
			P->CullParams[i] = Params.CullParams[i];
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_GraphCull(Nodes=%u Active=%u)", Params.NumNodes, Params.NumInstances),
			P,
			ERDGPassFlags::Compute,
			[P, CS,
				GroupCountX,
				NumNodes = Params.NumNodes,
				NumInstances = Params.NumInstances,
				SlotCapacity = Params.SlotCapacity,
				WordsPerSlot = Params.WordsPerSlot,
				FinalNodeIndex = Params.FinalNodeIndex](FRHIComputeCommandList& RHICmdList)
			{
				FRHIComputeShader* ShaderRHI = CS.GetComputeShader();
				SetComputePipelineState(RHICmdList, ShaderRHI);

				FRHIBatchedShaderParameters& Batched = RHICmdList.GetScratchShaderParameters();
				CS->SetParameters(
					Batched,
					NumNodes,
					NumInstances,
					SlotCapacity,
					WordsPerSlot,
					FinalNodeIndex,
					P->ActiveInstanceIndices->GetRHI(),
					P->RW_NeedNodeBits->GetRHI());

				for (uint32 i = 0; i < (uint32)GIAG::MaxGraphCullParamBuffers; ++i)
				{
					if (P->CullParams[i] != nullptr)
					{
						const uint32 BaseIndex = (uint32)GIAG::GraphCullParamSRVRegisterBase + i;
						Batched.SetShaderResourceViewParameter(BaseIndex, P->CullParams[i]->GetRHI());
					}
				}

				RHICmdList.SetBatchedShaderParameters(ShaderRHI, Batched);
				RHICmdList.DispatchComputeShader((uint32)GroupCountX, 1, 1);
				UnsetShaderUAVs(RHICmdList, CS, ShaderRHI);
			});
	}
}


