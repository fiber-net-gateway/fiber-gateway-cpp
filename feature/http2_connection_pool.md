# HTTP/2 Client Connection Pool 设计方案

单线程（单 `EventLoop`）内的池化 HTTP/2 客户端连接池设计。目标是：**用尽量少的连接承载尽量多的并发请求**，把负载压在链表头部的少数连接上，让尾部连接自然空闲并超时关闭。

本文只描述实现方案，不含代码。

---

## 1. 目标与非目标

### 目标

- 以 `HttpConnectionGroupKey`（由 `Http1ConnectionGroupKey` 更名）为 key，内部用与 H1 池同源的开放寻址 hash 表按组管理连接。
- 同一连接在未达并发上限时**保留在池中**，多个协程可以基于它并发发起请求（多路复用）。
- 一旦某连接 concurrent stream 数达到上限，把它从可用链表中**摘出**，不再分配新请求。
- 被摘出的连接上任意一个请求结束后，把它**放回可用链表头部**。
- 头部重载、尾部空闲；尾部连接空闲超时后优雅关闭（GOAWAY）。
- 不考虑跨线程：单个池只在一个 loop 上使用，所有状态 loop-affine。

### 非目标

- 不做跨 loop steal（不提供 `StealableHttp2ConnectionPoolSet`）。
- 不做 H1/H2 混合协商池（ALPN 协商产物落到哪个池由上层决定）。
- 不做请求级重试/负载均衡策略（属于上层 upstream 逻辑）。

---

## 2. 与 HTTP/1 池的语义差异

这是整个设计的根源，实现时必须时刻对照：

| 维度 | `Http1ConnectionPoolCore` | `Http2ConnectionPoolCore` |
|---|---|---|
| 池中缓存的对象 | **idle** 连接 | **全部存活** 连接（含正在跑请求的） |
| lease 语义 | 独占整条连接；lease 期间连接不在池中 | 占用连接上的**一个 stream 名额**；连接始终在池中 |
| 命中失败 | 直接新建（调用方自己 dial） | 可能等待在建连接就绪 / 等待名额释放 |
| `acquire` | 同步返回 | **协程**（可能挂起） |
| 组内顺序 | LIFO（back 复用） | **head 优先**，头部最热 |
| 淘汰依据 | `idle_since_` 全局 LRU | 仅 `active_streams_ == 0` 的连接进 LRU |
| 连接失效 | 下次复用时由 `reusable()` 判定 | 需要 GOAWAY / SETTINGS / close 的**主动通知** |

结论：H2 池不是 H1 池加一个计数器，`Lease`、淘汰、生命周期都要重做；只有 **group key + bucket index（hash 表）** 可以真正复用。

---

## 3. 命名与共用组件抽取

### 3.1 重命名（独立一次 `refactor(http)` 提交）

| 现名 | 新名 |
|---|---|
| `Http1ConnectionGroupKey` | `HttpConnectionGroupKey` |
| `Http1ConnectionPoolAffinity` | `HttpConnectionPoolAffinity` |
| `Http1ConnectionBucketIndex` | `HttpConnectionBucketIndex` |
| `Http1ConnectionGroupHintTable` | `HttpConnectionGroupHintTable`（可选，仅为一致性） |

头文件同步改名，宏改成 `FIBER_HTTP_HTTP_CONNECTION_GROUP_KEY_H` 等。涉及改动的文件（`grep` 结果）：

```
include/fiber/http/{Http1ConnectionGroupKey,Http1ConnectionPoolAffinity,
                    Http1ConnectionBucketIndex,Http1ConnectionGroupHintTable,
                    Http1ConnectionPoolCore,Http1ConnectionPoolEntry,
                    LocalHttp1ConnectionPoolSet,StealableHttp1ConnectionPoolSet}.h
include/fiber/http_script/HttpScriptServices.h
src/http/{Http1ConnectionBucketIndex,Http1ConnectionGroupHintTable,Http1ConnectionGroupKey,
          Http1ConnectionPoolCore,Http1ConnectionPoolEntry,StealableHttp1ConnectionPoolSet}.cpp
tests/{Http1ConnectionBucketIndex,Http1ConnectionGroupHintTable,Http1ConnectionGroupKey,
       Http1ConnectionPool,LocalHttp1ConnectionPoolSet,StealableHttp1ConnectionPoolSet}Test.cpp
apps/lite_nginx/src/runtime/{HttpScriptServices.cpp,RuntimeBuilder.cpp,RuntimeConfig.h}
apps/lite_nginx/src/upstream/{ConnectionPool.h,ConnectionPool.cpp,UpstreamConnection.h,
                              UpstreamConnection.cpp,UpstreamRegistry.h}
docs/http1-connection-pool.md, docs/tls-client-identity.md
```

不保留旧名 alias：仓库内全量引用可控，留 alias 反而让两套名字长期共存。

### 3.2 `HttpConnectionBucketIndex` 泛化

