# Access Server 迁移计划

## 1. 文档目的

本文定义将 Java `ploto-gateway/ploto-unified-access` 迁移到
`fiber-gateway-cpp/apps/access-server` 的范围、工作分解、阶段门槛和完成标准。

首个 Java 行为基线：

```text
repository: /home/dear/CLionProjects/ploto-gateway
commit:     22c2bf543b96b52c0ccecd4ceb07d4911c502f45
```

具体字段、默认值、配置更新和请求响应规则见
[compatibility-contract.md](compatibility-contract.md)。实现前必须先把契约转成可执行
fixtures；不能仅凭 Java/C++ 类型名相同判定兼容。

## 2. 迁移目标

迁移只承诺两个外部边界：

1. **配置兼容**：现网 `ploto-unified-access` Nacos 配置可以被 C++ 服务读取，并按
   Java 的默认值、容错、校验和热更新规则生成同等路由快照。
2. **请求执行结果兼容**：给定相同配置、请求和可控上游，C++ 服务在路由选择、上游
   请求、响应映射和业务错误结果上与 Java 基线一致。

`unified-access-server` 的启动代码只作为部署参数来源，不是逐类移植目标。C++ 程序
按本仓库 C++23、无异常、EventLoop 和生命周期规范重新组织。

## 3. 范围边界

### 3.1 范围内

- 项目列表和逐项目路由配置的 Nacos data ID、group、内容格式与订阅生命周期；
- `ProjectConf`、`HostStrategy`、`RouteItem`、灰度配置的 JSON 输入行为；
- Host 校验、精确/通配符匹配、项目选择、HTTPS 和网络入口策略；
- Path 匹配、条件路由、路由冲突和同路径执行顺序；
- `RESPONSE` 与 `PROXY` 路由的请求/响应结果；
- header 过滤和模板覆盖、URI rewrite、query 保留、context 更新；
- service/cluster 和静态 addresses 两类上游配置；
- body 限制、超时、flush、WebSocket 和 CIDR allow/deny；
- JSON/HTML 错误响应的状态码、字段、选择规则和 body；
- 配置更新的版本去重、失败保旧、项目移除和原子发布。

### 3.2 复用但不做 Java 内部兼容验收

下列能力直接使用本仓库实现，不要求与 Java `fiber-net-gateway` 的内部算法、对象模型
或线程模型一致：

| 能力 | C++ 复用位置 | 本次验收边界 |
| --- | --- | --- |
| runtime、HTTP、路由基础设施 | `src/` 中的 `fiber_lib` | 只验收 access-server 最终请求结果 |
| JSON | `src/common/json/` | 验收统一接入配置的输入行为，不比较 Jackson 实现 |
| 脚本引擎 | `src/script/`、`src/http_script/` | 不做通用脚本语言/VM 兼容 |
| connection pool | `src/http/*ConnectionPool*` | 不比较池算法、连接选择和内部时序 |
| Nacos | `apps/nacos/` | 验收 access-server 的 data ID、group、更新结果 |
| CAT | `apps/cat/` | 不比较协议编码、线程和客户端内部实现 |
| Prometheus | `apps/prometheus/` | 不比较 Java 指标库内部实现 |

脚本字段仍然是路由配置的一部分，C++ 必须正确接收并交给本地引擎。通用脚本语法、
异步语义、对象模型和边界值兼容不属于迁移门槛；对于切流所需的实际配置，只建立有限
的配置样例和请求结果回归，不由此扩展为“兼容 Java 脚本引擎”的承诺。

### 3.3 明确不在范围内

- 逐类翻译 Java IoC、`GatewayModule`、Engine 或 shutdown hook；
- Java 和 C++ JSON/脚本/连接池/Nacos/CAT/Prometheus 的内部实现对齐；
- 连接 ID、线程调度、实例选择随机序列、日志顺序、毫秒级失败时点完全一致；
- Java 未使用的通用 gateway 功能；
- 未经配置契约要求的新管理 API、新配置格式或路由能力。

## 4. 实现原则

### 4.1 行为先于类结构

以配置和 HTTP 输入输出为迁移单位，不复制 Java 继承层次。每个工作包先建立 Java
oracle/golden fixture，再写 C++ 实现和差分测试。

