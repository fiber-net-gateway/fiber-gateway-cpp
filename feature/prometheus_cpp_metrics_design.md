# Prometheus Metrics 库设计方案

## 1. 目标与边界

在 `apps/prometheus` 下实现一个面向高性能 C++ 服务的 Prometheus Metrics
静态库。库的核心职责是：

- 定义和记录 Counter、Gauge、Histogram；
- 支持多个 EventLoop/worker 的本地指标分片；
- 在不产生 C++ data race 的前提下生成当前指标快照；
- 聚合各 worker 快照；
- 按 Prometheus text exposition format 0.0.4 编码；
- 将编码结果输出到 `fiber::mem::IoBufChain`，并提供受限的 `IoBuf`
  输出接口。

记录路径的目标：

- owner thread 内无锁；
- 不使用 atomic RMW；
- 不分配动态内存；
- 不执行字符串、map 或 hash 查找；
- API 为内联、`noexcept` 的简单整数更新。

快照和编码属于慢路径，允许在初始化或 collect 过程中使用动态内存、同步原语和
格式化逻辑，但不得影响记录路径。

### 1.1 非目标

v1 不负责：

- HTTP server；
- `/metrics` 路由；
- HTTP response、Content-Type、压缩或内容协商；
- Pushgateway、remote write；
- OpenMetrics、exemplar、summary、native histogram；
- 运行期创建 metric family、label 或 worker shard；
- 动态增删 worker。

HTTP 层以后只需要调用本库的 collect 接口，并把返回的 `IoBufChain` 作为响应体。

## 2. 总体架构

每个 worker 对应一个只允许 owner EventLoop 访问的 `MetricsShard`。业务代码通过
预先绑定的 metric handle 直接修改 shard 中的值。

collect 时不能从其他线程直接读取 shard。Registry 向每个 owner EventLoop 投递
快照任务；快照任务在 owner loop 上复制本地数据。所有快照完成后，collect 发起方
再聚合和编码。

```text
                         MetricsRegistry
                    immutable schema/metadata
                              |
        +---------------------+---------------------+
        |                     |                     |
  EventLoop 0           EventLoop 1           EventLoop 2
  MetricsShard 0        MetricsShard 1        MetricsShard 2
  plain integers        plain integers        plain integers
        |                     |                     |
        +--------- owner-loop snapshot tasks ------+
                              |
                       wait for completion
                              |
                    aggregate fixed series
                              |
                   Prometheus text encoder
                              |
                          IoBufChain
```

Registry 不参与记录路径。它只参与初始化、快照、聚合和编码。

## 3. 并发模型

### 3.1 Owner-loop 不变量

每个 `MetricsShard` 在初始化时绑定唯一的 `fiber::event::EventLoop`：

- 只有该 EventLoop 可以修改或读取 shard 的 metric value；
- metric handle 只能在其所属 EventLoop 上使用；
- shard 的地址和内部 value 地址在运行期保持稳定；
- shard 不会在 EventLoop 运行期间迁移到其他线程。

这些不变量应在构造、绑定和启动边界建立。可以在 debug 构建中验证 owner loop，
但 steady-state 的 `inc()`、`set()`、`observe()` 不应每次做线程查找。

### 3.2 为什么不能跨线程直接读取

worker 使用普通 `uint64_t` 写入，同时由其他线程读取，即使业务上允许采样误差，
在 C++ 内存模型中仍然属于 data race 和未定义行为。Prometheus 的弱一致采样语义
不能替代 C++ 的线程同步。

因此 v1 不允许 exporter 直接遍历其他 worker 的 `MetricsShard`，也不使用
“atomic 版本号 + 非 atomic value”的 seqlock 变体。

### 3.3 快照流程

一次 collect 按以下顺序执行：

1. collect 必须从一个正在运行的 EventLoop 发起；
2. Registry 为本次 collect 准备固定大小的 snapshot slots；
3. 对发起 collect 的本地 shard，可在当前 loop 直接复制；
4. 对其他 shard，通过 `EventLoop::post()` 或 `async::spawn(loop, ...)`
   投递快照任务；
5. 每个任务只读取自己的 shard，并写入自己独占的 snapshot slot；
6. 任务完成后通过明确的同步点通知 collect 发起方；
7. 发起方等待所有 slot 完成后，才读取 snapshot 数据；
8. 聚合和 `IoBufChain` 编码全部在 collect 发起方所在 EventLoop 执行。

