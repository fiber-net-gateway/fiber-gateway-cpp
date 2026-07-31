# Access Server 迁移计划

## 基线与范围

本次迁移以相邻仓库 `/home/dear/CLionProjects/ploto-gateway` 的
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45` 为首个行为基线。

Java 工程分为两层：

- `unified-access-server` 是进程启动模块，负责系统属性、Engine 组装、Nacos 启动
  注册和 shutdown hook；
- `ploto-unified-access` 承载实际业务，包括动态配置、Host/Path 路由、代理与固定
  响应、条件表达式、灰度策略、错误页和监控接入。

因此迁移范围不能只覆盖 `unified-access-server/src/main/.../Startup.java`。除非本文
明确记录有意差异，`ploto-unified-access` 的外部可观察行为同样属于兼容范围。

## 已确认的 Java 契约

### 进程启动

`Startup.java` 当前设置并装配：

- 应用名 `unified-access-server`；
- `ploto.sd.circuitBroken=false`；
- HTTP server 全局最大 body 为 400 MiB；
- HTTP client 全局最大 body 为 600 MiB；
- `GatewayModule`、`UnifiedAccessModule` 和 Nacos 启动监听器；
- 进程退出时通知 Engine 停止。

这些值先作为待逐项验证的兼容契约记录。C++ 版本是否保留同名环境配置、改成严格的
应用配置文件，或把全局上限收窄为路由级限制，需要在实现配置层时形成显式决定和
测试，不能由脚手架阶段猜测。

### Nacos 动态配置

`UAConstant` 定义的现有标识为：

| 用途 | Java 值 |
| --- | --- |
| 项目列表 data ID | `ploto.unified-access.projects` |
| 项目路由 data ID 前缀 | `ploto.unified-access.route.` |
| 项目配置 group | `ACCESS-SERVER` |
| 项目列表覆盖属性 | `ploto.access.projectsKey` |

每个项目的配置由 data ID 前缀拼接项目名得到。配置更新以 `version` 判断重复版本，
成功构建新的 Host 映射和 Path matcher 后才替换运行态；空 Host 会卸载对应项目。

生产环境还订阅 `ploto.unified-access.gray-match` 灰度配置。其 group 来自 Java
`PlotoConstants.NACOS_GROUP`，迁移该功能时必须继续向 `ploto-common` 追踪实际值，
不能假设它与 `ACCESS-SERVER` 相同。

### 项目和路由模型

项目配置 `ProjectConf` 的顶层字段是：

- `version`；
- `host`：Host pattern 到 HTTPS/网络策略；
- `routes`：项目内的 Path 路由数组。

`RouteItem` 当前支持：

- `path`，以及默认 `PROXY` 的 `type`；
- Nacos `service`/`cluster` 或静态 `addresses` 上游；
- `condition` 条件表达式；
- `proxy_headers`、`response_headers` 和 `context` 模板；
- `rewrite`；
- 固定响应的 `status` 与 `body`，body 类型为 `TEXT`、`BASE64` 或 `TEMPLATE`；
- `timeout`、`websocket_timeout` 和 `flush`；
- `max_client_body_size`、`max_proxy_body_size`；
- `allows` CIDR 白名单。

Duration 和 DataSize 的 Java 解析/序列化规则必须从现有 codec 和
`RouteItemTest` 提取成 golden fixtures，再实现 C++ codec。字段名和大小写枚举不能
在迁移过程中顺手更改。

### 请求路由与执行

当前入口先校验并匹配 `Host`，再选择项目路由，未找到项目与非法 Host 分别进入既有
错误处理路径。测试环境还支持从 Host 中提取 cluster 的扩展形式；非测试环境接入
内部 IP 灰度拦截器。

项目内部使用 `RoutePathMatcher`，并允许同一路径挂载条件执行。执行类型至少包括：

- 代理到服务发现实例或静态地址；
- 返回固定或模板响应；
- 请求前 CIDR/body 限制；
- 请求/响应 header 模板和上下文；
- Path 改写、WebSocket、超时及 flush 行为。

迁移时应先记录 Java matcher 的冲突、变量捕获和条件优先级测试，再判断本仓库现有
`RoutePathMatcher` 能否直接承载，不能仅凭类型同名视为行为等价。

## C++ 目标结构

计划按职责形成以下边界；目录在出现首个实际实现时创建：

| 目录 | 职责 |
| --- | --- |
| `src/config/` | 严格 DTO/JSON codec、Duration/DataSize、配置校验 |
| `src/routing/` | Host matcher、项目快照、Path/条件选择 |
| `src/proxy/` | 上游计划、header/rewrite、HTTP/WebSocket 执行 |
| `src/runtime/` | EventLoop、listener、Nacos 生命周期、启动与关闭 |
| `src/observability/` | CAT、Prometheus、日志和兼容错误观测 |
| `tests/` | codec、matcher、快照、执行与端到端兼容测试 |

`src/main.cpp` 最终只负责读取进程配置、初始化依赖、启动 runtime、处理信号和有序
关闭。可独立测试的业务实现放入 `access_server_core` 静态库；该库在首批业务源码
加入时创建，避免现在为尚未确定的接口制造空抽象。

## 分阶段实施

### 0. 构建脚手架

- [x] 建立 `apps/access-server`；
- [x] 注册 `fiber_app_access_server` 和 `access-server` 产物名；
- [x] 记录 Java 基线、迁移边界和验收顺序；
- [x] 保持未实现程序 fail closed。

### 1. 配置契约

- [ ] 固化项目列表、`ProjectConf`、`RouteItem` 和灰度配置 fixtures；
- [ ] 实现严格 JSON、Duration、DataSize、CIDR 与模板字段解析；
- [ ] 覆盖缺字段、未知字段、重复字段、溢出和非法组合；
- [ ] 建立 `access_server_core` 与聚焦单元测试。

### 2. 路由快照

- [ ] 对齐 Host 校验、通配符匹配及端口/IPv6 literal 行为；
- [ ] 对齐 Path 变量、通配符、冲突与条件执行顺序；
- [ ] 构建完整项目快照后原子发布；
- [ ] 验证请求始终 pin 住同一版本快照。

### 3. 本地执行语义

- [ ] 实现固定/模板响应、错误结果和 header/context 模板；
- [ ] 实现 CIDR、body size、rewrite、timeout 和 flush 语义；
- [ ] 用 Java fixtures 做逐字段和逐状态码对照测试。

### 4. 上游与动态配置

- [ ] 接入 `fiber::nacos` 配置订阅和 NamingService；
- [ ] 实现 service/cluster 与静态 address 上游；
- [ ] 实现 HTTP、HTTPS、连接复用和 WebSocket 代理；
- [ ] 覆盖配置首值、更新失败、服务实例变化和关闭竞态。

### 5. 入口策略与可观测性

- [ ] 对齐 cluster Host、内部 IP 灰度和 HTTPS/网络策略；
- [ ] 对齐 CAT trace/tag/error 形态；
- [ ] 对齐 Prometheus 路由、指标名与 labels；
- [ ] 明确日志配置和敏感字段边界。

### 6. 端到端切换

- [ ] 使用相同 Nacos 配置对 Java/C++ 发起差分请求；
- [ ] 覆盖成功、错误、流式 body、WebSocket、配置热更和 shutdown；
- [ ] 压测并检查延迟、分配、连接复用与内存稳定性；
- [ ] 完成运行配置、部署和回滚文档后才移除 fail-closed 状态。

## 每阶段验收

每阶段至少执行：

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_access_server
ctest --test-dir build
./format_code.sh
git diff --check
```

若只增加 access-server 聚焦测试，先运行其测试目标，再运行全量 CTest。任何有意偏离
Java 的行为都需要在本文件追加“差异决定”，写明旧行为、新行为、理由和覆盖测试。

## 差异决定

当前没有已批准的行为差异。
