# 上游需求：HttpExchange 请求头原地改写 API

> 交付对象：`fiber-gateway-cpp`（基于当前 access-gateway 所 pin 的 revision `270035b`）。
> 需求方：access-server（C++ 版 unified-access 数据面）。本文档自包含，不依赖 access-gateway 仓库上下文。

## 0. 决议（2026-09-30，已实施）

评审后**不采用**本文第 2 节的 `rewrite_request_header` API，改为在 `HttpExchange` 上开放
请求态的非 const 访问重载（`request_headers()` / `request_header_refs()` / `uri()`），
`accept_encoding` 同步加入 `RequestHeaderRefs`。原理与完整契约见
`feature/http-exchange-mutable-request-api.md` 与 `HttpExchange.h` 的注释块。与本文的差异：

- §2/§3：原地改 value、不动结构、只改第一个匹配 —— 由 `HttpHeaders::set()` 语义取代：
  新值 pool 拷贝、新字段链到表尾（遍历序变化）、重复同名字段折叠为一个、字段不存在则插入。
- §4：refs 一致性（`request_header_refs_.host` 等）从框架保证改为**调用方维护**：
  `set()` 返回新字段直接赋给对应 ref；`remove()` 后须置空或用 `get_all(lowcase, hash)` 重查。
- §6：验收 1（`host_header()` 指针一致性）相应改为调用方 set 后重指 ref 的行为断言
  （见 `tests/Http1ServerTest.cpp` 的 `MutableRequestHeadersSetRewritesHostAndRef` 等 5 个用例）。
- §7：access-server 接入代码变为：

```cpp
static constexpr std::uint64_t kHostHash = fiber::http::http_header_name_hash("host");
auto *host = exchange.request_headers().set("host", normalized_host, "host", kHostHash);
if (host == nullptr) { /* OOM → 500 */ }
exchange.request_header_refs().host = host;
```

以下原文保留作背景。

## 1. 背景与动机

Java 版网关（ploto-gateway）在 test 模式下支持「Host 带 `_cluster` 后缀」：路由名解析器
`ExtClusterNameFetcher` 解析 `api_gray.example.com` → 去后缀 host `api.example.com` + cluster `gray`，
然后调用 `exchange.setRequestHeader("host", 去后缀host)` 改写 exchange 上的请求头。此后请求生命周期内
**所有**读 request Host 的代码看到的都是去后缀值，包括：

- 脚本/模板/路由匹配条件里的 `$header.host`（Java fiber 的 `$header` 对象按 exchange 请求头快照构建）；
- 上游 3xx 的 Location / Refresh 重写（用 `exchange.getRequestHeader(HOST)` 拼外部地址）。

C++ 版 access-server 已实现同等的后缀解析、项目匹配与 cluster 透传，但**无法改写 exchange 的请求头**：

- `HttpExchange::request_headers()` 只返回 `const HttpHeaders &`，`request_headers_` 为 private，
  没有任何请求头变更 API（`include/fiber/http/HttpExchange.h`）；
- `$header.*` 的解析在 Fiber 内部（`src/http_script/ScriptExchangeCtx.cpp` 的 `headers()` 惰性遍历
  `exchange_.request_headers()` 构建 JS 对象），应用层没有按名字拦截/覆盖的点。

因此需要在 `HttpExchange` 上提供一个**请求头 value 原地改写** API。

## 2. API 需求

```cpp
// include/fiber/http/HttpExchange.h
public:
    // Rewrites the value of the first request header field matching (lowcase_name, name_hash),
    // in place. The new value is copied into pool storage. Never inserts, removes, or reorders
    // fields, so cached request_header_refs_ pointers stay valid. Returns false when no field
    // matches or the pool copy fails (original value is left untouched).
    bool rewrite_request_header(std::string_view lowcase_name, std::uint64_t name_hash,
                                std::string_view value) noexcept;

    // Convenience overload; hashes/normalizes internally. For tests and cold paths.
    bool rewrite_request_header(std::string_view name, std::string_view value) noexcept;
```

签名风格与现有热路径 API 一致（预哈希入参，参照 `HttpHeaders::get(lowcase_name, hash)` 与
`ScriptExchangeCtx` 里 `kCookieHash` 的用法）。

## 3. 语义细则（必须遵守）

1. **原地改 value，不动结构**。只更新匹配字段的 `value` / `value_len`；不得 append 新字段、不得
   remove / reorder。理由：`request_header_refs_.host` 等缓存的是 `HeaderField *`（见
   `src/http/HttpExchange.cpp` 的 `cache_request_header_field`），且哈希桶 / 双链表结构一旦变化，
   这些指针与迭代器全部失效。字段名、`lowcase_name`、`name_hash` 保持原样，因此 ref 缓存无需
   失效或更新。
