#include "GIAG_MaterialUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "MaterialCachedData.h"

int32 GIAG::DetectNumCustomDataFloatsFromMaterials(const USkeletalMesh& SkeletalMesh)
{
	int32 MaxFloatsNeeded = 0;

	const TArray<FSkeletalMaterial>& Materials = SkeletalMesh.GetMaterials();
	for (const FSkeletalMaterial& MatSlot : Materials)
	{
		UMaterialInterface* MatInterface = MatSlot.MaterialInterface;
		if (!MatInterface)
		{
			continue;
		}
		const FMaterialCachedExpressionData& CachedData = MatInterface->GetCachedExpressionData();

		for (const int32 Idx : CachedData.ScalarPrimitiveDataIndexValues)
		{
			if (Idx != INDEX_NONE)
			{
				MaxFloatsNeeded = FMath::Max(MaxFloatsNeeded, Idx + 1);
			}
		}

		for (const int32 Idx : CachedData.VectorPrimitiveDataIndexValues)
		{
			if (Idx != INDEX_NONE)
			{
				MaxFloatsNeeded = FMath::Max(MaxFloatsNeeded, Idx + 4);
			}
		}
	}

	return FMath::Clamp(MaxFloatsNeeded, 0, (int32)FCustomPrimitiveData::NumCustomPrimitiveDataFloats);
}

TMap<FName, int32> GIAG::BuildCustomDataIndexMapFromMaterials(const USkeletalMesh& SkeletalMesh)
{
	TMap<FName, int32> Result;

	const TArray<FSkeletalMaterial>& Materials = SkeletalMesh.GetMaterials();
	for (const FSkeletalMaterial& MatSlot : Materials)
	{
		UMaterialInterface* MatInterface = MatSlot.MaterialInterface;
		if (!MatInterface)
		{
			continue;
		}
		const FMaterialCachedExpressionData& CachedData = MatInterface->GetCachedExpressionData();

		auto CollectFromEntry = [&](EMaterialParameterType Type, const TArray<int32>& PrimitiveDataIndices)
		{
			const FMaterialCachedParameterEntry& Entry = CachedData.RuntimeEntries[static_cast<int32>(Type)];
			int32 ParamIdx = 0;
			for (const FMaterialParameterInfo& Info : Entry.ParameterInfoSet)
			{
				if (ParamIdx < PrimitiveDataIndices.Num())
				{
					const int32 DataIdx = PrimitiveDataIndices[ParamIdx];
					if (DataIdx != INDEX_NONE && !Info.Name.IsNone())
					{
						if (const int32* Existing = Result.Find(Info.Name))
						{
							ensureMsgf(*Existing == DataIdx,
								TEXT("GIAG::BuildCustomDataIndexMapFromMaterials: parameter '%s' has conflicting DataIndex (%d vs %d)"),
								*Info.Name.ToString(), *Existing, DataIdx);
						}
						else
						{
							Result.Add(Info.Name, DataIdx);
						}
					}
				}
				++ParamIdx;
			}
		};

		CollectFromEntry(EMaterialParameterType::Scalar, CachedData.ScalarPrimitiveDataIndexValues);
		CollectFromEntry(EMaterialParameterType::Vector, CachedData.VectorPrimitiveDataIndexValues);
	}

	return Result;
}
