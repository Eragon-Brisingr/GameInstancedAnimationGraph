// Minimal runtime subsystem that manages instanced meshes and GPU-driven animation state.

#include "GameInstancedAnimationGraphSubsystem.h"

#include "AnimationRuntime.h"
#include "GIAG_MaterialUtils.h"
#include "GameInstancedAnimationGraphSettings.h"
#include "GIAG_ActorInterface.h"
#include "GIAG_AnimGraphCpuRunner.h"
#include "GIAG_AnimGraphUploadBuilder.h"
#include "GIAG_AnimSequenceUserData.h"
#include "GIAG_BucketCapacityLadder.h"
#include "GIAG_EvalAnimUtils.h"
#include "GIAG_SkeletalMeshUserData.h"
#include "GIAG_TransformProviderBridge.h"
#include "GIAG_AttachRegistry.h"
#include "GIAG_TransformProviderData.h"
#include "InstanceDataTypes.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "TextureResource.h"
#include "Animation/AnimSequence.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/Skeleton.h"
#include "Async/Async.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "MaterialCachedData.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Rendering/SkeletalMeshRenderData.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

DECLARE_STATS_GROUP(TEXT("GameInstancedAnim"), STATGROUP_GameInstancedAnim, STATCAT_Advanced);

static TAutoConsoleVariable<bool> CVar_InstancedAnimEnableCull
{
	TEXT("GameInstancedAnim.EnableCull"),
	true,
	TEXT("Enable CPU frustum culling for GameInstancedAnim (partial graph evaluation + preserve outputs)."),
};

static TAutoConsoleVariable<int32> CVar_InstancedAnimDebugBuckets
{
	TEXT("GameInstancedAnim.DebugBuckets"),
	0,
	TEXT("Debug bucket creation and per-tick evaluation stats. 0=off, 1=log per tick aggregates."),
};

struct UGameInstancedAnimationGraphSubsystem::FPrivateUtils
{
	// Mark that we're evaluating nodes on CPU outside the normal tick-driven batch path.
	struct FGIAG_FallbackEvalGuard
	{
		explicit FGIAG_FallbackEvalGuard(const UGameInstancedAnimationGraphSubsystem* InSubsystem)
			: Subsystem(InSubsystem)
		{
			Subsystem->CpuNodeFallbackEvalCounter.Increment();
		}
		~FGIAG_FallbackEvalGuard()
		{
			Subsystem->CpuNodeFallbackEvalCounter.Decrement();
		}
		const UGameInstancedAnimationGraphSubsystem* Subsystem = nullptr;
	};
	
	FORCEINLINE static void AddSlotToList(TArray<uint32>& InOutList, TArray<int32>& InOutIndexBySlot, int32 SlotIndex)
	{
		check(InOutIndexBySlot.IsValidIndex(SlotIndex));
		check(InOutIndexBySlot[SlotIndex] == INDEX_NONE);
		InOutIndexBySlot[SlotIndex] = InOutList.Num();
		InOutList.Add((uint32)SlotIndex);
	}

	FORCEINLINE static void RemoveSlotFromList(TArray<uint32>& InOutList, TArray<int32>& InOutIndexBySlot, int32 SlotIndex)
	{
		check(InOutIndexBySlot.IsValidIndex(SlotIndex));
		const int32 RemoveAt = InOutIndexBySlot[SlotIndex];
		check(RemoveAt != INDEX_NONE);
		const uint32 SwapSlotU = InOutList.Last();
		InOutList[RemoveAt] = SwapSlotU;
		InOutList.Pop(EAllowShrinking::No);
		InOutIndexBySlot[SlotIndex] = INDEX_NONE;
		if ((int32)SwapSlotU != SlotIndex)
		{
			check(InOutIndexBySlot.IsValidIndex((int32)SwapSlotU));
			InOutIndexBySlot[(int32)SwapSlotU] = RemoveAt;
		}
	}

	static void UpdateISKMCInstanceTransformById(
		UInstancedSkinnedMeshComponent& Component,
		FPrimitiveInstanceId InstanceId,
		const FTransform& NewTransform,
		bool bWorldSpace)
	;

	// Create the bucket-owned ISKMC + transform provider. UE 5.8 has one ISKMC per bucket (no sharding).
	FORCEINLINE static void InitBucketRendering(FMeshBucket& InBucket, AActor& HostActor, USkeletalMesh& SkeletalMesh)
	{
		check(InBucket.ISKMC == nullptr);
		check(InBucket.TransformProvider == nullptr);

		UInstancedSkinnedMeshComponent* ISKMC = NewObject<UInstancedSkinnedMeshComponent>(&HostActor);
		check(ISKMC);

		float AnimationMinScreenSize = 0.0001f;
		int32 ConfiguredMaterialDataFloats = -1;
		if (auto UserData = SkeletalMesh.GetAssetUserData<UGIAG_SkeletalMeshUserData>())
		{
			AnimationMinScreenSize = UserData->AnimationMinScreenSize;
			ConfiguredMaterialDataFloats = UserData->NumMaterialDataFloats;
		}

		int32 NumMaterialDataFloats = 0;
		if (ConfiguredMaterialDataFloats >= 0)
		{
			NumMaterialDataFloats = FMath::Clamp(ConfiguredMaterialDataFloats, 0, (int32)FCustomPrimitiveData::NumCustomPrimitiveDataFloats);
		}
		else
		{
			NumMaterialDataFloats = GIAG::DetectNumMaterialDataFloatsFromMaterials(SkeletalMesh);
		}
		InBucket.NumMaterialDataFloats = NumMaterialDataFloats;
		if (NumMaterialDataFloats > 0)
		{
			InBucket.MaterialDataNameToIndex = GIAG::BuildMaterialDataIndexMapFromMaterials(SkeletalMesh);
		}

		ISKMC->SetAnimationMinScreenSize(AnimationMinScreenSize);
		ISKMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ISKMC->bNavigationRelevant = false;
		ISKMC->CreationMethod = EComponentCreationMethod::Instance;

		if (NumMaterialDataFloats > 0)
		{
			ISKMC->SetNumCustomDataFloats(NumMaterialDataFloats);
		}

		ISKMC->RegisterComponent();
		ISKMC->SetSkinnedAssetAndUpdate(&SkeletalMesh);

		UGIAG_TransformProviderData* Provider = NewObject<UGIAG_TransformProviderData>(ISKMC);
		check(Provider);

		InBucket.ISKMC = ISKMC;
		InBucket.TransformProvider = Provider;
	}

	FORCEINLINE static void InitBucketAsMaster(
		FMeshBucket& InBucket,
		const FGraphGroup& Group,
		AActor& HostActor,
		USkeletalMesh& SkeletalMesh,
		int32 SlotCapacity)
	{
		check(!InBucket.bStorageInitialized);
		check(SlotCapacity > 0);
		check(Group.Compiled);

		InitBucketRendering(InBucket, HostActor, SkeletalMesh);

		InBucket.TransformProvider->AnimationSlotCount = SlotCapacity;
		InBucket.TransformProvider->SetState(InBucket.SharedState.GetReference());
		InBucket.ISKMC->SetTransformProvider(InBucket.TransformProvider);

		InBucket.SharedState->SlotCapacity = SlotCapacity;
		InBucket.TotalSlotCapacity = SlotCapacity;
		InBucket.InitBucketStorage(*Group.Compiled, MakeArrayView(Group.NodeStrideBytes));
	}

	FORCEINLINE static void InitBucketAsFollower(
		FMeshBucket& InBucket,
		AActor& HostActor,
		USkeletalMesh& SkeletalMesh,
		int32 SlotCapacity,
		const TRefCountPtr<FGIAG_TransformProviderState>& MasterState,
		int32 DstNumBones,
		int32 SrcBones,
		const TSharedPtr<const TArray<uint32>>& RemapShared)
	{
		InitBucketRendering(InBucket, HostActor, SkeletalMesh);

		InBucket.TransformProvider->AnimationSlotCount = SlotCapacity;
		InBucket.TransformProvider->SetState(InBucket.SharedState.GetReference());
		InBucket.TransformProvider->ConfigureAsFollower(MasterState.GetReference(), DstNumBones, SrcBones, RemapShared, SkeletalMesh.GetFName());
		InBucket.ISKMC->SetTransformProvider(InBucket.TransformProvider);
	}

	static AActor* SpawnCpuProxyActor(const FGameInstancedAnimationGraphHandle& Handle, const FInstancedAnimRecord& Rec, UWorld* World, const FTransform& Transform)
	{
		check(!Rec.CpuProxyActor);
		check(Rec.CpuProxyClass);

		FActorSpawnParameters SpawnParams;
		SpawnParams.bDeferConstruction = true;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Proxy = World->SpawnActor<AActor>(Rec.CpuProxyClass, Transform, SpawnParams);
		check(Proxy);
		check(Proxy->GetClass()->ImplementsInterface(UGIAG_ActorInterface::StaticClass()));
		IGIAG_ActorInterface::Execute_SetInstancedAnimationGraphHandle(Proxy, Handle);
		Proxy->FinishSpawning(Transform);
		USkeletalMeshComponent* Skinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Proxy);
		check(Skinned);
		Skinned->SetSkeletalMesh(Rec.SkeletalMesh);
		return Proxy;
	}

	static USkinnedMeshComponent* CreateCpuFollowComponent(AActor* ProxyActor, USkeletalMeshComponent* LeaderSkinnedMesh, USkeletalMesh* FollowMesh)
	{
		check(ProxyActor);
		check(LeaderSkinnedMesh);
		check(FollowMesh);

		UPoseableMeshComponent* Comp = NewObject<UPoseableMeshComponent>(ProxyActor);
		check(Comp);
		Comp->CreationMethod = EComponentCreationMethod::Instance;
		Comp->SetSkinnedAssetAndUpdate(FollowMesh);
		Comp->SetupAttachment(LeaderSkinnedMesh);
		Comp->SetLeaderPoseComponent(LeaderSkinnedMesh, true, false);
		Comp->RegisterComponent();
		return Comp;
	}

		static void DestroyCpuFollowComponent(USkinnedMeshComponent*& InOutComp)
	{
		check(InOutComp);

		InOutComp->DestroyComponent();
		InOutComp = nullptr;
	}
};

void UGameInstancedAnimationGraphSubsystem::FMeshBucket::InitBucketStorage(
	const FGIAG_AnimGraphCompiledData& CompiledData, TConstArrayView<uint32> InNodeStrideBytes)
{
	check(!bStorageInitialized);
	bStorageInitialized = true;

	const int32 TotalCap = GetTotalSlotCapacity();
	check(TotalCap > 0);

	RecordIndexBySlot.SetNum(TotalCap);
	for (int32 i = 0; i < TotalCap; ++i)
	{
		RecordIndexBySlot[i] = INDEX_NONE;
	}
	SlotAlive.SetNum(TotalCap, false);

	FreeSlots.Reserve(TotalCap);
	for (int32 i = TotalCap - 1; i >= 0; --i)
	{
		FreeSlots.Add(i);
	}

	TimeSlotIndexBySlot.SetNumZeroed(TotalCap);

	TransformBySlot.SetNum(TotalCap);
	for (int32 i = 0; i < TotalCap; ++i)
	{
		TransformBySlot[i] = FTransform::Identity;
	}
	TransformDirty.SetNum(TotalCap, false);
	DirtyTransformSlots.Reset();
	NewSlotsThisTick.Reset();

	const int32 NumNodes = CompiledData.NumNodes;
	checkf(NumNodes > 0, TEXT("GIAG: invalid CompiledData.NumNodes."));
	checkf(InNodeStrideBytes.Num() == NumNodes, TEXT("GIAG: NodeStrideBytes mismatch."));

	NodeStorageByNode.SetNum(NumNodes);
	NodeBasePtrsByNode.SetNum(NumNodes);
	NodeStrideBytes.SetNum(NumNodes);
	for (int32 NodeIdx = 0; NodeIdx < NumNodes; ++NodeIdx)
	{
		const uint32 Stride = InNodeStrideBytes[NodeIdx];
		check(Stride > 0 && (Stride % 16u) == 0u);
		NodeStrideBytes[NodeIdx] = Stride;
		NodeStorageByNode[NodeIdx].SetNumZeroed((int64)TotalCap * (int64)Stride);
		NodeBasePtrsByNode[NodeIdx] = NodeStorageByNode[NodeIdx].GetData();
	}

	NodeParamDirtyBitsByNode.SetNum(NumNodes);
	DirtyNodeParamSlotsByNode.SetNum(NumNodes);
	for (int32 NodeIdx = 0; NodeIdx < NumNodes; ++NodeIdx)
	{
		NodeParamDirtyBitsByNode[NodeIdx].SetNum(TotalCap, false);
		DirtyNodeParamSlotsByNode[NodeIdx].Reset();
	}
	DirtyNodeMask.SetNum(NumNodes, false);
	DirtyNodeIndices.Reset();

	if (NumMaterialDataFloats > 0)
	{
		MaterialDataBySlot.SetNumZeroed((int64)TotalCap * (int64)NumMaterialDataFloats);
	}

	GpuAliveSlots.Reset();
	CpuAliveSlots.Reset();
	GpuAliveListIndexBySlot.SetNum(TotalCap);
	CpuAliveListIndexBySlot.SetNum(TotalCap);
	for (int32 i = 0; i < TotalCap; ++i)
	{
		GpuAliveListIndexBySlot[i] = INDEX_NONE;
		CpuAliveListIndexBySlot[i] = INDEX_NONE;
	}
	GpuActiveInstanceIndices.Reset();

	UploadBuilder.Initialize(CompiledData);
}

void UGameInstancedAnimationGraphSubsystem::FMeshBucket::GrowCapacity(
	int32 NewCapacity, const FGIAG_AnimGraphCompiledData& CompiledData, TConstArrayView<uint32> InNodeStrideBytes)
{
	check(bStorageInitialized);
	const int32 OldCap = RecordIndexBySlot.Num();
	checkf(NewCapacity > OldCap, TEXT("GIAG: GrowCapacity NewCapacity=%d must exceed OldCap=%d."), NewCapacity, OldCap);

	RecordIndexBySlot.SetNum(NewCapacity);
	for (int32 i = OldCap; i < NewCapacity; ++i)
	{
		RecordIndexBySlot[i] = INDEX_NONE;
	}
	SlotAlive.SetNum(NewCapacity, false);

	// Push new free slots from high to low so AllocateSlot pops the lowest-index free slot first.
	FreeSlots.Reserve(FreeSlots.Num() + (NewCapacity - OldCap));
	for (int32 i = NewCapacity - 1; i >= OldCap; --i)
	{
		FreeSlots.Add(i);
	}

	TimeSlotIndexBySlot.SetNum(NewCapacity);
	for (int32 i = OldCap; i < NewCapacity; ++i)
	{
		TimeSlotIndexBySlot[i] = 0;
	}

	TransformBySlot.SetNum(NewCapacity);
	for (int32 i = OldCap; i < NewCapacity; ++i)
	{
		TransformBySlot[i] = FTransform::Identity;
	}
	TransformDirty.SetNum(NewCapacity, false);

	const int32 NumNodes = CompiledData.NumNodes;
	checkf(InNodeStrideBytes.Num() == NumNodes, TEXT("GIAG: NodeStrideBytes mismatch (Grow)."));
	for (int32 NodeIdx = 0; NodeIdx < NumNodes; ++NodeIdx)
	{
		const uint32 Stride = InNodeStrideBytes[NodeIdx];
		const int64 NewBytes = (int64)NewCapacity * (int64)Stride;
		NodeStorageByNode[NodeIdx].SetNumZeroed(NewBytes);
		NodeBasePtrsByNode[NodeIdx] = NodeStorageByNode[NodeIdx].GetData();

		NodeParamDirtyBitsByNode[NodeIdx].SetNum(NewCapacity, false);
	}

	if (NumMaterialDataFloats > 0)
	{
		MaterialDataBySlot.SetNumZeroed((int64)NewCapacity * (int64)NumMaterialDataFloats);
	}

	GpuAliveListIndexBySlot.SetNum(NewCapacity);
	CpuAliveListIndexBySlot.SetNum(NewCapacity);
	for (int32 i = OldCap; i < NewCapacity; ++i)
	{
		GpuAliveListIndexBySlot[i] = INDEX_NONE;
		CpuAliveListIndexBySlot[i] = INDEX_NONE;
	}

	TotalSlotCapacity = NewCapacity;
}

int32 UGameInstancedAnimationGraphSubsystem::FMeshBucket::AllocateSlot()
{
	if (FreeSlots.Num() > 0)
	{
		return FreeSlots.Pop(EAllowShrinking::No);
	}
	return INDEX_NONE;
}

void UGameInstancedAnimationGraphSubsystem::FMeshBucket::FreeSlot(int32 SlotIndex)
{
	check(SlotIndex >= 0 && SlotIndex < GetTotalSlotCapacity());
	FreeSlots.Add(SlotIndex);
}

void UGameInstancedAnimationGraphSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Shared optional resource bus (GT publishes, RT reads).
	SharedResourceBus = MakeShared<FGIAG_SharedResourceBus>();
	AttachBus = MakeShared<FGIAG_AttachBus>();
	AttachReadbackBus = MakeShared<FGIAG_AttachReadbackBus>();
	NiagaraAttachRegistry = MakeShared<FGIAG_NiagaraAttachRegistry, ESPMode::ThreadSafe>();
	NativeAttachRegistry = MakeShared<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe>();

	// TimeSlot 0 = WorldTime (always allocated).
	TimeSlotAllocated.Init(false, GIAG_MAX_TIME_SLOTS);
	TimeSlotAllocated[0] = true;

	// Precompute CPU pose cache before Actor/Component ticks (Anim BP evaluation happens later).
	PreActorTickHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &ThisClass::OnWorldPreActorTick);

	DefaultStaticMeshAttachmentNiagaraSystem = GetDefault<UGameInstancedAnimationGraphSettings>()->GlobalStaticMeshAttachNiagaraSystem.LoadSynchronous();
}

void UGameInstancedAnimationGraphSubsystem::Deinitialize()
{
	if (PreActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);
		PreActorTickHandle.Reset();
	}

	SharedResourcesByKey.Reset();
	SharedResourceBus.Reset();
	AttachBus.Reset();
	AttachReadbackBus.Reset();
	NiagaraAttachRegistry.Reset();
	NativeAttachRegistry.Reset();

	Super::Deinitialize();
}

void UGameInstancedAnimationGraphSubsystem::OnWorldPreActorTick(UWorld* World, ELevelTick /*TickType*/, float /*DeltaSeconds*/)
{
	if (!World || World != GetWorld())
	{
		return;
	}
	
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UGameInstancedAnimationGraphSubsystem::WorldPreActorTick"), STAT_UGameInstancedAnimSubsystem_WorldPreActorTick, STATGROUP_GameInstancedAnim);
	
	TimeSlots[0] = GetWorld()->GetTimeSeconds();

	// Flush deferred Niagara attach meta to RT early in the frame so Niagara ticks can observe updated versions.
	FlushNiagaraAttachBuckets_GameThread();
	PrecomputeCpuPoseCache_GameThread();
}

void UGameInstancedAnimationGraphSubsystem::PrecomputeCpuPoseCache_GameThread()
{
	check(IsInGameThread());

	CpuPoseCacheByRecordIndex.Reset();

	// CPU cache is defined as "built for this frame" once we finish filling entries.
	// (Anim node reads it later in the same frame.)
	const uint64 Frame = GFrameCounter;

	for (auto GroupIt = Groups.CreateIterator(); GroupIt; ++GroupIt)
	{
		const int32 GroupIndex = GroupIt.GetIndex();
		FGraphGroup& Group = *GroupIt;
		FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
		check(Cache->ParentIndices.Num() == Group.NumBones);

		for (const int32 BucketIndex : Group.BucketIndices)
		{
			FMeshBucket& Bucket = Buckets[BucketIndex];
			check(Bucket.GroupIndex == GroupIndex);

			if (!Bucket.bStorageInitialized || Bucket.CpuAliveSlots.Num() <= 0)
			{
				continue;
			}

			// Sync per-slot transforms from CpuProxyActors.
			for (const uint32 SlotU : Bucket.CpuAliveSlots)
			{
				const int32 SlotIndex = (int32)SlotU;
				const int32 RecordIndex = Bucket.RecordIndexBySlot[SlotIndex];
				check(RecordIndex != INDEX_NONE && AnimRecords.IsValidIndex(RecordIndex));
				const FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
				check(Rec.MasterRecordIndex == INDEX_NONE);
				check(Rec.CpuProxyActor);

				if (USkeletalMeshComponent* Skinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec.CpuProxyActor))
				{
					Bucket.TransformBySlot[SlotIndex] = Skinned->GetComponentTransform();
				}
				else
				{
					Bucket.TransformBySlot[SlotIndex] = Rec.CpuProxyActor->GetActorTransform();
				}
			}

			if (!Group.CpuRunner)
			{
				Group.CpuRunner = MakeUnique<FGIAG_AnimGraphCpuRunner>();
			}

			FGIAG_AnimGraphCpuRunParams CpuParams;
			CpuParams.NumInstances = Bucket.CpuAliveSlots.Num();
			CpuParams.SlotCapacity = Bucket.GetTotalSlotCapacity();
			CpuParams.NumBones = Group.NumBones;
			CpuParams.TimeSlots = MakeArrayView(TimeSlots);
			CpuParams.TimeSlotIndexBySlot = Bucket.TimeSlotIndexBySlot;
			check(Bucket.SkeletalMesh);
			CpuParams.SkeletalMesh = Bucket.SkeletalMesh;
			CpuParams.Skeleton = Group.Skeleton;
			CpuParams.ParentIndices = Cache->ParentIndices;
			CpuParams.RefPoseLocal = Cache->RefPoseLocal;
			CpuParams.ActiveInstanceIndices = Bucket.CpuAliveSlots;
			CpuParams.ComponentToWorldBySlot = Bucket.TransformBySlot;
			CpuParams.AnimSequencesByClipIndex = Cache->ClipIndexToSequence;
			CpuParams.NodeData = MakeArrayView(
				reinterpret_cast<const uint8* const*>(Bucket.NodeBasePtrsByNode.GetData()),
				Bucket.NodeBasePtrsByNode.Num());
			CpuParams.NodeStrideBytes = Bucket.NodeStrideBytes;
			CpuParams.RequestedFinalPoseType = EGIAG_AnimPinType::LocalPose;

			const FGIAG_AnimGraphCpuRunner::FOutputs Outputs = Group.CpuRunner->Evaluate(*Group.Compiled, CpuParams);
			check(Outputs.FinalLocalPose.IsValid());

			for (const uint32 SlotU : Bucket.CpuAliveSlots)
			{
				const int32 SlotIndex = (int32)SlotU;
				const int32 RecordIndex = Bucket.RecordIndexBySlot[SlotIndex];
				check(RecordIndex != INDEX_NONE && AnimRecords.IsValidIndex(RecordIndex));

				FCpuPoseCacheEntry& Entry = CpuPoseCacheByRecordIndex.FindOrAdd(RecordIndex);
				Entry.Frame = Frame;
				Entry.SerialNumber = AnimRecordSerials[RecordIndex];
				Entry.NumBones = Group.NumBones;
				Entry.LocalPose.SetNum(Group.NumBones, EAllowShrinking::No);

				for (int32 BoneIndex = 0; BoneIndex < Group.NumBones; ++BoneIndex)
				{
					Entry.LocalPose[BoneIndex] = Outputs.FinalLocalPose.At(SlotIndex, BoneIndex);
				}
			}
		}
	}

	CpuPoseCacheBuiltFrame.Store(Frame);
}

bool UGameInstancedAnimationGraphSubsystem::TryGetCpuPoseCache_NoLock(const FGameInstancedAnimationGraphHandle& Handle, uint64& OutFrame, TConstArrayView<FTransform3f>& OutLocalPose) const
{
	const FCpuPoseCacheEntry* Entry = CpuPoseCacheByRecordIndex.Find(Handle.RecordIndex);
	if (!Entry || Entry->SerialNumber != Handle.SerialNumber)
	{
		return false;
	}
	OutFrame = Entry->Frame;
	OutLocalPose = Entry->LocalPose;
	return true;
}

