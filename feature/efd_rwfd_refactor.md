# Efd / RWFd 单线程归属与就绪转换订阅重构

日期：2026-09-15。状态：设计已确认，待实施。

源码核对基线：`0c56be5`。本文记录本次讨论的最终方案，不表示代码、调用方迁移或验证已经完成。接口示意用于固定职责和前置条件，具体命名可以在实施时沿用仓库习惯。

本文接替 [持久 epoll ET 设计](epoll_et_design.md) 中有关 Efd / RWFd、跨线程 I/O、readiness 与 callback 的设计。旧文档的实施及压测记录仍属于原版本，不能作为本次重构的验证证据。

## 1. 最终决策

1. 一个 fd 在任意时刻只由一个 current event loop 操作。Efd 保存固定的 `owner_loop` 引用、可变的 `current_loop` 指针，以及唯一一份 poller 注册记录。
2. Poller 的 ADD / MOD / DEL 在所属 event loop 线程调用。借用线程不操作原 loop 的 poller。
3. current loop 变化时，旧 loop 先 DEL，再通过消息发布对象；接收方接管 current loop。接收动作不 ADD，首次需要监听时才注册。
4. 连接池优先复用本地连接。远程借用和归还执行相同的交接协议，不在 owner poller 保留暂停注册，不维护多个注册对象。
5. RWFd 每个方向保存 `Event { callback, ctx, state }`，state 只有 `Unknown / Ready / Blocked`。EOF、半关闭、具体 I/O 错误与 stream 终止语义由 StreamFd 管理。
6. RWFd 的 `read/write(lambda)` 断言 current loop，执行 lambda 并记录 I/O 结果；不确保注册，不根据 readiness 跳过 lambda，不隐式等待。
7. 调用方根据业务需要决定是否等待。set callback 要求该方向非 Ready 且没有现存订阅，确保对应方向已监听，然后等待非 Ready 到 Ready 的转换。
8. clear callback 只清除匹配的 callback/ctx，不改变 readiness，不修改内核 interest。已开启的方向继续监听，即使没有 callback 也更新状态。
9. 普通就绪 callback 只在 `Unknown/Blocked → Ready` 时通知。`Ready → Ready` 不重复通知；set callback 不内联通知，也不安排 Ready 的 defer 补发。
10. RWFd awaiter 完全在 current loop 中等待、超时、取消和恢复。删除通用跨线程 waiter、sticky external I/O 模式和 `poll(fd, 0)` 兼容探测。
11. callback 订阅与内核注册分离，但回调销毁、替换订阅、同批事件失效、交接失败与 loop 停止仍必须有完整处理。

## 2. 背景与当前源码差异

当前远程借用允许业务在 borrower loop 执行 syscall，同时 fd 仍在 home loop 的 poller 中。这使实际 I/O 消耗与事件通知分属两个线程。

基线实现并不是让两个线程直接写同一份 readiness：非 owner I/O 将 `external_io_` 置为 true，非 owner 的 `finish_io()` 不更新状态，等待通过 `poll(0)` 重新确认就绪。代价是借用后长期进入保守模式，同时保留跨线程 waiter 分配、消息传递和取消状态机。

| 基线位置 | 当前行为 | 本次变化 |
|---|---|---|
| [Efd.h](../include/fiber/net/detail/Efd.h)、[Efd.cpp](../src/net/detail/Efd.cpp) | 唯一固定 loop，单个 Item | 固定 owner、可交接 current，继续使用单个 Item |
| [RWFd.h](../include/fiber/net/detail/RWFd.h)、[RWFd.cpp](../src/net/detail/RWFd.cpp) | prepare I/O 确保注册，并可能跳过 syscall | lambda 始终执行，注册由显式监听触发 |
| 同上 | setter 对 Ready 排队补发；按内核提示派发 | setter 要求非 Ready；仅状态转换通知 |
| 同上 | terminal/read_hangup 与跨线程 waiter | stream 状态移至 StreamFd；awaiter 本地化 |
| [StreamFd.cpp](../src/net/detail/StreamFd.cpp)、[DatagramFd.cpp](../src/net/detail/DatagramFd.cpp) | prepare/finish 成对更新 | 用模板包装统一执行与状态反馈边界 |
| [StealableHttp1ConnectionPoolSet.cpp](../src/http/StealableHttp1ConnectionPoolSet.cpp) | 摘取 idle entry 后直接传给 caller，fd 留在 home poller | 摘取后先解除 I/O 归属，caller 接收后才使用 |
| [Poller.cpp](../src/event/Poller.cpp)、[EventLoop.cpp](../src/event/EventLoop.cpp) | data.ptr 派发，DEL 清空当前批次匹配指针 | 保留批次安全机制，补齐交接与线程归属约束 |

旧 ET 文档包含 token 注册表等历史描述，当前基线已经使用 `data.ptr + invalidate_batch()`。本次以当前代码为起点，不要求重新引入全局 token 表。

## 3. 分层职责与最小状态

### 3.1 Poller / EventLoop

- Poller 管理 ADD、MOD、DEL 和当前事件批次失效；EventLoop 负责线程归属和事件派发。
- ADD/MOD/DEL 必须在该 poller 所属 loop 中执行，不能只断言“当前存在某个 event loop”。
- DEL 后旧批次不能再解引用此 Item；即使 fd 数值复用或 Item 在新 loop 注册，也不能向新对象或新归属投递旧事件。
- ADD/MOD 失败不得发布不存在的内核注册或 interest；DEL 失败需同时处理用户态身份失效与内核清理，不能据此宣布成功交接。
- 保留现有 LT/ET 通用能力，网络 RWFd 使用 ET。eventfd、timerfd、listener 和 connect attempt 的模式按其自身职责处理。
- 审计 EventLoop 构造中的 wakeup fd 注册、timerfd 后端初始化及直接使用 Poller 的测试：若严格线程断言与构造阶段冲突，应调整注册时机或初始化归属，而不是给稳态跨线程调用留后门。

