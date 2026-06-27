# GameInstancedAnimationGraph 节点编写规则

<!-- markdownlint-disable MD010 -->

[中文](NewNodeRule_CN.md)|[English](NewNodeRule.md)|[框架总览](README_CN.md)

本文用于统一 `GameInstancedAnimationGraphNode` 中新节点的实现规范，目标是：

- CPU/GPU 双路径语义一致
- 参数与资源布局稳定
- 以契约式编程为主，不写冗余防御逻辑

---

## 快速上手：新增一个节点要做哪些事

这一节是实操入口；后续章节是对应的严格规则。

### 第 1 步：创建节点类型

在 `Source/GameInstancedAnimationGraphNode` 下新增：

- `Public/GIAG_XXXNode.h`
- `Private/GIAG_XXXNode.cpp`
- `Private/GIAG_XXXNode.ispc`（CPU 热路径需要 SIMD 时）
- `Shaders/Nodes/GIAG_XXXNode.usf`

在 `.cpp` 中注册：

```cpp
GIAG_REGISTER_ANIM_NODE(FGIAG_XXXNode);
```

可参考已实现节点：

- `FGIAG_RefPoseNode`
- `FGIAG_ClipPlayerNode`
- `FGIAG_LayerBlendNode`
- `FGIAG_AdditiveNode`
- `FGIAG_LookAtNode`

### 第 2 步：定义 pin、静态配置与运行时参数

建议先明确三件事：

- 输入/输出 pose pin 的空间：默认是 `LocalPose`，需要组件空间时显式实现 `GetInputPinType()` / `GetOutputPinType()`。
- 静态配置：放入 `FSettings`，通过 `Builder.AddNode(Node, Settings)` 进入编译数据。
- 运行时状态：放入节点实例成员，通过 `FindAnimNode<T>()` 拿到节点后修改，并在值变化时 `NodeRef.MarkDirty()`。

### 第 3 步：实现 CPU/GPU 执行逻辑

节点执行入口包括：

- `GatherUploadsGPU(...)`：把运行时参数暴露给 GPU 稀疏上传；没有上传数据时返回 `nullptr`。
- `AddPassesCPU(...)`：CPU 解算，优先使用和 shader 同构的数学语义。
- `AddPassesGPU(...)`：GPU RDG pass 调度，按 `Context.NodeIndices` 遍历同类型节点批次。

推荐先把 CPU 路径跑通，再补 GPU shader。GPU pass 参考现有节点的 `RDGDispatchTiling::ForEachChunk` 分块调度写法，不要手写一套和项目不一致的 dispatch 逻辑。

### 第 4 步：复制 CPU/GPU 透传模板

下面是最小可跑通的节点形态：**1 输入 Pose -> 1 输出 Pose，CPU/GPU 直接透传**。先让这套模板编译/运行通过，再往里替换成自己的算法、参数、optional resource、cull 或 ISPC。

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

#### 4.4 在图里接入最小节点

```cpp
const auto Default = Builder.AddNode(Instance.Default);
const auto Min = Builder.AddNode(Instance.Min);
Builder.Link(GIAG_PIN_OUT(Default, Out), GIAG_PIN_IN(Min, Base));
Builder.SetFinalPose(GIAG_PIN_OUT(Min, Out));
```

### 第 5 步：按需补扩展能力

按节点需求选择：

- CPU 热点逐骨/逐实例循环：补 `.ispc` 内核。
- 静态且可共享的数据：补 optional resource。
- 输入裁剪：同时实现 `ComputeCullNeedMaskCPU(...)` 与 `EmitCullNeedMaskHlslBody(...)`。
- 运行时控制：提供 `UBlueprintFunctionLibrary` 或脚本封装。

### 第 6 步：验证并继续替换算法

最小模板打通后，再逐步替换内容：

1. 先确认透传模板编译/运行通过。
2. 加运行时参数，并在变化时 `MarkDirty()`。
3. 静态配置进入 `FSettings`，不要每帧上传。
4. 在示例图或测试里验证 CPU/GPU 语义一致。

参考：

- `Source/InstancedAnimGraphExample/Public/GIAG_AnimGraphExample.h`
- `Source/InstancedAnimGraphExample/Private/GIAG_AnimGraphExample.cpp`

---

## 1. 总体原则

- 采用**契约式编程**：前置条件用 `check/checkf` 明确约束。
- 框架已保证的数据，不再做重复判空/兜底分支。
- 节点语义必须在 CPU/GPU 两条路径保持同构（公式、分支条件、参数含义一致）。
- 运行时热路径避免额外分配，优先复用现有缓存和批处理上下文。

