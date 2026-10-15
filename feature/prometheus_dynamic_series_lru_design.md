# Prometheus 动态标签与 LRU 容量管理设计

日期：2026-10-15。

状态：已实现。本文保留设计约束及验收标准；实际测试、并发检查、性能测量与验证边界见
[实现与验证记录](prometheus_dynamic_series_lru_validation.md)。

本文扩展 [Prometheus Metrics 初版设计](prometheus_cpp_metrics_design.md)，以当前
`apps/prometheus` 源码为实现基线。静态指标继续使用 owner-loop 分片；动态 LRU 指标采用独立存储。

## 1. 已确认需求

需要在请求执行时，根据当时才知道的用户名、upstream IP 等 label 值创建指标，例如：

```text
user_requests_total{user="alice"} 12
user_requests_total{user="bob"} 7
upstream_requests_total{upstream="backend",ip="192.0.2.10"} 35
```

已确认的约束：

- metric family、指标类型、HELP 和 label 名称在启动时确定，仅 label 值组合动态增加。
- 每个 family 最多保留 N 个不同的完整标签组合，N 为配置值且必须大于 0。
- 容量属于同一个 Registry 中的该 family，由所有 worker 共享；不是每 worker 各有 N 个名额。
- 满容量后，新标签替换最久未被业务更新的标签，采用严格 LRU。
- 淘汰删除整个标签组合的累计状态；相同标签再次出现时从零重新累计。
- 接受淘汰造成的累计中断，不要求恢复已淘汰标签的历史值。

不同 Registry 的 family 是独立对象。本方案不增加跨 Registry 或跨进程的全局容量管理。

这里的一条 series 指一个 family 下的一组 label 值。Histogram 的一组标签占一个名额，
导出时仍展开为有限 bucket、`+Inf` bucket、`_sum` 和 `_count` 多条 Prometheus 时间序列。

## 2. 实现范围

首版动态 LRU 模式支持 Counter 和 Histogram，覆盖请求数、字节数、延迟和响应大小等累计指标。

首版不支持动态 LRU Gauge。现有 Gauge 的 Sum/Min/Max 是跨 shard 聚合语义；共享 Gauge 的
`set()` 不能直接替代这些语义。增减式 Gauge 也不能在淘汰后正确重建：例如 in-flight 值为 10 时
被淘汰，随后一个请求结束并执行 `dec()`，重建后的值会变成 -1。

未来如果扩展动态 Gauge，应独立定义完整值 `set()`、worker 观测值和淘汰的关系。本次不增加
动态 Gauge 接口，也不借用现有 `GaugeRef` 提供语义不完整的支持。

本次还不包含：

- 动态注册 family、label 名称、Histogram bucket 或 worker；
- TTL、显式删除、跨 family 联动淘汰、标签白名单或 Top-K；
- 淘汰数据归档、精确历史计费、累计值持久化；
- HTTP `/metrics` 路由、OpenMetrics、exemplar 或 native histogram；
- 无锁回收、近似 LRU、动态 series 裸指针缓存。

同一个 family 使用一种存储模式：`Static` 或 `Lru`。不在一个 family 中混用不可淘汰的固定
series 和可淘汰的动态 series，避免容量、重复标签和导出语义出现歧义。

## 3. 当前实现与改造依据

当前代码的关键路径：

| 位置 | 当前行为 | 扩展要求 |
|---|---|---|
| `MetricsRegistry.cpp::register_series()` | 冻结后拒绝新增标签组合 | 增加独立的动态记录路径 |
| `MetricsRegistry.cpp::freeze()` | 计算固定偏移并分配每 shard 的连续值区 | 静态值区保持固定，另行初始化动态 family |
| `Counter.h` / `Histogram.h` | Ref 保存值或桶数组地址 | 不将这些 Ref 暴露给可淘汰 series |
| `PrometheusInternal.h::RegistryData` | 持有静态 schema、shard 快照及 collect 状态 | 增加动态 store 和一代快照拥有的数据 |
| `MetricsRegistry.cpp::prepare_collect()` | 并发 collect 共享一代稳定快照 | 将动态快照纳入同一代的就绪与生命周期管理 |
| `TextEncoder.cpp` | 根据固定 series 偏移聚合 shard 值 | 增加已聚合动态样本的编码入口 |

