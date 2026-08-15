# Script LLVM JIT 详细设计

## 1. 文档状态

- 状态：第一版核心实现已落地；本文同时保留后续性能与工程化路线。
- 目标版本：第一版以 LLVM 22 为基线。
- 设计对象：`src/script`、`include/fiber/script` 下现有脚本编译器、解释器、异步 ABI 与 GC。
- Java 参考：`temp/fiber-net-gateway/fiber-gateway-script/src/main/java/io/fiber/net/script/aot`。
- Java 参考提交：`869b87ab31d0c2e717b3e85e90eb99ba8141c9f5`。

本文描述一个与现有 `InterpreterVm` 语义等价的 LLVM 原生执行后端。它保留当前 parser、AST、
`ir::Compiled`、运行时运算函数、host library ABI 和 `GcHeap`，新增从栈字节码到 SSA/CFG、从
SSA/CFG 到 LLVM IR、以及通过 ORC 加载机器码的路径。

第一版应被理解为“脚本加载时完成的内存 AOT”或 eager JIT，而不是带热点计数、OSR、推测执行和
deoptimization 的多层 JIT。解释器始终保留，并作为不支持 LLVM、JIT 编译失败或显式禁用 JIT 时的
fallback。

### 1.1 当前实现（2026-08-15）

第一版已经落到以下代码：

- `include/fiber/script/jit/Cfg.h`、`src/script/jit/Cfg.cpp`：CFG、SSA/Phi、异常边、指令级
  liveness 和 async spill；
- `src/script/jit/llvm/LlvmJitCompiler.cpp`：boxed `JsValue` LLVM IR、O2 pipeline、显式
  `gc.statepoint/gc.relocate`、异步入口 switch 和 ORC 装载；
- `src/script/jit/llvm/StackMapPlugin.cpp`：JITLink prune 前保活 `.llvm_stackmaps`，fixup 后严格解析并
  校验 return PC/SP-relative roots；
- `include/fiber/script/run/JitRuntime.h`、`src/script/run/JitRuntime.cpp`：固定 C ABI、x86-64/AArch64
  safepoint trampoline 和现有全部 opcode 的 runtime 语义；
- `include/fiber/script/run/JitVm.h`、`src/script/run/JitVm.cpp`：同步执行、异步 persistent storage、
  参数所有权、completion 和 GC root source；
- `include/fiber/script/run/JitCode.h`、`src/script/run/JitCode.cpp`：机器码、stack map、Compiled 与
  ORC ResourceTracker 生命周期；
- `ScriptCompileOptions`、`ScriptBackendMode::{Interpreter, PreferJit, RequireJit}`：公共 backend 选择、
  structured JIT diagnostic 与解释器 fallback；
- `tests/ScriptJitCfgTest.cpp`、`tests/ScriptJitExecutionTest.cpp`：CFG/SSA、同步与异步差分语义、连续
  forced GC、spread 参数、异常/abort、loop await 和 code owner 生命周期。

构建默认仍不依赖 LLVM；启用方式：

```bash
cmake -S . -B build-jit -DFIBER_ENABLE_SCRIPT_JIT=ON
cmake --build build-jit
ctest --test-dir build-jit
```

当前固定 LLVM 22，native JIT 支持 x86-64 和 AArch64。旧 `compile_script()` overload 仍选择解释器；
调用方显式传入 `ScriptCompileOptions` 才启用 `PreferJit` 或 `RequireJit`。同步临时值没有任何 JitVm
frame root slot：LLVM 只在 statepoint 周围生成 pointer-sized native spill location；JitVm 中的
`async_values` 只保存真正跨 suspend 的 SSA 值。连续两次 forced collection 测试用于排除
`first_collect_protected` 带来的假阳性。

## 2. 结论

当前运行时具备实现 LLVM JIT 的关键前提：

- `JsValue` 是固定 16 字节、16 字节对齐、trivially-copyable 的 tagged POD；
- GC 是非移动 mark-sweep，JIT 不需要在回收后修正对象地址；
- `GcRootSource` 和 `GcRootRegistration` 已支持 VM 扩展根扫描，可接入 JIT stack map；
- `Compiled` 已提供稳定的栈字节码、常量、host callable、源码位置和 catch 查询；
- `InterpreterVm::iterate()` 已定义异步挂起、恢复、成功、异常和 abort 的完整状态机语义；
- `AsyncTask` 的 completion callback 只需记录结果，不要求从 callback 中重入 JIT；
- 本地开发环境已经安装 LLVM 22.1.6，并包含 ORC、JITLink、native codegen 和优化 passes。

实现难点不在 LLVM 指令生成本身，而在以下三个边界：

1. 从栈字节码精确恢复 SSA、普通控制流和异常控制流；
2. 计算跨异步调用的持久值，并保持 C++ lazy coroutine 所引用参数的地址稳定；
3. 使用 LLVM statepoint/stack map 精确描述每个 GC safepoint 的活跃 heap references，并让当前
   `GcHeap` 能从 JIT native frame 标记它们。

只要这三个边界先以保守方式实现，完整方案可行。

## 3. 设计目标

### 3.1 功能目标

- JIT 与解释器对同一 `Compiled` 执行结果一致；
- 支持当前全部 opcode，包括：
  - 常量、root、变量读写；
  - 对象、数组、spread、属性和索引访问；
  - 一元、二元、比较和逻辑运算；
  - 同步/异步 function 与 constant；
  - branch、loop、foreach；
  - `try/catch`、`throw`；
  - value、void、exception、abort 四种最终结果；
- 支持 `exec_sync()` 和 `exec_async()`；
- 支持 JIT code 卸载和 `Script`/活动 VM 生命周期绑定；
- JIT 不可用或编译失败时可回退解释器；
- 保留源码位置，使 abort 和诊断仍能定位原脚本位置。

### 3.2 性能目标

- 消除解释器逐 opcode dispatch；
- 消除大部分操作数栈和变量表的运行时 load/store；
- 让 LLVM 负责 CFG 简化、PHI 简化、SROA、mem2reg、DCE、SCCP、GVN、LICM 和寄存器分配；
- 第一版保持 boxed `JsValue` 语义，不以激进类型特化换取正确性风险；
- 只有跨异步挂起的数据进入持久 frame；同步临时值保持为 LLVM SSA/native stack value；
- 执行热路径不使用 `std::vector`、`std::function` 或逐值动态分配；
- JIT 编译不发生在请求处理热路径或 EventLoop 线程上。

### 3.3 非目标

第一版不实现：

- 热点计数和解释器到 JIT 的 OSR；
- speculative type feedback；
- deoptimization；
- LLVM coroutine lowering；
- moving GC；
- 自定义 LLVM optimization pass；statepoint 生成和 stack map 解析属于 backend lowering/runtime 集成，
  不属于语言优化器；
- 独立磁盘 AOT 文件格式；
- 跨进程持久化 object cache；
- 替换现有 parser、AST optimizer 或 stack bytecode compiler；
- 移除解释器。

## 4. 当前语义基线

JIT 必须以当前 C++ 行为为基线，而不是机械复制 Java AOT 的行为。

### 4.1 值模型

`JsValue` 可能是：

- immediate：`Undefined`、`Null`、`Boolean`、`Int64`、`Double`；
- borrowed：`BorrowedString`、`BorrowedBinary`；
- GC heap ref：string、binary、array、object、exception、iterator；
- tagged exception。

JIT 第一版始终把脚本值建模为完整 `JsValue`。`TypeMask` 仅用于证明某个值不可能是 heap ref、减少
不必要的 managed roots，以及为后续 tag fast path 提供信息；它不能改变语言语义。

### 4.2 GC 模型

`GcHeap::collect()` 从 local/global handles 和所有 `GcRootSource` 标记，然后 sweep 未标记对象。GC 不移动
存活对象，因此：

- JIT 寄存器中的对象地址不需要在 collection 后更新；
- 但 GC 必须通过持久根或 JIT stack map 看见该对象；
- 当前 GC 默认不认识机器寄存器或普通 native stack 中的 `JsValue`，JIT backend 必须为 safepoint
  提供精确机器位置；
- `first_collect_protected` 只能保护新对象第一次 collection，不能替代完整活跃性和根管理。

Java AOT 没有显式的 frame root slots，也没有单独进行 GC safepoint root 分配。它生成的普通值位于 JVM
operand stack 或 local variables 中，跨异步挂起的值位于生成类的 `async$N` 实例字段中；这些引用均由 JVM
依据 bytecode frame/对象布局自动追踪。Java 的 `AsyncSpillAnalysis` 只解决跨异步返回后的值恢复，不负责
告诉 GC 哪些同步临时值仍然存活。

C++ 的 `GcHeap` 默认不扫描 LLVM 寄存器和 native stack。本文通过 LLVM statepoint 生成精确 stack map，
在 GC 真正发生时按当前 JIT return PC 枚举 native spill locations；不为同步临时值建立 frame root slots。
这部分是 C++/LLVM backend 集成，不是从 Java AOT 移植的结构。

### 4.3 异步模型

