#include "GIAG_ClipPlayerNode.h"

#include "BonePose.h"
#include "GameInstancedAnimationGraphSubsystem.h"
#include "GIAG_AnimCommon.h"
#include "GIAG_AnimGraph.h"
#include "GIAG_AnimNodeMetaManager.h"
#include "GIAG_AnimSequenceUserData.h"
#include "GIAG_EvalAnimUtils.h"
#include "GIAG_RdgDispatchTiling.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

GIAG_REGISTER_ANIM_NODE(FGIAG_ClipPlayerNode);

namespace
{
	class FGIAG_PoseClipPlayerCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseClipPlayerCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseClipPlayerCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER_ARRAY(FVector4f, TimeSlots, [GIAG_MAX_TIME_SLOTS / 4])
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER(uint32, NodeIndex)
			SHADER_PARAMETER(uint32, NeedNodeWordsPerSlot)

			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_ClipMeta>, ClipMetas)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_SlotState>, SlotStates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, TimeSlotIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, NeedNodeBits)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, AnimTRS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, RefPoseLocalTRS)
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

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseClipPlayerCS, "/GameInstancedAnimationGraphNode/GIAG_ClipPlayerNode.usf", "Main", SF_Compute);

	static void AddPoseClipPlayerPass(
		FRDGBuilder& GraphBuilder,
		uint32 NumBones,
		uint32 NumInstances,
		TConstArrayView<float> InTimeSlots,
		FRDGBufferSRVRef ClipMetas,
		FRDGBufferSRVRef SlotStates,
		FRDGBufferSRVRef ActiveInstanceIndices,
		FRDGBufferSRVRef TimeSlotIndicesSRV,
		FRDGBufferSRVRef NeedNodeBits,
		uint32 NeedNodeWordsPerSlot,
		uint32 NodeIndex,
		FRDGBufferSRVRef AnimTRS,
		FRDGBufferSRVRef RefPoseLocalTRS,
		FRDGBufferUAVRef RW_OutPose)
	{
		FGIAG_PoseClipPlayerCS::FParameters* BaseParameters = GraphBuilder.AllocParameters<FGIAG_PoseClipPlayerCS::FParameters>();
		BaseParameters->NumBones = NumBones;
		BaseParameters->NumInstances = NumInstances;
		GIAG_FillTimeSlotsParameter(BaseParameters->TimeSlots.GetData(), InTimeSlots);
		BaseParameters->NodeIndex = NodeIndex;
		BaseParameters->NeedNodeWordsPerSlot = NeedNodeWordsPerSlot;
		BaseParameters->ClipMetas = ClipMetas;
		BaseParameters->SlotStates = SlotStates;
		BaseParameters->ActiveInstanceIndices = ActiveInstanceIndices;
		BaseParameters->TimeSlotIndices = TimeSlotIndicesSRV;
		BaseParameters->NeedNodeBits = NeedNodeBits;
		BaseParameters->AnimTRS = AnimTRS;
		BaseParameters->RefPoseLocalTRS = RefPoseLocalTRS;
		BaseParameters->RW_OutPose = RW_OutPose;

		TShaderMapRef<FGIAG_PoseClipPlayerCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)NumBones * (int64)NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32 /*ChunkGroups1D*/, int32 GroupOffset1D, const FIntVector& GroupCount)
			{
				FGIAG_PoseClipPlayerCS::FParameters* ChunkParameters = GraphBuilder.AllocParameters<FGIAG_PoseClipPlayerCS::FParameters>();
				*ChunkParameters = *BaseParameters;
				ChunkParameters->DispatchGroupCountX = (uint32)GroupCount.X;
				ChunkParameters->DispatchGroupCountY = (uint32)GroupCount.Y;
				ChunkParameters->DispatchGroupOffset = (uint32)GroupOffset1D;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GIAG_ClipPlayer"),
					ChunkParameters,
					ERDGPassFlags::Compute,
					[ChunkParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *ChunkParameters, GroupCount);
					});
			});
	}
}

