# Script Interpreter / LLVM JIT Benchmark

## 1. 目标与边界

本 benchmark 直接执行脚本，不接入 HTTP、EventLoop 或 lite_nginx。它回答两个独立问题：

1. 同一份源代码从 parser 到可执行 Script，Interpreter 与 LLVM JIT 的编译延迟相差多少；
2. 编译完成并预热后，重复调用 `Script::exec_sync()` / `exec_async()` 时，JIT 相对解释器的稳态收益是多少。

它不是 HTTP 吞吐量替代品，也不把首次 LLVM 初始化时间混入稳态执行结果。I/O、线程调度、网络协议和配置解析均不在测量范围内。

## 2. Workload

| Case | 主要覆盖 | 输入 |
|---|---|---|
| `route_branch` | 短脚本、属性读取、字符串比较、多级路由分支、每次执行的 VM 固定成本 | 固定 route object |
| `foreach_arithmetic` | foreach、整数运算、变量更新、loop Phi | 64 个整数 |
| `branch_heavy` | loop 内多分支、比较与不同回边值 | 64 个整数 |
| `mixed_float_arithmetic` | loop 内 Float 乘、减、除的多态慢路径，验证构建期 bitcode 导入和跨 helper 内联 | 64 个整数 |
| `object_property` | 每次执行创建对象、属性 get/set、分支、GC 压力 | 64 个整数 |
| `array_build` | 每次执行复制数组并执行动态 index get/set | 64 个整数 |
| `sync_host_call` | 固定参数同步 C ABI host call | 64 个整数 |
| `async_host_call` | 立即完成的 AsyncTask、persistent values、resume dispatch | 64 个整数 |

`--input-size` 可以修改 loop case 的元素数。所有 case 返回整数，计时前先执行一次 Interpreter/JIT differential check；结果类型或值不一致时 benchmark 直接失败。

## 3. 测量方法

- 使用 `Release` 构建，JIT 固定采用当前 O2 pipeline；
- Interpreter 和 JIT 使用相同 source、Library 和逻辑输入，但各自持有独立 `GcHeap`，避免一端的 allocation/GC 状态污染另一端；
- 先单独执行一次 JIT engine warmup，并输出 `JIT_ENGINE warmup_compile_us`；
- 每个 case 分别重复编译，报告 compile time 中位数；JIT compile time 包含 CFG/SSA、LLVM O2、codegen、JITLink、stack-map 和 ORC lookup；
- 执行计时前自动校准每个 backend 的 batch iteration，使单个 sample 接近 `--target-ms`；
- 每个 case 默认测 7 轮，并逐轮交换 Interpreter/JIT 的先后顺序，降低温度、频率和系统漂移偏差；
- 报告每次完整 Script 执行的 median ns、IQR、每秒 work units，以及 `interp_ns / jit_ns`；`jit_inlined_helpers`
  给出该脚本被导入并在 O2 后消除调用的 operator helper 数量；
- `speedup > 1` 表示 JIT 更快；`jit_delta_pct < 0` 表示 JIT 延迟更低；
- GC 保持正常启用。对象和数组 case 因而包含真实 allocation/collection 成本；
- benchmark 没有增加 frame root slots；JIT 仍使用 production LLVM statepoint/stack-map roots，只有跨异步边界的值进入 persistent storage；
- checksum 在计时区内消费每次结果，防止宿主编译器删除执行循环。

短脚本容易受 CPU frequency 和批次顺序影响。正式对比建议关闭其他负载，并用 `taskset` 固定到一个物理核；至少重复运行三次，比较各次 median，而不是只取单次最好值。

## 4. 构建与运行

```bash
cmake -S . -B build-script-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_ENABLE_SCRIPT_JIT=ON \
  -DFIBER_BUILD_SCRIPT_BENCHMARK=ON \
  -DFIBER_BUILD_TESTS=OFF \
  -DFIBER_BUILD_APPS=OFF \
  -DFIBER_BUILD_EXAMPLES=OFF \
  -DFIBER_BUILD_NACOS=OFF \
  -DFIBER_BUILD_CAT=OFF \
  -DFIBER_BUILD_PROMETHEUS=OFF

cmake --build build-script-bench --target script_jit_benchmark -j2

taskset -c 2 ./build-script-bench/script_jit_benchmark \
  --target-ms 200 --rounds 7 --compile-samples 3 --input-size 64
```

快速检查单个 case：

```bash
./build-script-bench/script_jit_benchmark \
  --filter foreach_arithmetic --target-ms 50 --rounds 3
```

程序输出一行环境信息、一行 JIT engine warmup、每个 case 一行 `RESULT`，最后输出全部 case speedup 的几何平均数。原始 `RESULT` 行设计为稳定的 `key=value` 格式，便于脚本收集。

## 5. 本机执行结果

2026-08-15 在完成构建期 operator bitcode 导入和传递内联后，于当前开发机执行三次独立进程测量：

- CPU：13th Gen Intel Core i7-13700H，进程固定在逻辑 CPU 2；
- 系统：Linux 6.6.114.1-microsoft-standard-WSL2 x86_64；
- 编译器：Clang 22.1.6，`Release`，LTO 开启；
- 参数：`--target-ms 200 --rounds 7 --compile-samples 3 --input-size 64`。

三次完整运行的 geometric mean speedup 分别是 `2.792x`、`2.800x`、`2.805x`。下表给出第三次运行的成对时间，保证每行 speedup 都由同一次进程中的 Interpreter/JIT median 计算；最后一列给出三次独立运行的 speedup 范围。

