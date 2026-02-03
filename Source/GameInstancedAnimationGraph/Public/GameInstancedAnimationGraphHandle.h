// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameInstancedAnimationGraphHandle.generated.h"

class UGameInstancedAnimationGraphSubsystem;

USTRUCT(BlueprintType)
struct GAMEINSTANCEDANIMATIONGRAPH_API FGameInstancedAnimationGraphHandle
{
	GENERATED_BODY()
public:
	UPROPERTY(Transient)
	int32 RecordIndex = INDEX_NONE;
	
	UPROPERTY(Transient)
	int32 SerialNumber = INDEX_NONE;

	FORCEINLINE bool IsValid() const { return RecordIndex != INDEX_NONE; }
	explicit operator bool() const { return IsValid(); }
};

USTRUCT(BlueprintType)
struct GAMEINSTANCEDANIMATIONGRAPH_API FGameInstancedAnimationAttachHandle
{
	GENERATED_BODY()
public:
	UPROPERTY(Transient)
	int32 OwnerRecordIndex = INDEX_NONE;

	UPROPERTY(Transient)
	int32 OwnerRecordSerialNumber = INDEX_NONE;

	UPROPERTY(Transient)
	uint32 BucketId = 0;

	UPROPERTY(Transient)
	uint16 Slot = 0;

	UPROPERTY(Transient)
	uint16 Generation = 0;

	bool IsValid() const { return BucketId != 0; }
	explicit operator bool() const { return IsValid(); }
};