### 3.2 Efd

逻辑数据模型：

```cpp
event::EventLoop &owner_loop_;
event::EventLoop *current_loop_;
Item item_;
```

- owner 固定，current 初始等于 owner；活动期间 current 非空。
- owner 用于 home 归属与生命周期协调，current 用于 I/O、注册、回调和等待。
- 明确提供 `owner_loop()` 与 `current_loop()`；如保留底层 `loop()`，其含义统一为 current，迁移调用方，避免同名接口混淆归属。
- 上层 pool/home core 的 loop 仍表示固定归属，不能把所有 `loop()` 调用机械替换成 current；Http1ClientConnection 的固定归属与 active exchange loop 也需要分别核对。
- 已注册时 Item 只能属于 current poller；交接中的对象不在任何 poller 注册。
- 原 `set_owner(void*, callback)` 中的 owner 实为回调上下文，实施时应避免与 owner loop 混淆。
- 不引入按线程分配的注册数组、borrower 注册对象或原子 current 指针。跨线程发布由上层交接消息提供同步，旧线程发布后停止访问对象。
- 交接中禁止业务 I/O。可以由上层移交对象表示这个阶段，不必为所有 fd 增加一套重复的原子状态机。

### 3.3 RWFd

```cpp
enum class State : std::uint8_t { Unknown, Ready, Blocked };

struct Event {
    ReadyCallback callback = nullptr;
    void *ctx = nullptr;
    State state = State::Unknown;
};
```

- 分别保存 read Event 和 write Event，不保存 EOF、连接错误或 Error readiness。
- callback 使用函数指针和 ctx；I/O 包装使用模板 lambda，不引入 `std::function` 或每次等待的堆分配。
- 对外暴露只读的方向 readiness 查询；状态修改通过受约束的 I/O 反馈接口完成，不让调用方任意改写 callback slot。
- 订阅代次、派发销毁保护属于回调身份与生命周期机制，可以与 Event 聚合，也可独立保存；不能为追求只有三个字段而移除它们的安全语义。
- raw fd 使用者也必须在 current loop 中通过包装器或明确的结果反馈路径维护状态。不保留绕过状态维护后用 `poll(0)` 修补的机制。

### 3.4 StreamFd / DatagramFd / TlsStreamFd

- StreamFd 管理流式 syscall、EOF、peer write-side hangup、具体错误、terminal callback 和相关等待的完成语义。
- DatagramFd 按报文语义解释结果；短包和零长度包不代表 EOF，某次 UDP 错误也不能直接套用 TCP 永久终止模型。
- TlsStreamFd 继续负责 TLS 缓冲、协议错误、close_notify、WANT_READ/WANT_WRITE 与重试参数；底层 socket 状态由其 StreamFd 管理。
- TLS BIO 必须继续调用 StreamFd 的统一 I/O 路径，不能直接绕过它操作原始 fd。

## 4. read/write(lambda) 契约

read/write 是同步、不可挂起的执行包装器。它们断言 `current_loop().in_loop()`，对符合参数契约的调用执行一次 lambda，不因 Unknown、Ready 或 Blocked 而跳过，也不触发 ADD/MOD。

示意调用：

```cpp
return rwfd.read([&](IoStateUpdate &state) noexcept {
    auto result = read_once(buffer, size);
    // StreamFd 在这里解释成功、耗尽、EOF 和错误。
    // 成功但未确认耗尽：state.mark_ready();
    // WouldBlock 或确认耗尽：state.mark_blocked();
    return result;
});
```

`IoStateUpdate` 只是接口示意，应当是可内联、无分配的状态访问，不含 callback 操作。结果分类可以用等价的轻量返回结构完成；关键是 RWFd 不自行猜测不同 fd 的耗尽条件。

lambda 的状态反馈不调用普通就绪 callback，也不排队通知。执行本次 I/O 的调用方已经获得结果并负责继续推进；普通 callback 的派发入口是后续内核就绪事件。

| 本次操作结果 | 状态与处理 |
|---|---|
| 请求 100 字节，实际读到 100 字节 | Ready：尚未确认耗尽，不保证下一次还能读到数据 |
| 请求 100 字节，读到 10 字节，满足流式耗尽条件 | Blocked |
| EAGAIN / WouldBlock | Blocked |
| 成功写入，尚未确认写空间耗尽 | Ready |
| 满足受约束的短写耗尽条件 | 允许具体 I/O 层标记 Blocked，必须有覆盖该 syscall 的验证 |
| EINTR | 具体 I/O 层重试，不作为耗尽 |
| 参数错误或零长度请求 | 在适配层返回，不污染既有 readiness，不把零长度请求当 EOF |
| stream 非零长度 read 返回 0 | StreamFd 记录 EOF；禁止当作普通短读后等待新边沿 |
| 不可恢复 I/O 错误 | StreamFd 记录并处理，不往 Event 增加 Error 状态 |

短读/短写策略必须基于实际底层 syscall 的有效请求长度，排除人为分片、内核单次长度上限、特殊 flags 等不能证明耗尽的情况。UDP、TLS 明文长度不用于判断底层 fd 耗尽。对于无法证明的部分成功，保持 Ready 并允许再尝试到 EAGAIN。

已知 EOF、半关闭或错误时，StreamFd 必须先按这些状态决定读写/等待行为，不能被一个 Blocked readiness 挡住而永久等候。因 EOF/error 返回前，包装器与通知路径不得重入销毁尚在执行的 syscall/SSL 栈。

## 5. 注册、订阅与三态转换

### 5.1 延迟注册与持久 interest

