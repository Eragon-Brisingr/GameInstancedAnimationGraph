#include "GIAG_SkinningTransformProviderExtension.h"

#include "GIAG_AnimCommon.h"
#include "GIAG_ProviderData.h"
#include "GIAG_TransformProviderData.h"
#include "GIAG_DebugReadback.h"
#include "GIAG_AttachRegistry.h"
#include "GameInstancedAnimationGraphSubsystem.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RHIGPUReadback.h"
#include "ScenePrivate.h"
#include "SceneExtensions.h"
#include "SkinningSceneExtensionProxy.h"
#include "Animation/Skeleton.h"
#include "HAL/Event.h"
#include "Misc/Crc.h"

IMPLEMENT_SCENE_EXTENSION(FGIAG_SkinningTransformProviderExtension);

namespace
{
	static void UploadStructuredBuffer(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef Dst,
		uint64 DstOffsetBytes,
		const TCHAR* UploadName,
		uint32 BytesPerElement,
		const void* Data,
		uint32 NumElements)
	{
		if (!Dst || !Data || NumElements == 0)
		{
			return;
		}
		const uint64 NumBytes = (uint64)BytesPerElement * (uint64)NumElements;
		FRDGBufferRef Upload = CreateStructuredBuffer(
			GraphBuilder,
			UploadName,
			BytesPerElement,
			NumElements,
			Data,
			NumBytes);
		AddCopyBufferPass(GraphBuilder, Dst, DstOffsetBytes, Upload, 0, NumBytes);
	}


	static FRDGBufferRef CreateOrRegisterExternalBuffer(
		FRDGBuilder& GraphBuilder,
		TRefCountPtr<FRDGPooledBuffer>& External,
		const FRDGBufferDesc& Desc,
		const TCHAR* Name)
	{
		if (External.IsValid() && External->Desc == Desc)
		{
			return GraphBuilder.RegisterExternalBuffer(External, Name);
		}
		FRDGBufferRef NewBuf = GraphBuilder.CreateBuffer(Desc, Name);
		External = GraphBuilder.ConvertToExternalBuffer(NewBuf);
		return NewBuf;
	}

	static FRDGBufferRef CreateOrRegisterExternalBuffer_FromCache(
		FRDGBuilder& GraphBuilder,
		FGIAG_AnimResourceCache& Cache,
		const FGIAG_AnimResourceKey& ShareKey,
		const FRDGBufferDesc& Desc,
		const TCHAR* Name)
	{
		TRefCountPtr<FRDGPooledBuffer>* Found = Cache.Buffers.Find(ShareKey);
		if (Found && Found->IsValid() && (*Found)->Desc == Desc)
		{
			return GraphBuilder.RegisterExternalBuffer(*Found, Name);
		}
		FRDGBufferRef NewBuf = GraphBuilder.CreateBuffer(Desc, Name);
		Cache.Buffers.Add(ShareKey, GraphBuilder.ConvertToExternalBuffer(NewBuf));
		return NewBuf;
	}
}

FGIAG_SkinningTransformProviderExtension::~FGIAG_SkinningTransformProviderExtension()
{
	// Scene invalid when packaged run
	/*if (auto TransformProvider = Scene.GetExtensionPtr<FSkinningTransformProvider>())
	{
		TransformProvider->UnregisterProvider(UGIAG_TransformProviderData::ProviderGuid);
	}*/
}

bool FGIAG_SkinningTransformProviderExtension::ShouldCreateExtension(FScene& InScene)
{
	// Keep it simple: create whenever the renderer is present. We will no-op if the skinning extension isn't available.
	return true;
}

void FGIAG_SkinningTransformProviderExtension::InitExtension(FScene& InScene)
{
	if (InScene.World)
	{
		if (UGameInstancedAnimationGraphSubsystem* Subsys = InScene.World->GetSubsystem<UGameInstancedAnimationGraphSubsystem>())
		{
			SharedResourceBus_RT = Subsys->GetSharedResourceBus();
			AttachBus_RT = Subsys->GetAttachBus();
			AttachReadbackBus_RT = Subsys->GetAttachReadbackBus();
			NiagaraAttachRegistry_RT = Subsys->GetNiagaraAttachRegistry();
			NativeAttachRegistry_RT = Subsys->GetNativeAttachRegistry();
		}
	}

	if (auto TransformProvider = InScene.GetExtensionPtr<FSkinningTransformProvider>())
	{
		TransformProvider->RegisterProvider(
			UGIAG_TransformProviderData::ProviderGuid,
			FSkinningTransformProvider::FOnProvideTransforms::CreateRaw(this, &FGIAG_SkinningTransformProviderExtension::ProvideTransforms),
			false /* UsesSkeletonBatching */
		);
	}
}

class FGIAG_SkinningTransformProviderExtensionRenderer final : public ISceneExtensionRenderer
{
public:
	explicit FGIAG_SkinningTransformProviderExtensionRenderer(FSceneRendererBase& InSceneRenderer, FGIAG_SkinningTransformProviderExtension& InOwner)
		: ISceneExtensionRenderer(InSceneRenderer)
		, Owner(InOwner)
	{}

	void PreRender(FRDGBuilder& GraphBuilder) override
	{
		// Ensure attach registry + output buffers are kept updated even when no skinning provider evaluation runs
		// (e.g., master instance switched to CPU mode).
		Owner.ProcessAttachOpsAndOutputs_RT(GraphBuilder);
	}

private:
	FGIAG_SkinningTransformProviderExtension& Owner;
};

ISceneExtensionRenderer* FGIAG_SkinningTransformProviderExtension::CreateRenderer(FSceneRendererBase& InSceneRenderer, const FEngineShowFlags& /*EngineShowFlags*/)
{
	return new FGIAG_SkinningTransformProviderExtensionRenderer(InSceneRenderer, *this);
}

