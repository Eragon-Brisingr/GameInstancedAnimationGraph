#pragma once

#include "CoreMinimal.h"

#include "Shader.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "Serialization/MemoryImage.h"

class FShaderCommonCompileJob;

/**
 * GIAG per-AnimGraph GraphCull shader map (Niagara-style).
 *
 * Key points:
 * - NOT a global shader permutation.
 * - Compiled/cached per graph hash + generated HLSL CRC.
 * - In cooked builds, runtime compilation is forbidden (must be cooked).
 */
namespace GIAG
{
	/** Persistent identifier for a per-graph GraphCull shader map (DDC + ShaderCodeLibrary). */
	class FGIAG_GraphCullShaderMapId
	{
		DECLARE_TYPE_LAYOUT(FGIAG_GraphCullShaderMapId, NonVirtual);
	public:
		LAYOUT_FIELD(FGuid, CompilerVersionID);
		LAYOUT_FIELD(FSHAHash, BaseCompileHash); // derived from graph hash + gen crc + binding version
		LAYOUT_FIELD(FPlatformTypeLayoutParameters, LayoutParams);
		LAYOUT_FIELD(TMemoryImageArray<FShaderTypeDependency>, ShaderTypeDependencies);

		FGIAG_GraphCullShaderMapId()
			: CompilerVersionID()
		{}

		friend uint32 GetTypeHash(const FGIAG_GraphCullShaderMapId& Ref)
		{
			return GetTypeHash(Ref.BaseCompileHash);
		}

		void GetScriptHash(FSHAHash& OutHash) const;
		bool operator==(const FGIAG_GraphCullShaderMapId& Rhs) const;

#if WITH_EDITOR
		void AppendKeyString(FString& KeyString) const;
#endif
	};

	/** Compile-time description for one graph's cull shader generation. Editor/cook only. */
	struct FGIAG_GraphCullShaderScript
	{
		uint64 GraphHash = 0;
		FString FriendlyName;
		FString GeneratedHlsl;
		TArray<FString> CullParamSymbols;
		uint32 GeneratedCrc = 0;
		uint32 BindingVersion = 0;

		void BuildShaderMapId(EShaderPlatform Platform, const ITargetPlatform* TargetPlatform, FGIAG_GraphCullShaderMapId& OutId) const;
	};
}

// -----------------------------------------------------------------------------
// GIAG shader meta types (must be in global namespace for DECLARE_SHADER_TYPE(..., GIAG))
// -----------------------------------------------------------------------------

/** ShaderMap content for GIAG shaders. */
class FGIAGShaderMapContent : public FShaderMapContent
{
	using Super = FShaderMapContent;
	friend class FGIAGShaderMap;
	DECLARE_TYPE_LAYOUT(FGIAGShaderMapContent, NonVirtual);
private:
	explicit FGIAGShaderMapContent(EShaderPlatform InPlatform)
		: Super(InPlatform)
	{}

	LAYOUT_FIELD(GIAG::FGIAG_GraphCullShaderMapId, ShaderMapId);
	// SRV base indices for graph-generated cull params, in decl order (matches Script.CullParamSymbols / graph compiled data).
	LAYOUT_FIELD(TMemoryImageArray<uint16>, GraphCullParamSRVBaseIndices);
};

/** GIAG shader meta type (non-global shader type). */
class FGIAGShaderType : public FShaderType
{
public:
	using FParameters = FShaderType::FParameters;
	using CompiledShaderInitializerType = FShaderCompiledShaderInitializerType;

