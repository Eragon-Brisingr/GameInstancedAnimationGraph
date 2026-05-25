#pragma once

#include "CoreMinimal.h"

#include "GIAG_AnimCommon.h"
#include "GIAG_AnimGraphGpuRunner.h"

/**
 * Render-thread payload for one bucket evaluation (one ISKMC per bucket since UE 5.8).
 * Produced on GT, transferred to RT via ENQUEUE_RENDER_COMMAND, consumed by TransformProvider callback.
 */
struct FGIAG_RenderPayload
{
	const FGIAG_AnimGraphCompiledData* Compiled = nullptr;

	FGIAG_AnimGraphRunParams Params;
	FGIAG_AnimGraphUploads Uploads;
};

/**
 * Shared provider state with explicit lifetime management across GT/RT.
 *
 * Notes:
 * - The render proxy holds a ref on RT, so UGIAG_TransformProviderData GC cannot free this early.
 * - Pending payload is RT-owned and only touched on RT.
 * - Runner/resources caches are RT-only (no GT access) to avoid data races.
 */
struct FGIAG_TransformProviderState final : FRefCountedObject
{
	FGIAG_TransformProviderState()
	{
		RunnerRT = MakeUnique<FGIAG_AnimGraphGpuRunner>();
	}

	/** RT-only: replace the pending payload. */
	void EnqueuePayload_RenderThread(FGIAG_RenderPayload&& NewPayload)
	{
		check(IsInRenderingThread());
		PendingPayload_RT.Enqueue(MoveTemp(NewPayload)) ;
	}

	/** RT-only: consume payload (caller owns). */
	bool DequeuePayload_RenderThread(FGIAG_RenderPayload& OutPayload)
	{
		check(IsInRenderingThread());
		return PendingPayload_RT.Dequeue(OutPayload);
	}

	/** RT-only runner. */
	FGIAG_AnimGraphGpuRunner* GetRunnerRT() const { return RunnerRT.Get(); }

	/** Total slot capacity for the bucket. GT-only — RT-side capacity is delivered via PendingCapacityChange_RT.
	 *  One ISKMC backs the whole bucket. */
	int32 SlotCapacity = 0;
	int32 GetTotalCapacity() const { return SlotCapacity; }

	/**
	 * Pending capacity change for this bucket (RT-only). Set by ENQUEUE_RENDER_COMMAND from GT,
	 * consumed by the renderer extension during ProvideTransforms.
	 *
	 * - PendingNewCap > 0: the bucket wants the engine ExtensionProxy's UniqueAnimationCount to
	 *   become NewCap. The renderer extension will call SetUniqueAnimationCount on the proxy,
	 *   and the engine's per-frame polling will reallocate + copy the per-primitive TransformBuffer
	 *   span on the next pre-update.
	 * - PendingSlotMoves: when shrinking with compaction, lists (OldSlot, NewSlot) pairs the GPU
	 *   compaction CS will use to remap data within the existing span before SetUniqueAnimationCount.
	 *   Empty for grow paths.
	 *
	 * Once the renderer has applied the change it resets PendingNewCap to 0 and empties the
	 * move list. Both fields are RT-owned; only access from RT after enqueue.
	 */
	struct FGIAG_PendingCapacityChange
	{
		struct FSlotMove { uint32 OldSlot; uint32 NewSlot; };

		int32 PendingNewCap = 0;
		TArray<FSlotMove> PendingSlotMoves;
	};
	FGIAG_PendingCapacityChange PendingCapacityChange_RT;

private:
	// RT-only.
	TQueue<FGIAG_RenderPayload> PendingPayload_RT;

	// RT-only persistent caches.
	TUniquePtr<FGIAG_AnimGraphGpuRunner> RunnerRT;
};