不能通过删除 `register_series()` 的冻结检查实现本需求。静态值区、Ref 和 encoder 都依赖
固定布局；新增和删除会改变地址、元数据生命周期及聚合方式。

当前 `collect_text(CollectOptions)` 不需要显式传入 node pool。并发 collect 已支持共享快照，
不应退回初版设计文档中“第二个调用返回 Busy”的行为。

## 4. 总体架构

```text
                         MetricsRegistry
                     immutable family schema
                         /             \
                 Static family       Lru family
                       |                  |
               owner-loop shards     family-local mutex
               fixed value storage   hash index + LRU list
                       |              shared aggregate values
               owner-loop copies           |
                       |              one locked full copy
                       \                  /
                        stable generation
                     owned labels and values
                              |
                     encode on collector loop
                              |
                      IoBuf / IoBufChain
```

动态 Counter 每个标签组合只有一个 `uint64_t` 累计值；动态 Histogram 每个标签组合只有
一组 interval bucket、count 和 sum。不同 worker 在 family 锁保护下更新同一份累计状态。

既然严格全局 LRU 已需要协调每次访问，就不再为动态 Counter/Histogram 保留多份 shard 值。
这能减少内存，保证一次淘汰删除完整累计状态，也避免采集时重复合并动态值。

每个动态 family 一把互斥锁。不同 family 不共享记录锁，静态 Ref 更新不进入这些锁。
单个热门 family 仍会发生跨 worker 锁竞争；本方案不声称动态路径具有静态路径相同的吞吐量。

## 5. 配置与公共接口

### 5.1 注册配置

建议在公共类型中增加以下配置概念，最终命名可按实现保持一致：

| 配置 | 语义 |
|---|---|
| `SeriesMode::Static` | 默认模式，沿用现有固定 series 注册 |
| `SeriesMode::Lru` | 运行时动态记录并按 family 淘汰 |
| `SeriesOptions::max_series` | LRU family 的活跃标签组合硬上限 |
| `SeriesOptions::max_storage_bytes` | LRU store 的可选内存预算，0 表示只使用由 N 和标签长度等推导的上界 |
| `RegistryOptions::max_dynamic_snapshot_bytes` | 一代动态快照的内存预算，建议默认 64 MiB，可在启动时调整 |

在 `register_counter()`、`register_histogram()` 的末尾增加默认的 `SeriesOptions`，使现有
调用保持源码兼容。静态模式要求 `max_series` 和动态 store 预算为 0，避免无效配置被静默忽略。
现有 Gauge 注册接口只提供静态模式。

注册时校验 N、枚举值、大小计算及预算；`freeze()` 后不能修改模式、容量、预算或 schema。
单个 label value 继续受现有 `max_label_value_bytes` 的转义后长度限制。

### 5.2 绑定稳定的 family 记录对象

建议增加公共轻量对象 `DynamicCounter`、`DynamicHistogram`，以及 Registry 的类型化绑定接口：

```text
registry.dynamic_counter(FamilyId)   -> IoResult<DynamicCounter>
registry.dynamic_histogram(FamilyId) -> IoResult<DynamicHistogram>

DynamicCounter.inc(label_values)        -> IoResult<void>
DynamicCounter.add(label_values, delta) -> IoResult<void>
DynamicHistogram.observe(label_values, value) -> IoResult<void>
```

这些是拟议 API，不是已有实现。

绑定发生在 `freeze()` 成功之后，验证 Registry 身份、family 类型和 LRU 模式。记录对象内部只
引用地址稳定的 family store，不引用某个 series。通过私有构造和类型化绑定建立有效性不变量，
不提供供业务随意构造的空对象；对象可以复制并由多个 worker 使用。

动态记录对象不绑定某个 EventLoop，也不依赖 `EventLoop::current()`；同步更新由 family 锁
保证线程安全。Registry 初始化完成后必须通过正常的线程启动或同步机制发布给使用线程。

所有运行期记录接口为 `[[nodiscard]]`、`noexcept`，返回 `IoResult`，不抛异常。调用方必须
处理失败，不能假设动态记录与静态 Ref 一样永远成功。

