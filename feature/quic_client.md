# QUIC Client 实现方案

## Status: Implemented / Validated

## 0. 落地状态（2026-07-20）

本方案已经落地到 QUIC transport 层。后续 HTTP/3 client 也已基于该边界实现，详见
[`http3_client.md`](http3_client.md)。两层实现保持“一个角色中立的 UDP endpoint +
`QuicClient` 主动建连编排层”的核心结构，没有增加 send-only endpoint。

已完成：

- `QuicUdpEndpoint::EndpointOptions` 与 `ServerAdmissionOptions`，并保留旧 `Options` 兼容入口。
- client-only、server-only 和混合连接模式；`Http3Server` 已迁移到拆分后的配置入口。
- `QuicClient`、move-only `QuicClientAttempt`、connect/confirmed waiter、绝对握手 deadline、
  cancel 和结构化 `QuicConnectError`。
- TLS client 的 SNI、ALPN、CA/default trust、hostname/IP SAN 校验和显式 insecure 测试开关。
- ODCID、current Initial DCID、Server Initial SCID、Retry SCID 和 Initial token 的独立状态。
- Retry integrity 校验、Initial key 重新派生、recovery/congestion/pacer reset，以及 QUIC v1
  Version Negotiation 处理。
- external TLS session/remembered transport/NEW_TOKEN cache 回调和 server identity 绑定。
- 显式 `ReplaySafe` 0-RTT stream、accepted/rejected 路径；rejection 会删除 early stream/data，
  不把应用数据静默重放为 1-RTT。
- preferred-address codec、CID/token 安装、`PATH_CHALLENGE/PATH_RESPONSE` 验证后切换。
- wildcard local address 在首个认证服务端包后锁定；客户端不受服务端 anti-amplification
  限制。
- endpoint 固定桶 stateless-reset token 反向索引；索引节点内嵌在 remote CID slot，unknown
  DCID 不再遍历全部连接，并覆盖 server transport parameter 提供的 sequence-0 token。
- ACK delay/exponent 从每条连接的本地 transport settings 读取，不再误用 server endpoint
  默认值。

新增回归集中在 `tests/QuicClientTest.cpp`，并扩展 connection、shutdown、packet codec 和
transport-parameter codec 测试。最终构建和完整 CTest 结果记录在本文末尾。

## 1. 背景与结论

当前 QUIC 实现已经具备连接、stream、ACK、丢包恢复、拥塞控制、pacing、连接 ID、
路径管理和服务端 TLS 等公共能力，但整体入口仍以服务端为中心：

- `QuicUdpEndpoint::Options` 强制要求服务端 `create_connection` callback。
- unknown DCID 的收包路径直接进入 Version Negotiation、Retry 和服务端连接创建。
- `QuicUdpEndpoint::create_connection` 固定构造 `QuicRole::Server`。
- `QuicTlsSession` 只有 `init_server()`。
- 客户端收到 Retry 或 Version Negotiation 时当前直接拒绝。
- `QuicConnection::Options` 中的 CID 同时承担初始 CID、当前远端 CID 和路径 CID 等
  多种语义，不能正确表达 Retry 前后的客户端状态。

本方案的核心决策是：

> 改造现有 `QuicUdpEndpoint`，使其成为角色无关的 UDP 数据面；新增
> `QuicClient` 作为主动建连编排层。不要新增一个只负责发包的 UDP endpoint。

QUIC 客户端的 Initial、Retry、Version Negotiation、Handshake、1-RTT、路径验证和
stateless reset 必须在同一个 UDP socket 上完成。独立的 send-only endpoint 无法自然接收
握手响应，还会复制现有 CID 分流、批量收发、GSO、send scheduler、pacing、timer 和
detach 生命周期。

最终组件关系如下：

```text
Application
    |
    +-- QuicClient
    |     +-- client connect orchestration
    |     +-- TLS/SNI/ALPN/peer verification
    |     +-- Retry/VN/session/NEW_TOKEN/0-RTT
    |     +-- connect waiter/timeout/cancel
    |
    +-- QuicConnection
          +-- connection state/CID/streams/flow control
          +-- TLS session/packet protection keys
          +-- ACK/loss recovery/congestion control/path
                    |
                    v
             QuicUdpEndpoint
                  +-- one UDP socket
                  +-- CID/reset-token dispatch
                  +-- recv/send batching and GSO
                  +-- QuicSendScheduler
                  +-- optional server admission
```

同一个 `QuicUdpEndpoint` 可以处于以下任一使用模式：

1. 仅承载主动客户端连接。
2. 仅接受服务端连接。
3. 同时承载主动客户端连接和被动服务端连接。

## 2. 目标

1. 实现可与标准 QUIC v1 服务端互操作的 transport client。
2. 复用现有 UDP、connection、stream、ACK、recovery、congestion 和 pacing 热路径。
3. 支持安全的 TLS client handshake、SNI、ALPN、证书链和 hostname/IP SAN 校验。
4. 支持 Retry、Version Negotiation 和客户端 CID 协商。
5. 支持 NEW_TOKEN、TLS session resumption 和显式启用的 0-RTT。
6. 支持服务端 preferred address、同 socket 路径验证和 NAT rebinding 所需状态。
7. 明确连接创建、endpoint 挂载、waiter、timer 和 `Lease` 的所有权边界。
8. 保持无异常、owner-loop 单线程和性能优先的现有代码约束。
9. 不改变现有 QUIC/HTTP3 服务端的协议行为。

## 3. 非目标与完整性边界

- 本阶段不实现 HTTP/3 client、QPACK、HTTP request/response 或 WebTransport。
- `QuicClient` 不内置 DNS；调用者传入已经解析的远端 `SocketAddress`。
- 只支持 QUIC v1。版本选择逻辑会保留扩展点，但本阶段不实现 QUIC v2。
- 不实现 multipath QUIC。
- 第一版支持同一 UDP socket 上的 preferred-address/path validation 和系统路由变化，
  不支持把一条活动连接迁移到另一个 `QuicUdpEndpoint`/本地 UDP socket。跨 endpoint
  迁移需要单独的 multi-attachment 重构，不属于 QUIC v1 基础互操作前提。
