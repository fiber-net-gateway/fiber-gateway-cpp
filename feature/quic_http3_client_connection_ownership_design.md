# QUIC / HTTP-3 客户端连接所有权重构详细设计

> 起因：`QuicConnection::Options` 中 TLS 相关字段散落，评估"按 role 做 union"时发现根因是
> client 侧建连编排放在 `QuicClient` 外部对象里，Options 只剩 cache 残留。
> 状态：PR 1（quic 层，§6.3）已实施；PR 2（值类型 `Http3ClientConnection`）未实施
> 目标读者：实现者 / 评审者
> 关联：`feature/quic_client.md`（现状）、`feature/http3_client.md`（现状）、
> `feature/http2_client_connection.md`（对齐目标）、`feature/fiber-lib-h3-client-teardown-uaf.md`（生命周期背景）

---

## 1. 目标与非目标

### 1.1 目标

1. **HTTP/3 client 与 H1/H2 client 同一所有权模型**：`Http3ClientConnection` 成为用户持有的
   `NonMovable` 值类型——构造、`co_await connect()`、开 exchange、`shutdown()`/`graceful_shutdown()`、
   `co_await wait_closed()`、析构。和 `Http2ClientConnection` 一样。
2. **删除 `QuicClient` / `QuicClientAttempt` / `create_connection` 工厂回调**，以及
   `Http3Client::connect` 里靠 `last_created_connection_` 从回调中捞指针的 hack
   （`src/http/Http3Client.cpp:91-93`）。
3. **`QuicConnection::Options` 只保留两个 role 共用的字段和 server 的 TLS 指针**：client 建连时
   一次性消费的输入走 `QuicConnection::connect(params)`，不再存进 `options_`。
4. **cache 钩子并入 `Ops`**：单一 owner 后不再需要 `client_cache_owner` 和另一套回调三件套。
5. QUIC 协议逻辑（CID 分配、token、Initial key、TLS init、attach、相位分类）**留在 `fiber::quic`**，
   `QuicConnection` 直接可用作非 HTTP 的 QUIC client。

### 1.2 非目标

- 不改 `QuicConnection` 的协议实现（握手、丢包恢复、流控、迁移等）。
- 不改 server 侧的连接创建与所有权：server 连接仍由 endpoint 在收到 Initial 时通过
  `create_connection` 创建，lease 引用计数，`on_destroy` 释放。
- 不引入连接池、DNS、重试策略。
- 不在本次实现 `feature/fiber-lib-h3-client-teardown-uaf.md` 的方案 A（共享资源束）。本设计依赖
  刚落地的 "endpoint outlives connections" 规则（`ce0c741`），见 §5。

### 1.3 已确认的决策

| # | 决策点 | 结论 |
|---|--------|------|
| D1 | `TlsClientParam`/`TlsServerParam` 做 union | **否**。两者生命周期契约不对称（client 参数 create 后即弃，server 参数借用到握手完成且 QUIC 下懒创建）；client 字段大多不是 TLS 参数而是 cache 身份 |
| D2 | client 字段按 role 分组成 `Options::Client` | **被 D4 取代**。client 建连输入不再存进 Options |
| D3 | `Options::Client` 做指针 | **否**。每连接数据没有比连接本身更自然的所有者 |
| D4 | 删除 `QuicClient` | **是**。编排逻辑收成 `QuicConnection::connect(params)`；配置（ALPN、cache ops、TLS security）归 `Http3Client` |
| D5 | `Http3ClientConnection` 形态 | **值类型、`NonMovable`**，连接目标在**构造时**给出，`connect()` 只做建连；理由见 §4.2 |
| D6 | cache key 的名字所有权 | `Http3ClientConnection` 自持 `std::string` 副本。之前"view 由外层保证活过连接"的契约不再需要，`connect()` 借用契约与 H2 一致 |
| D7 | 外部所有权判定 | `Options::on_destroy == nullptr` ⇒ 连接存储由外部拥有。不加新字段 |
| D8 | endpoint `shutdown()` 对外部所有连接 | 只等 **detach**，不等析构；`~QuicUdpEndpoint` 断言没有外部连接对象仍存活 |
| D9 | `wait_closed()` 语义 | 覆盖 QUIC detach。析构断言"从未连接 或 已关闭且已 detach"，且无未完成 exchange |
| D10 | server 0-RTT 开关 | 单一来源 `TlsServerParam::enable_early_data`；删除 `QuicConnection::Options::enable_early_data` 与 endpoint 上的同名字段 |
| D11 | `connect()` 失败后的对象状态 | attach 之后失败的路径由 `connect()` 自己 `close_immediately` 并 `co_await wait_closed()`，返回时对象已可析构 |

---

## 2. 现状与问题

### 2.1 `QuicConnection::Options` 的 client 残留

`include/fiber/quic/QuicConnection.h:455-497`。`sizeof(Options) = 672`，`sizeof(QuicConnection) = 7040`。

