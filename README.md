# GameInstancedAnimationGraph

`GameInstancedAnimationGraph` 是一个支持**批量实例动画**的动画图框架。

---

## 你会得到什么

- 一个可在 AnimGraph 中拼接的节点类型（可 CPU/GPU 双路径）
- 可选的运行时控制 API（蓝图/脚本）
- 可选的静态资源（按骨架缓存）

---

## 快速上手：新增一个节点要做哪些事

### 第 1 步：创建节点类型

在 `Source/GameInstancedAnimationGraphNode` 下新增：

- `Public/GIAG_XXXNode.h`
- `Private/GIAG_XXXNode.cpp`
- `Shaders/Nodes/GIAG_XXXNode.usf`（如果要走 GPU）

在 `.cpp` 注册：

- `GIAG_REGISTER_ANIM_NODE(FGIAG_XXXNode);`

可参考已实现节点：

- `GIAG_ClipPlayerNode`
- `GIAG_LayerBlendNode`
- `GIAG_AdditiveNode`
- `GIAG_LookAtNode`

---

### 第 2 步：定义参数

建议分成两类：

- **静态配置**（建图时给定）  
  用 `FSettings`（例如骨骼名、轴、掩码、混合时长）
- **运行时参数**（每实例可改）  
  放在节点实例里（例如目标位置、开关、动态权重）

这样你在图里是：

- `Builder.AddNode(Instance.MyNode, MySettings)`（带静态配置）
- 运行时通过 `FindAnimNode<T>()` + 节点函数去改动态值

---

### 第 3 步：实现执行逻辑

通常最少需要：

- `GatherUploadsGPU(...)`：把运行时参数打包上传（若有 GPU）
- `AddPassesCPU(...)`：CPU 解算
- `AddPassesGPU(...)`：GPU pass 调度

建议做法：

- 先把 CPU 路径跑通（逻辑更容易调试）
- 再实现 GPU，保持与 CPU 同语义

---

### 第 3.5 步：给 CPU 路径加 ISPC（可选，但推荐）

如果你的节点 CPU 计算比较重（逐骨/逐实例循环），建议加 `.ispc` 内核。

先理解这个项目里 HLSL/ISPC 的同构方式：

- 真正的共享数学实现放在：
  - `Shaders/Common/Shared/GIAG_MathShared.ush`
- HLSL 包装层：
  - `Shaders/Common/GIAG_Math.ush`
- ISPC 包装层：
  - `Source/GameInstancedAnimationGraph/Public/GIAG_Math.isph`

也就是说：**HLSL 与 ISPC 不是各写一套数学函数，而是共享同一份实现文件**。

#### 3.5.1 新增 `.ispc` 文件

例如：

- `Source/GameInstancedAnimationGraphNode/Private/GIAG_XXXNode.ispc`

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
		if (SlotIndex < 0 || SlotIndex >= SlotCapacity) continue;

		int Index = SlotIndex * NumBones + BoneIndex;
		OutPose[Index] = InPose[Index];
	}
}
```

#### 3.5.2 在节点 `.cpp` 中调用生成头

```cpp
#include "GIAG_XXXNode.ispc.generated.h"
static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), "layout mismatch");
static_assert(sizeof(ispc::FGIAG_XXXNode_ISPC) == sizeof(FGIAG_XXXNode), "node layout mismatch");

// AddPassesCPU(...)
ispc::GIAG_XXXKernel(
	Context.NumBones,
	Context.NumInstances,
	Context.SlotCapacity,
	(const uint32*)Context.ActiveInstanceIndices.GetData(),
	(const ispc::FGIAG_XXXNode_ISPC*)Context.NodeData[NodeIdx],
	(const ispc::FGIAG_BoneTRS*)InPose.Data,
	(ispc::FGIAG_BoneTRS*)OutPose.Data);
