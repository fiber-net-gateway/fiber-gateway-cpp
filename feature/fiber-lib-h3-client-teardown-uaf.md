# Fiber lib HTTP/3 客户端 teardown 悬垂指针(UAF)缺陷报告

记录日期:2026-09-09;行号基于 `fiber-gateway-cpp` @ `1e30624`(行号在
`dfa5676c` 引脚上仅 `QuicConnection.cpp` 因 0004 修复有少量漂移,机制相同)。

**状态:已定位根因、修复方案已定稿(见文末),暂不实施。**

## 症状

ASan 构建下任何 H3 客户端形态的测试(连最老的
`Http3EndpointTest.ServesHttp3Requests`)在收尾阶段 100% 触发:

```
ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
    #0 QuicCryptoBlockPool::release(...)   src/quic/QuicConnection.cpp:230
    #1 QuicCryptoState::reset(...)         src/quic/QuicConnection.cpp:~241-305
    #2 QuicCryptoState::~QuicCryptoState()
    #3 QuicConnection::~QuicConnection()   src/quic/QuicConnection.cpp:855
    ...
freed by: 客户端协程帧析构(帧内 QuicUdpEndpoint 成员随之消亡)
```

Release 构建不崩——时序上 `delete session` 通常先于下一次对该内存的复用——
但属确定性 UB,线上任何分配器布局变化都可能把它变成野指针写。

## 复现

```bash
# 注意:ASan 构建必须用独立 FIBER_DEPS_DIR,共享 temp/_deps 会被 ASan 标志
# 重编 BoringSSL 等依赖,污染后续 Release 链接(__asan_* undefined)
cmake -S . -B build-asan -DCMAKE_CXX_COMPILER=/usr/bin/clang++-20 \
  -DCMAKE_CXX_FLAGS='-stdlib=libc++ -isystem /usr/lib/llvm-20/include/c++/v1 \
  -fsanitize=address -fno-omit-frame-pointer -g' \
  -DFIBER_DEPS_DIR=$PWD/temp/_deps-asan
cmake --build build-asan -j
./build-asan/fiber_tests --gtest_filter=Http3EndpointTest.ServesHttp3Requests
```

任何 `run_http3_client` 形态(shutdown → reset 句柄 → sleep → 帧结束)同理,
100% 触发,无需特殊构造。

## 根因:endpoint 共享资源的生命周期错配

### 生命周期链条

```
客户端协程帧(栈): [ QuicUdpEndpoint(含 crypto_block_pool_/output_frame_pool_/
                                       recv_storage_budget_ 三个成员) ]
                          │ admission / 建连时注入裸指针(QuicClient.cpp:239-241)
Session(堆):     QuicConnection ── crypto states 持 pool_ ────→ endpoint 成员
                                 └─ recv_storage_budget_.parent_ → endpoint 成员
连接关闭 → QuicConnection::release() → on_destroy_
  → Session::destroy_connection(src/http/Http3Client.cpp:51-55)
     spawn[ co_await h3_.wait_closed(); delete session; ]   ← DetachedTask
客户端协程帧析构(endpoint 随之消亡)
  → 稍后的 drain_notify 才执行 delete session
     → ~QuicConnection 成员析构 → ~QuicCryptoState::reset() → pool_->release()  ← UAF
```

关键点:`wait_closed()` 需要后续 loop 轮次,因此 `delete session` 必然排在
宿主协程帧析构之后;而 `~QuicConnection` 的成员析构会回吐资源给
**endpoint 的成员对象**——此时 endpoint 已随帧消亡。这不是某个孤立指针的
笔误,而是"借用方生命周期可超出出借方"的结构性错配。

### 晚期析构对 endpoint 内存的全部触点(三个注入资源逐一判定)

| 注入的资源(注入点) | 晚期触点 | 判定 |
|---|---|---|
| `crypto_block_pool_`(QuicUdpEndpoint.cpp:1620 / QuicClient.cpp:240) | `~QuicCryptoState::reset() → pool_->release()`(QuicConnection.cpp:230 读 `active_blocks_` 等) | **已证实 UAF**(ASan 现场) |
| `recv_storage_budget_` 作 `recv_storage_parent`(:1621/:241) | 成员析构释放 recv extents → `IoBufStorageBudget::unreserve() → parent_->unreserve()`(src/common/mem/IoBuf.cpp:110-111) | **代码路径证实**,同族缺陷,只是被 crypto 的崩溃掩盖(成员析构顺序在後) |
| `output_frame_pool_`(:1619/:239) | packet number spaces 持池指针,但 `detach_from_endpoint() → clear_frames_for_detach()`(QuicConnection.cpp:1092-1101)在 detach 时已归还余帧并把池指针重指向连接自有池;`~QuicConnection` 断言 `!attached_to_endpoint_` | **按不变量安全**,但该安全依赖"detach 必先于析构"——正是本缺陷打破的那类时序假设,宜一并治理 |

