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
			SHADER_PARAMETER(uint32, TransformOffset)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, UseInitPrevBySlot)
			SHADER_PARAMETER(uint32, ForceInitPrevAllSlots)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ParentIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, InverseRefPoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, LocalPoseTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InitPrevBySlot)
			SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, TransformBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RW_TransformBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<FVector4f>, PrevCacheFloat4)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<FVector4f>, RW_PrevCacheFloat4)
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

	class FGIAG_TransformBufferFollowCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_TransformBufferFollowCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_TransformBufferFollowCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, SrcNumBones)
			SHADER_PARAMETER(uint32, SrcTransformOffsetBytes)
			SHADER_PARAMETER(uint32, SlotCount)
			SHADER_PARAMETER(uint32, NumDsts)
			SHADER_PARAMETER(uint32, UseBoneRemap)
			SHADER_PARAMETER(uint32, UseInitPrevBySlot)
			SHADER_PARAMETER(uint32, ForceInitPrevAllSlots)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, DstTransformOffsetBytesByDst)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoneRemap)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, InitPrevBySlot)
			SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, TransformBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RW_TransformBuffer)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return true;
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_TransformBufferFollowCS, "/GameInstancedAnimationGraphShader/GIAG_TransformBufferFollow_CS.usf", "Main", SF_Compute);

	class FGIAG_AttachToTransformBufferCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_AttachToTransformBufferCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_AttachToTransformBufferCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumAttachments)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ParentIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, LocalPoseTRS)
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

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ParentIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, LocalPoseTRS)
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

		if (!Params.TransformBuffer || !Params.ParentIndices || !Params.InverseRefPoseTRS || !Params.LocalPoseTRS || !ActiveIndicesSRV || !Params.PrevCacheFloat4)
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
			P->TransformOffset = Params.TransformOffset;
			P->DispatchGroupCountX = (uint32)GroupCount.X;
			P->DispatchGroupCountY = (uint32)GroupCount.Y;
			P->DispatchGroupOffset = (uint32)GroupOffset1D;
			P->ParentIndices = Params.ParentIndices;
			P->InverseRefPoseTRS = Params.InverseRefPoseTRS;
			P->LocalPoseTRS = Params.LocalPoseTRS;
			P->ActiveInstanceIndices = ActiveIndicesSRV;

			P->UseInitPrevBySlot = (Params.InitPrevBySlot != nullptr) ? 1u : 0u;
			P->ForceInitPrevAllSlots = Params.ForceInitPrevAllSlots;
			if (Params.InitPrevBySlot)
			{
				P->InitPrevBySlot = Params.InitPrevBySlot;
			}
			else
			{
				const uint32 Dummy = 0;
				FRDGBufferRef DummyBuf = CreateStructuredBuffer(
					GraphBuilder,
					TEXT("GIAG_PoseToTransformBuffer_DummyInitPrev"),
					sizeof(uint32),
					1,
					&Dummy,
					sizeof(uint32));
				P->InitPrevBySlot = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyBuf, PF_R32_UINT));
			}

			// ByteAddressBuffer binding uses PF_R32_UINT for SRV/UAV descriptors.
			P->TransformBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Params.TransformBuffer, PF_R32_UINT));
			P->RW_TransformBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Params.TransformBuffer, PF_R32_UINT));

			P->PrevCacheFloat4 = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Params.PrevCacheFloat4, PF_A32B32G32R32F));
			P->RW_PrevCacheFloat4 = GraphBuilder.CreateUAV(Params.PrevCacheFloat4, PF_A32B32G32R32F);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GIAG_PoseToTransformBuffer_Chunk"),
				P,
				ERDGPassFlags::Compute,
				[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
				});
		});
	}

	void AddTransformBufferFollowPasses(FRDGBuilder& GraphBuilder, const FTransformBufferFollowPassParams& Params)
	{
		if (!Params.TransformBuffer || !Params.DstTransformOffsetBytesByDst || Params.NumDsts == 0)
		{
			return;
		}
		if (Params.NumBones == 0 || Params.SrcNumBones == 0)
		{
			return;
		}
		if (Params.SlotCount == 0)
		{
			return;
		}

		TShaderMapRef<FGIAG_TransformBufferFollowCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FGIAG_TransformBufferFollowCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_TransformBufferFollowCS::FParameters>();
		P->NumBones = Params.NumBones;
		P->SrcNumBones = Params.SrcNumBones;
		P->SrcTransformOffsetBytes = Params.SrcTransformOffsetBytes;
		P->SlotCount = Params.SlotCount;
		P->NumDsts = Params.NumDsts;
		P->UseBoneRemap = (Params.BoneRemap != nullptr) ? 1u : 0u;
		P->UseInitPrevBySlot = (Params.InitPrevBySlot != nullptr) ? 1u : 0u;
		P->ForceInitPrevAllSlots = Params.ForceInitPrevAllSlots;
		P->DstTransformOffsetBytesByDst = Params.DstTransformOffsetBytesByDst;
		if (Params.BoneRemap)
		{
			P->BoneRemap = Params.BoneRemap;
		}
		else
		{
			const uint32 Dummy = 0;
			FRDGBufferRef DummyBuf = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GIAG_Follow_DummyBoneRemap"),
				sizeof(uint32),
				1,
				&Dummy,
				sizeof(uint32));
			P->BoneRemap = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyBuf, PF_R32_UINT));
		}

		if (Params.InitPrevBySlot)
		{
			P->InitPrevBySlot = Params.InitPrevBySlot;
		}
		else
		{
			const uint32 Dummy = 0;
			FRDGBufferRef DummyBuf = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GIAG_Follow_DummyInitPrev"),
				sizeof(uint32),
				1,
				&Dummy,
				sizeof(uint32));
			P->InitPrevBySlot = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyBuf, PF_R32_UINT));
		}

		// ByteAddressBuffer binding uses PF_R32_UINT for SRV/UAV descriptors.
		P->TransformBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Params.TransformBuffer, PF_R32_UINT));
		P->RW_TransformBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Params.TransformBuffer, PF_R32_UINT));

		const uint32 Total = Params.NumDsts * Params.SlotCount * Params.NumBones;
		const uint32 Threads = 64;
		const uint32 Groups = (Total + Threads - 1u) / Threads;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_TransformBufferFollow"),
			P,
			ERDGPassFlags::Compute,
			[P, CS, Groups](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, FIntVector((int32)Groups, 1, 1));
			});
	}

	void AddAttachToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FAttachToTransformBufferPassParams& Params)
	{
		check(Params.NumBones > 0);
		check(Params.NumAttachments > 0);
		check(Params.ParentIndices != nullptr);
		check(Params.LocalPoseTRS != nullptr);
		check(Params.ComponentToWorldBySlot != nullptr);
		check(Params.AttachDescs != nullptr);
		check(Params.RW_FxTransform != nullptr);

		TShaderMapRef<FGIAG_AttachToTransformBufferCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* P = GraphBuilder.AllocParameters<FGIAG_AttachToTransformBufferCS::FParameters>();
		P->NumBones = Params.NumBones;
		P->NumAttachments = Params.NumAttachments;
		P->ParentIndices = Params.ParentIndices;
		P->LocalPoseTRS = Params.LocalPoseTRS;
		P->ComponentToWorldBySlot = Params.ComponentToWorldBySlot;
		P->AttachDescs = Params.AttachDescs;
		P->RW_FxTransform = Params.RW_FxTransform;

		const uint32 GroupCountX = FMath::DivideAndRoundUp(Params.NumAttachments, 64u);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GIAG_AttachToTransformBuffer(Num=%u)", Params.NumAttachments),
			CS,
			P,
			FIntVector((int32)GroupCountX, 1, 1));
	}

	void AddAttachToISMInstanceBuffersPasses(FRDGBuilder& GraphBuilder, const FAttachToISMInstanceBuffersPassParams& Params)
	{
		check(Params.NumBones > 0);
		check(Params.NumAttachments > 0);
		check(Params.ParentIndices != nullptr);
		check(Params.LocalPoseTRS != nullptr);
		check(Params.ComponentToWorldBySlot != nullptr);
		check(Params.AttachDescs != nullptr);
		check(Params.RW_InstanceOrigin != nullptr);
		check(Params.RW_InstanceTransform != nullptr);

		TShaderMapRef<FGIAG_AttachToISMInstanceBuffersCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* P = GraphBuilder.AllocParameters<FGIAG_AttachToISMInstanceBuffersCS::FParameters>();
		P->NumBones = Params.NumBones;
		P->NumAttachments = Params.NumAttachments;
		P->ParentIndices = Params.ParentIndices;
		P->LocalPoseTRS = Params.LocalPoseTRS;
		P->ComponentToWorldBySlot = Params.ComponentToWorldBySlot;
		P->AttachDescs = Params.AttachDescs;
		P->RW_InstanceOrigin = Params.RW_InstanceOrigin;
		P->RW_InstanceTransform = Params.RW_InstanceTransform;

		const uint32 GroupCountX = FMath::DivideAndRoundUp(Params.NumAttachments, 64u);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GIAG_AttachToISMInstanceBuffers(Num=%u)", Params.NumAttachments),
			CS,
			P,
			FIntVector((int32)GroupCountX, 1, 1));
	}

	void AddScatterWriteFxTransformPasses(FRDGBuilder& GraphBuilder, const FScatterWriteFxTransformPassParams& Params)
	{
		if (Params.NumWrites == 0 || Params.OutputIndices == nullptr || Params.ValuesTransform == nullptr || Params.RW_FxTransform == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_ScatterWriteFxTransformCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		const uint32 ThreadsPerGroup = 64;
		const uint32 GroupCountX = FMath::DivideAndRoundUp(Params.NumWrites, ThreadsPerGroup);

		FGIAG_ScatterWriteFxTransformCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteFxTransformCS::FParameters>();
		P->NumWrites = Params.NumWrites;
		P->OutputIndices = Params.OutputIndices;
		P->ValuesTransform = Params.ValuesTransform;
		P->RW_FxTransform = Params.RW_FxTransform;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_ScatterWriteFxTransform"),
			P,
			ERDGPassFlags::Compute,
			[P, CS, GroupCountX](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, FIntVector((int32)GroupCountX, 1, 1));
			});
	}

	void AddScatterWriteInstanceBuffersPasses(FRDGBuilder& GraphBuilder, const FScatterWriteInstanceBuffersPassParams& Params)
	{
		if (Params.NumWrites == 0 || Params.OutputIndices == nullptr || Params.ValuesTransform == nullptr || Params.RW_InstanceOrigin == nullptr || Params.RW_InstanceTransform == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_ScatterWriteInstanceBuffersCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		const uint32 ThreadsPerGroup = 64;
		const uint32 GroupCountX = FMath::DivideAndRoundUp(Params.NumWrites, ThreadsPerGroup);

		FGIAG_ScatterWriteInstanceBuffersCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteInstanceBuffersCS::FParameters>();
		P->NumWrites = Params.NumWrites;
		P->OutputIndices = Params.OutputIndices;
		P->ValuesTransform = Params.ValuesTransform;
		P->RW_InstanceOrigin = Params.RW_InstanceOrigin;
		P->RW_InstanceTransform = Params.RW_InstanceTransform;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_ScatterWriteInstanceBuffers"),
			P,
			ERDGPassFlags::Compute,
			[P, CS, GroupCountX](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, FIntVector((int32)GroupCountX, 1, 1));
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

		const uint32 ThreadsPerGroup = 64;
		const uint32 GroupCountX = FMath::DivideAndRoundUp(Params.NumWrites, ThreadsPerGroup);

		FGIAG_ScatterWriteTransformsBySlotCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteTransformsBySlotCS::FParameters>();
		P->NumWrites = Params.NumWrites;
		P->OutputIndices = Params.OutputIndices;
		P->ValuesComponentToWorld = Params.ValuesComponentToWorld;
		P->ValuesWorldToComponent = Params.ValuesWorldToComponent;
		P->RW_ComponentToWorldBySlot = Params.RW_ComponentToWorldBySlot;
		P->RW_WorldToComponentBySlot = Params.RW_WorldToComponentBySlot;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_ScatterWriteTransformsBySlot"),
			P,
			ERDGPassFlags::Compute,
			[P, CS, GroupCountX](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, FIntVector((int32)GroupCountX, 1, 1));
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

		const uint32 ThreadsPerGroup = 64;
		const uint32 GroupCountX = FMath::DivideAndRoundUp(Params.NumWrites, ThreadsPerGroup);

		FGIAG_ScatterWriteBytesByIndexCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_ScatterWriteBytesByIndexCS::FParameters>();
		P->NumWrites = Params.NumWrites;
		P->StrideBytes = Params.StrideBytes;
		P->OutputIndices = Params.OutputIndices;
		P->ValuesBytes = Params.ValuesBytes;
		P->RW_DstBytes = Params.RW_DstBytes;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_ScatterWriteBytesByIndex(Stride=%u)", Params.StrideBytes),
			P,
			ERDGPassFlags::Compute,
			[P, CS, GroupCountX](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, FIntVector((int32)GroupCountX, 1, 1));
			});
	}

	void AddFillUintBufferPasses(FRDGBuilder& GraphBuilder, const FFillUintBufferPassParams& Params)
	{
		if (Params.NumDwords == 0 || Params.RW_Out == nullptr)
		{
			return;
		}

		TShaderMapRef<FGIAG_FillUintBufferCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FGIAG_FillUintBufferCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_FillUintBufferCS::FParameters>();
		P->NumDwords = Params.NumDwords;
		P->Value = Params.Value;
		P->RW_Out = Params.RW_Out;

		const uint32 ThreadsPerGroup = 256;
		const uint32 GroupCountX = FMath::DivideAndRoundUp<uint32>(Params.NumDwords, ThreadsPerGroup);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GIAG_FillUintBuffer(Value=%u)", Params.Value),
			P,
			ERDGPassFlags::Compute,
			[P, CS, GroupCountX](FRHIComputeCommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, FIntVector((int32)GroupCountX, 1, 1));
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


