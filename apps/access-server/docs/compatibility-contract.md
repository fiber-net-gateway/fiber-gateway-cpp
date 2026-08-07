# Ploto Unified Access 兼容契约

## 1. 契约和优先级

本文记录 Java `ploto-unified-access` 在首个基线 commit
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45` 下的外部行为。迁移验收优先级：

- **P0**：现网配置能加载、项目/Host/Path 选择、PROXY/RESPONSE 主路径、稳定错误结果；
- **P1**：热更新、header/rewrite、body limit、timeout、CIDR、HTTPS/net、WebSocket；
- **P2**：测试环境 cluster Host、灰度观测细节、罕见宽松输入和错误页遗留细节。

Java 源码位置：

```text
ploto-unified-access/src/main/java/com/haiercash/ploto/access/
  UAConstant.java
  UnifiedAccessModule.java
  core/
  route/
```

本文描述的是兼容目标，不规定 C++ 内部 API。脚本 VM、连接池、JSON 库和监控客户端
内部等价不属于本契约。

## 2. Nacos 配置拓扑

| 用途 | data ID | group |
| --- | --- | --- |
| 项目列表 | `ploto.unified-access.projects` | `ACCESS-SERVER` |
| 项目路由 | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` |
| production 灰度 | `ploto.unified-access.gray-match` | `ploto.nacos.group`，默认 `DEFAULT_GROUP` |

项目列表 data ID 可由 Java property `ploto.access.projectsKey` 覆盖。C++ 应提供等价
部署入口，但不要求保留 Java property 机制本身。灰度 group 来自 Java
`PlotoConstants.NACOS_GROUP`，可由 property `ploto.nacos.group` 覆盖。

项目列表不是 JSON，而是分号分隔字符串：

```text
project-a;project-b;project-c
```

Java 先 `trim()` 整体字符串，再按 `;` 分割。空内容产生空项目集合。迁移前应从
生产数据确认是否存在空项、重复项或项目名两侧空格，并将现状固化为 fixture。

项目列表变化时：

1. 为新增项目订阅 `ploto.unified-access.route.<project>`；
2. 为删除项目移除 listener，并从有效路由集合卸载项目；
3. 保留未变化项目的当前快照；
4. listener 增删和全局快照发布必须在明确的 runtime/EventLoop 所有者上串行化。

当前 C++ `AccessConfigWatcher` 已按该拓扑实现 owner-loop 订阅图。每个 listener
使用独立停止信号，关闭时先停止项目列表和全部逐项目任务，再等待 barrier 后销毁
subscription；已发布请求快照保持不可变，可由 worker 继续 pin。

## 3. 项目配置 wire model

### 3.1 ProjectConf

```json
{
  "version": 1,
  "host": {
    "api.example.com": {
      "https": "S_NOT_MUST",
      "net": "S_VDI,S_OFFICE"
    }
  },
  "routes": []
}
```

字段：

| 字段 | 类型/默认值 | 说明 |
| --- | --- | --- |
| `version` | int，缺失为 `0` | 相同 version 更新会被忽略 |
| `host` | object 或 null | Host pattern 到 `HostStrategy` |
| `routes` | array 或 null | 项目 Path 路由 |

配置更新规则：

- 项目 listener 收到空字符串时不修改当前配置；
- 新配置 version 与当前 version 相同时忽略；
- 必须先完整解析并构建 route matcher/Host 映射，再发布；
- 解析、编译或构建失败时继续使用旧配置；
- `host` 缺失或为空表示卸载该项目的 Host/route；
- route 与 Host 映射必须作为同一不可变候选发布。

当 `host` 非空而 `routes` 为 null 时，Java 在构建 route matcher 时失败；不能把 null
静默归一为空 route 集合后发布一个可用项目。

### 3.2 Java JSON 输入行为

Java 使用的 Jackson 配置会产生以下行为，C++ 的 wire codec 必须用 fixtures 锁定：

- 未知 object 字段忽略；
- 同名字段重复时后值覆盖前值；
- 未知 enum 解码为 null，随后是否拒绝由业务构建阶段决定；
- 缺失或 null 的 primitive int/bool 使用 Java primitive 默认值；
- 部分数字字符串可转数字，数字可转字符串，字符串可转 bool；
- `Map<String, String>` 的数值 value 会转成字符串；
- JSON 语法错误、目标 primitive 范围溢出或 custom codec 不接受的格式通常拒绝；
  custom codec 内部的 Java 整数运算溢出按下节遗留行为处理。