void FGIAG_SkinningTransformProviderExtension::ProcessAttachOpsAndOutputs_RT(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());

	// ---- RT incremental Niagara-attach ops (published by GT subsystem) ----
	if (AttachBus_RT.IsValid())
	{
		FGIAG_AttachBus::FOp Op;
		while (AttachBus_RT->Dequeue_RenderThread(Op))
		{
			Visit([&](const auto& V)
			{
				using T = std::decay_t<decltype(V)>;

				if constexpr (std::is_same_v<T, FGIAG_AttachBus::FPublishNiagaraMetaOp>)
				{
					const uint32 BucketId = V.BucketId;
					check(BucketId != 0u);

					FAttachBucketRT& Bucket = AttachBuckets_RT.FindOrAdd(BucketId);
					Bucket.BucketKind = FGIAG_AttachBus::EBucketKind::Niagara;
					check(!Bucket.IsNativeInstanceOnly());

					// Slot tables: upload only if provided (event-driven).
					if (V.SlotToDenseIndex.IsValid() && V.SlotGeneration.IsValid() && V.FxParticleGenBySlot.IsValid())
					{
						Bucket.SlotTableVersion = V.SlotTableVersion;

						const int32 SlotCap = V.SlotToDenseIndex->Num();
						check(V.SlotGeneration->Num() == SlotCap);
						check(V.FxParticleGenBySlot->Num() == SlotCap);

						FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), FMath::Max(1, SlotCap));
						FRDGBufferRef SlotToDenseRDG = CreateOrRegisterExternalBuffer(GraphBuilder, Bucket.SlotToDenseIndexBuffer, Desc, TEXT("GIAG_Attach_SlotToDense_External"));
						FRDGBufferRef SlotGenRDG = CreateOrRegisterExternalBuffer(GraphBuilder, Bucket.SlotGenerationBuffer, Desc, TEXT("GIAG_Attach_SlotGen_External"));
						FRDGBufferRef FxGenRDG = CreateOrRegisterExternalBuffer(GraphBuilder, Bucket.FxParticleGenBuffer, Desc, TEXT("GIAG_Attach_FxGen_External"));

						UploadStructuredBuffer(GraphBuilder, SlotToDenseRDG, 0, TEXT("GIAG_Attach_Upload_SlotToDense"), sizeof(int32), V.SlotToDenseIndex->GetData(), (uint32)SlotCap);
						UploadStructuredBuffer(GraphBuilder, SlotGenRDG, 0, TEXT("GIAG_Attach_Upload_SlotGen"), sizeof(int32), V.SlotGeneration->GetData(), (uint32)SlotCap);
						UploadStructuredBuffer(GraphBuilder, FxGenRDG, 0, TEXT("GIAG_Attach_Upload_FxGen"), sizeof(int32), V.FxParticleGenBySlot->GetData(), (uint32)SlotCap);

						GraphBuilder.SetBufferAccessFinal(SlotToDenseRDG, ERHIAccess::SRVMask);
						GraphBuilder.SetBufferAccessFinal(SlotGenRDG, ERHIAccess::SRVMask);
						GraphBuilder.SetBufferAccessFinal(FxGenRDG, ERHIAccess::SRVMask);
					}

					// Add list: upload only if provided (versioned).
					Bucket.AddListVersion = V.AddListVersion;
					Bucket.AddListCount = V.AddListCount;
					if (V.AddListPacked.IsValid())
					{
						const int32 NumAddItems = V.AddListPacked->Num();
						check((uint32)NumAddItems == V.AddListCount);
						FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), FMath::Max(1, NumAddItems));
						FRDGBufferRef AddRDG = CreateOrRegisterExternalBuffer(GraphBuilder, Bucket.AddListPackedBuffer, Desc, TEXT("GIAG_Attach_AddListPacked_External"));
						UploadStructuredBuffer(GraphBuilder, AddRDG, 0, TEXT("GIAG_Attach_Upload_AddListPacked"), sizeof(int32), V.AddListPacked->GetData(), (uint32)NumAddItems);
						GraphBuilder.SetBufferAccessFinal(AddRDG, ERHIAccess::SRVMask);

						// Defer VM-visible snapshot publish until after we have updated the registry for this bucket.
						Bucket.bPendingVmPublishAddList = true;
						Bucket.PendingVmPublishAddListVersion = V.AddListVersion;
						Bucket.PendingVmPublishAddListCount = V.AddListCount;
					}
				}
				else if constexpr (std::is_same_v<T, FGIAG_AttachBus::FAttachAddOp>)
				{
					const uint32 BucketId = V.BucketId;
					check(BucketId != 0u);

					FAttachBucketRT& Bucket = AttachBuckets_RT.FindOrAdd(BucketId);
					Bucket.BucketKind = V.BucketKind;
					Bucket.NumInstances += 1;

					const FAttachGroupKey GK{ V.State, BucketId };
					FAttachGroupRT& G = AttachGroupsByStateBucket_RT.FindOrAdd(GK);

					const int32 NewIndex = G.CPU.Num();
					G.IndexByAttachSlot.Add(V.AttachSlot, NewIndex);
					G.CPU.Add(V.Desc);
					G.bDirty = true;
				}
				else if constexpr (std::is_same_v<T, FGIAG_AttachBus::FAttachUpdateOp>)
				{
					const uint32 BucketId = V.BucketId;
					check(BucketId != 0u);

					FAttachBucketRT& Bucket = AttachBuckets_RT.FindOrAdd(BucketId);
					Bucket.BucketKind = V.BucketKind;

					const FAttachGroupKey GK{ V.State, BucketId };
					FAttachGroupRT& G = AttachGroupsByStateBucket_RT.FindOrAdd(GK);
					const int32* IdxPtr = G.IndexByAttachSlot.Find(V.AttachSlot);
					check(IdxPtr);
					G.CPU[*IdxPtr] = V.Desc;
					G.bDirty = true;
				}
				else if constexpr (std::is_same_v<T, FGIAG_AttachBus::FAttachRemoveOp>)
				{
					const uint32 BucketId = V.BucketId;
					check(BucketId != 0u);

					FAttachBucketRT& Bucket = AttachBuckets_RT.FindOrAdd(BucketId);
					Bucket.BucketKind = V.BucketKind;
					Bucket.NumInstances = (Bucket.NumInstances > 0) ? (Bucket.NumInstances - 1) : 0;

					const FAttachGroupKey GK{ V.State, BucketId };
					FAttachGroupRT& G = AttachGroupsByStateBucket_RT.FindOrAdd(GK);
					const int32* RemoveIdxPtr = G.IndexByAttachSlot.Find(V.AttachSlot);
					check(RemoveIdxPtr);

					const int32 RemoveIdx = *RemoveIdxPtr;
					const int32 LastIdx = G.CPU.Num() - 1;
					if (RemoveIdx != LastIdx)
					{
						G.CPU[RemoveIdx] = G.CPU[LastIdx];
						// Recover swapped slot by searching map value (small; event-driven).
						uint32 SwappedSlot = 0;
						for (const auto& Pair : G.IndexByAttachSlot)
						{
							if (Pair.Value == LastIdx)
							{
								SwappedSlot = Pair.Key;
								break;
							}
						}
						G.IndexByAttachSlot.FindChecked(SwappedSlot) = RemoveIdx;
					}
					G.CPU.Pop(EAllowShrinking::No);
					G.IndexByAttachSlot.Remove(V.AttachSlot);
					G.bDirty = true;
				}
				else if constexpr (std::is_same_v<T, FGIAG_AttachBus::FWriteFxTransformOp>)
				{
					const uint32 BucketId = V.BucketId;
					check(BucketId != 0u);

					FAttachBucketRT& Bucket = AttachBuckets_RT.FindOrAdd(BucketId);
					Bucket.BucketKind = FGIAG_AttachBus::EBucketKind::Niagara;

					FAttachBucketRT::FPendingWriteTransform W;
					W.OutputIndex = V.OutputIndex;
					W.FxTransform = V.TransformWS;
					Bucket.PendingWritesTransform.Add(W);
				}
				else if constexpr (std::is_same_v<T, FGIAG_AttachBus::FWriteInstanceOp>)
				{
					const uint32 BucketId = V.BucketId;
					check(BucketId != 0u);

					FAttachBucketRT& Bucket = AttachBuckets_RT.FindOrAdd(BucketId);
					Bucket.BucketKind = FGIAG_AttachBus::EBucketKind::Native;

					FAttachBucketRT::FPendingWriteTransform W;
					W.OutputIndex = V.OutputIndex;
					W.FxTransform = V.TransformWS;
					Bucket.PendingWritesInstance.Add(W);
				}
			}, Op);
		}
	}

	// Ensure per-bucket output buffers exist and publish to registry.
	for (auto It = AttachBuckets_RT.CreateIterator(); It; ++It)
	{
		const uint32 BucketId = It.Key();
		FAttachBucketRT& Bucket = It.Value();
		const uint32 Num = Bucket.NumInstances;

		if (Num == 0)
		{
			Bucket.FxTransformBuffer.SafeRelease();
			Bucket.InstanceOriginBuffer.SafeRelease();
			Bucket.InstanceTransformBuffer.SafeRelease();
			Bucket.PendingWritesTransform.Reset();
			Bucket.PendingWritesInstance.Reset();
			if (Bucket.IsNativeInstanceOnly())
			{
				NativeAttachRegistry_RT->Unregister(BucketId);
			}
			else
			{
				NiagaraAttachRegistry_RT->Unregister(BucketId);
			}
			It.RemoveCurrent();
			continue;
		}

		const uint32 NumFloat4 = Num * 3u;

		if (Bucket.IsNativeInstanceOnly())
		{
			// Ensure we don't keep Niagara-only buffers alive for native buckets.
			Bucket.FxTransformBuffer.SafeRelease();
			Bucket.PendingWritesTransform.Reset();

			// Native bucket: only instance buffers (no FxTransform).
			FRDGBufferRef InstanceOriginRDG = nullptr;
			FRDGBufferRef InstanceTransformRDG = nullptr;
			{
				// Never shrink: keep prior capacity to avoid remove-churn realloc (which would invalidate contents for a frame).
				uint32 PrevCapInstances = 0u;
				if (Bucket.InstanceOriginBuffer.IsValid())
				{
					PrevCapInstances = FMath::Max<uint32>(PrevCapInstances, (uint32)Bucket.InstanceOriginBuffer->Desc.NumElements);
				}
				if (Bucket.InstanceTransformBuffer.IsValid())
				{
					PrevCapInstances = FMath::Max<uint32>(PrevCapInstances, (uint32)(Bucket.InstanceTransformBuffer->Desc.NumElements / 3u));
				}

				const uint32 RequiredCapInstances = FMath::RoundUpToPowerOfTwo(FMath::Max(1u, Num));
				const uint32 CapInstances = FMath::Max(PrevCapInstances, RequiredCapInstances);
				const uint32 CapFloat4 = CapInstances * 3u;

				FRDGBufferDesc OriginDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), CapInstances);
				OriginDesc.Usage |= (BUF_ShaderResource | BUF_UnorderedAccess | BUF_VertexBuffer);

				FRDGBufferDesc TransformDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), CapFloat4);
				TransformDesc.Usage |= (BUF_ShaderResource | BUF_UnorderedAccess | BUF_VertexBuffer);

				const TRefCountPtr<FRDGPooledBuffer> OldOrigin = Bucket.InstanceOriginBuffer;
				const TRefCountPtr<FRDGPooledBuffer> OldTransform = Bucket.InstanceTransformBuffer;

				InstanceOriginRDG = CreateOrRegisterExternalBuffer(
					GraphBuilder,
					Bucket.InstanceOriginBuffer,
					OriginDesc,
					TEXT("GIAG_Attach_InstanceOrigin_External"));

				InstanceTransformRDG = CreateOrRegisterExternalBuffer(
					GraphBuilder,
					Bucket.InstanceTransformBuffer,
					TransformDesc,
					TEXT("GIAG_Attach_InstanceTransform_External"));

				// Preserve old contents on growth.
				if (OldOrigin.IsValid() && OldOrigin != Bucket.InstanceOriginBuffer && InstanceOriginRDG != nullptr)
				{
					const uint32 OldElems = (uint32)OldOrigin->Desc.NumElements;
					const uint32 NewElems = (uint32)Bucket.InstanceOriginBuffer->Desc.NumElements;
					const uint32 CopyElems = FMath::Min(OldElems, NewElems);
					if (CopyElems > 0)
					{
						FRDGBufferRef SrcRDG = GraphBuilder.RegisterExternalBuffer(OldOrigin, TEXT("GIAG_Attach_InstanceOrigin_Old_External"));
						const uint64 NumBytes = (uint64)CopyElems * (uint64)sizeof(FVector4f);
						AddCopyBufferPass(GraphBuilder, InstanceOriginRDG, 0, SrcRDG, 0, NumBytes);
					}
				}

				if (OldTransform.IsValid() && OldTransform != Bucket.InstanceTransformBuffer && InstanceTransformRDG != nullptr)
				{
					const uint32 OldElems = (uint32)OldTransform->Desc.NumElements;
					const uint32 NewElems = (uint32)Bucket.InstanceTransformBuffer->Desc.NumElements;
					const uint32 CopyElems = FMath::Min(OldElems, NewElems);
					if (CopyElems > 0)
					{
						FRDGBufferRef SrcRDG = GraphBuilder.RegisterExternalBuffer(OldTransform, TEXT("GIAG_Attach_InstanceTransform_Old_External"));
						const uint64 NumBytes = (uint64)CopyElems * (uint64)sizeof(FVector4f);
						AddCopyBufferPass(GraphBuilder, InstanceTransformRDG, 0, SrcRDG, 0, NumBytes);
					}
				}
			}

			NativeAttachRegistry_RT->RegisterOrUpdate(
				BucketId,
				Num,
				Bucket.InstanceOriginBuffer,
				Bucket.InstanceTransformBuffer);

			// Ensure external buffers end the graph in SRV state for cross-system reads (native renderer, debug readbacks).
			if (InstanceOriginRDG)
			{
				GraphBuilder.SetBufferAccessFinal(InstanceOriginRDG, ERHIAccess::SRVMask);
			}
			if (InstanceTransformRDG)
			{
				GraphBuilder.SetBufferAccessFinal(InstanceTransformRDG, ERHIAccess::SRVMask);
			}

			// Apply pending direct writes (CPU mode sync / hiding) into instance buffers.
			if (Bucket.PendingWritesInstance.Num() > 0 && InstanceOriginRDG != nullptr && InstanceTransformRDG != nullptr)
			{
				TMap<uint32, FGIAG_Transform> LatestByOutput;
				LatestByOutput.Reserve(Bucket.PendingWritesInstance.Num());
				for (const FAttachBucketRT::FPendingWriteTransform& W : Bucket.PendingWritesInstance)
				{
					LatestByOutput.Add(W.OutputIndex, W.FxTransform);
				}

				TArray<uint32> OutputIndicesCPU;
				TArray<FGIAG_Transform> ValuesCPU;
				OutputIndicesCPU.Reserve(LatestByOutput.Num());
				ValuesCPU.Reserve(LatestByOutput.Num());
				for (const auto& Pair : LatestByOutput)
				{
					OutputIndicesCPU.Add(Pair.Key);
					ValuesCPU.Add(Pair.Value);
				}

				FRDGBufferRef IndicesRDG = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1, OutputIndicesCPU.Num())),
					TEXT("GIAG_Attach_Instance_ScatterIndices"));
				FRDGBufferRef ValuesRDG = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), FMath::Max(1, ValuesCPU.Num())),
					TEXT("GIAG_Attach_Instance_ScatterValues"));

				GraphBuilder.QueueBufferUpload(IndicesRDG, OutputIndicesCPU.GetData(), sizeof(uint32) * OutputIndicesCPU.Num(), ERDGInitialDataFlags::None);
				GraphBuilder.QueueBufferUpload(ValuesRDG, ValuesCPU.GetData(), sizeof(FGIAG_Transform) * ValuesCPU.Num(), ERDGInitialDataFlags::None);

				GIAG::FScatterWriteInstanceBuffersPassParams ScatterParams;
				ScatterParams.NumWrites = (uint32)OutputIndicesCPU.Num();
				ScatterParams.OutputIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndicesRDG, PF_R32_UINT));
				ScatterParams.ValuesTransform = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ValuesRDG));
				ScatterParams.RW_InstanceOrigin = GraphBuilder.CreateUAV(InstanceOriginRDG, PF_A32B32G32R32F);
				ScatterParams.RW_InstanceTransform = GraphBuilder.CreateUAV(InstanceTransformRDG, PF_A32B32G32R32F);
				GIAG::AddScatterWriteInstanceBuffersPasses(GraphBuilder, ScatterParams);

				Bucket.PendingWritesInstance.Reset();
			}
		}
		else
		{
			// Ensure we don't keep native-only buffers alive for Niagara buckets.
			Bucket.InstanceOriginBuffer.SafeRelease();
			Bucket.InstanceTransformBuffer.SafeRelease();
			Bucket.PendingWritesInstance.Reset();

			// Niagara bucket: FxTransform only.
			FRDGBufferRef OutRDG = CreateOrRegisterExternalBuffer(
				GraphBuilder,
				Bucket.FxTransformBuffer,
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), FMath::Max(1u, Num)),
				TEXT("GIAG_Attach_FxTransform_External"));

			NiagaraAttachRegistry_RT->RegisterOrUpdate(
				BucketId,
				Num,
				Bucket.FxTransformBuffer,
				Bucket.SlotToDenseIndexBuffer,
				Bucket.SlotGenerationBuffer,
				Bucket.FxParticleGenBuffer,
				Bucket.AddListPackedBuffer,
				Bucket.SlotTableVersion,
				Bucket.AddListVersion,
				Bucket.AddListCount);

			// Now that buffers are ensured and the registry points at the latest pooled buffers,
			// publish the VM-visible spawn snapshot (version+count) for this bucket.
			if (Bucket.bPendingVmPublishAddList)
			{
				AttachBus_RT->NiagaraVm_UpdateAddList_RenderThread(
					BucketId,
					Bucket.PendingVmPublishAddListVersion,
					Bucket.PendingVmPublishAddListCount);
				Bucket.bPendingVmPublishAddList = false;
			}

			// Ensure the external buffer ends the graph in SRV state for cross-system reads (e.g. Niagara).
			GraphBuilder.SetBufferAccessFinal(OutRDG, ERHIAccess::SRVMask);

			// Apply pending direct writes (CPU mode sync / hiding) into FxTransform.
			if (Bucket.PendingWritesTransform.Num() > 0)
			{
				TMap<uint32, FGIAG_Transform> LatestByOutput;
				LatestByOutput.Reserve(Bucket.PendingWritesTransform.Num());
				for (const FAttachBucketRT::FPendingWriteTransform& W : Bucket.PendingWritesTransform)
				{
					LatestByOutput.Add(W.OutputIndex, W.FxTransform);
				}

				TArray<uint32> OutputIndicesCPU;
				TArray<FGIAG_Transform> ValuesCPU;
				OutputIndicesCPU.Reserve(LatestByOutput.Num());
				ValuesCPU.Reserve(LatestByOutput.Num());
				for (const auto& Pair : LatestByOutput)
				{
					OutputIndicesCPU.Add(Pair.Key);
					ValuesCPU.Add(Pair.Value);
				}

				FRDGBufferRef IndicesRDG = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1, OutputIndicesCPU.Num())),
					TEXT("GIAG_Attach_FxTransform_ScatterIndices"));
				FRDGBufferRef ValuesRDG = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), FMath::Max(1, ValuesCPU.Num())),
					TEXT("GIAG_Attach_FxTransform_ScatterValues"));

				GraphBuilder.QueueBufferUpload(IndicesRDG, OutputIndicesCPU.GetData(), sizeof(uint32) * OutputIndicesCPU.Num(), ERDGInitialDataFlags::None);
				GraphBuilder.QueueBufferUpload(ValuesRDG, ValuesCPU.GetData(), sizeof(FGIAG_Transform) * ValuesCPU.Num(), ERDGInitialDataFlags::None);

				GIAG::FScatterWriteFxTransformPassParams ScatterParams;
				ScatterParams.NumWrites = (uint32)OutputIndicesCPU.Num();
				ScatterParams.OutputIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndicesRDG, PF_R32_UINT));
				ScatterParams.ValuesTransform = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ValuesRDG));
				ScatterParams.RW_FxTransform = GraphBuilder.CreateUAV(OutRDG);
				GIAG::AddScatterWriteFxTransformPasses(GraphBuilder, ScatterParams);

				Bucket.PendingWritesTransform.Reset();
			}
		}

	}

	// Ensure per-group attachment descriptor upload buffers exist and are uploaded when dirty.
	for (auto It = AttachGroupsByStateBucket_RT.CreateIterator(); It; ++It)
	{
		FAttachGroupRT& AttachGroup = It.Value();
		const uint32 Num = (uint32)AttachGroup.CPU.Num();
		if (Num == 0)
		{
			AttachGroup.DescUploadBuffer.SafeRelease();
			It.RemoveCurrent();
			continue;
		}

		FRDGBufferRef DescRDG = CreateOrRegisterExternalBuffer(
			GraphBuilder,
			AttachGroup.DescUploadBuffer,
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FAttachDescPacked), FMath::Max(1u, Num)),
			TEXT("GIAG_Attach_Desc_External"));

		if (AttachGroup.bDirty)
		{
			GraphBuilder.QueueBufferUpload(
				DescRDG,
				AttachGroup.CPU.GetData(),
				(uint32)(sizeof(FAttachDescPacked) * (SIZE_T)AttachGroup.CPU.Num()),
				ERDGInitialDataFlags::None);
			AttachGroup.bDirty = false;
		}
	}
}

