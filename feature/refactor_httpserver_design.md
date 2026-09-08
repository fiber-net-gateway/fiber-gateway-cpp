# HttpServer 重写详细设计

> 对应需求草稿：`feature/refactor_httpserver.md`
> 状态：设计待评审
> 目标读者：实现者 / 评审者

---

## 1. 目标与非目标

### 1.1 目标

1. **一个 Server 管多个 endpoint**：把「监听端口 + 协议 + TLS/超时配置 + handler」收敛成 `Endpoint`，`Server` 只负责 endpoint 生命周期编排、worker 编排、停机屏障。
2. **统一三套并行的服务端生命周期代码**：现在 `HttpServer`、`Http1Server`、`Http3Server` 各自实现了一遍 bind/serve/close/shutdown_and_wait，语义还不完全一致（见 §3）。重写后只有一套。
3. **真正的优雅停机**：`stop()` 后 in-flight 请求跑完再关连接（H2/H3 发 GOAWAY，H1 在当前 exchange 结束后关），超过 `drain_timeout` 才硬中断。
4. **消除跨线程共享容器**：per-worker 的连接注册表全部走 loop-affine 的侵入式链表，去掉 `std::mutex + std::vector<std::shared_ptr<Http1Entry>>` 这类全局结构。
5. **停机后资源归属明确**：`co_await server.serve()` 返回时，所有 listener、连接、per-worker 上下文都已析构，调用方可以安全地 `worker_group->stop(); join();`。

### 1.2 非目标

- 不改 `Http1Connection` / `Http2Connection` / `Http3Connection` / `QuicConnection` 的协议实现，只改它们的**停机入口**和**注册方式**。
- `Server` 不拥有、也不停止 `EventLoopGroup`。草稿里说的「event loop group 通知停止」在本设计中的含义是：**Server 向每个 worker loop 投递停机通知并等待其资源回收**；loop 本身的 `stop()/join()` 仍由调用方负责（时序见 §6.4）。
- 不引入指标/埋点系统（预留 §12 的扩展位）。

### 1.3 已确认的决策（评审已定）

| # | 决策点 | 结论 |
|---|--------|------|
| D1 | Endpoint 抽象形式 | **虚接口基类**，不用 `EndpointOps` 函数表 |
| D2 | TCP accept 模型 | **保持单 accept loop + round-robin 分发到 worker**（不引入 SO_REUSEPORT 分片；HTTP/3 因为是 UDP 无连接，仍保持既有的 per-worker 分片） |
| D3 | 停机语义 | **优雅 drain + `drain_timeout` 超时硬中断兜底** |
| D4 | handler 归属 | **每个 endpoint 自带 handler**，Server 构造时的 handler 作为未设置时的默认值 |
| D5 | 配置结构 | **每个 endpoint 定义自己的 options，只保留自己关心的字段；不存在 server 级 options**。现有的大杂烩 `HttpServerOptions` 拆散（§7.1），`drain_timeout` 也下沉到每个 endpoint |
| D6 | 明文 HTTP/2 | **不支持 `Upgrade: h2c`**，只支持 prior-knowledge h2c |
| D7 | `HttpServer` 兼容门面 | **只作为迁移期脚手架，P7 直接删除**，不保留版本周期 |

---

## 2. 概念模型

```
                      ┌──────────────────────────────────────────────┐
                      │  Server        (owner loop / 主线程)          │
                      │  - endpoints_ : vector<unique_ptr<Endpoint>> │
                      │  - workers_   : vector<unique_ptr<Worker>>   │
                      │  - state_ / drain 屏障                       │
                      └───────┬───────────────────────┬──────────────┘
              add_endpoint()  │                       │  worker i 与 loop i 绑定
                              ▼                       ▼
   ┌───────────────────────────────────┐   ┌──────────────────────────────────┐
   │ Endpoint(虚接口)                   │   │ Server::Worker                    │
   │  Http1Endpoint / Http2Endpoint    │   │  slots_ : vector<unique_ptr<      │
   │  Http21Endpoint / Http3Endpoint   │   │             EndpointWorker>>      │
   │  - listener / QUIC shards         │   │  slots_[i] 属于 endpoints_[i]     │
   │  - options(tls/timeout) + handler │   │  notify_stop() / run_wait_stop()  │
   └───────────────────────────────────┘   └──────────────────────────────────┘
                              │                       ▲
                              │ accept 后按 index 分发  │
                              └───────────────────────┘
```

- **Endpoint**：主线程对象。一个 endpoint = 一个监听地址 + 一种协议策略 + 一份**只属于该协议**的 options（§7.1）+ 一个 handler。
- **EndpointWorker**：worker 线程对象。endpoint 在**每个 worker loop 上**的分身，持有该 loop 上属于这个 endpoint 的所有连接，并实现 drain / abort / wait_stopped。
- **Server::Worker**：worker 线程的编排器，持有 N 个 `EndpointWorker`（N = endpoint 数），只做「统一 drain、统一等待、超时统一 abort」。

`slots_[i]` 与 `endpoints_[i]` 下标严格对齐，这样 accept 路径不需要查表：endpoint 自己记着 `std::vector<Http1EndpointWorker *> workers_`（自己的具体类型），投递连接时直接 `workers_[idx]`，无虚调用。

---

## 3. 现状问题清单（重写的依据）

| 问题 | 位置 | 说明 |
|------|------|------|
| 三套生命周期实现 | `HttpServer.cpp` / `Http1Server.cpp` / `Http3Server.cpp` | `Http1Server::close()` 只关 listener；`HttpServer::close()` 走完整状态机；`Http3Server::close()` 直接 `endpoint.close()` 强杀连接。语义不一致 |
| H1 连接注册表是全局互斥容器 | `HttpServer.cpp` `Runtime::http1_connections` | `std::mutex` + `vector<shared_ptr<Http1Entry>>` + 每条连接一次 `shared_ptr` 分配；停机时逐条 `spawn` 投递。H2 侧（`Http2ServerWorker`）已经是 per-loop 侵入式链表，两边不统一 |
| 停机会打断 in-flight 请求 | `Runtime::request_shutdown()` → `Http1Connection::shutdown()` = `finish()` = `transport_->close()` | 不区分「空闲 keep-alive 连接」和「正在处理请求的连接」，一律关 socket |
| H2 停机是硬中断 | `Http2ServerConnection::request_shutdown()` → `conn_.shutdown(Canceled)` | `Http2Connection::graceful_shutdown()` 已存在但服务端没用上 |
| H3 停机是硬中断 | `Http3Server::close()` → `QuicUdpEndpoint::close()` | `close()` 会 `force_detach_connection` 掉所有连接并关 socket，现有连接直接消失；`Http3Connection::graceful_shutdown()` 同样没用上 |
| 一个端口的 TCP+UDP 绑在一个 HttpServer 上 | `HttpServer::bind()` 内部 new `Http3Server` | HTTP/3 是否开启由 `options.http3.enabled` 隐式决定，配置耦合；同时 `Http1Server`/`Http3Server` 又能单独用，三者关系混乱 |
| 析构语义不安全 | `HttpServer::~HttpServer()` 调 `request_close()` 但不等待 | 依赖调用方自觉先 `shutdown_and_wait()`；lite_nginx 为此写了 `std::promise` + 手动 `accept_loop_.run()` 的一大段兜底逻辑（`ServerLauncher::close()`） |
| 多 listener 要开多个 Server | `ServerLauncher::start()` | 每个 listener 一个 `HttpServer`，停机时串行 `co_await server->shutdown_and_wait()`，drain 无法并行 |

