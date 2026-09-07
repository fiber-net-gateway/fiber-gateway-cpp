# Http2Connection 宿主接口与关闭生命周期改造方案

状态：设计，尚未实施。范围包括统一连接回调及 ctx、删除独立 closed callback、允许客户端空 peer-stream 工厂，并迁移现有调用方。

## 1. 最终接口

在 `include/fiber/http/Http2Connection.h` 的 `Http2Connection` public 区域定义嵌套类型 `Ops`，删除 `Http2StreamFactory.h`，不再创建独立的操作表头文件。以下为类内接口片段：

```cpp
struct Ops {
    Http2Stream::Lease (*create_peer_stream)(
        void *ctx, std::uint32_t stream_id, Http2Connection &connection) noexcept = nullptr;
    void (*on_state_change)(void *ctx, Http2Connection &connection) noexcept = nullptr;
    void (*on_capacity_change)(void *ctx, Http2Connection &connection) noexcept = nullptr;
};
```

`Http2Connection` 的构造接口变为：

```cpp
Http2Connection(Options options, void *ctx, const Ops &ops);
```

- 三项操作共用一个借用的 ctx；由宿主保证其有效性。ctx 允许为空，例如无状态函数。
- 构造时断言 `create_peer_stream` 非空当且仅当 role 为 server。客户端不支持 server push，没有对端流可创建。
- state/capacity 通知可选。工厂返回空 Lease 表示本次无法创建流。
- 按值复制 ops，调用方可以传临时表，不引入 ops 表的外部生命周期要求。
- 删除旧类型和兼容别名，仓库内一次性迁移；这是公共 C++ 源接口变更。
- 不改变 `Options` 的其他默认值；客户端一律禁用推送，由 role 推导，不增加配置开关。

连接保存 `void *ctx_` 和 `Ops ops_`，类外统一使用 `Http2Connection::Ops`。ops 在正常生命周期内固定，只有析构清理前将两个通知指针置空；因此成员不声明为 const。这避免为析构禁用通知额外增加状态位。

声明返回 `Http2Connection::Ops` 的头文件必须包含 `Http2Connection.h`，仅前向声明 `class Http2Connection` 不足以使用嵌套类型。`ServerRequestFactory.h` 因此改为包含连接头；客户端/服务端连接宿主头已包含它。`ClientHttp2Request.h` 删除旧工厂接口后可继续仅前向声明连接，若保留返回 Ops 的兼容入口则必须包含连接头。`Http2Connection.h` 不反向包含这些宿主或工厂头，避免循环依赖。

删除连接上的 `set/clear_state_callback`、`set/clear_capacity_callback`、对应函数指针和 ctx 成员。Gate 对外的容量观察者是连接池与 Gate 之间的另一层关系，其回调与 ctx 保留；frame payload hook 仍为独立的调试/扩展入口，不纳入这三个宿主操作。

## 2. 通知契约

state/capacity 均在连接所属 EventLoop 上同步调用，禁止阻塞、执行 I/O、嵌套驱动事件循环或销毁连接及宿主。允许修改内存、调用现有允许重入的连接控制方法、投递延迟任务。

两个通知保持独立：

- state：状态变化后通知，观察者调用 `state()` 读取当前状态。重入导致的变化按现有逻辑合并，不保证逐个交付所有中间状态。
- capacity：保留现有触发条件及关闭 bookkeeping；不因合并 ops 而增加额外通知。
- GOAWAY 只改变可接入状态而未改变流数量或预算时，仍只发 state 通知。
- 各自保留 `dispatch_running/dispatch_again`，不能共用一个重入标志。
- 构造函数不调用任何宿主操作。`start()` 必须发生在整个宿主构造完成后。
- 析构先禁用通知，再清理流和 transport；析构清理不是正常 Closed 通知渠道。

`State::Closed` 表示协议和 transport 关闭完成，不表示当前 C++ 调用栈已经退出。错误必须在发布 Closed 前写入 `terminal_error_`，发布后不再改变。关闭过程中不得凭 Closed 状态直接释放连接。

## 3. 删除独立 closed callback

从 `Http2Connection` 删除：

- `ClosedCallback`、`set_closed_callback()`、`clear_closed_callback()`、`has_closed_callback()`、`close_dispatched()`。
- `on_closed_`、`closed_ctx_`、`close_completion_entry_`、`close_completion_posted_`、`close_completion_dispatched_`。
- `on_closed_completion()`、`schedule_closed_completion()`、`dispatch_closed_completion()`。

`CloseResult` 作为 `wait_closed()` 返回值别名可以继续保留，它不代表需要独立回调。`close_finished_` 负责幂等资源清理，不能因为名称相似而一起删除。

