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
- JSON 语法错误、数值溢出或 custom codec 不接受的格式仍应拒绝。

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
- 负数字符串、带小数、其他单位和溢出：拒绝。

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
- 溢出或不匹配格式：拒绝。

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

测试环境使用 Host 扩展提取 cluster；production 使用内部 IP 灰度拦截器。两者不可
同时按普通 Host pattern 处理。

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

`allows` 中的每个值必须非空；Java build 会直接检查首字符，空字符串会导致配置构建
失败。

## 7. Path 与条件路由

- 使用 Path matcher 选择 route；
- 同一个 path 可安装多条有 condition 的 route；
- condition 必须是同步表达式；异步表达式配置拒绝；
- 依次判断条件，首个满足者执行；
- 节点已有无条件 route 后，再加入同节点 route 属于 dead-route conflict；
- condition route key 是 `path + "@" + CRC32(condition) 的十六进制形式`；
- 未找到 Path 返回 404、`URL_NOT_MATCHED`，message：
  `url not matched is project:<project>`。

本仓库已有 `RoutePathMatcher`，但必须通过 Java/C++ fixture 验证静态段、参数段、
wildcard、尾 slash、冲突和 condition 顺序后才能直接使用。

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
3. 任一 header 模板失败时，不提交任何配置 header，进入错误处理；
4. 设置 header；
5. 发送配置 status 和 body。

模板 body 的 Java value 转换：

- null/missing -> 空；
- scalar -> `asText()`；
- object/array -> 空。

这里仅记录 access 执行顺序和 value-to-body 规则，不承诺通用脚本语法兼容。

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

### 10.2 Method、URI 和 body

- 保留原 method；
- 无 rewrite 时保留原 URI；
- rewrite 结果为空时 path 变为 `/`；
- rewrite path 做 Java 等价 escape；
- 无论是否 rewrite，都保留原 query；
- downstream request body 流式转给 upstream；
- `flush` 传入 body 写入逻辑；
- WebSocket 仅在 timeout 大于 0 且 Upgrade/Connection 满足条件时启用。

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
- 普通 HTTP 的 Content-Length 走专用路径复制；
- WebSocket 专门转发 Upgrade/Connection；
- Host 由 upstream 请求构造逻辑决定，不透传 inbound Host。

### 10.4 Context

context 模板先计算后更新 trace user data：

- 空值移除 key；
- `cluster` 和 trace cluster key 归一到 cluster key；
- 该步骤的业务 key/value 结果属于排障契约；
- CAT 的底层 message 编码和线程行为不属于兼容范围。

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

## 12. 错误响应

错误处理先 discard request body。若 response 已经提交，则不再二次写入。

内容协商规则是 Java 遗留的前缀判断：

- Accept 为空，或不是从 offset 0 开始大小写不敏感的 `text/html`：JSON；
- Accept 从 offset 0 以 `text/html` 开始：HTML。

JSON 格式：

```json
{
  "name": "ENTRY_ERROR",
  "message": "entry error",
  "meta": null
}
```

status 不在 JSON body 中。已知业务错误使用其 code；未知错误使用 500。Java 未知错误
name 包含 Java class name，C++ 不应伪造 Java 类型名；迁移实现前为本模块定义稳定的
C++ unknown error name，并把它登记为有意差异。

稳定业务错误矩阵：

| 场景 | HTTP | name | message |
| --- | ---: | --- | --- |
| Host 未找到/非法 | 404 | `ROUTER_NOT_FOUND` | `error find router` |
| bad routing handler | 400 | `BAD_REQUEST` | `error find router` |
| Path 未匹配 | 404 | `URL_NOT_MATCHED` | `url not matched is project:<project>` |
| X-Entry 不允许 | 403 | `ENTRY_ERROR` | `entry error` |
| source IP 不允许 | 403 | `NOT_ALLOW_IP` | `source ip is not allowed` |

HTML 页的 status、name、message、trace ID、meta 和 Content-Type 要建立 golden fixture。
Java 页面当前对部分字段没有 HTML escaping；这是安全和兼容冲突点，必须单独形成差异
决定，不能在实现中静默改变。

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

## 14. 待从现网确认

实现阶段开始前应收集并脱敏：

- 当前项目列表 data ID 是否被覆盖；
- production 是否覆盖 `ploto.nacos.group`；
- 所有 Host pattern 和 HostStrategy 组合；
- RouteItem 字段使用频率及真实 Duration/DataSize 写法；
- 实际 condition/template/rewrite 脚本 corpus；
- static address 的 scheme/IPv6/权重格式；
- `allows` 中 IPv6、deny-only、空列表的使用情况；
- HTML 错误页是否有调用方依赖；
- Location/Refresh 和 WebSocket 的现网项目；
- Java 运行环境默认 charset 是否固定为 UTF-8。

上述数据只用于建立有限兼容 corpus，不扩大通用脚本或基础设施兼容范围。