附带发现(同族、latent):`QuicUdpEndpoint::close()`(QuicUdpEndpoint.cpp:629)
断言 `recv_storage_budget_.retained_capacity() == 0`。deferred session 在
endpoint close 时可合法持有未释放 credit(如客户端中途 abort 的响应体),该
断言在此场景会误触发——时机应后移到"最后一个借用方消失"。

## 修复方案(已定稿,未实施)

### 候选对比

- **A. 引用计数共享资源束(推荐)**:endpoint 把三个共享成员搬进堆上、带
  侵入式原子引用计数的小对象;endpoint 持 1 引用(close/析构时放),每条
  注入的连接 ctor `retain`、析构 `release`,最后一个引用释放才销毁池/预算。
  迟到的 `delete session` 触碰的是仍然有效的束。
  结构性修复,所有权与真实语义一致;无需论证"析构期无触点";服务端
  admission、QuicClient、Http3Client 一次覆盖;改动 ~5 文件 ~100-150 行。
- **B. detach 时彻底归还并切断指针**(crypto 块清空后 `pool_=nullptr`、
  预算 credit 还清后 `parent_=nullptr`):须穷举封死 detach 后一切触点,
  本质仍是靠时序假设保安全,漏一处即复发。只可作 A 之上的可选加固。
- **C. Http3Client 层修 Session 生命周期**(client.close 排空 session /
  同步 delete):只治 H3 症状;close() 同步语义要求泵 loop(重入风险);
  on_destroy 回调里同步 delete 属 delete-this-from-method;QuicClient 及
  直接使用 quic 层的调用方仍暴露。否决。

### 方案 A 实施细节

新类型放 `include/fiber/quic/QuicConnection.h`(紧邻 `QuicCryptoBlockPool`,
头文件依赖已齐):

```cpp
class QuicEndpointResources : public common::NonCopyable, public common::NonMovable {
public:
    static QuicEndpointResources *create() noexcept;   // new,引用计数=1
    void retain() noexcept;
    void release() noexcept;                           // 归零 delete this
    QuicCryptoBlockPool crypto_block_pool{};
    QuicOutputFramePool output_frame_pool{};
    mem::IoBufStorageBudget recv_storage_budget{};
};
```

1. `QuicConnection::Options` 新增 `QuicEndpointResources *resources = nullptr`,
   **保留**原三个裸指针字段(改指束内成员)。直接构造 QuicConnection 的既有
   调用方(含 `tests/QuicConnectionTest.cpp:1374` 只传 `recv_storage_parent`
   的用例)行为不变,零测试迁移。
2. 连接侧持引用:ctor 里 `retain`;释放用**声明在最前的第一个数据成员
   RAII guard**——成员析构逆序,guard 在 crypto states / budget / packet
   spaces 等全部成员析构完之后才放引用,保证束在最后一个触点结束后才可能
   销毁。这是方案里唯一必须做对的位置点(以注释+断言固化)。
3. `QuicUdpEndpoint`:三个成员(QuicUdpEndpoint.h:325-327)→ 一个束指针;
   `init()`(cpp:502 处)建束并配预算;`close()` 末尾放 endpoint 引用;
   cpp:629 的 budget 清零断言移入束析构(时机后移到"最后一个借用方消失",
   消除上述 latent assert-fire);admission 注入(cpp:1619-1621)改为传束 +
   束内指针;stats getter(h:155-161)改读束。
4. `QuicClient`(cpp:239-241):经 `endpoint_` 的束注入(加
   `QuicUdpEndpoint::shared_resources()` 访问器,顺手消掉 friend 裸摸成员)。
5. 引用计数用原子(连接 ctor/dtor 与 endpoint close 理论上可能不同 loop;
   每连接仅两次,成本可忽略)。
6. `Session::destroy_connection` 的异步 `wait_closed → delete` **保持原样**——
   束使该模式变安全,不需要也不应在此引入同步 delete。

改动面:`QuicConnection.h/.cpp`、`QuicUdpEndpoint.h/.cpp`、`QuicClient.cpp`;
无 API 破坏、无测试迁移。

## 验证方法(实施时)

1. 红:独立 `FIBER_DEPS_DIR` 的 ASan 构建,当前 master 跑
   `Http3EndpointTest.ServesHttp3Requests` → `heap-use-after-free
   @ QuicCryptoBlockPool::release`(已验证 100% 复现)。
2. 绿:修复后同构建跑全部 `Http3EndpointTest` + `fiber_tests`,ASan 零报告;
   `--gtest_repeat=20` 抖动时序。
3. Release 全量 ctest 全绿。
4. 无需新增回归测试(`ServesHttp3Requests` 即确定性回归用例)。

## 关联

- 发现于 0004(`feature/fiber-lib-quic-awaiters-not-destruction-safe.md`)
  的 ASan 验证过程,但为独立缺陷:干净 master(不含 0004)同样 100% 复现,
  与 awaiter resume 投递方式无关。
