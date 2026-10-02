# TLS 自研实现 · 14 写路径：封装即接受，TlsStreamFd 自己写完

状态：已实现（2026-10-01），bench 见 §7。范围：`TlsStreamFd` 已连接阶段的写侧契约，以及受它影响的
`TlsTcpStream`、`HttpTransport`/`TlsTransport`、H2 出站和 H1 客户端的取消路径。握手、读路径、
`TlsConnection` 都不改。

## 1. 动机

feature/tls/13 之后，`try_write` 的契约沿用 BoringSSL `SSL_write` 在 WANT_WRITE 后"同一缓冲区重试"
的约定：

- 封装好的一批 record 一次没写完时，返回 WouldBlock；明文不从 chain 消费，剩余密文留在 `out_pending_`；
- `TlsStreamFd` 记下这条 chain 的地址（`pending_write_chain_`）和批长度，调用方必须拿**同一条 chain**
  重试，换别的 chain（包括空 chain）一律返回 Busy；
- 整批写完才消费 chain、返回整批长度；
- 调用方放弃时要调 `abandon_pending_write()`，之后残留的密文一直留到 close，任何写都返回 Busy。

这套约定带来的问题：

1. **TLS 层不会自己写剩下的数据**：没有内部订阅，`out_pending_` 只能靠调用方重试才能写出去。
2. **契约渗透到所有调用方**：`HttpTransport` 要提供 `abandon_pending_io`，接口注释要讲"TLS 会保留
   chain 指针"；H2 的 `abort_outbound` 要先关 transport 再释放 chain；H1 客户端失败路径要调 abandon。
3. **部分写出不算进度**：一批分多次写出时，中间每次都返回 WouldBlock；外部回调在每次部分写出时都被
   唤醒，H2 每次都要跑一整轮 `drive_io`。

## 2. 定谳

1. **封装即接受**。`try_write` 封装一批（约 64 KiB 明文）后立即 flush；不管 socket 收下多少，都从 chain
   消费这一批、返回它的长度。剩余密文留在 `out_pending_`，由 `TlsStreamFd` 自己写完。语义和内核 socket
   缓冲区一致：`write()` 返回 n 只表示数据被接受，不表示已到对端。
2. **`out_pending_` 不为空时写方向不可写**：
   - `try_write` 直接返回 WouldBlock（任何 chain 都一样，不再有 Busy）；
   - `write_ready() = write_error_ != None || (out_pending_.empty() && stream_fd_.write_ready())`；
   - 新增 `has_pending_write() = !out_pending_.empty()`。
3. **`TlsStreamFd` 独占 StreamFd 的写订阅**。flush 返回 WouldBlock 时（此时 RWFd 写状态一定是 Blocked，
   满足"Ready 时不许订阅"的断言），把自己的 `on_stream_writable` 挂到 StreamFd 上，置 `draining_`。
   不变式：连接阶段 `out_pending_` 不为空 ⇔ `draining_`。
4. **TLS 层的写订阅位**。外部的 `set_write_callback` 不再直接挂到 StreamFd，而是存进 `TlsStreamFd`
   （`write_callback_`），由 `on_stream_writable` 转发：
   - 有 drain 在进行：继续 flush；仍然 WouldBlock 就保持订阅、不通知外部；
   - 写完（或失败）后：有外部订阅者就通知它（`None`），没有就取消 StreamFd 订阅；
   - 订阅规则沿用 RWFd：`write_ready()` 为真时不许订阅（断言），setter 从不内联调用回调，
     订阅是持久的、只在"不可写→可写"时通知一次；
   - 外部回调放在最后调用：回调里可以写、重新订阅或取消订阅；`HttpTransport` 的约定保证回调期间
     transport 不会被销毁。
5. **TLS 层自己的 `WaitWritableAwaiter`**。StreamFd 的写订阅被 `TlsStreamFd` 占着，`writev`、shutdown
   用的 `wait_tls_event`、`TlsTcpStream` 的用户都不能再直接等 StreamFd。新 awaiter 装在第 4 点的订阅位上，
   语义对齐 RWFd 的 `WaitAwaiter`：超时、零超时返回 TimedOut、终止的 fd 直接返回其错误、析构时取消
   订阅（`timeout_for` 靠析构放弃它）。
6. **`writev` 写完才返回**。协程版每次封装一批，然后等 `out_pending_` 清空才返回这一批的长度；空 chain
   相当于 flush。H1（客户端和服务端）只用 `writev`，所以"写完就是数据已交给内核"的语义不变。
7. **失败**：flush 的硬错误（EPIPE、ECONNRESET 等）和封装时的 NoMem 一样锁存进 `write_error_`，同时
   丢弃 `out_pending_`、结束 drain。之后的 `try_write`、`poll_shutdown` 返回该错误，`close()` 什么都不发。
   drain 中途失败时，外部订阅者照常收到通知（`None`），下一次写拿到错误。`try_write` 自己 flush 失败
   时不消费 chain，直接返回错误。
