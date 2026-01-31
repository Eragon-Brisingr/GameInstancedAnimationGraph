#include "GIAG_GraphCullShaderMap.h"

#include "GIAG_GraphCullConstants.h"
#include "GlobalShader.h" // GlobalBeginCompileShader
#include "ShaderCompiler.h"
#include "ShaderSerialization.h"

#if WITH_EDITOR
#include "DerivedDataCacheInterface.h"
#include "Interfaces/ITargetPlatform.h"
#include "ShaderCompilerCore.h"
#endif

IMPLEMENT_TYPE_LAYOUT(FGIAGShaderMapContent);
IMPLEMENT_TYPE_LAYOUT(GIAG::FGIAG_GraphCullShaderMapId);

bool GIAG::GIAG_SerializeShaderMap(FArchive& Ar, FGIAGShaderMap& ShaderMap, FName SerializingAsset, bool bLoadingCooked)
{
	FShaderSerializeContext Ctx(Ar);
	Ctx.SerializingAsset = SerializingAsset;
	Ctx.bLoadingCooked = bLoadingCooked;
	return ShaderMap.Serialize(Ctx);
}

namespace
{
	static FCriticalSection GGraphCullSMCS;
	static TMap<GIAG::FGIAG_GraphCullShaderMapId, FGIAGShaderMap*> GIdToShaderMap[SP_NumPlatforms];

#if WITH_EDITOR
	static FString GetGIAGGraphCullShaderMapKeyString(const GIAG::FGIAG_GraphCullShaderMapId& ShaderMapId, EShaderPlatform Platform)
		{
			// Custom version for GIAG GraphCull shader map DDC payloads.
			static const TCHAR* GIAG_DERIVEDDATA_VER = TEXT("GIAG_GRAPHCULL_SM_VER_0001");

			FName Format = LegacyShaderPlatformToShaderFormat(Platform);
			FString ShaderMapKeyString = Format.ToString() + TEXT("_") + FString::FromInt(GetTargetPlatformManagerRef().ShaderFormatVersion(Format)) + TEXT("_");
			ShaderMapAppendKeyString(Platform, ShaderMapKeyString);
			ShaderMapId.AppendKeyString(ShaderMapKeyString);
			return FDerivedDataCacheInterface::BuildCacheKey(TEXT("GIAGGCSM"), GIAG_DERIVEDDATA_VER, *ShaderMapKeyString);
		}
#endif
}

FGIAGShaderType::FGIAGShaderType(
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
		const FShaderParametersMetadata* InRootParametersMetadata
#if WITH_EDITOR
		, GetPermutationIdStringType InGetPermutationIdStringRef
#endif
	)
		: FShaderType(
			EShaderTypeForDynamicCast::NNERuntimeIREE,
			InTypeLayout,
			InName,
			InSourceFilename,
			InFunctionName,
			InFrequency,
			InTotalPermutationCount,
			InConstructSerializedRef,
			InConstructCompiledRef,
			InShouldCompilePermutationRef,
			InShouldPrecachePermutationRef,
			InGetRayTracingPayloadTypeRef,
			InGetShaderBindingLayoutTypeRef,
#if WITH_EDITOR
			InModifyCompilationEnvironmentRef,
			InValidateCompiledResultRef,
			InGetOverrideJobPriorityRef,
#endif
			InTypeSize,
			InRootParametersMetadata
#if WITH_EDITOR
			, InGetPermutationIdStringRef
#endif
)
{
}

void GIAG::FGIAG_GraphCullShaderMapId::GetScriptHash(FSHAHash& OutHash) const
	{
		FString Key;
		Key.Reserve(512);
		CompilerVersionID.AppendString(Key);
		Key.AppendChar('_');
		BaseCompileHash.AppendString(Key);
		Key.AppendChar('_');
#if WITH_EDITOR
		AppendKeyStringShaderDependencies(MakeArrayView(ShaderTypeDependencies), LayoutParams, Key);
#endif
		FSHA1 HashState;
		FTCHARToUTF8 Utf8(*Key);
		HashState.Update((const uint8*)Utf8.Get(), (uint32)Utf8.Length());
		HashState.Final();
		HashState.GetHash(OutHash.Hash);
	}