`finish_connection()`、I/O pump 尾部、启动失败和 Init 下 shutdown 统一以发布 Closed 为结束通知，不再自行投递关闭完成任务。内部栈仍可在发布后做正常收尾，宿主只负责投递任务。

## 4. Http2CloseGate 接管关闭完成

Gate 从“注册连接回调的适配器”改为“由宿主显式通知的关闭完成协调器”。建议接口：

```cpp
Http2CloseGate(event::EventLoop &loop, Http2Connection &connection) noexcept;
void on_connection_closed() noexcept;
```

保留 `join()`、观察者接口、`closed()`、`terminal_error()` 和 joiner 查询；删除 `arm()`、`armed()`、`installed_` 及自动注册/注销行为。构造时建立 loop/connection 非空不变量，使用引用或构造初始化的稳定指针，不要求 connection 已有 transport。

Gate 新增一个内嵌 DeferEntry，用一个枚举表达阶段：

| 阶段 | 含义 | `closed()` | `join()` |
| --- | --- | --- | --- |
| Open | 未收到关闭 | false | 排队 |
| Pending | 收到 Closed，已投递任务 | false | 排队 |
| Dispatching | 正在通知观察者 | false | 排队 |
| Complete | 观察者全部处理完成 | true | 立即返回保存的结果 |

`on_connection_closed()` 断言连接为 Closed 和当前在指定 loop：Open 时保存终止结果并投递一次任务；其他阶段幂等返回。构造 Gate 时如果连接已经 Closed，也走 Pending，不能跳过延迟边界。

延迟任务的执行顺序：

1. 将 Pending 改为 Dispatching。
2. 逐个从观察者链表头摘除 hook，再调用回调。不要跨用户回调缓存下一个 hook 指针，因为回调可能注销或释放其他观察者。
3. 全部观察者完成后标记 Complete。
4. 摘除所有 joiner，投递各自的恢复任务；不在 Gate 的调度栈内直接恢复协程。

观察者允许注销其他观察者、投递工作，但不得销毁 Gate/宿主，禁止重入增加关闭观察者。`add_observer()` 限于 Open/Pending 阶段并断言；已有调用方都是启动前注册。Dispatching 阶段新调用 `join()` 可以排队，随本轮一起完成。

生命周期约束：正常路径中宿主存活到 Gate Complete，且不能在观察者回调中销毁。关闭完成后，等待协程或连接池 maintenance 才能释放宿主。直接使用底层连接的代码若需要销毁，应投递自有任务或采用 Gate，不能在 state callback 中 delete。

析构中的应急清理保留：取消 Gate 自己尚未执行的 DeferEntry，摘除观察者，对 joiner 按现有方式完成 Canceled。它只用于明确的 teardown，不能当作正常关闭完成；客户端和服务端正常调用链必须先等待 Gate。Gate 析构时不再写连接回调槽。

明确禁止嵌套运行 EventLoop，延迟任务才能成为“退出当前连接调用栈”的可靠边界。

## 5. 宿主和 Gate 的组装

### Http2ClientConnection

- `conn_(normalize_h2_options(...), this, connection_ops())`。
- ops 的 `create_peer_stream` 必须为 `nullptr`，不再为丢弃推送创建 `ClientHttp2Push`。
- `on_capacity_change` 转发给 `stream_gate_.on_capacity_change()`。
- `on_state_change` 先转发给 `stream_gate_.on_state_change()`，再在 Closed 时调用 `close_gate_.on_connection_closed()`。
- 成员顺序保持 conn、stream gate、close gate；close gate 显式使用构造参数 loop。连接构造不触发回调，解决成员尚未初始化的问题。
- 保留 `wait_closed()` 在 Init 返回 Invalid 的现有语义。
- connect/adopt/start 返回失败后，如果底层已 Closed，正常持有者须等待 Gate 完成再销毁；若底层仍 Init，按原来的未启动对象路径处理。

### Http2ServerConnection

- 因为 closed 通知也需要宿主转发，ctx 统一改为 `this`，不再直接传 request factory。
- 保存构造时初始化的非空 `ServerRequestFactory *request_factory_`，静态工厂 trampoline 转发其 `create_peer_stream()`；不使用引用成员，因为该类必须保持 standard-layout。
- 构造函数增加显式 `event::EventLoop &loop`，用于尚无 transport 时的关闭调度；`HttpServer` 从对应 worker 传入 loop。
- state callback 在 Closed 时通知 close gate，capacity 可为空。
- 成员全部保持同一访问级别，保留 standard-layout static_assert 和 worker 的 offsetof 约束。
- `start()` 检查 transport 所属 loop；start 失败且连接已 Closed 时，serve 协程仍等待 close gate 再返回。失败后仍 Init 的输入拒绝路径不需要等待。