| 字段 | 真实消费者 | 问题 |
|---|---|---|
| `tls` (`const TlsServerParam*`) | `ensure_server_tls()` → `QuicTlsSession::init_server` | 正常，只是命名没体现 server-only |
| `client_server_name` / `client_verify_name` (`std::string`) | 仅 `QuicClient::store_session/store_token` 重建 cache key | 不参与 SSL 创建；每连接两个 `std::string` |
| `client_cache_remote_addr` | 同上 | 与 `remote_addr` 初值相同（`QuicClient.cpp:220` vs `:244`）；但 `remote_addr` 会被路径迁移改写（`QuicPathManager.cpp:212`），所以"重复"只在初值层面 |
| `client_tls_credential` / `client_trust_store` | 无调用者 | 死字段；`cache_key()` 读的是 `QuicClient::tls_security_` |
| `client_cache_owner` + `on_new_tls_session` + `on_new_token` | `QuicConnection.cpp:2194`, `:2200` | 与 `Options::owner`/`Ops` 平行的第二套 owner+回调，因为 cache 和应用是两个 owner |
| `enable_early_data` | 两个 role 语义不同 | server 侧与 `TlsServerParam::enable_early_data` 双源，`init_server` 用前者覆盖后者（`QuicTlsSession.cpp:296`） |
| `remembered_peer_transport` / `has_remembered_peer_transport` | ctor `:497`、`:2552` | client-only，握手完成后不再需要，却存整个连接期 |

关键事实：client 的 `TlsClientParam` **完全不经过 Options**——`QuicClient::start_connect` 在栈上拼出来
直接调 `connection->tls().init_client()`（`QuicClient.cpp:270-274`）。散落的根源是编排在外部对象。

### 2.2 两层工厂与句柄

```
Http3Client::connect(options)
  └─ QuicClient::start_connect(options)
       ├─ options_.create_connection(owner, endpoint, conn_options)   ← 回调
       │    └─ Http3Client::create_connection_op → Impl::create (堆)
       │         └─ last_created_connection_ = session               ← 从回调里捞指针
       ├─ init token / initial crypto / TLS / start_handshake / attach
       └─ 返回 QuicClientAttempt(lease, deadline)
  └─ attempt.wait_connected() → ALPN 校验 → session->start()
  └─ 返回 Http3ClientConnection(handle: Lease + Impl*)
```

Impl 由 lease 引用计数持有，最后一个 lease 释放后 `destroy_connection` spawn 一个 join 再 `delete`
（`Http3ClientConnectionImpl.cpp:152-160`）。H2 client 没有这些层：`Http2ClientConnection` 按值拥有
`Http2Connection`，析构断言已关闭（`Http2ClientConnection.cpp:48-51`）。

---

## 3. 目标模型

```text
Application (per connection)
    |
    +-- Http3ClientConnection (值类型, 用户持有)
    |     +-- quic::QuicConnection quic_       (按值; on_destroy = nullptr ⇒ 外部所有权)
    |     +-- Http3ControlStreams / gate / 请求表 / drain   (原 Impl 成员)
    |     +-- server_name_ / verify_name_ / remote_addr_    (cache key 材料, 自持)
    |     +-- connect(): cache load → quic_.connect(params) → wait_established → ALPN → control start
    |     +-- Ops owner: create_stream / state / capacity / on_new_tls_session / on_new_token
    |
Application (shared)
    +-- Http3Client                     (配置: TlsClientSecurity, QuicClientCacheOps, H3 settings, ALPN "h3")
    +-- QuicUdpEndpoint                 (不变: socket / CID 分流 / scheduler / server admission)
```

### 3.1 使用方式

```cpp
fiber::quic::QuicUdpEndpoint endpoint(loop);
// endpoint.init(EndpointOptions{...}); endpoint.start();

fiber::http::Http3Client::Options client_options{};
client_options.tls.trust_store = trust_store.get();
client_options.tls.verify_peer = true;
fiber::http::Http3Client h3_client(endpoint, std::move(client_options));

{
    fiber::http::Http3ClientConnectOptions target{};
    target.remote_addr = {ip, 443};
    target.server_name = "example.com";      // string_view, 借用到构造函数返回
    fiber::http::Http3ClientConnection conn(h3_client, target);   // 在 endpoint loop 上构造

    auto connected = co_await conn.connect();  // handshake_timeout 来自 target
    if (!connected) { /* connected.error().phase / quic_error */ }

    auto exchange = conn.open_exchange(pool);
    // ...
    conn.graceful_shutdown();
    co_await conn.wait_closed();              // H3 任务 join + QUIC 已 detach
}                                             // 析构: 断言已关闭、无 exchange

co_await endpoint.shutdown();
```

`endpoint.shutdown()` 与连接对象的顺序不再互相等待：先析构连接再 shutdown 是常规顺序；反过来
（先 `shutdown()` 强制关闭所有连接，之后再析构对象）也成立，见 §4.6 与 §5.5。唯一硬规则是
**endpoint 对象必须比连接对象活得久**（RAII 声明顺序），与 H2 的"loop 比连接活得久"一致。

---

## 4. QUIC 层设计

### 4.1 `QuicConnection::Options` 最终形态

