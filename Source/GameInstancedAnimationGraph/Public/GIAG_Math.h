#pragma once

#include "CoreMinimal.h"

namespace GIAG
{
	FORCEINLINE float Clamp01(float V)
	{
		return FMath::Clamp(V, 0.0f, 1.0f);
	}
}