### 5.3 标签和初始化

- label 值按 family 注册的名称顺序传入，使用 `span<const string_view>`。
- 每次调用检查数量及长度。标签数不匹配、超长或类型绑定错误返回 `Invalid`。
- family 的同一组标签以完整字符串内容判断相等，不依赖调用方地址。
- 首次创建时复制标签内容；调用结束后原始请求字符串可以销毁。
- label 规范化由业务负责，例如统一 IP 表示。库不自动改写大小写、IPv6 文本或用户名。
- `add(labels, 0)` 是合法更新：可创建零值条目并刷新 LRU，可用于启动后的预热。
- 无 label 的动态 family 最多只有一个空标签组合；允许使用，但通常静态指标更合适。
- 动态 family 不调用 `register_series()`，也不产生动态 `SeriesId`；误用返回 `Invalid`。
- 未被成功记录过的标签组合不输出，空动态 family 不输出 HELP/TYPE。

成功的 `inc()`/`add()`/`observe()` 同时完成查找、必要的创建或淘汰、数值更新和 LRU 刷新。
不暴露“取得 series 后稍后更新”的窗口，不返回现有 `CounterRef` 或 `HistogramRef`。

## 6. LRU 与计数语义

### 6.1 顺序定义

LRU 表示该 family 中最后一次成功业务记录的先后顺序。并发调用以持有 family 锁并提交更新的
顺序确定先后，不要求反映请求到达网卡的时间。无需获取 wall clock 或时间戳。

- 成功命中：更新数值，将条目移动到 MRU 头部；已经在头部时无需改链。
- 成功新增：创建零值条目，应用当前操作，再置于头部。
- 满容量新增：淘汰 LRU 尾部，新增并应用当前操作。
- 参数错误或资源失败：不改变已有值、LRU 顺序和活跃条目数。
- 采集、编码及诊断读取：不刷新 LRU。

每次成功记录提交后 `active_series <= N`。淘汰只发生在新增标签且活跃数已为 N 时；命中已有
标签不会淘汰其他条目。资源预算不足不触发额外批量淘汰，而是返回错误。

### 6.2 示例

N=2，初始为空：

| 操作 | 保存的 Counter | MRU 到 LRU |
|---|---|---|
| alice +1 | alice=1 | alice |
| bob +1 | alice=1，bob=1 | bob，alice |
| alice +1 | alice=2，bob=1 | alice，bob |
| carol +1 | alice=2，carol=1 | carol，alice |
| bob +1 | carol=1，bob=1 | bob，carol |

一个标签组合在多个 worker 中被更新时，只有一个计数和一个 LRU 节点。淘汰不会只删除某个
worker 的贡献。同一标签在不同 family 中相互独立，不保证同时创建或同时淘汰。

### 6.3 Histogram

沿用现有固定 bucket 和整数单位转换。一次 observe 只更新命中的有限区间桶（如果存在）、
count 和 sum；超出最大上界时只更新 count 和 sum。整个过程与 LRU 修改位于同一临界区。

淘汰同时删除全部区间桶、count 和 sum，重建时全部清零。快照不得混合不同驻留周期的字段。
导出时仍执行累计桶前缀和，并保证 `+Inf == count`。

Counter、Histogram 的整数溢出契约沿用当前实现，不在本次引入饱和计数或大整数。

### 6.4 统计边界

淘汰后重建意味着计数重置。两次 scrape 之间出现又消失的标签可能从未被 Prometheus 采到；
重置也未必能从离散采样中识别。不能把 `rate()`/`increase()` 描述为能恢复所有丢失记录。

业务若需要不受明细淘汰影响的进程总量，应另行记录固定标签或无标签的静态 Counter。
动态明细相加不保证等于该总量。本方案不自动把淘汰值转入 `other` 标签。

LRU 约束的是客户端活跃集合和一次快照的标签数。Prometheus 已采集的历史序列不会因客户端
淘汰而立即删除；长期持续变化的用户名仍可能产生较大的服务端历史基数。

## 7. 存储结构与资源上界

### 7.1 Family store

每个 LRU store 包含：

