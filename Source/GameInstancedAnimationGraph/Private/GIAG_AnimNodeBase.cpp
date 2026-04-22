#include "GIAG_AnimNodeBase.h"

#include "GameInstancedAnimationGraphSubsystem.h"
#include "GIAG_AnimNodeMetaManager.h"

void FGIAG_AnimNodeRef::MarkDirty() const
{
	check(NodeIndex != INDEX_NONE);
	auto& Bucket = System->Buckets[BucketIndex];
	Bucket.MarkNodeParamDirty(NodeIndex, SlotIndex);
}

float FGIAG_AnimNodeRef::GetTimeSlotSeconds() const
{
	return System->TimeSlots[System->AnimRecords[RecordIndex].TimeSlotIndex];
}

IGIAG_AnimNodeMeta::IGIAG_AnimNodeMeta()
{
	auto& Manager = FGIAG_AnimNodeMetaManager::Get();
	Manager.Metas.Add(this);
}

IGIAG_AnimNodeMeta::~IGIAG_AnimNodeMeta()
{
	auto& Manager = FGIAG_AnimNodeMetaManager::Get();
	Manager.Metas.RemoveSingleSwap(this);
}