```cpp
struct Options {
    QuicConnectionRole role = QuicConnectionRole::Server;
    net::SocketAddress local_addr{};
    net::SocketAddress remote_addr{};
    QuicConnectionId original_destination_connection_id{};
    QuicConnectionId initial_destination_connection_id{};
    QuicConnectionId local_connection_id{};
    QuicConnectionId remote_connection_id{};
    QuicConnectionId retry_source_connection_id{};
    QuicTransportSettings transport{};
    std::chrono::milliseconds keepalive_interval{0};
    QuicRecvFlowControlSettings recv_flow{};
    std::uint64_t max_peer_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
    std::uint64_t max_peer_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
    std::uint64_t max_local_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
    std::uint64_t max_local_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
    // Null on_destroy means the connection storage is owned outside the lease
    // model: the owner destroys it after wait_closed(). See ownership modes.
    void *destroy_owner = nullptr;
    DestroyCallback on_destroy = nullptr;
    void *owner = nullptr;
    Ops ops{};
    std::chrono::milliseconds graceful_shutdown_grace{30000};
    bool has_retry_source_connection_id = false;
    bool initial_path_validated = false;
    // role == Server. Lazily consumed by ensure_server_tls() after the first
    // Initial authenticates; the endpoint owns it and outlives the connection.
    // Early data is enabled iff server_tls->enable_early_data.
    const net::TlsServerParam *server_tls = nullptr;
};
```

删除：`enable_early_data`、`remembered_peer_transport`、`has_remembered_peer_transport`、
`client_server_name`、`client_verify_name`、`client_cache_remote_addr`、`client_tls_credential`、
`client_trust_store`、`client_cache_owner`、`on_new_tls_session`、`on_new_token`；`tls` 改名 `server_tls`。

删除对应 accessor：`client_server_name()`、`client_verify_name()`、`client_tls_credential()`、
`client_trust_store()`、`client_cache_remote_addr()`（`QuicConnection.h:805-813`）。

构造断言：`role == Server || server_tls == nullptr`。

地址与 CID 保留在 Options：它们是连接身份，两个 role 都在构造时确定，ctor 用它们初始化 CID 槽位和初始
路径（`QuicConnection.cpp:469-522`）。这是 D5 选择"目标在构造时给出"的直接原因——否则要把这段初始化
拆成 client 的延迟绑定，改动面大得多。

### 4.2 `QuicClientConnectParams` 与 `QuicConnection::connect()`

新头文件 `include/fiber/quic/QuicClientConnect.h` 取代 `QuicClient.h`，保留原有的
`QuicClientCacheKey`、`QuicClientCachedState`、`QuicClientCacheOps`、`QuicConnectPhase`、
`QuicConnectError`（定义不变），新增：

```cpp
// Consumed synchronously by QuicConnection::connect(). Every pointer and view
// is borrowed only until connect() returns: BoringSSL copies what it needs out
// of tls during SSL creation, SSL_set_session retains its own reference, and
// the token and remembered transport are copied into connection state.
struct QuicClientConnectParams {
    net::TlsClientParam tls{};
    bool allow_insecure = false;
    SSL_SESSION *resumption_session = nullptr;
    const std::uint8_t *token = nullptr;
    std::size_t token_len = 0;
    // 0-RTT: only honoured when resumption_session and remembered_peer_transport
    // are both present. The remembered settings seed the peer limits until the
    // real transport parameters arrive and are re-validated against them
    // (RFC 9000 §7.4.1).
    bool attempt_early_data = false;
    const QuicTransportSettings *remembered_peer_transport = nullptr;
};
```

`QuicConnection` 新增：

```cpp
// Client role only, exactly once, on the connection's loop, before attach.
// Runs the whole client connect sequence synchronously and leaves the
// connection attached with its Initial queued for sending. On failure the
// connection is unattached and Closed; connect_error() classifies the phase.
[[nodiscard]] common::IoResult<void> connect(const QuicClientConnectParams &params) noexcept;

// Phase classification for a connect()/wait_established() failure
// (formerly QuicClientAttempt::make_error).
[[nodiscard]] QuicConnectError connect_error(common::IoErr error) const noexcept;

// Resolves once the connection has been detached from its endpoint (state
// Closed). Immediate for a connection that was never attached. The awaiting
// task holds a lease; destroying the connection with a waiter pending is a
// bug and is asserted.
[[nodiscard]] async::Task<void> wait_closed() noexcept;
```

`connect()` 的顺序（从 `QuicClient::start_connect` 搬入，`QuicClient.cpp:257-297`）：

1. 前置：`role == Client`、`state_ == Init`、`loop_.in_loop()`、`!attached_to_endpoint()`、
   `!tls_.initialized()`；否则 `Invalid`。
2. 0-RTT 记忆（原 ctor `:497-504` 的块搬到这里）：`attempt_early_data && resumption_session &&
   remembered_peer_transport` 时把 remembered 参数写入 `peer_transport_.params`、`peer_max_data_`、
   `early_*`，并把 `options_.max_local_*_streams` 设为 remembered 的 `initial_max_streams_*`；否则
   `max_local_*` 设为 0（client 在拿到对端参数前不开流，与今天 `QuicClient.cpp:236-239` 一致）。
   记录 `attempt_early_data_`，`early_data_enabled()` 对 client 返回它。
3. `set_initial_token(token, token_len)`。
4. `init_initial_crypto(original_destination_connection_id())`。
5. `tls_.init_client(params.tls, *this, params.allow_insecure, params.resumption_session)`。
6. `tls_.drive_handshake()`，`WouldBlock` 视为成功。
7. `start_handshake()`。
8. `endpoint_.attach_client_connection(lease())`。
9. `endpoint_.schedule_send(*this)`。

