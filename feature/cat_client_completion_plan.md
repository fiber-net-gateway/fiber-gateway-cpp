# CAT Client 完整实现计划

> 状态：本机可验证的核心功能已实现；真实 CAT Server 互操作因当前环境不可用而跳过。可选长事务分段、手工
> Heartbeat、Latest/Tag Metric、Linux resource provider 和传统全局初始化仍按需实施。
>
> 创建日期：2026-07-22。
>
> 行为与 wire format 参考：仓库缓存的官方 Dianping CAT C/C++ 客户端
> `e815e74d4c2dd74edac831241f1253fcc7d25381`。主要参考 `temp/cat/lib/c/src/ccat/`，不把薄封装的
> C++ facade 作为唯一行为依据。

## 1. 背景与当前基线

`apps/cat` 的实现基线已经扩展为：

- `MessageTrace`、`Transaction` 和 `Event` 是 move-only 单指针句柄，内部消息树使用 trace-owned
  `BufPool`，Transaction 子节点保存在每块 16 个指针的链式 chunk 中。
- 消息树显式表达父子关系，不依赖 OS thread-local transaction stack，能够支持同一 EventLoop 上多个协程交错。
- 最后一个未完成消息完成时同步冻结并编码消息树，随后立即释放 `MessageTraceData` 和 trace pool。
- 已实现官方 NT1 的 Transaction/Event 字段顺序、长度前缀、时间、status、data 和深度优先嵌套编码。
- 已实现 CAT Router HTTP 拉取、`routers/sample/block` 解析、collector 轮换、连接失败退避和 raw TCP 发送。
- sender 数据面不使用协程和 CAT 私有 MPSC 队列；跨 loop 的 `OutboundFrame` 直接进入 sender EventLoop
  Notify 队列，同 loop 提交直接进入 owner-loop FIFO，并由 `DeferEntry` 合并 `try_writev()`。
- outstanding message/byte budget 覆盖 Notify 队列、owner-loop FIFO、可写等待和部分发送帧；关闭时停止准入、
  跨越完整 Notify phase、限时排空并释放全部剩余帧。
- 已实现自动 Message ID、owning propagation context、采样后聚合、Metric `M`、Heartbeat `H`、PT1、截断标记、
  problem/system 有界优先级，以及 producer-loop shard 显式 detach。

本文档同时保留尚未实施的可选项和必须在真实 CAT Server 环境完成的互操作项，避免把本机 fake peer 结果等同于
服务端报表验证。

## 2. 完成标准

### 2.1 生产完整

满足以下条件后，可以把 `apps/cat` 视为可生产使用的完整 CAT 客户端：

1. 自动生成 CAT message ID，并支持显式的跨服务 root/parent/child context 传播。
2. router sampling 对错误树、普通树和聚合树具有正确且可测试的语义。
3. 被采样掉的 Transaction/Event 进入有界聚合器，不直接丢失统计信息。
4. Count/Duration Metric 能周期聚合、编码为 NT1 `M` 记录并提交给现有 sender。
5. 客户端能周期发送 NT1 `H` Heartbeat、版本和必要的自监控数据。
6. Router、collector、重连、动态 block/sample、部分写和关闭路径具有确定性的端到端测试。
7. 使用真实 CAT Server 完成 Transaction/Event/Metric/Heartbeat 互操作验证。

### 2.2 官方兼容扩展

以下能力用于扩大与官方 C/C++ 客户端的功能兼容面，但不是 NT1 生产互操作的前置条件：

- PT1 文本编码和编码器选择。
- 手工 Heartbeat 消息 API。
- completed-transaction、log-error 等便捷 API。
- 自动探测 hostname/IP、传统全局 singleton 初始化、`client.xml`、fork/multiprocessing 兼容。

## 3. 设计约束

