# ploto-ai MCP 业务迁移计划

## 1. 目标与边界

本次迁移把 `../ploto-gateway/ploto-ai` 中对外可观察的 MCP 业务能力迁移到本项目
`apps/ai-server`。实现使用本项目现有 C++23、协程、`EventLoop`、HTTP、脚本引擎和
Nacos 客户端，不移植 Java/Netty/RxJava/IoC 框架代码。

迁移范围：

- 从 Nacos 动态维护 MCP 项目及各项目工具列表；
- 从 `ploto-admin-app` 加载工具脚本和工具描述，并兼容已有本地脚本缓存文件；
- 编译、共享和执行工具脚本；
- 提供 MCP Streamable HTTP 和旧版 HTTP+SSE 两种传输；
- 实现初始化、心跳、工具列表、工具调用及工具列表变化通知；
- 保留多 ai-server 实例之间基于 session id 前缀的请求转发能力；
- 完整处理启动、配置替换、项目删除、会话过期和进程关闭。

明确不迁移：Java 注入器、模块装配、RxJava 类型、Netty handler、管理后台页面、通用
网关路由框架，以及与上述 MCP 业务无关的 Java 监控实现。日志、CAT 根事务和 HTTP
统计复用 ai-server 现有设施。

## 2. 源端行为基线

### 2.1 配置与工具加载

源端使用 Nacos group `AI-SERVER`：

| data id | 内容 |
| --- | --- |
| `ploto.ai.project.lists` | `VersionedConfig<Set<String>>` 项目名集合 |
| `ploto.ai.tools.<project>` | `VersionedConfig<Set<String>>` 工具脚本 id 集合 |

工具由 Nacos 服务 `ploto-admin-app` 的
`GET /ploto-admin-app/aiMcp/getTool/{scriptId}` 返回。响应业务对象包含 `id`、`script`
和 MCP tool 描述（`name`、`description`、`inputSchema`）。缓存文件名为 `<id>.dat`，内容
格式为：

````text
```
<script>
```
<script 字段置空后的工具 JSON>
````

加载顺序为本地缓存优先，缓存不存在或无效时再访问管理端；远端加载成功后回写缓存。
同一个 script id 在多个项目间共享编译结果，项目替换必须等全部工具成功加载和编译后
一次性生效。

### 2.2 路由与协议

每个项目暴露：

- `POST|GET|DELETE /<project>/mcp`：Streamable HTTP；
- `GET /<project>/sse`：旧版 SSE 建立会话；
- `POST /<project>/message?sessionId=...`：旧版客户端消息入口。

JSON-RPC 固定为 `2.0`，请求 id 支持字符串或数字。业务方法为：

- `initialize`；
- `ping`；
- `tools/list`；
- `tools/call`；
- 客户端通知 `notifications/initialized`；
- 服务端通知 `notifications/tools/list_changed`。

支持协议版本 `2025-03-26`、`2024-11-05`，首选版本为 `2025-03-26`。初始化结果的
server name 为 `ploto-ai-server`，capabilities 为 `tools.listChanged=true`。

Streamable HTTP 使用 `mcp-session-id` 和 `mcp-protocol-version` header。初始化 POST
创建 session；含 request 的 POST 返回 `text/event-stream`，仅 notification/response 的
POST 返回 202；GET 打开该 session 唯一的独立 SSE 流；DELETE 结束 session。无独立 SSE
流时 session 空闲 60 秒过期。

旧版 SSE 在连接建立后先发送 `endpoint` event，内容为对应 `/message` URL；JSON-RPC
消息使用 `message` event。心跳在 20 秒后开始，之后每 25 秒发送一次 ping。

### 2.3 脚本结果

`tools/call` 的 `arguments` 作为脚本根对象。脚本返回标量时转换为文本，返回对象或数组
时序列化为 JSON 文本；无返回值时 content 为空。脚本异常转换为
`isError=true` 的 text content；协议、会话或运行时错误使用 JSON-RPC error。

