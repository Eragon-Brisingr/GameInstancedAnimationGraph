#include "GameInstancedAnimationGraphSubsystem.h"

#include "GIAG_DebugReadback.h"
#include "GIAG_AnimGraphShaders.h"

void UGameInstancedAnimationGraphSubsystem::DebugSetLocalPoseReadbackEnabled(FGameInstancedAnimationGraphHandle Handle, bool bEnabled)
{
	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return;
	}

	if (bEnabled)
	{
		DebugReadbackEnabledSerialByRecordIndex.Add(Handle.RecordIndex, Handle.SerialNumber);
	}
	else
	{
		DebugReadbackEnabledSerialByRecordIndex.Remove(Handle.RecordIndex);
		DebugLatestLocalPoseByRecordIndex.Remove(Handle.RecordIndex);
	}
}

void UGameInstancedAnimationGraphSubsystem::DebugSetNeedNodeBitsReadbackEnabled(FGameInstancedAnimationGraphHandle Handle, bool bEnabled)
{
	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return;
	}

	if (bEnabled)
	{
		DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Add(Handle.RecordIndex, Handle.SerialNumber);
	}
	else
	{
		DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Remove(Handle.RecordIndex);
		DebugLatestNeedNodeBitsByRecordIndex.Remove(Handle.RecordIndex);
	}
}

bool UGameInstancedAnimationGraphSubsystem::DebugGetLatestLocalPoseReadbackTRS(FGameInstancedAnimationGraphHandle Handle, int64& OutCpuRequestFrame, int32& OutNumBones, TArray<FTransform3f>& OutLocalPoseTRS)
{
	OutCpuRequestFrame = 0;
	OutNumBones = 0;
	OutLocalPoseTRS.Reset();

	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return false;
	}

	PumpDebugReadbacks_GameThread();

	const int32* EnabledSerial = DebugReadbackEnabledSerialByRecordIndex.Find(Handle.RecordIndex);
	if (!EnabledSerial || *EnabledSerial != Handle.SerialNumber)
	{
		return false;
	}

	const FDebugLocalPoseCache* Cache = DebugLatestLocalPoseByRecordIndex.Find(Handle.RecordIndex);
	if (!Cache || Cache->NumBones <= 0 || Cache->LocalPoseTRS.Num() == 0)
	{
		return false;
	}

	OutCpuRequestFrame = (int64)Cache->CpuRequestFrame;
	OutNumBones = Cache->NumBones;
	OutLocalPoseTRS = Cache->LocalPoseTRS;
	return true;
}

bool UGameInstancedAnimationGraphSubsystem::DebugGetLatestNeedNodeBitsReadback(
	const FGameInstancedAnimationGraphHandle Handle,
	int64& OutCpuRequestFrame,
	int32& OutSlotIndex,
	int32& OutNumNodes,
	int32& OutWordsPerSlot,
	TArray<int32>& OutWords)
{
	OutCpuRequestFrame = 0;
	OutSlotIndex = 0;
	OutNumNodes = 0;
	OutWordsPerSlot = 0;
	OutWords.Reset();

	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec)
	{
		return false;
	}

	PumpDebugReadbacks_GameThread();

	const int32* EnabledSerial = DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Find(Handle.RecordIndex);
	if (!EnabledSerial || *EnabledSerial != Handle.SerialNumber)
	{
		return false;
	}

	const FDebugNeedNodeBitsCache* Cache = DebugLatestNeedNodeBitsByRecordIndex.Find(Handle.RecordIndex);
	if (!Cache || Cache->WordsPerSlot == 0 || Cache->Words.Num() == 0)
	{
		return false;
	}

	OutCpuRequestFrame = (int64)Cache->CpuRequestFrame;
	OutSlotIndex = (int32)Cache->SlotIndex;
	OutNumNodes = (int32)Cache->NumNodes;
	OutWordsPerSlot = (int32)Cache->WordsPerSlot;
	OutWords.SetNumZeroed(Cache->Words.Num());
	for (int32 i = 0; i < Cache->Words.Num(); ++i)
	{
		OutWords[i] = (int32)Cache->Words[i];
	}
	return true;
}

bool UGameInstancedAnimationGraphSubsystem::DebugGetCpuLocalPoseTRS(FGameInstancedAnimationGraphHandle Handle, int64& OutCpuFrame, int32& OutNumBones, TArray<FTransform3f>& OutLocalPoseTRS)
{
	OutCpuFrame = 0;
	OutNumBones = 0;
	OutLocalPoseTRS.Reset();

	FInstancedAnimRecord* Rec = ResolveRecord(Handle);
	if (!Rec || !Rec->CpuProxyActor)
	{
		return false;
	}

	uint64 Frame = 0;
	TConstArrayView<FTransform3f> LocalPose;
	if (!TryGetCpuPoseCache_NoLock(Handle, Frame, LocalPose) || LocalPose.Num() <= 0)
	{
		return false;
	}

	OutCpuFrame = (int64)Frame;
	OutNumBones = LocalPose.Num();
	OutLocalPoseTRS.SetNumZeroed(OutNumBones);
	for (int32 Bone = 0; Bone < OutNumBones; ++Bone)
	{
		OutLocalPoseTRS[Bone] = LocalPose[Bone];
	}
	return true;
}

void UGameInstancedAnimationGraphSubsystem::DebugRequestAttachFxTransformReadback(int32 BucketId, int32 OutputIndex)
{
	if (!AttachReadbackBus.IsValid() || BucketId <= 0 || OutputIndex < 0)
	{
		return;
	}
	FGIAG_AttachReadbackBus::FRequest Req;
	Req.BucketId = (uint32)BucketId;
	Req.OutputIndex = (uint32)OutputIndex;
	Req.CpuRequestFrame = (uint64)GFrameCounter;
	AttachReadbackBus->Enqueue_GameThread(MoveTemp(Req));
}