### Http2LocalStreamGate

- 构造仅绑定 connection，析构只取消 waiter，不注册/清除连接回调。
- 增加公开的 `on_capacity_change()`、`on_state_change()`，两者进入现有容量处理逻辑。
- 保留 FIFO、公平性、超时及 Gate 对外的 capacity callback。现有状态变化也通知池容量观察者的行为不变。

### Http2ConnectionPoolCore

- 继续通过 stream gate 观察容量、close gate 观察关闭。
- 删除 maintenance 中“Init/start failure 没有 closed callback，所以手动调用 on_closed”的补偿分支。
- `on_closed` 仍只退休条目并投递 maintenance，不同步销毁宿主。
- 正常销毁条件增加或保留 Gate Complete 的检查；dialing、active lease 和 waiter 生命周期约束保持。
- 重点检查 dial 失败到 shutdown 的路径：底层 Init 时先 shutdown，Closed 通过统一 state 路径发布，不能提前把条目标记为最终可销毁。

### 其他直接调用方

- `ServerRequestFactory::ops()` 返回新类型，仅填工厂，便于不需要 Gate 的底层服务端用法。
- 测试统一引入宿主 fixture，内含工厂、state/capacity observer 及可选 Gate；所有 trampoline 从同一个 ctx 获取成员。
- Nacos 的三组测试服务端使用新类型/工厂接口；未改变 Nacos 协议行为。
- 原 closed callback 中直接 delete 的测试，改为通过 Gate 的等待协程安全销毁，并保留释放后访问检测价值。

## 6. 禁用推送

客户端不接收 server push：浏览器已不再使用该特性，实现完整推送需要 reserved(remote)、请求语义验证、authority 验证和向应用交付推送，收益不足以承担这些状态机。因此 client role 禁止传入 `create_peer_stream`，不保留任何兼容路径，也不提供推送业务 API。

原 `ClientHttp2Push` 只丢弃数据，PUSH_PROMISE 也未被连接显式处理，因此禁用推送是行为收紧。删除该私有类及其源项，同时删除 `ClientHttp2Request::factory_ops()` 与私有 trampoline。

### 6.1 SETTINGS

- client 的首个 SETTINGS 增加 ENABLE_PUSH=0，修正参数数量、payload 长度和输出缓冲大小；已有其他设置不变。server 不声明该项。
- 不用 MAX_CONCURRENT_STREAMS=0 替代 ENABLE_PUSH；前者不能禁止 PUSH_PROMISE。
- 增加 `initial_settings_acked_`，仅在收到合法的本端初始 SETTINGS ACK 时设为 true；不能复用表示“收到对端 SETTINGS”的 `peer_settings_received_`。
- 当前只发送一轮初始非 ACK SETTINGS，使用一个布尔足够。以后支持动态本地设置时需按发送顺序跟踪待确认批次，不能沿用单布尔。
- server 收到 ENABLE_PUSH=0/1 的已有行为保留；client 收到服务端显式 ENABLE_PUSH=1 应报协议错误，值 0 可接受。两种方向不能混淆。

### 6.2 禁用生效后的 PUSH_PROMISE

在帧分派中显式识别 PUSH_PROMISE。client 在初始设置已 ACK 后收到该帧报连接级 PROTOCOL_ERROR；不能走默认忽略钩子，也不能伪装成工厂分配失败返回 REFUSED_STREAM。server 收到 PUSH_PROMISE 同样报协议错误。

### 6.3 ACK 前的在途推送

本端尚未收到设置 ACK 时，允许对端尚不知道禁用设置。对合法的在途 PUSH_PROMISE：

1. 校验关联 stream、promised stream ID 的奇偶/递增/空闲状态、payload 最小长度及 padding。关联的本地 stream 被 reset 后可能仍有在途 promise，不能一律按未知流拒绝。
2. 解析完整请求头块，使用连接共用的 HPACK decoder 更新动态表；不创建业务 stream。
3. promise ID 推进对端 stream ID 水位；头块处理完成后向 promised ID 发 RST_STREAM(CANCEL)。
4. 带 CONTINUATION 时，以 PUSH_PROMISE 帧头的关联 stream ID 校验连续性，而不是 promised ID。单连接同时只允许一个未结束头块。
5. 重用现有 inbound 状态容器，增加头块用途枚举及必要的 promised ID/前缀暂存；不创建每个拒绝流的堆对象或容器。
6. 拒绝后的在途 HEADERS 继续解码并丢弃；DATA 按现有关闭流路径计入连接级流控；RST/WINDOW_UPDATE 按关闭流规则处理。

