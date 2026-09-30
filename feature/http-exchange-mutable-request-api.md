# HttpExchange 请求态可变访问（非 const 重载）

> 2026-09-30 定稿并实施。对应上游需求与决议见
> `feature/upstream-rewrite-request-header-requirements.md` §0。
> 交付形态：`include/fiber/http/HttpExchange.h` 的三个非 const 重载 +
> `RequestHeaderRefs::accept_encoding`。

## 1. 背景与决策

下游（access-server 等）在 `HttpHandler` 内经常需要改写请求侧信息（Host 去后缀、
注入/覆盖业务头、改写 URI），而 `HttpExchange` 请求态此前全 const。上游最初要求的
`rewrite_request_header`（原地改 value、不动结构）经评审**不采用**，理由是它引入的
专用 API 与 `HttpHeaders` 既有原语（`set`/`remove`/`add`）语义重叠，且把「refs 一致性」
这一本应由调用方掌控的职责切成两套心智模型。最终形态：

```cpp
[[nodiscard]] HttpHeaders &request_headers() noexcept;          // 非 const 重载
[[nodiscard]] RequestHeaderRefs &request_header_refs() noexcept; // 非 const 重载
[[nodiscard]] HttpUri &uri() noexcept;                           // 非 const 重载
```

`request_trailers()` 刻意**不**加非 const 重载：解析器在读 body 期间仍在向其 append。

## 2. 为什么这是安全的（评估记录）

### 2.1 HeaderField 指针稳定性

`HeaderField` 节点从 exchange 的 `BufPool` 分配，只分配、请求结束统一回收，节点地址
从不移动、内容从不抹除。结构性操作（`remove`/`erase`/rehash）只把节点从桶/双链表
unlink，不 free。因此：

- 未被触动的字段指针永远有效；
- 被 remove 的字段（孤儿节点）指针**仍可读但读旧值**——这是唯一的陷阱，见 §3.3。

### 2.2 协议派生态在解析期冻结

框架在解析期把协议控制头蒸馏进独立状态，响应/成帧路径**只读这些状态、不再回头查表**：

| 派生态 | 来源 | 消费方 |
|---|---|---|
| `request_body_spec_` | content-length / transfer-encoding | H1 响应成帧（`Http1ExchangeIo.cpp`）、body 读写 |
| `request_close_` / `request_keep_alive_` | connection | 连接复用决策 |
| H2/H3 流帧 | HEADERS/DATA 帧边界 | 流层，与头表无耦合 |

因此调用方改头表**不会**反过来破坏成帧——只要不去改协议控制名（§3.1）。

### 2.3 修改只发生在 HttpHandler 阶段

`HttpHandler` 被调起时请求头已解析完毕、响应未开始，是天然的读写窗口。脚本层
`$header` 惰性快照的时序约束见 §3.5。

## 3. 调用方契约（完整版在 `HttpExchange.h` 注释块）

### 3.1 禁改协议控制头

content-length、transfer-encoding、connection、te、trailer、expect、upgrade、
sec-websocket-*：这些在解析期已被消费进 §2.2 的冻结状态，改表即与线上事实脱钩。
**安全**的名字：host、accept-encoding、cache-control、业务自定义头——gzip 协商在
首次响应写时才惰性读取。

### 3.2 只用拷贝语义的重载

`add` / `add_prehashed` / `set` 会把 name 与 value pool 拷贝；`add_view` / `set_view`
族保留外部指针，在 handler 协程帧生命周期下即悬空。**禁用 view 族。**

### 3.3 refs 自行维护（first-wins 缓存）

`request_header_refs_` 缓存每个名字**第一个**匹配字段（线序）。结构性变更后：

- `set()` 返回新字段 → 直接赋给对应 ref；注意 set 会 remove 所有同名旧字段并把新
  字段链到表尾，副作用是：重复同名字段折叠为一个、遍历顺序变化、字段不存在则插入
  （与上游原稿「原地改第一个、不动结构」不同，行为见 §4 测试锚定）；
- `remove()` 后旧 ref 变孤儿指针——仍可读但读旧值，须置空或用
  `get_all(lowcase, hash).begin()` 重查。

当前解析后 ref 消费方：host（访问日志）、expect（100-continue 限流器）、
accept_encoding（gzip 决策，存在性短路）。

