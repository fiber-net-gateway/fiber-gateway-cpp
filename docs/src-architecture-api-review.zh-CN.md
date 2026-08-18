# `src` 代码结构与 API 设计评审

> 评审日期：2026-08-18
>
> 评审范围：`src/` 实现代码及其对应的 `include/fiber/` 公共头文件
>
> 评审重点：模块结构、class 职责、对象生命周期、函数参数和返回值、所有权、错误与异步语义

## 1. 结论摘要

当前代码不需要推倒重写。`IoBuf`/intrusive list、协程和协议状态机、零拷贝缓冲、显式 `IoResult` 以及较完整的测试，已经形成了比较扎实的性能基础。

从规模看，`src` 约有 198 个 `.cpp` 文件、7.8 万行代码，公共头文件约 286 个、2.8 万行代码。随着功能增长，主要风险已经从局部算法实现转向边界契约：

1. Server 生命周期和 EventLoop 归属需要统一表达（`HttpServer` 的 P0 项已在本次变更中处理）；
2. timeout 和 cancellation 语义在 HTTP/2、HTTP/3 及通用 awaiter 之间不一致；
3. `QuicConnection`、`HttpTransport`、`HttpExchange` 的职责和公共接口偏宽；
4. `noexcept`、内存分配和 C++ 异常策略并不完全一致；
5. fd、`void*` callback、borrowed view 和裸指针参数较多，所有权主要依赖调用者自律。

因此，下一阶段建议优先治理“生命周期、时间预算、取消、错误契约”，再进行 QUIC/HTTP 的内部拆分和构建目标拆分。局部容器或 `std::function` 的替换应放在 profiling 之后。

## 2. 验证结果

使用独立构建和依赖目录完成了以下验证：

```bash
cmake -S . -B /tmp/fiber-gateway-cpp-normal-final \
  -DFETCHCONTENT_BASE_DIR=/tmp/fiber-deps-normal \
  -DFIBER_FETCH_DEPS=ON \
  -DFIBER_BUILD_APPS=OFF \
  -DFIBER_BUILD_EXAMPLES=OFF \
  -DFIBER_BUILD_TESTS=ON \
  -DFIBER_ENABLE_LTO=OFF
cmake --build /tmp/fiber-gateway-cpp-normal-final -j2
ctest --test-dir /tmp/fiber-gateway-cpp-normal-final --output-on-failure
```

结果：`fiber_lib` 和 `fiber_tests` 编译通过；CTest 1441/1441 个已执行用例通过，`Http3ClientTest.NginxInterop` 1 个用例按环境跳过。新增生命周期测试和 HTTP/2 延迟 handler 关闭回归均通过。

## 3. P0：优先处理的契约风险

### 3.1 Server 生命周期不一致，存在悬空访问风险（本次已处理）

评审时的问题是：`HttpServer` 的异步 accept/connection 协程捕获了 facade 的成员，而 `close()` 没有完成性契约；HTTP/2 的 `ServerRequestFactory` 也借用了 `options_`/`handler_`。调用者在 `close()` 后立即销毁对象时，存在悬空访问窗口。TLS 初始化失败时，listener 还可能保留已 bind 的 fd。

先确定的代码修改方案：

1. 把 listener、配置、协议实例、任务计数和活动连接 registry 收拢到独立的 runtime owner；异步协程只捕获 runtime，不捕获 `HttpServer` facade。
2. 用显式状态机协调 `bind()`、`serve()` 和关闭竞态；关闭请求先改变状态并设置取消标志，再由 owner loop 执行 loop-affine 资源释放。
3. 将关闭拆成非阻塞的 `request_close()`/`close()` 和可等待的 `shutdown_and_wait()`；活动 HTTP/1、HTTP/2、HTTP/3 连接都登记并在完成前保持 runtime 存活。
4. 让 HTTP/2/HTTP/3 请求持有共享 handler 所有权，消除 factory/server facade 提前析构造成的悬空 handler；所有 bind 失败路径显式回滚已创建资源。

本次按“状态机 + 运行时所有权 + 可等待关闭”的方案落地：

