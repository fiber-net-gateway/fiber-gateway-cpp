# 项目整体评审（性能 / 设计 / 职责 / 可读性）

> 评审日期：2026-09-26
>
> 评审范围：`src/`、`include/fiber/`、`apps/`、`tests/`、`CMakeLists.txt`、`docs/`、`feature/`。
> 方法：按模块抽样读码 + 全库静态扫描（异常、`std::function`、裸 `new/delete`、函数长度、命名一致性、头文件依赖）+ 与既有审计文档交叉核对。`src/http/` 的逐行性能审计不在此重复，见 [`http_perf_audit.md`](http_perf_audit.md)；2026-08 的 API 契约评审见 [`../docs/src-architecture-api-review.zh-CN.md`](../docs/src-architecture-api-review.zh-CN.md)，本文只复核其中仍然成立或已有变化的条目。

---

## 1. 总体结论

项目整体质量高于同类自研网络库的平均水平，不需要推倒重写。性能优先不是口号，而是落实到了数据结构与调用约定：`IoBuf`/`IoBufChain` 零拷贝视图、intrusive 容器、协程 awaiter 栈上分配、UDP `recvmmsg`/`sendmmsg`/GSO、epoll 批量派发、TLS record writer 整节点 splice、日志 `writev` + EINTR 重试。`std::function` 在整个核心库中只有个位数使用，热路径纪律执行得很到位。测试规模（177 个测试文件、2128 个 `TEST`）和注释质量（大量 inline 所有权 / 生命周期 / 协议契约说明）同样是明显强项。

当前的主要风险不在局部算法，而在**跨模块契约**与**公共边界**：

| 维度 | 评价 | 说明 |
|------|------|------|
| 性能 | ★★★★☆ | 热路径设计扎实；剩余问题集中在 IoBuf 存储无池化、若干每请求分配，以及尚未统一的 deadline 语义 |
| 架构 | ★★★★☆ | 模块目录清晰、公共/私有头分离严格；但 `fiber_lib` 单体过大，QUIC/TLS/HTTP/script 全部耦合在一个静态库 |
| class 职责 | ★★★☆☆ | 多数类边界清楚；`QuicConnection`、`HttpTransport`、`HttpExchange` 仍是明显的 god-class / god-interface |
| 可读性 | ★★★★☆ | 函数长度纪律整体很好（13 万行中 >90 行的产线函数约 95 个）；主要问题是少量巨型状态机、混合语言注释和内部编号引用 |
| 正确性契约 | ★★★☆☆ | timeout / cancellation / loop-affinity 的约定仍靠断言和注释，未进入类型系统 |
| 工程化 | ★★★☆☆ | 无 CI workflow；测试全打进单个 `fiber_tests`；`feature/` 72 个文件平铺且命名混用 |

如果只能做三件事，优先级依次是：**统一 deadline 与 cancellation 契约**、**收缩 `QuicConnection` / `HttpTransport` 公共边界**、**拆分 CMake target**。局部微优化（容器替换、`std::function` 消除等）应在 profiling 之后进行，当前不是主要矛盾。

---

## 2. 值得保持的亮点

这些是评审中确认的、应当作为后续演进的基准约束，而不是偶然的好运：

1. **热路径分配纪律真实存在。** `std::function` 全库仅 5 处（`ThreadGroup`、`HttpHandler`、Nacos `SubscriptionPool`×2、一处注释），核心协议路径为零。`IoBufChain` 节点走 per-loop free-list（`IoBufChain.h:21`，上限 1000 节点），awaiter 全部栈上构造。
2. **系统调用批处理到位。** UDP 收发走 `recvmmsg`/`sendmmsg`（`DatagramFd.cpp:574,662`），流式写走 `writev`/`sendmsg`（`StreamFd.cpp:56,58`），日志写出也用 `writev` 并处理 EINTR 与部分写（`Appender.cpp:228-263`）。
3. **注释即契约。** `Poller`、`HttpTransport`、`TlsRecordWriter`、`QuicConnection::Ops` 的注释明确写出 loop 归属、回调可否销毁 owner、缓冲 retained 指针的存活条件。这类注释在人员变动时的价值极高。
4. **测试策略有层次。** 除单元测试外还有 BoringSSL 差分对端、zlib 1.3.2 符号前缀差分 oracle、nginx/HTTP3 interop、连接池压力与生命周期回归。
5. **函数长度整体可控。** 剔除生成代码与第三方后，>90 行的产线函数约 95 个，主要集中在解析器 / VM / 编译器等状态机；业务与协议封装类基本都在 90 行以内。
6. **头文件规范一致。** 全库 437 个头文件统一使用 `FIBER_<NAME>_H` guard，无 `#pragma once` 混用；公共头不包含 `src/` 的约束在 CMake include 路径上也成立。

---

## 3. P0：契约与正确性（优先修复）

### 3.1 HTTP/3 客户端 timeout 预算会重启 ✅ 已修复

~~`ClientHttp3Exchange::send_request_header()` 先用完整 timeout 打开 stream，再把**同一个相对 timeout** 原样传给 header 发送：~~

- `src/http/ClientHttp3Exchange.cpp:37-47`

```cpp
auto opened = co_await ensure_request_opened(timeout);
...
co_return co_await (*opened)->send_request_header(head, end_stream, timeout);
```

若 stream attach 已消耗部分时间，header 阶段会重新获得完整预算。HTTP/2 侧（`ClientHttp2Exchange.cpp:70` 附近）已经是“先算绝对 deadline，再传剩余时间”的写法，两侧语义不一致。

