#include "GIAG_AnimGraph.h"

#include "GIAG_AnimGraphBuilder.h"
#include "GIAG_AnimGraphShaders.h"
#include "GIAG_GraphCullShaderMap.h"
#include "GIAG_GraphCullConstants.h"
#include "Algo/Reverse.h"
#include "Interfaces/ITargetPlatform.h"
#include "Misc/Crc.h"
#include "Misc/SecureHash.h"
#include "RHIStrings.h"
#include "Serialization/MemoryReader.h"

FGIAG_GraphCullShaderMapPtr::FGIAG_GraphCullShaderMapPtr(FGIAGShaderMap* InPtr)
	: Ptr(InPtr)
{
	if (Ptr)
	{
		Ptr->AddRef();
	}
}

FGIAG_GraphCullShaderMapPtr::FGIAG_GraphCullShaderMapPtr(const FGIAG_GraphCullShaderMapPtr& Other)
	: Ptr(Other.Ptr)
{
	if (Ptr)
	{
		Ptr->AddRef();
	}
}

FGIAG_GraphCullShaderMapPtr::FGIAG_GraphCullShaderMapPtr(FGIAG_GraphCullShaderMapPtr&& Other) noexcept
	: Ptr(Other.Ptr)
{
	Other.Ptr = nullptr;
}

FGIAG_GraphCullShaderMapPtr& FGIAG_GraphCullShaderMapPtr::operator=(const FGIAG_GraphCullShaderMapPtr& Other)
{
	if (this != &Other)
	{
		Reset();
		Ptr = Other.Ptr;
		if (Ptr)
		{
			Ptr->AddRef();
		}
	}
	return *this;
}

FGIAG_GraphCullShaderMapPtr& FGIAG_GraphCullShaderMapPtr::operator=(FGIAG_GraphCullShaderMapPtr&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Ptr = Other.Ptr;
		Other.Ptr = nullptr;
	}
	return *this;
}

FGIAG_GraphCullShaderMapPtr::~FGIAG_GraphCullShaderMapPtr()
{
	Reset();
}

void FGIAG_GraphCullShaderMapPtr::Reset()
{
	if (Ptr)
	{
		Ptr->Release();
		Ptr = nullptr;
	}
}

namespace
{
	static bool NeedsGraphCullCookData(const FGIAG_AnimGraphCompiledData& CompiledData)
	{
		return CompiledData.bEnableNodeCull && CompiledData.FinalPoseOutput.NodeIndex >= 0;
	}

	static uint64 HashGraphForCull(const FGIAG_AnimGraphCompiledData& CompiledData)
	{
		FSHA1 HashState;
		auto UpdateU32 = [&HashState](uint32 Value)
		{
			HashState.Update((const uint8*)&Value, sizeof(Value));
		};
		auto UpdateI32 = [&HashState](int32 Value)
		{
			HashState.Update((const uint8*)&Value, sizeof(Value));
		};
		auto UpdateStr = [&HashState](const FString& String)
		{
			FTCHARToUTF8 Utf8(*String);
			HashState.Update((const uint8*)Utf8.Get(), (uint32)Utf8.Length());
		};

		UpdateI32(CompiledData.NumNodes);
		UpdateI32(CompiledData.NumPoseResources);
		UpdateI32(CompiledData.ExecOrder.Num());

		for (int32 NodeIdx : CompiledData.ExecOrder)
		{
			UpdateI32(NodeIdx);
		}

		UpdateI32(CompiledData.FinalPoseOutput.NodeIndex);
		UpdateI32(CompiledData.FinalPoseOutput.PinIndex);

		for (int32 NodeIdx = 0; NodeIdx < CompiledData.NumNodes; ++NodeIdx)
		{
			const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
			UpdateStr(Node.TypeId.ToString());
			UpdateStr(Node.MemberName.ToString());
			UpdateI32(Node.NumInputPins);
			UpdateI32(Node.NumOutputPins);
			UpdateU32((Node.NodeMeta != nullptr && Node.NodeMeta->HasCullLogic()) ? 1u : 0u);
			if (Node.NodeMeta != nullptr && Node.NodeMeta->HasCullLogic())
			{
				FString DummyBody;
				const TCHAR* ElemType = nullptr;
				const TCHAR* MemberName = nullptr;
				Node.NodeMeta->EmitCullNeedMaskHlslBody(DummyBody, ElemType, MemberName);
				checkf(ElemType != nullptr && MemberName != nullptr,
					TEXT("GIAG: node type '%s' cull HLSL did not provide element type/member name."),
					*Node.TypeId.ToString());
				UpdateStr(FString(ElemType));
				UpdateStr(FString(MemberName));
			}
			for (int32 Pin = 0; Pin < Node.NumInputPins; ++Pin)
			{
				const int32 SrcNode = Node.InputSources.IsValidIndex(Pin) ? Node.InputSources[Pin].NodeIndex : INDEX_NONE;
				UpdateI32(SrcNode);
			}
		}

		HashState.Final();
		uint8 HashBytes[20];
		HashState.GetHash(HashBytes);
		uint64 Out = 0;
		FMemory::Memcpy(&Out, HashBytes, sizeof(uint64));
		return Out;
	}