工具脚本支持源端 `service`/`address` directive 及 `request`、`postJson`、`postForm`、
`getJson` 调用。`service "foo[/cluster]"` 映射到 Nacos 服务 `foo.app`，默认 cluster 为
`default`。

## 3. C++ 目标设计

### 3.1 所有权和事件循环

```text
Nacos EventLoop
  McpConfigManager
    project/tools subscriptions
    admin discovery + cold-path tool load/compile
    immutable McpConfigSnapshot publisher
                |
                v
HTTP worker EventLoops
  McpHttpHandler + worker-local clients/script services
                |
                v
process-wide McpSessionManager
  session control state + stream owner loop mailbox
```

- 所有 `ConfigService`/`NamingService` subscription、admin 服务发现和配置图变更只在
  Nacos loop 上运行；subscription 不跨 loop 使用。
- 配置构建成功后发布不可变 `shared_ptr` 快照。任一项目或工具加载失败时保留最后一次
  有效快照，不发布半成品。
- HTTP exchange 始终属于接收它的 worker。MCP session 是低频控制面状态，使用进程级
  并发表保证同一进程的非粘性 worker 调度仍能找到 session；独立 SSE/旧版 SSE 的写入
  通过有界 mailbox 投递回创建 stream 的 loop，绝不跨 loop 操作 exchange。
- 编译后的脚本和 directive 元数据由工具 runtime 共享所有权；工具更新时已有 session
  原子切换到新项目 runtime，正在执行的请求继续 pin 旧 runtime，避免悬空引用且保证
  `tools/list_changed` 后可以读到新列表。已经为编译脚本建立的低频 NamingService 监听
  保留到进程关闭，避免执行中的旧脚本失去服务目录。

### 3.2 配置图

新增 `McpConfigManager`，管理一个项目列表节点和按项目创建/回收的工具列表节点。每个
候选更新按以下顺序处理：

1. 校验版本 envelope、项目名和 script id，并拒绝重复值；
2. 计算新增、保留和删除的项目/工具；
3. 对新增工具执行 cache/admin 加载和脚本编译；
4. 所有依赖就绪后构造新的 `McpProjectRuntime` 和总快照；
5. 发布快照并发送 `notifications/tools/list_changed`；
6. 关闭不再需要的项目/工具配置 subscription；脚本服务发现监听保留到进程关闭。

与 Java 当前实现不同，项目从 `ploto.ai.project.lists` 删除时会同时从路由快照移除，并
回收其工具配置监听；不会只关闭监听而继续暴露旧项目。

### 3.3 工具模型与脚本执行

- `McpTool` 保存 script id、MCP tool JSON 描述和不可变编译程序；
- 工具 JSON schema 按管理端原 JSON 保真保存并在 `tools/list` 中原样输出；
- 使用本项目 `script::compile_script`、`GcHeap`、`JsValue` codec 执行，不引入异常；
- 增加 ai-server 专用 directive resolver，把 `service` 和 `address` 编译为固定目标；
- worker-local `McpScriptServices` 使用不可变服务发现快照、DNS 和
  `LocalHttp1ConnectionPoolSet` 获取连接；JSON/form 便捷函数建立在现有 HTTP 脚本调用
  语义之上；同逻辑 cluster 优先选择本 `AI_SERVER_ZONE` 实例，没有本地实例时才使用
  其他 zone，并忽略非正权重实例；
- MCP/admin 输入设置明确上限；脚本 HTTP 调用沿用本项目现有 HTTP timeout 和背压语义，
  失败转换成脚本异常或 MCP tool error，不影响 server 进程。

### 3.4 会话、SSE 与转发

