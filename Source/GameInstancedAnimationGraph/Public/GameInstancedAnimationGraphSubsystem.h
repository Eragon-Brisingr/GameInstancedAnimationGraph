// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h" // FRWLock
#include "Misc/TVariant.h"
#include "GameInstancedAnimationGraphHandle.h"
#include "GIAG_AnimCommon.h"
#include "GIAG_AnimGraphCpuRunner.h"
#include "InstanceDataTypes.h"
#include "GIAG_AnimGraph.h"
#include "GIAG_AnimGraphGpuRunner.h"
#include "GIAG_AnimGraphUploadBuilder.h"
#include "GIAG_TimeSlot.h"
#include "Subsystems/WorldSubsystem.h"
#include "GameInstancedAnimationGraphSubsystem.generated.h"

struct FGIAG_TransformProviderState;
class UStaticMesh;
class UStaticMeshComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class FGIAG_NiagaraAttachRegistry;
class FGIAG_NativeAttachRegistry;

/** Thread-safe bus for RT to read the latest shared optional resources without touching UObjects. */
struct FGIAG_SharedResourceBus
{
	/** GT (single producer) -> RT (single consumer) incremental publish (MPSC safe if we ever add more GT producers). */
	TQueue<FGIAG_AnimGraphResourceUploadRun, EQueueMode::Mpsc> PendingRuns;

	void Enqueue_GameThread(FGIAG_AnimGraphResourceUploadRun&& Run)
	{
		check(IsInGameThread());
		PendingRuns.Enqueue(MoveTemp(Run));
	}

	bool Dequeue_RenderThread(FGIAG_AnimGraphResourceUploadRun& OutRun)
	{
		check(IsInRenderingThread());
		return PendingRuns.Dequeue(OutRun);
	}
};

/** Thread-safe bus for RT to read incremental Niagara-attach operations without touching UObjects. */
struct FGIAG_AttachBus
{
	enum class EBucketKind : uint8
	{
		Niagara = 0,
		Native = 1,
	};

	struct FAttachAddOp
	{
		uint32 BucketId = 0;
		EBucketKind BucketKind = EBucketKind::Niagara;
		uint32 AttachSlot = 0;
		FGIAG_TransformProviderState* State = nullptr;
		FGIAG_AttachDescPacked Desc;
	};

	struct FAttachUpdateOp
	{
		uint32 BucketId = 0;
		EBucketKind BucketKind = EBucketKind::Niagara;
		uint32 AttachSlot = 0;
		FGIAG_TransformProviderState* State = nullptr;
		FGIAG_AttachDescPacked Desc;
	};

	struct FAttachRemoveOp
	{
		uint32 BucketId = 0;
		EBucketKind BucketKind = EBucketKind::Niagara;
		uint32 AttachSlot = 0;
		FGIAG_TransformProviderState* State = nullptr;
	};

	struct FWriteFxTransformOp
	{
		uint32 BucketId = 0;
		uint32 OutputIndex = 0;
		FGIAG_Transform TransformWS = FGIAG_Transform::Identity;
	};

	struct FWriteInstanceOp
	{
		uint32 BucketId = 0;
		uint32 OutputIndex = 0;
		FGIAG_Transform TransformWS = FGIAG_Transform::Identity;
	};

	struct FPublishNiagaraMetaOp
	{
		uint32 BucketId = 0;

		// Contract: payload arrays are immutable after enqueue (shared ptr), GT owns lifetime.
		uint32 SlotTableVersion = 0;
		uint32 AddListVersion = 0;
		uint32 AddListCount = 0;
		TSharedPtr<const TArray<int32>> SlotToDenseIndex;    // int32[SlotCap], -1 => invalid
		TSharedPtr<const TArray<int32>> SlotGeneration;     // int32[SlotCap]
		TSharedPtr<const TArray<int32>> FxParticleGenBySlot; // int32[SlotCap]
		TSharedPtr<const TArray<int32>> AddListPacked;      // int32[AddListCount], Packed=(FxGen<<16)|Slot
	};

	using FOp = TVariant<
		FAttachAddOp,
		FAttachUpdateOp,
		FAttachRemoveOp,
		FWriteFxTransformOp,
		FWriteInstanceOp,
		FPublishNiagaraMetaOp>;

	/** GT (single producer) -> RT (single consumer) incremental publish (MPSC safe if we ever add more GT producers). */
	TQueue<FOp, EQueueMode::Mpsc> PendingOps;

	// ---- AnyThread snapshot for Niagara VM spawn gating ----
	//
	// Contract:
	// - Updated on RT when the corresponding PublishNiagaraMeta op is *consumed* (i.e., after RT has scheduled uploads).
	// - Read on AnyThread by Niagara VM to decide whether to spawn for a new add-list version.
	// This intentionally introduces a one-frame delay (GT publish -> RT consume -> VM sees),
	// but guarantees VM only reacts to versions that are already GPU-visible to Niagara.
	mutable FRWLock VmAddListLock;
	struct FVmAddListSnapshot
	{
		uint32 Version = 0;
		uint32 Count = 0;
		// Frame counter at the time RT published this snapshot.
		// VM will only consume snapshots from a strictly earlier frame to guarantee GPU visibility.
		uint64 PublishFrame = 0;
	};
	TMap<uint32, FVmAddListSnapshot> VmAddListByBucketId;

	template<typename TOp>
	void Enqueue_GameThread(TOp&& Op)
	{
		check(IsInGameThread());
		using TOpDecayed = typename TDecay<TOp>::Type;
		PendingOps.Enqueue(FOp(TInPlaceType<TOpDecayed>(), Forward<TOp>(Op)));
	}

	bool Dequeue_RenderThread(FOp& OutOp)
	{
		check(IsInRenderingThread());
		return PendingOps.Dequeue(OutOp);
	}

	/** RT-only: publish the latest GPU-visible add-list (version,count) for Niagara VM. */
	void NiagaraVm_UpdateAddList_RenderThread(uint32 BucketId, uint32 Version, uint32 Count)
	{
		check(IsInRenderingThread());
		check(BucketId != 0u);
		FWriteScopeLock WriteLock(VmAddListLock);
		FVmAddListSnapshot& Snap = VmAddListByBucketId.FindOrAdd(BucketId);
		Snap.Version = Version;
		Snap.Count = Count;
		Snap.PublishFrame = GFrameCounter;
	}

