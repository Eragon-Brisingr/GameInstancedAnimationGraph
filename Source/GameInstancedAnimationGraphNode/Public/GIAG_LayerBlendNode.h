#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GIAG_LayerBlendNode.generated.h"

class UHierarchyTable;

/**
 * BlendLayer settings (compile-time).
 * Uses HierarchyTable Mask to provide per-bone weights.
 */
USTRUCT(BlueprintType)
struct GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_BlendLayerSettings
{
	GENERATED_BODY()

	/** Skeleton table type + Mask element type (see HierarchyTableAnimation). */
	UPROPERTY(EditAnywhere, Category = "Blend")
	TObjectPtr<UHierarchyTable> BlendMaskTable = nullptr;
};

/** BlendLayer node instance (2 pose inputs, 1 pose output). */
USTRUCT(BlueprintType)
struct alignas(16) GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_LayerBlendNode final : public FGIAG_AnimNodeBase
{
	GENERATED_BODY()
public:
	using FNodeMeta = TGIAG_AnimNodeMeta<FGIAG_LayerBlendNode>;
	using FSettings = FGIAG_BlendLayerSettings;

	enum class EInputPin : uint8
	{
		Base = 0,
		Layer = 1,
		Num,
	};

	void SetAlpha(const FGIAG_AnimNodeRef& NodeRef, float NewAlpha)
	{
		if (Alpha == NewAlpha)
		{
			return;
		}
		Alpha = NewAlpha;
		NodeRef.MarkDirty();
	}
	FORCEINLINE float GetAlpha() const { return Alpha; }
protected:
	friend FNodeMeta;

	/** Layer alpha weight. Slot-indexed; default 1.0. */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float Alpha = 1.0f;

	uint32 ComputeCullNeedMaskCPU(uint32 NumInputs) const;
	static void EmitCullNeedMaskHlslBody(FString& Out, const TCHAR*& OutHlslElementType, const TCHAR*& OutMemberName);

	const void* GatherUploadsGPU(uint32& OutUploadStrideBytes) const;

	static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context);
	static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context);

	/** Slot 0 reserved for per-bone mask weights. */
	static void EnumerateResourceRequests(FConstStructView Settings, const USkeleton* Skeleton, EGIAG_AnimResourceTarget Target, TArray<FGIAG_AnimResourceRequest>& Out);
	static bool BuildResourceForGPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TArray<uint8>& OutBytes);
	static bool BuildResourceForCPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TSharedPtr<void>& OutResource);
};

UCLASS(meta = (ScriptMixin = FGIAG_LayerBlendNode))
class UGIAG_LayerBlendNodeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable)
	static void SetAlpha(UPARAM(Ref)FGIAG_LayerBlendNode& Node, const FGIAG_AnimNodeRef& NodeRef, float NewAlpha)
	{
		Node.SetAlpha(NodeRef, NewAlpha);
	}
};