- 指向冻结 family schema 的稳定引用；
- family 互斥锁；
- 按完整标签元组查找的 hash 索引；
- 可复用的条目槽位、空闲槽位管理和侵入式 LRU 链；
- 拥有标签内容的可回收存储；
- Counter 值或 Histogram interval/count/sum 存储；
- 有限的统计字段和内存用量计数。

Hash 按每个 label 的长度和内容组合，比较时再逐个比较完整值。不得使用无分隔拼接、单独 hash
值或调用方 `string_view` 地址作为唯一身份，必须正确处理空串、分隔符和 hash 碰撞。

推荐 `freeze()` 时建立能容纳 N 个条目的固定容量索引及槽位管理结构，使稳定命中和替换不需要
扩容索引。Hash 删除策略必须能重复利用空间，不能让 tombstone 随淘汰次数无界累积。

标签内容按实际长度拥有并可回收；如果使用 size class 或 slab，空闲缓存也必须有明确上限。
不能使用只增不减的 arena。运行期不得依靠可能抛出 `bad_alloc` 的容器扩容来兑现 `NoMem` 契约。
可使用项目可失败分配接口或 `nothrow` 分配，所有大小乘加先检查溢出。

### 7.2 内存估算

定义：

- N：family 容量；
- K：label 数量；
- L：单个 label value 的配置长度上限；
- B：有限 Histogram bucket 数；Counter 的值区只有一个 word；
- E：条目、索引和 LRU 的固定元数据开销。

动态值区大致为 Counter 的 `8 * N`，或 Histogram 的 `8 * N * (B + 2)`。
标签上界为 `N * K * L` 再加每个 label 的长度/偏移元数据，固定元数据约为 `N * E`。
还须计入 hash 索引容量取整、对齐、
分配器块头、受限空闲块和创建暂存区，不能只按数值字段估算内存。

一次更新为保证失败原子性，可以临时持有一个尚未发布的候选标签记录。它不属于活跃集合，
但必须计入 store 预算；不能用这个暂存例外长期保留被淘汰对象。

当 `max_storage_bytes` 非零时，它约束 store 拥有的索引、槽位、标签、值、受限空闲块及创建
暂存区。初始固定结构已超出预算时 `freeze()` 失败；运行时超过预算返回 `MessageTooLarge`。
配置为 0 时仍受 N、K、L、B 推导出的有限上界约束，不表示允许历史标签无限累积。

快照使用独立的 Registry 预算，累计计算所有动态 family 的已分配缓冲容量、标签副本及必要的
排序临时存储，而不只计算当前使用字节数；缓冲增长时的暂时双份内存也计入预算。
输出使用现有每次调用的 `max_output_bytes`。
并发调用方各自持有输出，不能据此承诺任意数量并发 collect 的整个进程内存为固定值。

## 8. 更新事务与错误处理

一次记录按以下顺序执行：

1. 在锁外验证标签数量、长度，并计算查找 hash；Histogram 可在锁外定位 bucket。
2. 获取 family 锁，以完整标签内容查找。
3. 命中则更新值和 LRU，无分配地完成操作。
4. 未命中则准备候选标签和值存储，检查容量计算、预算及分配结果。
5. 在旧条目仍完整时完成所有可能失败的准备工作。
6. 提交不再失败的索引/LRU 修改：必要时移除尾部条目，发布新条目并记录本次值。
7. 回收旧存储，释放锁并返回成功。

首版允许首次创建的分配位于锁内，换取简单的事务边界；它的延迟必须在频繁换标签基准中单独
测量。不能宣称动态创建无分配，也不能把冷路径分配成本隐含到命中性能数据中。

对于 Counter/Histogram，只有成功记录才改变数值。库不返回成功的 no-op 句柄，不静默丢弃
本次更新；错误由业务按埋点失败策略处理。

| 情况 | 结果 |
|---|---|
| 错误 Registry/family/type/mode，未冻结，非法配置或标签 | `IoErr::Invalid` |
| 分配器无法分配所需资源 | `IoErr::NoMem` |
| 超过显式 store 或动态快照预算、输出容量限制 | `IoErr::MessageTooLarge` |
| 活跃数达到 N，但新条目能正常替换 | 成功，淘汰一个条目 |
| `stop_collecting()` 后发起 collect | `IoErr::Canceled`，沿用当前契约 |
| 多个 collect 重叠 | 共享同一代快照，不返回 Busy |