**建议**：协议层统一引入 `Deadline { TimePoint at; milliseconds remaining(now); }`，公共 API 仍收 `milliseconds`，进入协议层立即转换；`ensure_request_opened` 与后续子操作共享同一个 deadline。补一条“attach 消耗部分时间后 header 必须遵守总预算”的回归。

**落地（2026-09-27）**：`ClientHttp3Exchange::send_request_header()` 进入时先计算绝对 deadline；私有 `ensure_request_opened()` 改为接收 `TimePoint`，内部把剩余时间传给 stream gate；header 发送同样只接收 `remaining_timeout(deadline)`，与 HTTP/2 侧语义对齐。新增回归 `Http3ClientConnectionTest.SendRequestHeaderDeadlineSurvivesStreamAttachWait`：先用 8 条控制流耗尽 bidi stream 配额，400ms 后用 `MAX_STREAMS` 放行，同时用 0 的 bidi-remote 流量窗口卡住 HEADERS 写出——共享 deadline 在 ~500ms 超时，重启预算的旧实现会在 ~900ms 才超时。

### 3.2 timeout / when_any 的取消仍未进入类型系统

`TimeoutAwaiter` 超时后只标记状态并 resume 父协程，不调用 inner awaiter 的 `cancel()`：

- `include/fiber/async/Timeout.h:145-149`

`cancel()` 方法本身已经存在（`Timeout.h:122-135`，带 `requires` 约束），但 `on_timeout` 没有走它。`WhenAny` 胜出后对 loser 只做 `destroy_losers()`（`WhenAny.h:321-326`），同样没有统一取消动作——正确性依赖每个 awaiter 的析构恰好等价于取消。

**建议（2026-09-27 修正）**：原建议 1、3 过度 prescribed，需要按下面的分层模型修正；2、4 仍然成立。

**修正评估：为什么“通用 cancel”不可行，而“销毁协程帧”才是本库的取消模型**

1. **`Task` 没有也无法有通用 `cancel()`。** 一个运行中的 `Task` 是一棵不透明协程树：外层拿到的是 `coroutine_handle`，拿不到“当前挂起在最内层的那个 awaiter”；C++ 协程也不提供这种反射。要通用 cancel 只有两条路：(a) 在每个 Task 内部贯穿协作式 cancellation token（侵入极大，且协议代码已经在用 IoErr/abort 表达终止）；(b) 销毁协程帧，让所有 in-flight awaiter 的析构函数统一反注册。本库选的是 (b)，而且是刻意设计：
   - `TaskSelectAwaiter::~TaskSelectAwaiter()` 直接 `handle_.destroy()`（`TaskSelect.h:28-35`）——销毁整棵帧树，逐层触发内层 awaiter 析构；
   - `WaitAwaiter` 的头文件注释明确写了这条模型：“a hard-destroyed coroutine -- a task discarded by when_any after its result stopped being needed, say -- tears its awaiter down while a resume is still queued”，并用可撤回的 local defer 队列（而非不可撤回的 MPSC）保证销毁安全；
   - `WhenAny` 的 `destroy_losers()` 正是该模型的实现，不是遗漏；
   - `timeout_for(task.select(), T)` 超时后父协程拿到 `TimedOut`，临时对象析构 → `TaskSelectAwaiter` 析构 → 整棵 Task 帧销毁，取消已经发生。

2. **`cancel()` 只对叶子 awaiter 有意义，且语义是“终止 + resume”，不能被 timeout/when_any 在 resume 前调用。** 现有实现里 `ConnectAwaiter::cancel()` 会 unwatch、关 fd、置 `Canceled` 并 `handle.resume()`（`ConnectFd.h:173-188`）；`Watch::NextAwaiter::cancel()` 会从队列摘除 waiter（`Watch.h:283-288`）。若 `TimeoutAwaiter::on_timeout` 先调 `awaiter_.cancel()` 再自己 `handle_.resume()`，对 ConnectAwaiter 这类实现就是 double-resume。当前“标记 timed_out → resume 父协程 → 临时对象析构 → 内层 awaiter 析构反注册”的顺序反而是正确且统一的。`TimeoutAwaiter::cancel()` 保留给外部主动取消（此时由内层 cancel 负责那次 resume）；但“内层 cancel 必然 resume”并非对所有实现成立——两个叶子实现的 resume 语义并不一致（ConnectAwaiter resume，`Watch::NextAwaiter` 只摘队列不 resume，外部调用会挂死父协程），这正是下条把 `CancellableAwaiter` 契约钉死为 must-resume 的原因。

