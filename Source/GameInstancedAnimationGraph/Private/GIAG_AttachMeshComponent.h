#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"

#include "GIAG_AttachMeshComponent.generated.h"

class FGIAG_NativeAttachRegistry;

/**
 * Native GIAG attach mesh renderer:
 */
UCLASS()
class GAMEINSTANCEDANIMATIONGRAPH_API UGIAG_AttachMeshComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGIAG_AttachMeshComponent();

	UPROPERTY(EditAnywhere, Category = "GIAG")
	int32 BucketId = 0;

	UPROPERTY(EditAnywhere, Category = "GIAG")
	TObjectPtr<UStaticMesh> StaticMesh = nullptr;

	/** Optional material overrides (indexed by StaticMesh material slot). */
	UPROPERTY(EditAnywhere, Category = "GIAG")
	TArray<TObjectPtr<UMaterialInterface>> MaterialOverrides;

	/** RT-only registry (owned by World subsystem; injected at creation time). */
	TSharedPtr<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe> AttachRegistry;

	// UPrimitiveComponent
	FPrimitiveSceneProxy* CreateSceneProxy() override;
	FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	int32 GetNumMaterials() const override;
	UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
};