void FGIAG_ClipPlayerNode::PlayAnimation(const FGIAG_AnimNodeRef& NodeRef, const UAnimSequence* AnimSequence, float BlendDurationSeconds, float StartSeconds, bool bLoop, float PlayRate)
{
	int32 ClipIndex = NodeRef.System->RequestAnimClipIndex(NodeRef, AnimSequence);
	if (ClipIndex == INDEX_NONE)
	{
		return;
	}
	const auto NowSeconds = NodeRef.GetTimeSlotSeconds();

	const float BlendDur = FMath::Max(0.f, BlendDurationSeconds);
	if (BlendDur <= 0.f || SlotState.NumClips == 0)
	{
		SlotState.NumClips = 1;
		SlotState.Clips[0].Clip = ClipIndex;
		SlotState.Clips[0].StartTime = NowSeconds;
		SlotState.Clips[0].StartSeconds = StartSeconds;
		SlotState.Clips[0].PlayRate = PlayRate;
		SlotState.Clips[0].bLoop = bLoop ? 1u : 0u;
		NodeRef.MarkDirty();
		return;
	}

	// Chained cross-fade: keep up to 4 clips with nested-lerp semantics.
	auto ShiftLeftOne = [](FGIAG_SlotState& SlotState)
	{
		if (SlotState.NumClips <= 1u)
		{
			SlotState.NumClips = 1u;
			return;
		}

		const uint32 OldClipCount = FMath::Min<uint32>(4u, SlotState.NumClips);
		for (uint32 ClipIndex = 0; ClipIndex + 1 < OldClipCount; ++ClipIndex)
		{
			SlotState.Clips[ClipIndex] = SlotState.Clips[ClipIndex + 1];
		}
		for (uint32 BlendIndex = 0; BlendIndex + 1 < (OldClipCount - 1u); ++BlendIndex)
		{
			if (BlendIndex + 1 < 3u)
			{
				SlotState.BlendStartTimes[BlendIndex] = SlotState.BlendStartTimes[BlendIndex + 1];
				SlotState.BlendDurations[BlendIndex] = SlotState.BlendDurations[BlendIndex + 1];
			}
		}
		if (OldClipCount - 2u < 3u)
		{
			SlotState.BlendStartTimes[OldClipCount - 2u] = 0.f;
			SlotState.BlendDurations[OldClipCount - 2u] = 0.f;
		}
		SlotState.NumClips = OldClipCount - 1u;
	};

	auto CalcAlpha01 = [NowSeconds](float StartTime, float Duration) -> float
	{
		if (Duration <= KINDA_SMALL_NUMBER)
		{
			return 1.f;
		}
		return GIAG::Clamp01((NowSeconds - StartTime) / Duration);
	};

	SlotState.NumClips = FMath::Clamp<uint32>(SlotState.NumClips, 1u, 4u);
	while (SlotState.NumClips > 1u)
	{
		const float A0 = CalcAlpha01(SlotState.BlendStartTimes[0], SlotState.BlendDurations[0]);
		if (A0 < 0.9999f)
		{
			break;
		}
		ShiftLeftOne(SlotState);
	}
	if (SlotState.NumClips >= 4u)
	{
		ShiftLeftOne(SlotState);
	}

	const uint32 InsertIndex = FMath::Clamp<uint32>(SlotState.NumClips, 1u, 3u);
	SlotState.Clips[InsertIndex].Clip = ClipIndex;
	SlotState.Clips[InsertIndex].StartTime = NowSeconds;
	SlotState.Clips[InsertIndex].StartSeconds = StartSeconds;
	SlotState.Clips[InsertIndex].PlayRate = PlayRate;
	SlotState.Clips[InsertIndex].bLoop = bLoop ? 1u : 0u;
	SlotState.BlendStartTimes[InsertIndex - 1u] = NowSeconds;
	SlotState.BlendDurations[InsertIndex - 1u] = BlendDur;
	SlotState.NumClips = InsertIndex + 1u;

	NodeRef.MarkDirty();
	return;
}