3. **因此 3.2 真正缺的不是“补一个通用 cancel”，而是把既有模型写成可验证契约。** 建议改为：
   - **文档化 Destruction-Cancels 契约**：任何 `SelectableAwaiter` 在挂起状态下被析构，必须 (i) 在所属 loop 上反注册全部 timer/poller/队列/内核状态；(ii) 绝不 resume；(iii) 对“resume 已排队”的场景可撤回（WaitAwaiter 已示范）；(iv) nothrow。`SelectableAwaiter` concept 已强制 nothrow dtor + `completed()`，缺的是把这四条写进 `Awaitable.h` 注释与 `feature/coroutine.md`。
   - **补齐 destruction-safety 回归（2026-09-27 复核修正：部分已存在，按缺口补，不必从零建）**：`WhenAnyTest` 已覆盖自定义 awaiter 的析构取消通知（`PendingAwaiter` 析构置 canceled）、loser Task 帧销毁断言（`loser_task_frame_destroyed`）与 Sleep/Mutex/nested 组合；`QuicLocalStreamGateTest` 已有 4 个 destroying/queued-resume 用例（`DestroyingSignaledHead/MiddleRedistributesCreditInFifoOrder`、`TimedOutHeadRedistributesCreditBeforeItsQueuedResume`、`DestroyingGateCancelsQueuedResumesWithoutAccessingDestroyedOwner`，即 0004 修复的回归）。真正缺的同构用例只有两族：fd 系（`RWFd::WaitAwaiter`、`ConnectAwaiter` 挂起中不 resume 直接析构）与 `timeout_for` + `Task::select()` 组合超时后的整帧销毁。断言口径可行：`EventLoop::poller()` 与 `Poller::size()` 均公开（`Poller.h:60`），“注册数回基线”写得出；更便宜的代理是无崩溃 + 后续操作正常 + ASan。这是把“析构即取消”从注释变成测试的唯一方式（编译期无法验证行为）。
   - **`cancel()` 降级为叶子专属能力（2026-09-27 复核修正：契约必须是 must-resume，现有两个叶子实现语义不一致）**：`ConnectAwaiter::cancel()` 会 resume 父协程（`ConnectFd.h:186`；`DnsClient` 的 `arm_inflight_cancel`（`DnsClient.cpp:536-538`，全库唯一生产调用面）正依赖这一点）；而 `Watch::NextAwaiter::cancel()` 只摘队列、既不 resume 也不置 `completed_`（`Watch.h:283-289`），外部调用会让父协程永久挂起——它事实上只是析构 helper（唯一调用方是自己的析构函数，`Watch.h:254`）。因此 `Awaitable.h` 中的 `CancellableAwaiter` 契约不能写成“可能 resume 恰好一次”（may-resume 让调用方无法写出正确代码），必须钉死为“必须 resume 恰好一次并携带终态错误”；`NextAwaiter` 当前不满足，要么不纳入该 concept，要么先补 resume 语义再纳入。该 concept 仅供外部主动中止使用；禁止 timeout/when_any 在自身 resume 前调用。
   - **`when_any` 保持 `destroy_losers()`**：除非发现某个 awaiter 无法在析构中完成撤回，才为它增加区分于 cancel 的 `detach()`；不要为了对称性预引入。
   - **`Task<T>::operator co_await() &&` ref-qualifier（`Task.h:110,181`）仍然建议做（2026-09-27 复核修正：动机表述与迁移面）**：这与取消无关，但原动机不准确——完成后的 Task 二次 await 并不会 panic：`result_` 是 optional，`return_value` 之后恒 engaged（`Task.h:47-64`），二次 await 拿到的是 moved-from 值，属静默数据 bug 而非崩溃。`&&` 真正在编译期挡住的是三类用法：(a) lvalue 重 await（上述 moved-from 结果）；(b) await moved-from Task——`handle_` 为 null，`await_resume` 里 `handle.promise()` 空解引用，这才是运行期崩溃；(c) 两处并发 await 同一 lvalue——`set_continuation` 被覆盖，第一个父协程永不恢复。迁移面已全库扫描：src/apps/tests/include 无任何 lvalue Task await（现存 `co_await connect_operation` / `co_await waiter` / `co_await saved_combination` 分别是 ConnectAwaiter、WaitAwaiter、WhenAnyAwaiter，不受影响），可零改动落地；`Task::select() &&`（`Task.h:112-113,183-184`）已经示范了该约束的写法。

### 3.3 `SharedDnsCache2` 在 mutex 内隐式依赖 current EventLoop ✅ 已修复

三个 upsert 路径都在持锁状态下调用 `event::EventLoop::current().now()`：

- `src/dns/DnsCache2.cpp:783`（address_set）
- `src/dns/DnsCache2.cpp:801`（cname）
- `src/dns/DnsCache2.cpp:816`（nxdomain）

mutex 不能消除 `EventLoop::current()` 的 TLS 前置条件；从无 loop 线程调用会在断言处崩溃。`lookup()` 已经显式接收 `TimePoint now`，说明接口本可以统一。

**建议**：所有 cache 操作显式传入 `now`（调用方按项目约定取 `EventLoop::current().now()`），或统一投递到 owner loop。补一条“无 current loop 线程调用 upsert”的单元测试。

**修改方案（2026-09-27）**：选择“显式传 `now`”，不选择 owner-loop 投递。

1. **API 变更（`DnsCache2.h`）**：`SharedDnsCache2` 的三个 upsert 方法补 `TimePoint now` 参数，位置与 `lookup(key, now, out)` 保持一致（第二个参数），例如：

   ```cpp
   [[nodiscard]] common::IoErr upsert_address_set(DnsCacheKey key, TimePoint now,
                                                  net::IpFamily family, const net::IpAddress *addresses,
                                                  std::uint16_t count, TimePoint expire_at) noexcept;
   ```

   `upsert_cname / upsert_nxdomain` 同样处理。`erase` 与 `entry_count / bytes_used` 不触碰时间，不需要改。头文件注释明确契约：`now` 是调用方提供的单调时间（通常是调用方自己 loop 的 `now()`）；`SharedDnsCache2` 本身不再要求存在 current EventLoop。

