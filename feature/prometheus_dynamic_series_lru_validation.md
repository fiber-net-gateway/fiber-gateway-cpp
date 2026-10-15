# Prometheus 动态 series LRU 实现与验证

日期：2026-10-15。

实现依据：[动态标签与 LRU 方案](prometheus_dynamic_series_lru_design.md)。

## 实现

- `SeriesOptions` 默认 `Static`；Counter/Histogram 可选择 `Lru`，Gauge 保留静态接口。
- `DynamicCounter` / `DynamicHistogram` 为只能通过冻结后的 Registry 绑定的可复制 family 对象，
  不绑定 EventLoop、不暴露 series Ref，生命周期由宿主保证不超过 Registry。
- 每 family 一把 mutex、N 个固定槽位、N 个 hash bucket、侵入式严格 LRU；hash chain 删除直接
  回收链接，没有 tombstone。标签元组按长度及内容 hash，再按完整内容比较。
- 稳定命中不分配；新增先分配标签候选再提交替换。标签使用独立、可失败、可释放的分配；
  无增长 arena。固定值区与槽位复用，淘汰立即释放标签，重建清零。
- 每 family 完整持锁复制，快照拥有标签和值，锁外按 unsigned byte 标签元组排序、编码。
  每代动态批次占一个完成项，即使没有 shard 也不会提前 Ready；捕获失败由全代共享，静态
  回调仍全部完成。取消只解除当前调用的附着关系。静态 Ref 更新实现未改动。
- freeze 的运行期数组改为可失败的固定分配，失败回滚后可重试。

## 内存记账

store 预算包括 family 运行期结构、固定索引/槽位/值、拥有的标签以及尚未发布的候选。
分配按 Linux `malloc_usable_size()` 计入 size class 容量，另留两个 `size_t` 的块头余量。
这不是整个进程 RSS 上限，也不包含 libc/jemalloc 的全局 arena、线程缓存或 sanitizer 自身开销。

每个 family 快照使用一个连续缓冲，按 N 预留样本、label view、值的容量，尾部容纳拥有的标签字节。
Registry 汇总所有快照的已分配容量，增长时先检查新旧缓冲同时存在的预算再替换，保留已分配容量
供下一代复用。排序为原地排序，没有堆上排序临时数组。输出继续使用每调用的独立上限。

## 回归覆盖

`DynamicSeriesTest` 覆盖配置/ID/类型/模式校验、N=1/2、命中与零增量刷新、采集不刷新、
重新累计、标签所有权、转义边界、空 schema、字节字典序、元组分割与强制完整 hash 碰撞、
Histogram 边界/单位/完整重建、创建失败原子性、freeze 各分配点失败及回滚、显式预算、
长短标签持续替换回收、8 线程精确累计、并发淘汰时快照标签和值的一致性，以及跨 family 的
快照预算和双缓冲峰值。

`DynamicSnapshotTest` 覆盖两个 shard 与动态 family 混合输出、动态值只编码一次、复制后释放
live 标签、跨 loop 重叠采集、独立输出错误、共享预算/分配错误及回调收尾、零 shard 错误恢复、
停止采集后仍可记录、销毁 leader 或 follower 后剩余调用继续读取旧快照。

静态路径继续运行 `MetricValueTest`、`MetricsRegistryTest`、`TextEncoderTest` 和
`MultiLoopSnapshotTest`。

## 验证记录

已完成：

| 检查 | 结果 |
|---|---|
| `cmake --build build -j 4` | 完整构建成功 |
| `build/components/prometheus/fiber_prometheus_tests` | 36/36 通过 |
| ASan + UBSan 模块测试 | 36/36 通过，无 sanitizer 报告，开启 leak detection |
| TSan 模块测试 | 36/36 通过，无数据竞争报告 |
| 模块全部 12 个 TU 的 clang-tidy 20 | 0 findings、0 frontend errors |
| `./format_code.sh` / `git diff --check` | 通过 |

`ctest --test-dir build --output-on-failure -j 4`：共 2,712 项，2,708 项通过，4 项条件跳过，
0 失败，耗时 57.79 秒。跳过的是 `Http3ClientTest.NginxInterop` 以及 Nacos ConfigService、
NamingService、Rpc 的 `RnacosInteropWhenEnabled`，未把这些外部互操作测试记为通过。

