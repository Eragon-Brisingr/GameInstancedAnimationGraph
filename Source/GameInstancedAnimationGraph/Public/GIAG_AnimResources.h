#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "GIAG_AnimNodeBase.h"

/**
 * Render-thread owned resource cache for AnimGraph.
 *
 * IMPORTANT threading contract:
 * - This cache is intended to be mutated and read on the render thread only (inside the Runner's enqueued RDG work).
 * - Game thread should only pass pointers/references through FGIAG_AnimGraphRunParams and must not access it concurrently.
 */
struct FGIAG_AnimResourceCache
{
	/** ShareKey -> external pooled buffer */
	TMap<FGIAG_AnimResourceKey, TRefCountPtr<FRDGPooledBuffer>> Buffers;
};