`Library::AsyncFunction` 和 `Library::AsyncConstant` 返回 `AsyncTask`。`AsyncTask` 是 lazy coroutine：

- 调用 host async function 只创建 coroutine frame；
- coroutine 在外层 `co_await` 交换 handle 后才开始运行；
- `Library::Arguments` 是指向 VM 存储的非拥有 view；
- `HostCallFrame` 通过引用传给 coroutine，并且必须覆盖整个 suspend/resume 周期；
- completion callback 可能写入 value、exception 或 abort，然后恢复外层脚本 coroutine。

因此，异步 call 的实参即使在脚本 CFG 中只被 call 自身使用，也必须保存在 VM 的稳定存储中。Java
版本中“仅作为 async call 参数的值不需要 spill”的规则不适用于这里。

### 4.4 错误模型

项目不使用 C++ exception 表达脚本异常：

- `CallResult::Success`：运算成功；
- `CallResult::Exception`：脚本异常，可进入 catch；
- `CallResult::Abort`：运行时终止，不可进入 catch；
- host ABI 使用 `AbiResult` 表达同样的三类结果；
- script 最终结果还区分 `Value` 和 `Void`。

JIT 生成普通条件分支处理这些状态，不生成 LLVM `invoke`、landing pad 或 C++ unwind table。

## 5. 总体架构

```text
Parser / AST Optimiser
        |
        v
ir::Compiler -> ir::Compiled
        |             |
        |             +----------------------+
        |                                    |
        v                                    v
InterpreterVm                         jit::CfgBuilder
                                             |
                                             v
                                  CFG + stack shape
                                             |
                                             v
                                  SsaValue + Phi + effects
                                             |
                                             v
                   liveness / async spill / GC reference sets
                                             |
                                             v
                                      LLVM IR lowering
                                             |
                                             v
                                    LLVM O1/O2 pipeline
                                             |
                                             v
                                      ORC LLJIT/JITLink
                                             |
                                             v
                                           JitCode
                                             |
                                             v
                                           JitVm
```

`ir::Compiled` 继续是解释器和 JIT 的共同输入。这样 parser、AST optimizer、变量作用域、默认参数、
opcode encoding、异常范围和源码位置只有一份实现。

## 6. 建议模块划分

### 6.1 不依赖 LLVM 的 runtime 层

建议放入 `fiber_lib`：

```text
include/fiber/script/run/JitCode.h
include/fiber/script/run/JitFrame.h
include/fiber/script/run/JitVm.h
src/script/run/JitVm.cpp
src/script/run/JitRuntime.cpp
```

职责：

- 定义稳定的 JIT entry ABI 和异步持久 frame layout；
- 管理 `HostCallFrame`、`AsyncTask`、GC registration 和 active stack-map anchor；
- 提供 `extern "C" noexcept` runtime thunks；
- 不包含任何 LLVM header；
- 即使 LLVM backend 未编译，也能保持公共 script runtime ABI 稳定。

### 6.2 依赖 LLVM 的 compiler 层

建议建立独立可选 target `fiber_script_jit`：

```text
include/fiber/script/jit/Engine.h
include/fiber/script/jit/CompileError.h
src/script/jit/Cfg.cpp
src/script/jit/CfgBuilder.cpp
src/script/jit/Liveness.cpp
src/script/jit/AsyncSpill.cpp
src/script/jit/FrameLayout.cpp
src/script/jit/StatepointLowering.cpp
src/script/jit/StackMapRegistry.cpp
src/script/jit/LlvmLowering.cpp
src/script/jit/LlvmOptimizer.cpp
src/script/jit/Engine.cpp
```

依赖关系：

```text
fiber_script_jit -> fiber_lib + LLVM
```

`fiber_lib` 不反向链接 `fiber_script_jit`，避免核心库在默认配置下携带 LLVM。由于当前 CMake 使用
`GLOB_RECURSE src/*.cpp`，实施时应将 `src/script/jit/` 从 `fiber_lib` source list 排除，再单独建立 target。

### 6.3 CMake 开关

建议新增：

```cmake
option(FIBER_ENABLE_SCRIPT_JIT "Build the LLVM script JIT backend" OFF)
set(FIBER_SCRIPT_JIT_LLVM_MAJOR 22 CACHE STRING "Supported LLVM major version")
```

当开关开启时：

1. `find_package(LLVM 22 CONFIG REQUIRED)`；
2. 只链接所需组件或系统提供的 shared `libLLVM-22`；
3. 生成 `FIBER_ENABLE_SCRIPT_JIT=1`；
4. 构建 `fiber_script_jit` 和对应测试；
5. LLVM 不存在时明确配置失败，而不是在链接阶段失败。

首版固定一个 LLVM major，避免 ORC C++ API 变动导致未经测试的兼容范围。后续可在 CI 覆盖多个 major
后再放宽。

## 7. 高层 SSA/CFG IR

LLVM IR 不应直接从原始 opcode switch 临时生成。中间的语言级 SSA/CFG 用于：

- 验证 stack shape；
- 精确构造普通和异常控制流；
- 计算 instruction-level liveness；
- 计算 async persistent values；
- 计算 GC safepoint roots；
- 保留 PC、源码位置和脚本语义 effect；
- 在 LLVM lowering 前执行结构校验和可读 dump。

### 7.1 标识符

使用稳定整数 ID，而不是在分析结果中长期持有可失效容器指针：

```cpp
using BlockId = std::uint32_t;
using InstId = std::uint32_t;
using ValueId = std::uint32_t;
using SlotId = std::uint32_t;
```

编译阶段不是请求执行热路径，可以使用 `std::vector` 或 LLVM `SmallVector`；为了降低大量小对象分配，
建议 CFG/Instruction/SsaValue 使用 arena 或连续数组。

### 7.2 TypeMask

```cpp
enum class SsaType : std::uint16_t {
    Undefined      = 1u << 0,
    Null           = 1u << 1,
    Boolean        = 1u << 2,
    Int64          = 1u << 3,
    Double         = 1u << 4,
    BorrowedString = 1u << 5,
    BorrowedBinary = 1u << 6,
    HeapString     = 1u << 7,
    HeapBinary     = 1u << 8,
    Array          = 1u << 9,
    Object         = 1u << 10,
    Iterator       = 1u << 11,
    Exception      = 1u << 12,
};

using TypeMask = std::uint16_t;
```

要求：

- `Unknown` 表示全部可能类型；
- 常量、比较结果、`typeof`、`new object/array`、iterator 等可给出精确或较窄类型；
- property/index/host call 结果默认 `Unknown`；
- 类型合并使用集合 union；
- 只有类型集合明确不含任何 heap ref 时，才允许跳过 GC root；
- 类型推导错误最多损失优化，不能造成漏标 root，因此所有不确定情况必须保守。

### 7.3 SsaValue

第一版保持最小状态：

```cpp
struct SsaValue {
    InstId def = kInvalidInst;
    TypeMask types = kAllTypes;
};
```

不需要复制 Java 版本用于 optimizer 的 mutable use-list。uses 可通过遍历 instruction operands 建立，
liveness 使用 `ValueId` bit set。若后续需要语言级 rewrite，再增加 use-list。

### 7.4 Instruction

```cpp
enum class Effect : std::uint16_t {
    None         = 0,
    ReadsHeap    = 1u << 0,
    WritesHeap   = 1u << 1,
    CallsHost    = 1u << 2,
    MayAllocate  = 1u << 3,
    MayException = 1u << 4,
    MayAbort     = 1u << 5,
    Async        = 1u << 6,
};

struct Instruction {
    Opcode opcode;
    std::uint32_t pc;
    ValueId result;
    OperandRange operands;
    Effect effects;
    Immediate immediate;
};
```

effect 是正确性信息，不只是 optimizer hint：

- `MayAllocate` 和 `CallsHost` 默认是 GC safepoint；
- `MayException` 需要 exception edge；
- `MayAbort` 需要 terminal abort path；
- `WritesHeap` 阻止跨调用错误地缓存 heap read；
- host call 默认同时具有 `ReadsHeap | WritesHeap | CallsHost | MayAllocate | MayException | MayAbort`；
- 只有经过代码审计的 runtime helper 才能缩窄 effect。

不能照搬 Java `Instruction.Throw` 表。C++ 的 `NEW_OBJECT`、`NEW_ARRAY` 等操作会因为 OOM 返回 abort，
各 runtime op 的 effect 必须以当前 C++ 实现为准。

### 7.5 BasicBlock

```cpp
enum class EdgeKind : std::uint8_t {
    Fallthrough,
    Jump,
    True,
    False,
    Exception,
};

struct BasicBlock {
    std::uint32_t start_pc;
    std::uint32_t end_pc;
    InstRange instructions;
    EdgeRange predecessors;
    EdgeRange successors;
    ValueRange phis;
    ValueRange entry_stack;
    ValueRange exit_stack;
    ValueRange entry_vars;
    ValueRange exit_vars;
};
```

`end_pc` 为开区间。每个 block 必须只有一个 terminator，可能抛脚本异常或异步挂起的 instruction 也视为
block 末尾，以便精确表达 continuation 和 catch edge。

