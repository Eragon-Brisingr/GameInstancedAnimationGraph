#include "GIAG_DebugHelpers.h"

#include "DrawDebugHelpers.h"
#include "GameInstancedAnimationGraphSubsystem.h"
#include "Engine/World.h"

void UGIAG_DebugHelpers::DrawDebugSkeletonFromLocalPoseTRS(const UObject* WorldContextObject, const FTransform& ComponentToWorld, const FGameInstancedAnimationGraphHandle& Handle, const TArray<FTransform3f>& LocalPoseTRS, FLinearColor Color, float Duration, float Thickness)
{
	if (!Handle)
	{
		return;
	}
	
	UGameInstancedAnimationGraphSubsystem* Subsystem = WorldContextObject->GetWorld()->GetSubsystem<UGameInstancedAnimationGraphSubsystem>();
	check(Subsystem);
	
	UWorld* World = Subsystem->GetWorld();

	TArray<FTransform3f> ComponentPose;
	const int32 NumBones = LocalPoseTRS.Num();
	Subsystem->ConvertLocalPoseToComponentPoseChecked(Handle, MakeArrayView(LocalPoseTRS), ComponentPose);
	check(ComponentPose.Num() == NumBones);

	const TArray<int32>& ParentIndices = Subsystem->GetSkeletonParentIndicesChecked(Handle);
	check(ParentIndices.Num() >= NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const int32 Parent = ParentIndices[BoneIndex];
		if (Parent < 0)
		{
			continue;
		}
		check(Parent < NumBones);

		const FVector A = ComponentToWorld.TransformPosition(FVector{ ComponentPose[Parent].GetTranslation() });
		const FVector B = ComponentToWorld.TransformPosition(FVector{ ComponentPose[BoneIndex].GetTranslation() });
		DrawDebugLine(World, A, B, Color.ToFColor(true), false, Duration, SDPG_Foreground, Thickness);
	}
}

