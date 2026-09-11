# Fiber lib ASan 全量扫描报告

记录日期:2026-09-11;行号基于 `fiber-gateway-cpp` @ `e9a8040`(clean master)。

**状态:扫描完成。1 处测试自身 teardown 泄露(库稳态无泄露)——**已修**;
新发现 2 个产品级内存缺陷未修(Http1 registry drain UAF、H3/QUIC 析构
顺序契约);2 个命中项归属既有已知缺陷文档;1 个测试 harness UAF 待查。**

## 构建与规模

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/clang-20 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-20 \
  -DFIBER_USE_LIBCXX=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -stdlib=libc++ -isystem /usr/lib/llvm-20/include/c++/v1" \
  -DFIBER_DEPS_DIR=$PWD/temp/_deps_asan \
  -DFIBER_ENABLE_LTO=OFF \
  -DFIBER_STATIC_LIBCXX=OFF
ASAN_OPTIONS="detect_leaks=1:detect_odr_violation=0" ctest --test-dir build-asan -j8 --timeout 180 --output-on-failure
```

结果:**1892 个用例,1873 通过 / 19 失败**。1873 个通过用例进程退出时零
LeakSanitizer 报告(覆盖 server/连接池/DNS/QUIC/script 全模块)。

构建坑(复现必读):

1. **独立 `FIBER_DEPS_DIR` 强制**——共享 `temp/_deps` 会被 ASan 标志重编
   BoringSSL 等依赖,污染后续 Release 链接(同 `fiber-lib-quic-awaiters-not-destruction-safe.md`)。
2. 命令行传 `CMAKE_CXX_FLAGS` 会以缓存值顶掉 toolchain 经
   `CMAKE_CXX_FLAGS_INIT` 注入的 libc++ 头路径,`<expected>` 直接找不到;
   必须手动带上 `-stdlib=libc++ -isystem /usr/lib/llvm-20/include/c++/v1`。
3. 默认静态 libc++ 下 `fiber_prometheus_tests` 链接失败:
   `libgtest.a` 排在静态 `libc++.a` 之后,`std::__1::__next_prime`
   仅在前序对象恰好引用它时才会被抽出(链接顺序脆弱,`fiber_tests`
   侥幸过、prometheus 侥幸挂)。加 `-DFIBER_STATIC_LIBCXX=OFF` 用共享
   libc++ 规避。这是 SelectToolchain 静态 libc++ 接线的一个潜在独立问题。

## 失败分类总览

| 类别 | 用例数 | 归属 |
|---|---|---|
| LeakSanitizer(测试 teardown 泄露) | 1 | 测试自身,见下 |
| Http1 registry drain UAF | 7 | **产品缺陷(未修)** |
| H3/QUIC 析构顺序 stack-use-after-scope | 6 | **产品缺陷(未修)** |
| QuicCryptoBlockPool::release UAF | 2 | 既有已知,见 `fiber-lib-h3-client-teardown-uaf.md`(暂不修) |
| 池测试协程帧 UAF | 1 | 测试 harness lifetime,待查 |
| ASan 时序 flaky(断言失败,无内存错误) | 2 | 非内存问题 |

## 内存泄露:结论与唯一命中

**库稳态路径无泄露。**唯一 LSan 报告:

```
HttpServerLifecycleTest.ShutdownClosesAnIdleHttp2Connection
ERROR: LeakSanitizer: detected memory leaks
SUMMARY: AddressSanitizer: 10272 byte(s) leaked in 10 allocation(s).
```

测试本体 PASSED,exit code 23 由 LSan 置位。10 块**全部 Indirect、无
Direct root**——引用环,成环双方互相钉死:

| 块 | 内容 | 分配点 |
|---|---|---|
| 1296 B | `Http2ClientConnection`(make_shared 控制块+对象) | tests/HttpServerLifecycleTest.cpp:257 |
| 96 B | `watch_http2_client_close` 协程帧(持上述 shared_ptr) | 同上 |
| 64 B | `wait_closed()` 协程帧 | src/http/Http2ClientConnection.cpp:130 |
| 144 B | `Http2CloseGate::join()` 协程帧 | src/http/Http2CloseGate.cpp:127 |
| 256 B | `TlsTransport` | `Http2ClientConnection::connect_impl` |
| 4096 B ×2 | HPACK 解码表 | src/http/Http2HpackDecodeTable.cpp:20/31 |
| 128 B | `Http2StreamTable::rehash` 桶 | — |
| 64 B | `open_idle_http2_client` spawn entry | — |
| 32 B | `RWFd::WaitAwaiter` 读等待节点 | — |

### 根因:测试 teardown 抛弃挂起协程

测试收尾顺序(tests/HttpServerLifecycleTest.cpp:362-374):

```cpp
co_await server.stop_and_wait();     // 服务端关停完成,断言通过
closed_promise.set_value();
...
group.stop();                        // 373:直接停 loop
group.join();                        // 374
```

`open_idle_http2_client` 此刻仍挂在 keepalive 读上(RWFd 等待节点),
`watch_http2_client_close` 仍挂在 `connection->wait_closed()`;测试既不
等待其 `state->done`,也没有任何取消路径。`group.stop()/join()` 后这些
协程帧永久悬置:帧持 `shared_ptr<Http2ClientConnection>` → 连接、TLS
transport、HPACK 表整簇不可达,约 10 KB/连接。

判定:**测试卫生问题,非库缺陷**。生产场景 loop 与进程同生命周期,进程
退出由 OS 回收。

### 修复(2026-09-11 已落地)

`Http2LifecycleRunState`(done/err 原子量)换成 `std::promise<IoErr>`:
`watch_http2_client_close` 在 `wait_closed()` 返回时 `set_value`(连接
终端原因);`open_idle_http2_client` 增加透传参数;测试在
`server.stop_and_wait()` 断言之后、`group.stop()`(原 373 行)之前增加:

```cpp
EXPECT_EQ(client_close_future.wait_for(5s), std::future_status::ready);
```

等待期间 loop 仍在运行,客户端读泵观察到服务端关闭 → close gate 完成
→ `wait_closed()` 返回 → watcher 帧走完并析构 → `shared_ptr` 释放 →
整簇(连接、TLS transport、HPACK 表、流表桶、协程帧)正常回收。

红→绿验证:修复前 LSan 报 10272 B/10 块、ctest Failed(exit 23);修复后
单用例 ASan 构建零 LSan 输出、exit 0。套件逐用例复跑:同套件
`ShutdownClosesAnIdleHttp1Connection` 仍因缺陷 1 的 heap-use-after-free
失败(预期,与本修复无关),其余全绿;Release 增量构建零告警通过、
用例通过。

## 缺陷 1(新):Http1 连接注册表 drain 读已释放节点

**严重度:HIGH(服务端关停路径确定性 UB,Release 下静默)。**

7 个用例 100% 命中:Http1EndpointTest.DrainClosesAnIdleKeepAliveConnection、
Http1EndpointTest.ConnectionsSpreadOverWorkerLoops、
HttpServerLifecycleTest.ShutdownClosesAnIdleHttp1Connection、
RouteVarTest ×4、StealableHttp1ConnectionPoolSetTest(偶发形态)。

```
READ of size 8 ... thread T1
  #0 IntrusiveList<Http1Connection,152>::next_of(...)   include/fiber/common/IntrusiveList.h:38
  #1 Http1ConnectionRegistry::drain_all()               include/fiber/http/endpoint/Http1ConnectionRegistry.h:30
  #2 Http2EndpointWorker::drain()                       include/fiber/http/endpoint/Http2Endpoint.h:30
  #3 Server::Worker::on_stop(...)                       src/http/Server.cpp:53
  ... Server::stop_and_wait() (.resume)                 src/http/Server.cpp:264
