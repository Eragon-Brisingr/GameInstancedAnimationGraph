#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GIAG_LookAtNode.generated.h"

USTRUCT(BlueprintType)
struct GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_LookAtSettings
{
	GENERATED_BODY()

	/** Skeleton bone name to rotate. */
	UPROPERTY(EditAnywhere, Category = "LookAt")
	FName BoneToModify = NAME_None;

	/** Local-space look-at axis of BoneToModify. */
	UPROPERTY(EditAnywhere, Category = "LookAt")
	FVector3f LookAtAxis = FVector3f(0.0f, 1.0f, 0.0f);

	/** Max angular deviation (degrees) around LookAtAxis. 0 means no clamp. */
	UPROPERTY(EditAnywhere, Category = "LookAt")
	float LookAtClamp = 0.0f;

	/** Blend duration for enable/disable transition. */
	UPROPERTY(EditAnywhere, Category = "LookAt")
	float BlendDurationSeconds = 0.2f;
};

/** Runtime GPU upload payload for one slot. */
struct alignas(16) FGIAG_LookAtRuntimeData
{
	FVector3f TargetLocationWS = FVector3f(100.0f, 0.0f, 0.0f);
	float LastEnableDisableTimeSeconds = 0.0f;
	uint32 bEnabled = 1u;
	FVector3f Padding = FVector3f::ZeroVector;
};
static_assert(sizeof(FGIAG_LookAtRuntimeData) == 32, "FGIAG_LookAtRuntimeData layout changed; update shader struct to match.");

/** LookAt node instance (1 pose input, 1 pose output). */
USTRUCT(BlueprintType)
struct alignas(16) GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_LookAtNode final : public FGIAG_AnimNodeBase
{
	GENERATED_BODY()
public:
	FGIAG_LookAtNode() = default;
	FGIAG_LookAtNode(bool bEnable)
		: RuntimeData{ FVector3f::ZeroVector, 0, bEnable }
	{}
	
	using FNodeMeta = TGIAG_AnimNodeMeta<FGIAG_LookAtNode>;
	using FSettings = FGIAG_LookAtSettings;

	enum class EInputPin : uint8
	{
		Base = 0,
		Num,
	};

	static EGIAG_AnimPinType GetInputPinType(int32 PinIndex)
	{
		check(PinIndex == (int32)EInputPin::Base);
		return EGIAG_AnimPinType::ComponentPose;
	}

	static EGIAG_AnimPinType GetOutputPinType(int32 PinIndex)
	{
		check(PinIndex == (int32)EOutputPin::Out);
		return EGIAG_AnimPinType::ComponentPose;
	}

	void SetTargetLocationWS(const FGIAG_AnimNodeRef& NodeRef, const FVector& NewTargetLocationWS)
	{
		const FVector3f NewValue = FVector3f(NewTargetLocationWS);
		if (RuntimeData.TargetLocationWS.Equals(NewValue, KINDA_SMALL_NUMBER))
		{
			return;
		}
		RuntimeData.TargetLocationWS = NewValue;
		NodeRef.MarkDirty();
	}

	void SetEnabled(const FGIAG_AnimNodeRef& NodeRef, bool bNewEnabled);
	bool IsEnabled() const { return RuntimeData.bEnabled != 0u; }

protected:
	friend FNodeMeta;

	FGIAG_LookAtRuntimeData RuntimeData;

	const void* GatherUploadsGPU(uint32& OutUploadStrideBytes) const;

	static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context);
	static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context);

	/** Slot 0 reserved for static BoneIndex. */
	static void EnumerateResourceRequests(FConstStructView Settings, const USkeleton* Skeleton, EGIAG_AnimResourceTarget Target, TArray<FGIAG_AnimResourceRequest>& Out);
	static bool BuildResourceForGPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TArray<uint8>& OutBytes);
	static bool BuildResourceForCPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TSharedPtr<void>& OutResource);
};

UCLASS(meta = (ScriptMixin = FGIAG_LookAtNode))
class UGIAG_LookAtNodeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable)
	static void SetTargetLocationWS(UPARAM(Ref) FGIAG_LookAtNode& Node, const FGIAG_AnimNodeRef& NodeRef, FVector NewTargetLocationWS)
	{
		Node.SetTargetLocationWS(NodeRef, NewTargetLocationWS);
	}

	UFUNCTION(BlueprintCallable)
	static void SetEnabled(UPARAM(Ref) FGIAG_LookAtNode& Node, const FGIAG_AnimNodeRef& NodeRef, bool bEnabled)
	{
		Node.SetEnabled(NodeRef, bEnabled);
	}
};
