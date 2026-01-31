#pragma once

#include "CoreMinimal.h"

#include "Misc/MemStack.h"
#include "BonePose.h"
#include "BoneContainer.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/Skeleton.h"

namespace GIAG
{
	// CPU-side pose evaluation for animation sequences.
	FORCEINLINE bool EvalAnimSequenceLocalPose(
		const UAnimSequence* Anim,
		double TimeSeconds,
		USkeleton* Skeleton,
		TArray<FTransform>& OutLocalTransforms)
	{
		check(Anim);
		check(Skeleton);
		check(Anim->GetSkeleton() == Skeleton);

		FMemMark Mark(FMemStack::Get());

		const int32 NumBones = Skeleton->GetReferenceSkeleton().GetNum();
		check(NumBones > 0);

		TArray<FBoneIndexType> RequiredBoneIndexArray;
		RequiredBoneIndexArray.AddUninitialized(NumBones);
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			RequiredBoneIndexArray[BoneIndex] = (FBoneIndexType)BoneIndex;
		}

		FBoneContainer RequiredBones;
		RequiredBones.InitializeTo(
			RequiredBoneIndexArray,
			UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::DisallowAll),
			*Skeleton);
		RequiredBones.SetUseRAWData(false);

		FCompactPose CompactPose;
		FBlendedCurve Curve;
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData PoseData(CompactPose, Curve, Attributes);

		CompactPose.SetBoneContainer(&RequiredBones);
		Curve.InitFrom(RequiredBones);

		FAnimExtractContext Context(TimeSeconds, false);
		Anim->GetAnimationPose(PoseData, Context);

		OutLocalTransforms.SetNumZeroed(NumBones);
		for (const FCompactPoseBoneIndex BoneIndex : CompactPose.ForEachBoneIndex())
		{
			const int32 SkeletonBoneIndex = RequiredBones.GetSkeletonIndex(BoneIndex);
			check(SkeletonBoneIndex != INDEX_NONE);

			FTransform T = CompactPose[BoneIndex];
			T.NormalizeRotation();
			OutLocalTransforms[SkeletonBoneIndex] = T;
		}
		return true;
	}

}