- `HttpServer::State` 明确表达 `Created -> Bound -> Running -> Closing -> Closed`；`bind()` 失败会回滚 listener、TLS context 和 HTTP/3 实例。
- 新增 `request_close()`。`close()` 保留为兼容入口，但现在只发起非阻塞关闭请求；跨线程调用会把 loop-affine 操作投递到 owner loop。
- 新增 `shutdown_and_wait()`。它等待 accept loop、HTTP/1、HTTP/2 和 HTTP/3 的清理完成，完成后状态才进入 `Closed`。
- `HttpServer::Runtime` 集中持有 listener、handler/options、TLS/HTTP/3 实例、任务计数和连接 registry。accept/connection 协程只捕获 `shared_ptr<Runtime>`，不再捕获 `HttpServer` facade。
- HTTP/1 连接支持 loop 内 `shutdown()`，HTTP/2 统一使用 `wait_closed()`；registry 在关闭时向每条连接投递终止请求，并由 `shutdown_and_wait()` 汇合。
- `ServerRequestFactory` 改为值持有 `HttpServerOptions`，并通过共享 handler 所有权把 `HttpHandler` 生命周期延伸到协议请求协程，不再保存配置裸指针；HTTP/3 server/request 路径采用相同策略。

相关接口和实现：

- [`HttpServer.h`](../include/fiber/http/HttpServer.h)
- [`HttpServer.cpp`](../src/http/HttpServer.cpp)
- [`Http1Connection.h`](../include/fiber/http/Http1Connection.h)
- [`ServerRequestFactory.h`](../include/fiber/http/ServerRequestFactory.h)
- [`Http3Server.h`](../include/fiber/http/Http3Server.h)

调用约定也随之明确：

1. `bind()` 可以在事件循环启动前执行；`serve()` 必须在 owner loop 上启动。
2. `close()`/`request_close()` 只表示“开始关闭”，不表示 fd 和连接已经全部退出。
3. 需要销毁 server 或停止相关 loop 前，应在 loop 仍可运行时 `co_await shutdown_and_wait()`。
4. `EventLoopGroup` 的生命周期仍需覆盖活动连接；server 不拥有外部传入的 worker group。

新增回归覆盖 TLS bind 回滚、非 owner 线程关闭，以及带空闲 HTTP/1 连接的完整 shutdown：

- [`HttpServerLifecycleTest.cpp`](../tests/HttpServerLifecycleTest.cpp)
- [`HttpClientServerInteropTest.cpp`](../tests/HttpClientServerInteropTest.cpp)
- [`HttpServerTlsDynamicCertTest.cpp`](../tests/HttpServerTlsDynamicCertTest.cpp)

### 3.2 HTTP/2 与 HTTP/3 的 timeout 预算不一致

HTTP/2 会先创建绝对 deadline，再把剩余时间传给后续操作：