2. **实现变更（`DnsCache2.cpp:783,801,816`）**：把三处 `cache_.expire_due(event::EventLoop::current().now())` 改为 `cache_.expire_due(now)`。`maintain_and_rearm()` 保持现状——它运行在 owner loop 上，使用 `owner_loop_->now()` 是显式且正确的。顺带检查 `DnsCache2.cpp` 是否仍需要包含 `EventLoop.h`（`SharedDnsCache2` 仍持有 `EventLoop*`，头文件仍需要；cpp 视使用情况清理）。

3. **产线调用（`DnsResolverLocal.cpp`）**：无需新逻辑。`inspect_response()` 已经在 `:743` 一次性取 `const auto now = loop_->now()`，四处 upsert（`:751,807,840,900`）只是把已有的 `now` 传下去，同时消除“同一响应处理内 expire 与 expire_at 使用不同时间源”的微小不一致。

4. **测试迁移**：三个 app 测试辅助（`NacosTestDns.h:44-45`、`NacosClientTest.cpp:382-383`、`CatClientTest.cpp:670-671`）传入 `std::chrono::steady_clock::now()` 或所属 loop 的 `now()`。

5. **新增回归（放在 `tests/DnsCache2Test.cpp`）**：
   - `UpsertWorksWithoutCurrentEventLoop`：启动一个 `SharedDnsCache2`（owner loop 运行中），从普通 `std::thread`（无 current loop）调用三个 upsert，断言不触发 `FIBER_ASSERT` 且返回 `None`；回到 owner loop `lookup` 能命中。这是 3.3 的直接回归。
   - `CallerSuppliedNowDrivesExpiry`：用 `now = T` 写入 `expire_at = T - 1ms` 的条目，随后用 `T + 1ms` lookup 应 Miss，证明过期完全由调用方时间驱动，与任何 TLS loop 无关。

6. **为什么不选 owner-loop 投递**：resolver 在解析响应后需要同步拿到 upsert 结果决定 `RetryFromCache`；投递到 owner loop 会引入队列分配、跨线程往返和结果回传，复杂度远高于传入一个已经在调用栈里的时间值。mutex 已保证数据结构线程安全，“当前时间”本来就不该是隐式 TLS 依赖。若未来出现高频跨线程写缓存，再评估 sharded cache 或 owner-loop 批量提交，并与本方案对比 profiling 数据。

**落地（2026-09-27）**：按上述方案实施。`SharedDnsCache2` 三个 upsert 的 `TimePoint now` 均为第二个参数（与 `lookup` 对齐），头文件注释写明“调用方提供的单调时间，写入不要求 calling thread 存在 current EventLoop”；`DnsCache2.cpp` 三处 `EventLoop::current().now()` 全部替换为参数；`DnsResolverLocal.cpp` 四处复用 `inspect_response()` 已有的 `now`。测试侧迁移 `DnsResolverLocalTest / DnsResolverTest / NacosTestDns / NacosClientTest / CatClientTest`，并新增 `SharedDnsCache2Test.UpsertWorksWithoutCurrentEventLoop`（在无 current loop 的主线程直接调用三个 upsert，随后回 owner loop 验证三类条目均命中）与 `SharedDnsCache2Test.CallerSuppliedNowDrivesExpiry`（`expire_at < now` 的条目按调用方时间判 Miss）。DNS 相关 58/58、Nacos 58 通过 + 3 环境跳过、CAT 12/12 通过。

### 3.4 异常策略违反仓库自身规范 ✅ 已修复

`AGENTS.md` 明确“不要写 `throw`”，但仍有两类公共代码抛异常：

- `include/fiber/common/mem/BufPool.h:67,71` — `PoolAllocator::allocate()` 抛 `std::bad_alloc`；
- `include/fiber/common/util/RoutePathMatcher.h:344,350,354,479` — builder 抛 `RoutePatternError` / `logic_error`。

同时仓库其他部分全部使用 `IoResult` / `std::expected` / nothrow 分配。这不是风格问题：调用方无法稳定推断某条链路是否可能展开异常。

**建议**：二选一并写入文档——(a) 核心库彻底 nothrow：删除 `PoolAllocator`，Route builder 改 `std::expected<..., RouteError>`；(b) 仅脚本编译等冷路径保留异常并显式标注。不要维持“`noexcept` 函数内部可能抛异常”的混合状态。另外审计所有 `noexcept + std::make_unique/string` 组合（旧评审已列出若干）。

**细化评估（2026-09-27）**：两个问题应分开处理，且改动成本都比想象中小。

1. **`RoutePathMatcher` → `std::expected`：建议采纳，收益明确。** 当前唯一抛异常的公共入口是 `Builder::add_route()`，共 3 个校验点（空 pattern、非 ASCII、通配符必须在末段，`RoutePathMatcher.h:350,354,479`）；另有一处 `ensure_mutable()` 的 `logic_error`（`:344`）。而两处产线调用（`RuntimeBuilder.cpp:337,438`）的本意就是 expected——它们 catch 后立即转成 `std::unexpected(make_error(location, error.what()))`，异常只是多余的间接层。建议 API：

   ```cpp
   struct RoutePatternError {   // 不再继承 invalid_argument
       std::string message;
   };
   [[nodiscard]] std::expected<void, RoutePatternError>
   add_route(std::string_view pattern, BuilderPayload payload);
   ```

   `ensure_mutable()` 按 AGENTS 的不变量约定改为 `assert(!complete_)`（builder 完成后再 add 是编程错误，不是配置错误）。`build()` 可继续直接返回 matcher——它的容器分配属于冷路径，且与本次“显式校验错误不抛异常”的目标无关。迁移面：2 处产线调用 + 2 个 `EXPECT_THROW` 测试，成本很小。冷路径里 `std::string message` 可接受；若想彻底零分配，可用 `enum class RoutePatternErrorCode` + 固定文案，但属可选优化。