	/** AnyThread: read the latest GPU-visible add-list (version,count) for Niagara VM. */
	bool NiagaraVm_GetAddList_AnyThread(uint32 BucketId, uint32& OutVersion, uint32& OutCount) const
	{
		OutVersion = 0u;
		OutCount = 0u;
		if (BucketId == 0u)
		{
			return false;
		}
		FReadScopeLock ReadLock(VmAddListLock);
		if (const FVmAddListSnapshot* Snap = VmAddListByBucketId.Find(BucketId))
		{
			// Enforce at least one-frame separation between RT publish and VM consume,
			// so Niagara will never spawn in the same frame as the GPU upload scheduling.
			const uint64 Now = GFrameCounter;
			if (Snap->PublishFrame >= Now)
			{
				return false;
			}
			OutVersion = Snap->Version;
			OutCount = Snap->Count;
			return true;
		}
		return false;
	}
};

/** Thread-safe bus for GT to request RT readbacks of the Niagara attach FxTransform buffer. */
struct FGIAG_AttachReadbackBus
{
	struct FRequest
	{
		uint32 BucketId = 0;
		uint32 OutputIndex = 0;
		uint64 CpuRequestFrame = 0;
	};

	/** GT (single producer) -> RT (single consumer) incremental publish (MPSC safe if we ever add more GT producers). */
	TQueue<FRequest, EQueueMode::Mpsc> Pending;

	void Enqueue_GameThread(FRequest&& Req)
	{
		check(IsInGameThread());
		Pending.Enqueue(MoveTemp(Req));
	}

	bool Dequeue_RenderThread(FRequest& OutReq)
	{
		check(IsInRenderingThread());
		return Pending.Dequeue(OutReq);
	}
};

class UInstancedSkinnedMeshComponent;
class UGIAG_TransformProviderData;
class UGIAG_AttachMeshComponent;
class USkeletalMesh;
class USkeleton;
class UAnimSequence;
class USkeletalMeshComponent;
struct FGIAG_AnimNodeRef;
struct FGIAG_AnimationsTestAPI;

template<typename T>
struct TGIAG_AnimNodePtr;

inline uint32 GetTypeHash(const FPrimitiveInstanceId& Id) { return GetTypeHash(Id.Id); }

UCLASS()
class GAMEINSTANCEDANIMATIONGRAPH_API UGameInstancedAnimationGraphSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
	
	friend FGIAG_AnimationsTestAPI;
	friend FGameInstancedAnimationGraphHandle;
	friend FGameInstancedAnimationAttachHandle;
protected:
	bool DoesSupportWorldType(const EWorldType::Type WorldType) const override { return WorldType != EWorldType::None && WorldType != EWorldType::Inactive && WorldType != EWorldType::GameRPC; }