## 8. CFG 构造

### 8.1 发现 block 边界

先扫描 `Compiled::codes()`，加入：

- PC 0；
- `JUMP`、`JUMP_IF_TRUE`、`JUMP_IF_FALSE` 的 target；
- branch 后的下一条指令；
- `END_RETURN`、`THROW_EXP` 后的下一条指令（若存在）；
- 每个 catch entry；
- 每个具有 `MayException`、`MayAbort` 或 `Async` effect 的指令之后；
- async call 指令自身所在 block 的结束边界。

对不可达的尾部 block 可以先建立，完成 edge 构造后再从 entry 做 reachability 标记并删除。

### 8.2 构造普通边

- `JUMP`：一条 `Jump` edge；
- conditional jump：一条条件 edge 和一条 fallthrough edge；
- `END_RETURN`：无 successor；
- `THROW_EXP`：只有 exception edge 或 terminal exception；
- 普通 instruction 结束的 block：fallthrough；
- abort：无 successor；
- async call：逻辑上连接到 resume continuation，但机器码中通过函数入口 state dispatch 到达。

### 8.3 构造异常边

对于 `MayException` instruction：

1. 使用该 instruction 的原始 `pc` 调用 `Compiled::find_catch(pc)`；
2. 有 catch 时增加 `Exception` edge；
3. 无 catch 时连接到 terminal exception exit；
4. exception edge 上操作数栈为空；
5. 变量环境来自 instruction 执行前的 SSA var map；
6. pending exception 作为 catch entry 的特殊 SSA 定义；
7. `INTO_CATCH` 将 pending exception 写入对应脚本变量并清空 pending 状态。

`MayAbort` 不产生 catch edge，直接进入 terminal abort。一个 runtime helper 同时可能 success、exception 或
abort，lowering 后必须形成三个显式分支。

### 8.4 Stack shape 传播

在生成 SSA 前，为每种 opcode 定义：

- consume count；
- produce count；
- direct call 的 argc；
- spread call 固定 consume 1、produce 1；
- exception edge 的输出 stack size 为 0。

算法：

1. 每个 block 先独立计算 `required_input_depth` 和 `stack_delta`；
2. entry block 输入深度为 0；
3. worklist 沿普通 edge 传播输出深度；
4. exception edge 向 successor 传播 0；
5. 同一 block 从不同 predecessor 得到的输入深度必须完全一致；
6. underflow、overflow 或不一致视为 JIT compile error，而不是生成带 assert 的机器码。

### 8.5 字节码模拟

对每个 block 建立编译期 frame：

```cpp
struct BuildFrame {
    SmallVector<ValueId> stack;
    SmallVector<ValueId> vars;
};
```

模拟规则与解释器一致，但每个结果产生新的 `ValueId`：

- `LOAD_VAR` 读取当前 var value；
- `STORE_VAR` 更新编译期 var map，不生成运行时内存写；
- `DUMP` 复制同一个 `ValueId`；
- binary/unary/access/call 消费 operand ValueId 并产生 result ValueId；
- branch 消费 condition；
- foreach iterator 也作为普通 SSA value；
- block 退出时保存 `exit_stack` 和 `exit_vars`。

### 8.6 Lazy Phi / MaybePhi

循环和前向 edge 使 block 在模拟时可能尚不知道所有 predecessor 的 exit values。采用与 Java 参考相同的
延迟 PHI 策略：

1. block 第一次读取 entry stack/var 时创建 `MaybePhi(block, kind, index)`；
2. 所有 block 模拟完成后，收集每个 predecessor 对应的 incoming value；
3. 所有 incoming value 相同，直接替换 MaybePhi；
4. incoming value 不同时创建真实 Phi；
5. 忽略 Phi 自引用后若只剩一个唯一 value，则消除 trivial Phi；
6. 重复直到没有可简化 Phi。

也可以实现 sealed-block SSA 算法，但首版优先沿用已经由 Java 测试验证过的 MaybePhi 思路。

### 8.7 CFG verifier

进入 liveness 和 LLVM lowering 前必须验证：

- entry block 存在且无普通 predecessor；
- 所有 reachable block 都有确定输入 stack depth；
- 所有 operand 和 Phi incoming 已定义；
- Phi incoming 数量与 predecessor 数量一致；
- exception edge 不传递 operand stack；
- terminator 与 successor 类型一致；
- result type mask 非空；
- async instruction 位于 block 末尾；
- PC 在 `Compiled::code_size()` 内；
- catch target 是 block entry；
- ValueId、BlockId、SlotId 均未溢出限制。

debug/test 构建提供稳定文本 dump，便于与 Java CFG 和解释器 bytecode 对照。

## 9. Liveness 与值位置分类

### 9.1 普通活跃性

以 SSA value 为单位计算：

```text
liveOut(block) = union(edgeLiveIn(successor))
liveIn(block)  = use(block) union (liveOut(block) - def(block))
```

Phi operand 属于 predecessor edge，不是 successor block 内普通 use。反向遍历 block instructions，生成：

- `live_before(inst)`；
- `live_after(inst)`。

使用稠密 bitset 或按 ValueId 编号的动态 bitset，避免分析中大量 hash set 分配。

### 9.2 运行时位置

逻辑上的“局部变量/成员变量”分为：

| 类别 | 条件 | 运行时位置 |
|---|---|---|
| 常量/root | 来自 `Compiled` 常量或 VM root | 稳定常量地址或 VM 固定字段 |
| LLVM local | 不跨 async | LLVM SSA value，必要时由寄存器分配器 spill 到 native stack |
| statepoint root | heap-capable 且活跃跨过 GC safepoint | `ptr addrspace(1)`，机器位置记录在 stack map |
| async persistent | 活跃跨过 async suspend | JitVm 的 persistent `JsValue` storage |
| async arguments | 被 lazy host coroutine 引用 | 稳定且连续的 persistent argument storage |

这里有两个彼此独立的条件：

- `async persistent` 对应 Java AOT 的 `AsyncFieldLocation`，由 suspend liveness 决定；
- `statepoint root` 由 safepoint liveness 和 `TypeMask` 决定，只在 native activation 存在期间有效。

同步临时值不会因为 GC 成为 JitVm 成员。LLVM statepoint 只保留一个 8 字节 `GcHeader *` 机器位置，
而不是复制完整 16 字节 `JsValue`。跨 async 的 immediate-only 值需要持久化，但不会出现在 GC stack map 中。

### 9.3 Async spill 公式

对 async instruction `A`：

```text
ResumeSpill(A) = LiveAfter(A) - {Result(A)}
```

这里的 `LiveAfter` 必须包含 normal continuation 和 exception continuation 所需值，因此 catch 中仍会读取的
变量也会进入 spill 集合。

C++ 还需要额外计算：

```text
AsyncPersistent(A) = ResumeSpill(A) union AsyncArgumentStorage(A)
```

`AsyncArgumentStorage(A)` 不是普通 SSA liveness：参数在脚本角度被 call 消费，但 lazy `AsyncTask` 在 JIT
函数已经返回后才读取它们。

### 9.4 Direct 参数

direct async call 的参数数量在 bytecode 中已知。persistent layout 预留：

```text
max_direct_async_argc = max(all direct async call argc)
```

所有 direct async 参数在调用前复制到这一段连续、16 字节对齐的 `JsValue` storage。任一时刻一个 VM 最多只有
一个活动 async call，因此不同 call site 可以复用同一段空间。该区域：

- 在 suspend 期间保持地址不变；
- 在挂起期间由 `visit_roots()` 扫描；
- completion 后才允许重用；
- 包含 compiler 添加的 default arguments。

同步 host call 不使用 persistent 区域。固定 argc 时使用 native call arguments 或 entry 内一次性 `alloca`
的同步 scratch；其 heap references 同时列入 call statepoint 的 `gc-live` bundle。

### 9.5 Spread 参数

spread argc 运行时才知道，不能仅凭 bytecode 预留固定 tail array。首版保持解释器语义：

- 将 spread array owner 放入专用 async persistent field；
- `Arguments` 指向 `GcArray::elems`；
- JIT 在 suspend 期间不修改该 array；
- host ABI 将 `Arguments` 视为只读，不得使 array storage reallocate；
- array owner 必须一直可达直到 async completion。

如果未来需要允许 host 修改 spread array，必须改为 JitVm 拥有的动态 argument buffer，并将 buffer 纳入
root scan；不能继续暴露可能 reallocate 的 `GcArray::elems`。

### 9.6 Persistent storage 分配

持久 storage 只保存 native activation 返回后仍必须存在的数据：

- 每个 async-spilled static SSA value 分配一个 persistent slot；
- direct async arguments 使用独立可复用区域；
- pending result、pending exception、root、spread owner 使用保留字段；
- safepoint-only values 不分配 persistent slot。

所有 persistent `JsValue` 初始化为 `undefined`，并由 JitVm 的 `visit_roots()` 扫描。首版可为每个
async-spilled SSA value 分配独立 slot；若 async frame 实测过大，再只对 persistent live sets 做 interference
graph coloring。GC safepoint locations 完全交给 LLVM register allocator 和 stack map，不参与该 slot allocator。

