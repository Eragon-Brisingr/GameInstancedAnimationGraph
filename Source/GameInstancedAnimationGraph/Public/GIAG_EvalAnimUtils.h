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
#include "GIAG_SkeletonUserData.h"

namespace GIAG
{
	struct FRootRotationOffset
	{
		FTransform Transform = FTransform::Identity;
		bool bEnabled = false;

		FRootRotationOffset() = default;

		explicit FRootRotationOffset(const FRotator& Rotator)
		{
			if (!Rotator.IsNearlyZero())
			{
				Transform = FTransform(Rotator.Quaternion());
				bEnabled = true;
			}
		}

		FORCEINLINE void Apply(FTransform& RootTransform) const
		{
			if (!bEnabled)
			{
				return;
			}

			RootTransform = RootTransform * Transform;
			RootTransform.NormalizeRotation();
		}
	};

	FORCEINLINE FRootRotationOffset GetSkeletonRootRotationOffset(USkeleton* Skeleton)
	{
		check(Skeleton);
		if (const UGIAG_SkeletonUserData* UserData = Cast<UGIAG_SkeletonUserData>(Skeleton->GetAssetUserDataOfClass(UGIAG_SkeletonUserData::StaticClass())))
		{
			return FRootRotationOffset(UserData->RootRotationOffset);
		}
		return FRootRotationOffset();
	}

	FORCEINLINE void ApplySkeletonRootRotationOffset(USkeleton* Skeleton, FTransform& RootTransform)
	{
		GetSkeletonRootRotationOffset(Skeleton).Apply(RootTransform);
	}

	// CPU-side pose evaluation for animation sequences.
	FORCEINLINE bool EvalAnimSequenceLocalPose(
		const UAnimSequence* Anim,
		double TimeSeconds,
		USkeleton* Skeleton,
		TArray<FTransform>& OutLocalTransforms,
		bool bExtractRootMotion = false)
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

		FAnimExtractContext Context(TimeSeconds, bExtractRootMotion);
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

	FORCEINLINE bool EvalAnimSequenceVisualLocalPose(
		const UAnimSequence* Anim,
		double TimeSeconds,
		USkeleton* Skeleton,
		TArray<FTransform>& OutLocalTransforms,
		const FRootRotationOffset& RootRotationOffset)
	{
		if (!EvalAnimSequenceLocalPose(Anim, TimeSeconds, Skeleton, OutLocalTransforms, Anim->HasRootMotion()))
		{
			return false;
		}
		check(OutLocalTransforms.Num() > 0);
		RootRotationOffset.Apply(OutLocalTransforms[0]);
		return true;
	}

	FORCEINLINE bool EvalAnimSequenceVisualLocalPose(
		const UAnimSequence* Anim,
		double TimeSeconds,
		USkeleton* Skeleton,
		TArray<FTransform>& OutLocalTransforms)
	{
		const FRootRotationOffset RootRotationOffset = GetSkeletonRootRotationOffset(Skeleton);
		if (!EvalAnimSequenceVisualLocalPose(Anim, TimeSeconds, Skeleton, OutLocalTransforms, RootRotationOffset))
		{
			return false;
		}
		return true;
	}

}
