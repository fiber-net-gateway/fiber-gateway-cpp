# HTTP/3 连接按客户端与服务端拆分

状态：已实施（2026-09-11）。基于 2026-09-11 工作区源码设计并落地；
审查修正：`on_push_stream` 返回 `NoError` 视为接受、服务端启动 `Canceled` 不再误报
`InternalError`/`StreamCreationError`，验收矩阵补齐 control/startup/server/client drain 单测。

目标是删除公共 `Http3Connection` 类，将连接级请求管理分别归入
`Http3ServerConnection` 和 `Http3ClientConnection` 的内部实现。保留一份私有控制流组件，
复用 SETTINGS、控制流与 QPACK 流读写。单个请求的解析、body 和 exchange 逻辑继续留在 Request 中。

## 1. 当前结构和改动边界

当前服务端链路：

```text
Http3EndpointWorker
  -> Http3ServerConnection { QuicConnection, Http3Connection }
       -> Http3Connection::Ops::create_server_request
       -> ServerHttp3Request
       -> Http3Connection 请求计数变化
       -> Http3ServerConnection 更新 idle timer
```

当前客户端链路：

```text
Http3Client
  -> Session { QuicConnection, Http3Connection }
  -> Http3ClientConnection { QuicConnection::Lease, Http3Connection* }
       -> ClientHttp3Exchange -> ClientHttp3Request
```

`Http3Connection` 同时携带两端请求状态、两套 drain 策略，还直接引用
`ServerHttp3Request`。客户端已经存在同名公共句柄，本次重构其内部实现。

本次保持现有 wire 行为、错误码、请求结果分类、服务端 idle 默认值和两端 drain 超时策略。
不新增 push、动态 QPACK、HTTP/3 0-RTT、连接池、自动重试或 QUIC transport 能力。
不迁移 HTTP/1、HTTP/2，也不改变 endpoint 保留 UDP 直到全部会话释放的规则。

## 2. 最终结构

```text
Http3EndpointWorker
  -> Http3ServerConnection（地址稳定，一次分配）
       QuicConnection
       QuicLocalStreamGate
       Http3ControlStreams
       服务端请求计数、GOAWAY、idle、任务与清理
       -> ServerHttp3Request

Http3Client（建连编排）
  -> Http3ClientConnectionImpl（地址稳定，一次分配）
       QuicConnection
       QuicLocalStreamGate
       Http3ControlStreams
       客户端请求表、GOAWAY、drain、任务与清理
  -> Http3ClientConnection（可移动句柄，Lease + Impl*）
       -> ClientHttp3Exchange -> ClientHttp3Request
```

`Http3ClientConnectionImpl` 是 `src/http/` 下的私有类型，对应此前评估中的 Impl。
采用独立私有类名，公共头仅前置声明，方便 Exchange 在不包含私有头的情况下保存类型化指针。
不采用公共基类、虚函数协议接口或模板化两端连接。

两端分别安装 `QuicConnection::Ops`，并直接处理 peer bidirectional stream。
共享组件不安装 QUIC callbacks，不携带角色，不创建 Request，不拥有连接生命周期。

## 3. 私有共享组件 Http3ControlStreams

新增 `src/http/Http3ControlStreams.h/.cpp`。

### 3.1 职责和接口

接口轮廓如下；涉及的 event 类型复用现有私有 `Http3ControlStreamDecoder.h`：

```cpp
class Http3ControlStreams {
public:
    struct Ops {
        Http3ErrorCode (*on_control_event)(void *owner,
                                         const Http3ControlStreamEvent &event) noexcept;
        Http3ErrorCode (*on_push_stream)(void *owner) noexcept;
        void (*on_error)(void *owner, Http3ErrorCode error) noexcept;
    };

    Http3ControlStreams(quic::QuicConnection &quic,
                        quic::QuicLocalStreamGate &local_stream_gate,
                        Http3Settings local_settings, void *owner,
                        const Ops &ops) noexcept;

    async::Task<common::IoResult<void>> start() noexcept;
    void accept_peer_stream(quic::QuicStream &stream) noexcept;
    async::Task<common::IoResult<void>> send_goaway(std::uint64_t id) noexcept;
    void stop(Http3ErrorCode error) noexcept;
    async::Task<void> join_readers() noexcept;

    const Http3Settings &local_settings() const noexcept;
    const Http3Settings &peer_settings() const noexcept;
    bool peer_settings_received() const noexcept;
    bool local_control_stream_available() const noexcept;
};
```