### 4.2 配置采用两阶段处理

```text
Nacos bytes
  -> 兼容解码
  -> 语义校验与预编译
  -> 完整候选快照
  -> EventLoop 所有者原子发布
  -> 请求 pin 不可变快照
```

- “兼容解码”复现现有配置实际接受的未知字段、重复字段、默认值和标量转型。
- “语义校验”拒绝无法执行的 path、route type、status、上游和冲突组合。
- 任一步失败都保留旧快照；不能边解析边修改运行态。
- 同一请求只能观察一个快照版本。

### 4.3 C++23 和本仓库约束

- 不使用 C++ exception；预期失败通过 `std::expected` 或显式结果传递；
- callback 和析构/关闭路径按需要标记 `noexcept`；
- `main.cpp` 只负责进程配置、依赖装配、信号与有序关闭；
- 运行态状态归属明确的 EventLoop，跨线程只发布不可变数据或投递消息；
- 请求路径取时间使用 `fiber::event::EventLoop::current().now()`；
- 配置编译阶段允许一次性分配；请求热路径避免重复构造
  `std::string`、`std::vector`、`std::function`；
- 模板、CIDR、header 规则、路由条件尽量在快照发布前编译；
- 所需依赖在初始化边界建立并断言，稳态路径不反复检查 nullable 成员。

## 5. C++ 目标模块

Java 源码到工作包的直接映射：

| Java 源 | C++ 工作包 |
| --- | --- |
| `UAConstant`、`ProjectConf`、`HostStrategy`、`RouteItem` | `src/config/` wire model 与 compiled model |
| `AccessRouteConfigWatcher` | `src/runtime/` Nacos listener 与快照发布 |
| `HostMatchNameFetcher`、`WildHostNode` | `src/routing/` Host 校验与 matcher |
| `RouteExecutionBuilder`、`ConditionalExecution`、`Context` | `src/routing/` route compile/selection |
| `AbstractRouteExecution` | `src/execution/` 前置检查 |
| `ResponseRouteExecution` | `src/execution/` RESPONSE executor |
| `ProxyRouteExecution` | `src/execution/` PROXY executor |
| `PageErrorHandler` | `src/execution/` ErrorResponder |
| `InternalIpGrayInterceptor` | `src/routing/` production gray policy |
| `UnifiedAccessModule` | `src/runtime/` 依赖装配参考，不复制 Java IoC |

业务源码开始迁移后按下列职责落位：

| 目录/目标 | 职责 |
| --- | --- |
| `src/config/` | wire DTO、兼容 JSON codec、Duration/DataSize、语义校验 |
| `src/routing/` | Host matcher、项目表、Path/条件选择、不可变快照 |
| `src/execution/` | RESPONSE/PROXY 执行、header/rewrite、错误响应 |
| `src/runtime/` | listener、Nacos 订阅、配置发布、启动和关闭 |
| `src/observability/` | CAT/Prometheus/logging 的 access-server 适配层 |
| `tests/fixtures/java/` | Java 输入、归一化配置和 HTTP golden 数据 |
| `tests/` | codec、路由、执行、热更新和端到端差分测试 |

应用内可测试代码形成 `access_server_core` 静态库；最终可执行目标仍为
`fiber_app_access_server`，产物名 `access-server`。

优先复用：

- `fiber_lib`；
- `src/common/util/RoutePathMatcher.h`；
- `src/http/HttpProxyCore.h`、`src/http/HttpWebSocketProxy.*`；
- `src/http/Http1ConnectionPoolCore.*` 及本地 pool set；
- `src/common/json/`、`src/script/`、`src/http_script/`；
- `apps/nacos/`、`apps/cat/`、`apps/prometheus/`。

“优先复用”不表示直接认定行为等价。适配层负责把 access-server 契约翻译为本地 API，
差分测试只观察模块边界的配置和 HTTP 结果。

## 6. 分阶段工作包

### 阶段 0：脚手架与契约冻结

状态：完成。

工作：

- [x] 创建 `apps/access-server`、CMake 目标和 fail-closed 入口；
- [x] 固定首个 Java commit；
- [x] 明确只兼容 `ploto-unified-access` 的配置和请求执行结果；
- [x] 记录字段、默认值、更新语义和请求行为；
- [x] 从 Java 单元测试与运行配置生成第一批 fixtures；
- [x] 建立“已覆盖/未覆盖/有意差异”清单。

