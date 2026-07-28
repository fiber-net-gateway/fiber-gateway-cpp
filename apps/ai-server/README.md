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
- 固定 schema Prometheus 指标、专用对话审计日志和可选 CAT transaction；
- 延迟绑定、请求排空和跨 EventLoop 有序关闭。

完整设计见 [`docs/architecture.md`](docs/architecture.md)，JSON 字段能力见
[`docs/json-field-transform.md`](docs/json-field-transform.md)。配置控制台的前端需求、
交互、配置后台接口和发布模型见
[`docs/config-console-requirements.md`](docs/config-console-requirements.md)。

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
sidecar 或服务网格限制访问。内部 check 会携带请求快照中的限流规则版本和参数，
owner 会直接信任这些字段；因此限流节点还必须运行兼容协议并处在同一信任边界内。

## 构建与运行

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_ai_server
cp apps/ai-server/ai-server.env.example ai-server.env
cp apps/ai-server/ai-server.logging.json.example ai-server.logging.json
./build/apps/ai-server
```

也可以把 dotenv 文件作为唯一位置参数传入：

```bash
./build/apps/ai-server /etc/ai-server/production.env
```

未传参数时读取当前目录的 `ai-server.env`。配置解析支持空行、注释、可选
`export`、单/双引号和行尾注释；重复键、未知键和非法值都会让启动失败。
日志使用独立的严格 JSON 文件，dotenv 中只保留它的路径。

## 启动配置

HTTP 和集群成员：

- `AI_SERVER_LISTEN_ADDRESS`：默认 `0.0.0.0`；
- `AI_SERVER_LISTEN_PORT`：默认 `8080`，`0` 可用于本地测试；
- `AI_SERVER_ADVERTISE_ADDRESS`：可选的 Nacos 注册 IPv4；未配置时选择第一个
  非 loopback IPv4，找不到时使用 `127.0.0.1`；
- `AI_SERVER_SERVICE_NAME`：默认 `ploto-ai-server`；
- `AI_SERVER_SERVICE_GROUP`：默认 `DEFAULT_GROUP`；
- `AI_SERVER_ZONE`：默认 `daily1`，与 Java `dev` 环境默认值一致；
- `AI_SERVER_CLUSTER`：默认 `dev`，与 `AI_SERVER_ZONE` 组合成实例注册的 Nacos
  cluster（默认 `daily1-dev`）；
- `AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS`：默认 `60000`，`0` 表示无限等待。

日志：

- `AI_SERVER_LOG_CONFIG_PATH`：必填，指向独立日志 JSON；相对路径以 dotenv 文件所在
  目录为基准，最多 4096 字节。

Nacos：

- `NACOS_SERVER_ADDRESSES`：必填，逗号分隔的 IPv4/IPv6 literal；
- `NACOS_HTTP_PORT`：默认 `8848`；
- `NACOS_GRPC_PORT`：默认 `9848`；
- `NACOS_NAMESPACE_ID`：Naming namespace，默认 `public`；
- `NACOS_TENANT`：ConfigService tenant，默认空；
- `NACOS_USERNAME`、`NACOS_PASSWORD`：必须同时为空或同时配置；
- `NACOS_CLIENT_VERSION`：默认 `fiber-ai-server/1.0`。

认证路径固定为 `/nacos/v1/auth/users/login`，不提供 context path 配置项。

CAT 默认关闭。只要任意 CAT 值非空，就必须同时提供：

- `CAT_APP_KEY`、`CAT_HOSTNAME`、`CAT_IP`；
- `CAT_ROUTER_ADDRESSES` 或 `CAT_COLLECTOR_ADDRESSES` 至少一个。

CAT endpoint 是逗号分隔的 `IPv4:port` 或 `[IPv6]:port`，不在启动配置中解析域名。
可直接复制并修改 [`ai-server.env.example`](ai-server.env.example) 和
[`ai-server.logging.json.example`](ai-server.logging.json.example)。

## 日志配置

日志配置只在进程启动时加载，不热更新。文件最大 1 MiB，必须是严格 JSON；未知字段、
重复字段、缺少必填字段、非法引用或无效值都会让启动失败。所有相对日志文件路径都以
日志 JSON 所在目录为基准。

顶层字段全部必填：

| 字段 | 规则 |
| --- | --- |
| `version` | 当前只能为整数 `1` |
| `queue.capacity_bytes` | 正整数；队列满策略固定为 `DropNewest`，不能从配置改成阻塞 |
| `appenders` | 常规运行日志 appender 数组 |
| `root_logger` | 必须配置 `level` 和至少一个 appender；`verbosity` 可选 |
| `loggers` | category 覆盖数组，可以为空 |
| `audit` | 对话审计文件和记录上限 |

常规 appender 的 `name` 在文件内唯一，`min_level`/`max_level` 可选，level 取值为
`trace`、`debug`、`info`、`warn`、`error`、`fatal`：

- console appender 使用 `type: "console"`，必须显式配置 `stream: "stderr"`；
  `stdout` 不属于日志配置；
- file appender 使用 `type: "file"` 并提供 `path`；可选 `mode`（四位八进制字符串，
  默认 `"0644"`）；
- file appender 的 `buffer_bytes` 和 `flush_interval_ms` 必须同时出现且都是正整数；
- 可选 `rotation` 必须同时提供正整数 `max_bytes`、`max_archives` 和
  `archive_name`。归档名只能使用安全文件名字符及 `{base}`、`{utc}`、`{seq}`
  占位符，其中 `{base}` 和 `{seq}` 各出现一次；
- 每个常规 appender 必须被 root 或某个 category 引用，同一 logger 不能重复引用，
  不允许两个 file appender 或 audit 使用同一规范化路径。

`root_logger` 和 category 的 `verbosity` 是非负整数。category 可覆盖 `level`、
`verbosity`、`appenders` 和 `additive`，但名字只允许：

- `ai_server`
- `ai_server.lifecycle`
- `ai_server.config`
- `ai_server.http`
- `ai_server.llm`
- `ai_server.discovery`
- `ai_server.rate_limit`

`ai_server.audit` 和内部 appender 名 `ai_server_audit_file` 由代码保留，不能覆盖。
`audit` 必须提供 `path`、正整数 `max_record_bytes`、非负整数 `rotate_bytes` 和正整数
`max_archives`；`rotate_bytes: 0` 禁用轮转。审计 appender 始终为 unbuffered、
`0600`、no-follow、普通文件限定、启动尾部恢复，logger 始终为精确
`ai_server.audit`、INFO、`additive=false`。这些安全和隔离约束不开放配置。

进程仍会在 listener 成功后向 stdout 输出一行服务发现信息。这不是运行日志，不受
日志 JSON 控制，便于本地启动脚本发现实际绑定端口。

## Nacos 业务配置

所有 LLM 配置使用 group `LLM-SERVER`。固定订阅：

- `ploto.ai-llm.auth.bt1.keys`
- `ploto.ai-llm.models`

模型表按引用动态订阅：

- `ploto.ai-llm.provider.<provider-name>`
- `ploto.ai-llm.user-group.<group-name>`

`service://<service-name>` Provider 还会订阅 `DEFAULT_GROUP` 下的 NamingService
实例。只有 enabled、healthy、正权重、有效 IP/port 的实例进入快照。
服务实例默认使用平滑加权轮询；模型的 `load-balance` 配置
`service-instance-policy: weighted-rendezvous` 后，改用基于 route key 和 Nacos
权重的 Rendezvous Hash。实例级失败后的同 Provider 重试会排除本次请求已经失败的
实例。

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
- 请求 pin 住进入时的不可变配置快照，认证、授权、限流和 Provider 选择不会跨刷新
  混用配置；