	static FString GenerateGraphCullShaderSource(const FGIAG_AnimGraphCompiledData& CompiledData)
	{
		checkf(CompiledData.NumNodes <= 256, TEXT("GIAG: GraphCull generator supports up to 256 nodes (NumNodes=%d)."), CompiledData.NumNodes);

		FString Out;
		Out.Reserve(32 * 1024);

		auto SanitizeIdent = [](const FString& In) -> FString
		{
			FString Result;
			Result.Reserve(In.Len() + 1);
			for (int32 i = 0; i < In.Len(); ++i)
			{
				const TCHAR Ch = In[i];
				const bool bOk =
					(Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
					(Ch >= TEXT('a') && Ch <= TEXT('z')) ||
					(Ch >= TEXT('0') && Ch <= TEXT('9')) ||
					(Ch == TEXT('_'));
				Result.AppendChar(bOk ? Ch : TEXT('_'));
			}
			if (Result.IsEmpty() || !((Result[0] >= TEXT('A') && Result[0] <= TEXT('Z')) || (Result[0] >= TEXT('a') && Result[0] <= TEXT('z')) || Result[0] == TEXT('_')))
			{
				Result = TEXT("_") + Result;
			}
			return Result;
		};

		Out += TEXT("// Generated per-AnimGraph GraphCull include (no disk file).\n");
		Out += TEXT("// Do not edit.\n");
		Out += TEXT("#pragma once\n\n");
		Out += TEXT("#define GIAG_GRAPH_CULL_GENERATED 1\n");
		Out += TEXT("#include \"/GameInstancedAnimationGraphShader/GIAG_AnimCommon.ush\"\n\n");

		// Declare per-node cull param buffers (strong-typed StructuredBuffer<ElementType>).
		//
		// Binding model:
		// - C++ GraphCull pass binds a fixed set of SRV slots: GIAG_CullParam0..GIAG_CullParam31 (RDG parameters).
		// - Generated code declares GIAG_CullParamN with the correct element type for that slot.
		// - For node code ergonomics, we also #define the node symbol (e.g. LayerBlend_Alpha_Params) to GIAG_CullParamN.
		TArray<int32, TInlineAllocator<256>> CullParamIndexByNode;
		CullParamIndexByNode.SetNumUninitialized(CompiledData.NumNodes);
		for (int32 i = 0; i < CompiledData.NumNodes; ++i)
		{
			CullParamIndexByNode[i] = INDEX_NONE;
		}
		for (int32 i = 0; i < CompiledData.CullParamNodeIndices.Num(); ++i)
		{
			const int32 NodeIdx = CompiledData.CullParamNodeIndices[i];
			check(NodeIdx >= 0 && NodeIdx < CompiledData.NumNodes);
			CullParamIndexByNode[NodeIdx] = i;

			const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
			check(Node.NodeMeta != nullptr);
			FString DummyBody;
			const TCHAR* ElemType = nullptr;
			const TCHAR* MemberName = nullptr;
			Node.NodeMeta->EmitCullNeedMaskHlslBody(DummyBody, ElemType, MemberName);
			checkf(ElemType != nullptr,
				TEXT("GIAG: node type '%s' cull HLSL did not provide element type."),
				*Node.TypeId.ToString());
			checkf(CompiledData.CullParamSymbols.IsValidIndex(i), TEXT("GIAG: CullParamSymbols size mismatch."));
			const uint32 RegisterIndex = (uint32)GIAG::GraphCullParamSRVRegisterBase + (uint32)i;
			Out += FString::Printf(TEXT("StructuredBuffer<%s> %s : register(t%u);\n"),
				ElemType,
				*CompiledData.CullParamSymbols[i],
				RegisterIndex);
		}
		Out += TEXT("\n");

		// Per-node-type cull functions (generated once per type).
		{
			TSet<FName> EmittedTypes;
			for (int32 NodeIdx = 0; NodeIdx < CompiledData.NumNodes; ++NodeIdx)
			{
				const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
				const IGIAG_AnimNodeMeta* Meta = Node.NodeMeta;
				if (!Meta || !Meta->HasCullLogic())
				{
					continue;
				}
				if (EmittedTypes.Contains(Node.TypeId))
				{
					continue;
				}
				EmittedTypes.Add(Node.TypeId);

				// Contract: HasCullLogic implies cull HLSL is provided and uses NodeData.
				FString Body;
				const TCHAR* ElemType = nullptr;
				const TCHAR* MemberName = nullptr;
				Meta->EmitCullNeedMaskHlslBody(Body, ElemType, MemberName);
				check(ElemType != nullptr);

				const FString TypeFn = SanitizeIdent(Node.TypeId.ToString());
				const FString FnName = FString::Printf(TEXT("GIAG_CullNeedMask_%s"), *TypeFn);

				// Use the first encountered element type for this type (contract: same node type uses same upload type).
				Out += FString::Printf(TEXT("uint %s(uint SlotIndex, uint NumInputs, StructuredBuffer<%s> NodeData)\n{\n"),
					*FnName, ElemType);

				// Insert user-authored body verbatim (already contains return).
				Out += Body;
				if (!Body.EndsWith(TEXT("\n")))
				{
					Out += TEXT("\n");
				}
				Out += TEXT("}\n\n");
			}
		}

		Out += TEXT("void GIAG_GraphCull_Propagate(uint SlotIndex, inout uint WordsLocal[GIAG_MAX_GRAPH_NODES / 32])\n{\n");

		auto EmitSet = [&Out](int32 SrcNodeIdx)
		{
			const uint32 SrcU = (uint32)SrcNodeIdx;
			Out += FString::Printf(TEXT("\t\tWordsLocal[%u] |= 0x%08Xu;\n"), (SrcU >> 5), (1u << (SrcU & 31u)));
		};

		for (int32 i = CompiledData.ExecOrder.Num() - 1; i >= 0; --i)
		{
			const int32 NodeIdx = CompiledData.ExecOrder[i];
			check(NodeIdx >= 0 && NodeIdx < CompiledData.NumNodes);
			const uint32 NodeIdxU = (uint32)NodeIdx;
			const uint32 Word = (NodeIdxU >> 5);
			const uint32 Mask = (1u << (NodeIdxU & 31u));

			const FGIAG_AnimCompiledNode& Node = CompiledData.Nodes[NodeIdx];
			const IGIAG_AnimNodeMeta* Meta = Node.NodeMeta;

			Out += FString::Printf(TEXT("\tif ((WordsLocal[%u] & 0x%08Xu) != 0u)\n\t{\n"), Word, Mask);
			const uint32 NumInputsU = (uint32)FMath::Max(0, Node.NumInputPins);

			bool bUsedCustom = false;
			if (Meta && Meta->HasCullLogic())
			{
				const FString TypeFn = SanitizeIdent(Node.TypeId.ToString());
				const FString FnName = FString::Printf(TEXT("GIAG_CullNeedMask_%s"), *TypeFn);
				FStringView ParamSymbol;
				if (CullParamIndexByNode.IsValidIndex(NodeIdx))
				{
					const int32 ParamIdx = CullParamIndexByNode[NodeIdx];
					if (ParamIdx >= 0 && CompiledData.CullParamSymbols.IsValidIndex(ParamIdx))
					{
						ParamSymbol = FStringView(CompiledData.CullParamSymbols[ParamIdx]);
					}
				}
				check(!ParamSymbol.IsEmpty());
				Out += FString::Printf(TEXT("\t\tuint NeedMask = %s(SlotIndex, %u, %.*s);\n"),
					*FnName, NumInputsU, ParamSymbol.Len(), ParamSymbol.GetData());
				bUsedCustom = true;
			}

			if (bUsedCustom)
			{
				for (int32 Pin = 0; Pin < Node.NumInputPins; ++Pin)
				{
					const int32 Src = Node.InputSources.IsValidIndex(Pin) ? Node.InputSources[Pin].NodeIndex : INDEX_NONE;
					if (Src >= 0)
					{
						const uint32 PinMask = (1u << (uint32)Pin);
						Out += FString::Printf(TEXT("\t\tif ((NeedMask & 0x%08Xu) != 0u) { "), PinMask);
						EmitSet(Src);
						Out += TEXT("\t\t}\n");
					}
				}
			}
			else
			{
				// Fallback: conservative propagation.
				for (int32 Pin = 0; Pin < Node.NumInputPins; ++Pin)
				{
					const int32 Src = Node.InputSources.IsValidIndex(Pin) ? Node.InputSources[Pin].NodeIndex : INDEX_NONE;
					if (Src >= 0)
					{
						EmitSet(Src);
					}
				}
			}

			Out += TEXT("\t}\n");
		}

		Out += TEXT("}\n");

		return Out;
	}

	static void EnsurePin(const FGIAG_AnimInputPinRef& Pin, int32 NumNodes)
	{
		check(Pin.NodeIndex >= 0 && Pin.NodeIndex < NumNodes);
		check(Pin.PinIndex >= 0);
	}

	static void EnsurePin(const FGIAG_AnimOutputPinRef& Pin, int32 NumNodes)
	{
		check(Pin.NodeIndex >= 0 && Pin.NodeIndex < NumNodes);
		check(Pin.PinIndex >= 0);
	}

	static void TopoSortOrDie(
		int32 NumNodes,
		const TArray<TArray<int32>>& OutEdges,
		TArray<int32>& OutOrder)
	{
		TArray<int32> InDegree;
		InDegree.SetNumZeroed(NumNodes);
		for (int32 NodeIndex = 0; NodeIndex < NumNodes; ++NodeIndex)
		{
			for (int32 OutNodeIndex : OutEdges[NodeIndex])
			{
				InDegree[OutNodeIndex]++;
			}
		}

		TQueue<int32> Queue;
		for (int32 NodeIndex = 0; NodeIndex < NumNodes; ++NodeIndex)
		{
			if (InDegree[NodeIndex] == 0)
			{
				Queue.Enqueue(NodeIndex);
			}
		}

		OutOrder.Reset();
		while (!Queue.IsEmpty())
		{
			int32 NodeIndex = INDEX_NONE;
			Queue.Dequeue(NodeIndex);
			OutOrder.Add(NodeIndex);
			for (int32 OutNodeIndex : OutEdges[NodeIndex])
			{
				const int32 NewDeg = --InDegree[OutNodeIndex];
				if (NewDeg == 0)
				{
					Queue.Enqueue(OutNodeIndex);
				}
			}
		}

		checkf(OutOrder.Num() == NumNodes, TEXT("GIAG_AnimGraph: cycle detected (graph must be a DAG)."));
	}
}

const FGIAG_AnimGraphCompiledData& UGIAG_AnimGraph::Compile(bool bForce)
{
	if (bCompiled && bForce == false)
	{
		return Compiled;
	}

	FGIAG_AnimGraphBuilder Builder;
	{
		auto DefaultInstance = GetDefaultGraphInstance();
		check(DefaultInstance.IsValid());
		Builder.DefaultInstance = DefaultInstance;
	}
	BuildGraph(Builder);

	const auto& NodeMetas = Builder.GetNodeMetas();
	const auto& NodeSettings = Builder.GetNodeSettings();
	const TArray<int32>& NodeInstanceOffsets = Builder.GetNodeInstanceOffsets();
	const TArray<FName>& NodeMemberNames = Builder.GetNodeMemberNames();
	const TArray<FGIAG_AnimLink>& Links = Builder.GetLinks();

	Compiled = {};
	Compiled.NumNodes = NodeMetas.Num();
	Compiled.Nodes.SetNum(Compiled.NumNodes);

	for (int32 i = 0; i < Compiled.NumNodes; ++i)
	{
		check(NodeMetas[i]);
		FGIAG_AnimCompiledNode& Node = Compiled.Nodes[i];
		Node.NodeMeta = NodeMetas[i];
		UScriptStruct* NodeStruct = NodeMetas[i]->GetStruct();
		Node.TypeId = NodeStruct ? NodeStruct->GetFName() : NAME_None;
		checkf(NodeMemberNames.Num() == Compiled.NumNodes, TEXT("GIAG_AnimGraph: NodeMemberNames size mismatch."));
		Node.MemberName = NodeMemberNames[i];
		if (NodeSettings.Num() > 0)
		{
			checkf(NodeSettings.Num() == Compiled.NumNodes, TEXT("GIAG_AnimGraph: NodeSettings size mismatch."));
			Node.Settings = NodeSettings[i];
		}
		checkf(NodeInstanceOffsets.Num() == Compiled.NumNodes, TEXT("GIAG_AnimGraph: NodeInstanceOffsets size mismatch."));
		Node.InstanceDataOffset = NodeInstanceOffsets[i];
		checkf(Node.InstanceDataOffset >= 0, TEXT("GIAG_AnimGraph: Node %d has invalid InstanceDataOffset (bind via Builder.AddNode(node))."), i);
		// Cache GPU upload stride for this node (used for cull-param binding even when no slots are dirty).
		{
			uint32 Stride = 0;
			const uint8* NodePtr = (const uint8*)Builder.DefaultInstance.GetMemory() + (int64)Node.InstanceDataOffset;
			const void* Blob = NodeMetas[i]->GatherUploadsGPU(NodePtr, Stride);
			Node.GpuUploadStrideBytes = (Blob != nullptr) ? Stride : 0u;
		}
		Node.NumInputPins = NodeMetas[i]->GetNumInputPins();
		Node.NumOutputPins = NodeMetas[i]->GetNumOutputPins();
		Node.InputSources.SetNum(Node.NumInputPins);
		Node.InputPoseResources.SetNumZeroed(Node.NumInputPins);
		Node.OutputPoseResources.SetNumZeroed(Node.NumOutputPins);
		for (int32 PinIdx = 0; PinIdx < Node.NumInputPins; ++PinIdx)
		{
			Node.InputSources[PinIdx] = { INDEX_NONE, INDEX_NONE };
			Node.InputPoseResources[PinIdx] = INDEX_NONE;
		}
		for (int32 OutPinIdx = 0; OutPinIdx < Node.NumOutputPins; ++OutPinIdx)
		{
			Node.OutputPoseResources[OutPinIdx] = INDEX_NONE;
		}
	}

	// Validate and apply links.
	for (const FGIAG_AnimLink& Link : Links)
	{
		EnsurePin(Link.FromOutput, Compiled.NumNodes);
		EnsurePin(Link.ToInput, Compiled.NumNodes);

		const FGIAG_AnimCompiledNode& SrcNode = Compiled.Nodes[Link.FromOutput.NodeIndex];
		FGIAG_AnimCompiledNode& DstNode = Compiled.Nodes[Link.ToInput.NodeIndex];

		check(Link.FromOutput.PinIndex < SrcNode.NumOutputPins);
		check(Link.ToInput.PinIndex < DstNode.NumInputPins);

		const EGIAG_AnimPinType SrcType = SrcNode.NodeMeta->GetOutputPinType(Link.FromOutput.PinIndex);
		const EGIAG_AnimPinType DstType = DstNode.NodeMeta->GetInputPinType(Link.ToInput.PinIndex);
		const bool bSameType = (SrcType == DstType);
		const bool bCrossPoseSpaceConvert = GIAG_IsPosePinType(SrcType) && GIAG_IsPosePinType(DstType);
		checkf(bSameType || bCrossPoseSpaceConvert, TEXT("GIAG_AnimGraph: pin type mismatch (SrcType=%u DstType=%u)."), (uint32)SrcType, (uint32)DstType);

		// Store input source.
		DstNode.InputSources[Link.ToInput.PinIndex] = Link.FromOutput;
	}

	// Allocate pose resources for pose-typed output pins. (v1: unique resource per output pose pin)
	Compiled.NumPoseResources = 0;
	Compiled.PoseResourceTypes.Reset();
	for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
	{
		FGIAG_AnimCompiledNode& Node = Compiled.Nodes[NodeIdx];
		for (int32 OutPinIdx = 0; OutPinIdx < Node.NumOutputPins; ++OutPinIdx)
		{
			const EGIAG_AnimPinType OutType = Node.NodeMeta->GetOutputPinType(OutPinIdx);
			if (GIAG_IsPosePinType(OutType))
			{
				Node.OutputPoseResources[OutPinIdx] = Compiled.NumPoseResources++;
				Compiled.PoseResourceTypes.Add(OutType);
			}
		}
	}

	Compiled.PoseConvertTasks.Reset();
	TMap<TTuple<int32, uint8>, int32> ConvertTaskByKey;
	TArray<TArray<int32>> ConvertTaskConsumers;
	TBitArray<> ConvertTaskNeededByFinal;

	auto GetOrAddPoseConvertTask = [&](int32 SrcPoseRes, EGIAG_AnimPinType SrcType, EGIAG_AnimPinType DstType) -> int32
	{
		const TTuple<int32, uint8> Key{ SrcPoseRes, (uint8)DstType };
		if (int32* FoundTaskIndex = ConvertTaskByKey.Find(Key))
		{
			return *FoundTaskIndex;
		}

		FGIAG_AnimPoseConvertTask Task;
		Task.SrcPoseResource = SrcPoseRes;
		Task.DstPoseResource = Compiled.NumPoseResources++;
		Task.SrcPoseType = SrcType;
		Task.DstPoseType = DstType;
		const int32 NewTaskIndex = Compiled.PoseConvertTasks.Add(Task);
		Compiled.PoseResourceTypes.Add(DstType);
		ConvertTaskByKey.Add(Key, NewTaskIndex);
		ConvertTaskConsumers.SetNum(NewTaskIndex + 1);
		ConvertTaskNeededByFinal.PadToNum(NewTaskIndex + 1, false);
		return NewTaskIndex;
	};

	// Resolve pose input resources from connected output pins.
	for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
	{
		FGIAG_AnimCompiledNode& Node = Compiled.Nodes[NodeIdx];
		for (int32 InputIdx = 0; InputIdx < Node.NumInputPins; ++InputIdx)
		{
			const EGIAG_AnimPinType InputType = Node.NodeMeta->GetInputPinType(InputIdx);
			if (!GIAG_IsPosePinType(InputType))
			{
				continue;
			}

			const FGIAG_AnimOutputPinRef SrcPin = Node.InputSources[InputIdx];
			if (SrcPin.NodeIndex >= 0)
			{
				const FGIAG_AnimCompiledNode& SrcNode = Compiled.Nodes[SrcPin.NodeIndex];
				const EGIAG_AnimPinType SrcType = SrcNode.NodeMeta->GetOutputPinType(SrcPin.PinIndex);
				checkf(GIAG_IsPosePinType(SrcType), TEXT("GIAG_AnimGraph: source pin is not pose-typed."));
				const int32 SrcPoseRes = SrcNode.OutputPoseResources[SrcPin.PinIndex];
				check(SrcPoseRes >= 0);

				if (SrcType == InputType)
				{
					Node.InputPoseResources[InputIdx] = SrcPoseRes;
				}
				else
				{
					const int32 TaskIndex = GetOrAddPoseConvertTask(SrcPoseRes, SrcType, InputType);
					check(Compiled.PoseConvertTasks.IsValidIndex(TaskIndex));
					Node.InputPoseResources[InputIdx] = Compiled.PoseConvertTasks[TaskIndex].DstPoseResource;
					ConvertTaskConsumers[TaskIndex].AddUnique(NodeIdx);
				}
			}
			else
			{
				Node.InputPoseResources[InputIdx] = INDEX_NONE;
			}
		}
	}

	// Final pose pin (compile-time forced to ComponentPose).
	Compiled.FinalPoseOutput = Builder.GetFinalPose();
	Compiled.FinalPoseResource = INDEX_NONE;
	Compiled.FinalPoseType = EGIAG_AnimPinType::LocalPose;
	if (Compiled.FinalPoseOutput.NodeIndex >= 0)
	{
		check(Compiled.FinalPoseOutput.NodeIndex < Compiled.NumNodes);
		const FGIAG_AnimCompiledNode& FinalNode = Compiled.Nodes[Compiled.FinalPoseOutput.NodeIndex];
		checkf(
			Compiled.FinalPoseOutput.PinIndex >= 0 && Compiled.FinalPoseOutput.PinIndex < FinalNode.NumOutputPins,
			TEXT("GIAG_AnimGraph: invalid final pose output pin (Node=%d Pin=%d)."),
			Compiled.FinalPoseOutput.NodeIndex,
			Compiled.FinalPoseOutput.PinIndex);

		const EGIAG_AnimPinType FinalPinType = FinalNode.NodeMeta->GetOutputPinType(Compiled.FinalPoseOutput.PinIndex);
		checkf(GIAG_IsPosePinType(FinalPinType), TEXT("GIAG_AnimGraph: final output must be pose-typed."));

		const int32 FinalPinPoseRes = FinalNode.OutputPoseResources[Compiled.FinalPoseOutput.PinIndex];
		checkf(FinalPinPoseRes >= 0, TEXT("GIAG_AnimGraph: final output pose resource is invalid."));

		if (FinalPinType == EGIAG_AnimPinType::ComponentPose)
		{
			Compiled.FinalPoseResource = FinalPinPoseRes;
			Compiled.FinalLocalPoseResource = INDEX_NONE;
			Compiled.FinalPoseType = EGIAG_AnimPinType::ComponentPose;
		}
		else
		{
			Compiled.FinalLocalPoseResource = FinalPinPoseRes;
			const int32 ConvertTaskIndex = GetOrAddPoseConvertTask(
				FinalPinPoseRes,
				EGIAG_AnimPinType::LocalPose,
				EGIAG_AnimPinType::ComponentPose);
			check(Compiled.PoseConvertTasks.IsValidIndex(ConvertTaskIndex));
			ConvertTaskNeededByFinal[ConvertTaskIndex] = true;
			Compiled.FinalPoseResource = Compiled.PoseConvertTasks[ConvertTaskIndex].DstPoseResource;
			Compiled.FinalPoseType = EGIAG_AnimPinType::ComponentPose;
		}
	}

	// Build dependency edges based on pose inputs (v1: any connected input creates dependency).
	TArray<TArray<int32>> OutEdges;
	OutEdges.SetNum(Compiled.NumNodes);

	for (int32 DstNodeIdx = 0; DstNodeIdx < Compiled.NumNodes; ++DstNodeIdx)
	{
		const FGIAG_AnimCompiledNode& DstNode = Compiled.Nodes[DstNodeIdx];
		for (int32 InputPinIdx = 0; InputPinIdx < DstNode.NumInputPins; ++InputPinIdx)
		{
			const FGIAG_AnimOutputPinRef SrcPin = DstNode.InputSources[InputPinIdx];
			if (SrcPin.NodeIndex >= 0)
			{
				OutEdges[SrcPin.NodeIndex].Add(DstNodeIdx);
			}
		}
	}

	TopoSortOrDie(Compiled.NumNodes, OutEdges, Compiled.ExecOrder);

	TArray<int32> ExecOrderIndexByNode;
	ExecOrderIndexByNode.SetNumUninitialized(Compiled.NumNodes);
	for (int32 i = 0; i < Compiled.ExecOrder.Num(); ++i)
	{
		const int32 NodeIdx = Compiled.ExecOrder[i];
		check(NodeIdx >= 0 && NodeIdx < Compiled.NumNodes);
		ExecOrderIndexByNode[NodeIdx] = i;
	}
	for (int32 TaskIndex = 0; TaskIndex < Compiled.PoseConvertTasks.Num(); ++TaskIndex)
	{
		int32 FirstConsumerNodeIndex = INDEX_NONE;
		int32 BestOrder = TNumericLimits<int32>::Max();
		for (const int32 ConsumerNodeIndex : ConvertTaskConsumers[TaskIndex])
		{
			check(ConsumerNodeIndex >= 0 && ConsumerNodeIndex < ExecOrderIndexByNode.Num());
			const int32 Order = ExecOrderIndexByNode[ConsumerNodeIndex];
			if (Order < BestOrder)
			{
				BestOrder = Order;
				FirstConsumerNodeIndex = ConsumerNodeIndex;
			}
		}
		if (FirstConsumerNodeIndex == INDEX_NONE)
		{
			checkf(
				ConvertTaskNeededByFinal.IsValidIndex(TaskIndex) && ConvertTaskNeededByFinal[TaskIndex],
				TEXT("GIAG_AnimGraph: pose conversion task has no consumer (TaskIndex=%d)."),
				TaskIndex);
		}
		Compiled.PoseConvertTasks[TaskIndex].FirstConsumerNodeIndex = FirstConsumerNodeIndex;
	}

	TMap<int32, TArray<int32>> ConvertTaskIndicesByFirstConsumerNode;
	TArray<int32> FinalOnlyConvertTaskIndices;
	for (int32 TaskIndex = 0; TaskIndex < Compiled.PoseConvertTasks.Num(); ++TaskIndex)
	{
		const int32 FirstConsumer = Compiled.PoseConvertTasks[TaskIndex].FirstConsumerNodeIndex;
		if (FirstConsumer >= 0)
		{
			check(FirstConsumer < Compiled.NumNodes);
			ConvertTaskIndicesByFirstConsumerNode.FindOrAdd(FirstConsumer).Add(TaskIndex);
		}
		else
		{
			FinalOnlyConvertTaskIndices.Add(TaskIndex);
		}
	}

	// Dispatch schedule:
	// - node batches are grouped by node type
	// - pose conversion batches are inserted right before first consumer node
	Compiled.DispatchSchedule.Reset();
	FGIAG_AnimDispatchBatch CurrentNodeBatch;
	CurrentNodeBatch.Kind = EGIAG_AnimDispatchBatchKind::Node;
	auto FlushNodeBatch = [&]()
	{
		if (CurrentNodeBatch.NodeIndices.Num() > 0)
		{
			Compiled.DispatchSchedule.Add(MoveTemp(CurrentNodeBatch));
			CurrentNodeBatch = {};
			CurrentNodeBatch.Kind = EGIAG_AnimDispatchBatchKind::Node;
		}
	};

	for (const int32 NodeIdx : Compiled.ExecOrder)
	{
		if (TArray<int32>* ConvertTaskIndices = ConvertTaskIndicesByFirstConsumerNode.Find(NodeIdx))
		{
			FlushNodeBatch();
			FGIAG_AnimDispatchBatch ConvertBatch;
			ConvertBatch.Kind = EGIAG_AnimDispatchBatchKind::PoseSpaceConvert;
			ConvertBatch.ConvertTaskIndices = *ConvertTaskIndices;
			Compiled.DispatchSchedule.Add(MoveTemp(ConvertBatch));
		}

		const FName TypeId = Compiled.Nodes[NodeIdx].TypeId;
		if (CurrentNodeBatch.NodeIndices.Num() == 0)
		{
			CurrentNodeBatch.TypeId = TypeId;
			CurrentNodeBatch.NodeIndices.Add(NodeIdx);
		}
		else if (CurrentNodeBatch.TypeId == TypeId)
		{
			CurrentNodeBatch.NodeIndices.Add(NodeIdx);
		}
		else
		{
			FlushNodeBatch();
			CurrentNodeBatch.TypeId = TypeId;
			CurrentNodeBatch.NodeIndices.Add(NodeIdx);
		}
	}
	FlushNodeBatch();
	if (FinalOnlyConvertTaskIndices.Num() > 0)
	{
		FGIAG_AnimDispatchBatch ConvertBatch;
		ConvertBatch.Kind = EGIAG_AnimDispatchBatchKind::PoseSpaceConvert;
		ConvertBatch.ConvertTaskIndices = MoveTemp(FinalOnlyConvertTaskIndices);
		Compiled.DispatchSchedule.Add(MoveTemp(ConvertBatch));
	}

	// Reverse dispatch schedule: same nodes as reverse ExecOrder, grouped by TypeId.
	// Convert batches are intentionally excluded (cull propagation only traverses nodes).
	// This is used by CPU node cull propagation to traverse reverse topo order in batches.
	Compiled.ReverseDispatchSchedule.Reset();
	Compiled.ReverseDispatchSchedule.Reserve(Compiled.DispatchSchedule.Num());
	for (int32 BatchIdx = Compiled.DispatchSchedule.Num() - 1; BatchIdx >= 0; --BatchIdx)
	{
		const FGIAG_AnimDispatchBatch& Batch = Compiled.DispatchSchedule[BatchIdx];
		if (Batch.Kind != EGIAG_AnimDispatchBatchKind::Node || Batch.NodeIndices.Num() == 0)
		{
			continue;
		}
		FGIAG_AnimDispatchBatch DispatchBatch;
		DispatchBatch.Kind = EGIAG_AnimDispatchBatchKind::Node;
		DispatchBatch.TypeId = Batch.TypeId;
		DispatchBatch.NodeIndices = Batch.NodeIndices;
		Algo::Reverse(DispatchBatch.NodeIndices); // reverse within the batch for reverse topo traversal
		Compiled.ReverseDispatchSchedule.Add(MoveTemp(DispatchBatch));
	}

	// Precompute CPU cull helper tables (no per-frame scans/allocations).
	Compiled.NumInputPinsByNode.SetNumUninitialized(Compiled.NumNodes);
	for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
	{
		const int32 NumPins = Compiled.Nodes[NodeIdx].NumInputPins;
		check(NumPins >= 0 && NumPins <= 0xFFFF);
		Compiled.NumInputPinsByNode[NodeIdx] = (uint16)NumPins;
	}

	Compiled.CullIndexByNode.SetNumUninitialized(Compiled.NumNodes);
	for (int32 i = 0; i < Compiled.CullIndexByNode.Num(); ++i)
	{
		Compiled.CullIndexByNode[i] = INDEX_NONE;
	}
	Compiled.CullDispatchSchedule.Reset();
	Compiled.NumCullNodes = 0;
	Compiled.CullDispatchSchedule.Reserve(Compiled.DispatchSchedule.Num());
	for (const FGIAG_AnimDispatchBatch& Batch : Compiled.DispatchSchedule)
	{
		if (Batch.Kind != EGIAG_AnimDispatchBatchKind::Node)
		{
			continue;
		}
		if (Batch.NodeIndices.Num() == 0)
		{
			continue;
		}
		const int32 FirstNodeIdx = Batch.NodeIndices[0];
		const IGIAG_AnimNodeMeta* Meta = Compiled.Nodes[FirstNodeIdx].NodeMeta;
		check(Meta != nullptr);
		if (!Meta->HasCullLogic())
		{
			continue;
		}

		Compiled.CullDispatchSchedule.Add(Batch);
		for (const int32 NodeIdx : Batch.NodeIndices)
		{
			check(NodeIdx >= 0 && NodeIdx < Compiled.NumNodes);
			Compiled.CullIndexByNode[NodeIdx] = Compiled.NumCullNodes++;
		}
	}

	// Detect whether the graph has any nodes that support cull logic.
	Compiled.bHasNodeCullLogic = false;
	for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
	{
		const IGIAG_AnimNodeMeta* Meta = Compiled.Nodes[NodeIdx].NodeMeta;
		if (Meta && Meta->HasCullLogic())
		{
			Compiled.bHasNodeCullLogic = true;
			break;
		}
	}
	Compiled.bEnableNodeCull = bEnableNodeCull && Compiled.bHasNodeCullLogic;

	// Build per-node cull param buffer symbol list (strong-typed StructuredBuffer<ElementType>).
	// Each AnimGraph permutation gets its own HLSL resource declarations with these exact names.
	Compiled.CullParamNodeIndices.Reset();
	Compiled.CullParamSymbols.Reset();
	auto SanitizeIdent = [](const FString& In) -> FString
	{
		FString Out;
		Out.Reserve(In.Len() + 1);
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR Ch = In[i];
			const bool bOk =
				(Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
				(Ch >= TEXT('a') && Ch <= TEXT('z')) ||
				(Ch >= TEXT('0') && Ch <= TEXT('9')) ||
				(Ch == TEXT('_'));
			Out.AppendChar(bOk ? Ch : TEXT('_'));
		}
		if (Out.IsEmpty() || !((Out[0] >= TEXT('A') && Out[0] <= TEXT('Z')) || (Out[0] >= TEXT('a') && Out[0] <= TEXT('z')) || Out[0] == TEXT('_')))
		{
			Out = TEXT("_") + Out;
		}
		return Out;
	};
	for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
	{
		const FGIAG_AnimCompiledNode& Node = Compiled.Nodes[NodeIdx];
		const IGIAG_AnimNodeMeta* Meta = Node.NodeMeta;
		const bool bNeedsParams = Compiled.bEnableNodeCull && (Meta != nullptr) && Meta->HasCullLogic();
		if (!bNeedsParams)
		{
			continue;
		}

		// Keep a conservative hard cap to avoid exploding SRV count (D3D/VK typical limit: 64 SRVs per shader stage).
		checkf(Compiled.CullParamNodeIndices.Num() < (int32)GIAG::MaxGraphCullParamBuffers,
			TEXT("GIAG: exceeded MaxGraphCullParamBuffers=%u (NodeIdx=%d)."),
			(uint32)GIAG::MaxGraphCullParamBuffers,
			NodeIdx);

		FString DummyBody;
		const TCHAR* ElemType = nullptr;
		const TCHAR* MemberName = nullptr;
		Meta->EmitCullNeedMaskHlslBody(DummyBody, ElemType, MemberName);
		checkf(ElemType != nullptr && MemberName != nullptr,
			TEXT("GIAG: node type '%s' cull HLSL did not provide element type/member name."),
			*Node.TypeId.ToString());

		const FString NodeName = SanitizeIdent(Node.MemberName.ToString());
		const FString Suffix = SanitizeIdent(FString(MemberName));
		const FString Symbol = FString::Printf(TEXT("%s_%s_Params"), *NodeName, *Suffix);

		Compiled.CullParamNodeIndices.Add(NodeIdx);
		Compiled.CullParamSymbols.Add(Symbol);
	}

	// GraphCull shader specialization (Niagara-style): generate propagation code and cook/compile per platform.
	Compiled.GraphHash = 0;
	if (Compiled.bEnableNodeCull && Compiled.FinalPoseOutput.NodeIndex >= 0)
	{
		Compiled.GraphHash = HashGraphForCull(Compiled);
	}

	// NOTE: GraphCull HLSL is generated per-graph; no debug logging here (tests rely on deterministic output).

	GIAG::FGIAG_GraphCullShaderScript Script;
	Script.GraphHash = Compiled.GraphHash;
	Script.FriendlyName = GetPathName();
	Script.BindingVersion = 6;

	if (NeedsGraphCullCookData(Compiled))
	{
		const FString CullHlsl = GenerateGraphCullShaderSource(Compiled);
		Script.GeneratedHlsl = CullHlsl;
		Script.CullParamSymbols = Compiled.CullParamSymbols;
		Script.GeneratedCrc = FCrc::StrCrc32(*CullHlsl);
	}
	else
	{
		// Skip GraphCull: no supported nodes or user disabled.
		Compiled.GraphCullShaderMap.Reset();
		Compiled.CullParamSRVBaseIndices.Reset();
		bCompiled = true;
		return Compiled;
	}

	if (FPlatformProperties::RequiresCookedData())
	{
		// Cooked runtime: shader map must have been serialized into the asset for current shader format.
		const FName CurrentFormat = LegacyShaderPlatformToShaderFormat(GMaxRHIShaderPlatform);
		for (const FCookedGraphCullShaderMapEntry& Entry : CookedGraphCullShaderMaps)
		{
			if (Entry.ShaderFormat == CurrentFormat && Entry.ShaderMap.IsValid())
			{
				Compiled.GraphCullShaderMap = Entry.ShaderMap;
				break;
			}
		}
		checkf(Compiled.GraphCullShaderMap.IsValid(),
			TEXT("GIAG: missing cooked GraphCull ShaderMap for Format=%s (AnimGraph=%s)."),
			*CurrentFormat.ToString(),
			*GetPathName());
	}
	else
	{
		TRefCountPtr<FGIAGShaderMap> ShaderMap = GIAG::GIAG_GetOrCreateGraphCullShaderMap(
			Script,
			GMaxRHIShaderPlatform,
			/*TargetPlatform=*/nullptr,
			/*bAllowCompile=*/true);
		check(ShaderMap != nullptr);
		Compiled.GraphCullShaderMap = FGIAG_GraphCullShaderMapPtr(ShaderMap.GetReference());
	}

	// Binding model no longer relies on ParameterMap-derived base indices.
	Compiled.CullParamSRVBaseIndices.Reset();

	bCompiled = true;
	return Compiled;
}

#if WITH_EDITOR
void UGIAG_AnimGraph::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	bCompiled = false;
	Compiled = {};
}

