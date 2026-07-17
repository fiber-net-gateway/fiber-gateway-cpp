# Nacos ConfigService 通信机制与开发计划

> 状态：ConfigService 公共操作、订阅、恢复与 rnacos 互操作已实现。
>
> 更新日期：2026-07-17。
>
> 行为参考：`/home/dear/CLionProjects/ploto-gateway/nacos-client/target/nacos-client-2.1-SNAPSHOT-sources.jar`
> 中的 Java `ConfigService`、`ConfigServiceImpl`、`GrpcConnection` 和 `GrpcRequester`。服务端互操作验证使用仓库内
> `temp/rnacos`，但 rnacos 只作为测试夹具，不作为客户端行为定义。

## 1. 背景与范围

`apps/nacos` 当前已经具备：

- Nacos HTTP 登录、token 刷新和多服务器故障转移。
- 通过 `async::Watch<NacosAuthSnapshot>` 发布认证状态。
- 绑定单个 `EventLoop` 的客户端生命周期和可等待关闭。
- 通用 unary/bidirectional gRPC 客户端、HTTP/2 和 protobuf lite runtime。
- Nacos gRPC `Payload` 信封编解码和完整 internal/config DTO codec。
- config 模块的 gRPC 连接、握手、心跳和重连。
- 查询、发布、CAS 发布和删除配置。
- 双向流上的服务端请求处理。
- 配置订阅池、缓存、MD5 去重、注销和断线恢复。

本文档记录 ConfigService 的通信机制、模块边界、状态机和分阶段实施结果。第一版对齐本地 Java 2.1
客户端的外部行为与 wire protocol，不包含 NamingService，也不实现本地磁盘快照、配置过滤器、加密配置或官方
Java SDK 的全量能力。

## 2. 设计原则

- 保持 Nacos wire compatibility，DTO 类型名、JSON 字段名和 protobuf 字段号必须与 Java 端一致。
- API 采用 C++ 协程和 `std::expected`，不复制 Java 的异常、阻塞调用和响应式类型体系。
- 所有 Nacos 状态机固定运行在创建客户端时传入的 `EventLoop`。
- ConfigService 与认证共享生命周期，但配置通信逻辑不继续堆入 `NacosClientImpl`。
- transient 连接失败不清空已知配置，也不终止现有订阅；连接恢复后重新注册。
- 对服务端输入设置消息大小、配置内容和错误文本上限。
- 同一个 gRPC 双向流只允许一个写协程，所有 ACK/响应通过串行发送路径输出。
- 不使用 C++ 异常；内部回调和状态转换保持 `noexcept`。

## 3. 总体架构

```text
NacosClient
├── Authentication coroutine
│   └── Watch<NacosAuthSnapshot>
└── ConfigService
    ├── Config subscription registry
    │   └── one entry per (dataId, group)
    └── Nacos config RPC connection
        ├── auth metadata provider
        ├── connection / handshake / heartbeat / reconnect
        ├── unary Request/request
        └── bidirectional BiRequestStream/requestBiStream
            └── protobuf Payload + JSON DTO
```

最终内部模块：

```text
apps/nacos/
├── include/fiber/nacos/
│   ├── ConfigService.h
│   └── NacosClient*.h
├── include/fiber/nacos/dto/
│   ├── internal request/response DTO
│   └── config request/response DTO
├── src/rpc/
│   ├── NacosPayloadCodec.*
│   ├── NacosRequester.*
│   └── NacosGrpcConnection.*
├── src/config/
│   └── ConfigServiceImpl.*
└── src/dto/
    └── corresponding JSON codecs
```

`NacosGrpcConnection` 保持为可供未来 NamingService 复用的基础设施；config 特有的 DTO、订阅表和推送处理留在
`ConfigServiceImpl`。

## 4. Wire Protocol

### 4.1 传输层

- 默认连接 `NacosClientConfig::grpc_port()`，当前默认端口为 `9848`。
- 使用 plaintext HTTP/2 prior knowledge（h2c），不是 HTTP/1.1 Upgrade。
- gRPC unary service/method：`Request/request`，HTTP/2 path 为 `/Request/request`。
- gRPC 双向流 service/method：`BiRequestStream/requestBiStream`，HTTP/2 path 为 `/BiRequestStream/requestBiStream`。
- unary 请求在同一 HTTP/2 连接上并发复用不同 stream。
- 双向流长期存活，承担服务端主动请求、连接探测和配置变更通知。

