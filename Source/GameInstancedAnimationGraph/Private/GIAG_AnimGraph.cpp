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
	static uint64 HashGraphForCull(const FGIAG_AnimGraphCompiledData& C)
	{
		FSHA1 Sha;
		auto UpdateU32 = [&Sha](uint32 V) { Sha.Update((const uint8*)&V, sizeof(V)); };
		auto UpdateI32 = [&Sha](int32 V) { Sha.Update((const uint8*)&V, sizeof(V)); };
		auto UpdateStr = [&Sha](const FString& S)
		{
			FTCHARToUTF8 Utf8(*S);
			Sha.Update((const uint8*)Utf8.Get(), (uint32)Utf8.Length());
		};

		UpdateI32(C.NumNodes);
		UpdateI32(C.NumPoseResources);
		UpdateI32(C.ExecOrder.Num());

		for (int32 NodeIdx : C.ExecOrder) { UpdateI32(NodeIdx); }

		UpdateI32(C.FinalPoseOutput.NodeIndex);
		UpdateI32(C.FinalPoseOutput.PinIndex);

		for (int32 NodeIdx = 0; NodeIdx < C.NumNodes; ++NodeIdx)
		{
			const FGIAG_AnimCompiledNode& N = C.Nodes[NodeIdx];
			UpdateStr(N.TypeId.ToString());
			UpdateStr(N.MemberName.ToString());
			UpdateI32(N.NumInputPins);
			UpdateI32(N.NumOutputPins);
			UpdateU32((N.NodeMeta != nullptr && N.NodeMeta->HasCullLogic()) ? 1u : 0u);
			if (N.NodeMeta != nullptr && N.NodeMeta->HasCullLogic())
			{
				FString DummyBody;
				const TCHAR* ElemType = nullptr;
				const TCHAR* MemberName = nullptr;
				N.NodeMeta->EmitCullNeedMaskHlslBody(DummyBody, ElemType, MemberName);
				checkf(ElemType != nullptr && MemberName != nullptr,
					TEXT("GIAG: node type '%s' cull HLSL did not provide element type/member name."),
					*N.TypeId.ToString());
				UpdateStr(FString(ElemType));
				UpdateStr(FString(MemberName));
			}
			for (int32 Pin = 0; Pin < N.NumInputPins; ++Pin)
			{
				const int32 SrcNode = N.InputSources.IsValidIndex(Pin) ? N.InputSources[Pin].NodeIndex : INDEX_NONE;
				UpdateI32(SrcNode);
			}
		}

		Sha.Final();
		uint8 HashBytes[20];
		Sha.GetHash(HashBytes);
		uint64 Out = 0;
		FMemory::Memcpy(&Out, HashBytes, sizeof(uint64));
		return Out;
	}

	static FString GenerateGraphCullShaderSource(const FGIAG_AnimGraphCompiledData& C)
	{
		checkf(C.NumNodes <= 256, TEXT("GIAG: GraphCull generator supports up to 256 nodes (NumNodes=%d)."), C.NumNodes);

		FString Out;
		Out.Reserve(32 * 1024);

		auto SanitizeIdent = [](const FString& In) -> FString
		{
			FString R;
			R.Reserve(In.Len() + 1);
			for (int32 i = 0; i < In.Len(); ++i)
			{
				const TCHAR C = In[i];
				const bool bOk =
					(C >= TEXT('A') && C <= TEXT('Z')) ||
					(C >= TEXT('a') && C <= TEXT('z')) ||
					(C >= TEXT('0') && C <= TEXT('9')) ||
					(C == TEXT('_'));
				R.AppendChar(bOk ? C : TEXT('_'));
			}
			if (R.IsEmpty() || !((R[0] >= TEXT('A') && R[0] <= TEXT('Z')) || (R[0] >= TEXT('a') && R[0] <= TEXT('z')) || R[0] == TEXT('_')))
			{
				R = TEXT("_") + R;
			}
			return R;
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
		CullParamIndexByNode.SetNumUninitialized(C.NumNodes);
		for (int32 i = 0; i < C.NumNodes; ++i) { CullParamIndexByNode[i] = INDEX_NONE; }
		for (int32 i = 0; i < C.CullParamNodeIndices.Num(); ++i)
		{
			const int32 NodeIdx = C.CullParamNodeIndices[i];
			check(NodeIdx >= 0 && NodeIdx < C.NumNodes);
			CullParamIndexByNode[NodeIdx] = i;

			const FGIAG_AnimCompiledNode& Node = C.Nodes[NodeIdx];
			check(Node.NodeMeta != nullptr);
			FString DummyBody;
			const TCHAR* ElemType = nullptr;
			const TCHAR* MemberName = nullptr;
			Node.NodeMeta->EmitCullNeedMaskHlslBody(DummyBody, ElemType, MemberName);
			checkf(ElemType != nullptr,
				TEXT("GIAG: node type '%s' cull HLSL did not provide element type."),
				*Node.TypeId.ToString());
			checkf(C.CullParamSymbols.IsValidIndex(i), TEXT("GIAG: CullParamSymbols size mismatch."));
			const uint32 RegisterIndex = (uint32)GIAG::GraphCullParamSRVRegisterBase + (uint32)i;
			Out += FString::Printf(TEXT("StructuredBuffer<%s> %s : register(t%u);\n"),
				ElemType,
				*C.CullParamSymbols[i],
				RegisterIndex);
		}
		Out += TEXT("\n");

		// Per-node-type cull functions (generated once per type).
		{
			TSet<FName> EmittedTypes;
			for (int32 NodeIdx = 0; NodeIdx < C.NumNodes; ++NodeIdx)
			{
				const FGIAG_AnimCompiledNode& Node = C.Nodes[NodeIdx];
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

		auto EmitSet = [&Out](int32 Src)
		{
			const uint32 U = (uint32)Src;
			Out += FString::Printf(TEXT("\t\tWordsLocal[%u] |= 0x%08Xu;\n"), (U >> 5), (1u << (U & 31u)));
		};

		for (int32 i = C.ExecOrder.Num() - 1; i >= 0; --i)
		{
			const int32 NodeIdx = C.ExecOrder[i];
			check(NodeIdx >= 0 && NodeIdx < C.NumNodes);
			const uint32 U = (uint32)NodeIdx;
			const uint32 Word = (U >> 5);
			const uint32 Mask = (1u << (U & 31u));

			const FGIAG_AnimCompiledNode& Node = C.Nodes[NodeIdx];
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
					if (ParamIdx >= 0 && C.CullParamSymbols.IsValidIndex(ParamIdx))
					{
						ParamSymbol = FStringView(C.CullParamSymbols[ParamIdx]);
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
					if (Src >= 0) { EmitSet(Src); }
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

const FGIAG_AnimGraphCompiledData& UGIAG_AnimGraph::Compile()
{
	if (bCompiled)
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
		FGIAG_AnimCompiledNode& N = Compiled.Nodes[i];
		N.NodeMeta = NodeMetas[i];
		UScriptStruct* S = NodeMetas[i]->GetStruct();
		N.TypeId = S ? S->GetFName() : NAME_None;
		checkf(NodeMemberNames.Num() == Compiled.NumNodes, TEXT("GIAG_AnimGraph: NodeMemberNames size mismatch."));
		N.MemberName = NodeMemberNames[i];
		if (NodeSettings.Num() > 0)
		{
			checkf(NodeSettings.Num() == Compiled.NumNodes, TEXT("GIAG_AnimGraph: NodeSettings size mismatch."));
			N.Settings = NodeSettings[i];
		}
		checkf(NodeInstanceOffsets.Num() == Compiled.NumNodes, TEXT("GIAG_AnimGraph: NodeInstanceOffsets size mismatch."));
		N.InstanceDataOffset = NodeInstanceOffsets[i];
		checkf(N.InstanceDataOffset >= 0, TEXT("GIAG_AnimGraph: Node %d has invalid InstanceDataOffset (bind via Builder.AddNode(node))."), i);
		// Cache GPU upload stride for this node (used for cull-param binding even when no slots are dirty).
		{
			uint32 Stride = 0;
			const uint8* NodePtr = (const uint8*)Builder.DefaultInstance.GetMemory() + (int64)N.InstanceDataOffset;
			const void* Blob = NodeMetas[i]->GatherUploadsGPU(NodePtr, Stride);
			N.GpuUploadStrideBytes = (Blob != nullptr) ? Stride : 0u;
		}
		N.NumInputPins = NodeMetas[i]->GetNumInputPins();
		N.NumOutputPins = NodeMetas[i]->GetNumOutputPins();
		N.InputSources.SetNum(N.NumInputPins);
		N.InputPoseResources.SetNumZeroed(N.NumInputPins);
		N.OutputPoseResources.SetNumZeroed(N.NumOutputPins);
		for (int32 p = 0; p < N.NumInputPins; ++p)
		{
			N.InputSources[p] = { INDEX_NONE, INDEX_NONE };
			N.InputPoseResources[p] = INDEX_NONE;
		}
		for (int32 op = 0; op < N.NumOutputPins; ++op)
		{
			N.OutputPoseResources[op] = INDEX_NONE;
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
		checkf(SrcType == DstType, TEXT("GIAG_AnimGraph: pin type mismatch."));

		// Store input source.
		DstNode.InputSources[Link.ToInput.PinIndex] = Link.FromOutput;
	}

	// Allocate pose resources for pose-typed output pins. (v1: unique resource per output pose pin)
	Compiled.NumPoseResources = 0;
	for (int32 n = 0; n < Compiled.NumNodes; ++n)
	{
		FGIAG_AnimCompiledNode& Node = Compiled.Nodes[n];
		for (int32 op = 0; op < Node.NumOutputPins; ++op)
		{
			if (Node.NodeMeta->GetOutputPinType(op) == EGIAG_AnimPinType::Pose)
			{
				Node.OutputPoseResources[op] = Compiled.NumPoseResources++;
			}
		}
	}

	// Resolve pose input resources from connected output pins.
	for (int32 n = 0; n < Compiled.NumNodes; ++n)
	{
		FGIAG_AnimCompiledNode& Node = Compiled.Nodes[n];
		for (int32 ip = 0; ip < Node.NumInputPins; ++ip)
		{
			if (Node.NodeMeta->GetInputPinType(ip) != EGIAG_AnimPinType::Pose)
			{
				continue;
			}
			const FGIAG_AnimOutputPinRef SrcPin = Node.InputSources[ip];
			if (SrcPin.NodeIndex >= 0)
			{
				const FGIAG_AnimCompiledNode& SrcNode = Compiled.Nodes[SrcPin.NodeIndex];
				const int32 PoseRes = SrcNode.OutputPoseResources[SrcPin.PinIndex];
				check(PoseRes >= 0);
				Node.InputPoseResources[ip] = PoseRes;
			}
			else
			{
				Node.InputPoseResources[ip] = INDEX_NONE;
			}
		}
	}

	// Build dependency edges based on pose inputs (v1: any connected input creates dependency).
	TArray<TArray<int32>> OutEdges;
	OutEdges.SetNum(Compiled.NumNodes);

	for (int32 dst = 0; dst < Compiled.NumNodes; ++dst)
	{
		const FGIAG_AnimCompiledNode& Dst = Compiled.Nodes[dst];
		for (int32 ip = 0; ip < Dst.NumInputPins; ++ip)
		{
			const FGIAG_AnimOutputPinRef SrcPin = Dst.InputSources[ip];
			if (SrcPin.NodeIndex >= 0)
			{
				OutEdges[SrcPin.NodeIndex].Add(dst);
			}
		}
	}

	TopoSortOrDie(Compiled.NumNodes, OutEdges, Compiled.ExecOrder);

	// Dispatch schedule: group consecutive nodes with the same TypeId.
	Compiled.DispatchSchedule.Reset();
	for (int32 i = 0; i < Compiled.ExecOrder.Num(); ++i)
	{
		const int32 NodeIdx = Compiled.ExecOrder[i];
		const FName TypeId = Compiled.Nodes[NodeIdx].TypeId;
		if (Compiled.DispatchSchedule.Num() == 0 || Compiled.DispatchSchedule.Last().TypeId != TypeId)
		{
			FGIAG_AnimDispatchBatch B;
			B.TypeId = TypeId;
			B.NodeIndices = { NodeIdx };
			Compiled.DispatchSchedule.Add(MoveTemp(B));
		}
		else
		{
			Compiled.DispatchSchedule.Last().NodeIndices.Add(NodeIdx);
		}
	}

	// Reverse dispatch schedule: same nodes as reverse ExecOrder, grouped by TypeId.
	// This is used by CPU node cull propagation to traverse reverse topo order in batches.
	Compiled.ReverseDispatchSchedule.Reset();
	Compiled.ReverseDispatchSchedule.Reserve(Compiled.DispatchSchedule.Num());
	for (int32 BatchIdx = Compiled.DispatchSchedule.Num() - 1; BatchIdx >= 0; --BatchIdx)
	{
		const FGIAG_AnimDispatchBatch& B = Compiled.DispatchSchedule[BatchIdx];
		FGIAG_AnimDispatchBatch RB;
		RB.TypeId = B.TypeId;
		RB.NodeIndices = B.NodeIndices;
		Algo::Reverse(RB.NodeIndices); // reverse within the batch for reverse topo traversal
		Compiled.ReverseDispatchSchedule.Add(MoveTemp(RB));
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
	for (int32 i = 0; i < Compiled.CullIndexByNode.Num(); ++i) { Compiled.CullIndexByNode[i] = INDEX_NONE; }
	Compiled.CullDispatchSchedule.Reset();
	Compiled.NumCullNodes = 0;
	Compiled.CullDispatchSchedule.Reserve(Compiled.DispatchSchedule.Num());
	for (const FGIAG_AnimDispatchBatch& Batch : Compiled.DispatchSchedule)
	{
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

	// Final pose pin.
	Compiled.FinalPoseOutput = Builder.GetFinalPose();
	if (Compiled.FinalPoseOutput.NodeIndex >= 0)
	{
		check(Compiled.FinalPoseOutput.NodeIndex < Compiled.NumNodes);
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
			const TCHAR C = In[i];
			const bool bOk =
				(C >= TEXT('A') && C <= TEXT('Z')) ||
				(C >= TEXT('a') && C <= TEXT('z')) ||
				(C >= TEXT('0') && C <= TEXT('9')) ||
				(C == TEXT('_'));
			Out.AppendChar(bOk ? C : TEXT('_'));
		}
		if (Out.IsEmpty() || !((Out[0] >= TEXT('A') && Out[0] <= TEXT('Z')) || (Out[0] >= TEXT('a') && Out[0] <= TEXT('z')) || Out[0] == TEXT('_')))
		{
			Out = TEXT("_") + Out;
		}
		return Out;
	};
	for (int32 NodeIdx = 0; NodeIdx < Compiled.NumNodes; ++NodeIdx)
	{
		const FGIAG_AnimCompiledNode& N = Compiled.Nodes[NodeIdx];
		const IGIAG_AnimNodeMeta* Meta = N.NodeMeta;
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
		const TCHAR* MemberNameT = nullptr;
		Meta->EmitCullNeedMaskHlslBody(DummyBody, ElemType, MemberNameT);
		checkf(ElemType != nullptr && MemberNameT != nullptr,
			TEXT("GIAG: node type '%s' cull HLSL did not provide element type/member name."),
			*N.TypeId.ToString());

		const FString NodeName = SanitizeIdent(N.MemberName.ToString());
		const FString Suffix = SanitizeIdent(FString(MemberNameT));
		const FString Sym = FString::Printf(TEXT("%s_%s_Params"), *NodeName, *Suffix);

		Compiled.CullParamNodeIndices.Add(NodeIdx);
		Compiled.CullParamSymbols.Add(Sym);
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

	const bool bShouldBuildGraphCull = Compiled.bEnableNodeCull && (Compiled.FinalPoseOutput.NodeIndex >= 0);
	if (bShouldBuildGraphCull)
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
		for (const FCookedGraphCullShaderMapEntry& E : CookedGraphCullShaderMaps)
		{
			if (E.ShaderFormat == CurrentFormat && E.ShaderMap.IsValid())
			{
				Compiled.GraphCullShaderMap = E.ShaderMap;
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
void UGIAG_AnimGraph::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	Super::BeginCacheForCookedPlatformData(TargetPlatform);
	check(TargetPlatform);

	// Ensure graph topology is compiled (deterministic) and we have a stable GraphHash / symbol list.
	Compile();

	// If node culling is disabled (or no supported cull nodes), there is nothing to cook.
	if (!Compiled.bEnableNodeCull || Compiled.FinalPoseOutput.NodeIndex < 0)
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
	// If node culling is disabled, no cooked shader data is required.
	if (!bEnableNodeCull)
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