void FGIAG_ClipPlayerNode::EnumerateClips(TArray<int32>& OutClipIndices) const
{
	const uint32 Count = FMath::Clamp<uint32>(SlotState.NumClips, 0u, 4u);
	for (uint32 i = 0; i < Count; ++i)
	{
		OutClipIndices.Add(SlotState.Clips[i].Clip);
	}
}

const void* FGIAG_ClipPlayerNode::GatherUploadsGPU(uint32& OutUploadStrideBytes) const
{
	OutUploadStrideBytes = sizeof(SlotState);
	return &SlotState;
}

void FGIAG_ClipPlayerNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		AddPoseClipPlayerPass(
			Context.GraphBuilder,
			(uint32)Context.NumBones,
			(uint32)Context.NumInstances,
			Context.TimeSlots,
			Context.ClipMetasSRV,
			Context.NodeParamSRVsPerNode[NodeIndexInBatch],
			Context.ActiveInstanceIndicesSRV,
			Context.TimeSlotIndicesSRV,
			Context.NeedNodeBitsSRV,
			Context.NeedNodeWordsPerSlot,
			(uint32)Context.NodeIndices[NodeIndexInBatch],
			Context.AnimTRSSRV,
			Context.RefPoseLocalTRSSRV,
			Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out].UAV);
	}
}

namespace
{
	static float WrapOrClampTime(float T, float Len, bool bLoop)
	{
		const float SafeLen = FMath::Max(Len, 1e-6f);
		if (bLoop)
		{
			float X = FMath::Fmod(T, SafeLen);
			if (X < 0.0f) { X += SafeLen; }
			return X;
		}
		return FMath::Clamp(T, 0.0f, SafeLen);
	}

	static float CalcBlendAlpha(float NowSeconds, float BlendStartTime, float BlendDuration)
	{
		if (BlendDuration <= 1e-6f)
		{
			return 1.0f;
		}
		return GIAG::Clamp01((NowSeconds - BlendStartTime) / BlendDuration);
	}

	static void ComputeSlotBlendWeights(
		float CurrentTimeSeconds,
		const FGIAG_SlotState& S,
		uint32 N,
		float& W0,
		float& W1,
		float& W2,
		float& W3)
	{
		constexpr float EPS = 1e-4f;
		W0 = 0.0f;
		W1 = 0.0f;
		W2 = 0.0f;
		W3 = 0.0f;

		const float A0 = (N >= 2u) ? CalcBlendAlpha(CurrentTimeSeconds, S.BlendStartTimes[0], S.BlendDurations[0]) : 0.0f;
		const float A1 = (N >= 3u) ? CalcBlendAlpha(CurrentTimeSeconds, S.BlendStartTimes[1], S.BlendDurations[1]) : 0.0f;
		const float A2 = (N >= 4u) ? CalcBlendAlpha(CurrentTimeSeconds, S.BlendStartTimes[2], S.BlendDurations[2]) : 0.0f;

		if (N >= 3u)
		{
			const float ALast = (N == 3u) ? A1 : A2;
			if (ALast >= 1.0f - EPS)
			{
				if (N == 3u) { W2 = 1.0f; }
				else { W3 = 1.0f; }
				return;
			}
		}

		if (N == 1u) { W0 = 1.0f; return; }
		if (N == 2u)
		{
			W0 = 1.0f - A0;
			W1 = A0;
			return;
		}
		if (N == 3u)
		{
			const float Om1 = 1.0f - A1;
			W0 = Om1 * (1.0f - A0);
			W1 = Om1 * A0;
			W2 = A1;
			return;
		}

		const float Om2 = 1.0f - A2;
		const float Om1 = 1.0f - A1;
		W0 = Om2 * Om1 * (1.0f - A0);
		W1 = Om2 * Om1 * A0;
		W2 = Om2 * A1;
		W3 = A2;
	}
}

