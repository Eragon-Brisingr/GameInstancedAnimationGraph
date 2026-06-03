#include "GIAG_MaterialExpressionParameter.h"
#include "MaterialCompiler.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "MaterialCachedData.h"

#define LOCTEXT_NAMESPACE "GIAG_MaterialExpressionParameter"

// ── Base ────────────────────────────────────────────────────────────────────

UGIAG_MaterialExpressionParameterBase::UGIAG_MaterialExpressionParameterBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ParameterName = TEXT("Param");
	bIsParameterExpression = true;

	Outputs.Reset();
	Outputs.Add(FExpressionOutput(TEXT("")));

	UpdateParameterGuid(false, false);

#if WITH_EDITORONLY_DATA
	MenuCategories.Add(LOCTEXT("GIAG", "GIAG"));
#endif
}

void UGIAG_MaterialExpressionParameterBase::PatchPerInstanceCustomDataFlag(UMaterialInterface* Material)
{
	if (!Material)
	{
		return;
	}
	const FMaterialCachedExpressionData& CachedData = Material->GetCachedExpressionData();
	if (&CachedData == &FMaterialCachedExpressionData::EmptyData)
	{
		return;
	}
	const_cast<FMaterialCachedExpressionData&>(CachedData).bHasPerInstanceCustomData = true;
}

void UGIAG_MaterialExpressionParameterBase::PostLoad()
{
	Super::PostLoad();
	PatchPerInstanceCustomDataFlag(Cast<UMaterialInterface>(GetOuter()));
}

#if WITH_EDITOR

int32 UGIAG_MaterialExpressionParameterBase::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	const int32 NC = GetNumChannels();
	const int32 SafeIndex = static_cast<int32>(GIAG_MaterialExpressionParameter::ClampDataIndex(DataIndex, NC));

	if (SafeIndex + NC > GIAG_MaterialExpressionParameter::MaxDataFloats)
	{
		return Compiler->Errorf(
			TEXT("GIAG_Parameter: DataIndex %u + %d channels exceeds max %d"),
			DataIndex, NC, GIAG_MaterialExpressionParameter::MaxDataFloats);
	}

	if (NC == 1)
	{
		const int32 CPD = Compiler->CustomPrimitiveData(SafeIndex, MCT_Float);
		return Compiler->PerInstanceCustomData(SafeIndex, CPD);
	}

	if (NC == 3)
	{
		const int32 CPD3 = Compiler->CustomPrimitiveData(SafeIndex, MCT_Float3);
		return Compiler->PerInstanceCustomData3Vector(SafeIndex, CPD3);
	}

	// Float2 / Float4: build per-component and append
	int32 Result = INDEX_NONE;
	for (int32 i = 0; i < NC; ++i)
	{
		const int32 Idx = SafeIndex + i;
		const int32 CPD_i = Compiler->CustomPrimitiveData(Idx, MCT_Float);
		const int32 PICD_i = Compiler->PerInstanceCustomData(Idx, CPD_i);
		Result = (i == 0) ? PICD_i : Compiler->AppendVector(Result, PICD_i);
	}
	return Result;
}

void UGIAG_MaterialExpressionParameterBase::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(FString::Printf(TEXT("GIAG Param [%u]"), DataIndex));
	OutCaptions.Add(FString::Printf(TEXT("'%s'"), *ParameterName.ToString()));
}

FString UGIAG_MaterialExpressionParameterBase::GetEditableName() const
{
	return ParameterName.ToString();
}

void UGIAG_MaterialExpressionParameterBase::SetEditableName(const FString& NewName)
{
	ParameterName = *NewName;
	FProperty* Prop = FindFProperty<FProperty>(GetClass(), GET_MEMBER_NAME_CHECKED(UGIAG_MaterialExpressionParameterBase, ParameterName));
	FPropertyChangedEvent Event(Prop);
	PostEditChangeProperty(Event);
}

void UGIAG_MaterialExpressionParameterBase::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	DataIndex = GIAG_MaterialExpressionParameter::ClampDataIndex(DataIndex, GetNumChannels());
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

#endif // WITH_EDITOR

// ── Scalar (Float1) ────────────────────────────────────────────────────────

UGIAG_ScalarParameter::UGIAG_ScalarParameter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
void UGIAG_ScalarParameter::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(FString::Printf(TEXT("GIAG Scalar [%u]"), DataIndex));
	OutCaptions.Add(FString::Printf(TEXT("'%s'"), *ParameterName.ToString()));
}

bool UGIAG_ScalarParameter::GetParameterValue(FMaterialParameterMetadata& OutMeta) const
{
	if (ParameterName.IsNone()) return false;
	OutMeta.Value = FMaterialParameterValue(DefaultValue);
	OutMeta.ExpressionGuid = ExpressionGUID;
	OutMeta.PrimitiveDataIndex = DataIndex;
	OutMeta.Description = Desc;
	return true;
}
#endif

// ── Vector2 (Float2) ───────────────────────────────────────────────────────

UGIAG_Vector2Parameter::UGIAG_Vector2Parameter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
void UGIAG_Vector2Parameter::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(FString::Printf(TEXT("GIAG Vector2 [%u]"), DataIndex));
	OutCaptions.Add(FString::Printf(TEXT("'%s'"), *ParameterName.ToString()));
}

bool UGIAG_Vector2Parameter::GetParameterValue(FMaterialParameterMetadata& OutMeta) const
{
	if (ParameterName.IsNone()) return false;
	OutMeta.Value = FMaterialParameterValue(FLinearColor(DefaultValue.X, DefaultValue.Y, 0.f, 0.f));
	OutMeta.ExpressionGuid = ExpressionGUID;
	OutMeta.PrimitiveDataIndex = DataIndex;
	OutMeta.Description = Desc;
	return true;
}
#endif

// ── Vector (Float3) ────────────────────────────────────────────────────────

UGIAG_VectorParameter::UGIAG_VectorParameter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
void UGIAG_VectorParameter::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(FString::Printf(TEXT("GIAG Vector [%u]"), DataIndex));
	OutCaptions.Add(FString::Printf(TEXT("'%s'"), *ParameterName.ToString()));
}

bool UGIAG_VectorParameter::GetParameterValue(FMaterialParameterMetadata& OutMeta) const
{
	if (ParameterName.IsNone()) return false;
	OutMeta.Value = FMaterialParameterValue(FLinearColor(DefaultValue.X, DefaultValue.Y, DefaultValue.Z, 0.f));
	OutMeta.ExpressionGuid = ExpressionGUID;
	OutMeta.PrimitiveDataIndex = DataIndex;
	OutMeta.Description = Desc;
	return true;
}
#endif

// ── Color (Float4) ─────────────────────────────────────────────────────────

UGIAG_ColorParameter::UGIAG_ColorParameter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
void UGIAG_ColorParameter::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(FString::Printf(TEXT("GIAG Color [%u]"), DataIndex));
	OutCaptions.Add(FString::Printf(TEXT("'%s'"), *ParameterName.ToString()));
}

bool UGIAG_ColorParameter::GetParameterValue(FMaterialParameterMetadata& OutMeta) const
{
	if (ParameterName.IsNone()) return false;
	OutMeta.Value = FMaterialParameterValue(DefaultValue);
	OutMeta.ExpressionGuid = ExpressionGUID;
	OutMeta.PrimitiveDataIndex = DataIndex;
	OutMeta.Description = Desc;
	return true;
}
#endif

#undef LOCTEXT_NAMESPACE
