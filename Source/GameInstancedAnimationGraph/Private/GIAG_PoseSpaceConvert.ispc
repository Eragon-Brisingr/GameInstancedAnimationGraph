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

// Per-slot topological sweep: each ISPC lane handles one active slot and walks bones
// [0..NumBones) in parent-before-child order, reading the parent's already-written
// component pose from the destination buffer instead of climbing the parent chain.
// Reads NumBones per slot vs. NumBones*AvgChainDepth in a per-bone walk-to-root variant,
// and BoneIndex stays uniform across lanes so there is no chain-depth divergence.
//
// Measured ~5x faster than per-bone walk on AVX2 at 5000-20000 slots x ~80 bones (CPU
// SIMD parallelism is well-saturated by the slot count alone, so trading lane count for
// less memory traffic and uniform control flow is a net win).
//
// Requires the standard UE invariant: ParentIndex < BoneIndex in the reference skeleton.
export void GIAG_ConvertLocalToComponentPoseTRS(
	uniform int NumBones,
	uniform int NumInstances,
	uniform int SlotCapacity,
	const uniform unsigned int32 ActiveInstanceIndices[],
	const uniform int ParentIndices[],
	const uniform FGIAG_BoneTRS SourcePoseTRS[],
	uniform FGIAG_BoneTRS DestinationPoseTRS[])
{
	if (NumInstances <= 0 || NumBones <= 0)
	{
		return;
	}

	foreach (ActiveIdx = 0 ... NumInstances)
	{
		GIAG_IGNORE_PERF_WARNING
		int SlotIndex = (int)ActiveInstanceIndices[ActiveIdx];
		if (SlotIndex < 0 || SlotIndex >= SlotCapacity)
		{
			continue;
		}

		// Bone index is uniform across lanes; parent is a uniform load for each iteration.
		for (uniform int BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const uniform int Parent = ParentIndices[BoneIndex];
			const int Index = SlotIndex * NumBones + BoneIndex;
			GIAG_IGNORE_PERF_WARNING
			FGIAG_BoneTRS Local = SourcePoseTRS[Index];

			FGIAG_BoneTRS OutBone;
			if (Parent < 0)
			{
				OutBone = Local;
			}
			else
			{
				const int ParentIndex = SlotIndex * NumBones + Parent;
				GIAG_IGNORE_PERF_WARNING
				const FGIAG_BoneTRS ParentComponent = DestinationPoseTRS[ParentIndex];
				OutBone = GIAG_MultiplyBoneTRS(Local, ParentComponent);
			}
			GIAG_IGNORE_PERF_WARNING
			DestinationPoseTRS[Index] = OutBone;
		}
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
