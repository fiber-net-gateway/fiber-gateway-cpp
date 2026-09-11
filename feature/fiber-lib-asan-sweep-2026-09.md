# Fiber lib ASan 全量扫描报告

记录日期:2026-09-11;行号基于 `fiber-gateway-cpp` @ `e9a8040`(clean master)。

**状态:扫描完成。1 处测试自身 teardown 泄露(库稳态无泄露)——**已修**;
Http1 registry drain UAF——**已修**(含 H2/H3 同形循环预防性加固);
H3/QUIC 析构顺序契约未修;2 个命中项归属既有已知缺陷文档;1 个测试
harness UAF——**已修**(2026-09-11 定位为 harness lifetime bug,见下)。**

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
| Http1 registry drain UAF | 7 | **产品缺陷(已修)** |
| H3/QUIC 析构顺序 stack-use-after-scope | 6 | **产品缺陷(未修)** |
| QuicCryptoBlockPool::release UAF | 2 | 既有已知,见 `fiber-lib-h3-client-teardown-uaf.md`(暂不修) |
| 池测试协程帧 UAF | 1 | 测试 harness lifetime,**已修** |
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

### 机制(2026-09-11 二次定位,修正初版推断)

`Http1Endpoint::serve_connection`(src/http/endpoint/Http1Endpoint.cpp:42-53)
与 `Http2Endpoint::serve_http1`(src/http/endpoint/Http2Endpoint.cpp:88-103)
同一形态:

```cpp
Http1Connection connection(...);              // 会话活在协程帧上
worker.connections().link(connection);
co_await connection.run();
worker.connections().unlink(connection);      // ← unlink 在挂起点之后
co_return;
```

**初版推断"unlink 被硬销毁跳过"不成立**——用
`fast_unwind_on_malloc=0` 重新采栈后确认:协程链是**正常跑完**的,
unlink 也执行了。真实的缺陷是 **drain_all 自己的循环推进在解引用已被
回调同步销毁的节点**:

```
Server::stop_and_wait → Worker::on_stop → Http2EndpointWorker::drain
  → Http1ConnectionRegistry::drain_all()            for 循环持有 conn=A
      → A->request_drain()                          (Http1Connection.cpp:418)
          idle_==true → finish() 同步内联执行        (:428,代码注释明言
                                                     "closing the transport wakes
                                                     the pending keep-alive read")
          → transport_->close() → RWFd::close()      (RWFd.cpp:113)
              内联触发 read_callback(Canceled)
              → WaitAwaiter::on_complete             (RWFd.h:277)
                  coro_.resume() —— 同步恢复 run()
                      (挂在 co_await wait_readable, Http1Connection.cpp:348)
                  run() 收到错误 → break → co_return → Task 完成
                  完成链全是对称转移(尾调用):
                    serve_http1 续体: unlink(A) → co_return
                      → 语句尾 Task 临时量析构 → handle_.destroy()
                        → serve_http1 帧(含 A)释放
                    serve_connection 续体: co_return
                    run_accepted 续体(TcpEndpointBase.cpp:171 语句结束)
                      → Task 临时量析构 → serve_connection 帧释放
      request_drain() 返回
  → for 循环推进: connections_.next_of(*A)            读 A 的
      worker_hook_.next —— A 的内存已释放 → UAF READ
```

要点:

1. **request_drain() 对 idle 连接是一次同步的全量拆除**。整条
   close→resume→run() 收尾→serve 链完成→帧释放,全部发生在
   `A->request_drain()` 的一个 C 栈帧内部。
2. **链表本身保持一致**(unlink 在帧释放前执行了);不安全的是
   drain_all 的遍历——循环变量还指着 A,用 `next_of(*A)` 前进时读
   已释放内存。
3. 释放栈里 `Task<void>::~Task()` 直接出现在
   `run_accepted (.resume)` 内、且中间协程的 .resume 都不在栈上,正是
   对称转移尾调用链的特征:只有最后一个被恢复的协程(run_accepted)
   留在物理栈上。ASan 的分配点显示 "allocated in serve_connection"
   则是协程 ramp(普通函数)被内联的正常表现。
4. busy 连接不受影响(request_drain 只置 draining_,拆除推迟到当前
   exchange 结束,那时 drain_all 已返回);只有 **idle(挂在 keep-alive
   读上)的连接**触发,与 7 个失败用例全部是"关停时有 idle h1 连接"
   的形态一致。Release 下读的是已释放但通常未复用的内存,故静默。