freed by thread T1:
  #1 async::Task<void>::~Task()                         include/fiber/async/Task.h:162
  #2 TcpEndpointBase::run_accepted(...) (.resume)       src/http/endpoint/TcpEndpointBase.cpp:171
  #3 RWFd::WaitAwaiter<Read>::on_complete(...)          src/net/detail/RWFd.cpp
  #4 RWFd::close()                                      src/net/detail/RWFd.cpp:113
previously allocated:
  #1 Http2Endpoint::serve_connection(...)               (ALPN 协商下探到 HTTP/1 的路径)
```

### 机制

`Http1Endpoint::serve_connection`(src/http/endpoint/Http1Endpoint.cpp:42-53)
与 `Http2Endpoint::serve_http1`(src/http/endpoint/Http2Endpoint.cpp:88-103)
同一形态:

```cpp
Http1Connection connection(...);              // 会话活在协程帧上
worker.connections().link(connection);
co_await connection.run();
worker.connections().unlink(connection);      // ← unlink 在挂起点之后
```

ASan 确证的事实链:**连接对象在仍挂链于 registry 时被释放**——释放路径
经由 `Task<void>::~Task()` 析构挂起中的协程帧(而非协程体跑完尾部的
unlink),帧销毁只运行成员析构,`co_await connection.run()` 之后的
`unlink` 被跳过;随后 `stop_and_wait → drain_all()` 遍历链表读到已释放
节点。传输层 close 经 `RWFd::close() → on_complete` 的内联 resume 链是
触发点。

精确持链待修时定位:候选是 `run_accepted`(DetachedTask,持有 serve 的
`Task<void>`)在某条 close-resume 路径上析构尚未完成的子 Task(与
`fiber-lib-quic-awaiters-not-destruction-safe.md` 同类:协程硬销毁
跳过挂起点之后的清理)。修复方向:

- 把 `unlink` 从尾语句改为 RAII 守卫(帧销毁必经),或
- serve Task 的销毁路径保证先 resume-to-completion 再析构。

H1/H2 两个 endpoint 的同形代码都要改;`Http3Endpoint` 的连接表
(src/http/endpoint/Http3Endpoint.cpp:122-135)是堆对象+回调 unlink,
形态不同,ASan 未报,但值得对照检查。

## 缺陷 2(新):Http3Connection 必须活过 QuicConnection,但无强制

**严重度:MEDIUM(组合契约缺口,堆形态下即 heap-UAF)。**

6 个用例命中:Http3ConnectionTest.ServerCanSendFinalResponseHeader、
ServerCanWriteFinalResponseBody(×2 变体)、ServerFinalResponseStops…、
ServerWriteReturns…、ServerWriteTimeout…。

```
READ of size 8 ... stack-use-after-scope
  #0 Http3Connection::end_server_request()              src/http/Http3Connection.cpp:207
  #1 ServerHttp3Request::~ServerHttp3Request()          src/http/ServerHttp3Request.cpp:243
  #2 ServerHttp3Request::destroy_owner(...)             src/http/ServerHttp3Request.cpp:248
  #3 QuicStreamTable::clear()                           src/quic/QuicStreamTable.cpp:31
  #4 QuicStreamTable::~QuicStreamTable()                src/quic/QuicStreamTable.cpp:16
  #5 QuicConnection::~QuicConnection()                  src/quic/QuicConnection.cpp:901
  #6 Http3ConnectionTest_...::TestBody()                tests/Http3ConnectionTest.cpp:734
