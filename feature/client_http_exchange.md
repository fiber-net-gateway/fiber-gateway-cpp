# ClientHttpExchange 统一客户端出口 —— 评估与实现方案

> 决策已定并已实现（2026-09-06）。落地情况见第 3 节。
>
> 决策：
> 1. **彻底统一 head 类型**：`ClientRequestHead` / `ClientResponseHead` 取代 6 个协议专有 head，
>    h1 编码时用 `authority` **覆盖** `Host`。
> 2. **union + switch** 分发，不用虚接口。
> 3. **删掉 `FIBER_ENABLE_HTTP3`** 这个死选项。

---

## 0. 结论

可行，且第 1 条决策把方案变简单了不少：**head 类型统一之后，`ClientHttpExchange` 的每个方法都退化成
纯转发函数，一个 coroutine frame 都不多加**，只剩一次可内联的 `switch`。

- Server 端已有同构先例：`HttpExchange`（具体前端） + `HttpExchangeIo`（虚接口）。客户端做镜像，
  但因为协议集合封闭，用 union 代替虚表。
- "TLS 握手后按 ALPN 择路"的真正阻塞点在**连接层把 dial 和 ALPN 校验焊死了**
  （`Http1ClientConnection::connect_impl` 硬校验 `http/1.1`，`Http2ClientConnection::connect_impl` 硬校验 `h2`），
  所以 `adopt(transport)` 是地基。
- 最大的坑在池层：h1 池和 h2 池控制流是**反的**。方案用 h2 池现有的 `Connector` 回调绕过去，
  **两个池一行都不改**。

分 4 层，A/B 是纯重构（现有 ctest 即验收），C/D 是新增。

---

## 1. 现状评估

### 1.1 三个 client exchange 的对齐度

| 能力 | ClientHttp1Exchange | ClientHttp2Exchange | ClientHttp3Exchange |
|---|---|---|---|
| 发头 | `send_header(Http1RequestHead, end_stream, t)` | `send_request_header(Http2RequestHead, ...)` | `send_request_header(Http3RequestHead, ...)` |
| 发体 | `write_all` / `write`（chain + raw 两组重载） | 同左，签名一致 | 同左，签名一致 |
| 发 trailer | `send_trailer(HttpHeaders, t)` | `write_trailer(...)` | `write_trailer(...)` |
| 读头 | `-> const Http1ResponseHead*` | `-> const Http2ResponseHead*` | `-> const Http3ResponseHead*` |
| 读体 | `read_body(max_bytes, t) -> IoBufChain` | 同左 | 同左 |
| 中止 | `abort(reason)` | `abort` / `cancel` | `abort` / `cancel` |
| 可移动 | **否**（NonCopyable + NonMovable） | 是（持 `Http2Stream::Lease`） | 是（持 `QuicStream::Lease`） |

### 1.2 `read_header` 流结束语义（已核实，无需再拍板）

| | 没有更多头块时的返回 | 位置 |
|---|---|---|
| h2 | **成功 + `nullptr`** | `src/http/detail/Http2HeaderBlockQueue.cpp:205` |
| h3 | **成功 + `nullptr`** | `src/http/ClientHttp3Request.cpp:1103` |
| h1 | **无限重复返回同一个 final head** | `ClientHttp1Exchange::read_header` 开头的 `final_response_received_` 分支 |

h2/h3 已经一致，**统一层直接采用"成功 + nullptr"，h1 对齐**。这不只是为了统一——h1 现在的行为是个真实的坑：
不知情的调用方写 `while (auto *h = co_await read_header())` 会死循环。

### 1.3 连接层：两份 90% 重复的 dial

`Http1ClientConnection::connect_impl`（`src/http/Http1ClientConnection.cpp:190`）与
`Http2ClientConnection::connect_impl`（`src/http/Http2ClientConnection.cpp:52`）都在做：

```
TCP connect → (h2 额外 getsockname) → AcceptResult → Tcp/TlsTransport::create
           → handshake(make_httpN_client_tls_param) → 校验 negotiated_alpn → 落地
```

差异只有三点：h2 多一次 `getsockname`；h1 支持 Happy Eyeballs 多地址而 h2 只支持单地址；ALPN 集合不同。

### 1.4 池层：控制流是反的（本方案最大的取舍点）

