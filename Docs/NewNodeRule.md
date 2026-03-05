# GameInstancedAnimationGraph 节点编写规则

本文用于统一 `GameInstancedAnimationGraphNode` 中新节点的实现规范，目标是：
- CPU/GPU 双路径语义一致
- 参数与资源布局稳定
- 以契约式编程为主，不写冗余防御逻辑

---

## 1. 总体原则

- 采用**契约式编程**：前置条件用 `check/checkf` 明确约束。
- 框架已保证的数据，不再做重复判空/兜底分支。
- 节点语义必须在 CPU/GPU 两条路径保持同构（公式、分支条件、参数含义一致）。
- 运行时热路径避免额外分配，优先复用现有缓存和批处理上下文。

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

## 4. 必备接口（按需）

节点由 `TGIAG_AnimNodeMeta<T>` 自动适配，常见实现点：

- `GatherUploadsGPU(uint32& OutUploadStrideBytes) const`
- `static void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context)`
- `static void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context)`

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

## 11. 推荐最小实现顺序

1. 定义 `FSettings` + 节点实例结构 + API  
2. 打通 `GatherUploadsGPU`  
3. 打通 `AddPassesCPU`  
4. 打通 `AddPassesGPU + .usf`  
5. 接入 optional resource（若需要）  
6. 接入示例图并验证编译/运行

---

## 12. ISPC 与 HLSL 同构实现（必读）

### 12.1 共享实现架构

本项目的数学同构不是“复制两份代码”，而是：

- 单一实现源：`Shaders/Common/Shared/GIAG_MathShared.ush`
- HLSL 包装：`Shaders/Common/GIAG_Math.ush`
- ISPC 包装：`Source/GameInstancedAnimationGraph/Public/GIAG_Math.isph`

约定：

- 包装层必须先定义类型和宏，再 include 共享文件
- 共享文件里只写语言无关实现（通过宏适配）

### 12.2 新增 ISPC 节点内核规则

- 文件位置：`Source/GameInstancedAnimationGraphNode/Private/GIAG_XXXNode.ispc`
- 必须 `#include "GIAG_AnimCommon.isph"`
- 导出函数必须用 `export`，并且参数使用裸数组/POD
- 节点 AoS 结构体（`FGIAG_XXXNode_ISPC`）必须显式对齐/补齐，和 C++ 实例布局一致

### 12.3 C++ 侧绑定规则

- 在节点 `.cpp` 引入：
  - `#include "GIAG_XXXNode.ispc.generated.h"`
- 必须做布局校验：
  - `static_assert(sizeof(ispc::FGIAG_BoneTRS) == sizeof(FGIAG_BoneTRS), ...)`
  - `static_assert(sizeof(ispc::FGIAG_XXXNode_ISPC) == sizeof(FGIAG_XXXNode), ...)`
- `AddPassesCPU` 中传入 `Context.NodeData[NodeIdx]` 时，按 slot-indexed AoS 解释，禁止自行重排

### 12.4 HLSL/ISPC 同构校验清单

- 输入/输出索引公式一致：
  - `ActiveIndex -> SlotIndex -> BoneIndex -> LinearIndex`
- 分支一致：
  - 例如 `alpha<=0`、`alpha>=1`、null 权重、early return 条件
- 数值语义一致：
  - quat 归一化、符号对齐、nlerp/slerp 选择、clamp 规则
- 边界一致：
  - `SlotIndex` 范围检查、`NumBones*NumInstances` 总迭代范围

### 12.5 常见坑

- `.ispc` 结构体少了 padding，导致读取错位
- HLSL 改了公式，ISPC 未同步
- 没有 `static_assert`，布局问题到运行时才暴露
- 在 ISPC 里使用不适合 SIMD 的复杂对象/容器

### 12.6 模块配置说明

`GameInstancedAnimationGraphNode.Build.cs` 已包含：

- `PrivateIncludePaths.Add(.../Shaders/Common/Shared)`

用于让 ISPC 预处理器可见共享实现文件。新增节点一般不需要额外改 Build.cs。
