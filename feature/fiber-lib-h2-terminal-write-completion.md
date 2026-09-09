# Fiber lib HTTP/2 终止写(terminal-only write)未消费链完成标记缺陷报告

基准引脚:`fiber-net-gateway/fiber-gateway-cpp` @ `dfa5676c0a4e186767372ea5d2e1dd5573ba925a`
(行号为此引脚原始代码;现成 diff 见文末,与 access-gateway 仓库
`native/patches/0003-http2-terminal-write-consumes-chain-completion.patch` 相同)

## 症状(线上实测,2026-09-09)

CAT/journal 上大量失败记录,形如(以 `/ai-test/mcp` POST 为代表,约 50% 的请求):

```
status=200 result=canceled error="RESPONSE_ERROR" io_error=invalid
duration_us≈600-1200  response_bytes=59-60   ← 全量响应字节
```

- **客户端实际完整收到响应**(200、字节齐全、流正常结束);
- 服务端 `access_server_proxy_failures_total{phase="write_response_body"}` 增长;
- CAT 每条记一个 ResponseError 事件 + 根事务 status=Error(即用户在 CAT 看到的失败记录);
- 仅影响 HTTP/2 客户端 + 上游 chunked(无 Content-Length)+ 路由 `flush=true`(非缓冲代理)的组合;
  h1/h3 客户端不受影响。

时间线:14:08 部署 patch-0001(h1 chunked 跨读修复)后 3 分钟开始出现,此前 journal
历史(回溯至 8/13)为零。

## 根因

### 契约:`http::pipe_http_body` 对 sink 的终止写要求

`src/http/HttpBodyPipe.cpp` 写路径不变量(L126-139):

```cpp
const bool completion_progress = written == 0 && before_bytes == 0
                                 && before_complete && !buffer.complete();
if (!consumed_expected_bytes || (written == 0 && !completion_progress) || ...) {
    abort_guard.abort(common::IoErr::Invalid);
    co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Write);
}
```

**终止-only 写**(空 + `complete()==true` 的链)成功时必须把链的 `complete()` 翻转为
false——"完成标记已被消费"。h1 与 h3 的 sink 都遵守:

- h1(`src/http/Http1ExchangeIo.cpp` L1415-1422):chunked 分支 `len==0 && end` 时写
  `0\r\n\r\n` 后 `chunk.clear_complete()`;
- h3(`src/http/ServerHttp3Request.cpp` L1490-1500):fin-only 分支
  `stream_.write(nullptr,0,true)` 成功后 `chunk.clear_complete()`。

### 缺陷:h2 的 Some 写操作符终止分支漏掉 `clear_complete()`

`src/http/ServerHttp2Request.cpp`,`SendResponseBodySomeOp::on_encode` 的
`total_bytes_ == 0` 终止分支(约 L448-466):

```cpp
if (total_bytes_ == 0) {
    FIBER_ASSERT(end_);
    mem::IoBufChain empty(request.conn_->transport().loop().io_buf_node_pool());
    ...  // 从局部 empty 链编码 END_STREAM DATA 帧 —— 正确发出
    result.flow_controlled_bytes = 0;
    result.operation_final_batch = true;
    return common::IoErr::None;   // ← 但借用的 chunk_ 从未被 clear_complete()
}
```

END_STREAM 帧从**局部临时链**编码、正确发送(所以客户端完好),但**借用的调用方链**
(`chunk_`)的完成标记没人消费 → 回到 pipe 后 `completion_progress == false` →
`pipe_error(Invalid, Write)` → ProxyExecutor 记 `write_response_body` 失败 +
`record_response_error(Invalid)` → journal `result=canceled error="RESPONSE_ERROR"
io_error=invalid` + CAT 失败事务。纯簿记误报,无客户端可见影响。

同文件**非终止分支**是正确的(尾部 `chunk_->clear_complete()`),只有 `total_bytes_==0`
分支漏了。客户端侧 `src/http/ClientHttp2Request.cpp` 的 `SendRequestBodySomeOp::on_encode`
终止分支(L694-711)有一模一样的遗漏(流式请求体经 pipe 转发时会踩中);
两个 `*AllOp` 按值持有链,无借用状态,不受影响。

### 触发链(为何 14:11 才开始)

1. 路由 `flush=true` → ProxyExecutor 以 `low_water = kUnbufferedBodyPipeLowWater(0)`
   调 pipe:每读即写(`can_read = buffered == 0`),终止读(空+complete)成为
   **独立的终止-only 写**;缓冲模式(`low_water=48K`)会把终止读合并进最后一次
   数据写,不触发。
2. 上游 h1 chunked:Node 服务把 JSON-RPC 响应体与 `0\r\n\r\n` 终止符**分两次 socket
   写**送出 → 网关分两次读到 → 命中上述时序。
