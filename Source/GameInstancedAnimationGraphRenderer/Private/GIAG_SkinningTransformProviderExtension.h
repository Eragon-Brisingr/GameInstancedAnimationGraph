#pragma once

#include "GIAG_AnimCommon.h"
#include "GIAG_AnimResources.h"
#include "Delegates/Delegate.h"
#include "SceneExtensions.h"
#include "Skinning/SkinningTransformProvider.h"
#include "UObject/ObjectKey.h"

#include "GIAG_AnimGraphShaders.h"
#include "GameInstancedAnimationGraphSubsystem.h" // FGIAG_AttachBus::EBucketKind

class USkeleton;
class FRDGPooledBuffer;
class FRHIGPUBufferReadback;
class UGameInstancedAnimationGraphSubsystem;
class FGIAG_NiagaraAttachRegistry;
class FGIAG_NativeAttachRegistry;
struct FGIAG_TransformProviderState;
struct FGIAG_SharedResourceBus;
struct FGIAG_AttachBus;
struct FGIAG_AttachReadbackBus;

/**
 * Scene extension that registers our GIAG TransformProvider into UE's FSkinningTransformProvider.
 *
 * This module is the only one that touches Renderer private headers.
 */
class FGIAG_SkinningTransformProviderExtension final : public ISceneExtension
{
	DECLARE_SCENE_EXTENSION(GAMEINSTANCEDANIMATIONGRAPHRENDERER_API, FGIAG_SkinningTransformProviderExtension);

public:
	using ISceneExtension::ISceneExtension;
	virtual ~FGIAG_SkinningTransformProviderExtension() override;

	static bool ShouldCreateExtension(FScene& InScene);
	virtual void InitExtension(FScene& InScene) override;
	virtual ISceneExtensionRenderer* CreateRenderer(FSceneRendererBase& InSceneRenderer, const FEngineShowFlags& EngineShowFlags) override;

private:
	friend class FGIAG_SkinningTransformProviderExtensionRenderer;
	void ProvideTransforms(FSkinningTransformProvider::FProviderContext& Context);
	void ProcessAttachOpsAndOutputs_RT(FRDGBuilder& GraphBuilder);

	/** RT-safe handle to shared resource bus (published by GT subsystem). */
	TSharedPtr<FGIAG_SharedResourceBus, ESPMode::ThreadSafe> SharedResourceBus_RT;
	/** RT-safe handle to attach bus (published by GT subsystem). */
	TSharedPtr<FGIAG_AttachBus, ESPMode::ThreadSafe> AttachBus_RT;
	/** RT-safe handle to attach readback bus (published by GT subsystem). */
	TSharedPtr<FGIAG_AttachReadbackBus, ESPMode::ThreadSafe> AttachReadbackBus_RT;
	/** RT-safe handle to Niagara attach registry (owned by GT subsystem; RT reads/writes). */
	TSharedPtr<FGIAG_NiagaraAttachRegistry, ESPMode::ThreadSafe> NiagaraAttachRegistry_RT;
	/** RT-safe handle to native attach registry (owned by GT subsystem; RT reads/writes). */
	TSharedPtr<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe> NativeAttachRegistry_RT;

	// ---- RT-only Niagara attach state ----
	using FAttachDescPacked = FGIAG_AttachDescPacked;
	struct FAttachGroupRT
	{
		TArray<FAttachDescPacked> CPU;
		// Stable mapping for incremental updates/removals.
		TMap<uint32, int32> IndexByAttachSlot;
		bool bDirty = false;
		TRefCountPtr<FRDGPooledBuffer> DescUploadBuffer;
	};
	struct FAttachGroupKey
	{
		const FGIAG_TransformProviderState* State = nullptr;
		uint32 BucketId = 0;
		bool operator==(const FAttachGroupKey&) const = default;
	};
	friend uint32 GetTypeHash(const FAttachGroupKey& K)
	{
		return HashCombine(::GetTypeHash((uintptr_t)K.State), ::GetTypeHash(K.BucketId));
	}
	struct FAttachBucketRT
	{
		uint32 NumInstances = 0;
		FGIAG_AttachBus::EBucketKind BucketKind = FGIAG_AttachBus::EBucketKind::Niagara;
		bool IsNativeInstanceOnly() const { return BucketKind == FGIAG_AttachBus::EBucketKind::Native; }