2. **`IoBuf::allocate`：无需改动，它已经是这个模式。** `IoBuf::allocate()` / `allocate_trackable()` 本身就是 `noexcept`，分配失败返回 invalid（`operator bool == false`）的空 `IoBuf`（`IoBuf.cpp:174-176,207`）。全库 54 个调用点全部走 `if (!buf) return NoMem / co_return unexpected(NoMem)` 的检查路径，没有异常。真正违反策略的是 `BufPool::PoolAllocator::allocate()`（`BufPool.h:65-73`）——它抛 `std::bad_alloc`，且受 C++ Allocator named requirement 约束**不能**改成返回空指针（`allocate()` 的契约是“返回有效指针或抛异常”）。同时全库 grep 显示 `PoolAllocator` 零使用。结论：直接删除 `PoolAllocator`（连同 `<new>` 依赖）是最优解；需要池分配的调用方直接用 `pool.alloc<T>()`（已返回 `nullptr`），需要 STL 容器时再设计显式的 `PoolVector`，不要伪装成 conforming allocator。

**落地（2026-09-27）**：按上述方案实施。

1. `RoutePatternError` 改为普通值类型（`std::string message`，不再继承 `invalid_argument`）；`Builder::add_route()` 返回 `std::expected<void, RoutePatternError>`，内部 `validate_ascii_pattern()` 与 `add_path()` 同步改为 expected 传播；`ensure_mutable()` 删除，builder 完成后继续使用按不变量处理为 `assert(!complete_)`。`RuntimeBuilder.cpp` 两处 try/catch 改为直接检查 `added` 并 `std::move(added.error().message)` 组装 `RuntimeError`，消除异常间接层。
2. 删除 `BufPool.h` 中零使用的 `PoolAllocator` 及其 `<new>/<memory>/<type_traits>` 依赖；`BufPool.cpp` 因自身使用 `std::align` 显式补 `<memory>`。
3. 测试更新：两个 `EXPECT_THROW` 改为断言 `expected` 失败与错误文案；`HttpScriptFuncsTest` 的直连 builder 调用补 `EXPECT_TRUE`。`RoutePathMatcherTest.* + HttpScriptFuncsTest.*`（12/12）与 `LiteNginxConfigTest.* + LiteNginxRuntimeTest.*`（102/102）通过，`fiber_tests`、`fiber_app_lite_nginx`、`lite_nginx_tests` 构建通过。

### 3.5 `EventLoopGroup` 公开可变内部容器 ✅ 已修复

- `include/fiber/event/EventLoopGroup.h:38-39`

```cpp
std::vector<std::unique_ptr<EventLoop>> loops_;
fiber::async::ThreadGroup threads_;
```

外部可以直接 move 走 loop、清空 vector，破坏 stop/join 的全部前提。

**建议**：改 private，暴露 `size()/at()/next()/running()`；确需轮询调度时提供 `EventLoop& pick负载均衡策略()` 而不是暴露数组。

**代码实现方案（2026-09-27，未写代码）**：

1. **调用面核查结论**：`rg "\.loops_|->loops_|\.threads_|->threads_" src include apps tests example` 在 `EventLoopGroup`/`ThreadGroup` 自身之外命中 **0 处**；105 个使用 `EventLoopGroup` 的文件全部通过 `at()/size()/start()/stop()/join()/running()` 访问。因此不需要任何 accessor 替换或兼容层。

2. **头文件变更（`EventLoopGroup.h`）**：把 `private:` 标签上移到 `loops_` 与 `threads_` 之前，成员物理顺序保持 `loops_ -> threads_ -> running_` 不变（只改访问属性，不改声明顺序，布局与 ABI 事实不变）。删除成员旁的公开注释噪音，类定义只保留既有方法面。

3. **不新增 `next()/pick()`**：当前没有调用方需要组内轮询调度；按“最小变更、不预设需求”原则不添加。若未来 `lite_nginx` 或连接池需要负载均衡入口，再以 `EventLoop &next(RoundRobinState&)` 形式单独设计（要考虑 per-caller 游标还是 per-group 游标，避免多租户互相干扰）。

4. **`ThreadGroup` 不动**：它的 `threads_` 已经是 private，公开的 `Thread` 方法面（`index()/group()/stop_token()`）没有暴露可变内部状态。

5. **验证**：纯访问控制收窄，现有代码应零改动编译通过——`cmake --build build --target fiber_tests fiber_app_lite_nginx lite_nginx_tests fiber_nacos_tests fiber_cat_tests fiber_prometheus_tests`；随后跑 `EventLoop*`、`Server*Lifecycle*`、`Http*Endpoint*` 与 apps 侧使用 group 的套件。若出现编译错误，说明该处本来就在破坏不变量，按调用意图改用 `at(index)` 而不是回退访问属性。

6. **回归加固（可选，一行）**：`tests/EventLoopGroupTest.cpp` 若存在 `at()` 越界断言用例则直接复用；无需为“成员变 private”写行为测试——编译器就是测试。

