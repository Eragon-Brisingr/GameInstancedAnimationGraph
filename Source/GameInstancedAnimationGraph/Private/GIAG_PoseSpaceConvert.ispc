// ISPC kernels for pose-space conversion used by CPU runner:
// - LocalPose -> ComponentPose
// - ComponentPose -> LocalPose

#include "GIAG_AnimCommon.isph"

inline FGIAG_BoneTRS GIAG_MultiplyBoneTRS(FGIAG_BoneTRS A, FGIAG_BoneTRS B)
{
	FGIAG_BoneTRS R;
	R.Rotation = GIAG_QuatMultiply(B.Rotation, A.Rotation);
	R.Translation = GIAG_QuatRotateVector(B.Rotation, B.Scale3D * A.Translation) + B.Translation;
	R.TranslationPad = 0.0f;
	R.Scale3D = A.Scale3D * B.Scale3D;
	R.ScalePad = 0.0f;
	R.Rotation = GIAG_NormalizeQuat(R.Rotation);
	return R;
}

inline FGIAG_BoneTRS GIAG_ConvertComponentToLocal(FGIAG_BoneTRS ComponentBone, FGIAG_BoneTRS ParentComponentBone)
{
	FGIAG_BoneTRS Result;
	FQuat ParentQuat = GIAG_NormalizeQuat(ParentComponentBone.Rotation);
	FQuat ComponentQuat = GIAG_NormalizeQuat(ComponentBone.Rotation);
	FQuat ParentInv = GIAG_FLOAT4(-GIAG_GET4(ParentQuat, 0), -GIAG_GET4(ParentQuat, 1), -GIAG_GET4(ParentQuat, 2), GIAG_GET4(ParentQuat, 3));

	Result.Rotation = GIAG_NormalizeQuat(GIAG_QuatMultiply(ParentInv, ComponentQuat));
	const float3 DeltaTranslation = ComponentBone.Translation - ParentComponentBone.Translation;
	Result.Translation = GIAG_QuatRotateVector(ParentInv, DeltaTranslation) / ParentComponentBone.Scale3D;
	Result.TranslationPad = 0.0f;
	Result.Scale3D = ComponentBone.Scale3D / ParentComponentBone.Scale3D;
	Result.ScalePad = 0.0f;
	return Result;
}

export void GIAG_ConvertLocalToComponentPoseTRS(
	uniform int NumBones,
	uniform int NumInstances,
	uniform int SlotCapacity,
	const uniform unsigned int32 ActiveInstanceIndices[],
	const uniform int ParentIndices[],
	const uniform FGIAG_BoneTRS SourcePoseTRS[],
	uniform FGIAG_BoneTRS DestinationPoseTRS[])
{
	const uniform int Total = NumBones * NumInstances;
	if (Total <= 0)
	{
		return;
	}

	foreach (DispatchId = 0 ... Total)
	{
		GIAG_IGNORE_PERF_WARNING
		int ActiveIndex = DispatchId / NumBones;
		int BoneIndex = DispatchId - ActiveIndex * NumBones;
		GIAG_IGNORE_PERF_WARNING
		int SlotIndex = (int)ActiveInstanceIndices[ActiveIndex];
		if (SlotIndex < 0 || SlotIndex >= SlotCapacity)
		{
			continue;
		}

		const int OutIndex = SlotIndex * NumBones + BoneIndex;
		GIAG_IGNORE_PERF_WARNING
		FGIAG_BoneTRS ComponentBone = SourcePoseTRS[OutIndex];
		GIAG_IGNORE_PERF_WARNING
		int Parent = ParentIndices[BoneIndex];
		while (Parent >= 0)
		{
			const int ParentIndex = SlotIndex * NumBones + Parent;
			GIAG_IGNORE_PERF_WARNING
			const FGIAG_BoneTRS ParentLocal = SourcePoseTRS[ParentIndex];
			ComponentBone = GIAG_MultiplyBoneTRS(ComponentBone, ParentLocal);
			GIAG_IGNORE_PERF_WARNING
			Parent = ParentIndices[Parent];
		}
		GIAG_IGNORE_PERF_WARNING
		DestinationPoseTRS[OutIndex] = ComponentBone;
	}
}

export void GIAG_ConvertComponentToLocalPoseTRS(
	uniform int NumBones,
	uniform int NumInstances,
	uniform int SlotCapacity,
	const uniform unsigned int32 ActiveInstanceIndices[],
	const uniform int ParentIndices[],
	const uniform FGIAG_BoneTRS SourcePoseTRS[],
	uniform FGIAG_BoneTRS DestinationPoseTRS[])
{
	const uniform int Total = NumBones * NumInstances;
	if (Total <= 0)
	{
		return;
	}

	foreach (DispatchId = 0 ... Total)
	{
		GIAG_IGNORE_PERF_WARNING
		int ActiveIndex = DispatchId / NumBones;
		int BoneIndex = DispatchId - ActiveIndex * NumBones;
		GIAG_IGNORE_PERF_WARNING
		int SlotIndex = (int)ActiveInstanceIndices[ActiveIndex];
		if (SlotIndex < 0 || SlotIndex >= SlotCapacity)
		{
			continue;
		}

		const int OutIndex = SlotIndex * NumBones + BoneIndex;
		GIAG_IGNORE_PERF_WARNING
		const FGIAG_BoneTRS ComponentBone = SourcePoseTRS[OutIndex];
		GIAG_IGNORE_PERF_WARNING
		const int Parent = ParentIndices[BoneIndex];
		if (Parent >= 0)
		{
			const int ParentIndex = SlotIndex * NumBones + Parent;
			GIAG_IGNORE_PERF_WARNING
			const FGIAG_BoneTRS ParentComponentBone = SourcePoseTRS[ParentIndex];
			GIAG_IGNORE_PERF_WARNING
			DestinationPoseTRS[OutIndex] = GIAG_ConvertComponentToLocal(ComponentBone, ParentComponentBone);
		}
		else
		{
			GIAG_IGNORE_PERF_WARNING
			DestinationPoseTRS[OutIndex] = ComponentBone;
		}
	}
}
