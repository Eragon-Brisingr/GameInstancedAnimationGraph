#include "GIAG_LookAtNode.h"

#include "GameInstancedAnimationGraphSubsystem.h"
#include "GIAG_AnimNodeMetaManager.h"
#include "GIAG_RdgDispatchTiling.h"
#include "Animation/Skeleton.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

GIAG_REGISTER_ANIM_NODE(FGIAG_LookAtNode);

namespace
{
	class FGIAG_PoseLookAtCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseLookAtCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseLookAtCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(float, CurrentTimeSeconds)
			SHADER_PARAMETER(float, BlendDurationSeconds)
			SHADER_PARAMETER(FVector3f, LookAtAxisLocal)
			SHADER_PARAMETER(float, LookAtClampDegrees)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, NodeIndex)
			SHADER_PARAMETER(uint32, NeedNodeWordsPerSlot)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, BasePose)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_LookAtRuntimeData>, NodeParams)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, BoneIndexBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_Transform>, WorldToComponentBySlot)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, NeedNodeBits)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_BoneTRS>, RW_OutPose)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
			OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
			OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseLookAtCS, "/GameInstancedAnimationGraphNode/GIAG_LookAtNode.usf", "Main", SF_Compute);

	static float ComputeEnableAlpha(const FGIAG_LookAtRuntimeData& RuntimeData, float CurrentTimeSeconds, float BlendDurationSeconds)
	{
		checkf(BlendDurationSeconds >= 0.0f, TEXT("GIAG LookAt: BlendDurationSeconds must be >= 0."));
		if (BlendDurationSeconds <= 0.0f)
		{
			return RuntimeData.bEnabled ? 1.0f : 0.0f;
		}

		const float T = FMath::Clamp((CurrentTimeSeconds - RuntimeData.LastEnableDisableTimeSeconds) / BlendDurationSeconds, 0.0f, 1.0f);
		return RuntimeData.bEnabled ? T : (1.0f - T);
	}

	static FVector3f ClampToLookAtCone(const FVector3f& AimVector, const FVector3f& ToTarget, float LookAtClampDegrees)
	{
		FVector3f Result = ToTarget;
		if (LookAtClampDegrees > ZERO_ANIMWEIGHT_THRESH)
		{
			const float ClampRadians = FMath::DegreesToRadians(FMath::Min(LookAtClampDegrees, 180.0f));
			const float Dot = FMath::Clamp(FVector3f::DotProduct(AimVector, Result), -1.0f, 1.0f);
			const float DiffAngle = FMath::Acos(Dot);
			if (DiffAngle > ClampRadians)
			{
				FVector3f DeltaTarget = Result - AimVector;
				DeltaTarget *= (ClampRadians / DiffAngle);
				Result = (AimVector + DeltaTarget).GetSafeNormal();
			}
		}
		return Result;
	}

	static void AddPoseLookAtPass(
		FRDGBuilder& GraphBuilder,
		uint32 NumBones,
		uint32 NumInstances,
		float CurrentTimeSeconds,
		float BlendDurationSeconds,
		const FVector3f& LookAtAxisLocal,
		float LookAtClampDegrees,
		FRDGBufferSRVRef BasePose,
		FRDGBufferSRVRef NodeParams,
		FRDGBufferSRVRef BoneIndexBuffer,
		FRDGBufferSRVRef WorldToComponentBySlot,
		FRDGBufferSRVRef ActiveInstanceIndices,
		FRDGBufferSRVRef NeedNodeBits,
		uint32 NeedNodeWordsPerSlot,
		uint32 NodeIndex,
		FRDGBufferUAVRef RW_OutPose)
	{
		FGIAG_PoseLookAtCS::FParameters* BaseP = GraphBuilder.AllocParameters<FGIAG_PoseLookAtCS::FParameters>();
		BaseP->NumBones = NumBones;
		BaseP->NumInstances = NumInstances;
		BaseP->CurrentTimeSeconds = CurrentTimeSeconds;
		BaseP->BlendDurationSeconds = BlendDurationSeconds;
		BaseP->LookAtAxisLocal = LookAtAxisLocal;
		BaseP->LookAtClampDegrees = LookAtClampDegrees;
		BaseP->NodeIndex = NodeIndex;
		BaseP->NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;
		BaseP->BasePose = BasePose;
		BaseP->NodeParams = NodeParams;
		BaseP->BoneIndexBuffer = BoneIndexBuffer;
		BaseP->WorldToComponentBySlot = WorldToComponentBySlot;
		BaseP->ActiveInstanceIndices = ActiveInstanceIndices;
		BaseP->NeedNodeBits = NeedNodeBits;
		BaseP->RW_OutPose = RW_OutPose;

		TShaderMapRef<FGIAG_PoseLookAtCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)NumBones * (int64)NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
			{
				FGIAG_PoseLookAtCS::FParameters* P = GraphBuilder.AllocParameters<FGIAG_PoseLookAtCS::FParameters>();
				*P = *BaseP;
				P->DispatchGroupCountX = (uint32)GroupCount.X;
				P->DispatchGroupCountY = (uint32)GroupCount.Y;
				P->DispatchGroupOffset = (uint32)GroupOffset1D;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GIAG_PoseLookAt_Chunk"),
					P,
					ERDGPassFlags::Compute,
					[P, CS, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, CS, *P, GroupCount);
					});
			});
	}
}