3. **patch-0001 之前**此组合不出现:旧解析器遇到跨界 framing 会等更多数据,终止符
   总是与数据合并在同一次读里交付。0001 让解析器及时、分段地交付,才暴露了 h2 sink
   的契约违反 —— 三个 patch 是同一类"流式语义闭环"的连续修复。

### 单测级确定性复现(已写成测试,修复前红、修复后绿)

`tests/Http2EndpointTest.cpp` `StreamedAutoBodyThroughPipeCompletes`:
FixedSource 两段读(body 40B 不完整 / 空且 complete)+ `low_water=0` +
`pipe_http_body` → h2 endpoint。修复前 pipe 返回 `Invalid`(Write),修复后通过且
客户端收到全量 body。h3 同型测试(`Http3EndpointTest.StreamedAutoBodyThroughPipeCompletes`)
固定 h3 侧契约。

## 修复

两处终止分支在编码成功后补上完成标记消费:

```cpp
// total_bytes_ == 0 分支,frame encode 成功之后:
if (chunk_ != nullptr) {
    chunk_->clear_complete();
}
```

- `ServerHttp2Request::SendResponseBodySomeOp::on_encode`
- `ClientHttp2Request::SendRequestBodySomeOp::on_encode`

行为影响仅限"终止-only 写成功后链的 complete() 状态"—— 与 h1/h3 sink 及本文件
非终止分支对齐,调用方(pipe)的不变量因此成立。

## 验证

- 定向:`Http2EndpointTest`(12/12,含新回归测试)、`Http3EndpointTest`、
  `Http2ConnectionTest`/`Http2ClientConnectionTest`/`Http2ServerConnectionTest`/
  `Http2StreamTest`/`HttpBodyPipeTest` 共 171/171;
- 全量:fiber_tests 1598 通过、1 失败(`Http1EndpointTest.DrainWaitsForRequestBodyAndHandlerLifetime`,
  引脚上既有失败,与本改动无关);
- 线上(172.28.2.111,2026-09-09 部署后 3 分钟窗口):journal canceled=0,
  `/ai-test/mcp` 9/9 `result=success`(修复前 ~50% canceled),
  `write_response_body`=0。部署前同窗口对照:每 20s 一条 canceled。

## 附注(排查中排除的疑点)

- 曾观察到 h2 响应 DATA 帧载荷里出现"完整的 HEADERS 帧字节":为我测试代码自身
  `IoBuf::allocate` 后未初始化即 commit(读到 malloc 复用内存的旧内容)所致,
  非服务端缺陷;服务端编码与线上字节(HEADERS → DATA(body) → DATA(0, END_STREAM))
  完全正确。
- `sink.flush()`(非缓冲 pipe 每次排空后调用)是 exchange 写者的 no-op 成功,
  与本缺陷无关。

---

## 完整 diff(可直接 git apply 到 fiber-lib 仓库;测试部分见 patch 文件)

```diff
diff --git a/src/http/ServerHttp2Request.cpp b/src/http/ServerHttp2Request.cpp
--- a/src/http/ServerHttp2Request.cpp
+++ b/src/http/ServerHttp2Request.cpp
@@ -461,6 +461,13 @@ common::IoErr ServerHttp2Request::SendResponseBodySomeOp::on_encode(...)
         if (err != common::IoErr::None) {
             return err;
         }
+        // The terminal-only batch (empty complete chain) consumed the borrowed
+        // chain's completion marker, same as the data branch below: callers
+        // (http::pipe_http_body) require a successful terminal write to flip
+        // complete() to false or their completion-progress invariant trips.
+        if (chunk_ != nullptr) {
+            chunk_->clear_complete();
+        }
         result.flow_controlled_bytes = 0;
         result.operation_final_batch = true;
         return common::IoErr::None;

diff --git a/src/http/ClientHttp2Request.cpp b/src/http/ClientHttp2Request.cpp
--- a/src/http/ClientHttp2Request.cpp
+++ b/src/http/ClientHttp2Request.cpp
@@ -704,6 +704,13 @@ common::IoErr ClientHttp2Request::SendRequestBodySomeOp::on_encode(...)
         if (err != common::IoErr::None) {
             return err;
         }
+        // The terminal-only batch (empty complete chain) consumed the borrowed
+        // chain's completion marker, same as the data branch below: callers
+        // (http::pipe_http_body) require a successful terminal write to flip
+        // complete() to false or their completion-progress invariant trips.
+        if (chunk_ != nullptr) {
+            chunk_->clear_complete();
+        }
         result.flow_controlled_bytes = 0;
         result.operation_final_batch = true;
         return common::IoErr::None;
```
