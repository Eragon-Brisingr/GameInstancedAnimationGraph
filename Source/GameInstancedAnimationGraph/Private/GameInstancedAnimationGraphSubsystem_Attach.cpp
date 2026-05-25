#include "GameInstancedAnimationGraphSubsystem.h"
#include "GameInstancedAnimationGraphSettings.h"
#include "GIAG_ActorInterface.h"
#include "GIAG_TransformProviderData.h"
#include "GIAG_AttachMeshComponent.h"

#include "Components/StaticMeshComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

int32 UGameInstancedAnimationGraphSubsystem::FindOrCreateNiagaraAttachBucket(UStaticMesh* StaticMesh, UNiagaraSystem* NiagaraSystem)
{
	check(IsInGameThread());
	check(NiagaraSystem);

	EnsureHostActor();
	check(HostActor);

	const FNiagaraAttachBucketKey Key{ StaticMesh, NiagaraSystem };
	if (const int32* Found = NiagaraAttachBucketIndexByKey.Find(Key))
	{
		return *Found;
	}

	FNiagaraAttachBucket Bucket;
	Bucket.BucketId = NextAttachBucketId++;
	Bucket.StaticMesh = StaticMesh;
	Bucket.NiagaraSystem = NiagaraSystem;

	UNiagaraComponent* Comp = NewObject<UNiagaraComponent>(HostActor);
	check(Comp);
	Comp->CreationMethod = EComponentCreationMethod::Instance;
	Comp->SetTickBehavior(ENiagaraTickBehavior::ForceTickLast);
	Comp->SetAsset(NiagaraSystem);
	Comp->SetAutoActivate(false);
	static const FName InternalBucketIdName = TEXT("User.GIAG_InternalAttachBucketId");
	Comp->SetVariableInt(InternalBucketIdName, (int32)Bucket.BucketId);
	if (Bucket.StaticMesh)
	{
		static const FName AttachStaticMeshName = TEXT("User.GIAG_AttachStaticMesh");
		Comp->SetVariableStaticMesh(AttachStaticMeshName, Bucket.StaticMesh);
	}
	Bucket.NiagaraComponent = Comp;
	InitNiagaraBucketMeta(Bucket);

	const int32 NewIndex = NiagaraAttachBuckets.Add(MoveTemp(Bucket));
	NiagaraAttachBucketIndexByKey.Add(Key, NewIndex);
	AttachBucketById.Add(NiagaraAttachBuckets[NewIndex].BucketId, FAttachBucketLocator{ EAttachBucketType::Niagara, NewIndex });
	NiagaraAttachBucketIdByComponent.Add(NiagaraAttachBuckets[NewIndex].NiagaraComponent, NiagaraAttachBuckets[NewIndex].BucketId);

	Comp->RegisterComponent();
	Comp->Activate(true);
	return NewIndex;
}

uint32 UGameInstancedAnimationGraphSubsystem::ResolveNiagaraAttachBucketIdOrZero(const UNiagaraComponent* NiagaraComponent) const
{
	check(IsInGameThread());
	if (!NiagaraComponent)
	{
		return 0u;
	}
	if (const uint32* Found = NiagaraAttachBucketIdByComponent.Find(NiagaraComponent))
	{
		return *Found;
	}
	return 0u;
}

bool UGameInstancedAnimationGraphSubsystem::NiagaraAttach_GetSpawnVersionAndCount(uint32 BucketId, uint32& OutAddListVersion, uint32& OutAddListCount) const
{
	check(AttachBus);
	return AttachBus->NiagaraVm_GetAddList_AnyThread(BucketId, OutAddListVersion, OutAddListCount);
}

int32 UGameInstancedAnimationGraphSubsystem::FindOrCreateNativeAttachBucket(UStaticMesh* StaticMesh)
{
	check(IsInGameThread());
	check(StaticMesh);

	EnsureHostActor();
	check(HostActor);

	const FNativeAttachBucketKey Key{ StaticMesh };
	if (const int32* Found = NativeAttachBucketIndexByKey.Find(Key))
	{
		return *Found;
	}

	FNativeAttachBucket Bucket;
	Bucket.BucketId = NextAttachBucketId++;
	Bucket.StaticMesh = StaticMesh;

	UGIAG_AttachMeshComponent* Comp = NewObject<UGIAG_AttachMeshComponent>(HostActor);
	check(Comp);
	Comp->CreationMethod = EComponentCreationMethod::Instance;
	Comp->SetCastShadow(true);
	Comp->SetMobility(EComponentMobility::Movable);
	Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Comp->BucketId = (int32)Bucket.BucketId;
	Comp->StaticMesh = StaticMesh;
	Comp->AttachRegistry = NativeAttachRegistry;
	Bucket.NativeMeshComponent = Comp;
	Comp->RegisterComponent();

	const int32 NewIndex = NativeAttachBuckets.Add(MoveTemp(Bucket));
	NativeAttachBucketIndexByKey.Add(Key, NewIndex);
	AttachBucketById.Add(NativeAttachBuckets[NewIndex].BucketId, FAttachBucketLocator{ EAttachBucketType::Native, NewIndex });
	return NewIndex;
}