public:
	TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(GameInstancedAnimSubsystem, STATGROUP_Tickables); }
	bool IsTickableInEditor() const override { return true; }
	void Tick(float DeltaTime) override;
	void Initialize(FSubsystemCollectionBase& Collection) override;
	void Deinitialize() override;

	// ---- TimeSlot API ----
	FGIAG_TimeSlot AllocateTimeSlot();
	void FreeTimeSlot(FGIAG_TimeSlot Slot);
	void SetTimeSlotSeconds(FGIAG_TimeSlot Slot, float Seconds);
	float GetTimeSlotSeconds(FGIAG_TimeSlot Slot) const;

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	FGameInstancedAnimationGraphHandle AddInstance(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, const FTransform& Transform, TSubclassOf<AActor> CpuProxyClass, bool bCpuMode = false, FGIAG_TimeSlot TimeSlot = FGIAG_TimeSlot());

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CPU")
	FGameInstancedAnimationGraphHandle AddInstanceWithExternalProxyActor(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, const FTransform& Transform, AActor* CpuProxyActor, FGIAG_TimeSlot TimeSlot = FGIAG_TimeSlot());

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void RemoveInstance(UPARAM(Ref)FGameInstancedAnimationGraphHandle& Handle, bool bAllowAutoShrink = true);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void ReserveBucketCapacity(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, int32 Count);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void ShrinkBucket(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void ShrinkAllBuckets();

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	int32 GetBucketSlotCapacity(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph) const;

	/** Switch this handle between GPU backend (ISKMC) and CPU backend (CpuProxyActor). Master only. */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CPU")
	void SetInstanceUseCPUMode(const FGameInstancedAnimationGraphHandle& Handle, bool bUseCPU);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CPU")
	bool IsInstanceUsingCPUMode(const FGameInstancedAnimationGraphHandle& Handle) const;

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void SetInstanceTransform(const FGameInstancedAnimationGraphHandle& Handle, const FTransform& NewTransform, bool bTeleport = false);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	FTransform GetInstanceTransform(const FGameInstancedAnimationGraphHandle& Handle) const;

	// ---- Per-instance custom data (material parameters) ----

	void SetInstanceCustomDataRange(const FGameInstancedAnimationGraphHandle& Handle, int32 StartIndex, TConstArrayView<float> Values);
	const float* GetInstanceCustomDataPtr(const FGameInstancedAnimationGraphHandle& Handle, int32& OutNum) const;

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	void SetInstanceCustomDataFloat(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex, float Value) { SetInstanceCustomDataRange(Handle, DataIndex, MakeArrayView(&Value, 1)); }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	void SetInstanceCustomDataVector2(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex, FVector2f Value) { SetInstanceCustomDataRange(Handle, DataIndex, MakeArrayView(&Value.X, 2)); }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	void SetInstanceCustomDataVector3(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex, FVector3f Value) { SetInstanceCustomDataRange(Handle, DataIndex, MakeArrayView(&Value.X, 3)); }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	void SetInstanceCustomDataColor(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex, FLinearColor Value) { SetInstanceCustomDataRange(Handle, DataIndex, MakeArrayView(&Value.R, 4)); }

	void SetInstanceCustomData(const FGameInstancedAnimationGraphHandle& Handle, TConstArrayView<float> Values) { SetInstanceCustomDataRange(Handle, 0, Values); }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	float GetInstanceCustomDataFloat(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex) const { int32 N=0; const float* P=GetInstanceCustomDataPtr(Handle,N); return (P && DataIndex>=0 && DataIndex<N) ? P[DataIndex] : 0.f; }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	FVector2f GetInstanceCustomDataVector2(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex) const { int32 N=0; const float* P=GetInstanceCustomDataPtr(Handle,N); return (P && DataIndex>=0 && DataIndex+2<=N) ? FVector2f(P[DataIndex],P[DataIndex+1]) : FVector2f::ZeroVector; }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	FVector3f GetInstanceCustomDataVector3(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex) const { int32 N=0; const float* P=GetInstanceCustomDataPtr(Handle,N); return (P && DataIndex>=0 && DataIndex+3<=N) ? FVector3f(P[DataIndex],P[DataIndex+1],P[DataIndex+2]) : FVector3f::ZeroVector; }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|CustomData")
	FLinearColor GetInstanceCustomDataColor(const FGameInstancedAnimationGraphHandle& Handle, int32 DataIndex) const { int32 N=0; const float* P=GetInstanceCustomDataPtr(Handle,N); return (P && DataIndex>=0 && DataIndex+4<=N) ? FLinearColor(P[DataIndex],P[DataIndex+1],P[DataIndex+2],P[DataIndex+3]) : FLinearColor(0,0,0,0); }

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void PlayAnimation(const FGameInstancedAnimationGraphHandle& Handle, const UAnimSequence* AnimSequence, FName NodeName = TEXT("Default"), float BlendDurationSeconds = 0.2f, float StartSeconds = 0.f, bool bLoop = true, float PlayRate = 1.f);

	template<typename T>
	TGIAG_AnimNodePtr<T> FindAnimNode(const FGameInstancedAnimationGraphHandle& Handle, FName NodeName) const
	{
		TGIAG_AnimNodePtr<T> NodePtr;
		NodePtr.NodePtr = (T*)FindAnimNodeImpl(Handle, NodeName, NodePtr);
		if constexpr (std::is_same_v<T, FGIAG_AnimNodeBase> == false)
		{
			if (!ensure(NodePtr.NodePtr == nullptr || T::StaticStruct() == GetAnimNodeStruct(NodePtr)))
			{
				return {};
			}
		}
		return NodePtr;
	}
	
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	void CleanupUnusedAnimations(float OlderThanSeconds = 60.0f);

	/** Create a follow instance which shares Master's animation and transform. */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim")
	FGameInstancedAnimationGraphHandle AddFollowInstance(const FGameInstancedAnimationGraphHandle& MasterHandle, USkeletalMesh* SkeletalMesh);

	// ---- Attach API (no GPU->CPU readback) ----
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Attach")
	FGameInstancedAnimationAttachHandle AttachStaticMesh(const FGameInstancedAnimationGraphHandle& Handle, UStaticMesh* StaticMesh, FName BoneName, const FTransform& SocketLocalTransform, bool bCreateCpuProxyMesh = false);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Attach")
	FGameInstancedAnimationAttachHandle AttachNiagara(const FGameInstancedAnimationGraphHandle& Handle, UNiagaraSystem* NiagaraSystem, FName BoneName, const FTransform& SocketLocalTransform, UNiagaraSystem* CpuProxyNiagaraSystem = nullptr);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Attach")
	void UpdateAttach(const FGameInstancedAnimationAttachHandle& Handle, FName NewBoneName, const FTransform& NewSocketLocalTransform);

	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Attach")
	void RemoveAttach(const FGameInstancedAnimationAttachHandle& Handle);

	/** Debug: enable/disable GPU->CPU readback of this instance's Final LocalPose. Master instances only. */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	void DebugSetLocalPoseReadbackEnabled(const FGameInstancedAnimationGraphHandle Handle, bool bEnabled);

	/**
	 * Debug: get the latest completed readback for this handle.
	 * OutLocalPoseTRS is sized NumBones (one FTransform3f per bone).
	 * Returns false if no completed readback is available yet.
	 */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugGetLatestLocalPoseReadbackTRS(const FGameInstancedAnimationGraphHandle Handle, int64& OutCpuRequestFrame, int32& OutNumBones, TArray<FTransform3f>& OutLocalPoseTRS);

	/** Debug: enable/disable GPU->CPU readback of this instance's NeedNodeBits (GraphCull output). Master instances only. */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	void DebugSetNeedNodeBitsReadbackEnabled(const FGameInstancedAnimationGraphHandle Handle, bool bEnabled);

	/**
	 * Debug: get the latest completed NeedNodeBits readback for this handle.
	 * OutWords size == OutWordsPerSlot.
	 */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugGetLatestNeedNodeBitsReadback(const FGameInstancedAnimationGraphHandle Handle, int64& OutCpuRequestFrame, int32& OutSlotIndex, int32& OutNumNodes, int32& OutWordsPerSlot, TArray<int32>& OutWords);

	/** Debug: CPU backend only. Return current BoneSpaceTransforms as TRS (NumBones * FTransform3f). */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugGetCpuLocalPoseTRS(const FGameInstancedAnimationGraphHandle Handle, int64& OutCpuFrame, int32& OutNumBones, TArray<FTransform3f>& OutLocalPoseTRS);

	/** Debug: resolve attachment OutputIndex (dense index into the bucket FxTransform buffer). */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugResolveAttachOutputIndex(const FGameInstancedAnimationAttachHandle& Handle, int32& OutOutputIndex) const;

	/** Debug: return GT-side bucket entry count (dense OutputIndex space). */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugGetAttachBucketEntryCount(int32 BucketId, int32& OutEntryCount) const;

	// ---- CPU pose cache API (used by AnimBP Source node; no locks by design) ----
	bool TryGetCpuPoseCache_NoLock(const FGameInstancedAnimationGraphHandle& Handle, uint64& OutFrame, TConstArrayView<FTransform3f>& OutLocalPose) const;
	/** Evaluate this handle on CPU and return skeleton-indexed local pose. Any thread; no locks by design. */
	bool EvalCpuAnimationPoseAnyThread(const FGameInstancedAnimationGraphHandle& Handle, TArray<FTransform3f>& OutLocalPose) const;

	/** Convert skeleton-indexed LocalPose (bone local space) into ComponentPose (component space) */
	void ConvertLocalPoseToComponentPoseChecked(const FGameInstancedAnimationGraphHandle& Handle, TConstArrayView<FTransform3f> LocalPose, TArray<FTransform3f>& OutComponentPose) const;

	/** C++ helper: return cached skeleton ParentIndices for this Handle. */
	const TArray<int32>& GetSkeletonParentIndicesChecked(const FGameInstancedAnimationGraphHandle& Handle) const;

	// Extra padding added to the culling sphere radius (in cm).
	UPROPERTY(EditAnywhere, Category="GameInstancedAnim|Culling")
	float FrustumCullPadding = 100.0f;

	// Minimum sphere radius used for culling when mesh bounds are tiny/unknown (in cm).
	UPROPERTY(EditAnywhere, Category="GameInstancedAnim|Culling")
	float FrustumCullMinRadius = 30.0f;

	int32 RequestAnimClipIndex(const FGIAG_AnimNodeRef& NodeRef, const UAnimSequence* AnimSequence);

	/** Detect NumCustomDataFloats from SkeletalMesh materials (runtime-safe, no editor-only data). */
	static int32 DetectNumCustomDataFloatsFromMaterials(const USkeletalMesh& SkeletalMesh);

protected:
	void* FindAnimNodeImpl(const FGameInstancedAnimationGraphHandle& Handle, FName NodeName, FGIAG_AnimNodeRef& OutNodeRef) const;
	FORCEINLINE const UScriptStruct* GetAnimNodeStruct(const FGIAG_AnimNodeRef& AnimNodeRef) const { return Groups[AnimNodeRef.GroupIndex].NodeProperties[AnimNodeRef.NodeIndex]->Struct; }
private:
	struct FPrivateUtils;

	// ---- TimeSlot state ----
	float TimeSlots[GIAG_MAX_TIME_SLOTS] = {};
	TBitArray<> TimeSlotAllocated;

	UPROPERTY(Transient)
	TObjectPtr<AActor> HostActor;
	// Keep GC references to input assets (optional safety).
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMesh>> SkeletalMeshes;
	UPROPERTY(Transient)
	TArray<TObjectPtr<const UAnimSequence>> AnimSequences;

	/** Ensure we have a host actor to own scene components. */
	void EnsureHostActor();

	// Thread-safe bake payload shared between GT (task submission), worker (produce pixels), and RT (optional wait + upload).
	struct FClipBakePayload;
	using FClipBakePayloadPtr = TSharedPtr<FClipBakePayload>;

	struct FSkeletonAnimCache
	{
		USkeleton* Skeleton;
		int32 NumBones = 0;

		/** Stable clip slots (ClipIndex is stable across time; slots may be invalid/evicted). */
		struct FClipSlot
		{
			bool bAllocated = false;
			bool bEvicted = false;

			/** Latest shader-visible meta for this slot. Invalid => StartTransformIndex=-1, NumFrames=0. */
			FGIAG_ClipMeta Meta;

			/** Transform range (in FGIAG_BoneTRS elements) inside the AnimTRS buffer. */
			int32 StartTransformIndex = INDEX_NONE;
			int32 NumTransforms = 0;

			/** Last request time (seconds, world time). */
			double LastRequestedTimeSeconds = 0.0;

			/** Bake payload (may be null). */
			FClipBakePayloadPtr Bake;

			/** Dirty flags to build incremental uploads. */
			bool bMetaDirty = false;
			bool bTRSDirty = false;
		};

		TArray<FClipSlot> ClipSlots;              // index == ClipIndex
		TMap<const UAnimSequence*, int32> SequenceToClipIndex;
		/** ClipIndex -> AnimSequence (CPU backend uses this; GPU backend may leave it set too). */
		TArray<const UAnimSequence*> ClipIndexToSequence;
		TArray<int32> FreeClipIndices;

		/** AnimTRS allocator state (GT). */
		struct FTransformBlock { int32 Start = 0; int32 Size = 0; };
		TArray<FTransformBlock> FreeTransformBlocks;
		int32 AnimTRSCapacity = 0; // total transforms allocated in GPU buffer (StructuredBuffer<FGIAG_BoneTRS>)

		/** Persistent GPU buffers for anim library (owned by SceneExtension RT cache; GT maintains capacity metadata). */
		GIAG::FAnimLibraryBuffers AnimLibraryBuffers;

		/** Monotonic version for merged anim library (GT). */
		uint32 AnimLibraryVersion = 0;
		/** Latest upload blob (GT-built, consumed by RT). Kept alive via shared ptr. */
		FGIAG_AnimLibraryUploadDataPtr PendingAnimLibraryUpload;
		/** One-time warning gate for empty/unavailable anim library. */
		bool bWarnedAnimLibraryUnavailable = false;

		/** Cached skeleton static data (built from AssetUserData or derived at runtime). */
		TArray<int32> ParentIndices;
		TArray<FGIAG_BoneTRS> InverseRefPoseTRS;

		/** Cached skeleton RefPose in local space (bone order == Skeleton reference skeleton). */
		TArray<FTransform> RefPoseLocal;

		/**
		 * Cached skeleton RefPose in TRS layout (one FGIAG_BoneTRS per bone), size == NumBones.
		 * Stored as shared ptr so we can pass through GT->RT upload blobs without copying.
		 */
		TSharedPtr<TArray<FGIAG_BoneTRS>> RefPoseLocalTRS;

		/** Monotonic version for RefPose buffer uploads. */
		uint32 RefPoseVersion = 0;
		/** Gate for one-time (or rebuild) RefPose upload. */
		bool bRefPoseUploadSent = false;
	};

	/** Per-skeleton merged animation caches (heap-backed for address stability across render enqueues). */
	TArray<TUniquePtr<FSkeletonAnimCache>> SkeletonCaches;
	TMap<USkeleton*, int32> SkeletonCacheIndexBySkeleton;

	struct FGraphGroup
	{
		FGraphGroup() = default;
		FGraphGroup(const FGraphGroup&) = delete;
		FGraphGroup& operator=(const FGraphGroup&) = delete;
		FGraphGroup(FGraphGroup&&) = default;
		FGraphGroup& operator=(FGraphGroup&&) = default;

		UGIAG_AnimGraph* AnimGraph = nullptr;
		USkeleton* Skeleton;
		int32 SkeletonCacheIndex = INDEX_NONE;

		/** Cached compiled data for AnimGraph. */
		const FGIAG_AnimGraphCompiledData* Compiled = nullptr;

		/** Runtime node AoS layout: one array per node, indexed by SlotIndex. */
		/** StructProperty on DefaultGraphInstance for each node (used for Initialize/Copy/Destroy). */
		TArray<const FStructProperty*> NodeProperties; // size == Compiled->NumNodes
		TArray<uint32> NodeStrideBytes;                 // size == Compiled->NumNodes (stride aligned to 16)
		/** Points into AnimGraph's DefaultGraphInstance memory. */
		TMap<FName, int32> NodeIndexByMemberName;             // MemberName -> NodeIdx

		/** Buckets that reference this group. */
		TArray<int32> BucketIndices;

		// Cached stats
		int32 NumBones = 0;

		// ---- Optional shared resources (GT-built once per graph+skeleton; reused by all buckets in this group) ----
		int32 MaxOptionalSRVSlot = -1;
		TArray<TArray<FGIAG_AnimResourceKey>> OptionalSRVKeyByNodeBySlot; // [NodeIndex][Slot] -> ShareKey

		/** CPU backend runner (GT-only). */
		TUniquePtr<class FGIAG_AnimGraphCpuRunner> CpuRunner;
	};

	friend FGIAG_AnimNodeRef;

	struct FMeshBucket
	{
		FMeshBucket() = default;
		FMeshBucket(const FMeshBucket&) = delete;
		FMeshBucket& operator=(const FMeshBucket&) = delete;
		FMeshBucket(FMeshBucket&&) = default;
		FMeshBucket& operator=(FMeshBucket&&) = default;

		USkeletalMesh* SkeletalMesh = nullptr;
		int32 GroupIndex = INDEX_NONE;
		double BoundSphereRadius = 0.0;

		UInstancedSkinnedMeshComponent* ISKMC = nullptr;
		UGIAG_TransformProviderData* TransformProvider = nullptr;
		int32 NumInstances = 0;

		TRefCountPtr<FGIAG_TransformProviderState> SharedState;

		FGIAG_AnimGraphUploadBuilder UploadBuilder;
		bool bSentSkeletonStaticUpload = false;

		// ---- Computation data (bucket-level, indexed by SlotIndex) ----
		int32 TotalSlotCapacity = 0;
		bool bStorageInitialized = false;

		bool bGrewThisFrame = false;
		bool bPendingShrinkEvaluation = false;

		TArray<int32> RecordIndexBySlot;
		TBitArray<> SlotAlive;
		TArray<int32> FreeSlots;

		TArray<uint8> TimeSlotIndexBySlot;

		TArray<FTransform> TransformBySlot;
		TBitArray<> TransformDirty;
		TArray<uint32> DirtyTransformSlots;
		TArray<uint32> NewSlotsThisTick;

		TArray<TArray<uint8, TAlignedHeapAllocator<16>>> NodeStorageByNode;
		TArray<uint8*> NodeBasePtrsByNode;
		TArray<uint32> NodeStrideBytes;

		TArray<TBitArray<>> NodeParamDirtyBitsByNode;
		TArray<TArray<uint32>> DirtyNodeParamSlotsByNode;
		TBitArray<> DirtyNodeMask;
		TArray<int32> DirtyNodeIndices;

		// Per-instance custom data float count for this bucket's mesh (0 = no custom data).
		int32 NumCustomDataFloats = 0;

		TArray<uint32> GpuAliveSlots;
		TArray<uint32> CpuAliveSlots;
		TArray<int32> GpuAliveListIndexBySlot;
		TArray<int32> CpuAliveListIndexBySlot;
		TArray<uint32> GpuActiveInstanceIndices;

		int32 GetTotalSlotCapacity() const { return TotalSlotCapacity; }

		void InitBucketStorage(const FGIAG_AnimGraphCompiledData& CompiledData, TConstArrayView<uint32> InNodeStrideBytes);
		void GrowCapacity(int32 NewCapacity, const FGIAG_AnimGraphCompiledData& CompiledData, TConstArrayView<uint32> InNodeStrideBytes);
		int32 AllocateSlot();
		void FreeSlot(int32 SlotIndex);

		FORCEINLINE void MarkNodeParamDirty(int32 NodeIdx, int32 SlotIndex)
		{
			check(NodeIdx >= 0 && NodeIdx < NodeParamDirtyBitsByNode.Num());
			check(SlotIndex >= 0 && SlotIndex < GetTotalSlotCapacity());

			TBitArray<>& Bits = NodeParamDirtyBitsByNode[NodeIdx];
			if (!Bits[SlotIndex])
			{
				Bits[SlotIndex] = true;
				DirtyNodeParamSlotsByNode[NodeIdx].Add((uint32)SlotIndex);
			}

			if (!DirtyNodeMask[NodeIdx])
			{
				DirtyNodeMask[NodeIdx] = true;
				DirtyNodeIndices.Add(NodeIdx);
			}
		}

		FORCEINLINE uint8* GetNodePtr(int32 NodeIdx, int32 SlotIndex)
		{
			return NodeStorageByNode[NodeIdx].GetData() + (int64)NodeStrideBytes[NodeIdx] * (int64)SlotIndex;
		}

		FORCEINLINE const uint8* GetNodePtr(int32 NodeIdx, int32 SlotIndex) const
		{
			return NodeStorageByNode[NodeIdx].GetData() + (int64)NodeStrideBytes[NodeIdx] * (int64)SlotIndex;
		}
	};
	
	float GetWorldTimeSeconds() const;

	/** Find or create a GraphGroup for (AnimGraph, AnimLibrary). */
	int32 FindOrCreateGroup(UGIAG_AnimGraph* AnimGraph, USkeleton* Skeleton);
	int32 RequestClipBake(int32 GroupIndex, const UAnimSequence* AnimSequence, double NowSeconds);
	int32 RequestClipIndexOnly(int32 GroupIndex, const UAnimSequence* AnimSequence, double NowSeconds);
	int32 FindOrCreateSkeletonCache(USkeleton* Skeleton);
	FSkeletonAnimCache* GetSkeletonCache(int32 CacheIndex);
	const FSkeletonAnimCache* GetSkeletonCache(int32 CacheIndex) const { return const_cast<ThisClass*>(this)->GetSkeletonCache(CacheIndex); }

	/** Find or create the master-eval bucket for this mesh+rig. */
	int32 FindOrCreateBucket(USkeletalMesh* SkeletalMesh, int32 GroupIndex);

	/** Find or create a follower bucket for this mesh+rig, bound to MasterState. The TransformProvider
	 *  is configured as Follower BEFORE the SceneProxy is created, so the engine sees follower mode
	 *  on first proxy build (master→follower retargeting after SceneProxy creation does not work). */
	int32 FindOrCreateFollowerBucket(
		USkeletalMesh* SkeletalMesh,
		int32 GroupIndex,
		const TRefCountPtr<FGIAG_TransformProviderState>& MasterState,
		int32 DstNumBones,
		int32 SrcNumBones,
		const TSharedPtr<const TArray<uint32>>& RemapShared);

	void CleanupBucketIfEmpty(int32 BucketIndex);
	void CleanupGroupIfUnused(int32 GroupIndex);

	void GrowMasterBucketAndFollowers(int32 MasterBucketIndex, int32 NewCapacity);

	void CompactAndShrinkMaster(int32 MasterBucketIndex, int32 NewCapacity);

	/** Per graph-group data. */
	TSparseArray<FGraphGroup> Groups;

	// ---- Shared optional resources (GT; World-scoped) ----
	struct FSharedResourceEntry
	{
		FGIAG_AnimResourceRequest Request;
		TSharedPtr<TArray<uint8>> Bytes;
	};
	TMap<FGIAG_AnimResourceKey, FSharedResourceEntry> SharedResourcesByKey;

	TSharedPtr<FGIAG_SharedResourceBus> SharedResourceBus;
	TSharedPtr<FGIAG_AttachBus> AttachBus;
	TSharedPtr<FGIAG_AttachReadbackBus> AttachReadbackBus;
	TSharedPtr<FGIAG_NiagaraAttachRegistry, ESPMode::ThreadSafe> NiagaraAttachRegistry;
	TSharedPtr<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe> NativeAttachRegistry;

	// ---- Attach buckets (GT only; components owned by HostActor) ----
	enum class EAttachBucketType : uint8
	{
		Niagara,
		Native,
	};
	struct FAttachBucketLocator
	{
		EAttachBucketType Type = EAttachBucketType::Niagara;
		int32 Index = INDEX_NONE;
	};

	struct FNiagaraAttachBucketKey
	{
		UStaticMesh* StaticMesh = nullptr;
		UNiagaraSystem* NiagaraSystem = nullptr;
		bool operator==(const FNiagaraAttachBucketKey&) const = default;
		friend uint32 GetTypeHash(const FNiagaraAttachBucketKey& K)
		{
			return HashCombine(::GetTypeHash(K.StaticMesh), ::GetTypeHash(K.NiagaraSystem));
		}
	};
	struct FNativeAttachBucketKey
	{
		UStaticMesh* StaticMesh = nullptr;
		bool operator==(const FNativeAttachBucketKey&) const = default;
		friend uint32 GetTypeHash(const FNativeAttachBucketKey& K)
		{
			return ::GetTypeHash(K.StaticMesh);
		}
	};

	struct FAttachSlotTable
	{
		TArray<int32> OutputIndexByAttachSlot;   // size == SlotCapacity, -1 => free
		TArray<uint16> SlotGeneration;           // size == SlotCapacity, increments on reuse
		TArray<uint16> FreeSlots;
	};

	struct FAttachEntry
	{
		uint16 AttachSlot = 0;
		uint16 AttachGeneration = 0;
		/** Skeleton used to resolve BoneName->BoneIndex (GT only). */
		USkeleton* Skeleton = nullptr;
		FGIAG_TransformProviderState* State = nullptr;
		uint32 SlotIndex = 0;
		FName BoneName;
		uint32 BoneIndex = 0; // resolved from BoneName on GT (USkeleton ReferenceSkeleton index)
		/** Per-attachment flags consumed by RT attach desc + GPU attach compute. */
		uint32 DescFlags = 0;
		FGIAG_BoneTRS SocketLocalTRS;

		// ---- CPU mode options / runtime proxies (GT only) ----
		bool bCreateCpuProxyStaticMesh = false;
		UNiagaraSystem* CpuProxyNiagaraSystem = nullptr;
		UStaticMeshComponent* CpuStaticMeshComponent = nullptr;
		UNiagaraComponent* CpuNiagaraComponent = nullptr;
	};

	struct FNiagaraAttachBucket
	{
		uint32 BucketId = 0;
		UStaticMesh* StaticMesh = nullptr;
		UNiagaraSystem* NiagaraSystem = nullptr;
		UNiagaraComponent* NiagaraComponent = nullptr;

		FAttachSlotTable Slots;
		TArray<FAttachEntry> Entries; // dense, index == OutputIndex

		// ---- Niagara meta (GT-owned, published to RT via AttachBus; consumed by DI) ----
		// Slot identity:
		// - AttachSlotGeneration is for handle ABA safety.
		// - FxParticleGenBySlot is for Niagara GPU particle lifetime (kill by mismatch).
		uint32 SlotTableVersion = 0; // bumps on any slot table / FxGen change
		TArray<int32> SlotToDenseIndex;     // int32[SlotCap], -1 => invalid
		TArray<int32> SlotGenerationI32;    // int32[SlotCap]
		TArray<int32> FxParticleGenBySlot;  // int32[SlotCap]

		// Versioned add list (per-frame). Consumer gates by AddListVersion.
		uint32 AddListVersion = 0;          // bumps when PublishedAddsPacked is replaced with a non-empty list
		TArray<int32> PendingAddsPacked;    // per-frame build list (Packed=(FxGen<<16)|Slot)
		TArray<int32> PublishedAddsPacked;  // last published list for this version (kept for at least a frame)

		bool bNiagaraMetaDirty = false;     // slot tables or fxgen changed (event-driven)
		bool bNiagaraAddListDirty = false;  // pending add list changed (per-frame)
	};

	struct FNativeAttachBucket
	{
		uint32 BucketId = 0;
		UStaticMesh* StaticMesh = nullptr;
		UGIAG_AttachMeshComponent* NativeMeshComponent = nullptr;

		FAttachSlotTable Slots;
		TArray<FAttachEntry> Entries; // dense, index == OutputIndex
	};

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> DefaultStaticMeshAttachmentNiagaraSystem;
	TMap<FNiagaraAttachBucketKey, int32> NiagaraAttachBucketIndexByKey;
	TMap<FNativeAttachBucketKey, int32> NativeAttachBucketIndexByKey;
	TMap<uint32, FAttachBucketLocator> AttachBucketById;
	TArray<FNiagaraAttachBucket> NiagaraAttachBuckets;
	TArray<FNativeAttachBucket> NativeAttachBuckets;
	// GT-only: fast lookup for DI (NiagaraComponent* -> BucketId). Only populated for GIAG-created bucket components.
	TMap<const UNiagaraComponent*, uint32> NiagaraAttachBucketIdByComponent;
	uint32 NextAttachBucketId = 1;
	uint64 NiagaraAttachLastFlushFrame = 0;

	// ---- CPU attach sync working set (GT only) ----
	// Dense list of master record indices that are currently in CPU backend and have at least one attachment
	// that does NOT have a CPU proxy (i.e. requires per-frame GT sync writes).
	TArray<int32> CpuAttachSyncMasterRecordIndices;
	// RecordIndex -> index into CpuAttachSyncMasterRecordIndices, or INDEX_NONE if not registered.
	TArray<int32> CpuAttachSyncListIndexByRecordIndex;

	void EnsureCpuAttachSyncIndexCapacity(int32 RecordIndex);
	void RegisterCpuAttachSyncMasterRecord(int32 RecordIndex);
	void UnregisterCpuAttachSyncMasterRecord(int32 RecordIndex);
	void UpdateCpuAttachSyncRegistration_GameThread(int32 RecordIndex);

	void InitNiagaraBucketMeta(FNiagaraAttachBucket& Bucket);
	void PublishNiagaraBucketMetaToRT(FNiagaraAttachBucket& Bucket);
	int32 FindOrCreateNiagaraAttachBucket(UStaticMesh* StaticMesh, UNiagaraSystem* NiagaraSystem);
	int32 FindOrCreateNativeAttachBucket(UStaticMesh* StaticMesh);
	void FlushNiagaraAttachBuckets_GameThread();
	void NiagaraAttach_KillGpuParticleBySlot(uint32 BucketId, uint16 AttachSlot);
	void NiagaraAttach_SpawnGpuParticleBySlot(uint32 BucketId, uint16 AttachSlot);

	// ---- Internal Niagara Attach API (Nanite-friendly; no GPU->CPU readback) ----
	FGameInstancedAnimationAttachHandle AttachStaticMesh_Backend(const FGameInstancedAnimationGraphHandle& Handle, UStaticMesh* StaticMesh, UNiagaraSystem* NiagaraSystem, FName BoneName, const FTransform& SocketLocalTransform);
	bool UpdateAttach_Backend(const FGameInstancedAnimationAttachHandle& Handle, FName NewBoneName, const FTransform& NewSocketLocalTransform);
	bool RemoveAttach_Backend(const FGameInstancedAnimationAttachHandle& Handle);

	// ---- Internal attach helpers (CPU mode sync / proxy management) ----
	void EnqueueAttachWriteFxTransform(uint32 BucketId, uint32 OutputIndex, const FTransform3f& SocketWS);
	void EnqueueAttachHide(uint32 BucketId, uint32 OutputIndex);
	void EnqueueAttachWriteInstance(uint32 BucketId, uint32 OutputIndex, const FTransform3f& SocketWS);
	void EnqueueAttachHideInstance(uint32 BucketId, uint32 OutputIndex);
	void TickCpuAttachSync_GameThread();
	void CleanupAttachCpuProxies(const FGameInstancedAnimationAttachHandle& Handle);

public:
	TSharedPtr<FGIAG_SharedResourceBus> GetSharedResourceBus() const { return SharedResourceBus; }
	TSharedPtr<FGIAG_AttachBus> GetAttachBus() const { return AttachBus; }
	TSharedPtr<FGIAG_AttachReadbackBus> GetAttachReadbackBus() const { return AttachReadbackBus; }
	TSharedPtr<FGIAG_NiagaraAttachRegistry, ESPMode::ThreadSafe> GetNiagaraAttachRegistry() const { return NiagaraAttachRegistry; }
	TSharedPtr<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe> GetNativeAttachRegistry() const { return NativeAttachRegistry; }

	// ---- Niagara attach DI helpers (GT only; contract: only GIAG-created NiagaraComponents call into this) ----
	/** Resolve the GIAG attach bucket id for a NiagaraComponent created/owned by this subsystem. Returns 0 if not found. */
	uint32 ResolveNiagaraAttachBucketIdOrZero(const UNiagaraComponent* NiagaraComponent) const;
	/** AnyThread: read the latest GPU-visible add-list version+count (RT-consumed). Returns false if bucket not found. */
	bool NiagaraAttach_GetSpawnVersionAndCount(uint32 BucketId, uint32& OutAddListVersion, uint32& OutAddListCount) const;

	/** Debug: request RT readback for an FxTransform element. */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	void DebugRequestAttachFxTransformReadback(int32 BucketId, int32 OutputIndex);

	/** Debug: return latest completed FxTransform readback for a requested (BucketId, OutputIndex). */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugGetLatestAttachFxTransformReadback(int32 BucketId, int32 OutputIndex, int64& OutCpuRequestFrame, FTransform3f& OutSocketWS) const;

	/** Debug: return latest completed native instance buffers readback (Origin + 3 rows) for a requested (BucketId, OutputIndex). */
	UFUNCTION(BlueprintCallable, Category="GameInstancedAnim|Debug")
	bool DebugGetLatestAttachInstanceBuffersReadback(int32 BucketId, int32 OutputIndex, int64& OutCpuRequestFrame, FVector3f& OutOrigin, FVector3f& OutRow0, FVector3f& OutRow1, FVector3f& OutRow2) const;

	/** Per bucket (MeshAsset+Rig). */
	TSparseArray<FMeshBucket> Buckets;

	/** Map (MeshAsset, Rig, IsFollower) -> bucket index. */
	struct FBucketKey
	{
		USkeletalMesh* SkeletalMesh;
		int32 GroupIndex = INDEX_NONE;
		bool bFollower = false;
		bool operator==(const FBucketKey&) const = default;
		friend uint32 GetTypeHash(const FBucketKey& Key)
		{
			return HashCombine(HashCombine(::GetTypeHash(Key.SkeletalMesh), ::GetTypeHash(Key.GroupIndex)), ::GetTypeHash(Key.bFollower));
		}
	};
	TMap<FBucketKey, int32> BucketByKey;

	/** Map (AnimGraph, AnimLibrary) -> group index. */
	struct FGroupKey
	{
		UGIAG_AnimGraph* AnimGraph = nullptr;
		USkeleton* Skeleton;
		bool operator==(const FGroupKey&) const = default;
		friend uint32 GetTypeHash(const FGroupKey& Key) { return HashCombine(::GetTypeHash(Key.AnimGraph), ::GetTypeHash(Key.Skeleton)); }
	};
	TMap<FGroupKey, int32> GroupByKey;
	
	struct FInstancedAnimRecord
	{
		/** Primary mesh asset used by this instance (for CPU backend and backend switching). */
		USkeletalMesh* SkeletalMesh = nullptr;

		UInstancedSkinnedMeshComponent* ISKMC = nullptr;
		AActor* CpuProxyActor = nullptr;
		bool bExternalCpuProxyActor = false;
		/** CPU-only follow component (only valid when MasterRecordIndex!=INDEX_NONE). Owned by master's CpuProxyActor. */
		USkinnedMeshComponent* CpuFollowSkinnedMesh = nullptr;
		TSubclassOf<AActor> CpuProxyClass;
		FPrimitiveInstanceId InstanceId;
		int32 BucketIndex = INDEX_NONE;
		int32 GroupIndex = INDEX_NONE;
		/** Bucket-wide slot index. Directly indexes FMeshBucket arrays. */
		int32 SlotIndex = INDEX_NONE;

		uint8 TimeSlotIndex = 0;

		/** If set, this record is a Follow instance which shares Master's animation instance. */
		int32 MasterRecordIndex = INDEX_NONE;

		/** Only valid when MasterRecordIndex==INDEX_NONE (master): list of follow record indices. */
		TArray<int32> FollowRecordIndices;

		/** Attachments owned by this record (lifetime is tied to the instance). */
		TArray<FGameInstancedAnimationAttachHandle> AttachHandles;

		/** Per-instance material custom data (source of truth for all instances, synced to backend on write). */
		TArray<float> MaterialCustomData;
	};
	TSparseArray<FInstancedAnimRecord> AnimRecords;

	/** Serial numbers by record index (monotonically increases per record index across reuse). */
	TArray<int32> AnimRecordSerials;

	FInstancedAnimRecord* ResolveRecord(const FGameInstancedAnimationGraphHandle& Handle);
	const FInstancedAnimRecord* ResolveRecord(const FGameInstancedAnimationGraphHandle& Handle) const { return const_cast<ThisClass*>(this)->ResolveRecord(Handle); }

	// ---- Instance creation (shared core) ----
	FGameInstancedAnimationGraphHandle AddInstance_Internal(
		USkeletalMesh* SkeletalMesh,
		UGIAG_AnimGraph* AnimGraph,
		const FTransform& Transform,
		TSubclassOf<AActor> CpuProxyClass,
		bool bCpuMode,
		AActor* ExternalCpuProxyActor,
		FGIAG_TimeSlot TimeSlot);

	// ---- Backend switching helpers (GT only; master instances only) ----
	void SwitchMasterGpuToCpu(const FGameInstancedAnimationGraphHandle& Handle, FInstancedAnimRecord* Rec);
	void SwitchMasterCpuToGpu(const FGameInstancedAnimationGraphHandle& Handle, FInstancedAnimRecord* Rec, FGraphGroup& Group);

	static void InvalidateHandle(FGameInstancedAnimationGraphHandle& Handle);

	// ---- Debug readback (GT) ----
	struct FDebugLocalPoseCache
	{
		uint64 CpuRequestFrame = 0;
		int32 NumBones = 0;
		TArray<FGIAG_BoneTRS> LocalPoseTRS;
	};

	struct FDebugNeedNodeBitsCache
	{
		uint64 CpuRequestFrame = 0;
		uint32 SlotIndex = 0;
		uint32 NumNodes = 0;
		uint32 WordsPerSlot = 0;
		TArray<uint32> Words;
	};

	struct FDebugAttachFxTransformCache
	{
		uint64 CpuRequestFrame = 0;
		FGIAG_Transform FxTransform = FGIAG_Transform::Identity;
	};

	struct FDebugAttachInstanceBuffersCache
	{
		uint64 CpuRequestFrame = 0;
		FVector3f Origin = FVector3f::ZeroVector;
		FVector3f Row0 = FVector3f::ZeroVector;
		FVector3f Row1 = FVector3f::ZeroVector;
		FVector3f Row2 = FVector3f::ZeroVector;
	};

	/** RecordIndex -> SerialNumber gate for which instances are currently enabled for readback. */
	TMap<int32, int32> DebugReadbackEnabledSerialByRecordIndex;
	TMap<int32, int32> DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex;

	/** Latest completed readback per RecordIndex (latest-only). */
	TMap<int32, FDebugLocalPoseCache> DebugLatestLocalPoseByRecordIndex;
	TMap<int32, FDebugNeedNodeBitsCache> DebugLatestNeedNodeBitsByRecordIndex;

	/** Latest completed attach FxTransform readback per (BucketId, OutputIndex) (latest-only). */
	TMap<uint64, FDebugAttachFxTransformCache> DebugLatestAttachFxTransformByKey;

	/** Latest completed attach instance buffers readback per (BucketId, OutputIndex) (latest-only). */
	TMap<uint64, FDebugAttachInstanceBuffersCache> DebugLatestAttachInstanceBuffersByKey;

	/** Drain completed readbacks from RT and update DebugLatestLocalPoseByRecordIndex. */
	void PumpDebugReadbacks_GameThread();

	/** Fill per-skeleton static buffers (ParentIndices/InverseRefPoseTRS) from AssetUserData or runtime build. */
	bool GetOrBuildSkeletonStaticData(USkeleton* Skeleton, TArray<int32>& OutParentIndices, TArray<FGIAG_BoneTRS>& OutInverseRefPoseTRS, int32& OutNumBones) const;

	/** Get or build per-AnimSequence baked TRS (local pose per bone per frame). */
	bool GetOrBuildAnimSequencePixels(USkeleton* Skeleton, UAnimSequence* AnimSequence, float SecondsPerFrame, FGIAG_ClipMeta& OutMeta, TArray<FGIAG_BoneTRS>& OutTRS) const;

	/** Cross-mesh follow bone remap cache. Keyed by (DstMesh, SrcSkeleton). Values are heap-owned for stable pointer addresses. */
	struct FFollowBoneRemapKey
	{
		USkeletalMesh* DstMesh;
		USkeleton* SrcSkeleton;
		bool operator==(const FFollowBoneRemapKey&) const = default;
		friend uint32 GetTypeHash(const FFollowBoneRemapKey& K) { return HashCombine(::GetTypeHash(K.DstMesh), ::GetTypeHash(K.SrcSkeleton)); }
	};
	TMap<FFollowBoneRemapKey, TSharedPtr<const TArray<uint32>>> FollowBoneRemapCache;

private:
	// ---- CPU pose cache (written on GT in OnWorldPreActorTick; read on AnyThread by Anim nodes) ----
	struct FCpuPoseCacheEntry
	{
		uint64 Frame = 0;
		int32 SerialNumber = 0;
		int32 NumBones = 0;
		TArray<FTransform3f> LocalPose; // Skeleton bone index order
	};
	TMap<int32, FCpuPoseCacheEntry> CpuPoseCacheByRecordIndex;
	TAtomic<uint64> CpuPoseCacheBuiltFrame { 0 };
	mutable FThreadSafeCounter CpuNodeFallbackEvalCounter;

	FDelegateHandle PreActorTickHandle;
	void OnWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void PrecomputeCpuPoseCache_GameThread();
};
