# GameInstancedAnimationGraph Node Authoring Rules

<!-- markdownlint-disable MD010 -->

[English](NewNodeRule.md)|[中文](NewNodeRule_CN.md)|[Framework Overview](../README.md)

This document standardizes how new nodes should be implemented in `GameInstancedAnimationGraphNode`. The goals are:

- Keep CPU/GPU semantics consistent.
- Keep parameter and resource layouts stable.
- Prefer contract-style programming over redundant defensive fallbacks.

---

## Quick Start: What To Do When Adding a Node

This section is the practical entry point. Later sections describe the stricter rules behind it.

### Step 1: Create the Node Type

Add the following under `Source/GameInstancedAnimationGraphNode`:

- `Public/GIAG_XXXNode.h`
- `Private/GIAG_XXXNode.cpp`
- `Private/GIAG_XXXNode.ispc` when the CPU hot path benefits from SIMD
- `Shaders/Nodes/GIAG_XXXNode.usf`

Register the node in the `.cpp` file:

```cpp
GIAG_REGISTER_ANIM_NODE(FGIAG_XXXNode);
```

Existing nodes to use as references:

- `FGIAG_RefPoseNode`
- `FGIAG_ClipPlayerNode`
- `FGIAG_LayerBlendNode`
- `FGIAG_AdditiveNode`
- `FGIAG_LookAtNode`

### Step 2: Define Pins, Static Settings, and Runtime Parameters

Decide these first:

- Input/output pose pin space: the default is `LocalPose`; implement `GetInputPinType()` / `GetOutputPinType()` explicitly when a node needs component space.
- Static settings: put them in `FSettings`, then pass them through `Builder.AddNode(Node, Settings)` into compiled data.
- Runtime state: store it in node instance members, update it after finding the node with `FindAnimNode<T>()`, and call `NodeRef.MarkDirty()` when values change.

### Step 3: Implement CPU/GPU Execution

Node execution entry points include:

- `GatherUploadsGPU(...)`: exposes runtime parameters for sparse GPU upload; return `nullptr` when there is no upload data.
- `AddPassesCPU(...)`: CPU solving, preferably using math semantics that match the shader.
- `AddPassesGPU(...)`: GPU RDG pass scheduling, iterating same-type node batches through `Context.NodeIndices`.

A practical workflow is to get the CPU path running first, then add the GPU shader. For GPU passes, follow the existing `RDGDispatchTiling::ForEachChunk` pattern instead of writing a dispatch style that diverges from the project.

### Step 4: Copy the CPU/GPU Passthrough Template

The following is the smallest runnable node shape: **1 input Pose -> 1 output Pose, direct passthrough on both CPU and GPU**. Get this compiling and running first, then replace the passthrough with your own algorithm, parameters, optional resource, cull logic, or ISPC kernel.

#### 4.1 `Public/GIAG_MinNode.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimNodeBase.h"
#include "GIAG_MinNode.generated.h"

USTRUCT(BlueprintType)
struct alignas(16) GAMEINSTANCEDANIMATIONGRAPHNODE_API FGIAG_MinNode final : public FGIAG_AnimNodeBase
{
	GENERATED_BODY()
public:
	using FNodeMeta = TGIAG_AnimNodeMeta<FGIAG_MinNode>;

	enum class EInputPin : uint8
	{
		Base = 0,
		Num,
	};

	static EGIAG_AnimPinType GetInputPinType(int32 PinIndex)
	{
		check(PinIndex == (int32)EInputPin::Base);
		return EGIAG_AnimPinType::LocalPose;
	}

	static EGIAG_AnimPinType GetOutputPinType(int32 PinIndex)
	{
		check(PinIndex == (int32)EOutputPin::Out);
		return EGIAG_AnimPinType::LocalPose;
	}

protected:
	friend FNodeMeta;

	const void* GatherUploadsGPU(uint32& OutUploadStrideBytes) const
	{
		OutUploadStrideBytes = 0;
		return nullptr;
	}

	static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context);
	static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context);
};
```

#### 4.2 `Private/GIAG_MinNode.cpp`

```cpp
#include "GIAG_MinNode.h"

#include "GIAG_AnimNodeMetaManager.h"
#include "GIAG_RdgDispatchTiling.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

GIAG_REGISTER_ANIM_NODE(FGIAG_MinNode);