现在 `Http1ConnectionBucketIndex` 的 slot 里存的是 `Http1ConnectionPoolGroupBucket *`，H2 需要放自己的 bucket。做法（避免模板化整份 Robin-Hood 探测实现）：

- 新增 `HttpConnectionPoolBucketBase`：只含 `std::uint32_t slot_index_ = kInvalidSlotIndex;` 和 `slot_index()` 访问器。
- `Http1ConnectionPoolGroupBucket` 与新增的 `Http2ConnectionPoolGroupBucket` 都继承它（非虚，无虚表，仍是 POD-ish）。
- `HttpConnectionBucketIndex` 的 slot 改存 `HttpConnectionPoolBucketBase *`，`find()/insert()/bucket_at()` 返回基类指针，调用方 `static_cast` 回自己的 bucket 类型。
- `key_storage`、hash、探测逻辑完全不动，`Http1ConnectionBucketIndexTest.cpp` 只需改名和类型。

> 备选：`template<class Bucket> class HttpConnectionBucketIndex`。会把 400 行探测逻辑搬进头文件、双份实例化，不划算。选基类方案。

### 3.3 group key 语义

`HttpConnectionGroupKey` 字段不变（host identity + port + scheme + affinity）。H2 池与 H1 池是**两个独立实例**，同 key 不会串（一个 h2 连接不会被 h1 池取到）。若同一进程里同一 endpoint 同时需要 h2c 与 h2-over-TLS 之外的差异（比如不同 client cert profile），仍靠 `HttpConnectionPoolAffinity` 区分，语义与 H1 一致。

---

## 4. HTTP/2 层的前置重构

池落地前，`Http2Connection` 需要四步重构。每一步单独成立（都不依赖池），合起来正好把池需要的接口暴露出来，同时删掉现有实现里三处重复与耦合。

### 4.1 stream table 改为按需扩容（消掉容量口径里的偶然项）

现状：`Http2StreamTable::init()` 在 `Http2Connection` 构造函数里 eager 分配 `next_pow2(configured_max_active_streams() * 2)` 个桶。默认 `128 + max(100, 0) = 228` → 512 桶 × 16 B = **每条连接 8 KB**，与实际并发无关。池里 200 条连接就是 1.6 MB，绝大部分是空的。

改造：

- 惰性分配（初始 8 桶 = 128 B）；`(size + 1) * 2 > bucket_count` 时 2× rehash，摊还 O(1)。
- 只在「已经长大过的表被排空」时释放桶数组（`size == 0 && bucket_count > kInitialBucketCount`），初始大小的表保留 —— 否则一问一答式的串行客户端会在每个请求上 alloc/free 一次。不做渐进收缩，避免抖动。
- 删掉 `insert()` 里的 `size_ >= max_active_streams_` 策略检查，表退化为纯容器：本地流上限由 `local_stream_attach_gate()` 把关，对端流上限由 `can_accept_peer_stream()` 把关（后者已在查 `peer_active_stream_count_ < local_max_concurrent_streams`），表这一层是重复设防。
- 删掉 `configured_max_active_streams()`，以及构造函数里那句不可失败的 `FIBER_ASSERT(streams_.init(...))`。

安全性：`close_all_streams` / `close_streams_after_goaway` 迭代的是 `owned_stream_list_` 侵入式链表，不走表；`for_each` 全仓库只有 `Http2Connection.cpp:1471` 一处（更新 send window），回调内不 insert。所以「迭代中 rehash」这个经典坑在这里不存在。

**连带收益（这条比省内存更重要）**：`available_local_stream_attach_slots()` 从 `min(协议余量, 表余量)` 塌缩成单项。原先「对端宣告 1000、实际却卡在 228」的坑消失，`max_peer_concurrent_streams` 回归为纯粹的「收到 SETTINGS 前的猜测值」，`local_max_concurrent_streams` 回归为纯粹的「我方宣告值」，两个选项各自单一职责。

### 4.2 只读访问器（public）

池要判断一条连接是否还能接新请求，必须能读到这些状态，它们现在全是 `private`/`protected`：

- `std::uint32_t peer_max_concurrent_streams() const noexcept`：`protected` 提升为 `public`。
- `std::size_t local_active_stream_count() const noexcept`。
- `std::size_t available_local_stream_slots() const noexcept`：即 4.1 之后塌缩为一个减法的 `available_local_stream_attach_slots()`。
- `bool peer_goaway_received() const noexcept`。
- `bool accepts_new_local_stream() const noexcept`：等价于 `local_stream_attach_gate() == IoErr::None`。

### 4.3 容量变化回调

```
using CapacityCallback = void (*)(void *ctx, Http2Connection &conn) noexcept;
void set_capacity_callback(CapacityCallback cb, void *ctx) noexcept;
```

触发点：

1. `on_local_stream_attach_capacity_changed()` 末尾（覆盖 stream detach、`SETTINGS_MAX_CONCURRENT_STREAMS` 变化、状态迁移）。
2. `handle_peer_goaway()` 里补一次 —— 目前 GOAWAY 路径直接调 `cancel_local_stream_attach_waiters()`，**不经过** capacity hook，池会漏掉「对端开始 draining」这个最重要的事件。
3. `local_stream_ids_exhausted_` 置位处（走 1 即可覆盖）。