bool GIAG::FGIAG_GraphCullShaderMapId::operator==(const FGIAG_GraphCullShaderMapId& Rhs) const
	{
	if (CompilerVersionID != Rhs.CompilerVersionID
		|| BaseCompileHash != Rhs.BaseCompileHash
		|| LayoutParams != Rhs.LayoutParams
		|| ShaderTypeDependencies.Num() != Rhs.ShaderTypeDependencies.Num())
	{
		return false;
	}
	for (int32 i = 0; i < ShaderTypeDependencies.Num(); ++i)
	{
		if (ShaderTypeDependencies[i] != Rhs.ShaderTypeDependencies[i])
		{
			return false;
		}
	}
	return true;
	}

#if WITH_EDITOR
void GIAG::FGIAG_GraphCullShaderMapId::AppendKeyString(FString& KeyString) const
	{
		KeyString.AppendChar('_');
		CompilerVersionID.AppendString(KeyString);
		KeyString.AppendChar('_');
		BaseCompileHash.AppendString(KeyString);

		AppendKeyStringShaderDependencies(MakeArrayView(ShaderTypeDependencies), LayoutParams, KeyString);
	}
#endif

void GIAG::FGIAG_GraphCullShaderScript::BuildShaderMapId(EShaderPlatform Platform, const ITargetPlatform* TargetPlatform, FGIAG_GraphCullShaderMapId& OutId) const
	{
	OutId.CompilerVersionID = FGuid(0xB9E9A3A8, 0xD1A1C8B2, 0xA11BC0DE, 0x0000000D); // bump when binding/layout changes
		OutId.LayoutParams = FPlatformTypeLayoutParameters(Platform);

		// BaseCompileHash: include graph hash, generated crc, binding version.
		FSHA1 Hash;
		Hash.Update((const uint8*)&GraphHash, sizeof(GraphHash));
		Hash.Update((const uint8*)&GeneratedCrc, sizeof(GeneratedCrc));
		Hash.Update((const uint8*)&BindingVersion, sizeof(BindingVersion));
		Hash.Final();
		Hash.GetHash(OutId.BaseCompileHash.Hash);

#if WITH_EDITOR
		// Shader type dependency: only our GraphCullCS type.
		TArray<FShaderTypeDependency> ShaderDeps;
	ShaderDeps.Add(FShaderTypeDependency(&FGIAG_GraphCullCS::GetStaticType(), Platform));
		OutId.ShaderTypeDependencies = ShaderDeps;
#endif
	}

FGIAGShaderMap::FGIAGShaderMap() = default;

FGIAGShaderMap::~FGIAGShaderMap()
{
	check(bDeletedThroughDeferredCleanup);
	check(!bRegistered);
}

FGIAGShaderMap* FGIAGShaderMap::FindId(const GIAG::FGIAG_GraphCullShaderMapId& ShaderMapId, EShaderPlatform Platform)
	{
		FScopeLock Lock(&GGraphCullSMCS);
		return GIdToShaderMap[Platform].FindRef(ShaderMapId);
	}

	void FGIAGShaderMap::Register(EShaderPlatform Platform)
	{
		FScopeLock Lock(&GGraphCullSMCS);
	if (!bRegistered)
	{
		GIdToShaderMap[Platform].Add(GetContent()->ShaderMapId, this);
		bRegistered = true;
	}
	}

void FGIAGShaderMap::AddRef()
{
	FScopeLock Lock(&GGraphCullSMCS);
	check(!bDeletedThroughDeferredCleanup);
	++NumRefs;
}

