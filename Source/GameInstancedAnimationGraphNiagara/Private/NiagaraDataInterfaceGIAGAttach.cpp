#include "NiagaraDataInterfaceGIAGAttach.h"
#include "GIAG_AttachRegistry.h"
#include "GameInstancedAnimationGraphSubsystem.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraTypes.h"
#include "RenderGraphUtils.h"
#include "VectorVM.h"
#include "Engine/World.h"

namespace
{
	struct FGIAGAttachInstanceData
	{
		uint32 BucketId = 0;
		FGIAG_NiagaraAttachRegistry* RegistryPtr = nullptr;
		UGameInstancedAnimationGraphSubsystem* Subsys = nullptr; // GT only (for VM lookups)
	};

	// NOTE: Must be 16-byte aligned for Niagara GT->RT transfer.
	struct alignas(16) FGIAGAttachDataForRT
	{
		uint32 BucketId = 0;
		uint32 Pad0 = 0;
		uint64 RegistryPtr = 0;
	};

	struct FNiagaraDataInterfaceProxyGIAGAttach : public FNiagaraDataInterfaceProxy
	{
		TMap<FNiagaraSystemInstanceID, FGIAGAttachInstanceData> PerInstanceData_RT;

		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return sizeof(FGIAGAttachDataForRT); }

		virtual void ConsumePerInstanceDataFromGameThread(void* PerInstanceData, const FNiagaraSystemInstanceID& InstanceID) override
		{
			const FGIAGAttachDataForRT* In = static_cast<const FGIAGAttachDataForRT*>(PerInstanceData);
			PerInstanceData_RT.FindOrAdd(InstanceID) = { In->BucketId, reinterpret_cast<FGIAG_NiagaraAttachRegistry*>((uintptr_t)In->RegistryPtr) };
			const_cast<FGIAGAttachDataForRT*>(In)->~FGIAGAttachDataForRT();
		}
	};

	static FGIAG_NiagaraAttachRegistry* ResolveRegistryPtrOrNull(FNiagaraSystemInstance* SystemInstance)
	{
		if (!SystemInstance)
		{
			return nullptr;
		}
		UWorld* World = SystemInstance->GetWorld();
		if (!World)
		{
			return nullptr;
		}
		UGameInstancedAnimationGraphSubsystem* Subsys = World->GetSubsystem<UGameInstancedAnimationGraphSubsystem>();
		if (!Subsys)
		{
			return nullptr;
		}
		return Subsys->GetNiagaraAttachRegistry().Get();
	}

	static UGameInstancedAnimationGraphSubsystem* ResolveSubsysOrNull(FNiagaraSystemInstance* SystemInstance)
	{
		if (!SystemInstance)
		{
			return nullptr;
		}
		UWorld* World = SystemInstance->GetWorld();
		return World ? World->GetSubsystem<UGameInstancedAnimationGraphSubsystem>() : nullptr;
	}

	// Minimal helper to read an int parameter from a Niagara system instance parameter store (VM-safe).
	static int32 ReadIntParamOrZero(FNiagaraSystemInstance* SystemInstance, const FName ParamName)
	{
		if (!SystemInstance)
		{
			return 0;
		}

		const FNiagaraParameterStore& Store = SystemInstance->GetInstanceParameters();
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), ParamName);
		int32 Value = 0;
		Store.GetParameterValue(Value, Var);
		return Value;
	}
}

void UNiagaraDataInterfaceGIAGAttach::PostInitProperties()
{
	Super::PostInitProperties();
	if (!Proxy.IsValid())
	{
		Proxy = MakeUnique<FNiagaraDataInterfaceProxyGIAGAttach>();
	}
}

bool UNiagaraDataInterfaceGIAGAttach::CanExecuteOnTarget(ENiagaraSimTarget Target) const
{
	return Target == ENiagaraSimTarget::GPUComputeSim || Target == ENiagaraSimTarget::CPUSim;
}

