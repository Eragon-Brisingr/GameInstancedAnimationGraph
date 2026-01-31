#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimCommon.h"
#include "RenderGraphResources.h"

#include "GIAG_AnimGraph.h"
#include "GIAG_AnimResources.h"

class USkeleton;
class FEvent;

/**
 * Incremental AnimLibrary upload blob.
 * Built on GT, consumed on RT; safe to share across many shard payloads via TSharedPtr.
 */
struct FGIAG_AnimLibraryClipMetaUpdate
{
	uint32 ClipIndex = 0;
	FGIAG_ClipMeta Meta;
};

using FGIAG_AnimTRSPtr = TSharedPtr<TArray<FGIAG_BoneTRS>>;
using FGIAG_EventPtr = TSharedPtr<FEvent>;

struct FGIAG_AnimLibraryAnimTRSUpdate
{
	/** Destination start transform (index into StructuredBuffer<FGIAG_BoneTRS>). */
	uint32 StartTransformIndex = 0;
	/** Number of FGIAG_BoneTRS elements. */
	uint32 NumTransforms = 0;

	/** Producer-owned transforms (written by worker, then treated as immutable). May be null on cancel/failure. */
	FGIAG_AnimTRSPtr TRS;

	/** Optional completion event; if set, RT may wait until signaled before reading TRS. */
	FGIAG_EventPtr CompletionEvent;
};

struct FGIAG_AnimLibraryRepackCopyOp
{
	/** Source start transform (index into old StructuredBuffer<FGIAG_BoneTRS>). */
	uint32 SrcStartTransformIndex = 0;
	/** Destination start transform (index into new StructuredBuffer<FGIAG_BoneTRS>). */
	uint32 DstStartTransformIndex = 0;
	/** Number of FGIAG_BoneTRS elements. */
	uint32 NumTransforms = 0;
};

struct FGIAG_AnimLibraryUploadData
{
	/** Total clip slot count (capacity). ClipIndex must be < NumClips. */
	uint32 NumClips = 0;
	/** AnimTRS buffer capacity (in FGIAG_BoneTRS elements). */
	uint32 AnimTRSCapacity = 0;
	uint32 NumBones = 0;
	uint32 Version = 0;

	/** Optional: one-time (or rebuild) RefPose upload. Layout is TRS (NumBones * FGIAG_BoneTRS). */
	uint32 RefPoseVersion = 0;
	TSharedPtr<const TArray<FGIAG_BoneTRS>> RefPoseLocalTRS;

	/** If true, rebuild AnimTRS buffer with AnimTRSCapacity and GPU-copy old->new according to RepackCopyOps. */
	bool bRepack = false;
	TArray<FGIAG_AnimLibraryRepackCopyOp> RepackCopyOps;

	/** Incremental updates. */
	TArray<FGIAG_AnimLibraryClipMetaUpdate> ClipMetaUpdates;
	TArray<FGIAG_AnimLibraryAnimTRSUpdate> AnimTRSUpdates;
};
using FGIAG_AnimLibraryUploadDataPtr = TSharedPtr<const FGIAG_AnimLibraryUploadData>;

/**
 * Incremental per-shard transform upload blob.
 * Built on GT, consumed on RT.
 *
 * Contract:
 * - SlotCapacity matches Params.SlotCapacity for the shard evaluation.
 * - DirtySlots and DirtyComponentToWorld are 1:1 aligned.
 * - DirtySlots contains unique slot indices (no duplicates).
 */
struct FGIAG_TransformUploadData
{
	uint32 SlotCapacity = 0;
	TArray<uint32> DirtySlots;
	/** Per-slot ComponentToWorld transforms (slot-indexed, only for DirtySlots). */
	TArray<FTransform3f> DirtyComponentToWorld;

	/**
	 * Optional per-slot init flag (size SlotCapacity):
	 * - InitPrevBySlot[SlotIndex] != 0 indicates this SlotIndex is newly created this frame, so we should set previous=current
	 *   (avoid first-frame velocity spikes from uninitialized old current).
	 */
	TArray<uint32> InitPrevBySlot;
};
using FGIAG_TransformUploadDataPtr = TSharedPtr<const FGIAG_TransformUploadData>;