namespace
{
	class FGIAG_PoseMinCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGIAG_PoseMinCS);
		SHADER_USE_PARAMETER_STRUCT(FGIAG_PoseMinCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumBones)
			SHADER_PARAMETER(uint32, NumInstances)
			SHADER_PARAMETER(uint32, DispatchGroupCountX)
			SHADER_PARAMETER(uint32, DispatchGroupCountY)
			SHADER_PARAMETER(uint32, DispatchGroupOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint32>, ActiveInstanceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, BasePose)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_BoneTRS>, RW_OutPose)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
			OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
			OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseMinCS, "/GameInstancedAnimationGraphNode/GIAG_MinNode.usf", "Main", SF_Compute);

	static void AddPoseMinPass(
		FRDGBuilder& GraphBuilder,
		uint32 NumBones,
		uint32 NumInstances,
		FRDGBufferSRVRef ActiveInstanceIndices,
		FRDGBufferSRVRef BasePose,
		FRDGBufferUAVRef RW_OutPose)
	{
		FGIAG_PoseMinCS::FParameters* BaseParameters = GraphBuilder.AllocParameters<FGIAG_PoseMinCS::FParameters>();
		BaseParameters->NumBones = NumBones;
		BaseParameters->NumInstances = NumInstances;
		BaseParameters->ActiveInstanceIndices = ActiveInstanceIndices;
		BaseParameters->BasePose = BasePose;
		BaseParameters->RW_OutPose = RW_OutPose;

		TShaderMapRef<FGIAG_PoseMinCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		constexpr int32 ThreadsPerGroup = 64;
		const int64 TotalWorkItems = (int64)NumBones * (int64)NumInstances;
		GIAG::RDGDispatchTiling::ForEachChunk(
			TotalWorkItems,
			ThreadsPerGroup,
			[&](int32, int32 GroupOffset1D, const FIntVector& GroupCount)
			{
				FGIAG_PoseMinCS::FParameters* ChunkParameters = GraphBuilder.AllocParameters<FGIAG_PoseMinCS::FParameters>();
				*ChunkParameters = *BaseParameters;
				ChunkParameters->DispatchGroupCountX = (uint32)GroupCount.X;
				ChunkParameters->DispatchGroupCountY = (uint32)GroupCount.Y;
				ChunkParameters->DispatchGroupOffset = (uint32)GroupOffset1D;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GIAG_Min"),
					ChunkParameters,
					ERDGPassFlags::Compute,
					[ChunkParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *ChunkParameters, GroupCount);
					});
			});
	}
}

void FGIAG_MinNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		AddPoseMinPass(
			Context.GraphBuilder,
			(uint32)Context.NumBones,
			(uint32)Context.NumInstances,
			Context.ActiveInstanceIndicesSRV,
			Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base].SRV,
			Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out].UAV);
	}
}

void FGIAG_MinNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	check(Context.InputPosesPerNode.Num() == Context.NodeIndices.Num());
	check(Context.OutputPosesPerNode.Num() == Context.NodeIndices.Num());

	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const FGIAG_CPUPoseBufferView BasePose = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base];
		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(BasePose.IsValid() && OutPose.IsValid());
		check(BasePose.PoseType == EGIAG_AnimPinType::LocalPose);
		check(OutPose.PoseType == EGIAG_AnimPinType::LocalPose);

		for (const int32 SlotIndex : Context.ActiveInstanceIndices)
		{
			check(SlotIndex >= 0 && SlotIndex < Context.SlotCapacity);
			FMemory::Memcpy(
				&OutPose.At(SlotIndex, 0),
				&BasePose.At(SlotIndex, 0),
				sizeof(FGIAG_BoneTRS) * (SIZE_T)Context.NumBones);
		}
	}
}
```

#### 4.3 `Shaders/Nodes/GIAG_MinNode.usf`

```hlsl
#include "/Engine/Public/Platform.ush"
#include "/GameInstancedAnimationGraphShader/GIAG_AnimCommon.ush"

uint NumBones;
uint NumInstances;
uint DispatchGroupCountX;
uint DispatchGroupCountY;
uint DispatchGroupOffset;

StructuredBuffer<uint> ActiveInstanceIndices;
StructuredBuffer<FGIAG_BoneTRS> BasePose;
RWStructuredBuffer<FGIAG_BoneTRS> RW_OutPose;