**落地（2026-09-27）**：`EventLoopGroup.h` 将 `private:` 上移至 `loops_` / `threads_` 之前，成员物理顺序不变。全库构建（`fiber_tests`、`fiber_app_lite_nginx`、`lite_nginx_tests`、`fiber_nacos_tests`、`fiber_cat_tests`、`fiber_prometheus_tests`）零改动通过，证实无外部直接访问。`EventLoop*` + `ServerLifecycleTest` + 三协议 `OneShardPerWorkerLoop` 共 23/23、`LiteNginxRuntimeTest` 63/63 通过。

---

## 4. P1：class 职责与模块边界

### 4.1 `QuicConnection` 仍是 god-class

- 头文件 1170 行、28 个 class/struct；实现 3673 行。
- 单类同时管握手、stream 表、frame 编解码、crypto、流控、丢包恢复、拥塞控制、path、timer、endpoint lease。
- `Options`（`QuicConnection.h:434` 起）混合地址、CID、内存池、EventLoop、TLS、owner 裸指针和 7 个 C 函数指针回调（`Ops`，`:462-503`）。

`Ops` 的注释契约写得很好（回调不得销毁 owner、不得嵌套驱动 loop、close 时无 detach 通知等），这正是问题所在的证明：**约束只能靠注释维持，说明边界本身太宽**。

**建议**（渐进，不推倒）：

1. 保留 `QuicConnection` façade，内部按值组合 `HandshakeContext / StreamManager / FlowControl / LossRecovery / PathManager / CidManager / TimerSet`；
2. `Ops` 拆成 `StreamOps / StateOps / SessionOps` 三个小结构，或用带生命周期说明的 callback handle 包装 `void* + fn`；
3. `QuicPathManager::paths()`（`QuicPathManager.h:28`）不再返回整个可写 `std::array`，改 `for_each_active()` / `find()` / `create()` 受控操作；
4. 把 `QuicProtocol.h` / `QuicPacketProcessor.h` 对完整 `QuicConnection.h` 的包含降为 `QuicTypes.h` 轻量类型。

### 4.2 `HttpTransport` 是“整个传输栈”接口

- `include/fiber/http/HttpTransport.h`，约 30 个纯虚函数，覆盖 shutdown、三个方向的 readiness callback、六个 poll 读写、六个异步读写、loop handover、idle 观测、close/状态/ALPN/fd。

注释把每种能力的行为都定义得很清楚，但接口本身没有按能力分界，新增一种传输（如 QUIC datagram、Unix stream 变体）必须实现全部。

**建议**：拆 `Readable / Writable / ReadinessNotifier / Handshakeable / Closable` 能力接口，或引入统一的 `IoOperation` 消除 `void*` 与 `IoBuf`/`IoBufChain` 的重复虚函数矩阵。`HttpExchange`（190 行、12 个 struct、多个协议 friend）可同步拆成 `RequestView / BodyReader / ResponseWriter / ExchangeState` façade。

### 4.3 单体 `fiber_lib` 放大编译与依赖耦合

- `CMakeLists.txt:37` 用 `file(GLOB_RECURSE)` 把全部 `src/*.cpp` 收进一个静态库；
- HTTP 使用者被迫带上 QUIC/TLS/script/dns 的全部符号与头依赖；
- 任何一个 `.cpp` 改动都可能触发大范围重链。

**建议**分三步：

1. 先把 QUIC 协议基础类型从 HTTP 公共头中移出（无 ABI 变化）；
2. 拆 `fiber_common / fiber_event_async / fiber_net_dns / fiber_http / fiber_quic / fiber_script`；
3. `fiber_http` 之类的目标内部再用 `target_sources` 显式列文件，替代 `GLOB_RECURSE`（保留 `CONFIGURE_DEPENDS` 只是缓解，不是解决）。

### 4.4 fd 与 borrowed view 的所有权仍靠约定

`TcpStream` 直接接收/返回裸 `int fd`；`HttpHeaders::add_view/set_view` 保留外部指针；`JsValue` 的 borrowed string/binary、`Script::exec_async()` 的 `void* attach` 同理。旧评审的判断仍然成立。

**建议**：引入 `UniqueFd` + `AdoptFd` 标签区分接管/借用；header 分离 `OwnedHeaderBlock` 与 `HeaderView` 两种类型；脚本侧至少在文档固定 borrowed 生命周期，理想情况下用 `std::span` / `Borrowed<T>` 表达。

---

## 5. P2：性能（HTTP 之外；HTTP 见专项审计）

> `src/http/` 已有 [`http_perf_audit.md`](http_perf_audit.md) 逐条覆盖（chunked syscall 合并、QPACK 静态表 hash、Huffman 决策、header hash 重载、H3 scratch 复用等），此处不重复。以下是非 HTTP 模块的新增或跨模块观察。

### 5.1 `IoBuf` 存储仍是裸 `operator new`，无尺寸分级池

- `src/common/mem/IoBuf.cpp:207`

`IoBufChain` 节点有 per-loop free-list，但节点承载的 **storage** 每次 `IoBuf::allocate` 都是裸分配、释放。全库 54 处直接调用，集中在 TLS handshake（7+7）、H1 收发（7+7+2）与 QUIC。小包（TLS record 头、QUIC header、HTTP 前缀）反复走全局分配器。

**建议**：先测后做——在 `IoBufStorageBudget` 已有的容量统计上加 per-loop 尺寸分级 free-list（如 256B/1K/4K/16K 档），或复用现有 `BufPool`。注意 `IoBufStorageBudget` 的 retain 语义：池化层必须尊重 budget 的 reject 语义，不能为了复用绕过内存上限。

### 5.2 脚本 VM 每次执行分配寄存器/变量槽

