// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

struct IGIAG_AnimNodeMeta;

class FGameInstancedAnimationGraphModule;

class GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimNodeMetaManager : FNoncopyable
{
	friend IGIAG_AnimNodeMeta;
	friend FGameInstancedAnimationGraphModule;
public:
	static const FGIAG_AnimNodeMetaManager& Get();
	
	const IGIAG_AnimNodeMeta* FindMetaChecked(const UScriptStruct* Struct) const
	{
		return StructMetaMap.FindChecked(Struct);
	}

	const auto& GetMetas() const { return Metas; }
private:
	mutable TArray<const IGIAG_AnimNodeMeta*> Metas;
	mutable TMap<const UScriptStruct*, const IGIAG_AnimNodeMeta*> StructMetaMap;
	
	void InitManager() const;
};

#define GIAG_REGISTER_ANIM_NODE(NodeType) \
namespace GIAG::NodeMeta { NodeType::FNodeMeta NodeType##_Meta; }
	