这不是要求本地 JSON 库模拟 Jackson。实现应在 access-server 的 wire DTO/codec
边界显式完成必要转型，随后转换为类型严格的 compiled model。

首批 probe 已确认：

| 输入 | Java 结果 |
| --- | --- |
| `{}` | version `0`，host/routes 为 null |
| unknown field | 忽略 |
| duplicate `version` | 使用最后一个值 |
| unknown route `type` | wire decode 成功且 type 为 null，route build 拒绝 |
| `"version":"3"` | 转成 int `3` |
| route `"path":7` | 转成 string `"7"` |
| route `"status":"201"` | 转成 int `201` |
| route `"flush":"true"` | 转成 bool `true` |
| response header value `5` | 转成 string `"5"` |

以上宽松行为只应用于本模块配置入口，不能扩散到仓库公共 JSON 默认策略。

## 4. 字段 codec

### 4.1 Duration

- JSON integer：毫秒；
- string：大小写不敏感地匹配 `(\d+)(s|ms)?`；
- 无后缀或 `ms`：毫秒；
- `s`：秒；
- 纯空白 string：null；
- 负数字符串、带小数、其他单位，以及数字部分超过 Java int：拒绝；
- `s` 会在 Java int 上先乘 1000 再传给 `Duration.ofMillis`，所以乘法按 32 位
  two's-complement wrap；例如 `"2147484s"` 得到 `-2147483296` ms。

示例：

| JSON | 归一化 |
| --- | --- |
| `500` | 500 ms |
| `"500"` | 500 ms |
| `"500ms"` | 500 ms |
| `"5S"` | 5 s |
| `"   "` | null |

### 4.2 DataSize

- JSON integer：Java signed long 值；包括 `0` 和负数；
- string：大小写不敏感地匹配 `(\d+)([kmg])?`；
- K/M/G 按二进制 `<< 10/20/30`；
- string 解析后的值必须大于 0；
- 数字部分超过 Java long 或格式不匹配：拒绝；
- K/M/G 左移使用 Java long wrap，最终结果大于 0 才接受。

因此 JSON number `0` 可解码，而 JSON string `"0"` 会被拒绝。语义层是否接受非正值
由具体字段的 Java 使用方式决定，不能提前用统一规则改写。

## 5. HostStrategy 与项目选择

### 5.1 HostStrategy

```json
{
  "https": "S_301",
  "net": "S_VDI,S_OFFICE,S_INTERNET,S_CUSTOM"
}
```

`https`：

- `S_NOT_MUST`：默认，不重定向；
- `S_301`、`S_302`、`S_307`、`S_308`：当
  `X-Forwarded-Proto` 不是大小写不敏感的 `https` 时返回对应重定向。

重定向：

```text
Location: https://<original Host><original URI>
Strict-Transport-Security: max-age=31536000
body: empty
```

`ProjectRouteHandler` 会在任何已经选中项目的请求上先添加
`Strict-Transport-Security: max-age=31536000`，并不只对 HTTPS redirect 添加。这个
header 属于 access-owned 响应契约。

该 header 在 Host 匹配成功后、X-Entry/HTTPS/Path/CIDR 检查前加入，因此这些错误响应
也携带 HSTS；Host 未匹配的 `ROUTER_NOT_FOUND` 不携带。路由配置中的同名
`response_headers` 随后使用 set 语义，可以覆盖该值。

`net` 是逗号分隔 enum 名称，Java 映射：

| enum | X-Entry name |
| --- | --- |
| `S_VDI` | `vdi` |
| `S_OFFICE` | `desktop` |
| `S_INTERNET` | `internet` |
| `S_CUSTOM` | `custom` |

X-Entry 与 HostStrategy net mask 不匹配时返回 403 `ENTRY_ERROR`。

### 5.2 Host 规范化和匹配

- Host 大小写不敏感；
- 匹配前去除端口；
- bracket IPv6 literal 要正确保留地址并去除其后的端口；
- 去除一个末尾 `.`；
- 空 Host、连续点、slash、控制字符等非法值不进入 matcher；
- pattern 支持 exact、`*` 和 `*.suffix`；
- `*.suffix` 可覆盖多级前缀；
- exact 分支存在但其更深匹配失败时，Java 的 ancestor wildcard 回退有特殊限制，必须
  用组合 fixtures 复现，不能改成普通“最长后缀总是回退”。