### 4.2 Protobuf Payload 信封

Nacos gRPC 只使用 protobuf 包装信封；具体请求和响应对象仍编码为 JSON。

```proto
message Metadata {
  string type = 3;
  map<string, string> headers = 7;
  string clientIp = 8;
}

message Payload {
  Metadata metadata = 2;
  google.protobuf.Any body = 3;
}
```

当前 C++ 实现为保持 protobuf lite-only 链接，使用了与 `google.protobuf.Any` 字段号完全相同的本地
wire-equivalent `Any`（`type_url = 1`、`value = 2`）。它不会改变线上字节格式，也避免 Nacos 库额外引入 full
protobuf runtime。

兼容性要求：

- 字段号不可改变。
- `metadata.type` 是 DTO 类型名，例如 `ConfigQueryRequest`。
- `body.value` 保存 DTO JSON 字节。
- 与 Java 当前实现一致，`Any.type_url` 保持为空。
- proto 添加 `option optimize_for = LITE_RUNTIME;`，通过仓库的 `fiber_proto_library` 生成 C++ lite 代码。
- 解码时先校验 protobuf 和消息大小，再根据 `metadata.type` 选择 JSON codec。
- unary 响应类型必须与请求期待类型完全匹配；`ErrorResponse` 单独映射为 Nacos 业务错误。

### 4.3 Metadata headers

每个 Payload 都包含以下动态 metadata：

| 字段 | 来源 | 说明 |
|---|---|---|
| `Metadata.clientIp` | 当前 gRPC socket 的本地地址或配置覆盖值 | 与 Java `NetUtil.getLocalIp()` 对应 |
| `headers.clientIp` | 同上 | Java 客户端同时放入 headers |
| `headers.clientVersion` | `NacosClientConfig::client_version()` | 客户端版本 |
| `headers.namespace` | `NacosClientConfig::namespace_id()` | Nacos namespace |
| `headers.accessToken` | 最新 Ready 认证快照 | 每次构造 Payload 时读取 |

`namespace_id` 和 `tenant` 不可混用：

- namespace 放在 metadata headers。
- tenant 放在 config 请求 DTO 中。

token 刷新后不需要重建 gRPC 连接；后续 Payload 自动携带新 token。若 token 已过期且认证状态变为 Unavailable，配置连接停止接受新请求、关闭现有 gRPC 连接并等待认证恢复。

## 5. 连接建立与维护

### 5.1 连接状态机

```text
WaitingAuth
    │ auth Ready
    ▼
Connecting ──failure──> Backoff
    │ connected            │ timer / next server
    ▼                      └──────────────┐
Checking                                  │
    │ ServerCheck success                 │
    ▼                                      │
Handshaking ──failure──────────────────────┘
    │ SetupAck or compatibility delay
    ▼
Ready ──stream end / transport failure──> Backoff
    │ shutdown
    ▼
Stopping ──all tasks exited──> Stopped
```

所有状态转换只在客户端 EventLoop 上发生。

### 5.2 建连流程

1. 订阅认证 Watch，等待非过期的 Ready token。
2. 从 preferred server 开始遍历 `server_ips`，连接对应 gRPC 端口。
3. 构造 `ServerCheckRequest`，通过 unary `Request/request` 发送。
4. 校验 gRPC status、响应 Payload 类型和 `ServerCheckResponse` JSON。
5. 打开 `BiRequestStream/requestBiStream` 双向流。
6. 发送 `ConnectionSetupRequest`：
   - `tenant`
   - `clientVersion`
   - labels：`source=sdk`、`module=config`
   - 空 `abilityTable`
7. 如果 ServerCheck 表示支持能力协商，等待服务端 `SetupAckRequest`。
8. 如果不支持能力协商，按 Java 兼容行为等待短暂延迟后进入 Ready；默认兼容延迟为 1 秒，并受总握手超时约束。
9. 发布新的 connection generation，恢复全部配置订阅并启动 heartbeat。

### 5.3 Heartbeat