失败处理：步骤 2-7 失败时连接从未 attach，直接 `mark_closed()`（不排任何 timer）；步骤 8 失败同样。
每个失败点记录 `connect_phase_`（`InitialCrypto` / `Tls` / `Handshake` / `Endpoint` / `Connection`），
`connect_error()` 据此分类，握手期的分类逻辑（VN / TransportParameters / Tls / PeerClose / Timeout）
原样保留。

握手 deadline：`QuicClientAttempt` 的绝对 deadline 机制删除。调用方直接
`co_await wait_established(handshake_timeout)`；超时后调用方负责 `close_immediately()`。
`wait_established`/`wait_confirmed` 现有签名不变。

### 4.3 `Ops` 新增 cache 钩子

```cpp
struct Ops {
    ...
    // Client role. A NewSessionTicket arrived; returning true transfers the
    // SSL_SESSION reference to the owner. Runs from inside the TLS stack --
    // store and return, do not touch the connection.
    bool (*on_new_tls_session)(void *owner, QuicConnection &connection, SSL_SESSION *session) noexcept = nullptr;
    // Client role. A NEW_TOKEN frame arrived; the bytes are borrowed for the call.
    void (*on_new_token)(void *owner, QuicConnection &connection, const std::uint8_t *token,
                         std::size_t token_len) noexcept = nullptr;
};
```

`recv_new_token_frame` / `on_new_tls_session`（`QuicConnection.cpp:2188-2203`）改读
`options_.ops.*` 与 `options_.owner`。`role() == Client` 的运行时检查保留——那是对对端行为的防御，
不是配置一致性。

cache key 的重建不再是 QUIC 层的事：owner 自己知道 server_name / verify_name / 原始 remote_addr /
credential / trust_store。`QuicClientCacheKey` 类型保留在 `QuicClientConnect.h` 供 owner 与 cache
实现共用。

### 4.4 endpoint 的 client 身份分配

`QuicClient::start_connect` 里的 CID 生成与唯一性重试（`QuicClient.cpp:198-216`）搬到 endpoint 公开方法：

```cpp
struct QuicClientIdentity {
    QuicConnectionId original_destination_connection_id{};
    QuicConnectionId local_connection_id{};
};
// On the endpoint's loop. Random ODCID plus a local CID that is unique in this
// endpoint's index and differs from the ODCID. Nothing is registered until the
// connection attaches.
[[nodiscard]] common::IoResult<QuicClientIdentity> allocate_client_identity() noexcept;
```

`generate_connection_id` / `generate_unique_connection_id` 保持私有；`friend class QuicClient`
从 `QuicConnection` 和 `QuicUdpEndpoint` 删除。

### 4.5 所有权模式

`Options::on_destroy` 是判定：

| | endpoint 创建（server；以及仍想用 lease 模型的 client） | 外部拥有（值类型 client） |
|---|---|---|
| 存储 | 堆，`create_connection` 分配 | 调用方对象的成员 |
| `ref_count_` 初值 1 的归属 | `Lease::adopt` 拿走，最后一个 lease 释放 → `on_destroy` | 所有者隐式持有，从不释放 |
| 析构时机 | `on_destroy` 回调内/之后 | 所有者析构 |
| `~QuicConnection` 断言 | 不变 | `ref_count_ == 1 && !attached_to_endpoint()`（无 stream/exchange/wait lease 未释放） |
| `hosted_.done()` | 析构时（不变） | **detach 时** |
| endpoint 额外计数 | — | `external_connections_alive_`：attach 时 `++`，析构时 `--` |

`release()` 的 `ready_for_destruction()` 分支对外部所有权永远不触发（计数不会到 0），保留现有断言。

`attach_client_connection()` 去掉对 `on_destroy_ != nullptr` 的要求（原在 `QuicClient.cpp:257`）；
server admission 的断言（`QuicUdpEndpoint.cpp:1663`）保留——server 连接始终是 endpoint 创建的。

### 4.6 endpoint `shutdown()` / `close()` / 析构

- `shutdown()`：语义改为"关闭所有 hosted 连接，等到 **endpoint 创建的连接全部析构** 且 **外部拥有的连接
  全部 detach**"。实现上就是 `hosted_.join()`，只是外部连接在 detach 时 `done()`。
- `close()`：`hosted_.empty()` 与 `connections_.empty()` 断言保留。
  `recv_storage_budget_.retained_capacity() == 0` 断言（`QuicUdpEndpoint.cpp:625`）**移到
  `~QuicUdpEndpoint`**：一个已 detach 但仍存活的外部连接可以合法持有未释放的 recv credit（应用还拿着
  响应体的 `IoBuf`）。`feature/fiber-lib-h3-client-teardown-uaf.md` 也指出过这条断言时机偏早。
- `init()`：新增断言 `external_connections_alive_ == 0`——外部连接仍持有上一轮 budget 的 credit 时不能
  重新 `init` 预算。
- `~QuicUdpEndpoint`：断言 `external_connections_alive_ == 0`。声明顺序错误（连接对象比 endpoint 活得久）
  在这里响。

