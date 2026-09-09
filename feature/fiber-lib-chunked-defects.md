# Fiber lib HTTP/1 chunked 解析缺陷报告

基准引脚:`fiber-net-gateway/fiber-gateway-cpp` @ `dfa5676c0a4e186767372ea5d2e1dd5573ba925a`
(文中行号均为此引脚的原始代码;完整现成 diff 见文末,与 access-gateway 仓库
`native/patches/0001-http-h1-chunked-body-across-split-transport-reads.patch` 相同)

## 症状(线上实测)

openresty 对动态 gzip 资源返回 `Transfer-Encoding: chunked` 且无 Content-Length。
经 access-server 代理后:

- `curl -H 'Accept-Encoding: gzip'` 大 JS/CSS:**200 + 0 字节**,~16ms 即中止(100% 复现);
  gzip_static 资源(Content-Length)正常 → 确认只影响 chunked。
- 浏览器 Chrome netlog:`HTTP2_SESSION_ERROR "Server reset stream"`,stream 3/5,~4ms,
  status 200、response_bytes=0 → 代理侧主动 RST_STREAM(CANCEL);h1 表现为连接被断。

## 触发时序

chunked 响应的 framing 与 payload 天然可能跨 TCP 段(发送端 headers、size 行、压缩数据
分别 flush;MSS 边界还可能把 size 行本身拆成两半)。两种典型拆分:

- **拆分 A(线上确定性命中)**:响应头 + 完整 size 行(`1f47\r\n`)先到,payload 在下一段。
- **拆分 B**:`1f47\r\n` 本身被拆开,如 `"1f4"` + `"7\r\n"`(大 chunk 的 size 行 5-8 字节,
  随机可能)。

原始代码在两种拆分下都会失败;**只修"空返回"不修"部分 size"会把中止变成 body 错乱**,
三个缺陷必须一起修(实测验证过)。

---

## 缺陷 1(根):`BodyParser::remaining()` 报告了不完整的 chunk size

**位置**:`src/http/Http1Parser.cpp`

- `ChunkedBodyParser::execute`(L1033-1252):状态机在 `ChunkSize` 状态下逐字符消费
  十六进制位并累进 `size_`。size 行不完整时循环耗尽缓冲,走到 `data:` 标签
  (L1205-1207)`buffer->consume(pos - begin)` —— **已到的数字位被消费、部分值留在
  `size_`**,返回 `ParseCode::Again`(consumed>0)。
- `BodyParser::remaining()`(L1279-1289):

```cpp
case Type::Chunked:
    return chunked_parser_.size();   // ← 无条件返回 size_,包括 size 行只解析了一半
```

**后果**:size 行只解析了一半时(`"1f4"` 已消费,`size_ == 0x1f4`),`remaining()` 把这个
**部分值当作可读 payload 长度**上报。调用方(`read_body` 的读 sizing 与 take 计算)
据此从 transport 读 / 从缓冲切走 `remaining()` 字节——**size 行的剩余字节(`"7\r\n"`)
被当成 payload 泄漏进 body**,且真实 payload 长度错位,后续所有 framing 全部错乱。

**实测表现**:拆分 B 下,修复了空返回但没修本缺陷时,下游收到的 payload 是 `"7\r"`
(size 行残片),body 从第一个字节就是错的。

**修法**:`ChunkedBodyParser` 增加 `payload_remaining()`,仅在进入 `ChunkData` 状态
(size 行已完整解析)后报告 `size_`;`BodyParser::remaining()` 的 Chunked 分支改用它:

```cpp
// Http1Parser.h (ChunkedBodyParser public:)
// Payload bytes are only available once the chunk-size line has fully parsed; while the
// line is still incomplete size_ holds a partial value that must not surface as payload.
[[nodiscard]] std::size_t payload_remaining() const noexcept;

// Http1Parser.cpp:
std::size_t ChunkedBodyParser::payload_remaining() const noexcept {
    return state_ == State::ChunkData ? size_ : 0;
}

// BodyParser::remaining() 的 Chunked case:
case Type::Chunked:
    return chunked_parser_.payload_remaining();
```

语义核对:`execute` 顶部 `if (state == ChunkData && size_ == 0) state = AfterData;`
在下次 execute 才推进,所以 payload 恰好消费完时 `remaining()` 返回 0(size_ 已减到
0),AfterData/trailer 阶段也返回 0,均正确。

---

## 缺陷 2:`read_body` / `read_request_body` 的 chunked 分支会返回「空且未 complete」的链

