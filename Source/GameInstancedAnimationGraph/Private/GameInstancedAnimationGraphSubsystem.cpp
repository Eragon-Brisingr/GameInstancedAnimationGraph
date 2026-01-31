// Minimal runtime subsystem that manages instanced meshes and GPU-driven animation state.

#include "GameInstancedAnimationGraphSubsystem.h"

#include "AnimationRuntime.h"
#include "GameInstancedAnimationGraphSettings.h"
#include "GIAG_ActorInterface.h"
#include "GIAG_AnimGraphCpuRunner.h"
#include "GIAG_AnimGraphUploadBuilder.h"
#include "GIAG_AnimSequenceUserData.h"
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

DECLARE_STATS_GROUP(TEXT("GameInstancedAnim"), STATGROUP_GameInstancedAnim, STATCAT_Advanced);

static TAutoConsoleVariable<bool> CVar_InstancedAnimEnableCull
{
	TEXT("GameInstancedAnim.EnableCull"),
	true,
	TEXT("Enable CPU frustum culling for GameInstancedAnim (partial graph evaluation + preserve outputs)."),
};

static TAutoConsoleVariable<int32> CVar_InstancedAnimDebugShards
{
	TEXT("GameInstancedAnim.DebugShards"),
	0,
	TEXT("Debug shard creation and per-tick evaluation stats. 0=off, 1=log per tick aggregates."),
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

	FORCEINLINE static void SetAnimationMinScreenSize(UInstancedSkinnedMeshComponent& Component, float Value)
	{
		struct FAccessor : UInstancedSkinnedMeshComponent
		{
			using UInstancedSkinnedMeshComponent::AnimationMinScreenSize;
		};
		static_cast<FAccessor*>(&Component)->AnimationMinScreenSize = Value;
	}

	static void UpdateISKMCInstanceTransformById(
		UInstancedSkinnedMeshComponent& Component,
		FPrimitiveInstanceId InstanceId,
		const FTransform& NewTransform,
		bool bWorldSpace)
	;

	struct FSkinnedShardCommonInit
	{
		UInstancedSkinnedMeshComponent* ISKMC = nullptr;
		UGIAG_TransformProviderData* TransformProvider = nullptr;
	};

	// Create a shard with a registered component + mesh, and an enabled (but not yet bound) transform provider.
	FORCEINLINE static FSkinnedShardCommonInit CreateSkinnedShardCommon(AActor& HostActor, USkeletalMesh& SkeletalMesh)
	{
		FSkinnedShardCommonInit Out;

		Out.ISKMC = NewObject<UInstancedSkinnedMeshComponent>(&HostActor);
		check(Out.ISKMC);

		float AnimationMinScreenSize = 0.0001f;
		if (auto UserData = SkeletalMesh.GetAssetUserData<UGIAG_SkeletalMeshUserData>())
		{
			AnimationMinScreenSize = UserData->AnimationMinScreenSize;
		}
		SetAnimationMinScreenSize(*Out.ISKMC, AnimationMinScreenSize);
		Out.ISKMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Out.ISKMC->bNavigationRelevant = false;
		Out.ISKMC->CreationMethod = EComponentCreationMethod::Instance;

		Out.ISKMC->RegisterComponent();
		Out.ISKMC->SetSkinnedAssetAndUpdate(&SkeletalMesh);

		Out.TransformProvider = NewObject<UGIAG_TransformProviderData>(Out.ISKMC);
		check(Out.TransformProvider);

		return Out;
	}

	FORCEINLINE static int32 AddDefaultShardToBucket(
		FMeshBucket& InBucket,
		const FGraphGroup& Group,
		AActor& HostActor,
		USkeletalMesh& SkeletalMesh,
		int32 SlotCapacity)
	{
		const FSkinnedShardCommonInit Common = CreateSkinnedShardCommon(HostActor, SkeletalMesh);
		const int32 NewShardIndex = InBucket.Shards.Add(FSkinnedShard());
		FSkinnedShard& NewShard = InBucket.Shards[NewShardIndex];
		NewShard.ISKMC = Common.ISKMC;
		NewShard.TransformProvider = Common.TransformProvider;
		NewShard.TransformProvider->AnimationSlotCount = SlotCapacity;
		NewShard.ISKMC->SetTransformProvider(NewShard.TransformProvider);
		check(Group.Compiled);
		NewShard.InitStorage(*Group.Compiled, MakeArrayView(Group.NodeStrideBytes), NewShard.TransformProvider->AnimationSlotCount);
		return NewShardIndex;
	}

	FORCEINLINE static int32 AddFollowerShardToBucket(
		FMeshBucket& InBucket,
		const FGraphGroup& Group,
		AActor& HostActor,
		USkeletalMesh& SkeletalMesh,
		int32 SlotCapacity,
		const TRefCountPtr<FGIAG_TransformProviderState>& MasterState,
		int32 DstNumBones,
		int32 SrcBones,
		const TSharedPtr<const TArray<uint32>>& RemapShared)
	{
		const FSkinnedShardCommonInit Common = CreateSkinnedShardCommon(HostActor, SkeletalMesh);
		const int32 NewShardIndex = InBucket.Shards.Add(FSkinnedShard());
		FSkinnedShard& NewShard = InBucket.Shards[NewShardIndex];
		NewShard.ISKMC = Common.ISKMC;
		NewShard.TransformProvider = Common.TransformProvider;
		NewShard.TransformProvider->AnimationSlotCount = SlotCapacity;
		NewShard.TransformProvider->ConfigureAsFollower(MasterState.GetReference(), DstNumBones, SrcBones, RemapShared);
		NewShard.ISKMC->SetTransformProvider(NewShard.TransformProvider);

		check(Group.Compiled);
		NewShard.InitStorage(*Group.Compiled, MakeArrayView(Group.NodeStrideBytes), NewShard.TransformProvider->AnimationSlotCount);
		return NewShardIndex;
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

void UGameInstancedAnimationGraphSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Shared optional resource bus (GT publishes, RT reads).
	SharedResourceBus = MakeShared<FGIAG_SharedResourceBus>();
	AttachBus = MakeShared<FGIAG_AttachBus>();
	AttachReadbackBus = MakeShared<FGIAG_AttachReadbackBus>();
	NiagaraAttachRegistry = MakeShared<FGIAG_NiagaraAttachRegistry, ESPMode::ThreadSafe>();
	NativeAttachRegistry = MakeShared<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe>();

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
	
	// Flush deferred Niagara attach meta to RT early in the frame so Niagara ticks can observe updated versions.
	FlushNiagaraAttachBuckets_GameThread();
	PrecomputeCpuPoseCache_GameThread((float)World->GetTimeSeconds());
}

void UGameInstancedAnimationGraphSubsystem::PrecomputeCpuPoseCache_GameThread(float NowSeconds)
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

			for (auto ShardIt = Bucket.Shards.CreateIterator(); ShardIt; ++ShardIt)
			{
				FSkinnedShard& Shard = *ShardIt;

				// Follower shard does not evaluate.
				if (Shard.TransformProvider && Shard.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap)
				{
					continue;
				}

				if (Shard.CpuAliveSlots.Num() <= 0)
				{
					continue;
				}

				// Sync per-slot transforms from CpuProxyActors.
				for (const uint32 SlotU : Shard.CpuAliveSlots)
				{
					const int32 Slot = (int32)SlotU;
					const int32 RecordIndex = Shard.RecordIndexBySlot[Slot];
					check(RecordIndex != INDEX_NONE && AnimRecords.IsValidIndex(RecordIndex));
					const FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
					check(Rec.MasterRecordIndex == INDEX_NONE);
					check(Rec.CpuProxyActor);

					if (USkeletalMeshComponent* Skinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec.CpuProxyActor))
					{
						Shard.TransformBySlot[Slot] = Skinned->GetComponentTransform();
					}
					else
					{
						Shard.TransformBySlot[Slot] = Rec.CpuProxyActor->GetActorTransform();
					}
				}

				if (!Group.CpuRunner)
				{
					Group.CpuRunner = MakeUnique<FGIAG_AnimGraphCpuRunner>();
				}

				FGIAG_AnimGraphCpuRunParams CpuParams;
				CpuParams.NumInstances = Shard.CpuAliveSlots.Num();
				CpuParams.SlotCapacity = Shard.SlotCapacity;
				CpuParams.NumBones = Group.NumBones;
				CpuParams.CurrentTimeSeconds = NowSeconds;
				check(Bucket.SkeletalMesh);
				CpuParams.SkeletalMesh = Bucket.SkeletalMesh;
				CpuParams.Skeleton = Group.Skeleton;
				CpuParams.ParentIndices = Cache->ParentIndices;
				CpuParams.RefPoseLocal = Cache->RefPoseLocal;
				CpuParams.ActiveInstanceIndices = Shard.CpuAliveSlots;
				CpuParams.ComponentToWorldBySlot = Shard.TransformBySlot;
				CpuParams.AnimSequencesByClipIndex = Cache->ClipIndexToSequence;
				CpuParams.NodeData = MakeArrayView(
					reinterpret_cast<const uint8* const*>(Shard.NodeBasePtrsByNode.GetData()),
					Shard.NodeBasePtrsByNode.Num());
				CpuParams.NodeStrideBytes = Shard.NodeStrideBytes;

				const FGIAG_AnimGraphCpuRunner::FOutputs Outputs = Group.CpuRunner->Evaluate(*Group.Compiled, CpuParams);
				check(Outputs.FinalLocalPose.IsValid());

				for (const uint32 SlotU : Shard.CpuAliveSlots)
				{
					const int32 Slot = (int32)SlotU;
					const int32 RecordIndex = Shard.RecordIndexBySlot[Slot];
					check(RecordIndex != INDEX_NONE && AnimRecords.IsValidIndex(RecordIndex));

					FCpuPoseCacheEntry& Entry = CpuPoseCacheByRecordIndex.FindOrAdd(RecordIndex);
					Entry.Frame = Frame;
					Entry.SerialNumber = AnimRecordSerials[RecordIndex];
					Entry.NumBones = Group.NumBones;
					Entry.LocalPose.SetNum(Group.NumBones, EAllowShrinking::No);

					for (int32 BoneIndex = 0; BoneIndex < Group.NumBones; ++BoneIndex)
					{
						Entry.LocalPose[BoneIndex] = Outputs.FinalLocalPose.At(Slot, BoneIndex);
					}
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
	checkf(Bucket.Shards.IsValidIndex(Rec->ShardIndex), TEXT("GIAG CPU: invalid ShardIndex=%d (BucketIndex=%d)."), Rec->ShardIndex, Rec->BucketIndex);
	const FSkinnedShard& Shard = Bucket.Shards[Rec->ShardIndex];
	check(Rec->SlotIndex >= 0 && Rec->SlotIndex < Shard.SlotCapacity);

	// Base params (slot-capacity evaluation view into the shard).
	FGIAG_AnimGraphCpuRunParams Params;
	Params.NumBones = Group.NumBones;
	Params.SlotCapacity = Shard.SlotCapacity;
	Params.CurrentTimeSeconds = GetWorld() ? (float)GetWorld()->GetTimeSeconds() : 0.0f;
	Params.SkeletalMesh = Rec->SkeletalMesh;
	Params.Skeleton = Group.Skeleton;
	Params.ParentIndices = Cache->ParentIndices;
	Params.RefPoseLocal = Cache->RefPoseLocal;
	Params.ComponentToWorldBySlot = Shard.TransformBySlot;
	Params.AnimSequencesByClipIndex = Cache->ClipIndexToSequence;
	Params.NodeData = MakeArrayView(
		reinterpret_cast<const uint8* const*>(Shard.NodeBasePtrsByNode.GetData()),
		Shard.NodeBasePtrsByNode.Num());
	Params.NodeStrideBytes = Shard.NodeStrideBytes;

	// Single-slot pack: evaluate only this instance with SlotCapacity=1 to minimize framework overhead.
	const int32 SlotIndex = Rec->SlotIndex;
	const uint32 ActiveSlotU = 0u;

	FGIAG_AnimGraphCpuRunParams PackedParams = Params;
	PackedParams.NumInstances = 1;
	PackedParams.SlotCapacity = 1;
	PackedParams.ActiveInstanceIndices = TConstArrayView<uint32>(&ActiveSlotU, 1);

	const FTransform SingleC2W = Params.ComponentToWorldBySlot[SlotIndex];
	PackedParams.ComponentToWorldBySlot = TConstArrayView<FTransform>(&SingleC2W, 1);

	TArray<const uint8*, TInlineAllocator<64>> PackedNodeData;
	PackedNodeData.SetNumUninitialized(Params.NodeData.Num());
	for (int32 NodeIdx = 0; NodeIdx < Params.NodeData.Num(); ++NodeIdx)
	{
		const uint32 Stride = Params.NodeStrideBytes[NodeIdx];
		PackedNodeData[NodeIdx] = Params.NodeData[NodeIdx] + (int64)Stride * (int64)SlotIndex;
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
	const double NowSeconds = NodeRef.System->GetWorld()->GetTimeSeconds();
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
	OutNodeRef.ShardIndex = Rec->ShardIndex;
	OutNodeRef.SlotIndex = Rec->SlotIndex;
	OutNodeRef.NodeIndex = NodeIdx;

	FMeshBucket& Bucket = OutNodeRef.System->Buckets[Rec->BucketIndex];
	FSkinnedShard& Shard = Bucket.Shards[Rec->ShardIndex];
	return Shard.GetNodePtr(NodeIdx, Rec->SlotIndex);
}

namespace PrivateAccess
{
	template<typename Tag, typename Tag::type M>
	struct Rob
	{ 
		FORCEINLINE friend typename Tag::type Access(Tag)
		{
			return M;
		}
	};

#define ROB_MEMBER(Class, Member, MemberType) \
struct Class##Member##Rob \
{ \
typedef MemberType Class::*type; \
friend type Access(Class##Member##Rob); \
};\
template struct Rob<Class##Member##Rob, &Class::Member>; \
FORCEINLINE MemberType& Class##_##Member(Class& Inst) { return Inst.*Access(Class##Member##Rob()); }

	// Type, Member, MemberType
	ROB_MEMBER(UInstancedSkinnedMeshComponent, InstanceDataManager, FInstanceDataManager);
	ROB_MEMBER(FInstanceDataManager, InstanceUpdateTracker, FInstanceAttributeTracker);

#undef ROB_MEMBER

#define ROB_MEMBER_FUNCTION(Class, Member, ReturnType, ...) \
struct Class##Member##Rob \
{ \
typedef ReturnType (Class::*type)(__VA_ARGS__); \
friend type Access(Class##Member##Rob); \
};\
template struct Rob<Class##Member##Rob, &Class::Member>; \
template<typename... TArgs> \
FORCEINLINE ReturnType Class##_##Member(Class& Inst, TArgs&&... Args) { return (Inst.*Access(Class##Member##Rob()))(Args...); }
    
	// Type, Member, ReturnType, Args...
	// ROB_MEMBER_FUNCTION(UGameplayTagsManager, FindOrAddTagSource, FGameplayTagSource*, FName, EGameplayTagSourceType, const FString&);

#undef ROB_MEMBER_FUNCTION

#define ROB_STATIC_FUNCTION(Class, Member, ReturnType, ...) \
struct Class##Member##Rob \
{ \
typedef ReturnType (*type)(__VA_ARGS__); \
friend type Access(Class##Member##Rob); \
};\
template struct Rob<Class##Member##Rob, &Class::Member>; \
template<typename... TArgs> \
FORCEINLINE ReturnType Class##_##Member(TArgs&&... Args) { return (*Access(Class##Member##Rob()))(Args...); }

	// Type, Member, ReturnType, Args...
	// ROB_STATIC_FUNCTION(FTickableCookObject, GetStatics, FTickableObjectVisitor::FTickableStatics&);

#undef ROB_STATIC_FUNCTION
}

void UGameInstancedAnimationGraphSubsystem::FPrivateUtils::UpdateISKMCInstanceTransformById(
	UInstancedSkinnedMeshComponent& Component,
	FPrimitiveInstanceId InstanceId,
	const FTransform& NewTransform,
	bool bWorldSpace)
{
	// GPU-only instances are not backed by CPU InstanceData; do not attempt CPU-side updates.
	check(!Component.UsesGPUOnlyInstances());

	FInstanceDataManager& IDM = PrivateAccess::UInstancedSkinnedMeshComponent_InstanceDataManager(Component);
	check(IDM.IsValidId(InstanceId));

	const int32 InstanceIndex = IDM.IdToIndex(InstanceId);
	check(InstanceIndex != INDEX_NONE);

	// GetInstanceData() returns const&, but Component is mutable; we can legally update its backing storage.
	auto& Instances = const_cast<TArray<FSkinnedMeshInstanceData>&>(Component.GetInstanceData());
	check(Instances.IsValidIndex(InstanceIndex));

	const FTransform LocalTransform = bWorldSpace
		? NewTransform.GetRelativeTransform(Component.GetComponentTransform())
		: NewTransform;

	Instances[InstanceIndex].Transform = FTransform3f(LocalTransform);
	{
		FInstanceAttributeTracker& Tracker = PrivateAccess::FInstanceDataManager_InstanceUpdateTracker(IDM);
		Tracker.MarkIndex<FInstanceAttributeTracker::EFlag::TransformChanged>(InstanceIndex, IDM.GetMaxInstanceIndex());
		Component.MarkRenderInstancesDirty();
	}
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
	for (int32 Frame = 0; Frame < NumFrames; ++Frame)
	{
		const float Time = FMath::Min(Frame * SPF, Len);
		if (!GIAG::EvalAnimSequenceLocalPose(AnimSequence, Time, Skeleton, LocalPose))
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
			const FProperty* P = DefaultInstanceStruct->FindPropertyByName(Node.MemberName);
			const FStructProperty* SP = CastField<FStructProperty>(P);
			checkf(SP && SP->Struct, TEXT("GIAG: Node member '%s' is not a valid struct property on DefaultGraphInstance."), *Node.MemberName.ToString());
			checkf(SP->Struct->IsChildOf(FGIAG_AnimNodeBase::StaticStruct()),
				TEXT("GIAG: Node member '%s' is not derived from FGIAG_AnimNodeBase."), *Node.MemberName.ToString());

			const int32 Offset = SP->GetOffset_ForInternal();
			checkf(Offset == Node.InstanceDataOffset,
				TEXT("GIAG: Node member '%s' offset mismatch (Compiled=%d Property=%d). Recompile graph."),
				*Node.MemberName.ToString(), Node.InstanceDataOffset, Offset);

			UScriptStruct* NodeStruct = SP->Struct;
			check(NodeStruct);

			const uint32 Size = (uint32)NodeStruct->GetStructureSize();
			check(NodeStruct->GetMinAlignment() == 16);
			const uint32 Stride = Align(Size, NodeStruct->GetMinAlignment());
			check(Stride >= Size);

			Group.NodeProperties[NodeIdx] = SP;
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
				for (const TPair<uint8, FGIAG_AnimResourceKey>& P : NodeSlotKeysTmp[NodeIdx])
				{
					if ((int32)P.Key <= Group.MaxOptionalSRVSlot)
					{
						Group.OptionalSRVKeyByNodeBySlot[NodeIdx][(int32)P.Key] = P.Value;
					}
				}
			}

			// Build per-key bytes once per World (SharedResourcesByKey) and enqueue incremental uploads for RT.
			for (const TPair<FGIAG_AnimResourceKey, FUniqueResourceSource>& KV : UniqueResources)
			{
				const FUniqueResourceSource& Src = KV.Value;
				check(Src.Meta);

				FSharedResourceEntry* Existing = SharedResourcesByKey.Find(KV.Key);
				if (!Existing)
				{
					FSharedResourceEntry NewEntry;
					NewEntry.Request = Src.Request;
					NewEntry.Bytes = MakeShared<TArray<uint8>>();
					const bool bOk = Src.Meta->BuildResourceForGPU(Src.Request, Src.Settings, Skeleton, *NewEntry.Bytes);
					check(bOk);
					Existing = &SharedResourcesByKey.Add(KV.Key, MoveTemp(NewEntry));

					FGIAG_AnimGraphResourceUploadRun Run;
					Run.Request = Existing->Request;
					Run.Bytes = Existing->Bytes;
					SharedResourceBus->Enqueue_GameThread(MoveTemp(Run));
				}
				else
				{
					// Contract: same key must imply identical layout.
					check(Existing->Request.Layout.Kind == Src.Request.Layout.Kind);
					check(Existing->Request.Layout.StrideBytes == Src.Request.Layout.StrideBytes);
					check(Existing->Request.Layout.NumElements == Src.Request.Layout.NumElements);
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

	auto StartBakeTask = [](USkeleton* Skeleton, const UAnimSequence* Anim, float SPF, float Len, int32 NumFrames, int32 NumBonesLocal, const FClipBakePayloadPtr& BakePayload)
	{
		Async(EAsyncExecution::ThreadPool, [Skeleton, Anim, SPF, Len, NumFrames, NumBonesLocal, BakePayload]()
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
				if (!GIAG::EvalAnimSequenceLocalPose(Anim, Time, Skeleton, LocalPose))
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

	StartBakeTask(Group.Skeleton, AnimSequence, SPF, Len, NumFrames, NumBones, Slot.Bake);

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
	
	const FBucketKey Key{ SkeletalMesh, GroupIndex };
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

	// Create initial shard.
		FPrivateUtils::AddDefaultShardToBucket(Bucket, Group, *HostActor, *SkeletalMesh, DefaultSlotsPerShard);
	check(Bucket.Shards.Num() > 0);
	Bucket.Shards[0].UploadBuilder.Initialize(*Group.Compiled);

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
	check(Handle.InstancedAnimSubsystem == this);
	return &AnimRecords[Handle.RecordIndex];
}

void UGameInstancedAnimationGraphSubsystem::InvalidateHandle(FGameInstancedAnimationGraphHandle& Handle)
{
	Handle.InstancedAnimSubsystem = nullptr;
	Handle.RecordIndex = INDEX_NONE;
	Handle.SerialNumber = INDEX_NONE;
}

void UGameInstancedAnimationGraphSubsystem::CleanupGroupIfUnused(int32 GroupIndex)
{
	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);

	// Keep group alive while any bucket references it (bucket owns shards + instance storage now).
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
	const FBucketKey Key{ Bucket.SkeletalMesh, Bucket.GroupIndex };
	BucketByKey.Remove(Key);

	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d (from BucketIndex=%d)."), GroupIndex, BucketIndex);
	Groups[GroupIndex].BucketIndices.RemoveSingleSwap(BucketIndex, EAllowShrinking::No);

	for (FSkinnedShard& Shard : Bucket.Shards)
	{
		if (Shard.ISKMC)
		{
			Shard.ISKMC->DestroyComponent();
			Shard.ISKMC = nullptr;
		}
		Shard.TransformProvider = nullptr;
	}
	Bucket.Shards.Reset();

	if (Bucket.SkeletalMesh)
	{
		SkeletalMeshes.RemoveSingle(Bucket.SkeletalMesh);
		Bucket.SkeletalMesh = nullptr;
	}

	Buckets.RemoveAt(BucketIndex);

	CleanupGroupIfUnused(GroupIndex);
}

void UGameInstancedAnimationGraphSubsystem::CleanupShardIfEmpty(int32 BucketIndex, int32 ShardIndex)
{
	if (!Buckets.IsValidIndex(BucketIndex))
	{
		return;
	}
	FMeshBucket& Bucket = Buckets[BucketIndex];
	check(Bucket.Shards.IsValidIndex(ShardIndex));
	FSkinnedShard& Shard = Bucket.Shards[ShardIndex];
	if (Shard.NumInstances > 0 || Shard.GpuAliveSlots.Num() > 0 || Shard.CpuAliveSlots.Num() > 0)
	{
		return;
	}

	if (Shard.ISKMC)
	{
		Shard.ISKMC->DestroyComponent();
		Shard.ISKMC = nullptr;
	}
	Shard.TransformProvider = nullptr;

	Bucket.Shards.RemoveAt(ShardIndex);
}

FGameInstancedAnimationGraphHandle UGameInstancedAnimationGraphSubsystem::AddInstance(USkeletalMesh* SkeletalMesh, UGIAG_AnimGraph* AnimGraph, const FTransform& Transform, TSubclassOf<AActor> CpuProxyClass, bool bCpuMode)
{
	FGameInstancedAnimationGraphHandle Handle;

	if (SkeletalMesh == nullptr || !AnimGraph)
	{
		return Handle;
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

	// Allocate slot in a shard (<=127 slots each). AnimationIndex == SlotIndex.
	auto AllocateSlot = [&](FSkinnedShard& Shard) -> int32
	{
		if (Shard.FreeSlots.Num() > 0)
		{
			const int32 Slot = Shard.FreeSlots.Pop(EAllowShrinking::No);
			return Slot;
		}
		if (Shard.SlotNum < Shard.SlotCapacity)
		{
			return Shard.SlotNum++;
		}
		return INDEX_NONE;
	};

	int32 ShardIndex = INDEX_NONE;
	int32 SlotIndex = INDEX_NONE;
	for (auto ShardIt = Bucket->Shards.CreateIterator(); ShardIt; ++ShardIt)
	{
		const int32 Idx = ShardIt.GetIndex();
		FSkinnedShard& Shard = *ShardIt;
		if (Shard.NodeStorageByNode.Num() == 0)
		{
			check(Shard.TransformProvider);
			check(Group.Compiled);
			Shard.InitStorage(*Group.Compiled, MakeArrayView(Group.NodeStrideBytes), Shard.TransformProvider->AnimationSlotCount);
			Shard.UploadBuilder.Initialize(*Group.Compiled);
		}
		const int32 Slot = AllocateSlot(Shard);
		if (Slot != INDEX_NONE)
		{
			ShardIndex = Idx;
			SlotIndex = Slot;
			break;
		}
	}
	if (ShardIndex == INDEX_NONE)
	{
		ShardIndex = FPrivateUtils::AddDefaultShardToBucket(*Bucket, Group, *HostActor, *SkeletalMesh, DefaultSlotsPerShard);
		check(Bucket->Shards.IsValidIndex(ShardIndex));
		Bucket->Shards[ShardIndex].UploadBuilder.Initialize(*Group.Compiled);
		SlotIndex = AllocateSlot(Bucket->Shards[ShardIndex]);
	}
	check(ShardIndex != INDEX_NONE && SlotIndex != INDEX_NONE);

	check(Bucket->Shards.IsValidIndex(ShardIndex));
	FSkinnedShard& Shard = Bucket->Shards[ShardIndex];

	// Initialize per-node AoS data for this slot from graph defaults.
	check(Group.Compiled);
	check(Group.NodeProperties.Num() == Group.Compiled->NumNodes);
	
	auto DefaultGraphInstance = AnimGraph->GetDefaultGraphInstance();
	for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
	{
		const FGIAG_AnimCompiledNode& Node = Group.Compiled->Nodes[NodeIdx];
		check(Node.NodeMeta);
		const FStructProperty* SP = Group.NodeProperties[NodeIdx];
		check(SP && SP->Struct);

		uint8* NodePtr = Shard.GetNodePtr(NodeIdx, SlotIndex);
		SP->Struct->InitializeStruct(NodePtr);
		SP->Struct->CopyScriptStruct(NodePtr, Group.NodeProperties[NodeIdx]->ContainerPtrToValuePtr<void>(DefaultGraphInstance.GetMemory()));
		Node.NodeMeta->InitInstanceData(NodePtr);
	}

	// Record (authoritative).
	const int32 RecordIndex = AnimRecords.Add({});
	FInstancedAnimRecord& NewRecord = AnimRecords[RecordIndex];
	NewRecord.SkeletalMesh = SkeletalMesh;
	NewRecord.CpuProxyClass = CpuProxyClass;
	NewRecord.BucketIndex = BucketIndex;
	NewRecord.ShardIndex = ShardIndex;
	NewRecord.GroupIndex = GroupIndex;
	NewRecord.SlotIndex = SlotIndex;

	if (AnimRecordSerials.Num() <= RecordIndex)
	{
		AnimRecordSerials.SetNumZeroed(RecordIndex + 1);
	}
	int32& Serial = AnimRecordSerials[RecordIndex];
	// Avoid INDEX_NONE (UE uses -1); keep serial positive.
	Serial = FMath::Max(1, Serial + 1);
	Handle.InstancedAnimSubsystem = this;
	Handle.RecordIndex = RecordIndex;
	Handle.SerialNumber = Serial;

	Bucket->NumInstances++;
	checkf(SlotIndex >= 0 && SlotIndex < Shard.SlotCapacity, TEXT("GIAG: invalid SlotIndex=%d."), SlotIndex);
	checkf(Shard.SlotAlive.Num() == Shard.SlotCapacity && Shard.RecordIndexBySlot.Num() == Shard.SlotCapacity,
		TEXT("GIAG: shard slot arrays size mismatch (SlotAlive=%d RecordIndexBySlot=%d Cap=%d)."),
		Shard.SlotAlive.Num(), Shard.RecordIndexBySlot.Num(), Shard.SlotCapacity);
	Shard.RecordIndexBySlot[SlotIndex] = RecordIndex;
	Shard.SlotAlive[SlotIndex] = true;
	Shard.TransformBySlot[SlotIndex] = Transform;
	Shard.NewSlotsThisTick.Add((uint32)SlotIndex);
	if (!Shard.TransformDirty[SlotIndex])
	{
		Shard.TransformDirty[SlotIndex] = true;
		Shard.DirtyTransformSlots.Add((uint32)SlotIndex);
	}

	// Force initial GPU param upload for all nodes on this slot (event-driven; no per-frame scan).
	for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
	{
		Shard.MarkNodeParamDirty(NodeIdx, SlotIndex);
	}

	// Create backend:
	// - GPU: create ISKMC instance immediately.
	// - CPU: create a slot-only record, then spawn CpuProxyActor (no ISKMC instance).
	if (!bCpuMode)
	{
		check(Shard.ISKMC);
		const FPrimitiveInstanceId InstanceId = Shard.ISKMC->AddInstance(Transform, SlotIndex, true);
		Shard.NumInstances += 1;
		NewRecord.ISKMC = Shard.ISKMC;
		NewRecord.InstanceId = InstanceId;
		FPrivateUtils::AddSlotToList(Shard.GpuAliveSlots, Shard.GpuAliveListIndexBySlot, SlotIndex);
	}
	else
	{
		check(CpuProxyClass);
		NewRecord.ISKMC = nullptr;
		NewRecord.InstanceId = FPrimitiveInstanceId();
		FPrivateUtils::AddSlotToList(Shard.CpuAliveSlots, Shard.CpuAliveListIndexBySlot, SlotIndex);
		FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
		Rec.CpuProxyActor = FPrivateUtils::SpawnCpuProxyActor(Handle, Rec, GetWorld(), Transform);
	}

	return Handle;
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
	check(Rec->BucketIndex != INDEX_NONE && Rec->ShardIndex != INDEX_NONE && Rec->SlotIndex != INDEX_NONE);

	checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), Rec->BucketIndex);
	FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	checkf(Bucket.Shards.IsValidIndex(Rec->ShardIndex), TEXT("GIAG: invalid ShardIndex=%d."), Rec->ShardIndex);
	FSkinnedShard& Shard = Bucket.Shards[Rec->ShardIndex];
	check(Rec->SlotIndex >= 0 && Rec->SlotIndex < Shard.SlotCapacity);

	const int32 SlotIndex = Rec->SlotIndex;
	const FTransform CurrentTransform = Shard.TransformBySlot[SlotIndex];

	Rec->ISKMC->RemoveInstance(Rec->InstanceId);
	Shard.NumInstances = FMath::Max(0, Shard.NumInstances - 1);

	// Keep shard slot + instance bytes alive; only switch backend list membership.
	FPrivateUtils::RemoveSlotFromList(Shard.GpuAliveSlots, Shard.GpuAliveListIndexBySlot, SlotIndex);
	FPrivateUtils::AddSlotToList(Shard.CpuAliveSlots, Shard.CpuAliveListIndexBySlot, SlotIndex);

	// Spawn CPU proxy actor.
	Rec->CpuProxyActor = FPrivateUtils::SpawnCpuProxyActor(Handle, *Rec, GetWorld(), CurrentTransform);
	Rec->ISKMC = nullptr;
	Rec->InstanceId = FPrimitiveInstanceId();

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
				const int32 FollowShardIndex = FollowRec.ShardIndex;

				checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG Follow GPU->CPU: invalid BucketIndex=%d."), FollowBucketIndex);
				check(FollowRec.ISKMC);
				FollowRec.ISKMC->RemoveInstance(FollowRec.InstanceId);
				{
					FMeshBucket& FollowBucket = Buckets[FollowBucketIndex];
					FollowBucket.NumInstances = FMath::Max(0, FollowBucket.NumInstances - 1);
					if (FollowBucket.Shards.IsValidIndex(FollowShardIndex))
					{
						FSkinnedShard& FollowShard = FollowBucket.Shards[FollowShardIndex];
						FollowShard.NumInstances = FMath::Max(0, FollowShard.NumInstances - 1);
					}
				}
				CleanupShardIfEmpty(FollowBucketIndex, FollowShardIndex);
				CleanupBucketIfEmpty(FollowBucketIndex);

				FollowRec.ISKMC = nullptr;
				FollowRec.InstanceId = FPrimitiveInstanceId();
				FollowRec.BucketIndex = INDEX_NONE;
				FollowRec.ShardIndex = INDEX_NONE;
			}

			FollowRec.CpuFollowSkinnedMesh = FPrivateUtils::CreateCpuFollowComponent(Rec->CpuProxyActor, Leader, FollowRec.SkeletalMesh);
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
	check(Rec->BucketIndex != INDEX_NONE && Rec->ShardIndex != INDEX_NONE && Rec->SlotIndex != INDEX_NONE);

	checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG CPU->GPU: invalid BucketIndex=%d."), Rec->BucketIndex);
	FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	check(Bucket.Shards.IsValidIndex(Rec->ShardIndex));
	FSkinnedShard& Shard = Bucket.Shards[Rec->ShardIndex];
	const int32 SlotIndex = Rec->SlotIndex;

	// Convert CPU follow components back to GPU follow instances first (Handle stability).
	if (Rec->FollowRecordIndices.Num() > 0)
	{
		for (const int32 FollowIndex : Rec->FollowRecordIndices)
		{
			check(AnimRecords.IsValidIndex(FollowIndex));
			const FInstancedAnimRecord& FollowRec = AnimRecords[FollowIndex];
		}

		TRefCountPtr<FGIAG_TransformProviderState> MasterState = Shard.TransformProvider ? Shard.TransformProvider->GetState() : nullptr;
		check(MasterState.IsValid());
		const int32 MasterSlotCapacity = Shard.SlotCapacity;

		for (const int32 FollowIndex : Rec->FollowRecordIndices)
		{
			check(AnimRecords.IsValidIndex(FollowIndex));
			FInstancedAnimRecord& FollowRec = AnimRecords[FollowIndex];
			check(FollowRec.MasterRecordIndex == Handle.RecordIndex);

			FPrivateUtils::DestroyCpuFollowComponent(FollowRec.CpuFollowSkinnedMesh);
			check(!FollowRec.CpuFollowSkinnedMesh);
			check(!FollowRec.ISKMC);

			// Rebuild follower shard + instance (copy of AddFollowInstance GPU path, but without adding a record).
			USkeletalMesh* FollowMesh = FollowRec.SkeletalMesh;
			check(FollowMesh);

			const int32 FollowBucketIndex = FindOrCreateBucket(FollowMesh, Rec->GroupIndex);
			checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG CPU->GPU: invalid follow BucketIndex=%d."), FollowBucketIndex);
			FMeshBucket& FollowBucket = Buckets[FollowBucketIndex];
			check(FollowBucket.Shards.Num() > 0);

			const FReferenceSkeleton& DstRef = FollowMesh->GetRefSkeleton();
			const int32 DstNumBones = DstRef.GetNum();
			const FReferenceSkeleton& SrcRef = Group.Skeleton->GetReferenceSkeleton();
			const int32 SrcBones = SrcRef.GetNum();

			// Optional remap: share a cached table across all follows of (DstMesh, SrcSkeleton).
			TSharedPtr<const TArray<uint32>> RemapShared;
			const uint32* RemapPtr = nullptr;
			{
				const FFollowBoneRemapKey Key{ FollowMesh, Group.Skeleton };
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

			EnsureHostActor();

			// Find a shared follower shard already bound to this master (Bridge, remap ptr, bone counts).
			int32 FollowShardIndex = INDEX_NONE;
			for (auto ShardIt = FollowBucket.Shards.CreateIterator(); ShardIt; ++ShardIt)
			{
				const int32 ShardIndex = ShardIt.GetIndex();
				FSkinnedShard& FollowShard = *ShardIt;
				check(FollowShard.ISKMC);
				check(FollowShard.TransformProvider);
				if (FollowShard.TransformProvider->GetMode() != EGIAG_TransformProviderMode::FollowerCopyOrRemap)
				{
					continue;
				}
				if (FollowShard.TransformProvider->GetMasterState() != MasterState)
				{
					continue;
				}
				if (FollowShard.TransformProvider->AnimationSlotCount != MasterSlotCapacity)
				{
					continue;
				}
				if (FollowShard.TransformProvider->GetNumBones() != DstNumBones || FollowShard.TransformProvider->GetSrcNumBones() != SrcBones)
				{
					continue;
				}
				if (FollowShard.TransformProvider->GetBoneRemapPtr() != RemapPtr)
				{
					continue;
				}
				FollowShardIndex = ShardIndex;
				break;
			}
			if (FollowShardIndex == INDEX_NONE)
			{
				FollowShardIndex = FPrivateUtils::AddFollowerShardToBucket(FollowBucket, Group, *HostActor, *FollowMesh, MasterSlotCapacity, MasterState, DstNumBones, SrcBones, RemapShared);
			}
			check(FollowBucket.Shards.IsValidIndex(FollowShardIndex));

			FSkinnedShard& FollowShard = FollowBucket.Shards[FollowShardIndex];
			check(FollowShard.TransformProvider);

			const int32 AnimIndex = SlotIndex;
			check(AnimIndex >= 0 && AnimIndex < MasterSlotCapacity);

			const FTransform SpawnTransform = Shard.TransformBySlot[AnimIndex];
			const FPrimitiveInstanceId InstanceId = FollowShard.ISKMC->AddInstance(SpawnTransform, AnimIndex, true);
			FollowShard.NumInstances += 1;
			FollowBucket.NumInstances++;

			FollowRec.ISKMC = FollowShard.ISKMC;
			FollowRec.InstanceId = InstanceId;
			FollowRec.BucketIndex = FollowBucketIndex;
			FollowRec.ShardIndex = FollowShardIndex;
			FollowRec.GroupIndex = Rec->GroupIndex;
			FollowRec.SlotIndex = AnimIndex;
		}
	}

	FTransform SpawnTransform = Shard.TransformBySlot[SlotIndex];
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

	check(Shard.ISKMC);
	const FPrimitiveInstanceId InstanceId = Shard.ISKMC->AddInstance(SpawnTransform, SlotIndex, true);
	Shard.NumInstances += 1;
	Shard.TransformBySlot[SlotIndex] = SpawnTransform;
	// Slot is (re)entering GPU evaluation; initialize previous=current once to avoid velocity spikes from stale/uninitialized GPU buffer.
	Shard.NewSlotsThisTick.Add((uint32)SlotIndex);
	if (!Shard.TransformDirty[SlotIndex])
	{
		Shard.TransformDirty[SlotIndex] = true;
		Shard.DirtyTransformSlots.Add((uint32)SlotIndex);
	}

	FPrivateUtils::RemoveSlotFromList(Shard.CpuAliveSlots, Shard.CpuAliveListIndexBySlot, SlotIndex);
	FPrivateUtils::AddSlotToList(Shard.GpuAliveSlots, Shard.GpuAliveListIndexBySlot, SlotIndex);

	// Ensure GPU AnimLibrary data exists for any clips referenced by this instance.
	// (CPU may have allocated clip indices via RequestClipIndexOnly without baking pixels.)
	const double NowSeconds = GetWorld() ? (double)GetWorld()->GetTimeSeconds() : 0.0;
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
			const void* NodePtr = Shard.GetNodePtr(NodeIdx, SlotIndex);
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

	Rec->ISKMC = Shard.ISKMC;
	Rec->InstanceId = InstanceId;
	if (Shard.TransformDirty[SlotIndex])
	{
		Shard.DirtyTransformSlots.Add(SlotIndex);
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
		check(MeshBucket.Shards.IsValidIndex(Rec.ShardIndex));
		FSkinnedShard& Shard = MeshBucket.Shards[Rec.ShardIndex];
		checkf(Rec.SlotIndex >= 0 && Rec.SlotIndex < Shard.SlotCapacity, TEXT("GIAG AttachSync: invalid SlotIndex=%d."), Rec.SlotIndex);

		// Use the CPU pose cache produced earlier in the frame.
		FGameInstancedAnimationGraphHandle Handle;
		Handle.InstancedAnimSubsystem = this;
		Handle.RecordIndex = RecordIndex;
		Handle.SerialNumber = AnimRecordSerials[RecordIndex];

		uint64 Frame = 0;
		TConstArrayView<FTransform3f> LocalPose;
		if (!TryGetCpuPoseCache_NoLock(Handle, Frame, LocalPose))
		{
			continue;
		}

		TArray<FTransform3f> ComponentPose;
		ConvertLocalPoseToComponentPoseChecked(Handle, LocalPose, ComponentPose);

		const FTransform3f C2W = FTransform3f(Shard.TransformBySlot[Rec.SlotIndex]);

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

void UGameInstancedAnimationGraphSubsystem::RemoveInstance(FGameInstancedAnimationGraphHandle& Handle)
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
				FGameInstancedAnimationGraphHandle FollowHandle;
				FollowHandle.InstancedAnimSubsystem = this;
				FollowHandle.RecordIndex = FollowIndex;
				FollowHandle.SerialNumber = AnimRecordSerials[FollowIndex];
				RemoveInstance(FollowHandle);
			}
		}

		const int32 BucketIndex = Rec->BucketIndex;
		const int32 ShardIndex = Rec->ShardIndex;
		const int32 SlotIndex = Rec->SlotIndex;
		const int32 GroupIndex = Rec->GroupIndex;
		AActor* ProxyActor = Rec->CpuProxyActor;

		checkf(Buckets.IsValidIndex(BucketIndex), TEXT("GIAG CPU: invalid BucketIndex=%d."), BucketIndex);
		FMeshBucket& Bucket = Buckets[BucketIndex];
		check(Bucket.Shards.IsValidIndex(ShardIndex));
		FSkinnedShard& Shard = Bucket.Shards[ShardIndex];

		checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG CPU: invalid GroupIndex=%d."), GroupIndex);
		FGraphGroup& Group = Groups[GroupIndex];

		checkf(SlotIndex >= 0 && SlotIndex < Shard.SlotCapacity, TEXT("GIAG CPU: invalid SlotIndex=%d."), SlotIndex);
		check(Shard.SlotAlive[SlotIndex]);

		// Destroy per-node AoS data for this slot.
		check(Group.Compiled);
		check(Group.NodeProperties.Num() == Group.Compiled->NumNodes);
		for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
		{
			const FStructProperty* SP = Group.NodeProperties[NodeIdx];
			check(SP && SP->Struct);
			uint8* NodePtr = Shard.GetNodePtr(NodeIdx, SlotIndex);
			SP->Struct->DestroyStruct(NodePtr);
			FMemory::Memzero(NodePtr, (SIZE_T)Shard.NodeStrideBytes[NodeIdx]);
		}

		Shard.SlotAlive[SlotIndex] = false;
		Shard.RecordIndexBySlot[SlotIndex] = INDEX_NONE;
		Shard.FreeSlots.Add(SlotIndex);
		Shard.TransformBySlot[SlotIndex] = FTransform::Identity;

		FPrivateUtils::RemoveSlotFromList(Shard.CpuAliveSlots, Shard.CpuAliveListIndexBySlot, SlotIndex);

		ProxyActor->Destroy();
		ProxyActor = nullptr;
		Bucket.NumInstances = FMath::Max(0, Bucket.NumInstances - 1);

		const int32 RecordIndex = Handle.RecordIndex;
		CpuPoseCacheByRecordIndex.Remove(RecordIndex);
		AnimRecords.RemoveAt(RecordIndex);
		InvalidateHandle(Handle);

		CleanupShardIfEmpty(BucketIndex, ShardIndex);
		CleanupBucketIfEmpty(BucketIndex);
		return;
	}

	// Follow instance: only remove the render instance; shared animation instance stays alive with master.
	if (Rec->MasterRecordIndex != INDEX_NONE)
	{
		const int32 FollowBucketIndex = Rec->BucketIndex;
		const int32 FollowShardIndex = Rec->ShardIndex;

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
			if (Bucket.Shards.IsValidIndex(FollowShardIndex))
			{
				FSkinnedShard& Shard = Bucket.Shards[FollowShardIndex];
				Shard.NumInstances = FMath::Max(0, Shard.NumInstances - 1);
			}
		}

		const int32 RecordIndex = Handle.RecordIndex;
		AnimRecords.RemoveAt(RecordIndex);
		InvalidateHandle(Handle);

		CleanupShardIfEmpty(FollowBucketIndex, FollowShardIndex);
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
			FGameInstancedAnimationGraphHandle FollowHandle;
			FollowHandle.InstancedAnimSubsystem = this;
			FollowHandle.RecordIndex = FollowIndex;
			FollowHandle.SerialNumber = AnimRecordSerials[FollowIndex];
			RemoveInstance(FollowHandle);
		}
	}

	const int32 BucketIndex = Rec->BucketIndex;
	checkf(Buckets.IsValidIndex(BucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), BucketIndex);
	FMeshBucket& Bucket = Buckets[BucketIndex];

	const int32 GroupIndex = Rec->GroupIndex;
	const int32 SlotIndex = Rec->SlotIndex;
	const int32 ShardIndex = Rec->ShardIndex;

	checkf(Groups.IsValidIndex(GroupIndex), TEXT("GIAG: invalid GroupIndex=%d."), GroupIndex);
	FGraphGroup& Group = Groups[GroupIndex];
	check(Bucket.Shards.IsValidIndex(ShardIndex));
	FSkinnedShard& Shard = Bucket.Shards[ShardIndex];
	checkf(Shard.SlotAlive.Num() == Shard.SlotCapacity && Shard.RecordIndexBySlot.Num() == Shard.SlotCapacity,
		TEXT("GIAG: shard slot arrays size mismatch (SlotAlive=%d RecordIndexBySlot=%d Cap=%d)."),
		Shard.SlotAlive.Num(), Shard.RecordIndexBySlot.Num(), Shard.SlotCapacity);
	checkf(SlotIndex >= 0 && SlotIndex < Shard.SlotCapacity, TEXT("GIAG: invalid SlotIndex=%d."), SlotIndex);
	check(Shard.SlotAlive[SlotIndex]);

	// Destroy per-node AoS data for this slot.
	check(Group.Compiled);
	check(Group.NodeProperties.Num() == Group.Compiled->NumNodes);
	for (int32 NodeIdx = 0; NodeIdx < Group.Compiled->NumNodes; ++NodeIdx)
	{
		const FStructProperty* SP = Group.NodeProperties[NodeIdx];
		check(SP && SP->Struct);
		uint8* NodePtr = Shard.GetNodePtr(NodeIdx, SlotIndex);
		SP->Struct->DestroyStruct(NodePtr);
		FMemory::Memzero(NodePtr, (SIZE_T)Shard.NodeStrideBytes[NodeIdx]);
	}

	Shard.SlotAlive[SlotIndex] = false;
	Shard.RecordIndexBySlot[SlotIndex] = INDEX_NONE;
	Shard.FreeSlots.Add(SlotIndex);
	FPrivateUtils::RemoveSlotFromList(Shard.GpuAliveSlots, Shard.GpuAliveListIndexBySlot, SlotIndex);

	Shard.TransformBySlot[SlotIndex] = FTransform::Identity;
	if (Shard.TransformDirty[SlotIndex])
	{
		Shard.TransformDirty[SlotIndex] = false;
		Shard.DirtyTransformSlots.RemoveSingleSwap((uint32)SlotIndex, EAllowShrinking::No);
	}

	check(Rec->ISKMC != nullptr);
	Rec->ISKMC->RemoveInstance(Rec->InstanceId);
	Bucket.NumInstances = FMath::Max(0, Bucket.NumInstances - 1);
	if (Bucket.Shards.IsValidIndex(ShardIndex))
	{
		Shard.NumInstances = FMath::Max(0, Shard.NumInstances - 1);
	}

	const int32 RecordIndex = Handle.RecordIndex;
	AnimRecords.RemoveAt(RecordIndex);
	InvalidateHandle(Handle);

	CleanupShardIfEmpty(BucketIndex, ShardIndex);
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
	if (Rec->MasterRecordIndex != INDEX_NONE)
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

		// Keep shard slot transform in sync for culling and backend switching.
		checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG CPU: invalid BucketIndex=%d."), Rec->BucketIndex);
		FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
		check(Bucket.Shards.IsValidIndex(Rec->ShardIndex));
		FSkinnedShard& Shard = Bucket.Shards[Rec->ShardIndex];
		checkf(Rec->SlotIndex >= 0 && Rec->SlotIndex < Shard.SlotCapacity, TEXT("GIAG CPU: invalid SlotIndex=%d."), Rec->SlotIndex);
		Shard.TransformBySlot[Rec->SlotIndex] = NewTransform;
		Shard.TransformDirty[Rec->SlotIndex] = true;
		return;
	}

	// Prefer in-place instance transform update (keeps FPrimitiveInstanceId stable); fallback to remove+add if unavailable.
	if (Rec->ISKMC)
	{
		FPrivateUtils::UpdateISKMCInstanceTransformById(*Rec->ISKMC, Rec->InstanceId, NewTransform, true);

		checkf(Buckets.IsValidIndex(Rec->BucketIndex), TEXT("GIAG: invalid BucketIndex=%d."), Rec->BucketIndex);
		FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
		checkf(Bucket.Shards.IsValidIndex(Rec->ShardIndex),
			TEXT("GIAG: invalid ShardIndex=%d (BucketIndex=%d)."), Rec->ShardIndex, Rec->BucketIndex);
		FSkinnedShard& Shard = Bucket.Shards[Rec->ShardIndex];
		checkf(Rec->SlotIndex >= 0 && Rec->SlotIndex < Shard.SlotCapacity, TEXT("GIAG: invalid SlotIndex=%d."), Rec->SlotIndex);

		Shard.TransformBySlot[Rec->SlotIndex] = NewTransform;
		if (!Shard.TransformDirty[Rec->SlotIndex])
		{
			Shard.TransformDirty[Rec->SlotIndex] = true;
			Shard.DirtyTransformSlots.Add((uint32)Rec->SlotIndex);
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
		auto& Shard = Bucket.Shards[Rec.ShardIndex];
		return Shard.TransformBySlot[Rec.SlotIndex];
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
	const double NowSeconds = GetWorld() ? (double)GetWorld()->GetTimeSeconds() : 0.0;
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

			for (auto ShardIt = Bucket.Shards.CreateConstIterator(); ShardIt; ++ShardIt)
			{
				const FSkinnedShard& Shard = *ShardIt;
				if (!Shard.TransformProvider || Shard.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap)
				{
					continue;
				}
				if (Shard.NodeStorageByNode.Num() == 0)
				{
					continue;
				}

				for (int32 Slot = 0; Slot < Shard.SlotCapacity; ++Slot)
				{
					if (!Shard.SlotAlive.IsValidIndex(Slot) || !Shard.SlotAlive[Slot])
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
						const void* NodePtr = Shard.GetNodePtr(NodeIdx, Slot);
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

		FInstancedAnimRecord NewRec;
		NewRec.SkeletalMesh = SkeletalMesh;
		NewRec.CpuFollowSkinnedMesh = FPrivateUtils::CreateCpuFollowComponent(MasterRec->CpuProxyActor, Leader, SkeletalMesh);
		NewRec.GroupIndex = MasterRec->GroupIndex;
		NewRec.SlotIndex = MasterRec->SlotIndex;
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
		check(MasterRec);
		MasterRec->FollowRecordIndices.Add(RecordIndex);

		Handle.InstancedAnimSubsystem = this;
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
	checkf(Bucket.Shards.IsValidIndex(MasterRec->ShardIndex),
		TEXT("GIAG: invalid master ShardIndex=%d (BucketIndex=%d)."), MasterRec->ShardIndex, MasterRec->BucketIndex);
	FSkinnedShard& MasterShard = Bucket.Shards[MasterRec->ShardIndex];
	const int32 MasterSlotCapacity = MasterShard.SlotCapacity;

	const int32 FollowBucketIndex = FindOrCreateBucket(SkeletalMesh, MasterRec->GroupIndex);
	checkf(Buckets.IsValidIndex(FollowBucketIndex), TEXT("GIAG: invalid follow BucketIndex=%d."), FollowBucketIndex);
	FMeshBucket& FollowBucket = Buckets[FollowBucketIndex];
	if (FollowBucket.Shards.Num() <= 0)
	{
		return Handle;
	}

	TRefCountPtr<FGIAG_TransformProviderState> MasterState = MasterShard.TransformProvider ? MasterShard.TransformProvider->GetState() : nullptr;
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

	// Find a shared follower shard already bound to this master (Bridge, remap ptr, bone counts).
	int32 FollowShardIndex = INDEX_NONE;
	for (auto ShardIt = FollowBucket.Shards.CreateIterator(); ShardIt; ++ShardIt)
	{
		const int32 ShardIndex = ShardIt.GetIndex();
		FSkinnedShard& Shard = *ShardIt;
		check(Shard.ISKMC);
		check(Shard.TransformProvider);
		if (Shard.TransformProvider->GetMode() != EGIAG_TransformProviderMode::FollowerCopyOrRemap)
		{
			continue;
		}
		if (Shard.TransformProvider->GetMasterState() != MasterState)
		{
			continue;
		}
		if (Shard.TransformProvider->AnimationSlotCount != MasterSlotCapacity)
		{
			continue;
		}
		if (Shard.TransformProvider->GetNumBones() != DstNumBones || Shard.TransformProvider->GetSrcNumBones() != SrcBones)
		{
			continue;
		}
		if (Shard.TransformProvider->GetBoneRemapPtr() != RemapPtr)
		{
			continue;
		}
		FollowShardIndex = ShardIndex;
		break;
	}
	if (FollowShardIndex == INDEX_NONE)
	{
		FollowShardIndex = FPrivateUtils::AddFollowerShardToBucket(FollowBucket, Group, *HostActor, *SkeletalMesh, MasterSlotCapacity, MasterState, DstNumBones, SrcBones, RemapShared);;
	}
	if (FollowShardIndex == INDEX_NONE || !FollowBucket.Shards.IsValidIndex(FollowShardIndex))
	{
		return Handle;
	}

	FSkinnedShard& FollowShard = FollowBucket.Shards[FollowShardIndex];
	check(FollowShard.TransformProvider);

	const int32 AnimIndex = MasterRec->SlotIndex;
	checkf(AnimIndex >= 0 && AnimIndex < MasterSlotCapacity, TEXT("GIAG: invalid master SlotIndex=%d for follow."), AnimIndex);

	// Spawn follow instance at master's transform, but select animation slot by AnimationIndex==MasterRec->SlotIndex.
	const FTransform SpawnTransform = MasterShard.TransformBySlot[AnimIndex];
	const FPrimitiveInstanceId InstanceId = FollowShard.ISKMC->AddInstance(SpawnTransform, AnimIndex, true);
	FollowShard.NumInstances += 1;
	FollowBucket.NumInstances++;

	FInstancedAnimRecord NewRec;
	NewRec.SkeletalMesh = SkeletalMesh;
	NewRec.ISKMC = FollowShard.ISKMC;
	NewRec.InstanceId = InstanceId;
	NewRec.BucketIndex = FollowBucketIndex;
	NewRec.ShardIndex = FollowShardIndex;
	NewRec.GroupIndex = MasterRec->GroupIndex;
	NewRec.SlotIndex = AnimIndex;
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

	Handle.InstancedAnimSubsystem = this;
	Handle.RecordIndex = RecordIndex;
	Handle.SerialNumber = Serial;
	return Handle;
}

void UGameInstancedAnimationGraphSubsystem::Tick(float DeltaTime)
{
	const bool bDoGPU = !GIAG::IsNullRHI();

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UGameInstancedAnimationGraphSubsystem::Tick"), STAT_UGameInstancedAnimSubsystem_Tick, STATGROUP_GameInstancedAnim);

	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;

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
	}
	bEnableFrustumCull = ViewFrustum.IsSet();
	const float CullPadding = FMath::Max(0.0f, FrustumCullPadding);
	const float MinRadius = FMath::Max(0.0f, FrustumCullMinRadius);

	const bool bDebugShards = (CVar_InstancedAnimDebugShards.GetValueOnGameThread() > 0);

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

		// Evaluate each bucket shard independently (slot capacity is per-shard).
		for (const int32 BucketIndex : Group.BucketIndices)
		{
			FMeshBucket& Bucket = Buckets[BucketIndex];
			check(Bucket.GroupIndex == GroupIndex);

			int32 DebugTotalAlive = 0;
			int32 DebugTotalActive = 0;

			for (FSkinnedShard& Shard : Bucket.Shards)
			{
				if (bSkeletonStaticRebuilt)
				{
					Shard.bSentSkeletonStaticUpload = false;
				}

				// Follower shard: does not participate in GIAG evaluation (it reuses master's TransformBuffer on RT).
				if (Shard.TransformProvider && Shard.TransformProvider->GetMode() == EGIAG_TransformProviderMode::FollowerCopyOrRemap)
				{
					continue;
				}

				const int32 SlotCapacity = Shard.SlotCapacity;
				checkf(Shard.SlotAlive.Num() == SlotCapacity && Shard.RecordIndexBySlot.Num() == SlotCapacity,
					TEXT("GIAG: shard slot arrays size mismatch (SlotAlive=%d RecordIndexBySlot=%d Cap=%d)."),
					Shard.SlotAlive.Num(), Shard.RecordIndexBySlot.Num(), SlotCapacity);

				if (!bDoGPU)
				{
					continue;
				}

				check(Shard.ISKMC);
				check(Shard.TransformProvider);
				check(Shard.TransformProvider->GetState().IsValid());

				// ---------------- GPU backend ----------------
				if (Shard.GpuAliveSlots.Num() == 0)
				{
					continue;
				}

				DebugTotalAlive += Shard.GpuAliveSlots.Num();

				// Filter to active GPU slots (frustum cull) into persistent array.
				Shard.GpuActiveInstanceIndices.Reset();
				bool bPreserve = false;
				if (!bEnableFrustumCull)
				{
					Shard.GpuActiveInstanceIndices = Shard.GpuAliveSlots;
				}
				else
				{
					Shard.GpuActiveInstanceIndices.Reserve(Shard.GpuAliveSlots.Num());
					for (const uint32 SlotU : Shard.GpuAliveSlots)
					{
						const int32 Slot = (int32)SlotU;
						const FVector Center = Shard.TransformBySlot[Slot].GetLocation();

						float Radius = MinRadius;
						const float Scale3D = Shard.TransformBySlot[Slot].GetScale3D().GetAbsMax();
						Radius = FMath::Max(Radius, (float)Bucket.BoundSphereRadius * Scale3D);
						Radius += CullPadding;

						if (ViewFrustum->IntersectSphere(Center, Radius))
						{
							Shard.GpuActiveInstanceIndices.Add(SlotU);
						}
					}

					if (Shard.GpuActiveInstanceIndices.Num() > 0 && Shard.GpuActiveInstanceIndices.Num() < Shard.GpuAliveSlots.Num())
					{
						bPreserve = true;
					}
				}

				FGIAG_AnimGraphRunParams Params;
				Params.NumInstances = Shard.GpuActiveInstanceIndices.Num();
				Params.SlotCapacity = SlotCapacity;
				Params.NumBones = Group.NumBones;
				Params.CurrentTimeSeconds = Now;
				Params.Skeleton = Group.Skeleton;
				Params.ParentIndices = &Cache->ParentIndices;
				Params.InverseRefPoseTRS = &Cache->InverseRefPoseTRS;

				Params.ActiveInstanceIndices = Shard.GpuActiveInstanceIndices;
				if (DebugReadbackEnabledSerialByRecordIndex.Num() > 0 || DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Num() > 0)
				{
					Params.DebugCpuRequestFrame = GFrameCounter;
					for (const uint32 SlotU : Params.ActiveInstanceIndices)
					{
						const int32 Slot = (int32)SlotU;
						const int32 RecordIndex = Shard.RecordIndexBySlot[Slot];
						if (RecordIndex == INDEX_NONE)
						{
							continue;
						}
						const FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
						if (Rec.MasterRecordIndex != INDEX_NONE)
						{
							continue; // follower does not evaluate
						}

						if (const int32* EnabledSerialLocalPose = DebugReadbackEnabledSerialByRecordIndex.Find(RecordIndex))
						{
							if (*EnabledSerialLocalPose == AnimRecordSerials[RecordIndex])
							{
								FGIAG_LocalPoseReadbackRequest Req;
								Req.RecordIndex = RecordIndex;
								Req.SerialNumber = *EnabledSerialLocalPose;
								Req.SlotIndex = SlotU;
								Params.DebugLocalPoseReadbackRequests.Add(MoveTemp(Req));
							}
						}

						if (const int32* EnabledSerialNeedBits = DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Find(RecordIndex))
						{
							if (*EnabledSerialNeedBits == AnimRecordSerials[RecordIndex])
							{
								FGIAG_NeedNodeBitsReadbackRequest Req;
								Req.RecordIndex = RecordIndex;
								Req.SerialNumber = *EnabledSerialNeedBits;
								Req.SlotIndex = SlotU;
								Params.DebugNeedNodeBitsReadbackRequests.Add(MoveTemp(Req));
							}
						}
					}
				}
				DebugTotalActive += Params.NumInstances;
				Params.AnimLibraryUpload = Cache->PendingAnimLibraryUpload;
				Params.AnimLibraryVersion = Cache->AnimLibraryVersion;
				Params.NumClips = (uint32)Cache->ClipSlots.Num();
				Params.AnimTRSNum = (uint32)Cache->AnimTRSCapacity;

				checkf(Params.AnimLibraryVersion != 0 && Params.NumClips != 0 && Params.AnimTRSNum != 0 && Group.NumBones > 0,
					TEXT("GIAG: AnimLibrary not ready for Skeleton '%s'."), *GetNameSafe(Cache->Skeleton));

				if (Params.NumInstances <= 0)
				{
					continue;
				}

				// GT -> RT: pack dirty transforms (RT computes World->Component and uploads sparse ranges).
				// Also pass "new slot" flags to initialize previous=current on first frame (velocity correctness).
				if (Shard.DirtyTransformSlots.Num() > 0 || Shard.NewSlotsThisTick.Num() > 0)
				{
					auto TransformUpload = MakeShared<FGIAG_TransformUploadData>();
					TransformUpload->SlotCapacity = (uint32)SlotCapacity;

					TransformUpload->DirtySlots.Reserve(Shard.DirtyTransformSlots.Num());
					TransformUpload->DirtyComponentToWorld.Reserve(Shard.DirtyTransformSlots.Num());

					for (const int32 Slot : Shard.DirtyTransformSlots)
					{
						checkf(Slot < SlotCapacity, TEXT("GIAG: invalid dirty slot %d (Cap=%d)."), Slot, SlotCapacity);
						if (Shard.TransformDirty[Slot] == false)
						{
							continue;
						}

						Shard.TransformDirty[Slot] = false;
						TransformUpload->DirtyComponentToWorld.Add(FTransform3f(Shard.TransformBySlot[Slot]));
						TransformUpload->DirtySlots.Add(Slot);
					}
					Shard.DirtyTransformSlots.Reset();

					if (Shard.NewSlotsThisTick.Num() > 0)
					{
						TransformUpload->InitPrevBySlot.SetNumZeroed(SlotCapacity);
						for (const uint32 NewSlotU : Shard.NewSlotsThisTick)
						{
							const int32 NewSlot = (int32)NewSlotU;
							checkf(NewSlot >= 0 && NewSlot < SlotCapacity, TEXT("GIAG: invalid NewSlot=%d (Cap=%d)."), NewSlot, SlotCapacity);
							TransformUpload->InitPrevBySlot[NewSlot] = 1u;
						}
						Shard.NewSlotsThisTick.Reset();
					}

					Params.TransformUpload = TransformUpload;
				}

				// One-time (or rebuild) skeleton upload: driven by shard-local GT flag.
				const bool bUploadSkeletonStatic = !Shard.bSentSkeletonStaticUpload;
				FGIAG_AnimGraphUploads Uploads = Shard.UploadBuilder.BuildUploads_GameThread(
					*Group.Compiled,
					Shard.NodeBasePtrsByNode,
					Shard.NodeStrideBytes,
					SlotCapacity,
					Params,
					Shard.DirtyNodeIndices,
					Shard.DirtyNodeMask,
					Shard.NodeParamDirtyBitsByNode,
					Shard.DirtyNodeParamSlotsByNode,
					bUploadSkeletonStatic,
					Cache->ParentIndices,
					Cache->InverseRefPoseTRS);

				// Optional shared resources: GT provides the per-node key mapping; RT uploads/binds from global cache.
				Uploads.MaxOptionalSRVSlot = Group.MaxOptionalSRVSlot;
				if (Group.MaxOptionalSRVSlot >= 0)
				{
					Uploads.OptionalSRVKeyByNodeBySlot = Group.OptionalSRVKeyByNodeBySlot;
				}
				if (bUploadSkeletonStatic)
				{
					Shard.bSentSkeletonStaticUpload = true;
				}

				FGIAG_RenderPayload Payload;
				Payload.Compiled = Group.Compiled;
				Payload.Params = Params;
				Payload.Uploads = MoveTemp(Uploads);

				TRefCountPtr<FGIAG_TransformProviderState> State = Shard.TransformProvider->GetState();
				ENQUEUE_RENDER_COMMAND(GIAG_SetProviderPayload)(
					[State, Payload = MoveTemp(Payload)](FRHICommandListImmediate& RHICmdList) mutable
					{
						if (State.IsValid())
						{
							State->EnqueuePayload_RenderThread(MoveTemp(Payload));
						}
					});
			}

			if (bDebugShards)
			{
				UE_LOG(LogTemp, Log, TEXT("GIAG GT: Group=%d Bucket=%d Shards=%d AliveSlots=%d ActiveSlots=%d Cull=%d"),
					GroupIndex, BucketIndex, Bucket.Shards.Num(), DebugTotalAlive, DebugTotalActive, (int32)bEnableFrustumCull);
			}
		}
	}
}

