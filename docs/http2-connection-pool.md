# HTTP/2 Connection Pool 使用文档

## 1. 设计目标

`Http2ConnectionPoolCore` 在单个 EventLoop 内管理 HTTP/2 客户端连接。同组优先使用可用链表头部连接，达到并发上限时摘链，释放容量时重新放到头部。未饱和连接释放请求不改变顺序，让尾部连接自然空闲并被淘汰。

池管理全部存活连接，包括正在拨号、正在处理请求和正在关闭的连接。Lease 占用一个请求名额，同一连接可以同时被多个 Lease 使用。请求重试、负载均衡、ALPN 分流和跨 loop steal 不在本池的职责中。

## 2. 分层概览

- `HttpConnectionGroupKey`、`HttpConnectionPoolAffinity`、`HttpConnectionBucketIndex` 为 H1/H2 共用组件，原 `Http1` 前缀名称已移除。两种协议的池实例相互独立。
- `Http2ConnectionPoolCore` 管理一个 loop 的连接、FIFO 等待队列、拨号限制和空闲定时器。
- `Http2PooledExchange` 组合一个 Lease 和一个 `ClientHttp2Exchange`，建议请求层统一使用。
- `LocalHttp2ConnectionPoolSet` 为 EventLoopGroup 中每个 loop 内联构造一个 core，按当前 loop 路由。

HTTP/2 底层由 `Http2LocalStreamGate` 提供流准入和容量通知，由 `Http2CloseGate` 将关闭通知先派发给池观察者，再唤醒协程 joiner。

## 3. 分组与拨号

Key 包含主机身份、端口、scheme 和 affinity。TLS 客户端身份或传输 profile 不同时，必须使用不同 affinity；相同 key 的新请求可能直接复用既有连接，传入的新 connector 不会被执行。

Connector 是函数指针和 `void *ctx`，不在池中长期保存。参数必须在 acquire 完成前有效。回调成功返回前必须完成 `Http2ClientConnection::connect()`，启动 HTTP/2 I/O；拨号地址、TCP/TLS 参数由回调负责。

acquire 按值持有 key 和 Connector，因此支持临时参数，也不会在 bucket index 扩容时保留失效的 key 引用。`Connector::ctx` 指向的数据仍由调用者管理。

## 4. Lease 与 PooledExchange

`try_acquire(key)` 是同步的纯复用快路径：命中已有可用连接就返回 Lease，否则返回 `std::nullopt`。它不拨号、不挂起、不创建分组，也不会抢占已排队的等待者，因此按引用取 key、不构造协程帧。稳态复用可以先走它，miss 再 `co_await acquire()`。

`acquire()` 成功后返回可移动、不可复制的 Lease。`connection()` 返回共享连接，`key()` 从所属 bucket 获取 key；该引用只应在当前同步操作内使用，其他组插入、删除或扩容会使引用失效。

每个 Lease 对应一个 exchange。不要在同一 Lease 上创建多个并行 exchange，也不要绕过池在该连接上额外创建请求。使用裸 `Lease::open_exchange()` 时，须在响应完成或请求取消后先销毁 exchange，再 reset Lease。

`Http2PooledExchange` 将这个顺序封装起来。析构、reset 和移动赋值都会先取消尚未完成的流、释放 exchange，再归还名额。仅析构原始 `ClientHttp2Exchange` 并不会取消仍由连接持有的流。

Lease 本身在连接关闭后仍然有效，直到 reset；随后的读写错误由 exchange 返回。关闭观察者只摘链，最后一个 Lease 归还后通过 `post_local` 延迟销毁连接。池必须比所有 Lease、acquire 协程和 join 协程活得更久。

## 5. Core 的使用