---

## 4. 文件与命名

```
include/fiber/http/
  Server.h                    // fiber::http::Server + Endpoint + EndpointWorker
  Http1ServerOptions.h        // 由 HttpServerOptions 拆出：H1/H2/H3 各一份（§7.1）
  Http2ServerOptions.h
  Http3ServerOptions.h
  endpoint/Http1Endpoint.h
  endpoint/Http2Endpoint.h
  endpoint/Http21Endpoint.h   // ALPN 协商 h2 / http/1.1
  endpoint/Http3Endpoint.h
  HttpServer.h                // 保留：薄兼容门面，实现改为委托 Server（见 §10）

src/http/
  Server.cpp
  ServerWorker.cpp            // Server::Worker（也可并入 Server.cpp）
  endpoint/TcpEndpointBase.cpp  // accept loop / TLS 握手 / 分发，H1/H2/H21 共用
  endpoint/Http1Endpoint.cpp
  endpoint/Http2Endpoint.cpp
  endpoint/Http21Endpoint.cpp
  endpoint/Http3Endpoint.cpp
```

命名空间统一 `fiber::http`。类名按草稿用 `Server`（与 `HttpServer` 兼容门面共存一段时间）。

> 备选：把 `Server/Endpoint/EndpointWorker` 这套与协议无关的骨架抽到 `fiber::server` 模块，为将来非 HTTP 的服务端复用。本次不做——骨架只有约 300 行，且当前所有 endpoint 都是 HTTP 的；等出现第二个协议族再抽。

---

## 5. 核心接口

### 5.1 EndpointWorker

```cpp
namespace fiber::http {

// 一个 endpoint 在某个 worker loop 上的分身。
// 除构造外，所有成员函数都在该 worker 的 loop 上调用。
class EndpointWorker : public common::NonCopyable, public common::NonMovable {
public:
    virtual ~EndpointWorker() = default;

    // 停止接纳新工作，并请求已有连接优雅收尾：
    //   H1  -> 空闲连接立刻关；正在处理请求的连接打 draining 标记，响应带 Connection: close
    //   H2  -> GOAWAY，已开流跑完
    //   H3  -> 拒绝新连接，已有连接 GOAWAY
    // 幂等。
    virtual void drain() noexcept = 0;

    // drain_timeout 到期后的硬中断：abort 所有流 + 关 transport / QUIC endpoint。幂等。
    virtual void abort() noexcept = 0;

    // 该 worker 上属于本 endpoint 的连接全部结束后完成。
    // 必须可重复 co_await（Worker 只 await 一次，但实现不应假设）。
    [[nodiscard]] virtual async::Task<void> wait_stopped() noexcept = 0;
};

} // namespace fiber::http
```

析构在**自己的 worker loop 上**执行（由 `Server::Worker::run_wait_stop` 在 `wait_stopped()` 完成后清空 `slots_`），因此 loop-affine 成员可以安全释放。

### 5.2 Endpoint

```cpp
class Server;

class Endpoint : public common::NonCopyable, public common::NonMovable {
public:
    virtual ~Endpoint() = default;

    // ---- 以下三个函数在 owner loop（主线程）各执行一次 ----

    // 绑定监听资源。失败会让整个 Server::start() 失败并回滚已启动的 endpoint。
    // server 提供 worker 数量/loop、默认 handler、drain_timeout。
    [[nodiscard]] virtual common::IoResult<void> on_start(Server &server) noexcept = 0;

    // accept / 收包循环。listener 关闭后返回。
    [[nodiscard]] virtual async::Task<void> on_serve() noexcept = 0;

    // 关闭 listener：此后不再产生新连接。幂等，noexcept。
    virtual void on_stop() noexcept = 0;

    // ---- 下面这个在 start() 期间为每个 worker 各调用一次（主线程构造） ----
    // 返回 nullptr 表示分配失败 -> start() 失败。
    [[nodiscard]] virtual EndpointWorker *create_worker(event::EventLoop &loop, std::size_t index) noexcept = 0;

    // 绑定后的本地地址（端口 0 时为内核分配的实际端口）。on_start 成功后有效。
    [[nodiscard]] virtual const net::SocketAddress &local_addr() const noexcept = 0;

    // 本 endpoint 的优雅停机预算，来自自己的 options（D5：没有 server 级配置）。
    // 0 = 不等待，直接硬停；max() = 无限等待。Server::Worker 按 endpoint 分别计时。
    [[nodiscard]] virtual std::chrono::milliseconds drain_timeout() const noexcept = 0;
};
```

**为什么用虚接口而不是 Ops 表**：这些函数在整个进程生命周期里一共只被调用 `O(endpoint × worker)` 次，全在冷路径；换成函数表要为每个 endpoint 手写 trampoline，还要额外处理 `void*` ctx 的所有权与析构（`unique_ptr<void, void(*)(void*)>`），收益为零。真正的热路径（accept 后把 fd 交给 worker）不经过这个接口——endpoint 持有自己具体类型的 worker 指针数组，直接静态派发。

**分配约定**：项目禁用异常，`create_worker` 内部用 `new (std::nothrow)`，失败返回 `nullptr`；`Server` 立即接管为 `std::unique_ptr<EndpointWorker>`。

### 5.3 Server

`Server` 自身**没有配置结构**（D5）：它只有 owner loop、worker group、默认 handler 三个构造参数，所有可调项都在各 endpoint 的 options 里。