构造时保证 owner 和三个回调有效；Ops 使用静态存储。所有操作在连接所属 loop 执行。
回调只允许更新 owner 状态、取消等待和发起关闭，不得同步删除 owner、启动嵌套事件循环，
也不得在 reader 回调中等待 reader 自身退出。

组件负责：

- 本地 control stream 分配、通过传入的 gate 等待 uni credit、发送 SETTINGS preface。
- peer uni stream 分类、关键流重复检查、未知流处理、reader 跟踪及退出。
- SETTINGS 接收与保存；GOAWAY、MAX_PUSH_ID 等角色相关事件交给 owner。
- QPACK encoder/decoder stream 的现有解析行为。
- GOAWAY 帧编码与完整写出；连接选择 ID、发送时机及发送后策略。
- 协议错误通过 `on_error` 交回 owner；保留现有错误映射，避免重复报告同一次错误。

`on_control_event` 和 `on_push_stream` 用 `NoError` 表示事件被接受；其他返回值由组件统一
提交给 `on_error`。客户端收到 push 的现有错误与服务端收到 push 的现有错误分别由 owner 返回。
共享组件不自行决定客户端进入 Draining，也不解释服务端请求截止 ID。

`stop()` 幂等：先禁止新增 reader，再终止正在等待的 peer reader。
本地 control stream 上的挂起写由 owner 的 QUIC 关闭路径唤醒；不能在正常 graceful drain
刚开始时调用 `stop()`，否则会提前终止控制流。
调用者保证 `start()` 与 GOAWAY 写不并发修改本地输出：drain 在启动期间发生时，
先同步关闭请求准入，再等待已登记的启动任务结束；启动失败则直接关闭。
同一连接最多存在一个本地主动 drain 任务。

### 3.2 状态范围

只保留 local/peer settings、本地 control lease、peer reader 侵入式表、每种关键流的 seen
标志、reader WaitGroup 和停止标志。seen 标志不能因 reader 退出而清除。
原来的三个 peer stream 裸指针仅用于注销时清空，可随迁移删除；reader 的存活由 Lease 和表管理。

不放入 `Http3ConnectionState`、请求计数/表、drain timeout、idle timer 或请求准入条件。
SETTINGS 在组件中保存一份，owner 通过查询访问；请求字段大小限制归各自请求策略。
因此它不是改名后的 `Http3Connection`。

## 4. Http3ServerConnection

继续作为 `src/http/` 私有类型。保留 `create()`、`quic()`、`start()`、
`graceful_shutdown()`、`Ops::on_closed` 和 registry 的对外连接方式。

### 4.1 合并职责

删除 `h3_`、`make_http3_options()` 和 H3 Ops 中转。
连接直接实现 QUIC stream factory、peer attached、state/capacity callbacks。

| 原位置 | 新位置 |
|---|---|
| H3 创建服务端请求的回调 + owner 再次转发 | `Http3ServerConnection` 直接调用 `ServerHttp3Request::create` |
| `handle_peer_stream_attached` 服务端分支 | 服务端连接请求准入、登记与启动 |
| `live_server_requests_`、`server_request_group_` | 服务端连接 |
| `end_server_request()` | 服务端连接，直接更新 idle timer |
| `goaway_request_id()`、请求 ID 上界 | 服务端连接 |
| `run_server_graceful_shutdown()` | 服务端连接 |
| H3 reader 和本地 control stream | 内嵌 `Http3ControlStreams` |

保留请求计数与 WaitGroup：计数用于 O(1) idle 查询，WaitGroup 用于异步 drain，
不为消除这两个字段而扩大修改通用同步原语。
以 `next_rejected_request_id_ = 0` 表示尚未接收任何请求；接收后更新为
`max(old, stream_id + 4)`，替代 `last_peer_request_stream_id_` 和 `any_peer_request_seen_`。
边界值保持现有可编码范围与行为，不在本次重构中另改 GOAWAY 协议策略。

### 4.2 构造与准入

`create()` 完成参数检查、对象构造和 QUIC app ops 安装后才返回成功对象。
失败时对象尚未交给 endpoint、没有启动协程，由工厂直接释放并返回空指针。
删除重复的 `prepared_`；成功返回时连接状态为 Prepared。