| Case | 内联 helper | Interpreter ns/exec | JIT ns/exec | Speedup | JIT latency change | 三次 speedup 范围 |
|---|---:|---:|---:|---:|---:|---:|
| `route_branch` | 0 | 275.84 | 246.74 | 1.118x | -10.55% | 1.110x–1.125x |
| `foreach_arithmetic` | 3 | 4,219.66 | 796.21 | 5.300x | -81.13% | 5.025x–5.300x |
| `branch_heavy` | 3 | 4,641.58 | 357.42 | 12.986x | -92.30% | 12.957x–13.153x |
| `mixed_float_arithmetic` | 3 | 2,768.22 | 664.94 | 4.163x | -75.98% | 4.120x–4.163x |
| `object_property` | 2 | 13,326.19 | 9,985.51 | 1.335x | -25.07% | 1.335x–1.372x |
| `array_build` | 1 | 5,963.95 | 3,084.44 | 1.934x | -48.28% | 1.917x–1.942x |
| `sync_host_call` | 0 | 2,577.54 | 971.11 | 2.654x | -62.32% | 2.654x–2.692x |
| `async_host_call` | 0 | 3,847.96 | 2,201.70 | 1.748x | -42.78% | 1.736x–1.751x |

同一次运行的编译延迟如下。Interpreter 列包含 parser/IR 构建，JIT 列还包含 CFG/SSA、按需解析和链接
bitcode、LLVM 优化、机器码安装；JIT engine 已预热，单独的 engine warmup compile 为 3.38–3.53 ms，
没有计入执行时间。

| Case | Interpreter compile | JIT compile |
|---|---:|---:|
| `route_branch` | 10.12 us | 9.76 ms |
| `foreach_arithmetic` | 12.38 us | 32.90 ms |
| `branch_heavy` | 16.43 us | 47.79 ms |
| `mixed_float_arithmetic` | 12.22 us | 25.02 ms |
| `object_property` | 18.80 us | 59.84 ms |
| `array_build` | 14.73 us | 30.61 ms |
| `sync_host_call` | 6.99 us | 12.52 ms |
| `async_host_call` | 10.64 us | 13.16 ms |

### 5.1 优化前后对照

同一机器、同一 workload 的 boxed-runtime 基线 geometric mean 只有约 `1.01x`；`foreach_arithmetic`、
`branch_heavy` 分别约为 `1.09x`、`1.01x`。本轮改动没有引入 unboxed frame 或自定义 LLVM pass，主要变化是：

- 删除 binary/unary opcode 到函数指针的运行时分派；
- 整数算术、比较和常见 truth test 生成原生 LLVM IR，保留 tag guard；
- NoGC 多态慢路径调用各自的精确 helper，不生成 statepoint；
- 只有字符串 `+` 等 MayGC 慢路径及分配/host call 使用 statepoint；
- array iterator 的 `next/key/value` 直接 lower，object iterator 保留精确慢路径。

因此本次数据也验证了原先的瓶颈判断：仅把解释器 CFG 改写为 boxed helper call 几乎没有收益；让 LLVM
看见热点运算、并把 NoGC 调用移出 statepoint 后，循环 workload 才出现稳定的数量级差异。

### 5.2 operator bitcode 隔离对照

为了隔离本轮 bitcode 改动，使用改动前提交 `1eb4c1e`，只回填完全相同的
`mixed_float_arithmetic` workload，并与当前代码各执行三次独立进程。旧版本的 Float 慢路径调用 native
helper；新版本导入 multiply/minus/divide 的构建期 bitcode，强制传递闭包内联，并在 O2 后验证 JIT entry
不再调用导入定义。

| 指标 | native helper | bitcode 传递内联 |
|---|---:|---:|
| JIT ns/exec 范围 | 1,556.40–1,617.76 | 643.10–650.50 |
| Interpreter/JIT speedup 范围 | 1.581x–1.687x | 4.110x–4.188x |
| JIT compile 范围 | 10.13–11.34 ms | 23.93–25.62 ms |

因此传递内联使这个 helper 密集的 JIT 执行路径再快约 `2.4–2.5x`，但每个脚本多支付约 13–15 ms 编译
成本。只消除外层 C ABI wrapper 不够：若 `Binaries::*`、`JsValue` 等传递依赖仍是内部 call，实测没有收益；
实现和结构校验必须覆盖从 JIT entry 可达的整个导入闭包。

## 6. 结果分析与后续优化

当前实现八项 workload 的稳态 geometric mean 为约 `2.80x`。纯分支/整数循环达到约 `13x`，foreach
算术达到 `5.0–5.3x`，新增 Float helper 热循环达到约 `4.1x`；动态属性、数组 mutation、host call 和 async 仍受外部 runtime、分配或挂起协议限制，
但也都已经快于解释器。短 route 脚本只有约 `1.11x`，说明 VM 创建和属性/字符串访问的固定成本仍占主导。

编译延迟为 10–60 ms；含导入 helper 的脚本比 native helper 版本更贵，仍然不能在每次请求或每次脚本执行时支付。生产接入需要以脚本内容/配置版本
缓存编译结果，并设置 hot threshold 或后台编译，只对重复执行足够多次的脚本启用 JIT。

后续性能工作按收益和风险排序：

1. 为固定 shape 属性访问和常见 array index get/set 增加 guarded fast path；
2. 为已解析同步 host callable 生成专用调用路径，减少通用 host dispatch；
3. 完成编译缓存、hot threshold 和后台编译，并计算各 workload 的盈亏平衡执行次数；
4. 只有在剩余纯计算 workload 仍有明确瓶颈时，再考虑 unboxed loop Phi；当前 `4.1x–13.2x` 的结果不足以
   支持立即引入 deopt/representation merge 的复杂度。

以上数字只代表该次本机运行，不能直接外推为生产 HTTP 吞吐量；WSL2 调度也使少数 case 的跨进程波动高于单进程 IQR。