**契约**:`read_body(max_bytes)` 不得返回 0 字节且未 complete 的 `IoBufChain`。
调用方 `http::pipe_http_body`(`src/http/HttpBodyPipe.cpp` L105-108)把
`bytes == 0 && !complete` 视为 Validate 阶段不变量违规 → Invalid → 代理中止下游响应
(h1 断连 / h2 RST_STREAM(CANCEL))——**这就是线上浏览器报错的直接来源**。

**位置 1(客户端,读上游响应)**:`src/http/ClientHttp1Exchange.cpp`,`read_body` chunked
分支(L1876-1952),两条提前返回:

```cpp
// L1903:advance 返回 Again 时无条件返回 —— out 可能还是空的
if (*parse_result == ParseCode::Again) {
    ...stash...
    co_return out;
}
// L1912-1918:framing 已解析、payload 未到、且本调用已做过 framing IO 时返回空链
if (read_buf.readable() == 0) {
    if (read_call_used_io || out.readable_bytes() != 0) {   // ← read_call_used_io 挡死了补读
        ...stash...
        co_return out;
    }
    auto more = co_await read_more(read_buf, std::min(remaining_budget, response_body_parser_.remaining()), ...);
```

拆分 A 命中:头部解析后 leftover 里有完整 `1f47\r\n`;`advance_chunked_body` 不需要读
IO 就解析出 framing(payload 未到,`read_call_used_io` 保持 false 时能落到 read_more,
**但如果 framing 是靠 advance 里的一次 transport read 补齐的**,`read_call_used_io` 已为
true,payload 又不在缓冲 → 返回空链)。拆分 B 命中 Again 路径同理。

**位置 2(服务端,读下游请求体)**:`src/http/Http1ExchangeIo.cpp`,`read_request_body`
chunked 分支(L524-575),同构的 L545-547(`Again` → 无条件 return)与
L550-553(`read_call_used_io_` → return)。代理转发 chunked 请求体(流式上传)时同样触发。

**修法**:两处相同——out 已有字节才提前返回;out 为空时落到底部继续读,读 sizing 在
framing 未就绪(`remaining()==0`)时用 `remaining_budget`:

```cpp
if (*parse_result == ParseCode::Again && out.readable_bytes() != 0) {
    ...stash...
    co_return out;
}
...
if (read_buf.readable() == 0) {
    if (out.readable_bytes() != 0) {
        ...stash...
        co_return out;
    }
    // An empty, non-complete body would violate the read_body contract
    // (callers such as http::pipe_http_body treat it as a protocol
    // error), so when nothing has been delivered yet keep reading even
    // if this call already performed framing I/O — the same policy the
    // ContentLength branch above uses.
    const std::size_t want = response_body_parser_.remaining() == 0
                                     ? remaining_budget
                                     : std::min(remaining_budget, response_body_parser_.remaining());
    auto more = co_await read_more(read_buf, want, read_call_used_io, timeout);
    ...
}
```

对照:ContentLength 分支(L1843-1874 客户端侧)本来就在缓冲为空时无条件读——chunked
分支多出的 `read_call_used_io` 保守限制正是堵死空链补读的原因,对齐即可。

无自旋风险:循环每轮要么消费字节、要么 advance 消费 framing、要么阻塞读(读 0 字节 →
ConnReset 报错),必有进展。

---

## 缺陷 3:`advance_chunked_body`(客户端/服务端两份)把「缓冲里的部分 framing」当协议错误

**位置**:

- `src/http/ClientHttp1Exchange.cpp` L605-642;
- `src/http/Http1ExchangeIo.cpp` L418-459(逻辑同构)。

```cpp
if (code == ParseCode::Again) {
    if (consumed == 0) {
        co_return std::unexpected(common::IoErr::Invalid);   // ← 部分 framing 在缓冲、还允许读,却直接判死
    }
    continue;
}
```

`Again` 的语义是「需要更多字节」,不是「格式非法」。注意配合缺陷 1 的事实:
size 行不完整时 execute 会消费数字位(consumed>0 → continue → 缓冲空 → 因
`allow_read` 已被置 false 返回 Again),而**数字位之外**的情况(如缓冲里只剩
`"\r"`、或极端拆分下首字节就是非数字的扩展位)consumed 可能为 0——此时若还允许读,
应补读;确实不允许读时应返回 `Again` 让调用方读,而不是 `Invalid`。

**修法**(两处同构):

```cpp
if (code == ParseCode::Again) {
    if (consumed > 0) {
        continue;
    }
    if (!allow_read) {
        co_return ParseCode::Again;
    }
    // Incomplete framing (a chunk-size line split across transport reads) is
    // buffered; extend it with one more read instead of failing the exchange.
    auto more = co_await read_more(...);   // 客户端: read_more(read_buf, max_bytes, read_call_used_io, timeout)
                                           // 服务端: read_more(max_bytes, timeout)
    if (!more)  co_return std::unexpected(more.error());
    if (*more == 0) co_return std::unexpected(common::IoErr::ConnReset);
    allow_read = false;
    continue;
}
```

