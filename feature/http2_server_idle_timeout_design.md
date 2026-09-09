# Http2ServerConnection 无 stream 超时回收设计

状态：已按方案实施并完成本地验证（2026-09-09）。验证结果见第 11 项。

当 HTTP/2 服务端连接连续没有任何挂接的 stream 达到 `idle_timeout`，发送
`GOAWAY(NO_ERROR)` 并通过现有 drain 流程关闭连接。PING 应答和其他控制帧不能延长这个期限。

1. **职责与范围**

   `Http2ServerConnection` 持有并执行回收策略；`Http2ServerOptions` 提供 endpoint 配置入口。
   `Http2Connection` 只补充 stream 存在性查询、明确已有回调契约，不加入服务端专属的空闲回收策略。

   现有 `read_timeout` 继续检测入站静默并发送 PING，`write_timeout` 继续限制写阻塞。
   本功能不改变客户端连接池回收、HTTP/1、HTTP/3、stream 自身超时或服务端整体关闭时限。

2. **配置和公开接口**

   在 `include/fiber/http/Http2ServerOptions.h` 的现有字段末尾增加：

   ```cpp
   // Maximum continuous time with no attached streams while Running.
   // Control frames do not refresh this deadline. max() disables it.
   std::chrono::milliseconds idle_timeout{std::chrono::seconds(70)};
   ```

   放在末尾保留旧代码对前三个字段的聚合初始化顺序。70 秒是本方案选择的默认策略值。

   | 取值 | 契约 |
   |---|---|
   | 正数 | 连续无 stream 达到该时间后发起 drain |
   | `milliseconds::max()` | 禁用该回收策略，不创建定时器 |
   | 零 | 无 stream 时提交立即到期的定时器，由事件循环回调发起 drain |
   | 负数 | 非法配置 |

   零值不在 stream/state 通知栈内同步关闭；在定时器实际执行前，新 stream 仍可能被接纳并取消计时。

   `Http2ServerConnection.h` 显式包含 `<chrono>` 和 `Http2ServerOptions.h`。
   构造函数追加一个带默认值的参数：

   ```cpp
   Http2ServerConnection(
       event::EventLoop &loop,
       Http2Connection::Options options,
       ServerRequestFactory &request_factory,
       std::chrono::milliseconds idle_timeout = Http2ServerOptions{}.idle_timeout) noexcept;
   ```

   默认值直接取自服务端配置，避免两处默认值漂移。保留原有三个参数及顺序。
   构造时断言 `idle_timeout >= milliseconds::zero()`，建立后续路径无需反复验证的约束。
   `Http2Endpoint::on_start()` 在调用 `TcpEndpointBase::on_start()` 前检查负值并返回 `IoErr::Invalid`，
   让错误 endpoint 配置在绑定监听端口前失败。

   `src/http/endpoint/Http2Endpoint.cpp::serve_http2()` 显式传入 `options_.http2.idle_timeout`：

   ```cpp
   Http2ServerConnection connection(worker.loop(), make_connection_options(),
                                    *request_factory_, options_.http2.idle_timeout);
   ```

   `make_connection_options()` 仍只映射 `Http2Connection::Options` 中的协议及 I/O 参数。
   不增加 setter；配置在构造时固定。

3. **“没有 stream”的精确定义和通知来源**

   在 `include/fiber/http/Http2Connection.h` 增加：

   ```cpp
   // True while any locally or remotely initiated stream remains attached.
   // Includes half-closed streams. Query on the owning event loop.
   [[nodiscard]] bool has_active_streams() const noexcept {
       return !streams_.empty();
   }
   ```

   查询以连接 stream 表为准，覆盖本端和对端创建的 stream，不使用 `local_active_stream_count()`。
   handler 返回、暂时没有 DATA、某一侧 END_STREAM 都不单独表示连接空闲。
   stream 在连接中完成移除才释放这项空闲约束；外部仍持有已移除 stream 的 Lease 不阻止回收。

   当前源码已经有三个相关通知点：

   - `create_peer_stream()`：成功插入表并增加 peer 计数后通知。
   - `try_attach_local_stream()`：成功插入本端 stream 后通知。
   - `detach_stream()`：从表移除并更新计数后通知；Closing 分支直接走关闭完成逻辑。

   这些路径调用 `on_local_stream_attach_capacity_changed()`，最终触发 `Ops::on_capacity_change`。
   在 `Ops` 字段注释中明确：通知涵盖任一方向 stream 成功挂入、移除及容量配置变化；
   回调读取的是更新后的状态；重入通知可合并，不承诺每个内部操作对应一次调用。
   关闭阶段由 `on_state_change` 负责撤销回收定时器，不能依赖最后一个 detach 必然通知。

   保留现有回调名称、分发机制和结构布局，不新增另一套 stream 事件回调。
   `Http2ServerConnection::connection_ops()` 的第三个槽从 `nullptr` 改为 `&on_capacity_change`。