- 构造、attach、接收交接、read/write 都不注册。
- 首次 read callback 订阅确保读方向监听；首次 write callback 订阅确保写方向监听。
- 尚未注册时 ADD；已经注册但缺少该方向时 MOD 扩展 interest；已包含时不调用 epoll_ctl。
- current loop 的一次归属期内，已经开启的读写方向持续监听。clear callback 不缩减 interest，不 DEL，也不执行 MOD 重新武装。
- StreamFd 的显式状态观察需求可以要求监听 RDHUP/terminal；它与普通读写 callback 独立。注册 mask 应由明确的监听需求生成，不由“当前 callback 是否为空”反推关闭。
- 交接、close 或显式 release 才解除注册。交接后新 loop 不继承旧 poller 的 registered/interest 值，首次需要监听时重新 ADD。

因此，一个只做立即完成 I/O、从未订阅或请求状态观察的 fd，可以在整个使用期间都不进入 poller。

### 5.2 set callback

前置条件：

```cpp
FIBER_ASSERT(current_loop().in_loop());
FIBER_ASSERT(event.callback == nullptr);
FIBER_ASSERT(event.state != State::Ready);
```

此外检查 fd、callback 参数及 loop 生命周期是否允许安装。检查 readiness、确保监听和发布 callback 必须在同一次 loop 执行中完成，不能在中间挂起或调用任意业务代码。

ADD/MOD 失败返回错误，不留下一个永远无法收到事件的已成功订阅假象。成功安装不内联调用 callback，也不提交 deferred-ready。普通订阅是持续订阅，除非调用方 clear 或 fd 关闭，不因第一次通知自动删除。

### 5.3 clear callback

- 只删除 callback 与 ctx 都匹配的订阅；不匹配的 clear 不影响新订阅。
- 更新订阅身份，保证同一次派发中尚未调用的旧订阅失效。
- 保留方向 readiness，保留已开启的内核监听。
- 不触发取消通知；显式清除表示调用方已经接管该订阅的结束。close 导致的取消单独处理。

### 5.4 内核就绪派发

| 旧状态 | 对应就绪提示后的状态 | 普通 callback |
|---|---|---|
| Unknown | Ready | 已订阅则通知 |
| Blocked | Ready | 已订阅则通知 |
| Ready | Ready | 不重复通知 |

没有 callback 时也执行状态转换。这里的转换指用户态 Event 的转换，不要求每次内核提示严格对应物理不可读到可读的变化。

核心语义示意：

```cpp
const auto previous = event.state;
event.state = State::Ready;
if (previous != State::Ready && event.callback != nullptr) {
    // 在订阅身份和对象存活检查下通知。
}
```

必须先更新 Ready，再进入 callback。callback 可以立即执行 I/O 并改回 Blocked；回调返回后不能再次写 Ready 覆盖其结果。

### 5.5 调用方承担继续执行责任

- Ready 时直接尝试 I/O；不能安装 callback 等待“再次 Ready”。
- Unknown/Blocked 且业务需要等待时才订阅。
- 预算用完而仍为 Ready 时，调用方通过现有有界 continuation 继续，不能依赖新的 epoll 边沿。
- 背压暂停时可以 clear；恢复后先检查状态或尝试 I/O，Ready 则推进，Unknown/Blocked 才订阅。
- callback 仍在安装状态时可继续使用；如果需要更换订阅，先 clear，再满足新订阅的前置条件。

典型时序：

```text
读空 → Blocked → clear callback
数据到达 → epoll 派发 → Ready（没有 callback，不通知）
业务恢复 → 看见 Ready → 直接读取
```

```text
读空 → Blocked → 安装 callback
数据到达 → Blocked → Ready → callback
callback 继续读空 → Blocked
下一次数据到达 → 再次通知同一个持续订阅
```

## 6. 本地 awaiter 与终止状态

### 6.1 RWFd awaiter

1. 断言 current loop；拒绝同方向并发订阅。
2. 按既有超时约定处理已到期等待，保持兼容或在迁移中明确记录差异。
3. Ready 时不安装 callback、不注册、不挂起。
4. Unknown/Blocked 时安装 callback，成功后在 current loop 设置 timeout 并挂起。
5. 就绪完成时先 clear 自己的订阅、取消 timer，再恢复协程。
6. timeout 或 awaiter 析构在同一个 loop 清除自己的订阅；不更改 fd 的 readiness 或 interest。
7. close 先拆除全部等待关系，再完成取消通知，保证每个 waiter 最多结束一次。

删除 `RWFdCrossThreadWaiter`、`RWFdWaiterState`、跨线程 Notify/Cancel/Resume 项及其独立堆分配。调用者必须先交接 fd，不能继续直接在非 current loop 上等待。

### 6.2 StreamFd 状态与等待包装

StreamFd 不再简单把 terminal/peer_closed 查询转发给 RWFd。它保存必要的流状态，并在 public read/write/wait 路径中解释它们。

- EOF、RDHUP、HUP、具体错误的语义不能混为一个布尔值。RDHUP/HUP 后可能还有未读数据，读取应先返回数据，再返回 EOF；读侧结束不自动禁止写侧继续。
- RWFd 向适配层传递必要的内核事件信息，但不把它们缓存为连接终止状态。若现有 `IoEvent::Terminal` 合并 ERR/HUP 不足以表达处理差异，应在事件转换层保留所需区别。
- StreamFd 先更新可观察的流状态，再完成业务通知。终止通知独立于普通 `Unknown/Blocked → Ready` 条件，不能因为 Event 已是 Ready 而漏掉错误或关闭。
- 已知 EOF/error 时，上层等待立即得到对应的可读结束/错误结果，不安装一个等待新边沿的普通 callback。当前 StreamFd 直接别名 RWFd awaiter 的方式需要相应调整。
- terminal 迟订阅继续保证能够观察已发生的终止，并保持 setter 不内联调用业务回调。若需排队，由 StreamFd 自己管理；这不恢复 RWFd 的 Ready 补发机制。
- syscall/BIO 栈内的错误反馈先记录状态；任何可能销毁 SSL/stream 的业务完成都在安全边界进行，不能在 BIO 调用未退出时释放 SSL。
- timeout、单个 waiter 取消、参数错误不自动使 stream 永久终止。