2. **value 拷入 pool**。用现有 `HttpHeaders::copy_to_pool`（`src/http/HttpHeaders.cpp`）或等价机制把
   新值复制进 exchange 的 pool；**不得**保存调用方传入的 `string_view` 指针。理由：调用方的值通常
   来自协程帧上的局部 `std::string`，生命周期不可依赖；h2 侧 `ServerHttp2Request::commit_regular_header`
   对非稳定 value 也是 pool 拷贝的先例。
3. **只改第一个匹配字段**，与 `get()` / `header()` 的取值语义一致（内部可用
   `find_first_node_lowcase` 一类的现有查找）。
4. **失败语义**：字段不存在 → 返回 `false`，不做任何改动；pool 拷贝失败（OOM）→ 返回 `false`，
   原值保持可读。不得出现半改写状态。
5. **请求方向专用**：只作用于 `request_headers_`；不碰 `request_trailers_`，不碰响应方向。
6. **开销上限**：一次哈希查找 + 一次 value 的 pool 拷贝；不重新哈希、不重建桶、不分配新字段节点。

## 4. 边界条件与调用方契约

- **空 value**：允许改写为空值（与 `set()` 行为一致）。
- **重复字段**：只改第一个；第二个同名字段保持原值（h1 重复 Host 属解析异常路径，不在本 API 职责内）。
- **调用时机契约**（写入 API 的文档注释即可，不需要内部防御）：调用方必须在首次 `$header` 访问前
  调用。`ScriptExchangeCtx::headers()` 是惰性快照——JS 对象一旦构建，后续改写不会反映进去。
  实际调用点（handler 入口、任何脚本求值之前）天然满足。
- **协议统一**：h1 的 `Host`、h2 的 `:authority`、h3 的对应字段在解析期已物化为普通 `host` 字段
  （`src/http/ServerHttp2Request.cpp` 的 `handle_authority` → `commit_regular_header("host", ...)`），
  本 API 三协议天然通用，无需分协议处理。
- **informational / 100-continue 阶段**：不做限制，但调用方一般在 handler 入口调用一次。

## 5. 非目标

- 不提供请求头的**新增/删除**通用 API：那涉及 ref 缓存失效与结构变更，超出本需求；本需求只需要
  「已存在字段的 value 改写」。
- 不改 trailers、不改响应头。
- 不做大小写不敏感匹配的预哈希重载（便捷重载内部归一为 lowcase + hash 再查即可）。

## 6. 验收测试（上游）

1. 存在字段改写：置 `Host: a.example.com` → `rewrite_request_header("host", kHostHash, "b.example.com")`
   → `header("Host")` 返回 `b.example.com`，且 `host_header()->value_view()` 同步为新值
   （验证 `request_header_refs_.host` 指针一致性，未失效、未指向旧值）。
2. 字段不存在：返回 `false`，头表内容与遍历顺序不变。
3. **pool 拷贝验证**：value 指向调用方临时 buffer，改写后销毁 buffer，`header("Host")` 仍返回正确值。
4. 重复字段：两个同名头只改第一个，第二个原值不变。
5. 空 value 改写成功，`header()` 返回空。
6. `get_all` / `MatchRange` 遍历在改写后仍完整、稳定（结构与迭代器未受影响）。
7. （可选，集成层）h2 请求带 `:authority` + `_cluster` 后缀，handler 改写后脚本 `$header.host`
   取到去后缀值。

## 7. 消费方接入说明（access-server 侧，供上游评审了解使用方式）

gitlink 刻意 bump 后，access-server 侧接入（已评估，约 30–50 行 + 测试）：

- `resolve_request_host`（`native/access-server/src/execution/AccessRequestHandler.cpp`）改为接收
  `HttpExchange &`；test 模式提取到 `_cluster` 后缀时：**先**记录原始 Host 为 `origin_host`（改写前），
  **再**调用 `rewrite_request_header("host", kHostHash, normalized_host)`。
- 该函数是 handler 的第一条语句，先于一切脚本/模板/路由条件求值，满足第 4 节的时机契约。
- 改写后自动对齐 Java 的点：`$header.host`（模板/路由条件/script 路由）、Location/Refresh 重写、
  websocket 会话记录 host。出站方向不受影响：代理请求头本就过滤 `host`（出站 Host = 上游实例
  authority），`ploto-origin-host` 由独立的 `origin_host` 通道携带原始值。
- 生效范围仅 test 模式且 Host 带后缀；prod 模式行为零变化。

## 8. 交付物清单

- `include/fiber/http/HttpExchange.h`：两个重载声明 + 语义注释（含时机契约）。
- `src/http/HttpExchange.cpp`：实现（复用 `HttpHeaders` 的查找与 `copy_to_pool`）。
- 上游单测覆盖第 6 节 1–6（7 可选）。