```

### 机制

`~QuicConnection` 清流表时经 `destroy_owner` 回调 H3 层
(`~ServerHttp3Request → end_server_request` 读 `live_server_requests_`),
即**QUIC 析构会回调其上的 H3 层**。但测试的自然组合顺序(tests/Http3ConnectionTest.cpp:705-706):

```cpp
fiber::quic::QuicConnection quic(quic_options);   // 先声明 → 后析构
fiber::http::Http3Connection h3(quic, h3_options); // 先析构 → 回调进已亡 h3
```

反声明序析构使 `end_server_request` 读到已出作用域的 `h3` 栈槽。
栈形态是 stack-use-after-scope;同样的顺序在堆上(先 delete h3 再
delete quic)就是 heap-UAF。当前仅测试代码踩中,但库未以任何方式
(所有权、断言、文档契约)表达"Http3Connection 必须活过 QuicConnection",
任何嵌入者都可能踩中。

修复方向(择一):`~Http3Connection` 主动 detach/quiesce 所有流 owner,
使 `~QuicConnection` 不再回调;或 `Http3Connection` 拥有 `QuicConnection`;
短期最小修复=测试交换声明顺序+在头文件写明契约。

## 既有已知命中(交叉引用)

Http3EndpointTest.DestroyedSuspendedBodyWriterKeepsLoopIntact、
StoppedEndpointRefusesNewConnections 命中的:

```
#0 QuicCryptoBlockPool::release(...)          src/quic/QuicConnection.cpp:214/230
#1 QuicCryptoState::release_transient()/release_application()
#2 QuicCryptoState::reset()
```

即 `fiber-lib-h3-client-teardown-uaf.md` 已记录的 endpoint 共享资源
生命周期错配(该文档标注 `QuicCryptoBlockPool::release` 100% 复现),
状态:已定稿方案、暂不实施。本次扫描不改变该结论。

## 待查:池测试协程帧 UAF

Http2ConnectionPoolTest.AbandonedPooledExchangeCancelsStreamBeforeReturningSlot:

```
READ ... #0 PoolHarness 构造 lambda (.resume)   tests/Http2ConnectionPoolTest.cpp:54
          #1 coroutine_handle::resume()
          #2 async::SleepAwaiter::fire()         src/async/Sleep.cpp:50
freed by: run_scenario 协程帧析构(同文件)
```

定时器到点 resume 了一个其帧已随 `run_scenario` 析构的协程。归属
(test harness 自身 lifetime bug vs Sleep/Task 的销毁竞态)未定,
需按缺陷 1 的方法追 `SleepAwaiter` 注册/撤销与帧销毁的顺序。

## 非内存失败(记录备查)

- StealableHttp1ConnectionPoolSetTest.ClearAllowsBorrowedConnectionToReturnHome:
  断言失败,复跑 2/3 挂——ASan 减速下的时序 flaky。
- LiteNginxRuntimeTest.ScriptHeapExceptionServes500JsonWithName:断言失败,
  ASan 构建下单跑恒绿——同上。

## 结论

1. **泄露检查(本次主诉求):库无稳态泄露**;唯一 LSan 命中为测试
   teardown 抛弃挂起协程——**已修**(见上文修复小节)。
2. 新发现 2 个产品缺陷:Http1 registry drain UAF(HIGH)、H3/QUIC
   析构顺序契约(MEDIUM)。两者 Release 下静默,仅 ASan 可见。
3. ASan 构建配方与坑已固化在本文件开头,`temp/_deps_asan`、`build-asan`
   留存可直接复验。