非法/未匹配 Host 返回：

- 404；
- error name `ROUTER_NOT_FOUND`；
- message `error find router`。

bad routing 特殊入口返回 400、`BAD_REQUEST`、`error find router`。

测试环境使用 Host 扩展提取 cluster：对原 Host 查找第一个 `_`，再查找其后的第一个
`.`；两者都存在且 `_` 不在首位时，删除 `_<cluster>` 后再做项目 Host 匹配。例如
`api_gray.example.com` 按 `api.example.com` 匹配，`gray` 写入请求 cluster，并向上游
增加 `ploto-origin-host: api_gray.example.com`。未提取到 Host cluster 时，
`HI-TRACE-CLUSTER` 请求头作为 cluster。production 使用内部 IP 灰度规则，两种模式由
`ACCESS_SERVER_TEST_MODE` 显式区分。

## 6. RouteItem

```json
{
  "path": "/v1/example",
  "type": "PROXY",
  "service": "example-service/gray",
  "cluster": "stable",
  "addresses": ["127.0.0.1:8080"],
  "condition": "expression",
  "proxy_headers": {"X-Test": "template"},
  "response_headers": {"X-Result": "template"},
  "context": {"cluster": "template"},
  "rewrite": "/internal/example",
  "status": 200,
  "body": {"type": "TEXT", "content": "ok"},
  "timeout": "60s",
  "max_client_body_size": "10m",
  "max_proxy_body_size": "20m",
  "websocket_timeout": "300s",
  "flush": false,
  "allows": ["10.0.0.0/8", "!10.1.0.0/16"]
}
```

字段/default：

| 字段 | 默认/规则 |
| --- | --- |
| `path` | 必须非空 |
| `type` | `PROXY`；另支持 `RESPONSE` |
| `service` | 与 addresses 二选一的上游来源 |
| `cluster` | 显式值覆盖 service 中 `/cluster` suffix |
| `addresses` | service 为空时使用；两者都空则 build 失败 |
| `condition` | 同一路径的可选同步条件 |
| `proxy_headers` | upstream request header 模板 |
| `response_headers` | downstream response header 模板 |
| `context` | CAT/trace context 模板 |
| `rewrite` | upstream path 模板 |
| `status` | primitive int，缺失为 0 |
| `body` | RESPONSE body |
| `timeout` | PROXY 默认 60000 ms；配置值必须不少于 5 ms |
| `max_client_body_size` | 当前 route 的 client body limit |
| `max_proxy_body_size` | 非零时应用，并 clamp 到不小于 0 |
| `websocket_timeout` | 大于 0 才开启 WebSocket 代理 |
| `flush` | body flush；响应额外写 `X-Accel-Buffering: no` |
| `allows` | CIDR allow/deny |

Body：

| type | 规则 |
| --- | --- |
| `TEXT` | content 必须非空；按当前 Java 运行环境的 UTF-8 字节固化 fixture |
| `BASE64` | Java basic Base64 decoder；非法输入 build 失败 |
| `TEMPLATE` | 由模板执行器生成 |

RESPONSE 的 status 必须满足 `100 <= status < 1000`。body 缺失表示空 body。

body size 的 Java 执行边界：

- `max_client_body_size` 缺失或数值 `0`：使用 server 全局 request body 检查；
- `max_client_body_size` 为非零负值：clamp 为 `0` 后设置 route limit；
- `max_proxy_body_size` 缺失或数值 `0`：不覆盖 client 默认 response body limit；
- `max_proxy_body_size` 为非零负值：clamp 为 `0` 后设置 upstream response limit。

Java server 默认 request body limit 为 4 MiB。命中限制返回 413：

```json
{"name":"REQ_BODY_TOO_LARGE","message":"request body is too large","meta":null}
```

C++ handler 将默认值作为启动选项，并允许部署时覆盖；已知 Content-Length 在 CIDR
检查前拒绝，chunked/stream body 在读取时累计检查。显式负值按 Java 的 clamp-to-zero
路径解释为 unlimited。

`allows` 中的每个值必须非空；Java build 会直接检查首字符，空字符串会导致配置构建
失败。

## 7. Path 与条件路由

- 使用 Path matcher 选择 route；
- 同一个 path 可安装多条有 condition 的 route；
- condition 必须是同步表达式；异步表达式配置拒绝；
- 依次判断条件，首个满足者执行；
- 节点已有无条件 route 后，再加入同节点 route 属于 dead-route conflict；placeholder
  名称不同会形成不同节点，Java 不把这种 sibling 遮蔽判为该冲突；
