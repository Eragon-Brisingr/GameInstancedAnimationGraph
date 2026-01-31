// Fill out your copyright notice in the Description page of Project Settings.


#include "GameInstancedAnimationGraphHandle.h"

#include "GameInstancedAnimationGraphSubsystem.h"

bool FGameInstancedAnimationGraphHandle::IsValid() const
{
	if (InstancedAnimSubsystem == nullptr)
	{
		return false;
	}
	return InstancedAnimSubsystem->ResolveRecord(*this) != nullptr;
}
