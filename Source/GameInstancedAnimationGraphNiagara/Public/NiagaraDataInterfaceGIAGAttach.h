#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceGIAGAttach.generated.h"

/**
 * Niagara DataInterface that exposes GIAG Niagara-attach TRS buffer + meta (GPU + VM).
 *
 * Usage contract:
 * - Niagara script reads transforms via `GIAG_GetAttachTransform(Index, out Pos, out Rot, out Scale)` and queries Count.
 * - For per-attach particles: script spawns from versioned AddListPacked, stores (Slot, FxGen), and kills when FxGen mismatches.
 *
 * Note:
 * - We provide both GPU HLSL and VM (CPU) implementations for spawn-driving functions (versioned AddList).
 */
UCLASS(EditInlineNew, Category="GIAG", meta=(DisplayName="GIAG Attach (GPU)"))
class GAMEINSTANCEDANIMATIONGRAPHNIAGARA_API UNiagaraDataInterfaceGIAGAttach : public UNiagaraDataInterface
{
	GENERATED_BODY()

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FShaderParameters, )
		// RDG SRV so Niagara can establish correct GPU resource transitions.
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, AttachTransform)
		SHADER_PARAMETER(uint32, AttachCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, SlotToDenseIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, SlotGeneration)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, FxParticleGenBySlot)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, AddListPacked)
		SHADER_PARAMETER(uint32, SlotTableVersion)
		SHADER_PARAMETER(uint32, AddListVersion)
		SHADER_PARAMETER(uint32, AddListCount)
	END_SHADER_PARAMETER_STRUCT()

	// UNiagaraDataInterface
	bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override;
	void PostInitProperties() override;
#if WITH_EDITOR
	void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
	bool AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const override;
	void GetParameterDefinitionHLSL(const struct FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL) override;
	bool GetFunctionHLSL(const struct FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const struct FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL) override;
#endif
	void BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const override;
	void SetShaderParameters(const struct FNiagaraDataInterfaceSetShaderParametersContext& Context) const override;
	void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc) override;

	int32 PerInstanceDataSize() const override;
	int32 PerInstanceDataPassedToRenderThreadSize() const override;
	void ProvidePerInstanceDataForRenderThread(void* DataForRenderThread, void* PerInstanceData, const FNiagaraSystemInstanceID& SystemInstance) override;
	bool InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	void DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	bool PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds) override;
private:
	static constexpr TCHAR const* FunctionName_GetAttachTransform = TEXT("GIAG_GetAttachTransform");
	static constexpr TCHAR const* FunctionName_GetAttachCount = TEXT("GIAG_GetAttachCount");
	static constexpr TCHAR const* FunctionName_GetAttachSpawnVersionAndCount = TEXT("GIAG_GetAttachSpawnVersionAndCount");
	static constexpr TCHAR const* FunctionName_GetAddListPacked = TEXT("GIAG_GetAttachAddListPacked");
	static constexpr TCHAR const* FunctionName_ResolveDenseIndex = TEXT("GIAG_ResolveAttachDenseIndex");
	static constexpr TCHAR const* FunctionName_GetFxParticleGen = TEXT("GIAG_GetAttachFxParticleGen");
};

