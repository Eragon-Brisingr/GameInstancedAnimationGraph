// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GIAG_TimeSlot.generated.h"

inline constexpr int32 GIAG_MAX_TIME_SLOTS = 16;

/** Fill FVector4f-packed cbuffer array from a linear TimeSlots span. */
FORCEINLINE void GIAG_FillTimeSlotsParameter(FVector4f* OutPacked, TConstArrayView<float> InTimeSlots)
{
	check(InTimeSlots.Num() == GIAG_MAX_TIME_SLOTS);
	for (int32 i = 0; i < GIAG_MAX_TIME_SLOTS; ++i)
	{
		OutPacked[i >> 2][i & 3] = InTimeSlots[i];
	}
}

USTRUCT(BlueprintType)
struct FGIAG_TimeSlot
{
	GENERATED_BODY()
	
	int32 Index = 0;

	static FGIAG_TimeSlot WorldTime() { return {0}; }
	bool IsWorldTime() const { return Index == 0; }
	bool IsValid() const { return Index >= 0 && Index < GIAG_MAX_TIME_SLOTS; }

	bool operator==(const FGIAG_TimeSlot& Other) const { return Index == Other.Index; }
	bool operator!=(const FGIAG_TimeSlot& Other) const { return Index != Other.Index; }
};
