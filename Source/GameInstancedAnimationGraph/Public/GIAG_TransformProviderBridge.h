#pragma once

#include "CoreMinimal.h"

#include "GIAG_AnimGraphGpuRunner.h"

/**
 * Render-thread payload for one ISKMC shard evaluation.
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
struct FGIAG_TransformProviderState final : public FThreadSafeRefCountedObject
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

private:
	// RT-only.
	TQueue<FGIAG_RenderPayload> PendingPayload_RT;

	// RT-only persistent caches.
	TUniquePtr<FGIAG_AnimGraphGpuRunner> RunnerRT;
};

