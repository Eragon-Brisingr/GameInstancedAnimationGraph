#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GIAG_AnimNodeBase.h"
#include "GIAG_AnimGraphInstance.h"
#include "Engine/DataAsset.h"
#include "GIAG_AnimGraph.generated.h"

struct FGIAG_AnimGraphBuilder;
class UGIAG_AnimGraph;
class UScriptStruct;
class ITargetPlatform;
class FGIAGShaderMap;

/** Opaque reference-counted handle for a per-graph GraphCull shader map (Niagara-style). */
struct FGIAG_GraphCullShaderMapPtr
{
	FGIAG_GraphCullShaderMapPtr() = default;
	explicit FGIAG_GraphCullShaderMapPtr(FGIAGShaderMap* InPtr);
	FGIAG_GraphCullShaderMapPtr(const FGIAG_GraphCullShaderMapPtr& Other);
	FGIAG_GraphCullShaderMapPtr(FGIAG_GraphCullShaderMapPtr&& Other) noexcept;
	FGIAG_GraphCullShaderMapPtr& operator=(const FGIAG_GraphCullShaderMapPtr& Other);
	FGIAG_GraphCullShaderMapPtr& operator=(FGIAG_GraphCullShaderMapPtr&& Other) noexcept;
	~FGIAG_GraphCullShaderMapPtr();

	void Reset();
	bool IsValid() const { return Ptr != nullptr; }

	FGIAGShaderMap& operator*() const { return *Ptr; }
	FGIAGShaderMap* operator->() const { return Ptr; }
	operator FGIAGShaderMap*() const { return Ptr; }
private:
	FGIAGShaderMap* Ptr = nullptr;
};

/** Compiled schedule step: execute a consecutive batch of nodes with the same node type (USTRUCT FName). */
struct FGIAG_AnimDispatchBatch
{
	/** Node type id = NodeType->GetStruct()->GetFName() */
	FName TypeId;

	TArray<int32> NodeIndices;
};

/** Per-node compiled metadata. */
struct FGIAG_AnimCompiledNode
{
	/** Node type id = NodeType->GetStruct()->GetFName() */
	FName TypeId;

	/** Node member name in the AnimGraph's DefaultGraphInstance (stable identifier for APIs). */
	FName MemberName = NAME_None;

	/** Non-owning pointer to the node type implementation. Resolved at runtime via registry. */
	const IGIAG_AnimNodeMeta* NodeMeta = nullptr;

	/** Per-node settings blob (owned by compiled data). Interpretation is node-type specific. */
	FConstStructView Settings;

	/** Cached member offset (bytes) within the instance struct. Avoids runtime FindPropertyByName. */
	int32 InstanceDataOffset = INDEX_NONE;

	/** GPU upload stride (bytes) returned by GatherUploadsGPU for this node (0 => no GPU upload). */
	uint32 GpuUploadStrideBytes = 0;

	int32 NumInputPins = 0;

	int32 NumOutputPins = 0;

	/** Input pin sources (pose-only for v1). Size = NumInputPins; INDEX_NONE if unconnected. */
	TArray<FGIAG_AnimOutputPinRef> InputSources;

	/** Resolved pose resource indices for each input pin (only valid when pin type is Pose). */
	TArray<int32> InputPoseResources;

	/** Pose resource indices for each output pin (only valid when pin type is Pose). */
	TArray<int32> OutputPoseResources;
};

/**
 * Compiled AnimGraph: frozen topology + dispatch schedule.
 * No runtime topological work should be required beyond iterating DispatchSchedule.
 */
struct FGIAG_AnimGraphCompiledData
{
	int32 NumNodes = 0;

	int32 NumPoseResources = 0;

	TArray<FGIAG_AnimCompiledNode> Nodes;

	TArray<int32> ExecOrder;

	TArray<FGIAG_AnimDispatchBatch> DispatchSchedule;

	/** Which output pose pin is the final pose to be skinned. */
	FGIAG_AnimOutputPinRef FinalPoseOutput;