4. **新增成员与辅助函数**

   ```cpp
   static void on_capacity_change(void *ctx, Http2Connection &connection) noexcept;
   static void on_idle_timer(Http2ServerConnection *connection) noexcept;
   void sync_idle_timer() noexcept;
   void cancel_idle_timer() noexcept;

   const std::chrono::milliseconds idle_timeout_;
   event::EventLoop::TimerEntry idle_timer_entry_{};
   ```

   定时器成员在构造期间初始化完成，`start()` 后才可能挂入事件循环。
   用 `idle_timer_entry_.is_in_heap()` 判断是否已计时；不增加 `idle_`、`timer_armed_`、
   `idle_since_`、重复 stream 计数或单独的关闭状态。

   单连接增加一个 duration 和一个内嵌 timer entry。每次通知只做常数次判断；
   只有进入、离开空闲状态才进行定时器堆操作，无轮询、无额外协程、无单独分配的 timer 对象。

5. **计时器状态转移**

   `sync_idle_timer()` 的算法为：

   ```text
   如果 idle_timeout == max，或 conn.state != Running，或 conn.has_active_streams：
       cancel_idle_timer()
       返回

   如果 idle_timer_entry 已在堆内：
       返回                     // 保留原期限，不重新计时

   deadline = EventLoop::current().now() + idle_timeout
   post_at(deadline, idle_timer_entry)
   ```

   时间取自所属事件循环，沿用项目单调时钟和 `post_at` 接口。
   `max()` 分支必须在计算 deadline 前返回。

   | 事件 | 处理 |
   |---|---|
   | `Init` / `Start` | 不启动本项定时器；前言接收仍受原 read timeout 控制 |
   | 首次进入 `Running` 且表为空 | 启动计时，即使从未收到业务请求 |
   | 第一个 stream 成功挂入 | 取消计时 |
   | 仍有其他 stream 时某个 stream 结束 | 保持未计时 |
   | 最后一个 stream 移除 | 从此刻开始一个完整空闲期限 |
   | 空闲期间再次收到容量通知 | 保留原期限 |
   | 收到 PING / ACK / SETTINGS / WINDOW_UPDATE 等 | 不延长空闲期限 |
   | 退出 `Running` | 取消计时 |

   当前服务端在消费正确的客户端 preface 后进入 `Running`，不额外等待 SETTINGS ACK。
   还没有形成并成功挂入 stream 的零散入站字节也不延长期限。

   `on_capacity_change()` 调用 `sync_idle_timer()`。
   `on_state_change()` 先调用 `sync_idle_timer()`；如果已为 `Closed`，再通知
   `close_gate_.on_connection_closed()`。这样关闭等待者被唤醒前定时器已经撤销。

6. **到期行为与关闭结果**

   `EventLoop::run_due_timers()` 在调用 timer callback 前已经从堆中移除 entry，
   因此到期回调无需维护另一份 armed 状态。

   `on_idle_timer()` 再次检查 `conn_.state() == Running && !conn_.has_active_streams()`。
   条件不满足就返回；满足就调用现有 `request_drain()`，随后返回，不重新挂 timer。

   现有关闭路径为：

   ```text
   request_drain()
     -> Http2Connection::graceful_shutdown()
     -> send_goaway(last_peer_stream_id_, NoError)
     -> Draining
     -> stream 表为空，进入 Closing
     -> 冲刷待发送数据
     -> Closed
     -> Http2CloseGate 延迟完成
     -> wait_closed() 恢复，endpoint 从 registry 移除连接
   ```

   空闲回收属于正常关闭，不主动写入 `IoErr::TimedOut`。
   没有其他错误时 `wait_closed()` 成功；GOAWAY 分配、传输或写超时错误仍按现有路径报告。
   到期表示“发起关闭”，不承诺该时刻 TCP 已经关闭；写阻塞仍由 `write_timeout` 约束。
   如果调用者显式禁用了写超时，不能据此声称回收具有有限的物理关闭时限。