	FGIAGShaderType(
		FTypeLayoutDesc& InTypeLayout,
		const TCHAR* InName,
		const TCHAR* InSourceFilename,
		const TCHAR* InFunctionName,
		uint32 InFrequency,
		int32 InTotalPermutationCount,
		ConstructSerializedType InConstructSerializedRef,
		ConstructCompiledType InConstructCompiledRef,
		ShouldCompilePermutationType InShouldCompilePermutationRef,
		ShouldPrecachePermutationType InShouldPrecachePermutationRef,
		GetRayTracingPayloadTypeType InGetRayTracingPayloadTypeRef,
		GetShaderBindingLayoutType InGetShaderBindingLayoutTypeRef,
#if WITH_EDITOR
		ModifyCompilationEnvironmentType InModifyCompilationEnvironmentRef,
		ValidateCompiledResultType InValidateCompiledResultRef,
		GetOverrideJobPriorityType InGetOverrideJobPriorityRef,
#endif
		uint32 InTypeSize,
		const FShaderParametersMetadata* InRootParametersMetadata = nullptr
#if WITH_EDITOR
		, GetPermutationIdStringType InGetPermutationIdStringRef = nullptr
#endif
	);
};

/** GIAG shader map (per-graph). */
class GAMEINSTANCEDANIMATIONGRAPHSHADER_API FGIAGShaderMap : public TShaderMap<FGIAGShaderMapContent, FShaderMapPointerTable>, public FDeferredCleanupInterface
{
public:
	using Super = TShaderMap<FGIAGShaderMapContent, FShaderMapPointerTable>;

	static FGIAGShaderMap* FindId(const GIAG::FGIAG_GraphCullShaderMapId& ShaderMapId, EShaderPlatform Platform);

	// ShaderMap interface expected by TShaderMapRef / engine diagnostics.
	template<typename ShaderType>
	TShaderRef<ShaderType> GetShader(int32 PermutationId) const { return TShaderRef<ShaderType>(GetContent()->GetShader<ShaderType>(PermutationId), *this); }
	TShaderRef<FShader> GetShader(FShaderType* ShaderType, int32 PermutationId) const { return TShaderRef<FShader>(GetContent()->GetShader(ShaderType, PermutationId), *this); }

#if WITH_EDITOR
	static void LoadFromDerivedDataCache(const GIAG::FGIAG_GraphCullShaderScript* Script, const GIAG::FGIAG_GraphCullShaderMapId& ShaderMapId, EShaderPlatform Platform, TRefCountPtr<FGIAGShaderMap>& InOutShaderMap);
	void SaveToDerivedDataCache(const GIAG::FGIAG_GraphCullShaderScript* Script);
	void Compile(
		const GIAG::FGIAG_GraphCullShaderScript* Script,
		const GIAG::FGIAG_GraphCullShaderMapId& InShaderMapId,
		EShaderPlatform Platform,
		bool bSynchronousCompile);
#endif

	FGIAGShaderMap();
	virtual ~FGIAGShaderMap();

	void Register(EShaderPlatform Platform);
	const GIAG::FGIAG_GraphCullShaderMapId& GetShaderMapId() const { return GetContent()->ShaderMapId; }
	EShaderPlatform GetShaderPlatform() const { return GetContent()->GetShaderPlatform(); }
	TConstArrayView<uint16> GetGraphCullParamSRVBaseIndices() const { return GetContent()->GraphCullParamSRVBaseIndices; }
	bool DidCompileSucceed() const { return bCompileSucceeded; }

	// Reference counting for TRefCountPtr (mirrors NiagaraShaderMap).
	void AddRef();
	void Release();

	virtual void GetShaderList(TMap<FHashedName, TShaderRef<FShader>>& OutShaders) const override { GetContent()->GetShaderList(*this, OutShaders); }
	virtual void GetShaderPipelineList(TArray<FShaderPipelineRef>& OutShaderPipelines) const override { GetContent()->GetShaderPipelineList(*this, OutShaderPipelines, FShaderPipeline::EAll); }

private:
	bool ProcessCompilationResults(const GIAG::FGIAG_GraphCullShaderScript* Script, const TArray<TRefCountPtr<FShaderCommonCompileJob>>& Results);

private:
	uint32 CompilingId = 1;
	int32 NumRefs = 0;
	bool bDeletedThroughDeferredCleanup = false;
	bool bRegistered = false;
	bool bCompileSucceeded = false;
};

/**
 * GraphCull compute shader (non-global shader) compiled into a per-graph shader map.
 *
 * Note: generated per-graph parameter buffers are declared in the injected include, and are bound
 * by SRV BaseIndex (resolved during cook/compile, not via FShaderResourceParameter::Bind).
 */