每次 advance 仍至多补一次读(`allow_read=false`),维持「每调用一次 IO」的原设计。

---

## 修复后的三条不变量

1. `BodyParser::remaining()`(Chunked)只在 size 行完整解析后报告 payload 长度;
2. `read_body` / `read_request_body` 永不返回「空且未 complete」的链(对齐 ContentLength
   分支:缓冲为空且无产出 → 继续读);
3. framing 不完整 → 读更多(或返回 `Again` 交回调用方读),绝不 `Invalid`。

## 验证方法

- **单测回归**(access-server 仓库已建):`ProxyExecutorTest.StreamsChunkedUpstreamWhoseFramingArrivesSeparatelyFromPayload`
  —— 裸 TCP 脚本上游:headers 单独 flush → 20ms → `"2"`(半行 size)→ 20ms →
  `"8\r\n"`(size 补全、无 payload)→ 20ms → 40 字节 payload → 20ms → `"\r\n0\r\n\r\n"`。
  同时覆盖拆分 A 与拆分 B。已验证:原始代码失败(空返回中止),只修缺陷 2 失败
  (payload 错乱成 `"8\r"`),三修全好后通过。
- **线上命令**:`curl -sk -H 'Accept-Encoding: gzip' -o /dev/null -w "%{http_code} size=%{size_download}" https://<host>/<大js>`
  —— 修复前 200/0 字节,修复后完整 877233 字节且 gunzip 精确解压。

## 附带

此引脚上 `Http1EndpointTest.DrainWaitsForRequestBodyAndHandlerLifetime` 本来就不通过
(与 chunked 修复无关,已验证原始代码同样失败):Content-Length drain 场景,
`serve_done` 后 handler 的 `weak_ptr` 未过期(L481)。改上游时可顺带排查。

---

## 完整 diff(可直接套用到 fiber-lib 仓库,git apply)

