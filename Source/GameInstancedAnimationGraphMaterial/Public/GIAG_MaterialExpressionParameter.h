#pragma once

#include "CoreMinimal.h"
#include "SceneTypes.h"
#include "Materials/MaterialExpression.h"
#include "GIAG_MaterialExpressionParameter.generated.h"

class UMaterialInterface;

namespace GIAG_MaterialExpressionParameter
{
	inline constexpr int32 MaxDataFloats = FCustomPrimitiveData::NumCustomPrimitiveDataFloats;

	inline uint32 ClampDataIndex(uint32 InIndex, int32 InNumChannels)
	{
		const int32 MaxIndex = MaxDataFloats - InNumChannels;
		return static_cast<uint32>(FMath::Clamp(static_cast<int32>(InIndex), 0, FMath::Max(MaxIndex, 0)));
	}
}

/**
 * Base class for GIAG adaptive material parameters.
 * Reads PerInstanceCustomData when available (ISKMC), falling back to CustomPrimitiveData (SKM).
 */
UCLASS(Abstract, collapsecategories, hidecategories = Object, MinimalAPI)
class UGIAG_MaterialExpressionParameterBase : public UMaterialExpression
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, Category = Parameter)
	FName ParameterName;

	UPROPERTY(EditAnywhere, Category = Parameter, meta = (ClampMin = "0", ClampMax = "35"))
	uint32 DataIndex = 0;

	UPROPERTY()
	FGuid ExpressionGUID;

	virtual FGuid& GetParameterExpressionId() override { return ExpressionGUID; }
	virtual void PostLoad() override;

	static void PatchPerInstanceCustomDataFlag(UMaterialInterface* Material);

protected:
	virtual int32 GetNumChannels() const PURE_VIRTUAL(UGIAG_MaterialExpressionParameterBase::GetNumChannels, return 1;);

#if WITH_EDITOR
public:
	virtual int32 Compile(class FMaterialCompiler* Compiler, int32 OutputIndex) override;
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	virtual bool CanRenameNode() const override { return true; }
	virtual FString GetEditableName() const override;
	virtual void SetEditableName(const FString& NewName) override;

	virtual bool HasAParameterName() const override { return true; }
	virtual FName GetParameterName() const override { return ParameterName; }
	virtual void SetParameterName(const FName& Name) override { ParameterName = Name; }
#endif
};

// ────────────────────────────────────────────────────────────────────────────

UCLASS(MinimalAPI, meta = (MaterialNewNode, DisplayName = "GIAG Scalar Parameter"))
class UGIAG_ScalarParameter : public UGIAG_MaterialExpressionParameterBase
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, Category = Parameter)
	float DefaultValue = 0.f;

protected:
	virtual int32 GetNumChannels() const override { return 1; }

#if WITH_EDITOR
public:
	virtual uint32 GetOutputType(int32 OutputIndex) override { return MCT_Float; }
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual bool GetParameterValue(FMaterialParameterMetadata& OutMeta) const override;
#endif
};

// ────────────────────────────────────────────────────────────────────────────

UCLASS(MinimalAPI, meta = (MaterialNewNode, DisplayName = "GIAG Vector2 Parameter"))
class UGIAG_Vector2Parameter : public UGIAG_MaterialExpressionParameterBase
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, Category = Parameter)
	FVector2f DefaultValue = FVector2f::ZeroVector;

protected:
	virtual int32 GetNumChannels() const override { return 2; }

#if WITH_EDITOR
public:
	virtual uint32 GetOutputType(int32 OutputIndex) override { return MCT_Float2; }
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual bool GetParameterValue(FMaterialParameterMetadata& OutMeta) const override;
#endif
};

// ────────────────────────────────────────────────────────────────────────────

UCLASS(MinimalAPI, meta = (MaterialNewNode, DisplayName = "GIAG Vector Parameter"))
class UGIAG_VectorParameter : public UGIAG_MaterialExpressionParameterBase
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, Category = Parameter)
	FVector3f DefaultValue = FVector3f::ZeroVector;

protected:
	virtual int32 GetNumChannels() const override { return 3; }

#if WITH_EDITOR
public:
	virtual uint32 GetOutputType(int32 OutputIndex) override { return MCT_Float3; }
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual bool GetParameterValue(FMaterialParameterMetadata& OutMeta) const override;
#endif
};

// ────────────────────────────────────────────────────────────────────────────

UCLASS(MinimalAPI, meta = (MaterialNewNode, DisplayName = "GIAG Color Parameter"))
class UGIAG_ColorParameter : public UGIAG_MaterialExpressionParameterBase
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, Category = Parameter)
	FLinearColor DefaultValue = FLinearColor::Black;

protected:
	virtual int32 GetNumChannels() const override { return 4; }

#if WITH_EDITOR
public:
	virtual uint32 GetOutputType(int32 OutputIndex) override { return MCT_Float4; }
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual bool GetParameterValue(FMaterialParameterMetadata& OutMeta) const override;
#endif
};