void FGIAGShaderMap::Release()
{
	{
		FScopeLock Lock(&GGraphCullSMCS);
		check(NumRefs > 0);
		if (--NumRefs == 0)
		{
			if (bRegistered)
			{
				GIdToShaderMap[GetShaderPlatform()].Remove(GetContent()->ShaderMapId);
				bRegistered = false;
			}
			check(!bDeletedThroughDeferredCleanup);
			bDeletedThroughDeferredCleanup = true;
		}
	}
	if (bDeletedThroughDeferredCleanup)
	{
		BeginCleanup(this);
	}
}

#if WITH_EDITOR
void FGIAG_GraphCullCS::ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		OutEnvironment.SetDefine(TEXT("GIAG_MAX_GRAPH_NODES"), 256);
	}

	IMPLEMENT_SHADER_TYPE(, FGIAG_GraphCullCS, TEXT("/GameInstancedAnimationGraphShader/GIAG_GraphCull_CS.usf"), TEXT("Main"), SF_Compute);

void FGIAGShaderMap::LoadFromDerivedDataCache(const GIAG::FGIAG_GraphCullShaderScript* Script, const GIAG::FGIAG_GraphCullShaderMapId& ShaderMapId, EShaderPlatform Platform, TRefCountPtr<FGIAGShaderMap>& InOutShaderMap)
	{
		if (InOutShaderMap != nullptr)
		{
			return;
		}

		TArray<uint8> CachedData;
		const FString DataKey = GetGIAGGraphCullShaderMapKeyString(ShaderMapId, Platform);
		if (GetDerivedDataCacheRef().GetSynchronous(*DataKey, CachedData, Script ? *Script->FriendlyName : TEXT("GIAG_GraphCull")))
		{
			InOutShaderMap = new FGIAGShaderMap();
			FMemoryReader Ar(CachedData, true);
			FShaderSerializeContext Ctx(Ar);
			if (InOutShaderMap->Serialize(Ctx))
			{
				check(InOutShaderMap->GetShaderMapId() == ShaderMapId);
				InOutShaderMap->Register(Platform);
			}
			else
			{
				InOutShaderMap = nullptr;
			}
		}
	}

void FGIAGShaderMap::SaveToDerivedDataCache(const GIAG::FGIAG_GraphCullShaderScript* Script)
	{
		TArray<uint8> SaveData;
		FMemoryWriter Ar(SaveData, true);
		FShaderSerializeContext Ctx(Ar);
		Serialize(Ctx);
		GetDerivedDataCacheRef().Put(*GetGIAGGraphCullShaderMapKeyString(GetContent()->ShaderMapId, GetShaderPlatform()), SaveData, Script ? *Script->FriendlyName : TEXT("GIAG_GraphCull"));
	}

