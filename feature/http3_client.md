# HTTP/3 Client

## 状态

HTTP/3 client 基础能力已经实现。它复用 `QuicUdpEndpoint`、`QuicConnection::connect()`
和静态 QPACK 编解码路径，没有引入第二套 UDP 或 QUIC runtime。

> 2026-09-12：`QuicClient` 已拆解，`Http3ClientConnection` 改为调用方持有的值类型，与
> `Http2ClientConnection` 同一所有权模型。设计与理由见
> [`quic_http3_client_connection_ownership_design.md`](quic_http3_client_connection_ownership_design.md)。

## 组件边界

- `Http3Client`：一个 endpoint 上所有客户端连接共享的配置——TLS 安全配置、session cache 回调、
  `h3` ALPN 和 HTTP/3 settings。不持有任何连接状态。
- `Http3ClientConnection`：调用方持有的 `NonMovable` 值类型，按值内嵌 `QuicConnection`（外部所有权，
  `on_destroy == nullptr`）、stream gate 和 control streams。构造时从 endpoint 分配 QUIC 身份；
  `connect()` 负责 cache 读取、QUIC 建连、握手等待、`h3` ALPN 校验和 control stream 启动；负责请求表、
  GOAWAY、drain；自持 session cache key 的名字和拨号地址，通过 `QuicConnection::Ops` 接收
  NewSessionTicket / NEW_TOKEN 并回写 cache。
- `ClientHttp3Exchange`：一个请求/响应 exchange。首次发送请求头时才分配并 attach QUIC
  双向流。
- `ClientHttp3Request`：与 `QuicStream` 同一所有权单元，负责 HEADERS/DATA/trailer、响应解析、
  取消及请求结果分类。持有连接的 QUIC lease，因此连接析构前必须结束全部请求。
- `Http3ControlStreams`：两端按值内嵌的协议组件（头文件已公开，因为客户端连接按值内嵌它），
  负责 SETTINGS 和 control/QPACK stream；不包含请求表、角色或 drain 策略。服务端策略由
  `Http3ServerConnection` 直接负责。

## 使用方式

```cpp
fiber::http::Http3Client h3_client(endpoint, client_options);

fiber::http::Http3ClientConnectOptions target{};
target.remote_addr = {ip, 443};
target.server_name = "example.com";
fiber::http::Http3ClientConnection conn(h3_client, target);   // 在 endpoint loop 上构造
auto connected = co_await conn.connect();
if (!connected) { /* connected.error().phase / quic_error */ }

auto exchange = conn.open_exchange(pool);
// ...
conn.graceful_shutdown();
co_await conn.wait_closed();   // H3 任务 join + QUIC 已 detach，之后才可析构
```

## 生命周期约束

1. 调用者先初始化并启动一个 client-only 或混合角色的 `QuicUdpEndpoint`。
2. `Http3Client` 只是配置；它、endpoint 和 TLS 材料必须比所有连接对象活得久。
3. `Http3ClientConnection` 在 endpoint 的 loop 上构造；`connect()` 只能调用一次。
4. `open_exchange(pool)` 借用 `BufPool`；pool 和连接必须长于 exchange。放弃未完成的 exchange 时显式
   `abort()`。
5. 析构前必须 `shutdown()` 或 `graceful_shutdown()` 并 `co_await wait_closed()`；`wait_closed()` 返回
   意味着 H3 任务已 join 且 QUIC 已从 endpoint detach。析构断言这两点以及没有存活的请求。
   `connect()` 失败的对象已经满足该条件（失败路径自己走立即关闭并等待 detach）。
   若通过外层超时或竞速取消 `connect()` 任务，任务析构会发起立即关闭，但不能异步等待清理；
   调用方必须保持连接存活并 `co_await wait_closed()`，之后才可析构，无需额外调用 `shutdown()`。
6. `endpoint.shutdown()` 会关闭并 detach 尚存的连接，但不等待连接对象析构；对象可以在 `shutdown()`
   之后、`~QuicUdpEndpoint` 之前析构，仍需先 `co_await wait_closed()`。先 `endpoint.shutdown()` 再
   `wait_closed()` 可以跳过 3×PTO 的 closing 期。
   `shutdown()` 完成不代表 endpoint 可以析构或重新 `init()`；必须先销毁所有连接对象，
   因为已 detach 的对象仍可能持有 endpoint 的内存池资源。