/** GT-side packed uploads collected by Runner for one evaluation. */
struct FGIAG_AnimGraphNodeUploadRun
{
	int32 NodeIndex = INDEX_NONE;
	uint32 StrideBytes = 0;
	/** Slot indices to write (slot-indexed param buffers). */
	TArray<uint32> InstanceIndices;
	/** Packed bytes: StrideBytes * InstanceIndices.Num(). */
	TArray<uint8> Bytes;
};

struct FGIAG_AnimGraphResourceUploadRun
{
	FGIAG_AnimResourceRequest Request;
	/** Immutable CPU bytes (built on GT, consumed on RT). */
	TSharedPtr<TArray<uint8>> Bytes;
};

struct FGIAG_AnimGraphUploads
{
	// Static skeleton (uploaded once)
	TArray<int32> ParentIndices;
	TArray<FGIAG_BoneTRS> InverseRefPoseTRS;
	bool bUploadSkeleton = false;

	// Per-node param sparse uploads (slot-indexed)
	/** StrideBytes for each node param buffer (GT-derived, consumed on RT). Size = CompiledData.NumNodes. */
	TArray<uint32> NodeParamStrideBytesByNode;
	TArray<FGIAG_AnimGraphNodeUploadRun> NodeRuns;

	// Optional shared resources declared by node meta
	TArray<FGIAG_AnimGraphResourceUploadRun> ResourceRuns;
	int32 MaxOptionalSRVSlot = -1;
	TArray<TArray<FGIAG_AnimResourceKey>> OptionalSRVKeyByNodeBySlot; // [NodeIndex][Slot] -> ShareKey (IsNone = none)
};

/**
 * Runner resources shared across frames for a compiled graph.
 * Keeps persistent pooled buffers to enable event-driven (dirty) uploads.
 */
struct FGIAG_AnimGraphPersistentResources
{
	/** Pose buffers for each node output pin (flattened). */
	struct FPoseResource
	{
		TRefCountPtr<FRDGPooledBuffer> Buffer;
		uint32 NumTransforms = 0;
	};

	/** Node-type parameter buffer for a specific node in the graph (first version: one buffer per node). */
	struct FNodeParamResource
	{
		TRefCountPtr<FRDGPooledBuffer> Buffer;
		uint32 StrideBytes = 0;
		uint32 NumInstances = 0;
	};

	TArray<FPoseResource> PoseOutputs;
	TArray<FNodeParamResource> NodeParams;

	/** Static skeleton buffers. */
	TRefCountPtr<FRDGPooledBuffer> ParentIndices;
	TRefCountPtr<FRDGPooledBuffer> InverseRefPoseTRS;

	/** Per-slot world->component transform (TRS), sized by SlotCapacity. */
	TRefCountPtr<FRDGPooledBuffer> WorldToComponentBySlot;
	/** Per-slot component->world transform (TRS), sized by SlotCapacity. */
	TRefCountPtr<FRDGPooledBuffer> ComponentToWorldBySlot;

	/** ActiveInstanceIndices GPU buffer (StructuredBuffer<uint32>) and last uploaded CPU mirror. */
	TRefCountPtr<FRDGPooledBuffer> ActiveInstanceIndices;
	TArray<uint32> ActiveInstanceIndicesCPU;
	uint32 ActiveInstanceIndicesNum = 0;

	/**
	 * GPU node culling mask (optional).
	 * Layout: uint bitset words, packed by (SlotIndex, NodeIndex).
	 * WordsPerSlot = ceil(NumNodes/32).
	 */
	TRefCountPtr<FRDGPooledBuffer> NeedNodeBits;
	uint32 NeedNodeWordsPerSlot = 0;
	uint32 NeedNodeBitsNumNodes = 0;
	uint32 NeedNodeBitsSlotCapacity = 0;

	/**
	 * RT-only: prev-cache for TransformBuffer motion blur.
	 * Layout: RWBuffer<float4>, 3 float4 rows per (SlotIndex, BoneIndex) storing a float3x4 current matrix.
	 * Index = (SlotIndex * NumBones + BoneIndex) * 3 + Row (0..2).
	 */
	TRefCountPtr<FRDGPooledBuffer> PrevCacheFloat4;
};

/** Debug: request GPU->CPU readback of Final LocalPose for one slot (slot-indexed). */
struct FGIAG_LocalPoseReadbackRequest
{
	int32 RecordIndex = INDEX_NONE;
	int32 SerialNumber = INDEX_NONE;
	uint32 SlotIndex = 0;
};