- Ready 后每 10 秒发送一次 unary `HealthCheckRequest`。
- 同一连接最多保留一个进行中的 heartbeat。
- 单次 heartbeat 失败记录状态和指标，但主要由双向流/HTTP2 receive loop 判断连接终止。
- 连续失败策略应可配置；第一版不因一次 heartbeat 失败立即断线。

### 5.4 重连与服务器故障转移

- 成功连接的 server 记录为 preferred server。
- 连接失败后尝试其余配置 server；整轮失败后进入 Backoff。
- Backoff 有最小值、最大值和 jitter，避免大量客户端同步重连。
- 初次建连失败也持续重试，不复制 Java 当前初始连接失败后可能停止恢复的缺陷。
- transient 断线期间保留订阅表和最近配置值。
- 每次重新进入 Ready 都执行全量订阅恢复。

### 5.5 ConnectResetRequest

服务端可能通过双向流发送 `ConnectResetRequest`：

1. 校验可选 `serverIp` 和 `serverPort`。
2. 返回携带原 `requestId` 的 `ConnectResetResponse`。
3. 保存一次性重定向目标。
4. 关闭当前连接。
5. 下一次连接优先使用重定向目标；无效目标回退到静态 server 列表。

### 5.6 关闭

- `NacosClient::shutdown()` 发布统一 shutdown 信号。
- ConfigService 停止接收新操作和新订阅。
- 取消 heartbeat、补偿重订阅和 reconnect timer。
- 取消所有进行中的 unary 调用。
- 取消双向流，唤醒阻塞 read/write。
- 关闭 gRPC/HTTP2 连接并等待 receive loop 退出。
- 发布订阅 Stopped 状态。
- ConfigService 任务退出后再从客户端 `WaitGroup` 返回。

通用 `GrpcClient` 由调用者显式运行 `run()` 协程，`shutdown()` 会等待该协程和 HTTP/2 内部任务完全退出。
ConfigService 已将连接监视、注册和查询协程纳入 `WaitGroup`，Nacos shutdown 不依赖轮询或后台遗留任务。

## 6. Unary 配置操作

### 6.1 通用请求流程

```text
ConfigService operation
    -> build DTO
    -> encode JSON
    -> wrap Payload(metadata + Any.value)
    -> gRPC Request/request
    -> check transport result
    -> check grpc-status
    -> parse Payload
    -> check metadata.type
    -> parse JSON response
    -> map Nacos resultCode/errorCode
```

所有 unary 操作使用独立 request deadline。错误分为：

- 参数或生命周期错误。
- 认证不可用。
- TCP/HTTP2/gRPC transport 错误。
- protobuf/JSON/响应类型协议错误。
- Nacos `ErrorResponse` 或非成功业务响应。

业务错误应保留 Nacos `resultCode`、`errorCode` 和有界 message，CAS 冲突不能丢失服务端错误码。

### 6.2 getConfig

- key 为 `(dataId, group)`，tenant 来自客户端配置。
- 如果已有订阅 Entry 且已获得 Present/NotFound 状态，直接返回缓存。
- 否则发送 `ConfigQueryRequest`。
- 成功响应读取 `md5` 和 `content`。
- `ConfigQueryResponse::CONFIG_NOT_FOUND`，即 error code `300`，映射为“配置不存在”，不是 transport error。
- 其他非成功响应映射为 Nacos 业务错误。

### 6.3 publish

发送 `ConfigPublishRequest`：

- `dataId`
- `group`
- `tenant`
- `content`
- 可选 `casMd5`
- `additionMap.type`

ConfigType wire value：

| C++ 类型 | wire value |
|---|---|
| Json | `json` |
| Text | `text` |
| Yaml | `yaml` |
| Properties | `properties` |
| Xml | `xml` |
| Html | `html` |

空 CAS MD5 不发送 `casMd5`。发布成功只表示服务端接受请求，不乐观修改订阅缓存；缓存仍由服务端变更通知后的查询结果更新。

### 6.4 removeConfig

- 发送 `ConfigRemoveRequest`，第一版不暴露 tag 参数。
- 成功后不直接删除本地缓存。
- 等待服务端变更通知，随后查询得到 NotFound 并发布删除状态。