```cpp
class Server : public common::NonCopyable, public common::NonMovable {
public:
    enum class State : std::uint8_t {
        Created,   // 可以 add_endpoint
        Started,   // 已 bind，未 serve
        Serving,
        Draining,  // 已关 listener，等待连接收尾
        Stopped,   // 全部资源已释放
    };

    Server(event::EventLoop &loop, HttpHandler default_handler, event::EventLoopGroup *workers = nullptr);
    ~Server();  // FIBER_ASSERT(state == Created || state == Stopped)

    // 只能在 Created 状态、owner loop 上调用。失败（OOM）返回 nullptr。
    template<class E, class... Args>
    [[nodiscard]] E *add_endpoint(Args &&...args) noexcept;

    // owner loop，同步。依次 on_start() 所有 endpoint，再为每个 worker 创建 slots。
    // 任一步失败：已启动的 endpoint 逆序 on_stop()，已创建的 slot 就地销毁，返回错误。
    [[nodiscard]] common::IoResult<void> start() noexcept;

    // owner loop。spawn 所有 endpoint 的 on_serve()，然后挂起，直到停机完全结束。
    // 返回时：listener 已关、所有连接已结束、所有 EndpointWorker 已析构。
    [[nodiscard]] async::Task<void> serve() noexcept;

    // 任意线程、任意次数。非阻塞，只触发停机。
    void stop() noexcept;

    // = stop() + 等待与 serve() 相同的屏障。可与 serve() 并存（多个 joiner）。
    [[nodiscard]] async::Task<void> stop_and_wait() noexcept;

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool draining() const noexcept;   // 供连接层判断是否该收尾

    // 给 Endpoint 用的访问器
    [[nodiscard]] event::EventLoop &owner_loop() const noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] event::EventLoop &worker_loop(std::size_t index) const noexcept;
    [[nodiscard]] const HttpHandler &default_handler() const noexcept;

private:
    class Worker;
    ...
};
```

**worker 数量**：`workers != nullptr && workers->size() > 0 ? workers->size() : 1`；无 group 时唯一的 worker 绑定在 owner loop 上（与现状一致）。

**`serve()` 与 `stop_and_wait()` 的屏障**：内部是一个 `async::WaitGroup shutdown_wg_`，在 `start()` 成功时 `add(1)`，在 §6.3 第 7 步 `done()`。`serve()` 在 spawn 完所有 accept loop 后 `co_await shutdown_wg_.join()`。`WaitGroup` 是跨线程安全的（内部 mutex + 按 waiter 所在 loop 唤醒），因此 `stop_and_wait()` 可以在任意 loop 上被 await。

### 5.4 Server::Worker

因为 `drain_timeout` 是每个 endpoint 自己的（D5），deadline 定时器挂在 **slot** 上而不是 worker 上：一个 worker 上的 H3 slot 可以等 60s，同一 worker 的 H1 slot 只等 5s，互不影响。

```cpp
class Server::Worker : public common::NonCopyable, public common::NonMovable {
public:
    Worker(Server &server, event::EventLoop &loop, std::size_t index) noexcept;

    // 主线程 start() 期间填充，下标与 Server::endpoints_ 对齐。
    void install(std::unique_ptr<EndpointWorker> worker, std::chrono::milliseconds drain_timeout);

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }

    // 任意线程，幂等：把停机流程投递到自己的 loop。
    void notify_stop() noexcept;

private:
    // 一个 endpoint 在本 worker 上的槽位。TimerEntry 不可移动，所以按 unique_ptr 持有。
    struct Slot {
        Worker *owner = nullptr;
        std::unique_ptr<EndpointWorker> worker;
        std::chrono::milliseconds drain_timeout{};
        event::EventLoop::TimerEntry deadline{};
    };

    static void on_stop(Worker *self) noexcept;       // NotifyEntry trampoline
    static void on_deadline(Slot *slot) noexcept;     // TimerEntry trampoline
    static async::DetachedTask run_wait_stop(Worker *self) noexcept;

    Server *server_;
    event::EventLoop &loop_;
    std::size_t index_;
    std::vector<std::unique_ptr<Slot>> slots_;
    event::EventLoop::NotifyEntry stop_entry_{};
    std::atomic<bool> stop_posted_{false};
};
```

```cpp
void Server::Worker::notify_stop() noexcept {
    if (stop_posted_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (loop_.in_loop()) {
        on_stop(this);
    } else {
        loop_.post<Worker, &Worker::stop_entry_, &Worker::on_stop>(*this);
    }
}

void Server::Worker::on_stop(Worker *self) noexcept {
    for (auto &slot: self->slots_) {
        slot->worker->drain();
        const auto budget = slot->drain_timeout;
        if (budget == std::chrono::milliseconds::zero()) {
            slot->worker->abort();   // 不等待，直接硬停
        } else if (budget != std::chrono::milliseconds::max()) {
            self->loop_.post_at<Slot, &Slot::deadline, &Worker::on_deadline>(self->loop_.now() + budget, *slot);
        }
    }
    async::spawn(self->loop_, [self]() -> async::DetachedTask { return run_wait_stop(self); });
}

void Server::Worker::on_deadline(Slot *slot) noexcept { slot->worker->abort(); }

async::DetachedTask Server::Worker::run_wait_stop(Worker *self) noexcept {
    for (auto &slot: self->slots_) {
        co_await slot->worker->wait_stopped();
        if (slot->deadline.is_in_heap()) {
            self->loop_.cancel<Slot, &Slot::deadline>(*slot);
        }
    }
    self->slots_.clear();                // 在自己的 loop 上析构 loop-affine 资源
    self->server_->on_worker_stopped();  // 线程安全：workers_wg_.done()
    co_return;
}
```

**为什么用定时器而不是 `timeout_for(join())`**：`async::timeout_for` 超时时会连同内部 awaiter 一起销毁，而此时内部的 `Task` 协程仍挂起在 `wait_stopped()` 上，销毁语义不安全。用 `TimerEntry` 触发 `abort()`、再让同一个 `wait_stopped()` 自然完成，既没有取消语义问题，也保证「abort 之后一定还会等到资源真正释放」。

---

## 6. 生命周期

### 6.1 状态机

```
        add_endpoint()*
Created ─────────────┐
   │                 │
   │ start()         │ start() 失败 → 回滚，仍是 Created
   ▼                 │
Started ─────────────┘
   │ serve()
   ▼
Serving
   │ stop()  (任意线程)
   ▼
Draining ── 所有 endpoint on_serve() 返回 && 所有 worker 停完 ──▶ Stopped
```

`stop()` 在 `Created`/`Started` 上调用同样合法：直接推进到 `Stopped`（`Started` 需要先 `on_stop()` 各 endpoint 并回收 slot）。

### 6.2 启动时序（全部在 owner loop 上同步执行）

```
start():
  1. 断言 in_loop() 且 state == Created；endpoints_ 非空，否则 IoErr::Invalid
  2. for i in endpoints:  endpoints_[i]->on_start(*this)     // bind listener / QUIC init
        失败 -> 逆序 on_stop() 已成功的，返回 error
  3. 创建 workers_（count = group ? group->size() : 1）
  4. for w in workers: for i in endpoints:
        slot = endpoints_[i]->create_worker(w->loop(), w->index())
        nullptr -> 回滚（销毁已建 slot、逆序 on_stop），返回 IoErr::NoMem
        w->install(unique_ptr(slot))
  5. shutdown_wg_.add(1); state = Started
```