#if WITH_EDITOR
void UNiagaraDataInterfaceGIAGAttach::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = FunctionName_GetAttachTransform;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("GIAG Attach (GPU)")));
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("OutPos")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetQuatDef(), TEXT("OutRot")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("OutScale")));
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = FunctionName_GetAttachCount;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("GIAG Attach (GPU)")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("OutCount")));
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = FunctionName_GetAttachSpawnVersionAndCount;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = true;
		Sig.bSupportsGPU = false;
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("GIAG Attach (GPU)")));
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("PrevVersion")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("OutCount")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("OutVersion")));
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = FunctionName_GetAddListPacked;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("GIAG Attach (GPU)")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("OutPacked")));
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = FunctionName_ResolveDenseIndex;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("GIAG Attach (GPU)")));
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Slot")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("OutDenseIndex")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("bValid")));
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = FunctionName_GetFxParticleGen;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("GIAG Attach (GPU)")));
		Sig.AddInput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Slot")));
		Sig.AddOutput(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("OutFxGen")));
		OutFunctions.Add(Sig);
	}
}

bool UNiagaraDataInterfaceGIAGAttach::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	bool bSuccess = Super::AppendCompileHash(InVisitor);
	bSuccess &= InVisitor->UpdateShaderParameters<FShaderParameters>();
	return bSuccess;
}

