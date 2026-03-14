#include "AngelscriptBinds.h"
#include "GIAG_LookAtNode.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FGIAG_LookAtNode(FAngelscriptBinds::EOrder::Late, []
{
	auto Binds = FAngelscriptBinds::ExistingClass("FGIAG_LookAtNode");

	Binds.ImplicitConstructor("void f(bool bEnable)", [](FGIAG_LookAtNode* This, bool bEnable)
	{
		new(This) FGIAG_LookAtNode(bEnable);
	});
});