void FGIAG_LookAtNode::SetEnabled(const FGIAG_AnimNodeRef& NodeRef, bool bNewEnabled)
{
	const uint32 NewEnabledValue = bNewEnabled ? 1u : 0u;
	if (RuntimeData.bEnabled == NewEnabledValue)
	{
		return;
	}

	check(NodeRef.System);
	UWorld* World = NodeRef.System->GetWorld();
	check(World);

	RuntimeData.bEnabled = NewEnabledValue;
	RuntimeData.LastEnableDisableTimeSeconds = (float)World->GetTimeSeconds();
	NodeRef.MarkDirty();
}

const void* FGIAG_LookAtNode::GatherUploadsGPU(uint32& OutUploadStrideBytes) const
{
	OutUploadStrideBytes = sizeof(RuntimeData);
	return &RuntimeData;
}

void FGIAG_LookAtNode::EnumerateResourceRequests(FConstStructView Settings, const USkeleton* Skeleton, EGIAG_AnimResourceTarget Target, TArray<FGIAG_AnimResourceRequest>& Out)
{
	checkf(IsValid(Skeleton) && Skeleton->GetReferenceSkeleton().GetNum() > 0, TEXT("GIAG LookAt: invalid Skeleton."));
	const FGIAG_LookAtSettings* LookAtSettings = Settings.GetPtr<const FGIAG_LookAtSettings>();
	checkf(LookAtSettings != nullptr, TEXT("GIAG LookAt: Settings must be provided."));
	checkf(!LookAtSettings->BoneToModify.IsNone(), TEXT("GIAG LookAt: BoneToModify must be set."));

	FGIAG_AnimResourceRequest Req;
	Req.Slot = 0; // Slot 0 reserved for BoneIndex.
	Req.ShareKey.Object = Skeleton;
	Req.ShareKey.AddonDesc = LookAtSettings->BoneToModify;
	Req.ShareKey.AddonHash = HashCombine(GetTypeHash(LookAtSettings->BoneToModify), GetTypeHash((uint8)Target));
	Req.Layout.Kind = EGIAG_AnimResourceKind::Buffer;
	Req.Layout.StrideBytes = sizeof(int32);
	Req.Layout.NumElements = 1;
	Req.Access = EGIAG_AnimResourceAccess::SRV;
	Out.Add(Req);
}

bool FGIAG_LookAtNode::BuildResourceForGPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TArray<uint8>& OutBytes)
{
	checkf(IsValid(Skeleton) && Skeleton->GetReferenceSkeleton().GetNum() > 0, TEXT("GIAG LookAt: invalid Skeleton."));
	checkf(Req.Slot == 0 && Req.Layout.Kind == EGIAG_AnimResourceKind::Buffer, TEXT("GIAG LookAt: invalid request slot/kind."));
	checkf(Req.Layout.StrideBytes == sizeof(int32), TEXT("GIAG LookAt: invalid request stride."));
	checkf(Req.Layout.NumElements == 1, TEXT("GIAG LookAt: invalid request num elements."));

	const FGIAG_LookAtSettings* LookAtSettings = Settings.GetPtr<const FGIAG_LookAtSettings>();
	checkf(LookAtSettings != nullptr, TEXT("GIAG LookAt: Settings must be provided."));
	checkf(!LookAtSettings->BoneToModify.IsNone(), TEXT("GIAG LookAt: BoneToModify must be set."));

	const int32 BoneIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(LookAtSettings->BoneToModify);
	checkf(BoneIndex != INDEX_NONE, TEXT("GIAG LookAt: BoneToModify '%s' not found in skeleton."), *LookAtSettings->BoneToModify.ToString());

	OutBytes.SetNumUninitialized(sizeof(int32));
	FMemory::Memcpy(OutBytes.GetData(), &BoneIndex, sizeof(int32));
	return true;
}

