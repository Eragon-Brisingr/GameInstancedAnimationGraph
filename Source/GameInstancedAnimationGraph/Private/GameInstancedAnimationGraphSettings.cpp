// Fill out your copyright notice in the Description page of Project Settings.


#include "GameInstancedAnimationGraphSettings.h"

UGameInstancedAnimationGraphSettings::UGameInstancedAnimationGraphSettings()
{
	GlobalStaticMeshAttachNiagaraSystem = FSoftObjectPath{ TEXT("/GameInstancedAnimationGraph/GIAG_StaticMeshAttach.GIAG_StaticMeshAttach") };
}