回调内只做纯内存操作（重算容量、调链表、唤醒 waiter），不做 I/O、不销毁连接。

重入风险：回调 → gate 取消 waiter → 状态再变 → 又触发回调。用文件里已有的 `io_pump_running_ / io_pump_again_` 那套「dispatch 标志 + 返回后再查一次」写法处理，有现成先例。

### 4.4 attach 等待逻辑外移到 `Http2LocalStreamGate`

现状：等待逻辑占 `Http2Connection` 4 个成员（`wait_head_`/`wait_tail_`/`waiter_count_`/`granted_count_`）、6 个私有方法、170 行 `LocalStreamAttachAwaiter`，还反向耦合进 close 路径（`finish_connection()` 要等 waiter/grant 清零）和 4 条析构断言。而**等待版 API 在生产代码里只有一个调用者**：`ClientHttp2Exchange.cpp:237`。

更关键的是：**有池之后，连接内的队列不只是冗余，而是有害的**。池已经在「组内跨连接」这一层排队；连接内部再排一层 FIFO 意味着双队列、双超时，以及优先级反转 —— 池明明把请求指派给了连接 A，请求却卡在 A 的内部队列后面。池需要的只有 `try_attach` + 容量信号。

改造：把队列搬进 `Http2ClientConnection` 持有的 `Http2LocalStreamGate`（单 loop、无锁、FIFO），`Http2Connection` 只保留 `try_attach_local_stream()` 原语。`ClientHttp2Exchange` 改为持 `Http2LocalStreamGate *`（它的 `Http2Connection&` 构造器没有任何生产调用者，可直接删）。

`local_stream_attach_granted_count_` 随之删除。**注意归因**：它不是容量计数器，是防插队的公平性计数器（`Http2ConnectionTest.cpp:2655` 的 barging 用例测的就是它）。判据是 —— 预留只在有 ≥2 个互相看不见的准入点争抢同一份容量时才需要；单个漏斗内部的公平性是策略而非预留（「队列非空时新来者也排队」）。所以只有当队列和快路径归同一个 gate 所有时它才能删，光把 stream table 改成可扩容是删不掉它的。

同理，**不需要**引入 `Http2StreamSlot` 之类的预留 token：池 → gate 是串联而非竞争关系，池准入过的请求到 gate 时队列必然为空。

实现补充（已落地）：waiter 被唤醒后**仍留在队列里**直到真正 attach 成功，`try_attach` 只要看到队列非空就返回 Busy —— 这就是「公平性是策略而非预留」的具体形态，比原来「授予即出队 + 计数预留」还多一个好处：抢跑失败的 waiter 保持原位，不会像旧实现那样退到队尾。另外 gate 占用了连接唯一的 capacity 回调槽位，因此它自身再暴露一个 `set_capacity_callback` 供上层（池）串接，在 gate 处理完之后回调。

### 4.5 close 通知统一

现状三层重复：

- `Http2Connection` 里两套互斥机制 —— `closed_waiter_`（单个协程句柄）+ `on_closed_` 回调，靠 `wait_closed()` 返回 `Busy` 强制二选一，外加 `ClosedAwaiter` 及其析构补偿。
- `wait_closed()` **只支持一个 waiter**。
- `Http2ClientConnection` 因为占掉了回调槽位，只好用 `async::WaitGroup` 重新实现一遍多 waiter —— 而 `WaitGroup` 带 `std::mutex` 和 per-waiter 原子量，是给跨线程设计的，用在明确单 loop 的对象上纯属浪费；池里几百条连接就是几百个 mutex。

改造：`Http2Connection` 只保留一个 `ClosedCallback`（保留 `schedule_closed_completion()` 的延迟派发，这个是承重的：保证回调不在 I/O 栈上执行）。join 能力提到 loop-local 的 `Http2CloseGate`：

```
class Http2CloseGate {                       // 无 mutex，侵入式 waiter 链表
    void arm(Http2Connection &) noexcept;    // 注册为唯一 ClosedCallback
    Task<CloseResult> join() noexcept;       // 多 waiter
    void add_observer(ObserverHook &, cb, ctx) noexcept;   // 同步观察者，池用
};
```

派发顺序定死：**先同步 observer，再 `post_local` 唤醒协程 joiner**。这样池能在任何人有机会析构连接之前把 entry 摘链，顺手修掉当前「on_closed 回调里可以析构连接」这条尖角 —— 正是它导致池不能与 owner 共用回调。有了它就不再需要单独的 `set_closed_observer`。

细节：`start()` 失败路径会就地置 `close_completion_dispatched_` 并清掉回调，所以 `arm()` 必须能识别「连接已经关掉了」并让 `join()` 立即返回。