void UGameInstancedAnimationGraphSubsystem::InitNiagaraBucketMeta(FNiagaraAttachBucket& Bucket)
{
	check(IsInGameThread());

	const int32 SlotCap = Bucket.Slots.OutputIndexByAttachSlot.Num();
	Bucket.SlotToDenseIndex.SetNum(SlotCap);
	Bucket.SlotGenerationI32.SetNum(SlotCap);
	Bucket.FxParticleGenBySlot.SetNum(SlotCap);
	for (int32 i = 0; i < SlotCap; ++i)
	{
		Bucket.SlotToDenseIndex[i] = Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex(i) ? Bucket.Slots.OutputIndexByAttachSlot[i] : -1;
		Bucket.SlotGenerationI32[i] = Bucket.Slots.SlotGeneration.IsValidIndex(i) ? (int32)Bucket.Slots.SlotGeneration[i] : 0;
		Bucket.FxParticleGenBySlot[i] = 1; // start at 1 by convention
	}
	Bucket.SlotTableVersion = FMath::Max(1u, Bucket.SlotTableVersion + 1u);
	Bucket.bNiagaraMetaDirty = true;
}

void UGameInstancedAnimationGraphSubsystem::PublishNiagaraBucketMetaToRT(FNiagaraAttachBucket& Bucket)
{
	check(IsInGameThread());

	if (!AttachBus.IsValid())
	{
		return;
	}

	FGIAG_AttachBus::FPublishNiagaraMetaOp Op;
	Op.BucketId = Bucket.BucketId;
	Op.SlotTableVersion = Bucket.SlotTableVersion;
	Op.AddListVersion = Bucket.AddListVersion;
	Op.AddListCount = (uint32)Bucket.PublishedAddsPacked.Num();

	// Publish slot tables only when dirty.
	if (Bucket.bNiagaraMetaDirty)
	{
		Op.SlotToDenseIndex = MakeShared<TArray<int32>, ESPMode::ThreadSafe>(Bucket.SlotToDenseIndex);
		Op.SlotGeneration = MakeShared<TArray<int32>, ESPMode::ThreadSafe>(Bucket.SlotGenerationI32);
		Op.FxParticleGenBySlot = MakeShared<TArray<int32>, ESPMode::ThreadSafe>(Bucket.FxParticleGenBySlot);
	}

	// Publish add-list only when we bumped version this frame.
	if (Bucket.bNiagaraAddListDirty)
	{
		Op.AddListPacked = MakeShared<TArray<int32>, ESPMode::ThreadSafe>(Bucket.PublishedAddsPacked);
		Op.AddListCount = (uint32)Bucket.PublishedAddsPacked.Num();
	}

	AttachBus->Enqueue_GameThread(MoveTemp(Op));

	Bucket.bNiagaraMetaDirty = false;
	Bucket.bNiagaraAddListDirty = false;
}

void UGameInstancedAnimationGraphSubsystem::FlushNiagaraAttachBuckets_GameThread()
{
	check(IsInGameThread());

	// Flush at most once per frame.
	const uint64 Frame = GFrameCounter;
	if (NiagaraAttachLastFlushFrame == Frame)
	{
		return;
	}
	NiagaraAttachLastFlushFrame = Frame;

	for (FNiagaraAttachBucket& Bucket : NiagaraAttachBuckets)
	{
		const int32 NumPendingAdds = Bucket.PendingAddsPacked.Num();
		if (!Bucket.bNiagaraMetaDirty && NumPendingAdds == 0)
		{
			continue;
		}

		// If we have new add events this frame, bump version so Niagara can detect "new list",
		// and copy the list into PublishedAddsPacked (kept alive for at least one Niagara tick).
		if (NumPendingAdds > 0)
		{
			Bucket.AddListVersion = FMath::Max(1u, Bucket.AddListVersion + 1u);
			Bucket.PublishedAddsPacked = Bucket.PendingAddsPacked;
			Bucket.bNiagaraAddListDirty = true;
		}

		PublishNiagaraBucketMetaToRT(Bucket);

		Bucket.PendingAddsPacked.Reset();
	}
}

void UGameInstancedAnimationGraphSubsystem::NiagaraAttach_KillGpuParticleBySlot(uint32 BucketId, uint16 AttachSlot)
{
	check(IsInGameThread());
	const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
	if (!Loc || Loc->Type != EAttachBucketType::Niagara || !NiagaraAttachBuckets.IsValidIndex(Loc->Index))
	{
		return;
	}
	FNiagaraAttachBucket& Bucket = NiagaraAttachBuckets[Loc->Index];
	if (!Bucket.FxParticleGenBySlot.IsValidIndex((int32)AttachSlot))
	{
		return;
	}
	// Kill by generation mismatch: bump FxGen and publish slot table update.
	Bucket.FxParticleGenBySlot[(int32)AttachSlot] = FMath::Max(1, Bucket.FxParticleGenBySlot[(int32)AttachSlot] + 1);
	Bucket.SlotTableVersion = FMath::Max(1u, Bucket.SlotTableVersion + 1u);
	Bucket.bNiagaraMetaDirty = true;
}