bool UGameInstancedAnimationGraphSubsystem::EvalCpuAnimationPoseAnyThread(const FGameInstancedAnimationGraphHandle& Handle, TArray<FTransform3f>& OutLocalPose) const
{
	FPrivateUtils::FGIAG_FallbackEvalGuard Guard(this);

	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return false;
	}
	check(Rec->MasterRecordIndex == INDEX_NONE);
	check(Rec->SkeletalMesh);

	checkf(Groups.IsValidIndex(Rec->GroupIndex), TEXT("GIAG CPU: invalid GroupIndex=%d."), Rec->GroupIndex);
	const FGraphGroup& Group = Groups[Rec->GroupIndex];
	check(Group.Compiled && Group.Skeleton && Group.NumBones > 0);

	const FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	check(Cache && Cache->Skeleton == Group.Skeleton);

	checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG CPU: invalid BucketIndex=%d."), Rec->BucketIndex);
	const FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	check(Bucket.bStorageInitialized);
	const int32 BucketSlot = Rec->SlotIndex;
	check(BucketSlot >= 0 && BucketSlot < Bucket.GetTotalSlotCapacity());

	// Base params (slot-capacity evaluation view into the bucket).
	FGIAG_AnimGraphCpuRunParams Params;
	Params.NumBones = Group.NumBones;
	Params.SlotCapacity = Bucket.GetTotalSlotCapacity();
	Params.TimeSlots = MakeArrayView(TimeSlots);
	Params.TimeSlotIndexBySlot = Bucket.TimeSlotIndexBySlot;
	Params.SkeletalMesh = Rec->SkeletalMesh;
	Params.Skeleton = Group.Skeleton;
	Params.ParentIndices = Cache->ParentIndices;
	Params.RefPoseLocal = Cache->RefPoseLocal;
	Params.ComponentToWorldBySlot = Bucket.TransformBySlot;
	Params.AnimSequencesByClipIndex = Cache->ClipIndexToSequence;
	Params.NodeData = MakeArrayView(
		reinterpret_cast<const uint8* const*>(Bucket.NodeBasePtrsByNode.GetData()),
		Bucket.NodeBasePtrsByNode.Num());
	Params.NodeStrideBytes = Bucket.NodeStrideBytes;

	// Single-slot pack: evaluate only this instance with SlotCapacity=1 to minimize framework overhead.
	const uint32 ActiveSlotU = 0u;
	const uint8 SingleTimeSlotIndex = Bucket.TimeSlotIndexBySlot[BucketSlot];

	FGIAG_AnimGraphCpuRunParams PackedParams = Params;
	PackedParams.NumInstances = 1;
	PackedParams.SlotCapacity = 1;
	PackedParams.ActiveInstanceIndices = TConstArrayView<uint32>(&ActiveSlotU, 1);
	PackedParams.TimeSlotIndexBySlot = TConstArrayView<uint8>(&SingleTimeSlotIndex, 1);

	const FTransform SingleC2W = Params.ComponentToWorldBySlot[BucketSlot];
	PackedParams.ComponentToWorldBySlot = TConstArrayView<FTransform>(&SingleC2W, 1);
	PackedParams.RequestedFinalPoseType = EGIAG_AnimPinType::LocalPose;

	TArray<const uint8*, TInlineAllocator<64>> PackedNodeData;
	PackedNodeData.SetNumUninitialized(Params.NodeData.Num());
	for (int32 NodeIdx = 0; NodeIdx < Params.NodeData.Num(); ++NodeIdx)
	{
		const uint32 Stride = Params.NodeStrideBytes[NodeIdx];
		PackedNodeData[NodeIdx] = Params.NodeData[NodeIdx] + (int64)Stride * (int64)BucketSlot;
	}
	PackedParams.NodeData = PackedNodeData;

	FGIAG_AnimGraphCpuRunner Runner;
	const FGIAG_AnimGraphCpuRunner::FOutputs Outputs = Runner.Evaluate(*Group.Compiled, PackedParams);
	if (!Outputs.FinalLocalPose.IsValid())
	{
		return false;
	}

	OutLocalPose.SetNum(PackedParams.NumBones, EAllowShrinking::No);
	for (int32 BoneIndex = 0; BoneIndex < PackedParams.NumBones; ++BoneIndex)
	{
		OutLocalPose[BoneIndex] = Outputs.FinalLocalPose.At(0, BoneIndex);
	}
	return true;
}

void UGameInstancedAnimationGraphSubsystem::ConvertLocalPoseToComponentPoseChecked(const FGameInstancedAnimationGraphHandle& Handle, TConstArrayView<FTransform3f> LocalPose, TArray<FTransform3f>& OutComponentPose) const
{
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	check(Rec);

	checkf(Groups.IsValidIndex(Rec->GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), Rec->GroupIndex);
	const FGraphGroup& Group = Groups[Rec->GroupIndex];
	check(Group.SkeletonCacheIndex != INDEX_NONE);

	const FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	check(Cache);

	const auto& ParentIndices = Cache->ParentIndices;
	check(ParentIndices.Num() == LocalPose.Num());
	const int32 NumBones = LocalPose.Num();

	OutComponentPose.SetNumUninitialized(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const int32 Parent = ParentIndices[BoneIndex];
		const FTransform3f LocalT = LocalPose[BoneIndex];
		if (Parent >= 0)
		{
			check(Parent < BoneIndex);
			OutComponentPose[BoneIndex] = LocalT * OutComponentPose[Parent];
		}
		else
		{
			OutComponentPose[BoneIndex] = LocalT;
		}
	}
}

const TArray<int32>& UGameInstancedAnimationGraphSubsystem::GetSkeletonParentIndicesChecked(const FGameInstancedAnimationGraphHandle& Handle) const
{
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	check(Rec);

	checkf(Groups.IsValidIndex(Rec->GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), Rec->GroupIndex);
	const FGraphGroup& Group = Groups[Rec->GroupIndex];
	check(Group.SkeletonCacheIndex != INDEX_NONE);

	const FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	check(Cache);
	check(Cache->ParentIndices.Num() == Cache->NumBones);
	return Cache->ParentIndices;
}

int32 UGameInstancedAnimationGraphSubsystem::RequestAnimClipIndex(const FGIAG_AnimNodeRef& NodeRef, const UAnimSequence* AnimSequence)
{
	check(IsInGameThread());
	checkf(CpuNodeFallbackEvalCounter.GetValue() == 0, TEXT("GIAG: RequestAnimClipIndex must not run during AnimNode fallback evaluation."));

	auto& Rec = AnimRecords[NodeRef.RecordIndex];
	FGraphGroup& Group = Groups[NodeRef.GroupIndex];
	check(Group.Compiled);
	FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	check(Cache && Cache->Skeleton == Group.Skeleton);
	const double NowSeconds = NodeRef.System->GetWorldTimeSeconds();
	const int32 ClipIndex = Rec.CpuProxyActor ? RequestClipIndexOnly(Rec.GroupIndex, AnimSequence, NowSeconds) : RequestClipBake(Rec.GroupIndex, AnimSequence, NowSeconds);
	return ClipIndex;
}

void* UGameInstancedAnimationGraphSubsystem::FindAnimNodeImpl(const FGameInstancedAnimationGraphHandle& Handle, FName NodeName, FGIAG_AnimNodeRef& OutNodeRef) const
{
	check(IsInGameThread());
	
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (Rec == nullptr || Rec->MasterRecordIndex != INDEX_NONE)
	{
		return nullptr;
	}
	const FGraphGroup& Group = Groups[Rec->GroupIndex];
	const int32* FoundNodeIdx = Group.NodeIndexByMemberName.Find(NodeName);
	if (FoundNodeIdx == nullptr)
	{
		return nullptr;
	}
	const int32 NodeIdx = *FoundNodeIdx;
	OutNodeRef.System = const_cast<ThisClass*>(this);
	OutNodeRef.RecordIndex = Handle.RecordIndex;
	OutNodeRef.GroupIndex = Rec->GroupIndex;
	OutNodeRef.BucketIndex = Rec->BucketIndex;
	OutNodeRef.SlotIndex = Rec->SlotIndex;
	OutNodeRef.NodeIndex = NodeIdx;

	FMeshBucket& Bucket = OutNodeRef.System->Buckets[Rec->BucketIndex];
	return Bucket.GetNodePtr(NodeIdx, Rec->SlotIndex);
}

void UGameInstancedAnimationGraphSubsystem::FPrivateUtils::UpdateISKMCInstanceTransformById(
	UInstancedSkinnedMeshComponent& Component,
	FPrimitiveInstanceId InstanceId,
	const FTransform& NewTransform,
	bool bWorldSpace)
{
	// GPU-only instances are not backed by CPU InstanceData; do not attempt CPU-side updates.
	check(!Component.UsesGPUOnlyInstances());

	const bool bOk = Component.SetInstanceTransform(InstanceId, NewTransform, bWorldSpace);
	check(bOk);
}

void UGameInstancedAnimationGraphSubsystem::EnsureHostActor()
{
	if (HostActor)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	constexpr auto ActorName = TEXT("GameInstancedAnimHost");
	Params.Name = ActorName;
	Params.bAllowDuringConstructionScript = true;
#if WITH_EDITOR
	Params.InitialActorLabel = ActorName;
#endif

	AActor* A = World->SpawnActor<AActor>(Params);
	if (!A)
	{
		return;
	}
	A->SetCanBeDamaged(false);

	HostActor = A;
}

void UGameInstancedAnimationGraphSubsystem::SetMaterialDataRange(const FGameInstancedAnimationGraphHandle& Handle, int32 StartIndex, TConstArrayView<float> Values)
{
	check(IsInGameThread());
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	check(Rec);
	check(Rec->BucketIndex != INDEX_NONE);
	check(Buckets.IsValidIndex(Rec->BucketIndex));

	FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	const int32 N = Bucket.NumMaterialDataFloats;
	check(N > 0);
	check(StartIndex >= 0 && StartIndex + Values.Num() <= N);
	check(Rec->SlotIndex >= 0 && Rec->SlotIndex < Bucket.GetTotalSlotCapacity());

	float* SlotBase = Bucket.MaterialDataBySlot.GetData() + (int64)Rec->SlotIndex * (int64)N;
	FMemory::Memcpy(SlotBase + StartIndex, Values.GetData(), Values.Num() * sizeof(float));

	const int32 Count = Values.Num();
	if (Rec->ISKMC)
	{
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			Rec->ISKMC->SetCustomDataValue(Rec->InstanceId, StartIndex + Idx, Values[Idx]);
		}
	}
	else if (Rec->CpuProxyActor)
	{
		USkeletalMeshComponent* Skinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
		check(Skinned);
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			Skinned->SetCustomPrimitiveDataFloat(StartIndex + Idx, Values[Idx]);
		}
	}
	else if (Rec->CpuFollowSkinnedMesh)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			Rec->CpuFollowSkinnedMesh->SetCustomPrimitiveDataFloat(StartIndex + i, Values[i]);
		}
	}
}

const float* UGameInstancedAnimationGraphSubsystem::GetMaterialDataPtr(const FGameInstancedAnimationGraphHandle& Handle, int32& OutNum) const
{
	OutNum = 0;
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec || Rec->BucketIndex == INDEX_NONE)
	{
		return nullptr;
	}
	check(Buckets.IsValidIndex(Rec->BucketIndex));
	const FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	if (Bucket.NumMaterialDataFloats == 0)
	{
		return nullptr;
	}
	check(Rec->SlotIndex >= 0 && Rec->SlotIndex < Bucket.GetTotalSlotCapacity());
	OutNum = Bucket.NumMaterialDataFloats;
	return Bucket.MaterialDataBySlot.GetData() + (int64)Rec->SlotIndex * (int64)Bucket.NumMaterialDataFloats;
}

int32 UGameInstancedAnimationGraphSubsystem::ResolveMaterialDataIndex(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName) const
{
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (Rec == nullptr)
	{
		return INDEX_NONE;
	}
	check(Rec->BucketIndex != INDEX_NONE);
	check(Buckets.IsValidIndex(Rec->BucketIndex));

	const FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	const int32* Found = Bucket.MaterialDataNameToIndex.Find(ParameterName);
	if (!Found)
	{
		return INDEX_NONE;
	}
	return *Found;
}

void UGameInstancedAnimationGraphSubsystem::SetMaterialDataFloat(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName, float Value)
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE)
	{
		return;
	}
	SetMaterialDataRange(Handle, Idx, MakeArrayView(&Value, 1));
}

void UGameInstancedAnimationGraphSubsystem::SetMaterialDataVector2(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName, FVector2f Value)
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE)
	{
		return;
	}
	SetMaterialDataRange(Handle, Idx, MakeArrayView(&Value.X, 2));
}

void UGameInstancedAnimationGraphSubsystem::SetMaterialDataVector3(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName, FVector3f Value)
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE)
	{
		return;
	}
	SetMaterialDataRange(Handle, Idx, MakeArrayView(&Value.X, 3));
}

void UGameInstancedAnimationGraphSubsystem::SetMaterialDataColor(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName, FLinearColor Value)
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE)
	{
		return;
	}
	SetMaterialDataRange(Handle, Idx, MakeArrayView(&Value.R, 4));
}

float UGameInstancedAnimationGraphSubsystem::GetMaterialDataFloat(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName) const
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE) { return 0.f; }
	int32 N = 0;
	const float* P = GetMaterialDataPtr(Handle, N);
	check(P && Idx >= 0 && Idx < N);
	return P[Idx];
}

FVector2f UGameInstancedAnimationGraphSubsystem::GetMaterialDataVector2(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName) const
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE) { return FVector2f::ZeroVector; }
	int32 N = 0;
	const float* P = GetMaterialDataPtr(Handle, N);
	check(P && Idx >= 0 && Idx + 2 <= N);
	return FVector2f(P[Idx], P[Idx + 1]);
}

FVector3f UGameInstancedAnimationGraphSubsystem::GetMaterialDataVector3(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName) const
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE) { return FVector3f::ZeroVector; }
	int32 N = 0;
	const float* P = GetMaterialDataPtr(Handle, N);
	check(P && Idx >= 0 && Idx + 3 <= N);
	return FVector3f(P[Idx], P[Idx + 1], P[Idx + 2]);
}

FLinearColor UGameInstancedAnimationGraphSubsystem::GetMaterialDataColor(const FGameInstancedAnimationGraphHandle& Handle, FName ParameterName) const
{
	const int32 Idx = ResolveMaterialDataIndex(Handle, ParameterName);
	if (Idx == INDEX_NONE) { return FLinearColor(0, 0, 0, 0); }
	int32 N = 0;
	const float* P = GetMaterialDataPtr(Handle, N);
	check(P && Idx >= 0 && Idx + 4 <= N);
	return FLinearColor(P[Idx], P[Idx + 1], P[Idx + 2], P[Idx + 3]);
}

bool UGameInstancedAnimationGraphSubsystem::GetOrBuildSkeletonStaticData(
	USkeleton* Skeleton,
	TArray<int32>& OutParentIndices,
	TArray<FGIAG_BoneTRS>& OutInverseRefPoseTRS,
	int32& OutNumBones) const
{
	OutNumBones = 0;
	OutParentIndices.Reset();
	OutInverseRefPoseTRS.Reset();

	if (Skeleton == nullptr)
	{
		return false;
	}

	// Runtime build from reference skeleton.
	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	if (NumBones <= 0)
	{
		return false;
	}

	OutNumBones = NumBones;
	OutParentIndices.SetNumUninitialized(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		OutParentIndices[i] = RefSkel.GetParentIndex(i);
	}

	const TArray<FTransform>& LocalRefPose = RefSkel.GetRefBonePose();
	checkf(LocalRefPose.Num() == NumBones, TEXT("GIAG: ref pose size mismatch (Pose=%d Bones=%d)."), LocalRefPose.Num(), NumBones);
	TArray<FTransform> GlobalRefPose;
	GlobalRefPose.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		GlobalRefPose[i] = LocalRefPose[i];
		const int32 Parent = RefSkel.GetParentIndex(i);
		if (Parent >= 0)
		{
			GlobalRefPose[i] = GlobalRefPose[i] * GlobalRefPose[Parent];
		}
	}

	OutInverseRefPoseTRS.SetNumUninitialized(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		// NOTE: We store inverse *global* ref pose per bone (component-space inverse reference transform).
		// Assumes no negative scale (matching TRS multiply semantics).
		const FTransform Inv = GlobalRefPose[i].Inverse();
		const FQuat4f Q = FQuat4f(Inv.GetRotation().GetNormalized());
		const FVector3f T = FVector3f(Inv.GetTranslation());
		const FVector3f S = FVector3f(Inv.GetScale3D());
		FGIAG_BoneTRS& Out = OutInverseRefPoseTRS[i];
		Out = FGIAG_BoneTRS(Q, T, S);
	}

	return true;
}

namespace
{
	static void BuildRefPoseLocalCache(
		USkeleton* Skeleton,
		int32 NumBones,
		TArray<FTransform>& OutRefPoseLocal,
		TSharedPtr<TArray<FGIAG_BoneTRS>>& OutRefPoseLocalTRS)
	{
		check(Skeleton);
		check(NumBones > 0);

		const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
		check(RefSkel.GetNum() == NumBones);

		const TArray<FTransform>& RefPose = RefSkel.GetRefBonePose();
		check(RefPose.Num() == NumBones);

		OutRefPoseLocal = RefPose;
		GIAG::ApplySkeletonRootRotationOffset(Skeleton, OutRefPoseLocal[0]);

		OutRefPoseLocalTRS = MakeShared<TArray<FGIAG_BoneTRS>>();
		OutRefPoseLocalTRS->SetNumUninitialized(NumBones);

		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			const FTransform3f T = FTransform3f(OutRefPoseLocal[Bone]);
			const FQuat4f Q = T.GetRotation().GetNormalized();
			const FVector3f L = T.GetTranslation();
			const FVector3f S = T.GetScale3D();
			FGIAG_BoneTRS& Out = (*OutRefPoseLocalTRS)[Bone];
			Out = FGIAG_BoneTRS(Q, L, S);
		}
	}
}

bool UGameInstancedAnimationGraphSubsystem::GetOrBuildAnimSequencePixels(
	USkeleton* Skeleton,
	UAnimSequence* AnimSequence,
	float SecondsPerFrame,
	FGIAG_ClipMeta& OutMeta,
	TArray<FGIAG_BoneTRS>& OutTRS) const
{
	OutTRS.Reset();
	OutMeta.StartTransformIndex = 0;
	OutMeta.NumFrames = 0;
	OutMeta.SecondsPerFrame = SecondsPerFrame;
	OutMeta.SequenceLengthSeconds = 0.0f;

	if (Skeleton == nullptr || AnimSequence == nullptr || AnimSequence->GetSkeleton() != Skeleton)
	{
		return false;
	}

	// Runtime bake (sampling).
	const float Len = AnimSequence->GetPlayLength();
	const int32 NumBones = Skeleton->GetReferenceSkeleton().GetNum();
	if (NumBones <= 0 || Len <= 0.0f)
	{
		return false;
	}

	const float SPF = FMath::Max(1.0f / 120.0f, SecondsPerFrame);
	const int32 NumFrames = FMath::Max(1, FMath::CeilToInt(Len / SPF) + 1);

	OutMeta.StartTransformIndex = 0;
	OutMeta.NumFrames = NumFrames;
	OutMeta.SecondsPerFrame = SPF;
	OutMeta.SequenceLengthSeconds = Len;

	OutTRS.SetNumZeroed(NumFrames * NumBones);

	TArray<FTransform> LocalPose;
	const GIAG::FRootRotationOffset RootRotationOffset = GIAG::GetSkeletonRootRotationOffset(Skeleton);
	for (int32 Frame = 0; Frame < NumFrames; ++Frame)
	{
		const float Time = FMath::Min(Frame * SPF, Len);
		if (!GIAG::EvalAnimSequenceVisualLocalPose(AnimSequence, Time, Skeleton, LocalPose, RootRotationOffset))
		{
			return false;
		}

		const int32 FrameBase = Frame * NumBones;
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			const FTransform3f T = FTransform3f(LocalPose[Bone]);
			const FQuat4f Q = T.GetRotation().GetNormalized();
			const FVector3f L = T.GetTranslation();
			const FVector3f S = T.GetScale3D();
			FGIAG_BoneTRS& Out = OutTRS[FrameBase + Bone];
			Out = FGIAG_BoneTRS(Q, L, S);
		}
	}

	return true;
}

int32 UGameInstancedAnimationGraphSubsystem::FindOrCreateSkeletonCache(USkeleton* Skeleton)
{
	if (Skeleton == nullptr)
	{
		return INDEX_NONE;
	}
	if (const int32* Found = SkeletonCacheIndexBySkeleton.Find(Skeleton))
	{
		return *Found;
	}

	const int32 NewIndex = SkeletonCaches.Num();
	TUniquePtr<FSkeletonAnimCache> Cache = MakeUnique<FSkeletonAnimCache>();
	Cache->Skeleton = Skeleton;
	Cache->NumBones = Skeleton->GetReferenceSkeleton().GetNum();

	// Build skeleton static data now (fallback will rebuild on demand later too).
	GetOrBuildSkeletonStaticData(Skeleton, Cache->ParentIndices, Cache->InverseRefPoseTRS, Cache->NumBones);

	// Cache RefPose in local space (bone order == Skeleton reference skeleton).
	BuildRefPoseLocalCache(Skeleton, Cache->NumBones, Cache->RefPoseLocal, Cache->RefPoseLocalTRS);
	Cache->RefPoseVersion = 1;
	Cache->bRefPoseUploadSent = false;

	SkeletonCaches.Add(MoveTemp(Cache));
	SkeletonCacheIndexBySkeleton.Add(Skeleton, NewIndex);
	return NewIndex;
}

UGameInstancedAnimationGraphSubsystem::FSkeletonAnimCache* UGameInstancedAnimationGraphSubsystem::GetSkeletonCache(int32 CacheIndex)
{
	if (!SkeletonCaches.IsValidIndex(CacheIndex))
	{
		return nullptr;
	}
	return SkeletonCaches[CacheIndex].Get();
}

float UGameInstancedAnimationGraphSubsystem::GetWorldTimeSeconds() const
{
	return GetWorld()->GetTimeSeconds();
}

FGIAG_TimeSlot UGameInstancedAnimationGraphSubsystem::AllocateTimeSlot()
{
	check(IsInGameThread());
	for (int32 i = 1; i < GIAG_MAX_TIME_SLOTS; ++i)
	{
		if (!TimeSlotAllocated[i])
		{
			TimeSlotAllocated[i] = true;
			TimeSlots[i] = 0.f;
			return { i };
		}
	}
	checkf(false, TEXT("GIAG: All %d TimeSlots exhausted."), GIAG_MAX_TIME_SLOTS);
	return FGIAG_TimeSlot::WorldTime();
}

void UGameInstancedAnimationGraphSubsystem::FreeTimeSlot(FGIAG_TimeSlot Slot)
{
	check(IsInGameThread());
	check(Slot.IsValid() && !Slot.IsWorldTime());
	check(TimeSlotAllocated[Slot.Index]);
	TimeSlotAllocated[Slot.Index] = false;
	TimeSlots[Slot.Index] = 0.f;
}

void UGameInstancedAnimationGraphSubsystem::SetTimeSlotSeconds(FGIAG_TimeSlot Slot, float Seconds)
{
	check(IsInGameThread());
	check(Slot.IsValid());
	TimeSlots[Slot.Index] = Seconds;
}

float UGameInstancedAnimationGraphSubsystem::GetTimeSlotSeconds(FGIAG_TimeSlot Slot) const
{
	check(Slot.IsValid());
	return TimeSlots[Slot.Index];
}

