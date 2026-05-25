#include "GIAG_AttachMeshComponent.h"

#include "GIAG_AttachRegistry.h"

#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "PrimitiveSceneProxy.h"
#include "StaticMeshResources.h"
#include "SceneManagement.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "LocalVertexFactory.h"
#include "Engine/InstancedStaticMesh.h" // FInstancedStaticMeshVertexFactoryUniformShaderParameters
#include "MeshMaterialShader.h"         // FMeshMaterialShader
#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "SceneInterface.h"

UGIAG_AttachMeshComponent::UGIAG_AttachMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bTickInEditor = false;

	// Make sure we render in the main pass by default (editor preview relies on this).
	bRenderInMainPass = true;
	bRenderInDepthPass = true;
	bCastDynamicShadow = true;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
}

namespace
{
	/**
	 * LocalVertexFactory with instancing enabled (manual vertex fetch path),
	 * binding InstanceOrigin/InstanceTransform SRVs from GIAG attach registry.
	 *
	 * This VF FORCE-enables MANUAL_VERTEX_FETCH so we never depend on per-instance vertex streams.
	 */
	class FGIAG_AttachMeshVertexFactory : public FLocalVertexFactory
	{
		DECLARE_VERTEX_FACTORY_TYPE(FGIAG_AttachMeshVertexFactory);

	public:
		explicit FGIAG_AttachMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
			: FLocalVertexFactory(InFeatureLevel, "FGIAG_AttachMeshVertexFactory")
		{}

		static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
		{
			return FLocalVertexFactory::ShouldCompilePermutation(Parameters);
		}

		static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

			OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), TEXT("1"));
			OutEnvironment.SetDefine(TEXT("USE_INSTANCE_CULLING"), TEXT("0"));

			// Force MVF so instance data always comes from SRVs, for all permutations (BasePass/DepthOnly/etc).
			OutEnvironment.SetDefine(TEXT("MANUAL_VERTEX_FETCH"), TEXT("1"));

			// IMPORTANT:
			// LocalVertexFactory's instancing path uses SV_InstanceID for per-instance fetch.
			// GPUScene primitive data also wants SV_InstanceID, which leads to overlapping semantics on D3D.
			// So we must opt-out of PrimitiveSceneData/GPUScene for this VF.
			OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), TEXT("0"));
			OutEnvironment.SetDefine(TEXT("VF_USE_PRIMITIVE_SCENE_DATA"), TEXT("0"));
		}

		void SetInstanceSRVs(FRHIShaderResourceView* InOrigin, FRHIShaderResourceView* InTransform)
		{
			InstanceOriginSRV = InOrigin;
			InstanceTransformSRV = InTransform;

			FInstancedStaticMeshVertexFactoryUniformShaderParameters Uniform;
			Uniform.VertexFetch_InstanceOriginBuffer = InstanceOriginSRV;
			Uniform.VertexFetch_InstanceTransformBuffer = InstanceTransformSRV;
			Uniform.VertexFetch_InstanceLightmapBuffer = nullptr;
			Uniform.InstanceCustomDataBuffer = nullptr;
			Uniform.NumCustomDataFloats = 0;
			InstanceVFUniformBuffer = TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters>::CreateUniformBufferImmediate(
				Uniform,
				UniformBuffer_MultiFrame,
				EUniformBufferValidation::None);
		}

		FRHIUniformBuffer* GetInstanceVFUniformBuffer() const { return InstanceVFUniformBuffer.GetReference(); }
		FRHIShaderResourceView* GetInstanceOriginSRV() const { return InstanceOriginSRV; }
		FRHIShaderResourceView* GetInstanceTransformSRV() const { return InstanceTransformSRV; }

	private:
		FRHIShaderResourceView* InstanceOriginSRV = nullptr;
		FRHIShaderResourceView* InstanceTransformSRV = nullptr;
		TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters> InstanceVFUniformBuffer;
	};

	class FGIAG_AttachMeshVertexFactoryShaderParameters : public FLocalVertexFactoryShaderParametersBase
	{
		DECLARE_TYPE_LAYOUT(FGIAG_AttachMeshVertexFactoryShaderParameters, NonVirtual);
	public:
		void Bind(const FShaderParameterMap& ParameterMap)
		{
			FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);
			InstancingOffsetParameter.Bind(ParameterMap, TEXT("InstancingOffset"));
			VertexFetch_InstanceOriginBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceOriginBuffer"));
			VertexFetch_InstanceTransformBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceTransformBuffer"));
			InstanceOffset.Bind(ParameterMap, TEXT("InstanceOffset"));
		}

		void GetElementShaderBindings(
			const FSceneInterface* Scene,
			const FSceneView* View,
			const FMeshMaterialShader* Shader,
			const EVertexInputStreamType InputStreamType,
			ERHIFeatureLevel::Type FeatureLevel,
			const FVertexFactory* VertexFactory,
			const FMeshBatchElement& BatchElement,
			FMeshDrawSingleShaderBindings& ShaderBindings,
			FVertexInputStreamArray& VertexStreams) const
		{
			FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
			FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
				Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement, VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

			const auto* VF = static_cast<const FGIAG_AttachMeshVertexFactory*>(VertexFactory);

			ShaderBindings.Add(InstanceOffset, BatchElement.UserIndex);
			ShaderBindings.Add(InstancingOffsetParameter, FVector4f(0, 0, 0, 0));

			ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVertexFactoryUniformShaderParameters>(), VF->GetInstanceVFUniformBuffer());
			ShaderBindings.Add(VertexFetch_InstanceOriginBufferParameter, VF->GetInstanceOriginSRV());
			ShaderBindings.Add(VertexFetch_InstanceTransformBufferParameter, VF->GetInstanceTransformSRV());
		}

	private:
		LAYOUT_FIELD(FShaderParameter, InstancingOffsetParameter);
		LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceOriginBufferParameter);
		LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceTransformBufferParameter);
		LAYOUT_FIELD(FShaderParameter, InstanceOffset);
	};

	IMPLEMENT_TYPE_LAYOUT(FGIAG_AttachMeshVertexFactoryShaderParameters);
	IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGIAG_AttachMeshVertexFactory, SF_Vertex, FGIAG_AttachMeshVertexFactoryShaderParameters);
	IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGIAG_AttachMeshVertexFactory, SF_Pixel, FGIAG_AttachMeshVertexFactoryShaderParameters);

	IMPLEMENT_VERTEX_FACTORY_TYPE(
		FGIAG_AttachMeshVertexFactory,
		"/Engine/Private/LocalVertexFactory.ush",
		EVertexFactoryFlags::UsedWithMaterials
		| EVertexFactoryFlags::SupportsDynamicLighting
		| EVertexFactoryFlags::SupportsPositionOnly
		| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
		| EVertexFactoryFlags::SupportsManualVertexFetch
		| EVertexFactoryFlags::SupportsPSOPrecaching);

	class FGIAG_AttachMeshSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		explicit FGIAG_AttachMeshSceneProxy(const UGIAG_AttachMeshComponent* InComp)
			: FPrimitiveSceneProxy(InComp)
			, BucketId_RT((uint32)FMath::Max(0, InComp->BucketId))
			, AttachRegistry_RT(InComp->AttachRegistry)
			, StaticMesh(InComp->StaticMesh)
			, MaterialOverrides(InComp->MaterialOverrides)
			, bCastShadow_RT(InComp->CastShadow)
		{
			bWillEverBeLit = true;
			// We create/update VF uniform buffers from registry SRVs during GDME; must run on RT (not parallel tasks).
			bSupportsParallelGDME = false;

			// Compute material relevance from the actual draw material (not the mesh's materials).
			{
				const ERHIFeatureLevel::Type FeatureLevel = GetScene().GetFeatureLevel();
				const EShaderPlatform ShaderPlatform = GShaderPlatformForFeatureLevel[FeatureLevel];
				const int32 NumMats = StaticMesh ? StaticMesh->GetStaticMaterials().Num() : 0;
				for (int32 MatIndex = 0; MatIndex < NumMats; ++MatIndex)
				{
					UMaterialInterface* Mat = nullptr;
					if (MaterialOverrides.IsValidIndex(MatIndex) && MaterialOverrides[MatIndex])
					{
						Mat = MaterialOverrides[MatIndex];
					}
					else if (StaticMesh)
					{
						Mat = StaticMesh->GetMaterial(MatIndex);
					}
					if (Mat)
					{
						MaterialRelevance |= Mat->GetRelevance_Concurrent(ShaderPlatform);
					}
				}
			}
		}

		void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
		{
			FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);

			check(IsInRenderingThread());

			if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
			{
				return;
			}

			const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];

			VertexFactory = MakeUnique<FGIAG_AttachMeshVertexFactory>(GetScene().GetFeatureLevel());

			FGIAG_AttachMeshVertexFactory::FDataType Data;
			const bool bRHISupportsMVF = RHISupportsManualVertexFetch(GShaderPlatformForFeatureLevel[GetScene().GetFeatureLevel()]);

			LOD.VertexBuffers.PositionVertexBuffer.BindPositionVertexBuffer(VertexFactory.Get(), Data);
			LOD.VertexBuffers.StaticMeshVertexBuffer.BindTangentVertexBuffer(VertexFactory.Get(), Data);
			LOD.VertexBuffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(VertexFactory.Get(), Data);

			const int32 LightMapCoordIndex = StaticMesh->GetLightMapCoordinateIndex();
			if (LightMapCoordIndex >= 0 && LightMapCoordIndex < (int32)LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords())
			{
				LOD.VertexBuffers.StaticMeshVertexBuffer.BindLightMapVertexBuffer(VertexFactory.Get(), Data, LightMapCoordIndex);
			}

			if (LOD.bHasColorVertexData)
			{
				LOD.VertexBuffers.ColorVertexBuffer.BindColorVertexBuffer(VertexFactory.Get(), Data);
			}
			else
			{
				FColorVertexBuffer::BindDefaultColorVertexBuffer(
					VertexFactory.Get(),
					Data,
					bRHISupportsMVF ? FColorVertexBuffer::NullBindStride::FColorSizeForComponentOverride : FColorVertexBuffer::NullBindStride::ZeroForDefaultBufferBind);
			}

			VertexFactory->SetData(RHICmdList, Data);
			VertexFactory->InitResource(RHICmdList);

			// Provide LocalVF vertex-fetch uniform buffer (required for MVF correctness).
			LocalVFUniformBuffer = CreateLocalVFUniformBuffer(VertexFactory.Get(), /*LODLightmapDataIndex*/ 0, /*OverrideColorVB*/ nullptr, /*BaseVertexIndex*/ 0, /*PreSkinBaseVertexIndex*/ 0);

			// Create always-valid fallback instance buffers so the ISM VF uniform buffer is never null.
			// (Some passes will assert if a required uniform buffer is null.)
			{
				const FRHIBufferCreateDesc OriginDesc =
					FRHIBufferCreateDesc::CreateVertex<FVector4f>(TEXT("GIAG_Attach_FallbackOrigin"), 1)
					.AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::VertexBuffer)
					.SetInitialState(ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask)
					.SetInitActionZeroData();

				const FRHIBufferCreateDesc TransformDesc =
					FRHIBufferCreateDesc::CreateVertex<FVector4f>(TEXT("GIAG_Attach_FallbackTransform"), 3)
					.AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::VertexBuffer)
					.SetInitialState(ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask)
					.SetInitActionZeroData();

				FallbackOriginBuffer = RHICmdList.CreateBuffer(OriginDesc);
				FallbackTransformBuffer = RHICmdList.CreateBuffer(TransformDesc);

				// Fill identity transform (rows) + origin (translation).
				{
					void* OriginPtr = RHICmdList.LockBuffer(FallbackOriginBuffer, 0, sizeof(FVector4f), RLM_WriteOnly);
					((FVector4f*)OriginPtr)[0] = FVector4f(0, 0, 0, 0);
					RHICmdList.UnlockBuffer(FallbackOriginBuffer);

					void* XformPtr = RHICmdList.LockBuffer(FallbackTransformBuffer, 0, sizeof(FVector4f) * 3, RLM_WriteOnly);
					FVector4f* X = (FVector4f*)XformPtr;
					X[0] = FVector4f(1, 0, 0, 0);
					X[1] = FVector4f(0, 1, 0, 0);
					X[2] = FVector4f(0, 0, 1, 0);
					RHICmdList.UnlockBuffer(FallbackTransformBuffer);
				}

				FallbackOriginSRV = RHICmdList.CreateShaderResourceView(
					FallbackOriginBuffer,
					FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_A32B32G32R32F));
				FallbackTransformSRV = RHICmdList.CreateShaderResourceView(
					FallbackTransformBuffer,
					FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_A32B32G32R32F));
			}

			// Initialize VF instancing uniform with fallback SRVs; we'll overwrite with real registry SRVs during GDME.
			VertexFactory->SetInstanceSRVs(FallbackOriginSRV.GetReference(), FallbackTransformSRV.GetReference());
		}

		void DestroyRenderThreadResources() override
		{
			check(IsInRenderingThread());
			if (VertexFactory)
			{
				VertexFactory->ReleaseResource();
				VertexFactory.Reset();
			}
			FallbackOriginSRV.SafeRelease();
			FallbackTransformSRV.SafeRelease();
			FallbackOriginBuffer.SafeRelease();
			FallbackTransformBuffer.SafeRelease();
			FPrimitiveSceneProxy::DestroyRenderThreadResources();
		}

		SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View);
			Result.bShadowRelevance = IsShadowCast(View);
			Result.bDynamicRelevance = true;
			Result.bRenderInMainPass = ShouldRenderInMainPass();
			Result.bRenderInDepthPass = ShouldRenderInDepthPass();
			Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
			MaterialRelevance.SetPrimitiveViewRelevance(Result);
			return Result;
		}

		uint32 GetMemoryFootprint() const override { return(sizeof(*this) + GetAllocatedSize()); }
		uint32 GetAllocatedSize() const { return FPrimitiveSceneProxy::GetAllocatedSize(); }

		void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
		{
			check(IsInRenderingThread() || IsInParallelRenderingThread());
			check(AttachRegistry_RT.IsValid());

			if (!VertexFactory || !StaticMesh || BucketId_RT == 0)
			{
				return;
			}
			const FStaticMeshRenderData* RD = StaticMesh->GetRenderData();
			if (!RD || RD->LODResources.Num() == 0)
			{
				return;
			}
			const FStaticMeshLODResources& LOD = RD->LODResources[0];

			uint32 NumInstances = AttachRegistry_RT->FindNumInstances(BucketId_RT);
			FRHIShaderResourceView* OriginSRV = AttachRegistry_RT->FindInstanceOriginSRV(BucketId_RT);
			FRHIShaderResourceView* TransformSRV = AttachRegistry_RT->FindInstanceTransformSRV(BucketId_RT);

			if (NumInstances == 0 || OriginSRV == nullptr || TransformSRV == nullptr)
			{
				return;
			}

			// Update instancing SRVs on the VF so all permutations fetch the correct per-instance data.
			VertexFactory->SetInstanceSRVs(OriginSRV ? OriginSRV : FallbackOriginSRV.GetReference(), TransformSRV ? TransformSRV : FallbackTransformSRV.GetReference());

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				if ((VisibilityMap & (1u << ViewIndex)) == 0)
				{
					continue;
				}

				for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
				{
					const FStaticMeshSection& Section = LOD.Sections[SectionIndex];

					FMeshBatch& Mesh = Collector.AllocateMesh();
					Mesh.VertexFactory = VertexFactory.Get();
					Mesh.Type = PT_TriangleList;
					Mesh.DepthPriorityGroup = SDPG_World;
					Mesh.CastShadow = bCastShadow_RT && Section.bCastShadow;
					Mesh.bCanApplyViewModeOverrides = true;
					Mesh.LODIndex = 0;
					Mesh.SegmentIndex = SectionIndex;
					Mesh.MeshIdInPrimitive = SectionIndex;
					Mesh.LCI = nullptr;
					// Critical: make it behave like an opaque static mesh draw w.r.t depth / occlusion / view modes.
					Mesh.bUseForMaterial = true;
					Mesh.bUseForDepthPass = true;
					Mesh.bUseAsOccluder = true;
					Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();

					UMaterialInterface* MatInterface = nullptr;
					if (MaterialOverrides.IsValidIndex(Section.MaterialIndex) && MaterialOverrides[Section.MaterialIndex])
					{
						MatInterface = MaterialOverrides[Section.MaterialIndex];
					}
					else
					{
						MatInterface = StaticMesh->GetMaterial(Section.MaterialIndex);
					}
					if (!MatInterface)
					{
						MatInterface = UMaterial::GetDefaultMaterial(MD_Surface);
					}
					const FMaterialRenderProxy* MaterialRenderProxy = MatInterface->GetRenderProxy();
					// Force fallback proxy to be selected if needed (async compile / missing permutations).
					{
						const FMaterialRenderProxy* FallbackProxy = MaterialRenderProxy;
						MaterialRenderProxy->GetMaterialWithFallback(GetScene().GetFeatureLevel(), FallbackProxy);
						MaterialRenderProxy = FallbackProxy;
					}
					Mesh.MaterialRenderProxy = MaterialRenderProxy;

					FMeshBatchElement& Element = Mesh.Elements[0];
					Element.IndexBuffer = &LOD.IndexBuffer;
					Element.FirstIndex = Section.FirstIndex;
					Element.NumPrimitives = Section.NumTriangles;
					Element.MinVertexIndex = Section.MinVertexIndex;
					Element.MaxVertexIndex = Section.MaxVertexIndex;
					// IMPORTANT:
					// Our instance buffers are authored in WORLD space (native bucket writes instance buffers directly),
					// but LocalVertexFactory instancing will also apply the Primitive's LocalToWorld via the primitive uniform buffer.
					// If we bind the component's primitive uniform, we'd double-transform (and depth/occlusion will look "wrong"/see-through).
					// NOTE:
					// Our instance buffers are authored in WORLD space, and the shader will convert to TranslatedWorld by adding
					// ResolvedView.PreViewTranslation inside TransformLocalToTranslatedWorld(). Therefore Primitive LocalToWorld must be identity
					// (otherwise we'd double-apply PreViewTranslation).
					{
						FDynamicPrimitiveUniformBuffer& DynUB = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
						// IMPORTANT (depth correctness):
						// Our instance buffers already encode WORLD-space transforms (origin + basis rows in world space).
						// Therefore the primitive's LocalToWorld must be identity, *and* the primitive's LWC origin must be zero.
						// If Actor/ObjectWorldPosition comes from the proxy bounds (default Set overload), LWC can interpret the
						// same vertex positions relative to a non-zero origin, causing BasePass/Depth to disagree and depth to be wrong.
						const float HugeR = 5.0e6f;
						const FBoxSphereBounds ZeroBounds(FBox(FVector(-HugeR), FVector(HugeR)));
						DynUB.Set(
							Collector.GetRHICommandList(),
							FMatrix::Identity,
							FMatrix::Identity,
							FVector::ZeroVector,
							ZeroBounds,
							ZeroBounds,
							ZeroBounds,
							/*bReceivesDecals*/ true,
							/*bHasPrecomputedVolumetricLightmap*/ false,
							AlwaysHasVelocity(),
							/*CustomPrimitiveData*/ nullptr);
						// Our per-instance transforms are already in WORLD space, so the primitive LocalToWorld must be identity.
						// We intentionally opt out of GPUScene primitive data for this VF (see ModifyCompilationEnvironment),
						// so bind the RHI uniform buffer pointer directly.
						Element.PrimitiveUniformBuffer = DynUB.UniformBuffer.GetUniformBufferRHI();
						Element.PrimitiveUniformBufferResource = nullptr;
					}
					Element.VertexFactoryUserData = LocalVFUniformBuffer.GetReference();
					Element.NumInstances = NumInstances;
					Element.UserIndex = 0; // InstanceOffset

					Collector.AddMesh(ViewIndex, Mesh);
				}
			}
		}

	private:
		const uint32 BucketId_RT = 0;
		const TSharedPtr<FGIAG_NativeAttachRegistry, ESPMode::ThreadSafe> AttachRegistry_RT;
		TObjectPtr<UStaticMesh> StaticMesh;
		TArray<TObjectPtr<UMaterialInterface>> MaterialOverrides;
		mutable TUniquePtr<FGIAG_AttachMeshVertexFactory> VertexFactory;
		TUniformBufferRef<FLocalVertexFactoryUniformShaderParameters> LocalVFUniformBuffer;
		FBufferRHIRef FallbackOriginBuffer;
		FBufferRHIRef FallbackTransformBuffer;
		FShaderResourceViewRHIRef FallbackOriginSRV;
		FShaderResourceViewRHIRef FallbackTransformSRV;
		FMaterialRelevance MaterialRelevance;
		const bool bCastShadow_RT = true;
	};
}