- condition route key 是 `path + "@" + CRC32C(condition)`，CRC 的 8 个十六进制
  nibble 按低位到高位依次输出；
- 未找到 Path 返回 404、`URL_NOT_MATCHED`，message：
  `url not matched is project:<project>`。

本仓库 `RoutePathMatcher` 已通过聚焦用例验证静态段、参数段、wildcard、冲突和
condition 顺序，并用于 compiled snapshot。条件表达式在候选快照发布前由本地脚本
adapter 编译为同步程序，请求热路径只执行已编译程序。通用脚本语法兼容仍不属于本次
迁移门槛，但现网已观察到的有限语法集合必须通过
[script-corpus-differential.md](script-corpus-differential.md) 的 corpus 差分。

## 8. 请求前置策略

### 8.1 CIDR allows

- `!` 开头为 deny CIDR，其他为 allow CIDR；
- Java 从 `X-Real-Ip` 取来源地址；
- header 缺失或无法解析时，Java 当前会跳过 allow/deny 检查；
- 被拒绝返回 403、`NOT_ALLOW_IP`、`source ip is not allowed`；
- Java 对带端口地址的截取方式偏向 IPv4，IPv6 和多值 header 必须作为显式 fixture，
  在没有差异决定前不得“顺手修复”。

### 8.2 production gray-match

灰度配置是 X-Entry name 到以下对象的 JSON map：

```json
{
  "vdi": {
    "ratio": 1000,
    "cidrs": ["10.0.0.0/8"]
  }
}
```

对于匹配的 X-Entry：

1. `X-Real-Ip` 命中 CIDR whitelist，或
2. 随机数 `nextInt(10000) < ratio`

则选择 gray cluster，并写相应业务 trace/tag。随机序列本身不做兼容；ratio 的区间
判断、CIDR 优先和最终 cluster 结果属于契约。

灰度配置编译还保留以下 Java 规则：

- 无法映射到 HostStrategy X-Entry name 的项忽略；
- ratio 小于 0 的项忽略；
- ratio 为 0 且 CIDR 为空的项忽略；
- 非法 CIDR 被过滤，其他有效项继续生效；
- ratio 没有 clamp 到 10000，超过 10000 时随机分支总能命中；
- 空灰度配置内容保持当前配置，非空但解析为空 map 则清空当前规则。

## 9. RESPONSE 执行

顺序：

1. 丢弃请求 body；
2. 先计算全部 response header 模板；
3. 计算 body 模板；
4. 任一 header/body 模板或 header 校验失败时，不提交任何 route header/body，进入错误处理；
5. 全部准备成功后，将 route header 写入请求级最终 header 集，随后发送配置 status 和 body。

Java 会在全部 header 模板成功后先提交配置 header，再计算 body，因此 body 模板失败时
错误响应会继承这些 header。C++ 有意采用原子准备语义：body 模板失败时丢弃全部 route
header，只保留项目匹配后已经确定的 access-owned header，以及错误响应自身的
`Content-Type`/trace header。这是已登记的兼容差异，用来避免未成功 route 的 header
污染错误响应。

`setResponseHeader` 会忽略以下大小写不敏感的名称，`Host` 不在忽略集合：

```text
Connection
Content-Length
Proxy-Connection
Keep-Alive
Proxy-Authenticate
Proxy-Authorization
TE
Trailer
Transfer-Encoding
Upgrade
```

模板 body 的 Java value 转换：

- null/missing -> 空；
- scalar -> `asText()`；
- object/array -> 空。

模板字面量支持 `\\`、`\$`、`\{`、`\}` 转义，`${expression}` 交给脚本引擎。
C++ compiled plan 保留该分段/提交语义，并在候选配置发布前将每个 expression 预编译
为不可变本地程序；condition 使用同一编译边界。编译失败不会替换当前快照，请求阶段
只做同步执行；这里不承诺通用脚本语法兼容。现网使用的 `$context.hi_trace_cluster`
按 Java 规则执行 ASCII 大小写折叠和 `-/_` 归一化，可读取运行时
`HI-TRACE-CLUSTER`。