门槛：

- 文档范围没有把 Java 基础设施内部兼容带入迁移；
- 每个后续阶段都能指向明确的配置或 HTTP 观察点；
- 未实现二进制继续拒绝启动。

### 阶段 1：配置 wire model 与兼容解码

状态：进行中。项目配置和 gray wire decode、CIDR/address 语义及 compiled model
已落地；project/data ID 错误上下文仍待补齐。

工作：

- [x] 项目列表的分号解析及 data ID/group 常量；
- [x] `ProjectConf`、`HostStrategy`、`RouteItem`、Body wire DTO；
- [x] 灰度配置 wire DTO；
- [x] Java 默认值、unknown/duplicate/null/scalar coercion 行为；
- [x] Duration、DataSize 和 enum 解码；
- [x] CIDR 和 address 语义解析；
- [x] wire DTO 到不可变 compiled model 的显式转换；
- [ ] 配置错误补齐 project/data ID；当前已有字段路径和稳定错误类型。

聚焦测试：

- [x] 空配置、缺字段、未知字段、重复字段；
- [x] null、数值/字符串互转、大小写和遗留整数溢出；
- [x] Duration 的 ms/s 和 DataSize 的 K/M/G；
- [x] route type、status、upstream、path 与 route conflict；
- [ ] 扩大 Java 接受但 C++ 拒绝、以及 Java 拒绝但 C++ 接受的双向差分。

门槛：

- fixture 集的接受/拒绝结论与 Java 基线一致；
- 接受的配置生成相同归一化业务模型；
- 解析失败不产生可发布快照；
- 建立 `access_server_core` 和 access-server 聚焦测试目标。

### 阶段 2：项目、Host 与 Path 路由快照

状态：完成。Host/Path matcher、跨项目候选快照、失败保旧、version 去重、卸载、
请求 pin、HTTPS/net、production gray 和 live Nacos listener 已接入真实 HTTP runtime。

工作：

- [x] Host 去端口、IPv6 literal、末尾点和非法字符处理；
- [x] exact、`*`、`*.suffix` 和 Java wildcard 回退规则；
- [x] HostStrategy 的 HTTPS redirect、HSTS 和 X-Entry/net HTTP 执行；
- [x] 复用并验证 C++ `RoutePathMatcher`；
- [x] 同路径条件执行、无条件 dead-route 冲突和 route key；
- [x] 完整候选快照构建、发布和请求 pin。

聚焦测试：

- [x] Host 大小写、端口、尾点、IPv6、非法/缺失 Host；
- [x] exact 与多层 wildcard 组合和回退；
- [x] 静态 Path、变量、通配符、冲突、条件真/假和顺序；
- [x] 旧请求 pin 保持旧完整版本，新请求只看到一次发布后的完整版本；
- [x] EventLoop/listener 并发发布与 shutdown 竞态。

门槛：

- Java/C++ 选择相同 project、Host pattern、route 和 route key；
- 非法候选快照不替换有效快照；
- 请求热路径不解析 JSON、不编译模板、不构建 CIDR。

### 阶段 3：RESPONSE 路由与统一错误结果

状态：完成。RESPONSE compiled plan、模板适配边界、`HttpExchange` executor、统一
错误 renderer，以及 Host/Path/前置策略到 RESPONSE 的 live handler 已装配真实
listener；通用脚本兼容仍按范围决定排除。

工作：

- [x] TEXT、BASE64、TEMPLATE 和空 body；
- [x] status、response headers、request body discard；
- [x] JSON/HTML 错误选择及稳定错误对象；
- [x] URL 未匹配、Host 未匹配、bad route、entry、CIDR 等稳定错误映射；
- [x] 将 Host/Path/X-Entry/HTTPS/CIDR/body limit 失败分支接到 `ErrorResponder`；
- [x] 提供可交给 HTTP listener 的 `AccessRequestHandler`，串联 snapshot pin、route
  selection 与 `ResponseExecutor`；
- [x] 在 `main` 的 runtime 生命周期中创建并启动真实 listener。

聚焦测试：