void UGameInstancedAnimationGraphSubsystem::NiagaraAttach_SpawnGpuParticleBySlot(uint32 BucketId, uint16 AttachSlot)
{
	check(IsInGameThread());
	const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
	if (!Loc || Loc->Type != EAttachBucketType::Niagara || !NiagaraAttachBuckets.IsValidIndex(Loc->Index))
	{
		return;
	}
	FNiagaraAttachBucket& Bucket = NiagaraAttachBuckets[Loc->Index];
	if (!Bucket.FxParticleGenBySlot.IsValidIndex((int32)AttachSlot))
	{
		return;
	}
	const int32 FxGen = Bucket.FxParticleGenBySlot[(int32)AttachSlot];
	const int32 PackedSlotFxGen = (FxGen << 16) | (int32)AttachSlot;
	Bucket.PendingAddsPacked.Add(PackedSlotFxGen);
	Bucket.bNiagaraAddListDirty = true;
}

FGameInstancedAnimationAttachHandle UGameInstancedAnimationGraphSubsystem::AttachStaticMesh_Backend(const FGameInstancedAnimationGraphHandle& Handle, UStaticMesh* StaticMesh, UNiagaraSystem* NiagaraSystem, FName BoneName, const FTransform& SocketLocalTransform)
{
	check(IsInGameThread());
	const FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return {};
	}

	// GPU backend only; master or follower are both fine as long as they have a bucket slot and provider state.
	if (Rec->BucketIndex == INDEX_NONE || Rec->SlotIndex == INDEX_NONE)
	{
		return {};
	}
	if (!Buckets.IsValidIndex(Rec->BucketIndex))
	{
		return {};
	}
	const FMeshBucket& Bucket = Buckets[Rec->BucketIndex];
	if (!Bucket.TransformProvider || !Bucket.TransformProvider->GetState().IsValid())
	{
		return {};
	}

	USkeleton* Skeleton = Rec->SkeletalMesh ? Rec->SkeletalMesh->GetSkeleton() : nullptr;
	const int32 ResolvedBoneIndex = (Skeleton && !BoneName.IsNone())
		? Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName)
		: INDEX_NONE;
	if (ResolvedBoneIndex == INDEX_NONE)
	{
		return {};
	}

	const bool bNativeBucket = (NiagaraSystem == nullptr);

	uint32 BucketId = 0;
	FAttachSlotTable* SlotTable = nullptr;
	TArray<FAttachEntry>* EntryArray = nullptr;
	FNiagaraAttachBucket* NiagaraBucket = nullptr;

	if (bNativeBucket)
	{
		check(StaticMesh);
		const int32 BucketIdx = FindOrCreateNativeAttachBucket(StaticMesh);
		FNativeAttachBucket& AttachBucket = NativeAttachBuckets[BucketIdx];
		BucketId = AttachBucket.BucketId;
		SlotTable = &AttachBucket.Slots;
		EntryArray = &AttachBucket.Entries;
	}
	else
	{
		const int32 BucketIdx = FindOrCreateNiagaraAttachBucket(StaticMesh, NiagaraSystem);
		FNiagaraAttachBucket& AttachBucket = NiagaraAttachBuckets[BucketIdx];
		BucketId = AttachBucket.BucketId;
		SlotTable = &AttachBucket.Slots;
		EntryArray = &AttachBucket.Entries;
		NiagaraBucket = &AttachBucket;
	}
	check(SlotTable && EntryArray);

	auto AllocStableSlot = [&](FAttachSlotTable& Slots, uint16& OutSlot, uint16& OutGen, bool& bOutReused)
	{
		// Allocate a stable attach slot (reused via free-list; generation prevents ABA when reused in the same frame).
		bOutReused = false;
		if (Slots.FreeSlots.Num() > 0)
		{
			OutSlot = Slots.FreeSlots.Pop(EAllowShrinking::No);
			bOutReused = true;
			if (!Slots.SlotGeneration.IsValidIndex((int32)OutSlot))
			{
				Slots.SlotGeneration.SetNum((int32)OutSlot + 1);
			}
			OutGen = (uint16)(Slots.SlotGeneration[(int32)OutSlot] + 1u);
			Slots.SlotGeneration[(int32)OutSlot] = OutGen;
		}
		else
		{
			const int32 NewSlot = Slots.OutputIndexByAttachSlot.Num();
			check(NewSlot >= 0 && NewSlot <= 0xFFFF);
			OutSlot = (uint16)NewSlot;
			Slots.OutputIndexByAttachSlot.Add(INDEX_NONE);
			Slots.SlotGeneration.Add(1);
			OutGen = 1;
		}
		check(Slots.OutputIndexByAttachSlot.IsValidIndex((int32)OutSlot));
		check(Slots.OutputIndexByAttachSlot[(int32)OutSlot] == INDEX_NONE);
	};

	uint16 AttachSlot = 0;
	uint16 AttachGen = 1;
	bool bSlotReused = false;
	AllocStableSlot(*SlotTable, AttachSlot, AttachGen, bSlotReused);

	const int32 OutputIndex = EntryArray->Num();

	FAttachEntry Entry;
	Entry.AttachSlot = AttachSlot;
	Entry.AttachGeneration = AttachGen;
	Entry.Skeleton = Skeleton;
	Entry.State = Bucket.SharedState.GetReference();
	Entry.SlotIndex = (uint32)Rec->SlotIndex;
	Entry.BoneName = BoneName;
	Entry.BoneIndex = (uint32)ResolvedBoneIndex;
	static constexpr uint32 SkipGpuWriteFlag = 1u; // must match HLSL GIAG_AttachFlag_SkipGpuWrite
	Entry.DescFlags = (Rec->CpuProxyActor != nullptr) ? SkipGpuWriteFlag : 0u;
	Entry.SocketLocalTRS = FTransform3f(SocketLocalTransform);

	EntryArray->Add(Entry);
	(*SlotTable).OutputIndexByAttachSlot[(int32)AttachSlot] = OutputIndex;

	// ---- Update Niagara user arrays (event-driven) ----
	if (NiagaraBucket)
	{
		// Ensure meta arrays can address AttachSlot.
		if (NiagaraBucket->SlotToDenseIndex.Num() <= (int32)AttachSlot)
		{
			const int32 NewCap = (int32)AttachSlot + 1;
			const int32 OldCap = NiagaraBucket->SlotToDenseIndex.Num();
			NiagaraBucket->SlotToDenseIndex.SetNum(NewCap);
			NiagaraBucket->SlotGenerationI32.SetNum(NewCap);
			NiagaraBucket->FxParticleGenBySlot.SetNum(NewCap);
			for (int32 i = OldCap; i < NewCap; ++i)
			{
				NiagaraBucket->SlotToDenseIndex[i] = -1;
				NiagaraBucket->SlotGenerationI32[i] = 0;
				NiagaraBucket->FxParticleGenBySlot[i] = 1;
			}
			NiagaraBucket->SlotTableVersion = FMath::Max(1u, NiagaraBucket->SlotTableVersion + 1u);
			NiagaraBucket->bNiagaraMetaDirty = true;
		}
		NiagaraBucket->SlotToDenseIndex[(int32)AttachSlot] = OutputIndex;
		NiagaraBucket->SlotGenerationI32[(int32)AttachSlot] = (int32)AttachGen;
		// Prevent ABA for GPU particles: if a Slot is reused, bump FxParticleGen so old particles kill immediately.
		if (bSlotReused && NiagaraBucket->FxParticleGenBySlot.IsValidIndex((int32)AttachSlot))
		{
			NiagaraBucket->FxParticleGenBySlot[(int32)AttachSlot] = FMath::Max(1, NiagaraBucket->FxParticleGenBySlot[(int32)AttachSlot] + 1);
		}
		NiagaraBucket->SlotTableVersion = FMath::Max(1u, NiagaraBucket->SlotTableVersion + 1u);

		// Record add event for this frame (published once per frame via flush).
		// Packed = (FxParticleGen<<16)|Slot. (FxGen is the Niagara particle lifetime generation.)
		const int32 FxGen = NiagaraBucket->FxParticleGenBySlot[(int32)AttachSlot];
		const int32 PackedSlotFxGen = (FxGen << 16) | (int32)AttachSlot;
		NiagaraBucket->PendingAddsPacked.Add(PackedSlotFxGen);
		NiagaraBucket->bNiagaraAddListDirty = true;
		NiagaraBucket->bNiagaraMetaDirty = true;
	}

	FGIAG_AttachBus::FAttachAddOp Op;
	Op.BucketId = BucketId;
	Op.BucketKind = bNativeBucket ? FGIAG_AttachBus::EBucketKind::Native : FGIAG_AttachBus::EBucketKind::Niagara;
	Op.AttachSlot = (uint32)AttachSlot;
	Op.State = Entry.State;
	Op.Desc.SlotIndex = Entry.SlotIndex;
	Op.Desc.BoneIndex = Entry.BoneIndex;
	Op.Desc.OutputIndex = (uint32)OutputIndex;
	Op.Desc.Flags = Entry.DescFlags;
	Op.Desc.SocketLocalTRS = Entry.SocketLocalTRS;
	AttachBus->Enqueue_GameThread(MoveTemp(Op));

	FGameInstancedAnimationAttachHandle Out;
	Out.BucketId = BucketId;
	Out.Slot = AttachSlot;
	Out.Generation = AttachGen;
	return Out;
}