8. **`poll_shutdown`**：close_notify 追加在仍在 drain 的密文后面，走同一套 flush；WouldBlock 时报
   `event = Write`，写就绪表示 drain（含 close_notify）已完成。
9. **`close()`**：先尽力 flush 一次（行为不变），然后取消 StreamFd 订阅、取出外部订阅者、复位全部成员，
   再关 fd，最后用局部变量以 Canceled 完成外部订阅者。StreamFd 关闭时的完成回调可能销毁本对象，
   所以 fd 关闭之后不再访问任何成员（原来的实现在 fd 关闭后还要复位成员）。
10. **线程转换**：`detach_for_handover` 断言 `out_pending_` 为空、没有任何写订阅。只有 H1 连接池做
    线程转换，而 H1 只用 `writev`，它返回时 drain 一定已完成。

## 3. 调用方

- **H2**（唯一的 `try_writev` 用户）：
  - 发送完成回调在封装时就触发，不再等数据交给内核；
  - `outbound_idle()` 加上 `!transport_->has_pending_write()`；出站 chain 为空但 transport 还有未写完
    的数据时，`pump_outbound` 返回 `wait_event = Write`。这样优雅关闭会等 drain 完成，不会被 `close()`
    截断（`finish_connection` 在 `outbound_stopped_` 后就调 `transport_->close()`）；等待期间写超时照常计时；
  - 预算恰好在 chain 清空时用完的情况也要等 Write，否则没有人唤醒 H2 去完成关闭；
  - `abort_outbound` 仍然先关 transport：TCP 短写或 TLS 批次可能在帧中间截断，不能把半帧留给对端。
    注释改成这个理由。
- **H1 客户端**：失败路径不再调 `abandon_pending_io()`。被取消的 `writev` 析构 awaiter，awaiter 取消
  TLS 层订阅；剩下的密文由 drain 继续写或在 close 时丢弃，连接已标记为不可用，不会再入池或做线程转换。
- **接口**：删除 `HttpTransport::abandon_pending_io`、`TlsTcpStream::abandon_pending_write`、
  `TlsStreamFd::abandon_pending_write`；新增 `HttpTransport::has_pending_write()`（默认 false）；
  `TlsTcpStream::wait_writable` 返回 `TlsStreamFd::WaitWritableAwaiter`。`try_writev` 保留：H2 完全靠
  回调驱动，需要不挂起的写。

## 4. 内存与时延

- `out_pending_` 最多一批（64 KiB 加一个 record）密文，和原来一样；明文在封装后立即释放，原来要等写完。
- H2 在发送缓冲区满时，部分写出不再唤醒 `drive_io`，只有 drain 完成才唤醒一次。

## 5. 测试

- `TlsStreamFdTest`：
  - 删除 `TlsTransportAbandonPendingWriteDropsChainReference`（契约已不存在）；
  - `TlsTransportPollWritevRetainsCoalescedGroupAcrossWouldBlock` 改名为
    `TlsTransportTryWritevBacksOffWhileTheSealedBatchDrains`：轮询写的客户端不订阅，
    靠 sleep 重试，验证 drain 自己完成、数据完整；
  - 新增 `TlsStreamFdDrainTest`（单 loop，客户端 SO_SNDBUF 4096）：
    - `TryWriteAcceptsTheSealedBatchAndDrainsItAlone`：首批返回 64 KiB 并消费 chain，同/异 chain
      都 WouldBlock，`write_ready()` 为假，之后不再调用也能写完；
    - `WriteCallbackFiresOnlyOnceTheDrainIsDone`：setter 不内联通知，回调只触发一次，触发时已写完、可写；
    - `CloseDuringTheDrainCancelsTheWriteSubscriber`：drain 中 close，订阅者收到一次 Canceled；
    - `WritevReturnsOnlyOnceItsBatchLeft`：每次 `writev` 返回时 `has_pending_write()` 都为假；
    - `WaitWritableWaitsForTheDrainAndUnsubscribesWhenAbandoned`：drain 中等待会超时，`timeout_for`
      放弃后订阅位已释放，drain 完成后等待成功；
    - `PollShutdownSendsCloseNotifyBehindTheDrain`：对端先读到整批数据，再读到 EOF；
    - `TlsStreamFdDrainDeathTest.HandoverWithUndrainedOutputAsserts`。
- `Http2ServerConnectionTest`：`WireTransport` 桩加上"已接受但未写出"的模式：
  - `GracefulCloseWaitsForOutputTheTransportHolds`：GOAWAY 被 transport 持有时连接不关闭，释放后正常结束；
  - `HeldTransportOutputIsBoundedByWriteTimeout`：一直不释放时由写超时结束。
  - 去掉 H2 的改动后，这两个用例都失败：第一个在 GOAWAY 还被持有时就关闭了 transport。