### 修复方向

最小且可证明安全的修法——回调前先取 next:

```cpp
void drain_all() noexcept {
    Http1Connection *connection = connections_.front();
    while (connection != nullptr) {
        Http1Connection *next = connections_.next_of(*connection); // 先读
        connection->request_drain();                                // 可能销毁 connection
        connection = next;                                          // next 节点不受 A 拆除影响
    }
}
```

安全性论证:A 的同步拆除只涉及 A 自己的 transport,不触碰其他节点;
预读的 next 在整个回调期间保持挂链且存活。

**同形循环审计(已一并加固)**:`Http2ConnectionRegistry::drain_all()`
与 `Http3ConnectionRegistry::drain_all()`(src/http/Http3ServerConnection.h)
是同一循环形态。当前 H2 的 request_drain 是 GOAWAY 语义、H3 是
graceful_shutdown,拆除都是异步的(观察不到命中),但该形态依赖
"回调不同步销毁节点"这一未声明不变量,已与 H1 同步改为预读 next,
循环内注释写明该约束。

### 修复(2026-09-11 已落地)

三处 `drain_all`(Http1ConnectionRegistry / Http2ConnectionRegistry /
Http3ConnectionRegistry)统一改为 while + 回调前预读 next。

红→绿验证:原 7 个命中用例 + Stealable 池偶发形态,ASan 构建下全部
通过、exit 0、零 ASan/LSan 输出——

- Http1EndpointTest.DrainClosesAnIdleKeepAliveConnection
- Http1EndpointTest.ConnectionsSpreadOverWorkerLoops
- HttpServerLifecycleTest.ShutdownClosesAnIdleHttp1Connection
- RouteVarTest ×4(ExchangeStrings…/RejectsExecution…/PathVar…/QueryHeaderCookieVars)
- StealableHttp1ConnectionPoolSetTest.ClearAllowsBorrowedConnectionToReturnHome

### 评估过的备选:连接自摘链(finish()/析构中 unlink)

结论:**不采纳为缺陷 1 的修复**(不修本 UAF),作为防御纵深另议。

1. **修不了本 UAF**:悬垂的是 drain_all 手里的遍历指针——`next_of(*A)`
   读 A 的 hook。unlink 时机只改变"A 是否还在链里",不改变"A 的内存
   是否已释放"。本次事故里 unlink 完整执行了(链表一致),死的仍是
   遍历者。把 unlink 提前到 finish() 或析构,循环照样解引用已释放的 A;
   预读 next 之后,回调销不销毁节点都无关紧要。
2. **finish() 处 unlink 语义混淆**:finish() 有三个调用上下文(run() 的
   FinishGuard、shutdown()、request_drain() idle 路径),前两者的 serve
   尾部 unlink 随后仍会跑(幂等 erase 变 no-op)。等于把"连接终结"与
   "从注册表消失"强行合并:run() 收尾期间连接还活着但已从 registry
   消失,任何"经 registry 找活连接"的扩展都会漏掉正在收尾的会话。
3. **析构处 unlink 需要反向指针**:`IntrusiveList::erase` 需要链表对象
   (head_/tail_ 归 list 所有,hook 无法在头/尾节点独立自摘),故连接必须
   持 registry 指针——新增成员、ctor 加参、把 endpoint 私有的注册知识
   下沉到连接层,且引入"registry 必须活过所有连接"这一需另行证明的
   生命周期耦合(违反时把读悬垂 next 变成写悬垂 list 头,等价危险)。
4. **自摘链真正防的是另一类风险**:serve Task 被硬 destroy 时(无人
   co_await、直接析构 Task),局部析构会跑但挂起点之后的 unlink 不会,
   留下挂链悬垂 hook——与 fiber-lib-quic-awaiters-not-destruction-safe.md
   同类。当前无此路径(缺陷 1 的事后分析恰好证明完成链自然跑完),故为
   纯防御纵深。若要做,**更优形态是 serve 协程内的 RAII guard**(link
   处声明,析构时仍挂链则 unlink):零新成员、零 ctor 波及、registry 知识
   留在 endpoint 层、天然覆盖硬 destroy 路径(帧 destroy 会跑局部析构)。

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

## 池测试协程帧 UAF(2026-09-11 已定位并修复)

Http2ConnectionPoolTest.AbandonedPooledExchangeCancelsStreamBeforeReturningSlot:

```
READ ... #0 PoolHarness 构造 lambda (.resume)   tests/Http2ConnectionPoolTest.cpp:54
          #1 coroutine_handle::resume()
          #2 async::SleepAwaiter::fire()         src/async/Sleep.cpp:50
freed by: run_scenario 协程帧析构(同文件)
```

### 归属判定:测试 harness lifetime bug,非 Sleep/Task 销毁竞态

完整因果链(全栈 ASan 证据):

1. **产品侧设计**:H2 服务端 handler 由
   `ServerHttp2Request::run_handler_task`(src/http/ServerHttp2Request.cpp:235
   `async::spawn`)**detached** 拉起,只持 `request` 裸指针 + stream Lease。
   连接关停只 quiesce exchange I/O——`on_stream_aborted`(:552)仅置
   `abort_reason_`、abort body recv、唤醒挂在 exchange 上的 awaiter;
   **handler 若挂在普通 `async::sleep` 上,连接侧既看不见也无法唤醒**。
2. **harness 侧**:测试 handler 以 1ms sleep 轮询 `hold_responses`
   (`PoolHarness` 成员,活在 `run_scenario` 协程帧内,帧分配栈即 ASan 的
   1168 B region)。`close()` 置 false 后的整条收尾(pool.shutdown/join →
   listener close → accept_done → conn.shutdown → gate.join)**全部由
   fd/defer 事件驱动,零定时器依赖**,可在 ≪1ms 内完成;而 handler 的
   下一次轮询至少要等 1ms 定时器到期。
3. **UAF**:`gate.join()` 的 `Joiner::on_notify` 最后一次恢复
   `run_scenario` → `~PoolHarness` → `done.set_value()` → 帧释放。~1ms 后
   仍挂在 timer 堆里的 `SleepTimer` 到期 → `fire()` resume handler
   (handler 自身帧与 `ServerHttp2Request` 因 Lease 存活,故能执行到用户
   代码)→ 读 `run_scenario` 已释放帧内偏移 728 处的 `hold_responses`。
4. `SleepAwaiter` 无缺陷:`~SleepAwaiter` 会 cancel 定时器
   (src/async/Sleep.cpp:11-16),问题是没人析构这个挂起的帧。

**隐含契约(嵌入方须知)**:handler 是 detached task,连接/服务端关停
不等待其完成;handler 内部若 park 在非 exchange I/O 的等待上(如裸
sleep),其引用的用户状态必须活过 handler 返回。

### 修复(2026-09-11 已落地)

harness 增加 `handlers_done` 计数(handler 入口用文件既有 Guard 惯用法
在帧析构时递增),`close()` 在 `hold_responses = false` 之后、
`pool.shutdown()` 之前等待 `handlers_done == requests`——放在 shutdown
前使挂起的 handler 仍能在活连接上完成收尾;被 RST 的流走
`SendResponseHeaderOp::on_encode` 的 `remote_rst()` 快速失败路径,有界
返回。run_case 自带 15s 兜底,潜在挂起会转为有界失败而非静默 UAF。

红→绿验证:修复前单用例 ASan 100% heap-use-after-free;修复后单用例与
`Http2ConnectionPoolTest.*` 全套(27 用例)ASan 构建零 ASan/LSan 输出
通过,Release 构建同套件通过。

## 非内存失败(记录备查)

- StealableHttp1ConnectionPoolSetTest.ClearAllowsBorrowedConnectionToReturnHome:
  断言失败,复跑 2/3 挂——ASan 减速下的时序 flaky。
- LiteNginxRuntimeTest.ScriptHeapExceptionServes500JsonWithName:断言失败,
  ASan 构建下单跑恒绿——同上。

## 结论

1. **泄露检查(本次主诉求):库无稳态泄露**;唯一 LSan 命中为测试
   teardown 抛弃挂起协程——**已修**(见上文修复小节)。
2. 新发现 2 个产品缺陷:Http1 registry drain UAF(HIGH)——已修;H3/QUIC
   析构顺序契约(MEDIUM)——未修。两者 Release 下静默,仅 ASan 可见。
3. 池测试协程帧 UAF 已定位为 harness 自身 lifetime bug 并修复;"handler
   为 detached task、其用户状态须活过 handler 返回"的隐含契约见上文。
4. ASan 构建配方与坑已固化在本文件开头,`temp/_deps_asan`、`build-asan`
   留存可直接复验。