Java 的 `discardReqBody()` 只触发忽略后续请求 body；本仓库现有
`HttpExchange::discard_body()` 会异步读完 body 后再继续 RESPONSE 执行。两者最终
HTTP 结果相同，但慢请求 body 的响应开始时点可能不同；毫秒级内部完成时序不属于本
契约，阶段 8 仍需覆盖慢 body 和下游断开生命周期。

## 10. PROXY 请求

### 10.1 上游选择

- 非空 `service` 优先；
- service 可写为 `service/cluster`；
- 非空显式 `cluster` 覆盖 service suffix；
- service 为空时使用 `addresses`；
- 两类上游都为空则配置 build 失败；
- call source 为 `<project>.unifiedAccess`；
- upstream request 写 `x-ploto-source-app`。

命名服务选择的具体健康实例、pool slot 和连接复用时机不是契约；选择输入
service/cluster/addresses 是契约。

静态 address 在配置编译期按 Java `HttpHost.create` 归一化：

- 未写 scheme 时，显式端口 443 选择 HTTPS，其他情况选择 HTTP；
- 写了 scheme 时，仅大小写不敏感的 `https` 选择 HTTPS，其他 scheme 按 HTTP；
- 端口缺失或小于等于 0 时使用 scheme 默认端口；
- Host header 省略默认端口，保留非默认端口；
- 数字 IP 在编译期保存，hostname 通过 runtime DNS adapter 解析；
- 无法解析为 Java int 或最终端口超过 65535 时配置 build 失败。

### 10.2 Method、URI 和 body

- 保留原 method；
- 无 rewrite 时保留 raw URI，包括原始 percent-encoding；
- rewrite 结果为空时 path 变为 `/`；
- rewrite path 使用 `fiber-net-gateway` `UriCodec.escapeUri` 的 Nginx byte mask：`/`
  保留，空格、`#`、`%`、`?` 以及对应控制/非 ASCII UTF-8 byte 使用大写 `%XX`；
- rewrite 后拼回原 query，不对 query 做二次 escape；
- 普通请求存在 Content-Length 时保留其 framing；缺失时 Java 因始终安装 streaming
  body function，由 client 自动使用 chunked framing；
- downstream request body 流式转给 upstream；
- `flush` 传入 body 写入逻辑；
- WebSocket 仅在 timeout 大于 0，且 `Upgrade` 大小写不敏感等于 `websocket`、
  `Connection` 大小写不敏感精确等于 `upgrade` 时启用；逗号 token 列表不命中这一
  Java 遗留判断。

Java `requestTimeout` 在请求 body 全部发送后启动，到收到 response header 为止；连接
建立使用 Java client 默认的 3000 ms connect timeout。C++ executor 保留这个边界：
connect 不占 route timeout，请求 body 写完后才把 route timeout 用于 response header
读取；负的遗留 int32 timeout 表示不启动该 timer。

连接/解析在 request header 尚未发送时失败，可以重新选择 service 实例，最多包含首选
实例在内尝试 4 次；选回同一失败实例时停止。request header 已开始发送后不重试，避免
重复提交非幂等请求。实例收到 `>=500` response header、upstream 写入/读取失败或
response body 未完整结束时 report 失败；`<500` 普通响应在 body 完整结束后 report
成功，WebSocket 在 101 成功切换 raw stream 后 report 成功。downstream body 错误、
本地 body limit、响应构造失败和 downstream 写入失败不归因给 upstream，不提交 report。

Nacos 实例沿用 Java `HttpHost` 的协议推导：端口 443 使用 HTTPS，其他端口使用 HTTP；
Host header 对 80/443 省略默认端口。没有配置/健康实例时返回 503
`UPSTREAM_NO_HOSTS`，实例均处于熔断状态时返回 503 `UPSTREAM_CIRCUIT_BREAK`；这两类
选择失败发生在连接池和 socket 建连之前，不得映射为 `HTTP_CLIENT_CONNECT_ERROR`。

### 10.3 Request header

下列 hop-by-hop/header 不直接复制，名称比较大小写不敏感：

```text
Connection
Content-Length
Proxy-Connection
Keep-Alive
Proxy-Authenticate
Proxy-Authorization
TE
Trailer
Transfer-Encoding
Upgrade
Host
```

- custom `proxy_headers` 覆盖的同名 inbound header 也先过滤；
- custom 模板结果为空时不写该 header；
- custom header 通过 Java client 的安全 setter：上面除 Host 外的固定 hop-by-hop
  名称即使显式配置也不会写入；Host 可以显式覆盖 upstream 默认 Host；