- Nginx 只作为服务端 endpoint、TLS callback 和互操作行为参考，不作为客户端协议规范。
  Retry、Version Negotiation、0-RTT 和 CID 规则以 RFC 9000/9001/9002 及 BoringSSL
  QUIC API 契约为准。

后续 HTTP/3 client 只需在 `QuicClient` 返回的 `QuicConnection::Lease` 上创建 H3 control、
QPACK 和 request streams，不需要改动 UDP endpoint 或 QUIC 建连状态机。

## 4. 核心不变量

### 4.1 Socket 与线程

- 每条连接在本阶段只挂载到一个 `QuicUdpEndpoint`。
- 客户端发送和接收使用 endpoint 的同一个 UDP socket。
- `QuicClient`、`QuicConnection`、endpoint、send scheduler 和所有 timer 属于同一个
  `EventLoop`。
- 公共 API 只允许在 owner loop 调用并在边界断言，不为稳态收发增加 mutex。

### 4.2 生命周期

- endpoint 持有每条已挂载连接的一份 `QuicConnection::Lease`。
- `QuicClientAttempt` 或成功的调用者持有另一份 lease。
- connect waiter 必须在 endpoint detach 和最后一份 lease 释放之前完成或取消。
- endpoint detach 必须删除 local CID、remote reset-token、connection list、send queue 和
  timer 上的所有 intrusive 节点。
- 连接析构时断言其 endpoint、waiter、timer 和索引节点均已解除。

### 4.3 性能

- packet 收发继续复用 `QuicUdpEndpoint` 的 batch、GSO 和 `QuicSendScheduler`。
- CID 和 stateless-reset token 索引使用 intrusive/fixed-slot 节点，不做逐包分配。
- Retry token、session 和连接配置允许在建连冷路径分配，但必须设置容量上限。
- callback 使用函数指针和 `void *owner`，不在核心路径引入 `std::function`。
- callback、timer 和内部协议处理路径保持 `noexcept`。

## 5. 新增客户端公共类型

新增文件：

- `include/fiber/quic/QuicClient.h`
- `src/quic/QuicClient.cpp`

### 5.1 `QuicClient`

`QuicClient` 是绑定 endpoint、`TlsClientSecurity`、ALPN、cache 和 owner loop 的长期 connector，不代表
单条 QUIC 连接。

建议接口形态：

```cpp
class QuicClient {
public:
    common::IoResult<void> init(QuicUdpEndpoint &endpoint,
                                net::TlsClientSecurity tls_security,
                                Options options) noexcept;

    [[nodiscard]] std::expected<QuicClientAttempt, QuicConnectError>
    start_connect(const QuicClientConnectOptions &options) noexcept;

    [[nodiscard]] async::Task<QuicConnectResult>
    connect(const QuicClientConnectOptions &options) noexcept;

    void shutdown() noexcept;
};
```

`start_connect()` 同步完成：

1. 校验 owner loop、endpoint 状态和 connect options。
2. 生成 ODCID 和客户端 Initial SCID。
3. 调用客户端 connection factory。
4. 初始化 connection、path、Initial keys 和 client TLS。
5. 事务式挂载到 endpoint 并注册本地 CID。
6. 生成 ClientHello CRYPTO 数据。
7. 把第一个 Initial 加入现有 send scheduler。
8. 返回持有 caller lease 的 `QuicClientAttempt`。

任何一步失败都必须回滚前面已经建立的索引、timer、queue 和 lease。首包只能在整个
注册事务成功之后进入发送队列。

`connect()` 是便捷协程接口：内部调用 `start_connect()`，等待连接达到 1-RTT ready，
成功后把 attempt 持有的 lease 移交给调用者。

### 5.2 `QuicClientAttempt`

`QuicClientAttempt` 是 move-only 类型，表示一条尚未成功或失败的客户端连接。

接口职责：

- `connection()`：取得当前连接，用于显式允许的 0-RTT 操作。
- `wait_connected(timeout)`：等待 TLS、ALPN、证书、transport parameters 和 1-RTT keys
  全部就绪。
- `wait_confirmed(timeout)`：等待客户端收到 `HANDSHAKE_DONE`。
- `cancel()`：取消连接尝试。

取消语义：

- 已有可安全使用的写密钥时，进入正常 transport close。
- 尚未收到任何认证服务端包且无法安全发送 close 时，本地终止并从 endpoint detach。
- attempt 析构不允许留下悬挂 coroutine waiter；若调用者没有显式移交连接，则执行与
  `cancel()` 等价的 owner-loop 清理。

### 5.3 `QuicClientConnectOptions`

包含以下配置：

- 已解析的远端 `SocketAddress`。
- 可选本地绑定地址；允许 unspecified IP 交给内核选路。
- `server_name`，写入 SNI，并在 `verify_name` 为空时作为证书校验名。
- 可选 `verify_name`，允许证书校验目标与 SNI 独立。
- handshake timeout 和 idle timeout。
- 本地 QUIC transport parameters、flow-control 和 stream limits。
- 应用 `owner`、`QuicConnection::Ops` 和 client connection factory。
- session/token cache key 或生成 key 所需字段。
- 是否允许 insecure peer，仅用于显式测试配置。
- 是否请求 0-RTT，以及应用的 replay-safe 策略。
- preferred-address policy。

配置边界可使用 `std::string_view`/`std::span`；需要跨异步阶段保存的字段必须复制到
连接自己的有上限存储，不能保留调用者临时 view。

### 5.4 连接错误

新增 `QuicConnectError`，避免把所有失败压缩为 `IoErr::ProtocolError`：

```text
QuicConnectError
    phase                 endpoint/initial/retry/vn/tls/tp/timeout/peer-close
    io_error              底层 IoErr
    tls_alert             TLS alert（若存在）
    tls_verify_result     证书校验结果（若存在）
    transport_error       QUIC transport error（若存在）
    frame_type            触发 transport close 的 frame type
    application_error     peer application close（若存在）
    offered_version       VN/版本错误上下文
```

`QuicConnectResult` 使用 C++23 `std::expected<QuicConnection::Lease,
QuicConnectError>`。整个设计不使用 C++ exception。

## 6. `QuicUdpEndpoint` 角色中立化

修改文件：

