#pragma once

#include "CoreMinimal.h"

class USkeleton;
class UHierarchyTable;

namespace GIAG::HierarchyTableMaskUtils
{
	/**
	 * Build per-bone weights from a Skeleton+HierarchyTable(Mask).
	 *
	 * Mapping rule:
	 * - First try BoneIndex lookup and validate Entry.Identifier == RefSkel.BoneName(i).
	 * - If mismatch detected, fall back to BoneName lookup by Identifier.
	 *
	 * Fallback behavior:
	 * - Invalid/missing table or skeleton mismatch -> fill zeros (fallback to Base).
	 */
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API bool BuildPerBoneMaskWeights(
		const USkeleton* Skeleton,
		const UHierarchyTable* Table,
		TArray<float>& OutWeights);

	/** Hash helper for resources derived purely from (Skeleton, Table hierarchy). */
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API uint64 MakeMaskTableKeyHash(
		const USkeleton* Skeleton,
		UHierarchyTable* Table,
		uint64 Salt);
}

