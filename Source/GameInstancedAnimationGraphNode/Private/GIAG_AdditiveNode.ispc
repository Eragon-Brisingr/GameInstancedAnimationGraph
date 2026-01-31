// ISPC kernel for Additive (Base + Additive scaled by per-slot alpha) -> OutPose
//
// Semantics match UE's local-space AccumulateAdditivePose:
// - Translation is additive
// - Rotation is multiplicative (delta quaternion multiplied onto base)
// - Scale3D uses additive-scale delta: finalScale *= (1 + deltaScale * alpha)

#include "GIAG_AnimCommon.isph"

// Must match C++ layout of FGIAG_AdditiveNode for direct AoS access.
struct FGIAG_AdditiveNode_ISPC
{
	float Alpha;
	unsigned int32 _pad0;
	unsigned int32 _pad1;
	unsigned int32 _pad2;
};

export void GIAG_ApplyAdditiveTRS(
	uniform int NumBones,
	uniform int NumInstances,
	uniform int SlotCapacity,
	const uniform unsigned int32 ActiveInstanceIndices[],
	const uniform FGIAG_AdditiveNode_ISPC NodesBySlot[], // slot-indexed AoS node instances
	const uniform FGIAG_BoneTRS BasePose[],
	const uniform FGIAG_BoneTRS AdditivePose[],
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
			OutPose[Index] = BasePose[Index];
			continue;
		}

	    GIAG_IGNORE_PERF_WARNING
		FGIAG_BoneTRS Base = BasePose[Index];
	    GIAG_IGNORE_PERF_WARNING
		FGIAG_BoneTRS Add = AdditivePose[Index];

		if (A01 >= 1.0f)
		{
			FGIAG_BoneTRS R;
			R.Translation = Base.Translation + Add.Translation;
			R.Scale3D = Base.Scale3D * (GIAG_FLOAT3(1.0f, 1.0f, 1.0f) + Add.Scale3D);
			R.Rotation = GIAG_NormalizeQuat(GIAG_QuatMultiply(Add.Rotation, Base.Rotation));
			R.TranslationPad = 0.0f;
			R.ScalePad = 0.0f;
	        GIAG_IGNORE_PERF_WARNING
			OutPose[Index] = R;
			continue;
		}

		const float OneMinus = 1.0f - A01;
		const float Bias = (GIAG_GET4(Add.Rotation, 3) >= 0.0f) ? 1.0f : -1.0f;

		// Nlerp(Identity, Add.Rotation, A01) with shortest-path bias (identity is (0,0,0,1)).
		FQuat BlendedRot = GIAG_FLOAT4(
			GIAG_GET4(Add.Rotation, 0) * A01,
			GIAG_GET4(Add.Rotation, 1) * A01,
			GIAG_GET4(Add.Rotation, 2) * A01,
			GIAG_GET4(Add.Rotation, 3) * A01 + Bias * OneMinus);
		BlendedRot = GIAG_NormalizeQuat(BlendedRot);

		FGIAG_BoneTRS R;
		R.Translation = Base.Translation + Add.Translation * A01;
		R.Scale3D = Base.Scale3D * (GIAG_FLOAT3(1.0f, 1.0f, 1.0f) + Add.Scale3D * A01);
		R.Rotation = GIAG_NormalizeQuat(GIAG_QuatMultiply(BlendedRot, Base.Rotation));
		R.TranslationPad = 0.0f;
		R.ScalePad = 0.0f;
	    GIAG_IGNORE_PERF_WARNING
		OutPose[Index] = R;
	}
}

