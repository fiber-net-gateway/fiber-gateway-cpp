# Fiber lib HTTP/3 终止帧(fin)搁浅缺陷报告

基准引脚:`fiber-net-gateway/fiber-gateway-cpp` @ `dfa5676c0a4e186767372ea5d2e1dd5573ba925a`
(行号为此引脚原始代码;现成 diff 见文末,与 access-gateway 仓库
`native/patches/0002-quic-h3-terminal-fin-strand.patch` 相同)

## 症状(线上实测,2026-09-09)

浏览器(Chrome,h3)经 access-server 访问 `lingxi-manager.haierxj.cn` 的无 Content-Length
API(如 `/lingxi-manager/api/auth/decodeUserInfo`,286 字节流式响应):

- DevTools 单请求 **15 秒**,几乎全部计入 **Content Download**(响应头与 body 其实早已到达);
- 服务端 journal:`result=success duration_us≈8000-12000`,8-12ms 全部正常;
- CAT 耗时同样很短。

即:**响应头、全部 body 数据都在毫秒级送达,但流的终止帧迟迟不到**,浏览器一直等
body 结束,直到 SPA 的 15s 超时把请求杀掉(或页面其他流量把它捎带救活)。

## 复现与证据链(headless Chrome + netlog)

1. **fin 是独立小包**:无 CL 响应的终止帧 = `write(nullptr,0,true)` → 0 字节 STREAM 帧
   (fin=true, offset=final_size),单独一个 ~22B UDP 包。正常情况它紧跟 body 之后
   (+4ms,恰好一个 ACK 往返——本身就是缺陷 A 的表现)。
2. **搁浅现场**:某次加载中 oauth2Callback 流收到 `HEADERS + DATA(333, fin=false)` 后,
   服务端在流上**再无任何帧**(30ms 窗口,直到回敬客户端的 RST);而 journal 早已记
   `success`。客户端 ACK 了 body 包,服务端依旧没有发 fin。
3. **29s 会话超时**:整页 h3 加载后连接静默,29 秒时 Chrome QUIC idle timeout
   (`No recent network activity ... Timeout:29s`)杀掉会话,挂着的请求全部失败。
4. **单元级确定性复现**(已写成测试,修复前通过、修复后反转):
   body extent 编码进包(Inflight)后 append fin → `encode_stream_frame` 返回"不可编码"。

## 根因(三个事实叠加)

### 缺陷 A(根):fin-only 帧不允许在数据 inflight 时编码

`src/quic/QuicStreamSendQueue.cpp`,`encode_stream_frame`(L164-216)fin-only 分支:

```cpp
if (cur == nullptr) {
    if (!has_pending_fin() || buffered_bytes() > 0) {   // buffered = ready + inflight
        return result;                                   // ← inflight 数据未 ACK 时,fin 拒绝编码
    }
```

`buffered_bytes() = ready_bytes_ + inflight_bytes_` —— 只要 body 还在 inflight(未 ACK),
终止帧就不能编码。**这不满足任何 RFC 要求**:QUIC 的 offset 是绝对值、接收端乱序重组,
fin-only 帧在 `offset = final_size` 随时可发(RFC 9000 无"FIN 需等数据 ACK"的约束)。

代理响应的典型时序必然命中:pipe 先写 body chunk(extent 编码进包、转 Inflight),
随后 `ServerHttp3Request::write` 的终止分支(L1490-1500)发空 chunk+end → fin 落队,
但 body inflight → 编码被拒,车票退回 `pending_frames`。

### 缺陷 B:body ACK 之后没有任何机制重新触发发包

`src/quic/QuicAckHandler.cpp` L233:ACK 处理 → `on_stream_send_acked` → `mark_send_acked`
→ 释放 extent + `notify_write_waiter()`。**没有 `queue_stream_frame`、没有 flush、
没有调度**(整个 AckHandler 里 grep 不到任何 flush/schedule)。而此时终止写协程早已
返回"成功",没有 waiter 可唤醒。fin 车票躺在 `pending_frames` 里休眠。

### 缺陷 C:keepalive 定时器看到积压只重新武装、不发送

`src/quic/QuicConnection.cpp` `on_keepalive_timer`(L1852-1868):

```cpp
if (connection->has_pending_send_work()) {
    connection->arm_keepalive_timer();   // ← 只续期,不 flush
    return;
}
```

`has_pending_send_work()` 的定义包含 `!space.pending_frames.empty()` —— 搁浅的 fin 恰好
让它永远走这个分支。定时器救不了。