系统分配失败时不要无限重试或额外淘汰多个用户。冻结失败应回滚本次已分配的运行期结构，
Registry 保持未冻结状态，不能留下部分 family 可用的状态。

## 9. 快照、并发 collect 与编码

### 9.1 每 family 一次完整复制

动态 family 快照必须在一次持锁期间捕获完整的条目集合、标签内容和值。锁外只处理快照。
不能分批解锁后遍历实时表，否则采集期间的替换可能造成重复、遗漏或输出超过 N 个组合。

快照拥有标签字节，不引用可被淘汰或复用的 live store 内存。复制后即使业务立即淘汰该条目，
正在进行的采集仍可输出其快照值；下一代采集反映新的集合。

为了缩短临界区，快照缓冲按已知 N 和值宽度预留条目容量，标签字节缓冲在代际之间复用并受
预算限制。必要的容量探测和锁外增长可以单独进行，最终复制前仍必须在锁内重新验证容量；
不能通过无限重试换取无界延迟。首版可在最终复制时进行受限增长，并在基准中测量其持锁时间。

排序、转义、数字格式化、IoBuf 分配及输出编码全部在释放 family 锁后完成。多个 family
逐个复制，不同时持有多把 family 锁；不承诺不同 family 或静态 shard 的全局同一时刻快照。

### 9.2 一代共享快照

复用当前 Registry 管理的共享代际，扩展为以下阶段：

```text
Idle -> Capturing(static shards + dynamic families)
     -> Ready(success or shared snapshot error)
     -> all callbacks and attached collectors released
     -> Idle
```

第一个调用创建该代，并负责动态 family 快照；其他调用只加入等待，不能重复复制动态数据。
一代的完成计数应覆盖所有静态 shard 回调以及动态复制批次，例如使用 `shard_count + 1`
个完成项。即使没有静态 shard，也不能提前宣布快照就绪。

必须在发布代际时初始化完成计数和错误槽，然后才发布/执行捕获任务。动态完成项与每个静态
回调完成项都恰好完成一次。只有全部完成后，附着调用方才能读取这一代的数据和错误。

动态批次在 leader 的 collect EventLoop 上同步执行；在首次挂起前完成复制及既定回调投递。
批次完成后等待静态回调。不能因为动态复制失败而直接退出，使已记入 pending 的完成项悬空；
必须仍履行全部投递/完成责任，或者在尚未投递时明确撤销对应计数。

所有跨线程发布继续使用明确的锁及 WaitGroup 完成同步，保证 happens-before。代际内缓冲、
完成项和错误由 Registry 持有，不放在可能被取消的调用方临时栈中。

### 9.3 错误、取消和复用

动态快照任一 family 复制失败，该代整体失败；所有附着调用方得到相同的快照错误，不输出
缺失某个 family 的“成功”结果。仍需完成所有已投递静态回调，才能释放这一代。

输出错误保持调用方独立。例如一个调用的 `max_output_bytes` 太小，只影响它的结果，
不改变共享快照，也不影响另一调用用更大上限成功编码。

leader 或 follower 在等待阶段被销毁，只减少该调用的附着计数。直到 pending callbacks 为 0
且 attached collectors 为 0，才允许清空或复用该代的静态/动态快照和错误状态。

锁纪律：`collect_mutex` 仅保护代际控制，不在持有它时获取 family 锁、复制数据或分配大缓冲。
复制期间只持有当前 family 锁，释放后再进行完成通知；不得跨 `co_await` 持有任何这些锁。

### 9.4 编码

family 按注册顺序输出，静态 family 保持现有 series 顺序。动态快照在发布 Ready 前，锁外
按原始 label 值元组的确定性字节字典序排序。逐标签比较 unsigned byte；公共前缀相同时较短
字符串排在前面。不能按 hash/LRU 顺序输出。

编码器区分两种输入：静态 series 从 shard 快照聚合；动态 series 直接使用已经合并的快照值。
共享转义、单位转换、metadata 和 Histogram 输出逻辑，避免把动态值再按 shard 数量累加。

