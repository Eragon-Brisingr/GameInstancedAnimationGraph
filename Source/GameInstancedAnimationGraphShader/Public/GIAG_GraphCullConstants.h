#pragma once

#include "CoreMinimal.h"

namespace GIAG
{
	// GraphCull cull-param SRVs are bound by fixed registers (explicit `register(tN)` in generated HLSL),
	// so C++ does not need compiler ParameterMap reflection for generated symbols.
	//
	// Contract:
	// - `ActiveInstanceIndices` uses t0 (see `GIAG_GraphCull_CS.usf`).
	// - Cull param buffers use [t(GraphCullParamSRVRegisterBase) .. t(GraphCullParamSRVRegisterBase + MaxGraphCullParamBuffers-1)].
	static constexpr uint16 GraphCullParamSRVRegisterBase = 1; // start at t1 (t0 reserved)
	static constexpr uint16 MaxGraphCullParamBuffers = 32;
}