[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]
void Main(uint3 GroupId : SV_GroupID, uint GroupIndex : SV_GroupIndex)
{
	uint GroupLinear =
		DispatchGroupOffset +
		GroupId.x +
		GroupId.y * DispatchGroupCountX +
		GroupId.z * (DispatchGroupCountX * DispatchGroupCountY);
	uint DispatchId = GroupLinear * (THREADS_X * THREADS_Y * THREADS_Z) + GroupIndex;
	uint Total = NumBones * NumInstances;
	if (DispatchId >= Total)
	{
		return;
	}

	uint ActiveIndex = DispatchId / NumBones;
	uint BoneIndex = DispatchId - ActiveIndex * NumBones;
	uint SlotIndex = ActiveInstanceIndices[ActiveIndex];
	uint Index = SlotIndex * NumBones + BoneIndex;

	RW_OutPose[Index] = BasePose[Index];
}
```

#### 4.4 Wire the Minimal Node into a Graph

```cpp
const auto Default = Builder.AddNode(Instance.Default);
const auto Min = Builder.AddNode(Instance.Min);
Builder.Link(GIAG_PIN_OUT(Default, Out), GIAG_PIN_IN(Min, Base));
Builder.SetFinalPose(GIAG_PIN_OUT(Min, Out));
```

### Step 5: Add Extension Points as Needed

Choose based on node requirements:

- CPU hot loops over bones or instances: add an `.ispc` kernel.
- Static and shareable data: add optional resources.
- Input culling: implement both `ComputeCullNeedMaskCPU(...)` and `EmitCullNeedMaskHlslBody(...)`.
- Runtime control: expose a `UBlueprintFunctionLibrary` or script wrapper.

### Step 6: Validate and Replace the Passthrough

After the minimal template works, replace it incrementally:

1. Confirm the passthrough template compiles and runs.
2. Add runtime parameters, and call `MarkDirty()` when values change.
3. Put static configuration into `FSettings`; do not upload it every frame.
4. Validate CPU/GPU semantic consistency in an example graph or test.

References:

- `Source/InstancedAnimGraphExample/Public/GIAG_AnimGraphExample.h`
- `Source/InstancedAnimGraphExample/Private/GIAG_AnimGraphExample.cpp`

---

## 1. General Principles

- Use **contract-style programming**: express preconditions with `check/checkf`.
- Do not repeat null checks or fallback branches for data already guaranteed by the framework.
- Node semantics must stay isomorphic across CPU/GPU paths, including formulas, branch conditions, and parameter meanings.
- Avoid extra allocations on runtime hot paths; prefer existing caches and batch contexts.

---

## 1.1 Pose-Space Contract (LocalPose / ComponentPose)

The framework uses a **pin-level pose-space contract**, not node-internal "space inference":

- Pose pin types are declared with `EGIAG_AnimPinType` (`LocalPose` / `ComponentPose`).
- The compiler inserts `PoseSpaceConvert` tasks for cross-space links.
- `FinalPose` converges to `ComponentPose` after compilation.

Node implementation rules:

- Declare each pose pin space in `GetInputPinType()` / `GetOutputPinType()`; the default is `LocalPose`.
- A node only computes in its declared space; do not add extra runtime space branches inside the node.
- CPU and GPU must use exactly the same pin-space semantics.

Recommendations:

- Pure local-bone operations, such as blend, additive, and sampling, should usually use `LocalPose`.
- IK, LookAt, Attach, and similar operations that depend on global bone relationships should usually use `ComponentPose`.
- World-space calculations should use `ComponentToWorldBySlot` near the end of the node; do not propagate world space as a pose pin space.

---

## 2. Node Files and Registration

New nodes usually include:

- C++ header:
  - `Source/GameInstancedAnimationGraphNode/Public/GIAG_XXXNode.h`
- C++ implementation:
  - `Source/GameInstancedAnimationGraphNode/Private/GIAG_XXXNode.cpp`
- GPU shader:
  - `Shaders/Nodes/GIAG_XXXNode.usf`

Register the node in the `.cpp` file:

- `GIAG_REGISTER_ANIM_NODE(FGIAG_XXXNode);`

---

## 3. Parameter Layering

Node parameters are split into two layers:

- **Static configuration (compile time)**: put it in `FSettings`.
  - Examples: bone names, axis definitions, blend strategies, mask tables.
  - Pass it into compiled data through `Builder.AddNode(Node, Settings)`.
- **Runtime state (instance state)**: store it as node instance members.
  - Examples: target location, enabled state, timestamps, dynamic weights.
  - Modify it through Blueprint/script APIs, and call `NodeRef.MarkDirty()` when needed.

Rules:

- Do not put compile-time-stable configuration into per-frame upload parameters.
- Runtime upload structs must match the corresponding shader layout exactly; `static_assert(sizeof(...))` is recommended.

---

## 4. Node Execution Interfaces

Nodes are adapted automatically by `TGIAG_AnimNodeMeta<T>`. Common implementation points are:

- `static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)`
- `static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)`
- `GatherUploadsGPU(uint32& OutUploadStrideBytes) const` (return `nullptr` when there are no GPU upload parameters)

Optional implementation points:

- `EnumerateResourceRequests(...)`
- `BuildResourceForGPU(...)`
- `BuildResourceForCPU(...)`
- `ComputeCullNeedMaskCPU(...)` + `EmitCullNeedMaskHlslBody(...)` (they must both exist or both be omitted)

---

## 5. CPU Path Rules

- Iterate batches through `Context.NodeIndices`.
- Check input/output views first (`check`).
- For one-input, one-output nodes, a common pattern is to copy Base -> Out first, then overwrite target bones or target data.
- Use shared math semantics that match the shader; avoid divergent CPU/GPU algorithms.

Space-related notes:

- `FGIAG_CPUPoseBufferView` carries `PoseType`; check expected types at entry, for example `checkf(InPose.PoseType == ExpectedType, ...)`.
- Do not manually convert Local <-> Component inside a node to "support" invalid links.
- If a node naturally changes pose space, express that through pin types so the compiler can insert or reuse conversions.

---

## 6. GPU Path Rules

- Put shaders under `Shaders/Nodes/`, referenced through the `/GameInstancedAnimationGraphNode/...` module mapping.
- For each node batch, issue passes by iterating `Context.NodeIndices`.
- Prefer existing context resources:
  - `NodeParamSRVsPerNode`
  - `InputPosesPerNode/OutputPosesPerNode`
  - `WorldToComponentBySlotSRV`
  - `ActiveInstanceIndicesSRV`
  - `NeedNodeBitsSRV`
- Node cull bit semantics must remain compatible with `GIAG_IsNodeNeeded(...)`.

Space-related notes:

- Shader input/output buffers follow the pin-space contract; do not introduce runtime branches such as `InputPoseType`.
- If the node needs `ComponentPose`, declare the pin as `ComponentPose`.
- Keep GPU behavior isomorphic to the CPU path to avoid consistency drift from space handling differences.

---

## 7. Optional Resource Rules

Optional resources are for **static and shareable** data, such as bone indices or per-bone weights:

- Declare `Slot + ShareKey + Layout + Access` in `EnumerateResourceRequests`.
- Build the corresponding resources in `BuildResourceForGPU/CPU`.
- `ShareKey` must include every factor that affects content layout or values.
- GPU and CPU resources should be derived from the same semantic configuration to avoid divergent behavior.

---

## 8. Node API Rules (Blueprint / Script)

- Expose runtime modification functions through `UBlueprintFunctionLibrary`.
- Call `MarkDirty()` only when the value actually changes, avoiding meaningless uploads.
- For "enabled + transition" logic, prefer exposing only `SetEnabled` at runtime and let the node compute Alpha by time internally.

---

## 9. Cull Logic Contract

If a node supports input culling:

- CPU: `ComputeCullNeedMaskCPU(uint32 NumInputs)` returns a bitmask.
- HLSL: `EmitCullNeedMaskHlslBody(...)` emits equivalent logic.
- Both sides must have the same semantics, and both must exist together.

Nodes that do not support culling do not implement those interfaces.

---

## 10. Validation Checklist

After adding a node, at least verify:

- The project compiles (Editor DebugGame).
- Shaders compile.
- The node can be mounted and run in an example graph.
- Key CPU/GPU semantics match; add automation tests when needed.

---

## 11. ISPC and HLSL Isomorphic Implementation

### 11.1 Shared Implementation Architecture

The project does not duplicate math logic in two separate implementations. Instead, it uses:

- Single implementation source: `Shaders/Common/Shared/GIAG_MathShared.ush`
- HLSL wrapper: `Shaders/Common/GIAG_Math.ush`
- ISPC wrapper: `Source/GameInstancedAnimationGraph/Public/GIAG_Math.isph`

Conventions:

- Wrappers define types and macros first, then include the shared file.
- The shared file contains language-neutral implementation, adapted through macros.

### 11.2 Adding an ISPC Node Kernel

- File location: `Source/GameInstancedAnimationGraphNode/Private/GIAG_XXXNode.ispc`
- Must include `#include "GIAG_AnimCommon.isph"`
- Exported functions must use `export`, and parameters should be raw arrays/POD.
- The node AoS struct (`FGIAG_XXXNode_ISPC`) must be explicitly aligned/padded to match the C++ instance layout.
- Node instance data is passed as slot-indexed AoS; the index chain is `ActiveIndex -> SlotIndex -> BoneIndex -> LinearIndex`.