- 保留显式 `MessageTrace` 边界，不给 `CatClient` 增加隐式 `start_transaction()` / `start_event()` stack。
- 不使用 OS TLS 保存 active transaction；跨服务传播上下文必须是显式值对象。
- sender 继续使用非协程 `try_writev()` 数据面；协程只用于 Router/DNS/connect 等需要等待的控制面操作。
- 所有可增长集合必须有消息数、字节数或 key 数量上限。CAT 故障不能给业务流量造成无界内存压力。
- 热记录路径不引入 `std::function`、全局锁、每字段独立分配或重复 `std::string` 扩容。
- producer-loop 状态只在其 owner EventLoop 修改；sender-loop transport 状态只在 sender EventLoop 修改。
- 错误树只绕过采样，不能绕过全局 block、关闭状态或内存/发送队列硬上限。
- Heartbeat 和聚合消息不参与普通业务采样，但仍受独立且有界的系统消息预算约束。
- 不使用 C++ 异常；callback、timer 和 intrusive-entry 回调保持 `noexcept`。
- 记录路径时间使用 `event::EventLoop::current().now()`；wall-clock 只在需要生成 CAT 时间戳或 message ID
  时间段时采样。

## 4. 总体架构

```text
producer EventLoop(s)
    |
    +-- MessageTrace
    |     +-- Transaction / Event tree
    |     +-- explicit propagation context
    |     +-- problem / sampling decision
    |     `-- owner-loop AggregationShard
    |
    +-- Count / Duration Metric
    |     `-- owner-loop AggregationShard
    |
    `-- sampled detailed NT1 frame or bounded aggregate delta
                              |
                              v
sender EventLoop        CatClientCore
    +-- MessageIdGenerator shared atomic sequence
    +-- registered owner-loop aggregation shards
    +-- aggregate/heartbeat flush timers
    +-- Router/DNS/connect control coroutine
    +-- bounded OutboundFrame admission
    `-- Notify collection -> local FIFO -> try_writev()
                              |
                              v
                         CAT collector
```

聚合 shard 的注册、snapshot 和销毁必须具有稳定生命周期。不能让 sender-loop collect callback 保存 producer
协程栈上的临时状态。第一版应采用显式注册、引用计数或 WaitGroup，使 shutdown 能先停止新 trace/metric，再收集或
丢弃所有 shard，最后关闭 sender。

## 5. 阶段一：Message ID 与跨服务传播

### 5.1 工作项

- [x] 新增 message ID 生成器，生成 CAT 可识别且在进程、小时切换和并发 producer loop 下唯一的 ID。
- [x] 对齐官方 ID 的 `domain-ipHex-hour-sequence` 可见结构；明确多进程和进程重启时的去重策略。
- [x] `MessageTrace::create()` 在 `message_id` 为空时自动生成 ID；调用方显式传入时保持原值。
- [x] 新增 owning propagation context，至少包含：
  - `message_id`
  - `root_message_id`
  - `parent_message_id`
  - `session_token`
- [x] 支持从上游请求 context 创建当前 MessageTrace：当前 trace ID 是上游 child ID，root ID 保持不变，parent ID
  指向上游 message ID。
- [x] 支持为指定远端 domain 生成 outbound child context，并允许 HTTP/gRPC 层自行映射到 headers/metadata。
- [x] 明确根 trace、下游 trace、缺失部分 header、非法/超长 ID 的归一化和拒绝行为。
- [x] context 字符串在 trace 创建时复制进 trace pool；公共 owning context 不借用即将释放的 trace 内存。
- [x] message ID 生成失败、context 非法或超限时增加独立统计，不伪装成普通 `Completed`。

### 5.2 API 边界

公共 API 保持显式，建议围绕以下能力设计，不引入 implicit current transaction：

```cpp
auto trace = MessageTrace::create(client, limits, inbound_context);
auto current = trace.propagation_context();
auto outbound = client.create_remote_context(current, remote_domain);
```

最终命名可在实现阶段调整，但必须满足：context 是可复制/可移动的 owning 值；生成 outbound context 不要求存在
OS TLS 或全局 active trace。

### 5.3 验收标准

