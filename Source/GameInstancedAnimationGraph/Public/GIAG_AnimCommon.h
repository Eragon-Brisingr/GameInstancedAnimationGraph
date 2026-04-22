// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GIAG_Math.h"

/**
 * Strongly-typed TRS payload shared between C++ and HLSL StructuredBuffers.
 *
 */
using FGIAG_BoneTRS = FTransform3f;
using FGIAG_Transform = FTransform3f;

/** Shader-visible clip meta (must match `Shaders/GIAG_AnimCommon.ush`). */
struct alignas(4) FGIAG_ClipMeta
{
	int32 StartTransformIndex = 0;
	int32 NumFrames = 1;
	float SecondsPerFrame = 1.0f / 30.0f;
	float SequenceLengthSeconds = 0.0f;
};

/**
 * Niagara attach desc packed payload shared between C++ and HLSL StructuredBuffers.
 * Must match `FGIAG_AttachDescPacked` in `Shaders/Common/GIAG_AttachToTransformBuffer_CS.usf`.
 */
struct alignas(16) FGIAG_AttachDescPacked
{
	uint32 SlotIndex = 0;
	uint32 BoneIndex = 0;
	uint32 OutputIndex = 0;
	uint32 Flags = 0;
	FGIAG_BoneTRS SocketLocalTRS = FGIAG_BoneTRS::Identity;
};

class FRDGPooledBuffer;

namespace GIAG
{
	// UE 5.7 packs `FSkinningHeader::UniqueAnimationCount` into 7 bits (see Engine/Shaders/Shared/SkinningDefinitions.h),
	// so a single instanced skinned mesh component can expose at most 127 unique animation slots.
	// We shard components so each stays within this engine constraint; computation data lives on FMeshBucket.
	static constexpr int32 DefaultSlotsPerShard = 127;

	// ---------------------------------------------------------------------
	// Node culling (CPU): input need-mask helpers
	//
	// NeedMask convention:
	// - bit i => input pin i is needed (and therefore should be evaluated/propagated)
	// - pins are indexed [0, NumInputs)
	// ---------------------------------------------------------------------

	FORCEINLINE uint32 AllInputsMask(uint32 NumInputs)
	{
		// Keep a safe behavior for large pin counts; graph v1 uses small counts.
		if (NumInputs >= 32u) { return 0xFFFFFFFFu; }
		return (NumInputs == 0u) ? 0u : ((1u << NumInputs) - 1u);
	}

	FORCEINLINE uint32 InputPinMask(uint32 PinIndex)
	{
		// Contract: PinIndex < 32.
		return (1u << PinIndex);
	}

	FORCEINLINE bool IsInputPinNeeded(uint32 NeedMask, uint32 PinIndex)
	{
		return (NeedMask & InputPinMask(PinIndex)) != 0u;
	}

	/** Persistent GPU buffers for one (merged) AnimLibrary atlas (owned by Subsystem). */
	struct FAnimLibraryBuffers
	{
		TRefCountPtr<FRDGPooledBuffer> ClipMetas;
		TRefCountPtr<FRDGPooledBuffer> AnimTRS;
		TRefCountPtr<FRDGPooledBuffer> RefPoseLocalTRS;

		uint32 NumClips = 0;
		uint32 NumBones = 0;
		uint32 AnimTRSNum = 0;
		uint32 AnimTRSCapacity = 0;
		uint32 RefPoseVersion = 0;
	};

	GAMEINSTANCEDANIMATIONGRAPH_API bool IsNullRHI();

	inline float QuantTime(float PlaybackTimeSeconds, float Len, float SecondsPerFrame, bool bLoop)
	{
		const float SPF = FMath::Max(1.0f / 120.0f, SecondsPerFrame);

		// Mirror shader `GIAG_CalcFrameIndex`:
		// - loop: T = fmod(T, Len) in [0, Len)
		// - clamp: T in [0, Len]
		float T = PlaybackTimeSeconds;
		if (bLoop)
		{
			if (Len > 1e-6f)
			{
				T = FMath::Fmod(T, Len);
				if (T < 0.0f) { T += Len; }
			}
			else
			{
				T = 0.0f;
			}
		}
		else
		{
			T = FMath::Clamp(T, 0.0f, Len);
		}

		// Bake formula matches Subsystem: NumFrames = ceil(Len/SPF)+1, so max frame index = ceil(Len/SPF).
		const int32 MaxFrame = FMath::Max(0, FMath::CeilToInt(Len / SPF));
		const int32 FrameIndex = FMath::Clamp(FMath::FloorToInt(T / SPF), 0, MaxFrame);
		return FMath::Min((float)FrameIndex * SPF, Len);
	}
}