## 7. 跨线程交接协议

### 7.1 发起方的前置条件

- 当前代码运行在旧 current loop。
- 上层拥有对象的独占使用权；HTTP/1 entry 已从 idle 容器摘除，或远程 lease 已完成/取消业务 I/O。
- 无活跃 read/write awaiter，无业务 callback、timer、pump 或 deferred completion 仍会访问该连接。
- 当前对象的事件派发栈已经退出。若从回调中发起归还，应在旧 loop 排队到安全边界执行实际交接，不允许发布后让旧栈继续读写状态。
- TLS 无未完成的 SSL 调用或握手；协议对象的停止/取消工作已收束。
- 交接消息与对象生命周期有明确的持有关系，目标 loop 和 home pool 在消息完成前保持有效。

### 7.2 旧 loop 解除

1. 清理上层业务订阅和本地继续任务，停止其后续执行。
2. 已注册则在旧 poller DEL，清理当前批次中的旧 Item 指针；未注册则跳过。
3. 撤下旧 loop 的 stop hook 及其他本地生命周期挂钩。任何失败都由旧归属继续处理，不能直接发布为可用对象。
4. 更新注册身份，清空注册状态与 interest。旧事件不能跨越此边界。
5. 通过连接池的移交消息发布对象。发布后旧线程不再访问其可变状态，包括 current 指针、StreamFd 状态和 callback。

该过程不关闭或重新创建 fd，不通过 raw `release_fd()` 重建整个连接，TLS 与协议缓冲随原对象保留。

解除阶段失败时，移交函数返回错误且不发布消息，对象仍属于旧 loop。连接池将该连接标记为不可复用，并在旧 loop 完成残留注册与 fd 的清理；确认旧 poller 不再持有可派发引用前，不能销毁或把同一 Item 用于新注册。成功交接路径和失败回收路径必须在接口结果上明确区分。

### 7.3 目标 loop 接收

1. 在消息处理函数中确认当前线程是目标 loop，接管独占使用权。
2. 将 current 设置为本 loop；读写 readiness 重置为 Unknown。
3. 保留 StreamFd 的 EOF、半关闭、错误和 TLS/HTTP 状态。
4. 接管 stop hook 和对象生命周期。延迟 ADD 不等于延迟生命周期接管。
5. 不执行 ADD。之后 read/write 可以直接执行 syscall，首次订阅或显式状态观察才确保注册。

接收动作不能仅靠指针赋值宣称成功；消息队列需要提供发布/接收的同步关系。此保证与单线程独占使 readiness、current 等字段无需原子化。

## 8. Stealable connection pool 集成

### 8.1 借出

- 保留本地优先策略：本地 hit 不改变 current，不做 DEL/ADD。
- 远程候选在 home loop 摘出后，检查可复用性与已观察的 peer_closed，再执行解除流程。
- borrower 收到后先接收归属，再完成 acquire awaiter，业务才可获得 lease。
- 即使借用期间立即完成全部 I/O，仍无需注册 borrower poller；只有实际监听需求才 ADD。

### 8.2 归还

- borrower 首先结束或取消 exchange，确认 reader/writer、transport callbacks 与本地继续任务均已解除。
- borrower 已注册则 DEL，再向 home loop 发布归还消息；不能只把 entry 放进归还队列而保留 borrower 注册。
- home 接收归属后，按 shutdown 状态与连接可复用性决定回池或销毁。
- 回池需要及时观察 idle peer close 时，由 pool/StreamFd 显式恢复状态监听，必要时触发 ADD；不是交接函数无条件注册。
- 即使 idle 监听仍保留 read Ready，也不能用普通 read callback 的重复通知来检测 terminal；独立流状态路径承担这项工作。

### 8.3 取消、失败与 shutdown

| 阶段 | 处理要求 |
|---|---|
| home 尚未发布，acquire 被取消 | 对象仍由 home 处理；已解除则在 home 重新接管，按需回池 |
| 已发布、borrower 尚未交付 lease | 接收消息仍须被处理；接管后结束取消并安全归还，不能遗弃 in-flight 对象 |
| 借用中任务被销毁 | 先本地取消叶子 I/O，再解除并归还；不能留下跨线程 waiter 等待后续清理 |
| 目标正在停止 | 不交付新 lease；由仍受保障的消息/清理流程完成接收和回收 |
| DEL 失败 | 禁止成功移交；统一处理残留注册与 fd，保证未来批次不会访问已释放对象 |
| 延迟 ADD 失败 | 将错误交给本次订阅/等待调用方，清理失败订阅；由当前归属处理连接，不伪造成功等待 |
| home pool 已 shutdown | 在 home 完成接收后回收，不重新加入 idle 容器 |
| 借用时 owner loop 停止 | owner 不能直接关闭 borrower 正在使用的 fd；停机流程必须协调归还/终止 |

当前 `shutdown_async()` 等待 `active_acquire_wg_` 后清理 shard，不能仅凭这一计数宣称所有借出 lease 和在途归还均已结束。实施时需追踪这些生命周期，复用或补足等待机制；正常关闭顺序必须保证归还消息处理完后才销毁 pool 或停止所需 loops。

## 9. 派发与生命周期安全