void FGIAGShaderMap::Compile(
		const GIAG::FGIAG_GraphCullShaderScript* Script,
		const GIAG::FGIAG_GraphCullShaderMapId& InShaderMapId,
		EShaderPlatform Platform,
		bool bSynchronousCompile)
	{
		check(IsInGameThread());

		if (FPlatformProperties::RequiresCookedData())
		{
			UE_LOG(LogShaders, Fatal, TEXT("GIAG: Trying to compile GraphCull shader at runtime, which is not supported in cooked builds. (%s)"),
				Script ? *Script->FriendlyName : TEXT("<null>"));
		}

		// NOTE: GlobalBeginCompileShader may modify the job input environment.
		// We inject virtual include contents after calling it to avoid being overwritten.
		check(Script != nullptr);
		checkf(!Script->GeneratedHlsl.IsEmpty(), TEXT("GIAG: GraphCull GeneratedHlsl is empty (GraphHash=0x%llX)."), Script->GraphHash);

		FGIAGShaderMapContent* NewContent = new FGIAGShaderMapContent(Platform);
		NewContent->ShaderMapId = InShaderMapId;
		AssignContent(NewContent);

		const FShaderType* ShaderType = &FGIAG_GraphCullCS::GetStaticType();
		const int32 PermutationId = 0;

	CompilingId = FShaderCommonCompileJob::GetNextJobId();
	FShaderCompileJob* NewJob = GShaderCompilingManager->PrepareShaderCompileJob(
		/*ShaderMapId*/ CompilingId,
		FShaderCompileJobKey(ShaderType, nullptr, PermutationId),
		EShaderCompileJobPriority::Normal);
	check(NewJob);

		// Start with an empty environment; shader type + GlobalBeginCompileShader will fill defaults.
		NewJob->Input.Environment = FShaderCompilerEnvironment();
		NewJob->Input.Target = FShaderTarget(SF_Compute, Platform);
		NewJob->Input.ShaderFormat = LegacyShaderPlatformToShaderFormat(Platform);
		NewJob->Input.VirtualSourceFilePath = TEXT("/GameInstancedAnimationGraphShader/GIAG_GraphCull_CS.usf");
		NewJob->Input.EntryPointName = TEXT("Main");

		// Let the shader type set baseline defines.
		ShaderType->ModifyCompilationEnvironment(FShaderPermutationParameters(Platform, PermutationId), NewJob->Input.Environment);

#if WITH_EDITOR
		// Our shader type is registered by a plugin module. Instead of relying on the engine's
		// referenced-UB scan timing, inject *all* uniform-buffer generated includes so core engine
		// includes (e.g. /Engine/Private/InstancedStereo.ush) can always find:
		//   /Engine/Generated/UniformBuffers/*.ush
		// This is compile-time only (DDC-cached) and avoids touching engine source.
		{
			FShaderParametersMetadata::InitializeAllUniformBufferStructs();
			TSet<const FShaderParametersMetadata*> AllUniformBuffers;
			for (TLinkedList<FShaderParametersMetadata*>::TIterator StructIt(FShaderParametersMetadata::GetStructList()); StructIt; StructIt.Next())
			{
				if (StructIt->GetUseCase() == FShaderParametersMetadata::EUseCase::UniformBuffer)
				{
					AllUniformBuffers.Add(*StructIt);
				}
			}
			UE::ShaderParameters::AddUniformBufferIncludesToEnvironment(NewJob->Input.Environment, AllUniformBuffers);
		}
#endif

		// Inject per-graph generated include (used by the physical GIAG_GraphCull_CS.usf).
		{
			// NOTE: Use the reserved /Generated/ virtual directory for in-memory shader sources.
			const FString VirtualIncludePath = TEXT("/GameInstancedAnimationGraphShader/Generated/GIAG_GraphCull_GeneratedShared.ush");
			FThreadSafeSharedAnsiStringPtr Stripped = MakeShareable(new TArray<ANSICHAR>());
			ShaderConvertAndStripComments(Script->GeneratedHlsl, *Stripped);
			check(!Stripped->IsEmpty());
			checkf(!Script->GeneratedHlsl.IsEmpty(), TEXT("GIAG: GraphCull GeneratedHlsl is empty when injecting include."));
			// Preprocessor checks IncludeVirtualPathToContentsMap first and asserts if found but empty.
			NewJob->Input.Environment.IncludeVirtualPathToContentsMap.Add(VirtualIncludePath, Script->GeneratedHlsl);
			NewJob->Input.Environment.IncludeVirtualPathToSharedContentsMap.Add(VirtualIncludePath, Stripped);
		}
		NewJob->Input.Environment.SetDefine(TEXT("GIAG_GRAPH_GEN_CRC"), Script->GeneratedCrc);
		NewJob->Input.Environment.SetDefine(TEXT("GIAG_GRAPH_CULL_BINDING_VERSION"), Script->BindingVersion);

		// NOTE: Engine ShaderPreprocessor will assert if a key exists in IncludeVirtualPathToContentsMap but value is empty.
		// We sanitize the job environment without modifying engine source.
		for (auto It = NewJob->Input.Environment.IncludeVirtualPathToContentsMap.CreateIterator(); It; ++It)
		{
			if (It.Value().IsEmpty())
			{
				// /Engine/Generated/GeneratedUniformBuffers.ush may be injected as an empty file when no UBs are referenced.
				// It is still included by /Engine/Private/Common.ush, so keep it as a non-empty stub.
				if (It.Key().EndsWith(TEXT("/Engine/Generated/GeneratedUniformBuffers.ush")))
				{
					It.Value() = TEXT("\n");
				}
				else
				{
					It.RemoveCurrent();
				}
			}
		}

	::GlobalBeginCompileShader(
		Script->FriendlyName,
		nullptr,
		ShaderType,
		nullptr,
		PermutationId,
		*NewJob->Input.VirtualSourceFilePath,
		*NewJob->Input.EntryPointName,
		NewJob->Input.Target,
		NewJob->Input,
		/*bAllowDevelopmentShaderCompile=*/true);

		// GlobalBeginCompileShader may add/modify in-memory sources. Sanitize again (engine will assert on empty strings).
		for (auto It = NewJob->Input.Environment.IncludeVirtualPathToContentsMap.CreateIterator(); It; ++It)
		{
			if (It.Value().IsEmpty())
			{
				if (It.Key().EndsWith(TEXT("/Engine/Generated/GeneratedUniformBuffers.ush")))
				{
					It.Value() = TEXT("\n");
				}
				else
				{
					It.RemoveCurrent();
				}
			}
		}

	TArray<TRefCountPtr<FShaderCommonCompileJob>> Jobs;
	Jobs.Add(NewJob);
	GShaderCompilingManager->SubmitJobs(Jobs, FString(), FString());

	if (bSynchronousCompile)
	{
		TArray<int32> ShaderMapIds;
		ShaderMapIds.Add((int32)CompilingId);
		GShaderCompilingManager->FinishCompilation(*Script->FriendlyName, ShaderMapIds);
	}

	const bool bOk = ProcessCompilationResults(Script, Jobs);
	bCompileSucceeded = bOk;
	if (bOk)
	{
		FinalizeContent();
		SaveToDerivedDataCache(Script);
	}
	else
	{
		// Leave content unfinalized; caller should treat shader map as invalid.
		CompilingId = 0;
	}
	}