FGameInstancedAnimationAttachHandle UGameInstancedAnimationGraphSubsystem::AttachStaticMesh(const FGameInstancedAnimationGraphHandle& Handle, UStaticMesh* StaticMesh, FName BoneName, const FTransform& SocketLocalTransform, bool bCreateCpuProxyMesh)
{
	check(IsInGameThread());

	if (StaticMesh == nullptr)
	{
		return {};
	}

	const UGameInstancedAnimationGraphSettings* Settings = GetDefault<UGameInstancedAnimationGraphSettings>();
	const bool bUseNative = Settings->bUseNativeStaticMeshAttachRenderer;

	UNiagaraSystem* NiagaraSystem = nullptr;
	if (!bUseNative)
	{
		NiagaraSystem = DefaultStaticMeshAttachmentNiagaraSystem;
		if (NiagaraSystem == nullptr)
		{
			return {};
		}
	}

	FGameInstancedAnimationAttachHandle Out = AttachStaticMesh_Backend(Handle, StaticMesh, NiagaraSystem, BoneName, SocketLocalTransform);
	if (!Out)
	{
		return {};
	}

	Out.OwnerRecordIndex = Handle.RecordIndex;
	Out.OwnerRecordSerialNumber = Handle.SerialNumber;

	if (FInstancedAnimRecord* Rec = ResolveRecord(Handle))
	{
		Rec->AttachHandles.Add(Out);
		UpdateCpuAttachSyncRegistration_GameThread(Handle.RecordIndex);
	}

	// Store per-attach CPU proxy option.
	{
		const uint32 BucketId = Out.BucketId;
		const uint16 Slot = Out.Slot;
		const uint16 Gen = Out.Generation;
		const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
		check(Loc && Loc->Index != INDEX_NONE);

		auto ApplyToBucket = [&](auto& Bucket, const bool bNativeBucket)
		{
			check(Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex((int32)Slot));
			check(Bucket.Slots.SlotGeneration.IsValidIndex((int32)Slot));
			check(Bucket.Slots.SlotGeneration[(int32)Slot] == Gen);
			const int32 OutputIndex = Bucket.Slots.OutputIndexByAttachSlot[(int32)Slot];
			check(Bucket.Entries.IsValidIndex(OutputIndex));

			FAttachEntry& Entry = Bucket.Entries[OutputIndex];
			Entry.bCreateCpuProxyStaticMesh = bCreateCpuProxyMesh;

			// If already in CPU mode, create the CPU proxy component immediately and kill Niagara GPU particles where applicable.
			if (bCreateCpuProxyMesh)
			{
				if (FInstancedAnimRecord* Rec = ResolveRecord(Handle))
				{
					if (Rec->MasterRecordIndex == INDEX_NONE && Rec->CpuProxyActor && Bucket.StaticMesh)
					{
						if (!Entry.CpuStaticMeshComponent)
						{
							USkeletalMeshComponent* ProxySkinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
							check(ProxySkinned);

							UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(Rec->CpuProxyActor);
							check(Comp);
							Comp->CreationMethod = EComponentCreationMethod::Instance;
							Comp->SetStaticMesh(Bucket.StaticMesh);
							Comp->SetupAttachment(ProxySkinned, Entry.BoneName);
							Comp->SetRelativeTransform(FTransform(Entry.SocketLocalTRS));
							Comp->RegisterComponent();
							Entry.CpuStaticMeshComponent = Comp;

							if (bNativeBucket)
							{
								EnqueueAttachHideInstance(Bucket.BucketId, (uint32)OutputIndex);
							}
							else
							{
								NiagaraAttach_KillGpuParticleBySlot(Bucket.BucketId, Slot);
							}
						}
					}
				}
			}
		};

		if (Loc->Type == EAttachBucketType::Native)
		{
			check(NativeAttachBuckets.IsValidIndex(Loc->Index));
			ApplyToBucket(NativeAttachBuckets[Loc->Index], /*bNativeBucket*/ true);
		}
		else
		{
			check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
			ApplyToBucket(NiagaraAttachBuckets[Loc->Index], /*bNativeBucket*/ false);
		}
	}

	return Out;
}