```cpp
#include <fiber/http/Http2PooledExchange.h>

using namespace fiber;

struct Endpoint {
    net::SocketAddress address;
    static async::Task<common::IoResult<void>> dial(
            void *ctx, http::Http2ClientConnection &conn,
            const http::HttpConnectionGroupKey &) noexcept {
        auto &endpoint = *static_cast<Endpoint *>(ctx);
        co_return co_await conn.connect(endpoint.address, std::chrono::seconds(3));
    }
};

async::Task<common::IoResult<void>> request(
        http::Http2ConnectionPoolCore &connections, Endpoint &endpoint,
        http::HttpConnectionGroupKey key, mem::BufPool &buffers) noexcept {
    auto lease = co_await connections.acquire(
        key, {&Endpoint::dial, &endpoint}, std::chrono::seconds(5));
    if (!lease) co_return std::unexpected(lease.error());

    http::Http2PooledExchange exchange(std::move(*lease), buffers);
    auto sent = co_await exchange->send_request_header(
        {.method = http::HttpMethod::Get, .scheme = "http",
         .authority = "localhost", .path = "/"}, true, std::chrono::seconds(5));
    if (!sent) co_return std::unexpected(sent.error());
    auto head = co_await exchange->read_header(std::chrono::seconds(5));
    if (!head) co_return std::unexpected(head.error());
    // 实际业务在这里处理响应头、读取响应体；提前退出会取消剩余流。
    co_return common::IoResult<void>{};
}
```

初始化时构造 `Http2ConnectionPoolCore(loop, options)` 并调用 `init()`，检查其返回值。请求、release、clear、shutdown 和 join 均运行于所属 loop。init 在发布给工作线程前调用。

### 5.1 容量与默认选项

| 选项 | 默认值 | 含义 |
|---|---:|---|
| `max_streams_per_connection` | 0 | 每连接软上限，0 表示不限 |
| `pre_settings_max_streams` | 16 | 首次完整 SETTINGS 前的保守上限，0 表示不发名额、等待 SETTINGS |
| `max_streams_lifetime` | 0 | 累计归还名额数达到该值后退休，0 表示不限 |
| `max_connections_per_group` | 4 | 单组全部存活连接上限，含连接中和 draining |
| `max_connections_total` | 64 | 全部组的存活连接上限 |
| `max_concurrent_dials_per_group` | 1 | 单组同时拨号数，默认单飞 |
| `max_idle_total` | 16 | active leases 为 0 的连接上限，0 表示不缓存空闲连接 |
| `idle_timeout` | 60 秒 | 空闲连接退休期限，必须大于 0 |
| `dial_retry_backoff` | 10ms | 拨号失败后该组的重试退避起点，必须大于 0 |
| `max_dial_retry_backoff` | 1s | 退避上限，每次连续失败翻倍直到该值，不得小于起点 |
| `initial_group_capacity` | 0 | bucket index 初始分组容量 |
| `h2` | 默认 H2 options | 每条连接的 HTTP/2 配置 |

`max_connections_total`、`max_connections_per_group` 和 `max_concurrent_dials_per_group` 必须至少为 1，`max_idle_total` 必须不超过总连接上限，`max_dial_retry_backoff` 必须不小于 `dial_retry_backoff`。

收到 SETTINGS 后，容量为对端宣告值与池软上限的较小值。首次 SETTINGS 完成会通知池切换容量口径。`pre_settings_max_streams` 为 0 时，已就绪但尚未收到 SETTINGS 的连接容量为 0，该组在此期间不再拨新连接，请求等待这条连接的 SETTINGS 到达；这与配置非 0 值时「先按保守上限发名额」是两种取舍。对端缩小上限时，已发出的 Lease 不会被撤销；其尚未 attach 的请求由 stream gate 等待，使用请求头发送超时限制等待时间。

### 5.2 等待、超时与错误

| 场景 | 结果 |
|---|---|
| `timeout <= 0`，有可用名额且无等待者 | 立即取得 Lease |
| `timeout <= 0`，无可用名额 | `Busy`，不会启动拨号 |
| `timeout == milliseconds::max()` | 无限等待 |
| 有限 deadline 到期，包括拨号期间 | `TimedOut` |
| clear、shutdown 或外部 shutdown flag 生效 | `Canceled` |
| bucket/entry 分配失败 | `NoMem` |
| 需要拨号但 Connector 函数为空 | `Invalid` |

等待者保持 FIFO 顺序直到实际取得名额，新请求不会抢占已唤醒的等待者。拨号成功时，拨号请求先取得一个名额，其余等待者共享剩余容量。