- [x] prepared response status/header/body 字节级比较；
- [x] BASE64 配置成功/失败；
- [x] header 模板全部成功后一次性提交，失败时不写部分 header；
- [x] body 模板失败时保留已提交的完整配置 header 集；
- [x] Accept 为空、`text/html` 开头和其他值；
- [x] JSON/HTML 错误 body 与 Content-Type 精确比较；
- [x] live HTTP/1 exchange 的 response status/header/body；
- [x] HSTS、HTTPS redirect、X-Entry、Path、CIDR、413 和条件 path variables；
- [x] response 已提交后的错误不二次写响应。

门槛：

- access-server 生成的 status、契约 header 和 body 与 Java fixture 一致；
- 错误路径不会留下半写入响应；
- 本阶段不宣称本地脚本引擎具备通用 Java 兼容性。

### 阶段 4：PROXY 请求构造

状态：完成。上游选择输入和 Java 兼容请求计划已接入 live handler；
`ProxyRequestSender` 已使用本项目 HTTP/1 connection pool 完成静态地址/可注入 service
实例的连接、请求发送和响应头接收，并通过真实 loopback upstream 验证实际 wire 请求。
最终 adapter 已在等待 response header/body 和 WebSocket tunnel 期间统一监视
downstream close；NamingService/DNS runtime 适配留在阶段 6。

工作：

- [x] service/cluster 与静态 addresses 上游计划；
- [x] method、URI、query、rewrite、Host override 与 Content-Length 计划；
- [x] Java 固定 hop-by-hop 集合和用户覆盖 header 过滤；
- [x] proxy header、context 和最终 source header；
- [x] 请求 body framing、client/proxy body limit、timeout、flush 参数；
- [x] WebSocket upgrade 请求条件；
- [x] 将 `PreparedProxyRequest` 通过 callback 接入 `AccessRequestHandler`；
- [x] 使用本项目 connection pool 实现 production upstream request sender；
- [x] sender 中完成 Content-Length/chunked request body 流式转发和动态超限中止；
- [x] 连接失败时按 Java 上限在 request header 发送前重新选择 service 实例；
- [x] 完成等待 upstream 时的 downstream 断开竞速；最终 adapter 必须取消 sender 并关闭
  正在使用的连接。

聚焦测试：

- [x] 使用可控网络 echo upstream 对比实际收到的 method、URI、header、body；
- [x] 连续 chunked/Content-Length 请求复用同一 upstream TCP 连接；
- [x] 首个 service 实例连接失败后重新选择、失败/成功 report；
- [x] live downstream exchange 将请求 body 交给 capture executor；
- [x] rewrite 空值变 `/`，保留原 query；无 rewrite 保留 raw URI；
- [x] service 中 `/cluster` 与显式 cluster 覆盖；
- [x] header 大小写、空模板值、重复 header、Content-Length 和最终 source 覆盖；
- [x] timeout 下限、body limit 参数和 WebSocket 开关；
- [x] Java `UriCodec.escapeUri` byte mask。
- [x] downstream 在 response header 前关闭时取消 sender，并使 active upstream
  exchange 退出 connection pool 复用。

门槛：

- Java/C++ 对可控 upstream 产生相同的 access-layer 请求；
- pool 和 NamingService 内部选路差异不进入该比较；
- body 流和 upstream lease 所有权已覆盖正常完成、错误和 downstream 断开路径。

### 阶段 5：PROXY 响应与 WebSocket

状态：完成。普通 HTTP status/header/body bridge、响应模板、固定 hop-by-hop 过滤、
Content-Length/Location/Refresh/flush、response body limit、双方提前关闭，以及
WebSocket 101 双向 raw tunnel/timeout 均已完成。

工作：

- [x] upstream status/body/header 转发；
- [x] response header 模板和固定 hop-by-hop 过滤；
- [x] Content-Length 特殊恢复；
- [x] Location/Refresh 绝对地址回写；
- [x] `X-Accel-Buffering: no`；
- [x] 101 后 WebSocket 双向 raw tunnel；
- [x] WebSocket tunnel timeout 边界回归。

聚焦测试：