FGameInstancedAnimationAttachHandle UGameInstancedAnimationGraphSubsystem::AttachNiagara(const FGameInstancedAnimationGraphHandle& Handle, UNiagaraSystem* NiagaraSystem, FName BoneName, const FTransform& SocketLocalTransform, UNiagaraSystem* CpuProxyNiagaraSystem)
{
	check(IsInGameThread());

	if (NiagaraSystem == nullptr)
	{
		return {};
	}

	// StaticMesh is optional (bucket key allows null); Niagara system can use GIAG_AttachBucketId only.
	FGameInstancedAnimationAttachHandle Out = AttachStaticMesh_Backend(Handle, /*StaticMesh*/ nullptr, NiagaraSystem, BoneName, SocketLocalTransform);
	if (!Out)
	{
		return {};
	}

	Out.OwnerRecordIndex = Handle.RecordIndex;
	Out.OwnerRecordSerialNumber = Handle.SerialNumber;

	if (FInstancedAnimRecord* Rec = ResolveRecord(Handle))
	{
		Rec->AttachHandles.Add(Out);
		UpdateCpuAttachSyncRegistration_GameThread(Handle.RecordIndex);
	}

	// Store per-attach CPU proxy option.
	{
		const uint32 BucketId = Out.BucketId;
		const uint16 Slot = Out.Slot;
		const uint16 Gen = Out.Generation;
		const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
		check(Loc && Loc->Type == EAttachBucketType::Niagara);
		check(NiagaraAttachBuckets.IsValidIndex(Loc->Index));
		FNiagaraAttachBucket& Bucket = NiagaraAttachBuckets[Loc->Index];

		check(Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex((int32)Slot));
		check(Bucket.Slots.SlotGeneration.IsValidIndex((int32)Slot));
		check(Bucket.Slots.SlotGeneration[(int32)Slot] == Gen);
		const int32 OutputIndex = Bucket.Slots.OutputIndexByAttachSlot[(int32)Slot];
		check(Bucket.Entries.IsValidIndex(OutputIndex));

		FAttachEntry& Entry = Bucket.Entries[OutputIndex];
		Entry.CpuProxyNiagaraSystem = CpuProxyNiagaraSystem;
		if (CpuProxyNiagaraSystem)
		{
			FInstancedAnimRecord* Rec = ResolveRecord(Handle);
			// If already in CPU mode and a proxy system is provided, spawn the CPU Niagara proxy immediately and kill GPU particles.
			if (Rec->CpuProxyActor)
			{
				check(!Entry.CpuNiagaraComponent);
				USkeletalMeshComponent* ProxySkinned = IGIAG_ActorInterface::Execute_GetInstancedAnimationSkinnedMesh(Rec->CpuProxyActor);
				check(ProxySkinned);

				UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this,
					CpuProxyNiagaraSystem,
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
					Comp->SetRelativeTransform(FTransform(Entry.SocketLocalTRS));
					Comp->Activate(true);
					Entry.CpuNiagaraComponent = Comp;
					NiagaraAttach_KillGpuParticleBySlot(Bucket.BucketId, Slot);
				}
			}
		}
	}

	return Out;
}