- header 被配置后，无论模板最终为空或该名称被安全 setter 忽略，同名 inbound
  header 都不会再复制；
- 普通 HTTP 的 Content-Length 走专用路径复制；
- WebSocket 专门转发 Upgrade/Connection；
- Host 由 upstream 请求构造逻辑决定，不透传 inbound Host；
- 只过滤上述固定集合，不额外解析 `Connection` 的动态 token；
- `x-ploto-source-app` 在最后强制 set 为 `<project>.unifiedAccess`，覆盖配置值和
  inbound 值。

### 10.4 Context

context 模板先计算后更新 trace user data：

- 入站 `tracestate` 的非 `bnrc` member 保存在请求级 telemetry，`bnrc` 使用 Java GMP
  Base62 解码为 trace context；被当前项目脚本引用的 key 在 route 匹配前绑定到
  `$context`；
- 空值移除 key；
- `cluster` 和 trace cluster key 归一到 cluster key；
- 非空 cluster context 覆盖 route/service 中的默认 cluster；空值移除覆盖；
- upstream 发送前保留非 `bnrc` member，并从更新后的完整 trace context 重建 `bnrc`；
- 该步骤的业务 key/value 结果属于排障契约；
- CAT 的底层 message 编码和线程行为不属于兼容范围。

当前 C++ live handler 将 pinned `CompiledProxyRoute` 和轻量 `ProxyExecutionInput` 直接交给
`ProxyExecutor`，不再固化跨层的 prepared request。executor 先计算 selector 所需 context，
选择并 pin service/static endpoint，再构造实际 `Http1RequestHead`；随后通过独立的
`ProxyUpstreamConnection` 以 `Http1ConnectionGroupKey` 查询 local pool，仅在 miss 且 key
为 hostname 时调用异步 DNS adapter，并依次尝试全部地址。连接失败重选时只更新由 endpoint
派生的 Host，不重复计算 request 模板；CAT attempt header 在连接成功后、发送 request header
前注入。executor coroutine 保持 pool lease、选中 generation 和栈上的
`ClientHttp1Exchange`，直到消费或放弃 upstream response。真实 loopback upstream 已覆盖
chunked、Content-Length、默认/配置 Host、source header、body 字节、service 连接失败重选
和连接复用。

service/cluster/address 视图只在当前 pinned snapshot 生命周期内有效。C++ 已实现
NamingService route 依赖协调和原子服务目录：仅接受 enabled、healthy、正 weight
实例，按 Java cluster 名的 `zone-cluster` 结构选择本 zone，并让请求 owner pin 选中
的 discovery generation；hostname 实例交给本地 DNS adapter。该选择/pool 算法本身
不属于兼容边界。

production gray 原子快照在 selector 前覆盖 cluster，非空 context cluster 次之，
最后才使用 route 默认 cluster。`ProxyExecutor` 在选择/连接 upstream、转发 request/response
以及 tunnel 期间监视 downstream response channel；downstream 关闭会销毁未完成的 proxy
coroutine，并使 active upstream exchange 退出 pool 复用。RESPONSE、redirect 和错误响应
直接等待 downstream IO，由相应读写操作返回 channel 关闭错误。上述组件已在
`AccessServerRuntime` 完成进程级装配：每个 request worker 使用自己的 DNS resolver
和 local pool shard，项目列表首值到达前不绑定 listener，关闭时先停止 listener 和
active exchange，再关闭 pool/DNS 和 Nacos 控制面。本地脚本 adapter 和测试环境
Host cluster 已装配。

请求观测使用一个贯穿 handler、RESPONSE 和 PROXY 的上下文：

- CAT 根事务类型为 `URL`，命中后名称为 `<project><route-pattern>`；
- CAT 根事务只表达 access-server 是否完整执行：完整转发的 upstream 4xx/5xx 仍为成功，
  路由、代理调用或 downstream 写入导致功能未完成时为 `ERROR`；HTTP status 仅作为 data，
  根事务不再写 `result`；
- 入站三段 CAT ID 被继续，无有效上下文时生成新 tree；响应写回 `Hi-Trace-Id`，
  upstream 写入新的 `HI-TRACE-ID`、`HI-SPAN-ID-PARENT`、`HI-SPAN-ID`；
- 入站 `traceparent` 原样传播；缺失时生成 `00-<32hex>-<16hex>-01`。该 sampled flag
  当前只属于 W3C header 契约，不覆盖 CAT router 的独立采样决策；