- [x] 同一小时内单线程、多个 EventLoop 和多个 OS thread 生成的 ID 不重复。
- [x] 小时切换后 prefix 和 sequence 行为正确，旧 context 仍可继续传播。
- [x] root -> service B -> service C 的 root/parent/message ID 关系与官方 CAT 语义一致。
- [x] 空 context 自动形成合法根 trace；显式 context 的 NT1 header golden bytes 不变化。
- [x] ID/context 上限和分配失败路径不能产生部分或悬空 context。

## 6. 阶段二：采样语义与 Transaction/Event 聚合

### 6.1 修正采样边界

- [x] 把采样决策从 `CatClientCore::submit_encoded()` 前移到 trace 已冻结但尚未 NT1 编码的边界。
- [x] 将 `MessageTraceData::has_problem` 传入采样决策；任一非成功 status 或 incomplete message 都使树绕过采样。
- [x] `block=true`、client stopping/stopped、硬预算不足仍可丢弃错误树，并记录准确原因。
- [x] `sample=0` 时普通树进入聚合器而不是在 `MessageTrace::create()` 边界全部拒绝。
- [x] Router 动态改变 sample 后只影响之后完成的树，不修改已经做出决定的树。
- [x] 详细树未命中采样时跳过 NT1 详细编码，避免无效的 exact-buffer 分配和 copy。
- [x] 普通详细树因 normal sender budget 满而无法准入时，在 trace 销毁前回退到聚合，避免统计直接消失。
- [x] problem tree 使用有上限的高优先级/保留预算，优先于普通详细帧；它仍然不能绕过关闭、block 或硬上限。
- [x] 高优先级帧仍然让每个 `OutboundFrame` 直接进入 EventLoop Notify 队列，只在 owner-loop 本地发送 FIFO 和
  admission budget 上区分优先级，不恢复 CAT 私有 MPSC queue。

### 6.2 owner-loop 聚合 shard

- [x] 为每个参与记录的 EventLoop 建立 `AggregationShard`，只允许其 owner loop 修改。
- [x] shard 注册属于冷路径；同一 trace 的 Transaction/Event 聚合更新不加锁、不跨 loop。
- [x] Transaction key 至少包含 type/name，累计 count、error count、duration sum 和官方兼容 duration bucket。
- [x] Event key 至少包含 type/name，累计 count 和 error count。
- [x] 限制 shard 数、key 数、每个 key 的文本长度和总内存；达到上限时记录 overflow/drop。
- [x] sender loop 周期请求 owner loop flush；记录、编码和 reset 均在 owner loop 串行完成。
- [x] flush 不借用 producer 协程栈或已 reset 的 pool；成功提交后才 reset，失败保留到重试或 detach drop。
- [x] 生成官方兼容的 `System/TransactionAggregator` 和 `System/EventAggregator` 消息树。
- [x] 聚合树不再次进入普通采样或聚合，防止递归。

### 6.3 生命周期

- [x] `start()` 后才能注册新 shard；`begin_stop()` 后停止注册和新 trace 准入。
- [x] `begin_stop()` 与 shard 注册通过 mutex barrier 跨过安全边界。
- [x] sender-loop shard 在 shutdown 前最终 flush；producer-loop shard 在显式 detach 时 flush，失败残留计入 drop。
- [x] producer EventLoop 停止前通过 `detach_current_event_loop()` 注销 shard，并跨过完整 Notify phase。

### 6.4 验收标准

- [x] `sample=1` 发送全部详细树，不产生重复 aggregate。
- [x] `sample=0` 不编码普通详细树，但 Transaction/Event 计数、错误数和 duration 聚合正确。
- [x] 中间采样率下，每棵普通树严格进入 detailed 或 aggregate 之一。
- [x] 错误或 incomplete 树在任何采样率下都不会因为采样而丢弃。
- [x] 动态 sample/block、聚合 key 超限、owner-loop flush、detach 和 shutdown race 有确定性测试。

## 7. 阶段三：Metric 聚合与 NT1 `M` 编码

### 7.1 工作项

