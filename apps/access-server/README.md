# Access Server

`access-server` 是 Java `../ploto-gateway/ploto-unified-access` 配置和请求执行
能力的 C++23 迁移目标。`../ploto-gateway/unified-access-server` 只作为进程装配和
部署参数参考。

实现直接复用本仓库的 `fiber_lib`、JSON、脚本、connection pool，以及
`apps/nacos`、`apps/cat`、`apps/prometheus`。迁移不要求这些基础设施与 Java
`fiber-net-gateway` 内部实现兼容；兼容性只在统一接入配置和最终请求结果边界验收。

## 当前状态

当前完成应用脚手架、项目配置 codec、Host/Path 不可变路由快照、RESPONSE 执行内核，
以及 PROXY 请求、普通响应和 WebSocket 101 tunnel：

- CMake 已注册 `fiber_app_access_server`，产物名为 `access-server`；
- 已建立 `access_server_core` 和 `fiber_access_server_tests`；
- 已实现项目列表、`ProjectConf`、`HostStrategy`、`RouteItem`、Duration/DataSize 的
  Java 兼容解码；
- 已实现 route build-time 校验、CIDR/address 编译、RESPONSE body 预解码和
  service/cluster 上游计划；
- 已实现 Java 兼容 Host 校验与 exact/wildcard 匹配，并直接复用本仓库
  `RoutePathMatcher` 完成 Path/条件路由选择；
- 已实现跨项目全局 Host 树、候选构建失败保旧、同 version 忽略、Host 为空卸载，
  以及请求对旧不可变快照的 pin；
- 已实现 RESPONSE 的 TEXT/BASE64/TEMPLATE/空 body、受保护响应头过滤、header/body
  原子准备和统一 JSON/HTML 错误结果；项目匹配后的 access-owned header、最终 route/proxy
  header 和 trace header 由请求级 `AccessRequestTelemetry` 统一持有；
- 已实现可直接交给本地 HTTP server 的请求 handler，完成快照 pin、Host/Path/条件
  路由、X-Entry、HTTPS redirect、CIDR、request body limit 和 RESPONSE 串联；
- 已将 PROXY 接入同一 live handler；handler 将 pinned route 和轻量请求上下文直接交给
  executor，由 executor 完成 service/cluster/addresses、method、URI/rewrite/query、Java
  固定 header 过滤、proxy/context/source header、body framing/limit、timeout、flush 和
  WebSocket 请求条件；
- 已实现基于 `StealableHttp1ConnectionPoolSet`/`ClientHttp1Exchange` 的完整 `ProxyExecutor`
  状态机：先选择静态地址或 service endpoint，再构造实际 `Http1RequestHead`，随后查询
  connection pool，并在 miss 后完成 DNS/多地址连接；header/body 流式收发、动态 body
  limit、Java request timeout 和 request header 发送前的连接失败重选均已实现；pool
  lease、discovery generation 与栈上的 upstream exchange 保持到 response/tunnel 结束；
- 已实现普通 upstream response bridge：status/reason/header/body、Java 固定
  hop-by-hop 过滤、自定义响应头模板、Content-Length 特殊恢复、Location/Refresh
  回写、flush 和已知/动态 response body limit；
- 已实现 response header/body 等待期间的 downstream close 竞速、upstream 提前结束
  处理，以及 WebSocket 101 的双向 raw tunnel；101 不经过普通 HTTP body 完成路径；
- 真实 loopback upstream 已覆盖 chunked/Content-Length wire 请求、默认 Host/source
  header、body 字节、service 重选、同一 TCP 连接复用、响应头改写/覆盖、双方提前关闭
  和 WebSocket 双向字节；
- condition 和 `${...}` expression 已在候选快照发布前交给本地 C++ 脚本引擎预编译，
  请求只同步执行不可变程序；支持 `$path/$query/$header/$cookie/$req/$context`，
  编译失败保留上一版配置，通用 Java 脚本兼容不属于本次迁移范围；
- Java golden fixtures 已覆盖未知字段、重复字段、null、标量转型和主要配置字段；
- 已实现 owner-loop Nacos 配置图：项目列表驱动逐项目订阅增删，空/同 version/非法
  更新保留当前路由，列表移除卸载项目，shutdown 等待全部 listener 关闭；
