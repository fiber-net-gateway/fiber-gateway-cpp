# TLS 自研实现 · 15 握手读路径：一个 wire 缓冲 + 已读空时跳过读

状态：已实现（2026-10-19）。范围：`TlsStreamFd` 握手阶段的读路径，以及两个阶段共用的"读空后跳过
wire 读"；`StreamFd`/`RWFd` 各加一个小接口。握手引擎、`TlsHandshakeContext`、`TlsConnection`、
写路径都不改。

## 1. 问题

用 LD_PRELOAD 记录 recv/sendmsg/epoll_ctl（宿主机没有 strace），对 `tls_probe` 和 openssl 两个方向
各跑一次握手，再做一次慢速分片实验，发现两个问题。

1. **每次握手读都会钉住 32 KiB**（内存放大，握手前、未认证的对端就能触发）。
   `read_handshake_chunk` 每次读都新分配一块 32 KiB 的 `IoBuf`，读到 N 字节后整块零拷贝挂进
   `TlsRecordReader::pending_`，直到整条 record 到齐才释放。一条 record 分 K 个 TCP 段到达，就同时
   钉住 K × 32 KiB。实测：声明长度 16384 的握手 record，1 字节一段发 4000 段，服务端 VmSize
   +128 MiB（正好 4000 × 32 KiB），RSS +16 MiB（每块碰一页）。一条 16 KiB 的 record 填满约
   512 MiB 虚拟 / 64 MiB RSS，10 s 的握手超时内很容易发完。已连接阶段的 `read_wire` 在
   feature/tls/12 已经用 tailroom 续读解决了同样的问题，握手阶段没有用上。
2. **每轮必然多一次返回 EAGAIN 的 recv**（性能）。短读之后 `StreamFd::finish_stream_read` 已经把
   读状态标成 Blocked，但 `RWFd::read` 按设计不会因为就绪状态跳过 I/O，而握手循环只要引擎没完成就一定
   再读一次——刚发完自己的 flight，对端至少还要一个 RTT，这次读只可能返回 EAGAIN，而且读之前还先分配
   了 32 KiB。已连接阶段的 `read_wire` 也是同样的模式（例如客户端收完 NST 之后）。

握手本身是正确的：引擎 `pump()` 会一直 step 到 NeedMore，flight 跨多个段时多次读也能正确拼接。

## 2. 定谳

1. **握手阶段复用 `inbound_`**。两个阶段共用同一个 inbound 缓冲；`read_wire` 拆成
   `read_inbound(capacity)`（只做 wire 读：不完整 record 能放下就在原缓冲 tailroom 续读，否则新分配
   `capacity` 并把 carry 拷到开头）和各阶段自己的后处理。握手阶段的容量是
   `kHandshakeReadCapacity = 32 KiB − 控制块`（分配正好落在 32 KiB 档，原来的 32 KiB 会落到 40 KiB 档），
   且 ≥ `kInboundMinCapacity`，保证 carry 的不完整 record 总能在下一块缓冲里补齐。
2. **只把完整 record 喂给引擎，一次一条**（`feed_handshake_records`）。用 `tls_frame_records` 切出
   完整 record，每条以 `retain_slice` 单独 feed，引擎完成（`done()`）就停：
   - 引擎 reader 每次都把这条 record 整个取走，两次 feed 之间 reader 永远是空的，所以 HandshakeDone 时
     `take_inbound_leftover()` 一定为空（改为断言）；最后一个 flight 之后的 record（piggyback 的应用
     数据）留在 `inbound_` 里，`install_connection` 直接 `process_inbound()`，不再需要 gather 拷贝，
     也不会再失败。
   - 切片的 view 止于 record 末尾，下一次读写入的 tailroom 在所有已交付切片之外；引擎原地解密只动
     切片 view 内的字节，这些字节在 `inbound_` 里已经 consume 掉了。
   - 拆分器报 fatal 时，把剩余字节原样交给引擎：`TlsRecordFramer` 和 `TlsRecordReader` 的拆分规则
     相同（见 TlsRecordFramer.h），引擎会以同一个 alert 失败，alert 及其顺序与改动前一致。
   - 缓冲完全消费后释放；引擎还在读的部分（如 ClientHello body）由它自己的切片钉住。
3. **读空后跳过读**（`StreamFd::read_would_block()`，见 §3）。握手阶段 `read_handshake_records`、
   已连接阶段 `read_wire` 在它为真时直接返回 WouldBlock，不分配也不发 syscall。
4. **握手开始时一次注册 IN + RDHUP**（`prepare_handshake` → `StreamFd::ensure_read_observation()`）。
   原来第一次等待时 ADD(IN)，现在 ADD 提前到握手开始、多带一个 RDHUP 位；之后的读等待已在关注集里，
   不需要 MOD。epoll_ctl 次数不变。