int32 UGameInstancedAnimationGraphSubsystem::FindOrCreateGroup(UGIAG_AnimGraph* AnimGraph, USkeleton* Skeleton)
{
	check(AnimGraph);
	check(Skeleton);

	const FGroupKey Key{ AnimGraph, Skeleton };
	if (const int32* Found = GroupByKey.Find(Key))
	{
		return *Found;
	}

	// Compile graph once.
	const FGIAG_AnimGraphCompiledData& Compiled = AnimGraph->Compile();
	check(Compiled.NumNodes > 0);

	// Default instance defines node layout/offsets; we also keep pointers into it for default initialization.
	const FConstStructView DefaultGraphInstance = AnimGraph->GetDefaultGraphInstance();
	const UScriptStruct* DefaultInstanceStruct = DefaultGraphInstance.GetScriptStruct();
	check(DefaultInstanceStruct && DefaultGraphInstance.IsValid());

	FGraphGroup Group;
	Group.AnimGraph = AnimGraph;
	Group.Skeleton = Skeleton;
	Group.SkeletonCacheIndex = FindOrCreateSkeletonCache(Group.Skeleton);
	Group.Compiled = &Compiled;
	Group.NumBones = Skeleton->GetReferenceSkeleton().GetNum();

	// Build per-node AoS layout (stride aligned to 16) and default node pointers.
	{
		Group.NodeProperties.SetNum(Compiled.NumNodes);
		Group.NodeStrideBytes.SetNum(Compiled.NumNodes);
		Group.NodeIndexByMemberName.Reset();

		const uint8* DefaultMem = (const uint8*)DefaultGraphInstance.GetMemory();
		check(DefaultMem);
		for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
		{
			const FGIAG_AnimCompiledNode& Node = Compiled.Nodes[NodeIdx];
			check(Node.NodeMeta);
			check(Node.InstanceDataOffset >= 0);

			checkf(!Node.MemberName.IsNone(), TEXT("GIAG: Node %d has no MemberName; cannot address node instances."), NodeIdx);

			// Cache the actual StructProperty on the graph's DefaultGraphInstance.
			const FProperty* MemberProperty = DefaultInstanceStruct->FindPropertyByName(Node.MemberName);
			const FStructProperty* StructProp = CastField<FStructProperty>(MemberProperty);
			checkf(StructProp && StructProp->Struct, TEXT("GIAG: Node member '%s' is not a valid struct property on DefaultGraphInstance."), *Node.MemberName.ToString());
			checkf(StructProp->Struct->IsChildOf(FGIAG_AnimNodeBase::StaticStruct()),
				TEXT("GIAG: Node member '%s' is not derived from FGIAG_AnimNodeBase."), *Node.MemberName.ToString());

			const int32 Offset = StructProp->GetOffset_ForInternal();
			checkf(Offset == Node.InstanceDataOffset,
				TEXT("GIAG: Node member '%s' offset mismatch (Compiled=%d Property=%d). Recompile graph."),
				*Node.MemberName.ToString(), Node.InstanceDataOffset, Offset);

			UScriptStruct* NodeStruct = StructProp->Struct;
			check(NodeStruct);

			const uint32 Size = (uint32)NodeStruct->GetStructureSize();
			check(NodeStruct->GetMinAlignment() == 16);
			const uint32 Stride = Align(Size, NodeStruct->GetMinAlignment());
			check(Stride >= Size);

			Group.NodeProperties[NodeIdx] = StructProp;
			Group.NodeStrideBytes[NodeIdx] = Stride;
			checkf(!Group.NodeIndexByMemberName.Contains(Node.MemberName),
				TEXT("GIAG: duplicate node MemberName '%s' in graph '%s'."), *Node.MemberName.ToString(), *GetNameSafe(AnimGraph));
			Group.NodeIndexByMemberName.Add(Node.MemberName, NodeIdx);
		}
	}

	// Build optional shared resources once for this graph+ skeleton.
	{
		struct FUniqueResourceSource
		{
			FGIAG_AnimResourceRequest Request;
			const IGIAG_AnimNodeMeta* Meta = nullptr;
			FConstStructView Settings;
		};

		TMap<FGIAG_AnimResourceKey, FUniqueResourceSource> UniqueResources;
		TArray<TArray<TPair<uint8, FGIAG_AnimResourceKey>>> NodeSlotKeysTmp;
		NodeSlotKeysTmp.SetNum(Compiled.NumNodes);

		Group.MaxOptionalSRVSlot = -1;
		Group.OptionalSRVKeyByNodeBySlot.Reset();

		for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
		{
			const FGIAG_AnimCompiledNode& Node = Compiled.Nodes[NodeIdx];
			check(Node.NodeMeta);

			TArray<FGIAG_AnimResourceRequest> Requests;
			Node.NodeMeta->EnumerateResourceRequests(Node.Settings, Skeleton, EGIAG_AnimResourceTarget::GPU, Requests);
			for (const FGIAG_AnimResourceRequest& Request : Requests)
			{
				if (Request.ShareKey.IsNone())
				{
					continue;
				}
				check(Request.Layout.Kind == EGIAG_AnimResourceKind::Buffer);
				check(Request.Layout.StrideBytes != 0);
				check(Request.Layout.NumElements != 0);

				Group.MaxOptionalSRVSlot = FMath::Max(Group.MaxOptionalSRVSlot, (int32)Request.Slot);
				NodeSlotKeysTmp[NodeIdx].Add({ Request.Slot, Request.ShareKey });

				if (!UniqueResources.Contains(Request.ShareKey))
				{
					FUniqueResourceSource Src;
					Src.Request = Request;
					Src.Meta = Node.NodeMeta;
					Src.Settings = Node.Settings;
					UniqueResources.Add(Request.ShareKey, Src);
				}
			}
		}

		if (Group.MaxOptionalSRVSlot >= 0)
		{
			check(SharedResourceBus.IsValid());

			// [NodeIndex][Slot] -> ShareKey
			Group.OptionalSRVKeyByNodeBySlot.SetNum(Compiled.NumNodes);
			for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
			{
				Group.OptionalSRVKeyByNodeBySlot[NodeIdx].SetNumZeroed(Group.MaxOptionalSRVSlot + 1);
				for (const TPair<uint8, FGIAG_AnimResourceKey>& SlotKeyPair : NodeSlotKeysTmp[NodeIdx])
				{
					if ((int32)SlotKeyPair.Key <= Group.MaxOptionalSRVSlot)
					{
						Group.OptionalSRVKeyByNodeBySlot[NodeIdx][(int32)SlotKeyPair.Key] = SlotKeyPair.Value;
					}
				}
			}

			// Build per-key bytes once per World (SharedResourcesByKey) and enqueue incremental uploads for RT.
			for (const TPair<FGIAG_AnimResourceKey, FUniqueResourceSource>& Pair : UniqueResources)
			{
				const FUniqueResourceSource& Source = Pair.Value;
				check(Source.Meta);

				FSharedResourceEntry* Existing = SharedResourcesByKey.Find(Pair.Key);
				if (!Existing)
				{
					FSharedResourceEntry NewEntry;
					NewEntry.Request = Source.Request;
					NewEntry.Bytes = MakeShared<TArray<uint8>>();
					const bool bOk = Source.Meta->BuildResourceForGPU(Source.Request, Source.Settings, Skeleton, *NewEntry.Bytes);
					check(bOk);
					Existing = &SharedResourcesByKey.Add(Pair.Key, MoveTemp(NewEntry));

					FGIAG_AnimGraphResourceUploadRun Run;
					Run.Request = Existing->Request;
					Run.Bytes = Existing->Bytes;
					SharedResourceBus->Enqueue_GameThread(MoveTemp(Run));
				}
				else
				{
					// Contract: same key must imply identical layout.
					check(Existing->Request.Layout.Kind == Source.Request.Layout.Kind);
					check(Existing->Request.Layout.StrideBytes == Source.Request.Layout.StrideBytes);
					check(Existing->Request.Layout.NumElements == Source.Request.Layout.NumElements);
				}
			}
		}
	}

	const int32 GroupIndex = Groups.Add(MoveTemp(Group));
	GroupByKey.Add(Key, GroupIndex);
	return GroupIndex;
}

namespace
{
	static FGIAG_EventPtr MakeSharedEvent()
	{
		FEvent* Raw = FPlatformProcess::GetSynchEventFromPool(true /* bManualReset */);
		return FGIAG_EventPtr(Raw, [](FEvent* E)
		{
			if (E)
			{
				E->Reset();
				FPlatformProcess::ReturnSynchEventToPool(E);
			}
		});
	}
}

struct UGameInstancedAnimationGraphSubsystem::FClipBakePayload
{
	FGIAG_EventPtr CompletionEvent;
	FGIAG_AnimTRSPtr TRS;
	TAtomic<bool> bCancelled{ false };
};

int32 UGameInstancedAnimationGraphSubsystem::RequestClipBake(int32 GroupIndex, const UAnimSequence* AnimSequence, double NowSeconds)
{
	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	if (AnimSequence == nullptr)
	{
		return INDEX_NONE;
	}
	FGraphGroup& Group = Groups[GroupIndex];
	check(Group.Skeleton);
	if (Group.Skeleton == nullptr)
	{
		return INDEX_NONE;
	}
	USkeleton* SeqSkeleton = AnimSequence->GetSkeleton();
	if (SeqSkeleton == nullptr || SeqSkeleton != Group.Skeleton)
	{
		return INDEX_NONE;
	}
	FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	if (!Cache || Cache->Skeleton != Group.Skeleton)
	{
		return INDEX_NONE;
	}

	auto StartBakeTask = [](USkeleton* Skeleton, const UAnimSequence* Anim, float SPF, float Len, int32 NumFrames, int32 NumBonesLocal, const GIAG::FRootRotationOffset RootRotationOffset, const FClipBakePayloadPtr& BakePayload)
	{
		Async(EAsyncExecution::ThreadPool, [Skeleton, Anim, SPF, Len, NumFrames, NumBonesLocal, RootRotationOffset, BakePayload]()
		{
			if (!BakePayload.IsValid())
			{
				return;
			}
			if (BakePayload->bCancelled.Load())
			{
				if (BakePayload->CompletionEvent.IsValid())
				{
					BakePayload->CompletionEvent->Trigger();
				}
				return;
			}

			if (Skeleton == nullptr || Anim == nullptr || Anim->GetSkeleton() != Skeleton)
			{
				BakePayload->bCancelled.Store(true);
				if (BakePayload->CompletionEvent.IsValid())
				{
					BakePayload->CompletionEvent->Trigger();
				}
				return;
			}

			TArray<FTransform> LocalPose;
			TArray<FGIAG_BoneTRS>& Out = *BakePayload->TRS;
			const int32 Expected = NumFrames * NumBonesLocal;
			if (Out.Num() != Expected)
			{
				Out.SetNumZeroed(Expected);
			}

			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				const float Time = FMath::Min(Frame * SPF, Len);
				if (!GIAG::EvalAnimSequenceVisualLocalPose(Anim, Time, Skeleton, LocalPose, RootRotationOffset))
				{
					BakePayload->bCancelled.Store(true);
					break;
				}

				const int32 FrameBase = Frame * NumBonesLocal;
				for (int32 Bone = 0; Bone < NumBonesLocal; ++Bone)
				{
					const FTransform3f T = FTransform3f(LocalPose[Bone]);
					const FQuat4f Q = T.GetRotation().GetNormalized();
					const FVector3f L = T.GetTranslation();
					const FVector3f S = T.GetScale3D();
					FGIAG_BoneTRS& O = Out[FrameBase + Bone];
					O = FGIAG_BoneTRS(Q, L, S);
				}
			}

			if (BakePayload->CompletionEvent.IsValid())
			{
				BakePayload->CompletionEvent->Trigger();
			}
		});
	};

	auto AllocTransforms = [&Cache](int32 InNumTransforms) -> int32
	{
		if (InNumTransforms <= 0)
		{
			return INDEX_NONE;
		}
		for (int32 i = 0; i < Cache->FreeTransformBlocks.Num(); ++i)
		{
			FSkeletonAnimCache::FTransformBlock& B = Cache->FreeTransformBlocks[i];
			if (B.Size >= InNumTransforms)
			{
				const int32 Start = B.Start;
				B.Start += InNumTransforms;
				B.Size -= InNumTransforms;
				if (B.Size <= 0)
				{
					Cache->FreeTransformBlocks.RemoveAtSwap(i, EAllowShrinking::No);
				}
				return Start;
			}
		}
		const int32 Start = Cache->AnimTRSCapacity;
		Cache->AnimTRSCapacity += InNumTransforms;
		return Start;
	};

	// Resolve (or allocate) a stable ClipIndex.
	const int32* Existing = Cache->SequenceToClipIndex.Find(AnimSequence);
	const bool bIsNewClipIndex = (Existing == nullptr);

	int32 ClipIndex = INDEX_NONE;
	if (!bIsNewClipIndex)
	{
		ClipIndex = *Existing;
		check(Cache->ClipSlots.IsValidIndex(ClipIndex));
	}
	else
	{
		if (Cache->FreeClipIndices.Num() > 0)
		{
			ClipIndex = Cache->FreeClipIndices.Pop(EAllowShrinking::No);
		}
		else
		{
			ClipIndex = Cache->ClipSlots.Num();
			Cache->ClipSlots.AddDefaulted();
		}
		checkf(ClipIndex >= 0, TEXT("GIAG: failed to allocate ClipIndex."));
	}

	// Ensure mapping table exists and is updated.
	if (Cache->ClipIndexToSequence.Num() <= ClipIndex)
	{
		Cache->ClipIndexToSequence.SetNumZeroed(ClipIndex + 1);
	}
	Cache->ClipIndexToSequence[ClipIndex] = AnimSequence;

	FSkeletonAnimCache::FClipSlot& Slot = Cache->ClipSlots[ClipIndex];

	// New clip index: reset slot state and add mapping.
	if (bIsNewClipIndex)
	{
		Slot = FSkeletonAnimCache::FClipSlot();
		Slot.bAllocated = true;
		Slot.bEvicted = false;
		Cache->SequenceToClipIndex.Add(AnimSequence, ClipIndex);
		AnimSequences.AddUnique(AnimSequence);
	}

	Slot.bAllocated = true;
	Slot.bEvicted = false;
	Slot.LastRequestedTimeSeconds = NowSeconds;

	// Fast-path: already baked.
	{
		const bool bHasTRS = (Slot.StartTransformIndex != INDEX_NONE && Slot.NumTransforms > 0);
		const bool bHasMeta = (Slot.Meta.StartTransformIndex != -1 && Slot.Meta.NumFrames > 0 && Slot.Meta.SecondsPerFrame > 0.0f);
		if (bHasTRS && bHasMeta)
		{
			return ClipIndex;
		}
	}

	// Runtime bake (sampling): do NOT block GT; produce pixels on worker thread and signal event.
	const float Len = AnimSequence->GetPlayLength();
	const int32 NumBones = Group.Skeleton->GetReferenceSkeleton().GetNum();
	if (NumBones <= 0 || Len <= 0.0f)
	{
		Slot.bAllocated = false;
		Cache->ClipIndexToSequence[ClipIndex] = nullptr;
		Cache->FreeClipIndices.Add(ClipIndex);
		return INDEX_NONE;
	}

	float SecondsPerFrame = 1.0f / 30.0f;
	if (UGIAG_AnimSequenceUserData* UserData = Cast<UGIAG_AnimSequenceUserData>(const_cast<UAnimSequence*>(AnimSequence)->GetAssetUserDataOfClass(UGIAG_AnimSequenceUserData::StaticClass())))
	{
		SecondsPerFrame = UserData->SecondsPerFrame;
	}
	const float SPF = FMath::Max(1.0f / 120.0f, SecondsPerFrame);
	const int32 NumFrames = FMath::Max(1, FMath::CeilToInt(Len / SPF) + 1);
	const int32 NumTransforms = NumFrames * NumBones;

	const int32 StartTransform = AllocTransforms(NumTransforms);
	if (StartTransform == INDEX_NONE)
	{
		Slot.bAllocated = false;
		if (Cache->FreeClipIndices.Num() == 0 || Cache->FreeClipIndices.Last() != ClipIndex)
		{
			Cache->FreeClipIndices.Add(ClipIndex);
		}
		return INDEX_NONE;
	}

	Slot.StartTransformIndex = StartTransform;
	Slot.NumTransforms = NumTransforms;
	Slot.Meta.StartTransformIndex = StartTransform;
	Slot.Meta.NumFrames = NumFrames;
	Slot.Meta.SecondsPerFrame = SPF;
	Slot.Meta.SequenceLengthSeconds = Len;
	Slot.bMetaDirty = true;
	Slot.bTRSDirty = true;

	Slot.Bake = MakeShared<FClipBakePayload>();
	Slot.Bake->CompletionEvent = MakeSharedEvent();
	Slot.Bake->TRS = MakeShared<TArray<FGIAG_BoneTRS>>();
	Slot.Bake->TRS->SetNumZeroed(NumTransforms);

	const GIAG::FRootRotationOffset RootRotationOffset = GIAG::GetSkeletonRootRotationOffset(Group.Skeleton);
	StartBakeTask(Group.Skeleton, AnimSequence, SPF, Len, NumFrames, NumBones, RootRotationOffset, Slot.Bake);

	return ClipIndex;
}

int32 UGameInstancedAnimationGraphSubsystem::RequestClipIndexOnly(int32 GroupIndex, const UAnimSequence* AnimSequence, double NowSeconds)
{
	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	check(AnimSequence);

	FGraphGroup& Group = Groups[GroupIndex];
	check(Group.Skeleton);

	USkeleton* SeqSkeleton = AnimSequence->GetSkeleton();
	check(SeqSkeleton && SeqSkeleton == Group.Skeleton);

	FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	check(Cache && Cache->Skeleton == Group.Skeleton);

	// Already assigned?
	if (const int32* Existing = Cache->SequenceToClipIndex.Find(AnimSequence))
	{
		const int32 ClipIndex = *Existing;
		if (Cache->ClipSlots.IsValidIndex(ClipIndex))
		{
			Cache->ClipSlots[ClipIndex].LastRequestedTimeSeconds = NowSeconds;
		}
		if (Cache->ClipIndexToSequence.Num() <= ClipIndex)
		{
			Cache->ClipIndexToSequence.SetNumZeroed(ClipIndex + 1);
		}
		Cache->ClipIndexToSequence[ClipIndex] = AnimSequence;
		return ClipIndex;
	}

	// Allocate a stable ClipIndex.
	int32 ClipIndex = INDEX_NONE;
	if (Cache->FreeClipIndices.Num() > 0)
	{
		ClipIndex = Cache->FreeClipIndices.Pop(EAllowShrinking::No);
	}
	else
	{
		ClipIndex = Cache->ClipSlots.Num();
		Cache->ClipSlots.AddDefaulted();
	}
	checkf(ClipIndex >= 0, TEXT("GIAG: failed to allocate ClipIndex."));

	FSkeletonAnimCache::FClipSlot& Slot = Cache->ClipSlots[ClipIndex];
	Slot = FSkeletonAnimCache::FClipSlot();
	Slot.bAllocated = true;
	Slot.bEvicted = false;
	Slot.LastRequestedTimeSeconds = NowSeconds;

	if (Cache->ClipIndexToSequence.Num() <= ClipIndex)
	{
		Cache->ClipIndexToSequence.SetNumZeroed(ClipIndex + 1);
	}
	Cache->ClipIndexToSequence[ClipIndex] = AnimSequence;

	Cache->SequenceToClipIndex.Add(AnimSequence, ClipIndex);
	AnimSequences.AddUnique(AnimSequence);
	return ClipIndex;
}

int32 UGameInstancedAnimationGraphSubsystem::FindOrCreateBucket(USkeletalMesh* SkeletalMesh, int32 GroupIndex)
{
	check(SkeletalMesh);

	EnsureHostActor();
	check(HostActor);

	const FBucketKey Key{ SkeletalMesh, GroupIndex, /*bFollower=*/false };
	if (const int32* Found = BucketByKey.Find(Key))
	{
		return *Found;
	}

	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	FGraphGroup& Group = Groups[GroupIndex];
	check(Group.Skeleton);
	check(Group.Compiled);

	FMeshBucket Bucket;
	Bucket.SkeletalMesh = SkeletalMesh;
	Bucket.BoundSphereRadius = SkeletalMesh->GetBounds().SphereRadius;
	Bucket.GroupIndex = GroupIndex;

	// Shared provider state for the bucket (one ISKMC per bucket since UE 5.8).
	Bucket.SharedState = TRefCountPtr<FGIAG_TransformProviderState>(new FGIAG_TransformProviderState());

	// Create the bucket-owned ISKMC + transform provider with the initial-tier slot capacity.
	FPrivateUtils::InitBucketAsMaster(Bucket, Group, *HostActor, *SkeletalMesh, GIAG::BucketCapacity::InitialCapacity);
	check(Bucket.ISKMC != nullptr);

	const int32 NewBucketIndex = Buckets.Add(MoveTemp(Bucket));
	BucketByKey.Add(Key, NewBucketIndex);
	SkeletalMeshes.Add(SkeletalMesh);

	checkf(!Group.BucketIndices.Contains(NewBucketIndex), TEXT("GIAG: bucket index already registered in group."));
	Group.BucketIndices.Add(NewBucketIndex);

	return NewBucketIndex;
}

void UGameInstancedAnimationGraphSubsystem::GrowMasterBucketAndFollowers(int32 MasterBucketIndex, int32 NewCapacity)
{
	check(IsInGameThread());
	checkf(Buckets.IsValidIndex(MasterBucketIndex), TEXT("GIAG: invalid MasterBucketIndex=%d."), MasterBucketIndex);

	FMeshBucket& MasterBucket = Buckets[MasterBucketIndex];
	const int32 OldCap = MasterBucket.GetTotalSlotCapacity();
	if (NewCapacity <= OldCap)
	{
		return;
	}

	checkf(MasterBucket.bStorageInitialized, TEXT("GIAG: cannot grow uninitialized master bucket."));
	checkf(MasterBucket.TransformProvider && MasterBucket.TransformProvider->GetMode() == EGIAG_TransformProviderMode::MasterEvaluate,
		TEXT("GIAG: GrowMasterBucketAndFollowers called on non-master bucket %d."), MasterBucketIndex);
	checkf(Groups.IsValidIndex(MasterBucket.GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), MasterBucket.GroupIndex);

	const FGraphGroup& Group = Groups[MasterBucket.GroupIndex];
	check(Group.Compiled);

	// Capture master's state before any TransformProvider mutations so follower lookup uses the right key.
	FGIAG_TransformProviderState* MasterState = MasterBucket.TransformProvider->GetState().GetReference();
	check(MasterState);

	auto ApplyCapacityToProvider = [NewCapacity](FMeshBucket& Bucket)
	{
		Bucket.SharedState->SlotCapacity = NewCapacity;
		Bucket.TotalSlotCapacity = NewCapacity;
		Bucket.TransformProvider->AnimationSlotCount = NewCapacity;
		Bucket.bGrewThisFrame = true;

		// Tell the engine ExtensionProxy to update its UniqueAnimationCount field. The engine's
		// per-frame polling (`FUpdater::UpdateLambda`) compares stored count vs. proxy count and,
		// on mismatch, allocates a new TransformBuffer span and dispatches `FCopyPreviousTransformsCS`
		// which copies `min(OldCount, NewCount)` transforms from old span to new span. After the
		// copy, the engine sets `DirtyBoneTransforms = Current` — the per-primitive Cur/Prev frame
		// rotation continues to work and motion blur is preserved across grow with no extra writes
		// from us. (Compare to the previous SetTransformProvider/MarkRenderStateDirty path which
		// tore down the SceneProxy entirely and forced DirtyBoneTransforms=All.)
		TRefCountPtr<FGIAG_TransformProviderState> StateRef(Bucket.SharedState);
		ENQUEUE_RENDER_COMMAND(GIAG_QueueGrow)(
			[StateRef, NewCapacity](FRHICommandListImmediate& /*RHICmdList*/) mutable
			{
				StateRef->PendingCapacityChange_RT.PendingNewCap = NewCapacity;
				StateRef->PendingCapacityChange_RT.PendingSlotMoves.Reset();
			});
	};

	// 1) Grow master bucket's CPU storage + push capacity to engine.
	MasterBucket.GrowCapacity(NewCapacity, *Group.Compiled, MakeArrayView(Group.NodeStrideBytes));
	ApplyCapacityToProvider(MasterBucket);

	// 2) Grow all follower buckets bound to the same master state. Followers don't have per-slot CPU
	//    storage (no graph eval), so we only sync TotalSlotCapacity + provider AnimationSlotCount and
	//    mark the ISKMC pending rebind so the engine reallocates its TransformBuffer at frame end.
	for (auto It = Buckets.CreateIterator(); It; ++It)
	{
		if (It.GetIndex() == MasterBucketIndex)
		{
			continue;
		}
		FMeshBucket& Other = *It;
		if (!Other.TransformProvider || !Other.ISKMC)
		{
			continue;
		}
		if (Other.TransformProvider->GetMode() != EGIAG_TransformProviderMode::FollowerCopyOrRemap)
		{
			continue;
		}
		if (Other.TransformProvider->GetMasterState().GetReference() != MasterState)
		{
			continue;
		}
		if (Other.GetTotalSlotCapacity() >= NewCapacity)
		{
			continue;
		}

		if (Other.NumMaterialDataFloats > 0)
		{
			Other.MaterialDataBySlot.SetNumZeroed((int64)NewCapacity * (int64)Other.NumMaterialDataFloats);
		}
		Other.TotalSlotCapacity = NewCapacity;
		ApplyCapacityToProvider(Other);
	}
}