- [x] 扩展内部消息类型和 encoder，支持 NT1 Metric `M` record。
- [x] Count Metric 编码为官方兼容 status `C` 和 quantity data。
- [x] Duration Metric 编码为官方兼容 status `S,C` 和 `count,sum` data。
- [x] 增加 CatClient-bound Metric 创建入口，使 Metric 能注册到当前 EventLoop 的 aggregation shard。
- [x] 保留 standalone snapshot-only Metric 时，文档和类型状态明确它不会自动发送。
- [x] Metric record 热路径只更新 owner-loop 固定状态；名称在创建/注册时复制一次。
- [x] 周期生成 `System/MetricAggregator` 树并通过现有 OutboundFrame 数据面发送。
- [x] aggregate 只有在成功转移给发送帧后才 reset；编码或准入失败时保留到重试，detach 时统计残留 drop。
- [x] 限制 metric cardinality、名称长度、累计值溢出和 aggregate 大小。
- [x] duration count/sum、signed count 和整数格式边界具有 wire golden 和边界测试。

### 7.2 可选扩展

- [ ] Latest/Gauge Metric。
- [ ] slow threshold 和 `.slowCount`。
- [ ] tag/dimension Metric；只有 CAT Server 的目标版本确认支持后再设计。

### 7.3 验收标准

- [x] `M` record 有来自官方 encoder 的 golden-byte 对照测试。
- [x] Count 的正数、负数、零值和溢出行为明确。
- [x] Duration 多次 observation 的 count/sum 正确，flush 后下一窗口从零开始。
- [ ] 多 producer loop 同名 metric 的服务端可见合计正确。
- [x] Metric 不受普通 sampling 影响，仍服从 block、关闭和独立系统消息预算。

## 8. 阶段四：Heartbeat 与客户端自监控

### 8.1 Wire 与周期任务

- [x] 扩展内部消息类型和 NT1 encoder，支持 Heartbeat `H` record。
- [x] 启动后发送一次 `System/Reboot` 以及 fiber2 CAT client 版本事件。
- [x] 默认每 60 秒生成一次 `System/Status` Transaction 和 `Heartbeat/<client-ip>` 消息。
- [x] Heartbeat 周期由 sender EventLoop `TimerEntry` 调度，不新增 pthread 或 Heartbeat coroutine，
  也不把 sender 改成协程；Router/DNS/connect 控制协程边界保持不变。
- [x] Heartbeat 不参与普通采样；Router block 时不发送。

### 8.2 第一版 Heartbeat 内容

- [x] app key、hostname、IP、客户端版本和进程启动时间。
- [x] 进程 ID、uptime、EventLoop 数量等低成本运行信息。
- [x] `CatClientStats`：queued messages/bytes、submitted/sent、queue full、sampled、unavailable、partial frame、
  encode/router/connect/write failures。
- [x] 当前 collector 状态、Router 最近成功时间和当前 sample/block 状态。
- [ ] Linux CPU、memory、load 等信息通过可选 provider 收集；平台不支持时省略，不阻塞 heartbeat。

### 8.3 约束与验收

- [x] Heartbeat 字段数和总 data bytes 有硬上限，编码失败不影响 CAT sender。
- [x] 周期任务不会积压；上一轮未提交时跳过下一轮。
- [x] `H` record 和包含它的 `System/Status` tree 有 golden-byte 测试。
- [x] Fake collector 能观察启动事件和至少两个可控时间点的 Heartbeat。
- [x] shutdown 取消 timer，结束后无 Heartbeat frame 或引用存活。

## 9. 阶段五：大树截断与长事务

当前 `RecordLimits` 在达到上限时返回 `LimitExceeded`，但 CAT Server 无法从已发送树中判断是否丢失了子消息。

### 9.1 第一阶段：显式丢失标记

- [x] trace 记录 `truncated`、dropped message count、dropped data bytes 和首次失败原因。
- [x] 截断标记由 encoder 的固定栈缓冲生成，不依赖已经耗尽的 trace pool。
- [x] 编码时给 root data 追加 `CatClient.Truncated` 丢失信息。
- [x] 限制事件写入失败、aggregate cardinality overflow 和 transport queue drop 使用不同统计。

### 9.2 第二阶段：可选分段续接