| | Http1ConnectionPoolCore | Http2ConnectionPoolCore |
|---|---|---|
| acquire | `acquire(key)` **同步**返回 Lease | `acquire(key, connector, timeout)` **协程** |
| miss 时谁 dial | **调用方**：`emplace_connection()` 后自己 `connect()` | **池**：回调 `Connector::connect(ctx, conn, key)` |
| Lease 语义 | 一个租约 = 整条连接（串行） | 一个租约 = 一个 stream 名额（并发） |
| 复用快路 | `acquire` 本身即同步复用 | `try_acquire(key)`（同步、不 dial、不排队） |

好消息：h2 池的 `try_acquire` + `Connector` 这两个现成件正好够统一层用。

### 1.5 其它已核对的事实

- `HttpTransport` 已是干净的抽象基类，`TcpTransport`/`TlsTransport` 都提供 `negotiated_alpn()`。
- `Http2Connection::start(std::unique_ptr<HttpTransport>)` 已存在且公开，h2 的 adopt 几乎白送。
- `make_http_server_tls_param` 已用 `{h2, http/1.1}` 做服务端 ALPN；客户端只有单协议版，需补协商版。
- `HttpConnectionGroupHintTable` 是**空闲连接计数**的近似过滤器，**不是**协议提示，不能直接复用。
- `ClientHttp1Exchange::response_trailers()` 全仓**只有 4 处调用，全在 `tests/ClientHttp1ExchangeTest.cpp`**
  —— 这让 h1 的 trailer 改造几乎零风险。
- `FIBER_ENABLE_HTTP3` 是死选项（`CMakeLists.txt:11` 声明后全仓无引用，`GLOB_RECURSE src/*.cpp` 无条件全编）。
  `feature/quic_audit.md:559` 早就标注过，README 也已注明"该值不影响源码列表"。

---

## 2. 分层设计

### Layer A —— `HttpClientDialer`（提取重复的 dial）

新增 `include/fiber/http/HttpClientDialer.h` + `src/http/HttpClientDialer.cpp`。

```cpp
namespace fiber::http {

enum class HttpProtocol : std::uint8_t { Http1, Http2, Http3 };

struct HttpClientDialRequest {
    // peer 有值走单地址；否则 race addresses（空集视为失败，不回退）
    std::optional<net::SocketAddress> peer{};
    std::span<const net::SocketAddress> addresses{};
    net::HappyEyeballsOptions happy{};
    net::TcpSocketOptions tcp = net::kNoDelayTcpSocketOptions;
    const HttpClientTlsOptions *tls = nullptr;   // null = 明文
    std::span<const std::string_view> alpn{};    // 空 = 不设 ALPN
    bool need_local_addr = false;                // h2 需要 getsockname
};

struct HttpClientDialResult {
    std::unique_ptr<HttpTransport> transport;
    net::SocketAddress peer{};
    std::optional<net::SocketAddress> local{};
    HttpProtocol protocol = HttpProtocol::Http1; // 由 negotiated_alpn 推出；空 ALPN => Http1
};

[[nodiscard]] async::Task<common::IoResult<HttpClientDialResult>>
http_client_dial(event::EventLoop &loop, HttpClientDialRequest request) noexcept;

} // namespace fiber::http
```

- ALPN → `HttpProtocol` 走常量表；出现不在请求集合里的协议返回 `IoErr::NotSupported`。
- `TlsAlpn.h` 补 `make_negotiating_client_tls_param(options, alpn_span)`，与现有两个 `make_http{1,2}_client_tls_param` 并列。
- 副产品：h2 顺手拿到 Happy Eyeballs 能力（今天只有 h1 有）。

### Layer B —— `adopt(transport)`

```cpp
// Http1ClientConnection
[[nodiscard]] common::IoErr adopt(std::unique_ptr<HttpTransport> transport,
                                  net::SocketAddress peer) noexcept;

// Http2ClientConnection
[[nodiscard]] common::IoErr adopt(std::unique_ptr<HttpTransport> transport,
                                  std::optional<net::SocketAddress> local = std::nullopt) noexcept;
```

约定（写进头注释）：

- 必须在 `loop()` 线程调用；状态必须是 `State::Init`（h2 是 `Http2Connection::State::Init`），否则 `Busy`。
- `transport` 必须 `valid()` 且 `transport->loop()` 与本连接同一个 loop，否则 `Invalid`。
- **ALPN 校验保留在 adopt 内**：h1 接受 空 / `"http/1.1"`；h2 接受 `"h2"` / 空（明文 prior-knowledge）。
- **失败时 adopt 负责 `close()` 并释放 transport**，调用方不再持有。理由：`Http2Connection::start()`
  本就会消费 transport，两条路径统一成"失败即吞掉"最不容易出错。