void UGameInstancedAnimationGraphSubsystem::CompactAndShrinkMaster(int32 MasterBucketIndex, int32 NewCapacity)
{
	check(IsInGameThread());
	checkf(Buckets.IsValidIndex(MasterBucketIndex), TEXT("GIAG: invalid MasterBucketIndex=%d."), MasterBucketIndex);

	FMeshBucket& MasterBucket = Buckets[MasterBucketIndex];
	const int32 OldCap = MasterBucket.GetTotalSlotCapacity();

	checkf(MasterBucket.bStorageInitialized, TEXT("GIAG: cannot shrink uninitialized master bucket."));
	checkf(MasterBucket.TransformProvider && MasterBucket.TransformProvider->GetMode() == EGIAG_TransformProviderMode::MasterEvaluate,
		TEXT("GIAG: CompactAndShrinkMaster called on non-master bucket %d."), MasterBucketIndex);
	checkf(GIAG::BucketCapacity::IsValidTier(NewCapacity),
		TEXT("GIAG: shrink NewCapacity=%d must be one of the bucket capacity tiers."),
		NewCapacity);
	checkf(NewCapacity < OldCap, TEXT("GIAG: shrink NewCapacity=%d must be < OldCap=%d."), NewCapacity, OldCap);

	checkf(Groups.IsValidIndex(MasterBucket.GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), MasterBucket.GroupIndex);
	const FGraphGroup& Group = Groups[MasterBucket.GroupIndex];
	check(Group.Compiled);
	const int32 NumNodes = Group.Compiled->NumNodes;

	// 1) Pair high-slot live instances with low-slot free ones. Cap shrink contract requires
	//    every live slot at index >= NewCapacity to migrate into [0, NewCapacity).
	TArray<int32> HighLiveSlots;
	HighLiveSlots.Reserve(FMath::Max(0, OldCap - NewCapacity));
	for (int32 SlotIdx = NewCapacity; SlotIdx < OldCap; ++SlotIdx)
	{
		if (MasterBucket.SlotAlive[SlotIdx])
		{
			HighLiveSlots.Add(SlotIdx);
		}
	}

	TArray<int32> LowFreeSlots;
	LowFreeSlots.Reserve(HighLiveSlots.Num());
	for (int32 SlotIdx = 0; SlotIdx < NewCapacity && LowFreeSlots.Num() < HighLiveSlots.Num(); ++SlotIdx)
	{
		if (!MasterBucket.SlotAlive[SlotIdx])
		{
			LowFreeSlots.Add(SlotIdx);
		}
	}
	checkf(LowFreeSlots.Num() == HighLiveSlots.Num(),
		TEXT("GIAG: shrink to %d cannot fit %d high-slot lives in %d low-slot frees (used=%d, OldCap=%d)."),
		NewCapacity, HighLiveSlots.Num(), LowFreeSlots.Num(), MasterBucket.NumInstances, OldCap);

	// Find all follower buckets bound to this master state (cap stays in lockstep with master).
	TArray<int32> FollowerBucketIndices;
	FGIAG_TransformProviderState* MasterStateRaw = MasterBucket.TransformProvider->GetState().GetReference();
	for (auto FIt = Buckets.CreateIterator(); FIt; ++FIt)
	{
		if (FIt.GetIndex() == MasterBucketIndex)
		{
			continue;
		}
		FMeshBucket& Other = *FIt;
		if (!Other.TransformProvider || !Other.ISKMC)
		{
			continue;
		}
		if (Other.TransformProvider->GetMode() != EGIAG_TransformProviderMode::FollowerCopyOrRemap)
		{
			continue;
		}
		if (Other.TransformProvider->GetMasterState().GetReference() != MasterStateRaw)
		{
			continue;
		}
		FollowerBucketIndices.Add(FIt.GetIndex());
	}

	// 2) Move each (HighLive -> LowFree) pair: rewire CPU arrays + ISKMC instance + follower records.
	for (int32 i = 0; i < HighLiveSlots.Num(); ++i)
	{
		const int32 OldSlot = HighLiveSlots[i];
		const int32 NewSlot = LowFreeSlots[i];
		check(MasterBucket.SlotAlive[OldSlot] && !MasterBucket.SlotAlive[NewSlot]);

		const int32 RecordIndex = MasterBucket.RecordIndexBySlot[OldSlot];
		checkf(AnimRecords.IsValidIndex(RecordIndex), TEXT("GIAG shrink: invalid RecordIndex=%d at slot=%d."), RecordIndex, OldSlot);
		FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
		checkf(Rec.SlotIndex == OldSlot, TEXT("GIAG shrink: Rec.SlotIndex=%d != OldSlot=%d."), Rec.SlotIndex, OldSlot);

		// Per-node AoS data: memcpy the row from old slot to new slot, zero old (DestroyStruct equivalent
		// not needed — the struct moves location, no copy/destroy ctor lifecycle).
		for (int32 NodeIdx = 0; NodeIdx < NumNodes; ++NodeIdx)
		{
			const uint32 Stride = MasterBucket.NodeStrideBytes[NodeIdx];
			uint8* Storage = MasterBucket.NodeStorageByNode[NodeIdx].GetData();
			uint8* OldPtr = Storage + (int64)Stride * (int64)OldSlot;
			uint8* NewPtr = Storage + (int64)Stride * (int64)NewSlot;
			FMemory::Memcpy(NewPtr, OldPtr, Stride);
			FMemory::Memzero(OldPtr, Stride);

			// Per-node dirty bitmap follows the slot.
			TBitArray<>& Bits = MasterBucket.NodeParamDirtyBitsByNode[NodeIdx];
			const bool bWasDirty = Bits[OldSlot];
			Bits[OldSlot] = false;
			Bits[NewSlot] = bWasDirty;
			TArray<uint32>& DirtyList = MasterBucket.DirtyNodeParamSlotsByNode[NodeIdx];
			for (int32 Idx = 0; Idx < DirtyList.Num(); ++Idx)
			{
				if (DirtyList[Idx] == (uint32)OldSlot) { DirtyList[Idx] = (uint32)NewSlot; break; }
			}
		}

		// Slot bookkeeping.
		MasterBucket.SlotAlive[OldSlot] = false;
		MasterBucket.SlotAlive[NewSlot] = true;
		MasterBucket.RecordIndexBySlot[OldSlot] = INDEX_NONE;
		MasterBucket.RecordIndexBySlot[NewSlot] = RecordIndex;

		MasterBucket.TransformBySlot[NewSlot] = MasterBucket.TransformBySlot[OldSlot];
		MasterBucket.TransformBySlot[OldSlot] = FTransform::Identity;

		MasterBucket.TimeSlotIndexBySlot[NewSlot] = MasterBucket.TimeSlotIndexBySlot[OldSlot];
		MasterBucket.TimeSlotIndexBySlot[OldSlot] = 0;

		if (MasterBucket.NumMaterialDataFloats > 0)
		{
			const int32 Stride = MasterBucket.NumMaterialDataFloats;
			float* Base = MasterBucket.MaterialDataBySlot.GetData();
			FMemory::Memcpy(Base + (int64)NewSlot * Stride, Base + (int64)OldSlot * Stride, Stride * sizeof(float));
			FMemory::Memzero(Base + (int64)OldSlot * Stride, Stride * sizeof(float));
		}

		const bool bWasTransformDirty = MasterBucket.TransformDirty[OldSlot];
		MasterBucket.TransformDirty[OldSlot] = false;
		MasterBucket.TransformDirty[NewSlot] = bWasTransformDirty;
		for (int32 Idx = 0; Idx < MasterBucket.DirtyTransformSlots.Num(); ++Idx)
		{
			if (MasterBucket.DirtyTransformSlots[Idx] == (uint32)OldSlot) { MasterBucket.DirtyTransformSlots[Idx] = (uint32)NewSlot; break; }
		}
		for (int32 Idx = 0; Idx < MasterBucket.NewSlotsThisTick.Num(); ++Idx)
		{
			if (MasterBucket.NewSlotsThisTick[Idx] == (uint32)OldSlot) { MasterBucket.NewSlotsThisTick[Idx] = (uint32)NewSlot; break; }
		}

		// Alive list membership: rewrite either GpuAliveSlots or CpuAliveSlots based on the slot's mode.
		auto MoveAliveListEntry = [OldSlot, NewSlot](TArray<uint32>& AliveSlots, TArray<int32>& IndexBySlot)
		{
			const int32 ListIdx = IndexBySlot[OldSlot];
			if (ListIdx == INDEX_NONE)
			{
				return;
			}
			AliveSlots[ListIdx] = (uint32)NewSlot;
			IndexBySlot[NewSlot] = ListIdx;
			IndexBySlot[OldSlot] = INDEX_NONE;
		};
		MoveAliveListEntry(MasterBucket.GpuAliveSlots, MasterBucket.GpuAliveListIndexBySlot);
		MoveAliveListEntry(MasterBucket.CpuAliveSlots, MasterBucket.CpuAliveListIndexBySlot);

		Rec.SlotIndex = NewSlot;

		// ISKMC: only GPU-mode masters have an instance; CPU-mode masters have CpuProxyActor instead.
		// `SetInstanceAnimationIndex` is the lightweight engine API that just rewrites the per-instance
		// AnimationIndex in InstanceData and marks SkinningDataChanged — keeps the same PrimitiveInstanceId,
		// no Remove+Add churn. The GPU TransformBuffer slot data still needs to be remapped to the new
		// position; that happens in the RT compaction CS dispatched by the renderer extension.
		if (Rec.ISKMC != nullptr && Rec.CpuProxyActor == nullptr)
		{
			check(Rec.ISKMC == MasterBucket.ISKMC);
			MasterBucket.ISKMC->SetInstanceAnimationIndex(Rec.InstanceId, NewSlot);
		}

		// Followers: every follower record bound to this master slot tracks the new SlotIndex and
		// updates its ISKMC instance's AnimationIndex (data remapping handled by the RT compaction CS).
		if (Rec.FollowRecordIndices.Num() > 0)
		{
			for (const int32 FollowIndex : Rec.FollowRecordIndices)
			{
				if (!AnimRecords.IsValidIndex(FollowIndex)) { continue; }
				FInstancedAnimRecord& FollowRec = AnimRecords[FollowIndex];
				if (FollowRec.SlotIndex != OldSlot)
				{
					continue;
				}
				FollowRec.SlotIndex = NewSlot;
				if (FollowRec.ISKMC != nullptr)
				{
					FollowRec.ISKMC->SetInstanceAnimationIndex(FollowRec.InstanceId, NewSlot);
				}
			}
		}
	}

	// 3) Rebuild FreeSlots from inactive low slots only (anything in [NewCapacity, OldCap) is gone).
	MasterBucket.FreeSlots.Reset();
	for (int32 SlotIdx = NewCapacity - 1; SlotIdx >= 0; --SlotIdx)
	{
		if (!MasterBucket.SlotAlive[SlotIdx])
		{
			MasterBucket.FreeSlots.Add(SlotIdx);
		}
	}

	// 4) Truncate per-slot CPU arrays.
	MasterBucket.RecordIndexBySlot.SetNum(NewCapacity);
	MasterBucket.SlotAlive.SetNum(NewCapacity, false);
	MasterBucket.TimeSlotIndexBySlot.SetNum(NewCapacity);
	MasterBucket.TransformBySlot.SetNum(NewCapacity);
	MasterBucket.TransformDirty.SetNum(NewCapacity, false);
	MasterBucket.GpuAliveListIndexBySlot.SetNum(NewCapacity);
	MasterBucket.CpuAliveListIndexBySlot.SetNum(NewCapacity);
	for (int32 NodeIdx = 0; NodeIdx < NumNodes; ++NodeIdx)
	{
		const uint32 Stride = MasterBucket.NodeStrideBytes[NodeIdx];
		MasterBucket.NodeStorageByNode[NodeIdx].SetNum((int64)NewCapacity * (int64)Stride);
		MasterBucket.NodeBasePtrsByNode[NodeIdx] = MasterBucket.NodeStorageByNode[NodeIdx].GetData();
		MasterBucket.NodeParamDirtyBitsByNode[NodeIdx].SetNum(NewCapacity, false);
	}
	if (MasterBucket.NumMaterialDataFloats > 0)
	{
		MasterBucket.MaterialDataBySlot.SetNum((int64)NewCapacity * (int64)MasterBucket.NumMaterialDataFloats);
	}

	// 4b) Compact + truncate follower bucket MaterialDataBySlot (same slot moves apply).
	for (const int32 FollowerBucketIndex : FollowerBucketIndices)
	{
		FMeshBucket& FollowerBucket = Buckets[FollowerBucketIndex];
		if (FollowerBucket.NumMaterialDataFloats > 0)
		{
			const int32 FStride = FollowerBucket.NumMaterialDataFloats;
			float* FBase = FollowerBucket.MaterialDataBySlot.GetData();
			for (int32 i = 0; i < HighLiveSlots.Num(); ++i)
			{
				const int32 OldSlot = HighLiveSlots[i];
				const int32 NewSlot = LowFreeSlots[i];
				FMemory::Memcpy(FBase + (int64)NewSlot * FStride, FBase + (int64)OldSlot * FStride, FStride * sizeof(float));
			}
			FollowerBucket.MaterialDataBySlot.SetNum((int64)NewCapacity * (int64)FStride);
		}
	}

	// 5) Build the RT-side slot move plan once. The renderer extension consumes this list to remap
	//    Cur+Prev data within the existing TransformBuffer span before triggering the engine span
	//    truncation. Master and every follower bucket bound to the same MasterState share the same
	//    move list (slot indices are 1:1 between master and follower spans).
	TArray<FGIAG_TransformProviderState::FGIAG_PendingCapacityChange::FSlotMove> SlotMoves;
	SlotMoves.Reserve(HighLiveSlots.Num());
	for (int32 i = 0; i < HighLiveSlots.Num(); ++i)
	{
		SlotMoves.Add({ (uint32)HighLiveSlots[i], (uint32)LowFreeSlots[i] });
	}

	// 6) Sync TotalSlotCapacity + provider AnimationSlotCount on master and all followers, and queue
	//    the RT-side capacity change (with the slot move plan, so the renderer extension knows to
	//    dispatch the GPU compaction CS before calling SetUniqueAnimationCount). Engine's per-frame
	//    polling will then truncate the span on next pre-update; FCopyPreviousTransformsCS will copy
	//    the (already-remapped) [0, NewCap) range, so motion blur is preserved across shrink.
	auto ApplyShrinkToBucket = [NewCapacity, &SlotMoves](FMeshBucket& Bucket)
	{
		Bucket.SharedState->SlotCapacity = NewCapacity;
		Bucket.TotalSlotCapacity = NewCapacity;
		Bucket.TransformProvider->AnimationSlotCount = NewCapacity;

		TRefCountPtr<FGIAG_TransformProviderState> StateRef(Bucket.SharedState);
		TArray<FGIAG_TransformProviderState::FGIAG_PendingCapacityChange::FSlotMove> MovesCopy = SlotMoves;
		ENQUEUE_RENDER_COMMAND(GIAG_QueueShrink)(
			[StateRef, NewCapacity, MovesCopy = MoveTemp(MovesCopy)](FRHICommandListImmediate& /*RHICmdList*/) mutable
			{
				StateRef->PendingCapacityChange_RT.PendingNewCap = NewCapacity;
				StateRef->PendingCapacityChange_RT.PendingSlotMoves = MoveTemp(MovesCopy);
			});
	};
	ApplyShrinkToBucket(MasterBucket);
	for (const int32 FollowerBucketIndex : FollowerBucketIndices)
	{
		ApplyShrinkToBucket(Buckets[FollowerBucketIndex]);
	}
}

void UGameInstancedAnimationGraphSubsystem::ReserveBucketCapacity(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, int32 Count)
{
	check(IsInGameThread());
	if (!SkeletalMesh || !AnimGraph || Count <= 0)
	{
		return;
	}

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (!Skeleton)
	{
		return;
	}

	const int32 GroupIndex = FindOrCreateGroup(AnimGraph, Skeleton);
	const int32 BucketIndex = FindOrCreateBucket(SkeletalMesh, GroupIndex);
	checkf(Buckets.IsValidIndex(BucketIndex), TEXT("GIAG Reserve: invalid BucketIndex=%d."), BucketIndex);

	FMeshBucket& Bucket = Buckets[BucketIndex];
	const int32 OldCap = Bucket.GetTotalSlotCapacity();
	const int32 TargetCap = GIAG::BucketCapacity::RoundUpToTier(Count);
	if (TargetCap > OldCap)
	{
		GrowMasterBucketAndFollowers(BucketIndex, TargetCap);
	}
}

void UGameInstancedAnimationGraphSubsystem::ShrinkBucket(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph)
{
	check(IsInGameThread());
	if (!SkeletalMesh || !AnimGraph)
	{
		return;
	}

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (!Skeleton)
	{
		return;
	}

	int32 GroupIndex = INDEX_NONE;
	for (auto It = Groups.CreateIterator(); It; ++It)
	{
		if (It->AnimGraph == AnimGraph && It->Skeleton == Skeleton)
		{
			GroupIndex = It.GetIndex();
			break;
		}
	}
	if (GroupIndex == INDEX_NONE)
	{
		return;
	}

	const FBucketKey Key{ SkeletalMesh, GroupIndex, /*bFollower=*/false };
	const int32* Found = BucketByKey.Find(Key);
	if (!Found)
	{
		return;
	}

	FMeshBucket& Bucket = Buckets[*Found];
	if (!Bucket.bStorageInitialized || !Bucket.TransformProvider)
	{
		return;
	}
	if (Bucket.TransformProvider->GetMode() != EGIAG_TransformProviderMode::MasterEvaluate)
	{
		return;
	}

	const int32 OldCap = Bucket.GetTotalSlotCapacity();
	const int32 TargetCap = GIAG::BucketCapacity::ComputeShrinkTarget(Bucket.NumInstances, OldCap);
	if (TargetCap >= OldCap)
	{
		return;
	}

	// Manual shrink ignores the same-frame grow flap guard (caller asked explicitly). The call below
	// queues the RT capacity change + slot-move plan onto SharedState->PendingCapacityChange_RT;
	// the renderer extension drains it on the next ProvideTransforms call, and the engine truncates
	// the per-primitive span on the following pre-update.
	CompactAndShrinkMaster(*Found, TargetCap);
}

int32 UGameInstancedAnimationGraphSubsystem::GetBucketSlotCapacity(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph) const
{
	if (!SkeletalMesh || !AnimGraph) { return 0; }
	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (!Skeleton) { return 0; }

	int32 GroupIndex = INDEX_NONE;
	for (auto It = Groups.CreateConstIterator(); It; ++It)
	{
		if (It->AnimGraph == AnimGraph && It->Skeleton == Skeleton)
		{
			GroupIndex = It.GetIndex();
			break;
		}
	}
	if (GroupIndex == INDEX_NONE) { return 0; }

	const FBucketKey Key{ SkeletalMesh, GroupIndex, /*bFollower=*/false };
	const int32* Found = BucketByKey.Find(Key);
	if (!Found || !Buckets.IsValidIndex(*Found)) { return 0; }

	return Buckets[*Found].GetTotalSlotCapacity();
}

void UGameInstancedAnimationGraphSubsystem::ShrinkAllBuckets()
{
	check(IsInGameThread());

	// Capture indices first; CompactAndShrinkMaster may mutate Buckets sparse array iteration.
	TArray<int32> MasterIndices;
	for (auto It = Buckets.CreateIterator(); It; ++It)
	{
		FMeshBucket& Bucket = *It;
		if (!Bucket.bStorageInitialized || !Bucket.TransformProvider)
		{
			continue;
		}
		if (Bucket.TransformProvider->GetMode() != EGIAG_TransformProviderMode::MasterEvaluate)
		{
			continue;
		}
		MasterIndices.Add(It.GetIndex());
	}

	for (const int32 MasterIndex : MasterIndices)
	{
		if (!Buckets.IsValidIndex(MasterIndex))
		{
			continue;
		}
		FMeshBucket& Bucket = Buckets[MasterIndex];
		const int32 OldCap = Bucket.GetTotalSlotCapacity();
		const int32 TargetCap = GIAG::BucketCapacity::ComputeShrinkTarget(Bucket.NumInstances, OldCap);
		if (TargetCap >= OldCap)
		{
			continue;
		}
		CompactAndShrinkMaster(MasterIndex, TargetCap);
	}
}

int32 UGameInstancedAnimationGraphSubsystem::FindOrCreateFollowerBucket(
	USkeletalMesh* SkeletalMesh,
	int32 GroupIndex,
	const TRefCountPtr<FGIAG_TransformProviderState>& MasterState,
	int32 DstNumBones,
	int32 SrcNumBones,
	const TSharedPtr<const TArray<uint32>>& RemapShared)
{
	check(SkeletalMesh);
	check(MasterState.IsValid());

	EnsureHostActor();
	check(HostActor);

	const FBucketKey Key{ SkeletalMesh, GroupIndex, /*bFollower=*/true };
	if (const int32* Found = BucketByKey.Find(Key))
	{
		// Existing follower bucket must be bound to the same master state. (A single follower
		// mesh attached to multiple distinct masters in the same group is not supported.)
		FMeshBucket& Existing = Buckets[*Found];
		checkf(Existing.TransformProvider && Existing.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap,
			TEXT("GIAG: follower bucket key marked follower but TransformProvider not in Follower mode (Mesh=%s)."), *GetNameSafe(SkeletalMesh));
		check(Existing.TransformProvider->GetMasterState() == MasterState);
		return *Found;
	}

	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	FGraphGroup& Group = Groups[GroupIndex];

	FMeshBucket Bucket;
	Bucket.SkeletalMesh = SkeletalMesh;
	Bucket.BoundSphereRadius = SkeletalMesh->GetBounds().SphereRadius;
	Bucket.GroupIndex = GroupIndex;

	// Follower bucket: SharedState exists for the SelfState non-null contract but is not graph-evaluated.
	Bucket.SharedState = TRefCountPtr<FGIAG_TransformProviderState>(new FGIAG_TransformProviderState());

	// Slot capacity must mirror the master's so AnimationIndex (= absolute master slot) fits.
	// Contract: master bucket has been through InitBucketAsMaster before any follower bucket is created.
	checkf(MasterState->SlotCapacity > 0, TEXT("GIAG: follower bucket created before master InitBucketAsMaster (SlotCapacity=0)."));
	const int32 MasterCap = MasterState->SlotCapacity;

	// Configure as Follower BEFORE SetTransformProvider so the engine sees follower mode on first
	// SceneProxy build (CreateRenderProxy snapshots provider state at proxy creation time).
	FPrivateUtils::InitBucketAsFollower(Bucket, *HostActor, *SkeletalMesh, MasterCap,
		MasterState, DstNumBones, SrcNumBones, RemapShared);
	check(Bucket.ISKMC != nullptr);
	Bucket.SharedState->SlotCapacity = MasterCap;
	Bucket.TotalSlotCapacity = MasterCap;

	if (Bucket.NumMaterialDataFloats > 0)
	{
		Bucket.MaterialDataBySlot.SetNumZeroed((int64)MasterCap * (int64)Bucket.NumMaterialDataFloats);
	}

	const int32 NewBucketIndex = Buckets.Add(MoveTemp(Bucket));
	BucketByKey.Add(Key, NewBucketIndex);
	SkeletalMeshes.Add(SkeletalMesh);

	checkf(!Group.BucketIndices.Contains(NewBucketIndex), TEXT("GIAG: bucket index already registered in group."));
	Group.BucketIndices.Add(NewBucketIndex);

	return NewBucketIndex;
}

UGameInstancedAnimationGraphSubsystem::FInstancedAnimRecord* UGameInstancedAnimationGraphSubsystem::ResolveRecord(const FGameInstancedAnimationGraphHandle& Handle)
{
	if (!AnimRecords.IsValidIndex(Handle.RecordIndex))
	{
		return nullptr;
	}
	if (!AnimRecordSerials.IsValidIndex(Handle.RecordIndex) || AnimRecordSerials[Handle.RecordIndex] != Handle.SerialNumber)
	{
		return nullptr;
	}
	return &AnimRecords[Handle.RecordIndex];
}

void UGameInstancedAnimationGraphSubsystem::InvalidateHandle(FGameInstancedAnimationGraphHandle& Handle)
{
	Handle.RecordIndex = INDEX_NONE;
	Handle.SerialNumber = INDEX_NONE;
}

void UGameInstancedAnimationGraphSubsystem::CleanupGroupIfUnused(int32 GroupIndex)
{
	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);

	// Keep group alive while any bucket references it.
	for (const FMeshBucket& Bucket : Buckets)
	{
		if (Bucket.GroupIndex == GroupIndex)
		{
			return;
		}
	}

	// Keep group alive while any record references it.
	for (const FInstancedAnimRecord& Rec : AnimRecords)
	{
		if (Rec.GroupIndex == GroupIndex)
		{
			return;
		}
	}

	FGraphGroup& Group = Groups[GroupIndex];

	GroupByKey.Remove(FGroupKey{ Group.AnimGraph, Group.Skeleton });
	Groups.RemoveAt(GroupIndex);
}

void UGameInstancedAnimationGraphSubsystem::CleanupBucketIfEmpty(int32 BucketIndex)
{
	checkf(Buckets.IsValidIndex(BucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), BucketIndex);

	FMeshBucket& Bucket = Buckets[BucketIndex];
	if (Bucket.NumInstances > 0)
	{
		return;
	}

	const int32 GroupIndex = Bucket.GroupIndex;
	const bool bFollowerKey = Bucket.TransformProvider
		&& Bucket.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap;
	const FBucketKey Key{ Bucket.SkeletalMesh, Bucket.GroupIndex, bFollowerKey };
	BucketByKey.Remove(Key);

	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d (from BucketIndex=%d)."), GroupIndex, BucketIndex);
	Groups[GroupIndex].BucketIndices.RemoveSingleSwap(BucketIndex, EAllowShrinking::No);

	if (Bucket.ISKMC)
	{
		Bucket.ISKMC->DestroyComponent();
		Bucket.ISKMC = nullptr;
	}
	Bucket.TransformProvider = nullptr;

	if (Bucket.SkeletalMesh)
	{
		SkeletalMeshes.RemoveSingle(Bucket.SkeletalMesh);
		Bucket.SkeletalMesh = nullptr;
	}

	Buckets.RemoveAt(BucketIndex);

	CleanupGroupIfUnused(GroupIndex);
}

