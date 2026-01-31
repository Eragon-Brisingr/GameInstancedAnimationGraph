#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GIAG_ClipPlayerNode.generated.h"

/**
 * ClipPlayer parameter layout.
 * Layout MUST match `Shaders/GIAG_AnimCommon.ush` struct `FGIAG_SlotState`.
 */
struct alignas(4) FGIAG_ClipState
{
	int32 Clip = 0;
	float StartTime = 0.0f;
	float StartSeconds = 0.0f;
	float PlayRate = 1.0f;
	uint32 bLoop = 1;
};

struct alignas(4) FGIAG_SlotState
{
	FGIAG_ClipState Clips[4];
	uint32 NumClips = 0;
	float BlendStartTimes[3] = {0.0f, 0.0f, 0.0f};
	float BlendDurations[3] = {0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(FGIAG_ClipState) == 20, "FGIAG_ClipState layout changed; update shader struct to match.");
static_assert(sizeof(FGIAG_SlotState) == 108, "FGIAG_SlotState layout changed; update shader struct to match.");

/** Clip player node instance (0 inputs, 1 pose output). */
USTRUCT(BlueprintType)
struct alignas(16) GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_ClipPlayerNode final : public FGIAG_AnimNodeBase
{
	GENERATED_BODY()
public:
	using FNodeMeta = TGIAG_AnimNodeMeta<FGIAG_ClipPlayerNode>;

	/** Play/queue a clip transition (same semantics as Subsystem::PlayAnimation). */
	void PlayAnimation(const FGIAG_AnimNodeRef& NodeRef, const UAnimSequence* AnimSequence, float BlendDurationSeconds = 0.2f, float StartSeconds = 0.0f, bool bLoop = true, float PlayRate = 1.0f);
protected:
	friend FNodeMeta;
	FGIAG_SlotState SlotState;

	/** Enumerate referenced clip indices for cleanup / CPU->GPU bake. */
	void EnumerateClips(TArray<int32>& OutClipIndices) const;

	const void* GatherUploadsGPU(uint32& OutUploadStrideBytes) const;

	static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context);
	static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context);
};

UCLASS(meta = (ScriptMixin = FGIAG_ClipPlayerNode))
class UGIAG_ClipPlayerNodeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable)
	static void PlayAnimation(UPARAM(Ref)FGIAG_ClipPlayerNode& Node, const FGIAG_AnimNodeRef& NodeRef, const UAnimSequence* AnimSequence, float BlendDurationSeconds = 0.2f, float StartSeconds = 0.0f, bool bLoop = true, float PlayRate = 1.0f)
	{
		Node.PlayAnimation(NodeRef, AnimSequence, BlendDurationSeconds, StartSeconds, bLoop, PlayRate);
	}
};