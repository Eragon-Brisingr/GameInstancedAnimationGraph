#pragma once

#include "CoreMinimal.h"

namespace GIAG
{
	FORCEINLINE float Clamp01(float Value)
	{
		return FMath::Clamp(Value, 0.0f, 1.0f);
	}
}
