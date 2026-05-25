// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class USkeleton;
class USkeletalMesh;
class UGameInstancedAnimationGraphSubsystem;
class UGIAG_AnimGraph;

struct GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimationsTestAPI
{
public:
	/** Test-only: query per-skeleton anim library stats (GT view). */
	static bool DebugGetAnimLibraryStats(const UGameInstancedAnimationGraphSubsystem* Subsystem, USkeleton* Skeleton, int32& OutAnimTRSCapacity, int32& OutNumClips, uint32& OutVersion);

	/** Test-only: query the master bucket's current SlotCapacity / NumInstances for (Mesh, Graph).
	 *  Returns false when the bucket doesn't exist. */
	static bool DebugGetMasterBucketStats(
		const UGameInstancedAnimationGraphSubsystem* Subsystem,
		USkeletalMesh* SkeletalMesh,
		UGIAG_AnimGraph* AnimGraph,
		int32& OutCapacity,
		int32& OutNumInstances);

	/** Test-only: query a follower bucket's current SlotCapacity / NumInstances for (FollowMesh, Graph).
	 *  Returns false when the follower bucket doesn't exist. */
	static bool DebugGetFollowerBucketStats(
		const UGameInstancedAnimationGraphSubsystem* Subsystem,
		USkeletalMesh* FollowSkeletalMesh,
		UGIAG_AnimGraph* AnimGraph,
		int32& OutCapacity,
		int32& OutNumInstances);
};
