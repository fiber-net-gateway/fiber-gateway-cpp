# ai-server 完整实现方案

## 1. 范围和兼容基线

业务行为以当前 Java `/home/dear/CLionProjects/ploto-gateway/ploto-llm` 的生产代码和
测试为基线，`docs/java-ploto-llm-business-flow-and-config.md` 为已经核对过的契约
索引。C++ 实现适配本仓库的 C++23 coroutine、EventLoop、`IoBuf`、HTTP client、
Nacos、CAT 和 Prometheus 设施，不机械复制 Java 对象图。

对外支持：

- `POST /v1/chat/completions`
- `POST /v1/messages`
- `POST /v1/message`
- `POST /internal/llm/rate-limit/check`
- `POST /internal/llm/rate-limit/settle`
- `GET /health`
- `GET /ready`
- `GET /metrics`
- `GET /_metric_prometheus`

Provider 只执行与入站协议相同的协议。OpenAI/Anthropic 隐式协议桥接是项目已经
确认的有意差异：不转换请求、响应或 SSE；缺少同协议候选时返回
`provider_protocol_unsupported`。`openai-embedding` 配置可加载，但没有入站路由。

## 2. 所有权模型

```text
accept EventLoop
└── AiServer / listener lifecycle

HTTP worker EventLoopGroup
├── WorkerState[N]
│   ├── current immutable LlmConfigSnapshot
│   ├── ProviderRuntimeRegistry
│   └── request-local ParsedLlmBody / execution plan / audit context
├── LocalHttp1ConnectionPoolSet[N]
└── WorkerDnsService[N] + one shared DNS cache

process-wide request service
├── TokenRateLimitService fixed hash shards + per-shard mutex
├── RateLimitShardRing immutable snapshot
├── TokenRateLimitCoordinator
└── fixed-schema AiServerMetrics worker slots

Nacos EventLoop
├── NacosClient
├── ConfigService
├── NamingService
├── LlmConfigManager dependency graph
├── ai-server instance registration
└── provider and rate-limit membership subscriptions

CAT EventLoop
└── CatClient sender ownership

log EventLoop
├── configured operational Appenders
└── code-owned audit FileAppender
```

核心约束：

- 一次请求在一个 HTTP worker 上完成，不把 `HttpExchange` 跨 loop 移动；
- 配置在 Nacos loop 构建成深不可变快照，worker 只替换一个
  `shared_ptr<const LlmConfigSnapshot>`；
- Provider 熔断和 token 暂停由所属 worker 单线程修改；
- token 限流按 `username + model` 固定 hash 到进程级 shard，并在 shard 内加锁，
  保证多个 HTTP worker 不会形成重复窗口；
- DNS resolver 和 HTTP pool 每个 worker 一份 owner-loop state；
- 请求 pin 住进入时的配置快照，刷新不改变执行中的认证、授权、限流规则和
  Provider 集合；
- 启动顺序为 dotenv -> 独立日志 JSON -> LoggerManager -> 业务 EventLoop；日志
  初始化成功后立即建立 shutdown guard，保证后续任意启动失败也会 drain/关闭；
- shutdown 顺序为 listener/drain -> metrics/limit/provider runtime -> CAT ->
  config manager/registration -> Nacos services -> 业务 EventLoopGroup -> log drain。

## 3. 入口流水线

LLM 入口严格按以下顺序：

1. 创建请求 ID、耗时和审计上下文；
2. 从当前 worker 取得并 pin 配置快照；
3. BT1 认证；
4. 校验 POST、`application/json` 和 4 MiB 正文上限；
5. 读取完整正文到单个 `IoBuf`；
6. 用预编译 JSONPath program 抽取路由字段并保存原始正文；
7. 在 CAT 根 `URL` Transaction data 记录 `stream=true|false`；
8. 校验逻辑模型名、查找模型、执行用户组授权，并将 CAT 根 Transaction name 更新为
   `<path>:<authorized-model>`；
