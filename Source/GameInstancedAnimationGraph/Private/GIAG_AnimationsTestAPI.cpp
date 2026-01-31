// Fill out your copyright notice in the Description page of Project Settings.


#include "GIAG_AnimationsTestAPI.h"

#include "GameInstancedAnimationGraphSubsystem.h"

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