**为什么 `create_worker` 在主线程构造、而不是投递到 worker loop 构造**：现有 `Http3Server::bind()` 已经是这个模式（主线程 `endpoint.init(*shard->loop, ...)`，随后 `post` 到该 loop 上 `start()`）。启动期没有并发，构造后第一次跨线程使用发生在 `post` 之后，`MpscQueue` 提供了必要的 happens-before。这样可以避免启动阶段的多线程 rendezvous。**约束**：`create_worker` 里不得执行 loop-affine 的 I/O 注册（epoll 注册、定时器），那些放到第一次在本 loop 上被使用时做。

### 6.3 停机时序

```
stop()                                     [任意线程]
  1. CAS state: Serving|Started -> Draining；已是 Draining/Stopped 直接返回
  2. in_loop(owner) ? on_owner_stop(this) : post(owner_loop, on_owner_stop)

on_owner_stop()                            [owner loop]
  3. for e in endpoints_: e->on_stop()            // 关 listener
        -> 各 accept loop 的 accept() 返回 Canceled/BadFd -> on_serve() 返回
  4. workers_wg_.add(workers_.size())
     for w in workers_: w->notify_stop()          // 跨线程 post

Worker::on_stop()                          [worker loop, 每个 worker 一次]
  5. 逐 slot：worker->drain()                     // GOAWAY / Connection: close / 关空闲连接
  6. 逐 slot：按该 endpoint 自己的 drain_timeout 挂 deadline 定时器
  7. spawn run_wait_stop:
        逐 slot：co_await worker->wait_stopped()；取消该 slot 的定时器
        slots_.clear()                            // 在本 loop 析构
        server_->on_worker_stopped()              // workers_wg_.done()
     某 slot 的 deadline 触发时：该 slot worker->abort()   // 硬中断，wait_stopped 随后完成

on_worker_stopped() / on_serve() 结束      [任意 loop -> 汇聚到 owner loop]
  8. serve_wg_（accept loops）与 workers_wg_ 都空后：
        for e in endpoints_: e 释放（unique_ptr 析构，listener 已关）
        state = Stopped
        shutdown_wg_.done()   -> serve() 与所有 stop_and_wait() 的 joiner 恢复
```

第 8 步的汇聚由 owner loop 上一个 `finish_shutdown()` 协程完成：

```cpp
async::DetachedTask Server::finish_shutdown(Server *self) noexcept {
    co_await self->serve_wg_.join();     // 所有 endpoint 的 accept loop 已返回
    co_await self->workers_wg_.join();   // 所有 worker 的 slot 已析构
    self->endpoints_.clear();            // Endpoint 本体在 owner loop 析构
    self->state_.store(State::Stopped, std::memory_order_release);
    self->shutdown_wg_.done();
    co_return;
}
```

**drain_timeout 的边界（实现期确认）**：`abort()` 能兜住的是**阻塞的 I/O**——关掉 transport 后 handler 挂起的读写立刻失败并返回。它兜不住「handler 因为自身原因永不返回」（纯定时器、死循环）：C++ 协程没有取消机制，这类连接协程仍会拖住 `wait_stopped()`。这与现状一致（今天 `HttpServer` 的 `tasks` WaitGroup 同样会无限等），不是本次重写引入的退化，但必须在 `EndpointWorker::abort()` 的注释里写明，避免被当成「预算一定生效」的强保证。

**顺序保证**：`slots_` 先于 `endpoints_` 析构。`EndpointWorker` 可能持有指向 `Endpoint` 的裸指针（读 options/handler），这个顺序保证了指针始终有效。`Endpoint` 持有的 `std::vector<XxxEndpointWorker *>` 在第 8 步时已全部悬空，但那之后不再有人 accept，不会被解引用（`on_stop()` 之后 accept loop 已退出）。

### 6.4 与 EventLoopGroup 的关系

调用方的正确停机顺序：

```cpp
// 业务线程 / 信号处理
server.stop();

// owner loop 上：
co_await server.serve();     // 或 co_await server.stop_and_wait();
// 到这里所有连接与 worker 资源已释放
owner_loop.stop();
worker_group.stop();
worker_group.join();
```

`Server` 不会去 `stop()` 任何 loop。这条约束让 `Server` 可以和别的组件（连接池、DNS）共享同一个 `EventLoopGroup`。

### 6.5 析构约定

`~Server()` 断言 `state == Created || state == Stopped`。这是「Nullability at Edges」的落法：把「必须先停完」变成构造/析构边界上的显式不变式，而不是像现在的 `~HttpServer()` 那样发一个不等待的 `request_close()`，把风险留给调用方。lite_nginx 里那段 `std::promise` + 手动 `accept_loop_.run()` 的兜底代码因此可以删掉。

---

## 7. Endpoint 实现

### 7.1 配置：拆散 `HttpServerOptions`（D5）

现在的 `HttpServerOptions` 是一个「三种协议 + TLS + QUIC 全塞在一起」的结构：任何一个 endpoint 拿到它，都有一大半字段与自己无关，而且哪些字段真正生效完全看不出来。按 D5，**删除 `HttpServerOptions`**，每个 endpoint 定义自己的 options，只留自己真正读的字段。

先把「谁在读哪个字段」查清楚（结论来自对 `src/http/` 的逐字段核对）：

| 字段 | H1 | H2 | H3 | 说明 |
|------|:--:|:--:|:--:|------|
| `header_init_size` / `header_large_size` / `header_large_num` | ✅ | ❌ | 只用 `header_large_size` | H1 在 `Http1Connection`/`Http1ExchangeIo` 里建头部 arena |
| `header_timeout` | ✅ | ❌ | ❌ | 只有 H1 的 `read_into(..., header_timeout)` |
| `keep_alive_timeout` | ✅ | 作为 `read_timeout` | ❌ | H2 里语义其实是「连接读空闲超时」，改名更准确 |
| `body_timeout` | ❌ | ❌ | ✅ | **H1 从来没读过这个字段**，只有 `ServerHttp3Request` 用 |
| `write_timeout` | ✅ | ✅ | ❌ | |
| `drain_unread_body` | ✅ | ❌ | ❌ | |
| `enable_extended_connect` | ❌ | ✅ | ✅ | 转成 `Http2Connection::Options::enable_connect_protocol` / `Http3Settings` |
| `tcp` (`net::TcpSocketOptions`) | ✅ | ✅ | ❌ | 只有 TCP endpoint 建 transport 时用 |
| `tls` (`HttpServerTlsOptions`) | ✅ | ✅ | ✅ | 三者 ALPN 集合不同（`make_*_server_tls_param`） |
| `http3.*` | ❌ | ❌ | ✅ | 整块只属于 H3 |