- 已实现 production gray 配置 codec、失败保旧的原子规则快照，以及 CIDR/ratio 对
  NamingService cluster 的覆盖；`context.cluster` 同样会覆盖 route 默认 cluster；
- 已实现 NamingService route 依赖协调、健康/权重/zone/cluster 过滤、不可变服务目录、
  discovery generation pin，以及基于本地 `DnsResolver` 的执行器 DNS adapter；
- 已建立兼容边界、详细配置/请求契约和分阶段验收清单；
- 已实现 `AccessServerRuntime`：启动 Nacos client/config/naming，建立 project/gray
  watcher 和 NamingService selector，在每个 HTTP worker 初始化 DNS resolver 与本地
  connection pool，并在收到项目列表首值后才绑定 listener；
- `main` 已装配 SIGINT/SIGTERM、accept loop、HTTP worker group、CAT sender loop、
  Nacos owner loop 和逆序关闭；关闭顺序为 metrics/业务 listener 与 active exchange、
  指标采集和 CAT worker 上下文、connection pool/DNS、CAT client、配置和服务订阅、
  NamingService、ConfigService、NacosClient；
- 已通过真实 rnacos 验证启动、项目/路由首值、version 热更新和 SIGTERM 退出，并通过
  loopback listener 测试验证 HTTP worker 资源关闭；
- 已实现 Java 测试环境 Host cluster 入口规则：`api_gray.example.com` 以
  `api.example.com` 路由，`gray` 作为请求 cluster，并向上游传递
  `ploto-origin-host`；无 Host cluster 时读取 `HI-TRACE-CLUSTER`；
- 已接入本地 CAT request tree：继续入站 `HI-TRACE-ID`/`HI-SPAN-ID-PARENT`/
  `HI-SPAN-ID`，响应写回 `Hi-Trace-Id`，代理调用生成下一 span；根事务采用 Java
  `URL` 类型和 `<project><route-pattern>` 名称，并记录 project、route、cluster、
  upstream、稳定错误名和最终响应状态；
- 已接入独立 Prometheus listener，默认 `0.0.0.0:16689`，请求完成计数、inflight
  和 duration 全部使用 worker 预绑定的固定 schema；动态 project/route/cluster
  不作为指标 label，避免测试 header 或热更新配置形成无限时序；
- 已接入共享异步 logging 生命周期，访问日志在 `access_server.access` 以结构化
  key/value 输出 trace、请求、路由、上游、结果、耗时和字节数；队列满时丢弃新日志，
  不反向影响请求执行；
- 现网脚本 corpus 差分和阶段 8 全量差分尚未完成；当前二进制可用于继续联调，但尚不
  满足生产切流条件。

首个迁移基线为 `ploto-gateway` commit
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45`。后续若 Java 基线发生变化，应先更新
迁移文档中的基线、fixtures 和差异记录，再移植对应行为。

## 构建

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_access_server
cmake --build build --target fiber_access_server_tests
./build/apps-build/access-server/fiber_access_server_tests
```

产物位于：

```text
build/apps/access-server
```

## 运行

复制示例配置并至少修改 Nacos 地址：

```bash
cp apps/access-server/access-server.env.example access-server.env
./build/apps/access-server access-server.env
```

不传参数时默认读取当前目录的 `access-server.env`。`--help` 只打印命令行用法。

Java 兼容的进程默认值是：

- HTTP 监听 `0.0.0.0:16688`；
- Prometheus 监听 `0.0.0.0:16689`；
- HTTP worker 数在启动时根据进程 CPU affinity 和 cgroup v1/v2 CPU quota 自动确定；
- 默认 request body 上限 400 MiB；
- 项目列表 `ploto.unified-access.projects`，route 前缀
  `ploto.unified-access.route.`，group `ACCESS-SERVER`；
- gray data ID `ploto.unified-access.gray-match`；
- Naming/gray group `DEFAULT_GROUP`；service 路由缺省 cluster 固定为 `default`。
- 测试环境 Host cluster 模式默认关闭；仅在明确配置
  `ACCESS_SERVER_TEST_MODE=true` 时启用。

完整键和值示例见 [`access-server.env.example`](access-server.env.example)。配置文件采用
严格的 `KEY=VALUE` 行格式，空行和以 `#` 开头的注释会忽略；重复键和未知键会使进程
启动失败。`NACOS_SERVER_ADDRESSES` 当前要求逗号分隔的 IP literal。
CAT 默认关闭；任一 `CAT_*` 设置非空后必须给出完整 app key、hostname、IP，以及
至少一个 router 或 bootstrap collector。CAT 不可用会在启动阶段 fail closed，不会
静默退化为无 trace 的生产实例。