- [`ClientHttp2Exchange.cpp`](../src/http/ClientHttp2Exchange.cpp#L70)

HTTP/3 则把同一个相对 timeout 同时传给“打开 stream”和“发送 header”：

- [`ClientHttp3Exchange.cpp`](../src/http/ClientHttp3Exchange.cpp#L38)
- [`ClientHttp3Exchange.cpp`](../src/http/ClientHttp3Exchange.cpp#L183)

如果 stream attach 已经消耗了部分时间，后续 header 操作可能重新获得完整预算。若设计意图是“每阶段 timeout”，应在命名和文档中明确；若意图是“整个操作的总预算”，则当前实现会重启预算。

建议在协议层统一使用：

```cpp
struct Deadline {
    TimePoint at;
    [[nodiscard]] milliseconds remaining(TimePoint now) const noexcept;
};
```

公共 API 可以继续接收 `milliseconds`，但进入协议层后应立即转换为 `Deadline`，后续所有子操作只接收 deadline。时间读取遵循项目约定，使用 `EventLoop::current().now()` 或显式传入的 loop 时间。

### 3.3 cancellation 约束没有被类型系统表达

`TimeoutAwaiter` 超时后只是标记状态并恢复父协程，没有调用内部 awaiter 的 `cancel()`：

- [`Timeout.h`](../include/fiber/async/Timeout.h#L145)

而 `SelectableAwaiter` 只要求 `completed()`，不要求可取消：

- [`Awaitable.h`](../include/fiber/async/Awaitable.h#L49)

`WhenAny` 对 loser 直接销毁，也没有统一的取消动作：

- [`WhenAny.h`](../include/fiber/async/WhenAny.h#L313)

当前正确性因此依赖每个具体 awaiter 的析构行为。建议：

- 增加明确的 `CancellableAwaiter` concept；
- timeout 时先 cancel，再 resume；
- `when_any` 销毁 loser 前显式 cancel；
- 如果某些 awaiter 只能依靠析构取消，应将这个约定统一成内部基类或可测试的契约。

另外，`Task<T>` 的结果是一次性 move 出来的，重复 await 会触发 panic，但 `operator co_await()` 没有 ref-qualified：

- [`Task.h`](../include/fiber/async/Task.h#L59)
- [`Task.h`](../include/fiber/async/Task.h#L110)

建议普通 `Task` 只允许右值 await，即 `operator co_await() &&`；需要多次等待时另提供显式的 `SharedTask`。

### 3.4 EventLoop affinity 主要靠断言

`EventLoop::current()` 在没有当前 loop 时直接断言：

- [`EventLoop.h`](../include/fiber/event/EventLoop.h#L209)

底层 fd、listener、QUIC endpoint 的 `close()` 也要求在绑定 loop 上执行：

- [`AcceptFd.h`](../include/fiber/net/detail/AcceptFd.h#L61)
- [`QuicUdpEndpoint.cpp`](../src/quic/QuicUdpEndpoint.cpp#L596)

这对内部热路径可以接受；`HttpServer` 的公共关闭入口已在 3.1 增加跨线程安全的 `request_close()` 和可等待的 `shutdown_and_wait()`。其他直接暴露 loop-affine `close()` 的公共对象仍建议增加对应的 `request_close()`/`close_async()`，必要时增加 `WrongLoop` 或 `InvalidState` 错误。

`EventLoopGroup` 的 `loops_` 和 `threads_` 还是公开可变成员：

- [`EventLoopGroup.h`](../include/fiber/event/EventLoopGroup.h#L38)

应改为 private，仅暴露 `size()/at()/running()` 等受控访问。

## 4. P1：class 职责和模块边界

### 4.1 `QuicConnection` 过于庞大

`QuicConnection` 同时承担握手、stream、frame、crypto、flow control、loss recovery、拥塞控制、path 和 timer 管理：

- [`QuicConnection.h`](../include/fiber/quic/QuicConnection.h#L370)
- [`QuicConnection.h`](../include/fiber/quic/QuicConnection.h#L533)
- [`QuicConnection.h`](../include/fiber/quic/QuicConnection.h#L644)

其 `Options` 还混合地址、连接 ID、内存池、EventLoop、TLS、缓存、owner 和 C callback：

- [`QuicConnection.h`](../include/fiber/quic/QuicConnection.h#L434)

建议保留 `QuicConnection` façade，但将内部职责拆成按值组合的组件：

```text
QuicConnection
 ├─ Handshake/CryptoContext
 ├─ StreamManager
 ├─ FlowControl
 ├─ LossRecovery
 ├─ PathManager
 ├─ ConnectionIdManager
 └─ TimerSet
```

不必为了缩短头文件盲目引入大量 pImpl；可以让组件按值嵌入，只把实现声明移到 private header。

`QuicPathManager::paths()` 返回整个可写数组，直接暴露内部不变量：

- [`QuicPathManager.h`](../include/fiber/quic/QuicPathManager.h#L28)

更适合返回只读 view，或提供 `create/activate/remove/for_each` 等受控操作。

QUIC 头文件的依赖也偏重：`QuicProtocol.h` 和 `QuicPacketProcessor.h` 都直接包含完整的 `QuicConnection.h`：

- [`QuicProtocol.h`](../include/fiber/quic/QuicProtocol.h#L7)
- [`QuicPacketProcessor.h`](../include/fiber/quic/QuicPacketProcessor.h#L11)

应把共享枚举、协议常量和轻量结构移到 `QuicTypes.h`/`QuicProtocolTypes.h`。

### 4.2 `HttpTransport` 接口过宽

`HttpTransport` 同时暴露 handshake/shutdown、readiness callback、六个 poll 读写函数、六个异步读写函数以及 close/状态查询：

- [`HttpTransport.h`](../include/fiber/http/HttpTransport.h#L23)
- [`HttpTransport.h`](../include/fiber/http/HttpTransport.h#L42)
- [`HttpTransport.h`](../include/fiber/http/HttpTransport.h#L54)

虽然注释很完整，但接口已经接近整个传输栈。建议拆成 `Readable`、`Writable`、`Handshakeable`、`ReadyNotifier`、`Closable` 等能力接口，或者引入统一的 `IoBufferView/IoOperation`，减少 `void*`、`IoBuf`、`IoBufChain` 的重复虚函数矩阵。

### 4.3 `HttpExchange` 和配置对象职责过多

`HttpExchange` 同时管理请求元数据、headers、body reader、response writer、统计、abort 状态和异步 waiter，并依赖多个协议类作为 friend：

- [`HttpExchange.h`](../include/fiber/http/HttpExchange.h#L81)
- [`HttpExchange.h`](../include/fiber/http/HttpExchange.h#L157)

建议逐步形成兼容 façade：

```text
HttpExchange
 ├─ RequestView
 ├─ BodyReader
 ├─ ResponseWriter
 └─ ExchangeState/Stats
```

`HttpServerOptions` 目前把 TCP、TLS、HTTP 通用参数和 HTTP/3/QUIC 参数全部放在一起：

- [`HttpExchange.h`](../include/fiber/http/HttpExchange.h#L33)

建议拆成 `HttpCommonOptions`、`Http1Options`、`Http2Options`、`Http3Options`、`TlsOptions` 后再组合。

### 4.4 异步 waiter 有重复状态机

`Mutex`、`RWMutex`、`WaitGroup`、`Watch` 都重复实现了 `Waiting -> Notified -> Resumed/Canceled` 状态、intrusive link 和 resume 逻辑：

- [`Mutex.h`](../include/fiber/async/Mutex.h#L73)
- [`RWMutex.h`](../include/fiber/async/RWMutex.h#L118)
- [`WaitGroup.h`](../include/fiber/async/WaitGroup.h#L17)
- [`Watch.h`](../include/fiber/async/Watch.h#L27)

建议提取内部 `WaiterBase`、intrusive queue 和 `cancel/resume_once` 辅助逻辑；reader batching 等特有策略仍保留在各自类型中。

## 5. API 参数、返回值和所有权

### 5.1 建议固定返回类型规范

目前同时存在 `bool init`、`IoErr`、`expected<T, IoErr>`、`Task<void>`、`Task<IoResult<T>>`、`DetachedTask` 和 `void close()`。建议采用以下约定：

- `bool`：只用于不会产生错误的状态查询；
- 同步可失败操作：`IoResult<T>`；
- 异步可失败操作：`Task<IoResult<T>>`；
- `DetachedTask`：只用于明确的 fire-and-forget，并配套错误 sink；
- `close()`：只表示发起关闭，完成状态由 `Task<IoResult<void>>` 或 `wait_closed()` 获取。

### 5.2 fd 和 callback 的所有权应显式化

例如 `TcpStream` 直接接收裸 `int fd`，并返回裸 fd：

- [`TcpStream.h`](../include/fiber/net/TcpStream.h#L38)
- [`TcpStream.h`](../include/fiber/net/TcpStream.h#L53)

建议引入 `UniqueFd` 和 `AdoptFd` 标签，明确“接管所有权”和“借用 fd”的区别。`void* owner + function pointer` 也可以在公共 API 层包装成带生命周期说明的 callback handle。

### 5.3 borrowed view 应通过类型表达

`HttpHeaders::add_view/set_view` 会保留外部指针，目前主要依赖注释保证生命周期：

- [`HttpHeaders.h`](../include/fiber/http/HttpHeaders.h#L54)

建议分离 `OwnedHeaderBlock` 和 `HeaderView`，避免调用者误以为 view API 会复制数据。类似问题还存在于 `JsValue` 的 borrowed string/binary 和 `Script::exec_async()` 的 `void* attach`：

- [`JsValue.h`](../include/fiber/script/JsValue.h#L115)
- [`Script.h`](../include/fiber/script/Script.h#L20)

### 5.4 DNS result API 可以保留预分配，但改善访问形态

DNS 结果对象需要手工 `init/release/clear`，并通过裸指针加 count 暴露记录：

- [`DnsResolver.h`](../include/fiber/dns/DnsResolver.h#L27)

这套设计有预分配和低分配的性能价值，可以保留底层 storage，但在外层提供 RAII storage 和 `std::span<const T>` 访问器。

`SharedDnsCache2` 在持 mutex 时调用 `EventLoop::current().now()`：

- [`DnsCache2.cpp`](../src/dns/DnsCache2.cpp#L780)

mutex 并不能消除 EventLoop TLS 前置条件。建议所有 cache 操作显式传入 `TimePoint now`，或统一投递到 owner loop。

### 5.5 `noexcept`、分配和异常策略需要统一

当前仍有几类不一致：

- [`BufPool.h`](../include/fiber/common/mem/BufPool.h#L65) 抛出 `std::bad_alloc`；
- [`RoutePathMatcher.h`](../include/fiber/common/util/RoutePathMatcher.h#L342) 抛出异常；
- `noexcept` 函数内部使用 `std::make_unique`，例如 [`DnsResolver.cpp`](../src/dns/DnsResolver.cpp#L90)；
- `QuicConnection::Options` 含 `std::string`，但构造函数声明为 `noexcept`：[`QuicConnection.h`](../include/fiber/quic/QuicConnection.h#L467)。

建议明确选择一种政策：

1. 核心库彻底采用 `nothrow`、自定义 allocator 和 `expected`；或
2. 仅在脚本编译等冷路径保留异常，并去掉相关 `noexcept`；
3. 不要在 `noexcept` 中使用会抛异常的标准分配器，然后再检查一个正常情况下不会返回空的指针。

## 6. 构建和模块结构

当前所有 `src/*.cpp` 被放进一个静态库：

- [`CMakeLists.txt`](../CMakeLists.txt#L37)

短期开发方便，但会扩大增量编译范围，也使 HTTP 使用者被迫依赖完整 QUIC/OpenSSL 头文件。中长期可以拆成：

```text
fiber_common
fiber_async_event
fiber_net_dns
fiber_http
fiber_quic
fiber_script
```

不必一次完成。优先把 QUIC 配置和协议基础类型从 HTTP 公共头中移出，再逐步拆 CMake target 和 private headers。

## 7. 建议实施路线

### 第一阶段：不大改 ABI，先修契约

1. 统一 `Deadline`；
2. 增加 HTTP/3 timeout 回归测试；
3. ~~增加 `request_close()` 和 `shutdown_and_wait()`；~~ 已完成（本次变更）；
4. ~~建立活动连接 registry；~~ 已完成（本次变更）；
5. ~~修复 bind 失败回滚；~~ 已完成（本次变更）；
6. 审计所有 `noexcept + make_unique`；
7. 让 `SharedDnsCache2` 显式接收时间。

### 第二阶段：减少重复和边界泄漏

1. 提取统一 waiter 基类；
2. 拆分 `HttpTransport` 能力接口；
3. 拆分 `HttpServerOptions`；
4. 引入 `UniqueFd/AdoptFd`；
5. 增加固定大小的 `Error` 详情；
6. 将 borrowed/owned header 明确分型。

### 第三阶段：结构性重构

1. 将 `QuicConnection` 拆成内部组合组件；
2. 清理 QUIC/HTTP 头文件依赖；
3. 拆分 CMake targets；
4. 收紧 `GcHeap`、`JsValue` 等脚本公共 API。

## 8. 建议补充的回归测试

- HTTP/3 stream attach 消耗部分时间后，header 是否遵守总 deadline；
- 有活动 HTTP/2/HTTP/3 连接时销毁 Server 是否安全；
- 从非 owner 线程调用 `request_close()`/`close_async()`；
- timeout 和 `when_any` loser 是否真正取消底层 waiter；
- `SharedDnsCache2` 从没有 current EventLoop 的线程调用；
- listener/TLS/HTTP3 初始化各阶段失败后的资源回滚；
- 自定义 allocator 返回失败时，所有 `noexcept` API 的返回值是否符合约定。

其中 listener/TLS 回滚、非 owner 线程 `request_close()` 和空闲 HTTP/1 连接 shutdown
已由 [`HttpServerLifecycleTest.cpp`](../tests/HttpServerLifecycleTest.cpp) 覆盖；HTTP/2 延迟
handler 在连接关闭后的取消结果由 `Http2ConnectionTest.ServerHandlerSendAfterConnectionCloseReturnsCanceled`
覆盖；HTTP/3 清理由 interop 与 HTTP/3 client 测试覆盖。

## 9. 最终建议

如果只能优先做三件事，应选择：

1. 统一 Server 生命周期和跨线程关闭；
2. 统一 deadline/cancellation 语义；
3. 收缩 `QuicConnection` 和 HTTP 公共头边界。

这三项对稳定性、后续开发速度和 API 可维护性的收益最大；局部容器替换、`std::function` 优化等微优化应在 profiling 之后进行。