- `include/fiber/quic/QuicUdpEndpoint.h`
- `src/quic/QuicUdpEndpoint.cpp`
- `include/fiber/http/Http3Server.h`
- `src/http/Http3Server.cpp`

### 6.1 拆分 endpoint 与服务端 admission 配置

将当前 `QuicUdpEndpoint::Options` 拆成：

#### `EndpointOptions`

角色无关字段：

- socket/local address/reuse 配置。
- recv/send batch 和 GSO 配置。
- buffer、pool、storage budget。
- endpoint connection 数量上限。
- 本地 CID 长度/生成策略。
- 生成本地 CID stateless-reset token 的 secret。

reset-token secret 是角色无关配置：客户端后续通过 `NEW_CONNECTION_ID` 发出的本地 CID
同样需要携带 stateless reset token。只有“对 unknown packet 发送 stateless response”
属于服务端 admission 行为。

#### `ServerAdmissionOptions`

仅服务端字段：

- server connection factory 和 owner。
- 指向不可变 TLS context 配置的 `TlsServerConnectionOptions`。
- Retry/token policy。
- 服务端 transport parameters 和 flow-control defaults。
- unknown Initial admission limits。
- Version Negotiation、Retry、stateless reset response 的 rate limit。
- 服务端 early-data policy。

提供两个初始化重载：

```cpp
init(loop, endpoint_options)
init(loop, endpoint_options, server_admission_options)
```

两者都允许后续挂载主动客户端连接。第二个重载额外设置明确的
`server_admission_enabled_`。服务端配置在 init 边界完成拷贝和验证，稳态路径不通过多个
nullable pointer 推断运行模式。

`Http3Server` 只需要把现有配置分别填入公共 options 和 server admission options，不改变
其 QUIC/H3 行为。

### 6.2 主动连接挂载

增加仅供 `QuicClient` 使用的内部能力：

- 生成客户端 local CID，并对 endpoint CID tree 做有界冲突重试。
- 事务式 `attach_outbound_connection()`。
- 注册 Initial local SCID。
- 把连接加入 endpoint connection list。
- 绑定 connection 的唯一 send entry。
- endpoint 取得自己的 connection lease。
- 初始化失败时按注册逆序回滚。

不要把 `connect()` 或 TLS client 状态放入 endpoint。endpoint 只知道“已有一条主动构造的
connection 需要注册和收发”，客户端握手策略仍属于 `QuicClient`/`QuicConnection`。

### 6.3 收包分流顺序

新的 ingress 顺序：

```text
parse invariant header
    |
    +-- DCID matches connection
    |      |
    |      +-- client Retry/VN -> unprotected client control handler
    |      +-- otherwise       -> existing protected packet processor
    |
    +-- DCID unknown
           |
           +-- tail token matches remote reset-token index -> close matched connection
           +-- server admission enabled                    -> existing unknown-packet path
           +-- client-only endpoint                         -> silently drop
```

`Retry` 和 `Version Negotiation` handler 建议放在 `QuicPacketProcessor` 中，由 endpoint 在
查到 client connection 后调用。这样 endpoint 不需要理解 TLS、CID 协商和 recovery reset，
也避免 `QuicClient` 与 endpoint 形成包处理循环依赖。

### 6.4 Stateless-reset token 反向索引

当前连接内的 stateless reset 检测主要发生在“DCID 已命中但 AEAD 失败”之后。真正的
stateless reset 报文通常不会携带 endpoint 能命中的 DCID，因此需要 endpoint 级反向索引：

- key：对端提供的 16-byte stateless reset token。
- value：connection 和对应 remote CID slot。
- server transport parameter、preferred-address CID 或 `NEW_CONNECTION_ID` 安装时注册。
- `RETIRE_CONNECTION_ID`、slot 替换和 connection detach 时删除。
- unknown short-header datagram 在任何 stateless response 之前查询尾部 token。

remote CID slot 数量固定，可以把 intrusive index node 内嵌在 slot 中，避免额外 allocation。
连接关闭通知仍复用现有 `QuicCloseSource::StatelessReset`。

### 6.5 本地地址选择

当 initial path 的本地 IP 为 unspecified 时：

- 发送 Initial 不设置 `has_local`/pktinfo，由内核选择源地址。
- 第一个通过 AEAD 认证的服务端包到达后，用 datagram 的实际 local address 锁定 initial
  path。
- 在此之前，本地地址差异不能被误判为 path migration。
- 地址锁定后才允许发送路径显式设置 local pktinfo。

## 7. `QuicConnection` 客户端状态

修改文件：

- `include/fiber/quic/QuicConnection.h`
- `src/quic/QuicConnection.cpp`
- `include/fiber/quic/QuicConnectionId.h`
- `src/quic/QuicConnectionId.cpp`

### 7.1 CID 状态必须分离

客户端构造时并不知道 Server Initial SCID。Retry 又会改变 Initial 的发送 DCID。因此不能
继续让一个可变 `remote_connection_id` 同时承担建连身份、transport parameter 校验和
active path CID。

新增明确的握手 CID 状态：

| 字段 | 生命周期与用途 |
|---|---|
| `original_destination_connection_id` | 第一次 Client Initial 随机选择的 DCID，建立期间不可变 |
| `current_initial_destination_connection_id` | 当前 Initial 编码使用的 DCID；Retry 后更新为 Retry SCID |
| `local_initial_source_connection_id` | Client Initial SCID，也是 endpoint 初始注册的 local CID |
| `server_initial_source_connection_id` | 第一个认证成功的 Server Initial SCID |
| `retry_source_connection_id` | 接受 Retry 后保存的 Retry SCID |
| `initial_token` | NEW_TOKEN 或当前 Retry token 的 owned bounded buffer |

服务端 transport parameters 分别校验：

- `original_destination_connection_id` 必须匹配 ODCID。
- 存在 Retry 时，`retry_source_connection_id` 必须匹配 Retry SCID。
- `initial_source_connection_id` 必须匹配认证后的 Server Initial SCID。

`QuicConnection::Options` 只保留构造输入和不可变初始值；运行过程中变化的 CID、token 和
path address 移到专用 runtime state，不再通过修改 options 表达状态。

### 7.2 Server Initial SCID 采纳时机