## 10. JIT Frame 与 VM

### 10.1 分离 C++ owner 和标准布局 frame

LLVM 只访问稳定的 plain ABI frame，不直接访问包含引用、虚函数或 coroutine owner 的 C++ 对象。

```cpp
namespace fiber::script::run {

enum class JitRunState : std::uint8_t {
    Init,
    Running,
    Suspend,
    AsyncValue,
    AsyncException,
    AsyncAbort,
    Success,
    SuccessVoid,
    Exception,
    Abort,
};

struct JitFrameHeader {
    void *vm_context = nullptr;
    const ir::Compiled *compiled = nullptr;
    JsValue *async_values = nullptr;
    std::uint32_t async_value_count = 0;
    std::uint32_t resume_id = 0;
    std::uint32_t active_pc = 0;
    const void *safepoint_return_pc = nullptr;
    const void *safepoint_stack_pointer = nullptr;
    JitRunState state = JitRunState::Init;
    ScriptAbort abort{};
};

using JitEntry = std::uint32_t (*)(JitFrameHeader *) noexcept;

} // namespace fiber::script::run
```

最终字段可调整，但必须满足：

- standard-layout；
- 显式固定宽度整数；
- 不包含 `std::string`、`std::vector`、`std::function`；
- JIT 只通过公开 offset 访问；
- 只有异步持久 `JsValue` 数据放在独立 16 字节对齐 allocation 中；
- safepoint anchor 只由架构 trampoline 在 runtime call 入口设置、退出时清理；
- ABI 有独立版本号。

### 10.2 JitVm

```cpp
class JitVm final : public GcRootSource {
public:
    JitVm(std::shared_ptr<const JitCode> code,
          std::shared_ptr<const ir::Compiled> compiled,
          JsValue root,
          void *attach,
          GcHeap &heap);

    void iterate();
    bool done() const noexcept;
    ScriptResult result() const noexcept;
    AsyncTask &async_task() noexcept;
    void visit_roots(GcRootVisitor &) noexcept override;

private:
    std::shared_ptr<const JitCode> code_;
    std::shared_ptr<const ir::Compiled> compiled_;
    Library::HostCallFrame host_frame_;
    GcRootRegistration registration_;
    JitFrameHeader frame_;
    std::unique_ptr<JsValue[]> async_values_;
    AsyncTask async_;
};
```

设计规则：

- `JitVm` 的生命周期覆盖异步 suspend/resume；
- `host_frame_` 地址稳定，满足 lazy `AsyncTask` 持有引用的要求；
- `code_` 保证当前正在执行的机器码不会被 ORC 卸载；
- `compiled_` 保证常量、payload、func const 和源码位置地址稳定；
- async storage 构造失败产生 `OutOfMemory` abort，不进入 JIT；
- `iterate()` 不在 completion callback 中递归调用自身。

### 10.3 Persistent storage 布局

建议 frame descriptor 按顺序描述：

```text
[root]
[pending value]
[pending exception]
[async spread owner]
[persistent SSA values...]
[direct async arguments...]
```

结果和 exception 使用不同字段，避免 union 当前 active member 与 GC scan 之间产生歧义。abort 只包含立即数，
保存在 header。该布局没有 safepoint-only root slots。

### 10.4 Root 扫描

```cpp
void JitVm::visit_roots(GcRootVisitor &visitor) noexcept {
    visitor.visit(&root_);
    visitor.visit(&pending_value_);
    visitor.visit(&pending_exception_);
    visitor.visit_range(frame_.async_values, frame_.async_value_count);
    if (frame_.safepoint_return_pc) {
        code_->stack_maps().visit(frame_.safepoint_return_pc,
                                  frame_.safepoint_stack_pointer,
                                  visitor);
    }
}
```

持久字段始终包含合法 `JsValue`，未使用字段为 `undefined`。stack map 中保存的是裸 `GcHeader *`，因此
`GcRootVisitor` 需要新增 `visit_heap_ref(GcHeader *) noexcept`；`MarkingVisitor` 直接调用 `gc_mark_obj()`，
不在 GC 热路径临时构造 `JsValue` 或分配 handle。

## 11. GC Safepoint 设计

### 11.1 Safepoint 定义

以下 instruction 默认是 safepoint：

- 所有 `MayAllocate` runtime operation；
- 所有同步/异步 host calls；
- 任何可能直接或间接调用 `GcHeap::collect()` 的 helper；
- borrowed materialization；
- 创建 array/object/string/binary/exception/iterator；
- JIT runtime 明确的 stress collection hook。

不分配且经过审计的逻辑比较、tag 检查、整数运算 fast path 可以不是 safepoint。

### 11.2 Native root set

对 safepoint instruction `S`：

```text
NativeRoots(S) = {
    V | V in LiveBefore(S),
        TypeMask(V) may contain GC heap ref,
        V is not already held by a stable persistent root
}
```

还必须包含本次 call 的 heap-capable 参数。参数即使不在 `LiveAfter(S)` 中，helper 执行期间仍会读取。VM root、
pending value/exception 和 async persistent values 已由 `JitVm::visit_roots()` 扫描，不需要重复进入 stack map。

该集合按 `ValueId` 去重。所有引用都是对象起始位置的 `GcHeader *`，不向 collector 暴露 `GcArray::elems`
等 derived pointer，因此非移动 GC 不需要 base/derived object 恢复逻辑。

### 11.3 从 boxed JsValue 提取 managed pointer

JIT 内部仍可用两个 `i64` 或等价 aggregate 表示完整 `JsValue`。对 `NativeRoots(S)` 中每个值，在
statepoint 前构造 nullable managed pointer：

```llvm
%is_heap = icmp eq i8 %tag, JsTag::HeapRef
%root_bits = select i1 %is_heap, i64 %payload, i64 0
%root = inttoptr i64 %root_bits to ptr addrspace(1)
```

`TypeMask` 能证明 exact heap ref 时省略 tag check；能证明 immediate/borrowed 时完全不生成 root。borrowed
string/binary 的 payload 不是 GC 对象，不能错误地作为 managed pointer 上报。

每个可能收集的 runtime call 直接 lower 为 `llvm.experimental.gc.statepoint`，所有 `%root` 放入
`"gc-live"` operand bundle。即使当前 GC 不移动对象，第一版仍为每个 root 生成并使用对应
`llvm.experimental.gc.relocate`：

```llvm
%sp = call token (...) @llvm.experimental.gc.statepoint(...) [
    "gc-live"(ptr addrspace(1) %root)
]
%root_after = call ptr addrspace(1)
    @llvm.experimental.gc.relocate(token %sp, i32 0, i32 0)
```

`root_after` 在 heap-ref 分支重新形成后续 boxed payload。对非移动 GC 它与原地址相同，但显式使用 relocate
能保证 LLVM 生成 relocation record、保留 root 机器位置，并为将来移动 GC 留下正确数据流。LLVM 22 在
x86-64 和 AArch64 上会把这种 root 放入 pointer-sized native spill slot；不创建 JitVm frame root slot。

若 root 只因“本次 call 参数”而需要、call 后已经不再活跃，仍生成 relocate，并用 `llvm.fake.use` 保留其
machine location；该 intrinsic 不生成业务机器指令，但 LLVM 22 当前仍会产生一次 spill/reload。后续只有在
object-level test 证明 stack map record 不会消失时，才可为 non-moving GC 专门消除这次无用 reload。

### 11.4 GCStrategy 与优化顺序

`fiber_script_jit` 注册私有策略 `fiber-script-statepoint`：

- `UseStatepoints = true`；
- `UseRS4GC = false`，前端直接生成显式 statepoint/relocate；
- `addrspace(1)` pointer 是 managed pointer；普通 runtime/frame pointer 位于 `addrspace(0)`；
- 不使用 `llvm.gcroot` 和 shadow-stack lowering。

statepoint 在 LLVM IR lowering 时生成，随后进入标准 O1/O2 pipeline。operand bundle 是语义 use，优化器不能
删除仍需标记的 root。项目不增加自定义优化 pass，但必须在优化前后运行 LLVM verifier，并在 object emission
后验证每个 statepoint 都有可解析的 stack map record。