同一个 shard 的快照在一次 EventLoop callback 内同步完成，因此 shard 内各字段不会
和本 loop 的业务更新并发执行。不同 shard 的快照时间可能略有差异，这是允许的
Prometheus 弱一致语义。

快照完成通知必须建立 happens-before 关系，不能只依赖普通布尔标志轮询。

snapshot slots、完成计数和已投递 callback 所引用的 collect state 必须由 Registry
或其他稳定对象持有，不能只放在 collect 协程栈上。即使调用方提前销毁等待中的
Task，已经投递的 callback 也不能访问失效内存；collect state 只能在所有 callback
完成后复用或释放。

### 3.4 并发 collect

v1 每个 Registry 最多允许一个 in-flight collect：

- 第一个 collect 获得 collect state；
- 后续并发 collect 返回 `fiber::common::IoErr::Busy`；
- collect state 的同步不进入 metric 记录路径。

这一限制使 snapshot slot、投递项、取消和 Registry 生命周期保持明确。以后若有实际
需求，可以改为每次 collect 独立分配状态来支持并发。

## 4. Registry、Family 与 Shard

### 4.1 MetricsRegistry

`MetricsRegistry` 负责：

- 保存 metric family schema；
- 保存 metric name、HELP、TYPE、单位和 label schema；
- 验证名称、label 和 family 冲突；
- 建立并持有绑定到 EventLoop 的 `MetricsShard`；
- 在初始化结束时冻结所有结构；
- 调度 owner-loop 快照；
- 聚合相同 family/series 的各 shard 值；
- 调用 text encoder 输出结果。

Registry 不保存任意业务对象的 exporter 回调。metric family 和 series 在冻结后使用
稳定的 index/offset 关联到每个 shard，避免回调上下文的生命周期问题。

### 4.2 初始化与冻结

推荐初始化顺序：

1. 创建 Registry；
2. 注册 Counter、Gauge、Histogram family；
3. 为 family 注册所有固定 label series；
4. 注册需要绑定的 worker EventLoop；
5. 调用 `freeze()`，一次性验证 schema 并分配所有 shard storage；
6. 从每个 shard 获取地址稳定的 metric handle，保存到对应 Worker；
7. 启动或进入业务处理。

`freeze()` 后禁止：

- 注册或删除 family；
- 增加 label value/series；
- 修改 Histogram bucket；
- 增加或删除 worker shard；
- 改变 family 的聚合方式。

所有 `std::string`、`std::vector` 等 setup-only 数据都应在 `freeze()` 前完成。运行期
记录只持有直接指向 value 的轻量 handle。

### 4.3 MetricsShard

每个 worker 有一个独立 shard。shard 内只保存 metric value，不重复保存 name、HELP
等元数据。

实现需要避免不同 worker 的热点 value 落在同一 cache line。可以让每个 shard 独立
分配并按 cache line 对齐；同一 worker 内的多个 value 则应尽量紧凑，以提高局部性。

Registry 持有 shard storage，Worker 只保存非 owning handle。Registry 必须晚于所有
业务更新停止后销毁。

## 5. Metric 类型

### 5.1 Counter

Counter 表示只增不减的累计值，例如：

- 请求总数；
- 收发字节或 packet 总数；
- 错误总数；
- accept 总数。

本地 value 使用零初始化的 `uint64_t`。公开操作为：

```cpp
void inc() noexcept;
void add(uint64_t delta) noexcept;
```

不公开可写的 `value` 字段，不提供减法。相同 series 的 Counter 在各 shard 之间使用
求和聚合。

Counter family 名应遵循 `_total` 后缀约定，例如：

```text
http_requests_total
```

v1 假定计数器在单进程生命周期内不会溢出 `uint64_t`。

### 5.2 Gauge

Gauge 表示可增、可减或直接设置的当前状态，例如：

- 当前连接数；
- in-flight 请求数；
- 当前队列长度；
- 当前内存用量。

v1 本地 value 使用零初始化的 `int64_t`。公开操作为：

```cpp
void set(int64_t value) noexcept;
void inc() noexcept;
void add(int64_t delta) noexcept;
void dec() noexcept;
```

Gauge family 必须在注册时声明跨 shard reduction：

- `Sum`：适合每个 worker 持有局部份额的当前连接数；
- `Min`：所有 shard 都有语义完整的观测值时取最小值；
- `Max`：所有 shard 都有语义完整的观测值时取最大值。