迁移成本：`HttpServer.cpp:455` 1 处 + `Http2ConnectionTest.cpp` 19 处（机械改动，测试加个 fixture helper 即可）。`Http3Connection` / `Http3ClientConnection` 也各有一份自己的 `wait_closed`，后续可复用同一个 helper，本次不动。

### 4.6 池侧的单连接容量口径

4.1 之后，池对一条连接的容量判断简化为：

```
capacity(entry) =
    min(options.max_streams_per_connection,                   // 池的软上限, 0 = 不限
        peer_settings_received ? peer_max_concurrent_streams  // 对端真实宣告
                               : options.pre_settings_max_streams)   // 未收到 SETTINGS 前的保守值
```

`pre_settings_max_streams` 默认给 16：`Http2Connection` 在收到对端 SETTINGS 前用 `max_peer_concurrent_streams`（默认 100）乐观猜测，若服务端实际宣告 1，池已经发出去的 lease 会造成 attach 排队。16 是「不牺牲首包并发」与「不过度超发」的折中；配 1 会退化成串行，配 100 在小 `MAX_CONCURRENT_STREAMS` 的服务端上会抖动一次。

## 5. 数据结构

### 5.1 `Http2ConnectionPoolEntry`

一个 entry 唯一对应一条 `Http2ClientConnection`，**内联持有**（placement new，与 `Http1ConnectionPoolEntry` 同风格；`Http2ClientConnection` 不可移动，必须内联 + free list 复用）。

```
class Http2ConnectionPoolEntry {
    enum class State : uint8_t { Free, Connecting, Ready, Draining, Closed };

    // 连接存储
    alignas(Http2ClientConnection) std::byte conn_storage_[sizeof(Http2ClientConnection)];
    bool has_connection_;

    // 归属与链表
    Http2ConnectionPoolGroupBucket *bucket_;      // 常驻，不随摘链清空
    common::IntrusiveListHook ready_hook_;        // 在 bucket->ready_ 上（未饱和时）
    common::IntrusiveListHook group_hook_;        // 在 bucket->all_ 上（存活期间恒定）
    common::IntrusiveListHook idle_hook_;         // 在 core->global_idle_ 上（active_==0 时）

    // 计数与状态
    std::size_t active_leases_;                   // 已发出、尚未归还的 stream 名额
    std::uint32_t capacity_cache_;                // 见 4.6，容量回调时刷新
    std::uint64_t served_streams_;                // 生命周期累计，供 max_streams_lifetime
    std::chrono::steady_clock::time_point idle_since_;
    State state_;

    event::EventLoop::DeferEntry destroy_entry_;  // 延迟销毁
    Http2ConnectionPoolEntry *next_free_;         // free list
};
```

三条链表钩子各自独立，语义正交：

- `group_hook_` / `bucket->all_`：只要 entry 存活就在链上，用于 shutdown/clear 遍历、组内连接计数。
- `ready_hook_` / `bucket->ready_`：**可分配**连接的有序表；饱和、draining、closed 时摘除。
- `idle_hook_` / `core->global_idle_`：`active_leases_ == 0` 才在链上，按 `idle_since_` 升序（push_back），front 最老 —— 与 H1 的全局 LRU 完全同构，可以共用同一套超时定时器写法。

不在 entry 里存 key 副本（`HttpConnectionGroupKey` ≈ 280+ 字节）。需要 key 时通过 `bucket_->slot_index()` 到 `bucket_index_.key_at()` 取。

### 5.2 `Http2ConnectionPoolGroupBucket`

```
class Http2ConnectionPoolGroupBucket : public HttpConnectionPoolBucketBase {
    ReadyList  ready_;          // 头部最热
    AllList    all_;            // 组内全部存活 entry
    std::size_t total_count_;   // == all_.size()
    std::size_t connecting_count_;
    WaiterList waiters_;        // FIFO 等待队列
    Http2ConnectionPoolGroupBucket *next_free_;
};
```

**bucket 生命周期规则**：只有 `total_count_ == 0 && connecting_count_ == 0 && waiters_.empty()` 时才从 `bucket_index_` 摘除并回收。饱和 entry 虽然不在 `ready_` 上，但仍在 `all_` 上、仍持有 `bucket_` 指针，所以 bucket 不会被提前回收 —— 这是 H1 里没有的约束，实现时要专门断言。

### 5.3 `Http2ConnectionPoolCore`

```
class Http2ConnectionPoolCore : NonCopyable, NonMovable {
    event::EventLoop *loop_;
    Options options_;
    HttpConnectionBucketIndex bucket_index_;
    GlobalIdleList global_idle_;                  // active_leases_ == 0 的 entry
    event::EventLoop::TimerEntry expiry_timer_;
    Http2ConnectionPoolGroupBucket *free_bucket_head_;
    Http2ConnectionPoolEntry *free_entry_head_;
    std::size_t conn_total_;                      // 全局连接数
    std::size_t idle_total_;
    bool shutdown_;
};
```

### 5.4 链表形态示意