帧前缀可能分散在多次 read 中，需要审计底层分片交付约定。PUSH_PROMISE 的 4 字节 ID、可选 padding 长度必须有边界检查或固定大小暂存，不能假设首个 payload chunk 足够长。

客户端收到任何 idle 对端流上的 HEADERS，报连接级 PROTOCOL_ERROR。普通响应找到已存在的本地 stream，完全不经过工厂分支。已经被拒绝的 promised ID 走关闭流分支，不误判为新的 idle stream。

### 6.4 HPACK 丢弃头块

不能复用 `ClientHttp2Push` 现有全 noop sink：decoder 的增量索引插入依赖 sink 填出的 FieldView，noop 不提供实际 name/value；也不能给 `begin_block()` 传空 ops。

增加 core-private 的 HPACK discard sink，按现有 decoder Ops 实现：

- 正确处理 indexed name/field、raw/Huffman name/value，并为增量索引提供有效 FieldView。
- 保存当前字段所需的 name/value，使用可复用的项目缓冲区；不构造完整 headers 集合，不分配业务请求。
- 验证 Huffman EOS/尾部填充，保留字符串长度上限；解码完成后允许回收大临时缓冲。
- 与当前连接共用动态表，不能使用一个独立 decoder 来丢弃。

现有 `discard_closed_stream_block` 直接跳过 HEADERS/CONTINUATION 字节，也改走同一 sink。拒绝分支必须保留正确的头块 payload 起止位置，剔除 padding/priority/promise 前缀后再交给 decoder，不能沿用当前为了直接跳过而清零的区间。用一个二值的 payload 模式表达 deliver/discard，同时覆盖头块丢弃和被丢弃流的 DATA 丢弃，避免堆叠互相矛盾的布尔；promise 丢弃不需要第三个枚举值，由非零的 `promised_stream_id` 区分，它同时是头块解码完成后 RST_STREAM(CANCEL) 的目标。

协议错误路径需要真正产生相应的 HTTP/2 错误码：增加或复用带 `Http2ErrorCode` 的内部连接错误收尾入口，向可写 transport 尽力发送 GOAWAY，再按现有有界发送/关闭机制收尾；不可写时直接关闭。不能仅返回 IoErr::Invalid 就认为测试过了 PROTOCOL_ERROR。HPACK 格式错误映射 COMPRESSION_ERROR，帧结构错误按具体规则区分 FRAME_SIZE_ERROR/PROTOCOL_ERROR，内存失败不冒充压缩格式错误。