FGameInstancedAnimationGraphHandle UGameInstancedAnimationGraphSubsystem::AddInstance(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, const FTransform& Transform, TSubclassOf<AActor> CpuProxyClass, bool bCpuMode, FGIAG_TimeSlot TimeSlot)
{
	return AddInstance_Internal(SkeletalMesh, AnimGraph, Transform, CpuProxyClass, bCpuMode, /*ExternalCpuProxyActor*/nullptr, TimeSlot);
}

FGameInstancedAnimationGraphHandle UGameInstancedAnimationGraphSubsystem::AddInstance_Internal(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, const FTransform& Transform, TSubclassOf<AActor> CpuProxyClass, bool bCpuMode, AActor* ExternalCpuProxyActor, FGIAG_TimeSlot TimeSlot)
{
	check(IsInGameThread());

	FGameInstancedAnimationGraphHandle Handle;

	if (SkeletalMesh == nullptr || !AnimGraph)
	{
		return Handle;
	}

	// External proxy actor => CPU-only, reuse this actor and never spawn/destroy.
	if (ExternalCpuProxyActor)
	{
		checkf(bCpuMode, TEXT("GIAG ExternalProxyActor path must be CPU-only."));
		check(ExternalCpuProxyActor->GetWorld() == GetWorld());
		check(ExternalCpuProxyActor->GetClass()->ImplementsInterface(UGIAG_ActorInterface::StaticClass()));
		CpuProxyClass = ExternalCpuProxyActor->GetClass();
	}

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (!ensureMsgf(Skeleton, TEXT("GIAG: SkeletalMesh '%s' has null Skeleton; refusing to bind."), *GetNameSafe(SkeletalMesh)))
	{
		return Handle;
	}
	
#if WITH_EDITOR
	if (!ensureMsgf(SkeletalMesh->IsNaniteEnabled(), TEXT("'%s' must have Nanite enabled, otherwise it will not enter the skinning TransformProvider path."), *GetNameSafe(SkeletalMesh)))
	{
		return Handle;
	}
#endif

	const int32 GroupIndex = FindOrCreateGroup(AnimGraph, Skeleton);
	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	FGraphGroup& Group = Groups[GroupIndex];
	check(Group.Skeleton);
	if (Group.Skeleton == nullptr || Group.NumBones <= 0)
	{
		return Handle;
	}
	FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	if (!Cache || Cache->Skeleton != Group.Skeleton)
	{
		return Handle;
	}
	if (!Group.Compiled || Group.Compiled->NumNodes <= 0)
	{
		return Handle;
	}

	EnsureHostActor();
	check(HostActor);

	const auto BucketIndex = FindOrCreateBucket(SkeletalMesh, GroupIndex);
	check(Buckets.IsValidIndex(BucketIndex));
	FMeshBucket* Bucket = &Buckets[BucketIndex];

	int32 AllocatedSlot = Bucket->AllocateSlot();
	if (AllocatedSlot == INDEX_NONE)
	{
		const int32 OldCap = Bucket->GetTotalSlotCapacity();
		const int32 NewCap = GIAG::BucketCapacity::ComputeGrowTarget(OldCap);
		GrowMasterBucketAndFollowers(BucketIndex, NewCap);
		Bucket = &Buckets[BucketIndex];
		AllocatedSlot = Bucket->AllocateSlot();
		checkf(AllocatedSlot != INDEX_NONE,
			TEXT("GIAG: bucket grow to %d slots did not yield a free slot — should be impossible."),
			NewCap);
	}

	check(Bucket->ISKMC != nullptr);

	// Initialize per-node AoS data for this slot from graph defaults.
	check(Group.Compiled);
	check(Group.NodeProperties.Num() == Group.Compiled->NumNodes);
	
	auto DefaultGraphInstance = AnimGraph->GetDefaultGraphInstance();
	for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
	{
		const FGIAG_AnimCompiledNode& Node = Group.Compiled->Nodes[NodeIdx];
		check(Node.NodeMeta);
		const FStructProperty* StructProp = Group.NodeProperties[NodeIdx];
		check(StructProp && StructProp->Struct);

		uint8* NodePtr = Bucket->GetNodePtr(NodeIdx, AllocatedSlot);
		StructProp->Struct->InitializeStruct(NodePtr);
		StructProp->Struct->CopyScriptStruct(NodePtr, Group.NodeProperties[NodeIdx]->ContainerPtrToValuePtr<void>(DefaultGraphInstance.GetMemory()));
		Node.NodeMeta->InitInstanceData(NodePtr);
	}

	// Record (authoritative).
	const int32 RecordIndex = AnimRecords.Add({});
	FInstancedAnimRecord& NewRecord = AnimRecords[RecordIndex];
	NewRecord.SkeletalMesh = SkeletalMesh;
	NewRecord.CpuProxyClass = CpuProxyClass;
	NewRecord.BucketIndex = BucketIndex;
	NewRecord.GroupIndex = GroupIndex;
	NewRecord.SlotIndex = AllocatedSlot;
	NewRecord.TimeSlotIndex = (uint8)TimeSlot.Index;

	if (AnimRecordSerials.Num() <= RecordIndex)
	{
		AnimRecordSerials.SetNumZeroed(RecordIndex + 1);
	}
	int32& Serial = AnimRecordSerials[RecordIndex];
	// Avoid INDEX_NONE (UE uses -1); keep serial positive.
	Serial = FMath::Max(1, Serial + 1);
	Handle.RecordIndex = RecordIndex;
	Handle.SerialNumber = Serial;

	Bucket->NumInstances++;
	checkf(AllocatedSlot >= 0 && AllocatedSlot < Bucket->GetTotalSlotCapacity(), TEXT("GIAG: invalid SlotIndex=%d."), AllocatedSlot);
	checkf(Bucket->SlotAlive.Num() == Bucket->GetTotalSlotCapacity() && Bucket->RecordIndexBySlot.Num() == Bucket->GetTotalSlotCapacity(),
		TEXT("GIAG: bucket slot arrays size mismatch (SlotAlive=%d RecordIndexBySlot=%d Cap=%d)."),
		Bucket->SlotAlive.Num(), Bucket->RecordIndexBySlot.Num(), Bucket->GetTotalSlotCapacity());
	Bucket->RecordIndexBySlot[AllocatedSlot] = RecordIndex;
	Bucket->SlotAlive[AllocatedSlot] = true;
	Bucket->TimeSlotIndexBySlot[AllocatedSlot] = (uint8)TimeSlot.Index;
	Bucket->TransformBySlot[AllocatedSlot] = Transform;
	Bucket->NewSlotsThisTick.Add((uint32)AllocatedSlot);
	if (!Bucket->TransformDirty[AllocatedSlot])
	{
		Bucket->TransformDirty[AllocatedSlot] = true;
		Bucket->DirtyTransformSlots.Add((uint32)AllocatedSlot);
	}

	// Force initial GPU param upload for all nodes on this slot (event-driven; no per-frame scan).
	for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
	{
		Bucket->MarkNodeParamDirty(NodeIdx, AllocatedSlot);
	}

	// Create backend:
	// - GPU: create ISKMC instance immediately.
	// - CPU: create a slot-only record, then spawn CpuProxyActor (no ISKMC instance), or reuse an external proxy actor.
	if (!bCpuMode)
	{
		const FPrimitiveInstanceId InstanceId = Bucket->ISKMC->AddInstance(Transform, AllocatedSlot, true);
		NewRecord.ISKMC = Bucket->ISKMC;
		NewRecord.InstanceId = InstanceId;
		FPrivateUtils::AddSlotToList(Bucket->GpuAliveSlots, Bucket->GpuAliveListIndexBySlot, AllocatedSlot);
	}
	else
	{
		check(CpuProxyClass);
		NewRecord.ISKMC = nullptr;
		NewRecord.InstanceId = FPrimitiveInstanceId();
		FPrivateUtils::AddSlotToList(Bucket->CpuAliveSlots, Bucket->CpuAliveListIndexBySlot, AllocatedSlot);
		FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
		if (ExternalCpuProxyActor)
		{
			Rec.CpuProxyActor = ExternalCpuProxyActor;
			Rec.bExternalCpuProxyActor = true;
			Rec.CpuProxyClass = ExternalCpuProxyActor->GetClass();
			IGIAG_ActorInterface::Execute_SetInstancedAnimationGraphHandle(ExternalCpuProxyActor, Handle);
			ExternalCpuProxyActor->SetActorTransform(Transform);
		}
		else
		{
			Rec.CpuProxyActor = FPrivateUtils::SpawnCpuProxyActor(Handle, Rec, GetWorld(), Transform);
		}
	}

	return Handle;
}

FGameInstancedAnimationGraphHandle UGameInstancedAnimationGraphSubsystem::AddInstanceWithExternalProxyActor(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, const FTransform& Transform, AActor* CpuProxyActor, FGIAG_TimeSlot TimeSlot)
{
	USkeletalMeshComponent* SkeletalMeshComponent = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(CpuProxyActor);
	check(SkeletalMeshComponent);
	check(SkeletalMeshComponent->GetSkeletalMeshAsset() == SkeletalMesh);
	return AddInstance_Internal(SkeletalMesh, AnimGraph, Transform, CpuProxyActor ? CpuProxyActor->GetClass() : nullptr, /*bCpuMode*/true, CpuProxyActor, TimeSlot);
}

void UGameInstancedAnimationGraphSubsystem::SetInstanceUseCPUMode(const FGameInstancedAnimationGraphHandle& Handle, bool bUseCPU)
{
	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return;
	}
	if (Rec->MasterRecordIndex != INDEX_NONE)
	{
		return;
	}

	const bool bIsCPU = (Rec->CpuProxyActor != nullptr);
	if (bUseCPU == bIsCPU)
	{
		return;
	}

	if (!bUseCPU && Rec->bExternalCpuProxyActor)
	{
		// External proxy actor path is CPU-only by contract.
		return;
	}

	checkf(Groups.IsValidIndex(Rec->GroupIndex), TEXT("GIAG CPU: invalid GroupIndex=%d."), Rec->GroupIndex);
	FGraphGroup& Group = Groups[Rec->GroupIndex];
	check(Group.Compiled);

	if (bUseCPU)
	{
		SwitchMasterGpuToCpu(Handle, Rec);
	}
	else
	{
		SwitchMasterCpuToGpu(Handle, Rec, Group);
	}
}

void UGameInstancedAnimationGraphSubsystem::EnsureCpuAttachSyncIndexCapacity(int32 RecordIndex)
{
	check(IsInGameThread());
	if (RecordIndex < 0)
	{
		return;
	}
	if (CpuAttachSyncListIndexByRecordIndex.Num() <= RecordIndex)
	{
		const int32 OldNum = CpuAttachSyncListIndexByRecordIndex.Num();
		CpuAttachSyncListIndexByRecordIndex.SetNum(RecordIndex + 1);
		for (int32 i = OldNum; i < CpuAttachSyncListIndexByRecordIndex.Num(); ++i)
		{
			CpuAttachSyncListIndexByRecordIndex[i] = INDEX_NONE;
		}
	}
}

void UGameInstancedAnimationGraphSubsystem::RegisterCpuAttachSyncMasterRecord(int32 RecordIndex)
{
	check(IsInGameThread());
	if (RecordIndex < 0)
	{
		return;
	}
	EnsureCpuAttachSyncIndexCapacity(RecordIndex);
	if (CpuAttachSyncListIndexByRecordIndex[RecordIndex] != INDEX_NONE)
	{
		return;
	}
	const int32 NewIndex = CpuAttachSyncMasterRecordIndices.Add(RecordIndex);
	CpuAttachSyncListIndexByRecordIndex[RecordIndex] = NewIndex;
}

void UGameInstancedAnimationGraphSubsystem::UnregisterCpuAttachSyncMasterRecord(int32 RecordIndex)
{
	check(IsInGameThread());
	if (RecordIndex < 0)
	{
		return;
	}
	EnsureCpuAttachSyncIndexCapacity(RecordIndex);
	const int32 ListIndex = CpuAttachSyncListIndexByRecordIndex[RecordIndex];
	if (ListIndex == INDEX_NONE)
	{
		return;
	}
	check(CpuAttachSyncMasterRecordIndices.IsValidIndex(ListIndex));
	const int32 LastRecordIndex = CpuAttachSyncMasterRecordIndices.Last();
	CpuAttachSyncMasterRecordIndices[ListIndex] = LastRecordIndex;
	CpuAttachSyncMasterRecordIndices.Pop(EAllowShrinking::No);
	CpuAttachSyncListIndexByRecordIndex[RecordIndex] = INDEX_NONE;
	if (LastRecordIndex != RecordIndex)
	{
		EnsureCpuAttachSyncIndexCapacity(LastRecordIndex);
		CpuAttachSyncListIndexByRecordIndex[LastRecordIndex] = ListIndex;
	}
}

void UGameInstancedAnimationGraphSubsystem::UpdateCpuAttachSyncRegistration_GameThread(int32 RecordIndex)
{
	check(IsInGameThread());
	if (RecordIndex < 0)
	{
		return;
	}

	if (!AnimRecords.IsValidIndex(RecordIndex))
	{
		UnregisterCpuAttachSyncMasterRecord(RecordIndex);
		return;
	}

	const FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];

	auto HasAnyNonProxyAttach = [&]() -> bool
	{
		if (Rec.AttachHandles.Num() == 0)
		{
			return false;
		}
		for (const FGameInstancedAnimationAttachHandle& AttachHandle : Rec.AttachHandles)
		{
			check(AttachHandle);
			const FAttachBucketLocator* Loc = AttachBucketById.Find(AttachHandle.BucketId);
			check(Loc);

			const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);
			const FAttachSlotTable* Slots = nullptr;
			const TArray<FAttachEntry>* Entries = nullptr;
			if (bNativeBucket)
			{
				check(NativeAttachBuckets.IsValidIndex(Loc->Index));
				const FNativeAttachBucket& B = NativeAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			else
			{
				check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
				const FNiagaraAttachBucket& B = NiagaraAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			check(Slots && Entries);

			check(Slots->OutputIndexByAttachSlot.IsValidIndex((int32)AttachHandle.Slot));
			check(Slots->SlotGeneration.IsValidIndex((int32)AttachHandle.Slot));
			check(Slots->SlotGeneration[(int32)AttachHandle.Slot] == AttachHandle.Generation);
			const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)AttachHandle.Slot];
			check(Entries->IsValidIndex(OutputIndex));

			const FAttachEntry& Entry = (*Entries)[OutputIndex];
			const bool bHasCpuProxy = (Entry.CpuStaticMeshComponent != nullptr) || (Entry.CpuNiagaraComponent != nullptr);
			if (!bHasCpuProxy)
			{
				return true;
			}
		}
		return false;
	};

	const bool bShouldRegister =
		(Rec.MasterRecordIndex == INDEX_NONE)
		&& (Rec.CpuProxyActor != nullptr)
		&& HasAnyNonProxyAttach();

	if (bShouldRegister)
	{
		RegisterCpuAttachSyncMasterRecord(RecordIndex);
	}
	else
	{
		UnregisterCpuAttachSyncMasterRecord(RecordIndex);
	}
}

void UGameInstancedAnimationGraphSubsystem::SwitchMasterGpuToCpu(const FGameInstancedAnimationGraphHandle& Handle, FInstancedAnimRecord* Rec)
{
	check(IsInGameThread());
	check(Rec);

	check(Rec->ISKMC);
	check(Rec->SkeletalMesh);
	check(Rec->CpuProxyClass);
	check(Rec->BucketIndex != INDEX_NONE && Rec->SlotIndex != INDEX_NONE);

	checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), Rec->BucketIndex);
	FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	check(Bucket.bStorageInitialized);
	check(Bucket.ISKMC != nullptr);
	check(Rec->SlotIndex >= 0 && Rec->SlotIndex < Bucket.GetTotalSlotCapacity());

	const int32 BucketSlot = Rec->SlotIndex;
	const FTransform CurrentTransform = Bucket.TransformBySlot[BucketSlot];

	Rec->ISKMC->RemoveInstance(Rec->InstanceId);

	// Keep slot + instance bytes alive; only switch backend list membership.
	FPrivateUtils::RemoveSlotFromList(Bucket.GpuAliveSlots, Bucket.GpuAliveListIndexBySlot, BucketSlot);
	FPrivateUtils::AddSlotToList(Bucket.CpuAliveSlots, Bucket.CpuAliveListIndexBySlot, BucketSlot);

	// Spawn CPU proxy actor.
	Rec->CpuProxyActor = FPrivateUtils::SpawnCpuProxyActor(Handle, *Rec, GetWorld(), CurrentTransform);
	Rec->ISKMC = nullptr;
	Rec->InstanceId = FPrimitiveInstanceId();

	if (Bucket.NumMaterialDataFloats > 0)
	{
		USkeletalMeshComponent* Skinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
		check(Skinned);
		const float* SlotBase = Bucket.MaterialDataBySlot.GetData() + (int64)BucketSlot * (int64)Bucket.NumMaterialDataFloats;
		Skinned->SetCustomPrimitiveDataFloatArray(0, TConstArrayView<float>(SlotBase, Bucket.NumMaterialDataFloats));
	}

	// Mark all attachments as CPU-owned (skip GPU attach compute writes).
	// This prevents other GPU instances evaluating the same ProviderState from overwriting CPU sync/hide.
	if (Rec->AttachHandles.Num() > 0)
	{
		static constexpr uint32 SkipGpuWriteFlag = 1u; // must match HLSL GIAG_AttachFlag_SkipGpuWrite

		for (const FGameInstancedAnimationAttachHandle& AttachHandle : Rec->AttachHandles)
		{
			if (!AttachHandle)
			{
				continue;
			}

			const uint32 AttachBucketId = AttachHandle.BucketId;
			const uint16 AttachSlot = AttachHandle.Slot;
			const uint16 AttachGen = AttachHandle.Generation;

			const FAttachBucketLocator* Loc = AttachBucketById.Find(AttachBucketId);
			check(Loc);

			const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);
			FAttachSlotTable* Slots = nullptr;
			TArray<FAttachEntry>* Entries = nullptr;
			if (bNativeBucket)
			{
				check(NativeAttachBuckets.IsValidIndex(Loc->Index));
				FNativeAttachBucket& B = NativeAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			else
			{
				check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
				FNiagaraAttachBucket& B = NiagaraAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			check(Slots && Entries);
			check(Slots->OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration[(int32)AttachSlot] == AttachGen);
			const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)AttachSlot];
			check(Entries->IsValidIndex(OutputIndex));

			FAttachEntry& Entry = (*Entries)[OutputIndex];
			Entry.DescFlags |= SkipGpuWriteFlag;

			if (AttachBus.IsValid())
			{
				FGIAG_AttachBus::FAttachUpdateOp Op;
				Op.BucketId = AttachBucketId;
				Op.BucketKind = bNativeBucket ? FGIAG_AttachBus::EBucketKind::Native : FGIAG_AttachBus::EBucketKind::Niagara;
				Op.AttachSlot = (uint32)AttachSlot;
				Op.State = Entry.State;
				Op.Desc.SlotIndex = Entry.SlotIndex;
				Op.Desc.BoneIndex = Entry.BoneIndex;
				Op.Desc.OutputIndex = (uint32)OutputIndex;
				Op.Desc.Flags = Entry.DescFlags;
				Op.Desc.SocketLocalTRS = Entry.SocketLocalTRS;
				AttachBus->Enqueue_GameThread(MoveTemp(Op));
			}
		}
	}

	// Attach CPU proxies (optional) and kill Niagara GPU particles where requested (via FxParticleGen mismatch).
	if (Rec->AttachHandles.Num() > 0)
	{
		USkeletalMeshComponent* ProxySkinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
		check(ProxySkinned);

		for (const FGameInstancedAnimationAttachHandle& AttachHandle : Rec->AttachHandles)
		{
			if (!AttachHandle)
			{
				continue;
			}

			const uint32 AttachBucketId = AttachHandle.BucketId;
			const uint16 AttachSlot = AttachHandle.Slot;
			const uint16 AttachGen = AttachHandle.Generation;

			const FAttachBucketLocator* Loc = AttachBucketById.Find(AttachBucketId);
			check(Loc);

			const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);
			FAttachSlotTable* Slots = nullptr;
			TArray<FAttachEntry>* Entries = nullptr;
			UStaticMesh* BucketStaticMesh = nullptr;
			if (bNativeBucket)
			{
				check(NativeAttachBuckets.IsValidIndex(Loc->Index));
				FNativeAttachBucket& B = NativeAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
				BucketStaticMesh = B.StaticMesh;
			}
			else
			{
				check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
				FNiagaraAttachBucket& B = NiagaraAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
				BucketStaticMesh = B.StaticMesh;
			}
			check(Slots && Entries);
			check(Slots->OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration[(int32)AttachSlot] == AttachGen);
			const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)AttachSlot];
			check(Entries->IsValidIndex(OutputIndex));

			FAttachEntry& Entry = (*Entries)[OutputIndex];

			const FTransform Rel = FTransform(Entry.SocketLocalTRS);

			if (BucketStaticMesh && Entry.bCreateCpuProxyStaticMesh && Entry.CpuStaticMeshComponent == nullptr)
			{
				UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(Rec->CpuProxyActor);
				check(Comp);
				Comp->CreationMethod = EComponentCreationMethod::Instance;
				Comp->SetStaticMesh(BucketStaticMesh);
				Comp->SetupAttachment(ProxySkinned, Entry.BoneName);
				Comp->SetRelativeTransform(Rel);
				Comp->RegisterComponent();
				Entry.CpuStaticMeshComponent = Comp;

				if (bNativeBucket)
				{
					EnqueueAttachHideInstance(AttachBucketId, (uint32)OutputIndex);
				}
				else
				{
					NiagaraAttach_KillGpuParticleBySlot(AttachBucketId, AttachSlot);
				}
			}

			if (Entry.CpuProxyNiagaraSystem && Entry.CpuNiagaraComponent == nullptr)
			{
				if (UWorld* W = GetWorld())
				{
					UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
						W,
						Entry.CpuProxyNiagaraSystem,
						FVector::ZeroVector,
						FRotator::ZeroRotator,
						FVector::OneVector,
						/*bAutoDestroy*/ false,
						/*bAutoActivate*/ true,
						ENCPoolMethod::ManualRelease,
						/*bPreCullCheck*/ false);
					if (Comp)
					{
						Comp->AttachToComponent(ProxySkinned, FAttachmentTransformRules::KeepRelativeTransform, Entry.BoneName);
						Comp->SetRelativeTransform(Rel);
						Comp->Activate(true);
						Entry.CpuNiagaraComponent = Comp;

						// Niagara CPU proxy => kill Niagara GPU particles for this attachment.
						NiagaraAttach_KillGpuParticleBySlot(AttachBucketId, AttachSlot);
					}
				}
			}
		}
	}

	// Convert follows to CPU components (LeaderPose) and remove their GPU render instances.
	if (Rec->FollowRecordIndices.Num() > 0)
	{
		USkeletalMeshComponent* Leader = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
		check(Leader);

		const TArray<int32> FollowIndicesCopy = Rec->FollowRecordIndices;
		for (const int32 FollowIndex : FollowIndicesCopy)
		{
			check(AnimRecords.IsValidIndex(FollowIndex));
			FInstancedAnimRecord& FollowRec = AnimRecords[FollowIndex];
			check(FollowRec.MasterRecordIndex == Handle.RecordIndex);
			check(!FollowRec.CpuFollowSkinnedMesh);

			if (FollowRec.ISKMC)
			{
				const int32 FollowBucketIndex = FollowRec.BucketIndex;
				checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG Follow GPU->CPU: invalid BucketIndex=%d."), FollowBucketIndex);

				FollowRec.ISKMC->RemoveInstance(FollowRec.InstanceId);
				Buckets[FollowBucketIndex].NumInstances = FMath::Max(0, Buckets[FollowBucketIndex].NumInstances - 1);

				FollowRec.ISKMC = nullptr;
				FollowRec.InstanceId = FPrimitiveInstanceId();
			}

			FollowRec.CpuFollowSkinnedMesh = FPrivateUtils::CreateCpuFollowComponent(Rec->CpuProxyActor, Leader, FollowRec.SkeletalMesh);
			if (FollowRec.BucketIndex != INDEX_NONE)
			{
				const FMeshBucket& FollowBucket = Buckets[FollowRec.BucketIndex];
				if (FollowBucket.NumMaterialDataFloats > 0)
				{
					const float* SlotBase = FollowBucket.MaterialDataBySlot.GetData() + (int64)FollowRec.SlotIndex * (int64)FollowBucket.NumMaterialDataFloats;
					FollowRec.CpuFollowSkinnedMesh->SetCustomPrimitiveDataFloatArray(0, TConstArrayView<float>(SlotBase, FollowBucket.NumMaterialDataFloats));
				}
			}
		}
	}

	UpdateCpuAttachSyncRegistration_GameThread(Handle.RecordIndex);
}