```
bucket->ready_ :  [A]  ->  [B]  ->  [C]        (head = 最热)
                 8/10     3/10     0/10
                  ^                  ^
                  |                  +-- 长期落在尾部, active==0 -> 挂 global_idle_ -> 超时 GOAWAY
                  +-- 下一次 acquire 命中它, 变成 9/10, 10/10 后摘出 ready_

摘出后:  ready_ : [B] -> [C]     ;  A 仅在 all_ 上, state=Ready(饱和)
A 上任一请求结束: --active_leases_ (10 -> 9)  ==> 重新 push_front(ready_)
                 ready_ : [A] -> [B] -> [C]
```

---

## 6. 关键算法

### 6.1 `acquire`

```
Task<IoResult<Lease>> acquire(const HttpConnectionGroupKey &key,
                              const Connector &connector,
                              std::chrono::milliseconds timeout);
```

`Connector` 是调用方提供的拨号回调（函数指针 + ctx，无 `std::function`）：

```
struct Connector {
    async::Task<IoResult<void>> (*connect)(void *ctx, Http2ClientConnection &conn,
                                           const HttpConnectionGroupKey &key) noexcept;
    void *ctx;
};
```

拨号参数（SocketAddress、TLS options、TCP options、超时）不进池 —— 与最近几次提交（`refactor(http): move client connect parameters out of connection state`）方向一致：池只管生命周期与调度，传输 profile 由 key + 调用方闭包决定。`Connector` 只在本次 `acquire` 期间被借用。

流程（全程 `FIBER_ASSERT(loop_->in_loop())`）：

1. `shutdown_effective()` → `unexpected(Canceled)`。
2. `bucket_index_.find(key)`；没有则建 bucket（失败 `NoMem`）。
3. **快路径**：`ready_` 非空 → 取 `front()`，`try_take_slot(entry)`：
   - 重算 `capacity(entry)`；`active_leases_ < capacity && conn.accepts_new_local_stream()` → `++active_leases_`，若 `active_leases_ == 1` 从 `global_idle_` 摘链，若达到 capacity 则从 `ready_` 摘出；返回 `Lease`。
   - 若该 entry 实际不可用（GOAWAY/Draining，回调还没来得及处理）→ 摘出 `ready_` 并 `retire()`，继续看下一个。
4. **建连路径**：`ready_` 为空且
   `total_count_ < options.max_connections_per_group` 且
   `conn_total_ < options.max_connections_total` 且
   `connecting_count_ < options.max_concurrent_dials_per_group`（默认 1）
   → 分配 entry，`state = Connecting`，`++connecting_count_`，`++conn_total_`，挂 `all_`，`co_await connector.connect(...)`：
   - 成功：`state = Ready`，注册 capacity/closed 观察者，`push_front(ready_)`，`--connecting_count_`，先给**自己**取一个名额，再 `wake_waiters(bucket)` 把剩余名额分给等待者。
   - 失败：销毁 entry（回收进 free list），`--connecting_count_`、`--conn_total_`，**唤醒 waiter 让它们自己重试**（不要把错误直接广播给所有 waiter —— 一次 DNS 抖动不该让整批请求失败；但要防惊群：唤醒后由 waiter 各自走步骤 2，只有一个能进入建连路径，其余继续等，直到超时）。
5. **等待路径**：以上都不满足 → 把 `AcquireAwaiter` 挂到 `bucket->waiters_` 尾部，带 deadline 定时器挂起。醒来后从步骤 2 重跑（循环，直到成功 / 超时 `TimedOut` / 池关闭 `Canceled`）。
   `timeout == 0` 表示纯 poll，直接返回 `Busy`；`timeout == max()` 无限等。

`AcquireAwaiter` 结构与 `Http2Connection::LocalStreamAttachAwaiter` 同构（`IntrusiveListHook` + `TimerEntry` + `std::coroutine_handle<>`），可以直接照抄那套 enqueue/unlink/complete/cancel 骨架。

> **单飞拨号的取舍**：`max_concurrent_dials_per_group = 1` 时，冷启动的一批请求会串行等第一次握手，但之后所有请求复用同一条连接 —— 这正是「用更少连接发更多请求」的目标。若某些场景要更快铺开，把它调到 2~4。默认 1。

### 6.2 `release`（`Lease` 析构）

```
release_slot(entry):
  --entry.active_leases_
  ++entry.served_streams_
  if entry.state == Ready:
      if !entry.ready_hook_.linked():         # 之前因饱和被摘出
          bucket->ready_.push_front(entry)    # 回到头部
      if options.max_streams_lifetime && served_streams_ >= limit:
          retire(entry)                       # 优雅退休, 见 6.4
          return
  if entry.active_leases_ == 0:
      if entry.state == Ready:
          entry.idle_since_ = loop_->now()
          global_idle_.push_back(entry); ++idle_total_
          arm_expiry_timer_if_needed()
          maybe_evict_over_idle_cap()         # idle_total_ > max_idle_total -> 关最老的
      else:                                   # Draining / Closed
          finish_retire(entry)                # 见 6.4 / 6.5
  wake_one_waiter(bucket)
```