- [x] informational 1xx 后 final response、2xx/3xx/4xx/5xx，以及 HEAD/204/304 空 body；
- [x] Location/Refresh 命中和不命中 upstream host；
- [x] 用户覆盖 header、空模板值和模板失败；
- [x] 定长和分块 body、已知/动态 body limit；
- [x] downstream 提前关闭、upstream 提前关闭和 101 双向 tunnel；
- [x] tunnel timeout。

门槛：

- status、access-owned header、重写 header 和 body 一致；
- 错误发生前后响应所有权清晰，不重复完成 exchange；
- WebSocket 不复用普通 HTTP body 完成路径。

### 阶段 6：Nacos 生命周期与配置热更新

状态：完成。项目配置和 production gray 的订阅所有权、候选发布、NamingService
route 依赖协调、per-worker DNS/pool、HTTP listener、初始项目列表门槛与进程级
shutdown barrier 已完成。

工作：

- [x] 项目列表订阅和逐项目 listener 的增删；
- [x] 初始值、空值、相同 version、非法更新和项目移除；
- [x] route 与 Host 映射作为一个候选快照提交；
- [x] production gray config 订阅；
- [x] NamingService 注入上游执行器；
- [x] 本地 DNS resolver 注入上游执行器；
- [x] 启停、服务内部重连和关闭竞态。
- [x] NacosClient、ConfigService、NamingService 和 watcher 的 owner-loop 装配；
- [x] 每个 HTTP worker 的 DNS resolver、共享 DNS cache 与 local connection pool；
- [x] 项目列表首值到达前不绑定 listener，超时则失败退出；
- [x] SIGINT/SIGTERM 下 listener -> pool/DNS -> watcher/service -> client 逆序关闭。

聚焦测试：

- [x] list `a;b` -> `a;b;c` -> `b;c` 的订阅变化；
- [x] 空项目配置保持旧版本；
- [x] 相同 version 忽略；
- [x] build 失败保旧；
- [x] Host 为空卸载项目；
- [x] listener 回调与 shutdown 并发；
- [x] gray 空内容保旧、非法更新保旧、空 map 清空和 ratio/CIDR 边界；
- [x] NamingService healthy/enabled/weight、zone/cluster、hostname 和旧 generation pin。
- [x] 真实 loopback listener 返回已发布快照并完整关闭 worker 资源；
- [x] 真实 rnacos 的项目/route 首值、version 热更新和 SIGTERM 退出。

门槛：

- 同一配置事件在 Java/C++ 产生相同可用路由集合；
- config callback 不直接持有已销毁 runtime；
- shutdown 后不再发布新快照或恢复订阅。

### 阶段 7：入口策略与观测适配

状态：进行中。入口策略、本地脚本 adapter 和请求观测适配已接入；尚余现网脚本
corpus 差分。

工作：

- [x] production gray-match；
- [x] 测试环境 cluster Host；
- [x] X-Entry net mask、HTTPS redirect、HSTS；
- [x] X-Real-IP CIDR allow/deny；
- [x] 本地脚本 engine adapter：配置发布前预编译、请求同步执行、编译失败保旧；
- [ ] 现网 condition/template/rewrite 配置样例差分；
- [x] 将 project、route、cluster、upstream、错误和最终响应映射到统一请求观测上下文；
- [x] CAT 使用 `URL/<project><route>`、继续/生成三段 message ID、响应回传 trace ID；
- [x] Prometheus 使用独立 `main-port + 1` listener 和固定 result schema，避免动态
  cluster/header 产生无限时序；
- [x] 共享异步 logging 输出同一请求上下文，观测失败不改变 HTTP 结果。

门槛：

- 入口策略的 HTTP 结果通过差分；
- 观测能定位 project、route、上游和稳定错误类型；
- 不以 CAT/Prometheus 内部实现和编码等价作为门槛。

### 阶段 8：差分验收与切流

工作：

- [x] 提供按项目列表精确导出 project/route/gray 配置、禁用代理、发布到 rnacos 并
  逐项回读 SHA-256 的同步工具；
- [x] 使用测试环境完整配置图启动 C++ access-server，并验证实际 RESPONSE 路由和
  Prometheus 请求结果；
