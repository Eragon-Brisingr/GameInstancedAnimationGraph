#pragma once

#include "CoreMinimal.h"
#include "Animation/TransformProviderData.h"

#include "GIAG_TransformProviderBridge.h"
#include "GIAG_ProviderData.h"

#include "GIAG_TransformProviderData.generated.h"

class FInstancedSkinningSceneExtensionProxy;

/**
 * GIAG render-thread proxy carrying `FGIAG_ProviderData` directly (no uint64 packing).
 *
 * Lifetime: created by `UGIAG_TransformProviderData::CreateRenderProxy` (GT), owned by the engine
 * `FInstancedSkinningSceneExtensionProxy`, destroyed when the SceneExtensionProxy is torn down.
 * Holds refs to the shared states + remap table to keep them alive on RT.
 */
class FGIAG_TransformProviderRenderProxy final : public FTransformProviderRenderProxy
{
public:
	FGIAG_TransformProviderRenderProxy(
		const FGIAG_ProviderData& InData,
		FInstancedSkinningSceneExtensionProxy* InEngineExtensionProxy,
		TRefCountPtr<FGIAG_TransformProviderState> InSelf,
		TRefCountPtr<FGIAG_TransformProviderState> InMaster,
		TSharedPtr<const TArray<uint32>> InRemap)
		: Data(InData)
		, EngineExtensionProxy(InEngineExtensionProxy)
		, SelfState(MoveTemp(InSelf))
		, MasterState(MoveTemp(InMaster))
		, BoneRemapShared(MoveTemp(InRemap))
	{}

	virtual void CreateRenderThreadResources(FSceneInterface& /*Scene*/, FRHICommandListBase& /*RHICmdList*/) override {}
	virtual void DestroyRenderThreadResources() override {}

	const FGIAG_ProviderData& GetData() const { return Data; }
	FInstancedSkinningSceneExtensionProxy* GetEngineExtensionProxy() const { return EngineExtensionProxy; }

private:
	FGIAG_ProviderData Data;
	/** Owning engine extension proxy. RT-only. The engine deletes this proxy and our RenderProxy
	 *  together, so the back-pointer is valid for the RenderProxy's whole lifetime. Used to call
	 *  `SetUniqueAnimationCount` directly on the proxy when the bucket grows or shrinks (the
	 *  engine then handles span reallocation + transform-preserving copy via FCopyPreviousTransformsCS,
	 *  with `DirtyBoneTransforms = Current` so motion blur is preserved for free). */
	FInstancedSkinningSceneExtensionProxy* EngineExtensionProxy = nullptr;
	TRefCountPtr<FGIAG_TransformProviderState> SelfState;
	TRefCountPtr<FGIAG_TransformProviderState> MasterState;
	TSharedPtr<const TArray<uint32>> BoneRemapShared;
};

/**
 * TransformProviderData for GameInstancedAnimationGraph.
 *
 * - ProviderId must match the Renderer-registered provider.
 * - SkinningDataOffset is defined as AnimationIndex * 2 (current/previous).
 */
UCLASS(BlueprintType)
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_TransformProviderData final : public UTransformProviderData
{
	GENERATED_BODY()

public:
	/** Engine-facing provider GUID (shared by all GIAG ISKMCs; the engine calls ProvideTransforms once). */
	static constexpr FGuid ProviderGuid{ 0xE6D4B2D1, 0x3B0D4F2B, 0x8B7E6C41, 0xF41B1A2D};

	/** Number of animation slots (AnimationIndex capacity) for this ISKMC. */
	int32 AnimationSlotCount = 1;

	virtual const FGuid& GetTransformProviderID() const override { return ProviderGuid; }
	virtual uint32 GetUniqueAnimationCount() const override { return (uint32)FMath::Max(1, AnimationSlotCount); }

	virtual uint32 GetSkinningDataOffset(int32 InstanceIndex, const FTransform& ComponentTransform, const FSkinnedMeshInstanceData& InstanceData) const override;

	TRefCountPtr<FGIAG_TransformProviderState> GetState() const { return State; }
	void SetState(FGIAG_TransformProviderState* InState) { State = InState; }
	TRefCountPtr<FGIAG_TransformProviderState> GetMasterState() const { return MasterState; }
	const uint32* GetBoneRemapPtr() const { return BoneRemapPtr; }
	int32 GetNumBones() const { return NumBones; }
	int32 GetSrcNumBones() const { return SrcNumBones; }

	/** Configure this provider as a master evaluator (default). */
	void ConfigureAsMaster();

	/**
	 * Configure this provider as a follower which reuses master's PoseBuffer data.
	 * For followers, instance AnimationIndex selects the slot to sample on the follow ISKMC.
	 * On RT we read master's PoseBuffer and write to follower's TransformBuffer regions.
	 *
	 * @param InMasterBridge Master's bridge pointer (shared state).
	 * @param InNumBones Destination bone count (follow mesh expected bones).
	 * @param InSrcNumBones Source bone count (master bones).
	 * @param InBoneRemapShared Optional remap table (DestBoneIndex -> SrcBoneIndex). Null => no remap (identity).
	 * @param InFollowMeshName Mesh debug name for grouping/profiling.
	 */
	void ConfigureAsFollower(
		FGIAG_TransformProviderState* InMasterBridge,
		int32 InNumBones,
		int32 InSrcNumBones,
		TSharedPtr<const TArray<uint32>> InBoneRemapShared,
		FName InFollowMeshName);

	EGIAG_TransformProviderMode GetMode() const { return Mode; }

	virtual FTransformProviderRenderProxy* CreateRenderProxy(FInstancedSkinningSceneExtensionProxy* ExtensionProxy) const override;

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
	FName FollowMeshName;
};