- h1 成功路径 = 现在 `connect_impl` 的尾巴；h2 成功路径 = 记 `local_addr_` + `conn_.start(...)`。

改造后两个 `connect_impl` 都退化成 `http_client_dial(...)` + `adopt(...)`。**行为不变，现有 ctest 全绿即验收。**

### Layer C-1 —— head 类型彻底统一（本轮决策）

新增 `include/fiber/http/ClientHttpTypes.h`，**取代** `ClientHttp{1,2,3}Types.h` 里的 6 个 head：

```cpp
namespace fiber::http {

struct ClientRequestHead {
    HttpMethod method = HttpMethod::Unknown;
    // h1: request-target 原样（origin / absolute / authority / asterisk form）
    // h2/h3: :path
    std::string_view path{};
    // h2/h3: :scheme。h1 忽略（scheme 由 transport 决定）
    std::string_view scheme{};
    // h2/h3: :authority
    // h1: 非空时编码为 `Host:`，并【覆盖】headers 里的任何 host 字段；为空时沿用 headers 自带的 Host
    std::string_view authority{};
    // extended CONNECT 的 :protocol。h1 非空时返回 NotSupported
    std::string_view protocol{};
    const HttpHeaders *headers = nullptr;
    // h1: 决定定帧（Content-Length / chunked / raw stream）
    // h2/h3: Chunked 直接判 Invalid（h2/h3 禁止 Transfer-Encoding）；其余仅作声明，
    //        实际定帧仍来自 end_stream + 调用方自己写的 content-length 头
    HttpBodySpec body = HttpBodySpec::None();
};

struct ClientResponseHead {
    explicit ClientResponseHead(mem::BufPool &pool,
                                HttpVersion v = HttpVersion::HTTP_1_1) noexcept
        : version(v), headers(pool) {}

    HttpVersion version = HttpVersion::HTTP_1_1;
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    bool end_stream = false;      // h2/h3 精确；h1 仅在发头时可证明无 body 才置 true
    std::string_view reason{};    // 仅 h1；h2/h3 恒空
    HttpHeaders headers;

    [[nodiscard]] bool is_informational() const noexcept {
        return kind == OutgoingHeaderKind::Informational;
    }
};

} // namespace fiber::http
```

**三个后端各自的填充责任**

| 字段 | h1 | h2 | h3 |
|---|---|---|---|
| `version` | 解析器写入 | ctor 传 `HTTP_2_0` | ctor 传 `HTTP_3_0` |
| `kind` | 1xx → `Informational`，否则 `Final`；trailer 节点 → `Trailer` | 现状透传 | 现状透传 |
| `status_code` / `headers` | 现状 | 现状 | 现状 |
| `reason` | 解析器写入 | 恒空 | 恒空 |
| `end_stream` | 仅在发头时可证明无 body 才 true（HEAD 响应 / 204 / 304 / `Content-Length: 0` 且非 upgrade）；否则 false，靠 `read_body` 的 `complete()` 判定 | 现状 | 现状 |

`is_informational()` 从"看 status 区间"改成"看 kind"，语义等价但更强（trailer 节点 status_code 为 0，
老实现会误判为非 informational，新实现直接是 `Trailer`）。

**尺寸影响**：`ClientRequestHead` 96 字节（h1 原 48、h2 原 80），`ClientResponseHead` 比 h2/h3 原 head
多 24 字节。每请求各一个、构造一次，可忽略。

**受影响的内部存储**（都是机械替换）：
- `ClientHttp1Exchange::ResponseHeaderNode::head`
- `detail::Http2HeaderBlockQueue::HeaderNode::head`（`include/fiber/http/detail/Http2HeaderBlockQueue.h:22`）
- `ClientHttp3Request::current_head_` / `pending_head_`

### Layer C-2 —— h1 的两处行为改造

这是类型统一带来的**仅有两处**侵入式改动，都在 `ClientHttp1Exchange`：

**(1) `authority` 覆盖 `Host`（编码路径）**

`estimate_header_bytes` / `encode_request_header` 里：
- `head.authority` 非空 → 请求行之后立刻发 `Host: <authority>\r\n`，并在遍历 `*head.headers` 时
  跳过 `name_hash == kHostHash && lowcase_view() == "host"` 的字段；估算函数相应加上这段长度。