- Java 和 C++ 加载相同配置，使用同一套 request corpus；
- 可控 Nacos、HTTP/HTTPS echo upstream 和 WebSocket upstream；
- 配置热更、慢 body、连接断开、超时与 shutdown；
- 延迟、分配、连接复用、内存和 fd 稳定性基线；
- 部署、健康检查、灰度、观测和回滚说明。

门槛：

- [compatibility-contract.md](compatibility-contract.md) 中 P0/P1 用例全部通过；
- 未通过项均形成获批的差异决定，不以“底层实现不同”笼统豁免；
- 具备按 project/Host 灰度和立即回滚能力；
- 生产切流前完成最小线上演练和立即回滚验证。

## 7. 差分测试判定

同一 fixture 至少包含：

```text
config bytes
environment/test-mode inputs
request method/target/headers/body
controlled upstream behavior
expected selected project/route/upstream plan
expected upstream request
expected downstream status/headers/body
```

默认要求完全一致：

- 配置接受或拒绝结论；
- 归一化字段值、默认值和 route 冲突结论；
- project、Host pattern、route 和 cluster 选择；
- upstream method、URI、access-owned header 和 body；
- downstream status、access-owned/rewritten header 和 body 字节；
- 稳定业务错误名、message 和 meta。

不要求完全一致：

- Date、Server、连接复用细节、端口、trace ID 等运行时生成值；
- CAT message ID、Prometheus 抓取时点、日志时间戳和顺序；
- 多个等价健康实例之间的随机选择；
- 在同一配置超时边界内的毫秒级完成时间。

对动态值必须在 fixture 中明确忽略或归一化，禁止测试代码临时“忽略所有 header”
掩盖差异。

## 8. 风险与控制

| 风险 | 控制 |
| --- | --- |
| 把 Jackson 宽松输入误改成严格输入 | Java probe + codec golden fixtures |
| 本地 matcher 同名但边界不同 | Host/Path 组合差分，不依赖类型名 |
| 配置更新产生 route/Host 混合版本 | 单候选快照、一次发布、请求 pin |
| 模板执行中途写入部分 header | 先求值到临时结果，全部成功后提交 |
| body 模板失败丢失 Java 已提交 header | header 与 body 分阶段准备，错误响应显式继承已提交 header |
| connection pool 生命周期泄漏到 exchange | 执行器显式拥有/移交 body 与 tunnel |
| 观测接入扩大热路径分配 | 固定标签、预编译 route metadata |
| 以脚本兼容问题阻塞整个迁移 | 限定真实配置回归，通用 VM 兼容明确排除 |
| Java 基线继续变化 | 每次基线升级单独更新 commit、fixture 和差异清单 |

## 9. 每阶段工程验证

每阶段至少执行：

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_access_server
ctest --test-dir build
./format_code.sh
git diff --check
```

有 access-server 聚焦测试后，先执行聚焦目标，再执行全量 CTest。若共享构建目录有无关
失败，必须记录失败命令和原因，并在干净构建目录完成本应用验证。

## 10. 完成定义

迁移完成必须同时满足：

- P0/P1 配置和请求 corpus 差分通过；
- 热更新失败保旧、同版本去重和项目卸载行为通过；
- HTTP、WebSocket、body、超时和关闭路径没有悬空生命周期；
- C++ 实现遵循本仓库无异常、EventLoop 所有权和热路径分配要求；
- CAT/Prometheus/logging 足够支持灰度判断和问题定位；
- README、运行配置、部署、健康检查、灰度和回滚文档齐全；
- 所有有意差异均在本文“差异决定”登记并有测试。

## 11. 差异决定

当前已批准的范围差异：

| 项目 | 决定 |
| --- | --- |
| Java gateway/runtime 内部实现 | 复用 `fiber_lib`，不做内部兼容 |
| 通用脚本引擎兼容 | 不在本次迁移范围 |
| connection pool 内部行为 | 不做算法、随机性和内部时序兼容 |
| JSON 库内部实现 | 使用本地 JSON；仅复现 access 配置输入契约 |
| Nacos/CAT/Prometheus 内部实现 | 使用本仓库 apps；仅验证 access-server 边界结果 |
| 未知 C++ 异常的 error name | 固定为 `ACCESS_UNKNOWN_ERROR`，不伪造 Java class name |

除上述范围决定外，当前没有已批准的配置或请求结果差异。
