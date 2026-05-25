#pragma once

#include "CoreMinimal.h"
#include "Math/UnrealMathUtility.h"

namespace GIAG::BucketCapacity
{
	// Master buckets allocate slot storage at one of a fixed set of "tier" capacities. When a
	// bucket needs more room, the capacity steps up to the next tier; when it has enough
	// headroom, it steps down. Discrete tiers (rather than allocating exactly the slots
	// requested) amortise the cost of GPU buffer reallocations and absorb spawn/despawn churn
	// without grow/shrink thrash.
	//
	// The tier ladder is shaped to be cheap for the long-tail case (many meshes carrying only
	// a few instances each) while keeping reallocation count bounded for hot meshes. The grow
	// quantum widens with capacity, so the ladder approximates ~1.5x geometric growth:
	//
	//   { 16, 32, 48, 64, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 1792, ... }
	//
	//   CurrentCapacity   GrowQuantum
	//   ---------------   -----------
	//   [0, 64)             16
	//   [64, 256)           64
	//   [256, 512)          128
	//   [512, +inf)         256

	/** Capacity used the very first time a bucket allocates storage. */
	inline constexpr int32 InitialCapacity = 16;

	/** Step size from CurrentCapacity to the next tier. */
	FORCEINLINE int32 GetGrowQuantum(int32 CurrentCapacity)
	{
		if (CurrentCapacity < 64)  { return 16;  }
		if (CurrentCapacity < 256) { return 64;  }
		if (CurrentCapacity < 512) { return 128; }
		return 256;
	}

	/**
	 * Smallest tier capacity that fits at least RequestedSlots.
	 * Walks the ladder from the start; bounded by O(log RequestedSlots) iterations for sane
	 * inputs (each tier is at least ~33% larger than the one before until the linear cap).
	 */
	FORCEINLINE int32 RoundUpToTier(int32 RequestedSlots)
	{
		const int32 Target = FMath::Max(RequestedSlots, 1);
		int32 Capacity = InitialCapacity;
		while (Capacity < Target)
		{
			Capacity += GetGrowQuantum(Capacity);
		}
		return Capacity;
	}

	/** True iff Capacity is one of the values appearing on the tier ladder. */
	FORCEINLINE bool IsValidTier(int32 Capacity)
	{
		return Capacity > 0 && RoundUpToTier(Capacity) == Capacity;
	}

	/**
	 * Capacity to grow to when CurrentCapacity is full and another slot is needed.
	 * Always advances by exactly one tier.
	 */
	FORCEINLINE int32 ComputeGrowTarget(int32 CurrentCapacity)
	{
		return CurrentCapacity + GetGrowQuantum(CurrentCapacity);
	}

	/**
	 * Capacity to shrink to when only LiveSlots of CurrentCapacity are still in use.
	 *
	 * Hysteresis (free room kept above LiveSlots before shrink fires) is constant at
	 * InitialCapacity. This is enough to absorb a full small-tier quantum of churn without
	 * flapping, and importantly does NOT scale up with the current tier — scaling with
	 * GetGrowQuantum(CurrentCapacity) makes the hysteresis exceed the gap to the prior tier
	 * at C >= 64 (e.g. C=64 has GrowQuantum=64 but the gap to 48 is only 16), so a constant
	 * choice is the simplest design that allows shrink at every tier.
	 *
	 * The result is rounded up to a valid tier, so a single shrink may skip multiple tiers
	 * when LiveSlots has dropped sharply (e.g. CurrentCapacity=1024, LiveSlots=2 → 32).
	 * The caller compares the result against CurrentCapacity to decide whether shrinking is
	 * actually required (target >= CurrentCapacity means "stay where we are").
	 */
	FORCEINLINE int32 ComputeShrinkTarget(int32 LiveSlots, int32 CurrentCapacity)
	{
		return RoundUpToTier(FMath::Max(LiveSlots, 0) + InitialCapacity);
	}
}