#else
	IMPLEMENT_SHADER_TYPE(, FGIAG_GraphCullCS, TEXT("/GameInstancedAnimationGraphShader/GIAG_GraphCull_CS.usf"), TEXT("Main"), SF_Compute);
#endif

	bool FGIAGShaderMap::ProcessCompilationResults(const GIAG::FGIAG_GraphCullShaderScript* Script, const TArray<TRefCountPtr<FShaderCommonCompileJob>>& Results)
	{
#if WITH_EDITOR
		check(Script != nullptr);
		check(Results.Num() == 1);
		auto Job = Results[0]->GetSingleShaderJob();
		check(Job);
		check(Job->bReleased);
		Job->bSucceeded = Job->Output.bSucceeded;
		if (!Job->bSucceeded)
		{
			UE_LOG(LogShaders, Error, TEXT("GIAG: GraphCull shader compile FAILED (%s) Source=%s Entry=%s"),
				Script ? *Script->FriendlyName : TEXT("<null>"),
				*Job->Input.VirtualSourceFilePath,
				*Job->Input.EntryPointName);

			for (const FShaderCompilerError& E : Job->Output.Errors)
			{
				UE_LOG(LogShaders, Error, TEXT("GIAG GraphCull ShaderError: %s"), *E.GetErrorStringWithLineMarker());
			}

			return false;
		}

		FSHAHash ShaderMapHash;
		GetContent()->ShaderMapId.GetScriptHash(ShaderMapHash);

		GetResourceCode()->AddShaderCompilerOutput(Job->Output, Job->Key, Job->Input.GenerateDebugInfo());

		FShader* Shader = Job->Key.ShaderType->ConstructCompiled(
			FShaderCompiledShaderInitializerType(
				Job->Key.ShaderType,
				static_cast<const FShaderType::FParameters*>(Job->ShaderParameters.Get()),
				Job->Key.PermutationId,
				Job->Output,
				ShaderMapHash,
				nullptr,
				nullptr));
		check(Shader && Shader->GetCodeSize() > 0);
		check(!GetContent()->HasShader(Job->Key.ShaderType, Job->Key.PermutationId));
		GetMutableContent()->FindOrAddShader(Job->Key.ShaderType->GetHashedName(), Job->Key.PermutationId, Shader);

		// Freeze SRV base indices for generated cull params (decl order).
		// NOTE: Binding model no longer relies on this. Keep for optional diagnostics only.
		TArray<uint16> BaseIndices;
		BaseIndices.Reserve(Script->CullParamSymbols.Num());
		const TMap<FString, FParameterAllocation>& ParamMap = Job->Output.ParameterMap.GetParameterMap();
		auto FindBySuffix = [&ParamMap](const FString& Symbol) -> const FParameterAllocation*
		{
			// Prefer exact match.
			if (const FParameterAllocation* Found = ParamMap.Find(Symbol))
			{
				return Found;
			}
			// Common case: compiler / parameter parser prefixes global names, e.g. "SomeUB.Symbol" or similar.
			// We accept a single unambiguous suffix match.
			const FParameterAllocation* SuffixMatch = nullptr;
			int32 Matches = 0;
			const FString DotSuffix = TEXT(".") + Symbol;
			for (const TPair<FString, FParameterAllocation>& It : ParamMap)
			{
				if (It.Key.EndsWith(DotSuffix) || It.Key.EndsWith(Symbol))
				{
					SuffixMatch = &It.Value;
					Matches++;
					if (Matches > 1)
					{
						return nullptr; // ambiguous
					}
				}
			}
			return (Matches == 1) ? SuffixMatch : nullptr;
		};

		for (const FString& Symbol : Script->CullParamSymbols)
		{
			FShaderResourceParameter Param;
			Param.Bind(Job->Output.ParameterMap, *Symbol, SPF_Optional);
			if (Param.IsBound())
			{
				BaseIndices.Add((uint16)Param.GetBaseIndex());
				continue;
			}

			// Fallback: try resolve by suffix match in ParameterMap keys (handles prefixed globals).
			const FParameterAllocation* Alloc = FindBySuffix(Symbol);
			BaseIndices.Add(Alloc ? Alloc->BaseIndex : 0xFFFFu);
		}
		GetMutableContent()->GraphCullParamSRVBaseIndices = BaseIndices;

		return true;
#else
		return false;
#endif
	}