- `head.authority` 为空 → 完全维持现状。

代价：只有 `authority` 非空时才多一次逐字段的 hash 比较，可忽略。

**(2) trailer 走 `read_header`，`read_header` 结束返回 nullptr**

- trailer 解析完成后，往 header 节点队列追加一个 `kind == Trailer` 的 `ClientResponseHead` 节点，
  由后续 `read_header()` 取走；`response_trailers()` 降级为该节点的薄访问器（全仓只有 4 处测试调用）。
- `final_response_received_` 之后再调 `read_header()`：若有待取的 Trailer 节点则返回它，否则返回
  **成功 + `nullptr`**，与 h2/h3 对齐（不再无限重复返回 final head）。

改完之后，三种协议的调用方是同一段代码：

```cpp
const ClientResponseHead *head = nullptr;
while ((head = *co_await ex.read_header(t)) != nullptr &&
       head->kind == OutgoingHeaderKind::Informational) {}   // 吃掉 1xx
// head 现在是 Final
while (!(co_await ex.read_body(n, t))->complete()) { /* ... */ }
const ClientResponseHead *trailer = *co_await ex.read_header(t);  // 有 trailer 则非空，否则 nullptr
```

### Layer C-3 —— `ClientHttpExchange`（union 分发）

因为 head 已经统一，三个后端的方法签名**完全同形**，前端的每个方法都是纯转发：

```cpp
// 非拥有的协议擦除句柄：三个 arm 都是借用指针，具体 exchange 由调用方 / PooledClientHttpExchange 持有。
// 平凡可拷贝可移动、16 字节、无 placement new、无生命周期管理。
class ClientHttpExchange {
public:
    ClientHttpExchange() noexcept = default;
    explicit ClientHttpExchange(ClientHttp1Exchange &e) noexcept;
    explicit ClientHttpExchange(ClientHttp2Exchange &e) noexcept;
    explicit ClientHttpExchange(ClientHttp3Exchange &e) noexcept;

    // 全部是 switch + return，不是协程 —— 零额外 frame
    async::Task<common::IoResult<void>> send_header(const ClientRequestHead &, bool end_stream, ms t) noexcept;
    async::Task<common::IoResult<std::size_t>> write_all(mem::IoBufChain, ms t) noexcept;
    async::Task<common::IoResult<std::size_t>> write_all(const std::uint8_t *, std::size_t, bool end, ms t) noexcept;
    async::Task<common::IoResult<std::size_t>> write(mem::IoBufChain &, ms t) noexcept;
    async::Task<common::IoResult<std::size_t>> write(const std::uint8_t *, std::size_t, bool end, ms t) noexcept;
    async::Task<common::IoResult<void>> send_trailer(const HttpHeaders &, ms t) noexcept;
    async::Task<common::IoResult<const ClientResponseHead *>> read_header(ms t) noexcept;
    async::Task<common::IoResult<mem::IoBufChain>> read_body(std::size_t max_bytes, ms t) noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] HttpProtocol protocol() const noexcept { return proto_; }
    [[nodiscard]] HttpVersion version() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return be_.h1 != nullptr; }
    // 逃生舱：协议专有能力（h1 的 switch_to_raw_stream、h2 的 stream_id 等）
    [[nodiscard]] ClientHttp1Exchange *as_http1() noexcept;
    [[nodiscard]] ClientHttp2Exchange *as_http2() noexcept;
    [[nodiscard]] ClientHttp3Exchange *as_http3() noexcept;

private:
    union Backend {
        ClientHttp1Exchange *h1;
        ClientHttp2Exchange *h2;
        ClientHttp3Exchange *h3;
    } be_{nullptr};
    HttpProtocol proto_ = HttpProtocol::Http1;
};
```

**为什么三个 arm 都用指针而不内嵌对象**：`ClientHttp1Exchange` 是 `NonMovable`（且必须紧挨着它的
`Http1ClientConnection` 活着），内嵌就会让 `ClientHttpExchange` 也不可移动。做成非拥有句柄之后，
所有权留在原地不动，union 退化成一个 tagged pointer，也不需要手写 placement new / 析构分发。

后端只需改名对齐（`send_request_header` → `send_header`、`write_trailer` → `send_trailer`），
或者保留旧名再加一个同名 wrapper —— 建议直接改名，全仓调用点不多。