---

## 1.1 计算空间契约（LocalPose / ComponentPose）

框架采用**pin 级空间契约**，不是节点内部“推断空间”：

- Pose pin 类型由 `EGIAG_AnimPinType` 声明（`LocalPose` / `ComponentPose`）
- 编译期在跨空间连线处自动插入 `PoseSpaceConvert`
- `FinalPose` 编译后会收敛到 `ComponentPose`

节点实现规范：

- 在 `GetInputPinType()` / `GetOutputPinType()` 中明确声明空间（默认为LocalPose）
- 节点内部只处理声明空间，不写额外 runtime 空间分支
- CPU 与 GPU 必须对同一 pin 空间语义完全一致

推荐做法：

- 纯骨骼局部运算（blend/additive/采样）优先 `LocalPose`
- IK/LookAt/Attach 等依赖全局骨骼关系的运算优先 `ComponentPose`
- 世界空间计算通过 `ComponentToWorldBySlot` 在节点末端完成，不把 World 作为 pose pin 空间扩散

---

## 2. 节点文件与注册

新增节点通常需要：

- C++ 头文件：
  - `Source/GameInstancedAnimationGraphNode/Public/GIAG_XXXNode.h`
- C++ 实现：
  - `Source/GameInstancedAnimationGraphNode/Private/GIAG_XXXNode.cpp`
- GPU Shader：
  - `Shaders/Nodes/GIAG_XXXNode.usf`

并在 `.cpp` 中注册：

- `GIAG_REGISTER_ANIM_NODE(FGIAG_XXXNode);`

---

## 3. 参数分层（强制）

节点参数分两层：

- **静态配置（编译期）**：放 `FSettings`
  - 例如骨骼名、轴定义、混合策略、掩码表等。
  - 通过 `Builder.AddNode(Node, Settings)` 进入编译数据。
- **运行时状态（实例态）**：放节点实例成员
  - 例如目标位置、开关状态、时间戳、动态权重。
  - 通过 Blueprint/脚本 API 修改，必要时 `NodeRef.MarkDirty()`。

规则：

- 不要把编译期不变的配置放到每帧上传参数里。
- 运行时上传结构体必须与 shader 对应结构严格同布局，建议 `static_assert(sizeof(...))`。

---

## 4. 节点执行接口

节点由 `TGIAG_AnimNodeMeta<T>` 自动适配，常见实现点：

- `static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)`
- `static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)`
- `GatherUploadsGPU(uint32& OutUploadStrideBytes) const`（没有 GPU 上传参数时返回 `nullptr`）

可选实现：

- `EnumerateResourceRequests(...)`
- `BuildResourceForGPU(...)`
- `BuildResourceForCPU(...)`
- `ComputeCullNeedMaskCPU(...)` + `EmitCullNeedMaskHlslBody(...)`（二者必须同时实现或同时不实现）

---

## 5. CPU 路径规范

- 以 `Context.NodeIndices` 为批次遍历入口。
- 输入输出视图必须先做契约检查（`check`）。
- 对 1 输入 1 输出节点，通常先做 Base->Out passthrough，再覆盖目标骨/目标数据。
- 使用现有公共数学语义（与 shader 保持一致），避免 CPU/GPU 分叉算法。

空间相关补充：

- `FGIAG_CPUPoseBufferView` 带 `PoseType`，建议在入口做 `checkf(InPose.PoseType == 期望类型, ...)`
- 不要在节点里手动做 Local<->Component 转换来“兼容”错误连线
- 如节点天然改变空间（少见），应通过 pin 类型表达，让编译器插入/复用转换

---

## 6. GPU 路径规范

- shader 放在 `Shaders/Nodes/`，通过模块映射 `/GameInstancedAnimationGraphNode/...` 引用。
- 每个批次节点按 `Context.NodeIndices` 逐个发 pass。
- 参数尽量走现有上下文资源：
  - `NodeParamSRVsPerNode`
  - `InputPosesPerNode/OutputPosesPerNode`
  - `WorldToComponentBySlotSRV`
  - `ActiveInstanceIndicesSRV`
  - `NeedNodeBitsSRV`
- 节点 cull 位图语义要兼容 `GIAG_IsNodeNeeded(...)`。

空间相关补充：