## 7. 双向流与服务端请求

### 7.1 读取和分发

双向流保持一个长期 read coroutine。每个 Payload 按 `metadata.type` 分发：

| 服务端类型 | 客户端行为 |
|---|---|
| `SetupAckRequest` | 完成能力协商，连接进入 Ready |
| `ClientDetectionRequest` | 返回同 requestId 的 `ClientDetectionResponse` |
| `ConnectResetRequest` | 返回 `ConnectResetResponse` 并触发重连 |
| `ConfigChangeNotifyRequest` | 返回 `ConfigChangeNotifyResponse`，异步查询并同步配置 |

未知类型不得造成进程异常或破坏订阅表。无法识别的 Payload 记录协议错误；对于可识别但暂不支持、且带 requestId 的 Request，可返回 `ErrorResponse`，避免服务端一直等待。

### 7.2 单写者约束

`GrpcStream` 允许一个 read 和一个 write 并行，但不允许多个 write 并发。服务端请求可能连续到达，因此：

- read coroutine 只负责解码、状态转换和投递响应。
- 所有 ACK/Response 进入连接级串行发送队列。
- write coroutine 是双向流唯一写者。
- 队列必须有长度和字节数上限；超限视为连接失去服务能力，关闭并重连。

## 8. 配置订阅机制

### 8.1 本地订阅模型

每个 `(dataId, group)` 只有一个内部 Entry：

```text
ConfigEntry
├── key: dataId + group
├── local subscriber count
├── current state: Pending / Present / NotFound / Stopped
├── md5
├── content
├── Watch<ConfigSnapshot>
├── query_in_flight
├── dirty
└── registered connection generation
```

公共订阅返回 move-only `ConfigSubscription`，内部持有 Entry lease，并转发 Watch 的 `current()`/`next(version)`。订阅句柄的创建、释放和显式关闭均在客户端 EventLoop 上完成。

不直接使用 Java 的 `ConfigData.EMPTY` 哨兵：

- `Present` 表示配置存在，即使 content 是空字符串。
- `NotFound` 表示服务端确认配置不存在。
- `Pending` 表示尚未获得初值。

### 8.2 首订阅与共享

1. 第一个本地订阅者创建 Entry。
2. 如果连接 Ready，发送 `ConfigBatchListenRequest(listen=true)`。
3. 没有缓存 MD5 时发送 JSON null；已有 Present 值时发送当前 MD5；NotFound 使用空 MD5 语义。
4. 同 key 的后续本地订阅者只增加引用计数，不重复注册服务端订阅。
5. 如果 Entry 已有 Present/NotFound 状态，新订阅者立即收到当前快照。

### 8.3 ConfigBatchListenResponse

服务端对订阅请求返回 `ConfigChangeBatchListenResponse.changedConfigs`。返回列表中的每个 key 表示客户端 MD5 与服务端不一致：

1. 查找本地 Entry。
2. 如果 Entry 仍存在，发送 `ConfigQueryRequest`。
3. 成功时比较 MD5；MD5 未变化不发布新版本。
4. error code 300 时发布 NotFound。
5. 其他错误保留旧值，等待后续推送、补偿订阅或重连恢复。

### 8.4 ConfigChangeNotifyRequest

服务端推送只包含 dataId/group/tenant，不包含完整配置：

1. 立即构造并排队发送同 requestId 的 `ConfigChangeNotifyResponse`。
2. tenant 与当前服务不匹配时忽略配置更新，但仍正确 ACK。
3. 查找本地 Entry；没有本地订阅时不查询。
4. 对存在的 Entry 调度 query-and-sync。

### 8.5 查询合并与乱序保护

同一 key 只能有一个 query-and-sync 在执行：

- 第一次通知设置 `query_in_flight=true` 并发起查询。
- 查询期间再次收到通知，只设置 `dirty=true`。
- 当前查询完成后，如果 dirty，则清除 dirty 并再查询一次。
- Entry 移除或 connection generation 改变后，旧查询结果不得覆盖新连接获得的结果。
- 可通过 Entry generation/query sequence 检查并丢弃过期完成结果。

这避免通知风暴产生并发请求，也防止旧响应晚到后覆盖新配置。

