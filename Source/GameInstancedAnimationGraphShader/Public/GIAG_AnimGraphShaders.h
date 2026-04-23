#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "RenderGraphResources.h"

#include "GIAG_GraphCullShaderMap.h"

class UTextureRenderTarget2DArray;

namespace GIAG
{
	/**
	 * Mirrors `GIAG::DefaultSlotsPerShard` in `GIAG_AnimCommon.h` (shader module cannot include that header).
	 * Distinct identifier so TUs that include both headers do not redefine `DefaultSlotsPerShard`.
	 */
	static constexpr uint32 AnimGraphShaderDefaultSlotsPerShard = 127;

	struct FPoseToSkinPassParams
	{
		uint32 NumBones = 0;
		uint32 NumInstances = 0;

		/** Optional ActiveIndex->AbsoluteInstanceIndex mapping (see FGIAG_AnimGraphRunParams::ActiveInstanceIndices). */
		FRDGBufferSRVRef ActiveInstanceIndices = nullptr; // StructuredBuffer<uint>

		uint32 OutTexSizeX = 0;
		uint32 OutTexSizeY = 0;

		FRDGBufferSRVRef ParentIndices = nullptr;      // StructuredBuffer<int>
		FRDGBufferSRVRef InverseRefPoseTRS = nullptr;  // StructuredBuffer<FGIAG_BoneTRS>
		FRDGBufferSRVRef LocalPoseTRS = nullptr;       // StructuredBuffer<FGIAG_BoneTRS>

		FRDGTextureUAVRef RW_OutSkinTex = nullptr;     // RWTexture2DArray<float4>
	};

	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddPoseToSkinPasses(FRDGBuilder& GraphBuilder, const FPoseToSkinPassParams& Params);

	struct FPoseToTransformBufferPassParams
	{
		uint32 NumBones = 0;
		uint32 NumInstances = 0;

		/** ActiveIndex->SlotIndex mapping. */
		FRDGBufferSRVRef ActiveInstanceIndices = nullptr; // StructuredBuffer<uint>

		FRDGBufferSRVRef InverseRefPoseTRS = nullptr;  // StructuredBuffer<FGIAG_BoneTRS>
		FRDGBufferSRVRef PoseTRS = nullptr;            // StructuredBuffer<FGIAG_BoneTRS>

		/** UE SkinningSceneExtension TransformBuffer (ByteAddress). */
		FRDGBufferRef TransformBuffer = nullptr;

		/** GIAG RT-only previous cache (RWBuffer<float4>): 3 float4 rows per (SlotIndex,BoneIndex). */
		FRDGBufferRef PrevCacheFloat4 = nullptr;

		/** If non-zero, force previous=current for all slots this frame (e.g. prev-cache newly created/resized). */
		uint32 ForceInitPrevAllSlots = 0;

		FRDGBufferSRVRef InitPrevBySlot = nullptr; // StructuredBuffer<uint>, size = SlotCapacity