## 3. 为什么只看 Blocked 不够

短读只能证明接收队列里**没有数据**了，不能证明没有待读的 EOF：FIN 跟在数据后面一起到达时，recv 先
返回数据（短读），下一次 recv 才返回 0，而且不会再有新的边沿。普通的读订阅只注册 EPOLLIN（实测
`ev=0x80000019`），这时如果跳过读去等边沿，会一直等到调用方超时——已连接阶段对端不发 close_notify
直接关 TCP（HTTP/1 很常见）就会卡住。

所以 `read_would_block()` = 读状态 Blocked **且** RDHUP 在关注集里 **且** 没有记录任何流结束状态
（`peer_closed()`）：

- RDHUP 在关注集里时，FIN 要么已随某个已分发的边沿到达（`peer_hangup_` 已记下），要么之后到达、
  产生新的事件排在 epoll 就绪队列里，等待不会卡住。RDHUP 是后来 MOD 进去的也一样：`ep_modify` 会重新
  poll，条件满足就入队。
- 记录了流结束状态时，读等待的 gate 会立即放行；这时如果还跳过读，就会空转到超时，所以必须去读，
  由 syscall 报告 0 或错误。
- handover 后关注集清空、读状态回到 Unknown，`read_would_block()` 为假，按老路径读；H1 连接池的
  `ensure_state_observation()` 会把 RDHUP 加回来。

副作用：TLS 连接从握手起就收到 RDHUP，半关闭会被记为 `peer_hangup_`，`peer_closed()` 更早为真（H1
客户端连接池据此不复用，这是期望的行为）。H1 exchange 的 `set_terminal_callback` 本来就会注册 RDHUP，
所以 H1 服务端的语义没有变化。

## 4. 实测（WSL2，tls_probe ↔ openssl 3，握手 + 一次请求响应）

| 场景 | recv 前 → 后 | 其中 EAGAIN 前 → 后 | epoll_ctl 前 → 后 |
|---|---|---|---|
| server, TLS 1.3 | 6 → 4 | 3 → 1 | 2 → 2 |
| server, TLS 1.2 | 5 → 3 | 2 → 0 | 2 → 2 |
| client, TLS 1.3 | 8 → 4 | 4 → 1 | 2 → 2 |
| client, TLS 1.2 | 6 → 4 | 3 → 1 | 2 → 2 |

剩下的 EAGAIN 都是第一次读（读状态 Unknown 时的乐观读：服务端 accept 之后 ClientHello 可能已经到了）。

慢速分片（一条声明 16384 字节的 record，1 字节一段发 4000 段）：VmSize +128 MiB → 0，
RSS +16 MiB → +0.1 MiB；recv 8001 次 → 4000 次。

## 5. 测试

- `StreamFdTest.ReadWouldBlockOnlyAfterAShortReadWithNoStreamEnd`：Unknown、未观察 RDHUP、边沿之后、
  半关闭与数据同一边沿到达这几种情况下都为假，只有"短读 + 观察 RDHUP + 无流结束"为真。
- `TlsStreamFdTest.TrickledHandshakeRecordStaysInOneWireBuffer`：用 `FIONREAD` 同步，每字节对应一次
  wire 读，512 次读后 `mallinfo2` 统计的堆增长 < 1 MiB（旧实现约 16.6 MiB，测试失败）；分配器对
  `mallinfo2` 不可见（jemalloc 构建）时 skip。
- `TlsStreamFdTest.HalfCloseBehindAPartialRecordFailsTheHandshakeAtOnce`：阻塞 loop 线程，让 3 字节
  和 FIN 落在同一个边沿里，握手必须在 2 s 内以 ConnReset 失败。两个变体都会让它失败：不看 RDHUP
  的跳过（卡到 10 s 超时），以及不看 `peer_closed()` 的跳过（空转）。
- 全量 ctest 2750 个通过；`scripts/interop/openssl_matrix.sh` 100/100。

## 6. 后续候选

- **TCP 路径**：`TcpStream`/`TcpTransport` 的读同样可以按 `read_would_block()` 跳过，前提是在合适的
  地方注册 RDHUP；影响面更大，单独评估。
- **握手阶段保留空缓冲**：服务端读完 ClientHello 后缓冲本来就被 ClientHello body 钉住，下一个 flight
  可以续读在它的 tailroom 里，每次握手省一次 32 KiB 分配。没做，收益小。
- **引擎内部的小 record 分片**：大量极小的握手 record 会让 `append_fragment` 每片分配一次（按字节数
  算有放大，但以消息上限为界），与本改动无关。
