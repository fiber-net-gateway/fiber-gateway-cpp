# AI Server

`ai-server` 是基于本仓库 C++23 fiber runtime 的完整 LLM 代理。业务语义以 Java
`ploto-llm` 及
[`docs/java-ploto-llm-business-flow-and-config.md`](../../docs/java-ploto-llm-business-flow-and-config.md)
为基线，并适配本项目的 EventLoop、HTTP client、Nacos、CAT 和 Prometheus
所有权模型。

当前实现包括：

- OpenAI Chat Completions 和 Anthropic Messages 入口；
- BT1 认证、模型授权、route key 和确定性 Provider/token 选择；
- Nacos 动态配置、`service://` 实例发现和不可变快照发布；
- HTTP/HTTPS Provider 调用、连接池、DNS、TLS 主机校验、重试、fallback、token
  暂停和 Provider 熔断；
- 同步响应及 SSE 流式透传；
- 单次遍历 JSONPath 抽取、任意已存在字段的原始区间改写，以及 ai-server
  `model`/`stream` 重试快路径；
- 集群 token 限流、内部 check/settle 协议和 Nacos 成员环；
- 固定 schema Prometheus 指标、无正文审计日志和可选 CAT transaction；
- 延迟绑定、请求排空和跨 EventLoop 有序关闭。

完整设计见 [`docs/architecture.md`](docs/architecture.md)，JSON 字段能力见
[`docs/json-field-transform.md`](docs/json-field-transform.md)。

## 路由

| 路由 | 用途 | 认证 |
| --- | --- | --- |
| `POST /v1/chat/completions` | OpenAI Chat Completions | BT1 |
| `POST /v1/messages` | Anthropic Messages | BT1 |
| `POST /v1/message` | Anthropic 兼容别名 | BT1 |
| `POST /internal/llm/rate-limit/check` | owner 节点限流检查 | 无，必须网络隔离 |
| `POST /internal/llm/rate-limit/settle` | owner 节点用量结算 | 无，必须网络隔离 |
| `GET /health` | 进程存活探针 | 无 |
| `GET /ready` | 配置和限流成员就绪探针 | 无 |
| `GET /metrics` | Prometheus 文本指标 | 无 |
| `GET /_metric_prometheus` | 指标兼容别名 | 无 |

`/ready` 只有在 worker 已安装完整配置快照且限流成员环非空时返回 `200`；否则返回
`503`。内部限流接口和指标接口没有应用层认证，生产部署必须通过监听地址、防火墙、
sidecar 或服务网格限制访问。

## 构建与运行

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_ai_server
cp apps/ai-server/ai-server.env.example ai-server.env
./build/apps/ai-server
```

也可以把 dotenv 文件作为唯一位置参数传入：

```bash
./build/apps/ai-server /etc/ai-server/production.env
```

未传参数时读取当前目录的 `ai-server.env`。配置解析支持空行、注释、可选
`export`、单/双引号和行尾注释；重复键、未知键和非法值都会让启动失败。

## 启动配置

HTTP 和集群成员：

- `AI_SERVER_LISTEN_ADDRESS`：默认 `0.0.0.0`；
- `AI_SERVER_LISTEN_PORT`：默认 `8080`，`0` 可用于本地测试；
- `AI_SERVER_ADVERTISE_ADDRESS`：可选的 Nacos 注册 IPv4；未配置时选择第一个
  非 loopback IPv4，找不到时使用 `127.0.0.1`；
- `AI_SERVER_SERVICE_NAME`：默认 `ploto-ai-server`；
- `AI_SERVER_SERVICE_GROUP`：默认 `DEFAULT_GROUP`；
- `AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS`：默认 `60000`，`0` 表示无限等待。

Nacos：

- `NACOS_SERVER_ADDRESSES`：必填，逗号分隔的 IPv4/IPv6 literal；
- `NACOS_HTTP_PORT`：默认 `8848`；
- `NACOS_GRPC_PORT`：默认 `9848`；
- `NACOS_NAMESPACE_ID`、`NACOS_TENANT`；
- `NACOS_USERNAME`、`NACOS_PASSWORD`：必须同时为空或同时配置；
- `NACOS_CONTEXT_PATH`：默认 `/nacos`；
- `NACOS_CLIENT_VERSION`：默认 `fiber-ai-server/1.0`。

CAT 默认关闭。只要任意 CAT 值非空，就必须同时提供：

- `CAT_APP_KEY`、`CAT_HOSTNAME`、`CAT_IP`；
- `CAT_ROUTER_ADDRESSES` 或 `CAT_COLLECTOR_ADDRESSES` 至少一个。

CAT endpoint 是逗号分隔的 `IPv4:port` 或 `[IPv6]:port`，不在启动配置中解析域名。
可直接复制并修改 [`ai-server.env.example`](ai-server.env.example)。

## Nacos 业务配置

所有 LLM 配置使用 group `LLM-SERVER`。固定订阅：

- `ploto.ai-llm.auth.bt1.keys`
- `ploto.ai-llm.models`

模型表按引用动态订阅：

- `ploto.ai-llm.provider.<provider-name>`
- `ploto.ai-llm.user-group.<group-name>`

`service://<service-name>` Provider 还会订阅 `DEFAULT_GROUP` 下的 NamingService
实例。只有 enabled、healthy、正权重、有效 IP/port 的实例进入快照。