保持请求可在 Prepared、Starting、Running 被接收的现有规则，防止握手附近的请求被误拒绝。
实际完成 QUIC stream attach 后，才登记请求计数并启动 reader；分配成功但 attach 失败不计数。
进入 Draining 后拒绝新请求，不依赖 GOAWAY 写出成功才关闭准入。
注册 app ops 必须早于将 QUIC lease 暴露给 endpoint。

### 4.3 Request、idle 与 drain

`ServerHttp3Request::create`、构造函数和 `start_read_loop` 改为接收
`Http3ServerConnection&`。构造时仍持有 QUIC lease；在正式登记并启动 reader 时，
记录需要结算的连接指针，未登记请求不能执行 `end_server_request()`。

计数只在 Request 析构时结算。handler 返回、最后一次 write 返回和读协程结束，都不等价于
QUIC 已交付响应。`end_server_request()` 断言计数非零，减计数、结算 WaitGroup，再更新 idle。
删除旧 `Http3Connection` 析构时替测试清零计数的兜底，并修正测试析构顺序。

idle 仍从无请求的持续时间计算：初次 start 时计时；有请求时撤销；最后请求销毁后重新计时；
关闭/drain 时撤销；控制帧活动不重置计时。保留零值禁用。

graceful shutdown 同步进入 Draining、撤销 idle，启动一个 drain 任务：
等待已开始的 SETTINGS 启动过程完成 -> 写出请求截止 ID 的 GOAWAY -> 等待请求计数归零 -> close。
Prepared 且尚未启动、或无法建立 control stream 时走关闭路径。
服务端不新增 H3 drain deadline，仍受现有 QUIC liveness 行为约束。

## 5. Http3ClientConnection 与 Impl

新增 `src/http/Http3ClientConnectionImpl.h/.cpp`，将 `Http3Client::Session` 和旧 H3 的
客户端状态移入该对象。它不可复制、不可移动；QUIC、gate、control 组件按值内嵌。
公共句柄继续是两个成员：`QuicConnection::Lease quic_` 和 `Http3ClientConnectionImpl *impl_`。
空句柄和 moved-from 句柄允许空指针，内部已构造对象使用引用或断言建立必需依赖。

### 5.1 公共接口

保留现有默认构造、move、`valid()`、`open_exchange()`、`shutdown()`、
`graceful_shutdown()`、`wait_closed()` 和 `quic()`。
删除 `http3()`，将必要查询直接放到句柄：

```cpp
bool accepting_requests() const noexcept;
Http3ConnectionState state() const noexcept;
Http3ErrorCode close_error() const noexcept;
bool peer_settings_received() const noexcept;
const Http3Settings &local_settings() const noexcept;
const Http3Settings &peer_settings() const noexcept;
bool peer_goaway_received() const noexcept;
std::uint64_t peer_goaway_id() const noexcept;
```

`valid()` 和 `accepting_requests()` 对空句柄返回 false；关闭和等待对空句柄无操作。
其余查询要求 valid，在边界断言。解析上限、gate、请求注册方法和关键流 seen 查询留在私有 Impl。
`Http3ConnectionState` 枚举移至公共 `Http3Protocol.h`，保留现有名称与枚举值；删除的是公共类，
无需为状态枚举额外制造调用方改名。

### 5.2 请求管理

Impl 持有客户端请求侵入式表、请求 WaitGroup、peer GOAWAY ID/标志、drain timeout。
`Http3ClientRequestEntry` 移到 Impl 私有头，保留现有拒绝/连接关闭回调，避免同时重写请求表。
移除未实现功能的 `enable_push` 选项；两端仍执行当前禁用 push 的路径。

`accepting_requests()` 必须同时检查 H3 Running、未收到 peer GOAWAY、
`quic.accepting_new_streams()`，保留 QUIC 已关闭准入但 H3 状态尚未更新时的拒绝行为。
peer GOAWAY 校验、非递增 ID、结果分类、拒绝表项的顺序保持现有行为。
先进入 Draining，再取消 gate 中尚未分配 bidi stream ID 的等待者，随后处理已登记请求。
取消/拒绝回调允许请求注销自己；遍历必须先保存 next，并在回调前完成表项和 WaitGroup 结算。