Minimal skeleton:

```cpp
#include "GIAG_AnimCommon.isph"

struct FGIAG_XXXNode_ISPC
{
	float Alpha;
	unsigned int32 _pad0;
	unsigned int32 _pad1;
	unsigned int32 _pad2;
};

export void GIAG_XXXKernel(
	uniform int NumBones,
	uniform int NumInstances,
	uniform int SlotCapacity,
	const uniform unsigned int32 ActiveInstanceIndices[],
	const uniform FGIAG_XXXNode_ISPC NodesBySlot[],
	const uniform FGIAG_BoneTRS InPose[],
	uniform FGIAG_BoneTRS OutPose[])
{
	const uniform int Total = NumBones * NumInstances;
	foreach (DispatchId = 0 ... Total)
	{
		int ActiveIndex = DispatchId / NumBones;
		int BoneIndex = DispatchId - ActiveIndex * NumBones;
		int SlotIndex = (int)ActiveInstanceIndices[ActiveIndex];
		if (SlotIndex < 0 || SlotIndex >= SlotCapacity)
		{
			continue;
		}

		int Index = SlotIndex * NumBones + BoneIndex;
		OutPose[Index] = InPose[Index];
	}
}
```

### 11.3 C++ Binding Rules

- Include the generated header in the node `.cpp`:
  - `#include "GIAG_XXXNode.ispc.generated.h"`