拨号失败不会将错误广播给其他请求；失败请求也继续等待，按组退避后重试，直到成功、取消或超时。退避从 `dial_retry_backoff` 起，每次连续失败翻倍到 `max_dial_retry_backoff`，一次成功即清零。**`timeout` 取默认的 `milliseconds::max()` 时，持续失败的 endpoint 会一直重试而不返回**，因此必须用 `set_dial_failed_callback(cb, ctx)` 观测；回调在每次拨号失败时同步触发，参数为 key、错误码、该组连续失败次数和下次重试的退避时长。

Connector 的挂起操作必须支持协程帧析构取消：池会在 deadline、clear、shutdown 或调用方销毁 acquire 协程时销毁未完成的拨号协程。仓库的 TCP/TLS connect 和 sleep awaiter 支持这种生命周期。不要让 Connector 启动脱离 acquire 生命周期、继续使用借用参数的后台任务。

### 5.3 淘汰与关闭

只有 `active_leases == 0` 的 Ready 连接进入全局 idle LRU。超过 idle 数量限制时淘汰最老连接；一个定时器处理最早到期连接，长时间持有的流不因 idle timeout 被关闭。

空闲超时、寿命限制、GOAWAY、clear 和 shutdown 都使连接永久离开 ready 链表。主动退休等待最后一个 Lease 归还后发送 GOAWAY。底层错误关闭则立即停止分配，等待 Lease 归还后回收存储。

`clear()` 取消当前等待和拨号，退休当前连接，允许之后的新 acquire；`shutdown()` 还永久拒绝新 acquire。这两个同步方法仅发起清理。

释放业务持有的 Lease 后，使用 `co_await pool.join()` 等待所有连接、拨号和 acquire 完成并执行延迟销毁，然后才能销毁 pool 或停止 loop。join 等待整个 core 变空；clear 后若仍持续发起新请求，join 会同时等待这些请求。

### 5.4 观测

`connection_total()`、`idle_total()`、`group_count()` 在所属 loop 读取。`set_conn_count_changed_callback(cb, ctx)` 在连接总数或 ready 连接数发生变化时同步调用，参数为 key、total、ready。ready 是可分配连接数，不是 stream 槽位数。

`set_dial_failed_callback(cb, ctx)` 在每次拨号失败时同步调用，参数为 key、拨号错误、该组连续失败次数和下次重试退避。这是无限等待的 acquire 唯一的错误暴露渠道。

回调只能观察状态，不能重入池进行 acquire、clear、销毁或其他修改。回调 key 仅在回调期间借用。

## 6. LocalHttp2ConnectionPoolSet

```cpp
fiber::event::EventLoopGroup group(4);
fiber::http::LocalHttp2ConnectionPoolSet connections(group);
if (!connections.init()) {
    // 初始化失败。
}
```

`acquire(key, connector, timeout)`、`try_acquire(key)` 和观测方法根据当前 loop 路由。每个 shard 独立执行 Options 中的上限，因此进程总连接数最多为 shard 数乘以 `max_connections_total`。

`clear_async()` 和 `shutdown_async()` 向每个 loop 派发清理，并等待各 core 的 join。多个并发 shutdown 调用者共享完成结果。先释放业务请求的 Lease，再等待 shutdown_async，最后停止 EventLoopGroup 并销毁 set；不要持有自己的 Lease 等待池 shutdown。

## 7. 测试参考

`tests/Http2ConnectionPoolTest.cpp` 使用真实 TCP loopback HTTP/2 服务端，覆盖多路复用、饱和、链表顺序、FIFO、防插队、超时、SETTINGS、GOAWAY、错误关闭、取消、重试、上限、free list 复用和观测。

`tests/LocalHttp2ConnectionPoolSetTest.cpp` 覆盖多 loop 路由、跨 shard 清理、并发 shutdown 和取消；真实连接的 facade 清理用例也位于 `Http2ConnectionPoolTest.cpp`。共用 bucket index 的测试为 `tests/HttpConnectionBucketIndexTest.cpp`。
