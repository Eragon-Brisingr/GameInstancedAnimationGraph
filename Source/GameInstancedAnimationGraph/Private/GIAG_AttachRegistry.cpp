#include "GIAG_AttachRegistry.h"

#include "RenderGraphResources.h"
#include "RHI.h"
#include "RHICommandList.h"

namespace
{
	static TRefCountPtr<FRHIShaderResourceView> CreateTypedFloat4SRV(FRDGPooledBuffer& Buffer)
	{
		check(IsInRenderingThread());

		const FBufferRHIRef RHIBuffer = Buffer.GetRHI();
		check(RHIBuffer.IsValid());
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

		// We publish this buffer as Buffer<float4>.
		return RHICmdList.CreateShaderResourceView(
			RHIBuffer.GetReference(),
			FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Typed)
				.SetFormat(PF_A32B32G32R32F));
	}
}

void FGIAG_NiagaraAttachRegistry::RegisterOrUpdate(
	uint32 BucketId,
	uint32 NumInstances,
	TRefCountPtr<FRDGPooledBuffer> FxTransformBuffer,
	TRefCountPtr<FRDGPooledBuffer> SlotToDenseIndexBuffer,
	TRefCountPtr<FRDGPooledBuffer> SlotGenerationBuffer,
	TRefCountPtr<FRDGPooledBuffer> FxParticleGenBuffer,
	TRefCountPtr<FRDGPooledBuffer> AddListPackedBuffer,
	uint32 SlotTableVersion,
	uint32 AddListVersion,
	uint32 AddListCount)
{
	check(IsInRenderingThread());
	check(BucketId != 0);

	FEntry& Entry = ByBucketId.FindOrAdd(BucketId);

	const bool bSlotToDenseChanged = (SlotToDenseIndexBuffer.IsValid() && Entry.SlotToDenseIndexBuffer != SlotToDenseIndexBuffer);
	const bool bSlotGenChanged = (SlotGenerationBuffer.IsValid() && Entry.SlotGenerationBuffer != SlotGenerationBuffer);
	const bool bFxGenChanged = (FxParticleGenBuffer.IsValid() && Entry.FxParticleGenBuffer != FxParticleGenBuffer);
	const bool bAddListChanged = (AddListPackedBuffer.IsValid() && Entry.AddListPackedBuffer != AddListPackedBuffer);

	Entry.NumInstances = NumInstances;
	Entry.FxTransformBuffer = MoveTemp(FxTransformBuffer);
	Entry.SlotTableVersion = SlotTableVersion;
	Entry.AddListVersion = AddListVersion;
	Entry.AddListCount = AddListCount;
	if (SlotToDenseIndexBuffer.IsValid()) { Entry.SlotToDenseIndexBuffer = MoveTemp(SlotToDenseIndexBuffer); }
	if (SlotGenerationBuffer.IsValid()) { Entry.SlotGenerationBuffer = MoveTemp(SlotGenerationBuffer); }
	if (FxParticleGenBuffer.IsValid()) { Entry.FxParticleGenBuffer = MoveTemp(FxParticleGenBuffer); }
	if (AddListPackedBuffer.IsValid()) { Entry.AddListPackedBuffer = MoveTemp(AddListPackedBuffer); }
}

void FGIAG_NiagaraAttachRegistry::Unregister(uint32 BucketId)
{
	check(IsInRenderingThread());
	check(BucketId != 0);
	ByBucketId.Remove(BucketId);
}

const FGIAG_NiagaraAttachRegistry::FEntry* FGIAG_NiagaraAttachRegistry::Find(uint32 BucketId) const
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	check(BucketId != 0);
	return ByBucketId.Find(BucketId);
}

uint32 FGIAG_NiagaraAttachRegistry::FindNumInstances(uint32 BucketId) const
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	check(BucketId != 0);
	if (const FEntry* Entry = ByBucketId.Find(BucketId))
	{
		return Entry->NumInstances;
	}
	return 0;
}

void FGIAG_NativeAttachRegistry::RegisterOrUpdate(
	uint32 BucketId,
	uint32 NumInstances,
	TRefCountPtr<FRDGPooledBuffer> InstanceOriginBuffer,
	TRefCountPtr<FRDGPooledBuffer> InstanceTransformBuffer)
{
	check(IsInRenderingThread());
	check(BucketId != 0);

	FEntry& Entry = ByBucketId.FindOrAdd(BucketId);

	const bool bOriginChanged = (Entry.InstanceOriginBuffer != InstanceOriginBuffer);
	const bool bTransformChanged = (Entry.InstanceTransformBuffer != InstanceTransformBuffer);

	Entry.NumInstances = NumInstances;
	Entry.InstanceOriginBuffer = MoveTemp(InstanceOriginBuffer);
	Entry.InstanceTransformBuffer = MoveTemp(InstanceTransformBuffer);

	// Instance buffers are always float4 typed (stride == 16).
	if (bOriginChanged || !Entry.InstanceOriginSRV.IsValid())
	{
		Entry.InstanceOriginSRV.SafeRelease();
		if (Entry.InstanceOriginBuffer.IsValid() && Entry.InstanceOriginBuffer->GetRHI() != nullptr)
		{
			Entry.InstanceOriginSRV = CreateTypedFloat4SRV(*Entry.InstanceOriginBuffer);
		}
	}

	if (bTransformChanged || !Entry.InstanceTransformSRV.IsValid())
	{
		Entry.InstanceTransformSRV.SafeRelease();
		if (Entry.InstanceTransformBuffer.IsValid() && Entry.InstanceTransformBuffer->GetRHI() != nullptr)
		{
			Entry.InstanceTransformSRV = CreateTypedFloat4SRV(*Entry.InstanceTransformBuffer);
		}
	}
}

void FGIAG_NativeAttachRegistry::Unregister(uint32 BucketId)
{
	check(IsInRenderingThread());
	check(BucketId != 0);
	ByBucketId.Remove(BucketId);
}

FRHIShaderResourceView* FGIAG_NativeAttachRegistry::FindInstanceOriginSRV(uint32 BucketId) const
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	check(BucketId != 0);
	if (const FEntry* Entry = ByBucketId.Find(BucketId))
	{
		return Entry->InstanceOriginSRV.GetReference();
	}
	return nullptr;
}

FRHIShaderResourceView* FGIAG_NativeAttachRegistry::FindInstanceTransformSRV(uint32 BucketId) const
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	check(BucketId != 0);
	if (const FEntry* Entry = ByBucketId.Find(BucketId))
	{
		return Entry->InstanceTransformSRV.GetReference();
	}
	return nullptr;
}

uint32 FGIAG_NativeAttachRegistry::FindNumInstances(uint32 BucketId) const
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	check(BucketId != 0);
	if (const FEntry* Entry = ByBucketId.Find(BucketId))
	{
		return Entry->NumInstances;
	}
	return 0;
}