### 3.4 uri() 的四视图耦合

`HttpUri` 是 4 个裸 `string_view`（path 解码值 / unparsed_uri 原始 / query / exten），
自身无存储。改写时：

- 只 reseat 到 exchange pool 存储（`pool().alloc` + memcpy），禁指向协程帧/临时串；
- 保持 path（解码）/ unparsed_uri（原始）同调：代理目标构建整体优先取 unparsed_uri，
  query 后缀也取自它（`HttpProxyCore.h` 的 `request_target_view`、ProxyHandler 的
  `raw_query_suffix`）；新 path 必须保持 origin-form；
- 路由匹配不会重跑。

### 3.5 时序

头表改动须在首次 `$header` 脚本访问前完成（`ScriptExchangeCtx` 一次性物化头对象）；
`$path` / `$query` / `$req.uri` 每次访问读活值，不受此限。

## 4. `set()` 行为锚定（与上游原稿差异）

上游原稿语义（原地改 value / 不动结构 / 只改第一个匹配）被 `HttpHeaders::set()`
既有语义取代，已在 h1 测试锚定：

| 行为 | 测试（`tests/Http1ServerTest.cpp`） |
|---|---|
| set 后 ref 重指、`header()`/`host_header()` 均新值 | `MutableRequestHeadersSetRewritesHostAndRef` |
| value pool 拷贝（源串改写后头值不变） | `MutableRequestHeadersSetCopiesValueIntoPool` |
| 重复同名折叠为 1、缺名则插入 | `MutableRequestHeadersSetInsertsAbsentAndCollapsesDuplicates` |
| remove 后孤儿 ref 读旧值、置空后归位 | `MutableRequestHeadersRemoveOrphansCachedRefUntilNulled` |
| uri reseat 到 pool 存储、`request_target_view` 跟随 | `MutableUriReseatUsesPoolStorage` |

## 5. accept_encoding ref 的定位（存在性缓存）

`RequestHeaderRefs` 增加 `accept_encoding`，三协议解析经
`cache_request_header_field` 漏斗填充（`src/http/HttpExchange.cpp`）。
**定位是存在性检查/缺席短路**，不是取值：gzip 协商（`accepts_gzip`）要折叠**所有**
accept-encoding 字段并解析 q 值，单个 first-match ref 无法承载。因此
`GzipResponseWriter` 的用法是：

```cpp
exchange_->accept_encoding_header() != nullptr && accepts_gzip(exchange_->request_headers())
```

头缺席 → 跳过整表扫描（省 get_all 遍历）；头在场 → 仍走完整协商。h3 侧该 ref 还
免掉了对 QPACK 静态存储依赖的转字符串（`Http3ConnectionTestSupport.h` 的
`capture_request` 现走 `accept_encoding_header()`，既有断言
`ServerRequestBorrowsQpackStaticStorage` 顺带覆盖该路径）。

## 6. 测试清单

- h1：`CachesImportantRequestHeaderPointer` 扩展（accept-encoding 进 refs）+ §4 五个新用例；
- h2：`CachesAcceptEncodingRequestHeaderRef`（手编 HPACK literal-without-indexing，
  名长 0x0f——"accept-encoding" 15 字节；fake transport 不锁存就绪通知，须等
  `State::Running` 再 feed）；
- h3：`ServerRequestBorrowsQpackStaticStorage`（经 capture_request 改道覆盖）；
- gzip：`GzipResponseWriterTest` 全量（存在性短路路径）。

全量回归 2497/2497 绿（基线 2491 + 新增 6）。

## 7. 明确不做 / 延后项

- `rewrite_request_header` 专用 API：否决（本文件全文即理由）。
- `refresh_request_header_refs()`（框架侧 ref 重缓存）：延后。当前 ref 消费方仅 3 个，
  手工两行（set 返回值赋 ref）成本低于引入新 API 与失效时机的论证成本。
- `set_request_uri()`（框架侧 uri 改写 + 存储 + 同调维护）：延后。唯一已知消费方
  （Host 后缀场景）不改 URI；等出现真实「改 path」需求再评估 unparsed_uri 原始性、
  exten 重推导、路由是否重跑的语义。
- `request_trailers()` 非 const：永久不做（解析器在读 body 期间仍 append）。