void UGameInstancedAnimationGraphSubsystem::UpdateAttach(const FGameInstancedAnimationAttachHandle& Handle, FName NewBoneName, const FTransform& NewSocketLocalTransform)
{
	check(IsInGameThread());
	if (!Handle)
	{
		return;
	}
	if (!UpdateAttach_Backend(Handle, NewBoneName, NewSocketLocalTransform))
	{
		return;
	}

	// Update CPU proxy components (if any).
	const uint32 BucketId = Handle.BucketId;
	const uint16 AttachSlot = Handle.Slot;
	const uint16 AttachGen = Handle.Generation;
	const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
	if (!Loc)
	{
		return;
	}

	FAttachEntry* EntryPtr = nullptr;
	if (Loc->Type == EAttachBucketType::Native)
	{
		if (!NativeAttachBuckets.IsValidIndex(Loc->Index))
		{
			return;
		}
		FNativeAttachBucket& Bucket = NativeAttachBuckets[Loc->Index];
		if (!Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot)
			|| !Bucket.Slots.SlotGeneration.IsValidIndex((int32)AttachSlot)
			|| Bucket.Slots.SlotGeneration[(int32)AttachSlot] != AttachGen)
		{
			return;
		}
		const int32 OutputIndex = Bucket.Slots.OutputIndexByAttachSlot[(int32)AttachSlot];
		if (!Bucket.Entries.IsValidIndex(OutputIndex))
		{
			return;
		}
		EntryPtr = &Bucket.Entries[OutputIndex];
	}
	else
	{
		if (!NiagaraAttachBuckets.IsValidIndex(Loc->Index))
		{
			return;
		}
		FNiagaraAttachBucket& Bucket = NiagaraAttachBuckets[Loc->Index];
		if (!Bucket.Slots.OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot)
			|| !Bucket.Slots.SlotGeneration.IsValidIndex((int32)AttachSlot)
			|| Bucket.Slots.SlotGeneration[(int32)AttachSlot] != AttachGen)
		{
			return;
		}
		const int32 OutputIndex = Bucket.Slots.OutputIndexByAttachSlot[(int32)AttachSlot];
		if (!Bucket.Entries.IsValidIndex(OutputIndex))
		{
			return;
		}
		EntryPtr = &Bucket.Entries[OutputIndex];
	}
	check(EntryPtr);
	FAttachEntry& Entry = *EntryPtr;
	const FTransform Rel = FTransform(Entry.SocketLocalTRS);

	if (Entry.CpuStaticMeshComponent)
	{
		Entry.CpuStaticMeshComponent->AttachToComponent(Entry.CpuStaticMeshComponent->GetAttachParent(), FAttachmentTransformRules::KeepRelativeTransform, Entry.BoneName);
		Entry.CpuStaticMeshComponent->SetRelativeTransform(Rel);
	}
	if (Entry.CpuNiagaraComponent)
	{
		if (USceneComponent* Parent = Entry.CpuNiagaraComponent->GetAttachParent())
		{
			Entry.CpuNiagaraComponent->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Entry.BoneName);
		}
		Entry.CpuNiagaraComponent->SetRelativeTransform(Rel);
		Entry.CpuNiagaraComponent->Activate(true);
	}
}

void UGameInstancedAnimationGraphSubsystem::RemoveAttach(const FGameInstancedAnimationAttachHandle& Handle)
{
	check(IsInGameThread());
	if (!Handle)
	{
		return;
	}

	// Release CPU proxies first (if present).
	CleanupAttachCpuProxies(Handle);
	RemoveAttach_Backend(Handle);

	// Prune from owner record list if still alive and serial matches.
	const int32 RecordIndex = Handle.OwnerRecordIndex;
	if (!AnimRecords.IsValidIndex(RecordIndex) || !AnimRecordSerials.IsValidIndex(RecordIndex) || AnimRecordSerials[RecordIndex] != Handle.OwnerRecordSerialNumber)
	{
		return;
	}

	FInstancedAnimRecord& Rec = AnimRecords[RecordIndex];
	for (int32 i = 0; i < Rec.AttachHandles.Num(); ++i)
	{
		if (Rec.AttachHandles[i].BucketId == Handle.BucketId
			&& Rec.AttachHandles[i].Slot == Handle.Slot
			&& Rec.AttachHandles[i].Generation == Handle.Generation)
		{
			Rec.AttachHandles.RemoveAtSwap(i, EAllowShrinking::No);
			break;
		}
	}

	UpdateCpuAttachSyncRegistration_GameThread(RecordIndex);
}