	/** Graph hash for shader specialization / caches (stable across runs for identical topology). */
	uint64 GraphHash = 0;

	/** Whether this graph has any nodes that declare GPU cull logic. */
	bool bHasNodeCullLogic = false;

	/** Whether node culling is enabled for this graph (user override + has-cull-node check). Used by both GPU and CPU runners for fast-path decisions. */
	bool bEnableNodeCull = false;

	/** Per-graph GraphCull shader map (compiled on-demand in editor; cooked in shipping). */
	FGIAG_GraphCullShaderMapPtr GraphCullShaderMap;

	/** Nodes that expose a cull param buffer (in the same order as CullParamSymbols). */
	TArray<int32> CullParamNodeIndices;
	/** Generated HLSL symbols for cull param buffers (per-node, strong-typed StructuredBuffer<ElementType>). */
	TArray<FString> CullParamSymbols;
	/** SRV base indices for CullParamSymbols (same order). */
	TArray<uint16> CullParamSRVBaseIndices;
};

USTRUCT(BlueprintType, BlueprintInternalUseOnly)
struct FGIAG_AnimGraphInstanceRef
#if CPP
	: FConstStructView
#endif
{
	GENERATED_BODY()

#if !CPP
	FConstStructView ConstStructView;
#endif

	FGIAG_AnimGraphInstanceRef() = default;
	FGIAG_AnimGraphInstanceRef(const UScriptStruct* InstanceStruct, const void* Instance)
		: FConstStructView(InstanceStruct, (const uint8*)Instance)
	{}
	template<typename T> requires std::is_base_of_v<FGIAG_AnimGraphInstance, T>
	FGIAG_AnimGraphInstanceRef(const T& AnimGraphInstance)
		: FGIAG_AnimGraphInstanceRef(T::StaticStruct(), &AnimGraphInstance)
	{}
};

UCLASS(BlueprintType, Abstract, Blueprintable)
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_AnimGraph : public UDataAsset
{
	GENERATED_BODY()
public:
	/** Enable/disable GPU node culling for this graph. Default: enabled. */
	UPROPERTY(EditDefaultsOnly, Category="GameInstancedAnim|Cull")
	bool bEnableNodeCull = true;

	/** User override: declare nodes and wiring. Must be deterministic. */
	virtual void BuildGraph(FGIAG_AnimGraphBuilder& Builder) const { ReceiveBuildGraph(Builder); }

	virtual FGIAG_AnimGraphInstanceRef GetDefaultGraphInstance() const { return ReceiveGetDefaultGraphInstance(); }

	/** Compile and cache compiled data. Safe to call multiple times. */
	const FGIAG_AnimGraphCompiledData& Compile();

	const FGIAG_AnimGraphCompiledData& GetCompiledData() const { return Compiled; }

	// UObject overrides for cooked shader caching (GraphCull per-graph ShaderMap).
#if WITH_EDITOR
	virtual void BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform) override;
	virtual void ClearCachedCookedPlatformData(const ITargetPlatform* TargetPlatform) override;
	virtual bool IsCachedCookedPlatformDataLoaded(const ITargetPlatform* TargetPlatform) override;
#endif // WITH_EDITOR
	virtual void Serialize(FArchive& Ar) override;
protected:
	UFUNCTION(BlueprintImplementableEvent)
	FGIAG_AnimGraphInstanceRef ReceiveGetDefaultGraphInstance() const;
	
	UFUNCTION(BlueprintImplementableEvent)
	void ReceiveBuildGraph(UPARAM(Ref)FGIAG_AnimGraphBuilder& Builder) const;
private:
	struct FCookedGraphCullShaderMapEntry
	{
		FName ShaderFormat = NAME_None;
		FGIAG_GraphCullShaderMapPtr ShaderMap;
	};

	bool bCompiled = false;

	FGIAG_AnimGraphCompiledData Compiled;

	// Cooked per-platform GraphCull ShaderMaps (serialized into the asset; loaded at runtime from ShaderCodeLibrary).
	TArray<FCookedGraphCullShaderMapEntry> CookedGraphCullShaderMaps;
};