void UGameInstancedAnimationGraphSubsystem::SwitchMasterCpuToGpu(const FGameInstancedAnimationGraphHandle& Handle, FInstancedAnimRecord* Rec, FGraphGroup& Group)
{
	check(IsInGameThread());
	check(Rec);

	// CPU -> GPU
	check(Rec->CpuProxyActor);
	check(Rec->SkeletalMesh);
	check(Rec->BucketIndex != INDEX_NONE && Rec->SlotIndex != INDEX_NONE);

	checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG CPU->GPU: invalid BucketIndex=%d."), Rec->BucketIndex);
	FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	check(Bucket.bStorageInitialized);
	check(Bucket.ISKMC != nullptr);
	check(Bucket.TransformProvider != nullptr);
	const int32 MasterBucketSlot = Rec->SlotIndex;

	// Convert CPU follow components back to GPU follow instances first (Handle stability).
	if (Rec->FollowRecordIndices.Num() > 0)
	{
		TRefCountPtr<FGIAG_TransformProviderState> MasterState = Bucket.TransformProvider->GetState();
		check(MasterState.IsValid());
		const int32 MasterSlotCapacity = Bucket.GetTotalSlotCapacity();

		for (const int32 FollowIndex : Rec->FollowRecordIndices)
		{
			check(AnimRecords.IsValidIndex(FollowIndex));
			FInstancedAnimRecord& FollowRec = AnimRecords[FollowIndex];
			check(FollowRec.MasterRecordIndex == Handle.RecordIndex);

		FPrivateUtils::DestroyCpuFollowComponent(FollowRec.CpuFollowSkinnedMesh);
			check(!FollowRec.CpuFollowSkinnedMesh);
			check(!FollowRec.ISKMC);

			// Follow bucket already exists (assigned during AddFollowInstance CPU path or prior GPU path).
			const int32 FollowBucketIndex = FollowRec.BucketIndex;
			checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG CPU->GPU: invalid follow BucketIndex=%d."), FollowBucketIndex);
			FMeshBucket& FollowBucket = Buckets[FollowBucketIndex];
			check(FollowBucket.ISKMC != nullptr);
			check(FollowBucket.TransformProvider != nullptr);
			checkf(FollowBucket.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap,
				TEXT("GIAG CPU->GPU: follower bucket TransformProvider not configured as Follower."));
			check(FollowBucket.TransformProvider->GetMasterState() == MasterState);

			check(MasterBucketSlot >= 0 && MasterBucketSlot < MasterSlotCapacity);
			const FTransform SpawnTransform = Bucket.TransformBySlot[MasterBucketSlot];

			const FPrimitiveInstanceId InstanceId = FollowBucket.ISKMC->AddInstance(SpawnTransform, MasterBucketSlot, true);
			FollowBucket.NumInstances++;

			FollowRec.ISKMC = FollowBucket.ISKMC;
			FollowRec.InstanceId = InstanceId;

			if (FollowBucket.NumMaterialDataFloats > 0)
			{
				const float* SlotBase = FollowBucket.MaterialDataBySlot.GetData() + (int64)MasterBucketSlot * (int64)FollowBucket.NumMaterialDataFloats;
				FollowBucket.ISKMC->SetCustomData(InstanceId, TConstArrayView<float>(SlotBase, FollowBucket.NumMaterialDataFloats));
			}
		}
	}

	FTransform SpawnTransform = Bucket.TransformBySlot[MasterBucketSlot];
	if (USkeletalMeshComponent* Skinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor))
	{
		SpawnTransform = Skinned->GetComponentTransform();
	}
	else
	{
		SpawnTransform = Rec->CpuProxyActor->GetActorTransform();
	}

	// Mark all attachments as GPU-owned again (allow GPU attach compute writes).
	if (Rec->AttachHandles.Num() > 0)
	{
		static constexpr uint32 SkipGpuWriteFlag = 1u; // must match HLSL GIAG_AttachFlag_SkipGpuWrite

		USkeletalMeshComponent* ProxySkinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
		for (const FGameInstancedAnimationAttachHandle& AttachHandle : Rec->AttachHandles)
		{
			if (!AttachHandle)
			{
				continue;
			}

			const uint32 AttachBucketId = AttachHandle.BucketId;
			const uint16 AttachSlot = AttachHandle.Slot;
			const uint16 AttachGen = AttachHandle.Generation;

			const FAttachBucketLocator* Loc = AttachBucketById.Find(AttachBucketId);
			check(Loc);

			const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);
			FAttachSlotTable* Slots = nullptr;
			TArray<FAttachEntry>* Entries = nullptr;
			if (bNativeBucket)
			{
				check(NativeAttachBuckets.IsValidIndex(Loc->Index));
				FNativeAttachBucket& B = NativeAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			else
			{
				check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
				FNiagaraAttachBucket& B = NiagaraAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			check(Slots && Entries);
			check(Slots->OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration[(int32)AttachSlot] == AttachGen);
			const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)AttachSlot];
			check(Entries->IsValidIndex(OutputIndex));

			FAttachEntry& Entry = (*Entries)[OutputIndex];
			Entry.DescFlags &= ~SkipGpuWriteFlag;

			FGIAG_AttachBus::FAttachUpdateOp Op;
			Op.BucketId = AttachBucketId;
			Op.BucketKind = bNativeBucket ? FGIAG_AttachBus::EBucketKind::Native : FGIAG_AttachBus::EBucketKind::Niagara;
			Op.AttachSlot = AttachSlot;
			Op.State = Entry.State;
			Op.Desc.SlotIndex = Entry.SlotIndex;
			Op.Desc.BoneIndex = Entry.BoneIndex;
			Op.Desc.OutputIndex = OutputIndex;
			Op.Desc.Flags = Entry.DescFlags;
			Op.Desc.SocketLocalTRS = Entry.SocketLocalTRS;
			AttachBus->Enqueue_GameThread(MoveTemp(Op));

			{
				const FTransform BoneWS = ProxySkinned->GetSocketTransform(Entry.BoneName, RTS_World);
				const FTransform3f SocketWS = FTransform3f(Entry.SocketLocalTRS) * FTransform3f(BoneWS);
				if (bNativeBucket)
				{
					EnqueueAttachWriteInstance(AttachBucketId, OutputIndex, SocketWS);
				}
				else
				{
					EnqueueAttachWriteFxTransform(AttachBucketId, OutputIndex, SocketWS);
				}
			}

			// If this attachment had a CPU proxy, we killed its GPU particle when entering CPU mode.
			// Now that we're switching back to GPU, request a respawn via the versioned AddList.
			if (!bNativeBucket)
			{
				const bool bHadCpuProxy = (Entry.CpuStaticMeshComponent != nullptr) || (Entry.CpuNiagaraComponent != nullptr);
				if (bHadCpuProxy)
				{
					NiagaraAttach_SpawnGpuParticleBySlot(AttachBucketId, AttachSlot);
				}
			}

			CleanupAttachCpuProxies(AttachHandle);
		}
	}

	UnregisterCpuAttachSyncMasterRecord(Handle.RecordIndex);

	Rec->CpuProxyActor->Destroy();
	Rec->CpuProxyActor = nullptr;

	check(Bucket.ISKMC);
	const FPrimitiveInstanceId InstanceId = Bucket.ISKMC->AddInstance(SpawnTransform, MasterBucketSlot, true);
	Bucket.TransformBySlot[MasterBucketSlot] = SpawnTransform;
	// Slot is (re)entering GPU evaluation; initialize previous=current once to avoid velocity spikes from stale/uninitialized GPU buffer.
	Bucket.NewSlotsThisTick.Add((uint32)MasterBucketSlot);
	if (!Bucket.TransformDirty[MasterBucketSlot])
	{
		Bucket.TransformDirty[MasterBucketSlot] = true;
		Bucket.DirtyTransformSlots.Add((uint32)MasterBucketSlot);
	}

	FPrivateUtils::RemoveSlotFromList(Bucket.CpuAliveSlots, Bucket.CpuAliveListIndexBySlot, MasterBucketSlot);
	FPrivateUtils::AddSlotToList(Bucket.GpuAliveSlots, Bucket.GpuAliveListIndexBySlot, MasterBucketSlot);

	// Ensure GPU AnimLibrary data exists for any clips referenced by this instance.
	// (CPU may have allocated clip indices via RequestClipIndexOnly without baking pixels.)
	const double NowSeconds = GetWorldTimeSeconds();
	FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
	if (Cache && Cache->Skeleton == Group.Skeleton)
	{
		TArray<int32> TmpClips;
		for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
		{
			const FGIAG_AnimCompiledNode& Node = Group.Compiled->Nodes[NodeIdx];
			if (!Node.NodeMeta)
			{
				continue;
			}
			const void* NodePtr = Bucket.GetNodePtr(NodeIdx, MasterBucketSlot);
			TmpClips.Reset();
			Node.NodeMeta->EnumerateClips(NodePtr, TmpClips);
			for (const int32 ClipIndex : TmpClips)
			{
				if (ClipIndex == INDEX_NONE)
				{
					continue;
				}
				if (!Cache->ClipIndexToSequence.IsValidIndex(ClipIndex))
				{
					continue;
				}
				const UAnimSequence* Anim = Cache->ClipIndexToSequence[ClipIndex];
				if (!Anim)
				{
					continue;
				}
				RequestClipBake(Rec->GroupIndex, Anim, NowSeconds);
			}
		}
	}

	Rec->ISKMC = Bucket.ISKMC;
	Rec->InstanceId = InstanceId;

	if (Bucket.NumMaterialDataFloats > 0)
	{
		const float* SlotBase = Bucket.MaterialDataBySlot.GetData() + (int64)MasterBucketSlot * (int64)Bucket.NumMaterialDataFloats;
		Bucket.ISKMC->SetCustomData(InstanceId, TConstArrayView<float>(SlotBase, Bucket.NumMaterialDataFloats));
	}

	if (Bucket.TransformDirty[MasterBucketSlot])
	{
		Bucket.DirtyTransformSlots.Add((uint32)MasterBucketSlot);
	}
}

void UGameInstancedAnimationGraphSubsystem::EnqueueAttachWriteFxTransform(uint32 BucketId, uint32 OutputIndex, const FTransform3f& SocketWS)
{
	check(IsInGameThread());
	if (!AttachBus.IsValid() || BucketId == 0u)
	{
		return;
	}

	FGIAG_AttachBus::FWriteFxTransformOp Op;
	Op.BucketId = BucketId;
	{
		const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
		checkf(Loc && Loc->Type == EAttachBucketType::Niagara, TEXT("GIAG: EnqueueAttachWriteFxTransform called for non-Niagara bucket %u."), BucketId);
	}
	Op.OutputIndex = OutputIndex;
	Op.TransformWS = SocketWS;
	AttachBus->Enqueue_GameThread(MoveTemp(Op));
}

void UGameInstancedAnimationGraphSubsystem::EnqueueAttachHide(uint32 BucketId, uint32 OutputIndex)
{
	check(IsInGameThread());
	if (!AttachBus.IsValid() || BucketId == 0u)
	{
		return;
	}

	FGIAG_AttachBus::FWriteFxTransformOp Op;
	Op.BucketId = BucketId;
	{
		const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
		checkf(Loc && Loc->Type == EAttachBucketType::Niagara, TEXT("GIAG: EnqueueAttachHide called for non-Niagara bucket %u."), BucketId);
	}
	Op.OutputIndex = OutputIndex;
	Op.TransformWS = FTransform3f::Identity;
	Op.TransformWS.SetScale3D(FVector3f::ZeroVector);
	AttachBus->Enqueue_GameThread(MoveTemp(Op));
}

void UGameInstancedAnimationGraphSubsystem::EnqueueAttachWriteInstance(uint32 BucketId, uint32 OutputIndex, const FTransform3f& SocketWS)
{
	check(IsInGameThread());
	if (!AttachBus.IsValid() || BucketId == 0u)
	{
		return;
	}

	FGIAG_AttachBus::FWriteInstanceOp Op;
	Op.BucketId = BucketId;
	Op.OutputIndex = OutputIndex;
	Op.TransformWS = SocketWS;
	AttachBus->Enqueue_GameThread(MoveTemp(Op));
}

void UGameInstancedAnimationGraphSubsystem::EnqueueAttachHideInstance(uint32 BucketId, uint32 OutputIndex)
{
	check(IsInGameThread());
	if (!AttachBus.IsValid() || BucketId == 0u)
	{
		return;
	}

	FGIAG_AttachBus::FWriteInstanceOp Op;
	Op.BucketId = BucketId;
	Op.OutputIndex = OutputIndex;
	Op.TransformWS = FTransform3f::Identity;
	Op.TransformWS.SetScale3D(FVector3f::ZeroVector);
	AttachBus->Enqueue_GameThread(MoveTemp(Op));
}

void UGameInstancedAnimationGraphSubsystem::CleanupAttachCpuProxies(const FGameInstancedAnimationAttachHandle& Handle)
{
	check(IsInGameThread());
	if (!Handle)
	{
		return;
	}
	const uint32 BucketId = Handle.BucketId;
	const uint16 Gen = Handle.Generation;
	const uint16 Slot = Handle.Slot;

	const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
	if (!Loc)
	{
		return;
	}

	FAttachSlotTable* Slots = nullptr;
	TArray<FAttachEntry>* Entries = nullptr;
	if (Loc->Type == EAttachBucketType::Native)
	{
		if (!NativeAttachBuckets.IsValidIndex(Loc->Index))
		{
			return;
		}
		FNativeAttachBucket& B = NativeAttachBuckets[Loc->Index];
		Slots = &B.Slots;
		Entries = &B.Entries;
	}
	else
	{
		if (!NiagaraAttachBuckets.IsValidIndex(Loc->Index))
		{
			return;
		}
		FNiagaraAttachBucket& B = NiagaraAttachBuckets[Loc->Index];
		Slots = &B.Slots;
		Entries = &B.Entries;
	}
	check(Slots && Entries);

	if (!Slots->OutputIndexByAttachSlot.IsValidIndex((int32)Slot)
		|| !Slots->SlotGeneration.IsValidIndex((int32)Slot)
		|| Slots->SlotGeneration[(int32)Slot] != Gen)
	{
		return;
	}
	const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)Slot];
	if (!Entries->IsValidIndex(OutputIndex))
	{
		return;
	}

	FAttachEntry& Entry = (*Entries)[OutputIndex];

	if (Entry.CpuStaticMeshComponent)
	{
		Entry.CpuStaticMeshComponent->DestroyComponent();
		Entry.CpuStaticMeshComponent = nullptr;
	}
	if (Entry.CpuNiagaraComponent)
	{
		Entry.CpuNiagaraComponent->Deactivate();
		Entry.CpuNiagaraComponent->ReleaseToPool();
		Entry.CpuNiagaraComponent = nullptr;
	}
}

void UGameInstancedAnimationGraphSubsystem::TickCpuAttachSync_GameThread()
{
	check(IsInGameThread());

	if (!AttachBus.IsValid())
	{
		return;
	}

	// Master records only; CPU backend only.
	// Performance: only iterate the registered CPU attach-sync working set (no full AnimRecords scan).
	for (const int32 RecordIndex : CpuAttachSyncMasterRecordIndices)
	{
		checkf(AnimRecords.IsValidIndex(RecordIndex), TEXT("GIAG AttachSync: RecordIndex=%d invalid; working set maintenance bug."), RecordIndex);
		FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
		checkf(Rec.MasterRecordIndex == INDEX_NONE, TEXT("GIAG AttachSync: RecordIndex=%d is not master; working set maintenance bug."), RecordIndex);
		checkf(Rec.CpuProxyActor != nullptr, TEXT("GIAG AttachSync: RecordIndex=%d is not CPU backend; working set maintenance bug."), RecordIndex);
		checkf(Rec.AttachHandles.Num() > 0, TEXT("GIAG AttachSync: RecordIndex=%d has no attachments; working set maintenance bug."), RecordIndex);

		checkf(Buckets.IsValidIndex(Rec.BucketIndex), TEXT("GIAG AttachSync: invalid BucketIndex=%d."), Rec.BucketIndex);
		FMeshBucket& MeshBucket = Buckets[Rec.BucketIndex];
		check(MeshBucket.bStorageInitialized);
		checkf(Rec.SlotIndex >= 0 && Rec.SlotIndex < MeshBucket.GetTotalSlotCapacity(), TEXT("GIAG AttachSync: invalid SlotIndex=%d."), Rec.SlotIndex);
		check(MeshBucket.ISKMC != nullptr);
		const int32 BucketSlot = Rec.SlotIndex;

		// Use the CPU pose cache produced earlier in the frame.
		const FGameInstancedAnimationGraphHandle Handle{ RecordIndex, AnimRecordSerials[RecordIndex] };
		uint64 Frame = 0;
		TConstArrayView<FTransform3f> LocalPose;
		if (!TryGetCpuPoseCache_NoLock(Handle, Frame, LocalPose))
		{
			continue;
		}

		TArray<FTransform3f> ComponentPose;
		ConvertLocalPoseToComponentPoseChecked(Handle, LocalPose, ComponentPose);

		const FTransform3f C2W = FTransform3f(MeshBucket.TransformBySlot[BucketSlot]);

		for (const FGameInstancedAnimationAttachHandle& AttachHandle : Rec.AttachHandles)
		{
			check(AttachHandle);

			const uint32 AttachBucketId = AttachHandle.BucketId;
			const uint16 AttachSlot = AttachHandle.Slot;
			const uint16 AttachGen = AttachHandle.Generation;

			const FAttachBucketLocator* Loc = AttachBucketById.Find(AttachBucketId);
			check(Loc);

			const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);
			const FAttachSlotTable* Slots = nullptr;
			const TArray<FAttachEntry>* Entries = nullptr;
			if (bNativeBucket)
			{
				check(NativeAttachBuckets.IsValidIndex(Loc->Index));
				const FNativeAttachBucket& B = NativeAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			else
			{
				check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
				const FNiagaraAttachBucket& B = NiagaraAttachBuckets[Loc->Index];
				Slots = &B.Slots;
				Entries = &B.Entries;
			}
			check(Slots && Entries);

			check(Slots->OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration.IsValidIndex((int32)AttachSlot));
			check(Slots->SlotGeneration[(int32)AttachSlot] == AttachGen);
			const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)AttachSlot];
			check(Entries->IsValidIndex(OutputIndex));

			const FAttachEntry& Entry = (*Entries)[OutputIndex];

			const bool bHasCpuProxy = (Entry.CpuStaticMeshComponent != nullptr) || (Entry.CpuNiagaraComponent != nullptr);
			if (bHasCpuProxy)
			{
				continue;
			}

			const FTransform3f BoneCS = ComponentPose[(int32)Entry.BoneIndex];
			const FTransform3f SocketCS = Entry.SocketLocalTRS * BoneCS;
			const FTransform3f SocketWS = SocketCS * C2W;
			if (bNativeBucket)
			{
				EnqueueAttachWriteInstance(AttachBucketId, (uint32)OutputIndex, SocketWS);
			}
			else
			{
				EnqueueAttachWriteFxTransform(AttachBucketId, (uint32)OutputIndex, SocketWS);
			}
		}
	}
}

bool UGameInstancedAnimationGraphSubsystem::IsInstanceUsingCPUMode(const FGameInstancedAnimationGraphHandle& Handle) const
{
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	return Rec && Rec->CpuProxyActor != nullptr;
}

bool UGameInstancedAnimationGraphSubsystem::DebugResolveAttachOutputIndex(const FGameInstancedAnimationAttachHandle& Handle, int32& OutOutputIndex) const
{
	OutOutputIndex = INDEX_NONE;
	if (!Handle)
	{
		return false;
	}

	// Validate owner identity first (helps tests avoid stale handles).
	if (!AnimRecords.IsValidIndex(Handle.OwnerRecordIndex)
		|| !AnimRecordSerials.IsValidIndex(Handle.OwnerRecordIndex)
		|| AnimRecordSerials[Handle.OwnerRecordIndex] != Handle.OwnerRecordSerialNumber)
	{
		return false;
	}

	const uint32 BucketId = Handle.BucketId;
	const uint16 Slot = Handle.Slot;
	const uint16 Gen = Handle.Generation;

	const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
	if (!Loc)
	{
		return false;
	}

	const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);
	if (bNativeBucket)
	{
		if (!NativeAttachBuckets.IsValidIndex(Loc->Index))
		{
			return false;
		}
		const FNativeAttachBucket& Bucket = NativeAttachBuckets[Loc->Index];
		if (!Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex((int32)Slot)
			|| !Bucket.Slots.SlotGeneration.IsValidIndex((int32)Slot)
			|| Bucket.Slots.SlotGeneration[(int32)Slot] != Gen)
		{
			return false;
		}
		const int32 OutputIndex = Bucket.Slots.OutputIndexByAttachSlot[(int32)Slot];
		if (!Bucket.Entries.IsValidIndex(OutputIndex))
		{
			return false;
		}
		OutOutputIndex = OutputIndex;
		return true;
	}

	if (!NiagaraAttachBuckets.IsValidIndex(Loc->Index))
	{
		return false;
	}
	const FNiagaraAttachBucket& Bucket = NiagaraAttachBuckets[Loc->Index];
	if (!Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex((int32)Slot)
		|| !Bucket.Slots.SlotGeneration.IsValidIndex((int32)Slot)
		|| Bucket.Slots.SlotGeneration[(int32)Slot] != Gen)
	{
		return false;
	}
	const int32 OutputIndex = Bucket.Slots.OutputIndexByAttachSlot[(int32)Slot];
	if (!Bucket.Entries.IsValidIndex(OutputIndex))
	{
		return false;
	}

	OutOutputIndex = OutputIndex;
	return true;
}

bool UGameInstancedAnimationGraphSubsystem::DebugGetAttachBucketEntryCount(int32 BucketId, int32& OutEntryCount) const
{
	check(IsInGameThread());
	OutEntryCount = 0;
	if (BucketId <= 0)
	{
		return false;
	}

	const FAttachBucketLocator* Loc = AttachBucketById.Find((uint32)BucketId);
	if (!Loc)
	{
		return false;
	}
	if (Loc->Type == EAttachBucketType::Native)
	{
		if (!NativeAttachBuckets.IsValidIndex(Loc->Index))
		{
			return false;
		}
		OutEntryCount = NativeAttachBuckets[Loc->Index].Entries.Num();
		return true;
	}

	if (!NiagaraAttachBuckets.IsValidIndex(Loc->Index))
	{
		return false;
	}
	OutEntryCount = NiagaraAttachBuckets[Loc->Index].Entries.Num();
	return true;
}

