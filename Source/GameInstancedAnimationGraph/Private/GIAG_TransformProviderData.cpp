#include "GIAG_TransformProviderData.h"

#include "Components/InstancedSkinnedMeshComponent.h"
#include "SkinningSceneExtensionProxy.h"

UGIAG_TransformProviderData::UGIAG_TransformProviderData()
{
	bEnabled = true;
	ConfigureAsMaster();
}

void UGIAG_TransformProviderData::BeginDestroy()
{
	// State is refcounted; RT render-proxy holds a reference.
	// Releasing here is safe: RT will keep it alive until proxy destruction.
	State.SafeRelease();
	MasterState.SafeRelease();
	BoneRemapShared.Reset();
	BoneRemapPtr = nullptr;
	Super::BeginDestroy();
}

uint32 UGIAG_TransformProviderData::GetSkinningDataOffset(int32 InstanceIndex, const FTransform& ComponentTransform, const FSkinnedMeshInstanceData& InstanceData) const
{
	// Match UE SkinningDataDecode.ush contract:
	// SkinningDataOffset = AnimationIndex * 2  (current/previous)
	return (uint32)InstanceData.AnimationIndex * 2u;
}

void UGIAG_TransformProviderData::ConfigureAsMaster()
{
	Mode = EGIAG_TransformProviderMode::MasterEvaluate;
	MasterState.SafeRelease();
	NumBones = 0;
	SrcNumBones = 0;
	BoneRemapShared.Reset();
	BoneRemapPtr = nullptr;
}

void UGIAG_TransformProviderData::ConfigureAsFollower(
	FGIAG_TransformProviderState* InMasterBridge,
	int32 InNumBones,
	int32 InSrcNumBones,
	TSharedPtr<const TArray<uint32>> InBoneRemapShared,
	int32 InMasterShardIndex,
	FName InFollowMeshName)
{
	Mode = EGIAG_TransformProviderMode::FollowerCopyOrRemap;
	MasterState = InMasterBridge;
	MasterShardIndex = InMasterShardIndex;
	NumBones = FMath::Max(0, InNumBones);
	SrcNumBones = FMath::Max(0, InSrcNumBones);
	BoneRemapShared = MoveTemp(InBoneRemapShared);
	BoneRemapPtr = BoneRemapShared.IsValid() ? BoneRemapShared->GetData() : nullptr;
	FollowMeshName = InFollowMeshName;
}

FTransformProviderRenderProxy* UGIAG_TransformProviderData::CreateRenderThreadResources(FSkinningSceneExtensionProxy* SceneProxy, FSceneInterface& Scene, FRHICommandListBase& RHICmdList)
{
	FGIAG_ProviderData ProviderData;
	ProviderData.AnimationSlotCount = (uint32)GetUniqueAnimationCount();
	ProviderData.SelfState = State.GetReference();
	ProviderData.Mode = Mode;
	ProviderData.MasterState = MasterState.GetReference();
	ProviderData.NumBones = (uint32)FMath::Max(0, NumBones);
	ProviderData.SrcNumBones = (uint32)FMath::Max(0, SrcNumBones);
	ProviderData.BoneRemap = BoneRemapPtr;
	ProviderData.ShardIndex = ShardIndex;
	ProviderData.MasterShardIndex = (uint32)MasterShardIndex;
	ProviderData.FollowMeshName = FollowMeshName;

	// Render proxy must hold refs to keep State/MasterState alive on RT.
	class FGIAG_TransformProviderRenderProxyWithRefs final : public FTransformProviderRenderProxy
	{
	public:
		FGIAG_TransformProviderRenderProxyWithRefs(const FGIAG_ProviderData& InData, TRefCountPtr<FGIAG_TransformProviderState> InSelf, TRefCountPtr<FGIAG_TransformProviderState> InMaster, TSharedPtr<const TArray<uint32>> InRemap)
			: Data(InData)
			, SelfState(MoveTemp(InSelf))
			, MasterState(MoveTemp(InMaster))
			, BoneRemapShared(MoveTemp(InRemap))
		{
		}

		void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override {}
		void DestroyRenderThreadResources() override {}

		const TConstArrayView<uint64> GetProviderData(bool& bOutValid) const override
		{
			bOutValid = true;
			const uint64* Words = reinterpret_cast<const uint64*>(&Data);
			return TConstArrayView<uint64>(Words, FGIAG_ProviderData::NumWords());
		}

	private:
		FGIAG_ProviderData Data;
		TRefCountPtr<FGIAG_TransformProviderState> SelfState;
		TRefCountPtr<FGIAG_TransformProviderState> MasterState;
		TSharedPtr<const TArray<uint32>> BoneRemapShared;
	};

	return new FGIAG_TransformProviderRenderProxyWithRefs(ProviderData, State, MasterState, BoneRemapShared);
}

void UGIAG_TransformProviderData::DestroyRenderThreadResources(FTransformProviderRenderProxy* ProviderProxy)
{
	delete ProviderProxy;
}