TRefCountPtr<FGIAGShaderMap> GIAG::GIAG_GetOrCreateGraphCullShaderMap(
		const FGIAG_GraphCullShaderScript& Script,
		EShaderPlatform Platform,
		const ITargetPlatform* TargetPlatform,
		bool bAllowCompile)
	{
		FGIAG_GraphCullShaderMapId ShaderMapId;
		Script.BuildShaderMapId(Platform, TargetPlatform, ShaderMapId);

		if (FGIAGShaderMap* Found = FGIAGShaderMap::FindId(ShaderMapId, Platform))
		{
			return TRefCountPtr<FGIAGShaderMap>(Found);
		}

		TRefCountPtr<FGIAGShaderMap> ShaderMap;

#if WITH_EDITOR
		FGIAGShaderMap::LoadFromDerivedDataCache(&Script, ShaderMapId, Platform, ShaderMap);
		if (ShaderMap == nullptr && bAllowCompile)
		{
			ShaderMap = new FGIAGShaderMap();
			ShaderMap->Compile(&Script, ShaderMapId, Platform, /*bSynchronousCompile=*/true);
			// Only register if compilation actually succeeded.
			if (ShaderMap->DidCompileSucceed())
			{
				ShaderMap->Register(Platform);
			}
			else
			{
				ShaderMap = nullptr;
			}
		}
#endif
		return ShaderMap;
	}