		// Niagara bucket
		// world-space socket TRS output (3 float4 per instance).
		TRefCountPtr<FRDGPooledBuffer> FxTransformBuffer;
		// meta buffers for Niagara DI spawn/kill/resolve.
		TRefCountPtr<FRDGPooledBuffer> SlotToDenseIndexBuffer; // int32
		TRefCountPtr<FRDGPooledBuffer> SlotGenerationBuffer;   // int32
		TRefCountPtr<FRDGPooledBuffer> FxParticleGenBuffer;    // int32
		TRefCountPtr<FRDGPooledBuffer> AddListPackedBuffer;    // int32 packed=(FxGen<<16)|Slot
		uint32 SlotTableVersion = 0;
		uint32 AddListVersion = 0;
		uint32 AddListCount = 0;
		// RT-only: defer VM snapshot publish until after we have ensured buffers exist and the registry is updated.
		bool bPendingVmPublishAddList = false;
		uint32 PendingVmPublishAddListVersion = 0;
		uint32 PendingVmPublishAddListCount = 0;

		// native bucket
		// UE instancing buffers used by native attach mesh renderer.
		TRefCountPtr<FRDGPooledBuffer> InstanceOriginBuffer;
		TRefCountPtr<FRDGPooledBuffer> InstanceTransformBuffer;

		struct FPendingWriteTransform
		{
			uint32 OutputIndex = 0;
			FGIAG_Transform FxTransform = FGIAG_Transform::Identity;
		};
		// RT direct-write (CPU mode sync / hide):
		// - PendingWritesTransform: Niagara bucket writes into FxTransformBuffer.
		// - PendingWritesInstance: native bucket writes into InstanceOrigin/InstanceTransform buffers.
		TArray<FPendingWriteTransform> PendingWritesTransform;
		TArray<FPendingWriteTransform> PendingWritesInstance;
	};
	TMap<FAttachGroupKey, FAttachGroupRT> AttachGroupsByStateBucket_RT;
	TMap<uint32, FAttachBucketRT> AttachBuckets_RT;

	/** RT-only: pending GPU->CPU readbacks for debug LocalPose slices. */
	struct FPendingLocalPoseReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		uint32 NumBytes = 0;
		uint32 NumBones = 0;
		uint64 CpuRequestFrame = 0;
		int32 RecordIndex = INDEX_NONE;
		int32 SerialNumber = INDEX_NONE;
	};
	TArray<FPendingLocalPoseReadback> PendingLocalPoseReadbacks_RT;

	/** RT-only: pending GPU->CPU readbacks for debug NeedNodeBits slices (one slot, WordsPerSlot uint32). */
	struct FPendingNeedNodeBitsReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		uint32 NumBytes = 0;
		uint64 CpuRequestFrame = 0;
		int32 RecordIndex = INDEX_NONE;
		int32 SerialNumber = INDEX_NONE;
		uint32 SlotIndex = 0;
		uint32 NumNodes = 0;
		uint32 WordsPerSlot = 0;
	};
	TArray<FPendingNeedNodeBitsReadback> PendingNeedNodeBitsReadbacks_RT;

	/** RT-only: pending RT->GT readbacks for Niagara attach FxTransform entries. */
	struct FPendingAttachFxTransformReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		uint32 NumBytes = 0;
		uint32 BucketId = 0;
		uint32 OutputIndex = 0;
		uint64 CpuRequestFrame = 0;
	};
	TArray<FPendingAttachFxTransformReadback> PendingAttachFxTransformReadbacks_RT;

	/** RT-only: pending RT->GT readbacks for native attach instance buffers (Origin + 3 rows). */
	struct FPendingAttachInstanceBuffersReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		uint32 NumBytes = 0; // always 4*float4
		uint32 BucketId = 0;
		uint32 OutputIndex = 0;
		uint64 CpuRequestFrame = 0;
	};
	TArray<FPendingAttachInstanceBuffersReadback> PendingAttachInstanceBuffersReadbacks_RT;

	/** RT-only: per-skeleton AnimLibrary GPU buffers (owned by this scene extension instance). */
	struct FAnimLibraryRTCacheEntry
	{
		GIAG::FAnimLibraryBuffers Buffers;
		uint32 Version = 0;
	};
	TMap<FObjectKey, FAnimLibraryRTCacheEntry> AnimLibraryBySkeleton_RT;

	/** RT-only: cache bone remap tables into persistent GPU buffers (keyed by pointer identity + size). */
	struct FBoneRemapKey
	{
		const uint32* Ptr = nullptr;
		uint32 Num = 0;
		bool operator==(const FBoneRemapKey&) const = default;
	};
	friend uint32 GetTypeHash(const FBoneRemapKey& K)
	{
		return HashCombine(::GetTypeHash((uintptr_t)K.Ptr), ::GetTypeHash(K.Num));
	}
	struct FBoneRemapRTCacheEntry
	{
		TRefCountPtr<FRDGPooledBuffer> Buffer;
		uint32 Num = 0;
	};
	TMap<FBoneRemapKey, FBoneRemapRTCacheEntry> BoneRemapBufferByKey_RT;

	/** RT-only: per-scene resource cache (ShareKey -> external pooled buffer). */
	FGIAG_AnimResourceCache AnimResourceCache_RT;
};