注意「回到头部」只发生在**饱和 → 非饱和**的跃迁上；未饱和连接上的请求结束**不重排**，避免每个请求结束都动链表、也避免热点连接在头部反复自我置换。这与需求描述「被取出的连接……放回头部」一致。

### 6.3 容量回调 `on_capacity_changed(entry)`

由 4.2 的观察者触发，纯内存操作：

1. 重算 `capacity_cache_`。
2. `conn.peer_goaway_received() || state != Running(且非 client Start) || local_stream_ids_exhausted` → `retire(entry)`（摘 `ready_`，`state = Draining`）。
3. 否则若 `active_leases_ < capacity_cache_` 且不在 `ready_` 上 → `push_front(ready_)`（对端调大 `MAX_CONCURRENT_STREAMS` 的情况）。
4. 若 `active_leases_ >= capacity_cache_` 且在 `ready_` 上 → 摘出（对端调小的情况；已发出的 lease **不回收**，只是不再新发；多出来的 attach 由 `attach_local_stream` 排队消化，靠 `send_request_header` 的 timeout 兜底）。
5. `wake_waiters(bucket)`。

### 6.4 `retire`（优雅退休）

用于：idle 超时、`max_streams_lifetime` 到期、收到 GOAWAY、`clear()`。

```
retire(entry):
  if entry.ready_hook_.linked(): bucket->ready_.erase(entry)
  if entry.idle_hook_.linked():  global_idle_.erase(entry); --idle_total_
  entry.state = Draining
  if entry.active_leases_ == 0:
      conn.graceful_shutdown()    # 发 GOAWAY, 无 stream 时会很快走到 Closed
  # 有在途请求时不主动发 GOAWAY, 等最后一个 lease 归还再发
```

`entry.state == Draining` 的连接永远不会再回 `ready_`（步骤 6.3 第 3 条要判 `state == Ready`）。

### 6.5 关闭与销毁生命周期（最容易出错的部分）

三个事实叠在一起：

- `Http2ClientConnection` 关闭可能发生在**任何时刻**，包括有在途 lease 时（对端 RST/网络断）。
- 关闭通知从连接自身的调用栈里发出，栈上还有连接对象。
- lease 归还可能发生在关闭之后（上层协程被唤醒返回错误后才析构 exchange）。

规则：

1. `closed_observer` 里：摘 `ready_`/`global_idle_`，`state = Closed`，**不销毁**。
2. 销毁条件：`state == Closed && active_leases_ == 0`。
3. 满足条件时不就地析构，而是 `loop_->post_local<...>(entry.destroy_entry_)` 延迟到下一次事件循环回合再 `destroy_connection()` + 回收 entry —— 保证不在连接自己的栈上析构连接。
4. entry 被回收时才允许把 bucket 的 `total_count_` 减一，并在归零时从 `bucket_index_` 摘除 bucket。
5. `Lease` 在连接已 Closed 时归还是完全合法路径，走 2/3。

### 6.6 idle 超时定时器

与 `Http1ConnectionPoolCore` 完全同构，可以照搬：`global_idle_` 按 `idle_since_` 升序，`arm_expiry_timer_if_needed()` 只对 `front()` 装一次 `post_at`，触发后 `evict_expired_entries(now)` 循环从 front 弹出过期的走 `retire()`，再重新 arm。区别只有一条：H2 里进 `global_idle_` 的前提是 `active_leases_ == 0`，所以「有长连流（如 gRPC streaming）的连接永不超时」是设计内行为。

---

## 7. Options

```
struct Http2ConnectionPoolCore::Options {
    // 每连接
    std::size_t   max_streams_per_connection   = 0;     // 0 = 只受对端 SETTINGS 限制
    std::uint32_t pre_settings_max_streams     = 16;    // 收到对端 SETTINGS 前的保守并发
    std::uint64_t max_streams_lifetime         = 0;     // 0 = 不限, 到期优雅退休

    // 每组 / 全局
    std::size_t   max_connections_per_group    = 4;
    std::size_t   max_connections_total        = 64;
    std::size_t   max_concurrent_dials_per_group = 1;   // 单飞
    std::size_t   max_idle_total               = 16;    // active==0 的连接上限

    std::chrono::milliseconds idle_timeout{60000};      // H2 建连贵, 比 H1 的 30s 长
    std::size_t   initial_group_capacity       = 0;

    Http2Connection::Options h2{};                       // 4.1 之后 max_peer_concurrent_streams 只影响 SETTINGS 前的猜测
};
```

`normalize_options()` 断言：`idle_timeout > 0`、`max_connections_per_group >= 1`、`max_concurrent_dials_per_group >= 1`、`max_idle_total <= max_connections_total`。

---

## 8. 对外 API

### 8.1 `Http2ConnectionPoolCore::Lease`

```
class Lease : NonCopyable {           // movable
    bool valid() const noexcept;
    Http2ClientConnection &connection() noexcept;
    const HttpConnectionGroupKey &key() const noexcept;
    ClientHttp2Exchange open_exchange(mem::BufPool &pool) noexcept;   // 便捷透传
    void reset() noexcept;            // 归还名额
    ~Lease();                         // reset()
};
```