为什么不需要 detach 时切断资源指针：endpoint **对象**在 `close()` 之后仍然存在，`crypto_block_pool_`、
`output_frame_pool_`、`recv_storage_budget_` 都是它的成员，`close()` 不释放它们
（`QuicUdpEndpoint.cpp:614-652`）；recv extent pool 是 loop 的（`QuicConnection.h:659`）。所以外部连接
在 `close()` 之后析构、把 crypto block 和 credit 归还给 endpoint 成员是内存安全的。UAF 文档里的方案 B
之所以脆弱，是因为它要对抗 endpoint 对象消失；这里由 RAII 顺序 + 析构断言保证对象存在。

### 4.7 `wait_closed()` 实现

`QuicConnection` 新增 `async::LocalWaitGroup closed_`：attach 时 `add()`，`detach_from_endpoint()`
末尾 `done()`；`wait_closed()` = 取 lease + `co_await closed_.join()`。`~QuicConnection` 断言
`!closed_.has_waiters()`（与 `handshake_gate_` 同款）。

### 4.8 server 0-RTT 单一来源（D10）

- `early_data_enabled()`：server 返回 `server_tls != nullptr && server_tls->enable_early_data`；
  client 返回 `attempt_early_data_`。
- `QuicTlsSession::init_server` 不再覆盖 `server_param_.enable_early_data`（`QuicTlsSession.cpp:296`）。
- 删除 `QuicUdpEndpoint::ServerAdmissionOptions::enable_early_data`、`Options::enable_early_data`
  及 `QuicUdpEndpoint.cpp:485`、`:1654` 的拷贝。`Http3Endpoint.cpp:60` 一行删除；
  `make_http3_server_tls_param` 已经把 `HttpServerTlsOptions::enable_early_data` 写进
  `TlsServerParam`（`TlsAlpn.cpp:37`），行为不变。
- 测试：`QuicClientTest.cpp:932`、`QuicConnectionTest.cpp:509` 改为通过 `TlsServerParam` 打开。

### 4.9 删除项

- `include/fiber/quic/QuicClient.h`、`src/quic/QuicClient.cpp`。
- `QuicClientAttempt`、`QuicClientConnectOptions`、`QuicClient::Options`。
- `QuicConnection` 与 `QuicUdpEndpoint` 中的 `friend class QuicClient`。

---

## 5. HTTP/3 层设计

### 5.1 `Http3Client`

只剩共享配置，不再有 `init()`：

```cpp
class Http3Client : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::TlsClientSecurity tls{.verify_peer = true};
        quic::QuicClientCacheOps cache{};
        Http3Settings local_settings{};
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(3);
        std::uint32_t max_qpack_string_size = 64 * 1024;
        std::size_t max_field_section_size = 128 * 1024;
    };
    // Credential and trust-store pointers are borrowed; their material, the
    // endpoint and this object must outlive every connection built on it.
    Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept;

    [[nodiscard]] quic::QuicUdpEndpoint &endpoint() const noexcept;
    [[nodiscard]] const Options &options() const noexcept;
    [[nodiscard]] std::span<const std::string_view> alpn() const noexcept;   // {"h3"}, 自持 TlsAlpnList
    [[nodiscard]] const net::TlsCredential *tls_credential() const noexcept;
    [[nodiscard]] const net::TrustStore *trust_store() const noexcept;
};
```

`local_settings.max_field_section_size == 0` 时回退到 `max_field_section_size` 的逻辑
（`Http3Client.cpp:52-54`）搬到连接构造。

### 5.2 `Http3ClientConnectOptions`

```cpp
struct Http3ClientConnectOptions {
    net::SocketAddress remote_addr{};
    // Borrowed until the Http3ClientConnection constructor returns; the
    // connection keeps its own copies for the session cache key.
    std::string_view server_name{};
    std::string_view verify_name{};
    quic::QuicTransportSettings transport{};
    quic::QuicRecvFlowControlSettings recv_flow{};
    std::chrono::milliseconds keepalive_interval{0};
    std::chrono::milliseconds handshake_timeout{net::kDefaultTlsHandshakeTimeout};
    bool allow_insecure = false;
};
```

### 5.3 `Http3ClientConnection`

`Http3ClientConnectionImpl` 与句柄合并为一个公开值类型，同名 `Http3ClientConnection`
（源码不兼容变更，仓库内消费者全部迁移）。