FPrimitiveSceneProxy* UGIAG_AttachMeshComponent::CreateSceneProxy()
{
	if (StaticMesh == nullptr)
	{
		return nullptr;
	}
	check(BucketId > 0);
	check(AttachRegistry.IsValid());
	return new FGIAG_AttachMeshSceneProxy(this);
}

FBoxSphereBounds UGIAG_AttachMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// We don't have per-instance CPU bounds; keep it simple and conservative.
	const float BoundsRadius = 5.0e6f;
	const FVector Origin = LocalToWorld.GetLocation();
	return FBoxSphereBounds(FBox(Origin - FVector(BoundsRadius), Origin + FVector(BoundsRadius)));
}

int32 UGIAG_AttachMeshComponent::GetNumMaterials() const
{
	return StaticMesh ? StaticMesh->GetStaticMaterials().Num() : 0;
}

UMaterialInterface* UGIAG_AttachMeshComponent::GetMaterial(int32 ElementIndex) const
{
	if (MaterialOverrides.IsValidIndex(ElementIndex) && MaterialOverrides[ElementIndex])
	{
		return MaterialOverrides[ElementIndex];
	}
	return StaticMesh ? StaticMesh->GetMaterial(ElementIndex) : nullptr;
}

void UGIAG_AttachMeshComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	OutMaterials.Reset();

	const int32 Num = GetNumMaterials();
	OutMaterials.Reserve(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		if (UMaterialInterface* Mat = GetMaterial(i))
		{
			OutMaterials.Add(Mat);
		}
	}

	Super::GetUsedMaterials(OutMaterials, bGetDebugMaterials);
}

