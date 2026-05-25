#include "GIAG_HierarchyTableMaskUtils.h"

#include "Animation/Skeleton.h"
#include "HierarchyTable.h"
#include "MaskProfile/HierarchyTableTypeMask.h"
#include "SkeletonHierarchyTableType.h"

namespace GIAG::HierarchyTableMaskUtils
{
	uint64 MakeMaskTableKeyHash(const USkeleton* Skeleton, UHierarchyTable* Table, uint64 Salt)
	{
		const uint64 SkeletonHash = (uint64)PointerHash(Skeleton);
		const uint64 TableHash = (uint64)PointerHash(Table);
		const uint64 BoneCountHash = (uint64)(Skeleton ? Skeleton->GetReferenceSkeleton().GetNum() : 0);
		uint64 GuidHash = 0;
		if (Table)
		{
			const FGuid Guid = Table->GetHierarchyGuid();
			GuidHash = ((uint64)Guid.A << 32) ^ (uint64)Guid.B ^ ((uint64)Guid.C << 16) ^ (uint64)Guid.D;
		}
		return (SkeletonHash << 32)
			^ (TableHash * 0x9E3779B185EBCA87ull)
			^ (BoneCountHash * 0xC2B2AE3D27D4EB4Full)
			^ (GuidHash * 0x165667B19E3779F9ull)
			^ Salt;
	}

	bool BuildPerBoneMaskWeights(const USkeleton* Skeleton, const UHierarchyTable* Table, TArray<float>& OutWeights)
	{
		OutWeights.Reset();

		if (!IsValid(Skeleton))
		{
			return false;
		}

		const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
		const int32 NumBones = RefSkel.GetNum();
		if (NumBones <= 0)
		{
			return false;
		}

		// Default fallback: zeros (=> output Base).
		OutWeights.SetNumZeroed(NumBones);

		if (!IsValid(Table))
		{
			return true;
		}

		if (!Table->IsTableType<FHierarchyTable_TableType_Skeleton>() || !Table->IsElementType<FHierarchyTable_ElementType_Mask>())
		{
			return true;
		}

		const FHierarchyTable_TableType_Skeleton& Meta = Table->GetTableMetadata<FHierarchyTable_TableType_Skeleton>();
		if (Meta.Skeleton != Skeleton)
		{
			return true;
		}

		bool bNeedNameFallback = false;
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const FHierarchyTableEntryData* Entry = Table->GetTableEntry(BoneIndex);
			if (!Entry || Entry->Identifier != RefSkel.GetBoneName(BoneIndex))
			{
				bNeedNameFallback = true;
				break;
			}
		}

		TMap<FName, int32> BoneEntryIndexByName;
		if (bNeedNameFallback)
		{
			const TArray<FHierarchyTableEntryData>& Data = Table->GetTableData();
			BoneEntryIndexByName.Reserve(Data.Num());
			for (int32 EntryIndex = 0; EntryIndex < Data.Num(); ++EntryIndex)
			{
				const FHierarchyTableEntryData& Entry = Data[EntryIndex];
				if (Entry.TablePayload.GetScriptStruct() == FHierarchyTable_TablePayloadType_Skeleton::StaticStruct()
					&& Entry.TablePayload.Get<FHierarchyTable_TablePayloadType_Skeleton>().EntryType == ESkeletonHierarchyTable_TablePayloadEntryType::Bone)
				{
					BoneEntryIndexByName.Add(Entry.Identifier, EntryIndex);
				}
			}
		}

		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const FName BoneName = RefSkel.GetBoneName(BoneIndex);

			const FHierarchyTableEntryData* Entry = nullptr;
			if (!bNeedNameFallback)
			{
				Entry = Table->GetTableEntry(BoneIndex);
			}
			else if (const int32* FoundIndex = BoneEntryIndexByName.Find(BoneName))
			{
				Entry = Table->GetTableEntry(*FoundIndex);
			}

			if (!Entry)
			{
				OutWeights[BoneIndex] = 0.0f;
				continue;
			}

			const float MaskValue = Entry->GetValue<FHierarchyTable_ElementType_Mask>()->Value;
			OutWeights[BoneIndex] = FMath::Clamp(MaskValue, 0.0f, 1.0f);
		}

		return true;
	}
}