客户端处理第一个 Server Initial 时必须按以下顺序：

1. 完成 header protection removal 和 AEAD 校验。
2. 确认该包来自配置的初始远端地址/允许的 path。
3. 记录 Server Initial SCID。
4. 将其安装为 remote CID sequence 0 和 active path peer CID。
5. 再处理该包中的 CRYPTO frame 并驱动 TLS。

这个顺序保证 TLS 在应用服务端 transport parameters 时已经有正确的
`initial_source_connection_id` 校验基准。第一个认证 Server Initial 之后，携带不同 SCID
的后续 Initial 必须丢弃。

### 7.3 Connection state 与 connect waiter

不增加第二套权威连接状态机，继续使用现有：

```text
Init -> Handshaking -> Established -> GracefulClosing/Closing -> Draining -> Closed
```

客户端语义：

- `Established`：TLS handshake 完成，peer certificate、hostname/IP、ALPN 和 transport
  parameters 已验证，1-RTT read/write keys 可用。
- `handshake_confirmed`：收到 `HANDSHAKE_DONE`，随后按现有规则丢弃 Handshake keys/space。
- `connect()` 默认在 `Established` 返回；需要确认语义的调用者使用
  `wait_confirmed()`。

connection 内新增 intrusive connect waiter。所有建立前发生的 local error、TLS error、
transport close、stateless reset、idle/handshake timeout 和 cancel 都必须以完整
`QuicConnectError` 唤醒 waiter。

### 7.4 NEW_TOKEN

客户端收到 `NEW_TOKEN` 后：

- 验证 frame 只出现在允许的 encryption level。
- 复制到有上限的 owned buffer。
- 通过 cache callback 保存到对应 server identity。
- 下一条同 identity 连接可把它作为 Initial token 使用。

Retry token 只能用于当前连接的 Retry 响应，不能缓存到下一条连接；NEW_TOKEN 和 Retry
token 必须使用不同的状态/类型表达。

## 8. Client TLS

修改文件：

- `include/fiber/quic/QuicTlsSession.h`
- `src/quic/QuicTlsSession.cpp`
- `include/fiber/net/TlsCredential.h`
- `include/fiber/net/TrustStore.h`
- `include/fiber/net/TlsParams.h`
- `src/net/detail/TlsSslFactory.cpp`

### 8.1 `QuicTlsSession::init_client`

新增 client 初始化入口，负责：

1. 创建 client-mode `SSL`。
2. 设置 QUIC method 和 connection back pointer/ex-data。
3. 在单个 SSL 上设置 SNI。
4. 设置 DNS hostname 或 IP SAN 校验目标。
5. 设置连接请求的 ALPN 列表。
6. 加载 cache 返回的 `SSL_SESSION`。
7. 编码并设置客户端 transport parameters。
8. 调用 `SSL_do_handshake()` 生成 ClientHello CRYPTO bytes。

共享 `SSL_CTX` 不能因为某次 connect 修改 server name、hostname 或 session；这些状态全部
设置在 per-connection `SSL` 上。

### 8.2 Transport parameter 解析位置

`add_handshake_data` callback 只负责接收 BoringSSL 产生的握手数据，并写入对应
encryption level 的 CRYPTO send queue，不在 callback 中按固定角色解析 peer transport
parameters。

每次 `SSL_do_handshake()` 有进展后调用统一的
`maybe_apply_peer_transport_parameters()`：

- 本端角色是 Client 时，peer owner 是 Server。
- 本端角色是 Server 时，peer owner 是 Client。
- peer parameters 只应用一次。
- TLS handshake 报成功前必须确认参数存在并通过 codec、owner 和连接身份校验。

### 8.3 证书与 ALPN 安全边界

客户端证书验证由 `TlsClientSecurity` 控制：

- `verify_peer=true` 时使用连接参数借用的 `TrustStore`。
- 系统默认 trust store 必须在创建 `TrustStore` 时显式选择。
- client SSL 设置 peer verification。
- DNS name 使用 hostname verification；IP literal 使用 IP SAN verification。

`TlsCredential` 只描述证书链和私钥，`TrustStore` 只描述信任锚；是否校验 peer 完全由每次连接的
`TlsClientSecurity::verify_peer` 决定。`QuicClient` 在此之上默认拒绝
`verify_peer=false` 的连接。只有显式 `allow_insecure=true` 才允许跳过校验，
该选项仅用于测试或受控环境。

ALPN 由长期 `QuicClient::Options` 持有，而不是每次 connect 或通用 TLS 参数持有。要求：

- client options 的 ALPN 列表不能为空。
- 服务端必须选择一个客户端提供的协议。
- 未选择或选择不匹配时，连接以 TLS/ALPN 错误结束。
- QUIC transport 层不硬编码 `h3`；未来 HTTP/3 client 自己传入 `h3`。

### 8.4 Post-handshake 与 TLS session cache

握手完成后收到 Application-level CRYPTO 数据时调用
`SSL_process_quic_post_handshake()`，以处理 `NewSessionTicket` 等 TLS 1.3 post-handshake
消息。

BoringSSL client session cache 使用 external cache：

- cache key 至少包含 server name、port、ALPN 和 TLS context identity。
- session 与服务器可记忆 transport parameter snapshot 一起保存。
- callback 同步运行在连接 owner loop，不能阻塞或跨线程恢复 connection。
- 如果 cache 保留 `SSL_SESSION`，必须按 BoringSSL 契约增加引用，并在替换/删除时释放。
- shared `SSL_CTX` 的 new-session callback 使用 SSL ex-data 找到当前
  `QuicTlsSession`/cache sink，不能假设所有 SSL 的 `app_data` 都是 QUIC connection。

## 9. Retry

修改文件：

- `include/fiber/quic/QuicPacketProcessor.h`
- `src/quic/QuicPacketProcessor.cpp`
- `src/quic/QuicPacketCodec.h`
- `src/quic/QuicPacketCodec.cpp`
- `src/quic/QuicCrypto.h`
- `src/quic/QuicCrypto.cpp`
- `src/quic/QuicLossRecovery.h`
- `src/quic/QuicLossRecovery.cpp`
- `include/fiber/quic/QuicCongestion.h + src/quic/QuicCongestion.cpp`
- `include/fiber/quic/QuicPacer.h + src/quic/QuicPacer.cpp`

