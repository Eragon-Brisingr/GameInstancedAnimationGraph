// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "GIAG_AnimGraphBuilder.generated.h"

// ---------------- Strongly typed (compile-time checked) pin refs ----------------

template<typename TNode>
struct TGIAG_AnimNodePinTraits
{
private:
	static consteval int32 ComputeNumInputPins()
	{
		if constexpr (requires { TNode::GetNumInputPins(); })
		{
			return (int32)TNode::GetNumInputPins();
		}
		else
		{
			return (int32)TNode::EInputPin::Num;
		}
	}

	static consteval int32 ComputeNumOutputPins()
	{
		if constexpr (requires { TNode::GetNumOutputPins(); })
		{
			return (int32)TNode::GetNumOutputPins();
		}
		else
		{
			return (int32)TNode::EOutputPin::Num;
		}
	}

public:
	static constexpr int32 NumInputPins = ComputeNumInputPins();
	static constexpr int32 NumOutputPins = ComputeNumOutputPins();
};

template<typename TNode, auto PinEnumValue>
struct TGIAG_AnimInputPinRef
{
	static_assert(std::is_enum_v<std::decay_t<decltype(PinEnumValue)>>, "GIAG: PinEnumValue must be an enum value.");

	using TPinEnum = std::decay_t<decltype(PinEnumValue)>;
	static constexpr int32 PinIndex = (int32)PinEnumValue;

	static_assert(PinIndex >= 0, "GIAG: pin index must be non-negative.");
	static_assert(std::is_same_v<TPinEnum, typename TNode::EInputPin>, "GIAG: pin enum type must be TNode::EInputPin.");
	static_assert(PinIndex < TGIAG_AnimNodePinTraits<TNode>::NumInputPins, "GIAG: input pin index out of range for this node.");

	int32 NodeIndex = INDEX_NONE;

	FORCEINLINE FGIAG_AnimInputPinRef ToRaw() const
	{
		return FGIAG_AnimInputPinRef::Make(NodeIndex, PinIndex);
	}
};

template<typename TNode, auto PinEnumValue>
struct TGIAG_AnimOutputPinRef
{
	static_assert(std::is_enum_v<std::decay_t<decltype(PinEnumValue)>>, "GIAG: PinEnumValue must be an enum value.");

	using TPinEnum = std::decay_t<decltype(PinEnumValue)>;
	static constexpr int32 PinIndex = (int32)PinEnumValue;

	static_assert(PinIndex >= 0, "GIAG: pin index must be non-negative.");
	static_assert(std::is_same_v<TPinEnum, typename TNode::EOutputPin>, "GIAG: pin enum type must be TNode::EOutputPin.");
	static_assert(PinIndex < TGIAG_AnimNodePinTraits<TNode>::NumOutputPins, "GIAG: output pin index out of range for this node.");

	int32 NodeIndex = INDEX_NONE;

	FORCEINLINE FGIAG_AnimOutputPinRef ToRaw() const
	{
		return FGIAG_AnimOutputPinRef::Make(NodeIndex, PinIndex);
	}
};

struct FGIAG_AnimNodeCompileRef
{
	int32 NodeIndex = INDEX_NONE;
};

template<typename T>
struct TGIAG_AnimNodeCompileRef : FGIAG_AnimNodeCompileRef
{
	using TNode = T;

	template<auto PinEnumValue>
	FORCEINLINE TGIAG_AnimInputPinRef<TNode, PinEnumValue> In() const
	{
		return { NodeIndex };
	}

	template<auto PinEnumValue>
	FORCEINLINE TGIAG_AnimOutputPinRef<TNode, PinEnumValue> Out() const
	{
		return { NodeIndex };
	}
};

struct FGIAG_AnimLink
{
	FGIAG_AnimOutputPinRef FromOutput;
	FGIAG_AnimInputPinRef ToInput;
};

class UGIAG_AnimGraph;

USTRUCT(BlueprintType, BlueprintInternalUseOnly)
struct GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimGraphBuilder
{
	GENERATED_BODY()

	friend UGIAG_AnimGraph;
public:
	template<typename T>
	requires (!requires { typename T::FSettings; })
	TGIAG_AnimNodeCompileRef<T> AddNode(const T& Node)
	{
		return { AddNode(T::StaticStruct(), &Node, nullptr, nullptr) };
	}

	template<typename T>
	requires requires { typename T::FSettings; }
	TGIAG_AnimNodeCompileRef<T> AddNode(const T& Node, const typename T::FSettings& Settings)
	{
		return { AddNode(T::StaticStruct(), &Node,  T::FSettings::StaticStruct(), &Settings) };
	}

	template<typename TFromNode, auto FromPin, typename TToNode, auto ToPin>
	FORCEINLINE void Link(TGIAG_AnimOutputPinRef<TFromNode, FromPin> FromOutput, TGIAG_AnimInputPinRef<TToNode, ToPin> ToInput)
	{
		Link(FromOutput.ToRaw(), ToInput.ToRaw());
	}

	template<typename TNode, auto OutPin>
	FORCEINLINE void SetFinalPose(TGIAG_AnimOutputPinRef<TNode, OutPin> FinalPoseOutput)
	{
		SetFinalPose(FinalPoseOutput.ToRaw());
	}
protected:
	void Link(const FGIAG_AnimOutputPinRef& FromOutput, const FGIAG_AnimInputPinRef& ToInput);
	void SetFinalPose(const FGIAG_AnimOutputPinRef& FinalPoseOutput);

	int32 AddNode(const UScriptStruct* NodeStruct, const void* NodePtr, const UScriptStruct* SettingsStruct, const void* SettingsPtr);

	const TArray<const IGIAG_AnimNodeMeta*>& GetNodeMetas() const { return NodeMetas; }
	const TArray<FConstStructView>& GetNodeSettings() const { return NodeSettings; }
	const TArray<int32>& GetNodeInstanceOffsets() const { return NodeInstanceOffsets; }
	const TArray<FName>& GetNodeMemberNames() const { return NodeMemberNames; }
	const TArray<FGIAG_AnimLink>& GetLinks() const { return Links; }
	const FGIAG_AnimOutputPinRef& GetFinalPose() const { return FinalPose; }

private:
	FConstStructView DefaultInstance;
	TArray<const IGIAG_AnimNodeMeta*> NodeMetas;
	TArray<FConstStructView> NodeSettings;
	TArray<int32> NodeInstanceOffsets;
	TArray<FName> NodeMemberNames;
	TArray<FGIAG_AnimLink> Links;
	FGIAG_AnimOutputPinRef FinalPose;

	bool ResolveNodeMemberByPtr(const void* NodePtr, int32& OutMemberOffset, FName& OutMemberName) const;
};

#define GIAG_PIN_IN(NodeRef, PinName) \
	(NodeRef).template In<decltype(NodeRef)::TNode::EInputPin::PinName>()
#define GIAG_PIN_OUT(NodeRef, PinName) \
	(NodeRef).template Out<decltype(NodeRef)::TNode::EOutputPin::PinName>()