bool FGIAG_LookAtNode::BuildResourceForCPU(
	const FGIAG_AnimResourceRequest& Req,
	FConstStructView Settings,
	const USkeleton* Skeleton,
	TSharedPtr<void>& OutResource)
{
	checkf(IsValid(Skeleton) && Skeleton->GetReferenceSkeleton().GetNum() > 0, TEXT("GIAG LookAt: invalid Skeleton."));
	checkf(Req.Slot == 0 && Req.Layout.Kind == EGIAG_AnimResourceKind::Buffer, TEXT("GIAG LookAt: invalid request slot/kind."));
	checkf(Req.Layout.StrideBytes == sizeof(int32), TEXT("GIAG LookAt: invalid request stride."));
	checkf(Req.Layout.NumElements == 1, TEXT("GIAG LookAt: invalid request num elements."));

	const FGIAG_LookAtSettings* LookAtSettings = Settings.GetPtr<const FGIAG_LookAtSettings>();
	checkf(LookAtSettings != nullptr, TEXT("GIAG LookAt: Settings must be provided."));
	checkf(!LookAtSettings->BoneToModify.IsNone(), TEXT("GIAG LookAt: BoneToModify must be set."));

	const int32 BoneIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(LookAtSettings->BoneToModify);
	checkf(BoneIndex != INDEX_NONE, TEXT("GIAG LookAt: BoneToModify '%s' not found in skeleton."), *LookAtSettings->BoneToModify.ToString());

	OutResource = MakeShared<int32>(BoneIndex);
	return true;
}

void FGIAG_LookAtNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const FGIAG_LookAtSettings* Settings = Context.NodeSettingsPerNode[NodeIndexInBatch].GetPtr<const FGIAG_LookAtSettings>();
		checkf(Settings != nullptr, TEXT("GIAG LookAt: settings missing for node in batch %d."), NodeIndexInBatch);
		checkf(Settings->BlendDurationSeconds >= 0.0f, TEXT("GIAG LookAt: BlendDurationSeconds must be >= 0."));
		const FVector3f LookAtAxisLocal = Settings->LookAtAxis.GetSafeNormal();
		checkf(!LookAtAxisLocal.IsNearlyZero(), TEXT("GIAG LookAt: LookAtAxis must be non-zero."));

		const FRDGBufferSRVRef BoneIndexSRV =
			(Context.OptionalBufferSRVsPerNodeBySlot.Num() > 0
				&& Context.OptionalBufferSRVsPerNodeBySlot[0].Num() > NodeIndexInBatch)
			? Context.OptionalBufferSRVsPerNodeBySlot[0][NodeIndexInBatch]
			: nullptr;
		checkf(BoneIndexSRV != nullptr, TEXT("GIAG LookAt: BoneIndex optional SRV missing for node in batch %d."), NodeIndexInBatch);

		AddPoseLookAtPass(
			Context.GraphBuilder,
			(uint32)Context.NumBones,
			(uint32)Context.NumInstances,
			Context.CurrentTimeSeconds,
			Settings->BlendDurationSeconds,
			LookAtAxisLocal,
			Settings->LookAtClamp,
			Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base].SRV,
			Context.NodeParamSRVsPerNode[NodeIndexInBatch],
			BoneIndexSRV,
			Context.WorldToComponentBySlotSRV,
			Context.ActiveInstanceIndicesSRV,
			Context.NeedNodeBitsSRV,
			Context.NeedNodeWordsPerSlot,
			(uint32)Context.NodeIndices[NodeIndexInBatch],
			Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out].UAV);
	}
}