丢弃的头块必须解码而不能跳过字节，依据是 [RFC 9113 §4.3](https://www.rfc-editor.org/rfc/rfc9113.html#section-4.3)："A receiver MUST terminate the connection with a connection error of type COMPRESSION_ERROR if it does not decompress a field block." 动态表是连接级状态，跳过一个块会让两端索引永久错位。其余规则来自 [§5.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1)、[§6.5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2)、[§6.6](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.6) 和 [§8.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.4)。

## 7. 文件改动清单

| 文件/范围 | 改动 |
| --- | --- |
| `include/fiber/http/Http2StreamFactory.h` | 删除，移除所有旧 include |
| `include/fiber/http/Http2Connection.h`、`src/http/Http2Connection.cpp` | 定义嵌套 Ops、统一 ctx/ops、删除 closed 通知、client 禁止工厂及 SETTINGS/拒绝推送路径 |
| `Http2CloseGate.h/.cpp` | 显式 loop/connection 构造、阶段状态、延迟完成 |
| `Http2LocalStreamGate.h/.cpp` | 改成宿主显式通知 |
| `Http2ClientConnection.h/.cpp` | this ctx、回调转发、空工厂、关闭生命周期 |
| `Http2ServerConnection.h/.cpp`、`HttpServer.cpp` | 宿主转发、显式 loop、启动失败等待 |
| `ServerRequestFactory.h/.cpp` | 新 ops 类型 |
| `ClientHttp2Request.h/.cpp`、`src/http/ClientHttp2Push.*` | 去除内置客户端对旧 factory ops 的依赖，清理无引用私有实现 |
| `src/http/Http2HpackDiscardSink.h/.cpp`（新） | 正确解码但不交付头字段的私有 sink |
| `src/http/Http2ConnectionPoolCore.cpp` | 删除关闭补偿分支，按 Gate 完成退休/销毁 |
| `tests/Http2TestSupport.h`、现有 HTTP/2 tests | 统一宿主 fixture 和生命周期断言 |
| `tests/Http2CloseGateTest.cpp`（新） | 独立验证 Gate 时序、重入和取消 |
| `tests/Http2HpackDiscardSinkTest.cpp`（新） | 动态表同步、Huffman、错误和跨片段 |
| `apps/nacos/tests/{NacosRpc,ConfigService,NamingService}Test.cpp` | 迁移直接构造连接的用法 |
| `CMakeLists.txt` 及实际持有源列表的 cmake 文件 | 注册新源/测试，去除删除的源 |
| `docs/http2-connection-pool.md`、`feature/http2_client_connection.md` | 更新回调关系、关闭时序和默认禁用推送说明 |

公共头只依赖 include 下的头，不向消费者暴露 src；新私有 sink 通过 core 的私有源路径使用。

## 8. 测试与验收

### 接口与回调

- server 非空断言、client 必须为空的断言、client 正常启动/收发；组合 ctx 正确路由三个操作。
- 可选通知为空；构造与析构不回调宿主；ops 按值复制可安全接受临时表。
- state/capacity 各自重入合并；GOAWAY 只触发 state 的原有测试保持。
- Gate FIFO、超时、取消及池容量变化顺序不变。

### 关闭

- Closed 同步通知时连接尚不可销毁；退出内部栈后 Gate 才通知观察者。
- Pending/Dispatching 期间 join 都不能立即完成；观察者先于所有 joiner。
- Init shutdown、启动失败、正常 EOF、主动 shutdown、GOAWAY drain 均只完成一次。
- 晚构造 Gate 绑定已 Closed 连接、晚 join、重复关闭。
- 观察者注销另一个观察者，不访问缓存的悬空 hook；观察者重入 join 正常完成。
- Pending Gate teardown 能取消任务；joiner 取消后无残留链接。
- 客户端释放、server start 失败返回、worker shutdown walk、pool dial 失败/retire/drain 均无释放后访问。
- 关闭回调迁移后仍覆盖“等待关闭后销毁整个连接”的真实 heap 生命周期场景。

### 协议

- 逐字节验证首个 SETTINGS 的 ENABLE_PUSH、长度、参数数量；server 完全不声明该项。
- 区分对端 SETTINGS 和本端 SETTINGS ACK，零长度 ACK 正确进入确认状态。
- ACK 前合法 promise 被 CANCEL，工厂调用次数为零；ACK 后 promise 收到 GOAWAY(PROTOCOL_ERROR)。
- padding、分片前缀、跨 CONTINUATION、错误关联 ID、重复/奇数 promise ID、非法交错帧。
- 本地 reset 后的在途 promise 可正确拒绝；拒绝流的后续 HEADERS/DATA 不破坏连接解码/流控。
- promise/被丢弃 HEADERS 使用增量索引写入动态表，后续正常响应引用同一条目，确保仍能正确解码。
- 原始字符串、Huffman 字符串、非法 Huffman 和截断头块；检查 wire error code，而非只检查本地 IoErr。
- 服务端正常创建对端流、trailer/CONTINUATION 与容量拒绝的行为保持；对端流用例全部在 server role 下覆盖。

实现完成后执行：

```bash
./format_code.sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

对关闭/取消相关测试增加一次现有工具链支持的 AddressSanitizer 构建验证；具体 sanitizer 配置沿用仓库能力，不预设未存在的 CMake 开关。若 Nacos 组件在当前配置关闭，补充启用 `FIBER_BUILD_NACOS=ON` 的构建与本地测试；此次接口迁移本身不要求启动外部 r-nacos 服务。

## 9. 实施顺序与收益

1. 实现统一 ops 和宿主转发，同时迁移 LocalStreamGate。
2. 实现 CloseGate 延迟完成，迁移 client/server/pool/test，再删除底层 closed callback 全部状态。
3. 实现 discard sink 和共享头块丢弃路径，再接入 SETTINGS ACK 和在途 promise 拒绝；不要先切换客户端而留下协议半成品。
4. 更新构建与文档，统一格式化，运行上述测试及内存生命周期验证。

可分成若干可独立编译的提交，但最终合入必须包含禁用推送所依赖的协议处理。

收益是连接只持有一个宿主上下文，不再管理外部关闭订阅和关闭完成调度。客户端默认不为无用推送分配 stream。不能只按删除三个 ctx 指针宣称总体节省：DeferEntry 移到了 Gate，server 新增工厂引用，协议拒绝路径也需要少量状态。最终记录 `sizeof(Http2Connection/Http2CloseGate/Http2ClientConnection/Http2ServerConnection)` 的前后值和实际分配行为，以数据评价内存变化。