```

#### 3.5.3 你只需要关心这 3 条

- `.ispc` 内的节点结构体必须和 C++ 节点实例内存布局一致（含 padding）
- 只处理 POD 数据，不在 ISPC 内使用 UObject/容器
- 算法语义与 GPU shader 保持一致（分支和公式尽量同构）

> 说明：当前模块已在 `GameInstancedAnimationGraphNode.Build.cs` 配好 `Shaders/Common/Shared` 的 include path，ISPC 可直接包含共享实现链路。

---

### 第 4 步：把节点接进图

在你的图类（`UGIAG_AnimGraph` 子类）里：

1. 在 `DefaultGraphInstance` 增加节点成员（`UPROPERTY`）
2. 在 `BuildGraph(...)` 中 `AddNode/Link/SetFinalPose`

参考：

- `Source/InstancedAnimGraphExample/Public/GIAG_AnimGraphExample.h`
- `Source/InstancedAnimGraphExample/Private/GIAG_AnimGraphExample.cpp`

---

### 第 5 步：提供运行时控制（可选）

如果节点需要在游戏中动态控制，建议提供 `UBlueprintFunctionLibrary` 封装：

- `SetEnabled(...)`
- `SetTarget...(...)`
- `SetAlpha(...)`

调用链通常是：

1. `Subsystem->FindAnimNode<FGIAG_XXXNode>(Handle, "NodeMemberName")`
2. 调节点实例函数更新状态

---

## 推荐开发顺序（实践上最快）

1. 先定义数据结构和 pin（输入/输出）
2. 先实现 CPU 路径并验证行为
3. 再实现 GPU shader 与 pass
4. 接入示例图做端到端联调
5. 最后补自动化测试

---

## 验证建议

- 编译：先保证 Editor DebugGame 编译通过
- 行为：在示例图里动态改参数，确认输出符合预期
- 一致性：CPU/GPU 同场景对比（必要时加自动化测试）

---

## 进阶与实现细节

如果你要看“严格规则/契约细节”（比如 cull、optional resource、布局要求），请看：

- `Plugins/GameInstancedAnimationGraph/Docs/NewNodeRule.md`

---

## 5 分钟最小节点模板

下面这套模板是最小可运行骨架：**1 输入 Pose -> 1 输出 Pose，CPU 直接透传，GPU 也透传**。  
你可以先复制通，再往里加自己的算法。

### 1) `Public/GIAG_MinNode.h`

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

### 2) `Private/GIAG_MinNode.cpp`

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
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGIAG_BoneTRS>, BasePose)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIAG_BoneTRS>, RW_OutPose)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters&, FShaderCompilerEnvironment& OutEnvironment)
		{
			OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
			OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
			OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FGIAG_PoseMinCS, "/GameInstancedAnimationGraphNode/GIAG_MinNode.usf", "Main", SF_Compute);
}

void FGIAG_MinNode::AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)
{
	for (int32 NodeInBatch = 0; NodeInBatch < Context.NodeIndices.Num(); ++NodeInBatch)
	{
		FGIAG_PoseMinCS::FParameters* P = Context.GraphBuilder.AllocParameters<FGIAG_PoseMinCS::FParameters>();
		P->NumBones = (uint32)Context.NumBones;
		P->NumInstances = (uint32)Context.NumInstances;
		P->DispatchGroupCountX = 0; // 按你项目现有 RDGDispatchTiling 封装补齐
		P->DispatchGroupCountY = 0;
		P->DispatchGroupOffset = 0;
		P->BasePose = Context.InputPosesPerNode[NodeInBatch][(uint8)EInputPin::Base].SRV;
		P->RW_OutPose = Context.OutputPosesPerNode[NodeInBatch][(uint8)EOutputPin::Out].UAV;
		// 这里复用你现有节点（RefPose/Additive/LayerBlend）的 AddPass 模式即可
	}
}

void FGIAG_MinNode::AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)
{
	for (int32 NodeInBatch = 0; NodeInBatch < Context.NodeIndices.Num(); ++NodeInBatch)
	{
		const FGIAG_CPUPoseBufferView Base = Context.InputPosesPerNode[NodeInBatch][(uint8)EInputPin::Base];
		const FGIAG_CPUPoseBufferView Out = Context.OutputPosesPerNode[NodeInBatch][(uint8)EOutputPin::Out];
		check(Base.IsValid() && Out.IsValid());

		for (const int32 SlotIndex : Context.ActiveInstanceIndices)
		{
			FMemory::Memcpy(&Out.At(SlotIndex, 0), &Base.At(SlotIndex, 0), sizeof(FGIAG_BoneTRS) * (SIZE_T)Context.NumBones);
		}
	}
}
```

### 3) `Shaders/Nodes/GIAG_MinNode.usf`

```hlsl
#include "/Engine/Public/Platform.ush"
#include "/GameInstancedAnimationGraphShader/GIAG_AnimCommon.ush"

uint NumBones;
uint NumInstances;
uint DispatchGroupCountX;
uint DispatchGroupCountY;
uint DispatchGroupOffset;

StructuredBuffer<FGIAG_BoneTRS> BasePose;
RWStructuredBuffer<FGIAG_BoneTRS> RW_OutPose;

[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]
void Main(uint3 GroupId : SV_GroupID, uint GroupIndex : SV_GroupIndex)
{
	uint GroupLinear = DispatchGroupOffset + GroupId.x + GroupId.y * DispatchGroupCountX + GroupId.z * (DispatchGroupCountX * DispatchGroupCountY);
	uint DispatchId = GroupLinear * (THREADS_X * THREADS_Y * THREADS_Z) + GroupIndex;
	uint Total = NumBones * NumInstances;
	if (DispatchId >= Total) return;
	RW_OutPose[DispatchId] = BasePose[DispatchId];
}
```

### 4) 在图里接入最小节点

```cpp
const auto Default = Builder.AddNode(Instance.Default);
const auto Min = Builder.AddNode(Instance.Min);
Builder.Link(GIAG_PIN_OUT(Default, Out), GIAG_PIN_IN(Min, Base));
Builder.SetFinalPose(GIAG_PIN_OUT(Min, Out));
```

### 5) 什么时候开始加复杂功能

- 先确认“透传模板”编译/运行通过
- 再逐步加：
  - 运行时参数（并在变化时 `MarkDirty()`）
  - 静态 `FSettings`
  - optional resource
  - cull 逻辑