session id 保持 Java 可观察格式：前 12 个十六进制字符为 ai-server IPv4+port 节点 id，
后缀为进程内递增十六进制序号。收到非本节点 session 时，从 ai-server 实例快照按节点 id
查找目标并透明转发 method、target、端到端 headers 和 body；SSE 响应按流转发，避免
无限缓冲。

本节点 session 状态机为 `Created -> Negotiated -> Initialized -> Closed`：

- 只有单个 `initialize` request 可创建和初始化 session；
- 后续请求要求 session id 一致且 session 已初始化；
- 一个 session 同时最多一个 standalone SSE stream；
- stream mailbox 有固定上限，慢客户端导致该 stream 关闭而不是无限分配；
- 无 standalone stream 的 session 使用 `EventLoop::current().now()` 更新活动时间并按源端
  60/65 秒规则清理；
- server shutdown 先停止接收新 session，再结束 streams、等待 handler、关闭连接池和
  Nacos subscription。

### 3.5 HTTP 路由顺序

保留现有 internal rate-limit、metrics、LLM、health/ready 精确路由优先级。只有未命中这些
保留路由时才解析 `/<project>/(mcp|sse|message)`，因此 MCP 项目名不能遮蔽现有 ai-server
接口。未知项目返回 404 JSON-RPC error envelope。

## 4. 实现文件与步骤

1. 协议与配置模型
   - `src/mcp/McpConfigSnapshot.*`
   - `src/mcp/McpJsonCodec.*`
2. 工具加载和配置生命周期
   - `src/mcp/McpToolLoader.*`
   - `src/mcp/McpConfigManager.*`
   - 在 `AiServerRuntime` 的 Nacos 启停序列中接入 manager
3. 脚本兼容层
   - `src/mcp/McpScriptRuntime.*`
   - `src/mcp/McpScriptServices.*`
4. 会话、协议和 HTTP 传输
   - `src/mcp/McpSessionManager.*`
   - `src/mcp/McpProtocol.*`
   - `src/server/McpHttpHandler.*`
   - 在 `AiServer` 路由、worker 初始化和 shutdown 中接入
5. 多实例转发
   - 从现有 ai-server NamingService membership 快照提供按 node id 查找；
   - `src/mcp/McpSessionForwarder.*` 实现普通响应和 SSE 流中继
6. 构建、示例配置和文档
   - 更新 `apps/ai-server/CMakeLists.txt`、README、env 示例和 MCP HTTP 示例

实现顺序遵循上述编号。每一步先完成可独立测试的 codec/state machine，再接入网络和
runtime，避免用端到端失败掩盖协议或生命周期错误。

## 5. 验证计划

单元测试至少覆盖：

- 项目/工具配置 envelope、非法名称、重复值和 schema 保真；
- cache 文件兼容读取、损坏回退、原子写入；
- initialize、版本协商、ping、tools/list、tools/call、batch、notification 和 JSON-RPC
  error；
- 重复初始化、缺失/错误 session header、协议 header、GET 冲突、DELETE 和过期；
- 配置全量替换、部分加载失败保留旧快照、项目删除、旧 session 持有旧 runtime；
- session id Java 前缀、节点查找和本地/远端判定；
- 脚本标量/JSON/void/exception 结果及 `service`/`address` directive 编译。

集成测试至少覆盖：

- Streamable HTTP 完整初始化、工具列表和调用；
- 旧版 SSE endpoint/message 往返及 list_changed；
- SSE 慢客户端/断开后的资源回收；
- 伪 Nacos 配置推送和伪 admin HTTP 服务加载；
- 两个 ai-server endpoint 之间的 session 转发。

最终执行：

```bash
./format_code.sh
cmake --build build --target fiber_ai_server_tests
ctest --test-dir build --output-on-failure
git diff --check
```

若本地存在可用 rnacos，再增加真实 ConfigService/NamingService 发布、读取和实例发现验证；
没有真实 ai-server 实例回读时，只把结果标记为本地/集成测试通过，不把它表述为生产运行
态已激活。
