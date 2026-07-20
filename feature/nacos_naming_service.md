# Nacos NamingService 实现说明

## 1. 语义基线与范围

NamingService 的行为基线是仓库外的
`/home/dear/CLionProjects/ploto-gateway/nacos-client` Java 实现。官方 Nacos
Java 客户端不作为本模块的设计依据。第一版实现下列能力：

- 按 `(serviceName, group)` 查询服务实例。
- 共享服务订阅，并接收 `NotifySubscriberRequest` 推送。
- 注册、更新和注销单个实例。
- 连接断开后恢复全部活动订阅和实例注册。
- 在固定 `EventLoop` 上完成启动、操作和关闭。

不提供阻塞查询，也不实现批量实例、持久实例或本地磁盘缓存。

## 2. 公共 API

`NacosClient::naming_service()` 返回与客户端同生命周期的 `NamingService`：

```cpp
auto &naming = client->naming_service();

auto queried = co_await naming.get("gateway", "DEFAULT_GROUP");
auto subscribed = naming.subscribe("gateway", "DEFAULT_GROUP");

fiber::nacos::Instance instance{
        .ip = "127.0.0.1",
        .port = 8080,
};
auto registered = naming.registry("gateway", "DEFAULT_GROUP", std::move(instance));
```

所有创建、更新、关闭操作必须在客户端所属 `EventLoop` 上执行。订阅内部的
`Watch::next(version)` 可以由其他 loop 等待，但 `Subscription::close()`、
`InstanceRegistration::update()` 和 `InstanceRegistration::close()` 仍属于 owner loop。

错误通过 `std::expected` 返回，区分参数、关闭、认证、transport、gRPC、协议、
Nacos 业务错误和响应超限。服务端 `resultCode`、`errorCode` 与有界 message 会保留。

## 3. 连接与生命周期

NamingService 拥有独立的 `NacosRpc` 连接，ConnectionSetup labels 固定为
`source=sdk,module=naming`。它复用 ConfigService 已验证的单连接 transport，
但不与 ConfigService 共享连接或业务状态。

```text
NacosClient::start()
    -> authentication coroutine
    -> ConfigService connection loop
    -> NamingService connection loop

NamingService ready
    -> restore live subscriptions
    -> replay live instance registrations
```

一次物理连接终止后，NamingService 等待该连接上的 unary 任务全部退出，清理连接态，
销毁旧 `NacosRpc`，再执行 server failover、退避和新建连接。订阅缓存和注册期望值属于
服务级状态，不随物理连接销毁。

关闭时先拒绝新工作并发布订阅 `Closed`，再停止当前 RPC、唤醒退避、等待所有任务退出。
仍存在的注册句柄收到 `RegistrationState::Closed`；客户端析构前必须完成
`NacosClient::shutdown()`。

## 4. Wire DTO

Naming DTO 与 ploto Java Jackson 字段一致：

| 方向 | Payload 类型 | 关键字段 |
|---|---|---|
| client -> server | `ServiceQueryRequest` | namespace, serviceName, groupName, cluster, healthyOnly, udpPort |
| client -> server | `SubscribeServiceRequest` | namespace, serviceName, groupName, subscribe, clusters |
| client -> server | `InstanceRequest` | namespace, serviceName, groupName, type, instance |
| server -> client | `NotifySubscriberRequest` | namespace, serviceName, groupName, serviceInfo |
| server -> client | `QueryServiceResponse` | serviceInfo |
| server -> client | `SubscribeServiceResponse` | serviceInfo |
| server -> client | `InstanceResponse` | type |

请求 module 固定为 `naming`。解析阶段使用 pool-backed `string_view`；进入公共 API 的
`ServiceInfo` 和 `Instance` 会转换为拥有型数据，并在复制前执行 host 数量、metadata
数量及 key/value 大小限制。

## 5. 查询与订阅

`get(service, group)` 与 ploto Java 行为一致：

1. 如果该 key 已有活动订阅且收到过成功推送，返回订阅缓存。
2. 否则发送 `ServiceQueryRequest`。
3. 普通查询结果不写入订阅缓存。

每个 `(serviceName, group)` 只有一个内部订阅 Entry。同 key 的多个本地订阅者共享一次
wire 订阅：首个引用发送 `subscribe=true`，最后一个引用释放时 best-effort 发送
`subscribe=false`。

`SubscribeServiceResponse.serviceInfo` 不发布给订阅者。ploto Java 明确依赖服务端随后立即
推送 `NotifySubscriberRequest`，因此只有该推送可以建立或更新订阅缓存。推送按
`lastRefTime` 精确去重；同版本不重复发布，不比较整个 hosts 列表。

每次新连接 Ready 后都会重新发送所有仍活动的订阅。断线期间保留最近一次成功值；关闭时
发布 `SubscriptionResult<ServiceInfo>{kind=Closed}` 唤醒等待者。

## 6. 实例注册

`registry()` 返回 move-only `InstanceRegistration`。句柄持有注册条目的生命期，并提供：

- `update(instance)`：提交新的期望实例。
- `subscribe_status()`：订阅 `Pending`、`Registered`、`Failed`、`Closed` 状态。
- `close()`：结束注册并 best-effort 发送 `deregisterInstance`。

注册采用 ploto Java 的 latest-value 语义：同一条目最多有一个 wire 操作进行中；操作期间的
多次更新只保留最新实例，当前请求完成后立即发送最新版本。连接 Ready 时注册最后值；重连后
再次注册最后值。关闭发生在注册进行中时，先等待当前请求完成，再注销最后一次提交的实例。

`registerInstance` 失败发布 `Failed` 并保留条目，后续 `update()` 或重连可以再次尝试。
注销为终止型 best effort 操作：无论响应成功、业务失败或连接终止，句柄最终都进入
`Closed`，不会在后台无限重试已经释放的注册。

## 7. 限制项

`NacosClientOptions` 为 NamingService 提供以下硬限制：

- `max_naming_service_name_bytes`
- `max_naming_group_bytes`
- `max_naming_hosts_per_service`
- `max_naming_metadata_entries`
- `max_naming_metadata_key_bytes`
- `max_naming_metadata_value_bytes`

所有值必须大于零，否则 `NacosClient::create()` 返回 `InvalidOptions`。

## 8. 验证

脚本化 HTTP/2/gRPC 测试覆盖：

- ploto Java wire JSON golden 与嵌套 ServiceInfo round-trip。
- 查询缓存命中与未订阅查询。
- 同 key 共享订阅、首订阅推送、末订阅注销。
- 注册状态、连续更新合并和显式注销。
- 断链后建立新 naming 连接、恢复全部订阅和重新注册实例。
- shutdown 发布 `Closed` 并等待任务退出。

`NacosNamingServiceTest.RnacosInteropWhenEnabled` 使用仓库的 rnacos 夹具验证真实注册、查询、
订阅推送、实例端口更新、再次推送和注销。rnacos 只用于互操作验证；行为冲突仍以 ploto Java
实现为准。