> 另一处发现：`ServerHttp2Request` 的构造函数收了 `const HttpServerOptions &`，但**一个字段都没用到**（构造初始化列表里根本没引用）。拆分时这个参数连同 `ServerRequestFactory::http_options_` 一起删掉。

拆分后：

```cpp
// —— H1 连接层真正需要的 —— 由 Http1Connection / Http1ExchangeIo 直接消费
struct Http1ServerOptions {
    std::chrono::seconds keep_alive_timeout{70};
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds write_timeout{30};
    std::size_t header_init_size = 8 * 1024;
    std::size_t header_large_size = 32 * 1024;
    std::size_t header_large_num = 4;
    bool drain_unread_body = false;
};

// —— H2 连接层 —— 由 endpoint 转成 Http2Connection::Options
struct Http2ServerOptions {
    std::chrono::milliseconds read_timeout{70'000};   // 旧 keep_alive_timeout，语义更准
    std::chrono::milliseconds write_timeout{30'000};
    bool enable_connect_protocol = false;             // 旧 enable_extended_connect
};

// —— H3 —— 旧 HttpServerOptions::Http3Options 原样 + 它自己用到的两个字段
struct Http3ServerOptions {
    std::size_t max_connections_per_shard = 1024;
    std::size_t retained_storage_limit = quic::kQuicDefaultEndpointRetainedStorageLimit;
    net::UdpBindOptions udp{};
    quic::QuicSendScheduler::Options send{};
    quic::QuicTransportSettings transport{};
    quic::QuicRecvFlowControlSettings recv_flow{};
    Http3Settings settings{};
    std::chrono::milliseconds keepalive_interval{0};
    std::chrono::milliseconds max_ack_delay{25};
    std::chrono::seconds body_timeout{60};
    std::size_t header_large_size = 32 * 1024;
    std::uint64_t ack_delay_exponent = 3;
    bool enable_connect_protocol = false;
    bool retry = false;
    bool issue_new_token = false;
    bool enable_early_data = false;
};
```

各 endpoint 的 options 就是「监听信息 + 自己协议的连接层 options + handler + drain_timeout」：

```cpp
struct Http1Endpoint::Options {
    net::SocketAddress address{};
    net::ListenOptions listen{};
    net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
    HttpServerTlsOptions tls{};             // 不设即明文
    Http1ServerOptions http1{};
    HttpHandler handler{};                  // 空则取 Server 的默认 handler
    std::chrono::milliseconds drain_timeout{30'000};
};

struct Http2Endpoint::Options {            // 同上，把 http1 换成 http2
    ...
    Http2ServerOptions http2{};
};

struct Http21Endpoint::Options {           // ALPN 协商，两套都要
    ...
    Http1ServerOptions http1{};
    Http2ServerOptions http2{};
};

struct Http3Endpoint::Options {
    net::SocketAddress address{};
    const Endpoint *inherit_port_from = nullptr;   // 见 §7.6
    HttpServerTlsOptions tls{};                    // H3 必须有 TLS，on_start 校验
    Http3ServerOptions http3{};
    HttpHandler handler{};
    std::chrono::milliseconds drain_timeout{30'000};
};
```

同名字段（`header_large_size`、`write_timeout` 等）在不同结构里重复出现是刻意的：它们本来就是各协议独立的旋钮，共用一个字段反而制造了「改一个动三处」的隐式耦合。

`start()` 时把最终 handler 固化成 `std::shared_ptr<const HttpHandler>`（沿用 `ServerRequestFactory` 的做法），使连接在 endpoint 门面析构后仍可安全持有。

### 7.2 TcpEndpointBase（H1 / H2 / H21 共用）

```cpp
class TcpEndpointBase : public Endpoint {
protected:
    // Endpoint 实现
    common::IoResult<void> on_start(Server &server) noexcept override;   // listener_.bind + 记录 local_addr_
    async::Task<void> on_serve() noexcept override;                      // accept loop
    void on_stop() noexcept override { listener_.close(); }
    const net::SocketAddress &local_addr() const noexcept override { return local_addr_; }

    // 子类实现：连接已完成 transport（含 TLS 握手），交给某个 worker
    virtual void dispatch(std::size_t worker_index, std::unique_ptr<HttpTransport> transport) noexcept = 0;
    // 子类实现：TLS ALPN 集合（决定 make_*_server_tls_param 用哪一个）
    virtual net::TlsServerParam make_tls_param() const noexcept = 0;

    net::TcpListener listener_;
    net::SocketAddress local_addr_{};
    std::atomic<std::size_t> next_worker_{0};
    async::WaitGroup handshakes_{};   // 握手中、尚未 dispatch 的连接
};
```

accept loop（owner loop）：

```
while (listener_.valid()):
    accept_result = co_await listener_.accept()
    if error is Canceled/BadFd: break
    if error otherwise: continue
    if server_->draining(): drop(fd); continue
    idx = next_worker_++ % worker_count
    handshakes_.add()
    spawn(worker_loop(idx), on_accepted(this, idx, std::move(accept)))
```

`on_accepted`（worker loop）：建 `TcpTransport` 或 `TlsTransport` + `co_await handshake(...)`，成功后 `dispatch(idx, std::move(transport))`，最后 `handshakes_.done()`。

> **握手期连接的归属**：握手尚未完成的连接不在任何 `EndpointWorker` 的连接表里，因此 `EndpointWorker::wait_stopped()` 必须同时等 `handshakes_` 归零，否则会出现「worker 报告停完、随后又冒出一条新连接」的竞态。实现上：`handshakes_` 拆成 per-worker 的 `async::WaitGroup`（放在 `EndpointWorker` 里），`wait_stopped()` 先 join 握手组、再 join 连接组；`drain()` 时置位 `draining_`，`on_accepted` 在 dispatch 前检查该位，命中就直接关掉 transport。

**与现状的差异**：现在这段逻辑写在 `HttpServer::handle_connection()` 里，用一个全局 `runtime->tasks` WaitGroup 兜住所有异步任务；重写后分解为 per-worker/per-endpoint 的两级 WaitGroup，停机时可以按 endpoint 粒度报告进度。

### 7.3 Http1Endpoint

`EndpointWorker` 实现：