- Validate layouts:
  - `static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), ...)`
  - `static_assert(sizeof(ispc::FGIAG_XXXNode_ISPC) == sizeof(FGIAG_XXXNode), ...)`
- When passing `Context.NodeData[NodeIdx]` in `AddPassesCPU`, interpret it as slot-indexed AoS. Do not reorder it yourself.

Example call:

```cpp
#include "GIAG_XXXNode.ispc.generated.h"
static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), "GIAG ISPC: FGIAG_BoneTRS layout mismatch.");
static_assert(sizeof(ispc::FGIAG_XXXNode_ISPC) == sizeof(FGIAG_XXXNode), "GIAG ISPC: FGIAG_XXXNode layout mismatch.");

void FGIAG_XXXNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	for (int32 NodeIndexInBatch = 0; NodeIndexInBatch < Context.NodeIndices.Num(); ++NodeIndexInBatch)
	{
		const int32 NodeIdx = Context.NodeIndices[NodeIndexInBatch];
		const FGIAG_CPUPoseBufferView InPose = Context.InputPosesPerNode[NodeIndexInBatch][(uint8)EInputPin::Base];
		const FGIAG_CPUPoseBufferView OutPose = Context.OutputPosesPerNode[NodeIndexInBatch][(uint8)EOutputPin::Out];
		check(InPose.IsValid() && OutPose.IsValid());

		ispc::GIAG_XXXKernel(
			Context.NumBones,
			Context.NumInstances,
			Context.SlotCapacity,
			(const uint32*)Context.ActiveInstanceIndices.GetData(),
			(const ispc::FGIAG_XXXNode_ISPC*)Context.NodeData[NodeIdx],
			(const ispc::FGIAG_BoneTRS*)InPose.Data,
			(ispc::FGIAG_BoneTRS*)OutPose.Data);
	}
}
```

### 11.4 HLSL/ISPC Isomorphism Checklist

- Input/output indexing formula is the same:
  - `ActiveIndex -> SlotIndex -> BoneIndex -> LinearIndex`
- Branches are the same:
  - For example `alpha<=0`, `alpha>=1`, null weights, early return conditions.
- Numeric semantics are the same:
  - Quaternion normalization, sign alignment, nlerp/slerp choice, clamp rules.
- Boundaries are the same:
  - `SlotIndex` range checks and `NumBones*NumInstances` total iteration range.

### 11.5 Common Pitfalls

- Missing padding in `.ispc` structs, causing misaligned reads.
- Updating a formula in HLSL without updating ISPC.
- Missing `static_assert`, so layout issues only show up at runtime.
- Using complex objects or containers inside ISPC that are unsuitable for SIMD.

### 11.6 Module Configuration Note

`GameInstancedAnimationGraphNode.Build.cs` already contains:

- `PrivateIncludePaths.Add(.../Shaders/Common/Shared)`

This lets the ISPC preprocessor see the shared implementation files. New nodes usually do not need additional `Build.cs` changes.