- [ ] 评估并实现官方 `RemoteCall/Next` 和 `TruncatedTransaction/TotalDuration` 兼容语义。
- [ ] 达到 soft message limit 或跨小时窗口时冻结已完成 segment，生成新的 child message ID，并继续记录未完成
  Transaction 链。
- [ ] segment 切换不得使仍存活的 public Transaction handle 指向已释放内存。
- [ ] 每个 segment 独立受队列预算保护，部分 segment 丢失时保留可诊断标记。

只有第一阶段仍不能满足长事务连续链路需求时，才实施第二阶段；分段会显著增加 handle 重绑定和内存所有权复杂度。

## 10. 阶段六：Router/Collector 生产加固与互操作

### 10.1 Router

- [x] 建立 fake HTTP Router，覆盖 literal IP、DNS、多 endpoint 轮换、HTTP 非 200、超时、截断 body、超限 body、
  malformed JSON 和非法 collector。
- [x] 验证失败 refresh 保留最后一个可用 snapshot；合法 `block=true` 可以使用空 collector 集合。
- [x] 验证 block -> unblock 和 sample 动态更新。
- [x] Router 返回的新集合仍包含当前 collector 时保持连接，避免无意义抖动。
- [x] 当前 collector 已被移除时标记连接 stale，在完整 frame 边界切换；不能把一个 frame 分到两个连接。
- [x] 明确 Router 成功返回空但 unblocked、重复 collector 和 IPv4/IPv6 混合列表的策略。

### 10.2 Collector sender

- [x] 覆盖第一个 collector 失败、后续 collector 成功和整轮失败退避。
- [x] 覆盖 `WouldBlock`、可写恢复，并验证部分 frame 不被后到高优先级 frame 抢占。
- [x] 覆盖连接 reset 和部分帧失败时丢弃。
- [x] 覆盖多个 frame 的优先级/FIFO 边界和单个超大 frame。
- [x] 覆盖 Router refresh/connect/write 与 shutdown 的退出路径及并发 submit/shutdown。
- [x] 对 queue-full、blocked、sampled、unavailable、partial-frame drop 做分离统计。

### 10.3 真实服务端互操作

- [ ] 提供 opt-in CAT Server 测试配置，不把外部服务变成普通 CTest 的硬依赖。
- [ ] 验证嵌套 Transaction/Event 的 type/name/status/data/duration 和时间戳。
- [ ] 验证 root/parent/message ID 能在跨服务链路中关联。
- [ ] 验证 Transaction/Event/Metric 聚合报表。
- [ ] 验证 Heartbeat 和客户端版本/状态事件。
- [ ] 记录 CAT Server 版本、启动命令、测试 domain 和查询结果，形成可重复的互操作说明。

## 11. 阶段七：PT1 与兼容扩展

### 11.1 PT1

- [x] `CatClientOptions` 增加 encoder 选择，默认继续使用 NT1。
- [x] 实现 PT1 header、Transaction/Event/Metric/Heartbeat 行记录。
- [x] 对齐 tab/newline、反斜杠和控制字符的官方原样写入，以及 transaction start/end/atomic 形式。
- [x] 使用固定版本官方 `encoder_text.c` 核对 golden text cases。
- [x] PT1 frame 继续复用四字节长度前缀和现有 sender admission，不增加第二套传输路径。

### 11.2 便捷 API

- [x] `log_error()`，同时确保错误树绕过采样。
- [x] completed transaction with duration。
- [ ] 可选手工 Heartbeat API。
- [ ] MessageTrace/Transaction 的只读 type/name/status/duration 访问器；仅在确有调用方需求时增加。

### 11.3 明确不机械移植的官方能力

- 官方 pthread sender、aggregator、monitor 不移植。
- 官方 CATTHREADLOCAL active transaction stack 不移植。
- 官方阻塞初始化、blocking join、私有 socket event loop 和 lock-heavy global map 不移植。
- `client.xml`、全局 singleton、fork/multiprocessing 只在 fiber2 应用提出实际需求后实现，不作为默认架构。

## 12. 统计与可观测性补全