bool UGameInstancedAnimationGraphSubsystem::DebugGetLatestAttachFxTransformReadback(int32 BucketId, int32 OutputIndex, int64& OutCpuRequestFrame, FTransform3f& OutSocketWS) const
{
	OutCpuRequestFrame = 0;
	OutSocketWS = FTransform3f::Identity;

	if (BucketId <= 0 || OutputIndex < 0)
	{
		return false;
	}

	const_cast<UGameInstancedAnimationGraphSubsystem*>(this)->PumpDebugReadbacks_GameThread();

	const uint64 Key = ((uint64)(uint32)BucketId << 32) | (uint64)(uint32)OutputIndex;
	const FDebugAttachFxTransformCache* Cache = DebugLatestAttachFxTransformByKey.Find(Key);
	if (!Cache)
	{
		return false;
	}

	OutCpuRequestFrame = (int64)Cache->CpuRequestFrame;
	OutSocketWS = FTransform3f(Cache->FxTransform);
	return true;
}

bool UGameInstancedAnimationGraphSubsystem::DebugGetLatestAttachInstanceBuffersReadback(
	int32 BucketId,
	int32 OutputIndex,
	int64& OutCpuRequestFrame,
	FVector3f& OutOrigin,
	FVector3f& OutRow0,
	FVector3f& OutRow1,
	FVector3f& OutRow2) const
{
	OutCpuRequestFrame = 0;
	OutOrigin = FVector3f::ZeroVector;
	OutRow0 = FVector3f::ZeroVector;
	OutRow1 = FVector3f::ZeroVector;
	OutRow2 = FVector3f::ZeroVector;

	if (BucketId <= 0 || OutputIndex < 0)
	{
		return false;
	}

	const_cast<UGameInstancedAnimationGraphSubsystem*>(this)->PumpDebugReadbacks_GameThread();

	const uint64 Key = ((uint64)(uint32)BucketId << 32) | (uint64)(uint32)OutputIndex;
	const FDebugAttachInstanceBuffersCache* Cache = DebugLatestAttachInstanceBuffersByKey.Find(Key);
	if (!Cache)
	{
		return false;
	}

	OutCpuRequestFrame = (int64)Cache->CpuRequestFrame;
	OutOrigin = Cache->Origin;
	OutRow0 = Cache->Row0;
	OutRow1 = Cache->Row1;
	OutRow2 = Cache->Row2;
	return true;
}

void UGameInstancedAnimationGraphSubsystem::PumpDebugReadbacks_GameThread()
{
	FGIAG_LocalPoseReadbackResult Result;
	while (GIAG::DebugReadback::DequeueLocalPose(Result))
	{
		const int32* EnabledSerial = DebugReadbackEnabledSerialByRecordIndex.Find(Result.RecordIndex);
		if (!EnabledSerial || *EnabledSerial != Result.SerialNumber)
		{
			continue;
		}

		FDebugLocalPoseCache& Cache = DebugLatestLocalPoseByRecordIndex.FindOrAdd(Result.RecordIndex);
		Cache.CpuRequestFrame = Result.CpuRequestFrame;
		Cache.NumBones = (int32)Result.NumBones;
		Cache.LocalPoseTRS = MoveTemp(Result.LocalPoseTRS);
	}

	FGIAG_NeedNodeBitsReadbackResult NeedBitsResult;
	while (GIAG::DebugReadback::DequeueNeedNodeBits(NeedBitsResult))
	{
		const int32* EnabledSerial = DebugNeedNodeBitsReadbackEnabledSerialByRecordIndex.Find(NeedBitsResult.RecordIndex);
		if (!EnabledSerial || *EnabledSerial != NeedBitsResult.SerialNumber)
		{
			continue;
		}

		FDebugNeedNodeBitsCache& Cache = DebugLatestNeedNodeBitsByRecordIndex.FindOrAdd(NeedBitsResult.RecordIndex);
		Cache.CpuRequestFrame = NeedBitsResult.CpuRequestFrame;
		Cache.SlotIndex = NeedBitsResult.SlotIndex;
		Cache.NumNodes = NeedBitsResult.NumNodes;
		Cache.WordsPerSlot = NeedBitsResult.WordsPerSlot;
		Cache.Words = MoveTemp(NeedBitsResult.Words);
	}

	FGIAG_AttachFxTransformReadbackResult AttachResult;
	while (GIAG::DebugReadback::DequeueAttachFxTransform(AttachResult))
	{
		const uint64 Key = ((uint64)AttachResult.BucketId << 32) | (uint64)AttachResult.OutputIndex;
		FDebugAttachFxTransformCache& Cache = DebugLatestAttachFxTransformByKey.FindOrAdd(Key);
		Cache.CpuRequestFrame = AttachResult.CpuRequestFrame;
		Cache.FxTransform = AttachResult.FxTransform;
	}

	FGIAG_AttachInstanceBuffersReadbackResult AttachInstResult;
	while (GIAG::DebugReadback::DequeueAttachInstanceBuffers(AttachInstResult))
	{
		const uint64 Key = ((uint64)AttachInstResult.BucketId << 32) | (uint64)AttachInstResult.OutputIndex;
		FDebugAttachInstanceBuffersCache& Cache = DebugLatestAttachInstanceBuffersByKey.FindOrAdd(Key);
		Cache.CpuRequestFrame = AttachInstResult.CpuRequestFrame;
		Cache.Origin = AttachInstResult.Origin;
		Cache.Row0 = AttachInstResult.Row0;
		Cache.Row1 = AttachInstResult.Row1;
		Cache.Row2 = AttachInstResult.Row2;
	}
}