### 8.2 `Http2PooledExchange`（建议提供）

`ClientHttp2Exchange` 本身不知道池的存在，如果调用方忘了在响应读完后析构 `Lease`，名额会一直泄漏（这是 H2 池最容易出的运行期 bug，而且表现为「并发上不去」而不是崩溃）。因此提供一个组合体：

```
class Http2PooledExchange {           // movable, 持有 Lease + ClientHttp2Exchange
    ClientHttp2Exchange &exchange() noexcept;
    ClientHttp2Exchange *operator->() noexcept;
    ~Http2PooledExchange();           // 先析构 exchange 再归还 lease
};
```

析构顺序必须是 exchange 先于 lease（成员声明顺序保证：`Lease lease_;` 在前，`ClientHttp2Exchange ex_;` 在后）。上层（lite_nginx upstream）统一用它。

### 8.3 `LocalHttp2ConnectionPoolSet`

与 `LocalHttp1ConnectionPoolSet` 一模一样的 facade：为 `EventLoopGroup` 每个 loop 内联构造一个 core，按 `EventLoop::current()` 路由，`init()/clear_async()/shutdown_async()` 复用同一套 `AdminAwaiter` 写法。因为不做 steal，这层没有额外逻辑。

### 8.4 观测回调

```
using ConnCountChangedCallback = void (*)(void *ctx, const HttpConnectionGroupKey &key,
                                          std::size_t total, std::size_t ready) noexcept;
```

对齐 H1 的 `set_idle_count_changed_callback`，供 lite_nginx 上报 metric。

---

## 9. 错误语义

| 场景 | `acquire` 返回 |
|---|---|
| 池已 shutdown | `Canceled` |
| `timeout == 0` 且无可用名额 | `Busy` |
| 等待超时 | `TimedOut` |
| 建连失败且无其它可用连接、等待超时 | `TimedOut`（最后一次拨号错误可通过回调/日志暴露，不覆盖等待语义） |
| bucket/entry 分配失败 | `NoMem` |
| 拿到 lease 后连接被对端关闭 | `acquire` 成功；错误由 `ClientHttp2Exchange` 的读写返回 |

约定：**池不做请求重试**。lease 拿到后连接立即挂掉，由上层决定是否重新 `acquire`。

---

## 10. 测试计划（`tests/Http2ConnectionPoolTest.cpp`）

用真实 loopback `Http2Connection` 服务端（参考现有 `tests/Http2ClientConnectionTest.cpp` 的搭法），不 mock：

1. **复用**：同 key 连发 N 个并发请求，服务端 `MAX_CONCURRENT_STREAMS = 100` → 只 accept 1 条连接。
2. **饱和摘链**：`MAX_CONCURRENT_STREAMS = 2`，发 3 个并发 → 第 3 个等待；结束一个后第 3 个立刻被唤醒且复用同一条连接。
3. **回到头部**：`max_streams_per_connection = 1`，`max_connections_per_group = 3`，制造 A/B/C 三条；结束 A 上的请求后，下一次 `acquire` 必须命中 A（校验 head 语义）。
4. **组上限 + 等待队列 FIFO**：`max_connections_per_group = 1`，并发 5，校验唤醒顺序与全部成功。
5. **超时**：`acquire(timeout=50ms)` 在名额耗尽时返回 `TimedOut`，且 waiter 已从链表摘除（无悬挂）。
6. **idle 超时关闭**：所有请求结束 → 推进 loop 时间 → 连接被 GOAWAY 关闭、`conn_total_` 归零、bucket 从 index 摘除。
7. **对端 GOAWAY**：服务端发 GOAWAY → 池不再把该连接分配出去；在途请求正常收尾；下一次 `acquire` 新建连接。
8. **SETTINGS 收缩/放大**：服务端后发 `MAX_CONCURRENT_STREAMS = 1`（原 100）→ 校验 `ready_` 摘链、不再超发；再放大 → 校验重新入链并唤醒 waiter。
9. **连接中途断开 + lease 未归还**：kill 服务端 → observer 触发但 entry 不被销毁；lease 归还后延迟销毁执行，无 UAF（配合 ASan 跑）。
10. **建连失败**：connector 返回错误 → waiter 不被整批打死，能重试；全部超时后干净退出。
11. **`clear()` / `shutdown()`**：有在途 lease 时调用 → 不崩，lease 归还后连接被清理，`~Http2ConnectionPoolCore` 断言全空。
12. **`Http2ConnectionPoolEntry` 与 bucket 的 free list 复用**：反复建/毁 100 轮，`new` 次数有界。

另加 `tests/HttpConnectionBucketIndexTest.cpp`（现有测试改名 + 基类化后的类型断言）。

---

## 11. 实施阶段与文件清单

前四步是 §4 的 HTTP/2 层前置重构，与池解耦，可独立验证：