1. **无 callback 也更新状态**：callback 清除只表示业务不再关注，不能停止内核提示记录。
2. **先状态、后通知**：同次事件中先计算各方向的转换并记录订阅身份，再执行可能重入的 callback；不能在读回调之后无条件覆写写状态。
3. **订阅身份稳定**：读回调清除/替换写回调时，同次旧派发不能通知新订阅；相同 callback/ctx 重装也按新身份处理。
4. **允许回调销毁**：保留 DispatchGuard 或等价机制，销毁后不读 epoch/current/state；guard 析构也不能再访问已销毁对象。
5. **批次失效**：DEL 清理旧 loop 当前批次；交接要等旧派发栈退出，不能认为 DEL 自动终结正在运行的回调。
6. **close 顺序**：解除注册、本地挂钩、订阅和 timer 后，再完成保存好的通知；后续通知不能通过已经销毁的 RWFd 访问成员。
7. **注册失败**：Poller/Efd 对 registered 与内核结果的描述必须一致。基线 DEL 在 syscall 前清除 Item 注册标志，失败路径尤其需要审计；不能把该标志当作内核已移除的证明。
8. **未启动对象清理**：保留未注册 fd 在初始化失败时由构造方清理的能力；这种路径没有 poller 操作，不应因新增 loop 断言破坏 UDP/QUIC 启动回滚。

## 10. 调用方迁移范围

这是 callback 语义与线程契约的变更，不能只修改 Efd/RWFd 后把失败测试删掉。

| 范围 | 必须完成的迁移 |
|---|---|
| StreamFd、DatagramFd 及 scalar/vector/batch I/O | 统一 lambda/状态反馈；所有实际 syscall 都覆盖；移除 prepare/finish 分裂契约 |
| TcpStream、UnixStream、TlsTcpStream、TlsStreamFd | 暴露必要的状态查询和交接入口，明确 loop 含义；保留协议错误与 TLS 缓冲语义 |
| HttpTransport 及测试替身 | readiness 可查询；普通订阅遵守非 Ready 前置条件；terminal 状态转发到新层次 |
| Http1ClientConnection、Http1ExchangeIo、HTTP/1 服务端等待 | 正确处理 Ready、EOF 与 terminal；等待提示不作为真实数据到达，不提前启动 header timeout |
| Http2Connection 及入站/出站 pump | 迁移 `sync_transport_callbacks()`；Ready 时推进/续跑，不能无条件 set 等待补发；保持公平性和背压 |
| QUIC UDP endpoint、DNS/UDP 消费者 | 预算停止自行续跑；短包不判耗尽；暂停恢复先检查状态或尝试 I/O |
| StealableHttp1ConnectionPoolSet、Http1ConnectionPoolCore/Entry | 借出/归还归属交接、取消回收、idle 状态观察、shutdown 生命周期 |
| AcceptFd、ConnectFd、HappyEyeballsConnectFd | 审计共用 Efd 的线程和生命周期接口；保留各自监听策略，不强行套用 RWFd 的订阅契约 |
| tests、example、apps、直接 raw fd 使用者 | 移除依赖跨线程 waiter、Ready defer 和 syscall suppression 的假设；先交接再跨 loop 使用 |

特别检查：

- [Http2Connection.cpp](../src/http/Http2Connection.cpp) 的 `sync_transport_callbacks()` 当前按 wait_event 安装 callback，需同时考虑底层方向 readiness。
- [Http1ExchangeIo.cpp](../src/http/Http1ExchangeIo.cpp) 的 terminal callback 负责 response channel 关闭，不能因终止状态上移而失效。
- [Http1ConnectionPoolEntry.cpp](../src/http/Http1ConnectionPoolEntry.cpp) 当前归还消息直接调用 home accept；接收归属必须在回池/销毁前完成。
- TLS 的 `SSL_pending` / WANT_READ / WANT_WRITE 与 fd readiness 不等价；不能把“底层 Ready”当成“TLS 本次操作必定完成”，也不能因 TLS WANT 就任意标记 fd Blocked。

公共头继续只依赖 `include/fiber` 可用头，不引用 `src`。新的内部回调采用 `noexcept`，不引入异常。

## 11. 实施顺序

1. **Poller/Efd 归属与交接基础**：单个 Item、owner/current、线程断言、DEL 批次失效、失败清理与 loop 初始化适配。
2. **三态与订阅契约**：Event 聚合、lambda 包装、显式监听、无 callback 更新、转换通知；删除 Ready 补发和 syscall suppression。
3. **流状态迁移**：StreamFd 终止/EOF/半关闭、状态事件接口、TLS BIO 安全边界与上层等待包装。
4. **borrow/return 集成与 waiter 本地化**：接入完整交接、取消与 shutdown；删除 cross-thread waiter、external mode 和 poll(0)。迁移依赖它们的 tests/example。
5. **全部调用方适配**：HTTP/1、HTTP/2、QUIC/UDP、TLS、应用及测试替身；完成注册、调度和状态语义审计。
6. **验证与性能记录**：定向测试、sanitizer、完整 CTest 与同机 A/B；记录实际执行结果再更新实施状态。

这些是实现依赖顺序，不允许把中间混合语义当作最终交付。不能在 caller 尚未迁移时，仅靠新的断言让旧能力失败并宣布完成。

## 12. 验证矩阵

测试使用真实非阻塞 fd、可控 loop 消息顺序和必要的故障注入。竞争测试用同步点构造时序，避免用固定 sleep 猜测结果。优先验证可观察行为、通知次数和 epoll_ctl 次数。

### 12.1 注册与就绪