- `tracestate` 的解码、route context 更新和 upstream 重建独立于 CAT client 可用性；
  `MessageTrace context` 是同步的观测镜像，不是传播状态的唯一 owner；
- 每次真实 upstream attempt 失败由对应 `Access.Provider` 记录 `CALL_ERROR`；最终错误返回
  handler 时使用 `Err::UpstreamException`，只标记根事务失败，不再重复记录
  `FiberException`。路由、模板、无可用地址和熔断等本地失败记录 `FiberException`；
- 原始错误之后若错误响应本身写入失败，独立记录 `ResponseError`，不覆盖也不重复原始
  `CALL_ERROR`/`FiberException`；
- project、route、context cluster、实际 upstream、稳定 `Exception.name` 和最终
  response completion 同时进入 CAT 与 `access_server.access`；
- Prometheus 在独立 listener 输出固定 `result` 标签的请求总数、inflight 和 duration。
  project/route/cluster 属于动态控制面或请求输入，不建立无限增长的 label series；
- CAT、指标或日志记录失败均为 best effort，不得改写 Java 兼容 HTTP 结果。

上述接入只约束 access-server 可见的 trace/header、维度和值，不比较 CAT 编码、
Prometheus 内部快照时点或日志行顺序。现网脚本 corpus 与阶段 8 全量差分仍未完成，
因此当前阶段仍不进入生产切流。

## 11. PROXY 响应

顺序：

1. 计算全部 custom response header；
2. 失败时 discard upstream body 并进入错误处理；
3. 过滤 upstream hop-by-hop 和将被 custom 覆盖的 header；
4. 写非空 custom response header；
5. 处理 Location/Refresh、Content-Length 和 flush；
6. 保留 upstream status 并转发 body；
7. 101 时切换 WebSocket tunnel。

响应规则：

- upstream status 原样返回；
- upstream Content-Length 走专用恢复路径；
- custom response header 空值表示不写；
- `flush=true` 写 `X-Accel-Buffering: no`；
- Location/Refresh 为 absolute URL 且 host 等于实际 upstream host 时，改写为：
  - scheme：inbound `X-Forwarded-Proto`，缺失时 `http`；
  - host：inbound Host；
  - path/query/fragment：保留原 location 内容；
- 用户显式覆盖 Location/Refresh 时不执行自动改写；
- WebSocket 101 后按 `websocket_timeout` 双向 tunnel。

Location/Refresh 的 host 比较、端口和大小写要使用 Java 原实现的 golden fixtures；
不能直接替换成 RFC 意义上的规范化 URL equality，因为 Java 当前是字符串区段比较。

当前 C++ 普通响应 bridge 遵循上述顺序，只过滤 Java 固定 hop-by-hop 集合，不解析
`Connection` 的动态 token。已知 Content-Length 超限会在提交 upstream status 前返回
500 `READ_RESP_BODY`；chunked/动态响应在提交后超限或 upstream 提前结束时，只中止
当前 response channel，不再写第二份错误响应。基本 WebSocket 101 已复用本仓库 raw
tunnel，保留 upstream `Sec-WebSocket-Accept` 和非 hop-by-hop header，并在握手响应中
重新建立 `Connection: Upgrade`/`Upgrade`。`websocket_timeout` 已接到 raw tunnel
读写超时，并由真实 loopback 空闲 tunnel 回归覆盖。

## 12. 错误响应

RESPONSE、PROXY、脚本和路由中间层只返回 `Result<T>`：`Err::Exception` 携带由 handler
记录 `FiberException` 的可渲染业务错误，`Err::UpstreamException` 携带已经由 proxy
记录 `CALL_ERROR` 的 HTTP client 错误，`Err::Error` 保留底层 IO/内存错误。只有请求 handler 最外层调用
`ErrorResponder`：response header 尚未提交且 channel 可用时，Exception 按原错误发送，
Error 统一映射为 `ACCESS_UNKNOWN_ERROR` 500；header 已提交或 channel 已关闭时不再写入
第二份响应，只终止 exchange。

`ErrorResponder` 只渲染和发送，不决定 CAT 错误事件类型；它不读取或 discard request body，
而是立即发送可用的错误响应。handler
返回后的未读请求体清理由 HTTP connection/stream 层负责；因此已知 Content-Length
超限等前置错误不会等待慢速请求体上传完成。RESPONSE 路由正常执行前仍会按其业务语义
读取并丢弃请求 body，这与错误响应职责无关。

