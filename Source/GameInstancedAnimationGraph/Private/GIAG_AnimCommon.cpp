// Fill out your copyright notice in the Description page of Project Settings.


#include "GIAG_AnimCommon.h"

#include "DynamicRHI.h"

namespace GIAG
{
	// for unit test CPU/GPU ConsistencyTest
	GAMEINSTANCEDANIMATIONGRAPH_API bool bQuantTimeInCpuEval = false;

	bool IsNullRHI()
	{
		static const bool bIsNullRHI = GDynamicRHI && FCString::Stricmp(GDynamicRHI->GetName(), TEXT("Null")) == 0;
		return bIsNullRHI;
	}
}
