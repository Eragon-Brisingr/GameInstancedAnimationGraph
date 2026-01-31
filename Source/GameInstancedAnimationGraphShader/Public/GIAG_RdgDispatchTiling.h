#pragma once

#include "CoreMinimal.h"
#include "RHI.h"

namespace GIAG::RDGDispatchTiling
{
	/**
	 * Decompose a 1D dispatch group count into a 3D (X,Y,Z) group count under RHI per-dimension limits.
	 *
	 * NOTE: This function assumes the caller already chunked Groups1D so that Groups1D <= MaxX*MaxY*MaxZ.
	 */
	inline FIntVector DecomposeGroups3D(int32 Groups1D)
	{
		Groups1D = FMath::Max(1, Groups1D);

		const FIntVector MaxGroups = GRHIMaxDispatchThreadGroupsPerDimension;
		const int32 X = FMath::Clamp(Groups1D, 1, MaxGroups.X);
		const int32 RemAfterX = FMath::DivideAndRoundUp(Groups1D, X);
		const int32 Y = FMath::Clamp(RemAfterX, 1, MaxGroups.Y);
		const int32 RemAfterY = FMath::DivideAndRoundUp(RemAfterX, Y);
		const int32 Z = FMath::Clamp(RemAfterY, 1, MaxGroups.Z);

		checkf(
			RemAfterY <= MaxGroups.Z,
			TEXT("GIAG: dispatch groups exceed RHI max product; expected caller-side chunking. RemZ=%d MaxZ=%d"),
			RemAfterY,
			MaxGroups.Z);

		return FIntVector(X, Y, Z);
	}

	inline int32 ComputeTotalGroups1D(int64 TotalWorkItems, int32 ThreadsPerGroup)
	{
		if (TotalWorkItems <= 0 || ThreadsPerGroup <= 0)
		{
			return 0;
		}

		const int64 Groups64 = (TotalWorkItems + (int64)ThreadsPerGroup - 1) / (int64)ThreadsPerGroup;
		return (int32)FMath::Clamp<int64>(Groups64, 0, (int64)MAX_int32);
	}

	/**
	 * Iterate dispatch chunks, first utilizing X/Y/Z up to per-dimension limits, and if still exceeded,
	 * splitting into multiple chunks (passes) with a 1D group offset.
	 *
	 * The callback signature is:
	 *   void(int32 ChunkGroups1D, int32 GroupOffset1D, const FIntVector& GroupCountXYZ)
	 */
	template <typename TLambda>
	inline void ForEachChunk(int64 TotalWorkItems, int32 ThreadsPerGroup, TLambda&& Callback)
	{
		const int32 TotalGroups1D = ComputeTotalGroups1D(TotalWorkItems, ThreadsPerGroup);
		if (TotalGroups1D <= 0)
		{
			return;
		}

		const FIntVector MaxGroups = GRHIMaxDispatchThreadGroupsPerDimension;
		const int64 MaxGroupsProduct = (int64)MaxGroups.X * (int64)MaxGroups.Y * (int64)MaxGroups.Z;
		const int32 MaxChunkGroups1D = (int32)FMath::Clamp<int64>(MaxGroupsProduct, 1, (int64)MAX_int32);

		int32 GroupOffset1D = 0;
		while (GroupOffset1D < TotalGroups1D)
		{
			const int32 RemainingGroups = TotalGroups1D - GroupOffset1D;
			const int32 ChunkGroups1D = FMath::Min(RemainingGroups, MaxChunkGroups1D);
			const FIntVector GroupCountXYZ = DecomposeGroups3D(ChunkGroups1D);

			Callback(ChunkGroups1D, GroupOffset1D, GroupCountXYZ);

			GroupOffset1D += ChunkGroups1D;
		}
	}
}