- shader 输入输出缓冲区按 pin 空间契约使用，不再引入 `InputPoseType` 之类运行时分支
- 若需要 `ComponentPose`，直接把 pin 声明为 `ComponentPose`
- 与 CPU 路径保持同构，避免 CPU/GPU 因空间处理差异造成一致性偏差

---

## 7. Optional Resource 规范

用于“静态且可共享”的资源（例如骨索引、每骨权重）：

- 在 `EnumerateResourceRequests` 声明 `Slot + ShareKey + Layout + Access`
- `BuildResourceForGPU/CPU` 构建对应资源
- `ShareKey` 必须包含会影响内容布局/值的所有因素
- GPU/CPU 资源都应可由同一语义配置推导，避免分叉

---

## 8. 节点 API 规范（Blueprint/脚本）

- 通过 `UBlueprintFunctionLibrary` 暴露运行时修改函数。
- 仅在值真正变化时才 `MarkDirty()`，避免无意义上传。
- 对“开关+过渡”类逻辑，推荐运行时只暴露 `SetEnabled`，Alpha 在节点内按时间计算。

---

## 9. Cull 逻辑契约

如果节点支持输入裁剪：

- CPU：`ComputeCullNeedMaskCPU(uint32 NumInputs)` 返回 bitmask
- HLSL：`EmitCullNeedMaskHlslBody(...)` 输出等价逻辑
- 两边必须语义一致，且必须同时存在

不支持 cull 的节点不实现上述接口。

---

## 10. 验证清单

新增节点后至少完成：

- 工程编译通过（Editor DebugGame）
- Shader 编译通过
- 示例图可挂载并运行
- CPU/GPU 关键语义一致（必要时补自动化测试）

---

## 11. ISPC 与 HLSL 同构实现（必读）

### 11.1 共享实现架构

本项目的数学同构不是“复制两份代码”，而是：

- 单一实现源：`Shaders/Common/Shared/GIAG_MathShared.ush`
- HLSL 包装：`Shaders/Common/GIAG_Math.ush`
- ISPC 包装：`Source/GameInstancedAnimationGraph/Public/GIAG_Math.isph`

约定：

- 包装层必须先定义类型和宏，再 include 共享文件
- 共享文件里只写语言无关实现（通过宏适配）

### 11.2 新增 ISPC 节点内核规则

- 文件位置：`Source/GameInstancedAnimationGraphNode/Private/GIAG_XXXNode.ispc`
- 必须 `#include "GIAG_AnimCommon.isph"`
- 导出函数必须用 `export`，并且参数使用裸数组/POD
- 节点 AoS 结构体（`FGIAG_XXXNode_ISPC`）必须显式对齐/补齐，和 C++ 实例布局一致
- 节点实例数据按 slot-indexed AoS 传入，索引链路是 `ActiveIndex -> SlotIndex -> BoneIndex -> LinearIndex`

最小骨架：

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

### 11.3 C++ 侧绑定规则

- 在节点 `.cpp` 引入：
  - `#include "GIAG_XXXNode.ispc.generated.h"`
- 必须做布局校验：
  - `static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), ...)`
  - `static_assert(sizeof(ispc::FGIAG_XXXNode_ISPC) == sizeof(FGIAG_XXXNode), ...)`
- `AddPassesCPU` 中传入 `Context.NodeData[NodeIdx]` 时，按 slot-indexed AoS 解释，禁止自行重排

调用示例：

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

### 11.4 HLSL/ISPC 同构校验清单

- 输入/输出索引公式一致：
  - `ActiveIndex -> SlotIndex -> BoneIndex -> LinearIndex`
- 分支一致：
  - 例如 `alpha<=0`、`alpha>=1`、null 权重、early return 条件
- 数值语义一致：
  - quat 归一化、符号对齐、nlerp/slerp 选择、clamp 规则
- 边界一致：
  - `SlotIndex` 范围检查、`NumBones*NumInstances` 总迭代范围

### 11.5 常见坑

- `.ispc` 结构体少了 padding，导致读取错位
- HLSL 改了公式，ISPC 未同步
- 没有 `static_assert`，布局问题到运行时才暴露
- 在 ISPC 里使用不适合 SIMD 的复杂对象/容器

### 11.6 模块配置说明

`GameInstancedAnimationGraphNode.Build.cs` 已包含：

- `PrivateIncludePaths.Add(.../Shaders/Common/Shared)`

用于让 ISPC 预处理器可见共享实现文件。新增节点一般不需要额外改 Build.cs。