本地主动 graceful shutdown 同样同步禁止新请求，并显式取消等待 bidi credit 的请求，
然后发送当前实现使用的 GOAWAY ID 0，按 `drain_timeout` 等待已登记请求，最后关闭。
这明确覆盖此前只有 peer GOAWAY 显式取消 bidi gate 的缺口；新增回归验证未发送请求仍为 NotSent。
已有请求仍按原有响应完成/拒绝/取消规则结算，不引入自动重试。

### 5.3 Http3Client 和 Exchange 迁移

`Http3Client` 保留 TLS/QUIC 建连、ALPN 验证和错误 phase 映射。
`make_h3_options()` 改为构造 Impl 的客户端 options，保留 field section size 的当前映射。
删除内嵌 Session 类，将工厂和清理实现移至 Impl。

`last_created_session_` 改为 `last_created_connection_`，继续只用于同步 `start_connect()`
工厂回传。在第一次 co_await 前取得并清空，保持该窗口没有挂起点；不为此次重构扩展 QUIC 工厂 API。
建连失败、ALPN 错误或 H3 启动失败仍由 attempt/lease 驱动释放，不能拿到裸指针后再手工重复删除。

`ClientHttp3Exchange` 删除接收 `Http3Connection&` 的构造，只保留客户端句柄构造。
其公共头前置声明 Impl，私有成员改为 Impl 指针；由 `Http3ClientConnection` 友元访问提供指针，
不增加公共 `impl()`。`ClientHttp3Request` 也直接依赖 Impl。
两者保存的是稳定 Impl 地址，不能保存可移动句柄对象的地址。

保持 pool 和有效父连接所有者长于 exchange 的现有契约。移动连接只转移这个所有者，旧句柄置空；
旧句柄析构不得关闭移走的连接。已有 Request 自己持有 QUIC lease。
本次不把未发送的 Exchange 改为独立拥有连接，也不增加一份共享所有权。

## 6. 生命周期与关闭契约

### 6.1 状态和任务

两端分别持有一份 H3 状态，组件不复制状态机：

```text
Init -> Prepared -> Starting -> Running -> Draining -> Closing -> Closed
                  任一尚未关闭阶段均可因失败进入 Closing
```

Init 只在构造/准备阶段可见。启动 coroutine 完成时，只有仍处于 Starting 才写 Running，
不能覆盖启动期间已经进入的 Draining/Closing。

保留启动任务和 drain 任务各自的计数，reader 计数在 control 组件中。
必须在 spawn 前登记任务；每条退出路径只结算一次。
不增加每请求调度层、虚调用、`std::function` 或额外控制组件分配。

### 6.2 QUIC callbacks

每个 owner 独占一套 app ops：

- `create_stream`：服务端创建 Request 或普通 uni stream；客户端按现有规则处理 peer stream。
- `on_peer_stream_attached`：bidi 交给本端角色逻辑，uni 交给 control 组件。
- `on_state_change`：转发 gate 状态通知，服务端在 QUIC 退出可服务状态时撤销 idle timer。
- `on_capacity_change`：转发 gate 容量通知。

保留当前 QUIC 的状态通知顺序，不在 H3 回调中删除 owner。尤其 Closed 回调发生在 QUIC
清理 stream 表之后；最终析构仍由 endpoint 脱离与最后一个 lease 释放共同决定。
不得通过给 QUIC 设置第二套 app ops 来让共享组件接管通知。

### 6.3 close、wait_closed 和对象释放

`close(error)` 幂等，按顺序：发布 H3 Closing -> 取消本地 gate 等待 -> 客户端注销/通知请求
-> 停止 peer readers -> 调用 `quic.close_application(error)`。
服务端撤销 idle，但不得直接把尚未析构的请求计数清零。

本次保持公开 `wait_closed()` 的 H3 任务完成边界，明确记录在公共头：

- 用于已发起 shutdown/drain 或被动关闭后的等待；它本身不发起关闭。
- 等待本连接登记的启动、reader、drain 任务退出，随后 H3 状态为 Closed。
- 不承诺 QUIC 已从 endpoint 脱离，不等待公共句柄自己的 lease 被释放，也不代表对象已析构。
- 不在健康 Running 会话上把它当作完整 QUIC 关闭通知使用。