不支持 `Last`，因为不同 EventLoop 的快照没有可比较的全局时间顺序。

进程级 Gauge 如果只应记录一次，可由一个指定 shard 更新并使用 `Sum`；其他 shard
保持 0。禁止让所有 worker 重复写入同一个全局值后再求和。

v1 不支持 floating-point Gauge；确有需求时可在后续增加独立类型，不改变整数 Gauge
的记录路径。

### 5.3 Histogram

Histogram 用于请求延迟、响应大小等分布统计。

每个 Histogram family 在注册时定义一组固定的有限上界：

```text
1us, 5us, 10us, 50us, 100us, 500us, 1ms, 10ms, 100ms
```

bucket 必须严格递增且不能重复。Registry 冻结时验证这一不变量。

本地存储：

```cpp
template<size_t N>
struct HistogramValue {
    uint64_t interval_counts[N];
    uint64_t count;
    uint64_t sum;
};
```

`interval_counts` 保存非累计区间计数。`observe(value)`：

1. 找到第一个 `value <= upper_bound[i]` 的 bucket；
2. 只增加该 `interval_counts[i]`；
3. 增加 `count`；
4. 将 value 增加到 `sum`；
5. value 大于最大上界时只更新 `count` 和 `sum`。

这样每次 observe 只更新一个 bucket，而不是更新所有后续累计 bucket。导出时对聚合
后的 `interval_counts` 做前缀和，生成 Prometheus 要求的累计 `_bucket` series。

导出必须额外生成：

```text
<name>_bucket{le="+Inf"} <count>
<name>_sum <sum>
<name>_count <count>
```

`+Inf` bucket 必须等于 `count`。

延迟 Histogram 的热路径使用整数 duration 单位，例如微秒或纳秒。若 family 名为
`http_request_duration_seconds`，有限 bucket 的 `le` 和 `_sum` 在编码时统一转换为
秒，不能把内部微秒整数直接作为 seconds 输出。

相同 Histogram family 的所有 shard 必须共享完全相同的 bucket 和内部单位。

## 6. Label 与 Series

### 6.1 固定 cardinality

v1 不支持在记录时传入任意 label 字符串。所有 label name 和允许的 label value
组合必须在 Registry 冻结前注册。

推荐使用枚举或固定 index 选择 series，例如：

```text
method = GET, POST
status_class = 2xx, 4xx, 5xx
```

记录前由业务初始化代码取得对应 series 的 handle：

```cpp
CounterRef requests_get_2xx;
```

请求路径只对该 handle 执行 `inc()`，不进行字符串构造、hash 或 label lookup。

禁止把 URL、用户 ID、trace ID 等无界数据作为 label value。

### 6.2 注册期验证

Registry 冻结前验证：

- metric name 合法；
- label name 合法且不重复；
- 同一 family 的所有 series 使用相同 label names；
- label set 不重复；
- Histogram 的用户 label 不得使用保留的 `le`；
- family name 不与其他 family 生成的 `_total`、`_bucket`、`_sum`、`_count`
  等 sample name 冲突；
- HELP、label value 的编码长度受配置上限约束。

Registry 应拥有注册字符串的存储，不依赖调用方 `string_view` 在运行期继续有效。

## 7. 聚合语义

快照完成后，按照 `(family, fixed label set)` 聚合：

- Counter：各 shard 求和；
- Gauge：使用 family 注册时指定的 Sum/Min/Max；
- Histogram：每个 interval bucket、count、sum 分别求和。

聚合顺序固定，输出顺序也固定：

1. family 注册顺序；
2. family 内 series 注册顺序；
3. Histogram bucket 上界顺序；
4. `+Inf`、`_sum`、`_count`。

确定性输出便于测试、diff 和问题排查。

一次 collect 不保证所有 worker 在同一时刻快照，也不保证不同 metric family 之间的
全局事务一致性；它只保证没有 data race，且每个 shard 快照在 owner loop 上完成。

## 8. Collect 与输出接口

### 8.1 `IoBufChain` 主接口

推荐主接口的语义为：

```cpp
fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
collect_text(fiber::mem::IoBufNodePool &node_pool,
             CollectOptions options = {}) noexcept;
```

具体命名可在实现时调整，但必须满足：

- 从当前 EventLoop 异步执行 owner-loop 快照；
- `node_pool` 由 collect 发起线程拥有；
- worker snapshot callback 不访问 `node_pool` 或输出 Chain；
- 等待全部 snapshot 后才在发起 EventLoop 上聚合、分配和编码；
- 成功时返回完整且 readable 的 `IoBufChain`；
- 失败时不返回部分 exposition；
- 不隐式标记 HTTP body complete，内容完成语义由上层决定。

