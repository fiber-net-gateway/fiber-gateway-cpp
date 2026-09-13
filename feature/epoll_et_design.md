# 持久 epoll ET 与用户态就绪管理设计

日期：2026-09-13。状态：已实施；验证记录见 §14。

设计基线：源码 `e5ad72c`、[全协议压测报告 #4](all_benchmark_4.md) §4.1–4.3。§1–13 说明目标与约束，§14 记录实际落地与验证。

## 1. 目标与最终决策

将网络稳态 I/O 从「每次等待注册、每次完成撤销」改为「fd 生命周期内持久 ET 注册，就绪状态与业务订阅在用户态维护」。直接设计最终 ET 形态，不以保留 ONESHOT 注册作为中间产品，也不维护 HTTP 专用的第二套事件机制。

最终决策：

1. 已连接 TCP/Unix stream 和 UDP socket 首次在 owner loop 激活时，一次注册 `EPOLLIN | EPOLLOUT | EPOLLET`；stream 额外注册 `EPOLLRDHUP`。不使用 `EPOLLONESHOT`。读写回调增删、超时、取消、背压暂停均不修改内核兴趣。
2. `RWFd` 保存每方向的 `Unknown / Ready / Blocked` 状态，I/O 适配层按真实 syscall 结果更新。回调通知完成不等于 fd 已不就绪。
3. TCP 短读在受约束的 stream 路径中可标记读侧耗尽；UDP、TLS 明文短读、短写不能套用同一规则。
4. 因预算退出而未耗尽就绪状态时，通过有界本地调度继续；因背压退出时，保留就绪状态但停止调度，待业务解除背压后继续。
5. Poller 使用带代次的注册 token，EventLoop 每次分发前解析 token，禁止解引用已销毁对象。注册安全与 ET 一起完成。
6. TLS BIO 必须进入统一的 socket I/O 状态维护路径；保留 TLS 自身缓冲、WANT_READ/WANT_WRITE 和重试参数语义。
7. 保留跨 loop I/O 和原 loop 恢复能力；纯 owner loop 是优化路径，发生外部 I/O 的 fd 使用明确的保守兼容路径。

目标是 keep-alive 稳态请求不产生 ADD/MOD/DEL；首次注册、连接建立交接和关闭仍允许系统调用。「0 次/请求」不包含连接生命周期成本，也不承诺所有 `recv(EAGAIN)` 消失。

本方案不同时调整 H2 编码预算、TLS 合并大小、body 缓冲池、协程分配器或 QUIC pacing。H2/QUIC pump 的修改仅限于适配 ET 的继续执行与暂停恢复，便于独立衡量收益。

## 2. 当前实现与必须一起修正的边界

| 位置 | 当前行为 | ET 所需变化 |
|---|---|---|
| `src/net/detail/RWFd.cpp:34` | `Mode::OneShot`；回调集合决定兴趣 | 固定 ET 注册；订阅与内核兴趣分离 |
| `src/net/detail/Efd.cpp:92` | `watch_set(None)` 执行 DEL | RWFd 不再按订阅调用 `watch_set`；解绑才撤销注册 |
| `src/net/detail/StreamFd.cpp:106` | 协程先 try I/O，再等待 | try I/O 根据就绪状态决定是否需要 syscall |
| `src/net/detail/TlsStreamFd.cpp:109` | BIO 直接 recv/send，data 保存 fd 整数 | BIO 关联稳定的 stream 状态对象，统一维护底层就绪 |
| `src/net/detail/DatagramFd.cpp:539` | recvmmsg/sendmmsg 等自行处理 WouldBlock | 所有入口同步方向状态，不能只改单包路径 |
| `src/event/EventLoop.cpp:177` | 直接解引用 `epoll_event.data.ptr` | 先验证注册 token 的槽位和代次 |
| `include/fiber/event/EventLoop.h:380` | `drain_defer<true>` 可持续处理回调新提交的任务 | 有界处理，并在队列非空时使用零超时 poll |
| `src/event/Poller.cpp:41` | 无 RDHUP 注册；ERR/HUP 映射依赖 interested | 明确 ReadHangup 与 Terminal，保留半关闭语义 |
| `include/fiber/net/detail/AcceptFd.h` | accept 成功、取消后移除读兴趣 | listener 生命周期内保持 Read ET |
| `include/fiber/net/detail/ConnectFd.h`、`HappyEyeballsConnectFd.h` | 每个 connect attempt 使用 Efd | attempt 内单次 ET 注册，结束时解绑交接 |

已有 `RWFd::DispatchGuard` 能处理当前回调销毁当前 owner，但不能保护 epoll 返回数组中另一个已被销毁的 Item，也不能只靠它识别同一对象 close/attach 后的新 fd。

已有测试包含跨 loop TLS 握手/读写和跨 loop `try_write` 错误返回。本次不能通过新增 `in_loop()` 断言使这些能力失效。

## 3. 参考行为与适用边界

Linux 文档允许在 ET 下固定注册 IN/OUT；未耗尽的数据需要用户态 ready list 继续处理，而不是再次阻塞等边沿。本文采用该模型。[epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html)

RDHUP 表示对端关闭写方向，HUP 时仍可能有未读数据；ERR/HUP 无须显式订阅即可报告。本文区分方向关闭与连接终止通知。[epoll_ctl(2)](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html)

仓库脚本固定 Nginx **1.31.3**；已确认 `temp/nginx-1.31.3/configure` 和 `temp/nginx-install/sbin/nginx` 可执行，直接复用现有准备结果。核对到的实现参考：

| Nginx 位置 | 观察到的行为 | 本项目采用方式 |
|---|---|---|
| `temp/nginx-1.31.3/src/event/modules/ngx_epoll_module.c:701` | connection 一次注册 IN/OUT/ET/RDHUP | 固定网络兴趣 |
| 同文件 `:839`、`:909` | 分发前、读回调后再次检查 connection instance | 使用注册槽位与代次，适配可立即析构的 C++ 对象 |
| `temp/nginx-1.31.3/src/os/unix/ngx_recv.c:155` | 短读时清 ready，但 pending EOF 时保留 | 将规则限制在底层普通 stream 读取，并显式处理 RDHUP |
| `temp/nginx-1.31.3/src/os/unix/ngx_send.c:41` | 短写可清写 ready | 本项目保守地仅在 EAGAIN 清写 ready，允许多一次尝试 |

以上是本地源码观察；本文的跨线程兼容、token 注册表和回调契约是本项目设计，不是 Nginx 行为的机械移植。

## 4. 分层职责与注册生命周期

### 4.1 Poller：只管理内核注册与事件身份

Poller 保留 LT/ET 等通用能力，EventLoop 的 eventfd、timerfd 不因网络切换被强制改成 ET。网络消费者不再使用 OneShot；仓库内无使用者后可移除其专用辅助逻辑。

每条注册返回一个 64 位 token，由 32 位槽位索引和 32 位 generation 组成，存入 `epoll_event.data.u64`。Poller 持有按需增长、free-list 复用的注册槽位，槽位记录 Item 指针、fd、generation 与注册状态。实现时合并可由空指针等字段推导的状态，不重复保存无必要的布尔值。

- 注册表属于 Poller，生命周期覆盖一次 wait 返回数组；不能把 generation 放在可能已被释放的 Item 中再去检查它。
- EventLoop 对每个事件调用等价于 `resolve(token)` 的 O(1) 查询；匹配且有效才读取 Item。不能先把整批解析成裸指针，再调用任意用户回调。
- MOD 保持 token；DEL 先使用户态 token 失效，再撤销内核注册。即使 DEL 报错，后续旧事件也不能访问 owner。
- ADD 失败回收槽位，不发布有效注册；MOD 失败保留旧内核兴趣的用户态描述，不能预先修改 Item 元数据。
- 槽位再次使用时 generation 增加；回绕时退休该槽位，不允许旧 token 再有效。需要 slot 增长失败时返回 NoMem，不能退回裸指针。
- timerfd 使用保留 token，wakeup eventfd 使用普通注册；两个 wait 后端都采用相同 token 规则。
- 关闭或解绑使用持有的注册句柄，不按 fd 数值误删新对象；不得用整数 fd 作为跨生命周期身份。

注册表不在每次事件或每次请求分配。按连接高水位复用；记录槽位字节数、容量及活跃数，并在大量空闲连接验收中核算常驻成本。实现使用私有连续槽位数组，容量不足时倍增；仅在注册冷路径 realloc，且不向外暴露槽位地址，因此搬迁不影响 token。不能在热路径引入哈希表或逐事件堆对象。

### 4.2 Efd：保存 fd 所有权与固定注册

Efd 提供「确保注册」和「解绑」的清晰接口，接口命名在实现时落定。`watch_set/add/del` 如仍供其他消费者使用，保留其明确的内核操作语义；不得偷偷改成用户态订阅 API。

`attach` 只接管 fd，可在启动阶段执行；首次 owner-loop I/O、注册业务回调或安装 waiter 时确保注册。这样保留未运行 loop 时构造 listener、以及现有跨 loop 创建对象的用法。首次 owner-loop syscall 前完成注册，之后的短读判断才有持续监视作保障。

固定兴趣由 fd 用途确定：

| 用途 | 固定注册 | 撤销时机 |
|---|---|---|
| 已连接 TCP/Unix stream、TLS 底层 socket | Read + Write + ReadHangup，Edge | close / release / 明确转移 owner |
| UDP | Read + Write，Edge | close / release |
| listener | Read，Edge | close |
| 未完成 connect attempt | Write + Terminal，Edge | 成功交接、失败、取消、超时 |

所有网络 fd 必须非阻塞。框架创建的 fd 在创建边界保证；外部 fd 接管路径明确设置或验证，失败通过已有错误通道返回。仍可在 socket syscall 中使用 MSG_DONTWAIT，不能令试探 I/O 阻塞 owner loop。

注册错误由首次激活操作返回 IoErr，回滚尚未发布的 callback/waiter。正常 close 完成撤销后再 close fd；release 必须完成解绑才能交出 fd。当前返回裸 int 的 `release_fd()` 若无法表达解绑失败，需要增加可失败的内部交接接口，不能仅 assert 后仍返回 fd。DEL 失败后 token 已失效，fd 进入待清理状态，不得当成未注册继续 ADD 或继续业务 I/O；关闭时仍 close，交接时返回错误并保留所有权用于清理。

### 4.3 RWFd：就绪状态、订阅和等待

RWFd 的方向状态与回调订阅分离：

| 状态 | 含义 | 本地 try I/O | 仅等待就绪 |
|---|---|---|---|
| Unknown | 新 attach，尚未获得真实方向信息 | 允许一次真实 syscall | 等待内核事件，不把 Unknown 当作成功 |
| Ready | 有继续尝试的依据，不保证一定成功 | 尝试 syscall | 可完成等待 |
| Blocked | 上次确认暂时无法继续 | 返回 WouldBlock，不再次探测 | 等下一边沿 |

新增状态限于两个方向状态、read-hangup 提示、一个去重的通知 entry 与 pending 方向 mask、callback 订阅序号，以及 §9 的外部 I/O 标志。现有 terminal/error 与 callback 槽保留；不另建每请求状态对象。

核心不变量：

- 就绪状态由内核事件和统一 I/O 层维护；协议层不得随意将它清零。
- 取消订阅、完成 waiter、业务数据发送完毕都不清 Ready。
- 回调没订阅时也记录内核就绪；以后重新订阅可得到一次缓存通知。
- 同方向只允许一个消费者，callback 与 waiter 互斥，保留 Busy 行为。不同方向可并存。
- 业务「暂时不想读写」只影响订阅或协议调度；不能伪装成 socket Blocked。
- fd 状态、订阅和本地队列只由 owner loop 修改；跨线程消息遵循 §9。

## 5. I/O 状态转移

### 5.1 事件输入

| 输入 | 状态更新 | 通知 |
|---|---|---|
| IN | Read = Ready | 有读订阅时通知 |
| OUT | Write = Ready | 有写订阅时通知；空闲 fd 不产生业务任务 |
| RDHUP | 记录 read-hangup；Read = Ready | 唤醒读方以读完尾部并观察 EOF；不触发 Terminal |
| ERR/HUP | 记录 terminal 提示；读写可尝试 | 通知当前读写消费者及一次 terminal 订阅 |

Poller/EventLoop 增加可区分的 ReadHangup 事件表示，不能仅依靠 IN 隐式表示 RDHUP。ERR/HUP 不因当时没有 terminal callback 而丢失。Terminal 回调仍表示连接错误/挂断提示，不替代 recv/send 的具体错误，也不要求 RWFd 提前丢弃未读数据。

在所有回调前先合并本条事件的方向与 terminal 状态；读回调销毁对象或变更注册代次后，不再调用旧事件的写回调。

### 5.2 TCP / Unix stream 的实际 I/O

所有标量、向量 read/write 和 TLS BIO socket 操作进入同一结果上报逻辑。

| syscall 结果 | 读状态 | 写状态 |
|---|---|---|
| EAGAIN / EWOULDBLOCK | Blocked | Blocked |
| EINTR | 不变，重试；不能据此进入等待 | 不变，重试 |
| 成功读取请求的全部字节 | Ready | — |
| `0 < n < requested` | 满足短读条件时 Blocked，否则 Ready | — |
| 非零长度读取返回 0 | 保持可立即观察 EOF；清除对未来边沿的依赖 | — |
| 成功写入，包括短写 | — | Ready；下次尝试可进一步确认 EAGAIN |
| 连接级错误 | 保存/上报既有错误，唤醒必要消费者，不再无限等待 | 同左 |
| 参数错误、零长度操作 | 不改变方向状态；零长度读不代表 EOF | 同左 |

短读耗尽优化只允许在以下条件全部成立时启用：owner loop 独占该方向；固定 ET 注册已生效；底层普通 SOCK_STREAM 接收；未观察到 RDHUP/HUP；无外部 I/O 标志；不存在特殊接收 flags 或会使短读不能证明耗尽的已知条件。

readv 的 requested 是经校验的 iovec 总容量，处理长度溢出、平台单次 I/O 上限与实际提交容量；不能用应用的逻辑 body 剩余长度比较。无法证明短读耗尽的路径保留 Ready，下一次 EAGAIN 再清零。H1 的目标是在普通小请求短读后避免下一次 keep-alive 空 recv，而不是对任意 fd 强行推断队列为空。

EOF 不等于连接终止：对端 `shutdown(SHUT_WR)` 后，本端仍可以写响应。数据加 FIN 的场景中，短读若已看到 read-hangup 必须保留尝试能力，不能等待不会再出现的新数据事件。

try 接口仍可能返回 WouldBlock；在优化路径上它可以来自缓存状态。保留原始非阻塞 syscall helper 供 BIO 与兼容路径使用，但业务代码不绕过状态层直接使用 helper。

### 5.3 UDP 与批量 I/O

单包成功、短数据报、零长度数据报均保持 Read = Ready。recvmmsg 返回少于槽位数量，也不推断已耗尽；只有明确 EAGAIN 才 Blocked。

sendmmsg 返回已发送前缀时保持 Write = Ready；不能把剩余未发送部分当成已完成，也不能假定一定阻塞。单包/批量/GSO 路径统一反馈底层状态。

QUIC 的 pacing、cwnd、连接流控造成「当前不能发」不是 socket EAGAIN，不能清 socket 写就绪，更不能安装无意义的 socket 写等待；仍由对应 timer/ACK/流控事件触发重试。

## 6. 回调、协程等待与本地继续执行

### 6.1 持久 callback 的契约

保留 read/write callback 的持久订阅形式，但通知语义明确为「发生了新就绪提示」：

- 内核新事件可以直接分发，延续当前低延迟路径；使用 DispatchGuard 和注册代次检查保护生命周期。
- 安装 callback 时已有 Ready，则通过内嵌 DeferEntry 去重提交一次通知，禁止 setter 内同步调用业务回调。
- pending mask 只表示未交付通知，不表示 ready；交付前移除对应 pending 位，方向仍可保持 Ready。
- callback 被 clear 时清除其未交付通知；若没有其他 pending 位则取消 entry。重新安装即使 callback/ctx 数值相同，也属于新订阅，重新评估当前状态。
- dispatch 检查当前订阅，不能持有跨任意回调的旧 ctx。读回调可能清除、替换写回调；旧写通知不得落到新订阅上。
- 若实际 I/O 已将方向置 Blocked，尚未交付的旧缓存通知可以丢弃；等待后来的真实边沿。
- 同一个 Ready 状态不会自动无限重投持久 callback。否则永久可写 socket 会在用户态重新形成忙循环。

每个 callback 槽附带单调的订阅序号，安装/移除时变化。分发一条多方向事件时快照各槽序号，逐个回调前比较；仅比较函数指针和 ctx 不足以识别「同一对指针清除后重新安装」。使用 64 位序号，禁止回绕复用；该序号只用于订阅身份，不替代 fd 注册 token。延迟通知交付前截取 pending mask 与槽序号并清除本次 pending，新提交的通知属于下一次交付，不能被旧批次顺带清掉。

callback 消费者必须做到其中之一：推进到 EAGAIN；因预算退出并自行排本地 continuation；因背压退出并约定明确的恢复触发。现有「回调只排协议 pump」仍然可用，协议 pump 拥有继续执行责任。

### 6.2 waiter 与 await_suspend

本地 waiter 安装分为三个结果：Error、ReadyNow、Waiting，不再仅用 IoErr 隐含「成功一定挂起」。检查有效性和同方向占用后，确保注册并评估 Ready。

- ReadyNow：记录成功，`await_suspend` 返回 false；不能在 `await_suspend` 中直接 resume 自己。
- Waiting：先发布 callback/waiter 状态，再设置定时器并返回 true。owner loop 内同步执行，安装过程中不调用用户回调。
- Error：不保留订阅和定时器，返回 false 并在 await_resume 返回错误。
- `timeout <= 0` 保持当前 TimedOut 契约，不能因 Ready 优先级变化而改变已有 API 行为。
- 完成、超时、取消只移除该 waiter 的匹配 callback/ctx，不删内核注册，不清 fd 就绪。
- close 的取消、timer 超时和就绪竞争只能有一个完成者；先移除订阅/定时器，再恢复协程。

跨线程安装已有 Ready 时也必须走已有原 loop 通知，不在 owner loop resume 外部协程，见 §9。

### 6.3 有界调度与不丢边沿

当前 `drain_defer<true>` 会不断执行新提交的任务。ET 需要主动 continuation，因此必须一并调整 EventLoop，而不是直接把未耗尽任务无限 post_local。

设计采用分轮快照与总预算：每轮先处理到期 timer，再处理有界跨线程通知、本地快照，随后 poll，再处理本轮事件及有界本地任务。可从每轮 256 个本地/通知回调的总预算开始，作为可调内部常量，不能在前后两个 drain 各自重新获得无限额度。

- 回调新提交的 continuation 排队尾，可留到下一轮；仍用可取消、去重的 intrusive entry。
- 本地或跨线程待处理队列非空时，poll deadline 取 `loop.now()`，绝不以无限或未来 timer deadline 阻塞。
- 每轮仍非阻塞读取新内核事件并更新时间，防止一个忙连接饿死其他 fd、超时及 stop。
- 跨线程 drain 若拆出链表后达到预算，剩余节点要保存在 loop 的待处理链中，保持 FIFO 和节点所有权；不能把节点遗失或重复推回 MPSC。
- 无待处理任务时仍按正常 timer deadline 阻塞，不以周期性空 poll 替代 ET。
- stop 时取消或结清网络通知与 waiter，不能留下指向已销毁 owner 的 DeferEntry。

H2 保留既有 operation/byte budget 与协议级 ready_hint；QUIC 保留自己的收发预算。它们的 hint 表示协议 pump 是否可运行，不应成为第二个可独立清除的内核 readiness 真相。

背压恢复必须是主动入口：例如 body buffer 释放、消费水位下降、连接窗口更新、出站队列从空变为非空时，重新尝试现有数据/就绪状态；不能依赖重新 set_callback 产生 epoll MOD 来补发事件。

## 7. TLS 适配

当前自定义 BIO 的 data 是指针编码的 fd，无法上报读写结果。改为指向 TlsStreamFd 内稳定的 BIO 上下文，至少关联底层 StreamFd；不增加逐 I/O 分配。TLS 对象在 SSL/BIO 存活期间不可移动；销毁顺序保证 BIO 先于所引用上下文失效。

BIO read/write 使用统一的非阻塞 socket helper：

- owner 路径检查/更新底层方向状态，返回真实字节数或符合 BIO 契约的 -1/errno/retry flag。
- 使用缓存 Blocked 时明确设置 EAGAIN 和 retry flag；EINTR 应在 helper 内重试，不能被误当成已阻塞。
- 继续使用 MSG_NOSIGNAL，保持 BrokenPipe 而非 SIGPIPE。
- 修改 BIO ctrl 的 SET_FD/GET_FD 初始化实现，不能残留把 data 同时解释为整数与指针的分支；GET_FD 仍返回真实 fd，BIO 不取得 fd 所有权。
- socket 结果上报不得在 SSL 调用栈中同步触发可能析构 SSL/stream 的 terminal 回调；先记录状态，延迟交付。审计现有 mark_terminal 调用者，形成一致的非重入通知约定。

TLS 有两层独立状态：底层 socket readiness 与 SSL 内部可推进性。`SSL_read` 返回少量明文不证明 socket 耗尽；`SSL_pending`/现有 `has_pending_read` 为真时必须先消费明文，不能被底层 Blocked 拦截。

允许在 socket Blocked 时调用 SSL，由 BIO 决定底层是否需要 syscall；SSL 仍可能从内部缓冲完成操作。SSL WANT_READ/WRITE 按实际等待方向安装订阅，不能以操作名推断方向。非 socket 原因的异步 TLS 等待不能凭空归类为 socket EAGAIN。

WANT 后的 write buffer、长度及密文/明文引用保持现有稳定性约束。ET 唤醒不允许改变尚未完成的 TLS 写操作，也不改变 `TlsTransport::poll_writev` 当前持有 scratch/chain 的生命周期。

## 8. listener 与 connect

listener 使用固定 Read ET，可保留专用 AcceptAwaiter，但必须增加 accept-ready 状态并删除每个 accept 成功/取消后的 unwatch。首次 accept 允许试探；成功后仍 ready，后续 accept 继续消耗 backlog；仅 EAGAIN 清 ready。

一次 accept 仍返回一个结果，不要求 API 一次吐出所有连接。监听循环在预算内连续 accept，预算耗尽则本地继续。没有 waiter 时缓存边沿，新 waiter 先尝试缓存状态。

ECONNABORTED 等可恢复 accept 错误不代表 backlog 耗尽。EMFILE/ENFILE 等资源不足时使用有界退避或资源恢复触发继续尝试，不能既清 ready 等边沿、又不安排重试，也不能无限热重试。

ConnectFd 与每个 HappyEyeballs attempt 显式使用 Edge；EINPROGRESS 后确保一次 Write/Terminal 注册，收到事件后仍必须 `getsockopt(SO_ERROR)` 判断成功。读写 ready 不代表 connect 一定成功。

成功交接执行「失效旧 token → DEL → release fd → 目标 StreamFd attach」，目标首次激活 ADD。这里允许有限的 DEL/ADD，不为省一次建连成本引入复杂注册所有权转移。立即 connect 成功可直接交接，无须创建临时注册。

## 9. 跨 loop 与外部 I/O 的兼容路径

不能只在 owner loop 更新 Ready，却继续允许其他 loop 直接 recv/send：外部读取可能耗尽缓存 Ready，外部写入可能改变 Blocked 的有效性，进而造成重复立即完成或永久等待。

最终方案保留现有跨 loop syscall 行为，并采用 sticky `external_io` 标志作为保守边界：

1. 每个 fd 生命周期初始为 false。off-owner 原始 I/O 在 syscall 前以原子方式发布 true，此后直到安全的 release/attach 边界才重置。禁止在操作还在途时重置。
2. owner 快路径检查该标志；false 时使用本地状态，true 时不使用缓存 Blocked 跳过真实 I/O，也不做短读耗尽推断。只增加一个原子读，不引入全局锁或每请求跨线程消息。
3. off-owner syscall 不访问 owner 的普通 ready 字段、不操作 poller、不调用 owner callback；原有错误直接返回。特别保留跨 loop BrokenPipe 不同步标记 owner terminal 的行为。
4. external 模式安装 waiter/读写订阅时，在 owner loop 对需要的方向做一次零超时 `poll`，验证真实 readiness，而不是相信可能过时的缓存 Ready。先确保持久 ET 注册，再 poll，再安装/完成等待；这三步在同一 owner 回调内完成。
5. poll 显示就绪则完成或排通知；无就绪则等待已有 ET 注册的下一事件。数据在 poll 之后到达会产生待处理事件；注册之前已经存在的数据由首次 ADD 和 poll 覆盖。poll 只观察、不消费数据；随后的 I/O 仍必须容忍 EAGAIN。
6. 已安装的 external 订阅收到内核事件时可以交付提示；新订阅不能重复复用旧提示。直接使用裸 fd 的适配者也必须声明 external 模式，否则不属于受支持的状态缓存用法。

poll 的额外 syscall 只属于使用外部 I/O 的 fd，不进入 lite-nginx 常规 owner-loop 数据路径。它是保留已有能力的明确代价，不是退回 ONESHOT/LT。external 标志不会使 socket、SSL 或 buffer 自动线程安全：同方向操作必须串行，SSL 对象延续现有独占约束。

跨线程 waiter 保留 Notify_Watch / Watching_Event / Notify_Resume 与取消握手机制。owner 根据 §6 返回 ReadyNow 时，先完成 waiter 的状态转移，再向 origin loop 发 Notify_Resume；不能在发布 Watching_Event 前同步完成导致双释放。超时仍由 origin loop 管理。

对象释放要求在途跨线程操作已取消并完成握手。注册 token 只能保护 epoll 事件，不保护包含 `RWFd*` 的任意跨线程消息；不能以 token 机制替代 waiter 生命周期管理。

## 10. close、release 与同批事件安全

close 的有序步骤：标记当前 fd 生命周期结束；撤销 token 和内核注册；取消本地通知；摘下所有 callback/waiter 与必要 timer；关闭 fd；再按既有取消语义完成摘下的操作。

处理完成回调时遵守：owner 可能被第一个回调销毁，不能再访问其成员。close 使用最多三个栈上完成记录保存已摘下的操作；waiter 的完成记录必须持有其在途生命周期保证，跨线程 waiter 继续由取消/恢复握手机制释放。

普通 callback 的 ctx 没有引用计数，因此明确保留一个注册契约：ctx 必须活到自己的取消通知，或者在 close 前撤销注册。允许一个 callback 销毁 RWFd，但不能同时销毁另一个仍待通知 callback 的 ctx；同属一个即将销毁的业务 owner 时，应先撤销其余普通订阅，再销毁。该约束在迁移调用者时逐项检查，不能声称注册 token 能解决任意 ctx 生命周期。read/write waiter 并存的 close 必须分别完成，不能因 RWFd 已销毁而静默遗漏另一个 waiter。

对当前内核事件的 read/terminal/write 多方向分发，回调前保存注册 token，回调后检查 DispatchGuard；owner 尚在时再验证 token 未改变。原地址 placement-new、同对象 close/attach、整数 fd 快速复用均不能接受旧通知。

本地通知 entry 必须在析构前从任何当前快照/待执行队列摘除；EventLoop 的快照仍使用可 O(1) 删除的 intrusive 节点，不能改为无法失效的回调指针数组。

本方案不允许把一条已交给框架的 socket 通过 dup 后同时交给另一个 reader/writer。close 自动移除 epoll 项也不是替代显式生命周期管理的理由；release 和多引用 open-file-description 场景需要明确解绑。

## 11. 实施范围与顺序

最终交付是同一 ET 方案，下面是实现依赖顺序，不是逐阶段让产品在 ET/ONESHOT 间切换：

| 顺序 | 文件/模块 | 完成条件 |
|---|---|---|
| 1 | Poller、EventLoop、对应测试 | token 注册、RDHUP 映射、有界调度与零超时 poll；先证明销毁和饥饿边界 |
| 2 | Efd、RWFd、waiter | 固定 Edge 注册、状态与订阅分离、缓存通知、取消/close/跨线程兼容 |
| 3 | StreamFd、DatagramFd、TlsStreamFd | 全部 syscall 入口反馈状态；短读边界与 BIO 重入处理 |
| 4 | AcceptFd、ConnectFd、HappyEyeballsConnectFd | listener 与 attempt 生命周期适配 |
| 5 | HttpTransport、Http1ClientConnection、Http2Connection、QuicUdpEndpoint 及其他调用者 | 暂停恢复、协议预算、TLS 内部缓冲与方向等待审计 |
| 6 | 测试、格式化、全协议基准 | 正确性先通过，再按 §13 验收 |

调用者审计不能仅搜索 HTTP：覆盖 TCP/Unix、DNS/UDP、gRPC、Nacos、CAT，以及直接使用 `RWFd::fd()`、原生 SSL/BIO、set/clear callback、wait_readable/writable 的 tests/example。对外可见的 detail 头与 callback 通知语义变更在文档中说明；公共头不依赖 src 私有头。

不会保留 `sync_interest()` 按 callback 计算内核兴趣的旧职责；可将其删除或改名为仅确保固定注册的函数。网络路径不再调用 `consume_ready()`，也不再通过 MOD 重新武装。

## 12. 必须覆盖的正确性测试

使用真实非阻塞 socketpair/TCP/UDP 配合可控 loop 顺序；错误返回、批次事件顺序等难以稳定复现的边界可使用私有测试钩子。不能只检查字段值或以 sleep 猜测竞态顺序。

| 场景 | 必须观察到的行为 |
|---|---|
| 注册前已有数据/可写 | 首次 try 与直接 waiter 都能完成；Unknown 不伪造读就绪 |
| 无订阅时 IN，稍后装 waiter | 不依赖新网络包即可继续 |
| 一个边沿，读取一部分且恰好填满小 buffer | 剩余数据在预算续跑中读完，不等新边沿 |
| 普通 stream 短读后 keep-alive 等待 | 不额外空 recv；随后请求能唤醒 |
| 数据与 FIN 同批/FIN 晚于短读 | 先读完数据再得 EOF；本端仍能发响应 |
| RST、只有 ERR/HUP、terminal 迟订阅 | 有限次通知，具体 I/O 不永久阻塞，不空转 |
| 部分写、发送缓冲写满后对端继续读 | 正确返回前缀并继续；不要求每次重新订阅 OUT |
| 长时间只有写就绪、没有业务写任务 | 无持续本地任务或重复 callback 忙循环 |
| clear 后相同 callback/ctx 重新安装 | 旧 pending 不误投，新订阅能取得当前就绪 |
| read callback 清除/替换 write callback | 同条旧事件不投递给新订阅 |
| callback 自毁、同址替换、同对象重 attach | 后续旧事件和本地通知不访问新对象 |
| epoll 同批 A 回调销毁 B，B 的事件排在后面 | token 解析失败后丢弃，不解引用 B |
| fd 数值与 token 槽位快速复用、generation 回绕钩子 | 旧事件永远不命中新连接 |
| ADD/MOD/DEL 失败 | 状态事务一致；失败 release 不交出仍注册的 fd |
| 0 长度 I/O、iovec 边界、EINTR | 不把 0 长度读当 EOF，不误清 Ready |
| UDP 短包、0 字节包、批量短返回、GSO | 后续数据报不丢唤醒，不错误判断 EOF/耗尽 |
| TLS 已有解密数据，底层 Blocked | 立即消费明文，不等待新 socket 边沿 |
| TLS read WANT_WRITE、write WANT_READ | 等待正确方向；无反复假唤醒 |
| TLS 部分 record、WANT 后链/scratch 持有 | 无提前释放、重试参数变化或 BIO 内析构重入 |
| 背压暂停期间仅来一次数据，之后恢复 | 无新包也能继续；暂停期间无重复通知 |
| 单个持续就绪 fd 与另一连接/timer/stop 并存 | continuation 有界，其他工作获得执行机会 |
| timeout/readiness/close/cancel 交错 | 每个 waiter 恰好完成一次，槽位和 timer 清理 |
| 跨 loop 握手、读写、超时、就绪先于 Notify_Watch | 在 origin loop 恢复，owner-only 字段无数据竞争 |
| external mode 多次读写及回到 owner loop | 不复用过时缓存，不永久等待；BrokenPipe 语义保持 |
| listener backlog 多于单轮预算、EMFILE 恢复 | 继续 accept，不需要新连接边沿；不热循环 |
| HappyEyeballs 失败/成功/取消同时发生 | 每 attempt 一次终结，胜出 fd 与旧 token 正确解绑 |
| timerfd fallback 与 epoll_pwait2 | token 分发、超时、队列非空零超时行为一致 |

修改当前 `RearmsPersistentReadCallbackAfterOneShotEvent` 等测试，使它验证最终 ET 可观察行为，而不是保留 rearm 实现假设。原有半关闭、同址替换、跨 loop、SIGPIPE、TLS pending/writev 测试必须继续通过。

实现阶段运行 `./format_code.sh`、`cmake --build build`、`ctest --test-dir build`；补充 ASan/UBSan 生命周期测试与有针对性的 TSan 跨线程测试。具体构建开关以仓库当时配置为准，记录实际命令。实际执行结果见 §14。

## 13. 性能验收与收益边界

首先验收机制，再验收吞吐。报告的 +15～20% 是完整 ET 的估算，不作为保证值；ONESHOT 保留注册实验的 +8～11% 也不能直接当成本方案实测。

| 指标 | 验收要求 |
|---|---|
| 固定 keep-alive 连接的稳态 epoll_ctl | 应无 ADD/MOD/DEL；把预热、建连、关闭与稳态分开计数 |
| 每连接注册 | RWFd 一次 ADD，解绑/关闭最多一次成功 DEL；connect 交接单列 |
| H1 小请求 EAGAIN | 普通 owner stream 短读后的额外探测减少；不要要求 UDP/TLS 等同样归零 |
| 调度成本 | 记录 callback/本地 task/epoll 唤醒数每请求，避免省 syscall 却增加大量 defer |
| 空闲开销 | 大量空闲连接无持续 OUT callback 或本地轮转；测常驻内存和首次 OUT 事件成本 |
| 延迟与公平性 | 记录 p50/p99/max、timer 延误及混合大/小请求，不能只看满载 RPS |
| 全协议回归 | H1 GET/POST、H2 1K/64K/1M/POST、H3 低/高并发、Unix 与 TLS 路径 |

以同构建选项、同连接数、同 worker 绑核交替 A/B，多样本比较 CPU/请求的 user/sys 分量。统计请求成功率、字节完整性、超时与取消；专门观测长时间 keep-alive 是否出现吞吐突降或挂起。

H3 同时报告源端口数与实际 worker 分布；UDP 接收错误计数按可归属范围解释。WSL2 与裸机分开报告，不能把虚拟化 IPI/缺页放大的收益直接外推。

若 epoll_ctl 已降到目标但 H1 收益偏小，优先检查初始/无用 OUT 事件数量、token 解析成本、通知重复排队和 external mode 是否误触发。若只有 H2 大体仍有差距，这是合理边界：本方案并未解决报告指出的输出突发时间分布和大块内存回收。

不以增加 worker、强制 NODELAY、扩大缓冲或关闭 pacing 掩盖 ET 自身退化。最终交付应附每项机制计数与同机性能结果，再决定是否调整调度预算。

## 14. 实施与验证记录（2026-09-13）

### 14.1 已落地行为

- `Poller` 注册使用 index/generation token，分发前校验，DEL 即使失败也先使 token 失效。槽位耗尽时在注册冷路径按倍数扩容连续数组；槽位地址不对外暴露，热路径不分配。generation 达到上限后退休槽位。
- `RWFd` 使用持久 ET 注册、每方向就绪缓存、订阅代次和可取消的本地通知。安装回调不内联调用；owner loop 中已经就绪的 awaiter 通过 `await_suspend(false)` 继续。关闭先拆除注册、通知和 waiter，再完成回调，允许终结回调销毁 owner。
- TCP/Unix scalar/vector I/O 与 TLS BIO 统一更新缓存；socket syscall 带 `MSG_DONTWAIT`，写入保留 `MSG_NOSIGNAL`。UDP 成功收包不推断耗尽。Raw fd 默认采用兼容探测；跨 loop 使用 sticky external 模式和原 loop 恢复流程。
- listener 保持 Read ET，每连续 64 次 accept 让出本轮；资源错误退避 1ms。connect/HappyEyeballs 使用 ET，成功移交前先解绑。EventLoop 每轮 notify/defer 合计预算 256，notify 优先额度 128；本地自重排延至下一轮，存在待办时执行零超时 poll。
- H2/QUIC 继续使用现有 pump 的预算与本地继续机制；未修改协议编码预算、缓存大小或 pacing。

接口变化：`Poller::del(int)` 改为 `del(Item&)`，调用者必须持有原注册项。`RWFd::release_fd()` 解绑失败返回 `-1` 并保留 fd 所有权；调用方不能把失败解释为成功转移。直接绕过 StreamFd/DatagramFd 进行系统调用的使用者应使用 Raw/external 兼容模式，不能依赖未经维护的就绪缓存。

### 14.2 构建、回归与内存安全

执行命令：

```bash
cmake -S . -B build -DFIBER_BUILD_TESTS=ON
./format_code.sh
cmake --build build -j 6
ctest --test-dir build --timeout 45 --output-on-failure
```

Release/Clang 22/ThinLTO 构建成功。最终串行 CTest 共 2005 项，2001 项通过，4 项跳过，0 失败，耗时 79.09s。跳过项是未启用外部环境的 HTTP/3 Nginx 与三个 r-nacos 互操作测试。新增覆盖包括 token 复用及注册失败、同批事件销毁、回调同址重新订阅、关闭回调销毁 owner 后完成已拆除 waiter、短读缓存、半关闭与零长度 I/O，以及持续本地任务下的 timer 公平性。

定向 sanitizer 验证使用 `temp/et-validation/sanitizers.py`：重新插桩全部 `src/event`、`src/net` 编译单元及 10 个相关测试文件，链接其余已有库。它不是全仓库 sanitizer 构建。

```bash
python3 temp/et-validation/sanitizers.py address,undefined
python3 temp/et-validation/sanitizers.py thread
python3 temp/et-validation/sanitizers.py address,undefined timerfd
timeout 45s temp/et-validation/address-undefined/tests
timeout 45s temp/et-validation/thread/tests --gtest_filter='RWFdTest.*:TlsStreamFdTest.CrossLoop*:StreamFdTest.CrossLoop*:EventLoopTest.*'
timeout 45s temp/et-validation/address-undefined-timerfd/tests
```

ASan/UBSan 75/75 通过；定向 TSan 26/26 通过；强制 timerfd 后端的 ASan/UBSan 75/75 通过。日志位于 `temp/et-validation/{build-final,ctest-final,asan-final,tsan-final,timerfd-final}.log`，临时工具与日志不纳入版本控制。

曾执行并行 CTest（`-j 6`），发现 `ScriptHeapExceptionServes500JsonWithName` 失败；该组四个应用测试共用固定 `/tmp/lite_nginx_script_result_test.js`。单独重复该失败项 100 次通过，最终全量串行通过。本次未改动该独立的测试文件隔离问题。

### 14.3 稳态系统调用

保存修改前的可执行文件为 `temp/et-validation/lite_nginx-baseline`，与修改后的 Release 程序使用相同配置：4 worker，后端 CPU 0–3，代理 CPU 4–9。在 16 条固定 H1 keep-alive 连接完成预热后，对 3200 个 1K 请求单独计数，采样结束后才关闭连接。计数不含启动、建连和关闭。

| 指标 | 修改前 | 持久 ET |
|---|---:|---:|
| epoll_ctl | 12888 | **0** |
| recv | 10034 | 7490 |
| send | 6400 | 6400 |
| recv 返回错误 | 3244 | 538 |
| epoll_pwait2 | 4103 | 4762 |
| epoll 返回事件数 | 6444 | 7254 |

该样本没有请求失败；recv 错误计数不应直接视作请求失败。它证明固定连接的稳态内核兴趣变更已消除，同时减少了额外接收探测；不代表全协议的 EAGAIN 均为零。本样本 poll 次数和返回事件数有所增加，省去的 ctl 不等于所有调度成本同步下降；尚未单独计数 callback 与本地 task。命令为 `python3 temp/et-validation/benchmark.py counters`，详细计数见 `temp/et-validation/counters.json`。

### 14.4 同机全协议短测

代理与后端绑核同上，负载工具绑 CPU 10–15。H1 使用 wrk（6 线程、256 连接），H2 使用 h2load（6 线程、32 连接、每连接 16 流），各场景 5s、3 个样本，交替运行修改前后程序，下表取各自 RPS 中位数。H3 使用同一个修改后构建的客户端测试两个代理版本，高并发为 16 线程/16 连接/每连接 4 流，低并发为 1/1/1；预热 1s、测量 3s，仅一个样本。客户端使用默认 pacing 设置，代理配置保持一致。

初次 H1 基线样本出现名义 5s 却报告 3.95s、248 个 timeout 的异常，与原报告的 WSL wall-clock 回拨表现一致；该批废弃。正式矩阵仅对 wrk 进程预加载 `temp/et-validation/wrk_clock.so`，把其 `gettimeofday/time` 统一映射到 `CLOCK_MONOTONIC`，不改变代理或 H2/H3 时钟。正式 H1/H2 均无工具报告的请求错误/超时；H3 全部完成，状态、字节长度、warmup 与正式请求错误均为 0。

| 协议/场景 | 修改前 RPS | ET RPS | 变化 |
|---|---:|---:|---:|
| H1 1k | 107,603.39 | 127,378.71 | +18.4% |
| H1 64k | 61,106.72 | 59,656.13 | -2.4% |
| H1 1m | 7,337.63 | 7,490.97 | +2.1% |
| H1 echo | 3,263.69 | 3,678.04 | +12.7% |
| H2 1k | 138,306.20 | 157,451.40 | +13.8% |
| H2 64k | 23,859.00 | 22,322.40 | -6.4% |
| H2 1m | 2,314.20 | 2,074.60 | -10.4% |
| H2 echo | 1,065.80 | 1,037.40 | -2.7% |
| H3 1k | 93,012.00 | 104,529.00 | +12.4% |
| H3 64k | 30,043.70 | 33,377.70 | +11.1% |
| H3 1m | 2,486.00 | 2,309.67 | -7.1% |
| H3 echo | 1,245.67 | 1,560.67 | +25.3% |
| H3 1k-low | 8,369.67 | 8,554.00 | +2.2% |

`echo` 为 POST 1MiB，`1k-low` 为 H3 单连接单流。命令：`python3 temp/et-validation/benchmark.py`；逐样本吞吐、延迟和错误见 `temp/et-validation/matrix.json` 及对应 `.txt/.json`。H3 每线程一个 UDP endpoint，配置对应 16/1 个源端口；本轮未采集服务端各 worker 的实际流量分布，因此 H3 数字仅作功能与吞吐观察，不能把 reuseport 分布差异算作 ET 的确定收益。

这批短测表明小请求的收益与降低系统调用成本相符，但 H1/H2 大响应收益不一致，H2 1MiB 短测出现负向差异，需要结合下述较长复测判断。它不支持“全场景提升 15～20%”的结论。WSL 调度、短时采样与 H3 单样本均限制可外推性；尚未完成裸机长稳、CPU user/sys 每请求分解、空闲连接内存及混合负载尾延迟验收。

