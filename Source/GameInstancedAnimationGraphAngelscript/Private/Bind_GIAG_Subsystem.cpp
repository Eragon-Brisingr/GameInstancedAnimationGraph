#include "AngelscriptBinds.h"
#include "GameInstancedAnimationGraphSubsystem.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_TGIAG_AnimNodePtr((int32)FAngelscriptBinds::EOrder::Late - 1, []
{
	using FGIAG_AnimNodeScriptPtr = TGIAG_AnimNodePtr<void>;
	
	FBindFlags Flags;
	Flags.bTemplate = true;
	Flags.TemplateType = "<T>";
	auto Binds = FAngelscriptBinds::ValueClass<FGIAG_AnimNodeScriptPtr>("TGIAG_AnimNodePtr<class T>", Flags);

	Binds.Method("bool opImplConv() const", [](const FGIAG_AnimNodeScriptPtr& This)->bool
	{
		return This.NodePtr != nullptr;
	});
	Binds.Method("FGIAG_AnimNodeRef& opImplConv()", [](FGIAG_AnimNodeScriptPtr& This)->FGIAG_AnimNodeRef&
	{
		return This;
	});
	Binds.Method("T& Get()", [](FGIAG_AnimNodeScriptPtr& This)->void*
	{
		return This.NodePtr;
	});
});

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_UGameInstancedAnimationGraphSubsystem(FAngelscriptBinds::EOrder::Late, []
{
	auto Binds = FAngelscriptBinds::ExistingClass("UGameInstancedAnimationGraphSubsystem");
	
	struct FGIAG_Subsystem : UGameInstancedAnimationGraphSubsystem
	{
		using UGameInstancedAnimationGraphSubsystem::FindAnimNodeImpl;
		using UGameInstancedAnimationGraphSubsystem::GetAnimNodeStruct;
	};

	Binds.TemplateMethod<const FGIAG_Subsystem*>("FindAnimNode", [](asCObjectType* Type, asCString& ErrorMessage)
	{
		const auto& TemplateType = Type->templateSubTypes[0];
		auto ScriptStruct = static_cast<UScriptStruct*>(TemplateType.GetTypeInfo()->GetUserData());
		if (ScriptStruct == nullptr || ScriptStruct->IsChildOf(FGIAG_AnimNodeBase::StaticStruct()) == false)
		{
			ErrorMessage = "T must as AnimNode";
			return false;
		}
		return true;
	})
	.Method("TGIAG_AnimNodePtr<T> opCall(const FGameInstancedAnimationGraphHandle& Handle, const FName& NodeName)", [](asCObjectType* Type, const FGIAG_Subsystem* This, const FGameInstancedAnimationGraphHandle& Handle, const FName& NodeName)->TGIAG_AnimNodePtr<void>
	{
		TGIAG_AnimNodePtr<void> NodePtr;
		const auto& TemplateType = Type->templateSubTypes[0];
		auto ScriptStruct = static_cast<UScriptStruct*>(TemplateType.GetTypeInfo()->GetUserData());
		NodePtr.NodePtr = This->FindAnimNodeImpl(Handle, NodeName, NodePtr);
		if (!ensure(NodePtr.NodePtr == nullptr || ScriptStruct == This->GetAnimNodeStruct(NodePtr)))
		{
			return TGIAG_AnimNodePtr<void>{};
		}
		return NodePtr;
	});
});

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FGameInstancedAnimationGraphHandle(FAngelscriptBinds::EOrder::Late, [] 
{
	auto Binds = FAngelscriptBinds::ExistingClass("FGameInstancedAnimationGraphHandle");
	
	Binds.Method("bool opImplConv() const", [](const FGameInstancedAnimationGraphHandle& This)
	{
		return This.IsValid();
	});
});