最终 downstream response header 由请求级 `AccessRequestTelemetry::response_headers()`
持有：Host 匹配成功后立即写入 HSTS；route/proxy 仅在准备成功后写入自己的 header；
错误响应写入 Content-Type；trace header 在每次最终发送前最后覆盖。header 集一旦开始
提交，后续失败只能作为 `Err::Error` 处理。

内容协商规则是 Java 遗留的前缀判断：

- Accept 为空，或不是从 offset 0 开始大小写不敏感的 `text/html`：JSON；
- Accept 从 offset 0 以 `text/html` 开始：HTML。

JSON 格式：

```json
{"name":"ENTRY_ERROR","message":"entry error","meta":null}
```

status 不在 JSON body 中。已知业务错误使用其 code；未知错误使用 500。Java 未知错误
name 包含 Java class name，C++ 不伪造 Java 类型名，固定使用
`ACCESS_UNKNOWN_ERROR`；这是已登记的有意差异。JSON Content-Type 固定为
`application/json; charset=utf-8`。

稳定业务错误矩阵：

| 场景 | HTTP | name | message |
| --- | ---: | --- | --- |
| Host 未找到/非法 | 404 | `ROUTER_NOT_FOUND` | `error find router` |
| bad routing handler | 400 | `BAD_REQUEST` | `error find router` |
| Path 未匹配 | 404 | `URL_NOT_MATCHED` | `url not matched is project:<project>` |
| X-Entry 不允许 | 403 | `ENTRY_ERROR` | `entry error` |
| source IP 不允许 | 403 | `NOT_ALLOW_IP` | `source ip is not allowed` |
| request body 超限 | 413 | `REQ_BODY_TOO_LARGE` | `request body is too large` |
| upstream 无可用实例 | 503 | `UPSTREAM_NO_HOSTS` | `no available service instance` |
| upstream 实例均熔断 | 503 | `UPSTREAM_CIRCUIT_BREAK` | `service upstream circuit breaker is open` |

HTML Content-Type 为 `text/html`。当前实现按 Java 页面精确拼接 status、name、
message、trace ID 和 null meta；Java 对 message 没有 HTML escaping，C++ 为保持当前
请求结果兼容也保留这一行为，并以 golden test 锁定。后续若要修复，必须作为显式的
安全/兼容变更处理。

## 13. 差分用例清单

### P0

- 最小 ProjectConf、完整 ProjectConf、未知/重复字段；
- project list 新增/删除；
- exact/wildcard Host；
- static/parameter/wildcard Path；
- RESPONSE TEXT/BASE64/empty；
- PROXY service/cluster 和 static address；
- request method/URI/query/header/body；
- upstream status/header/body；
- Host/Path/entry/CIDR 错误 JSON；
- 非法配置失败保旧。

### P1

- Duration/DataSize 全边界；
- condition route 与 dead-route conflict；
- rewrite、proxy/response/context headers；
- Content-Length、hop-by-hop headers；
- Location/Refresh；
- max client/proxy body；
- timeout、flush、downstream disconnect；
- HTTPS redirect/HSTS/net mask；
- WebSocket 101/tunnel/timeout；
- same version、empty project config、empty Host unload。

### P2

- IPv6/带端口/末尾点/非法 Host；
- ancestor wildcard 特殊回退；
- X-Real-IP 缺失、非法、IPv6、多值；
- test-mode cluster Host；
- production gray ratio/CIDR；
- HTML Accept 前缀和遗留 escaping；
- Jackson 罕见 scalar coercion。

## 14. 现网确认状态

condition/template/rewrite 已完成脱敏统计、Java golden 和 C++ 请求级差分，结果见
[script-corpus-differential.md](script-corpus-differential.md)。其余仍需收集并脱敏：

- 当前项目列表 data ID 是否被覆盖；
- production 是否覆盖 `ploto.nacos.group`；
- 所有 Host pattern 和 HostStrategy 组合；
- RouteItem 字段使用频率及真实 Duration/DataSize 写法；
- static address 的 scheme/IPv6/权重格式；
- `allows` 中 IPv6、deny-only、空列表的使用情况；
- HTML 错误页是否有调用方依赖；
- Location/Refresh 和 WebSocket 的现网项目；
- Java 运行环境默认 charset 是否固定为 UTF-8。

上述数据只用于建立有限兼容 corpus，不扩大通用脚本或基础设施兼容范围。