- `src/script/run/InterpreterVm.cpp:52-66`

构造函数按 `stack_size + var_table_size` `new[]`，请求级 `GcHeap` + 每请求 VM 执行意味着热脚本每请求一次堆分配（脚本槽位通常是编译期常量）。

**建议**：按 `Compiled` 尺寸在 per-loop / per-request 复用池中缓存 `slots_`；或小脚本（如 ≤64 槽）直接用栈上 `std::array`。需与 `GcRootSet` 的 visit 生命周期一起设计，避免复用时残留旧 root。

### 5.3 `BinaryHeap` 是指针二叉堆，cache 局部性一般

- `include/fiber/common/BinaryHeap.h:19-22`（每节点 3 指针）

EventLoop 定时器、QUIC 重传等高频使用。intrusive 设计换来了 O(log n) 任意删除与稳定句柄，这是合理取舍；但相比 4-heap 数组布局，指针跳转的 cache miss 在万级 timer 时会显现。

**建议**：仅在 timer 数量 profiling 显示为热点时改造为“数组 4-heap + 句柄槽位回收”，不要预先重构。保持 `BinaryHeapNode` 布局兼容的过渡方案是先加 benchmark（`tests/BinaryHeapTest.cpp` 已有基础）。

### 5.4 `EventLoop::run_once` 静默吞掉非 EINTR poller 错误

- `src/event/EventLoop.cpp:159-167`

`count < 0 && errno != EINTR` 时直接 `return`，下一轮继续 `wait`。若 `epoll_wait` 因 EBADF/EFAULT/EINVAL 持续失败，loop 会静默自旋，既无计数也无诊断。

**建议**：记录 `IoErr` / 诊断计数（与 log 系统解耦的 loop 内环形错误槽），连续失败 N 次后 `stop_requested_ = true` 并向 `EventLoopGroup` 上报。保持 nothrow。

### 5.5 日志与 DNS 的既有设计值得推广

日志 `FileAppender` 的缓冲、轮转、EINTR/部分写、incomplete-tail 回滚、归档保留（`Appender.cpp:228-263,661-758`）是库内工程化程度最高的组件之一，可作为其他 I/O 组件错误路径的参照。`DnsCache2` 显式 `TimePoint` 的 lookup 签名也应推广到 upsert（见 3.3）。

---

## 6. P3：可读性与一致性

### 6.1 公共枚举存在拼写错误：`JsNodeType::Interator`

- `include/fiber/script/JsValue.h:32`
- 全库 13 处使用（`NodeText.h:77`、`JsValueEncode.cpp:162`、`Compiler.cpp:508` 等）

应为 `Iterator`。作为公共 API，越晚改名成本越高。

**建议**：单次提交统一改名（脚本引擎尚未声明稳定 ABI，成本可控）；若必须兼容，保留旧枚举值并标记弃用一个版本。

### 6.2 巨型函数集中在状态机（需权衡，不宜一刀切）

剔除生成代码与第三方后，产线代码 >90 行函数约 95 个，最长的几个：

| 函数 | 位置 | 行数 | 性质 |
|------|------|------|------|
| `RequestLineParser::execute` | `src/http/Http1Parser.cpp:204-738` | 534 | 手写状态机 |
| `InterpreterVm::iterate` | `src/script/run/InterpreterVm.cpp:92-497` | 405 | VM 分发循环 |
| `RuntimeBuilder::build` | `apps/lite_nginx/src/runtime/RuntimeBuilder.cpp:231-590` | 359 | 配置装配 |
| `quic_create_output_frame` | `src/quic/QuicTransportCodec.cpp:966-1324` | 358 | frame 序列化 |
| `compile_expression` | `src/script/ir/Compiler.cpp:715-1067` | 352 | 编译器 visitor |
| `ClientHttp1Exchange::read_header` | `src/http/ClientHttp1Exchange.cpp:1515-1785` | 270 | 解析循环 |
| `Tokenizer::process` | `src/script/parse/Tokenizer.cpp:12-203` | 191 | 词法状态机 |

解析器 / VM 的单体 switch 有性能与局部性理由，不必强行拆散。但可以低成本改善：

- `InterpreterVm::iterate`：把 binary/unary 之外的 opcode 族拆成 `dispatch_load_store / dispatch_object / dispatch_call / dispatch_control` 私有方法，主循环保留 switch 骨架；或按 opcode 分组加 `// --- stack ops ---` 分节注释。
- `RuntimeBuilder::build`：按 listener/server/upstream/logging/dns 五段拆私有函数——这是普通装配代码，没有状态机理由。
- `quic_create_output_frame`：按 `QuicOutputFrame` 变体拆 encoder 函数，switch 只做派发。

### 6.3 注释中的内部编号与混合语言

代码注释质量很高，但存在两类噪音：

1. 26 个文件保留 IDE 生成的 `// Created by dear on 2025/...` 头（`Allocator.cpp`、`JsonDecode.cpp`、`JsValue.h`、`GcInternal.h` 等）；
2. 引用未在任何文档解释的内部编号，例如 `10 §10`、`10 定谳 2`、`06 codec补齐`（`TlsHandshakeContext.cpp:567`、`TlsServerHandshakeEngine.cpp:248`、`TlsHandshakeCodec.cpp:399` 等），以及中英文混排（`QuicStream.cpp:652-707`）。

