// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPtr.h"
#include "GameInstancedAnimationGraphSettings.generated.h"

class UNiagaraSystem;

UCLASS(Config=Game, DefaultConfig)
class GAMEINSTANCEDANIMATIONGRAPH_API UGameInstancedAnimationGraphSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UGameInstancedAnimationGraphSettings();
	
	/** If true, use GIAG native renderer (UGIAG_AttachMeshComponent) for StaticMesh attachments (no Niagara, no 1-frame GPU sim latency). */
	UPROPERTY(EditAnywhere, Config)
	bool bUseNativeStaticMeshAttachRenderer = true;

	UPROPERTY(EditAnywhere, Config)
	TSoftObjectPtr<UNiagaraSystem> GlobalStaticMeshAttachNiagaraSystem;
};