void UGameInstancedAnimationGraphSubsystem::RemoveInstance(FGameInstancedAnimationGraphHandle& Handle, bool bAllowAutoShrink)
{
	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (Rec == nullptr)
	{
		return;
	}

	// If this record is registered for CPU attach sync, unregister early to avoid stale indices.
	if (Rec->MasterRecordIndex == INDEX_NONE)
	{
		UnregisterCpuAttachSyncMasterRecord(Handle.RecordIndex);
	}

	// Record-owned attachments: always cleanup before tearing down the instance (keeps provider state pointers valid).
	if (Rec->AttachHandles.Num() > 0)
	{
		TArray<FGameInstancedAnimationAttachHandle> Handles = MoveTemp(Rec->AttachHandles);
		Rec->AttachHandles.Reset();
		for (const FGameInstancedAnimationAttachHandle& AttachHandle : Handles)
		{
			if (AttachHandle)
			{
				CleanupAttachCpuProxies(AttachHandle);
				RemoveAttach_Backend(AttachHandle);
			}
		}
	}

	// CPU backend: proxy actor.
	if (Rec->CpuProxyActor != nullptr)
	{
		check(Rec->MasterRecordIndex == INDEX_NONE);

		// Cascade delete follows first (they may own components attached to this proxy actor).
		if (Rec->FollowRecordIndices.Num() > 0)
		{
			const TArray<int32> FollowsCopy = Rec->FollowRecordIndices;
			for (const int32 FollowIndex : FollowsCopy)
			{
				if (!AnimRecords.IsValidIndex(FollowIndex) || !AnimRecordSerials.IsValidIndex(FollowIndex))
				{
					continue;
				}
				FGameInstancedAnimationGraphHandle FollowHandle{ FollowIndex, AnimRecordSerials[FollowIndex] };
				RemoveInstance(FollowHandle);
			}
		}

		const int32 BucketIndex = Rec->BucketIndex;
		const int32 GroupIndex = Rec->GroupIndex;
		const int32 BucketSlot = Rec->SlotIndex;
		AActor* ProxyActor = Rec->CpuProxyActor;

		checkf(Buckets.IsValidIndex(BucketIndex), TEXT("GIAG CPU: invalid BucketIndex=%d."), BucketIndex);
		FMeshBucket& Bucket = Buckets[BucketIndex];
		check(Bucket.bStorageInitialized);
		check(Bucket.ISKMC != nullptr);

		checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG CPU: invalid GroupIndex=%d."), GroupIndex);
		FGraphGroup& Group = Groups[GroupIndex];

		checkf(BucketSlot >= 0 && BucketSlot < Bucket.GetTotalSlotCapacity(), TEXT("GIAG CPU: invalid SlotIndex=%d."), BucketSlot);
		check(Bucket.SlotAlive[BucketSlot]);

		// Destroy per-node AoS data for this slot.
		check(Group.Compiled);
		check(Group.NodeProperties.Num() == Group.Compiled->NumNodes);
		for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
		{
			const FStructProperty* StructProp = Group.NodeProperties[NodeIdx];
			check(StructProp && StructProp->Struct);
			uint8* NodePtr = Bucket.GetNodePtr(NodeIdx, BucketSlot);
			StructProp->Struct->DestroyStruct(NodePtr);
			FMemory::Memzero(NodePtr, (SIZE_T)Bucket.NodeStrideBytes[NodeIdx]);
		}

		Bucket.SlotAlive[BucketSlot] = false;
		Bucket.RecordIndexBySlot[BucketSlot] = INDEX_NONE;
		Bucket.FreeSlot(BucketSlot);
		Bucket.TransformBySlot[BucketSlot] = FTransform::Identity;

		FPrivateUtils::RemoveSlotFromList(Bucket.CpuAliveSlots, Bucket.CpuAliveListIndexBySlot, BucketSlot);

		// If proxy actor is external, we do NOT destroy it.
		if (Rec->bExternalCpuProxyActor)
		{
			check(ProxyActor->GetClass()->ImplementsInterface(UGIAG_ActorInterface::StaticClass()));
			IGIAG_ActorInterface::Execute_SetInstancedAnimationGraphHandle(ProxyActor, FGameInstancedAnimationGraphHandle{});
		}
		else
		{
			ProxyActor->Destroy();
		}
		ProxyActor = nullptr;
		Bucket.NumInstances = FMath::Max(0, Bucket.NumInstances - 1);

		if (bAllowAutoShrink)
		{
			Bucket.bPendingShrinkEvaluation = true;
		}

		const int32 RecordIndex = Handle.RecordIndex;
		CpuPoseCacheByRecordIndex.Remove(RecordIndex);
		AnimRecords.RemoveAt(RecordIndex);
		InvalidateHandle(Handle);

		CleanupBucketIfEmpty(BucketIndex);
		return;
	}

	// Follow instance: only remove the render instance; shared animation instance stays alive with master.
	if (Rec->MasterRecordIndex != INDEX_NONE)
	{
		const int32 FollowBucketIndex = Rec->BucketIndex;

		const int32 MasterIndex = Rec->MasterRecordIndex;

		if (AnimRecords.IsValidIndex(MasterIndex))
		{
			AnimRecords[MasterIndex].FollowRecordIndices.RemoveSingleSwap(Handle.RecordIndex, EAllowShrinking::No);
		}

		// CPU follow: destroy dynamic follower component.
		if (Rec->CpuFollowSkinnedMesh)
		{
			FPrivateUtils::DestroyCpuFollowComponent(Rec->CpuFollowSkinnedMesh);

			const int32 RecordIndex = Handle.RecordIndex;
			AnimRecords.RemoveAt(RecordIndex);
			InvalidateHandle(Handle);
			return;
		}

		checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), FollowBucketIndex);
		checkf(Rec->ISKMC != nullptr, TEXT("GIAG: invalid ISKMC for record (BucketIndex=%d)."), FollowBucketIndex);

		Rec->ISKMC->RemoveInstance(Rec->InstanceId);
		{
			FMeshBucket& Bucket = Buckets[FollowBucketIndex];
			Bucket.NumInstances = FMath::Max(0, Bucket.NumInstances - 1);
		}

		const int32 RecordIndex = Handle.RecordIndex;
		AnimRecords.RemoveAt(RecordIndex);
		InvalidateHandle(Handle);

		CleanupBucketIfEmpty(FollowBucketIndex);
		return;
	}

	// Master instance: cascade delete follows first.
	if (Rec->FollowRecordIndices.Num() > 0)
	{
		const TArray<int32> FollowsCopy = Rec->FollowRecordIndices;
		for (const int32 FollowIndex : FollowsCopy)
		{
			if (!AnimRecords.IsValidIndex(FollowIndex) || !AnimRecordSerials.IsValidIndex(FollowIndex))
			{
				continue;
			}
			FGameInstancedAnimationGraphHandle FollowHandle{ FollowIndex, AnimRecordSerials[FollowIndex] };
			RemoveInstance(FollowHandle);
		}
	}

	const int32 BucketIndex = Rec->BucketIndex;
	checkf(Buckets.IsValidIndex(BucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), BucketIndex);
	FMeshBucket& Bucket = Buckets[BucketIndex];

	const int32 GroupIndex = Rec->GroupIndex;
	const int32 BucketSlot = Rec->SlotIndex;

	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	FGraphGroup& Group = Groups[GroupIndex];
	check(Bucket.bStorageInitialized);
	check(Bucket.ISKMC != nullptr);
	checkf(Bucket.SlotAlive.Num() == Bucket.GetTotalSlotCapacity() && Bucket.RecordIndexBySlot.Num() == Bucket.GetTotalSlotCapacity(),
		TEXT("GIAG: bucket slot arrays size mismatch (SlotAlive=%d RecordIndexBySlot=%d Cap=%d)."),
		Bucket.SlotAlive.Num(), Bucket.RecordIndexBySlot.Num(), Bucket.GetTotalSlotCapacity());
	checkf(BucketSlot >= 0 && BucketSlot < Bucket.GetTotalSlotCapacity(), TEXT("GIAG: invalid SlotIndex=%d."), BucketSlot);
	check(Bucket.SlotAlive[BucketSlot]);

	// Destroy per-node AoS data for this slot.
	check(Group.Compiled);
	check(Group.NodeProperties.Num() == Group.Compiled->NumNodes);
	for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
	{
		const FStructProperty* StructProp = Group.NodeProperties[NodeIdx];
		check(StructProp && StructProp->Struct);
		uint8* NodePtr = Bucket.GetNodePtr(NodeIdx, BucketSlot);
		StructProp->Struct->DestroyStruct(NodePtr);
		FMemory::Memzero(NodePtr, (SIZE_T)Bucket.NodeStrideBytes[NodeIdx]);
	}

	Bucket.SlotAlive[BucketSlot] = false;
	Bucket.RecordIndexBySlot[BucketSlot] = INDEX_NONE;
	Bucket.FreeSlot(BucketSlot);
	FPrivateUtils::RemoveSlotFromList(Bucket.GpuAliveSlots, Bucket.GpuAliveListIndexBySlot, BucketSlot);

	Bucket.TransformBySlot[BucketSlot] = FTransform::Identity;
	if (Bucket.TransformDirty[BucketSlot])
	{
		Bucket.TransformDirty[BucketSlot] = false;
		Bucket.DirtyTransformSlots.RemoveSingleSwap((uint32)BucketSlot, EAllowShrinking::No);
	}

	check(Rec->ISKMC != nullptr);
	Rec->ISKMC->RemoveInstance(Rec->InstanceId);
	Bucket.NumInstances = FMath::Max(0, Bucket.NumInstances - 1);

	if (bAllowAutoShrink)
	{
		Bucket.bPendingShrinkEvaluation = true;
	}

	const int32 RecordIndex = Handle.RecordIndex;
	AnimRecords.RemoveAt(RecordIndex);
	InvalidateHandle(Handle);

	CleanupBucketIfEmpty(BucketIndex);
}

void UGameInstancedAnimationGraphSubsystem::SetInstanceTransform(const FGameInstancedAnimationGraphHandle& Handle, const FTransform& NewTransform, bool bTeleport)
{
	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (Rec == nullptr)
	{
		return;
	}

	// Follow transform is driven by master.
	if (!ensure(Rec->MasterRecordIndex == INDEX_NONE))
	{
		return;
	}

	FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	check(Bucket.bStorageInitialized);
	const int32 BucketSlot = Rec->SlotIndex;
	if (FMemory::Memcmp(&Bucket.TransformBySlot[BucketSlot], &NewTransform, sizeof(FTransform)) == 0)
	{
		return;
	}

	// CPU backend.
	if (Rec->CpuProxyActor)
	{
		Rec->CpuProxyActor->SetActorTransform(
			NewTransform,
			false,
			nullptr,
			bTeleport ? ETeleportType::TeleportPhysics : ETeleportType::None);

		// Keep slot transform in sync for culling and backend switching.
		Bucket.TransformBySlot[BucketSlot] = NewTransform;
		Bucket.TransformDirty[BucketSlot] = true;
		return;
	}

	// Prefer in-place instance transform update (keeps FPrimitiveInstanceId stable); fallback to remove+add if unavailable.
	if (Rec->ISKMC)
	{
		FPrivateUtils::UpdateISKMCInstanceTransformById(*Rec->ISKMC, Rec->InstanceId, NewTransform, true);

		checkf(BucketSlot >= 0 && BucketSlot < Bucket.GetTotalSlotCapacity(), TEXT("GIAG: invalid SlotIndex=%d."), BucketSlot);

		Bucket.TransformBySlot[BucketSlot] = NewTransform;
		if (!Bucket.TransformDirty[BucketSlot])
		{
			Bucket.TransformDirty[BucketSlot] = true;
			Bucket.DirtyTransformSlots.Add((uint32)BucketSlot);
		}
	}

	// Drive follows.
	for (const int32 FollowIdx : Rec->FollowRecordIndices)
	{
		check(AnimRecords.IsValidIndex(FollowIdx));
		FInstancedAnimRecord& Follow = AnimRecords[FollowIdx];
		if (Follow.ISKMC)
		{
			FPrivateUtils::UpdateISKMCInstanceTransformById(*Follow.ISKMC, Follow.InstanceId, NewTransform, true);
		}
	}
}

FTransform UGameInstancedAnimationGraphSubsystem::GetInstanceTransform(const FGameInstancedAnimationGraphHandle& Handle) const
{
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (Rec == nullptr)
	{
		return FTransform::Identity;
	}
	auto GetTransformByRecord = [this](const FInstancedAnimRecord& Rec)
	{
		auto& Bucket = Buckets[Rec.BucketIndex];
		check(Bucket.bStorageInitialized);
		return Bucket.TransformBySlot[Rec.SlotIndex];
	};
	if (Rec->MasterRecordIndex != INDEX_NONE)
	{
		check(AnimRecords.IsValidIndex(Rec->MasterRecordIndex));
		return GetTransformByRecord(AnimRecords[Rec->MasterRecordIndex]);
	}
	return GetTransformByRecord(*Rec);
}

void UGameInstancedAnimationGraphSubsystem::PlayAnimation(const FGameInstancedAnimationGraphHandle& Handle, const UAnimSequence* AnimSequence, FName NodeName, float BlendDurationSeconds, float StartSeconds, bool bLoop, float PlayRate)
{
	if (AnimSequence == nullptr)
	{
		return;
	}
	auto NodePtr = FindAnimNode<FGIAG_AnimNodeBase>(Handle, NodeName);
	if (!NodePtr)
	{
		return;
	}

	const int32 NodeIdx = NodePtr.NodeIndex;
	FGraphGroup& Group = Groups[NodePtr.GroupIndex];
	check(Group.Compiled);
	const IGIAG_AnimNodeMeta* TargetMeta = Group.Compiled->Nodes[NodeIdx].NodeMeta;
	check(TargetMeta);
	TargetMeta->PlayAnimation(NodePtr.NodePtr, NodePtr, AnimSequence, BlendDurationSeconds, StartSeconds, bLoop, PlayRate);
}

void UGameInstancedAnimationGraphSubsystem::CleanupUnusedAnimations(float OlderThanSeconds)
{
	if (OlderThanSeconds <= 0.0f)
	{
		return;
	}
	const double NowSeconds = GetWorldTimeSeconds();
	const double Threshold = NowSeconds - (double)OlderThanSeconds;

	// Build referenced clip sets per skeleton cache index by scanning live instance node data.
	TMap<int32, TSet<int32>> ReferencedClipsByCacheIndex;
	TArray<int32> TmpClips;

	for (auto GroupIt = Groups.CreateConstIterator(); GroupIt; ++GroupIt)
	{
		const int32 GroupIndex = GroupIt.GetIndex();
		const FGraphGroup& Group = *GroupIt;
		if (!Group.Compiled || Group.SkeletonCacheIndex == INDEX_NONE)
		{
			continue;
		}

		TSet<int32>& Referenced = ReferencedClipsByCacheIndex.FindOrAdd(Group.SkeletonCacheIndex);

		for (const int32 BucketIndex : Group.BucketIndices)
		{
			const FMeshBucket& Bucket = Buckets[BucketIndex];
			check(Bucket.GroupIndex == GroupIndex);
			if (!Bucket.bStorageInitialized)
			{
				continue;
			}

			const int32 TotalCap = Bucket.GetTotalSlotCapacity();
			for (int32 SlotIdx = 0; SlotIdx < TotalCap; ++SlotIdx)
			{
				if (!Bucket.SlotAlive.IsValidIndex(SlotIdx) || !Bucket.SlotAlive[SlotIdx])
				{
					continue;
				}
				for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
				{
					const FGIAG_AnimCompiledNode& Node = Group.Compiled->Nodes[NodeIdx];
					if (!Node.NodeMeta)
					{
						continue;
					}
					const void* NodePtr = Bucket.GetNodePtr(NodeIdx, SlotIdx);
					TmpClips.Reset();
					Node.NodeMeta->EnumerateClips(NodePtr, TmpClips);
					for (int32 ClipIndex : TmpClips)
					{
						if (ClipIndex != INDEX_NONE)
						{
							Referenced.Add(ClipIndex);
						}
					}
				}
			}
		}
	}

	// NOTE: allocator free list is not used by the repack path; we always rebuild a compact layout.

	// For each cache: evict non-hot clips, then repack remaining clips into a compact AnimTRS buffer.
	for (const TPair<int32, TSet<int32>>& Pair : ReferencedClipsByCacheIndex)
	{
		const int32 CacheIndex = Pair.Key;
		const TSet<int32>& Referenced = Pair.Value;
		FSkeletonAnimCache* Cache = GetSkeletonCache(CacheIndex);
		if (!Cache)
		{
			continue;
		}

		bool bAnyChange = false;

		TBitArray<> bEvict;
		bEvict.SetNum(Cache->ClipSlots.Num(), false);

		// Decide evictions first (do NOT touch referenced clips).
		for (int32 ClipIndex = 0; ClipIndex < Cache->ClipSlots.Num(); ++ClipIndex)
		{
			FSkeletonAnimCache::FClipSlot& Slot = Cache->ClipSlots[ClipIndex];
			if (!Slot.bAllocated || Slot.bEvicted)
			{
				continue;
			}
			if (Slot.LastRequestedTimeSeconds > Threshold)
			{
				continue;
			}
			if (Referenced.Contains(ClipIndex))
			{
				continue;
			}
			bEvict[ClipIndex] = true;
			bAnyChange = true;

			// Cancel in-flight bake (unblock RT waiters).
			if (Slot.Bake.IsValid())
			{
				Slot.Bake->bCancelled.Store(true);
				if (Slot.Bake->CompletionEvent.IsValid())
				{
					Slot.Bake->CompletionEvent->Trigger();
				}
			}
		}

		if (!bAnyChange)
		{
			continue;
		}

		// Build repack upload.
		TSharedRef<FGIAG_AnimLibraryUploadData> Upload = MakeShared<FGIAG_AnimLibraryUploadData>();
		Upload->NumClips = (uint32)FMath::Max(1, Cache->ClipSlots.Num());
		Upload->NumBones = (uint32)FMath::Max(0, Cache->NumBones);
		Upload->bRepack = true;

		// New packed layout: assign new start offsets to all surviving clips.
		uint32 NewCursor = 0;
		for (int32 ClipIndex = 0; ClipIndex < Cache->ClipSlots.Num(); ++ClipIndex)
		{
			FSkeletonAnimCache::FClipSlot& Slot = Cache->ClipSlots[ClipIndex];
			if (!Slot.bAllocated || Slot.bEvicted)
			{
				continue;
			}

			if (bEvict[ClipIndex])
			{
				// Remove mapping (linear search; cleanup is infrequent).
				for (auto It = Cache->SequenceToClipIndex.CreateIterator(); It; ++It)
				{
					if (It.Value() == ClipIndex)
					{
						It.RemoveCurrent();
						break;
					}
				}
				if (Cache->ClipIndexToSequence.IsValidIndex(ClipIndex))
				{
					Cache->ClipIndexToSequence[ClipIndex] = nullptr;
				}

				// Invalidate meta (must be uploaded even if we immediately reuse this clip index later).
				Slot.Meta.StartTransformIndex = -1;
				Slot.Meta.NumFrames = 0;
				Slot.Meta.SecondsPerFrame = 0.0f;
				Slot.Meta.SequenceLengthSeconds = 0.0f;

				FGIAG_AnimLibraryClipMetaUpdate M;
				M.ClipIndex = (uint32)ClipIndex;
				M.Meta = Slot.Meta;
				Upload->ClipMetaUpdates.Add(M);

				Slot.bMetaDirty = false;
				Slot.bTRSDirty = false;
				Slot.StartTransformIndex = INDEX_NONE;
				Slot.NumTransforms = 0;
				Slot.Bake.Reset();

				Slot.bEvicted = true;
				Slot.bAllocated = false;
				Cache->FreeClipIndices.Add(ClipIndex);
				continue;
			}

			// Surviving clip: move to packed offset.
			const uint32 OldStart = (uint32)FMath::Max(0, Slot.StartTransformIndex);
			const uint32 NumTransforms = (uint32)FMath::Max(0, Slot.NumTransforms);
			const uint32 NewStart = NewCursor;
			NewCursor += NumTransforms;

			// If TRS not yet uploaded (Bake still present), upload into new location instead of copying old buffer.
			if (Slot.Bake.IsValid())
			{
				FGIAG_AnimLibraryAnimTRSUpdate P;
				P.StartTransformIndex = NewStart;
				P.NumTransforms = NumTransforms;
				P.TRS = Slot.Bake->TRS;
				P.CompletionEvent = Slot.Bake->CompletionEvent;
				Upload->AnimTRSUpdates.Add(MoveTemp(P));
				Slot.Bake.Reset();
			}
			else
			{
				FGIAG_AnimLibraryRepackCopyOp Op;
				Op.SrcStartTransformIndex = OldStart;
				Op.DstStartTransformIndex = NewStart;
				Op.NumTransforms = NumTransforms;
				Upload->RepackCopyOps.Add(Op);
			}

			Slot.StartTransformIndex = (int32)NewStart;
			Slot.Meta.StartTransformIndex = (int32)NewStart;

			FGIAG_AnimLibraryClipMetaUpdate M;
			M.ClipIndex = (uint32)ClipIndex;
			M.Meta = Slot.Meta;
			Upload->ClipMetaUpdates.Add(M);
		}

		Upload->AnimTRSCapacity = FMath::Max<uint32>(1u, NewCursor);
		Upload->Version = ++Cache->AnimLibraryVersion;

		// Optional one-time (or rebuild) RefPose upload piggy-backed on repack.
		if (!Cache->bRefPoseUploadSent)
		{
			Upload->RefPoseVersion = Cache->RefPoseVersion;
			Upload->RefPoseLocalTRS = Cache->RefPoseLocalTRS;
			Cache->bRefPoseUploadSent = true;
		}

		// After repack, allocator state is compact (no holes).
		Cache->AnimTRSCapacity = (int32)Upload->AnimTRSCapacity;
		Cache->FreeTransformBlocks.Reset();

		Cache->PendingAnimLibraryUpload = Upload;
		Cache->bWarnedAnimLibraryUnavailable = false;
	}
}