### 同步测试环境 Nacos 配置

`scripts/sync_test_nacos.py` 按 access-server 的实际订阅图导出项目列表、列表引用的
全部 route，以及 gray-match 配置，并可直接发布到测试 rnacos。工具在进程内禁用
HTTP/HTTPS 代理。源地址和凭据由环境变量或参数注入，不在仓库中提供默认值，且源地址、
token 和密码都不会写入 dump 或 manifest。输出目录必须为空，建议放在已忽略的
`temp/` 下：

```bash
ACCESS_SERVER_SOURCE_NACOS_URL='...' \
ACCESS_SERVER_SOURCE_NACOS_USERNAME='...' \
ACCESS_SERVER_SOURCE_NACOS_PASSWORD='...' \
python3 apps/access-server/scripts/sync_test_nacos.py \
  --destination-url http://127.0.0.1:18848/nacos \
  --output-dir temp/access-server-nacos-dump
```

同步完成后工具会逐项回读 rnacos 并比较原始内容；`manifest.json` 记录 dataId、group、
字节数和 SHA-256，但不记录 token 或密码。若项目列表引用了不存在的 route，该 dataId
保持 rnacos `NotFound` 状态，并记录在 `missingRoutes` 中。

listener 只在 Nacos client/config/naming、两类 watcher 和项目列表首值就绪后开放；
若项目列表不存在，服务会等待到
`ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS` 后失败退出。某个项目的 route 配置尚未
到达或不可用时不会开放对应 Host/Path，请求仍按现有稳定错误结果 fail closed。

## 目录

- `CMakeLists.txt`：应用目标和后续应用内静态库、测试的构建入口；
- `src/main.cpp`：进程配置、loop group、信号和有序关闭；
- `src/config/`：统一接入配置 wire model 和 Java 兼容 codec；
- `src/routing/`：compiled model、CIDR、Host/Path matcher 和全局不可变快照；
- `src/execution/`：live request handler、RESPONSE 计划/执行、PROXY 请求计划与执行器
  边界、模板适配边界和统一错误响应；
- `src/observability/`：请求 CAT 上下文、固定 schema Prometheus 指标和 logging
  category；
- `src/runtime/`：本地脚本 runtime、候选快照编译/原子发布、Nacos 配置 watcher、
  production gray、NamingService selector、per-worker DNS/pool、HTTP server 和进程
  runtime；
- `scripts/`：测试环境 Nacos 配置图的无代理导出、rnacos 发布和回读校验工具；
- `tests/`：access-server 聚焦测试和 Java golden fixtures；
- `docs/migration-plan.md`：范围边界、C++ 模块划分、工作包和阶段门槛；
- `docs/compatibility-contract.md`：配置字段、热更新和 HTTP 请求执行的 Java 契约；
- `docs/script-corpus-differential.md`：现网 condition/template/rewrite 的脱敏统计、
  Java golden、C++ 差分结果和私有 corpus 复跑方式。

业务代码开始迁移后，按职责放入 `src/config/`、`src/routing/`、`src/execution/`、
`src/runtime/` 和 `src/observability/`；对应测试放入 `tests/`，并在本目录的
`CMakeLists.txt` 中注册。

## 迁移原则

- Java 配置字段、默认值、宽松输入、Nacos data ID/group、路由优先级和错误结果
  属于外部兼容契约，未经明确决定不改名、不折叠；
- 配置更新先完整解析和校验，再以不可变快照发布；请求不能混用新旧配置；
- 热路径遵循本仓库的内存与异步约束，不按 Java 对象模型逐类机械翻译；
- 不把通用脚本语法、connection pool 算法或监控客户端内部行为纳入迁移验收；
- 每一阶段先增加聚焦测试，再接入下一层运行时依赖；
- 缺失或无效的控制面数据不得产生可用路由；切流前仍需完成阶段 7/8 的观测和差分门槛。

详细范围与阶段见 [`docs/migration-plan.md`](docs/migration-plan.md)，字段和请求契约见
[`docs/compatibility-contract.md`](docs/compatibility-contract.md)。