| 阶段 | 提交 | 内容 |
|---|---|---|
| 1 | `refactor(http): make http2 stream table grow on demand` | §4.1：表惰性分配 + 2× 扩容 + 排空释放；删 `configured_max_active_streams()` 与表层策略检查；`available_local_stream_attach_slots()` 塌缩成单项 |
| 2 | `feat(http): add http2 connection capacity callback` | §4.2 访问器 + §4.3 单一容量回调（含 `handle_peer_goaway()` 补调用） |
| 3 | `refactor(http): move local stream attach waiting into Http2LocalStreamGate` | §4.4：搬队列，删 `granted_count_` 及其全部分支（`finish_connection` 关闭条件、4 条析构断言、`consume_grant` 参数）；barging 用例改打 gate |
| 4 | `refactor(http): unify http2 close notification` | §4.5：`Http2CloseGate`；`Http2ClientConnection` 去掉 `WaitGroup`；迁移 `HttpServer` + 19 处测试 |
| 5 | `refactor(http): rename http1 connection group key to http-generic` | §3.1 全量重命名 + `HttpConnectionPoolBucketBase` 抽取 + `HttpConnectionBucketIndex` 泛化；行为零变化 |
| 6 | `feat(http): add single-loop http2 client connection pool` | `Http2ConnectionPoolEntry`/`GroupBucket`/`Core` + `Lease` + `AcquireAwaiter` |
| 7 | `feat(http): add pooled http2 exchange and per-loop pool set` | `Http2PooledExchange` + `LocalHttp2ConnectionPoolSet` |
| 8 | `test(http): cover http2 connection pool` | §10 |
| 9 | `docs: http2 connection pool` | `docs/http2-connection-pool.md`（对齐 `docs/http1-connection-pool.md` 结构） |

新增文件：

```
include/fiber/http/Http2LocalStreamGate.h          # 阶段 3
include/fiber/http/Http2CloseGate.h                # 阶段 4
include/fiber/http/HttpConnectionPoolBucketBase.h  # 阶段 5
include/fiber/http/Http2ConnectionPoolEntry.h
include/fiber/http/Http2ConnectionPoolCore.h
include/fiber/http/Http2PooledExchange.h
include/fiber/http/LocalHttp2ConnectionPoolSet.h
src/http/Http2LocalStreamGate.cpp
src/http/Http2CloseGate.cpp
src/http/Http2ConnectionPoolEntry.cpp
src/http/Http2ConnectionPoolCore.cpp
src/http/Http2PooledExchange.cpp
src/http/LocalHttp2ConnectionPoolSet.cpp
tests/Http2LocalStreamGateTest.cpp
tests/Http2ConnectionPoolTest.cpp
docs/http2-connection-pool.md
```

## 12. 风险点与取舍

1. **lease 泄漏 = 并发塌陷**。最大的运行期风险，且表现为「并发上不去」而不是崩溃，很难排查。缓解：`Http2PooledExchange` 用类型强制 lease 与 exchange 配对 + Debug 下 `~Http2ConnectionPoolCore` 断言 `active_leases_ == 0`。
2. **lease 数 ≠ 已 attach stream 数，但偏移方向恒定是保守的**。lease 在 `acquire` 时发放，stream 在首次 `send_request_header` 时才 attach，stream close 之后上层析构 exchange 才归还 lease —— 任何时刻「池计数 ≥ 实际 attach 数」。所以池永远不会超过对端限制，只会略微低估可用容量。**不需要**为此引入预留 token（见 §4.4）：预留只能防跨准入点的竞争，而池 → gate 是串联关系；它并不能消除这个偏移。
3. **单飞拨号 vs 冷启动延迟**。`max_concurrent_dials_per_group = 1` 有利于连接数最小化，但冷启动时一批请求会串行等一次握手。给出旋钮，默认 1，文档写明理由。
4. **头部连接的公平性**。head 优先会让第一条连接长期承担绝大部分流量。若服务端做每连接限速，吞吐可能不如轮询。这是需求明确要求的策略；日后如需，可在 `Options` 加 `SelectionPolicy { HeadFirst, LeastLoaded }`，`LeastLoaded` 用 `common/BinaryHeap.h` 按 `active_leases_` 选。本期只实现 HeadFirst。
5. **前置重构的重入与生命周期**。§4.3 的容量回调、§4.5 的 close 派发顺序是两处真正容易写错的地方：前者要防「回调 → 取消 waiter → 再触发回调」的递归，后者要保证同步 observer 先于协程 joiner，否则池还没摘链连接就被析构了。两处都要有针对性用例。
6. **不实现跨 loop steal**。H2 连接与 `EventLoop` 强绑定（transport、timer、outbound 调度全在 loop 上），跨 loop 借用需要迁移整条连接或跨线程提交 stream，代价远高于 H1 的 idle 连接搬运。单线程假设下不做。
7. **QUIC 侧有一份形状相同的 `LocalStreamAttachAwaiter`**。本次只动 H2，会有短期不对称。QUIC 暂无池需求，不建议一起改。