void UNiagaraDataInterfaceGIAGAttach::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	const FString ParamPrefix = ParamInfo.DataInterfaceHLSLSymbol;
	OutHLSL += TEXT("#include \"/GameInstancedAnimationGraphShader/GIAG_AnimCommon.ush\"\n");
	OutHLSL += FString::Printf(TEXT("StructuredBuffer<FGIAG_Transform> %s_AttachTransform;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("#define %s_AttachTRS %s_AttachTransform\n"), *ParamPrefix, *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("uint %s_AttachCount;\n"), *ParamPrefix);

	OutHLSL += FString::Printf(TEXT("StructuredBuffer<int> %s_SlotToDenseIndex;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("StructuredBuffer<int> %s_SlotGeneration;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("StructuredBuffer<int> %s_FxParticleGenBySlot;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("StructuredBuffer<int> %s_AddListPacked;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("uint %s_SlotTableVersion;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("uint %s_AddListVersion;\n"), *ParamPrefix);
	OutHLSL += FString::Printf(TEXT("uint %s_AddListCount;\n"), *ParamPrefix);
}

bool UNiagaraDataInterfaceGIAGAttach::GetFunctionHLSL(
	const FNiagaraDataInterfaceGPUParamInfo& ParamInfo,
	const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo,
	int /*FunctionInstanceIndex*/,
	FString& OutHLSL)
{
	TMap<FString, FStringFormatArg> FormatArgs;
	FormatArgs.Add(TEXT("FunctionName"), FunctionInfo.InstanceName);
	FormatArgs.Add(TEXT("AttachTransformName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_AttachTransform"));
	FormatArgs.Add(TEXT("AttachCountName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_AttachCount"));
	FormatArgs.Add(TEXT("SlotToDenseName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_SlotToDenseIndex"));
	FormatArgs.Add(TEXT("FxGenName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_FxParticleGenBySlot"));
	FormatArgs.Add(TEXT("AddListPackedName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_AddListPacked"));
	FormatArgs.Add(TEXT("AddListVersionName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_AddListVersion"));
	FormatArgs.Add(TEXT("AddListCountName"), ParamInfo.DataInterfaceHLSLSymbol + TEXT("_AddListCount"));

	if (FunctionInfo.DefinitionName == FName(FunctionName_GetAttachTransform))
	{
		static const TCHAR* FunctionFormat = TEXT(R"(
void {FunctionName}(int Index, out float3 OutPos, out float4 OutRot, out float3 OutScale)
{
	// Always initialize outputs (avoids undefined values / NaNs when Index is out of range).
	OutPos = float3(0, 0, 0);
	OutRot = float4(0, 0, 0, 1);
	OutScale = float3(0, 0, 0);

	if (Index < 0 || Index >= (int){AttachCountName})
	{
		return;
	}

	FGIAG_Transform T = {AttachTransformName}[Index];
	OutRot = T.Rotation;
	OutPos = T.Translation;
	OutScale = T.Scale3D;
}
)");
		OutHLSL += FString::Format(FunctionFormat, FormatArgs);

		return true;
	}

	if (FunctionInfo.DefinitionName == FName(FunctionName_GetAttachCount))
	{
		static const TCHAR* FunctionFormat = TEXT(R"(
void {FunctionName}(out int OutCount)
{
	OutCount = (int){AttachCountName};
}
)");
		OutHLSL += FString::Format(FunctionFormat, FormatArgs);
		return true;
	}

	if (FunctionInfo.DefinitionName == FName(FunctionName_GetAddListPacked))
	{
		static const TCHAR* FunctionFormat = TEXT(R"(
void {FunctionName}(int Index, out int OutPacked)
{
	OutPacked = 0;
	if (Index < 0 || Index >= (int){AddListCountName})
	{
		return;
	}
	OutPacked = {AddListPackedName}[Index];
}
)");
		OutHLSL += FString::Format(FunctionFormat, FormatArgs);
		return true;
	}

	if (FunctionInfo.DefinitionName == FName(FunctionName_ResolveDenseIndex))
	{
		static const TCHAR* FunctionFormat = TEXT(R"(
void {FunctionName}(int Slot, out int OutDenseIndex, out bool bValid)
{
	OutDenseIndex = -1;
	bValid = false;
	if (Slot < 0)
	{
		return;
	}
	OutDenseIndex = {SlotToDenseName}[Slot];
	bValid = (OutDenseIndex >= 0);
}
)");
		OutHLSL += FString::Format(FunctionFormat, FormatArgs);
		return true;
	}

	if (FunctionInfo.DefinitionName == FName(FunctionName_GetFxParticleGen))
	{
		static const TCHAR* FunctionFormat = TEXT(R"(
void {FunctionName}(int Slot, out int OutFxGen)
{
	OutFxGen = 0;
	if (Slot < 0)
	{
		return;
	}
	OutFxGen = {FxGenName}[Slot];
}
)");
		OutHLSL += FString::Format(FunctionFormat, FormatArgs);
		return true;
	}

	return false;
}
#endif // WITH_EDITORONLY_DATA

void UNiagaraDataInterfaceGIAGAttach::BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const
{
	ShaderParametersBuilder.AddNestedStruct<FShaderParameters>();
}

int32 UNiagaraDataInterfaceGIAGAttach::PerInstanceDataPassedToRenderThreadSize() const
{
	// 16-byte aligned by design.
	return sizeof(FGIAGAttachDataForRT);
}

void UNiagaraDataInterfaceGIAGAttach::ProvidePerInstanceDataForRenderThread(void* DataForRenderThread, void* PerInstanceData, const FNiagaraSystemInstanceID& /*SystemInstance*/)
{
	FGIAGAttachDataForRT* Out = new (DataForRenderThread) FGIAGAttachDataForRT();
	const FGIAGAttachInstanceData* In = reinterpret_cast<const FGIAGAttachInstanceData*>(PerInstanceData);
	Out->BucketId = In ? In->BucketId : 0u;
	Out->RegistryPtr = In ? (uint64)(uintptr_t)In->RegistryPtr : 0ull;
}

void UNiagaraDataInterfaceGIAGAttach::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	check(IsInRenderingThread());
	FShaderParameters* Params = Context.GetParameterNestedStruct<FShaderParameters>();
	check(Params);

	const FNiagaraDataInterfaceProxyGIAGAttach& ProxyRef = Context.GetProxy<FNiagaraDataInterfaceProxyGIAGAttach>();
	const FNiagaraSystemInstanceID SystemId = Context.GetSystemInstanceID();
	const FGIAGAttachInstanceData* InstData = ProxyRef.PerInstanceData_RT.Find(SystemId);
	const uint32 BucketId = InstData ? InstData->BucketId : 0u;
	FGIAG_NiagaraAttachRegistry* RegistryPtr = InstData ? InstData->RegistryPtr : nullptr;

	FRDGBufferSRVRef AttachTransform_SRV = nullptr;
	uint32 AttachCount = 0u;
	FRDGBufferSRVRef SlotToDense_SRV = nullptr;
	FRDGBufferSRVRef SlotGen_SRV = nullptr;
	FRDGBufferSRVRef FxGen_SRV = nullptr;
	FRDGBufferSRVRef AddListPacked_SRV = nullptr;
	uint32 SlotTableVersion = 0u;
	uint32 AddListVersion = 0u;
	uint32 AddListCount = 0u;

	FRDGBuilder& GraphBuilder = Context.GetGraphBuilder();
	if (BucketId != 0u && RegistryPtr != nullptr)
	{
		if (const FGIAG_NiagaraAttachRegistry::FEntry* Entry = RegistryPtr->Find(BucketId))
		{
			AttachCount = Entry->NumInstances;
			SlotTableVersion = Entry->SlotTableVersion;
			AddListVersion = Entry->AddListVersion;
			AddListCount = Entry->AddListCount;

			// Register the external buffers into Niagara's RDG so the graph can schedule transitions correctly.
			if (Entry->FxTransformBuffer.IsValid())
			{
				FRDGBufferRef RDG = GraphBuilder.RegisterExternalBuffer(Entry->FxTransformBuffer, TEXT("GIAG_Attach_FxTransform_External_Niagara"));
				AttachTransform_SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RDG));
			}
			if (Entry->SlotToDenseIndexBuffer.IsValid())
			{
				FRDGBufferRef RDG = GraphBuilder.RegisterExternalBuffer(Entry->SlotToDenseIndexBuffer, TEXT("GIAG_Attach_SlotToDense_External_Niagara"));
				SlotToDense_SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RDG));
			}
			if (Entry->SlotGenerationBuffer.IsValid())
			{
				FRDGBufferRef RDG = GraphBuilder.RegisterExternalBuffer(Entry->SlotGenerationBuffer, TEXT("GIAG_Attach_SlotGen_External_Niagara"));
				SlotGen_SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RDG));
			}
			if (Entry->FxParticleGenBuffer.IsValid())
			{
				FRDGBufferRef RDG = GraphBuilder.RegisterExternalBuffer(Entry->FxParticleGenBuffer, TEXT("GIAG_Attach_FxGen_External_Niagara"));
				FxGen_SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RDG));
			}
			if (Entry->AddListPackedBuffer.IsValid())
			{
				FRDGBufferRef RDG = GraphBuilder.RegisterExternalBuffer(Entry->AddListPackedBuffer, TEXT("GIAG_Attach_AddListPacked_External_Niagara"));
				AddListPacked_SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RDG));
			}
		}
	}

	// Fallback: bind tiny buffers (keeps HLSL reads defined).
	if (AttachTransform_SRV == nullptr || SlotToDense_SRV == nullptr || SlotGen_SRV == nullptr || FxGen_SRV == nullptr || AddListPacked_SRV == nullptr)
	{
		FRDGBufferRef DummyTransform = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), 1), TEXT("GIAG_Attach_FxTransform_Dummy"));
		FRDGBufferUAVRef DummyTransformUAV = GraphBuilder.CreateUAV(DummyTransform);
		AddClearUAVPass(GraphBuilder, DummyTransformUAV, 0u);
		AttachTransform_SRV = AttachTransform_SRV ? AttachTransform_SRV : GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyTransform));

		FRDGBufferRef DummyInt = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1), TEXT("GIAG_Attach_Int_Dummy"));
		FRDGBufferUAVRef DummyIntUAV = GraphBuilder.CreateUAV(DummyInt);
		AddClearUAVPass(GraphBuilder, DummyIntUAV, 0u);
		FRDGBufferSRVRef DummyIntSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyInt));
		SlotToDense_SRV = SlotToDense_SRV ? SlotToDense_SRV : DummyIntSRV;
		SlotGen_SRV = SlotGen_SRV ? SlotGen_SRV : DummyIntSRV;
		FxGen_SRV = FxGen_SRV ? FxGen_SRV : DummyIntSRV;
		AddListPacked_SRV = AddListPacked_SRV ? AddListPacked_SRV : DummyIntSRV;
	}
	Params->AttachTransform = AttachTransform_SRV;
	Params->AttachCount = AttachCount;
	Params->SlotToDenseIndex = SlotToDense_SRV;
	Params->SlotGeneration = SlotGen_SRV;
	Params->FxParticleGenBySlot = FxGen_SRV;
	Params->AddListPacked = AddListPacked_SRV;
	Params->SlotTableVersion = SlotTableVersion;
	Params->AddListVersion = AddListVersion;
	Params->AddListCount = AddListCount;
}

