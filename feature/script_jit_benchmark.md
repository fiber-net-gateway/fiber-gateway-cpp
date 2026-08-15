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
- 报告每次完整 Script 执行的 median ns、IQR、每秒 work units，以及 `interp_ns / jit_ns`；
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

2026-08-15 在完成 operator/iterator native lowering 后，于当前开发机执行三次独立进程测量：

- CPU：13th Gen Intel Core i7-13700H，进程固定在逻辑 CPU 2；
- 系统：Linux 6.6.114.1-microsoft-standard-WSL2 x86_64；
- 编译器：Clang 22.1.6，`Release`，LTO 开启；
- 参数：`--target-ms 200 --rounds 7 --compile-samples 3 --input-size 64`。

三次完整运行的 geometric mean speedup 分别是 `2.530x`、`2.558x`、`2.542x`。下表给出第三次运行的成对时间，保证每行 speedup 都由同一次进程中的 Interpreter/JIT median 计算；最后一列给出三次独立运行的 speedup 范围。

| Case | Interpreter ns/exec | JIT ns/exec | Speedup | JIT latency change | 三次 speedup 范围 |
|---|---:|---:|---:|---:|---:|
| `route_branch` | 261.24 | 236.90 | 1.103x | -9.32% | 1.081x–1.103x |
| `foreach_arithmetic` | 3,777.31 | 829.56 | 4.553x | -78.04% | 4.483x–4.620x |
| `branch_heavy` | 4,508.01 | 359.95 | 12.524x | -92.02% | 12.499x–12.670x |
| `object_property` | 12,553.27 | 9,018.11 | 1.392x | -28.16% | 1.392x–1.422x |
| `array_build` | 5,568.31 | 2,970.22 | 1.875x | -46.66% | 1.828x–1.913x |
| `sync_host_call` | 2,446.25 | 949.36 | 2.577x | -61.19% | 2.545x–2.578x |
| `async_host_call` | 3,605.20 | 2,220.64 | 1.623x | -38.40% | 1.619x–1.656x |

同一次运行的编译延迟如下。Interpreter 列包含 parser/IR 构建，JIT 列还包含 CFG/SSA、LLVM 优化和机器码安装；JIT engine 已预热，单独的 engine warmup compile 为 3.74–4.06 ms，没有计入执行时间。

| Case | Interpreter compile | JIT compile |
|---|---:|---:|
| `route_branch` | 9.68 us | 9.97 ms |
| `foreach_arithmetic` | 13.44 us | 14.59 ms |
| `branch_heavy` | 16.26 us | 21.22 ms |
| `object_property` | 18.85 us | 46.05 ms |
| `array_build` | 12.11 us | 23.14 ms |
| `sync_host_call` | 10.68 us | 12.93 ms |
| `async_host_call` | 8.73 us | 12.54 ms |

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

## 6. 结果分析与后续优化

当前实现七项 workload 的稳态 geometric mean 为约 `2.54x`。纯分支/整数循环达到 `12.5x`，foreach
算术达到 `4.5x`；动态属性、数组 mutation、host call 和 async 仍受外部 runtime、分配或挂起协议限制，
但也都已经快于解释器。短 route 脚本只有约 `1.09x`，说明 VM 创建和属性/字符串访问的固定成本仍占主导。

编译延迟为 10–46 ms，仍然不能在每次请求或每次脚本执行时支付。生产接入需要以脚本内容/配置版本
缓存编译结果，并设置 hot threshold 或后台编译，只对重复执行足够多次的脚本启用 JIT。

后续性能工作按收益和风险排序：

1. 为固定 shape 属性访问和常见 array index get/set 增加 guarded fast path；
2. 为已解析同步 host callable 生成专用调用路径，减少通用 host dispatch；
3. 完成编译缓存、hot threshold 和后台编译，并计算各 workload 的盈亏平衡执行次数；
4. 只有在剩余纯计算 workload 仍有明确瓶颈时，再考虑 unboxed loop Phi；当前 `4.5x–12.5x` 的结果不足以
   支持立即引入 deopt/representation merge 的复杂度。

以上数字只代表该次本机运行，不能直接外推为生产 HTTP 吞吐量；WSL2 调度也使少数 case 的跨进程波动高于单进程 IQR。