- Provider 执行后的 token usage settle 为 tracked best effort；失败只进入固定指标和
  WARN 日志，不替换同步响应，也不中止已经开始的 SSE。模型执行前的限流 check 仍然
  fail closed。

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

常规运行日志的 appender、level、verbosity 和 category 继承关系由独立日志 JSON
配置，主要 category 为 `ai_server.lifecycle`、`ai_server.config`、
`ai_server.http`、`ai_server.llm`、`ai_server.discovery` 和
`ai_server.rate_limit`。
LLM 对话审计使用独立的 `ai_server.audit` logger，不向 stderr 传播。请求 worker
直接生成 JSON 并构造一条 `audit_json=<json>` 日志记录，随后投递给进程共享的异步
日志线程；文件中的每行因此是常规日志前缀加稳定标记 `audit_json=` 和一个完整 JSON，
采集端应从该标记后解析 JSON。

当前审计 schema 为 `schema_version=3`。`request.body` 是入站 body 的唯一完整副本：
合法 UTF-8 以 `body_encoding=json_text` 保存，非法 UTF-8 以 base64 保存；图片 URL、
音频/base64 等多模态字段不会再被过滤。为了让采集服务不必解析 body，`request`
还直接提供 `request_model_name`、有效 `stream`、`messages_count` 和 `tools_count`。
`routing.resolved_model_name`、identity、rate-limit、完整 `provider_attempts`、response、
usage 等信息与它位于同一对象。