仍使用 text exposition 0.0.4，每 family 最多一次 HELP/TYPE；不添加内部槽位编号或 generation
作为 Prometheus label。失败不返回部分 Chain，`collect_text_into()` 失败不 commit 部分内容。

## 10. 生命周期

1. 注册静态/动态 family、静态 series 和需要的 worker shard。
2. `freeze()` 验证并建立静态值区、动态 store 和快照控制结构。
3. 绑定静态 Ref 与动态 family 记录对象，再向业务线程发布。
4. 请求线程执行记录，collect 在运行中的 EventLoop 上生成共享快照。
5. `stop_collecting()` 拒绝新的 collect；在运行中的 loop 上等待 `wait_for_idle()`。
6. 停止并等待所有可能更新指标的业务线程/worker，销毁记录对象。
7. 销毁 Registry、store 和快照存储。

`stop_collecting()` 只停止采集，不隐式停止业务记录；`wait_for_idle()` 也不表示所有业务写入
已经结束。Registry 销毁前必须由宿主保证没有动态记录调用在执行或将要执行。

记录对象不拥有 Registry，其生命周期不能超过 Registry。无需为每条 series 延迟回收或保留
引用计数，因为外部不持有 series 指针，快照也拥有独立数据。

## 11. 内部诊断

动态 family store 维护有限的诊断信息：活跃条目数、配置容量、当前存储用量、成功淘汰次数、
创建失败次数。诊断更新与相关操作使用同一把 family 锁，不能通过递归调用动态记录 API 实现。

建议提供只读诊断快照接口，由业务按需导出固定 family 标签的静态监控指标；不在首版自动注册
额外的公共 metric 名称，以免与业务命名冲突。错误原因使用有限枚举，不把失败的用户名/IP
作为新的诊断标签。诊断读取不刷新 LRU。

## 12. 实施文件与步骤

建议文件划分：

| 文件 | 工作 |
|---|---|
| `include/fiber/prometheus/MetricFamily.h` | 模式、容量选项及必要的 Registry 预算 |
| `include/fiber/prometheus/MetricsRegistry.h` | 注册配置和类型化动态绑定接口 |
| `include/fiber/prometheus/DynamicCounter.h` | 稳定 family 记录对象 |
| `include/fiber/prometheus/DynamicHistogram.h` | 动态 Histogram 记录对象 |
| `src/DynamicSeriesStore.h/.cpp` | 私有索引、LRU、事务更新、内存回收、诊断 |
| `src/PrometheusInternal.h` | store 所有权和动态快照结构 |
| `src/MetricsRegistry.cpp` | 冻结、绑定、共享快照代际及清理 |
| `src/TextEncoder.cpp` | 动态快照编码，复用现有格式逻辑 |
| `tests/DynamicSeriesTest.cpp` | 动态标签及 LRU 行为 |
| `tests/DynamicSnapshotTest.cpp` | 淘汰与快照的并发、取消和失败 |
| `bench/RecordBenchmark.cpp` 或独立动态基准文件 | 查找、争用、换标签及采集干扰 |
| `apps/prometheus/CMakeLists.txt` | 模块源文件、测试及可选基准注册 |
| `apps/prometheus/README.md` | 新接口、成本、重置语义和使用示例 |

公共头文件只声明公共对象和 opaque 私有结构，不包含 `src/` 私有头。模块测试仍注册到当前
`fiber_prometheus_tests`，不误加到核心 `fiber_tests`。

推荐实施顺序：先完成 store 及单线程事务测试，再接入类型化记录接口、多线程更新，然后接入
共享快照和 encoder，最后完成失败注入、sanitizer 与性能验证。整个过程中保留静态路径回归。

## 13. 正确性验收

### 13.1 配置、标签与容量

- 默认模式兼容现有调用；N=0、非法模式、非法预算及大小溢出被拒绝。
- 跨 Registry ID、错误类型、错误模式、冻结前绑定/冻结后注册被拒绝。
- N=1 和 N=2 的完整替换顺序；命中刷新、采集不刷新、失败不刷新。
- 不同 family 独立容量；多个 worker 共用同一标签只占一个名额。
- 空标签值、多标签元组、不同分割的相同拼接文本以及强制 hash 碰撞正确区分。
- 标签原始字符串在调用后销毁/修改，不影响记录和导出。
- 转义后长度边界、引号、反斜线、换行及空 label schema。
- 重复淘汰再创建从零应用本次操作，`add(0)` 的初始化语义。