| 场景 | 验收结果 |
|---|---|
| 构造、attach、接收交接、立即成功 I/O | 无 ADD/MOD；lambda 在 Unknown/Ready/Blocked 下都执行 |
| 首次订阅读，后续首次订阅写 | 按实际缺失 interest 执行 ADD/MOD；之后重复 set/clear 不调用 epoll_ctl |
| clear 后只有一次数据到达 | 无 callback 也变 Ready；恢复业务无需新包即可读取 |
| 数据到达早于首次 ADD / 扩展 MOD | 安装后仍能得到就绪，不永久等待 |
| Unknown→Ready、Blocked→Ready、Ready→Ready | 前两者最多通知当前订阅一次；最后一种不重复通知 |
| callback 内重新读空 | 返回后状态仍为 Blocked，下一次数据能再次通知持续订阅 |
| Ready 时安装普通 callback | 捕获违反前置条件；awaiter Ready 路径不挂起、不注册、不 defer |
| scalar/vector 普通短读、读满、EAGAIN | 按有效长度正确更新；有剩余数据时不能丢续跑 |
| 部分写、写满、发送空间恢复 | 不丢前缀与后续进度，受约束短写策略独立验证 |
| UDP 短包、零包、批量部分成功 | 不错误推断 EOF/耗尽，不丢后续包 |
| clear/reinstall 相同 callback/ctx | 旧派发不通知新订阅 |
| 持续 Ready、无 callback 或预算/背压暂停 | 无自动 defer 空转，恢复与公平性由 caller 保证 |

### 12.2 流状态与生命周期

| 场景 | 验收结果 |
|---|---|
| 数据与 FIN 同批、短读后 FIN、读到 EOF | 先交付数据再 EOF；本端写侧仍可工作 |
| ERR/HUP 到来时方向已经 Ready | StreamFd 仍观察终止，不受普通转换通知抑制 |
| 已知 EOF/error 时 wait，terminal 迟订阅 | 有限时间完成，不等待不存在的新边沿，setter 不内联业务回调 |
| TLS 已有明文、部分 record、双向 WANT | 方向与重试正确，底层 Blocked 不屏蔽已有明文 |
| BIO 内错误、terminal callback 自毁 | SSL/stream 栈安全，无重入释放 |
| timeout、close、取消、就绪交错 | waiter 恰好完成一次，timer 和订阅都清理 |
| read callback 替换 write、同批 A 销毁 B、同址复用 | 旧事件/旧订阅不访问新对象，无 UAF |
| 未注册对象在 current loop 停止时存在 | 生命周期仍被接管并清理，不能只依赖已注册 fd 的 stop hook |
| 初始化失败与 QUIC UDP rollback | 未启动、未注册 fd 的清理不触发错误线程断言 |

### 12.3 借用与归还

| 场景 | 验收结果 |
|---|---|
| 本地 pool hit | current 不变，无交接 DEL/ADD |
| home→borrower→home，各阶段均需监听 | 旧线程 DEL 先于发布，新线程首次订阅才 ADD；同一 fd 无重叠注册 |
| borrower 全部 I/O 立即完成 | borrower 无 ADD，也无需归还 DEL |
| 旧 loop 已取出事件后交接，borrower 立即读空 | 旧事件不更新新状态，无数据竞争 |
| 解除监听到新订阅之间数据/FIN/RST 到达 | 接收方 syscall/订阅能观察，保留已知流状态 |
| 获取取消、lease 析构、任务放弃 | 先结束叶子 I/O，再交接/归还，无悬挂 callback |
| 借还消息在途时停止/销毁 pool | 生命周期等待覆盖消息与借出 lease，不向失效 loop/pool 投递 |
| ADD/MOD/DEL 故障 | 不交付假成功等待或未解除归属的连接，无残留 UAF |
| 同一连接反复被不同 worker 借用 | 单个 Item 安全复用，无跨线程 waiter 分配和 sticky 状态 |
| TCP 与 TLS 的远程连接复用 | 内容、身份隔离、超时、错误和回到 home 后复用均正确 |

### 12.4 执行要求

实施阶段先运行受影响的定向 GoogleTest，再执行格式化、完整构建与串行 CTest。可从以下命令开始，实际配置和过滤器以实施时仓库为准：