struct FFollowerGroupKey
{
	const FGIAG_TransformProviderState* MasterState = nullptr;
	FName FollowMeshName;
	bool operator==(const FFollowerGroupKey&) const = default;
	friend uint32 GetTypeHash(const FFollowerGroupKey& K)
	{
		return HashCombine(::GetTypeHash((uintptr_t)K.MasterState), GetTypeHash(K.FollowMeshName));
	}
};
struct FFollowerGroupData
{
	uint32 NumBones = 0;
	uint32 SrcNumBones = 0;
	uint32 MaxTransformCount = 0;
	const uint32* BoneRemap = nullptr;
	// Per-DstIndex destination byte offset in TransformBuffer (Cur, plus Prev when EDirtyBoneTransforms::Previous fires).
	TArray<uint32> DstInfos;
};

void FGIAG_SkinningTransformProviderExtension::ProvideTransforms(FSkinningTransformProvider::FProviderContext& Context)
{
	checkf(Context.TransformBuffer != nullptr, TEXT("GIAG: expected TransformBuffer in provider context."));

	// ---- RT global optional resource upload prepass (incremental, world published -> per-scene cache) ----
	if (SharedResourceBus_RT.IsValid())
	{
		FGIAG_AnimGraphResourceUploadRun Run;
		while (SharedResourceBus_RT->Dequeue_RenderThread(Run))
		{
			const FGIAG_AnimResourceRequest& Req = Run.Request;
			if (Req.ShareKey.IsNone())
			{
				continue;
			}
			check(Req.Layout.Kind == EGIAG_AnimResourceKind::Buffer);
			check(Req.Layout.StrideBytes != 0);
			check(Req.Layout.NumElements != 0);
			check(Run.Bytes.IsValid());

			const uint64 ExpectedBytes = (uint64)Req.Layout.StrideBytes * (uint64)Req.Layout.NumElements;
			check((uint64)Run.Bytes->Num() >= ExpectedBytes);

			const FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(Req.Layout.StrideBytes, Req.Layout.NumElements);

			// Immutable by contract: if already present with same layout, skip.
			if (TRefCountPtr<FRDGPooledBuffer>* Found = AnimResourceCache_RT.Buffers.Find(Req.ShareKey))
			{
				if (Found->IsValid() && (*Found)->Desc == Desc)
				{
					continue;
				}
			}

			FRDGBufferRef ResourceRDG = CreateOrRegisterExternalBuffer_FromCache(
				Context.GraphBuilder,
				AnimResourceCache_RT,
				Req.ShareKey,
				Desc,
				TEXT("GIAG_AG_OptionalResource"));

			UploadStructuredBuffer(
				Context.GraphBuilder,
				ResourceRDG,
				0,
				TEXT("GIAG_AG_UploadOptionalResource"),
				Req.Layout.StrideBytes,
				Run.Bytes->GetData(),
				Req.Layout.NumElements);
		}
	}

	// Drain completed debug readbacks (RT -> GT).
	for (int32 i = PendingLocalPoseReadbacks_RT.Num() - 1; i >= 0; --i)
	{
		FPendingLocalPoseReadback& Pending = PendingLocalPoseReadbacks_RT[i];
		if (!Pending.Readback || !Pending.Readback->IsReady())
		{
			continue;
		}

		void* Ptr = Pending.Readback->Lock(Pending.NumBytes);
		check(Ptr);

		FGIAG_LocalPoseReadbackResult Result;
		Result.RecordIndex = Pending.RecordIndex;
		Result.SerialNumber = Pending.SerialNumber;
		Result.CpuRequestFrame = Pending.CpuRequestFrame;
		Result.NumBones = Pending.NumBones;
		Result.LocalPoseTRS.SetNumUninitialized((int32)(Pending.NumBytes / sizeof(FGIAG_BoneTRS)));
		FMemory::Memcpy(Result.LocalPoseTRS.GetData(), Ptr, Pending.NumBytes);

		Pending.Readback->Unlock();

		GIAG::DebugReadback::EnqueueLocalPose(MoveTemp(Result));
		PendingLocalPoseReadbacks_RT.RemoveAtSwap(i, EAllowShrinking::No);
	}

	// Drain completed NeedNodeBits readbacks (RT -> GT).
	for (int32 i = PendingNeedNodeBitsReadbacks_RT.Num() - 1; i >= 0; --i)
	{
		FPendingNeedNodeBitsReadback& Pending = PendingNeedNodeBitsReadbacks_RT[i];
		if (!Pending.Readback || !Pending.Readback->IsReady())
		{
			continue;
		}

		void* Ptr = Pending.Readback->Lock(Pending.NumBytes);
		check(Ptr);

		FGIAG_NeedNodeBitsReadbackResult Result;
		Result.RecordIndex = Pending.RecordIndex;
		Result.SerialNumber = Pending.SerialNumber;
		Result.CpuRequestFrame = Pending.CpuRequestFrame;
		Result.SlotIndex = Pending.SlotIndex;
		Result.NumNodes = Pending.NumNodes;
		Result.WordsPerSlot = Pending.WordsPerSlot;
		Result.Words.SetNumUninitialized((int32)Pending.WordsPerSlot);
		FMemory::Memcpy(Result.Words.GetData(), Ptr, Pending.NumBytes);

		Pending.Readback->Unlock();

		GIAG::DebugReadback::EnqueueNeedNodeBits(MoveTemp(Result));
		PendingNeedNodeBitsReadbacks_RT.RemoveAtSwap(i, EAllowShrinking::No);
	}

	// Drain completed attach FxTransform readbacks (RT -> GT).
	for (int32 i = PendingAttachFxTransformReadbacks_RT.Num() - 1; i >= 0; --i)
	{
		FPendingAttachFxTransformReadback& Pending = PendingAttachFxTransformReadbacks_RT[i];
		if (!Pending.Readback || !Pending.Readback->IsReady())
		{
			continue;
		}

		void* Ptr = Pending.Readback->Lock(Pending.NumBytes);
		check(Ptr);

		const FGIAG_Transform* Values = reinterpret_cast<const FGIAG_Transform*>(Ptr);
		FGIAG_AttachFxTransformReadbackResult Result;
		Result.BucketId = Pending.BucketId;
		Result.OutputIndex = Pending.OutputIndex;
		Result.CpuRequestFrame = Pending.CpuRequestFrame;
		Result.FxTransform = (Values != nullptr) ? Values[0] : FGIAG_Transform::Identity;

		Pending.Readback->Unlock();

		GIAG::DebugReadback::EnqueueAttachFxTransform(MoveTemp(Result));
		PendingAttachFxTransformReadbacks_RT.RemoveAtSwap(i, EAllowShrinking::No);
	}

	// Drain completed attach instance buffers readbacks (RT -> GT).
	for (int32 i = PendingAttachInstanceBuffersReadbacks_RT.Num() - 1; i >= 0; --i)
	{
		FPendingAttachInstanceBuffersReadback& Pending = PendingAttachInstanceBuffersReadbacks_RT[i];
		if (!Pending.Readback || !Pending.Readback->IsReady())
		{
			continue;
		}

		void* Ptr = Pending.Readback->Lock(Pending.NumBytes);
		check(Ptr);

		const FVector4f* Values = reinterpret_cast<const FVector4f*>(Ptr); // [0]=Origin, [1..3]=Rows
		FGIAG_AttachInstanceBuffersReadbackResult Result;
		Result.BucketId = Pending.BucketId;
		Result.OutputIndex = Pending.OutputIndex;
		Result.CpuRequestFrame = Pending.CpuRequestFrame;
		Result.Origin = FVector3f(Values[0].X, Values[0].Y, Values[0].Z);
		Result.Row0 = FVector3f(Values[1].X, Values[1].Y, Values[1].Z);
		Result.Row1 = FVector3f(Values[2].X, Values[2].Y, Values[2].Z);
		Result.Row2 = FVector3f(Values[3].X, Values[3].Y, Values[3].Z);

		Pending.Readback->Unlock();

		GIAG::DebugReadback::EnqueueAttachInstanceBuffers(MoveTemp(Result));
		PendingAttachInstanceBuffersReadbacks_RT.RemoveAtSwap(i, EAllowShrinking::No);
	}

	// Pass 1: group indirections by State.
	struct FMasterGroup
	{
		FGIAG_TransformProviderState* State = nullptr;
		uint32 BaseTransformOffset = 0;
		uint32 PreviousTransformOffset = 0;
		uint32 MaxTransformCount = 0;
		EDirtyBoneTransforms DirtyBoneTransforms = EDirtyBoneTransforms::None;
	};
	TMap<FGIAG_TransformProviderState*, FMasterGroup> MasterGroupsByState;

	TMap<FFollowerGroupKey, FFollowerGroupData> FollowerGroups_RT;

	for (const FSkinningTransformProvider::FProviderIndirection& Ind : Context.Indirections)
	{
		checkf((int32)Ind.Index >= 0 && (int32)Ind.Index < Context.Proxies.Num(), TEXT("GIAG: invalid proxy index=%d."), (int32)Ind.Index);

		FSkinningSceneExtensionProxy* SceneProxyBase = Context.Proxies[(int32)Ind.Index];
		if (!SceneProxyBase)
		{
			continue;
		}

		FInstancedSkinningSceneExtensionProxy* SceneProxy = static_cast<FInstancedSkinningSceneExtensionProxy*>(SceneProxyBase);
		FTransformProviderRenderProxy* ProviderProxyBase = SceneProxy->GetTransformProviderProxy();
		if (!ProviderProxyBase)
		{
			continue;
		}

		FGIAG_TransformProviderRenderProxy* OurProxy = static_cast<FGIAG_TransformProviderRenderProxy*>(ProviderProxyBase);
		const FGIAG_ProviderData& Data = OurProxy->GetData();
		checkf(Data.SelfState != nullptr, TEXT("GIAG: ProviderData missing SelfState."));

		// Drain any GT-queued capacity change before we route this Indirection to the master/follower
		// pass. Order matters: when shrinking we must dispatch the slot-compaction CS BEFORE calling
		// SetUniqueAnimationCount on the engine ExtensionProxy — the engine's per-frame polling on the
		// next pre-update sees the new count, allocates a smaller span, and dispatches its own copy
		// pass which preserves the [0, NewCap) range that we just compacted.
		// (Grow path has empty PendingSlotMoves; only the SetUniqueAnimationCount call runs and the
		//  engine-side resize-and-copy preserves Cur+Prev for free.)
		auto& PendingChange = Data.SelfState->PendingCapacityChange_RT;
		if (PendingChange.PendingNewCap > 0)
		{
			if (PendingChange.PendingSlotMoves.Num() > 0)
			{
				const uint32 NumMoves = (uint32)PendingChange.PendingSlotMoves.Num();
				FRDGBufferRef MoveBuf = CreateStructuredBuffer(
					Context.GraphBuilder,
					TEXT("GIAG_BucketCompaction_SlotMoves"),
					sizeof(FGIAG_TransformProviderState::FGIAG_PendingCapacityChange::FSlotMove),
					FMath::Max<uint32>(1u, NumMoves),
					PendingChange.PendingSlotMoves.GetData(),
					(uint64)sizeof(FGIAG_TransformProviderState::FGIAG_PendingCapacityChange::FSlotMove) * (uint64)NumMoves);

				GIAG::FBucketCompactionPassParams CompactParams;
				CompactParams.NumMoves            = NumMoves;
				CompactParams.MaxTransformCount   = SceneProxy->GetMaxBoneTransformCount();
				// Span base = min(Cur, Prev) byte offset (the smaller of the two equals TransformBufferOffset
				// since one of CurrentTransformSlot * MaxTransformCount is 0 each frame).
				CompactParams.BaseSpanOffsetBytes = FMath::Min(Ind.CurrentTransformOffset, Ind.PreviousTransformOffset);
				CompactParams.SlotMoves           = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(MoveBuf));
				CompactParams.TransformBuffer     = Context.TransformBuffer;
				GIAG::AddBucketCompactionPass(Context.GraphBuilder, CompactParams);
			}

			if (FInstancedSkinningSceneExtensionProxy* EngineProxy = OurProxy->GetEngineExtensionProxy())
			{
				EngineProxy->SetUniqueAnimationCount((uint32)PendingChange.PendingNewCap);
			}
			PendingChange.PendingNewCap = 0;
			PendingChange.PendingSlotMoves.Reset();
		}

		if (Data.Mode == EGIAG_TransformProviderMode::FollowerCopyOrRemap)
		{
			checkf(Data.NumBones > 0 && Data.SrcNumBones > 0, TEXT("GIAG: invalid follower bone counts."));
			checkf(Data.MasterState != nullptr, TEXT("GIAG: follower provider missing MasterState."));
			checkf(Data.AnimationSlotCount > 0, TEXT("GIAG: invalid follower AnimationSlotCount=0."));

			FFollowerGroupData& Group = FollowerGroups_RT.FindOrAdd(FFollowerGroupKey{ Data.MasterState, Data.FollowMeshName });
			Group.NumBones = Data.NumBones;
			Group.SrcNumBones = Data.SrcNumBones;
			Group.BoneRemap = Data.BoneRemap;
			Group.MaxTransformCount = SceneProxy->GetMaxBoneTransformCount();
			// Always write Current. Append a Previous DstInfo (Prev = Current) only when the engine flags
			// EDirtyBoneTransforms::Previous (first frame / re-bind); otherwise the engine's per-frame
			// Cur/Prev region rotation already places last frame's Cur at PreviousTransformOffset.
			Group.DstInfos.Add(Ind.CurrentTransformOffset);
			if (EnumHasAnyFlags(Ind.DirtyBoneTransforms, EDirtyBoneTransforms::Previous))
			{
				Group.DstInfos.Add(Ind.PreviousTransformOffset);
			}
			continue;
		}

		checkf(Data.Mode == EGIAG_TransformProviderMode::MasterEvaluate, TEXT("GIAG: unexpected provider mode (%d)."), (int32)Data.Mode);
		// Master: expect at most one indirection per state (one bucket = one ISKMC).
		FMasterGroup& MasterGroup = MasterGroupsByState.FindOrAdd(Data.SelfState);
		MasterGroup.State = Data.SelfState;
		MasterGroup.BaseTransformOffset = Ind.CurrentTransformOffset;
		MasterGroup.PreviousTransformOffset = Ind.PreviousTransformOffset;
		MasterGroup.MaxTransformCount = SceneProxy->GetMaxBoneTransformCount();
		MasterGroup.DirtyBoneTransforms = Ind.DirtyBoneTransforms;
	}

	// Pass 2: drain payloads, process AnimLibrary uploads, compute animation once, dispatch followers.
	for (auto& MasterKV : MasterGroupsByState)
	{
		FGIAG_TransformProviderState* State = MasterKV.Key;
		FMasterGroup& MasterGroup = MasterKV.Value;

		// Phase A: drain all queued payloads; process AnimLibrary uploads from each; keep last for computation.
		const FGIAG_AnimGraphCompiledData* LastCompiled = nullptr;
		FGIAG_AnimGraphRunParams LastParams;
		FGIAG_AnimGraphUploads LastUploads;
		bool bHasPayload = false;

		FGIAG_RenderPayload Payload;
		while (State->DequeuePayload_RenderThread(Payload))
		{
			RDG_EVENT_SCOPE(Context.GraphBuilder, "GIAG Upload Resource");

			FGIAG_AnimGraphRunParams Params = MoveTemp(Payload.Params);
			Params.OutputTransformBuffer = Context.TransformBuffer;
			Params.BaseTransformOffset = MasterGroup.BaseTransformOffset;
			Params.BasePreviousTransformOffset = MasterGroup.PreviousTransformOffset;
			// UE alternates Cur/Prev region offsets each frame, so last frame's writes already
			// live at PreviousTransformOffset. We only need to write Prev when the engine flagged
			// this slot for re-init via EDirtyBoneTransforms::Previous (first frame after re-bind /
			// cap change / mode switch). The shader's bWritePreviousTransforms is uint32.
			Params.bWritePreviousTransforms = EnumHasAnyFlags(MasterGroup.DirtyBoneTransforms, EDirtyBoneTransforms::Previous);
			Params.MaxTransformCount = MasterGroup.MaxTransformCount;

			FAnimLibraryRTCacheEntry& AnimLib = AnimLibraryBySkeleton_RT.FindOrAdd(FObjectKey(Params.Skeleton));
			if (Params.AnimLibraryUpload.IsValid() && Params.AnimLibraryUpload->Version > AnimLib.Version)
			{
				const FGIAG_AnimLibraryUploadData& Upload = *Params.AnimLibraryUpload;
				if (Upload.NumClips > 0 && Upload.AnimTRSCapacity > 0 && Upload.NumBones > 0)
				{
					AnimLib.Buffers.NumClips = (uint32)Upload.NumClips;
					AnimLib.Buffers.NumBones = Upload.NumBones;
					AnimLib.Buffers.AnimTRSNum = (uint32)Upload.AnimTRSCapacity;
					AnimLib.Buffers.AnimTRSCapacity = (uint32)FMath::RoundUpToPowerOfTwo(FMath::Max<uint32>(1, (uint32)Upload.AnimTRSCapacity));

					const uint32 ClipCountForAlloc = FMath::Max<uint32>(1, (uint32)Upload.NumClips);
					const uint32 TRSCountForAlloc = FMath::Max<uint32>(1, (uint32)FMath::RoundUpToPowerOfTwo(FMath::Max<uint32>(1, Upload.AnimTRSCapacity)));
					const uint32 RefPoseCountForAlloc = FMath::Max<uint32>(1, Upload.NumBones);

					// Keep old buffers around to preserve data across capacity growth (non-repack path).
					const TRefCountPtr<FRDGPooledBuffer> OldClipMetas = AnimLib.Buffers.ClipMetas;
					const TRefCountPtr<FRDGPooledBuffer> OldAnimTRS = AnimLib.Buffers.AnimTRS;

					FRDGBufferRef ClipRDGUpload = CreateOrRegisterExternalBuffer(
						Context.GraphBuilder,
						AnimLib.Buffers.ClipMetas,
						FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_ClipMeta), ClipCountForAlloc),
						TEXT("GIAG_AG_ClipMetas_External"));

					FRDGBufferRef AnimRDGUpload = CreateOrRegisterExternalBuffer(
						Context.GraphBuilder,
						AnimLib.Buffers.AnimTRS,
						FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_BoneTRS), TRSCountForAlloc),
						TEXT("GIAG_AG_AnimTRS_External"));

					FRDGBufferRef RefPoseRDGUpload = CreateOrRegisterExternalBuffer(
						Context.GraphBuilder,
						AnimLib.Buffers.RefPoseLocalTRS,
						FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_BoneTRS), RefPoseCountForAlloc),
						TEXT("GIAG_AG_RefPoseLocalTRS_External"));

					// Non-repack capacity growth: preserve existing data by copying old->new prefix.
					if (!Upload.bRepack)
					{
						if (OldClipMetas.IsValid() && OldClipMetas != AnimLib.Buffers.ClipMetas)
						{
							const uint32 OldElems = (uint32)OldClipMetas->Desc.NumElements;
							const uint32 NewElems = (uint32)AnimLib.Buffers.ClipMetas->Desc.NumElements;
							const uint32 CopyElems = FMath::Min(OldElems, NewElems);
							if (CopyElems > 0)
							{
								FRDGBufferRef SrcRDG = Context.GraphBuilder.RegisterExternalBuffer(OldClipMetas, TEXT("GIAG_AG_ClipMetas_Old_External"));
								const uint64 NumBytes = (uint64)CopyElems * (uint64)sizeof(FGIAG_ClipMeta);
								AddCopyBufferPass(Context.GraphBuilder, ClipRDGUpload, 0, SrcRDG, 0, NumBytes);
							}
						}

						if (OldAnimTRS.IsValid() && OldAnimTRS != AnimLib.Buffers.AnimTRS)
						{
							const uint32 OldElems = (uint32)OldAnimTRS->Desc.NumElements;
							const uint32 NewElems = (uint32)AnimLib.Buffers.AnimTRS->Desc.NumElements;
							const uint32 CopyElems = FMath::Min(OldElems, NewElems);
							if (CopyElems > 0)
							{
								FRDGBufferRef SrcRDG = Context.GraphBuilder.RegisterExternalBuffer(OldAnimTRS, TEXT("GIAG_AG_AnimTRS_Old_External"));
								const uint64 NumBytes = (uint64)CopyElems * (uint64)sizeof(FGIAG_BoneTRS);
								AddCopyBufferPass(Context.GraphBuilder, AnimRDGUpload, 0, SrcRDG, 0, NumBytes);
							}
						}
					}

					// Optional repack: GPU-copy old AnimTRS -> new AnimTRS (no CPU residency).
					if (Upload.bRepack && Upload.RepackCopyOps.Num() > 0)
					{
						if (OldAnimTRS.IsValid() && OldAnimTRS != AnimLib.Buffers.AnimTRS)
						{
							FRDGBufferRef SrcRDG = Context.GraphBuilder.RegisterExternalBuffer(OldAnimTRS, TEXT("GIAG_AG_AnimTRS_Old_External"));
							for (const FGIAG_AnimLibraryRepackCopyOp& Op : Upload.RepackCopyOps)
							{
								if (Op.NumTransforms == 0)
								{
									continue;
								}
								const uint64 NumBytes = (uint64)Op.NumTransforms * (uint64)sizeof(FGIAG_BoneTRS);
								const uint64 SrcOffsetBytes = (uint64)Op.SrcStartTransformIndex * (uint64)sizeof(FGIAG_BoneTRS);
								const uint64 DstOffsetBytes = (uint64)Op.DstStartTransformIndex * (uint64)sizeof(FGIAG_BoneTRS);
								AddCopyBufferPass(Context.GraphBuilder, AnimRDGUpload, DstOffsetBytes, SrcRDG, SrcOffsetBytes, NumBytes);
							}
						}
					}

					// Apply incremental updates. RT may wait for bake completion before uploading pixels.
					for (const FGIAG_AnimLibraryClipMetaUpdate& Upd : Upload.ClipMetaUpdates)
					{
						if (Upd.ClipIndex >= Upload.NumClips)
						{
							continue;
						}
						const uint64 DstOffsetBytes = (uint64)Upd.ClipIndex * (uint64)sizeof(FGIAG_ClipMeta);
						UploadStructuredBuffer(
							Context.GraphBuilder,
							ClipRDGUpload,
							DstOffsetBytes,
							TEXT("GIAG_AG_UploadClipMeta"),
							sizeof(FGIAG_ClipMeta),
							&Upd.Meta,
							1);
					}

					for (const FGIAG_AnimLibraryAnimTRSUpdate& PUpd : Upload.AnimTRSUpdates)
					{
						if (PUpd.NumTransforms == 0 || PUpd.StartTransformIndex >= Upload.AnimTRSCapacity)
						{
							continue;
						}
						if (PUpd.CompletionEvent.IsValid())
						{
							PUpd.CompletionEvent->Wait();
						}
						if (!PUpd.TRS.IsValid() || PUpd.TRS->Num() <= 0)
						{
							continue;
						}
						const uint32 MaxWritable = Upload.AnimTRSCapacity - PUpd.StartTransformIndex;
						const uint32 NumToUpload = FMath::Min<uint32>(PUpd.NumTransforms, FMath::Min<uint32>(MaxWritable, (uint32)PUpd.TRS->Num()));
						if (NumToUpload == 0)
						{
							continue;
						}
						const uint64 DstOffsetBytes = (uint64)PUpd.StartTransformIndex * (uint64)sizeof(FGIAG_BoneTRS);
						UploadStructuredBuffer(
							Context.GraphBuilder,
							AnimRDGUpload,
							DstOffsetBytes,
							TEXT("GIAG_AG_UploadAnimTRS"),
							sizeof(FGIAG_BoneTRS),
							PUpd.TRS->GetData(),
							NumToUpload);
					}

					// Optional one-time (or rebuild) RefPose upload (TRS layout).
					if (Upload.RefPoseLocalTRS.IsValid() && Upload.RefPoseVersion > AnimLib.Buffers.RefPoseVersion)
					{
						check(Upload.RefPoseLocalTRS->Num() > 0);
						check((uint32)Upload.RefPoseLocalTRS->Num() <= RefPoseCountForAlloc);
						UploadStructuredBuffer(
							Context.GraphBuilder,
							RefPoseRDGUpload,
							0,
							TEXT("GIAG_AG_UploadRefPoseLocalTRS"),
							sizeof(FGIAG_BoneTRS),
							Upload.RefPoseLocalTRS->GetData(),
							(uint32)Upload.RefPoseLocalTRS->Num());
						AnimLib.Buffers.RefPoseVersion = Upload.RefPoseVersion;
					}

					AnimLib.Version = Upload.Version;
				}
			}

			LastCompiled = Payload.Compiled;
			LastParams = MoveTemp(Params);
			LastUploads = MoveTemp(Payload.Uploads);
			bHasPayload = true;
		}

		if (!bHasPayload)
		{
			continue;
		}

		// Phase B: compute animation once + dispatch followers under a single Master scope.
		FAnimLibraryRTCacheEntry& AnimLib = AnimLibraryBySkeleton_RT.FindOrAdd(FObjectKey(LastParams.Skeleton));
		checkf(AnimLib.Version != 0
			&& AnimLib.Buffers.ClipMetas.IsValid()
			&& AnimLib.Buffers.AnimTRS.IsValid()
			&& AnimLib.Buffers.RefPoseLocalTRS.IsValid()
			&& AnimLib.Buffers.NumBones != 0,
			TEXT("GIAG: AnimLibrary buffers not ready for Skeleton '%s'."), *GetNameSafe(LastParams.Skeleton));

		RDG_EVENT_SCOPE(Context.GraphBuilder, "GIAG %s(%s)", *LastParams.DebugGraphName, *LastParams.DebugMeshName);
		const FGIAG_AnimGraphGpuRunner::FOutputs Outputs = State->GetRunnerRT()->AddPasses_RenderThread(
			Context.GraphBuilder,
			*LastCompiled,
			LastParams,
			MoveTemp(LastUploads),
			AnimLib.Buffers,
			AnimResourceCache_RT);

		// ---- Attach outputs: Niagara bucket writes FxTransform; native bucket writes instance buffers ----
		if (Outputs.FinalPoseBuffer != nullptr && AttachGroupsByStateBucket_RT.Num() > 0)
		{
			const uint32 NumBonesU = (uint32)LastParams.NumBones;
			checkf(
				Outputs.FinalPoseType == EGIAG_AnimPinType::ComponentPose,
				TEXT("GIAG: Attach expects FinalPose to be ComponentPose after compile-time convergence."));
			FRDGBufferSRVRef PoseSRV = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Outputs.FinalPoseBuffer));

			for (auto& Pair : AttachGroupsByStateBucket_RT)
			{
				const FAttachGroupKey& AttachKey = Pair.Key;
				FAttachGroupRT& AttachGroup = Pair.Value;

				if (AttachKey.State != State)
				{
					continue;
				}
				if (AttachGroup.CPU.Num() == 0 || !AttachGroup.DescUploadBuffer.IsValid())
				{
					continue;
				}

				FAttachBucketRT* BucketPtr = AttachBuckets_RT.Find(AttachKey.BucketId);
				if (!BucketPtr || BucketPtr->NumInstances == 0)
				{
					continue;
				}

				FRDGBufferRef DescRDG = Context.GraphBuilder.RegisterExternalBuffer(AttachGroup.DescUploadBuffer, TEXT("GIAG_Attach_Desc_External"));
				FRDGBufferSRVRef DescSRV = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DescRDG));

				if (BucketPtr->IsNativeInstanceOnly())
				{
					if (!BucketPtr->InstanceOriginBuffer.IsValid() || !BucketPtr->InstanceTransformBuffer.IsValid()) { continue; }

					FRDGBufferRef InstanceOriginRDG = Context.GraphBuilder.RegisterExternalBuffer(BucketPtr->InstanceOriginBuffer, TEXT("GIAG_Attach_InstanceOrigin_External"));
					FRDGBufferRef InstanceTransformRDG = Context.GraphBuilder.RegisterExternalBuffer(BucketPtr->InstanceTransformBuffer, TEXT("GIAG_Attach_InstanceTransform_External"));

					GIAG::FAttachToISMInstanceBuffersPassParams AttachParams;
					AttachParams.NumBones = NumBonesU;
					AttachParams.NumAttachments = (uint32)AttachGroup.CPU.Num();
					AttachParams.PoseTRS = PoseSRV;
					AttachParams.ComponentToWorldBySlot = Outputs.ComponentToWorldBySlotSRV;
					AttachParams.AttachDescs = DescSRV;
					AttachParams.RW_InstanceOrigin = Context.GraphBuilder.CreateUAV(InstanceOriginRDG, PF_A32B32G32R32F);
					AttachParams.RW_InstanceTransform = Context.GraphBuilder.CreateUAV(InstanceTransformRDG, PF_A32B32G32R32F);
					GIAG::AddAttachToISMInstanceBuffersPasses(Context.GraphBuilder, AttachParams);

					Context.GraphBuilder.SetBufferAccessFinal(InstanceOriginRDG, ERHIAccess::SRVMask);
					Context.GraphBuilder.SetBufferAccessFinal(InstanceTransformRDG, ERHIAccess::SRVMask);
				}
				else
				{
					if (!BucketPtr->FxTransformBuffer.IsValid()) { continue; }

					FRDGBufferRef OutTRSRDG = Context.GraphBuilder.RegisterExternalBuffer(BucketPtr->FxTransformBuffer, TEXT("GIAG_Attach_FxTransform_External"));
					FRDGBufferUAVRef OutTRSUAV = Context.GraphBuilder.CreateUAV(OutTRSRDG);

					GIAG::FAttachToTransformBufferPassParams AttachParams;
					AttachParams.NumBones = NumBonesU;
					AttachParams.NumAttachments = (uint32)AttachGroup.CPU.Num();
					AttachParams.PoseTRS = PoseSRV;
					AttachParams.ComponentToWorldBySlot = Outputs.ComponentToWorldBySlotSRV;
					AttachParams.AttachDescs = DescSRV;
					AttachParams.RW_FxTransform = OutTRSUAV;
					GIAG::AddAttachToTransformBufferPasses(Context.GraphBuilder, AttachParams);

					Context.GraphBuilder.SetBufferAccessFinal(OutTRSRDG, ERHIAccess::SRVMask);
				}
			}
		}

		// Debug: request LocalPose slice readbacks.
		if (LastParams.DebugLocalPoseReadbackRequests.Num() > 0 && Outputs.FinalPoseBuffer != nullptr)
		{
			FRDGBufferRef LocalReadbackSource = nullptr;
			if (Outputs.FinalLocalPoseBuffer != nullptr)
			{
				LocalReadbackSource = Outputs.FinalLocalPoseBuffer;
			}
			else
			{
				checkf(Outputs.FinalPoseType == EGIAG_AnimPinType::ComponentPose,
					TEXT("GIAG: Debug LocalPose readback expects FinalPose to be ComponentPose after compile-time convergence."));
				FRDGBufferRef ConvertedLocalPoseRDG = Context.GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_BoneTRS), FMath::Max(1, LastParams.SlotCapacity * LastParams.NumBones)),
					TEXT("GIAG_DebugFinalComponentToLocal"));
				GIAG::FPoseSpaceConvertPassParams ConvertParams;
				ConvertParams.NumBones = (uint32)LastParams.NumBones;
				ConvertParams.NumInstances = (uint32)LastParams.NumInstances;
				ConvertParams.SourcePoseType = 1u;
				ConvertParams.DestinationPoseType = 0u;
				ConvertParams.ActiveInstanceIndices = Outputs.ActiveInstanceIndicesSRV;
				ConvertParams.ParentIndices = Outputs.ParentIndicesSRV;
				ConvertParams.SourcePoseTRS = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Outputs.FinalPoseBuffer));
				ConvertParams.RW_DestinationPoseTRS = Context.GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ConvertedLocalPoseRDG));
				GIAG::AddPoseSpaceConvertPasses(Context.GraphBuilder, ConvertParams);
				LocalReadbackSource = ConvertedLocalPoseRDG;
			}
			const uint32 NumBonesU = (uint32)LastParams.NumBones;
			const uint32 NumBytes = NumBonesU * (uint32)sizeof(FGIAG_BoneTRS);
			for (const FGIAG_LocalPoseReadbackRequest& Req : LastParams.DebugLocalPoseReadbackRequests)
			{
				FRDGBufferRef SliceRDG = Context.GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_BoneTRS), FMath::Max(1u, NumBonesU)), TEXT("GIAG_DebugLocalPoseSlice"));
				AddCopyBufferPass(Context.GraphBuilder, SliceRDG, 0, LocalReadbackSource,
					(uint64)Req.SlotIndex * (uint64)NumBonesU * (uint64)sizeof(FGIAG_BoneTRS), (uint64)NumBytes);
				TUniquePtr<FRHIGPUBufferReadback> Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GIAG_DebugLocalPoseReadback"));
				AddEnqueueCopyPass(Context.GraphBuilder, Readback.Get(), SliceRDG, NumBytes);
				FPendingLocalPoseReadback Pending;
				Pending.Readback = MoveTemp(Readback);
				Pending.NumBytes = NumBytes;
				Pending.NumBones = NumBonesU;
				Pending.CpuRequestFrame = LastParams.DebugCpuRequestFrame;
				Pending.RecordIndex = Req.RecordIndex;
				Pending.SerialNumber = Req.SerialNumber;
				PendingLocalPoseReadbacks_RT.Add(MoveTemp(Pending));
			}
		}

		// Debug: request NeedNodeBits readbacks.
		if (LastParams.DebugNeedNodeBitsReadbackRequests.Num() > 0 && Outputs.NeedNodeBitsBuffer != nullptr && Outputs.NeedNodeWordsPerSlot > 0)
		{
			const uint32 WordsPerSlot = Outputs.NeedNodeWordsPerSlot;
			const uint32 NumBytes = WordsPerSlot * (uint32)sizeof(uint32);
			const uint32 NumNodesU = Outputs.NeedNodeNumNodes;
			for (const FGIAG_NeedNodeBitsReadbackRequest& Req : LastParams.DebugNeedNodeBitsReadbackRequests)
			{
				FRDGBufferRef SliceRDG = Context.GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1u, WordsPerSlot)), TEXT("GIAG_DebugNeedNodeBitsSlice"));
				AddCopyBufferPass(Context.GraphBuilder, SliceRDG, 0, Outputs.NeedNodeBitsBuffer,
					(uint64)Req.SlotIndex * (uint64)WordsPerSlot * (uint64)sizeof(uint32), (uint64)NumBytes);
				TUniquePtr<FRHIGPUBufferReadback> Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GIAG_DebugNeedNodeBitsReadback"));
				AddEnqueueCopyPass(Context.GraphBuilder, Readback.Get(), SliceRDG, NumBytes);
				FPendingNeedNodeBitsReadback Pending;
				Pending.Readback = MoveTemp(Readback);
				Pending.NumBytes = NumBytes;
				Pending.CpuRequestFrame = LastParams.DebugCpuRequestFrame;
				Pending.RecordIndex = Req.RecordIndex;
				Pending.SerialNumber = Req.SerialNumber;
				Pending.SlotIndex = Req.SlotIndex;
				Pending.NumNodes = NumNodesU;
				Pending.WordsPerSlot = WordsPerSlot;
				PendingNeedNodeBitsReadbacks_RT.Add(MoveTemp(Pending));
			}
		}

		// Dispatch followers for this MasterState (under the same Master RDG_EVENT_SCOPE).
		if (Outputs.FinalPoseBuffer != nullptr && Outputs.InverseRefPoseSRV != nullptr)
		{
			FRDGBufferSRVRef FinalPoseSRV = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Outputs.FinalPoseBuffer));
			const uint32 NumActive = (uint32)LastParams.NumInstances;

			for (auto& FollowerEntry : FollowerGroups_RT)
			{
				const FFollowerGroupKey& FollowKey = FollowerEntry.Key;
				FFollowerGroupData& FollowGroup = FollowerEntry.Value;
				if (FollowKey.MasterState != State)
				{
					continue;
				}
				check(FollowGroup.DstInfos.Num() > 0);

				const uint32 NumDsts = (uint32)FollowGroup.DstInfos.Num();
				FRDGBufferRef DstInfoRDG = CreateStructuredBuffer(
					Context.GraphBuilder, TEXT("GIAG_FollowerDstInfos"),
					sizeof(uint32), NumDsts, FollowGroup.DstInfos.GetData(),
					(uint64)sizeof(uint32) * (uint64)NumDsts);
				FRDGBufferSRVRef DstInfoSRV = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DstInfoRDG));

				FRDGBufferSRVRef BoneRemapSRV;
				{
					const FBoneRemapKey RemapKey{ FollowGroup.BoneRemap, FollowGroup.NumBones };
					FBoneRemapRTCacheEntry& RemapEntry = BoneRemapBufferByKey_RT.FindOrAdd(RemapKey);
					const uint32 RemapCount = FMath::Max<uint32>(1, FollowGroup.NumBones);
					const FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RemapCount);
					const bool bNeedUpload = (!RemapEntry.Buffer.IsValid() || RemapEntry.Buffer->Desc != Desc);
					FRDGBufferRef BoneRemapRDG = CreateOrRegisterExternalBuffer(
						Context.GraphBuilder, RemapEntry.Buffer, Desc, TEXT("GIAG_Follow_BoneRemap_External"));
					if (bNeedUpload && FollowGroup.NumBones > 0)
					{
						if (FollowGroup.BoneRemap != nullptr)
						{
							Context.GraphBuilder.QueueBufferUpload(BoneRemapRDG, FollowGroup.BoneRemap,
								sizeof(uint32) * FollowGroup.NumBones, ERDGInitialDataFlags::None);
						}
						else
						{
							TArray<uint32> Identity;
							Identity.SetNumUninitialized(FollowGroup.NumBones);
							for (uint32 BoneIdx = 0; BoneIdx < FollowGroup.NumBones; ++BoneIdx)
							{
								Identity[BoneIdx] = BoneIdx;
							}
							Context.GraphBuilder.QueueBufferUpload(BoneRemapRDG, Identity.GetData(),
								sizeof(uint32) * FollowGroup.NumBones, ERDGInitialDataFlags::None);
						}
					}
					RemapEntry.Num = RemapCount;
					BoneRemapSRV = Context.GraphBuilder.CreateSRV(FRDGBufferSRVDesc(BoneRemapRDG, PF_R32_UINT));
				}

				// The follower pass already keys Prev writes off the per-indirection DirtyBoneTransforms
				// flag (handled by FFollowerGroupData::DstInfos: an extra DstInfo entry pointing at
				// PreviousTransformOffset is appended only when EDirtyBoneTransforms::Previous fires).
				// The shader writes to whatever DstInfos says; no per-slot init signaling needed.
				GIAG::FFollowerPoseToTransformBufferPassParams FollowParams;
				FollowParams.NumBones = FollowGroup.NumBones;
				FollowParams.SrcNumBones = FollowGroup.SrcNumBones;
				FollowParams.NumActive = NumActive;
				FollowParams.NumDsts = NumDsts;
				FollowParams.MaxTransformCount = FollowGroup.MaxTransformCount;
				FollowParams.PoseTRS = FinalPoseSRV;
				FollowParams.InverseRefPoseTRS = Outputs.InverseRefPoseSRV;
				FollowParams.DstInfos = DstInfoSRV;
				FollowParams.BoneRemap = BoneRemapSRV;
				FollowParams.ActiveInstanceIndices = Outputs.ActiveInstanceIndicesSRV;
				FollowParams.TransformBuffer = Context.TransformBuffer;
				FollowParams.DebugName = FollowKey.FollowMeshName;
				GIAG::AddFollowerPoseToTransformBufferPasses(Context.GraphBuilder, FollowParams);
			}
		}
	}

	// ---- Debug: attach FxTransform readback requests (GT -> RT -> GT) ----
	if (AttachReadbackBus_RT.IsValid())
	{
		FGIAG_AttachReadbackBus::FRequest Req;
		while (AttachReadbackBus_RT->Dequeue_RenderThread(Req))
		{
			if (Req.BucketId == 0u)
			{
				continue;
			}

			FAttachBucketRT* BucketPtr = AttachBuckets_RT.Find(Req.BucketId);
			if (!BucketPtr || BucketPtr->NumInstances == 0)
			{
				continue;
			}

			if (Req.OutputIndex >= BucketPtr->NumInstances)
			{
				continue;
			}

			if (BucketPtr->IsNativeInstanceOnly())
			{
				if (!BucketPtr->InstanceOriginBuffer.IsValid() || !BucketPtr->InstanceTransformBuffer.IsValid())
				{
					continue;
				}

				static constexpr uint32 NumElemInst = 4u; // origin + 3 rows
				static constexpr uint32 NumBytesInst = NumElemInst * (uint32)sizeof(FVector4f);

				FRDGBufferRef OriginRDG = Context.GraphBuilder.RegisterExternalBuffer(BucketPtr->InstanceOriginBuffer, TEXT("GIAG_Attach_InstanceOrigin_External_Readback"));
				FRDGBufferRef TransformRDG = Context.GraphBuilder.RegisterExternalBuffer(BucketPtr->InstanceTransformBuffer, TEXT("GIAG_Attach_InstanceTransform_External_Readback"));

				FRDGBufferRef Slice2RDG = Context.GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), NumElemInst),
					TEXT("GIAG_Attach_InstanceBuffers_Slice"));

				const uint64 OriginSrcOffsetBytes = (uint64)Req.OutputIndex * (uint64)sizeof(FVector4f);
				const uint64 TransformSrcOffsetBytes = (uint64)(Req.OutputIndex * 3u) * (uint64)sizeof(FVector4f);

				// origin -> dst[0]
				AddCopyBufferPass(Context.GraphBuilder, Slice2RDG, 0, OriginRDG, OriginSrcOffsetBytes, (uint64)sizeof(FVector4f));
				// 3 rows -> dst[1..3]
				AddCopyBufferPass(Context.GraphBuilder, Slice2RDG, (uint64)sizeof(FVector4f), TransformRDG, TransformSrcOffsetBytes, (uint64)(3u * sizeof(FVector4f)));

				TUniquePtr<FRHIGPUBufferReadback> Readback2 = MakeUnique<FRHIGPUBufferReadback>(TEXT("GIAG_DebugAttachInstanceBuffersReadback"));
				AddEnqueueCopyPass(Context.GraphBuilder, Readback2.Get(), Slice2RDG, NumBytesInst);

				FPendingAttachInstanceBuffersReadback Pending2;
				Pending2.Readback = MoveTemp(Readback2);
				Pending2.NumBytes = NumBytesInst;
				Pending2.BucketId = Req.BucketId;
				Pending2.OutputIndex = Req.OutputIndex;
				Pending2.CpuRequestFrame = Req.CpuRequestFrame;
				PendingAttachInstanceBuffersReadbacks_RT.Add(MoveTemp(Pending2));
			}
			else
			{
				if (!BucketPtr->FxTransformBuffer.IsValid())
				{
					continue;
				}

				FRDGBufferRef OutTRSRDG = Context.GraphBuilder.RegisterExternalBuffer(BucketPtr->FxTransformBuffer, TEXT("GIAG_Attach_FxTransform_External_Readback"));

				// Copy just this element (one FGIAG_Transform) into a small staging RDG buffer and enqueue a GPU readback.
				static constexpr uint32 NumElemTRS = 1u;
				static constexpr uint32 NumBytesTRS = NumElemTRS * (uint32)sizeof(FGIAG_Transform);

				FRDGBufferRef SliceRDG = Context.GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIAG_Transform), NumElemTRS),
					TEXT("GIAG_Attach_FxTransform_Slice"));

				const uint64 SrcOffsetBytes = (uint64)Req.OutputIndex * (uint64)sizeof(FGIAG_Transform);
				AddCopyBufferPass(Context.GraphBuilder, SliceRDG, 0, OutTRSRDG, SrcOffsetBytes, (uint64)NumBytesTRS);

				TUniquePtr<FRHIGPUBufferReadback> Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GIAG_DebugAttachFxTransformReadback"));
				AddEnqueueCopyPass(Context.GraphBuilder, Readback.Get(), SliceRDG, NumBytesTRS);

				FPendingAttachFxTransformReadback Pending;
				Pending.Readback = MoveTemp(Readback);
				Pending.NumBytes = NumBytesTRS;
				Pending.BucketId = Req.BucketId;
				Pending.OutputIndex = Req.OutputIndex;
				Pending.CpuRequestFrame = Req.CpuRequestFrame;
				PendingAttachFxTransformReadbacks_RT.Add(MoveTemp(Pending));
			}
		}
	}
}