```cpp
class Http1EndpointWorker final : public EndpointWorker {
public:
    void link(Http1Connection &c) noexcept { connections_.push_back(c); }
    void unlink(Http1Connection &c) noexcept { connections_.erase(c); }

    void drain() noexcept override {
        draining_ = true;
        for (auto *c = connections_.front(); c != nullptr; c = connections_.next_of(*c)) {
            c->request_drain();     // 空闲则立刻关，忙则打标记
        }
    }
    void abort() noexcept override {
        aborted_ = true;
        for (auto *c = connections_.front(); c != nullptr; c = connections_.next_of(*c)) {
            c->shutdown();          // 硬关 transport
        }
    }
    async::Task<void> wait_stopped() noexcept override {
        co_await handshakes_.join();
        co_await connections_.wg().join();
    }
private:
    common::IntrusiveList<Http1Connection, offsetof(Http1Connection, worker_hook_)> connections_;
    async::WaitGroup handshakes_{}, live_{};
    bool draining_ = false, aborted_ = false;
};
```

对 `Http1Connection` 的改造：

| 现状 | 改为 |
|------|------|
| `Http1Server *server_` 反向指针 + `const std::atomic<bool> *shutdown_flag_` | 删除两者，改为 loop-affine 的 `bool draining_` |
| `stopping()` 读 `server_->shutting_down()` 或原子标志 | `return draining_;`（同 loop，无原子操作） |
| `std::atomic<bool> finished_` | 普通 `bool finished_`（只在自己的 loop 上访问） |
| `shutdown()` = 立刻关 transport | 保留为硬中断入口；新增 `request_drain()` |
| 无注册表钩子 | 新增私有 `common::IntrusiveListHook worker_hook_`，`friend class Http1EndpointWorker`（照抄 `Http2ServerConnection::worker_hook_` 的写法） |

```cpp
void Http1Connection::request_drain() noexcept {
    FIBER_ASSERT(loop_.in_loop());
    if (draining_) return;
    draining_ = true;
    if (idle_) {           // 阻塞在 wait_readable 等下一个请求 -> 直接关，唤醒读
        finish();
    }
    // 否则：run() 的循环在本次 exchange 结束后看到 draining_ 退出
}
```

`idle_` 在 `run()` 里 `co_await transport_->wait_readable(...)` 前后置位/清零。

**响应头**：`Http1ExchangeIo::compute_close_conn()` 已经读 `connection_->stopping()`，所以 drain 期间新完成的响应会自动带 `Connection: close`——这一条现状就是对的，改造后语义不变（`stopping()` 的实现从「查 server 原子标志」变成「读本地 bool」）。

### 7.4 Http2Endpoint / Http21Endpoint

复用现有的 `Http2ServerConnection` + `Http2ServerWorker`：`Http2EndpointWorker` 就是把 `Http2ServerWorker` 换成继承 `EndpointWorker`，并补上两点：

```cpp
void drain() noexcept override {
    draining_ = true;
    for (auto *c = connections_.front(); c; c = connections_.next_of(*c)) {
        c->request_drain();     // 新增：conn_.graceful_shutdown() —— 发 GOAWAY，已开流跑完
    }
}
void abort() noexcept override {
    for (auto *c = connections_.front(); c; c = connections_.next_of(*c)) {
        c->request_shutdown();  // 现有：conn_.shutdown(Canceled)
    }
}
```

`Http2ServerConnection` 新增 `request_drain()`（调 `Http2Connection::graceful_shutdown()`，该函数已存在）。`ServerRequestFactory` 同步瘦身：`ServerHttp2Request` 压根没读过 `HttpServerOptions` 的任何字段，构造参数和 `http_options_` 成员一起删掉，只留 handler。`claim_close_walk()/release_close_walk()` 这套「合并重复 walk」的机制可以删掉——新设计里 drain 每个 worker 只走一次（由 `Worker::stop_posted_` 保证），不再有重复投递。

- `Http2Endpoint`：TLS 时 ALPN 只报 `h2`；明文时按 **h2c prior-knowledge** 处理——直接把 transport 喂给 `Http2Connection`，由它校验 connection preface。**不支持 `Upgrade: h2c`（D6）**：明文端口上收到带 `Upgrade: h2c` 的 HTTP/1 请求，就按普通 HTTP/1 请求处理（忽略该头），不做协议升级。这一条要写进 `Http2Endpoint` 的类注释，避免后来者误以为是漏实现。
- `Http21Endpoint`：TLS ALPN 报 `{h2, http/1.1}`，按协商结果分派到 H1 或 H2 的 worker slot；明文时走 HTTP/1。这就是今天 `HttpServer` 的行为，`select_protocol()` 逻辑原样搬过来。

`Http21EndpointWorker` 内部同时持有 H1 和 H2 两张连接表，`drain/abort/wait_stopped` 依次作用于两者。

### 7.5 Http3Endpoint

由 `Http3Server` 改造而来，`Shard` 直接变成 `EndpointWorker`：

```cpp
class Http3EndpointWorker final : public EndpointWorker {
    quic::QuicUdpEndpoint endpoint_;                 // per-worker，SO_REUSEPORT
    common::IntrusiveList<Http3ServerConnection, ...> connections_;  // 新增（现在只有计数）
    async::WaitGroup live_{};
    bool admitting_ = true;
};
```

- `on_start`：为每个 worker 建一个 shard（`endpoint_.init(worker_loop, ...)`）；`worker_count > 1` 时开 `reuse_port`；端口 0 时第一个 shard 绑定后把实际端口回填给其余 shard（沿用 `Http3Server::bind()` 现逻辑）。
- `on_serve`：给每个 shard 的 loop `post` 一次 `endpoint_.start()`；然后 `co_await` 一个直到 `on_stop()` 才完成的 Signal（H3 没有 accept 循环）。
- `drain()`：`admitting_ = false`（`create_connection` 回调看到后返回空 lease，QUIC 层拒绝新连接），并遍历连接表逐个 `h3_.graceful_shutdown()` 发 GOAWAY。**保持 UDP socket 打开**，否则现有连接收不到包。
- `abort()`：`endpoint_.close()`（会 `force_detach_connection` 掉所有连接并关 socket）。
- `wait_stopped()`：`co_await live_.join()`；返回前若 socket 仍开着则 `endpoint_.close()`。

这是相对现状的实质性行为修正：今天 `Http3Server::close()` 第一时间就 `endpoint.close()`，等于把所有 QUIC 连接就地掐断。

**新增的连接表**：现在 `Http3Server` 只有一个 `WaitGroup connections`，没法遍历发 GOAWAY。给 `ServerConnection` 加一个侵入式钩子挂到所属 shard 上（写法同 `Http2ServerWorker`）。

### 7.6 同端口 TCP + UDP（Alt-Svc 场景）

今天由 `HttpServer::bind()` 内部隐式建 `Http3Server` 保证「TCP 和 UDP 用同一个端口」；拆成两个 endpoint 后要显式表达，尤其是端口 0（测试里普遍用）：

