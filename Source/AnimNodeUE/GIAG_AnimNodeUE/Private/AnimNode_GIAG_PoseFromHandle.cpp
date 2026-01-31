#include "AnimNode_GIAG_PoseFromHandle.h"

#include "GameInstancedAnimationGraphSubsystem.h"
#include "Animation/AnimInstanceProxy.h"

namespace
{
	static void ApplyLocalPoseToOutput(const TConstArrayView<FTransform3f>& LocalPose, FPoseContext& Output)
	{
		Output.ResetToRefPose();

		FCompactPose& Pose = Output.Pose;
		const FBoneContainer& RequiredBones = Pose.GetBoneContainer();

		for (const FCompactPoseBoneIndex PoseBoneIndex : Pose.ForEachBoneIndex())
		{
			const int32 SkeletonBoneIndex = RequiredBones.GetSkeletonIndex(PoseBoneIndex);
			if (LocalPose.IsValidIndex(SkeletonBoneIndex))
			{
				Pose[PoseBoneIndex] = FTransform{ LocalPose[SkeletonBoneIndex] };
			}
		}

		Pose.NormalizeRotations();
	}
}

void FAnimNode_GIAG_PoseFromHandle::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
}

void FAnimNode_GIAG_PoseFromHandle::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	FAnimNode_Base::CacheBones_AnyThread(Context);
}

void FAnimNode_GIAG_PoseFromHandle::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	FAnimNode_Base::Update_AnyThread(Context);

	GetEvaluateGraphExposedInputs().Execute(Context);
}

void FAnimNode_GIAG_PoseFromHandle::Evaluate_AnyThread(FPoseContext& Output)
{
	Output.ResetToRefPose();

	const UGameInstancedAnimationGraphSubsystem* Subsystem = Handle.InstancedAnimSubsystem.Get();
	if (!Subsystem || Handle.RecordIndex == INDEX_NONE)
	{
		return;
	}

	// Fast path: use per-frame cache.
	{
		uint64 CacheFrame = 0;
		TConstArrayView<FTransform3f> CachedLocalPose;
		if (Subsystem->TryGetCpuPoseCache_NoLock(Handle, CacheFrame, CachedLocalPose) && CacheFrame == GFrameCounter && CachedLocalPose.Num() > 0)
		{
			ApplyLocalPoseToOutput(CachedLocalPose, Output);
			return;
		}
	}

	// Fallback: evaluate this handle on CPU and output.
	TArray<FTransform3f> LocalPose;
	if (Subsystem->EvalCpuAnimationPoseAnyThread(Handle, LocalPose))
	{
		ApplyLocalPoseToOutput(LocalPose, Output);
	}
}

void FAnimNode_GIAG_PoseFromHandle::GatherDebugData(FNodeDebugData& DebugData)
{
	DebugData.AddDebugItem(DebugData.GetNodeName(this));
}

