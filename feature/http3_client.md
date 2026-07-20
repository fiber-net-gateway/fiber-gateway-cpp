# HTTP/3 Client

## 状态

HTTP/3 client 基础能力已经实现。它复用 `QuicUdpEndpoint`、`QuicClient`、`QuicConnection`
和静态 QPACK 编解码路径，没有引入第二套 UDP 或 QUIC runtime。

## 组件边界

- `Http3Client`：持有 TLS client context 和 `QuicClient`，负责 QUIC 建连、`h3` ALPN 校验、
  HTTP/3 session 创建与本地 control stream 启动。
- `Http3ClientConnection`：move-only 连接句柄，创建 exchange，并提供立即关闭、GOAWAY
  graceful shutdown 和 `wait_closed()`。
- `ClientHttp3Exchange`：一个请求/响应 exchange。首次发送请求头时才分配并 attach QUIC
  双向流。
- `ClientHttp3Request`：与 `QuicStream` 同一所有权单元，负责 HEADERS/DATA/trailer、响应解析、
  取消及请求结果分类。
- `Http3Connection`：服务端和客户端共享的 HTTP/3 control/critical stream 状态机、SETTINGS、
  GOAWAY 和连接级关闭逻辑。

## 生命周期约束

1. 调用者先初始化并启动一个 client-only 或混合角色的 `QuicUdpEndpoint`。
2. `Http3Client::init()` 初始化 TLS 和 QUIC connector；`connect()` 必须在 endpoint owner loop
   中调用。
3. `Http3Client` 和 endpoint 必须长于由它创建的全部连接。
4. `Http3ClientConnection::open_exchange(pool)` 借用 `BufPool`；pool 和连接必须长于 exchange。
5. 放弃未完成的 exchange 时显式调用 `abort()`。连接句柄析构会立即关闭仍活动的连接。
6. graceful shutdown 后保留连接句柄并 `co_await wait_closed()`，再释放句柄。

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
./build/fiber_tests --gtest_filter='Http3ClientTest.*:Http3ConnectionTest.*:Http3ControlStream*'
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