namespace GIAG
{
	extern GAMEINSTANCEDANIMATIONGRAPH_API bool bQuantTimeInCpuEval;
}

void FGIAG_ClipPlayerNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	check(Context.Compiled);
	check(Context.OutputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.SkeletalMesh);

	USkeleton* Skeleton = Context.SkeletalMesh->GetSkeleton();
	check(Skeleton);
	check(Skeleton->GetReferenceSkeleton().GetNum() == Context.NumBones);

	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const int32 NodeIdx = Context.NodeIndices[NodeIndexInBatch];
		check(NodeIdx >= 0 && NodeIdx < Context.Compiled->Nodes.Num());

		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(OutPose.IsValid());

		// Per active slot: evaluate up to 4 clips, then nested-lerp blend (matches shader semantics).
		for (const uint32 SlotU : Context.ActiveInstanceIndices)
		{
			const int32 SlotIndex = (int32)SlotU;
			const uint8 TSIdx = (SlotIndex < Context.TimeSlotIndexBySlot.Num()) ? Context.TimeSlotIndexBySlot[SlotIndex] : 0;
			const float InstanceTime = Context.TimeSlots[TSIdx];
			check(SlotIndex >= 0 && SlotIndex < Context.SlotCapacity);

			const FGIAG_ClipPlayerNode* Node = Context.GetNodePtrBySlot<FGIAG_ClipPlayerNode>(NodeIdx, SlotIndex);
			const FGIAG_SlotState& S = Node->SlotState;

			if (S.NumClips == 0)
			{
				// No animation: output RefPose (Local).
				check(Context.RefPoseLocal.Num() == Context.NumBones);
				for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
				{
					OutPose.At(SlotIndex, BoneIndex) = FTransform3f(Context.RefPoseLocal[BoneIndex]);
				}
				continue;
			}

			// Step 1) Compute blend weights (no pose evaluation needed).
			const uint32 N = FMath::Min<uint32>(4u, (S.NumClips > 0u) ? S.NumClips : 1u);
			float W0 = 0.0f, W1 = 0.0f, W2 = 0.0f, W3 = 0.0f;
			ComputeSlotBlendWeights(InstanceTime, S, N, W0, W1, W2, W3);

			const bool bUse0 = (W0 != 0.0f) && (S.NumClips >= 1u);
			const bool bUse1 = (W1 != 0.0f) && (S.NumClips >= 2u);
			const bool bUse2 = (W2 != 0.0f) && (S.NumClips >= 3u);
			const bool bUse3 = (W3 != 0.0f) && (S.NumClips >= 4u);

			auto ResolveAnim = [&](uint32 ClipSlot) -> const UAnimSequence*
			{
				const int32 ClipIndex = S.Clips[ClipSlot].Clip;
				check(ClipIndex >= 0);
				check(Context.AnimSequencesByClipIndex.IsValidIndex(ClipIndex));
				const UAnimSequence* Anim = Context.AnimSequencesByClipIndex[ClipIndex];
				check(Anim);
				return Anim;
			};

			auto ComputeSampleTimeSeconds = [&](const UAnimSequence* Anim, uint32 ClipSlot) -> float
			{
				const float Len = Anim->GetPlayLength();
				const float Playback = (InstanceTime - S.Clips[ClipSlot].StartTime) * S.Clips[ClipSlot].PlayRate + S.Clips[ClipSlot].StartSeconds;
#if WITH_EDITOR
				if (GIAG::bQuantTimeInCpuEval)
				{
					// Match GPU baked-clip sampling: frames are baked at SecondsPerFrame, so sample on the same quantized timeline.
					float SecondsPerFrame = 1.0f / 30.0f;
					if (UGIAG_AnimSequenceUserData* UserData = Cast<UGIAG_AnimSequenceUserData>(const_cast<UAnimSequence*>(Anim)->GetAssetUserDataOfClass(UGIAG_AnimSequenceUserData::StaticClass())))
					{
						SecondsPerFrame = UserData->SecondsPerFrame;
					}
					return GIAG::QuantTime(Playback, Len, SecondsPerFrame, S.Clips[ClipSlot].bLoop != 0u);
				}
#endif
				return WrapOrClampTime(Playback, Len, S.Clips[ClipSlot].bLoop != 0u);
			};

			// Step 2) Fast-path: a single clip contributes -> evaluate just that clip and write directly.
			if (bUse0 && !bUse1 && !bUse2 && !bUse3)
			{
				const UAnimSequence* Anim = ResolveAnim(0u);
				check(Anim->GetSkeleton() == Skeleton);

				TArray<FTransform> Pose;
				const float SampleTime = ComputeSampleTimeSeconds(Anim, 0u);
				GIAG::EvalAnimSequenceLocalPose(Anim, SampleTime, Skeleton, Pose);
				check(Pose.Num() == Context.NumBones);
				for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
				{
					OutPose.At(SlotIndex, BoneIndex) = FTransform3f(Pose[BoneIndex]);
				}
				continue;
			}
			if (!bUse0 && bUse1 && !bUse2 && !bUse3)
			{
				const UAnimSequence* Anim = ResolveAnim(1u);
				check(Anim->GetSkeleton() == Skeleton);

				TArray<FTransform> Pose;
				const float SampleTime = ComputeSampleTimeSeconds(Anim, 1u);
				GIAG::EvalAnimSequenceLocalPose(Anim, SampleTime, Skeleton, Pose);
				check(Pose.Num() == Context.NumBones);
				for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
				{
					OutPose.At(SlotIndex, BoneIndex) = FTransform3f(Pose[BoneIndex]);
				}
				continue;
			}
			if (!bUse0 && !bUse1 && bUse2 && !bUse3)
			{
				const UAnimSequence* Anim = ResolveAnim(2u);
				check(Anim->GetSkeleton() == Skeleton);

				TArray<FTransform> Pose;
				const float SampleTime = ComputeSampleTimeSeconds(Anim, 2u);
				GIAG::EvalAnimSequenceLocalPose(Anim, SampleTime, Skeleton, Pose);
				check(Pose.Num() == Context.NumBones);
				for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
				{
					OutPose.At(SlotIndex, BoneIndex) = FTransform3f(Pose[BoneIndex]);
				}
				continue;
			}
			if (!bUse0 && !bUse1 && !bUse2 && bUse3)
			{
				const UAnimSequence* Anim = ResolveAnim(3u);
				check(Anim->GetSkeleton() == Skeleton);

				TArray<FTransform> Pose;
				const float SampleTime = ComputeSampleTimeSeconds(Anim, 3u);
				GIAG::EvalAnimSequenceLocalPose(Anim, SampleTime, Skeleton, Pose);
				check(Pose.Num() == Context.NumBones);
				for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
				{
					OutPose.At(SlotIndex, BoneIndex) = FTransform3f(Pose[BoneIndex]);
				}
				continue;
			}

			// Step 3) Multi-clip: evaluate only contributing clips, then blend per bone.
			TArray<FTransform> ClipPose[4];
			bool bHasPose[4] = { false, false, false, false };

			auto EvalIfUsed = [&](uint32 ClipSlot)
			{
				const UAnimSequence* Anim = ResolveAnim(ClipSlot);
				check(Anim->GetSkeleton() == Skeleton);

				const float SampleTime = ComputeSampleTimeSeconds(Anim, ClipSlot);
				GIAG::EvalAnimSequenceLocalPose(Anim, SampleTime, Skeleton, ClipPose[ClipSlot]);
				check(ClipPose[ClipSlot].Num() == Context.NumBones);
				bHasPose[ClipSlot] = true;
			};

			if (bUse0) { EvalIfUsed(0u); }
			if (bUse1) { EvalIfUsed(1u); }
			if (bUse2) { EvalIfUsed(2u); }
			if (bUse3) { EvalIfUsed(3u); }
			check(bHasPose[0] || bHasPose[1] || bHasPose[2] || bHasPose[3]);

			const uint32 FirstContributing =
				bHasPose[0] ? 0u :
				bHasPose[1] ? 1u :
				bHasPose[2] ? 2u : 3u;

			for (int32 BoneIndex = 0; BoneIndex < Context.NumBones; ++BoneIndex)
			{
				// For non-contributing clips, the TRS value is irrelevant (weight==0); keep a valid fallback anyway.
				const FGIAG_BoneTRS TFallback = FTransform3f(ClipPose[FirstContributing][BoneIndex]);
				const FGIAG_BoneTRS T0 = bHasPose[0] ? FTransform3f(ClipPose[0][BoneIndex]) : TFallback;
				const FGIAG_BoneTRS T1 = bHasPose[1] ? FTransform3f(ClipPose[1][BoneIndex]) : TFallback;
				const FGIAG_BoneTRS T2 = bHasPose[2] ? FTransform3f(ClipPose[2][BoneIndex]) : TFallback;
				const FGIAG_BoneTRS T3 = bHasPose[3] ? FTransform3f(ClipPose[3][BoneIndex]) : TFallback;

				// Inline: match shader blend flow (skip exact-zero weights).
				FVector3f Translation = FVector3f::ZeroVector;
				FVector3f Scale = FVector3f::ZeroVector;
				if (W0 != 0.0f) { Translation += T0.GetTranslation() * W0; Scale += T0.GetScale3D() * W0; }
				if (W1 != 0.0f) { Translation += T1.GetTranslation() * W1; Scale += T1.GetScale3D() * W1; }
				if (W2 != 0.0f) { Translation += T2.GetTranslation() * W2; Scale += T2.GetScale3D() * W2; }
				if (W3 != 0.0f) { Translation += T3.GetTranslation() * W3; Scale += T3.GetScale3D() * W3; }

				FQuat4f Ref = FQuat4f::Identity;
				if (W0 != 0.0f) { Ref = T0.GetRotation(); }
				else if (W1 != 0.0f) { Ref = T1.GetRotation(); }
				else if (W2 != 0.0f) { Ref = T2.GetRotation(); }
				else { Ref = T3.GetRotation(); }
				Ref.Normalize();

				auto AddAlignedWeighted = [&Ref](FQuat4f& Accum, FQuat4f Q, float W)
				{
					if (W == 0.0f)
					{
						return;
					}
					if ((Ref | Q) < 0.0f)
					{
						Q.X = -Q.X; Q.Y = -Q.Y; Q.Z = -Q.Z; Q.W = -Q.W;
					}
					Accum.X += Q.X * W;
					Accum.Y += Q.Y * W;
					Accum.Z += Q.Z * W;
					Accum.W += Q.W * W;
				};

				FQuat4f Q = FQuat4f(0, 0, 0, 0);
				AddAlignedWeighted(Q, T0.GetRotation(), W0);
				AddAlignedWeighted(Q, T1.GetRotation(), W1);
				AddAlignedWeighted(Q, T2.GetRotation(), W2);
				AddAlignedWeighted(Q, T3.GetRotation(), W3);

				const float Len2 = Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W;
				if (Len2 <= 1e-12f)
				{
					Q = FQuat4f::Identity;
				}
				else
				{
					const float InvLen = FMath::InvSqrt(Len2);
					Q.X *= InvLen; Q.Y *= InvLen; Q.Z *= InvLen; Q.W *= InvLen;
				}

				OutPose.At(SlotIndex, BoneIndex) = FGIAG_BoneTRS(Q, Translation, Scale);
			}
		}
	}
}