普通构建树为 Release + ThinLTO。ASan 树为 RelWithDebInfo，CXX flags 为
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`；
TSan 树 CXX flags 为 `-fsanitize=thread -fno-omit-frame-pointer -g -O1`。
两个 sanitizer 树均禁用 LTO 和 jemalloc，使用各自的 `_deps/` 二进制目录，复用仓库缓存的源码，
避免不同构建树覆盖共享依赖的产物。执行命令：

```bash
cmake --build build-asan --target fiber_prometheus_tests -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  build-asan/components/prometheus/fiber_prometheus_tests
cmake --build build-tsan --target fiber_prometheus_tests -j 2
TSAN_OPTIONS=halt_on_error=1 build-tsan/components/prometheus/fiber_prometheus_tests
./format_code.sh
./run_tidy.sh -p build -j 2 apps/prometheus/src/*.cpp apps/prometheus/tests/*.cpp apps/prometheus/bench/*.cpp
git diff --check
```

最后一次基准输出上限修正后，还单独运行了
`./run_tidy.sh -p build -j 2 apps/prometheus/bench/DynamicBenchmark.cpp`，仍为零 findings。
clang-tidy 范围包含库、测试及两种基准的所有 TU；本次公共头的仓库内使用方均位于这些目标。
最初启动的全仓 537 TU 检查因耗时停止，未作为全仓 lint 通过证据。
本机未安装 `promtool`，未执行其格式校验；格式由模块的精确输出断言与共享 encoder 回归验证。

## 性能方法与边界

可选 target `prometheus_dynamic_benchmark` 独立于现有静态 Ref 基准。每个动态操作对包含一次
Counter inc 和一次 Histogram observe；p95/p99 每 64 对采样一次，测量两个记录调用的总耗时。
满容量换标签的吞吐量还包括不分配堆内存的标签生成；首次创建单列。记录侧的标签分配次数由
成功创建数推导，稳定命中为零；不统计输出/快照的分配次数。

分别运行 1/2/4/8 线程共享 family 与独立 family、首次创建、N=64 持续替换、目标间隔 1 ms 的
并行 scrape，以及 4096 字节标签和 128 个 bucket。采集时间是端到端值，包含锁等待、复制、
排序和编码；没有把它写成精确持锁时间。实际 scrape 频率受本次采集耗时和调度影响。
基准将每次输出上限配置为 64 MiB，以容纳 4096 字节标签展开 128 个 bucket 的约 35 MiB 文本；
库默认的 16 MiB 输出上限不变，使用默认值会正确返回 `MessageTooLarge`。

硬件为 Intel Core i5-13500H，当前环境可见 4 个 CPU、约 3.3 GiB 内存；Linux 6.17.0-41-generic、
Clang 20.1.8、glibc 2.42。Release 使用 `-O3 -DNDEBUG` 和 ThinLTO，运行时 allocator 为 libc。
线程未绑核；8 线程超过当前可用 CPU 数。结果用于局部路径对比，不能据此证明网关生产请求的
延迟 SLA。严格 family LRU 的锁竞争和长标签快照成本需要结合实际业务负载评估。

最终测量（所有构建、测试与静态分析结束后单独运行；一轮样本，不作为性能阈值）：

```bash
./build/components/prometheus/prometheus_dynamic_benchmark 500000
./build/components/prometheus/prometheus_record_benchmark 10000000
```

| 场景 | 线程 | 百万操作对/s | 采样 p95 (us) | 采样 p99 (us) | 记录标签分配次数 |
|---|---:|---:|---:|---:|---:|
| steady_shared | 1 | 5.356 | 0.231 | 0.365 | 0 |
| steady_separate | 1 | 5.830 | 0.210 | 0.317 | 0 |
| steady_shared | 2 | 4.942 | 0.759 | 1.607 | 0 |
| steady_separate | 2 | 10.677 | 0.239 | 0.381 | 0 |
| steady_shared | 4 | 3.613 | 3.057 | 19.400 | 0 |
| steady_separate | 4 | 16.459 | 0.298 | 0.420 | 0 |
| steady_shared | 8 | 3.655 | 8.544 | 58.087 | 0 |
| steady_separate | 8 | 18.473 | 0.342 | 0.432 | 0 |
| cold | 1 | 2.622 | 0.322 | 0.322 | 2048 |
| churn | 1 | 4.191 | 0.411 | 0.508 | 1000000 |
| churn_scrape | 4 | 1.685 | 13.130 | 37.708 | 4000000 |
| large_labels_buckets_scrape | 4 | 0.151 | 31.153 | 61.931 | 400008 |

首次创建场景固定执行 1,024 对，其余普通场景每线程 500,000 对，大标签场景每线程 50,001 对。
首次创建只有 16 个延迟采样点，其百分位数据仅作原始读数，不宜推断稳定分布。

32 字节标签持续替换后的两个 store 合计为 19,392 字节；4096 字节标签/128 bucket 场景为
601,024 字节。稳定命中标签分配为 0；单线程持续替换 500,000 对产生 1,000,000 次标签分配，
两类指标各一次，不隐藏创建/回收成本。

普通并行 scrape 场景完成 932 次采集，最大端到端采集时间 1,003 us；大标签/多 bucket 场景完成
36 次，最大 67,872 us。后者还包含约 35 MiB 文本输出的构造成本，不能将该耗时等同于复制持锁时间。
本次没有精确的 mutex 持锁时间测量，也没有对任意更大 N/B 配置给出性能结论。

静态基准为 Counter 3,830,535,574 ops/s、Histogram 298,739,552 ops/s，均为单项更新；动态基准
单位是 Counter + Histogram 的操作对，两者不能按相同操作直接相减。本次未做变更前/后多轮统计比较。

同 family 严格 LRU 在 4/8 线程下的采样尾延迟明显高于独立 family；此结果支持保留每 family
独立锁及静态总量路径，但不代表已通过具体网关的生产延迟验收。