void UGIAG_AnimGraph::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	Super::BeginCacheForCookedPlatformData(TargetPlatform);
	check(TargetPlatform);

	// Ensure graph topology is compiled (deterministic) and we have a stable GraphHash / symbol list.
	Compile();

	// If node culling is disabled (or no supported cull nodes), there is nothing to cook.
	if (!NeedsGraphCullCookData(Compiled))
	{
		CookedGraphCullShaderMaps.Reset();
		return;
	}

	GIAG::FGIAG_GraphCullShaderScript Script;
	Script.GraphHash = Compiled.GraphHash;
	Script.FriendlyName = GetPathName();
	Script.GeneratedHlsl = GenerateGraphCullShaderSource(Compiled);
	Script.CullParamSymbols = Compiled.CullParamSymbols;
	Script.GeneratedCrc = FCrc::StrCrc32(*Script.GeneratedHlsl);
	Script.BindingVersion = 6;

	TArray<FName> ShaderFormats;
	TargetPlatform->GetAllTargetedShaderFormats(ShaderFormats);

	CookedGraphCullShaderMaps.Reset();
	CookedGraphCullShaderMaps.Reserve(ShaderFormats.Num());

	for (const FName Format : ShaderFormats)
	{
		const EShaderPlatform Platform = ShaderFormatToLegacyShaderPlatform(Format);
		if (Platform == SP_NumPlatforms)
		{
			continue;
		}

		TRefCountPtr<FGIAGShaderMap> ShaderMap = GIAG::GIAG_GetOrCreateGraphCullShaderMap(
			Script,
			Platform,
			TargetPlatform,
			/*bAllowCompile=*/true);
		checkf(ShaderMap.IsValid(), TEXT("GIAG: failed to cook GraphCull ShaderMap (Format=%s AnimGraph=%s)."), *Format.ToString(), *GetPathName());

		ShaderMap->AssociateWithAsset(FName(*GetPathName()));

		FCookedGraphCullShaderMapEntry Entry;
		Entry.ShaderFormat = Format;
		Entry.ShaderMap = FGIAG_GraphCullShaderMapPtr(ShaderMap.GetReference());
		CookedGraphCullShaderMaps.Add(MoveTemp(Entry));
	}
}