7. **并发和对象生命周期**

   stream 通知、定时器和关闭操作均在同一 EventLoop，不增加锁或原子变量。
   新 stream 的挂入先执行时，取消空闲回收；定时器先执行时，连接进入 drain，
   后续请求遵循已有 GOAWAY 和停止接纳逻辑。判定以服务端实际接纳为准，不以字节抵达内核的时间为准。

   零超时也只提交定时器，避免在 stream 通知内部同步改变连接状态。
   同一读批次里已接纳的新 stream 可以在 timer callback 前撤销该 timer。

   `cancel_idle_timer()` 仅在 entry 在堆中时调用所属 loop 的 `cancel`。
   析构函数体先取消该 timer，再执行现有 worker hook 断言；保持成员销毁和 CloseGate 协议。
   从未 start、start 失败和已经关闭的对象通常没有挂起 timer，取消逻辑为空操作。
   有活动 timer 的对象仍必须在其所属 loop 上销毁，不扩展为跨线程析构。

   `request_shutdown()` 和 `request_drain()` 继续通过底层状态变化触发 timer 清理，
   不另写一套关闭状态；即使外部经 `http2()` 发起底层 shutdown，也能清理 timer。

8. **文件改动清单与兼容性**

   | 文件 | 改动 |
   |---|---|
   | `include/fiber/http/Http2Connection.h` | 添加 `has_active_streams()`，补充通知契约 |
   | `include/fiber/http/Http2ServerOptions.h` | 末尾增加 `idle_timeout` 和注释 |
   | `include/fiber/http/Http2ServerConnection.h` | 构造参数、直接 include、timer 成员和辅助函数声明 |
   | `src/http/Http2ServerConnection.cpp` | 参数断言、回调接线、计时及清理逻辑 |
   | `src/http/endpoint/Http2Endpoint.cpp` | 启动前配置校验、构造参数传递 |
   | `tests/Http2ConnectionTest.cpp` | 验证查询与 peer stream 变更通知的实际契约 |
   | `tests/Http2ServerConnectionTest.cpp` | 新增真实 owner 的回收行为测试 |
   | `tests/Http2EndpointTest.cpp` | 验证非默认配置传递和负值拒绝 |

   当前 `CMakeLists.txt` 已通过 `tests/*Test.cpp` 的 `CONFIGURE_DEPENDS` glob 把上述文件
   加入 `fiber_tests`，重新配置即可纳入新测试，不需要重复追加 source 条目。

   当前直接构造 owner 的调用方还包括 `apps/nacos/tests/ConfigServiceTest.cpp`、
   `NamingServiceTest.cpp`、`NacosRpcTest.cpp`。三个参数形式保持源码兼容，但默认会启用 70 秒回收。
   长时间需要保留空闲连接的调用方应显式传 `milliseconds::max()`；不为通过测试而统一禁用。
   公共类布局和构造符号发生变化，使用该静态库的调用方需重新构建。
   直接使用底层 `Http2Connection` 的服务端不会自动获得这一包装层策略。