bool UGameInstancedAnimationGraphSubsystem::UpdateAttach_Backend(const FGameInstancedAnimationAttachHandle& Handle, FName NewBoneName, const FTransform& NewSocketLocalTransform)
{
	check(IsInGameThread());
	const uint32 BucketId = Handle.BucketId;
	const uint16 AttachSlot = Handle.Slot;
	const uint16 AttachGen = Handle.Generation;
	const FAttachBucketLocator* Loc = AttachBucketById.Find(BucketId);
	if (!Loc)
	{
		return false;
	}
	const bool bNativeBucket = (Loc->Type == EAttachBucketType::Native);

	FAttachSlotTable* Slots = nullptr;
	TArray<FAttachEntry>* Entries = nullptr;
	if (bNativeBucket)
	{
		if (!NativeAttachBuckets.IsValidIndex(Loc->Index))
		{
			return false;
		}
		Slots = &NativeAttachBuckets[Loc->Index].Slots;
		Entries = &NativeAttachBuckets[Loc->Index].Entries;
	}
	else
	{
		if (!NiagaraAttachBuckets.IsValidIndex(Loc->Index))
		{
			return false;
		}
		Slots = &NiagaraAttachBuckets[Loc->Index].Slots;
		Entries = &NiagaraAttachBuckets[Loc->Index].Entries;
	}
	check(Slots && Entries);

	if (!Slots->OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot)
		|| !Slots->SlotGeneration.IsValidIndex((int32)AttachSlot)
		|| Slots->SlotGeneration[(int32)AttachSlot] != AttachGen)
	{
		return false;
	}
	const int32 OutputIndex = Slots->OutputIndexByAttachSlot[(int32)AttachSlot];
	if (!Entries->IsValidIndex(OutputIndex))
	{
		return false;
	}

	FAttachEntry& Entry = (*Entries)[OutputIndex];
	const int32 ResolvedBoneIndex = (Entry.Skeleton && !NewBoneName.IsNone())
		? Entry.Skeleton->GetReferenceSkeleton().FindBoneIndex(NewBoneName)
		: INDEX_NONE;
	if (ResolvedBoneIndex == INDEX_NONE)
	{
		return false;
	}
	Entry.BoneName = NewBoneName;
	Entry.BoneIndex = (uint32)ResolvedBoneIndex;
	Entry.SocketLocalTRS = FTransform3f(NewSocketLocalTransform);

	FGIAG_AttachBus::FAttachUpdateOp Op;
	Op.BucketId = BucketId;
	Op.BucketKind = bNativeBucket ? FGIAG_AttachBus::EBucketKind::Native : FGIAG_AttachBus::EBucketKind::Niagara;
	Op.AttachSlot = (uint32)AttachSlot;
	Op.State = Entry.State;
	Op.Desc.SlotIndex = Entry.SlotIndex;
	Op.Desc.BoneIndex = Entry.BoneIndex;
	Op.Desc.OutputIndex = (uint32)OutputIndex;
	Op.Desc.Flags = Entry.DescFlags;
	Op.Desc.SocketLocalTRS = Entry.SocketLocalTRS;
	AttachBus->Enqueue_GameThread(MoveTemp(Op));
	return true;
}