9. 计算协议相关 route key；
10. 对 `username + model` 做 token 限流 check；
11. 生成有限且不可扩张的 Provider 尝试计划；
12. 每次尝试从原始正文改写上游模型、调用 Provider；
13. 仅在响应尚未开始且错误可重试时进入下一尝试；
14. 成功响应提取 usage，并提交 tracked best-effort 限流 settle；
15. 写回同步响应或转发 SSE，best-effort 提交审计，并完成指标和 CAT transaction。

没有有效 BT1 的错误优先于 405/415。Content-Length 已知且大于上限时不读取正文即
返回 413；chunked/未知长度在累计读取时执行同一上限。

通用层 `JsonPathProgram` 同时提供 action visitor 和 `rewrite_json_paths`：
replacement 必须是一个完整合法 JSON value，命中值按原始 `[begin,end)` 区间
替换，未命中内容字节级保留。ai-server 的 Provider 重试不重复运行通用 rewriter，
而是在第一次抽取时记录 `model`/`stream` 区间，之后从同一原始 `IoBuf` 生成独立
slice chain。

## 4. 认证和模型授权

现有 BT1 verifier 和 request authenticator 保持不变，并接到两个 LLM 路由最外层。
认证成功对象同时持有 principal 和配置快照。

模型授权：

- `model` 必须为 1..128 字节，字符集 `[A-Za-z0-9_.-]`；
- 模型不存在和用户无权访问都返回相同的 403 外观；
- `allow-user-groups` 为空时所有已认证用户可用；
- 非空时命中任意组即可；
- 不保留 Java 的硬编码 `zhangwang` 绕过。若业务必须保留，应改成显式配置，
  不能在 C++ 热路径埋用户名后门。

OpenAI 和 Anthropic 分别生成自己的错误 JSON；内部原因只进入无敏感信息的审计。

## 5. 路由键和执行计划

路由键优先级、UTF-8 字节截断和 Java 保持一致：

- OpenAI：metadata route key -> prompt cache key -> `role:content\n` 前缀 -> model；
- Anthropic：metadata route key -> container -> system + `role:content\n` 前缀 -> model。

Rendezvous score 为
`SHA-256(routeKey + "\n" + candidateKey)` 的前 8 字节按 big-endian 解释。Provider
按 score 降序、名字升序；同 Provider token 用
`providerName + "\n" + tokenName` 作为 candidate key。

计划生成规则：

- 主 Provider 和 fallback 独立过滤；
- 只接受当前入站协议；
- Provider 熔断、协议缺失、空服务实例、token 暂停都会过滤；
- 空 token 列表生成一次无凭证尝试；
- 非空 token 列表中每个可用 token 生成一次尝试；
- `max-primary-attempts` 限制不同主 Provider 数，不截断该 Provider 的 token；
- fallback 只在启用时追加；
- 无尝试时按证据优先返回 config、token 或 protocol unavailable。

`ResolvedExecutionPlan` 只借用被请求快照 pin 住的配置对象；请求结束前不会悬空。

## 6. Provider 运行状态和地址

`ProviderRuntimeRegistry` 以 Provider 名为 key，在配置代际之间保留健康状态，并在
token 删除时清理对应状态：

- 401/403：当前 token 暂停 5 分钟；
- 429/额度错误：当前 token 暂停 30 秒；
- 正整数 `Retry-After` 秒覆盖默认值；
- 传输错误和配置为 retryable 的状态累计 Provider 连续失败；
- 连续 3 次失败后 Provider 熔断 30 秒；
- 成功清除 Provider 连续失败并恢复成功 token。

固定 `http(s)://` 地址在 worker DNS resolver 上解析全部 A/AAAA，再依序连接。
resolver 仍使用共享的正向/权威负向缓存；ai-server 额外维护一个进程内、固定 64 项的
DNS transient failure cache。它只缓存 `TimedOut`，默认 TTL 为 2 秒，hostname 以
ASCII 小写并去除末尾点后比较。缓存命中不会再次访问 resolver，成功解析会清除对应
项；NXDOMAIN、NoData 和其他 I/O 错误不进入该缓存。
`service://` 以 Provider-scoped route key 和实例 endpoint 计算带权 Rendezvous
score，使用 Nacos 基础权重稳定选择实例。同一次 Provider 的实例级失败会加入请求级
排除集合，后续 token 尝试选择下一实例。连接池 key 仍只包含 scheme、host/IP 和
port，不包含 route key；HTTPS name 地址保留 SNI。
连接、收发总超时遵循 300 秒 Provider 上限，并保留较短的 connect timeout。