```cpp
auto *tcp = server.add_endpoint<Http21Endpoint>(Http21Endpoint::Options{
    .address = {ip, 0}, .tls = tls_options, ...});
auto *h3  = server.add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
    .address = {ip, 0},
    .inherit_port_from = tcp,      // 自己端口为 0 且此项非空时，取 tcp->local_addr().port()
    .tls = tls_options, ...});
```

`Server::start()` 按插入顺序调用 `on_start()`，所以 `inherit_port_from` 指向的 endpoint 一定已经绑定完成。`Http3Endpoint::on_start()` 里断言这一点。

---

## 8. 线程与所有权规则（实现者须遵守）

| 对象 | 创建 | 使用 | 销毁 |
|------|------|------|------|
| `Server` | 调用方（通常主线程/owner loop） | `add_endpoint`/`start`/`serve` 必须在 owner loop；`stop`/`stop_and_wait`/`state` 任意线程 | 调用方，须处于 `Created`/`Stopped` |
| `Endpoint` | owner loop（`start()` 前） | `on_start/on_serve/on_stop` 只在 owner loop | owner loop，`finish_shutdown` 第 8 步 |
| `EndpointWorker` | owner loop（`start()` 中构造） | 仅在自己的 worker loop | 自己的 worker loop（`Worker::run_wait_stop`） |
| 连接对象 | worker loop | worker loop | worker loop |
| `handler` | 调用方 | 任意 worker loop（`shared_ptr<const HttpHandler>`，handler 自身必须线程安全） | 最后一个持有者 |

跨线程通信只有三处，全部走 `EventLoop::post` + 内嵌 `NotifyEntry`（零分配）：
1. `Server::stop()` → owner loop 的 `on_owner_stop`；
2. `Worker::notify_stop()` → worker loop 的 `on_stop`；
3. accept 后把连接投递到 worker loop（`async::spawn`）。

计数/屏障只有三个 `WaitGroup`：`serve_wg_`（accept loop 数）、`workers_wg_`（worker 数）、`shutdown_wg_`（终态屏障），加上每个 `EndpointWorker` 内部的握手/连接组。全部只在冷路径或每连接一次，不进请求热路径。

---

## 9. 对现有代码的改动清单

**新增**
- `include/fiber/http/Server.h`、`Http{1,2,3}ServerOptions.h`、`endpoint/Http{1,2,21,3}Endpoint.h`
- `src/http/Server.cpp`、`src/http/endpoint/*.cpp`

**修改**
- `Http1Connection`：删 `server_`（P2 完成），`finished_` 去原子化，新增 `worker_hook_`、`idle_`、`draining_`、`request_drain()`；handler 改为 `const HttpHandler *` + 可选 `shared_ptr` owner（去掉每连接一次 `std::function` 拷贝）。`shutdown_flag_` 保留到 P7——`HttpServer` 门面仍靠它驱动 H1 停机，`stopping()` 期间读 `draining_ || flag`。
- `Http1ExchangeIo`：逻辑无需改（`compute_close_conn` 已经读 `connection_->stopping()`），只跟随 options 类型替换。
- `Http1Parser`：`RequestLineParser`/`HeaderLineParser` 的 `HttpServerOptions` 参数是死参数（一个存了指针从未读，一个直接忽略），P2 顺手删掉。
- `Http2ServerConnection`：新增 `request_drain()` → `graceful_shutdown()`。
- `Http2ServerWorker`：改名 `Http2EndpointWorker` 并实现 `EndpointWorker`；删 `claim_close_walk/release_close_walk`。
- `ServerRequestFactory` / `ServerHttp2Request`：删掉从不读取的 `HttpServerOptions` 参数与成员，只留 handler。
- `ServerHttp3Request`：`HttpServerOptions` 参数换成 `Http3ServerOptions`（只读 `header_large_size`、`body_timeout`）。
- `Http1Connection` / `Http1ExchangeIo`：`HttpServerOptions` 参数换成 `Http1ServerOptions`。
- `Http3Server` 的 `ServerConnection`：新增 shard 侵入式钩子；`create_connection` 的准入判断从 `runtime_->shutting_down` 改为 per-shard `admitting_`。
- `HttpServer`：P5 内部实现替换为对 `Server` 的委托，P7 整体删除（见 §10）。
- `lite_nginx` `make_server_options()`：按 endpoint 类型分别构造 `Http1/2/3ServerOptions`。

**删除**
- `include/fiber/http/HttpServerOptions.h`（拆成 `Http{1,2,3}ServerOptions.h`，见 §7.1）；
- `HttpServer::Runtime` 里的 `http1_connections` / `connections_mutex` / `Http1Entry` / `shutdown_http1_entry`；
- `include/fiber/http/Http1Server.h` + `src/http/Http1Server.cpp`（能力被 `Server + Http1Endpoint` 完全覆盖）；
- `include/fiber/http/Http3Server.h` + `src/http/Http3Server.cpp`（并入 `Http3Endpoint`）；
- `ServerLauncher::close()` 里的 `std::promise` + 手动 `accept_loop_.run()` 兜底段；
- `include/fiber/http/HttpServer.h` + `src/http/HttpServer.cpp`（P7，D7）。

---

## 10. 兼容与迁移

现存直接依赖（`src/` 之外）：`HttpServer.h` 12 处（tests 5 / example 3 / apps 4）、`Http1Server.h` 3 处（tests 1 / example 2）、`Http3Server.h` 2 处（tests 1 / lite_nginx test 1），去重后 16 个文件。一次性全改风险大，因此 `HttpServer` 作为**迁移期脚手架**保留到 P6 结束，P7 连同它的头文件一起删除（D7）——它不进入长期 API。

**P5 起 `HttpServer` 变成薄门面**（内部就是 `Server` + 一个 `Http21Endpoint`，`options.http3.enabled` 时再加一个 `inherit_port_from` 的 `Http3Endpoint`）。因为 `HttpServerOptions` 在 §7.1 被拆掉，门面自己保留一份等价的旧字段结构，在 `bind()` 里翻译成各 endpoint 的 options：



| 旧 API | 新实现 |
|--------|--------|
| `HttpServer(loop, handler, options, group)` | 存参数，构造内部 `Server` |
| `bind(addr, listen_options)` | `add_endpoint(...)` × 1~2 + `Server::start()` |
| `serve()` → `DetachedTask` | `spawn(owner_loop, [&]{ return server_->serve(); })` |
| `close()` / `request_close()` | `Server::stop()` |
| `shutdown_and_wait()` | `Server::stop_and_wait()` |
| `fd()` | 内部 TCP endpoint 的 listener fd |
| `state()` | `Server::State` 映射（`Created/Bound/Running/Closing/Closed`） |

这样 12 处 `HttpServer` 调用点在 P1–P6 期间无改动继续编译，把「换骨架」和「换调用点」两件事分开验证。`Http1Server`（3 处）/ `Http3Server`（2 处）没有门面，在 P2/P4 直接改写为 `Server + Endpoint`。

