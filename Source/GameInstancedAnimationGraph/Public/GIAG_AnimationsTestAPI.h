// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class USkeleton;
class UGameInstancedAnimationGraphSubsystem;

struct GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimationsTestAPI
{
public:
	/** Test-only: query per-skeleton anim library stats (GT view). */
	static bool DebugGetAnimLibraryStats(const UGameInstancedAnimationGraphSubsystem* Subsystem, USkeleton* Skeleton, int32& OutAnimTRSCapacity, int32& OutNumClips, uint32& OutVersion);

};