### 8.6 末订阅注销

最后一个本地订阅者释放时：

1. Entry 标记 Closing，不再接受查询结果发布。
2. 如果当前连接 Ready 且该 generation 已注册，发送 `ConfigBatchListenRequest(listen=false)`。
3. 注销请求是 best effort；无连接时直接移除，因为连接恢复只会重建仍存在的 Entry。
4. 从 registry 移除 Entry。

### 8.7 重连恢复与补偿订阅

- 每次进入新的 Ready generation，扫描全部活动 Entry。
- 按最大 context 数和最大 Payload 字节数分批发送 `listen=true`。
- 每个 context 携带当前 MD5；服务端只返回发生变化的配置。
- Ready 期间每 180 秒执行同样的全量补偿注册。
- transient 失败不修改当前配置值；下一周期或下一 connection generation 重试。

## 9. 公共 API 语义

核心 API 只提供协程和 `std::expected`：

- `get_config` 返回 `expected<optional<ConfigData>, ConfigServiceError>`。
- `publish` 返回 `expected<void, ConfigServiceError>`。
- `remove_config` 返回 `expected<void, ConfigServiceError>`。
- `subscribe` 返回 move-only `ConfigSubscription` 或创建错误。

Java 的 `blockingGetConfig`、`syncPublish` 和 `syncRemoveConfig` 不进入核心库：

- 项目不使用 C++ 异常。
- EventLoop 线程上阻塞会导致死锁。
- 如果业务需要同步入口，应在应用边界提供明确的跨线程适配器。

ConfigService 与 NacosClient 同生命周期：客户端创建时建立不可空的 ConfigService 实例，`config_service()` 返回引用；网络任务只在 `start()` 后运行。

## 10. 配置项

建议在保持现有认证选项兼容的前提下增加：

| 选项 | 建议默认值 | 用途 |
|---|---:|---|
| gRPC connect timeout | 3 s | TCP/h2 建连上限 |
| unary request timeout | 3 s | 普通 RPC deadline |
| handshake timeout | 5 s | ServerCheck + stream setup 总上限 |
| compatibility setup delay | 1 s | 不支持能力协商时的 Java 兼容行为 |
| heartbeat interval | 10 s | HealthCheck 周期 |
| reconnect initial delay | 1 s | 首轮失败后的退避 |
| reconnect max delay | 60 s | 退避上限 |
| subscription redo interval | 180 s | 补偿订阅周期 |
| max inbound gRPC message | 10 MiB | 对齐 Java channel 上限 |
| max config content | 10 MiB 或更小的显式值 | 防止无界配置分配 |
| max listen contexts per request | 待 rnacos/Java 互操作验证 | 订阅分批 |
| max push response queue | 有界 | 防止服务端推送压垮内存 |
| client IP override | 空 | 容器/多网卡环境覆盖自动探测 |

测试中所有时间相关选项必须可缩短，避免依赖真实 10 秒/180 秒定时。

## 11. DTO 与 Codec 范围

第一版至少完成以下类型。

### 11.1 请求或客户端响应

- `ServerCheckRequest`
- `ConnectionSetupRequest`
- `HealthCheckRequest`
- `ConfigQueryRequest`
- `ConfigPublishRequest`
- `ConfigRemoveRequest`
- `ConfigBatchListenRequest`
- `ClientDetectionResponse`
- `ConnectResetResponse`
- `ConfigChangeNotifyResponse`
- `ErrorResponse`

### 11.2 响应或服务端请求

- `ServerCheckResponse`
- `SetupAckRequest`
- `HealthCheckResponse`
- `ConfigQueryResponse`
- `ConfigPublishResponse`
- `ConfigRemoveResponse`
- `ConfigChangeBatchListenResponse`
- `ConfigChangeNotifyRequest`
- `ClientDetectionRequest`
- `ConnectResetRequest`
- `ErrorResponse`

Codec 约束：

- JSON 字段名、null/absent 行为和默认值以 Java Jackson 输出为准。
- parser 使用 pool-backed `string_view`，需要跨解析生命周期保存的值再显式复制到拥有型对象。
- 未知 JSON 字段允许跳过，以兼容服务器扩展。
- 已知字段类型错误、整数越界和错误顶层类型必须拒绝。
- parse 失败保持输出对象事务性不变。
- 编码结果必须设置大小上限，不能先生成无界 `std::string` 再检查。