```cpp
class Http3ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    // On the endpoint's loop. Allocates the QUIC identity from the endpoint and
    // builds the QUIC connection in place; nothing is registered or sent until
    // connect(). Construction never fails: an identity allocation failure is
    // reported by connect() as phase Quic / QuicConnectPhase::Connection.
    Http3ClientConnection(Http3Client &client, const Http3ClientConnectOptions &options) noexcept;
    // Never connected, or closed and detached from the endpoint, with no
    // exchange, request or wait_closed() joiner alive. Asserted.
    ~Http3ClientConnection();

    // Exactly once. Loads the session cache, runs QUIC connect, waits for the
    // handshake (options.handshake_timeout), verifies ALPN "h3" and starts the
    // HTTP/3 control streams. A failure after the connection attached closes
    // it immediately and waits for detach before returning, so a failed
    // connect() always leaves this object destructible.
    [[nodiscard]] async::Task<Http3ClientConnectResult> connect() noexcept;

    [[nodiscard]] ClientHttp3Exchange open_exchange(mem::BufPool &pool) noexcept;
    void shutdown(Http3ErrorCode error = Http3ErrorCode::RequestCancelled) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    // Joins the H3 start/reader/drain tasks and then waits for the QUIC
    // connection to detach from the endpoint. After it returns the object may
    // be destroyed.
    [[nodiscard]] async::Task<void> wait_closed() noexcept;

    // 状态查询与今天的句柄一致：accepting_requests / state / close_error /
    // peer_settings_received / local_settings / peer_settings / peer_goaway_* / quic()
    ...

private:
    static quic::QuicConnection::Options make_quic_options(Http3Client &, const Http3ClientConnectOptions &,
                                                           Http3ClientConnection *owner,
                                                           common::IoErr &identity_error) noexcept;
    static bool on_new_tls_session(void *, quic::QuicConnection &, SSL_SESSION *) noexcept;
    static void on_new_token(void *, quic::QuicConnection &, const std::uint8_t *, std::size_t) noexcept;
    [[nodiscard]] quic::QuicClientCacheKey cache_key() const noexcept;

    Http3Client &client_;
    const std::string server_name_;
    const std::string verify_name_;
    const net::SocketAddress remote_addr_;      // 原始目标; quic_.remote_addr() 会随迁移变化
    const std::chrono::milliseconds handshake_timeout_;
    const bool allow_insecure_;
    common::IoErr identity_error_ = common::IoErr::None;
    quic::QuicConnection quic_;                 // on_destroy = nullptr
    quic::QuicLocalStreamGate local_stream_gate_;
    Http3ControlStreams control_;
    // 其余成员原样来自 Http3ClientConnectionImpl
    ...
};

using Http3ClientConnectResult = std::expected<void, Http3ClientConnectError>;
```

`make_quic_options` 内容（原 `Http3Client::connect` 的 `quic_options` 组装 + `QuicClient::start_connect`
的 `connection_options` 组装 + `Impl::make_quic_options`）：

- `role = Client`，`local_addr = endpoint.local_addr()`，`remote_addr = options.remote_addr`。
- 身份：`endpoint.allocate_client_identity()`；失败写入 `identity_error`，CID 留空。
  `original_destination_connection_id = initial_destination_connection_id = remote_connection_id = odcid`，
  `local_connection_id = local_cid`。
- `transport`：`options.transport`，`initial_max_streams_bidi = 0`，
  `initial_max_streams_uni = max(., 16)`；`max_peer_bidirectional_streams = 0`，
  `max_peer_unidirectional_streams = 16`（`Http3Client.cpp:83-87`）。
- `keepalive_interval`、`recv_flow` 透传。
- `owner = this`，`ops = {create_stream, on_peer_stream_attached, on_state_change, on_capacity_change,
  on_new_tls_session, on_new_token}`。`on_new_*` 仅在 `client.options().cache.store_*` 非空时设置。
- `destroy_owner = nullptr`，`on_destroy = nullptr`。

`connect()` 顺序：

1. `state_ == Prepared` 且 `identity_error_ == None` 且 `quic_.loop().in_loop()`，否则
   `ClientInit` / `Quic(Connection)` 错误。
2. cache load：`client_.options().cache.load` 非空时以 `cache_key()` 查询；token 长度校验与今天一致
   （`QuicClient.cpp:187-196`）。
3. 组 `QuicClientConnectParams`：`tls.security = client_.options().tls`，
   `tls.min_version = tls.max_version = 0x0304`，`tls.alpn = client_.alpn()`，
   `tls.server_name = server_name_`，`tls.verify_name = verify_name_`；`allow_insecure_`；
   `resumption_session = cached.session`；`token`；`attempt_early_data = false`（H3 层继续禁用 0-RTT）。
4. `quic_.connect(params)`；失败 → `{phase = Quic, io_error, quic_error = quic_.connect_error(err)}`。
   此时连接未 attach、已 Closed，对象可析构。
5. `co_await quic_.wait_established(handshake_timeout_)`；失败 → 若未终态则
   `quic_.close_immediately(NoError)`，`co_await quic_.wait_closed()`，返回
   `{phase = Quic, quic_error = quic_.connect_error(err)}`。
6. `quic_.tls().selected_alpn() != "h3"` → `close(VersionFallback)` 改为
   `quic_.close_immediately(...)` 路径（见 §7 开放问题 1），`co_await quic_.wait_closed()`，返回 `Alpn`。
7. 原 `Impl::start()`（control streams）；失败 → `close(InternalError)`，`co_await wait_closed()`，返回 `Http3`。
8. 返回 `{}`。

cache 钩子：

```cpp
bool Http3ClientConnection::on_new_tls_session(void *owner, quic::QuicConnection &, SSL_SESSION *session) noexcept {
    auto &self = *static_cast<Http3ClientConnection *>(owner);
    const auto &cache = self.client_.options().cache;
    return cache.store_session(cache.owner, self.cache_key(), session, self.quic_.peer_transport().params);
}
// cache_key(): {server_name_, verify_name_, remote_addr_, client_.tls_credential(), client_.trust_store()}
```