初次启动必须得到 BT1 keys、models 以及全部被引用 Provider/user-group 的有效首个
值；完成前不启动对外服务。运行期非法或缺失更新会被拒绝，并继续使用最后一个完整
快照。每个请求 pin 住进入时的深不可变快照，因此配置刷新不会改变执行中的认证、
授权或 Provider 计划。

## 请求行为

认证从 `Authorization: Bearer <BT1>` 读取；缺失或空值时兼容
`x-api-key`。BT1 校验覆盖结构、编码、过期时间、HMAC-SHA-256 和常量时间 MAC
比较，签名通过前不会信任 username。

入口顺序固定为认证、请求约束、JSON 抽取、模型授权、route key、token
限流、执行计划、Provider 调用、usage 结算。主要边界：

- 请求正文最大 4 MiB；
- 同步成功响应最大 32 MiB，Provider 错误正文最大 4 MiB；
- 内部限流正文最大 64 KiB，远端 owner 调用超时 3 秒；
- Provider 整体调用上限 300 秒；
- 401/403/429、配置的 retryable status 和传输错误可在响应开始前重试；
- SSE header 写给客户端后不再切换 token、Provider 或 fallback；
- 客户端断开会中止上游 exchange，损坏的连接不会回到连接池；
- 成功响应必须先完成 token usage settle；远端 owner 不可用时 fail closed。

Provider 只执行与入站相同的协议。C++ 版本有意不实现 OpenAI/Anthropic 隐式协议
桥接；缺少同协议候选时返回 `provider_protocol_unsupported`。
`openai-embedding` 配置可以解析，但当前没有 embedding 入站路由。

## JSON 字段抽取与改写

`src/common/json/JsonPath.*` 提供可预编译的确定性 JSONPath 子集：

- 固定对象字段和数组下标；
- 对象、数组通配符及捕获变量；
- 一次遍历匹配多个 action；
- 解码值、原始 `[begin,end)` 区间和捕获作用域；
- 按任意命中路径替换已有 JSON 值，校验 replacement，并字节级保留未修改内容。

ai-server 在静态初始化边界编译 OpenAI/Anthropic 路径。请求只解析一次并记录
`model`/`stream` 区间；每个 Provider 尝试都从原始 `IoBuf` 生成独立 slice chain，
不会把上一次重写结果当作输入。重复字段会全部替换，未知字段、顺序、空白和数字
词法保持不变，缺失字段不会被隐式创建。

## 可观测性与敏感信息

日志固定输出 stderr/INFO，主要 category 为 `ai_server.lifecycle`、
`ai_server.config`、`ai_server.http`、`ai_server.llm`、
`ai_server.rate_limit` 和 `ai_server.audit`。

审计记录 request ID、协议、用户、kid、模型、正文大小/SHA-256、授权和限流结果、
Provider/token 名、状态、耗时及 usage；不记录 prompt、完整正文、Authorization、
Provider token、BT1 secret 或 Nacos 凭据。Prometheus label 使用固定低基数集合，
不会把 request ID、username、原始 model 或 token 名放入 label。配置 CAT 后，
请求和 Provider 尝试还会生成 CAT transaction。

## 所有权与关闭

- accept EventLoop 拥有 listener；
- HTTP worker 各自拥有请求、Provider runtime、DNS 和连接池 shard；
- process-wide token limiter 由固定 hash shard 和 mutex 保护，保证同一
  `username + model` 在多 worker 下只有一份状态；
- Nacos EventLoop 拥有 client、config/naming service、配置图、实例注册和成员订阅；
- 可选 CAT EventLoop 独占 sender。

收到 `SIGINT` 或 `SIGTERM` 后，进程依次停止 listener/排空 HTTP、等待 metrics 和
限流结算、关闭 Provider pool/DNS、停止 CAT、注销成员并关闭配置/Nacos 服务，最后
停止各 EventLoopGroup。

## 测试

```bash
cmake --build build --target fiber_ai_server_tests fiber_tests
./build/apps-build/ai-server/fiber_ai_server_tests
./build/fiber_tests --gtest_filter='JsonPathTest.*:LocalHttp1ConnectionPoolSetTest.*'
ctest --test-dir build --output-on-failure
```

测试覆盖配置、BT1、JSON 抽取/改写、协议错误和 SSE、授权与路由、Provider
重试/fallback、token 限流、指标、真实 HTTP mock Provider 集成以及关闭生命周期。
