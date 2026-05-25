// Fill out your copyright notice in the Description page of Project Settings.


#include "GIAG_AnimationsTestAPI.h"

#include "GameInstancedAnimationGraphSubsystem.h"
#include "Engine/SkeletalMesh.h"

bool FGIAG_AnimationsTestAPI::DebugGetAnimLibraryStats(const UGameInstancedAnimationGraphSubsystem* Subsystem, USkeleton* Skeleton, int32& OutAnimTRSCapacity, int32& OutNumClips, uint32& OutVersion)
{
	OutAnimTRSCapacity = 0;
	OutNumClips = 0;
	OutVersion = 0;

	if (!Skeleton)
	{
		return false;
	}

	const int32* CacheIndexPtr = Subsystem->SkeletonCacheIndexBySkeleton.Find(Skeleton);
	if (!CacheIndexPtr)
	{
		return false;
	}
	const int32 CacheIndex = *CacheIndexPtr;
	if (!Subsystem->SkeletonCaches.IsValidIndex(CacheIndex) || !Subsystem->SkeletonCaches[CacheIndex].IsValid())
	{
		return false;
	}
	const auto& Cache = *Subsystem->SkeletonCaches[CacheIndex];
	OutAnimTRSCapacity = Cache.AnimTRSCapacity;
	OutNumClips = Cache.ClipSlots.Num();
	OutVersion = Cache.AnimLibraryVersion;
	return true;
}

bool FGIAG_AnimationsTestAPI::DebugGetMasterBucketStats(
	const UGameInstancedAnimationGraphSubsystem* Subsystem,
	USkeletalMesh* SkeletalMesh,
	UGIAG_AnimGraph* AnimGraph,
	int32& OutCapacity,
	int32& OutNumInstances)
{
	OutCapacity = 0;
	OutNumInstances = 0;
	if (!Subsystem || !SkeletalMesh || !AnimGraph) { return false; }

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	int32 GroupIndex = INDEX_NONE;
	for (auto It = Subsystem->Groups.CreateConstIterator(); It; ++It)
	{
		if (It->AnimGraph == AnimGraph && It->Skeleton == Skeleton) { GroupIndex = It.GetIndex(); break; }
	}
	if (GroupIndex == INDEX_NONE) { return false; }

	const UGameInstancedAnimationGraphSubsystem::FBucketKey Key{ SkeletalMesh, GroupIndex, /*bFollower=*/false };
	const int32* Found = Subsystem->BucketByKey.Find(Key);
	if (!Found) { return false; }

	const auto& Bucket = Subsystem->Buckets[*Found];
	OutCapacity = Bucket.GetTotalSlotCapacity();
	OutNumInstances = Bucket.NumInstances;
	return true;
}

bool FGIAG_AnimationsTestAPI::DebugGetFollowerBucketStats(
	const UGameInstancedAnimationGraphSubsystem* Subsystem,
	USkeletalMesh* FollowSkeletalMesh,
	UGIAG_AnimGraph* AnimGraph,
	int32& OutCapacity,
	int32& OutNumInstances)
{
	OutCapacity = 0;
	OutNumInstances = 0;
	if (!Subsystem || !FollowSkeletalMesh || !AnimGraph) { return false; }

	USkeleton* Skeleton = FollowSkeletalMesh->GetSkeleton();
	int32 GroupIndex = INDEX_NONE;
	for (auto It = Subsystem->Groups.CreateConstIterator(); It; ++It)
	{
		if (It->AnimGraph == AnimGraph && It->Skeleton == Skeleton) { GroupIndex = It.GetIndex(); break; }
	}
	if (GroupIndex == INDEX_NONE) { return false; }

	const UGameInstancedAnimationGraphSubsystem::FBucketKey Key{ FollowSkeletalMesh, GroupIndex, /*bFollower=*/true };
	const int32* Found = Subsystem->BucketByKey.Find(Key);
	if (!Found) { return false; }

	const auto& Bucket = Subsystem->Buckets[*Found];
	OutCapacity = Bucket.GetTotalSlotCapacity();
	OutNumInstances = Bucket.NumInstances;
	return true;
}