```diff
diff --git a/include/fiber/http/Http1Parser.h b/include/fiber/http/Http1Parser.h
index 3420d05..63fb8ff 100644
--- a/include/fiber/http/Http1Parser.h
+++ b/include/fiber/http/Http1Parser.h
@@ -180,6 +180,9 @@ class ChunkedBodyParser : public common::NonCopyable, public common::NonMovable
 public:
     void reset() noexcept;
     [[nodiscard]] std::size_t size() const noexcept { return size_; }
+    // Payload bytes are only available once the chunk-size line has fully parsed; while the
+    // line is still incomplete size_ holds a partial value that must not surface as payload.
+    [[nodiscard]] std::size_t payload_remaining() const noexcept;
     [[nodiscard]] std::size_t length() const noexcept { return length_; }
     void consume(std::size_t n) noexcept;
     ParseCode execute(mem::IoBuf *buffer) noexcept;
diff --git a/src/http/ClientHttp1Exchange.cpp b/src/http/ClientHttp1Exchange.cpp
index 5e3bef6..b47358a 100644
--- a/src/http/ClientHttp1Exchange.cpp
+++ b/src/http/ClientHttp1Exchange.cpp
@@ -629,9 +629,22 @@ ClientHttp1Exchange::advance_chunked_body(mem::IoBuf &read_buf, std::size_t max_
         }
 
         if (code == ParseCode::Again) {
-            if (consumed == 0) {
-                co_return std::unexpected(common::IoErr::Invalid);
+            if (consumed > 0) {
+                continue;
+            }
+            if (!allow_read) {
+                co_return ParseCode::Again;
+            }
+            // Incomplete framing (a chunk-size line split across transport reads) is
+            // buffered; extend it with one more read instead of failing the exchange.
+            auto more = co_await read_more(read_buf, max_bytes, read_call_used_io, timeout);
+            if (!more) {
+                co_return std::unexpected(more.error());
             }
+            if (*more == 0) {
+                co_return std::unexpected(common::IoErr::ConnReset);
+            }
+            allow_read = false;
             continue;
         }
         if (code != ParseCode::Ok && code != ParseCode::Done && code != ParseCode::BodyDone) {
@@ -1900,7 +1913,7 @@ ClientHttp1Exchange::read_body(std::size_t max_bytes, std::chrono::milliseconds
                 out.mark_complete();
                 co_return out;
             }
-            if (*parse_result == ParseCode::Again) {
+            if (*parse_result == ParseCode::Again && out.readable_bytes() != 0) {
                 auto stash_result = stash_pending_buf(read_buf);
                 if (!stash_result) {
                     co_return fail_exchange(stash_result.error());
@@ -1910,15 +1923,22 @@ ClientHttp1Exchange::read_body(std::size_t max_bytes, std::chrono::milliseconds
         }
 
         if (read_buf.readable() == 0) {
-            if (read_call_used_io || out.readable_bytes() != 0) {
+            if (out.readable_bytes() != 0) {
                 auto stash_result = stash_pending_buf(read_buf);
                 if (!stash_result) {
                     co_return fail_exchange(stash_result.error());
                 }
                 co_return out;
             }
-            auto more = co_await read_more(read_buf, std::min(remaining_budget, response_body_parser_.remaining()),
-                                           read_call_used_io, timeout);
+            // An empty, non-complete body would violate the read_body contract
+            // (callers such as http::pipe_http_body treat it as a protocol
+            // error), so when nothing has been delivered yet keep reading even
+            // if this call already performed framing I/O — the same policy the
+            // ContentLength branch above uses.
+            const std::size_t want = response_body_parser_.remaining() == 0
+                                             ? remaining_budget
+                                             : std::min(remaining_budget, response_body_parser_.remaining());
+            auto more = co_await read_more(read_buf, want, read_call_used_io, timeout);
             if (!more) {
                 co_return fail_exchange(more.error());
             }
diff --git a/src/http/Http1ExchangeIo.cpp b/src/http/Http1ExchangeIo.cpp
index 2c33694..0bf7d0e 100644
--- a/src/http/Http1ExchangeIo.cpp
+++ b/src/http/Http1ExchangeIo.cpp
@@ -446,9 +446,22 @@ Http1ExchangeIo::advance_chunked_body(std::size_t max_bytes, bool allow_read,
         }
 
         if (code == ParseCode::Again) {
-            if (consumed == 0) {
-                co_return std::unexpected(common::IoErr::Invalid);
+            if (consumed > 0) {
+                continue;
+            }
+            if (!allow_read) {
+                co_return ParseCode::Again;
+            }
+            // Incomplete framing (a chunk-size line split across transport reads) is
+            // buffered; extend it with one more read instead of failing the exchange.
+            auto more = co_await read_more(max_bytes, timeout);
+            if (!more) {
+                co_return std::unexpected(more.error());
             }
+            if (*more == 0) {
+                co_return std::unexpected(common::IoErr::ConnReset);
+            }
+            allow_read = false;
             continue;
         }
         if (code != ParseCode::Ok && code != ParseCode::Done && code != ParseCode::BodyDone) {
@@ -542,16 +555,22 @@ Http1ExchangeIo::read_body(HttpExchange &exchange, size_t max_bytes, std::chrono
                     out.mark_complete();
                     co_return out;
                 }
-                if (*parse_result == ParseCode::Again) {
+                if (*parse_result == ParseCode::Again && out.readable_bytes() != 0) {
                     co_return out;
                 }
             }
 
             if (body_input_readable() == 0) {
-                if (read_call_used_io_) {
+                if (out.readable_bytes() != 0) {
                     co_return out;
                 }
-                auto more = co_await read_more(std::min(remaining_budget, body_parser_.remaining()), timeout);
+                // An empty, non-complete body would violate the read_body contract (callers
+                // such as http::pipe_http_body treat it as a protocol error), so keep
+                // reading while nothing has been delivered yet.
+                const std::size_t want = body_parser_.remaining() == 0
+                                                 ? remaining_budget
+                                                 : std::min(remaining_budget, body_parser_.remaining());
+                auto more = co_await read_more(want, timeout);
                 if (!more) {
                     co_return std::unexpected(more.error());
                 }
diff --git a/src/http/Http1Parser.cpp b/src/http/Http1Parser.cpp
index 0baa073..0a7bde8 100644
--- a/src/http/Http1Parser.cpp
+++ b/src/http/Http1Parser.cpp
@@ -1022,6 +1022,10 @@ void ChunkedBodyParser::reset() noexcept {
     length_ = 0;
 }
 
+std::size_t ChunkedBodyParser::payload_remaining() const noexcept {
+    return state_ == State::ChunkData ? size_ : 0;
+}
+
 void ChunkedBodyParser::consume(std::size_t n) noexcept {
     if (n >= size_) {
         size_ = 0;
@@ -1283,7 +1287,7 @@ std::size_t BodyParser::remaining() const noexcept {
         case Type::ContentLength:
             return remaining_;
         case Type::Chunked:
-            return chunked_parser_.size();
+            return chunked_parser_.payload_remaining();
     }
     return 0;
 }
```