同步和 SSE 共用同一个 `llm.output` 聚合器。SSE delta 在转发前依次追加，最终
`role/content/tool_names/tool_arguments/finish_reason` 的形态与同步响应一致；
`complete` 表示 Provider 流完整结束，`canonical_complete` 还要求解析和聚合均成功。
内容不做截断。审计是 best effort：记录超过日志 JSON 中的
`audit.max_record_bytes`、
内存分配/JSON 生成失败、日志 backlog 满或文件写入失败时，丢弃受影响的审计记录，
不会改变 HTTP 状态、Provider 调用、同步正文或 SSE 转发结果。ai-server 把日志系统
配置为 `DropNewest`，请求线程不等待日志容量，也不读取投递结果。

审计 FileAppender 只在记录边界轮转，绝不拆分一条日志；启动和 reopen 时会删除活动
文件末尾由崩溃留下的不完整行。文件拒绝符号链接和非普通文件，并强制为 `0600`。
优雅停机先排空 HTTP 请求和业务 EventLoop，再由日志系统 drain 全部已提交记录。
Authorization、Provider token 值、BT1 secret 和 Nacos 凭据不会作为独立字段写入，
但完整 request body 和模型输出本身可能包含任意业务秘密，生产环境必须严格限制文件
读取、采集、传输和保留权限。

`usage` 将两种协议统一为 `in_cache`、`in_nocache`、`out` 和派生的
`total_tokens`。OpenAI 的 `in_cache` 来自 `cached_tokens`，`in_nocache` 为
`prompt_tokens - cached_tokens`；Anthropic 的 `in_cache` 来自
`cache_read_input_tokens`，`in_nocache` 为 `input_tokens +
cache_creation_input_tokens`。配置 CAT 后，每份有效 usage 还会生成
`LLMTokenUsage` 子 Event，携带相同三个用量字段、协议、上游模型和实际 Provider。

Prometheus 输出两个累计 Counter family：

- `ai_server_user_token_usage_total{username,token_type}`；
- `ai_server_provider_token_usage_total{provider_name,protocol,token_type}`。

其中 `token_type` 固定为 `in_cache`、`in_nocache`、`out`。username 是需求指定的
高基数 label；进程会保留首次出现的 username/Provider series 直至退出，部署时应
据实际用户规模评估时序数量。其他指标仍使用固定低基数 label，不会把 request ID、
原始 model 或 token 名放入 label。请求和 Provider 尝试继续生成 CAT transaction，
但 Provider 尝试不额外写独立的 audit 日志行。

审计指标区分请求线程生成与日志线程写入：`ai_server_audit_generated_records_total`、
`ai_server_audit_generation_failures_total`、`ai_server_audit_capture_incomplete_total`，
以及 written records/bytes、dropped records、write/reopen/rotation/retention failures
和 active file bytes。它们用于发现 best-effort 审计缺口，不参与请求成功判定或
`/ready`。

## 所有权与关闭

- accept EventLoop 拥有 listener；
- HTTP worker 各自拥有请求、Provider runtime、DNS 和连接池 shard；
- process-wide token limiter 由固定 hash shard 和 mutex 保护，保证同一
  `username + model` 在多 worker 下只有一份状态；
- Nacos EventLoop 拥有 client、config/naming service、配置图、实例注册和成员订阅；
- 可选 CAT EventLoop 独占 sender；
- 进程共享的 log EventLoop 是 stderr 和审计文件的唯一正常写入者。

收到 `SIGINT` 或 `SIGTERM` 后，进程依次停止 listener/排空 HTTP、等待 metrics 和
限流结算、关闭 Provider pool/DNS、停止 CAT、注销成员并关闭配置/Nacos 服务，停止
各业务 EventLoopGroup，最后由日志系统排空并关闭共享 log worker。

## 测试

```bash
cmake --build build --target fiber_ai_server_tests fiber_tests
./build/apps-build/ai-server/fiber_ai_server_tests
./build/fiber_tests --gtest_filter='JsonPathTest.*:LocalHttp1ConnectionPoolSetTest.*'
ctest --test-dir build --output-on-failure
```

测试覆盖配置、BT1、JSON 抽取/改写、协议错误和 SSE、授权与路由、Provider
重试/fallback、token 限流、指标、真实 HTTP mock Provider 集成以及关闭生命周期。