void UGIAG_AnimGraph::ClearCachedCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	Super::ClearCachedCookedPlatformData(TargetPlatform);
	CookedGraphCullShaderMaps.Reset();
}

bool UGIAG_AnimGraph::IsCachedCookedPlatformDataLoaded(const ITargetPlatform* TargetPlatform)
{
	Compile();
	if (!NeedsGraphCullCookData(Compiled))
	{
		return true;
	}
	return CookedGraphCullShaderMaps.Num() > 0;
}
#endif // WITH_EDITOR

void UGIAG_AnimGraph::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	int32 NumEntries = CookedGraphCullShaderMaps.Num();

	Ar << NumEntries;
	if (Ar.IsLoading())
	{
		CookedGraphCullShaderMaps.SetNum(NumEntries);
	}

	for (int32 i = 0; i < NumEntries; ++i)
	{
		FCookedGraphCullShaderMapEntry& Entry = CookedGraphCullShaderMaps[i];
		Ar << Entry.ShaderFormat;

		bool bHasMap = Entry.ShaderMap.IsValid();
		Ar << bHasMap;

		if (bHasMap)
		{
			if (Ar.IsLoading())
			{
				Entry.ShaderMap = FGIAG_GraphCullShaderMapPtr(new FGIAGShaderMap());
			}

			const bool bOk = GIAG::GIAG_SerializeShaderMap(
				Ar,
				*Entry.ShaderMap,
				FName(*GetPathName()),
				FPlatformProperties::RequiresCookedData());
			if (Ar.IsSaving())
			{
				checkf(bOk, TEXT("GIAG: failed to serialize GraphCull ShaderMap (AnimGraph=%s Format=%s)."), *GetPathName(), *Entry.ShaderFormat.ToString());
			}
			else if (bOk)
			{
				Entry.ShaderMap->Register(Entry.ShaderMap->GetShaderPlatform());
			}
			else
			{
				UE_LOG(LogShaders, Warning, TEXT("GIAG: GraphCull ShaderMap deserialize failed; ignoring entry (AnimGraph=%s Format=%s)."),
					*GetPathName(), *Entry.ShaderFormat.ToString());
				Entry.ShaderMap.Reset();
			}
		}
		else if (Ar.IsLoading())
		{
			Entry.ShaderMap.Reset();
		}
	}
}