## 6. 提交

`refactor(net)!: let TlsStreamFd drain sealed output on its own`，带 `BREAKING CHANGE:` 说明删除的接口。

## 7. Bench（2026-10-01，WSL2 i7-13700H）

方法与 feature/tls/13 §8 相同：temp/bench 的 lite_nginx 代理场景，h2 = h2load `-t6 -c32 -m16`，
h1s = wrk `-t6 -c256` 走 TLS，每轮 20 s，before/after 交替 8 个样本，CPU/req = 代理进程
utime+stime ÷ 成功请求数。before = HEAD（b34e2b0d），after = 本实现，两者在 `temp/ab` 里以相同配置构建。
共 128 轮，0 错误，没有 WSL2 时钟回跳需要修正。

| 协议 | 场景 | RPS before | RPS after | Δ RPS | CPU µs/req before | after | Δ CPU/req |
|---|---|---:|---:|---:|---:|---:|---:|
| h2 | get1k | 171,857 | 172,871 | +0.6% | 23.2 | 23.0 | −1.1% |
| h2 | get64k | 37,741 | 37,235 | −1.3% | 106.3 | 107.8 | +1.4% |
| h2 | get1m | 3,687 | 3,656 | −0.8% | 1,085.1 | 1,094.8 | +0.9% |
| h2 | post1m | 1,691 | 1,692 | +0.1% | 2,372.4 | 2,369.8 | −0.1% |
| h1s | get1k | 117,857 | 119,908 | +1.7% | 33.8 | 33.4 | −1.1% |
| h1s | get64k | 52,799 | 51,427 | −2.6% | 75.6 | 77.0 | +1.8% |
| h1s | get1m | 5,122 | 5,157 | +0.7% | 783.5 | 775.9 | −1.0% |
| h1s | post1m | 2,544 | 2,562 | +0.7% | 1,576.0 | 1,571.1 | −0.3% |

**解读**
- 所有场景的 RPS 和 CPU/req 样本范围都重叠，变化都在噪声内，结论是持平。这次改动的收益是结构上的
  （见 §1），本来就不期望有性能变化。
- h1s get64k 的 −2.6% / +1.8% 是变化最大的一格，但 8 对样本里 after 的 CPU 有 2 对更低、6 对更高，
  样本离散度（71.8–82.1 µs）比差值大得多。H1 只走 `writev`，和原来一样每批写完才返回，syscall 序列
  不变。
- h2 get64k 和 get1m 的 CPU 有约 +1% 的倾向（成对比较 6/8 更高），低于 8 个样本能分辨的 ±3%，
  没有确认。可能的来源：出站 chain 清空而 transport 还有未写完的数据时，H2 现在要等一次写就绪，
  drain 完成后多一轮 `drive_io`。如果以后需要压这 1%，可以只在关闭阶段才等 drain。

## 8. 后续：只用 write_ready() 对外（2026-10-02）

`write_ready() = write_error_ || (out_pending_.empty() && fd Ready)`，锁存错误时 `fail_write` 已清空
`out_pending_`，所以 Ready 必然意味着已接受的字节都上了线。`!write_ready()` 是原 `has_pending_write()`
的保守超集，多出来的只有"fd 还没报告过可写（Unknown）且没有积压"这一种情况：订阅后 ET 会立即给一个
边沿，多一次唤醒，不会卡住；H2 连接一开始就写 preface/SETTINGS，也不做 handover，实际碰不到。
（Blocked 只来自 WouldBlock，而 WouldBlock 时 TLS 的 `out_pending_`、TCP 上 H2 的 inflight chain
都一定非空。）

因此：
- 删除 `HttpTransport`/`TlsTransport`/`TlsTcpStream`/`TlsStreamFd` 的 `has_pending_write()`；
  `HttpTransport::write_ready()` 改为纯虚——H2 的空闲判断依赖它，不能默认 false。
- H2 的三处改用 `transport_draining() = transport_ && transport_->valid() && !write_ready()`；
  `valid()` 排除已关闭的 transport（close 已丢弃积压，不能再去订阅）。
- `shutdown_once` 断言没有 TLS 层写订阅者：它自己的 flush 可能就地写完 drain，socket 停在 Ready，
  不会再有边沿，常驻订阅者永远收不到通知。目前只有 H1 调 transport 的 shutdown，H1 不挂常驻回调；
  `TlsTransport::shutdown` 两次 `poll_shutdown` 之间用的 `WaitWritableAwaiter` 在恢复前已注销。
- `handle_stream_writable` 通知订阅者前断言 `write_ready()`（drain 写完时 fd 必为 Ready，失败时错误已锁存）。
