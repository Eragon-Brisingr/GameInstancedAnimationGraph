// Fill out your copyright notice in the Description page of Project Settings.


#include "GIAG_AnimCommon.h"

#include "DynamicRHI.h"

namespace GIAG
{
	// Tests can force CPU evaluation through the same baked-frame interpolation path used by GPU.
	GAMEINSTANCEDANIMATIONGRAPH_API bool bUseBakedFrameInterpolationInCpuEval = false;

	bool IsNullRHI()
	{
		static const bool bIsNullRHI = GDynamicRHI && FCString::Stricmp(GDynamicRHI->GetName(), TEXT("Null")) == 0;
		return bIsNullRHI;
	}
}
