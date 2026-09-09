# Fiber lib QUIC 四个 awaiter 的 resume 投递不可撤销(硬销毁悬垂)缺陷报告

基准引脚:`fiber-net-gateway/fiber-gateway-cpp` @ `dfa5676c0a4e186767372ea5d2e1dd5573ba925a`
(行号为此引脚原始代码;现成 diff 见文末,与 access-gateway 仓库
`native/patches/0004-quic-awaiter-destruction-safe-resume.patch` 相同)

## 症状(线上实测,2026-09-09)

172.28.2.111 上的 access-server(pid 25444,当时已带 0001/0002/0003 三个补丁)于
**17:05:09 SIGSEGV**,17:05:11 被 systemd 自动拉起(pid 26986,此后稳定):

- 内核 segfault:`error 15`(SEGV_MESIERR,不可执行内存取指),`ip == fault addr`
  —— 典型的"跳转到已释放/不可执行内存"(wild jump),而非普通空指针;
- 崩溃线程 25450,正值浏览器控制台页面加载;
- CAT 上出现事务 `fiber-access-server-ac1c026f-496929-3765`(用户看到的"报错记录"),
  但该请求 **journal 里永远没有对应行**:请求序号 3765 位于 3763(字体)与
  3767(user/callback)之间,缺失。原因:`RequestObservability.cpp` 中 CAT 完成在
  journal 落盘之前(`root_.complete()` 约 L170 → journal 行在 :187),进程在两步之间
  死亡 → **CAT 有记录、journal 无记录** = 崩溃发生在该请求收尾期间;
- Alt-Svc 已通告(`AccessServer.cpp:85`,h3 端口继承自 TLS 端口)→ 浏览器用 h3;
  登录页跳转(页面导航/复位)在响应体中途放弃 → 触发。

另:当日 15:35 / 16:07 两次 systemd stop 走到 90s 超时被 SIGKILL,是独立的
排空(drain)悬挂问题,与本缺陷无关(见文末附注)。

## 根因

### 触发链(谁在销毁挂起的协程)

`ProxyExecutor::execute`(`native/access-server/src/execution/ProxyExecutor.cpp` :885-886):

```cpp
auto completed = co_await async::when_any([&exchange]() { return exchange.wait_response_channel_closed(); },
                                          [&]() { return execute_impl(exchange, proxy, input, telemetry).select(); });
```

客户端中途放弃响应(h3 下为 RESET_STREAM/STOP_SENDING 或连接消失)→
`wait_response_channel_closed()` 先完成 → `when_any` 的 **destroy_losers 硬销毁**
仍在 `co_await stream_.write(chunk, timeout)`(`ServerHttp3Request.cpp` :1513,
HTTP/3 响应体写、受流控挂起)上挂起的整个代理任务树
(pipe_http_body → HttpResponseWriter → HttpExchange::write → ServerHttp3Request::write)。

流控细节:浏览器只给了很小的初始流窗口,响应体第一笔 `QuicStream::write` 就挂起;
`QuicStream::reset()`(`QuicStream.cpp` :558-575)先 `mark_send_aborted()`——同步触发
`send_aborted_callback_`,即 `ResponseChannelClosedAwaiter`(:827-829 注册),经
`post_local` 入 **defer 队列**(本轮 io 回调后的 trailing `drain_defer<false>` 执行)——
然后才 `notify_write_waiter(BrokenPipe)` 完成挂起的 WriteAwaiter。修复前该 resume
经 `loop_->post<>()` 入 **MPSC notify 队列**,只能在**下一轮循环开头**的
`drain_notify` 才被消费(`run_once` 顺序:run_due_timers → drain_notify → drain_defer
→ poll/io → drain_defer<false>)。于是顺序必然是:

1. trailing `drain_defer<false>` 先唤醒 when_any 外层协程;
2. destroy_losers 销毁挂起在 WriteAwaiter 上的写协程,`~WriteAwaiter` 运行;
3. **MPSC 里的 notify 表项无法撤销** → 悬垂;
4. 下一轮 `drain_notify` → `MpscQueue::try_pop_all`(`MpscQueue.h` :54)读取已释放
   内存 → `on_notify(悬垂 awaiter)` → 取出垃圾 `handle_` → `handle.resume()` → 跳飞。

### ASan 实证(本地复现,修复前)

本地 ASan 实例(CAT 黑洞到 127.0.0.1:1,`temp/access-server-local-cat.env`)+
失败模式搅拌(404 混 `curl --max-time 0.05/0.06` 在 h2/h3 上中途断体),约 2 分钟内
稳定触发 **heap-use-after-free**:

- 读侧(access):`MpscQueue::try_pop_all` → `drain_notify<true>` → `run_once`(线程 T6);
- 释放侧(free):when_any destroy_losers → TaskSelectAwaiter reset →
  `ProxyExecutor::execute_impl (.destroy)` :1009 → `UpstreamAttempt::run (.destroy)` :818 →
  `pipe_http_body (.destroy)` :122 → `HttpResponseWriter::write` :72 →
  `exchange_write_chain` :27 → `HttpExchange::write` :393 →
  `ServerHttp3Request::write (.destroy)` :1513 —— 与线上 17:05:09 崩溃同构;
- 唤醒侧:该帧由 `drain_defer<false>` 里的 `ResponseChannelClosedAwaiter::on_resume`
  (:194)resume `ProxyExecutor::execute` (:886) —— 即"销毁发生在 channel-closed
  唤醒的同一轮、写协程 resume 还压在 MPSC 里"。

### 缺陷本体

四个 QUIC awaiter 都用 `loop_->post<T,&T::notify_entry_,&T::on_notify>`(MPSC)投递
resume,而 MPSC 表项**一旦入队不可撤销**:

- `QuicStream::WriteAwaiter`(`src/quic/QuicStream.cpp` :23 起,写/流控挂起)
- `QuicStreamRecvQueue::ReadAwaiter`(`src/quic/QuicStreamRecvQueue.cpp` :28 起,读挂起)
- `QuicConnection::HandshakeAwaiter`(`src/quic/QuicConnection.cpp` :429 起)
- `QuicConnection::LocalStreamAttachAwaiter`(`src/quic/QuicConnection.cpp` :592 起)

协程被硬销毁(when_any 输家、连接拆除等)时,析构只撤了定时器和链表挂靠,队列里的
notify 表项随对象一起悬垂。对比:`Http2HeaderBlockQueue::HeaderReadAwaiter`
(`src/http/detail/Http2HeaderBlockQueue.cpp`)早已是正确写法 —— `post_local` +
`DeferEntry`(可用 `is_in_queue()` 判断、`loop_->cancel` 撤销)。

## 修复

四个 awaiter 统一改成 HeaderReadAwaiter 的模式(以 WriteAwaiter 为例):

```cpp
// 所有完成源(流状态迁移、超时)都发生在连接所属的 loop 上,所以 resume 入
// 可撤销的本地 defer 队列而不是 MPSC notify 队列:被硬销毁的协程(例如响应
// 通道关闭后 when_any 丢弃的代理任务)会在 resume 还在排队时拆掉本 awaiter,
// 而 MPSC 表项无法撤销——loop 随后会弹出并调用已释放的内存。
void post_resume() noexcept {
    if (resume_posted_ || loop_ == nullptr) { return; }
    FIBER_ASSERT(loop_->in_loop());
    resume_posted_ = true;
    loop_->post_local<WriteAwaiter, &WriteAwaiter::resume_entry_, &WriteAwaiter::on_notify>(*this);
}

void cancel_resume() noexcept {
    if (loop_ != nullptr && resume_entry_.is_in_queue()) {
        loop_->cancel<WriteAwaiter, &WriteAwaiter::resume_entry_>(*this);
    }
}
```

成员 `NotifyEntry notify_entry_{}` → `DeferEntry resume_entry_{}`;`cancel_resume()`
加入析构函数和 `await_resume()`(两处出口都要撤)。`post_resume` 断言
`loop_->in_loop()`。

为什么 `post_local` 安全:这些完成都在连接 loop 内触发
(`QuicConnection.cpp` :616 断言连接与 loop 绑定),不存在跨线程投递需求;
`cancel(DeferEntry&)` 是通用侵入式摘除,即便该表项正处于本轮 drain 批次中也安全
(批次是先快照后遍历;摘除只清 in_queue 标记与链指针)。

## 验证

- **ASan 前后对比**:修复前搅拌 ~2 分钟内必现 UAF(上述栈);修复后同型搅拌跑满
  两轮(约 8 分钟)零报告、实例存活。
- **回归测试**:`Http3EndpointTest.DestroyedSuspendedBodyWriterKeepsLoopIntact`
  —— 服务端 handler 以 `when_any(wait_response_channel_closed(), write_body.select())`
  复刻生产形态,客户端读到响应头后 abort 流(RESET_STREAM+STOP_SENDING,64 字节
  流窗口让写协程挂在流控上),断言 loop 事后仍健康(同 server 后续请求 200)。
  注:悬垂 UAF 的确切触发依赖事件循环内部队列顺序,黑盒用例在 Release 下不保证
  每次都踩中,ASan 构建 + abort 搅拌才是确定性口径。
- **Release 全量**:`fiber_tests` 1599 通过 / 1 失败(见附注)/ 1 跳过(需 nginx);
  access-server CTest 标签 371 例:369 通过、1 失败为本机 Python 3.8 < 3.9 的
  cutover gate 既有环境限制。

## 附注(独立发现,非本缺陷)

`Http1EndpointTest.DrainWaitsForRequestBodyAndHandlerLifetime` 在**未打任何补丁的
纯净引脚上同样失败**(`weak.expired()` 为 false:h1 drain 没有等 handler 生命周期
结束)—— 上游既有缺陷。它与 111 上两次 stop 走到 90s 超时被 KILL 的排空悬挂
可能相关,建议上游修复行程里一并排查(建议先跑该用例确认)。

## 上游修复建议

`fiber-net-gateway/fiber-gateway-cpp`(引脚 `dfa5676`):

1. `src/quic/QuicStream.cpp` — `WriteAwaiter`:NotifyEntry→DeferEntry +
   `post_local` + `cancel_resume()`(析构与 `await_resume`);
2. `src/quic/QuicStreamRecvQueue.cpp` — `ReadAwaiter`:同上;
3. `src/quic/QuicConnection.cpp` — `HandshakeAwaiter`(:429)与
   `LocalStreamAttachAwaiter`(:592):同上;
4. `tests/Http3EndpointTest.cpp` — 增加
   `DestroyedSuspendedBodyWriterKeepsLoopIntact` 回归用例。

完整 diff 即 `native/patches/0004-quic-awaiter-destruction-safe-resume.patch`
(4 个文件,含测试),全文如下:

```diff
diff --git a/src/quic/QuicConnection.cpp b/src/quic/QuicConnection.cpp
index 1d54757..a20a2ca 100644
--- a/src/quic/QuicConnection.cpp
+++ b/src/quic/QuicConnection.cpp
@@ -438,6 +438,7 @@ public:
     HandshakeAwaiter &operator=(HandshakeAwaiter &&) = delete;
 
     ~HandshakeAwaiter() {
+        cancel_resume();
         cancel_timer();
         if (connection_ != nullptr) {
             connection_->cancel_handshake_wait(*this);
@@ -491,6 +492,7 @@ public:
 
     common::IoErr await_resume() noexcept {
         common::IoErr result = result_;
+        cancel_resume();
         cancel_timer();
         if (connection_ != nullptr) {
             connection_->cancel_handshake_wait(*this);
@@ -554,19 +556,29 @@ private:
         }
     }
 
+    // Completions fire on the connection's loop; the cancellable local defer
+    // queue keeps this awaiter safe against hard coroutine destruction while a
+    // resume is queued (an MPSC entry cannot be retracted).
     void post_resume() noexcept {
         if (resume_posted_ || loop_ == nullptr) {
             return;
         }
+        FIBER_ASSERT(loop_->in_loop());
         resume_posted_ = true;
-        loop_->post<HandshakeAwaiter, &HandshakeAwaiter::notify_entry_, &HandshakeAwaiter::on_notify>(*this);
+        loop_->post_local<HandshakeAwaiter, &HandshakeAwaiter::resume_entry_, &HandshakeAwaiter::on_notify>(*this);
+    }
+
+    void cancel_resume() noexcept {
+        if (loop_ != nullptr && resume_entry_.is_in_queue()) {
+            loop_->cancel<HandshakeAwaiter, &HandshakeAwaiter::resume_entry_>(*this);
+        }
     }
 
     QuicConnection *connection_ = nullptr;
     std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
     event::EventLoop *loop_ = nullptr;
     std::coroutine_handle<> handle_{};
-    event::EventLoop::NotifyEntry notify_entry_{};
+    event::EventLoop::DeferEntry resume_entry_{};
     event::EventLoop::TimerEntry timer_entry_{};
     common::IntrusiveListHook wait_link_{};
     common::IoErr result_ = common::IoErr::WouldBlock;
@@ -589,6 +601,7 @@ public:
     LocalStreamAttachAwaiter &operator=(LocalStreamAttachAwaiter &&) = delete;
 
     ~LocalStreamAttachAwaiter() {
+        cancel_resume();
         cancel_timer();
         if (connection_ != nullptr) {
             connection_->cancel_local_stream_attach_wait(*this);
@@ -629,6 +642,7 @@ public:
 
     common::IoErr await_resume() noexcept {
         common::IoErr result = result_;
+        cancel_resume();
         cancel_timer();
         if (connection_ != nullptr) {
             connection_->cancel_local_stream_attach_wait(*this);
@@ -700,13 +714,23 @@ private:
         awaiter->complete(common::IoErr::TimedOut);
     }
 
+    // Completions fire on the connection's loop; the cancellable local defer
+    // queue keeps this awaiter safe against hard coroutine destruction while a
+    // resume is queued (an MPSC entry cannot be retracted).
     void post_resume() noexcept {
         if (resume_posted_ || loop_ == nullptr) {
             return;
         }
+        FIBER_ASSERT(loop_->in_loop());
         resume_posted_ = true;
-        loop_->post<LocalStreamAttachAwaiter, &LocalStreamAttachAwaiter::notify_entry_,
-                    &LocalStreamAttachAwaiter::on_notify>(*this);
+        loop_->post_local<LocalStreamAttachAwaiter, &LocalStreamAttachAwaiter::resume_entry_,
+                          &LocalStreamAttachAwaiter::on_notify>(*this);
+    }
+
+    void cancel_resume() noexcept {
+        if (loop_ != nullptr && resume_entry_.is_in_queue()) {
+            loop_->cancel<LocalStreamAttachAwaiter, &LocalStreamAttachAwaiter::resume_entry_>(*this);
+        }
     }
 
     QuicConnection *connection_ = nullptr;
@@ -714,7 +738,7 @@ private:
     std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
     event::EventLoop *loop_ = nullptr;
     std::coroutine_handle<> handle_{};
-    event::EventLoop::NotifyEntry notify_entry_{};
+    event::EventLoop::DeferEntry resume_entry_{};
     event::EventLoop::TimerEntry timer_entry_{};
     common::IntrusiveListHook wait_link_{};
     common::IoErr result_ = common::IoErr::None;
diff --git a/src/quic/QuicStream.cpp b/src/quic/QuicStream.cpp
index 08d72ed..db2ff40 100644
--- a/src/quic/QuicStream.cpp
+++ b/src/quic/QuicStream.cpp
@@ -31,6 +31,7 @@ public:
     WriteAwaiter &operator=(WriteAwaiter &&) = delete;
 
     ~WriteAwaiter() {
+        cancel_resume();
         cancel_timer();
         unlink_connection_window_wait();
         if (stream_ != nullptr) {
@@ -78,6 +79,7 @@ public:
 
     common::IoErr await_resume() noexcept {
         common::IoErr result = result_;
+        cancel_resume();
         cancel_timer();
         unlink_connection_window_wait();
         if (stream_ != nullptr && stream_->write_waiter_ == this) {
@@ -173,19 +175,33 @@ private:
         awaiter->complete(common::IoErr::TimedOut);
     }
 
+    // All completion sources (stream state transitions, timeouts) fire on the
+    // connection's loop, so the resume is queued on the cancellable local defer
+    // queue rather than the MPSC notify queue: a hard-destroyed coroutine (for
+    // example a proxy task discarded by when_any after the response channel
+    // closed) tears this awaiter down while the resume is still queued, and an
+    // MPSC entry cannot be retracted — the loop would later pop and call into
+    // freed memory.
     void post_resume() noexcept {
         if (resume_posted_ || loop_ == nullptr) {
             return;
         }
+        FIBER_ASSERT(loop_->in_loop());
         resume_posted_ = true;
-        loop_->post<WriteAwaiter, &WriteAwaiter::notify_entry_, &WriteAwaiter::on_notify>(*this);
+        loop_->post_local<WriteAwaiter, &WriteAwaiter::resume_entry_, &WriteAwaiter::on_notify>(*this);
+    }
+
+    void cancel_resume() noexcept {
+        if (loop_ != nullptr && resume_entry_.is_in_queue()) {
+            loop_->cancel<WriteAwaiter, &WriteAwaiter::resume_entry_>(*this);
+        }
     }
 
     QuicStream *stream_ = nullptr;
     std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
     event::EventLoop *loop_ = nullptr;
     std::coroutine_handle<> handle_{};
-    event::EventLoop::NotifyEntry notify_entry_{};
+    event::EventLoop::DeferEntry resume_entry_{};
     event::EventLoop::TimerEntry timer_entry_{};
     common::IntrusiveListHook peer_data_wait_link_{};
     common::IoErr result_ = common::IoErr::None;
diff --git a/src/quic/QuicStreamRecvQueue.cpp b/src/quic/QuicStreamRecvQueue.cpp
index 1d9b0db..cf97a35 100644
--- a/src/quic/QuicStreamRecvQueue.cpp
+++ b/src/quic/QuicStreamRecvQueue.cpp
@@ -36,6 +36,7 @@ public:
     ReadAwaiter &operator=(ReadAwaiter &&) = delete;
 
     ~ReadAwaiter() {
+        cancel_resume();
         cancel_timer();
         if (queue_ != nullptr) {
             queue_->cancel_read_waiter(this);
@@ -81,6 +82,7 @@ public:
 
     common::IoErr await_resume() noexcept {
         common::IoErr result = result_;
+        cancel_resume();
         cancel_timer();
         if (queue_ != nullptr && queue_->read_waiter_ == this) {
             queue_->read_waiter_ = nullptr;
@@ -145,19 +147,29 @@ private:
         awaiter->complete(common::IoErr::TimedOut);
     }
 
+    // Completions fire on the connection's loop; the cancellable local defer
+    // queue keeps this awaiter safe against hard coroutine destruction while a
+    // resume is queued (an MPSC entry cannot be retracted).
     void post_resume() noexcept {
         if (resume_posted_ || loop_ == nullptr) {
             return;
         }
+        FIBER_ASSERT(loop_->in_loop());
         resume_posted_ = true;
-        loop_->post<ReadAwaiter, &ReadAwaiter::notify_entry_, &ReadAwaiter::on_notify>(*this);
+        loop_->post_local<ReadAwaiter, &ReadAwaiter::resume_entry_, &ReadAwaiter::on_notify>(*this);
+    }
+
+    void cancel_resume() noexcept {
+        if (loop_ != nullptr && resume_entry_.is_in_queue()) {
+            loop_->cancel<ReadAwaiter, &ReadAwaiter::resume_entry_>(*this);
+        }
     }
 
     QuicStreamRecvQueue *queue_ = nullptr;
     std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
     event::EventLoop *loop_ = nullptr;
     std::coroutine_handle<> handle_{};
-    event::EventLoop::NotifyEntry notify_entry_{};
+    event::EventLoop::DeferEntry resume_entry_{};
     event::EventLoop::TimerEntry timer_entry_{};
     common::IoErr result_ = common::IoErr::None;
     bool resume_posted_ = false;
diff --git a/tests/Http3EndpointTest.cpp b/tests/Http3EndpointTest.cpp
index 42cae39..e1271da 100644
--- a/tests/Http3EndpointTest.cpp
+++ b/tests/Http3EndpointTest.cpp
@@ -13,11 +13,15 @@
 #include <fiber/async/Sleep.h>
 #include <fiber/async/Spawn.h>
 #include <fiber/async/Task.h>
+#include <fiber/async/TaskSelect.h>
+#include <fiber/async/WhenAny.h>
 #include <fiber/common/mem/BufPool.h>
 #include <fiber/event/EventLoopGroup.h>
 #include <fiber/http/ClientHttp3Exchange.h>
 #include <fiber/http/Http3Client.h>
+#include <fiber/http/HttpBodyPipe.h>
 #include <fiber/http/HttpExchange.h>
+#include <fiber/http/HttpResponseWriter.h>
 #include <fiber/http/HttpHeaders.h>
 #include <fiber/http/Server.h>
 #include <fiber/http/endpoint/Http1Endpoint.h>
@@ -82,7 +86,8 @@ struct ClientResult {
 // a client is still attached.
 DetachedTask run_http3_client(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
                               std::string cert_path, std::promise<ClientResult> *promise,
-                              std::shared_ptr<std::promise<void>> done_first = {}, std::shared_future<void> hold = {}) {
+                              std::shared_ptr<std::promise<void>> done_first = {}, std::shared_future<void> hold = {},
+                              std::uint64_t request_stream_window = 0) {
     ClientResult result{};
     fiber::quic::QuicUdpEndpoint endpoint;
     fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
@@ -126,6 +131,9 @@ DetachedTask run_http3_client(fiber::event::EventLoop *loop, fiber::net::SocketA
     connect_options.remote_addr = server_addr;
     connect_options.server_name = "localhost";
     connect_options.handshake_timeout = 3s;
+    if (request_stream_window != 0) {
+        connect_options.transport.initial_max_stream_data_bidi_local = request_stream_window;
+    }
     auto connected = co_await client.connect(std::move(connect_options));
     if (!connected) {
         result.error = connected.error().io_error;
@@ -187,6 +195,103 @@ DetachedTask run_http3_client(fiber::event::EventLoop *loop, fiber::net::SocketA
     co_return;
 }
 
+// Client that reads the response head and then aborts the stream without
+// draining the body (RESET_STREAM + STOP_SENDING, the browser-abort shape).
+// Combined with a tiny initial stream window this leaves the server's body
+// writer suspended on flow control when the send-abort arrives.
+DetachedTask run_http3_client_close_after_header(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
+                                                 std::string cert_path, std::promise<ClientResult> *promise,
+                                                 std::uint64_t request_stream_window) {
+    ClientResult result{};
+    fiber::quic::QuicUdpEndpoint endpoint;
+    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
+    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
+    auto endpoint_ready = endpoint.init(*loop, endpoint_options);
+    if (!endpoint_ready) {
+        result.error = endpoint_ready.error();
+        promise->set_value(std::move(result));
+        co_return;
+    }
+
+    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(cert_path));
+    if (!trust_store) {
+        result.error = trust_store.error();
+        endpoint.close();
+        promise->set_value(std::move(result));
+        co_return;
+    }
+
+    fiber::http::Http3Client::Options client_options{};
+    client_options.tls.trust_store = trust_store->get();
+    client_options.tls.verify_peer = true;
+    fiber::http::Http3Client client(endpoint, std::move(client_options));
+
+    auto started = endpoint.start();
+    if (!started) {
+        result.error = started.error();
+        endpoint.close();
+        promise->set_value(std::move(result));
+        co_return;
+    }
+    auto initialized = client.init();
+    if (!initialized) {
+        result.error = initialized.error();
+        endpoint.close();
+        promise->set_value(std::move(result));
+        co_return;
+    }
+
+    fiber::http::Http3ClientConnectOptions connect_options{};
+    connect_options.remote_addr = server_addr;
+    connect_options.server_name = "localhost";
+    connect_options.handshake_timeout = 3s;
+    connect_options.transport.initial_max_stream_data_bidi_local = request_stream_window;
+    auto connected = co_await client.connect(std::move(connect_options));
+    if (!connected) {
+        result.error = connected.error().io_error;
+        endpoint.close();
+        promise->set_value(std::move(result));
+        co_return;
+    }
+    result.connected = true;
+
+    {
+        fiber::mem::BufPool pool;
+        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
+        auto sent = co_await exchange.send_request_header(
+                {
+                        .method = fiber::http::HttpMethod::Get,
+                        .scheme = "https",
+                        .authority = "localhost",
+                        .path = "/h3",
+                },
+                true, 3s);
+        if (!sent) {
+            result.error = sent.error();
+        } else {
+            auto head = co_await exchange.read_header(5s);
+            if (!head || *head == nullptr) {
+                result.error = head ? fiber::common::IoErr::Invalid : head.error();
+            } else {
+                result.status = (*head)->status_code;
+                // Abort the response stream (RESET_STREAM + STOP_SENDING). The
+                // server observes send-aborted while its writer is suspended on
+                // the tiny stream window: the production browser-abort shape,
+                // where the channel-closed resume is queued ahead of the
+                // suspended writer's own completion.
+                (void) exchange.abort(fiber::common::IoErr::Canceled);
+            }
+        }
+    }
+
+    // Keep the connection alive long enough for the abort frames to reach the
+    // server and its teardown to run, then vanish.
+    co_await fiber::async::sleep(50ms);
+    endpoint.close();
+    promise->set_value(std::move(result));
+    co_return;
+}
+
 struct RunningServer {
     std::unique_ptr<Server> server{};
     Http3Endpoint *endpoint = nullptr;
@@ -299,6 +404,479 @@ TEST(Http3EndpointTest, ServesHttp3Requests) {
     client_group.join();
 }
 
+// A proxied streamed response (no Content-Length) sends the body through
+// HttpExchange::write with the completion marker riding the final chain, the
+// way http::pipe_http_body drives its sink. The exchange must observe the
+// marker consumed and record the response as completed.
+TEST(Http3EndpointTest, StreamedAutoBodyViaChainWriteCompletes) {
+    TestCredential tls;
+    ASSERT_TRUE(tls.init());
+
+    fiber::event::EventLoopGroup group(1);
+    fiber::event::EventLoopGroup client_group(1);
+    group.start();
+    client_group.start();
+
+    std::atomic<fiber::common::IoErr> header_error{fiber::common::IoErr::None};
+    std::atomic<fiber::common::IoErr> write_error{fiber::common::IoErr::None};
+    std::atomic<bool> write_ok{false};
+    std::atomic<bool> stats_completed{false};
+    std::atomic<fiber::common::IoErr> stats_terminal{fiber::common::IoErr::None};
+
+    RunningServer running;
+    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
+    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
+            .address = {fiber::net::IpAddress::loopback_v4(), 0},
+            .tls = tls_options(*tls.credential),
+            .handler =
+                    [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
+                        fiber::http::HttpHeaders headers(exchange.pool());
+                        headers.set("content-type", "application/json");
+                        auto sent = co_await exchange.send_header({
+                                .kind = fiber::http::OutgoingHeaderKind::Final,
+                                .status_code = 200,
+                                .headers = &headers,
+                                .body = fiber::http::HttpBodySpec::Auto(),
+                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
+                                .end_stream = false,
+                        }, 2s);
+                        header_error.store(sent ? fiber::common::IoErr::None : sent.error());
+                        if (!sent) {
+                            co_return;
+                        }
+
+                        fiber::mem::IoBufChain chain(fiber::event::EventLoop::current().io_buf_node_pool());
+                        fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(136);
+                        body.commit(136);
+                        chain.append(std::move(body));
+                        chain.mark_complete();
+
+                        auto written = co_await exchange.write(chain, 2s);
+                        write_ok.store(written.has_value());
+                        write_error.store(written ? fiber::common::IoErr::None : written.error());
+                        stats_completed.store(exchange.response_stats().completed);
+                        stats_terminal.store(exchange.response_stats().terminal_error);
+                        co_return;
+                    },
+    });
+    ASSERT_NE(running.endpoint, nullptr);
+    ASSERT_TRUE(running.server->start().has_value());
+    spawn_serve(running, group.at(0));
+
+    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
+    ASSERT_NE(server_addr.port(), 0);
+
+    std::promise<ClientResult> promise;
+    auto future = promise.get_future();
+    const std::string cert_path = tls.cert.path();
+    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
+        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
+    });
+
+    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
+    ClientResult result = future.get();
+    EXPECT_EQ(result.error, fiber::common::IoErr::None);
+    EXPECT_EQ(result.status, 200);
+    EXPECT_EQ(result.body.size(), 136u);
+
+    EXPECT_EQ(header_error.load(), fiber::common::IoErr::None);
+    EXPECT_TRUE(write_ok.load());
+    EXPECT_EQ(write_error.load(), fiber::common::IoErr::None);
+    EXPECT_TRUE(stats_completed.load());
+    EXPECT_EQ(stats_terminal.load(), fiber::common::IoErr::None);
+
+    running.stop_and_join();
+    EXPECT_EQ(running.server->state(), Server::State::Stopped);
+
+    group.stop();
+    group.join();
+    client_group.stop();
+    client_group.join();
+}
+
+// The same streamed response, but driven through http::pipe_http_body exactly
+// the way ProxyExecutor forwards a proxied body, with the unbuffered low-water
+// that turns the upstream's separately-delivered terminator into a terminal-
+// only write (empty complete chain). The h3 sink consumes the chain's
+// completion marker on that write; failing to (as the HTTP/2 sink did) trips
+// the pipe's completion-progress invariant.
+TEST(Http3EndpointTest, StreamedAutoBodyThroughPipeCompletes) {
+    TestCredential tls;
+    ASSERT_TRUE(tls.init());
+
+    fiber::event::EventLoopGroup group(1);
+    fiber::event::EventLoopGroup client_group(1);
+    group.start();
+    client_group.start();
+
+    std::atomic<fiber::common::IoErr> pipe_error{fiber::common::IoErr::None};
+    std::atomic<bool> pipe_ok{false};
+    std::atomic<bool> stats_completed{false};
+    std::atomic<fiber::common::IoErr> stats_terminal{fiber::common::IoErr::None};
+
+    RunningServer running;
+    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
+    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
+            .address = {fiber::net::IpAddress::loopback_v4(), 0},
+            .tls = tls_options(*tls.credential),
+            .handler =
+                    [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
+                        fiber::http::HttpHeaders headers(exchange.pool());
+                        headers.set("content-type", "application/json");
+                        auto sent = co_await exchange.send_header({
+                                .kind = fiber::http::OutgoingHeaderKind::Final,
+                                .status_code = 200,
+                                .headers = &headers,
+                                .body = fiber::http::HttpBodySpec::Auto(),
+                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
+                                .end_stream = false,
+                        }, 2s);
+                        if (!sent) {
+                            co_return;
+                        }
+
+                        struct FixedSource {
+                            fiber::mem::IoBufChain reads[2];
+                            unsigned served = 0;
+                            fiber::common::IoErr abort_error{fiber::common::IoErr::None};
+
+                            fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
+                            read_body(std::size_t max_bytes, std::chrono::milliseconds) noexcept {
+                                if (served >= 2) {
+                                    co_return std::unexpected(fiber::common::IoErr::Invalid);
+                                }
+                                if (reads[served].readable_bytes() > max_bytes) {
+                                    co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
+                                }
+                                co_return std::move(reads[served++]);
+                            }
+                            fiber::common::IoResult<void> abort(fiber::common::IoErr reason) noexcept {
+                                abort_error = reason;
+                                return {};
+                            }
+                        };
+                        FixedSource source{fiber::mem::IoBufChain(
+                                                   fiber::event::EventLoop::current().io_buf_node_pool()),
+                                           fiber::mem::IoBufChain(
+                                                   fiber::event::EventLoop::current().io_buf_node_pool())};
+                        fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(136);
+                        body.commit(136);
+                        source.reads[0].append(std::move(body));
+                        source.reads[1].mark_complete();
+
+                        fiber::http::HttpResponseWriter writer = fiber::http::make_http_response_writer(exchange);
+                        const fiber::http::HttpBodyPipeOptions pipe_options{
+                                .buffer_size = 64 * 1024,
+                                .low_water = fiber::http::kUnbufferedBodyPipeLowWater,
+                                .read_timeout = std::chrono::milliseconds::max(),
+                                .write_timeout = 2s,
+                        };
+                        auto piped = co_await fiber::http::pipe_http_body(
+                                fiber::http::make_http_body_pipe_reader(source),
+                                fiber::http::make_http_body_pipe_writer(writer),
+                                fiber::event::EventLoop::current().io_buf_node_pool(), pipe_options);
+                        pipe_ok.store(piped.has_value());
+                        pipe_error.store(piped ? fiber::common::IoErr::None : piped.error().code);
+                        stats_completed.store(exchange.response_stats().completed);
+                        stats_terminal.store(exchange.response_stats().terminal_error);
+                        co_return;
+                    },
+    });
+    ASSERT_NE(running.endpoint, nullptr);
+    ASSERT_TRUE(running.server->start().has_value());
+    spawn_serve(running, group.at(0));
+
+    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
+    ASSERT_NE(server_addr.port(), 0);
+
+    std::promise<ClientResult> promise;
+    auto future = promise.get_future();
+    const std::string cert_path = tls.cert.path();
+    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
+        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
+    });
+
+    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
+    ClientResult result = future.get();
+    EXPECT_EQ(result.error, fiber::common::IoErr::None);
+    EXPECT_EQ(result.status, 200);
+    EXPECT_EQ(result.body.size(), 136u);
+
+    EXPECT_TRUE(pipe_ok.load());
+    EXPECT_EQ(pipe_error.load(), fiber::common::IoErr::None);
+    EXPECT_TRUE(stats_completed.load());
+    EXPECT_EQ(stats_terminal.load(), fiber::common::IoErr::None);
+
+    running.stop_and_join();
+    EXPECT_EQ(running.server->state(), Server::State::Stopped);
+
+    group.stop();
+    group.join();
+    client_group.stop();
+    client_group.join();
+}
+
+TEST(Http3EndpointTest, StreamedAutoBodyWithTinyRequestStreamWindow) {
+    TestCredential tls;
+    ASSERT_TRUE(tls.init());
+
+    fiber::event::EventLoopGroup group(1);
+    fiber::event::EventLoopGroup client_group(1);
+    group.start();
+    client_group.start();
+
+    std::atomic<fiber::common::IoErr> pipe_error{fiber::common::IoErr::None};
+    std::atomic<bool> pipe_ok{false};
+    std::atomic<bool> stats_completed{false};
+    std::atomic<fiber::common::IoErr> stats_terminal{fiber::common::IoErr::None};
+
+    RunningServer running;
+    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
+    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
+            .address = {fiber::net::IpAddress::loopback_v4(), 0},
+            .tls = tls_options(*tls.credential),
+            .handler =
+                    [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
+                        fiber::http::HttpHeaders headers(exchange.pool());
+                        headers.set("content-type", "application/json");
+                        auto sent = co_await exchange.send_header({
+                                .kind = fiber::http::OutgoingHeaderKind::Final,
+                                .status_code = 200,
+                                .headers = &headers,
+                                .body = fiber::http::HttpBodySpec::Auto(),
+                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
+                                .end_stream = false,
+                        }, 2s);
+                        if (!sent) {
+                            co_return;
+                        }
+
+                        struct FixedSource {
+                            fiber::mem::IoBufChain reads[2];
+                            unsigned served = 0;
+                            fiber::common::IoErr abort_error{fiber::common::IoErr::None};
+
+                            fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
+                            read_body(std::size_t max_bytes, std::chrono::milliseconds) noexcept {
+                                if (served >= 2) {
+                                    co_return std::unexpected(fiber::common::IoErr::Invalid);
+                                }
+                                if (reads[served].readable_bytes() > max_bytes) {
+                                    co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
+                                }
+                                co_return std::move(reads[served++]);
+                            }
+                            fiber::common::IoResult<void> abort(fiber::common::IoErr reason) noexcept {
+                                abort_error = reason;
+                                return {};
+                            }
+                        };
+                        FixedSource source{fiber::mem::IoBufChain(
+                                                   fiber::event::EventLoop::current().io_buf_node_pool()),
+                                           fiber::mem::IoBufChain(
+                                                   fiber::event::EventLoop::current().io_buf_node_pool())};
+                        fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(136);
+                        body.commit(136);
+                        source.reads[0].append(std::move(body));
+                        source.reads[1].mark_complete();
+
+                        fiber::http::HttpResponseWriter writer = fiber::http::make_http_response_writer(exchange);
+                        const fiber::http::HttpBodyPipeOptions pipe_options{
+                                .buffer_size = 64 * 1024,
+                                .low_water = std::min<std::size_t>(64 * 1024,
+                                                                  fiber::http::kDefaultBodyPipeLowWater),
+                                .read_timeout = std::chrono::milliseconds::max(),
+                                .write_timeout = 2s,
+                        };
+                        auto piped = co_await fiber::http::pipe_http_body(
+                                fiber::http::make_http_body_pipe_reader(source),
+                                fiber::http::make_http_body_pipe_writer(writer),
+                                fiber::event::EventLoop::current().io_buf_node_pool(), pipe_options);
+                        pipe_ok.store(piped.has_value());
+                        pipe_error.store(piped ? fiber::common::IoErr::None : piped.error().code);
+                        stats_completed.store(exchange.response_stats().completed);
+                        stats_terminal.store(exchange.response_stats().terminal_error);
+                        co_return;
+                    },
+    });
+    ASSERT_NE(running.endpoint, nullptr);
+    ASSERT_TRUE(running.server->start().has_value());
+    spawn_serve(running, group.at(0));
+
+    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
+    ASSERT_NE(server_addr.port(), 0);
+
+    std::promise<ClientResult> promise;
+    auto future = promise.get_future();
+    const std::string cert_path = tls.cert.path();
+    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
+        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise, {}, {}, 64);
+    });
+
+    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
+    ClientResult result = future.get();
+    EXPECT_EQ(result.error, fiber::common::IoErr::None);
+    EXPECT_EQ(result.status, 200);
+    EXPECT_EQ(result.body.size(), 136u);
+
+    EXPECT_TRUE(pipe_ok.load());
+    EXPECT_EQ(pipe_error.load(), fiber::common::IoErr::None);
+    EXPECT_TRUE(stats_completed.load());
+    EXPECT_EQ(stats_terminal.load(), fiber::common::IoErr::None);
+
+    running.stop_and_join();
+    EXPECT_EQ(running.server->state(), Server::State::Stopped);
+
+    group.stop();
+    group.join();
+    client_group.stop();
+    client_group.join();
+}
+
+// A handler whose body writer is hard-destroyed while suspended on flow
+// control (the production shape: when_any(response-channel-closed, proxy
+// task) discards the loser after the peer vanished mid-response). The QUIC
+// write awaiter must retract every loop registration it armed, including a
+// queued resume, or the event loop later pops and calls into freed memory
+// (heap-use-after-free in MpscQueue::try_pop_all, wild jump in release).
+TEST(Http3EndpointTest, DestroyedSuspendedBodyWriterKeepsLoopIntact) {
+    TestCredential tls;
+    ASSERT_TRUE(tls.init());
+
+    fiber::event::EventLoopGroup group(1);
+    fiber::event::EventLoopGroup client_group(1);
+    group.start();
+    client_group.start();
+
+    std::atomic<bool> writer_finished{false};
+    std::atomic<bool> closed_path_taken{false};
+
+    RunningServer running;
+    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
+    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
+            .address = {fiber::net::IpAddress::loopback_v4(), 0},
+            .tls = tls_options(*tls.credential),
+            .handler =
+                    [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
+                        fiber::http::HttpHeaders headers(exchange.pool());
+                        headers.set("content-type", "application/json");
+                        auto sent = co_await exchange.send_header({
+                                .kind = fiber::http::OutgoingHeaderKind::Final,
+                                .status_code = 200,
+                                .headers = &headers,
+                                .body = fiber::http::HttpBodySpec::Auto(),
+                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
+                                .end_stream = false,
+                        }, 2s);
+                        if (!sent) {
+                            co_return;
+                        }
+
+                        auto write_body = [](fiber::http::HttpExchange &exchange,
+                                             std::atomic<bool> *finished) -> fiber::async::Task<void> {
+                            struct FixedSource {
+                                explicit FixedSource(fiber::mem::IoBufNodePool &pool) :
+                                    reads{fiber::mem::IoBufChain(pool), fiber::mem::IoBufChain(pool),
+                                          fiber::mem::IoBufChain(pool)} {}
+                                fiber::mem::IoBufChain reads[3];
+                                unsigned served = 0;
+                                fiber::common::IoResult<void> abort(fiber::common::IoErr) noexcept { return {}; }
+                                fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
+                                read_body(std::size_t max_bytes, std::chrono::milliseconds) noexcept {
+                                    if (served >= 3) {
+                                        co_return fiber::mem::IoBufChain{};
+                                    }
+                                    if (reads[served].readable_bytes() > max_bytes) {
+                                        co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
+                                    }
+                                    co_return std::move(reads[served++]);
+                                }
+                            };
+                            // 8 KiB total against a 64-byte peer stream window: the
+                            // first write suspends inside QuicStream::write.
+                            FixedSource source(fiber::event::EventLoop::current().io_buf_node_pool());
+                            for (auto &read : source.reads) {
+                                fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(2731);
+                                body.commit(2731);
+                                read.append(std::move(body));
+                            }
+                            source.reads[2].mark_complete();
+
+                            fiber::http::HttpResponseWriter writer = fiber::http::make_http_response_writer(exchange);
+                            const fiber::http::HttpBodyPipeOptions pipe_options{
+                                    .buffer_size = 64 * 1024,
+                                    .low_water = fiber::http::kUnbufferedBodyPipeLowWater,
+                                    .read_timeout = std::chrono::milliseconds::max(),
+                                    .write_timeout = 5s,
+                            };
+                            auto piped = co_await fiber::http::pipe_http_body(
+                                    fiber::http::make_http_body_pipe_reader(source),
+                                    fiber::http::make_http_body_pipe_writer(writer),
+                                    fiber::event::EventLoop::current().io_buf_node_pool(), pipe_options);
+                            (void)piped;
+                            finished->store(true);
+                        };
+
+                        auto completed = co_await fiber::async::when_any(
+                                [&exchange]() { return exchange.wait_response_channel_closed(); },
+                                [&exchange, &write_body, &writer_finished]() {
+                                    return write_body(exchange, &writer_finished).select();
+                                });
+                        closed_path_taken.store(completed.is<0>());
+                        co_return;
+                    },
+    });
+    ASSERT_NE(running.endpoint, nullptr);
+    ASSERT_TRUE(running.server->start().has_value());
+    spawn_serve(running, group.at(0));
+
+    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
+    ASSERT_NE(server_addr.port(), 0);
+
+    std::promise<ClientResult> promise;
+    auto future = promise.get_future();
+    const std::string cert_path = tls.cert.path();
+    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
+        return run_http3_client_close_after_header(&client_group.at(0), server_addr, cert_path, &promise, 64);
+    });
+
+    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
+    ClientResult result = future.get();
+    EXPECT_EQ(result.error, fiber::common::IoErr::None);
+    EXPECT_EQ(result.status, 200);
+
+    // The send-abort races the suspended writer's BrokenPipe completion
+    // through the loop's queues; either side may win depending on the drain
+    // interleaving. What must not happen is the loop touching freed memory
+    // when the when_any winner hard-destroys the loser while its write resume
+    // is still queued: the follow-up request below checks loop health (and,
+    // under ASan, the allocator checks the rest — the churn repro that found
+    // this defect reproduces it deterministically enough there).
+    EXPECT_TRUE(closed_path_taken.load() || writer_finished.load());
+
+    // The loop must still be healthy after the destroyed writer: a follow-up
+    // request through the same server completes normally.
+    {
+        std::promise<ClientResult> followup;
+        auto followup_future = followup.get_future();
+        fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &followup]() {
+            return run_http3_client(&client_group.at(0), server_addr, cert_path, &followup);
+        });
+        ASSERT_EQ(followup_future.wait_for(15s), std::future_status::ready);
+        ClientResult followup_result = followup_future.get();
+        EXPECT_EQ(followup_result.error, fiber::common::IoErr::None);
+        EXPECT_EQ(followup_result.status, 200);
+    }
+
+    running.stop_and_join();
+    EXPECT_EQ(running.server->state(), Server::State::Stopped);
+
+    group.stop();
+    group.join();
+    client_group.stop();
+    client_group.join();
+}
+
 // Closing the UDP socket the moment shutdown begins would
 // killed every session outright. Draining now keeps the socket open until the
 // live sessions finish.

```

以上 diff 与 `native/patches/0004-quic-awaiter-destruction-safe-resume.patch` 相同,
`git apply` 于 `dfa5676` 干净检出即可。
