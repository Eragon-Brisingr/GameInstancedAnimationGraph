#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h" // FRDGPooledBuffer (required for TRefCountPtr in headers)

class FRHIShaderResourceView;

/**
 * Render-thread registries for GIAG attach output buffers.
 *
 * Contract:
 * - RT-only: all functions must be called on the rendering thread.
 */

/** Niagara attach: FxTransform + Niagara meta buffers (slot tables + add list). */
class GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_NiagaraAttachRegistry
{
public:
	struct FEntry
	{
		// GIAG attach output (StructuredBuffer<FGIAG_Transform>, 1 per instance).
		TRefCountPtr<FRDGPooledBuffer> FxTransformBuffer;

		// Niagara meta buffers consumed by DI (GPU sim) and VM (CPU) for spawn/kill.
		// All are RT-owned pooled buffers published by the scene extension.
		TRefCountPtr<FRDGPooledBuffer> SlotToDenseIndexBuffer;   // StructuredBuffer<int>
		TRefCountPtr<FRDGPooledBuffer> SlotGenerationBuffer;     // StructuredBuffer<int>
		TRefCountPtr<FRDGPooledBuffer> FxParticleGenBuffer;      // StructuredBuffer<int>
		TRefCountPtr<FRDGPooledBuffer> AddListPackedBuffer;      // StructuredBuffer<int> packed=(FxGen<<16)|Slot
		uint32 SlotTableVersion = 0;
		uint32 AddListVersion = 0;
		uint32 AddListCount = 0;

		uint32 NumInstances = 0;
	};

	FGIAG_NiagaraAttachRegistry() = default;
	FGIAG_NiagaraAttachRegistry(const FGIAG_NiagaraAttachRegistry&) = delete;
	FGIAG_NiagaraAttachRegistry& operator=(const FGIAG_NiagaraAttachRegistry&) = delete;
	FGIAG_NiagaraAttachRegistry(FGIAG_NiagaraAttachRegistry&&) = default;
	FGIAG_NiagaraAttachRegistry& operator=(FGIAG_NiagaraAttachRegistry&&) = default;
	~FGIAG_NiagaraAttachRegistry() = default;

	void RegisterOrUpdate(
		uint32 BucketId,
		uint32 NumInstances,
		TRefCountPtr<FRDGPooledBuffer> FxTransformBuffer,
		// Niagara meta (optional; pass null buffers when unchanged / not applicable).
		TRefCountPtr<FRDGPooledBuffer> SlotToDenseIndexBuffer = nullptr,
		TRefCountPtr<FRDGPooledBuffer> SlotGenerationBuffer = nullptr,
		TRefCountPtr<FRDGPooledBuffer> FxParticleGenBuffer = nullptr,
		TRefCountPtr<FRDGPooledBuffer> AddListPackedBuffer = nullptr,
		uint32 SlotTableVersion = 0,
		uint32 AddListVersion = 0,
		uint32 AddListCount = 0);
	void Unregister(uint32 BucketId);

	const FEntry* Find(uint32 BucketId) const;

	// Convenience helpers (all RT-only).
	uint32 FindNumInstances(uint32 BucketId) const;

private:
	TMap<uint32, FEntry> ByBucketId;
};

/** Native attach mesh renderer: instance origin/transform buffers (world space). */
class GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_NativeAttachRegistry
{
public:
	struct FEntry
	{
		// UE instancing buffers (world space) used by LocalVertexFactory instancing manual fetch.
		// - InstanceOrigin: 1 float4 per instance (xyz=translation, w unused)
		// - InstanceTransform: 3 float4 per instance (xyz=row basis, w unused)
		TRefCountPtr<FRDGPooledBuffer> InstanceOriginBuffer;
		TRefCountPtr<FRHIShaderResourceView> InstanceOriginSRV;
		TRefCountPtr<FRDGPooledBuffer> InstanceTransformBuffer;
		TRefCountPtr<FRHIShaderResourceView> InstanceTransformSRV;
		uint32 NumInstances = 0;
	};

	FGIAG_NativeAttachRegistry() = default;
	FGIAG_NativeAttachRegistry(const FGIAG_NativeAttachRegistry&) = delete;
	FGIAG_NativeAttachRegistry& operator=(const FGIAG_NativeAttachRegistry&) = delete;
	FGIAG_NativeAttachRegistry(FGIAG_NativeAttachRegistry&&) = default;
	FGIAG_NativeAttachRegistry& operator=(FGIAG_NativeAttachRegistry&&) = default;
	~FGIAG_NativeAttachRegistry() = default;

	void RegisterOrUpdate(
		uint32 BucketId,
		uint32 NumInstances,
		TRefCountPtr<FRDGPooledBuffer> InstanceOriginBuffer,
		TRefCountPtr<FRDGPooledBuffer> InstanceTransformBuffer);
	void Unregister(uint32 BucketId);

	// Convenience helpers (all RT-only).
	FRHIShaderResourceView* FindInstanceOriginSRV(uint32 BucketId) const;
	FRHIShaderResourceView* FindInstanceTransformSRV(uint32 BucketId) const;
	uint32 FindNumInstances(uint32 BucketId) const;

private:
	TMap<uint32, FEntry> ByBucketId;
};

