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
 * Lives inside a `FGIAG_TransformProviderRenderProxy`.
 * The render-thread `ProvideTransforms` callback recovers the proxy via
 *   `static_cast<FInstancedSkinningSceneExtensionProxy*>(Proxies[Ind.Index])->GetTransformProviderProxy()`
 * and reads these fields directly. No more uint64 packing.
 */
struct FGIAG_ProviderData
{
	/** Animation slot count (AnimationIndex capacity) */
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

	/** Follower-only: mesh name for profiling / dispatch grouping. */
	FName FollowMeshName;
};