在现有 `CatClientStats` 基础上增加至少以下统计，并保持可原子读取：

- message ID/context 生成失败与非法 context。
- truncated trees/messages/data bytes。
- sampled detailed trees、forced problem trees、aggregated trees。
- aggregation shard/key overflow、aggregate snapshot/encode/drop。
- Metric observations、Metric flush/drop/overflow。
- Heartbeat submitted/sent/skipped/dropped/provider failure。
- Router block/unblock/sample change、collector set change/stale connection switch。
- shutdown flushed/dropped detailed、aggregate、metric 和 heartbeat frame。

统计名称必须区分“记录阶段丢失”“采样转换为 aggregate”“编码失败”“sender 准入失败”和“部分发送失败”，避免
一个消息被多个 dropped counter 重复计数。

## 13. 测试矩阵

| 层次 | 必测内容 |
|---|---|
| Message state | ID/context、status、duration、incomplete、limit/truncation、协程交错 |
| Encoder | NT1/PT1 的 T/E/M/H、空值、varint、escaping、超限和 exact frame bytes |
| Sampling | 0/中间值/1、错误强制详细、block、动态更新、aggregate 防递归 |
| Aggregation | Transaction/Event/Metric、bucket、overflow、snapshot/reset、多 producer loop |
| Router | HTTP/DNS、轮换、失败保留、malformed/oversize、block/sample 更新 |
| Collector | batch、WouldBlock、partial write、timeout、reconnect、failover、stale switch |
| Lifecycle | start 失败、并发提交、shard 注销、timer/callback cancel、drain timeout |
| Interop | 真实 CAT Server 上的详细树、传播链、聚合、Metric、Heartbeat |

每个阶段执行：

```bash
cmake --build build --target fiber_cat_tests
ctest --test-dir build -R '^(Cat|CatClientConfig)' --output-on-failure
```

涉及公共 runtime 或网络组件时再执行完整测试：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

代码和测试完成后统一运行一次 `./format_code.sh`，再执行 `git diff --check`。仅修改本文档时不需要为 Markdown
单独运行 C++ formatter。

## 14. 实施顺序与里程碑

### Milestone A：可关联的详细链路

完成阶段一，并修正阶段二中的错误树采样规则。此时 Transaction/Event 详细数据具有自动 ID 和跨服务关联能力，
错误链路不会因为 Router sampling 丢失。

### Milestone B：完整统计数据面

完成阶段二和阶段三。未采样详细树转化为 Transaction/Event aggregate，Count/Duration Metric 能周期发送，
采样不再意味着统计完全丢失。

### Milestone C：可运维客户端

完成阶段四以及阶段六的 fake peer 测试。CAT Server 能看到 Heartbeat 和客户端自身故障，Router/collector 动态行为
具有确定性回归覆盖。

### Milestone D：生产完整

完成阶段五第一部分和真实 CAT Server 互操作。大树丢失可诊断，详细消息、传播、聚合、Metric 和 Heartbeat 均在
目标 CAT Server 上验证。

### Milestone E：官方兼容扩展

按实际调用方需求实现 PT1、分段长事务和便捷 API。传统全局/TLS/pthread 架构不进入本里程碑。

## 15. 完成检查表

- [x] Message ID 自动生成和跨服务 propagation 已实现并验证。
- [x] 错误树绕过采样，普通未采样树进入聚合器。
- [x] Transaction/Event/Metric 聚合有界、owner-loop-local，并具有显式 producer-loop detach 生命周期。
- [x] NT1 `T/E/M/H` 全部具有官方对照 golden tests。
- [ ] Heartbeat 和 CatClient 自监控可以在 CAT Server 查询。
- [x] Router/collector 故障、动态配置、`WouldBlock`、部分写和关闭 race 有确定性 fake-peer 测试。
- [x] 超限消息树具有明确丢失标记和统计。
- [ ] 真实 CAT Server 互操作流程可重复。
- [x] README 与最终公共 API、配置、生命周期和统计语义一致。
- [x] CAT focused CTest 和完整 CTest 通过，最终 diff 无格式或空白错误。
