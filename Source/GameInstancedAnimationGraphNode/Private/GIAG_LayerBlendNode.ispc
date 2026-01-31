// ISPC kernel for BlendLayer (PoseA/PoseB + per-bone weights + per-slot alpha) -> OutPose

#include "GIAG_AnimCommon.isph"

// Must match C++ layout of FGIAG_BlendLayerNode for direct AoS access.
struct FGIAG_BlendLayerNode_ISPC
{
	float Alpha;
	unsigned int32 _pad0;
	unsigned int32 _pad1;
	unsigned int32 _pad2;
};

export void GIAG_BlendLayerTRS(
	uniform int NumBones,
	uniform int NumInstances,
	uniform int SlotCapacity,
	const uniform unsigned int32 ActiveInstanceIndices[],
	const uniform float WeightsByBone[], // may be null
	const uniform FGIAG_BlendLayerNode_ISPC NodesBySlot[], // slot-indexed AoS node instances
	const uniform FGIAG_BoneTRS PoseA[],
	const uniform FGIAG_BoneTRS PoseB[],
	uniform FGIAG_BoneTRS OutPose[])
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

		int Index = SlotIndex * NumBones + BoneIndex;

	    GIAG_IGNORE_PERF_WARNING
		float A01 = saturate(NodesBySlot[SlotIndex].Alpha);
		if (A01 <= 0.0f)
		{
	        GIAG_IGNORE_PERF_WARNING
			OutPose[Index] = PoseA[Index];
			continue;
		}

		float BW = 0.0f;
		if (WeightsByBone != 0)
		{
		    GIAG_IGNORE_PERF_WARNING
			BW = saturate(WeightsByBone[BoneIndex]);
		}
		if (BW <= 0.0f)
		{
	        GIAG_IGNORE_PERF_WARNING
			OutPose[Index] = PoseA[Index];
			continue;
		}

		float W = BW * A01;
		if (W <= 0.0f)
		{
	        GIAG_IGNORE_PERF_WARNING
			OutPose[Index] = PoseA[Index];
			continue;
		}
		if (W >= 1.0f)
		{
	        GIAG_IGNORE_PERF_WARNING
			OutPose[Index] = PoseB[Index];
			continue;
		}

		// Inline blend2 (no 4-way): match GIAG_AnimCommon.ush rotation nlerp convention.
	    GIAG_IGNORE_PERF_WARNING
		FGIAG_BoneTRS TA = PoseA[Index];
	    GIAG_IGNORE_PERF_WARNING
		FGIAG_BoneTRS TB = PoseB[Index];

		FGIAG_BoneTRS R;
		R.Translation = TA.Translation * (1.0f - W) + TB.Translation * W;
		R.Scale3D = TA.Scale3D * (1.0f - W) + TB.Scale3D * W;

		FQuat Ref = GIAG_NormalizeQuat(TA.Rotation);
		FQuat QA = GIAG_AlignQuatToRef(Ref, TA.Rotation);
		FQuat QB = GIAG_AlignQuatToRef(Ref, TB.Rotation);
		FQuat Q = QA * (1.0f - W) + QB * W;
		R.Rotation = GIAG_NormalizeQuat(Q);

		R.TranslationPad = 0.0f;
		R.ScalePad = 0.0f;
		GIAG_IGNORE_PERF_WARNING
		OutPose[Index] = R;
	}
}