9. **测试与验收**

   使用真实 `Http2ServerConnection`、`ServerRequestFactory` 和受控传输/对端帧交互覆盖主体逻辑。
   尽量复用已有 HTTP/2 帧构造和 EventLoop 测试支持，避免另造协议栈或修改生产 API 供测试使用。

   | 测试场景 | 验收结果 |
   |---|---|
   | 初始进入 Running 后一直无请求 | 到期发送 `GOAWAY(NO_ERROR)`，最终成功关闭 |
   | 一个请求结束 | 从最后一个 stream 移除后等待完整期限 |
   | 多 stream 分别结束 | 只有最后一个移除后才启动回收 |
   | 空闲期间接纳新 stream | 原期限不会关闭连接；新请求结束后获得新期限 |
   | 半关闭或持续开放的 stream | 跨过 idle timeout 仍存活；隔离 read timeout 干扰 |
   | 空闲期间持续 PING/ACK | 对端仍有入站活动，也按原期限回收 |
   | 空闲期间多次 SETTINGS 容量通知 | 通知不会重新计时 |
   | RST_STREAM 移除最后一个 stream | 启动空闲回收 |
   | 禁用 idle timeout | 多个观察周期内无本策略的 GOAWAY；测试最终主动关闭 |
   | 零 timeout | 异步发起 drain，不在容量通知中同步销毁或递归关闭 |
   | Start 阶段未完成 preface | idle timer 不启用，原 read timeout 仍有效 |
   | 到期和新请求的两种执行顺序 | 已接纳请求受保护；已 drain 的连接不再接纳 |
   | 已挂 timer 后主动 drain/shutdown、对端断开 | timer 取消，关闭完成与对象销毁后无残留回调 |
   | 从未 start / start 失败 / 重复关闭 | 清理安全，没有 timer 遗留 |
   | 关闭前存在写阻塞 | GOAWAY 尽力冲刷，写超时仍可终止关闭 |
   | endpoint 设置明显不同于默认值的短超时 | 通过 endpoint 建立的连接按该值回收 |
   | endpoint 使用负值 | `start()` 返回 Invalid，尚未绑定监听 |

   针对 callback 契约，额外确认 peer stream 挂入时查询为 true、最后一个移除时为 false，
   避免只验证本端 stream，从而遗漏服务端真正依赖的通知路径。
   实际状态转移优先通过协议输入驱动。时间测试使用明确的请求/应答同步点与宽裕观察窗口，
   不依赖相差 1 毫秒的 sleep 排序；顺序竞态用受控事件分别覆盖。
   除验证 read/write timeout 配合的用例外，将这些旧超时禁用或设得足够长。

   实施后的验证命令：

   ```bash
   cmake -S . -B build
   cmake --build build --target fiber_tests -j 4
   ./build/fiber_tests --gtest_filter='Http2ServerConnectionTest.*:Http2EndpointTest.*:Http2ConnectionTest.*:Http2LocalStreamGateTest.*:Http2CloseGateTest.*'
   ./format_code.sh
   git diff --check
   cmake --build build -j 4
   ctest --test-dir build --output-on-failure --timeout 60
   ```

   完成全部实现后统一格式化；检查格式化 diff，避免带入任务外改动。
   全量构建包含可用的调用方，CTest 单独记录未配置、环境受阻及原有失败，不把它们报告为通过。

10. **实施顺序**

    先补查询接口与通知契约，再实现 owner timer 和参数约束，接入 endpoint 配置，
    最后补齐协议行为和生命周期测试，执行格式化与验证。
    本方案没有要求改写现有 HTTP/2 I/O 驱动、drain 状态机或 client pool。

11. **实施及验证结果（2026-09-09）**

    上述接口、配置传递、回调和定时器清理已实现。新增 15 个
    `Http2ServerConnectionTest` 测试和 2 个 endpoint 测试，扩展已有 peer stream 通知测试。
    当前所有直接构造 owner 的调用方均无需修改；全量构建覆盖 Nacos 测试调用方。

    - `cmake -S . -B build`、定向构建和 `cmake --build build -j 4` 均通过。
    - 第 9 项所列 HTTP/2 定向测试共 151 项，全部通过。
    - `./format_code.sh` 已执行，`git diff --check` 通过。
    - 全量 CTest 共 1883 项：1879 项通过、4 项跳过、0 项失败，耗时 88.15 秒。

    环境条件跳过项为 `Http3ClientTest.NginxInterop`，以及 ConfigService、NamingService、
    NacosRpc 的三个 `RnacosInteropWhenEnabled` 测试。本轮未执行这些外部服务互操作测试。

    随后按要求将默认值从 60 秒调整为 70 秒，并同步文档。此默认值调整完成格式化及差异检查，
    未重新执行上述构建和测试。