内部抽出私有 `join_protocol_tasks()` 执行同一等待，清理任务不计入它自己等待的组。
公共等待在开始执行时取得临时 QUIC lease，保证挂起期间 Impl 存活；调用者仍须保证 lazy Task
开始执行之前句柄有效。内部 `on_destroy` 清理不能重新取得 lease，也不能调用重新 retain 的公共等待。

QUIC `on_destroy` 仅在未附着 endpoint 且 refcount 为零时触发：标记 cleanup 已开始，
在连接 loop 上等待未完成的协议任务，然后服务端调用一次 `on_closed`，最后删除对象。
没有 loop、没有启动任务的构造失败对象直接销毁。成员声明保持 QUIC 先构造、最后析构，
control/gate 析构前任务均已退出。最后 Request 析构仍必须先结算计数，再释放它持有的 QUIC lease。

服务端 worker 在 `on_closed` 摘除 registry 节点并结算 live 计数。
UDP 关闭仍发生在 `live_.join()` 完成之后。本方案不新增完整 QUIC close gate；
若未来需要增强公开等待语义，应单独设计与验证。

## 7. 文件和调用方迁移

| 文件 | 改动 |
|---|---|
| `include/fiber/http/Http3Connection.h`、`src/http/Http3Connection.cpp` | 删除 |
| `src/http/Http3ControlStreams.h/.cpp` | 新增内部控制流组件 |
| `src/http/Http3ClientConnectionImpl.h/.cpp` | 新增稳定客户端连接对象，吸收 Session 和客户端策略 |
| `src/http/Http3ServerConnection.h/.cpp` | 合并服务端逻辑，删除 h3_ 与中转回调 |
| `include/fiber/http/Http3ClientConnection.h`、对应 `.cpp` | Impl 句柄和直接查询接口，删除 http3() |
| `include/fiber/http/Http3Client.h`、对应 `.cpp` | 去除公共 H3 依赖，迁移 Session 工厂 |
| `include/fiber/http/Http3Protocol.h` | 接收公共状态枚举 |
| `include/fiber/http/ClientHttp3Exchange.h`、对应 `.cpp` | 删除通用连接构造，保存 Impl 指针 |
| `src/http/ClientHttp3Request.h/.cpp` | 连接和 RequestEntry 改用私有客户端类型 |
| `src/http/ServerHttp3Request.h/.cpp` | 改为服务端连接；保留析构结算 |
| `src/http/endpoint/Http3Endpoint.cpp` | 验证工厂失败与 registry/live 配对；保持对外接口 |
| `example/http3_benchmark_client.cpp` | `connection.http3().accepting_requests()` 改为直接查询 |
| `tests/Http3ConnectionTest.cpp` | 按公共控制流、服务端请求、客户端连接分拆并删除旧文件 |
| `tests/QuicUdpEndpointTest.cpp` | 用私有 control fixture 或直接编码控制流测试原 QUIC 调度行为 |
| `tests/QuicLocalStreamGateTest.cpp` | 更新旧 H3 owner 注释，保留 gate 测试 |
| `feature/http3_client.md` | 实施完成后更新当前组件、等待契约和验证命令 |

`Http3ServerOptions` 和 `Http3Client::Options` 的用户配置默认值不变。
服务端 QPACK 字符串限制仍从 Request 的 `header_large_size` 推导，不能误用客户端限制覆盖它。
服务端 CONNECT setting 的 OR 映射保持不变。

当前 CMake 用 `CONFIGURE_DEPENDS` glob 收集 `src/*.cpp` 和 `tests/*Test.cpp`，
新文件会自动参与构建；重新配置后核对实际目标源列表，无需机械添加重复 source。
白盒测试已有私有 `src/` include root；不向 `fiber_lib` 消费者传播私有头目录。

这是源码兼容性变更，不保留旧类 alias 或转发壳。仓库内所有编译调用方在同一改动中完成迁移。
仓库外直接使用旧公共类的消费者需按迁移表升级；本仓库构建不能证明它们已经兼容。
历史报告保留历史类型名，不做全量文本替换；当前 API 文档应更新。

## 8. 实施顺序

1. 提取 `Http3ControlStreams`，让旧 H3 暂时委托控制流；先跑原控制流/协议测试确认行为等价。
2. 合并服务端 owner、Request 依赖和任务管理，迁移服务端 fixture，验证 idle/drain/响应交付。
3. 合并客户端 Impl 和 Session，迁移句柄、Exchange、Request、工厂，验证移动和 GOAWAY/gate。
4. 迁移剩余 QUIC 测试、benchmark 和当前 API 文档，删除旧类、旧 include 与中转 API。
5. 统一格式化、完整构建、集中回归及 CTest；核对公共头可独立消费。

