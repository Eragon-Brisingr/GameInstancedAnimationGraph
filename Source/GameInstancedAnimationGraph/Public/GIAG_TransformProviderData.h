#pragma once

#include "CoreMinimal.h"
#include "Animation/TransformProviderData.h"

#include "GIAG_TransformProviderBridge.h"
#include "GIAG_ProviderData.h"

#include "GIAG_TransformProviderData.generated.h"

/**
 * TransformProviderData for GameInstancedAnimationGraph.
 *
 * - ProviderId must match the Renderer-registered provider.
 * - AnimationSlotCount <= 127 (engine header packs it into 7 bits).
 * - SkinningDataOffset is defined as AnimationIndex * 2 (current/previous).
 */
UCLASS(BlueprintType)
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_TransformProviderData final : public UTransformProviderData
{
	GENERATED_BODY()

public:
	/** Engine-facing provider GUID (must match registered TransformProvider). */
	static constexpr FGuid ProviderGuid{ 0xE6D4B2D1, 0x3B0D4F2B, 0x8B7E6C41, 0xF41B1A2D};

	/** Number of animation slots (AnimationIndex capacity) for this ISKMC shard (<=127). */
	int32 AnimationSlotCount = 1;

	virtual const FGuid& GetTransformProviderID() const override { return ProviderGuid; }
	virtual const uint32 GetUniqueAnimationCount() const override { return (uint32)FMath::Max(1, AnimationSlotCount); }

	virtual uint32 GetSkinningDataOffset(int32 InstanceIndex, const FTransform& ComponentTransform, const FSkinnedMeshInstanceData& InstanceData) const override;

	TRefCountPtr<FGIAG_TransformProviderState> GetState() const { return State; }
	TRefCountPtr<FGIAG_TransformProviderState> GetMasterState() const { return MasterState; }
	const uint32* GetBoneRemapPtr() const { return BoneRemapPtr; }
	int32 GetNumBones() const { return NumBones; }
	int32 GetSrcNumBones() const { return SrcNumBones; }

	/** Configure this provider as a master evaluator (default). */
	void ConfigureAsMaster();

	/**
	 * Configure this provider as a follower which reuses master's TransformBuffer data.
	 * For followers, instance AnimationIndex selects the slot to sample on the follow ISKMC.
	 * On RT we copy master slot -> follow slot (dstSlot==srcSlot) for all slots in AnimationSlotCount.
	 *
	 * @param InMasterBridge Master's bridge pointer (shared state).
	 * @param InNumBones Destination bone count (follow mesh expected bones).
	 * @param InSrcNumBones Source bone count (master bones).
	 * @param InBoneRemapShared Optional remap table (DestBoneIndex -> SrcBoneIndex). Null => no remap (identity).
	 */
	void ConfigureAsFollower(
		FGIAG_TransformProviderState* InMasterBridge,
		int32 InNumBones,
		int32 InSrcNumBones,
		TSharedPtr<const TArray<uint32>> InBoneRemapShared);

	EGIAG_TransformProviderMode GetMode() const { return Mode; }

	virtual FTransformProviderRenderProxy* CreateRenderThreadResources(FSkinningSceneExtensionProxy* SceneProxy, FSceneInterface& Scene, FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources(FTransformProviderRenderProxy* ProviderProxy) override;

	UGIAG_TransformProviderData();
	virtual void BeginDestroy() override;

private:
	/** Shared state (refcounted). Render proxy holds a ref on RT. */
	TRefCountPtr<FGIAG_TransformProviderState> State;

	EGIAG_TransformProviderMode Mode = EGIAG_TransformProviderMode::MasterEvaluate;
	TRefCountPtr<FGIAG_TransformProviderState> MasterState;
	int32 NumBones = 0;
	int32 SrcNumBones = 0;
	/** Optional remap storage to ensure RT lifetime; BoneRemapPtr points into this when set. */
	TSharedPtr<const TArray<uint32>> BoneRemapShared;
	const uint32* BoneRemapPtr = nullptr;
};