每次上游请求只构造固定头：

- `Host`
- `Content-Type: application/json`
- 同步 `Accept: application/json`，流式 `Accept: text/event-stream`
- 有 token 时 `Authorization: Bearer ...`

普通客户端头和客户端凭证不整体透传。

## 7. 响应和重试

同步路径先完整读取上游响应（错误正文上限 4 MiB，成功正文上限 32 MiB），
在客户端 header 尚未发送前完成失败分类：

- 401/403/429 始终可尝试后续 token/Provider；
- `retryable-status` 中的状态可重试；
- 非 DNS 传输错误可进入下一 attempt；
- DNS 错误直接跳到下一 Provider，并把当前 Provider 的剩余 token 计划项计为 skipped；
- 普通 4xx 不重试；
- 尝试耗尽后，同协议上游状态、Content-Type 和正文原样返回。

重试还要求客户端响应尚未开始且 response channel 仍然可用。任何下游关闭或写失败
一旦被当前请求观察到，都不会再切换 token、Provider 或 fallback。

SSE 路径先读取并验证上游最终 header。非 2xx 仍按同步错误正文分类；2xx 后发送
客户端 header，此刻设置 `response_started=true`。之后任何读取、SSE 解析或写出错误
只终止当前流，不再重试。

SSE parser 借助 `http::SseCursor` 处理跨 chunk 的 CRLF、多个 `data:` 行、注释和
空行边界。每次读取上游 body chain 后，先扫描完其中所有节点并提取完整 data event
的 usage，再把原始 `IoBufChain` 节点移动给客户端：

- 单个连续 data 借用输入；仅在 data 跨节点或包含多个 `data:` 行时拼装；
- CRLF、字段空格、注释、event/id/retry 和未知字段均保持原始字节；
- SSE framing 只识别 ASCII 分隔符和 `data` 字段名，其他字节不做 UTF-8 fatal 校验；
- OpenAI `[DONE]` 不补充、不去重，信任上游合法输出；
- usage collector 只旁路解析 data JSON 的少量 usage 路径，提取失败不改变响应；
- 每次下游 chain write 完成后才继续读取上游，保持背压；
- 下游 header/body 写失败后停止继续写客户端，但当前 upstream 仍由同一请求协程读到
  完成或 300 秒读超时，继续执行 SSE 解析、审计和 usage 提取；
- drain 完成后按观察到的 usage 结算 token；没有观察到 usage 才 settleNoUsage；
- drain 期间的上游错误只标记当前 Provider attempt 失败，绝不触发重试；完整读完的
  upstream 仍按 Provider 成功处理，连接可正常回池。

## 8. token 限流

没有 rate-limit 规则时直接返回无操作 session。有限流规则时：

- key 为 username + NUL + logical model；
- check 直接使用请求所 pin 的 `CompiledModelRoute` 及其编译后规则，不再读取进程
  当前配置；
- check 不预占 token；
- `used < max` 才允许；
- 编译后规则包含稳定 revision；它随 models 配置版本变化，但 Provider、服务发现或
  用户组刷新不会改变它；
- owner 按 `username + model + rule revision` 保存多个版本的状态，ticket 包含
  rule revision、state generation 和 window start；
- 成功 usage settle 计数，其余路径 settleNoUsage；
- Provider 执行后的 settle 为 tracked best effort：同步响应和 SSE 都不等待远端
  settle，结算失败不会替换或中止 Provider 响应；
- SSE 下游断开后仍以当前 upstream 最终观察到的 usage 结算，避免已经消耗的 token
  因客户端交付失败而漏计；
- settle 完成后记录 usage/no_usage/error 固定指标，stale、远端错误和 shutdown
  拒绝同时写 WARN 日志；
