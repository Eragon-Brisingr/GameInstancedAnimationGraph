// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameInstancedAnimationGraphHandle.h"
#include "UObject/Interface.h"
#include "GIAG_ActorInterface.generated.h"

class UGameInstancedAnimationGraphSubsystem;

UINTERFACE()
class UGIAG_ActorInterface : public UInterface
{
	GENERATED_BODY()
};

class GAMEINSTANCEDANIMATIONGRAPH_API IGIAG_ActorInterface
{
	GENERATED_BODY()
	
	friend UGameInstancedAnimationGraphSubsystem;
public:
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="GameInstancedAnim")
	FGameInstancedAnimationGraphHandle GetInstancedAnimationGraphHandle() const;

	UFUNCTION(BlueprintNativeEvent, Category="GameInstancedAnim")
	USkeletalMeshComponent* GetInstancedAnimationSkinnedMesh() const;
	
protected:
	UFUNCTION(BlueprintNativeEvent, Category="GameInstancedAnim")
	void SetInstancedAnimationGraphHandle(const FGameInstancedAnimationGraphHandle& Handle);
};