### Layer D —— `ClientHttpConnector`（协商 + 池化择路）

```cpp
enum class HttpProtocolPreference : std::uint8_t {
    Http1Only,
    Http2Only,   // 明文即 prior-knowledge h2
    Auto,        // TLS: ALPN {h2, http/1.1}；明文: 等价 Http1Only（不做已废弃的 h2c Upgrade）
};

// 持有池租约 + 具体 exchange 的存储。析构顺序：exchange 先于 lease 释放
//（照抄现成的 Http2PooledExchange 写法）。
// v1 设计为 NonMovable，由 acquire 通过出参填充 —— 见风险 3。
class PooledClientHttpExchange {
public:
    [[nodiscard]] ClientHttpExchange exchange() noexcept;  // 协议擦除句柄
    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;
};

class ClientHttpConnector {
public:
    ClientHttpConnector(StealableHttp1ConnectionPoolSet &h1, LocalHttp2ConnectionPoolSet &h2) noexcept;

    [[nodiscard]] async::Task<common::IoResult<void>>
    acquire(const HttpConnectionGroupKey &key, mem::BufPool &pool, HttpProtocolPreference pref,
            const HttpClientTlsOptions *tls, ms timeout, PooledClientHttpExchange &out) noexcept;
};
```

流程：

1. 查 **per-loop 协议提示表**（新增 `HttpAlpnHintTable`：`group key hash -> {protocol, expire_at}`，
   小容量开放寻址，每个 loop 一份，无锁）。
2. hint == Http2 → `h2_pool.try_acquire(key)`（同步、不 dial），命中即返回。
3. hint == Http1 或 Unknown → `h1_pool.acquire(key)`，`has_connection()` 命中即返回。
4. 全 miss → `http_client_dial(loop, {.alpn = {"h2", "http/1.1"}, ...})` 协商一次。
5. 结果 **h2** → `h2_pool.acquire(key, one_shot_connector, timeout)`，connector 的 ctx 里带着刚建好的
   transport，回调内直接 `conn.adopt(std::move(transport))` 返回成功。**h2 池内部一行不改。** 写 hint = Http2。
   - 边界：`acquire` 可能因并发复用而**根本不调用 connector**。ctx 里放 `consumed` 标志，
     未被消费就把 transport `close()` 掉。罕见。
6. 结果 **h1** → `h1_pool.acquire(key)` + `emplace_connection()` + `conn->adopt(std::move(transport), peer)`。
   写 hint = Http1。
7. 结果 **h3** → 本期返回 `IoErr::NotSupported`，分支留好，日后接 `Http3Client` / QUIC 池。

**取舍说明**：备选是给 `Http2ConnectionPoolCore` 加 `adopt(key, connection) -> IoResult<Lease>` 直接注入，
读起来更直白，但要碰 `conn_total_` / dial guard / waiter 唤醒 / 退避计时器这一整套内部记账，风险明显更高。
第一版用 one-shot connector 绕开。

### Layer E —— 删除 `FIBER_ENABLE_HTTP3`

改动点（4 处）：`CMakeLists.txt:11` 删 option；`CLAUDE.md:40`、`README.md:192`、`README.zh-CN.md:155`
删表格行。`feature/*` 里 benchmark 计划的 `-DFIBER_ENABLE_HTTP3=ON` 是历史记录，CMake 只会对未使用变量
发 warning 不会失败，留着即可（要清也行，纯文档）。

---

## 3. 实现状态

Step 0–4 已全部落地，`ctest --test-dir build` **1802 项全绿**（原 1798 + 新增 4）。

| Step | 内容 | 落地文件 |
|---|---|---|
| 0 | 删除死选项 `FIBER_ENABLE_HTTP3` | `CMakeLists.txt`、`CLAUDE.md`、`README.md`、`README.zh-CN.md` |
| 1 | `http_client_dial()` + `Http1/Http2ClientConnection::adopt()` | 新增 `HttpClientDialer.{h,cpp}`；`TlsAlpn` 加 `make_negotiating_client_tls_param`；两个 `connect_impl` 退化为 dial + adopt |
| 2 | head 类型彻底统一 | 新增 `ClientHttpTypes.h`（`ClientRequestHead` / `ClientResponseHead`）；6 个协议 head 删除，全仓 ~98 处引用迁移；`target` → `path` |
| 2c | h1 三处行为改造 | `ClientHttp1Exchange.cpp`：authority 覆盖 Host、trailer 走 `read_header`、结束返回 `nullptr` |
| 3 | 统一入口 | 新增 `ClientHttpExchange.{h,cpp}`；`ClientHttp2/3Exchange` 方法改名 `send_request_header`→`send_header`、`write_trailer`→`send_trailer` |
| 4 | 协商择路 | 新增 `HttpAlpnHintTable.{h,cpp}`、`ClientHttpConnector.{h,cpp}`（含 `PooledClientHttpExchange`） |
| 测试 | 4 个新用例 | `tests/ClientHttpExchangeTest.cpp` |