## 12. 开发计划

### 阶段 0：稳定当前认证基线

- [x] 完成当前未提交认证重构的评审和测试。
- [x] 确认认证 Watch 的 Ready/Unavailable/Stopped 语义稳定。
- [x] 确认 token 刷新、过期、server failover 和 shutdown 可供 config 复用。
- [x] 冻结 ConfigService 所依赖的 `NacosClientConfig`/`NacosClientOptions` 基线。

验收：现有 Nacos 定向测试和完整 CTest 通过，工作区中的认证行为与 README 一致。

### 阶段 1：Payload 和 DTO wire compatibility

- [x] 添加 Nacos Payload proto 和 protobuf lite CMake target。
- [x] 实现 Payload metadata/body 构造与解析。
- [x] 补齐 internal/config DTO 和 JSON codec。
- [x] 从 Java 2.1 生成或固定 golden JSON/serialized Payload fixtures。
- [x] 定义统一的 protocol/server/transport error 模型。

验收：所有 DTO 与 Java fixture 双向兼容；类型不匹配、ErrorResponse、非法 protobuf/JSON 和超限输入均有测试。

### 阶段 2：通用 Nacos gRPC 连接

- [x] 实现 `NacosRequester` unary 调用封装。
- [x] 实现 ServerCheck 和双向流 ConnectionSetup。
- [x] 实现 SetupAck/兼容延迟和握手超时。
- [x] 实现单写者 push response queue。
- [x] 实现 ClientDetection、ConnectReset 和 unknown request 策略。
- [x] 实现 heartbeat、server failover、backoff 和 jitter。
- [x] 监听 Auth Watch，动态更新 token metadata。
- [x] 为通用 GrpcClient 增加外部驱动的 `run()` 和可等待的 `shutdown()`。
- [x] 暴露连接本地地址并支持显式 client IP 覆盖。
- [x] 接入 NacosClient `WaitGroup` 和 shutdown。

验收：脚本化 gRPC 测试服务器可验证完整握手、双向请求/响应、token 更新、重连和干净关闭。

### 阶段 3：ConfigService unary 操作

- [x] 定义 ConfigService 公共类型和协程 API。
- [x] 实现 getConfig。
- [x] 实现 publish 和 ConfigType 映射。
- [x] 实现 CAS MD5 publish。
- [x] 实现 removeConfig。
- [x] 实现参数校验、NotFound 和 Nacos 业务错误映射。

验收：查询成功/不存在、普通发布、CAS 成功/失败和删除均通过脚本化服务器测试与 rnacos 互操作测试。

### 阶段 4：配置订阅池

- [x] 实现 `(dataId, group)` registry 和 move-only subscription lease。
- [x] 实现首订阅注册、同 key 共享和最新值重放。
- [x] 实现 ConfigBatchListenRequest 分批。
- [x] 实现 changedConfigs 查询同步。
- [x] 实现 ConfigChangeNotifyRequest ACK 和 query-and-sync。
- [x] 实现 MD5 去重、NotFound 发布和删除语义。
- [x] 实现 query in-flight 合并、dirty 重查和过期结果保护。
- [x] 实现末订阅 best-effort 注销。

验收：多个本地订阅者只产生一次服务端订阅；更新只发布一次；删除发布 NotFound；最后订阅释放触发注销。

### 阶段 5：恢复、限流与生命周期加固

- [x] 实现 connection generation 和断线后全量恢复。
- [x] 实现 180 秒补偿订阅。
- [x] 对订阅 batch、配置内容、Payload、响应队列设置硬上限。
- [x] 覆盖 token 过期、认证恢复、连接重置和多 server 故障转移。
- [x] 覆盖 shutdown 与进行中的 query/publish/push read/write 竞态。
- [x] 增加必要的连接状态、错误码和 generation 可观测信息。

验收：重启服务端或断开 TCP 后，旧值保持可读，客户端自动恢复连接和订阅，随后能收到新配置；shutdown 后无任务、timer、stream 或 Entry 泄漏。

### 阶段 6：rnacos 互操作与文档