- shutdown 先排空已经接受的远端 settle，再停止 metrics 和限流远端 client；
- 超额按 Java 公式延长 recoverAt；
- 配置刷新后旧 revision 的在途 ticket 仍结算到旧状态，不写入新规则；
- 无 in-flight、超过 recoverAt 且空闲 10 分钟后清理。

服务成员形成 MurmurHash3 一致性哈希环，每权重 200 虚拟节点，只选一个 owner。
本机 owner 直接执行；远端 check 会把请求所 pin 的 rule revision、窗口和额度传给
owner，settle 用 ticket 回到同一版本状态。远端调用 64 KiB 内部 check/settle 接口，
超时 3 秒并失败关闭。owner 信任 peer 传入的编译规则，且内部接口默认不做 BT1，
因此集群节点必须使用兼容协议并受信任，部署必须限制网络访问；文档和指标显式暴露
该边界。

## 9. 错误、审计和指标

dotenv 只保存必填的 `AI_SERVER_LOG_CONFIG_PATH`，不再承载 level、queue 或 audit
参数。相对路径以 dotenv 文件目录为基准；日志 JSON 内的相对 file/audit 路径再以
日志 JSON 目录为基准。日志 JSON 最大 1 MiB，仅在启动时加载，不参与 Nacos 热更新。
解析器按 `version=1` 严格校验未知字段、重复字段、类型、范围、appender 引用和路径
冲突，任何错误都在创建业务 EventLoop 前让进程失败。

JSON 是常规运行日志、异步队列容量和 audit 参数的唯一来源。常规配置支持 stderr
console、file、level range、buffer/flush、rotation、root logger，以及
`ai_server`、`ai_server.lifecycle`、`ai_server.config`、`ai_server.http`、
`ai_server.llm`、`ai_server.discovery`、`ai_server.rate_limit` 的精确 category
覆盖。所有常规 appender 必须被引用；console 只能选择 stderr。listener 成功后
main 向 stdout 输出的单行地址是服务发现通道，不经过 LoggerManager，也不受该 JSON
控制。

进程固定异步队列策略为 `DropNewest`。`ai_server.audit` 及
`ai_server_audit_file` 为代码保留名：外部配置只能给出 audit 路径、单条记录上限、
轮转阈值和归档数，不能改变 INFO、`additive=false`、`0600`、unbuffered、
no-follow、普通文件限定、权限强制、不完整尾部恢复和 message-only NDJSON 输出格式。
滚动归档名固定为 `{base}.{utc}.{seq}`，同时包含 UTC 时间戳和防重序号。

错误层维护一个内部 `LlmError`，包含 HTTP status、稳定 code、公开 message、field
和是否可重试。序列化器按入站协议生成 OpenAI 或 Anthropic 外观，绝不把 C++ 错误、
Provider token、BT1 token 或配置 secret 写入响应。

请求审计由请求级 RAII owner 聚合。请求结束时，当前 HTTP worker 把
`schema_version=5` 扁平对象直接编码到一条 `ai_server.audit` 日志记录中。记录随后
提交给进程共享的 log EventLoop，该线程是 stderr 和审计文件的唯一正常 writer；
审计 logger 关闭 additive，不会复制到 stderr；专用 FileAppender 只写 message 和
换行，因此文件中每个物理行都是可直接解析的完整 JSON。
同一对象包含：

- request ID、采集是否完整及稳定的采集错误；
- 来源、方法、路径、协议、User-Agent、Host、X-Real-Ip，以及唯一一份完整
  `request_json`、content type/size/hash；
- 从 body 单独物化的 `requested_model`、有效 `stream`、message/tool 数量；
- username、kid、resolved model、授权和限流结果；
- 从同步响应或 SSE delta 聚合出的 output role、`response_json`、tool/finish reason；
- `attempts_json` JSON 字符串中每次尝试的 Provider/token 名、协议、上游模型、路径、配置
  版本、fallback、状态和延迟，以及失败阶段、I/O 错误、失败来源、重试目标、是否
  真正重试和跳过的计划项数；
