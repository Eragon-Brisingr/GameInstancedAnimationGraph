#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GIAG_AdditiveNode.generated.h"

/**
 * Additive node (Base + Additive, scaled by Alpha) - Local-space additive, isomorphic to UE's ApplyAdditive.
 *
 * Contract (matching UE additive pose conventions):
 * - Additive pose contains delta transforms:
 *   - Translation: additive (0 means identity)
 *   - Rotation: delta quaternion (identity means no rotation)
 *   - Scale3D: additive scale delta (0 means identity; final uses (1 + delta))
 */
USTRUCT(BlueprintType)
struct alignas(16) GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_AdditiveNode final : public FGIAG_AnimNodeBase
{
	GENERATED_BODY()
public:
	using FNodeMeta = TGIAG_AnimNodeMeta<FGIAG_AdditiveNode>;

	enum class EInputPin : uint8
	{
		Base = 0,
		Additive = 1,
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

	/** Additive alpha weight. Slot-indexed; default 1.0. */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float Alpha = 1.0f;

	uint32 ComputeCullNeedMaskCPU(uint32 NumInputs) const;
	static void EmitCullNeedMaskHlslBody(FString& Out, const TCHAR*& OutHlslElementType, const TCHAR*& OutMemberName);

	const void* GatherUploadsGPU(uint32& OutUploadStrideBytes) const;

	static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context);
	static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context);
};

UCLASS(meta = (ScriptMixin = FGIAG_AdditiveNode))
class UGIAG_AdditiveNodeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable)
	static void SetAlpha(UPARAM(Ref)FGIAG_AdditiveNode& Node, const FGIAG_AnimNodeRef& NodeRef, float NewAlpha)
	{
		Node.SetAlpha(NodeRef, NewAlpha);
	}
};