**建议**：删除 `Created by` 头；把 `10 §N` 这类编号替换为指向具体 feature 文档的相对链接（如“见 `feature/tls.md#record-split`”）；注释语言统一为英文（现有英文注释占绝对多数，迁移成本最低）。

### 6.4 `feature/` 文档组织

- 72 个文件平铺，54 个用下划线、9 个用连字符，还有拼写错误 `all-berchmark.md`；
- `docs/`（稳定文档）与 `feature/`（审计/设计记录）的边界在部分条目上模糊。

**建议**：新建 `feature/http/`、`feature/quic/`、`feature/tls/`、`feature/lite-nginx/`、`feature/archive/` 子目录；统一 kebab-case；`docs/src-architecture-api-review.zh-CN.md` 中仍然有效的结论（本文 P0/P1 多数条目）合并成一份“当前有效契约”文档，历史评审移入 `feature/archive/`。

### 6.5 命名不一致的小项

- `GcMark::GcMark_0 / GcMark_1`（`JsValue.h:16-17`）违反仓库 PascalCase 约定，应为 `Phase0/Phase1`；
- `JsValue` 相关 API 是 `js_value_*` C 风格自由函数，与库内其余 PascalCase 方法风格不一致。若是刻意为 C ABI / 脚本引擎稳定接口保留，应在 `JsValue.h` 顶部加一句说明；否则逐步收敛为成员函数。

---

## 7. 测试与工程化

1. **无 CI workflow。** 仓库没有 `.github/workflows/`，全部验证依赖本地构建。建议最小化起步：Debug 构建 + `ctest`（可先只跑非 interop 子集）+ clang-format 检查 + `check_ssl_free.sh`；慢速的 BoringSSL 差分与 nginx interop 用 nightly job。
2. **单个 `fiber_tests` 装载 177 个文件 / 2128 个用例**（`CMakeLists.txt:203-227`）。优点是差分 oracle 与 mock 复用方便，缺点是链接时间与单点失败。apps 侧已有 `fiber_cat_tests / fiber_nacos_tests / fiber_prometheus_tests / lite_nginx_tests` 分目标先例，核心库可按模块渐进拆分（如先分出 `fiber_quic_tests`、`fiber_script_tests`）。
3. **缺 deadline / cancellation 回归。** 3.1、3.2 对应的行为目前没有测试兜底，是契约修复的前置条件。
4. **缺少性能回归基准的自动化。** `feature/` 下已有大量一次性 benchmark 报告，但没有可在 CI 稳定运行的微基准集合（BinaryHeap、IoBuf、QPACK、HPACK 已有测试但未纳入 perf 门禁）。建议先挑 3-5 个方差小的（QPACK 静态表、Huffman、IoBufChain splice、timer insert/remove）建立可重复脚本，再谈门禁。

---

## 8. 实施路线图

### 第一阶段（1-2 周，契约修复，不改 ABI）
1. ~~`Deadline` 类型 + HTTP/3 timeout 修复与回归（3.1）；~~ 已完成（HTTP/3 exchange 层；通用 `Deadline` 类型待 3.2 一并收敛）；
2. 文档化 Destruction-Cancels 契约 + 补齐 destruction-safety 回归缺口（fd 系 awaiter 与 timeout-over-Task，3.2）；
3. `Task::operator co_await() &&`（3.2）；
4. ~~`SharedDnsCache2` upsert 显式传 `now`（3.3）；~~ 已完成；
5. ~~`EventLoopGroup` 成员私有化（3.5）；~~ 已完成；
6. `Interator -> Iterator` 改名（6.1）；
7. 删除 `Created by` 头（6.3）。

### 第二阶段（2-4 周，边界收缩）
1. `QuicConnection::Ops` 拆分 + `QuicPathManager::paths()` 收口（4.1）；
2. `HttpTransport` 能力接口拆分（4.2）；
3. `UniqueFd / AdoptFd` 与 `HeaderView` 分型（4.4）；
4. ~~异常策略二选一落地：删除零使用的 `PoolAllocator`，`RoutePathMatcher::add_route` 改 `expected`（3.4）；~~ 已完成；
5. `EventLoop::run_once` 错误上报（5.4）。

### 第三阶段（1-2 月，结构与性能）
1. CMake target 拆分（4.3）；
2. `IoBuf` 尺寸分级池（先 benchmark，尊重 `IoBufStorageBudget` 语义）（5.1）；
3. 脚本 VM 槽位复用（5.2）；
4. `QuicConnection` 内部组件化（4.1）；
5. 巨型状态机函数分节/拆分（6.2）；
6. `feature/` 目录重组与稳定契约文档合并（6.4）；
7. 最小 CI + 核心测试拆分 + 微基准脚本（7.1-7.4）。

---

## 9. 附录：本次验证使用的静态扫描口径

- 异常：`rg "throw\b" src include apps tests`（原评审时生产代码命中 `BufPool.h`、`RoutePathMatcher.h`，均已修复；其余命中为 `new (std::nothrow)` / static_assert 文案 / 测试脚本字符串）；
- `std::function`：全库 5 处；
- 裸 `new/delete`：按文件聚合，`QuicFrame/QuicConnection/RWMutex/连接池` 等为主，均为 nothrow + 显式 destroy 模式；
- 函数长度：剔除字符串/字符字面量与注释后按花括号深度统计，排除 `tests/support/` 与 `src/http/generated/`；
- 头文件规范：`#pragma once` 0 处，`FIBER_*` guard 437 处；
- 测试规模：`tests/*Test.cpp` 177 个文件，`^TEST` 2128 处。
