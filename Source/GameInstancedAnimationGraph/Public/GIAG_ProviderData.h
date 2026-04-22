#pragma once

#include "CoreMinimal.h"

struct FGIAG_TransformProviderState;

enum class EGIAG_TransformProviderMode : uint8
{
	MasterEvaluate = 0,
	FollowerCopyOrRemap = 1,
};

/**
 * ProviderData payload for GIAG TransformProvider.
 *
 * Contract:
 * - Stored inside a render-proxy object (render-thread lifetime).
 * - Exposed to engine/renderer as TConstArrayView<uint64>.
 * - Consumers reinterpret_cast the uint64 words to this struct (no encode/decode, no memcpy).
 *
 * IMPORTANT:
 * - Must be POD (no constructors/destructors).
 * - Must be 8-byte aligned and size a multiple of 8, so it can be viewed as uint64 words.
 */
struct alignas(8) FGIAG_ProviderData
{
	/** Animation slot count (AnimationIndex capacity, <=127). */
	uint32 AnimationSlotCount = 1;

	/** Pointer to this component's shared state (refcount held by render proxy). */
	FGIAG_TransformProviderState* SelfState = nullptr;

	/** Mode. */
	EGIAG_TransformProviderMode Mode = EGIAG_TransformProviderMode::MasterEvaluate;

	/** Follower-only: master's state (refcount held by render proxy). */
	FGIAG_TransformProviderState* MasterState = nullptr;

	/** Destination bone count (follow mesh). */
	uint32 NumBones = 0;

	/** Source bone count (master skeleton). */
	uint32 SrcNumBones = 0;

	/** Optional: DestBoneIndex -> SrcBoneIndex remap table (size NumBones). Null => identity mapping. */
	const uint32* BoneRemap = nullptr;

	/** Shard index within the owning bucket (used by RT to build ShardTransformOffsets). */
	uint32 ShardIndex = 0;

	/** Follower-only: master shard index within the master's bucket (for PoseBuffer SrcSlotBase). */
	uint32 MasterShardIndex = 0;

	static constexpr int32 NumWords()
	{
		return (int32)(sizeof(FGIAG_ProviderData) / sizeof(uint64));
	}
};

static_assert(TIsTriviallyCopyConstructible<FGIAG_ProviderData>::Value, "ProviderData must be trivially copyable.");