```bash
cmake -S . -B build -DFIBER_BUILD_TESTS=ON
cmake --build build -j 6
./build/fiber_tests --gtest_filter='PollerTest.*:EventLoopTest.*:RWFdTest.*:StreamFdTest.*:TlsStreamFdTest.*:StealableHttp1ConnectionPoolSetTest.*'
./format_code.sh
git diff --check
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

新增/删除源文件后先重新配置 CMake。另做 ASan/UBSan 生命周期与回调重入检查，TSan 覆盖真实借还交接；启用 timerfd fallback 的配置也需验证。HTTP/2、QUIC/UDP、HttpTransport 和启动回滚测试不能因首轮过滤器未列出而漏验。

测试结果按“定向通过 / 完整通过 / 跳过 / 环境阻塞 / 尚未执行”记录，不能用旧 ET 文档的结果替代本次证据。

## 13. 性能验收

| 指标 | 目标与边界 |
|---|---|
| 本地归属期 set/clear 开销 | 已监听方向的 set/clear 为用户态操作，不触发 MOD/DEL |
| 未订阅 I/O | 无强制注册；每次有效 read/write 调用都执行 lambda，不再承诺省掉 EAGAIN syscall |
| Ready 补发 | 普通 setter 不产生 deferred-ready；统计 callback、continuation 与唤醒次数 |
| 跨线程等待 | 每次 I/O 等待无跨线程 waiter 分配和 Notify/Cancel/Resume 往返 |
| 完整远程借还 | 两端都实际注册时，通常是旧 DEL、新 ADD、借方 DEL、home 按需 ADD；未注册阶段省略相应操作 |
| 空闲成本 | 无持续 Ready callback 忙循环；记录长期监听的初始事件与无订阅状态更新成本 |
| 吞吐与延迟 | 对比本地优先和强制借用、立即完成和多次阻塞；记录借用比例、CPU/request、p50/p99、成功率 |

采用同机、同构建、同负载的交替 A/B，并将建连、预热、稳态、交接和关闭分别计数。覆盖 HTTP/1、HTTP/2、HTTP/3、Unix 与 TLS，尤其观察 budget continuation、背压恢复和 keep-alive 首字节超时。

收益来自归属与通知机制简化，并不保证吞吐必然提升；取消 syscall suppression 可能增加主动探测，必须与减少的 poll(0)、跨线程通知、defer 和分配一起测量。

## 14. 完成条件与参考

完成重构必须同时满足：

- Efd 单份注册和旧 loop DEL / 新 loop 延迟 ADD 已落地；全部 poller 操作符合线程归属。
- Event 三态、非 Ready 订阅、clear 不改 interest、无订阅也更新状态、仅转换通知均有验证。
- read/write 不注册、不抑制 lambda；EOF/error 已从 RWFd 移至合适的流层。
- cross-thread waiter、external mode、poll(0) 与 Ready 补发机制删除，所有调用方完成迁移。
- 借还取消、停止、在途消息与对象销毁路径闭合；没有遗留的旧 loop 访问。
- 定向、完整回归、sanitizer 与性能结果有实际记录；剩余限制明确列出。

### 14.1 实测记录（2026-09-15）

构建与测试命令（每个 sanitizer 周期单独执行；`FETCHCONTENT_BASE_DIR` 共享，切换构建目录前需
`rm -rf temp/_deps/boringssl-build temp/_deps/boringssl-tmp` 重建依赖，最后恢复 plain 构建）：

```bash
# 定向
./build/fiber_tests --gtest_filter='PollerTest.*:EventLoopTest.*:RWFdTest.*:StreamFdTest.*:TlsStreamFdTest.*:StealableHttp1ConnectionPoolSetTest.*'
# plain
cmake --build build -j 14 && (cd build && ctest -j 4)
# ASan / UBSan / TSan（UBSan、TSan 用 clang：-DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++，
# sanitizer 标志同时进 CMAKE_C_FLAGS 与 CMAKE_CXX_FLAGS）
ASAN_OPTIONS=alloc_dealloc_mismatch=0 ctest --test-dir build-asan -j 4
UBSAN_OPTIONS=print_stacktrace=1 ctest --test-dir build-ubsan -j 4
TSAN_OPTIONS=halt_on_error=0 ctest --test-dir build-tsan -j 4
./format_code.sh && git diff --check
```

结果：

| 阶段 | 结果 | 说明 |
|---|---|---|
| 定向过滤器（Poller/EventLoop/RWFd/StreamFd/TlsStreamFd/Stealable） | 全部通过 | Stealable 套件 11/11，另在 TSan 下循环 5 轮通过 |
| plain 完整 ctest | 2087/2087 通过 | 4 个互操作性测试环境性跳过（NginxInterop、RnacosInterop×3） |
| ASan | 除 1 个已知存量缺陷外全部通过 | `Http3ServerConnectionTest.GracefulShutdownRejectsNewRequestStreams` heap-use-after-free（`QuicStreamRecvQueue::received_end_offset`，测试 feed 路径）；`git stash` 后在 HEAD 复现同样失败，纯构建 5/5 通过，属重构前已存在问题；另需 `alloc_dealloc_mismatch=0` 规避 libstdc++ `stable_sort` 临时缓冲的 ASan 误报 |
| UBSan | 2087/2087 通过 | 连续两轮全绿；修复的命中项均非产品缺陷：gtest 对 packed `epoll_event.data.ptr` 绑引用（PollerTest 提局部变量）、空 `string_view`/空 `IoBufChain` 传入 memcpy/protobuf 解析（Http2ConnectionTest、ProtoCodec.cpp 加空保护） |
| TSan | 2086/2087 通过 | 唯一失败为上述同一存量 Http3 UAF；lite_nginx 跨 worker 借还复现并修复了真实缺陷（见 14.2） |
| `./format_code.sh` + `git diff --check` | 通过 | 无空白问题 |

已知环境性偶发（与本次重构无关，未计入失败）：`LiteNginxRuntimeTest` 部分用例在 `ctest -j 4` 高负载下偶发失败，
根源是测试侧 `reserve_loopback_port()` 先 bind-close 再回读的端口 TOCTOU（并发用例可保留同一端口）；单独重跑即通过。
`DnsClientTest.CloseAndReleaseCancelPendingTcpFallback` 在 TSan 高负载下同样单次偶发、单独重跑 4/4 通过。

### 14.2 验证中发现并修复的缺陷

- **借出连接归还时的 dispatch 重入（真实缺陷，TSan + FIBER_ASSERT 双确认）**：
  `LiteNginxRuntimeTest.StealsNamedUpstreamConnectionsAcrossWorkersWhenEnabled` 约 1/14 复现
  `FIBER_ASSERT failed: RWFd event dispatch cannot re-enter`。根因：`State::return_home()` 在
  `Lease::reset()` 内联执行 detach+发布，而 reset 可能运行在借方 loop 对该 fd 的在途 `handle_events`
  回调里（exchange 协程从读回调恢复）；home loop 立即 adopt+ADD 并派发，与借方仍在栈上的
  `DispatchGuard` 冲突（TSan 报 `Efd::watch_set` 写与 `Efd::epoch` 读竞争）。修复：归还拆为
  `Dispatch::DetachAndReturn` 投递到借方 loop 下一轮执行，任何在途 dispatch 先行退栈；修复后
  plain 80/80、TSan 25/25 无告警。测试侧同步更新：`StealsIdleConnectionFromOtherLoopAndReturnsItHome`
  与 `BorrowedConnectionHeldByOneLoopMakesOtherLoopMiss` 的 home 侧重新 acquire 前增加 10ms
  等待（归还旅程多出一个 loop 轮次，与既有用例的 settle 方式一致）。
- **测试直接跨线程改连接队列（TSan）**：`QuicUdpEndpointTest.DetachClearsFramesAndSuppressesNewPendingFrames`
  在主线程 push 帧队列、与 endpoint loop 的 send pump 并发。重构后 still-Ready 方向改为立即 pump，
  使该窗口必然打开。测试改为在 loop 上投递帧（产品代码不变）。
- **`GzipEncoderTest` 全局 new/delete 替换与 TSan 运行时冲突**：链接期 multiple definition；
  TSan 下编译排除替换（计数器保持 0，稳态断言在该构建下空过，其余构建执行真实断言）。

### 14.3 独立复审修复（2026-09-15 第二轮，`temp/rwfd-review/review.md`）

复审提出 5 项，逐条核对结论：2/3/4/5 成立，6 部分成立（wakeup fd 的 ctor ADD 确实违反线程归属；
timerfd 后端为 ctor 初始化属有意设计——内部 fd、`data.ptr=nullptr`、无派发身份，在任何线程接触
poller 前完成注册，保留并补充注释说明）。

- **半关闭短读后等待丢失（P1，StreamFd）**：RDHUP 与数据边沿同批到达被消费，短读把就绪态停在
  Blocked，之后不再有新边沿。修复：读方向的等待门在 `eof_ || peer_hangup_` 时直接放行（读侧
  已不可能真正阻塞），不需要边沿。写方向不受影响。
- **HUP→terminal 提示错误阻塞排空（P1，StreamFd）**：裸 HUP/ERR 提示（`terminal_error_==Unknown`）
  会把读等待直接判为错误，而缓冲区还有数据可读。修复：三态门按方向区分——Unknown 提示不否决
  读方向（数据排空后由 syscall 报告真实结尾 0 或具体错误）；已记录的具体致命错误仍立即交付；
  写方向遇任何 terminal 都报错。
- **adopt 不接管生命周期（P2，RWFd 及全线）**：交接后若从未订阅（永不 ADD），新 loop 停止时
  无人关闭 fd。修复：`adopt_loop` 安装目标 loop 的 stop hook（生命周期接管即刻生效、ADD 保持
  延迟）；目标 loop 已在停止则拒绝收养并关闭 fd，`adopt_loop` 返回值由 `void` 改为
  `IoErr` 并沿 TlsStreamFd/TcpStream/TlsTcpStream/HttpTransport/Http1ClientConnection 逐层传播。
  连接池 `take_result` 拒收收养失败时投递 ReturnHome 让 entry 回家回收（借出判空，与停机竞态
  同一出口）；`Http1ClientConnection::adopt_loop` 失败路径先绑定 `fd_loop_` 再走完整 `close()`，
  保证回家析构时 loop 身份正确。
- **shutdown 不覆盖借出/在途归还（P1，StealableHttp1ConnectionPoolSet）**：`active_acquire_wg_`
  原在投递完成时即 done，借出期间与归还消息在途期间停机不等待。修复：窗口延伸到整个旅程——
  本地出借在 `take_result` 结束、远端在 `arrive_home`/`finalize_canceled` 结束，各恰好一次。
  停机语义变为：等全部借出归还落地后再执行清空。
- **Poller 线程归属未强制（EventLoop/Poller）**：`EventLoopGroup` 在调用方线程构造 loop、worker
  线程运行，ctor 里的 wakeup ADD 落在错误线程。修复：wakeup eventfd 改为 ctor 仅创建、首次
  `run_once` 在 loop 线程注册（LT 保证首轮前写入的唤醒不丢）；`Poller` 记录
  `owner_thread_`，add/mod/del/wait 一律断言归属，`run_prepared()` 通过
  `rebind_owner_thread()` 把归属锚定到实际运行线程。

新增/迁移测试：`StreamFdTest.HalfCloseShortReadThenWaitCompletesWithoutNewEdge`、
`StreamFdTest.HupWithBufferedDataLetsWaitsDrainRemainingThenEof`、
`RWFdTest.AdoptionTakesLifecycleEvenWithoutSubscription`、
`StealableHttp1ConnectionPoolSetTest.ShutdownWaitsForBorrowedLeaseAndDropsItOnReturn`（原
`ShutdownDropsBorrowedConnectionOnReturn` 迁移强化：断言停机在借出期间不完成）。
独立复现程序 `temp/rwfd-review/repro.cpp` 三模式修复后输出：`half-close` next_wait_ok=1
timed_out=0 direct_read_bytes=0；`hup-buffer` next_wait_ok=1 timed_out=0 direct_read_bytes=4；
`adopt-stop` detach_ok=1 adopt_registered=0 valid_after_target_stopped=0。

本轮验证（总用例数增至 2090 = 2086 执行 + 4 环境跳过）：

| 阶段 | 结果 | 说明 |
|---|---|---|
| 定向过滤器（10 套件） | 93/93 通过 | 含上述 4 个新测试逐一确认 |
| plain 完整 ctest | 2086/2086 通过 | 4 个互操作性测试环境性跳过，ctest 退出码 0 |
| ASan | 除 1 个已知存量缺陷外全部通过 | 唯一失败仍为 `Http3ServerConnectionTest.GracefulShutdownRejectsNewRequestStreams`，签名与 HEAD 存量一致（`QuicStreamRecvQueue::received_end_offset` UAF） |
| UBSan | 2086/2086 通过 | 0 个 runtime error |
| TSan | 2085/2086 通过 | 唯一失败为同一存量 Http3 UAF（5 条告警均为该 freed 对象的各访问点），0 数据竞争 |

Linux 语义参考：

- ET 下可以长期保留监听；未确认耗尽的 I/O 由调用方继续推进。MOD 会重新检查可用 I/O；流式与报文式的耗尽判断不同。[epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html)
- ERR/HUP 无须显式订阅也可能报告，RDHUP 表示对端写侧关闭，HUP 不表示所有缓冲数据已经读完。本方案通过 DEL 完成交接，不依赖空 interest 屏蔽终止事件。[epoll_ctl(2)](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html)

本次文档交付只固定方案与验收要求。实施状态、运行命令和性能数字应在实际执行后另加记录。