void FGIAG_LookAtNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	check(Context.Compiled);
	check(Context.InputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.OutputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.ComponentToWorldBySlot.Num() == Context.SlotCapacity);

	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const int32 NodeIdx = Context.NodeIndices[NodeIndexInBatch];
		check(NodeIdx >= 0 && NodeIdx < Context.Compiled->Nodes.Num());

		const FGIAG_LookAtSettings* Settings = Context.NodeSettingsPerNode[NodeIndexInBatch].GetPtr<const FGIAG_LookAtSettings>();
		checkf(Settings != nullptr, TEXT("GIAG LookAt: settings missing for node in batch %d."), NodeIndexInBatch);
		checkf(Settings->BlendDurationSeconds >= 0.0f, TEXT("GIAG LookAt: BlendDurationSeconds must be >= 0."));
		const FVector3f LookAtAxisLocal = Settings->LookAtAxis.GetSafeNormal();
		checkf(!LookAtAxisLocal.IsNearlyZero(), TEXT("GIAG LookAt: LookAtAxis must be non-zero."));

		const int32* BoneIndexPtr = Context.GetOptionalResourcePtr<int32>(0, NodeIndexInBatch);
		checkf(BoneIndexPtr != nullptr, TEXT("GIAG LookAt: BoneIndex optional resource missing for node in batch %d."), NodeIndexInBatch);
		const int32 BoneIndex = *BoneIndexPtr;
		check(BoneIndex >= 0 && BoneIndex < Context.NumBones);

		const FGIAG_CPUPoseBufferView BasePose = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base];
		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(BasePose.IsValid() && OutPose.IsValid());
		check(BasePose.PoseType == EGIAG_AnimPinType::ComponentPose);
		check(OutPose.PoseType == EGIAG_AnimPinType::ComponentPose);

		for (const int32 SlotIndex : Context.ActiveInstanceIndices)
		{
			check(SlotIndex >= 0 && SlotIndex < Context.SlotCapacity);

			// Copy input to output first, then overwrite only BoneToModify rotation.
			FMemory::Memcpy(
				&OutPose.At(SlotIndex, 0),
				&BasePose.At(SlotIndex, 0),
				sizeof(FGIAG_BoneTRS) * (SIZE_T)Context.NumBones);

			const FGIAG_LookAtNode* Node = Context.GetNodePtrBySlot<FGIAG_LookAtNode>(NodeIdx, SlotIndex);
			const float Alpha = ComputeEnableAlpha(Node->RuntimeData, Context.CurrentTimeSeconds, Settings->BlendDurationSeconds);
			if (Alpha <= 0.0f)
			{
				continue;
			}

			const FTransform3f BoneCS = BasePose.At(SlotIndex, BoneIndex);
			const FQuat BoneCSRot = FQuat(BoneCS.GetRotation());

			const FVector3f AimVectorCS = FVector3f(BoneCSRot.RotateVector(FVector(LookAtAxisLocal))).GetSafeNormal();
			checkf(!AimVectorCS.IsNearlyZero(), TEXT("GIAG LookAt: Aim vector must be non-zero after rotation."));

			const FVector3f TargetLocationCS = FVector3f(Context.ComponentToWorldBySlot[SlotIndex].InverseTransformPosition(FVector(Node->RuntimeData.TargetLocationWS)));
			FVector3f ToTargetCS = (TargetLocationCS - BoneCS.GetTranslation()).GetSafeNormal();
			checkf(!ToTargetCS.IsNearlyZero(), TEXT("GIAG LookAt: target direction must be non-zero."));
			ToTargetCS = ClampToLookAtCone(AimVectorCS, ToTargetCS, Settings->LookAtClamp);

			const FQuat DeltaCS = FQuat::FindBetweenNormals(FVector(AimVectorCS), FVector(ToTargetCS));
			FQuat BlendedDeltaCS = FQuat::Slerp(FQuat::Identity, DeltaCS, Alpha);
			BlendedDeltaCS.Normalize();

			const FQuat NewBoneCSRot = (BlendedDeltaCS * BoneCSRot).GetNormalized();

			FGIAG_BoneTRS& OutBone = OutPose.At(SlotIndex, BoneIndex);
			OutBone.SetRotation(FQuat4f(NewBoneCSRot));
		}
	}
}