		/** Per-shard TransformBuffer byte offsets. Each shard writes to its own TransformBuffer region.
		 *  SlotIndex % SlotsPerShard = ShardSlot, SlotIndex / SlotsPerShard = ShardIndex. */
		FRDGBufferSRVRef ShardTransformOffsets = nullptr; // StructuredBuffer<uint>, size = NumShards (required)
		uint32 SlotsPerShard = AnimGraphShaderDefaultSlotsPerShard;
	};

	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddPoseToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FPoseToTransformBufferPassParams& Params);

	struct FPoseSpaceConvertPassParams
	{
		uint32 NumBones = 0;
		uint32 NumInstances = 0;
		/** 0: LocalPose, 1: ComponentPose. */
		uint32 SourcePoseType = 0;
		/** 0: LocalPose, 1: ComponentPose. */
		uint32 DestinationPoseType = 0;
		FRDGBufferSRVRef ActiveInstanceIndices = nullptr; // StructuredBuffer<uint>
		FRDGBufferSRVRef ParentIndices = nullptr;         // StructuredBuffer<int>
		FRDGBufferSRVRef SourcePoseTRS = nullptr;         // StructuredBuffer<FGIAG_BoneTRS>
		FRDGBufferUAVRef RW_DestinationPoseTRS = nullptr; // RWStructuredBuffer<FGIAG_BoneTRS>
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddPoseSpaceConvertPasses(FRDGBuilder& GraphBuilder, const FPoseSpaceConvertPassParams& Params);

	struct FFollowerDstInfo
	{
		uint32 SrcSlotBase = 0;
		uint32 DstTransformOffsetBytes = 0;
	};

	struct FFollowerPoseToTransformBufferPassParams
	{
		uint32 NumBones = 0;
		uint32 SrcNumBones = 0;
		uint32 SlotsPerShard = AnimGraphShaderDefaultSlotsPerShard;
		uint32 NumDsts = 0;

		FRDGBufferSRVRef PoseTRS = nullptr;
		FRDGBufferSRVRef InverseRefPoseTRS = nullptr;
		FRDGBufferSRVRef DstInfos = nullptr;
		FRDGBufferSRVRef BoneRemap = nullptr;          // DestBone -> SrcBone (identity when no remap)
		FRDGBufferSRVRef InitPrevBySlot = nullptr;
		uint32 ForceInitPrevAllSlots = 0;
		FRDGBufferSRVRef IsActiveBySlot = nullptr;     // per-slot activity from master frustum cull

		FRDGBufferRef TransformBuffer = nullptr;

		FName DebugName;
	};

	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddFollowerPoseToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FFollowerPoseToTransformBufferPassParams& Params);

	struct FAttachToTransformBufferPassParams
	{
		uint32 NumBones = 0;
		uint32 NumAttachments = 0;

		FRDGBufferSRVRef PoseTRS = nullptr;                 // StructuredBuffer<FGIAG_BoneTRS>
		FRDGBufferSRVRef ComponentToWorldBySlot = nullptr;  // StructuredBuffer<FGIAG_Transform>
		FRDGBufferSRVRef AttachDescs = nullptr;             // StructuredBuffer<FGIAG_AttachDescPacked>

		FRDGBufferUAVRef RW_FxTransform = nullptr;         // RWStructuredBuffer<FGIAG_Transform> (1 per instance)
	};

	/** Niagara attach: compute per-attachment world Transform into RW_FxTransform. */
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddAttachToTransformBufferPasses(FRDGBuilder& GraphBuilder, const FAttachToTransformBufferPassParams& Params);

	/**
	 * Native attach mesh: compute per-attachment world transform directly into UE instancing buffers (origin + 3 rows),
	 * bypassing the FxTransform buffer entirely.
	 */
	struct FAttachToISMInstanceBuffersPassParams
	{
		uint32 NumBones = 0;
		uint32 NumAttachments = 0;

		FRDGBufferSRVRef PoseTRS = nullptr;                 // StructuredBuffer<FGIAG_BoneTRS>
		FRDGBufferSRVRef ComponentToWorldBySlot = nullptr;  // StructuredBuffer<FGIAG_Transform>
		FRDGBufferSRVRef AttachDescs = nullptr;             // StructuredBuffer<FGIAG_AttachDescPacked>

		FRDGBufferUAVRef RW_InstanceOrigin = nullptr;       // RWBuffer<float4> (1 float4 per instance)
		FRDGBufferUAVRef RW_InstanceTransform = nullptr;    // RWBuffer<float4> (3 float4 per instance)
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddAttachToISMInstanceBuffersPasses(FRDGBuilder& GraphBuilder, const FAttachToISMInstanceBuffersPassParams& Params);

	/** RT direct-write: scatter per-instance Transform into RW_FxTransform (used for CPU mode sync / hide). */
	struct FScatterWriteFxTransformPassParams
	{
		uint32 NumWrites = 0;
		FRDGBufferSRVRef OutputIndices = nullptr; // StructuredBuffer<uint>
		FRDGBufferSRVRef ValuesTransform = nullptr; // StructuredBuffer<FGIAG_Transform> (SocketWS)
		FRDGBufferUAVRef RW_FxTransform = nullptr; // RWStructuredBuffer<FGIAG_Transform> (1 per instance)
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddScatterWriteFxTransformPasses(FRDGBuilder& GraphBuilder, const FScatterWriteFxTransformPassParams& Params);

	/** RT direct-write: scatter per-instance TRS into instance origin/transform buffers (used for CPU mode sync / hide for native buckets). */
	struct FScatterWriteInstanceBuffersPassParams
	{
		uint32 NumWrites = 0;
		FRDGBufferSRVRef OutputIndices = nullptr;           // StructuredBuffer<uint>
		FRDGBufferSRVRef ValuesTransform = nullptr;         // StructuredBuffer<FGIAG_Transform> (SocketWS)
		FRDGBufferUAVRef RW_InstanceOrigin = nullptr;       // RWBuffer<float4>
		FRDGBufferUAVRef RW_InstanceTransform = nullptr;    // RWBuffer<float4>
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddScatterWriteInstanceBuffersPasses(FRDGBuilder& GraphBuilder, const FScatterWriteInstanceBuffersPassParams& Params);

	/** RT direct-write: scatter per-slot transforms into slot-indexed buffers (used for dirty-slot incremental updates). */
	struct FScatterWriteTransformsBySlotPassParams
	{
		uint32 NumWrites = 0;
		FRDGBufferSRVRef OutputIndices = nullptr;           // StructuredBuffer<uint> (SlotIndex)
		FRDGBufferSRVRef ValuesComponentToWorld = nullptr;  // StructuredBuffer<FGIAG_Transform>
		FRDGBufferSRVRef ValuesWorldToComponent = nullptr;  // StructuredBuffer<FGIAG_Transform>
		FRDGBufferUAVRef RW_ComponentToWorldBySlot = nullptr; // RWStructuredBuffer<FGIAG_Transform>
		FRDGBufferUAVRef RW_WorldToComponentBySlot = nullptr; // RWStructuredBuffer<FGIAG_Transform>
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddScatterWriteTransformsBySlotPasses(FRDGBuilder& GraphBuilder, const FScatterWriteTransformsBySlotPassParams& Params);

	/** RT direct-write: scatter packed bytes into a slot-indexed param buffer (used for per-node param sparse updates). */
	struct FScatterWriteBytesByIndexPassParams
	{
		uint32 NumWrites = 0;
		uint32 StrideBytes = 0;                     // must be multiple of 4
		FRDGBufferSRVRef OutputIndices = nullptr;   // StructuredBuffer<uint> (SlotIndex)
		FRDGBufferSRVRef ValuesBytes = nullptr;     // ByteAddressBuffer (SRV PF_R32_UINT)
		FRDGBufferUAVRef RW_DstBytes = nullptr;     // RWByteAddressBuffer (UAV PF_R32_UINT)
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddScatterWriteBytesByIndexPasses(FRDGBuilder& GraphBuilder, const FScatterWriteBytesByIndexPassParams& Params);

	/** Utility: fill an RW uint buffer with a constant value (GPU-only, no CPU upload). */
	struct FFillUintBufferPassParams
	{
		uint32 NumDwords = 0;
		uint32 Value = 0;
		FRDGBufferUAVRef RW_Out = nullptr; // RWBuffer<uint>
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddFillUintBufferPasses(FRDGBuilder& GraphBuilder, const FFillUintBufferPassParams& Params);

	/** GPU AnimGraph culling prepass: build per-slot NodeNeeded bitset. */
	struct FGraphCullPassParams
	{
		/** Per-graph shader map, precompiled in cook or compiled on-demand in editor. */
		const FGIAGShaderMap* ShaderMap = nullptr;
		uint32 NumNodes = 0;
		uint32 NumInstances = 0;
		uint32 SlotCapacity = 0;
		uint32 WordsPerSlot = 0; // ceil(NumNodes/32) (used for output buffer layout)
		uint32 FinalNodeIndex = 0; // uint32(INDEX_NONE) => disable culling (fill all ones)

		FRDGBufferSRVRef ActiveInstanceIndices = nullptr; // StructuredBuffer<uint>
		/** Optional: cull-param SRVs in the same order as generated declarations for this permutation. */
		TConstArrayView<FRDGBufferSRVRef> CullParams;

		FRDGBufferUAVRef RW_NeedNodeBits = nullptr; // RWBuffer<uint> (slot-major words)
	};
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API void AddGraphCullPasses(FRDGBuilder& GraphBuilder, const FGraphCullPassParams& Params);

}


