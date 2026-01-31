// Fill out your copyright notice in the Description page of Project Settings.


#include "GIAG_AnimGraphBuilder.h"

#include "GIAG_AnimNodeMetaManager.h"

void FGIAG_AnimGraphBuilder::Link(const FGIAG_AnimOutputPinRef& FromOutput, const FGIAG_AnimInputPinRef& ToInput)
{
	Links.Add({ FromOutput, ToInput });
}

void FGIAG_AnimGraphBuilder::SetFinalPose(const FGIAG_AnimOutputPinRef& FinalPoseOutput)
{
	FinalPose = FinalPoseOutput;
}

int32 FGIAG_AnimGraphBuilder::AddNode(const UScriptStruct* NodeStruct, const void* NodePtr, const UScriptStruct* SettingsStruct, const void* SettingsPtr)
{
	int32 Offset = INDEX_NONE;
	FName MemberName = NAME_None;
	checkf(ResolveNodeMemberByPtr(NodePtr, Offset, MemberName), TEXT("GIAG: AddNode(node): failed to resolve node member. Ensure you passed a UPROPERTY member of the AnimGraph's DefaultGraphInstance."));

	auto Meta = FGIAG_AnimNodeMetaManager::Get().FindMetaChecked(NodeStruct);
	const int32 Idx = NodeMetas.Add(Meta);
	NodeSettings.AddDefaulted();
	NodeInstanceOffsets.Add(Offset);
	NodeMemberNames.Add(MemberName);
	if (SettingsStruct && SettingsPtr)
	{
		NodeSettings[Idx] = { SettingsStruct, (uint8*)SettingsPtr };
	}
	return Idx;
}

bool FGIAG_AnimGraphBuilder::ResolveNodeMemberByPtr(const void* NodePtr, int32& OutMemberOffset, FName& OutMemberName) const
{
	if (!DefaultInstance.IsValid())
	{
		return false;
	}
	if (!NodePtr)
	{
		return false;
	}

	int32 Matches = 0;
	for (TFieldIterator<FProperty> It(DefaultInstance.GetScriptStruct()); It; ++It)
	{
		const FStructProperty* SP = CastField<FStructProperty>(*It);
		if (!SP || !SP->Struct || !SP->Struct->IsChildOf(FGIAG_AnimNodeBase::StaticStruct()))
		{
			continue;
		}
		const int32 Offset = SP->GetOffset_ForInternal();
		if (DefaultInstance.GetMemory() + Offset == NodePtr)
		{
			OutMemberOffset = Offset;
			OutMemberName = It->GetFName();
			Matches++;
		}
	}
	return Matches == 1;
}