statepoint 和 stack map 仍是 LLVM experimental ABI，因此 JIT target 固定 LLVM major，stack map parser
校验 version、location count、pointer width、endianness 和 target triple；不接受其他 major 生成的 object。
实现以 LLVM 官方的 [Statepoints](https://llvm.org/docs/Statepoints.html) 和
[StackMaps](https://llvm.org/docs/StackMaps.html) 格式为基线，不自行猜测 backend 私有数据结构。

### 11.5 ORC/JITLink stack map 注册

`ObjectLinkingLayer` 添加一个 plugin，在 JITLink `PostFixupPasses` 中读取已完成 relocation 的
`.llvm_stackmaps`（Mach-O 使用对应 section name），转换为运行时紧凑表：

```text
SafepointRecord {
    return_pc;
    root_count;
    roots[root_count] = {signed_sp_offset};
}
```

状态点 ID 用于编译期诊断；运行时按精确 return PC 查找。base/derived 相同的 relocation pair 去重，null
constant 直接忽略。第一版禁止动态 `alloca`，只接受 pointer-sized、以 statepoint caller SP 为 base 的
indirect locations；若目标 backend 产生 FP-relative、register-only、超宽或未知 location，整个 module 编译
失败并回退解释器，不能静默漏标。

紧凑表由 `JitCode` 持有。`ResourceTracker::remove()` 前先从全局 PC index 注销；活动 `JitVm` 持有
`JitCode`，因此 stack map 不会早于机器码释放。

### 11.6 Runtime call stack anchor

GC 可能在 runtime helper 的深层调用中发生，届时必须仍能定位 statepoint caller frame。所有可能收集的
runtime thunk 使用一个无 prologue 的目标相关 entry trampoline：

- x86-64 SysV：entry 时读取 `[rsp]` 为 return PC，`rsp + 8` 为 statepoint caller SP；
- AArch64：entry 时读取 `x30` 为 return PC，当前 `sp` 为 statepoint caller SP；
- trampoline 只使用 ABI scratch registers，将两者写入第一个参数 `JitFrameHeader`，然后 tail-branch 到
  真正的 `extern "C" noexcept` C++ implementation；
- implementation 返回前清空 anchor，debug 构建断言不存在嵌套的同 VM JIT call。

不能用普通 C++ 函数中的局部变量地址猜 caller SP，也不能依赖可省略的 frame pointer。每个新增 target 必须
实现并单测自己的 trampoline 和 DWARF register mapping；未实现 target 不启用 JIT。

### 11.7 GC root 枚举

`JitVm` 继续注册为 `GcRootSource`。collection 发生时：

1. 扫描 root、pending result/exception 和 async persistent storage；
2. 若 stack anchor active，以 return PC 查找 `SafepointRecord`；
3. 以保存的 caller SP 计算每个 indirect location，读取 `GcHeader *`；
4. 调用 `GcRootVisitor::visit_heap_ref()`；
5. 按现有 mark-sweep 逻辑追踪对象 children。

stack map 只在真正 collection 时查询。普通 helper call 的运行时额外工作只有 trampoline 保存/清理两个机器字；
root spill/reload 由 LLVM 合并和复用 native stack slots。当前 GC 是同线程同步 collection，不需要暂停另一个
正在运行的 JIT frame；若未来允许并发 GC，必须增加线程停顿和完整寄存器上下文协议。

### 11.8 Runtime helper 内部临时值

JIT 负责 caller live values，runtime helper 继续负责其内部临时值：

- 使用现有 `GcHeap::LocalMark`；
- 使用 `local_value()` 保存分配链中的中间结果；
- 多对象构造需要 `NoGcScope` 时遵守现有规则；
- helper 返回前把最终结果写到 native out value 或 pending async field；
- 不依赖未登记的 JIT value 作为 root。

### 11.9 性能特征

与 frame root slots 相比：

- 每个动态 root 在 safepoint 最多保留 8 字节 header pointer，而不是写入 16 字节 boxed value；
- native spill slots 由 LLVM register allocator 着色复用，不增加 JitVm allocation；
- 没有每次 helper 前的 N 个 frame root stores，也不扫描静态分配但当前无效的 slots；
- GC 未触发时不解析 stack map；
- 代价是每个 maybe-heap value 的 tag test、statepoint spill/reload，以及 runtime trampoline 的固定开销。

`TypeMask`、statepoint root 去重和 non-GC helper fast path 直接决定开销。需要用汇编检查和 benchmark 验证，
不能假定 statepoint 一定优于 frame slots；但它更符合“不把同步临时值提升为成员”的性能目标。

## 12. Borrowed String/Binary 生命周期

GC rooting 只能保留 heap object，不能延长 borrowed backing buffer 的生命周期。这是解释器与 JIT 共同面对的
ABI 问题，不应误认为“出现在 stack map 或 JIT member 中就已经安全”。

### 12.1 首版生命周期约束

首版定义：

- `Compiled` 中的 borrowed literal backing 由 `Compiled` 自身持有，覆盖所有 VM；
- script root 中的 borrowed backing 必须覆盖完整 `exec_sync/exec_async` 生命周期；
- host function 返回 borrowed value 时，backing 必须至少覆盖完整脚本执行生命周期；
- 被写入 array/object 的 borrowed value 继承相同要求；
- `attach` 若承担 backing owner 角色，其 owner 必须由外层 coroutine 保持到脚本结束。

这与当前解释器能够安全工作的隐含前提一致，应升级为明确文档和 debug contract。

### 12.2 可选 async promotion

若业务需要接收只在 host call 内有效的 borrowed value，则必须实现 promotion：

- async suspend 前检查所有 persistent values 和 arguments；
- borrowed string materialize 为 `GcString`；
- borrowed binary materialize 为 `GcBinary`；
- 若 borrowed value 可能嵌套在 array/object 中，需要深度遍历或 copy，而不是只提升顶层 slot；
- promotion OOM 产生不可捕获的 `OutOfMemory` abort；
- 已知来自 `Compiled` 的稳定 literal 可跳过。

在未实现完整嵌套 promotion 前，不允许宣称 JIT 自动修复任意 borrowed backing 生命周期。

## 13. Runtime C ABI

LLVM IR 不直接调用复杂 C++ 成员函数或依赖聚合参数/返回值 ABI。提供一组白名单
`extern "C" noexcept` thunks。`JsValue` 在 JIT 边界拆为两个 `uint64_t`，同步结果通过 entry native frame
中的复用 out scratch 返回，不通过 JitVm persistent slots 中转。

示例：

```cpp
extern "C" std::uint32_t
fiber_script_jit_binary(JitFrameHeader *frame,
                        std::uint32_t opcode,
                        std::uint64_t lhs_payload,
                        std::uint64_t lhs_meta,
                        std::uint64_t rhs_payload,
                        std::uint64_t rhs_meta,
                        JsValue *out) noexcept;

extern "C" std::uint32_t
fiber_script_jit_call_sync(JitFrameHeader *frame,
                           std::uint32_t func_index,
                           const JsValue *args,
                           std::uint32_t argc,
                           JsValue *out) noexcept;

extern "C" std::uint32_t
fiber_script_jit_call_async(JitFrameHeader *frame,
                            std::uint32_t func_index,
                            std::uint32_t async_arg_offset,
                            std::uint32_t argc,
                            std::uint32_t resume_id) noexcept;

extern "C" std::uint32_t
fiber_script_jit_take_async_result(JitFrameHeader *frame,
                                   JsValue *out) noexcept;
```

`lhs_meta`/`rhs_meta` 是 `JsValue` 后 8 字节的 bit-preserving 表示，不重新定义 tag layout。固定 argc 的普通
operator helper 直接传 scalars；同步 host call 需要连续 `Arguments` 时，JIT entry 使用一次性、可复用的
native `alloca` argument scratch。该 scratch 不是 GC root region，参数中的 heap references 已进入当前
statepoint 的 `gc-live` bundle。

实际可以按 op family 拆分，规则如下：

- 函数返回固定宽度 status integer；
- 同步 success value/exception 写入 native out scratch，JIT 返回后立即 load 为 SSA value；
- async success value/exception 写入 JitVm pending fields；
- abort 写入 header；
- helper 内通过 `vm_context` 找到 `JitVm`、`HostCallFrame` 和 `AsyncTask`；
- host callable 通过 `Compiled::func_const(index)` 取得，不把 userdata/function address 硬编码进 IR；
- helper 根据 `active_pc` 或显式 PC 填充 abort position；
- ABI symbol 名字和签名由 `kJitAbiVersion` 管理；
- Engine 初始化时一次性向 ORC 注册绝对符号映射。
- 所有 MayGC thunk 的公开 symbol 指向 11.6 节的架构 trampoline，真实 C++ 实现使用 `_impl` 后缀；
- `JitFrameHeader *` 必须是第一个参数，供 trampoline 写 stack anchor。

建议 status：

```cpp
enum class JitStatus : std::uint32_t {
    Success = 0,
    Exception,
    Abort,
    Suspend,
    SuccessVoid,
};
```

不要直接从 JIT entry 返回 `ScriptResult` 或 `AbiResult` 聚合体，以避免不同编译器/平台的 return ABI 差异。

## 14. 异步状态机

### 14.1 Resume ID

为每个 reachable async instruction 分配非零 resume ID：

```text
0 = normal entry
1 = resume after async site 1
2 = resume after async site 2
...
```

JIT function 入口生成 `switch(frame->resume_id)`。非法 ID 写入 `InvalidState` abort。

### 14.2 Async call lowering

调用点的生成顺序：

1. 保存 `ResumeSpill(A)` 到 persistent slots；
2. 准备 direct async argument storage 或 persistent spread owner；
3. 保存 `active_pc`；
4. 调用 `fiber_script_jit_call_async` 创建 `AsyncTask`；
5. allocation failure/invalid task 进入 abort；
6. 成功创建 task 后设置 `resume_id`；
7. entry 返回 `Suspend`；
8. `Script::exec_async()` 使用与解释器相同的 awaiter 驱动 `AsyncTask`。

由于 `AsyncTask::initial_suspend()` 总是 suspend，host coroutine 即使没有内部 `co_await`，JIT 初次调用仍会
先返回 `Suspend`。host coroutine 开始后可以立即完成，completion callback 记录结果并恢复外层 coroutine，
然后再次进入 JIT resume dispatch。第一版不需要 Java 版本的“async call 同步立即返回”分支。

### 14.3 Completion callback

callback 只做：

- 校验 VM 当前为 Suspend 且 task 有效；
- 将 success value 写入 pending value slot；或
- 将 exception 写入 pending exception slot；或
- 将 abort 写入 header；
- 更新 `JitRunState`。

callback 不直接调用 JIT entry，避免 reentrancy 和 native stack 嵌套。

### 14.4 Resume lowering

对应 resume label：

1. 校验 async return state；
2. reset 已完成的 `AsyncTask`；
3. 将 `resume_id` 清零；
4. 从 persistent slots 恢复跨 suspend SSA values；
5. success：读取 pending value 作为 async instruction result；
6. exception：沿该 call PC 的 exception edge；
7. abort：直接 terminal abort；
8. 清理或覆盖 argument/spread slots 后继续 normal CFG。

async continuation 在 LLVM CFG 中由 entry switch 直接到达，因此所有跨 suspend values 必须从 frame load；
不能让 LLVM PHI 假设初次调用的 native predecessor 仍然存在。

## 15. LLVM IR Lowering

### 15.1 Module 与函数

每个 `Compiled` 首版生成一个 LLVM module 和一个 exported entry：

```text
fiber_script_entry_<unique-id>
```

entry 签名固定为：

```llvm
define i32 @fiber_script_entry_N(ptr %frame) nounwind
```

`JitCode` 保存解析后的 `JitEntry` function pointer、frame descriptor 和 code resource owner。

### 15.2 JsValue LLVM 表示

建议 named struct：

```llvm
%fiber.JsValue = type { i64, i32, i16, i8, i8 }
```

和 C++ ABI 交互只使用 `ptr`、固定宽度 integer 及 status；不按 C++ aggregate ABI 传递 `JsValue`。生成器对
native scratch 的 load/store 显式使用 16 字节 alignment。

Engine 初始化时校验：

- `sizeof(JsValue) == 16`；
- `alignof(JsValue) == 16`；
- payload/tag/subtag 等字段 offset；
- LLVM target `DataLayout` 下 struct size/field offsets 与 C++ 一致；
- pointer width、endianness 和 target triple 被支持。

不满足时拒绝启用 JIT 并回退解释器。

### 15.3 BasicBlock 和 PHI

- 每个高层 CFG block 映射一个或多个 LLVM basic blocks；
- 普通 SSA Phi 直接映射 `llvm::PHINode`；
- 不运行自定义 SSA destruction；
- 不实现寄存器分配；
- async continuation 的 spilled values 映射为 frame load，不建立跨函数返回的 PHI；
- exception/abort status 分支可能为每个 runtime call 额外生成 status blocks。

### 15.4 Locals

优先直接将 `SsaValue` 映射到 `llvm::Value*`。只有以下情况使用 alloca：

- helper C ABI 要求地址；
- 需要构造临时 `JsValue`；
- 实现初期简化某些 Phi/edge copy；
- debug 构建需要可读 IR。

alloca 集中在 entry block，显式 align 16。LLVM 的 SROA/mem2reg 负责删除不必要的 allocas。

### 15.5 Runtime call

boxed 第一版中，大部分语义操作仍调用 runtime thunk：

```text
extract/deduplicate live GcHeader pointers
statepoint call helper [gc-live roots]
gc.result + gc.relocate
switch status:
  success   -> load native out scratch
  exception -> catch or terminal exception
  abort     -> terminal abort
```

这已经可以消除解释器 dispatch、动态 pc/sp 更新和绝大部分虚拟 stack traffic，但 LLVM 看不到 opaque helper
内部语义，因此不能期待它自动将所有脚本算术变成 native arithmetic。

### 15.6 后续 tag fast path

第二阶段可为热点运算生成：

```text
if lhs.tag == Int64 && rhs.tag == Int64:
    checked native integer operation
else:
    runtime slow helper
```

适合优先内联：

- boolean logic；
- strict tag equality 的简单分支；
- int/double 比较；
- 无溢出或带明确溢出语义的整数算术；
- `typeof`；
- array length 等经过审计的只读 fast path。

必须保留：

- 与解释器一致的数值转换；
- overflow、除零、NaN、字符串拼接和异常语义；
- heap mutation 的版本号和 iterator mutation 规则；
- slow path 的 position attribution。

第一版不要实现 speculative unboxing 和 deopt。tag guard 失败直接进入 slow helper 即可。

## 16. LLVM 优化流水线

### 16.1 默认级别

- 预加载/配置加载时编译：默认 LLVM O2；
- 测试和 IR 调试：O0；
- 若编译延迟成为问题，可提供 O1；
- 不默认使用 O3，先以 code size、编译时间和实际 benchmark 决定。

使用 New Pass Manager 的标准 per-module pipeline，不维护一套自定义等价 optimizer。

### 16.2 Frontend 仍需完成的分析

以下不能交给 LLVM：

- stack bytecode 到 SSA；
- exception edge；
- async spill；
- argument lifetime；
- GC root liveness；
- managed pointer extraction/statepoint root sets；
- borrowed lifetime；
- script effect/host effect；
- async persistent layout；
- stack map ID/PC mapping；
- PC/source position mapping。

### 16.3 Attribute 原则

runtime helper attribute 宁可保守：

- 所有 helper `nounwind`；
- 不随意添加 `readonly`、`readnone`、`nosync`、`willreturn`；
- host call 不标 `nofree` 或内存无副作用；
- 只有纯 tag/int helper 经过单测和代码审计后才能添加更强 attribute；
- MayGC helper 必须由 statepoint 包裹，不能被错误标为 GC leaf；
- `gc-live` operand bundle 和 relocate uses 必须保留到 codegen。

每次增强 attribute 都应配合 GC stress test 和优化后 IR 检查。

## 17. ORC Engine 与机器码生命周期

### 17.1 Engine

进程通常持有一个共享 `jit::Engine`：

- 初始化 native target；
- 创建 `LLJIT`；
- 建立主 `JITDylib`；
- 注册 runtime ABI 的绝对符号；
- 配置 IR transform/optimization layer；
- 给 `ObjectLinkingLayer` 安装 stack map capture/validation plugin；
- 编译 `ThreadSafeModule`；
- lookup entry symbol；
- 管理 ResourceTracker。

只暴露固定白名单 symbol，避免依赖可执行文件是否使用 `-rdynamic`，也减少 JIT code 可调用的进程接口。

### 17.2 JitCode

```cpp
struct JitCode {
    JitEntry entry = nullptr;
    AsyncFrameDescriptor async_frame;
    StackMapTable stack_maps;
    std::shared_ptr<const ir::Compiled> compiled;
    ResourceOwner resources;
};
```

生命周期规则：

- `Script` 持有 `shared_ptr<const JitCode>`；
- 每个 `JitVm` 再持有一份；
- 最后一个 owner 析构时才调用 `ResourceTracker::remove()`；
- `Engine` 内部 state 必须比 ResourceOwner 活得更久，可由 ResourceOwner 反向持有共享 engine state；
- Engine shutdown 等待正在进行的 compile，并拒绝新的 compile；
- code 卸载前不得存在任何机器码调用栈或可恢复 VM。

### 17.3 Cache key

首版仅按 `Compiled` 实例缓存，不做跨实例内容 cache。因为相同 bytecode 可能绑定不同：

- host function pointer；
- userdata；
- defaults；
- literal payload address；
- Library 生命周期。

若后续做 object cache，key 至少包含：

- bytecode、constants 和调用签名摘要；
- JIT ABI version；
- LLVM major；
- target triple、CPU 和 feature set；
- `JsValue`/async frame layout version；
- statepoint/stack map ABI version；
- runtime helper version。

磁盘 object cache 不属于第一版。

## 18. Script API 集成

### 18.1 保持现有 API

现有 `compile_script()` 和 `compile_template_string()` 默认行为保持解释器可用，不因为系统缺少 LLVM 而改变。

可以新增 options：

```cpp
enum class ScriptBackendMode : std::uint8_t {
    Interpreter,
    PreferJit,
    RequireJit,
};

struct ScriptCompileOptions {
    bool allow_assign = true;
    std::size_t max_depth = kDefaultScriptMaxDepth;
    ScriptBackendMode backend = ScriptBackendMode::Interpreter;
};
```

当前实现的调用示例：

```cpp
ScriptCompileOptions options;
options.backend = ScriptBackendMode::PreferJit;
auto script = compile_script(library, source, options);
if (script && !script->uses_jit()) {
    const jit::JitCompileError *diagnostic = script->jit_compile_error();
    // PreferJit 已安全回退解释器；diagnostic 描述失败阶段。
}
```

以及可选 Engine 参数或独立的 JIT compile API。旧 overload 包装新 API，保持源码兼容。

### 18.2 PreferJit

流程：

1. 正常生成 `shared_ptr<ir::Compiled>`；
2. Engine 尝试生成 `JitCode`；
3. 成功则 Script 同时保存 interpreted IR 和 native code；
4. 失败记录结构化 JIT diagnostic，并只保存 interpreted IR；
5. `exec_sync/async` 自动选择 JitVm；
6. 调试开关可强制解释器。

### 18.3 RequireJit

JIT compile 失败必须返回结构化错误，不允许静默 fallback。错误至少包含：

- 阶段：CFG、SSA、async-frame、LLVM verify、optimize、object emit、stack-map、ORC add、lookup；
- 脚本 PC 和源码位置（若适用）；
- 稳定错误码；
- 有界错误消息；
- 可选 IR dump 路径。

不要把 LLVM `Error` 直接暴露成公共 API 类型。

### 18.4 执行选择

`Script::exec_sync()` 和 `exec_async()` 只在创建 VM 时选择 backend，单次执行中不切换。JIT machine code
一旦开始执行，运行时错误不回退解释器重跑，因为 host call 和 heap mutation 可能已有副作用。

## 19. 编译限制与运行安全

LLVM JIT 生成 native code，必须对输入规模施加比解释器更严格的限制：

- 最大 bytecode count；
- 最大 basic block count；
- 最大 CFG edge count；
- 最大 SSA value/Phi count；
- 最大 async persistent slot count 和 bytes；
- 最大单个 statepoint root count、stack map records 和 stack map bytes；
- 最大 direct argc；
- 最大 LLVM IR instruction count 估算；
- 最大单个 module 的字节数、进程内 module 数和 JIT code bytes；
- 编译队列并发数。

LLVM 编译很难在任意中间点安全取消，因此主要使用输入规模限制、后台 compile pool 和队列背压，不在
EventLoop 上用超时强杀编译线程。

安全规则：

- 对 LLVM module 运行 verifier，失败绝不执行；
- JIT 只解析白名单 runtime symbols；
- async persistent index 和 constant/function index 在 compile 时验证；
- JITLink 后强制验证 stack map version、return PC、location kind 和 offset bounds；
- release 机器码中不依赖 `FIBER_ASSERT` 做边界检查；
- 非法 `resume_id`、状态或 helper status 转换为 `Internal/InvalidState` abort；
- JIT compiler bug 仍可能成为 native memory safety bug，因此多租户不可信脚本场景需单独评估进程隔离；
- ORC memory manager 使用平台支持的 W^X 策略，不自行申请长期 RWX memory。

## 20. 源码位置、诊断与可观测性

### 20.1 运行时位置

每个可能异常/abort/host call 的 instruction 调用前写入 `frame.active_pc`。runtime thunk 使用：

```cpp
compiled.find_position(frame.active_pc)
```

补齐没有 position 的 abort。exception 进入 catch 时仍使用原 call/op PC 查 handler。

### 20.2 编译期 dump

建议提供受控诊断开关：

- bytecode dump；
- CFG before SSA；
- SSA CFG；
- liveness/async spill sets；
- async persistent mapping；
- statepoint ID/root set 和 decoded stack map；
- LLVM IR before optimization；
- LLVM IR after optimization；
- 可选 object/assembly dump。

文件名使用脚本 hash/unique ID，消息和路径有界。默认关闭，不在请求热路径写文件。

### 20.3 统计指标

至少记录：

- JIT compile success/failure/fallback count；
- 按阶段的 compile duration；
- input bytecode/block/value count；
- LLVM IR size、machine code size；
- async persistent slot/byte count；
- statepoint count、最大/平均 native root count 和 stack map bytes；
- async site count；
- JIT VM execution count；
- ORC live module/code bytes；
- JIT runtime internal abort count。

## 21. 测试方案

### 21.1 CFG/SSA 单元测试

新增：

```text
tests/ScriptJitCfgTest.cpp
tests/ScriptJitSsaTest.cpp
tests/ScriptJitLivenessTest.cpp
tests/ScriptJitFrameLayoutTest.cpp
```

覆盖：

- 直线代码；
- if/else 和短路逻辑；
- loop header/back edge；
- break/continue；
- nested try/catch/rethrow；
- foreach iterator variables；
- stack value 跨 block；
- trivial/self-referential Phi；
- unreachable block；
- stack underflow/shape mismatch verifier；
- async call 在 branch、loop 和 catch 中的 spill set；
- catch 中读取 async call 前变量。

### 21.2 LLVM lowering 测试

```text
tests/ScriptJitLlvmTest.cpp
```

覆盖：

- LLVM verifier 通过；
- entry signature 和 symbol lookup；
- PHI/branch 结构；
- async entry switch；
- runtime helper call status branches；
- MayGC call 全部 lower 为 statepoint；
- `gc-live`/`gc.relocate` 在 O2 后仍存在且 root count 正确；
- 不生成 invoke/landingpad；
- async frame offset 与 `DataLayout` 一致。

新增 `ScriptJitStackMapTest.cpp`，直接检查 LLVM 22 x86-64/AArch64 object 中的 stack map，并覆盖 malformed
version、截断 record、未知 DWARF register、register-only location、重复 relocation pair 和 code unload 注销。

### 21.3 Differential execution

将现有脚本执行测试参数化为：

```text
Interpreter
JIT O0
JIT O2
```

对每个 backend 比较：

- result kind；
- value 深度内容；
- exception kind/value；
- abort reason/position；
- host call 次数、参数和顺序；
- heap mutation 后的对象内容；
- iterator mutation 行为。

重点复用全部 `ScriptExecutionTest`、`ScriptRuntimeOpsTest`、`ScriptValueOpsTest` 和标准库测试。

### 21.4 GC stress

新增 `ScriptJitGcTest.cpp`，提供测试模式在每个 JIT safepoint 强制 `collect()`，而不是只依赖 threshold：

- root/local value 跨同步 host call；
- object/array result 跨连续分配；
- Phi value 跨 branch safepoint；
- loop 中 native spill slot 被复用；
- exception value 进入 catch；
- pending async result 在 resume 前 collection；
- direct async arguments 在 coroutine 内 collection 后仍有效；
- spread owner/elems 在 suspend 期间存活；
- JitVm 析构后对象可被回收。

GC stress 必须分别在 LLVM O0/O2 运行，以捕捉 optimizer/statepoint lowering 漏掉 root 的问题。测试还要在
collection 时校验 active return PC 能精确命中 stack map，不能回退到保守 native stack 扫描。

### 21.5 Async 测试

新增 `ScriptJitAsyncTest.cpp`，至少覆盖：

- 单次 await；
- 两次顺序 await；
- loop 内多次 await；
- branch 两侧不同 await site；
- direct 参数挂起后读取；
- default 参数挂起后读取；
- spread 临时 array 挂起后读取；
- async success/exception/abort；
- async exception 被最近 catch 捕获；
- nested catch/rethrow；
- task allocation failure；
- completion 后再触发 GC；
- Script/JitCode owner 在 suspend 期间释放外部引用；
- 非法重复 completion 不导致重入或 UAF。

### 21.6 生命周期测试

新增 `ScriptJitLifecycleTest.cpp`：

- Script 销毁但活动 JitVm 仍能 resume；
- JitVm 完成后 ResourceTracker 才能卸载；
- 多个 VM 共享同一 JitCode；
- Engine shutdown 等待 compile；
- code remove 后不能再创建 VM；
- PreferJit 编译失败正确 fallback；
- RequireJit 编译失败返回错误。

### 21.7 Fuzz/Differential

后续增加受限语法生成器：

- 生成合法 AST/脚本；
- 相同 root 和 deterministic library 下分别解释/JIT；
- 比较结果与 heap graph；
- 随机插入 branch、loop、try/catch、throw、async；
- 随机将 GC safepoint 插入 runtime helper 边界。

优先 fuzz 高层 CFG/SSA verifier、async indices 和 stack map parser，避免仅依赖 native crash 暴露问题。

## 22. Benchmark 方案

性能结论必须来自当前 C++ 运行时，不能直接使用 Java 文档中“AOT 约 10 倍”的数字。

至少建立以下 workload：

1. pure integer/boolean loop；
2. property/index read-heavy；
3. object/array construction；
4. string operations；
5. branch-heavy routing script；
6. sync host-call-heavy；
7. async host-call-heavy；
8. lite_nginx 真实配置脚本。

分别测量：

- interpreter throughput/latency；
- JIT O0/O1/O2 throughput/latency；
- compile duration；
- machine code bytes；
- async persistent allocation bytes；
- statepoint spill/reload 指令数和 native stack frame bytes；
- GC collection count/time/live bytes；
- 首次执行和稳定执行；
- 每个请求执行一次与同一 Script 高频复用。

预期收益最大的是控制流和简单数值逻辑；host I/O 或大规模 JSON 操作占主导时，JIT 收益可能较小。
第一版验收不绑定未经测量的固定倍数，但必须证明没有显著 GC、native-frame 和 code-size 回归。

## 23. 分阶段实施

### Phase 0：语义冻结与基线

内容：

- 固化 opcode effect 表；
- 补 async exception/abort 和 GC stress 的解释器测试；
- 明确 borrowed lifetime contract；
- 建立 interpreter differential harness；
- 保存代表性 benchmark 基线。

退出标准：所有现有 script tests 和新增语义测试通过，effect 表逐 opcode 审核完成。

### Phase 1：CFG + SSA（无 LLVM）

内容：

- block discovery；
- stack shape propagation；
- bytecode simulation；
- MaybePhi resolution；
- exception edges；
- verifier 和文本 dump；
- liveness。

退出标准：Java 参考中的 CFG/SSA 类测试在 C++ 对等通过；所有当前 bytecode 都能构造并验证 CFG。

### Phase 2：同步 boxed LLVM backend

内容：

- optional `fiber_script_jit` target；
- LLVM module/data layout；
- runtime C ABI；
- branch/Phi/return/exception/abort lowering；
- ORC add/lookup/remove；
- `JitCode` 和同步 `JitVm`；
- O0/O2 differential tests。

退出标准：不含 async opcode 的现有脚本测试在 JIT O0/O2 全部与解释器一致。

### Phase 3：GC root safepoints

内容：

- TypeMask；
- instruction-level live roots；
- `addrspace(1)` root extraction 和显式 statepoint/relocate；
- `fiber-script-statepoint` GCStrategy；
- JITLink stack map capture/validation/registry；
- x86-64 与 AArch64 runtime entry trampolines；
- `visit_heap_ref()` 和 active stack anchor；
- helper effect/alias 审核；
- forced collection tests；
- O0/O2 object/assembly 和 stack map 检查。

退出标准：每个 safepoint 强制 GC 时，同步 differential tests 全部通过，无 UAF/漏标。

### Phase 4：异步状态机

内容：

- async spill；
- persistent slots；
- direct stable args；
- spread owner；
- resume dispatch；
- completion callback；
- async exception/abort/catch；
- code/VM 生命周期。

退出标准：全部 async differential、GC stress 和 lifecycle tests 通过。

### Phase 5：集成与硬化

内容：

- PreferJit/RequireJit；
- compile pool/limits/metrics；
- diagnostic dumps；
- lite_nginx 集成；
- sanitizer、fuzz、长时间运行；
- code unload 和 Engine shutdown。

退出标准：JIT 可选开启，关闭时不增加 LLVM 依赖；真实应用可安全 fallback，指标可定位失败阶段。

### Phase 6：性能优化

内容按 benchmark 决定：

- tag fast path；
- statepoint root 去重和 TypeMask 精化；
- async storage pool/coloring；
- runtime helper 拆分和 attribute；
- module/object cache；
- CPU-specific codegen。

任何优化都必须保持 interpreter/JIT differential 和 GC stress tests。

## 24. 主要风险与对策

### 24.1 漏标 GC root

风险最高。对策：

- effect 默认保守；
- 每个 MayGC call 的 `gc-live` set 可 dump、可验证；
- 每 safepoint forced collection；
- O0/O2 双重测试；
- stack map parser fuzz 和 object-level golden tests；
- trampoline/return-PC/SP mapping 按 target 单测；
- 不支持的 location/module 直接拒绝 JIT。

### 24.2 Async 参数悬空

Java spill 规则不能直接复制。对策：

- direct async args 固定放 persistent 连续区域；
- spread owner 保存在 persistent field；
- HostCallFrame 由 JitVm 成员持有；
- 复用现有挂起后读参数测试。

### 24.3 LLVM 不能自动优化动态语义

Opaque helper 限制优化。对策：先获得 dispatch/stack 消除收益，再由 benchmark 驱动 tag fast path；不要为
追求理论峰值提前实现 deopt。

### 24.4 LLVM 版本和二进制体积

对策：JIT 单独 target、默认关闭、固定 major、优先 shared libLLVM、核心公共 header 不包含 LLVM 类型。

### 24.5 编译阻塞 EventLoop

对策：配置加载/后台 compile pool、输入规模限制、队列背压、解释器 fallback。

### 24.6 Code unload UAF

对策：Script/JitVm 共享 `JitCode`，ResourceTracker 随最后 owner 析构，Engine state 反向被资源 owner 持有。

### 24.7 Borrowed backing 失效

对策：明确完整脚本生命周期 contract；需要短生命周期 host view 时实现深度 promotion，不做不完整的顶层
promotion。

### 24.8 Native code 安全边界

对策：LLVM verifier、所有 index 编译期验证、白名单 symbols、W^X、不把 JIT 当作不可信租户的安全沙箱。

## 25. 被否决的首版方案

### 25.1 AST 直接生成 LLVM

否决原因：

- 会复制现有 bytecode compiler 的作用域、default args、foreach、try/catch 和源码位置逻辑；
- interpreter/JIT 更容易语义漂移；
- 无法直接复用 `Compiled` 测试和 fallback。

### 25.2 直接把整个解释器 switch 翻译成 LLVM

否决原因：

- 仍保留 pc/sp 和虚拟 stack；
- LLVM 很难跨动态 opcode dispatch 恢复语言级 SSA；
- async spill 和 GC roots 仍需额外分析；
- 收益有限。

### 25.3 使用 LLVM coroutine 自动生成 frame

否决原因：

- 当前 `AsyncTask` 和 Script 外层 coroutine 已经定义调度协议；
- GC 仍需认识 coroutine frame 中的 `JsValue`；
- frame layout 和跨 LLVM 版本行为更难控制；
- Java/Rust 式显式状态机已经足够。

### 25.4 使用 JitVm frame root slots 保存同步临时值

否决原因：每个 safepoint 都要复制完整 boxed `JsValue`，增加 persistent allocation、store traffic 和无效 slot
扫描，也阻碍“同步值保持 LLVM local”的目标。首版采用 statepoint + stack map，只让 LLVM 保存活跃
`GcHeader *` 的 pointer-sized machine location。

### 25.5 只用 llvm.experimental.stackmap/patchpoint

否决原因：普通 stackmap 面向任意 live values，不提供完整 GC relocation 语义，value 可能位于 helper 会破坏的
register 中。statepoint 明确表示 safepoint call，并通过 `gc-live`/`gc.relocate` 让 backend 生成可更新、可验证的
root locations。

### 25.6 复制 Java optimizer 和 ValueAllocator

否决原因：LLVM 已负责大多数通用优化、SSA destruction 和寄存器分配。C++ 前端只保留语言级 CFG、effect、
liveness、async 和 GC 分析。

### 25.7 JIT 失败后从中途切回解释器

否决原因：执行可能已经调用 host 或修改 heap，重跑会重复副作用。fallback 只允许发生在 VM 创建/执行前。

## 26. 首个可交付 PoC

PoC 不应一开始覆盖全部语言。推荐最小纵切：

1. `LOAD_CONST`、`LOAD_ROOT`、`LOAD_VAR`、`STORE_VAR`；
2. integer/boolean 常量和简单比较；
3. `JUMP`、conditional jump、Phi；
4. `END_RETURN` 的 value/void；
5. 一个通过 C ABI 调用的 boxed binary helper；
6. 一个可能 exception 的 helper 和单层 catch；
7. 一个 live heap value 跨 statepoint，并在 helper 内强制 GC；
8. JITLink 捕获/解析 stack map，collector 从 native spill location 标记对象；
9. ORC compile、lookup、execute、stack map unregister、code remove；
10. interpreter/JIT differential。

PoC 通过后再添加 host call 和 async。不要用只支持无 GC 的 arithmetic demo 判断总体方案完成度。

## 27. 完成定义

LLVM JIT 第一版完成需要同时满足：

- 当前支持的所有 reachable opcode 都有 lowering 或明确 compile-time rejection；
- sync/async、value/void、exception/abort 与解释器一致；
- direct/default/spread async 参数挂起后有效；
- try/catch 和 async exception 使用精确原 PC；
- O2 下 forced-GC differential tests 全部通过；
- 所有 MayGC call 均有 statepoint，优化后 `gc-live` root set 与语言级 liveness 一致；
- x86-64/AArch64 stack map、trampoline 和 SP-relative root walk 通过 object/assembly/stress tests；
- 同步临时值不分配 JitVm frame root slots；
- Script、JitVm、JitCode、Compiled、Engine 生命周期无 UAF；
- LLVM 不可用或 PreferJit 失败时解释器正常工作；
- RequireJit 返回结构化错误；
- JIT 默认关闭时核心 target 不链接 LLVM；
- 有编译延迟、code size、async frame、statepoint/stack map、fallback 和内部错误指标；
- 完成真实 lite_nginx 脚本 benchmark，结论基于实测而非 Java 数字。

满足这些条件后，才进入 tag fast path、async slot coloring、cache 等第二阶段优化。