### 13.2 值与失败原子性

- 多线程对固定标签累计，在无淘汰时得到精确总数，无重复条目。
- Histogram 各边界、超最大桶、单位转换、重建后全部字段清零。
- 创建分配失败或预算不足时旧值、标签集合、LRU 顺序均保持。
- freeze 失败回滚；索引和标签分配阶段逐点失败注入。
- 持续替换后 tombstone、空闲块和历史标签内存不无界增长。
- N 个长标签与短标签反复替换，验证暂存空间和实际预算记账。

### 13.3 快照与生命周期

- 静态和动态 family 混合导出，动态值不被重复聚合。
- 仅动态 family、零静态 shard 的采集完成计数正确。
- 采集期间连续淘汰，单代最多 N 个标签组合，标签和值始终属于同一条目。
- 复制后 live 条目被释放/复用，快照仍可安全编码；下一代反映新状态。
- 并发调用共享同一代动态值，输出上限错误仍相互独立。
- 动态快照失败时所有调用返回相同快照错误，并等待所有已投递回调收尾。
- 销毁 leader/follower、停止采集及等待 idle 不泄漏、死锁或提前复用快照。
- HELP/TYPE、转义、字典序、末尾换行及完整输出失败原子性。

对创建/淘汰/采集并发运行 TSan；对标签回收、槽位复用和取消路径运行 ASan/UBSan。
可用 `promtool check metrics` 验证格式，但它不能代替生命周期或并发验证。

## 14. 性能验收

分别测量以下路径，不能仅用静态 Ref microbenchmark 代表新功能性能：

| 场景 | 关注数据 |
|---|---|
| 静态 Ref 更新 | 对照基线，确认未引入动态锁和查找 |
| 动态单线程稳定命中 | ops/s、延迟、每次更新分配次数为 0 |
| 多 worker 更新同一 family | 1/2/4/8 等线程下的扩展性与 p95/p99 |
| 多 worker 更新不同 family | 验证锁按 family 隔离 |
| 满容量持续新标签 | 创建/淘汰吞吐量、分配次数、内存稳定性 |
| 同时 scrape | 更新尾延迟、快照复制持锁时间、采集时间 |
| 最大标签长度和 Histogram 桶数 | 内存预算及复制时间随数据规模的变化 |

记录硬件、构建模式、线程数、N、标签长度、bucket 数和 scrape 频率。使用 Release 构建，
报告 steady hit、首次创建及持续 churn，不能把三者混成一个平均值。

严格 family LRU 会串行化同一 family 的更新；std::mutex 竞争可能阻塞 EventLoop 线程，
完整快照持锁时间也会随规模增加。是否满足网关请求延迟必须以实测判断，目前没有性能结论。

若性能不达标，先检查索引、分配和复制的临界区成本。近似 LRU、分段配额或带代际的缓存属于
后续设计变化，不能在实现时静默降低“全 family N 条、严格 LRU”的已确认语义。

完成实现后运行模块回归、相关完整 CTest、仓库格式化和 clang-tidy；sanitizer 与性能结果分别
记录，不能把普通单元测试通过写成已完成并发或性能验收。本文编写阶段不运行这些实现验收。

## 15. 参考资料

- [Prometheus client library guidelines: labels and child lifecycle](https://prometheus.io/docs/instrumenting/writing_clientlibs/#labels)
- [Prometheus client library guidelines: performance](https://prometheus.io/docs/instrumenting/writing_clientlibs/#performance-considerations)
- [Prometheus query functions: rate](https://prometheus.io/docs/prometheus/latest/querying/functions/#rate)
- [Prometheus querying basics: staleness](https://prometheus.io/docs/prometheus/latest/querying/basics/#staleness)
- [Prometheus instrumentation: labels and cardinality](https://prometheus.io/docs/practices/instrumentation/#do-not-overuse-labels)

本方案沿用 label child 和 Counter reset 的基本模型，但动态 family 的共享锁、严格容量、LRU
顺序以及无 series Ref 接口是本项目的设计选择，不是 Prometheus 协议强制要求。