### 9.1 Retry 接受条件

客户端最多接受一次 Retry，并且必须满足：

- 连接仍处于允许 Retry 的初始握手阶段。
- 尚未收到任何通过认证的服务端包。
- Retry DCID/SCID 与当前客户端握手 CID 状态一致。
- token 非空且未超过配置容量。
- Retry integrity tag 校验成功。

在 `QuicCrypto` 增加 Retry integrity validation helper：复用现有 Retry integrity tag
生成能力，按原始报文计算期望 tag，并使用 constant-time comparison。

### 9.2 接受 Retry 后的事务

接受 Retry 后必须一次性完成：

1. 保持 local Initial SCID 不变。
2. 保持 ODCID 不变。
3. 保存 Retry SCID。
4. 将当前 Initial DCID 更新为 Retry SCID。
5. 保存 Retry token，后续 Initial 编码携带该 token。
6. 销毁并按新 Initial DCID 重新派生 Initial read/write keys。
7. 保留原 ClientHello CRYPTO bytes 和 offset，不重新启动 TLS。
8. 重新排队未确认的 Initial CRYPTO/control frames。
9. 按 0-RTT policy 处理已发送的 0-RTT frames。
10. 重置旧路径的 loss/PTO、congestion 和 pacing 发送历史。

不能调用当前会把 packet number 清零的通用 packet-number-space `reset()`。新增专用
`reset_after_retry()`：

- 保留各 packet number space 的 `next_packet_number`。
- 保留 TLS 状态、ClientHello bytes、0-RTT/Application secrets。
- 移除旧 Initial/0-RTT sent-packet 的 bytes-in-flight accounting。
- 清空对应 loss time、PTO count 和 recovery epoch。
- congestion controller 回到新路径的初始窗口。
- pacer 重新按新的 congestion 状态初始化。

依据：