int32 UNiagaraDataInterfaceGIAGAttach::PerInstanceDataSize() const
{
	return sizeof(FGIAGAttachInstanceData);
}

bool UNiagaraDataInterfaceGIAGAttach::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	FGIAGAttachInstanceData* Inst = static_cast<FGIAGAttachInstanceData*>(PerInstanceData);
	Inst->Subsys = ResolveSubsysOrNull(SystemInstance);
	static const FName InternalBucketIdName = TEXT("User.GIAG_InternalAttachBucketId");
	Inst->BucketId = (uint32)FMath::Max(0, ReadIntParamOrZero(SystemInstance, InternalBucketIdName));
	if (Inst->BucketId == 0u)
	{
		UNiagaraComponent* Comp = SystemInstance ? Cast<UNiagaraComponent>(SystemInstance->GetAttachComponent()) : nullptr;
		Inst->BucketId = (Inst->Subsys && Comp) ? Inst->Subsys->ResolveNiagaraAttachBucketIdOrZero(Comp) : 0u;
	}
	Inst->RegistryPtr = ResolveRegistryPtrOrNull(SystemInstance);
	return true;
}

void UNiagaraDataInterfaceGIAGAttach::DestroyPerInstanceData(void* /*PerInstanceData*/, FNiagaraSystemInstance* /*SystemInstance*/)
{
}