class FGIAG_GraphCullCS : public FShader
{
	DECLARE_SHADER_TYPE(FGIAG_GraphCullCS, GIAG);
public:
	FGIAG_GraphCullCS() = default;

	FGIAG_GraphCullCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FShader(Initializer)
	{
		NumNodesParam.Bind(Initializer.ParameterMap, TEXT("NumNodes"));
		NumInstancesParam.Bind(Initializer.ParameterMap, TEXT("NumInstances"));
		SlotCapacityParam.Bind(Initializer.ParameterMap, TEXT("SlotCapacity"));
		WordsPerSlotParam.Bind(Initializer.ParameterMap, TEXT("WordsPerSlot"));
		FinalNodeIndexParam.Bind(Initializer.ParameterMap, TEXT("FinalNodeIndex"));

		ActiveInstanceIndicesParam.Bind(Initializer.ParameterMap, TEXT("ActiveInstanceIndices"));
		RW_NeedNodeBitsParam.Bind(Initializer.ParameterMap, TEXT("RW_NeedNodeBits"));
	}

	void SetParameters(
		FRHIBatchedShaderParameters& BatchedParameters,
		uint32 NumNodes,
		uint32 NumInstances,
		uint32 SlotCapacity,
		uint32 WordsPerSlot,
		uint32 FinalNodeIndex,
		FRHIShaderResourceView* ActiveInstanceIndicesSRV,
		FRHIUnorderedAccessView* RW_NeedNodeBitsUAV) const
	{
		SetShaderValue(BatchedParameters, NumNodesParam, NumNodes);
		SetShaderValue(BatchedParameters, NumInstancesParam, NumInstances);
		SetShaderValue(BatchedParameters, SlotCapacityParam, SlotCapacity);
		SetShaderValue(BatchedParameters, WordsPerSlotParam, WordsPerSlot);
		SetShaderValue(BatchedParameters, FinalNodeIndexParam, FinalNodeIndex);

		SetSRVParameter(BatchedParameters, ActiveInstanceIndicesParam, ActiveInstanceIndicesSRV);
		SetUAVParameter(BatchedParameters, RW_NeedNodeBitsParam, RW_NeedNodeBitsUAV);
	}

	static bool ShouldCompilePermutation(const FShaderPermutationParameters& Parameters) { return true; }
	static EShaderPermutationPrecacheRequest ShouldPrecachePermutation(const FShaderPermutationParameters& Parameters) { return EShaderPermutationPrecacheRequest::NotPrecached; }

#if WITH_EDITOR
	static void ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
#endif

private:
	LAYOUT_FIELD(FShaderParameter, NumNodesParam);
	LAYOUT_FIELD(FShaderParameter, NumInstancesParam);
	LAYOUT_FIELD(FShaderParameter, SlotCapacityParam);
	LAYOUT_FIELD(FShaderParameter, WordsPerSlotParam);
	LAYOUT_FIELD(FShaderParameter, FinalNodeIndexParam);

	LAYOUT_FIELD(FShaderResourceParameter, ActiveInstanceIndicesParam);
	LAYOUT_FIELD(FShaderResourceParameter, RW_NeedNodeBitsParam);
};

namespace GIAG
{
	/** Helper to serialize a GIAG shader map using FShaderSerializeContext (Niagara-style). */
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API bool GIAG_SerializeShaderMap(FArchive& Ar, FGIAGShaderMap& ShaderMap, FName SerializingAsset, bool bLoadingCooked);

	/** Build/lookup per-graph shader map (Editor/Cook: may compile; Runtime cooked: must already be cooked). */
	GAMEINSTANCEDANIMATIONGRAPHSHADER_API TRefCountPtr<FGIAGShaderMap> GIAG_GetOrCreateGraphCullShaderMap(
		const FGIAG_GraphCullShaderScript& Script,
		EShaderPlatform Platform,
		const ITargetPlatform* TargetPlatform,
		bool bAllowCompile);
}