bool UGameInstancedAnimationGraphSubsystem::RemoveAttach_Backend(const FGameInstancedAnimationAttachHandle& Handle)
{
	check(IsInGameThread());
	const uint32 BucketId = Handle.BucketId;
	const uint16 AttachSlot = Handle.Slot;
	const uint16 AttachGen = Handle.Generation;
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
		FNativeAttachBucket& Bucket = NativeAttachBuckets[Loc->Index];
		FAttachSlotTable& Slots = Bucket.Slots;
		TArray<FAttachEntry>& Entries = Bucket.Entries;

		if (!Slots.OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot)
			|| !Slots.SlotGeneration.IsValidIndex((int32)AttachSlot)
			|| Slots.SlotGeneration[(int32)AttachSlot] != AttachGen)
		{
			return false;
		}
		const int32 RemoveIndex = Slots.OutputIndexByAttachSlot[(int32)AttachSlot];
		if (!Entries.IsValidIndex(RemoveIndex))
		{
			return false;
		}
		const FAttachEntry RemovedEntry = Entries[RemoveIndex];

		// Emit remove op for the removed id.
		{
			FGIAG_AttachBus::FAttachRemoveOp Op;
			Op.BucketId = Bucket.BucketId;
			Op.BucketKind = FGIAG_AttachBus::EBucketKind::Native;
			Op.AttachSlot = (uint32)RemovedEntry.AttachSlot;
			Op.State = RemovedEntry.State;
			AttachBus->Enqueue_GameThread(MoveTemp(Op));
		}

		// Dense compaction via swap-remove; if we swap, emit an update op for the moved entry (output index changed).
		const int32 LastIndex = Entries.Num() - 1;
		if (RemoveIndex != LastIndex)
		{
			FAttachEntry& Moved = Entries[LastIndex];
			Entries[RemoveIndex] = Moved;
			check(Slots.OutputIndexByAttachSlot.IsValidIndex((int32)Moved.AttachSlot));
			Slots.OutputIndexByAttachSlot[(int32)Moved.AttachSlot] = RemoveIndex;

			FGIAG_AttachBus::FAttachUpdateOp Op;
			Op.BucketId = Bucket.BucketId;
			Op.BucketKind = FGIAG_AttachBus::EBucketKind::Native;
			Op.AttachSlot = (uint32)Moved.AttachSlot;
			Op.State = Moved.State;
			Op.Desc.SlotIndex = Moved.SlotIndex;
			Op.Desc.BoneIndex = Moved.BoneIndex;
			Op.Desc.OutputIndex = (uint32)RemoveIndex;
			Op.Desc.Flags = Moved.DescFlags;
			Op.Desc.SocketLocalTRS = Moved.SocketLocalTRS;
			AttachBus->Enqueue_GameThread(MoveTemp(Op));
		}

		Entries.Pop(EAllowShrinking::No);
		Slots.OutputIndexByAttachSlot[(int32)AttachSlot] = INDEX_NONE;
		Slots.FreeSlots.Add(AttachSlot);
		return true;
	}

	if (!NiagaraAttachBuckets.IsValidIndex(Loc->Index))
	{
		return false;
	}
	FNiagaraAttachBucket& Bucket = NiagaraAttachBuckets[Loc->Index];
	FAttachSlotTable& Slots = Bucket.Slots;
	TArray<FAttachEntry>& Entries = Bucket.Entries;

	if (!Slots.OutputIndexByAttachSlot.IsValidIndex((int32)AttachSlot)
		|| !Slots.SlotGeneration.IsValidIndex((int32)AttachSlot)
		|| Slots.SlotGeneration[(int32)AttachSlot] != AttachGen)
	{
		return false;
	}
	const int32 RemoveIndex = Slots.OutputIndexByAttachSlot[(int32)AttachSlot];
	if (!Entries.IsValidIndex(RemoveIndex))
	{
		return false;
	}
	const FAttachEntry RemovedEntry = Entries[RemoveIndex];

	// Emit remove op for the removed id.
	{
		FGIAG_AttachBus::FAttachRemoveOp Op;
		Op.BucketId = Bucket.BucketId;
		Op.BucketKind = FGIAG_AttachBus::EBucketKind::Niagara;
		Op.AttachSlot = (uint32)RemovedEntry.AttachSlot;
		Op.State = RemovedEntry.State;
		AttachBus->Enqueue_GameThread(MoveTemp(Op));
	}

	// Dense compaction via swap-remove; if we swap, emit an update op for the moved entry (output index changed).
	const int32 LastIndex = Entries.Num() - 1;
	if (RemoveIndex != LastIndex)
	{
		FAttachEntry& Moved = Entries[LastIndex];
		Entries[RemoveIndex] = Moved;
		check(Slots.OutputIndexByAttachSlot.IsValidIndex((int32)Moved.AttachSlot));
		Slots.OutputIndexByAttachSlot[(int32)Moved.AttachSlot] = RemoveIndex;
		if (Bucket.SlotToDenseIndex.IsValidIndex((int32)Moved.AttachSlot))
		{
			Bucket.SlotToDenseIndex[(int32)Moved.AttachSlot] = RemoveIndex;
		}
		Bucket.SlotTableVersion = FMath::Max(1u, Bucket.SlotTableVersion + 1u);
		Bucket.bNiagaraMetaDirty = true;

		FGIAG_AttachBus::FAttachUpdateOp Op;
		Op.BucketId = Bucket.BucketId;
		Op.BucketKind = FGIAG_AttachBus::EBucketKind::Niagara;
		Op.AttachSlot = (uint32)Moved.AttachSlot;
		Op.State = Moved.State;
		Op.Desc.SlotIndex = Moved.SlotIndex;
		Op.Desc.BoneIndex = Moved.BoneIndex;
		Op.Desc.OutputIndex = (uint32)RemoveIndex;
		Op.Desc.Flags = Moved.DescFlags;
		Op.Desc.SocketLocalTRS = Moved.SocketLocalTRS;
		AttachBus->Enqueue_GameThread(MoveTemp(Op));
	}

	Entries.Pop(EAllowShrinking::No);
	Slots.OutputIndexByAttachSlot[(int32)AttachSlot] = INDEX_NONE;
	Slots.FreeSlots.Add(AttachSlot);

	if (Bucket.SlotToDenseIndex.IsValidIndex((int32)AttachSlot))
	{
		Bucket.SlotToDenseIndex[(int32)AttachSlot] = -1;
	}
	Bucket.SlotTableVersion = FMath::Max(1u, Bucket.SlotTableVersion + 1u);
	Bucket.bNiagaraMetaDirty = true;
	return true;
}