析构：

```cpp
Http3ClientConnection::~Http3ClientConnection() {
    FIBER_ASSERT(state_ == Http3ConnectionState::Prepared ||
                 (quic_.closed() && !quic_.attached_to_endpoint()));
    FIBER_ASSERT(client_requests_.empty() && client_request_group_.empty());
    FIBER_ASSERT(start_tasks_.empty() && drain_tasks_.empty());
    // ~QuicConnection 再断言 ref_count_ == 1
}
```

`shutdown()` / `graceful_shutdown()` / `close()` / `run_graceful_shutdown()` / `join_protocol_tasks()`
逻辑原样来自 Impl。`destroy_connection` 与 `make_handle` 删除。

### 5.4 `ClientHttp3Exchange`

`ClientHttp3Exchange(Http3ClientConnection &conn, BufPool &)` 签名不变；内部 `conn_` 从 `Impl*` 改为
`Http3ClientConnection*`。exchange 持有的 `QuicStream::Lease` 会 retain 连接，因此"析构连接前结束所有
exchange"由 `~QuicConnection` 的 `ref_count_ == 1` 断言兜底。

### 5.5 生命周期约束（替换 `feature/http3_client.md` §生命周期约束）

1. 调用者先初始化并启动一个 client-only 或混合角色的 `QuicUdpEndpoint`。
2. `Http3Client` 只是配置；它、endpoint 和 TLS 材料必须比所有连接对象活得久。
3. `Http3ClientConnection` 在 endpoint 的 loop 上构造；`connect()` 只能调用一次。
4. `open_exchange(pool)` 借用 `BufPool`；pool 和连接必须长于 exchange。放弃未完成的 exchange 时显式
   `abort()`。
5. 析构前必须 `shutdown()` 或 `graceful_shutdown()` 并 `co_await wait_closed()`；`wait_closed()` 返回
   意味着 H3 任务已 join 且 QUIC 已从 endpoint detach。`connect()` 失败的对象已经满足该条件。
6. `endpoint.shutdown()` 会关闭并 detach 尚存的连接，但不等待连接对象析构；对象可以在 `shutdown()`
   之后、`~QuicUdpEndpoint` 之前析构。
7. 单线程：连接的全部操作在其 endpoint loop 上。

---

## 6. 迁移与影响面

### 6.1 文件

| 文件 | 变更 |
|---|---|
| `include/fiber/quic/QuicConnection.h` / `src/quic/QuicConnection.cpp` | Options 裁剪；`Ops` 钩子；`connect()` / `connect_error()` / `wait_closed()`；外部所有权断言；`early_data_enabled()` 按 role |
| `include/fiber/quic/QuicClientConnect.h`（新） | cache/错误类型 + `QuicClientConnectParams` |
| `include/fiber/quic/QuicClient.h` / `src/quic/QuicClient.cpp` | 删除 |
| `include/fiber/quic/QuicUdpEndpoint.h` / `.cpp` | `allocate_client_identity()`；`hosted_` 对外部连接在 detach 时 done；`external_connections_alive_`；断言搬迁；删除 `enable_early_data` |
| `src/quic/QuicTlsSession.cpp` | `init_server` 不再覆盖 early data |
| `include/fiber/http/Http3Client.h` / `src/http/Http3Client.cpp` | 配置对象；删除 `init()`、工厂、`last_created_connection_` |
| `include/fiber/http/Http3ClientConnection.h` / `src/http/Http3ClientConnection.cpp` | 值类型；吸收 Impl |
| `src/http/Http3ClientConnectionImpl.h` / `.cpp` | 删除 |
| `src/http/ClientHttp3Exchange.cpp` | `conn_` 类型 |
| `src/http/endpoint/Http3Endpoint.cpp` | 删除 `admission.enable_early_data` |
| `tests/QuicClientTest.cpp` | 8 个用例改写到 `QuicConnection::connect()`；至少一个用例使用外部所有权（栈/成员构造、`on_destroy = nullptr`）覆盖 §4.5-4.7 |
| `tests/QuicConnectionTest.cpp:509`、`tests/QuicClientTest.cpp:932` | early data 改走 `TlsServerParam` |
| `tests/Http3ClientTest.cpp`、`tests/Http3EndpointTest.cpp`、`apps/lite_nginx/tests/LiteNginxRuntimeTest.cpp:1322-1344` | 去掉 `init()`；`Http3ClientConnection conn(client, options); co_await conn.connect()`；析构前 `wait_closed()` |
| `example/http3_benchmark_client.cpp:955-981` | 同上 |
| `feature/quic_client.md`、`feature/http3_client.md` | 顶部加状态说明指向本文；组件边界与生命周期段落更新 |

### 6.2 `QuicClientTest` 的新写法