- 请求级 `provider_attempt_skipped_count`；
- 最终状态、响应字节、客户端中断、`error_json`、usage 和毫秒总耗时。

模型输出仍是 Provider 已生成并被网关观察到的内容，是否完整交付客户端以
`client_aborted` 和 `error_json` 为准。
Provider 非 2xx 错误正文不会被当成模型输出。

`usage_json` 统一输出 `promptTokens`、`completionTokens` 和 `total_tokens`：
`promptTokens` 是 `in_cache + in_nocache`，`completionTokens` 来自 `out`，缺失值
按零处理。内部 token 提取规则为：
OpenAI 用 `prompt_tokens_details.cached_tokens` 作为缓存输入并从
`prompt_tokens` 中扣除；Anthropic 只把 `cache_read_input_tokens` 计入缓存命中，
`input_tokens + cache_creation_input_tokens` 计入非缓存输入。有效 usage 同时写入
CAT `LLMTokenUsage` 子 Event。
模型配置 `allow_user_groups` 时，每次检查还会生成 name 为 username 的 CAT `Auth`
子 Event。最终放行时 status 为 `Success`，命中用户组时 `allowed_user_group` 为按
模型配置顺序命中的第一个组；最终拒绝时 status 为 `Error`。现有 `zhangwang` 旁路
记录 `Success`，但因未实际命中用户组而不写 `allowed_user_group`。

request body 只写一次，不再构造或输出 `llm.input.prompt_parts`。UTF-8 body 作为
JSON string 保留原始字节，非 UTF-8 body 使用 base64；因此多模态 URL、base64 和音频
字段也属于完整审计内容。同步/流式输出不截断；stream delta 在转发给客户端之前追加，
聚合形态与同步 `llm.output` 一致。记录上限、内存分配、JSON 生成、日志准入或磁盘
writer 失败时只丢弃对应审计并增加指标，不改变认证、路由、Provider 调用、HTTP
状态、同步正文或 SSE 字节。日志系统使用 `DropNewest`，请求线程不等待 backlog，
也不读取投递结果。

专用文件以 `0600` 和 append 模式打开，拒绝符号链接和非普通文件；FileAppender
仅在记录之间轮转，进程启动和 reopen 时截掉活动文件末尾的不完整行，停机时在业务
EventLoop 全部结束后 drain 已提交记录。滚动归档文件名包含 UTC 时间戳和单调序号，
例如 `ai-server-audit.ndjson.20260730T083012Z.000001`；活动文件名保持配置的固定路径。
Authorization、Provider token 值、BT1 token 和配置 secret 不作为独立字段输出，
但 body/prompt/模型输出本身可能携带任意业务敏感信息，部署必须设置严格的读取、
采集、传输和保留策略。