/** Debug: request GPU->CPU readback of NeedNodeBits words for one slot. */
struct FGIAG_NeedNodeBitsReadbackRequest
{
	int32 RecordIndex = INDEX_NONE;
	int32 SerialNumber = INDEX_NONE;
	uint32 SlotIndex = 0;
};

/** Execution params for one run. */
struct FGIAG_AnimGraphRunParams
{
	int32 NumInstances = 0;
	/** Total slot capacity for this group (stable, allows holes). GPU buffers are sized by SlotCapacity. */
	int32 SlotCapacity = 0;
	int32 NumBones = 0;
	float CurrentTimeSeconds = 0.0f;

	/** Source skeleton asset (identity / validation). */
	USkeleton* Skeleton = nullptr;

	/** Static skeleton data (bone order == Skeleton reference skeleton). */
	const TArray<int32>* ParentIndices = nullptr;
	const TArray<FGIAG_BoneTRS>* InverseRefPoseTRS = nullptr;

	/** Optional incremental transform upload (RT computes World->Component and uploads sparse ranges). */
	FGIAG_TransformUploadDataPtr TransformUpload;

	/** Optional active instance indices mapping (ActiveIndex -> AbsoluteInstanceIndex). */
	TArray<uint32> ActiveInstanceIndices;

	/** Debug: monotonic frame id assigned on GT when this evaluation was submitted. */
	uint64 DebugCpuRequestFrame = 0;
	/** Debug: per-slot readback requests for this evaluation (MasterEvaluate only). */
	TArray<FGIAG_LocalPoseReadbackRequest> DebugLocalPoseReadbackRequests;
	/** Debug: per-slot NeedNodeBits readback requests for this evaluation (MasterEvaluate only). */
	TArray<FGIAG_NeedNodeBitsReadbackRequest> DebugNeedNodeBitsReadbackRequests;

	/** Optional AnimLibrary upload request (set when dirty). */
	FGIAG_AnimLibraryUploadDataPtr AnimLibraryUpload;
	uint32 AnimLibraryVersion = 0;
	uint32 NumClips = 0;
	uint32 AnimTRSNum = 0;

	/** Optional: output directly into UE SkinningSceneExtension TransformBuffer (RDG buffer). */
	FRDGBufferRef OutputTransformBuffer = nullptr;
	uint32 TransformBufferOffset = 0;
};

/**
 * AnimGraph runner: binds a compiled graph, maintains persistent buffers, and enqueues GPU work.
 * Runtime CPU should be O(numDirty + numBatches); no runtime topo/scheduling.
 */
class GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimGraphGpuRunner
{
public:
	FGIAG_AnimGraphGpuRunner() = default;

	struct FOutputs
	{
		/** Slot-indexed Final LocalPose buffer (TRS, NumBones * FGIAG_BoneTRS per slot). Null if graph has no final pose output. */
		FRDGBufferRef FinalLocalPoseBuffer = nullptr;
		/** Slot-indexed NeedNodeBits buffer (StructuredBuffer<uint32>). Null if not allocated. */
		FRDGBufferRef NeedNodeBitsBuffer = nullptr;
		/** NumNodes used to generate NeedNodeBits. */
		uint32 NeedNodeNumNodes = 0;
		/** WordsPerSlot for NeedNodeBits (ceil(NumNodes/32)). */
		uint32 NeedNodeWordsPerSlot = 0;
		/** Static skeleton parent indices (StructuredBuffer<int>). */
		FRDGBufferSRVRef ParentIndicesSRV = nullptr;
		/** Per-slot component->world transforms (StructuredBuffer<FGIAG_Transform>). */
		FRDGBufferSRVRef ComponentToWorldBySlotSRV = nullptr;
	};

	/** Render-thread: add all RDG passes for one evaluation to an existing GraphBuilder (caller executes). */
	FOutputs AddPasses_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FGIAG_AnimGraphCompiledData& CompiledData,
		const FGIAG_AnimGraphRunParams& Params,
		FGIAG_AnimGraphUploads&& Uploads,
		const GIAG::FAnimLibraryBuffers& AnimLibraryBuffers,
		FGIAG_AnimResourceCache& AnimResourceCache);

private:
	FGIAG_AnimGraphPersistentResources Resources;
};