前几步可有临时兼容路径以便逐步编译，最终提交不能残留两套业务实现。
每一步只跑受影响测试；最终集中验证后，不在没有新变更时反复重跑相同检查。

## 9. 验证与完成条件

测试文件划分为 `Http3ControlStreamsTest.cpp`、`Http3ServerConnectionTest.cpp`、
`Http3ClientConnectionTest.cpp`，保留现有 `Http3ClientTest` 和 `Http3EndpointTest`。
原服务端 headers/body/FIN/流控测试可留在服务端连接测试中，不改其断言语义。

测试 fixture 用角色专属工厂创建实际 owner 并持有 QUIC lease。先停止协议、释放 Request/Stream
lease，再释放连接 lease，等待 owner 清理完成后才销毁 loop、handler/options 和 buffer pool。
不重新提供一条绕过生产所有权的公共 `Http3Connection` 测试接口。
组件单测允许栈上 control/gate/QUIC，但必须先停止并 join 所有 reader，再按依赖顺序析构。

| 验证范围 | 必须证明的行为 |
|---|---|
| control | SETTINGS 首帧、重复 SETTINGS、重复关键流、关键流 FIN/reset、QPACK 解析、未知 uni 流及角色相关事件分发 |
| startup | uni credit 不足时可等待/取消；启动期间 peer uni/bidi 处理；drain 与 SETTINGS 写不交错；失败后没有悬挂任务 |
| server | drain 后拒绝新请求；已接收响应交付后才减计数；idle 从最后 Request 销毁重新计时；未发请求的连接可回收 |
| client GOAWAY | ID 非递增校验、拒绝 ID 边界、保留已接收请求、取消尚未分配 ID 的 gate 等待、NotSent/Rejected 分类不变 |
| client local drain | 立即取消等待 bidi credit 的请求；已登记请求仍按 drain_timeout 完成或取消 |
| move | 创建未发送/已发送 exchange 后移动句柄仍指向同一 Impl；旧句柄析构不关闭新句柄；move assignment 清理旧目标连接 |
| lifetime | 等待期间 lease 保活；wait_closed 无需释放自身句柄；最后 lease 释放后 owner 只销毁一次；on_closed 只通知一次 |
| failures | 工厂失败、握手失败、ALPN 失败、H3 start 失败以及关闭与 gate 容量通知相遇时，计数/等待队列均正确释放 |
| integration | 流式请求/响应、partial write、Extended CONNECT、endpoint drain、Server 启动回滚、lite_nginx 原有回归 |

实施完成后的计划命令（本设计阶段未执行构建/测试）：

```bash
cmake -S . -B build
cmake --build build -j2 --target fiber_tests
./build/fiber_tests --gtest_filter='Http3*:QuicLocalStreamGateTest.*:QuicUdpEndpointTest.*:ServerLifecycleTest.*'
./format_code.sh
git diff --check
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

若存在已配置的外部 interop 环境，单独报告其运行结果；跳过不能算互操作通过。
共享控制流正常保持长期存活，部分真实 QUIC 测试可能等待 transport idle timeout，
不能用过短的外层 timeout 把正常清理误判为挂起。

最终检查编译路径中没有 `Http3Connection` 类、旧 header include 或 `.http3()` 调用；
允许保留 `Http3ConnectionState` 和 `Http3ConnectionRegistry`，它们不是旧连接类。
公共头只依赖 `include/fiber/`，在只有公共 include root 的消费端可编译。
性能验收至少记录改动前后两端 owner 的 `sizeof`，确认每会话仍一次分配，
未新增每请求分配或请求热路径的同步回调层；不以删除类名直接宣称吞吐提升。
实测记录（Clang 17+/x86-64，2026-09-11）：旧 `Http3ServerConnection`（内嵌 `Http3Connection`）
7848 字节 -> 新 `Http3ServerConnection` 7688 字节；`Http3ClientConnectionImpl` 7616 字节；
内嵌 `Http3ControlStreams` 192 字节。两端均保持每会话单次 `new (nothrow)` 分配。
