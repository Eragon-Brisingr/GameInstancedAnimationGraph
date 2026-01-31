// Fill out your copyright notice in the Description page of Project Settings.


#include "GIAG_AnimNodeMetaManager.h"

#include "GIAG_AnimNodeBase.h"

const FGIAG_AnimNodeMetaManager& FGIAG_AnimNodeMetaManager::Get()
{
	static FGIAG_AnimNodeMetaManager Manager;
	return Manager;
}

void FGIAG_AnimNodeMetaManager::InitManager() const
{
	StructMetaMap.Reset();
	for (auto Meta : Metas)
	{
		StructMetaMap.Add(Meta->GetStruct(), Meta);
	}
}