- [x] 使用隔离端口和临时数据目录启动 `temp/rnacos`，验证 RPC 握手互通。
- [x] 验证 publish -> get。
- [x] 验证 CAS 成功；脚本化服务器验证冲突错误保真。
- [x] 验证 subscribe -> publish -> change push。
- [x] 验证 remove -> NotFound。
- [x] 以脚本化连接重启验证自动重连和订阅恢复。
- [x] 确认 rnacos 0.8.2 接受错误 `casMd5` 是测试夹具差异，并记录在 README。
- [x] 更新 `apps/nacos/README.md`，记录最终 API、生命周期和已知差异。

验收：所有本地测试通过，rnacos 互操作结果可重复且测试进程能清理服务端。

## 13. 测试计划

### 13.1 DTO/codec 单元测试

- Java golden JSON 编解码。
- null、absent 和默认值差异。
- unknown field 前向兼容。
- 错误字段类型、数值越界和事务性 parse。
- Payload type/header/body wire fixture。
- 大小上限和截断输入。

### 13.2 RPC 连接测试

- ServerCheck 成功和非成功响应。
- 支持/不支持 ability negotiation 的两条握手路径。
- SetupAck、ClientDetection、ConnectReset。
- unary 多路复用和双向流同时运行。
- push response 单写者顺序。
- heartbeat 不重叠。
- 初次连接失败、运行中断线和多 server failover。
- token generation 更新后新请求携带新 accessToken。
- shutdown 取消阻塞 connect/read/write 并等待全部任务退出。

### 13.3 ConfigService 测试

- get 成功、NotFound、业务错误和 timeout。
- 六种 ConfigType wire value。
- CAS MD5 presence/absence 和冲突错误保留。
- remove 成功和失败。
- 首订阅、共享订阅、缓存重放和末订阅注销。
- changedConfigs 和主动 ConfigChangeNotify 两种更新入口。
- MD5 相同不重复发布。
- 删除后发布 NotFound，空字符串配置仍为 Present。
- query 合并、dirty 重查和旧 generation 响应丢弃。
- reconnect/periodic redo 后批量恢复全部订阅。

### 13.4 验证命令

实现期间使用：

```bash
cmake -S . -B build
cmake --build build --target fiber_nacos_tests
ctest --test-dir build -R '^(NacosClientTest|NacosClientConfigTest|NacosDtoJsonTest|NacosPayloadTest|NacosGrpcConnectionTest|NacosConfigServiceTest)\.' --output-on-failure
```

完成后执行：

```bash
./format_code.sh
cmake --build build --target fiber_nacos_tests
ctest --test-dir build --output-on-failure
git diff --check
```

## 14. 完成标准

ConfigService 可以视为完成，必须同时满足：

- get、publish、CAS publish、remove 和 subscribe 的行为与 Java 参考一致。
- protobuf Payload 和 JSON DTO 与 Nacos/Java wire compatible。
- 认证刷新不会中断正常配置通信，token 过期后不会继续发送过期凭据。
- 双向流能处理配置变更、连接探测和连接重置。
- 多订阅者共享、MD5 去重、删除、注销、重连恢复和补偿订阅均有回归测试。
- 所有网络、解析、队列和内容大小均有明确上限。
- shutdown 返回时所有 config coroutine、timer、gRPC stream 和底层连接均已退出。
- Nacos 定向测试、完整 CTest、格式化检查和 `git diff --check` 全部通过。
- `apps/nacos/README.md` 与最终 API 和生命周期一致。

## 15. 明确的 Java 差异

以下差异是有意的，实施后应继续保留在公开文档中：

- 不提供抛异常的阻塞 API，只提供协程和 `std::expected`。
- 不使用 `ConfigData.EMPTY` 哨兵，以显式 Present/NotFound 状态表达删除。
- 初次 gRPC 连接失败会持续重试，而不是让 ConfigService 永久失败。
- 订阅 batch、配置内容、Payload 和 push response queue 都有硬上限。
- unknown server request 不会使进程异常；按协议错误策略响应或丢弃。
- ConfigService 和所有内部状态固定在一个 EventLoop 上，不复制 Java 的多线程/ConcurrentHashMap 模型。