Prometheus 的常规运行指标继续使用固定低基数 label，包含请求数/延迟/在途、
Provider 尝试与失败、重试、熔断、限流准入/拒绝/settle、配置代际和 SSE 中途失败。
进程 CPU、RSS/virtual memory、启动时间和 FD 数使用标准 `process_*` 名称，在
`/metrics` scrape 慢路径按需读取 Linux `/proc`、进程 CPU clock 和 rlimit；它们不
进入 worker shard，也不创建后台采集任务。单项读取失败时省略对应 family，不影响
业务指标输出。整机 CPU、内存和 load 仍由 node_exporter 拥有，Grafana dashboard
通过独立的 `node_job`、`node_instance` 变量查询，避免把 ai-server target 与
node_exporter 不同端口的 `instance` label 错误关联。
Provider 传输失败阶段、计划剪枝和 DNS timeout backoff 分别使用
`ai_server_provider_transport_failures_total{protocol,phase}`、
`ai_server_provider_attempts_skipped_total{protocol}` 和
`ai_server_dns_backoff_hits_total{protocol}`，不引入 Provider/host/token label。
`ai_server_sse_drains_total{protocol,result}` 记录下游交付失败后的 upstream drain，
`result` 固定为 `completed`、`upstream_error`、`timeout`。
审计另有 generated/generation failure/capture incomplete，以及 FileAppender 的
written/dropped/write/reopen/rotation/retention/active-bytes 指标，用于观测
best-effort 链路，不作为请求或 readiness 条件。
token usage 另有两个累计 Counter family：
`ai_server_user_token_usage_total{username,token_type}` 和
`ai_server_provider_token_usage_total{provider_name,protocol,token_type}`，
`token_type` 固定为 `in_cache`、`in_nocache`、`out`。username series 按需求保留原文
并具有高基数风险；series 在进程生命周期内不回收。request ID、model 原文和 token
名仍不能作为 label。CAT transaction 在独立 sender loop 发送，业务 worker 只提交
轻量消息。`URL` root data 使用空格分隔的 `key=value` 字段，并把最多 1024 字节的入站
`User-Agent` 作为最后一个 `user_agent` 字段；超出时同时记录
`user_agent_truncated=true`。CAT 客户端的其他消息仍默认使用官方 `&` 分隔规则。
`LLM.Provider` 保留整个 attempt 作为原生 duration，data 只保留 attempt 身份、实际
HTTP status、fallback 标记和三个 Provider 里程碑：开始发送到最终 response header、
开始发送到流式响应首个非空模型输出增量，以及首个非空 body chunk 到完整 body 结束，字段分别为
`time_to_response_header_us`、`time_to_first_token_us`、`body_transfer_us`。first
token 是网关解析完整 SSE 事件后观测到的 TTFT：OpenAI 文本、拒绝和 tool-call 增量及
Anthropic 文本和 tool-use 增量均计入，纯元数据事件不计入；它不表示 Provider 内部
tokenizer 的精确边界。只输出实际收到的 HTTP status 和已经观察到的里程碑，非流式响应
不输出 first token；SSE 的 body transfer 包含交替下游写入形成的背压时间。
每个 Provider 的 `RemoteCall` Event 归属相应的 `LLM.Provider` Transaction。失败
attempt 保持父 Transaction 的失败状态，并增加 `LLM.UpstreamError` 子 Event：
存在传输 `failure_code` 时，Event name 使用其小写 snake_case 名称，例如 `dns`、
`connect`、`read_header`；非 2xx 响应没有传输错误码，使用 `upstream_error`。
Event name 本身承载 failure phase；Event data 按实际存在的值记录 I/O error、
failure source、上游状态、retry target、是否执行重试、跳过的 attempt 数和响应是否
已经开始，默认的 `none`、`false`、`0` 不输出。成功 attempt 不生成错误 Event。
ai-server 通过 `send_error` 生成最终错误响应时，另在 root 下生成
`LLM.ResponseError` Event：name 使用响应 code，data 记录 status、type 和最多
1024 字节的 message，截断时增加 `message_truncated=true`；编码失败则记录实际回退的
`internal_error`/500。它描述网关准备发送的最终错误，`LLM.UpstreamError` 仍描述上游
原因；通过 `send_body` 原样透传的上游非 2xx 响应不在此范围。

listener 只在完整首个配置安装到所有 worker 后绑定；服务注册使用启动配置
`<AI_SERVER_ZONE>-<AI_SERVER_CLUSTER>` 作为 Nacos cluster，注册和初始本机限流节点
建立后才启动 accept。`/ready` 实时检查配置快照和非空成员环；审计运行状态不影响
readiness。

## 10. 完成标准

实现完成必须同时满足：

- 两个对外协议入口、别名、内部限流接口和 metrics 路由可用；
- Java 兼容的认证、授权、route key、计划顺序、重试和限流单元测试；
- 本地 mock Provider 的同步、SSE、断连、429/Retry-After、401 token 轮换、
  fallback 和响应开始后禁止重试集成测试；
- Nacos 快照刷新期间旧请求继续使用旧配置；
- shutdown 无悬挂 exchange、DNS 请求、pool lease 或配置 watcher；
- `fiber_ai_server_tests`、仓库 `fiber_tests` 和 `ctest` 通过；
- 最后统一执行 `./format_code.sh` 和 `git diff --check`。
