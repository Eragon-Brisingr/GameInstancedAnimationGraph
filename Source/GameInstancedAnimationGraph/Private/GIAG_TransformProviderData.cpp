#include "GIAG_TransformProviderData.h"

#include "Components/InstancedSkinnedMeshComponent.h"
#include "SkinningSceneExtensionProxy.h"

UGIAG_TransformProviderData::UGIAG_TransformProviderData()
{
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
	FName InFollowMeshName)
{
	Mode = EGIAG_TransformProviderMode::FollowerCopyOrRemap;
	MasterState = InMasterBridge;
	NumBones = FMath::Max(0, InNumBones);
	SrcNumBones = FMath::Max(0, InSrcNumBones);
	BoneRemapShared = MoveTemp(InBoneRemapShared);
	BoneRemapPtr = BoneRemapShared.IsValid() ? BoneRemapShared->GetData() : nullptr;
	FollowMeshName = InFollowMeshName;
}

FTransformProviderRenderProxy* UGIAG_TransformProviderData::CreateRenderProxy(FInstancedSkinningSceneExtensionProxy* ExtensionProxy) const
{
	FGIAG_ProviderData ProviderData;
	ProviderData.AnimationSlotCount = (uint32)GetUniqueAnimationCount();
	ProviderData.SelfState = State.GetReference();
	ProviderData.Mode = Mode;
	ProviderData.MasterState = MasterState.GetReference();
	ProviderData.NumBones = (uint32)FMath::Max(0, NumBones);
	ProviderData.SrcNumBones = (uint32)FMath::Max(0, SrcNumBones);
	ProviderData.BoneRemap = BoneRemapPtr;
	ProviderData.FollowMeshName = FollowMeshName;

	return new FGIAG_TransformProviderRenderProxy(ProviderData, ExtensionProxy, State, MasterState, BoneRemapShared);
}