`IoBufChain` 是主接口，因为输出长度通常不能事先精确预测，可以按固定 chunk size
增长，并能直接交给以后可能存在的 HTTP 层。

### 8.2 `IoBuf` 辅助接口

可以提供调用方预分配 buffer 的辅助接口：

```cpp
fiber::async::Task<fiber::common::IoResult<size_t>>
collect_text_into(fiber::mem::IoBuf &out,
                  CollectOptions options = {}) noexcept;
```

语义：

- 只使用 `out.writable()` 容量；
- 成功后一次性 commit 实际长度；
- 容量不足时返回错误，`out.readable()` 保持不变；
- 允许覆盖未 commit 的 writable 区域，但不得暴露部分结果。

不建议通过“先生成 Chain，再 flatten”实现该接口。可以先计算长度或在 writable 区域
尝试编码，只有全部成功后 commit。

### 8.3 Text writer

编码器应直接写入 `IoBuf`/`IoBufChain`，避免构造完整 `std::string`：

- 固定文本和注册期字符串按 `string_view` 写入；
- 整数和浮点表示使用 `std::to_chars` 或等价的无异常编码；
- Chain writer 按配置的 chunk size 获取尾部空间；
- `IoBuf::allocate()` 或 `IoBufChain::append()` 失败时返回 `IoErr::NoMem`；
- 超过 `max_output_bytes` 时返回 `IoErr::MessageTooLarge`。

为了保证失败原子性，主接口在本地临时 Chain 中构建，成功后整体 move 给调用方。

## 9. Prometheus Text 0.0.4 编码

v1 明确实现 Prometheus text exposition format 0.0.4，不输出 OpenMetrics 的
`# EOF`，也不处理 HTTP Accept/Content-Type 协商。

每个 family 按以下顺序输出一次 metadata：

```text
# HELP http_requests_total Total HTTP requests
# TYPE http_requests_total counter
http_requests_total 10000
```

要求：

- `# HELP` 和 `# TYPE` 必须出现在该 family 第一个 sample 之前；
- 每个 family 最多输出一次 HELP 和 TYPE；
- 所有行以 `\n` 结束；
- 整个 exposition 也以 `\n` 结束；
- v1 不输出 timestamp；
- HELP 中的反斜线和换行必须转义；
- label value 中的反斜线、双引号和换行必须转义；
- 相同 label set 的 label 顺序固定；
- 无 label 的 sample 不输出空 `{}`。

Histogram 示例：

```text
# HELP http_request_duration_seconds HTTP request duration.
# TYPE http_request_duration_seconds histogram
http_request_duration_seconds_bucket{method="GET",le="0.000001"} 10
http_request_duration_seconds_bucket{method="GET",le="0.000005"} 25
http_request_duration_seconds_bucket{method="GET",le="+Inf"} 30
http_request_duration_seconds_sum{method="GET"} 0.000142
http_request_duration_seconds_count{method="GET"} 30
```

Histogram 已有业务 label 时，encoder 负责追加 `le`，不允许 Registry 中预注册同名
label。

格式参考：

- <https://prometheus.io/docs/instrumenting/exposition_formats/>
- <https://prometheus.io/docs/concepts/metric_types/>

## 10. 错误处理

项目不使用 C++ exception。所有公开 collect/format 接口为 `noexcept`，通过
`fiber::common::IoResult` 返回错误。

建议错误语义：

- `IoErr::Busy`：已有 collect 正在执行；
- `IoErr::Invalid`：Registry 未冻结、输出对象不满足前置条件或 schema 非法；
- `IoErr::Canceled`：Registry 已进入停止收集状态；
- `IoErr::NoMem`：snapshot/output 分配失败；
- `IoErr::MessageTooLarge`：超过配置的最大输出大小。

注册期若需要返回详细错误，可定义 setup-only 的配置错误结构；不要把动态错误对象
引入记录路径。

## 11. 生命周期与停止顺序

正常生命周期：

```text
register families and fixed series
        |
register worker EventLoops
        |
freeze Registry and allocate shards
        |
bind stable handles to Workers
        |
start/update metrics
        |
collect snapshots while all loops are running
        |
stop accepting new collect calls
        |
wait for in-flight collect to finish
        |
stop and join worker EventLoops
        |
destroy Workers/handles
        |
destroy Registry and shards
```