- [RFC 9000 §17.2.5 Retry](https://www.rfc-editor.org/rfc/rfc9000.html#section-17.2.5)
- [RFC 9000 §8.1.2 Address Validation Using Retry Packets](https://www.rfc-editor.org/rfc/rfc9000.html#section-8.1.2)
- [RFC 9002 §6.3 Handling Retry Packets](https://www.rfc-editor.org/rfc/rfc9002.html#section-6.3)

## 10. Version Negotiation

扩展 `QuicPacketHeader` 或 VN 专用 decode result，暴露完整 version list：

- version list 长度必须非零且为 4-byte 的整数倍。
- VN DCID/SCID 必须与客户端最初发送的 CID 对应。
- 收到任何通过认证的服务端包后忽略 VN。
- VN 列表如果包含客户端原始 offered version，视为可能的伪造/无效 VN 并丢弃。

当前只支持 v1：

- 合法 VN 且包含 v1：丢弃，继续等待真正服务端响应。
- 合法 VN 且不包含 v1：以 `UnsupportedVersion` 本地终止 connect attempt。
- VN 是不受保护的协商包，失败时不发送 QUIC `CONNECTION_CLOSE`。

版本选择封装成独立 helper，未来增加 v2 时只扩展 supported-version table 和重新开始
Initial 的策略，不改 endpoint ingress 结构。

依据：[RFC 9000 §6 Version Negotiation](https://www.rfc-editor.org/rfc/rfc9000.html#section-6)。

## 11. Packet Builder 与 Initial Token

修改 `QuicUdpEndpoint`/packet builder 创建 `QuicPacketEncodeSpec` 的位置：

- Client Initial 使用 `current_initial_destination_connection_id`。
- Client Initial SCID 使用固定的 `local_initial_source_connection_id`。
- 如果存在 NEW_TOKEN 或 Retry token，把 owned token view 填入 encode spec。
- 继续满足客户端第一个 Initial datagram 至少 1200 bytes 的要求。
- Retry 之后的 Initial 使用新的 Initial keys，但 packet number 不回退。

发送 level 选择从固定的 `Initial/Handshake/Application` 改为按连接状态动态选择：

```text
Initial -> EarlyData -> Handshake -> Application
```

- 没有 early write keys 或未显式启用 0-RTT 时跳过 EarlyData。
- 1-RTT write keys 可用后停止构造 0-RTT packet。
- EarlyData 和 Application 共享 Application packet number space，但 sent-packet metadata
  必须记录实际 encryption level/`zero_rtt` 标记，以支持 rejection 时只清除 0-RTT
  recovery 状态而不重置 packet number。

## 12. Transport Parameters

修改文件：

- `src/quic/QuicTransportParamsCodec.h`
- `src/quic/QuicTransportParamsCodec.cpp`

### 12.1 角色与身份校验

- 客户端发送的 transport parameters 不能包含 server-only 参数。
- 服务端参数必须完成 ODCID、Retry SCID 和 Server Initial SCID 的独立校验。
- duplicate parameter 继续按现有规则作为 transport parameter error。
- `active_connection_id_limit` 等参数应用后继续复用现有固定 CID slots，不引入动态容器。

### 12.2 `preferred_address`

补齐当前缺失的 preferred-address codec 和语义：

```text
IPv4 address + port
IPv6 address + port
connection ID length + connection ID
16-byte stateless reset token
```

要求：

- 严格检查 wire length。
- preferred-address CID 不能为空。
- 该参数只能由 Server 发送。
- preferred CID 安装为 remote CID sequence 1。
- preferred CID 和 reset token 计入 CID limit 并注册到 endpoint reset-token index。
- 握手确认前不切换路径。

依据：[RFC 9000 §9.6 Preferred Address](https://www.rfc-editor.org/rfc/rfc9000.html#section-9.6)
和 [RFC 9000 §18.2 Transport Parameter Definitions](https://www.rfc-editor.org/rfc/rfc9000.html#section-18.2)。

### 12.3 0-RTT remembered parameters

session cache 保存服务器允许记忆的 transport parameters。以下参数不能作为下一次
0-RTT 的 remembered limit 使用：

- `ack_delay_exponent`
- `max_ack_delay`
- `preferred_address`
- `stateless_reset_token`
- ODCID、initial SCID、retry SCID 等连接身份参数

如果服务器接受 0-RTT，本次服务器参数不能把以下限制降低到缓存值以下：

- `active_connection_id_limit`
- `initial_max_data`
- `initial_max_stream_data_bidi_local`
- `initial_max_stream_data_bidi_remote`
- `initial_max_stream_data_uni`
- `initial_max_streams_bidi`
- `initial_max_streams_uni`

违反时以 transport parameter/protocol error 关闭连接。依据：
[RFC 9000 §7.4.1 Values of Transport Parameters for 0-RTT](https://www.rfc-editor.org/rfc/rfc9000.html#section-7.4.1)。

## 13. 0-RTT

0-RTT 是最终完整客户端的一部分，但默认关闭。应用只有显式声明数据可重放后才能使用。

### 13.1 Cache 输入

发起 0-RTT 需要 cache 同时提供：

- 可恢复的 `SSL_SESSION`。
- 上次服务器允许记忆的 transport parameters。
- 可选的 NEW_TOKEN；token 与 TLS session 独立，缺少 token 不阻止 TLS 0-RTT，但可能
  导致服务端再次做地址验证。

### 13.2 Stream 发送策略

为本地 stream 增加 early-data mode：

- `OneRttOnly`：默认值，握手期间可以排队，但不能编码进 0-RTT packet。
- `ReplaySafe`：应用显式声明可重放，允许使用 early keys。

建议给 `attach_local_stream()` 增加带默认值的 mode 参数，保持现有调用行为不变。
`QuicStream` 记录该 mode 以及是否在 0-RTT 阶段创建。packet builder 只从
`ReplaySafe` stream 选择 EarlyData STREAM frames。

### 13.3 0-RTT 被接受

- TLS 确认 early data accepted 后校验本次服务器 transport parameters 未降低 remembered
  limits。
- 0-RTT sent packets 继续按 Application packet number space 正常 ACK/loss recovery。
- 安装 1-RTT write keys 后停止使用 early write keys。

### 13.4 0-RTT 被拒绝

处理 BoringSSL early-data rejection：

1. 调用 `SSL_reset_early_data_reject()`。
2. 丢弃 early read/write keys。
3. 移除 0-RTT sent-packet 的 in-flight/recovery 状态，但保持 Application
   `next_packet_number`。
4. 恢复发送 0-RTT 前的 stream ID counters 和 flow-control snapshot。
5. detach 所有 0-RTT 阶段创建的 stream。
6. 调用应用 `on_early_data_rejected` callback。
7. 不把应用 STREAM 数据自动转换成 1-RTT 重发。
8. 允许纯 transport control state 按正常规则重新生成。

应用必须重建其请求/事务状态后决定是否以 1-RTT 重试，避免传输层造成不可见的业务重放。
依据：[RFC 9001 §4.6.3 Sending 0-RTT](https://www.rfc-editor.org/rfc/rfc9001.html#section-4.6.3)。

## 14. Path、anti-amplification 与 preferred address

修改文件：

- `include/fiber/quic/QuicPath.h`
- `include/fiber/quic/QuicPathManager.h`
- `src/quic/QuicPathManager.cpp`

### 14.1 Client 不受服务端 anti-amplification 限制

当前未验证 path 的 `send_limit` 不能无条件应用三倍接收字节限制。改为 role-aware：

- Server 未验证 path：保留 3x anti-amplification limit。
- Client path：不应用服务端 anti-amplification limit。

不能通过把 client initial path 伪装为 `validated=true` 绕过限制，因为 path validation
状态还影响迁移、challenge 和 preferred address。

### 14.2 Initial path 地址锁定

- 初始 remote address 来自 connect options。
- unspecified local address 在第一个认证服务端包后锁定实际地址。
- 第一个认证服务端包不能因为 local address 从 wildcard 变为实际地址而创建第二条 path。
- Retry/VN 尚未认证，不能用于锁定本地 path 或放宽服务端源地址。

### 14.3 服务端地址变化

- 普通服务端包来自未经授权的新 remote address 时不能直接替换 active path。
- preferred address 是显式允许的新远端 path。
- preferred-address policy 支持 `Ignore` 和 `ValidateAndMigrate`；默认使用
  `ValidateAndMigrate`。
- 握手确认后，使用 preferred CID sequence 1 在候选 path 上发送 `PATH_CHALLENGE`。
- 收到匹配 `PATH_RESPONSE` 后才切换 active path。
- validation timeout 或网络不可达时继续使用原 path，不关闭正常连接。

### 14.4 跨 endpoint 主动迁移

当前连接只有一个 endpoint index、一个 send entry 和一份 endpoint lease。跨本地 UDP
socket 迁移需要新增 `QuicEndpointAttachment`：

- 一条连接同时挂载多个 endpoint。
- local CID 在每个 endpoint 分别注册。
- 每个 endpoint 拥有独立 send entry 和 local path identity。
- detach 和 timer owner 需要重新定义。

这属于独立扩展。本方案不为尚未需要的跨 socket 迁移破坏现有单 endpoint 热路径。

## 15. TLS post-handshake、key discard 与握手状态

客户端握手阶段的关键顺序：

```text
create connection
  -> derive Initial keys from ODCID/current Initial DCID
  -> SSL_do_handshake creates ClientHello
  -> send Initial (+ optional 0-RTT)
  -> process Retry OR authenticated Server Initial
  -> install Handshake keys and validate server TP
  -> verify certificate/hostname/ALPN
  -> install 1-RTT keys
  -> state = Established; wake connect waiter
  -> receive HANDSHAKE_DONE
  -> handshake_confirmed = true; discard Handshake space
  -> process NewSessionTicket through post-handshake TLS
```

注意：

- TLS handshake complete 与 QUIC handshake confirmed 是两个不同节点。
- `connect()` 在 complete/1-RTT ready 返回，不必等待 `HANDSHAKE_DONE` 才允许应用发包。
- Initial/Handshake key discard 继续沿用现有 packet-number-space 生命周期规则。
- NewSessionTicket 发生在 post-handshake，不能在 `Established` 后停止处理 CRYPTO frame。

## 16. 文件级改动清单

| 文件 | 计划改动 |
|---|---|
| `include/fiber/quic/QuicClient.h + src/quic/QuicClient.cpp` | 新增 connector、attempt、connect options/error、cache/0-RTT policy |
| `include/fiber/quic/QuicUdpEndpoint.h + src/quic/QuicUdpEndpoint.cpp` | 配置拆分、outbound attach、client ingress、reset-token index、本地地址锁定 |
| `include/fiber/quic/QuicConnection.h + src/quic/QuicConnection.cpp` | client CID state、connect waiter、NEW_TOKEN、Retry/VN/0-RTT 协调 |
| `include/fiber/quic/QuicConnectionId.h + src/quic/QuicConnectionId.cpp` | preferred CID 和 endpoint reset-token index node |
| `include/fiber/quic/QuicTlsSession.h + src/quic/QuicTlsSession.cpp` | `init_client`、role-aware TP、peer verify、post-handshake/session callback |
| `include/fiber/quic/QuicPacketProcessor.h + src/quic/QuicPacketProcessor.cpp` | client unprotected control packets、Server Initial SCID 采纳、NEW_TOKEN |
| `src/quic/QuicPacketCodec.h + src/quic/QuicPacketCodec.cpp` | VN version list、Retry decode context、Initial token/0-RTT metadata |
| `src/quic/QuicCrypto.h + src/quic/QuicCrypto.cpp` | Retry integrity validation、只重派生 Initial keys |
| `src/quic/QuicLossRecovery.h + src/quic/QuicLossRecovery.cpp` | `reset_after_retry`、0-RTT rejection cleanup |
| `include/fiber/quic/QuicCongestion.h + src/quic/QuicCongestion.cpp` | Retry 后恢复到初始 congestion state |
| `include/fiber/quic/QuicPacer.h + src/quic/QuicPacer.cpp` | Retry 后按新 congestion/path 重置 pacing state |
| `src/quic/QuicTransportParamsCodec.h + src/quic/QuicTransportParamsCodec.cpp` | preferred address、角色约束、remembered TP snapshot |
| `include/fiber/quic/QuicPath.h`, `QuicPathManager.*` | role-aware amplification、initial path reconcile、preferred validation |
| `include/fiber/quic/QuicStream.h + src/quic/QuicStream.cpp` | early-data mode 和 rejection 所需 stream 标记 |
| `include/fiber/net/TlsCredential.h`, `TrustStore.h`, `TlsParams.h`, `src/net/detail/TlsSslFactory.*` | 独立证书/私钥与 trust store、每连接 peer verification、通用 client session trampoline |
| `include/fiber/http/Http3Server.h + src/http/Http3Server.cpp` | 适配 endpoint/server admission options 拆分，不增加 H3 client |
| `tests/*` | 单元、loopback、TLS、Retry、0-RTT、path 测试 |

构建系统当前递归收集 `src` 和 `tests` 下的对应源文件，新增 `QuicClient.cpp` 和测试文件
预计不需要手工修改 CMake source list；实施时必须通过重新 configure 验证。

## 17. 测试方案

### 17.1 Endpoint

修改 `tests/QuicUdpEndpointTest.cpp`：

- 不配置 server factory 时 endpoint 初始化成功。
- outbound connection attach/detach 完整。
- local CID 冲突重试和超限回滚。
- client-only endpoint 对 unknown Initial/short packet 静默丢弃。
- endpoint 同时承载 inbound server 和 outbound client。
- detach 后 CID、reset-token、send entry、timer 和 connection list 全部清空。
- wildcard local address 首次认证收包后正确锁定。

### 17.2 Codec 与 transport parameters

修改：

- `tests/QuicPacketCodecTest.cpp`
- `tests/QuicCryptoTest.cpp`
- `tests/QuicTransportParamsCodecTest.cpp`
- `tests/QuicTransportCodecTest.cpp`

覆盖：

- Retry integrity 官方/固定向量、tag 篡改、空 token、错误 CID。
- VN version list 正常、空列表、非 4-byte 对齐、CID mismatch、包含 v1。
- Initial token encode/decode。
- preferred address v4/v6、长度错误、零长 CID、错误 owner。
- server TP 无 Retry/有 Retry时的三类 CID 身份校验。
- remembered 0-RTT TP 的允许/禁止字段和降低限制检查。

### 17.3 Client connection/TLS

新增 `tests/QuicClientTest.cpp`，必要时新增 `tests/QuicTlsSessionTest.cpp`：

- start-connect 事务成功与各阶段失败回滚。
- Server Initial SCID 只采纳一次；后续不一致 SCID 被丢弃。
- 可信证书和正确 hostname。
- 错误 hostname、IP SAN、不可信 CA。
- 显式 insecure 测试配置。
- ALPN 选择成功、缺失和不匹配。
- connect ready 与 handshake confirmed 的状态区别。
- timeout、cancel、peer close、stateless reset 唤醒 waiter。
- post-handshake NewSessionTicket 进入 external cache。
- NEW_TOKEN 与 Retry token 的生命周期分离。

### 17.4 Retry 与 recovery

- Retry 后 ClientHello CRYPTO bytes/offset 不变。
- local Initial SCID 和 ODCID 不变。
- current Initial DCID、Retry SCID 和 token 正确更新。
- Initial keys 重新派生，Handshake/0-RTT keys 不被错误清除。
- packet number 不重置。
- 旧 bytes-in-flight、loss timer、PTO、congestion epoch、pacer 状态被清理。
- 第二个 Retry、认证包后的 Retry、错误 integrity tag 均被忽略。

### 17.5 Loopback integration

新增 `tests/QuicClientLoopbackTest.cpp`，在两个真实 `QuicUdpEndpoint` 之间使用测试 ALPN：

- 无 Retry 的完整握手。
- 强制 Retry 的完整握手。
- 双向流和单向流。
- stream/connection flow control blocking 与恢复。
- ACK、丢包、乱序、PTO 和重传。
- graceful close、transport close、application close、idle timeout。
- DCID 不命中但尾部 token 命中的 stateless reset。
- client/server connection 同 socket 共存。

测试只使用 QUIC transport application callback，不引入 HTTP/3 client。

### 17.6 Session、NEW_TOKEN 与 0-RTT

- 第一次连接保存 session ticket 和 remembered TP。
- 第二次连接成功 resumption。
- NEW_TOKEN 在同 server identity 上复用，不跨 identity 使用。
- 0-RTT accepted，ReplaySafe stream 正常发送。
- `OneRttOnly` stream 在握手期间不进入 0-RTT packet。
- 0-RTT rejected 后 early stream 全部 detach、ID/flow-control 恢复、callback 触发。
- rejection 后不自动重发应用 STREAM 数据。
- 服务端接受 0-RTT 但降低 remembered limit 时连接失败。

### 17.7 Path

- client 不受 server anti-amplification limit。
- wildcard local path reconcile 不产生假迁移。
- preferred CID sequence 1 和 reset token 正确安装。
- `PATH_CHALLENGE/PATH_RESPONSE` 成功后切换 preferred path。
- validation 超时继续原 path。
- 未授权的服务端源地址变化不替换 active path。

### 17.8 Nginx 互操作

仓库固定 Nginx 版本由 `scripts/build_nginx.sh` 管理，当前参考版本为 1.31.3：

- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic.c`
- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_ssl.c`
- `temp/nginx-install/sbin/nginx`

QUIC transport 阶段用 Nginx 验证：

- UDP endpoint 与服务端 Initial/Retry 行为。
- QUIC/TLS handshake 和 transport close。
- server transport parameters、ALPN 和证书互操作。

HTTP/3 client 落地后，`Http3ClientTest.NginxInterop` 又把互操作范围扩展到 H3 SETTINGS、
request stream、QPACK 响应头和响应体；具体命令和边界见 `feature/http3_client.md`。

## 18. 实施阶段

### Phase 1：Endpoint 角色中立化

- 拆分 `EndpointOptions` 与 `ServerAdmissionOptions`。
- 允许无 server factory 初始化。
- 增加 outbound attach/detach 事务。
- 适配 `Http3Server`。
- 服务端全部测试必须保持通过。

### Phase 2：安全的基础客户端握手

- 新增 `QuicClient`/`QuicClientAttempt`。
- client CID 初始状态和 endpoint 注册。
- `QuicTlsSession::init_client`。
- SNI、ALPN、证书/hostname verification。
- 无 Retry、无 0-RTT 的 Initial -> Handshake -> 1-RTT。
- connect waiter、timeout、cancel 和 error model。

### Phase 3：协议控制与连接身份

- Server Initial SCID 认证后采纳。
- 三类 transport-parameter CID 校验。
- Retry integrity、状态更新和 recovery reset。
- Version Negotiation。
- endpoint reset-token 反向索引。

### Phase 4：Session 与 token

- post-handshake CRYPTO。
- external TLS session cache。
- NEW_TOKEN cache/复用。
- resumption integration tests。

### Phase 5：0-RTT

- remembered transport parameters。
- EarlyData packet builder。
- ReplaySafe stream policy。
- accept/reject 和应用状态重置。

### Phase 6：Preferred address 与 path validation

- preferred-address codec/CID。
- role-aware anti-amplification。
- initial local address reconcile。
- preferred path challenge、切换与 fallback。

### Phase 7：完整验证

按以下顺序执行：

1. 新增和受影响的 focused tests。
2. 完整构建。
3. `ctest --test-dir build`。
4. 可用时执行 ASAN/UBSAN 和 packet codec fuzz/regression corpus。
5. Nginx QUIC/TLS handshake 互操作。
6. 完成全部代码后只运行一次 `./format_code.sh`。
7. 检查最终 diff，不提交 build/temp 产物。

## 19. 验收标准

完成本方案后，以下条件必须同时成立：

1. 一个 `QuicClient`、一个共享 `QuicUdpEndpoint` 和应用 connection factory 即可建立
   QUIC v1 客户端连接。
2. 不存在第二套 send-only UDP endpoint、recv pump、scheduler 或 CID table。
3. 客户端默认验证 peer certificate、hostname/IP 和 ALPN。
4. 无 Retry、一次合法 Retry、无效 Retry、VN 不支持版本均有确定行为。
5. ODCID、Retry SCID、Server Initial SCID 和当前发包 DCID 不互相覆盖。
6. 连接在 1-RTT ready 时可用，在 `HANDSHAKE_DONE` 后进入 confirmed。
7. 双向/单向 stream、flow control、ACK、loss recovery、congestion 和 pacing 复用现有实现。
8. stateless reset 在 DCID 不命中时仍能定位并关闭客户端连接。
9. TLS session、NEW_TOKEN 和 0-RTT 各自有正确的身份绑定和生命周期。
10. 0-RTT rejection 不会造成传输层静默重放应用数据。
11. preferred address 只在 path validation 成功后切换，失败不影响原 path。
12. endpoint detach 后没有 CID、token、waiter、timer、send entry 或 lease 泄漏。
13. 现有 QUIC/HTTP3 server 测试和行为不回退。

达到以上标准后，QUIC transport client 可以视为完整；后续 HTTP/3 client 作为独立应用层
建立在该接口上实现。

## 20. 实际验证（2026-07-20）

已执行：

1. `./build/fiber_tests --gtest_filter='Quic*' --gtest_brief=1`
   - 329 个 QUIC 测试全部通过。
2. `./format_code.sh`
   - 按仓库约定在实现完成后统一格式化一次。
3. `cmake --build build -j2`
   - `fiber_lib`、`fiber_tests`、examples、`lite_nginx` 和 Nacos targets 构建成功。
4. `ctest --test-dir build --output-on-failure`
   - CTest 汇总为 `100% tests passed out of 1408`。
   - 其中 1406 个测试通过，两个需要显式启用本地 rnacos 的互操作测试按配置跳过。

本次使用仓库固定的 Nginx 1.31.3 源码核对 endpoint、TLS callback 和 QUIC 服务端行为，
未把 live Nginx HTTP/3 互操作作为本阶段验收项：当前只实现 QUIC transport client，尚未实现
发送 H3 SETTINGS 所需的 HTTP/3 client 层。

后续 `feature/http3_client.md` 所述 HTTP/3 client 已完成，并已新增 live Nginx 1.31.3
SETTINGS/request/response 互操作测试；本节保留的是 QUIC transport 阶段当时的验收记录。
