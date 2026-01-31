#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "GIAG_AnimGraph.h"
#include "GIAG_AnimGraphBuilder.h"
#include "GIAG_AnimNodeMetaManager.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FGIAG_AnimGraphInstanceRef(FAngelscriptBinds::EOrder::Late, []
{
	auto Binds = FAngelscriptBinds::ExistingClass("FGIAG_AnimGraphInstanceRef");
	
	Binds.ImplicitConstructor("void f(const ?&in AnimGraphInstance)", [](FGIAG_AnimGraphInstanceRef* This, const uint8* Data, const int TypeId)
	{
		const auto StructDef = Cast<UScriptStruct>(FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId));
		ensure(StructDef);
		new(This) FGIAG_AnimGraphInstanceRef(StructDef, Data);
	});
});

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_TGIAG_AnimNodeCompileRef((int32)FAngelscriptBinds::EOrder::Late - 1, []
{
	{
		FBindFlags Flags;
		Flags.bPOD = true;
		FAngelscriptBinds::ValueClass<FGIAG_AnimInputPinRef>("FGIAG_AnimInputPinRef", Flags);
		FAngelscriptBinds::ValueClass<FGIAG_AnimOutputPinRef>("FGIAG_AnimOutputPinRef", Flags);
	}

	FAngelscriptBinds::FNamespace NS{ "GIAG" };
	
	{
		FBindFlags Flags;
		Flags.bTemplate = true;
		Flags.TemplateType = "<T>";
		auto Binds = FAngelscriptBinds::ValueClass<FGIAG_AnimNodeCompileRef>("TAnimNodeRef<class T>", Flags);
	}

	for (auto Meta : FGIAG_AnimNodeMetaManager::Get().GetMetas())
	{
		auto Struct = Meta->GetStruct();

		auto Bind = FAngelscriptBinds::ExistingClass(FString::Printf(TEXT("TAnimNodeRef<%s>"), *Struct->GetStructCPPName()));

		for (int32 Idx = 0; Idx < Meta->GetNumInputPins(); ++Idx)
		{
			Bind.Method(FString::Printf(TEXT("FGIAG_AnimInputPinRef Get%s() const"), *Meta->GetInputPinName(Idx)), [](const FGIAG_AnimNodeCompileRef& NodeRef, asCScriptFunction* MetaFunc)
			{
				const int32 PinIndex = (int32)(uintptr_t)MetaFunc->userData;
				return FGIAG_AnimInputPinRef{ NodeRef.NodeIndex, PinIndex };
			}, (void*)(uintptr_t)Idx);
			FAngelscriptBinds::PreviousBindPassScriptFunctionAsFirstParam();
		}
		for (int32 Idx = 0; Idx < Meta->GetNumOutputPins(); ++Idx)
		{
			Bind.Method(FString::Printf(TEXT("FGIAG_AnimOutputPinRef Get%s() const"), *Meta->GetOutputPinName(Idx)), [](const FGIAG_AnimNodeCompileRef& NodeRef, asCScriptFunction* MetaFunc)
			{
				const int32 PinIndex = (int32)(uintptr_t)MetaFunc->userData;
				return FGIAG_AnimOutputPinRef{ NodeRef.NodeIndex, PinIndex };
			}, (void*)(uintptr_t)Idx);
			FAngelscriptBinds::PreviousBindPassScriptFunctionAsFirstParam();
		}
	}
});

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FGIAG_AnimGraphBuilder(FAngelscriptBinds::EOrder::Late, []
{
	struct FGIAG_AnimGraphBuilderBind : FGIAG_AnimGraphBuilder
	{
		using FGIAG_AnimGraphBuilder::AddNode;
		using FGIAG_AnimGraphBuilder::Link;
		using FGIAG_AnimGraphBuilder::SetFinalPose;
	};

	auto Binds = FAngelscriptBinds::ExistingClass("FGIAG_AnimGraphBuilder");

	Binds.Method("GIAG::TAnimNodeRef<FScriptStructWildcard> AddNode(const ?&in Node)", [](FGIAG_AnimGraphBuilderBind& This, const uint8* Data, const int TypeId)->int32
	{
		const auto StructDef = Cast<UScriptStruct>(FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId));
		if (StructDef == nullptr || StructDef->IsChildOf(FGIAG_AnimNodeBase::StaticStruct()) == false)
		{
			FAngelscriptManager::Throw("Not a valid AnimNode");
			return INDEX_NONE;
		}
		return This.AddNode(StructDef, Data, nullptr, nullptr);
	});
	Binds.SetPreviousBindArgumentDeterminesOutputType(0);
	
	Binds.Method("GIAG::TAnimNodeRef<FScriptStructWildcard> AddNode(const ?&in Node, const ?&in Settings)", [](FGIAG_AnimGraphBuilderBind& This, const uint8* Data, const int TypeId, const uint8* Settings, const int SettingsTypeId)->int32
	{
		const auto StructDef = Cast<UScriptStruct>(FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId));
		if (StructDef == nullptr || StructDef->IsChildOf(FGIAG_AnimNodeBase::StaticStruct()) == false)
		{
			FAngelscriptManager::Throw("Not a valid AnimNode");
			return INDEX_NONE;
		}
		const auto SettingsStructDef = Cast<UScriptStruct>(FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(SettingsTypeId));
		if (SettingsStructDef == nullptr)
		{
			FAngelscriptManager::Throw("Not a valid Settings");
			return INDEX_NONE;
		}
		return This.AddNode(StructDef, Data, SettingsStructDef, Settings);
	});
	Binds.SetPreviousBindArgumentDeterminesOutputType(0);
	
	Binds.Method("void Link(const FGIAG_AnimOutputPinRef& FromOutput, const FGIAG_AnimInputPinRef& ToInput)", [](FGIAG_AnimGraphBuilderBind& This, const FGIAG_AnimOutputPinRef& FromOutput, const FGIAG_AnimInputPinRef& ToInput)
	{
		This.Link(FromOutput, ToInput);
	});
	Binds.Method("void SetFinalPose(const FGIAG_AnimOutputPinRef& FinalPoseOutput)", [](FGIAG_AnimGraphBuilderBind& This, const FGIAG_AnimOutputPinRef& FinalPoseOutput)
	{
		This.SetFinalPose(FinalPoseOutput);
	});
});