FGameInstancedAnimationGraphHandle UGameInstancedAnimationGraphSubsystem::AddFollowInstance(const FGameInstancedAnimationGraphHandle& MasterHandle, USkeletalMesh* SkeletalMesh)
{
	FGameInstancedAnimationGraphHandle Handle;

	if (SkeletalMesh == nullptr)
	{
		return Handle;
	}
#if WITH_EDITOR
	if (!ensureMsgf(SkeletalMesh->IsNaniteEnabled(), TEXT("'%s' must have Nanite enabled, otherwise it will not enter the skinning TransformProvider path."), *GetNameSafe(SkeletalMesh)))
	{
		return Handle;
	}
#endif

	FInstancedAnimRecord* MasterRec = ResolveRecord(MasterHandle);
	if (!MasterRec || MasterRec->MasterRecordIndex != INDEX_NONE)
	{
		return Handle;
	}
	if (MasterRec->CpuProxyActor)
	{
		USkeletalMeshComponent* Leader = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(MasterRec->CpuProxyActor);
		check(Leader);

		checkf(Groups.IsValidIndex(MasterRec->GroupIndex), TEXT("GIAG: invalid master GroupIndex=%d (CPU follow)."), MasterRec->GroupIndex);
		FGraphGroup& CpuGroup = Groups[MasterRec->GroupIndex];
		check(CpuGroup.Skeleton);
		checkf(Buckets.IsValidIndex(MasterRec->BucketIndex), TEXT("GIAG: invalid master BucketIndex=%d (CPU follow)."), MasterRec->BucketIndex);
		FMeshBucket& MasterBucket = Buckets[MasterRec->BucketIndex];

		TRefCountPtr<FGIAG_TransformProviderState> CpuMasterState = MasterBucket.TransformProvider->GetState();
		check(CpuMasterState.IsValid());

		const FReferenceSkeleton& CpuDstRef = SkeletalMesh->GetRefSkeleton();
		const int32 CpuDstNumBones = CpuDstRef.GetNum();
		const FReferenceSkeleton& CpuSrcRef = CpuGroup.Skeleton->GetReferenceSkeleton();
		const int32 CpuSrcBones = CpuSrcRef.GetNum();

		TSharedPtr<const TArray<uint32>> CpuRemapShared;
		{
			const FFollowBoneRemapKey Key{ SkeletalMesh, CpuGroup.Skeleton };
			if (TSharedPtr<const TArray<uint32>>* Found = FollowBoneRemapCache.Find(Key))
			{
				CpuRemapShared = *Found;
			}
			else
			{
				TSharedRef<TArray<uint32>> NewRemap = MakeShared<TArray<uint32>>();
				NewRemap->SetNum(CpuDstNumBones);
				for (int32 i = 0; i < CpuDstNumBones; ++i)
				{
					const FName BoneName = CpuDstRef.GetBoneName(i);
					const int32 SrcIdx = CpuSrcRef.FindBoneIndex(BoneName);
					(*NewRemap)[i] = (SrcIdx >= 0) ? (uint32)SrcIdx : 0xFFFFFFFFu;
				}
				bool bIdentity = (CpuDstNumBones == CpuSrcBones);
				if (bIdentity)
				{
					for (int32 i = 0; i < CpuDstNumBones; ++i)
					{
						if ((*NewRemap)[i] != (uint32)i) { bIdentity = false; break; }
					}
				}
				if (bIdentity)
				{
					FollowBoneRemapCache.Add(Key, TSharedPtr<const TArray<uint32>>());
				}
				else
				{
					CpuRemapShared = NewRemap;
					FollowBoneRemapCache.Add(Key, NewRemap);
				}
			}
		}

		const int32 CpuFollowBucketIndex = FindOrCreateFollowerBucket(SkeletalMesh, MasterRec->GroupIndex,
			CpuMasterState, CpuDstNumBones, CpuSrcBones, CpuRemapShared);
		checkf(Buckets.IsValidIndex(CpuFollowBucketIndex), TEXT("GIAG: invalid CPU follow BucketIndex=%d."), CpuFollowBucketIndex);

		FInstancedAnimRecord NewRec;
		NewRec.SkeletalMesh = SkeletalMesh;
		NewRec.CpuFollowSkinnedMesh = FPrivateUtils::CreateCpuFollowComponent(MasterRec->CpuProxyActor, Leader, SkeletalMesh);
		NewRec.GroupIndex = MasterRec->GroupIndex;
		NewRec.SlotIndex = MasterRec->SlotIndex;
		NewRec.BucketIndex = CpuFollowBucketIndex;
		NewRec.MasterRecordIndex = MasterHandle.RecordIndex;

		const int32 RecordIndex = AnimRecords.Add(MoveTemp(NewRec));
		if (AnimRecordSerials.Num() <= RecordIndex)
		{
			AnimRecordSerials.SetNumZeroed(RecordIndex + 1);
		}
		int32& Serial = AnimRecordSerials[RecordIndex];
		Serial = FMath::Max(1, Serial + 1);

		MasterRec = ResolveRecord(MasterHandle);
		check(MasterRec);
		MasterRec->FollowRecordIndices.Add(RecordIndex);

		Handle.RecordIndex = RecordIndex;
		Handle.SerialNumber = Serial;
		return Handle;
	}
	checkf(Groups.IsValidIndex(MasterRec->GroupIndex), TEXT("GIAG: invalid master GroupIndex=%d."), MasterRec->GroupIndex);
	FGraphGroup& Group = Groups[MasterRec->GroupIndex];
	check(Group.Skeleton);
	if (Group.Skeleton == nullptr || SkeletalMesh == nullptr || SkeletalMesh->GetSkeleton() == nullptr)
	{
		return Handle;
	}
	checkf(Buckets.IsValidIndex(MasterRec->BucketIndex), TEXT("GIAG: invalid master BucketIndex=%d."), MasterRec->BucketIndex);
	FMeshBucket& Bucket = Buckets[MasterRec->BucketIndex];
	check(Bucket.bStorageInitialized);
	check(Bucket.ISKMC != nullptr);
	check(Bucket.TransformProvider != nullptr);
	const int32 MasterSlotCapacity = Bucket.GetTotalSlotCapacity();

	TRefCountPtr<FGIAG_TransformProviderState> MasterState = Bucket.TransformProvider->GetState();
	if (!MasterState.IsValid())
	{
		return Handle;
	}

	const FReferenceSkeleton& DstRef = SkeletalMesh->GetRefSkeleton();
	const int32 DstNumBones = DstRef.GetNum();
	const FReferenceSkeleton& SrcRef = Group.Skeleton->GetReferenceSkeleton();
	const int32 SrcBones = SrcRef.GetNum();

	// Optional remap: share a cached table across all follows of (DstMesh, SrcSkeleton).
	TSharedPtr<const TArray<uint32>> RemapShared;
	const uint32* RemapPtr = nullptr;
	{
		const FFollowBoneRemapKey Key{ SkeletalMesh, Group.Skeleton };
		if (TSharedPtr<const TArray<uint32>>* Found = FollowBoneRemapCache.Find(Key))
		{
			RemapShared = *Found;
			RemapPtr = RemapShared.IsValid() ? RemapShared->GetData() : nullptr;
		}
		else
		{
			TSharedRef<TArray<uint32>> NewRemap = MakeShared<TArray<uint32>>();
			NewRemap->SetNum(DstNumBones);
			for (int32 i = 0; i < DstNumBones; ++i)
			{
				const FName BoneName = DstRef.GetBoneName(i);
				const int32 SrcIdx = SrcRef.FindBoneIndex(BoneName);
				(*NewRemap)[i] = (SrcIdx >= 0) ? (uint32)SrcIdx : 0xFFFFFFFFu;
			}

			bool bIdentity = (DstNumBones == SrcBones);
			if (bIdentity)
			{
				for (int32 i = 0; i < DstNumBones; ++i)
				{
					if ((*NewRemap)[i] != (uint32)i)
					{
						bIdentity = false;
						break;
					}
				}
			}

			if (bIdentity)
			{
				// Cache identity as an invalid (null) shared ptr so we skip remap uploads in the RT follow path.
				FollowBoneRemapCache.Add(Key, TSharedPtr<const TArray<uint32>>());
			}
			else
			{
				RemapShared = NewRemap;
				RemapPtr = NewRemap->GetData();
				FollowBoneRemapCache.Add(Key, NewRemap);
			}
		}
	}

	const int32 MasterBucketSlot = MasterRec->SlotIndex;
	checkf(MasterBucketSlot >= 0 && MasterBucketSlot < MasterSlotCapacity, TEXT("GIAG: invalid master SlotIndex=%d for follow."), MasterBucketSlot);
	const FTransform SpawnTransform = Bucket.TransformBySlot[MasterBucketSlot];

	const int32 FollowBucketIndex = FindOrCreateFollowerBucket(SkeletalMesh, MasterRec->GroupIndex,
		MasterState, DstNumBones, SrcBones, RemapShared);
	checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG: invalid follow BucketIndex=%d."), FollowBucketIndex);
	FMeshBucket& FollowBucket = Buckets[FollowBucketIndex];

	checkf(FollowBucket.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap,
		TEXT("GIAG: follower bucket TransformProvider not in Follower mode."));
	check(FollowBucket.TransformProvider->GetMasterState() == MasterState);
	check(FollowBucket.TransformProvider->GetNumBones() == DstNumBones);
	check(FollowBucket.TransformProvider->GetSrcNumBones() == SrcBones);
	check(FollowBucket.TransformProvider->GetBoneRemapPtr() == RemapPtr);

	// Spawn follow instance at master's transform; ISKMC animation index = absolute slot (mirrors master).
	const FPrimitiveInstanceId InstanceId = FollowBucket.ISKMC->AddInstance(SpawnTransform, MasterBucketSlot, true);
	FollowBucket.NumInstances++;

	FInstancedAnimRecord NewRec;
	NewRec.SkeletalMesh = SkeletalMesh;
	NewRec.ISKMC = FollowBucket.ISKMC;
	NewRec.InstanceId = InstanceId;
	NewRec.BucketIndex = FollowBucketIndex;
	NewRec.GroupIndex = MasterRec->GroupIndex;
	NewRec.SlotIndex = MasterBucketSlot;
	NewRec.MasterRecordIndex = MasterHandle.RecordIndex;

	const int32 RecordIndex = AnimRecords.Add(MoveTemp(NewRec));
	if (AnimRecordSerials.Num() <= RecordIndex)
	{
		AnimRecordSerials.SetNumZeroed(RecordIndex + 1);
	}
	int32& Serial = AnimRecordSerials[RecordIndex];
	Serial = FMath::Max(1, Serial + 1);

	// Link back to master.
	MasterRec = ResolveRecord(MasterHandle);
	if (MasterRec)
	{
		MasterRec->FollowRecordIndices.Add(RecordIndex);
	}

	Handle.RecordIndex = RecordIndex;
	Handle.SerialNumber = Serial;
	return Handle;
}

void UGameInstancedAnimationGraphSubsystem::Tick(float DeltaTime)
{
	const bool bDoGPU = !GIAG::IsNullRHI();

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UGameInstancedAnimationGraphSubsystem::Tick"), STAT_UGameInstancedAnimSubsystem_Tick, STATGROUP_GameInstancedAnim);

	UWorld* World = GetWorld();
	TimeSlots[0] = GetWorld()->GetTimeSeconds();

	FlushNiagaraAttachBuckets_GameThread();

	// CPU backend attach sync: when master switches to CPU mode, keep Niagara attach outputs updated via RT writes.
	if (bDoGPU)
	{
		TickCpuAttachSync_GameThread();
	}

	// ActiveIndices is always-on. This CVar only controls whether we apply frustum filtering to build ActiveIndices.
	// When frustum filtering reduces work, we preserve existing output for non-written slots (freeze_pose).
	TOptional<FConvexVolume> ViewFrustum;
	bool bEnableFrustumCull = CVar_InstancedAnimEnableCull.GetValueOnGameThread();
	if (bEnableFrustumCull && World)
	{
		if (ULocalPlayer* LocalPlayer = World->GetFirstLocalPlayerFromController())
		{
			if (UGameViewportClient* ViewportClient = LocalPlayer->ViewportClient)
			{
				FSceneViewProjectionData ProjectionData;
				if (LocalPlayer->GetProjectionData(ViewportClient->Viewport, ProjectionData))
				{
					GetViewFrustumBounds(ViewFrustum.Emplace(), ProjectionData.ComputeViewProjectionMatrix(), false);
				}
			}
		}
#if WITH_EDITOR
		// In SIE, frustum culling should follow the active debug viewport camera.
		if (UGameViewportClient* GameViewport = World->GetGameViewport())
		{
			if (GameViewport->IsSimulateInEditorViewport())
			{
				if (FLevelEditorViewportClient* LevelViewportClient = GCurrentLevelEditingViewportClient)
				{
					if (LevelViewportClient->IsPerspective() && LevelViewportClient->Viewport)
					{
						if (UWorld* ViewportWorld = LevelViewportClient->GetWorld())
						{
							if (ViewportWorld == World && ViewportWorld->Scene)
							{
								FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
									LevelViewportClient->Viewport,
									ViewportWorld->Scene,
									LevelViewportClient->EngineShowFlags).SetRealtimeUpdate(true));
								if (FSceneView* SceneView = LevelViewportClient->CalcSceneView(&ViewFamily))
								{
									GetViewFrustumBounds(ViewFrustum.Emplace(), SceneView->ViewMatrices.GetWorldToClip(), false);
								}
							}
						}
					}
				}
			}
		}
#endif
	}
	bEnableFrustumCull = ViewFrustum.IsSet();
	const float CullPadding = FMath::Max(0.0f, FrustumCullPadding);
	const float MinRadius = FMath::Max(0.0f, FrustumCullMinRadius);

	const bool bDebugBuckets = (CVar_InstancedAnimDebugBuckets.GetValueOnGameThread() > 0);

	for (auto GroupIt = Groups.CreateIterator(); GroupIt; ++GroupIt)
	{
		const int32 GroupIndex = GroupIt.GetIndex();
		FGraphGroup& Group = *GroupIt;
		check(Group.Skeleton);
		check(Group.Compiled);
		FSkeletonAnimCache* Cache = GetSkeletonCache(Group.SkeletonCacheIndex);
		check(Cache);

		// Ensure skeleton static buffers exist (ParentIndices/InverseRefPose).
		bool bSkeletonStaticRebuilt = false;
		if (Cache->ParentIndices.Num() != Group.NumBones || Cache->InverseRefPoseTRS.Num() != Group.NumBones)
		{
			Cache->ParentIndices.Reset();
			Cache->InverseRefPoseTRS.Reset();
			Cache->NumBones = 0;
			GetOrBuildSkeletonStaticData(Group.Skeleton, Cache->ParentIndices, Cache->InverseRefPoseTRS, Cache->NumBones);
			bSkeletonStaticRebuilt = true;
		}
		if (bSkeletonStaticRebuilt)
		{
			BuildRefPoseLocalCache(Group.Skeleton, Group.NumBones, Cache->RefPoseLocal, Cache->RefPoseLocalTRS);
			Cache->RefPoseVersion = FMath::Max<uint32>(1u, Cache->RefPoseVersion + 1u);
			Cache->bRefPoseUploadSent = false;
		}

		if (bDoGPU)
		{
			// Upload AnimLibrary resources (ClipMetas/AnimTRS) incrementally when dirty (per-skeleton shared cache).
			{
				Cache->AnimLibraryBuffers.NumBones = (uint32)Group.NumBones;

				const bool bNeedRefPoseUpload = !Cache->bRefPoseUploadSent;

				bool bAnyMetaDirty = false;
				bool bAnyTRSDirty = false;
				for (const FSkeletonAnimCache::FClipSlot& Slot : Cache->ClipSlots)
				{
					bAnyMetaDirty |= Slot.bMetaDirty;
					bAnyTRSDirty |= Slot.bTRSDirty;
					if (bAnyMetaDirty && bAnyTRSDirty)
					{
						break;
					}
				}

				const bool bNeedAnimLibraryUpload = (bAnyMetaDirty || bAnyTRSDirty || bNeedRefPoseUpload);
				if (bNeedAnimLibraryUpload && Group.NumBones > 0)
				{
					TSharedRef<FGIAG_AnimLibraryUploadData> Upload = MakeShared<FGIAG_AnimLibraryUploadData>();
					Upload->NumClips = (uint32)Cache->ClipSlots.Num();
					Upload->AnimTRSCapacity = (uint32)Cache->AnimTRSCapacity;
					Upload->NumBones = (uint32)Group.NumBones;
					Upload->Version = ++Cache->AnimLibraryVersion;

					// Optional one-time (or rebuild) RefPose upload.
					if (bNeedRefPoseUpload)
					{
						Upload->RefPoseVersion = Cache->RefPoseVersion;
						Upload->RefPoseLocalTRS = Cache->RefPoseLocalTRS;
						Cache->bRefPoseUploadSent = true;
					}

					// Collect dirty updates, then clear dirty flags.
					for (int32 ClipIndex = 0; ClipIndex < Cache->ClipSlots.Num(); ++ClipIndex)
					{
						FSkeletonAnimCache::FClipSlot& Slot = Cache->ClipSlots[ClipIndex];
						if (!Slot.bAllocated || Slot.bEvicted)
						{
							continue;
						}

						if (Slot.bMetaDirty)
						{
							FGIAG_AnimLibraryClipMetaUpdate Upd;
							Upd.ClipIndex = (uint32)ClipIndex;
							Upd.Meta = Slot.Meta;
							Upload->ClipMetaUpdates.Add(Upd);
							Slot.bMetaDirty = false;
						}

						if (Slot.bTRSDirty)
						{
							FGIAG_AnimLibraryAnimTRSUpdate PUpd;
							PUpd.StartTransformIndex = (uint32)FMath::Max(0, Slot.StartTransformIndex);
							PUpd.NumTransforms = (uint32)FMath::Max(0, Slot.NumTransforms);
							if (Slot.Bake.IsValid())
							{
								PUpd.TRS = Slot.Bake->TRS;
								PUpd.CompletionEvent = Slot.Bake->CompletionEvent;
							}
							Upload->AnimTRSUpdates.Add(MoveTemp(PUpd));

							// TRS are now owned by Upload; drop GT reference to avoid CPU residency.
							Slot.Bake.Reset();
							Slot.bTRSDirty = false;
						}
					}

					Cache->PendingAnimLibraryUpload = Upload;
					Cache->bWarnedAnimLibraryUnavailable = false;
				}
			}
		}

		if (!bDoGPU)
		{
			continue;
		}

		// Build one payload per bucket.
		for (const int32 BucketIndex : Group.BucketIndices)
		{
			FMeshBucket& Bucket = Buckets[BucketIndex];
			check(Bucket.GroupIndex == GroupIndex);
			check(Bucket.SharedState.IsValid());

			const int32 TotalCapacity = Bucket.GetTotalSlotCapacity();

			if (bSkeletonStaticRebuilt)
			{
				Bucket.bSentSkeletonStaticUpload = false;
			}

			int32 DebugTotalAlive = 0;
			int32 DebugTotalActive = 0;

			if (!Bucket.bStorageInitialized || TotalCapacity <= 0)
			{
				continue;
			}

			auto TransformUpload = MakeShared<FGIAG_TransformUploadData>();
			TransformUpload->SlotCapacity = (uint32)TotalCapacity;
			bool bHasTransformDirty = false;

			DebugTotalAlive = Bucket.GpuAliveSlots.Num();

			Bucket.GpuActiveInstanceIndices.Reset();
			if (Bucket.GpuAliveSlots.Num() == 0)
			{
				// no-op
			}
			else if (!bEnableFrustumCull)
			{
				Bucket.GpuActiveInstanceIndices = Bucket.GpuAliveSlots;
			}
			else
			{
				Bucket.GpuActiveInstanceIndices.Reserve(Bucket.GpuAliveSlots.Num());
				for (const uint32 SlotU : Bucket.GpuAliveSlots)
				{
					const int32 BucketSlot = (int32)SlotU;
					const FVector Center = Bucket.TransformBySlot[BucketSlot].GetLocation();
					float Radius = MinRadius;
					const float Scale3D = Bucket.TransformBySlot[BucketSlot].GetScale3D().GetAbsMax();
					Radius = FMath::Max(Radius, (float)Bucket.BoundSphereRadius * Scale3D);
					Radius += CullPadding;
					if (ViewFrustum->IntersectSphere(Center, Radius))
					{
						Bucket.GpuActiveInstanceIndices.Add(SlotU);
					}
				}
			}

			const auto& ActiveIndices = Bucket.GpuActiveInstanceIndices;
			DebugTotalActive = ActiveIndices.Num();

			if (Bucket.DirtyTransformSlots.Num() > 0 || Bucket.NewSlotsThisTick.Num() > 0)
			{
				bHasTransformDirty = true;
				for (const uint32 SlotU : Bucket.DirtyTransformSlots)
				{
					const int32 BucketSlot = (int32)SlotU;
					if (Bucket.TransformDirty[BucketSlot])
					{
						Bucket.TransformDirty[BucketSlot] = false;
						TransformUpload->DirtyComponentToWorld.Add(FTransform3f(Bucket.TransformBySlot[BucketSlot]));
						TransformUpload->DirtySlots.Add(SlotU);
					}
				}
				Bucket.DirtyTransformSlots.Reset();

				if (Bucket.NewSlotsThisTick.Num() > 0)
				{
					Bucket.NewSlotsThisTick.Reset();
				}
			}

			if (ActiveIndices.Num() == 0)
			{
				if (bDebugBuckets)
				{
					UE_LOG(LogTemp, Log, TEXT("GIAG GT: Group=%d Bucket=%d AliveSlots=%d ActiveSlots=0 Cull=%d"),
						GroupIndex, BucketIndex, DebugTotalAlive, (int32)bEnableFrustumCull);
				}
				continue;
			}

			FGIAG_AnimGraphRunParams Params;
			Params.NumInstances = ActiveIndices.Num();
			Params.SlotCapacity = TotalCapacity;
			Params.NumBones = Group.NumBones;
			static_assert(sizeof(Params.TimeSlots) == sizeof(TimeSlots));
			FMemory::Memcpy(Params.TimeSlots, TimeSlots, sizeof(TimeSlots));
			Params.TimeSlotIndexBySlot = Bucket.TimeSlotIndexBySlot;
			Params.Skeleton = Group.Skeleton;
			Params.ParentIndices = &Cache->ParentIndices;
			Params.InverseRefPoseTRS = &Cache->InverseRefPoseTRS;
			Params.ActiveInstanceIndices = ActiveIndices;

			// Debug readback requests (bucket SlotIndex values).
			if (DebugReadbackEnabledSerialByRecordIndex.Num() > 0 || DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Num() > 0)
			{
				Params.DebugCpuRequestFrame = GFrameCounter;
				for (const uint32 SlotU : Params.ActiveInstanceIndices)
				{
					const int32 BucketSlot = (int32)SlotU;

					const int32 RecordIndex = Bucket.RecordIndexBySlot[BucketSlot];
					if (RecordIndex == INDEX_NONE)
					{
						continue;
					}
					const FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
					if (Rec.MasterRecordIndex != INDEX_NONE)
					{
						continue;
					}

					if (const int32* EnabledSerial = DebugReadbackEnabledSerialByRecordIndex.Find(RecordIndex))
					{
						if (*EnabledSerial == AnimRecordSerials[RecordIndex])
						{
							FGIAG_LocalPoseReadbackRequest Req;
							Req.RecordIndex = RecordIndex;
							Req.SerialNumber = *EnabledSerial;
							Req.SlotIndex = (uint32)BucketSlot;
							Params.DebugLocalPoseReadbackRequests.Add(MoveTemp(Req));
						}
					}
					if (const int32* EnabledSerial = DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Find(RecordIndex))
					{
						if (*EnabledSerial == AnimRecordSerials[RecordIndex])
						{
							FGIAG_NeedNodeBitsReadbackRequest Req;
							Req.RecordIndex = RecordIndex;
							Req.SerialNumber = *EnabledSerial;
							Req.SlotIndex = (uint32)BucketSlot;
							Params.DebugNeedNodeBitsReadbackRequests.Add(MoveTemp(Req));
						}
					}
				}
			}

			Params.AnimLibraryUpload = Cache->PendingAnimLibraryUpload;
			Params.AnimLibraryVersion = Cache->AnimLibraryVersion;
			Params.NumClips = (uint32)Cache->ClipSlots.Num();
			Params.AnimTRSNum = (uint32)Cache->AnimTRSCapacity;

			checkf(Params.AnimLibraryVersion != 0 && Params.NumClips != 0 && Params.AnimTRSNum != 0 && Group.NumBones > 0,
				TEXT("GIAG: AnimLibrary not ready for Skeleton '%s'."), *GetNameSafe(Cache->Skeleton));

			if (bHasTransformDirty)
			{
				Params.TransformUpload = TransformUpload;
			}

			const bool bUploadSkeletonStatic = !Bucket.bSentSkeletonStaticUpload;
			FGIAG_AnimGraphUploads Uploads = Bucket.UploadBuilder.BuildUploads_GameThread(
				*Group.Compiled,
				Bucket.NodeBasePtrsByNode,
				Bucket.NodeStrideBytes,
				TotalCapacity,
				Params,
				Bucket.DirtyNodeIndices,
				Bucket.DirtyNodeMask,
				Bucket.NodeParamDirtyBitsByNode,
				Bucket.DirtyNodeParamSlotsByNode,
				bUploadSkeletonStatic,
				Cache->ParentIndices,
				Cache->InverseRefPoseTRS,
				0);

			// Optional shared resources.
			Uploads.MaxOptionalSRVSlot = Group.MaxOptionalSRVSlot;
			if (Group.MaxOptionalSRVSlot >= 0)
			{
				Uploads.OptionalSRVKeyByNodeBySlot = Group.OptionalSRVKeyByNodeBySlot;
			}
			if (bUploadSkeletonStatic)
			{
				Bucket.bSentSkeletonStaticUpload = true;
			}

			Params.DebugGraphName = GetNameSafe(Group.AnimGraph);
			Params.DebugMeshName = GetNameSafe(Bucket.SkeletalMesh);

			FGIAG_RenderPayload Payload;
			Payload.Compiled = Group.Compiled;
			Payload.Params = MoveTemp(Params);
			Payload.Uploads = MoveTemp(Uploads);

			TRefCountPtr<FGIAG_TransformProviderState> State = Bucket.SharedState;
			ENQUEUE_RENDER_COMMAND(GIAG_SetProviderPayload)(
				[State, Payload = MoveTemp(Payload)](FRHICommandListImmediate& RHICmdList) mutable
				{
					State->EnqueuePayload_RenderThread(MoveTemp(Payload));
				});

			if (bDebugBuckets)
			{
				UE_LOG(LogTemp, Log, TEXT("GIAG GT: Group=%d Bucket=%d AliveSlots=%d ActiveSlots=%d Cull=%d"),
					GroupIndex, BucketIndex, DebugTotalAlive, DebugTotalActive, (int32)bEnableFrustumCull);
			}
		}
	}

	// End-of-tick capacity maintenance:
	//   Pass A) Drain auto-shrink evaluations marked by RemoveInstance(bAllowAutoShrink=true).
	//           Buckets with bGrewThisFrame are skipped (flap guard) and re-evaluated next tick.
	//   Pass B) Unconditionally clear bGrewThisFrame on every bucket so the next tick can shrink
	//           even if today's frame grew. (Must be a separate pass — clearing inside Pass A would
	//           skip buckets that hit the flap guard or had no shrink eval.)
	// Capacity changes themselves are queued onto FGIAG_TransformProviderState::PendingCapacityChange_RT
	// from each Grow/CompactAndShrink call site (via ENQUEUE_RENDER_COMMAND). The renderer extension
	// drains them inside ProvideTransforms — no end-of-tick TransformProvider rebind needed; the engine
	// itself reallocates+copies the per-primitive span when SetUniqueAnimationCount is called.
	for (auto It = Buckets.CreateIterator(); It; ++It)
	{
		FMeshBucket& Bucket = *It;
		if (!Bucket.bPendingShrinkEvaluation)
		{
			continue;
		}

		// Flap guard: a same-frame grow defers the shrink to the *next* tick. Leave the eval flag
		// set so end-of-tick maintenance picks the bucket up again after Pass B clears bGrewThisFrame.
		if (Bucket.bGrewThisFrame)
		{
			continue;
		}

		Bucket.bPendingShrinkEvaluation = false;

		// Contract: bPendingShrinkEvaluation is only set in RemoveInstance for master GPU/CPU
		// branches (after the slot's bucket has gone through InitBucketAsMaster → InitBucketStorage).
		check(Bucket.TransformProvider != nullptr);
		check(Bucket.TransformProvider->GetMode() == EGIAG_TransformProviderMode::MasterEvaluate);
		check(Bucket.bStorageInitialized);

		const int32 OldCap = Bucket.GetTotalSlotCapacity();
		const int32 Used = Bucket.NumInstances;
		const int32 TargetCap = GIAG::BucketCapacity::ComputeShrinkTarget(Used, OldCap);
		if (TargetCap >= OldCap)
		{
			continue;
		}

		CompactAndShrinkMaster(It.GetIndex(), TargetCap);
	}

	for (auto It = Buckets.CreateIterator(); It; ++It)
	{
		It->bGrewThisFrame = false;
	}
}