bool UNiagaraDataInterfaceGIAGAttach::PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float /*DeltaSeconds*/)
{
	FGIAGAttachInstanceData* Inst = static_cast<FGIAGAttachInstanceData*>(PerInstanceData);
	Inst->Subsys = ResolveSubsysOrNull(SystemInstance);
	static const FName InternalBucketIdName = TEXT("User.GIAG_InternalAttachBucketId");
	uint32 NewBucketId = (uint32)FMath::Max(0, ReadIntParamOrZero(SystemInstance, InternalBucketIdName));
	if (NewBucketId == 0u)
	{
		UNiagaraComponent* Comp = SystemInstance ? Cast<UNiagaraComponent>(SystemInstance->GetAttachComponent()) : nullptr;
		NewBucketId = (Inst->Subsys && Comp) ? Inst->Subsys->ResolveNiagaraAttachBucketIdOrZero(Comp) : 0u;
	}
	FGIAG_NiagaraAttachRegistry* NewRegistryPtr = ResolveRegistryPtrOrNull(SystemInstance);

	const bool bChanged = (Inst->BucketId != NewBucketId) || (Inst->RegistryPtr != NewRegistryPtr);
	Inst->BucketId = NewBucketId;
	Inst->RegistryPtr = NewRegistryPtr;
	return bChanged;
}

void UNiagaraDataInterfaceGIAGAttach::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc)
{
	if (BindingInfo.Name == FunctionName_GetAttachSpawnVersionAndCount)
	{
		OutFunc = FVMExternalFunction::CreateLambda([](FVectorVMExternalFunctionContext& Context)
		{
			VectorVM::FUserPtrHandler<FGIAGAttachInstanceData> Inst(Context);
			FNDIInputParam<int32> InPrevVersion(Context);
			FNDIOutputParam<int32> OutCount(Context);
			FNDIOutputParam<int32> OutVersion(Context);

			uint32 Version = 0;
			uint32 Count = 0;
			if (Inst && Inst->Subsys)
			{
				Inst->Subsys->NiagaraAttach_GetSpawnVersionAndCount(Inst->BucketId, Version, Count);
			}
			const int32 ListNum = (int32)Count;

			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				auto PrevVersion = InPrevVersion.GetAndAdvance();
				OutVersion.SetAndAdvance(Version);
				OutCount.SetAndAdvance(PrevVersion != Version ? ListNum : 0);
			}
		});
		return;
	}
}