```cpp
auto identity = endpoint.allocate_client_identity();
fiber::quic::QuicConnection::Options options{};
options.role = fiber::quic::QuicConnectionRole::Client;
options.local_addr = endpoint.local_addr();
options.remote_addr = {loopback_v4(), 4433};
options.original_destination_connection_id = identity->original_destination_connection_id;
options.initial_destination_connection_id = identity->original_destination_connection_id;
options.remote_connection_id = identity->original_destination_connection_id;
options.local_connection_id = identity->local_connection_id;
options.ops.create_stream = create_stream;
fiber::quic::QuicConnection connection(endpoint, options);      // 外部所有权

fiber::quic::QuicClientConnectParams params{};
params.tls.security = security;
params.tls.min_version = params.tls.max_version = 0x0304;
params.tls.alpn = alpn.view();
params.tls.server_name = "localhost";
params.allow_insecure = true;
auto connected = connection.connect(params);
auto established = co_await connection.wait_established(5s);
// ... 断言 ...
connection.close_immediately();
co_await connection.wait_closed();
```

测试里"超时后 cancel 并 detach"（`HandshakeTimeoutCancelsAndDetachesConnection`）改为
`wait_established(timeout)` 超时 → `close_immediately()` → `wait_closed()` → 断言
`endpoint.active_connection_count() == 0`。

### 6.3 实施顺序

两个 PR，第一个就拿到 Options 的全部收益，第二个是 API 形态变更，可独立评审。

**PR 1 — `refactor(quic): fold client connect into QuicConnection`**

1. §4.2 `QuicClientConnect.h`、`QuicConnection::connect()` / `connect_error()` / `wait_closed()`。
2. §4.3 `Ops` 钩子；§4.1 Options 裁剪；§4.8 server early data 单一来源。
3. §4.4 `allocate_client_identity()`；§4.5-4.6 外部所有权与 endpoint 计数。
4. 删除 `QuicClient`。
5. `Http3Client` 内部过渡：仍在堆上 `new` Impl 并用 lease 持有（`on_destroy` 保留），但由
   `Http3Client::connect` 直接构造 Impl、调用 `impl->quic().connect(params)`、`wait_established`；
   Impl 实现 `on_new_tls_session` / `on_new_token` 并自持名字副本。工厂回调与
   `last_created_connection_` 在此步删除。公开的 `Http3ClientConnection` 句柄 API 不变，H3 测试不动。
6. `QuicClientTest.cpp` 改写。

验证：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/fiber_tests --gtest_filter='QuicClientTest.*:QuicClientMtlsTest.*:QuicUdpEndpointTest.*:Http3ClientTest.*:Http3EndpointTest.*'
```

**PR 2 — `refactor(http): make Http3ClientConnection a caller-owned value type`**

1. §5.1-5.4 合并 Impl 与句柄；删除 `init()`。
2. 调用方迁移（§6.1 表中 tests / example / lite_nginx tests）。
3. `feature/http3_client.md`、`feature/quic_client.md` 更新。

验证同上，另加 ASan 一轮（构建方式见 `feature/fiber-lib-h3-client-teardown-uaf.md` §复现）跑
`Http3EndpointTest.*:Http3ClientTest.*`，确认值类型析构路径没有晚期触点。

---

## 7. 开放问题

1. **`connect()` 失败与 `shutdown()` 是否应跳过 3×PTO closing 期**。`close_application()` 走完整的
   closing 期（`QuicConnection.h:576-587`），`wait_closed()` 因此可能等 3×PTO（loopback 约 100 ms）。
   `connect()` 的失败路径本文选 `close_immediately`（transport 级 CC）；正常 `shutdown()` 保持
   application close 与 closing 期不变。若测试或压测觉得慢，再给 `shutdown()` 加 `immediate` 参数，
   不在本次范围。
2. **`Http3ClientConnection` 是否需要 `std::string` 副本以外的更省的形态**。两个 `std::string` +
   `SocketAddress` 每连接约 92 字节，在用户对象上而非 `QuicConnection` 内，接受。
3. **`hosted_` 对外部连接在 detach 时 `done()`**：需要一个 `EndpointAttachment` 之外的位来避免析构时重复
   `done()`；实现时用 `on_destroy_ == nullptr` 判断即可，不加新状态。
4. `QuicUdpEndpointTest.cpp` 里直接构造 client 连接的用例（`:1138` 等）仍走 `on_destroy` 堆模式，
   不强制迁移；但需至少一处覆盖外部所有权模式（放在 `QuicClientTest`）。

---

## 8. 验收标准

1. `sizeof(QuicConnection::Options)` 从 672 降到 ≤ 460；`grep -rn "client_" include/fiber/quic/QuicConnection.h`
   无命中。
2. `QuicClient.h` / `QuicClient.cpp` / `Http3ClientConnectionImpl.*` 不存在；仓库内无 `create_connection`
   的 client 侧调用；`QuicConnection` / `QuicUdpEndpoint` 无 `friend class QuicClient`。
3. `Http3ClientConnection` 满足 `!std::is_move_constructible_v`；`Http3Client` 无 `init()`。
4. server early data 只有 `TlsServerParam::enable_early_data` 一个入口；
   `TlsAlpnTest.*`、`QuicClientTest.ReusesSessionAndNewTokenWithEarlyData` 通过。
5. 外部所有权用例：连接对象在 `endpoint.shutdown()` 之后析构，Release 与 ASan 均无报告；
   连接对象比 endpoint 活得久的错误顺序在 Debug 下命中 `~QuicUdpEndpoint` 断言。
6. `ctest --test-dir build` 全绿；`Http3ClientTest.NginxInterop`（设置 `FIBER_HTTP3_NGINX_PORT` 时）通过。