**实施阶段**

| 阶段 | 内容 | 验收 |
|------|------|------|
| P1 | `Server`/`Endpoint`/`EndpointWorker`/`Worker` 骨架 + 状态机；测试用的假 endpoint | `ServerLifecycleTest` 全绿 |
| P2 | `TcpEndpointBase` + `Http1Endpoint`；`Http1Connection` drain 改造；拆出 `Http1ServerOptions` | `Http1EndpointTest`（真实 H1 流量 + drain/abort）全绿；`Http1Server`/`HttpServer` 走桥接保持不变 |
| P3 | `Http2Endpoint` + `Http21Endpoint`；H2 GOAWAY drain | `Http2ConnectionTest`/`HttpClientServerInteropTest` 全绿 |
| P4 | `Http3Endpoint`（吸收 `Http3Server`）+ H3 GOAWAY drain + 端口继承 | `Http3ClientTest`/`Http3ConnectionTest` 全绿 |
| P5 | `HttpServer` 门面切到 `Server`；删 `Http1Server`/`Http3Server`（迁移其 5 处调用点）；`HttpServerOptions` 余下部分拆分落地 | 全量 `ctest` 绿 |
| P6 | lite_nginx 收敛为「1 个 `Server` + N 个 endpoint」，删 `ServerLauncher::close()` 的 promise 兜底 | `lite_nginx_tests` 全绿 + 一轮基准回归 |
| P7 | 12 处 `HttpServer` 调用点改写为 `Server + Endpoint`；删 `HttpServer.h/.cpp`（D7） | 全量 `ctest` 绿；仓库内不再有 `HttpServer` 引用 |

每个阶段独立成一个 `refactor(http): ...` 提交，`ctest --test-dir build` 必须绿。

---

## 11. 测试计划

**生命周期（`tests/ServerLifecycleTest.cpp`，新增）**
- 空 endpoint 列表 → `start()` 返回 `Invalid`
- 第 2 个 endpoint bind 失败 → 第 1 个已 `on_stop()`，端口已释放，状态回到 `Created`
- `stop()` 在 `Created` / `Started` / `Serving` / `Draining` 各状态下调用，均幂等
- 从非 owner 线程 `stop()`，`serve()` 正常返回
- 多个 `stop_and_wait()` joiner 同时等待，全部被唤醒
- `serve()` 返回后 `~Server()` 不触发断言；worker slot 确已析构（析构计数器）

**优雅 drain**
- H1：请求处理中（handler 里挂 500ms）触发 `stop()` → 响应完整送达且带 `Connection: close`，之后连接关闭
- H1：空闲 keep-alive 连接在 `stop()` 后 <10ms 内被关闭（不等 `keep_alive_timeout`）
- H2：`stop()` 后客户端收到 GOAWAY；已开流的响应完整；新建流被拒
- H3：`stop()` 后新连接握手被拒；已有连接的 in-flight 请求完成
- `drain_timeout` = 100ms + handler 阻塞在读请求体（客户端声明 Content-Length 但不发） → 到期后 abort 关 transport，读失败返回，`serve()` 在预算内返回
- `drain_timeout` = 0 → 等价硬停，行为与今天的 `HttpServer::close()` 一致

**多 endpoint**
- 一个 Server 上 3 个 endpoint（明文 H1 / TLS H21 / H3），各自独立 handler，请求分别命中正确 handler
- H3 endpoint 通过 `inherit_port_from` 与 TCP endpoint 共用端口 0 分配到的端口

**回归**：现有 `HttpServerLifecycleTest`、`HttpServerTlsDynamicCertTest`、`Http1ConnectionTest`、`Http2ConnectionTest`、`HttpClientServerInteropTest`、`Http3ClientTest`、`LiteNginxRuntimeTest` 全部保持绿。

---

## 12. 风险与后续

| 风险 | 缓解 |
|------|------|
| P4 的 `Http3Server` 吸收改动最大，QUIC 侧 drain 是新行为 | 先在 P4 内单独加 H3 drain 测试；`abort()` 路径保持与今天的 `close()` 完全一致，最坏情况退化为现状行为 |
| 握手中连接与 worker 停机的竞态 | §7.2 的 per-worker 握手 WaitGroup + dispatch 前二次检查 `draining_`；补一个「drain 与 accept 并发」的压力测试 |
| `Endpoint` 析构晚于 `EndpointWorker` 的顺序依赖 | 在 `finish_shutdown` 里用注释固化，并在 `~Endpoint()` 断言自己的 worker 指针数组已清空 |
| 门面期 `HttpServer` 与 `Server` 并存造成理解成本 | P5 起在 `HttpServer.h` 顶部标注「迁移期脚手架，新代码请用 `Server`，将于 P7 删除」；P7 必须真的删掉，不留长期双轨（D7） |
| 拆 `HttpServerOptions` 会横扫 `Http1Connection`/`ServerHttp{2,3}Request`/lite_nginx | 字段归属已逐个核对（§7.1 表格）；拆分放在 P5 一次做完，且是纯机械替换——字段值语义不变，只有 `keep_alive_timeout`→`Http2ServerOptions::read_timeout` 改了名 |

**后续可做（本次不做）**
- 把 `Server/Endpoint/EndpointWorker` 抽到 `fiber::server`，供非 HTTP 协议复用
- accept 模型可选 `SO_REUSEPORT` 分片（D2 暂定单 accept loop；接口上 `TcpEndpointBase` 已把「选 worker」收敛到一处，将来换分片只动这一处）
- 每 endpoint 的连接数/请求数计数器，`EndpointWorker` 上加一个 `stats()` 虚函数即可

---

## 13. 评审记录

| 议题 | 结论 | 落点 |
|------|------|------|
| Endpoint 用虚接口还是 Ops 函数表 | 虚接口（D1） | §5.2 |
| TCP accept 是否 `SO_REUSEPORT` 分片 | 保持单 accept loop + 分发（D2），分片列入后续 | §7.2、§12 |
| 停机是否等 in-flight 请求 | 优雅 drain + 超时兜底（D3） | §6.3、§7.3–7.5 |
| handler 是 server 级还是 endpoint 级 | endpoint 级，server 的作默认值（D4） | §5.3、§7.1 |
| 配置结构 | 每个 endpoint 自己的 options，拆散 `HttpServerOptions`，无 server 级 options（D5） | §7.1 |
| 明文 H2 是否支持 `Upgrade: h2c` | 不支持，只做 prior-knowledge（D6） | §7.4 |
| `HttpServer` 门面保留多久 | 仅迁移期脚手架，P7 直接删（D7） | §10 |

当前无待确认项。实现期若发现新的取舍，在此追加行。