新增测试覆盖：

- `SameRequestCodeRunsOverHttp1AndHttp2` —— **同一个 `run_request()` 函数**分别跑在明文 h1 server 和 TLS h2 server 上，
  断言状态码 / body / echo 头 / `authority`→Host 全部一致，且 `version()` 各自如实返回 1.1 与 2.0。
- `Http1AuthorityOverridesHostHeader` —— 裸 TCP server 校验线上字节：只有一行 `Host: authority.example`，
  调用方留在 headers 里的 `ignored.example` 被丢弃。
- `Http1TrailersArriveThroughReadHeader` —— chunked + trailer 响应，`read_header` 返回 `kind == Trailer` 的头块，
  再下一次返回 `nullptr`；`response_trailers()` 访问器仍给出同一份字段。
- `AutoNegotiatesHttp2AndReusesItThroughTheHint` —— 同一个 TLS server：`Auto` 协商到 h2 并写入 hint，
  第二次请求命中 hint + `try_acquire` 直接复用（不再握手）；`Http1Only` 在同一 server 上落到 h1 并同样复用。

### 实现中确认的两件事

1. **`read_header` 的流结束语义不需要拍板**：h2（`Http2HeaderBlockQueue.cpp:205`）和 h3（`ClientHttp3Request.cpp:1103`）
   本来就返回"成功 + `nullptr`"，只有 h1 是异端——它在 `final_response_received_` 之后**无限重复返回同一个 final head**。
   统一层直接采用 h2/h3 约定，h1 对齐，顺带修掉一个会让 `while (auto *h = co_await read_header())` 死循环的坑。
   为此给 `ProxyHandler` / `HttpClientFuncs` / `NacosClientImpl` / `CatClientCore` 的 `read_header` 循环都补了空头保护。
2. **union 用三个借用指针，不内嵌对象**：`ClientHttp1Exchange` 是 `NonMovable`，内嵌会传染。做成非拥有句柄后
   `ClientHttpExchange` 是 16 字节的 tagged pointer，平凡可拷贝、无 placement new、无析构分发；
   加上 head 已统一，**每个方法都是 `switch` + `return`，零额外 coroutine frame**。

## 4. 未做的部分与已知限制

1. **Step 5（lite_nginx 切到 `ClientHttpConnector`）未做。** 按原方案就是独立 PR，且它需要 app 侧先定几件事：
   `apps/lite_nginx` 的 `ConnectionPool` 同时封装了 `Local` 和 `Stealable` 两种 h1 池，而 `ClientHttpConnector`
   目前只接 `StealableHttp1ConnectionPoolSet`；另外还要接 `DnsService` 到 `HttpClientAddressResolver`，
   并给 location 配置加协议偏好开关。这些是产品面的取舍，不该顺手替业主决定。
2. **Step 6（HTTP/3）未接。** `ClientHttpExchange` 的 h3 分支已经写好并编译，`HttpProtocol::Http3` 在
   `ClientHttpConnector` 里显式返回 `NotSupported`，等 QUIC 侧连接池就位即可接上。
3. **h1 的 `end_stream` 是尽力而为**：只在头块本身就能证明无 body 时（HEAD 响应 / 204 / 304 / `Content-Length: 0`）
   才置 true，chunked 与 EOF-delimited 一律 false。跨协议可移植的完成判定仍然是 `read_body()` 的 `complete()`，
   这一点写进了 `ClientResponseHead` 的注释。
4. **并发语义不对称**：h1 一个租约串行、h2 一个租约是并发流。统一层不隐藏这一点。
5. **不做明文 h2c Upgrade**（已废弃）：`Auto` 在 `http://` 下等价 `Http1Only`；明文 h2 需显式 `Http2Only`（prior knowledge）。
6. **`switch_to_raw_stream()` / WebSocket 隧道不进统一 API**，通过 `as_http1()` 逃生舱暴露。