### 触发条件(为什么时有时无)

fin 车票只有当连接上**恰好有其他发包活动**(同连接新请求、需要回 ACK 的入包、MTU 探测)
才会被顺带 build 出去:

- 页面繁忙(资源并发)→ 每次都被救 → 正常;
- **温热但静默**的连接上单个 API 调用(用户场景:页面挂着,SPA 发请求)→ 无任何后续
  活动 → fin 永久搁置 → SPA 15s 超时(线上症状)/ Chrome 29s idle timeout(复现)。
- 新建连接不搁浅:握手期的包交换提供了救活时机。

## 修复(已验证)

fin-only 分支的守卫从 `buffered_bytes() > 0` 收紧为只看 **ready**(未发送)数据:

```cpp
if (!has_pending_fin() || ready_bytes_ > 0) {
    return result;
}
```

- ready 数据存在时仍推迟 fin,让最终 extent 携带它(L258 的合并编码路径,行为不变);
- inflight 不再阻塞:fin 在 body 的同一个 build 轮次即可发出(实测从 +4ms 降到 +3µs,
  与 body 同批),对"ACK 后有人再碰发包泵"的依赖彻底消失;
- 丢包重传语义完好:fin-only 帧在 `sent_frames` 里按包跟踪(STREAM 帧必使包
  ack-eliciting),`mark_failed(offset, final_size, 0, fin=true)` 复位 `fin_inflight_` 可重编码。

## 建议的上游加固(本 patch 未包含,可选)

1. `mark_send_acked` 释放 extent 后,若 stream 仍有 pending fin,主动
   `queue_stream_frame`(消除对任何未来事件的依赖,防御类似时序);
2. `on_keepalive_timer` 遇到 `has_pending_send_work()` 时尝试 flush 而不是仅续期。

## 验证方法

- **单测**(已随 patch 提供):`QuicStreamSendQueueTest.FinOnlyFrameEncodesWhileBodyInflight`
  —— 修复前:fin 被 inflight 阻塞(用同型测试验证过);修复后:立即编码、丢包可重传、
  ACK 后队列清空。fiber 全套 1594 通过;Quic*/Http3* 439 通过(1 skip 为环境项)。
- **线上对比**:修复前 netlog 中 fin 与 body 相隔 4ms(一个 ACK 往返)且可无限搁浅;
  修复后 fin 与 body 同批(+3µs),静默连接上流式响应正常终结。

## 附带(独立次要问题,未修)

h2/h3 上"无请求体的 GET + 无 CL 流式响应"会在服务端 journal 记
`result=canceled error="RESPONSE_ERROR" io_error=invalid`,但客户端实际完整收到响应
(19ms、字节齐全、连接完好)——疑似完成后误标记的簿记问题,建议上游顺带排查
(`ServerHttp2Request` 响应完成后某路径把 exchange 置为失败)。

---

## 完整 diff(可直接 git apply 到 fiber-lib 仓库)

```diff
diff --git a/src/quic/QuicStreamSendQueue.cpp b/src/quic/QuicStreamSendQueue.cpp
index 0e45ef0..3e7f852 100644
--- a/src/quic/QuicStreamSendQueue.cpp
+++ b/src/quic/QuicStreamSendQueue.cpp
@@ -171,7 +171,14 @@ QuicStreamSendQueue::encode_stream_frame(std::uint64_t stream_id, std::uint8_t *
     mem::IoBufNode *cur = ready_head_;
 
     if (cur == nullptr) {
-        if (!has_pending_fin() || buffered_bytes() > 0) {
+        // A fin-only STREAM frame at offset = final_size is deliverable while earlier
+        // extents are still inflight: offsets are absolute and the peer reassembles
+        // out of order, so the FIN must not wait for the body's ACK. Requiring
+        // buffered_bytes() == 0 stranded the terminal frame on connections with no
+        // further send activity after that ACK (response body never ends for the
+        // client). Pending *ready* data still holds the FIN back so the final
+        // extent can carry it instead (encode path below).
+        if (!has_pending_fin() || ready_bytes_ > 0) {
             return result;
         }
 
```

(配套回归测试 `tests/QuicStreamSendQueueTest.cpp` 的
`FinOnlyFrameEncodesWhileBodyInflight` + TestAccess 的 `mark_failed` 访问器,
完整内容见 `native/patches/0002-quic-h3-terminal-fin-strand.patch`。)