同一个 request 允许一个读协程和一个写协程并行，以支持流式上传和响应；同方向并发操作返回
`IoErr::Busy`。

## 协议行为

- TLS 固定为 TLS 1.3，ALPN 固定为 `h3`，默认校验证书和主机名。
- H3 application callbacks 在 QUIC connection 暴露给 endpoint 前同步安装，避免握手完成附近到达的
  peer uni stream 丢失。
- 每条连接创建本地 control stream 并首先发送 SETTINGS；peer control/QPACK critical stream 由共享
  reader 管理。
- 请求支持普通方法、CONNECT、peer 启用 `SETTINGS_ENABLE_CONNECT_PROTOCOL` 后的 Extended
  CONNECT、流式 DATA 和 trailer。
- 响应支持 informational/final/trailer HEADERS、流式 DATA、Content-Length 校验、HEAD/204/304
  无 body 语义、字段合法性和 field-section 上限。
- 未知 frame 按 RFC 跳过；请求流上的连接级 frame、非法 push、关闭 critical stream 等错误映射为
  对应 H3 application error。
- peer GOAWAY 进入 draining，禁止新请求；stream ID 大于等于 GOAWAY ID 的活动请求标记为
  `Rejected` 并取消，其余请求继续完成。后续 GOAWAY ID 只能递减。
- `Http3RequestOutcome` 区分 `NotSent`、`Rejected`、`PossiblyProcessed` 和 `Complete`，上层只应自动
  重试前两类，并仍需结合方法幂等性决定策略。

## 当前边界

- QPACK 仅使用静态表和 literal，不启用动态表，因此不会产生 blocked request stream。
- server push 禁用；客户端不发送 MAX_PUSH_ID，收到 push stream/PUSH_PROMISE 会按协议拒绝。
- HTTP/3 层禁用 0-RTT，避免在尚未建立请求重放策略前发送应用请求。
- 不包含 DNS、连接池、origin coalescing、代理选择或自动重试；这些属于更高层 client policy。
- 每个连接仍遵循 QUIC owner-loop 单线程模型。

## 验证

常规回归：

```bash
cmake --build build -j2 --target fiber_tests
./build/fiber_tests --gtest_filter='Http3ClientTest.*:Http3ClientConnectionTest.*:Http3ServerConnectionTest.*:Http3ControlStream*'
ctest --test-dir build --output-on-failure
```

仓库固定 Nginx 1.31.3 互操作：

```bash
temp/nginx-install/sbin/nginx -p "$PWD/" -c scripts/nginx.conf
FIBER_HTTP3_NGINX_PORT=9443 ./build/fiber_tests --gtest_filter=Http3ClientTest.NginxInterop
temp/nginx-install/sbin/nginx -p "$PWD/" -c scripts/nginx.conf -s stop
```

互操作用例覆盖 QUIC v1、TLS 1.3、ALPN `h3`、双向 request stream、SETTINGS、静态/literal
QPACK、GET 响应头和响应体。未设置 `FIBER_HTTP3_NGINX_PORT` 时该用例跳过，不给日常 CTest
增加外部服务依赖。

## 公共 API 迁移

2026-09-12：`Http3ClientConnection` 从 move-only 句柄变为调用方持有的值类型；
`Http3Client::init()` 与 `Http3Client::connect()` 删除，改为
`Http3ClientConnection conn(client, options); co_await conn.connect();`。
`Http3ClientConnectOptions` 的名字字段改为 `std::string_view`（借用到构造函数返回）。
需要放进容器时用 `std::unique_ptr<Http3ClientConnection>`（见 `example/http3_benchmark_client.cpp`）。

更早：删除 `Http3Connection` 类及其公共头。连接查询直接通过 `Http3ClientConnection`：
`connection.http3().accepting_requests()` 改为 `connection.accepting_requests()`；
state、settings、close error 和 peer GOAWAY 查询也直接位于连接。
`ClientHttp3Exchange` 只接收客户端连接，不再接受通用连接。
保留 `Http3ConnectionState` 枚举，声明移至 `Http3Protocol.h`。
这些属于源码兼容性变更，仓库外直接使用旧公共类的消费者需要迁移。

详细边界和验收见 [连接按角色拆分方案](http3_connection_role_split.md)。