关键约束：

- 只允许向正在运行且仍会处理 notify queue 的 EventLoop 发起快照；
- EventLoop 停止前必须拒绝新 collect，并等待已有 collect 完成；
- Registry/shard/collect state 必须存活到所有已投递快照任务结束；
- collect Task 被提前销毁时，Registry 仍负责收尾已经投递的快照任务并最终释放
  in-flight 状态；
- metric handle 不拥有 storage，不能比 Registry/shard 活得更久。

v1 不要求在 worker loop 已经停止后抢救一个进行中的 collect；正确的 shutdown 顺序
是公共契约的一部分。

## 12. 目录与构建

推荐目录：

```text
apps/prometheus/
    CMakeLists.txt
    README.md
    include/fiber/prometheus/
        Counter.h
        Gauge.h
        Histogram.h
        MetricFamily.h
        MetricsRegistry.h
        MetricsShard.h
    src/
        MetricsRegistry.cpp
        SnapshotCollector.cpp
        TextEncoder.cpp
        TextWriter.cpp
    tests/
        MetricValueTest.cpp
        HistogramTest.cpp
        TextEncoderTest.cpp
        MetricsRegistryTest.cpp
        MultiLoopSnapshotTest.cpp
        MetricsLifecycleTest.cpp
```

CMake target：

```text
fiber_prometheus
fiber::prometheus
```

`fiber_prometheus` 是依赖 `fiber_lib` 的可选上层库，不包含 `main()`。它只在
`FIBER_BUILD_APPS=ON` 时构建。

这一层次意味着 `src/` 中的 `fiber_lib` 代码不能反向依赖 `fiber::prometheus`。
如果以后需要直接在 HTTP、QUIC、EventLoop 等核心实现内部埋点，应另行设计位于
`fiber_lib` 内的基础记录层，避免循环依赖。

`apps/README.md` 当前只描述 runnable app；引入本库时应同步说明 `apps/` 也允许类似
`apps/nacos` 的可选静态库模块。

## 13. 测试与性能验收

### 13.1 Metric 单元测试

- Counter 初始值、`inc()`、`add()`；
- Gauge 的 set/inc/add/dec；
- Histogram 命中第一个、精确命中边界、超过最大边界；
- Histogram 非累计内部 bucket、累计导出、`+Inf == count`；
- Histogram sum 和 duration 单位转换；
- Gauge Sum/Min/Max reduction。

### 13.2 Registry 与格式测试

- 非法或重复 metric/label name；
- 重复 label set；
- Histogram `le` 冲突；
- family 生成 sample name 冲突；
- HELP 和 label 转义；
- 无 label 和多 label 输出；
- family/series/bucket 的确定性顺序；
- 输出末尾换行；
- 空 Registry；
- IoBuf 容量不足时不 commit 部分结果；
- Chain 分配失败或输出上限错误不暴露部分结果。

可以增加 golden text，并用 `promtool check metrics` 做可选互操作验证，但测试不能只
依赖外部工具。

### 13.3 多 EventLoop 测试

- 每个 worker 只更新自己的 shard；
- collect 在另一个 EventLoop 发起；
- owner-loop 快照正确聚合；
- collect 与持续更新并行时无死锁、无 data race；
- 同时发起第二次 collect 返回 Busy；
- 停止收集、等待 in-flight collect、停止 EventLoop 的顺序；
- 尽可能在 ThreadSanitizer 构建下执行快照并发测试。

### 13.4 性能验收

至少增加 microbenchmark 或可重复的基准程序，验证：

- Counter 记录路径没有动态分配和锁；
- Histogram observe 每次只更新一个有限 bucket；
- 固定 label handle 不进行 hash/string lookup；
- 多 worker shard 没有明显 false sharing；
- collect 的分配和同步不会出现在请求记录调用栈上。

## 14. 最终原则

```text
Fast path:
    cached metric handle
        -> owner-loop plain integer update
        -> return

Slow path:
    asynchronous owner-loop snapshots
        -> wait with synchronization
        -> aggregate fixed series
        -> encode Prometheus text 0.0.4
        -> return IoBufChain
```

该设计以 owner-loop ownership 保证记录路径性能，以投递和完成同步保证 C++ 并发
正确性。Prometheus 的弱一致语义只体现在不同 worker 的快照时间不同，不用于容忍
未同步的跨线程内存访问。
