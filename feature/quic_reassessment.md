# `src/quic` 再评估：性能、内存、职责与状态建模

> 评估日期：2026-07-19  
> 代码基线：`quic-optimize`，`3558af8`（`quic recv optimize`）  
> 范围：`src/quic/` 49 个文件，约 17,817 行  
> 对照实现：仓库固定版本 Nginx 1.31.3

## 1. 目的与结论

本次评估聚焦以下方面：

- 热路径和系统调用开销；
- 连接、packet、stream 和 frame 的常驻及动态内存；
- class 职责和所有权边界；
- 变量定义、布尔状态组合和状态机不变量；
- 现有 `feature/quic_audit.md` 完成多轮修复后的剩余结构性问题。

当前 QUIC 实现的基础质量已经较好：固定容量 ACK range/path/CID、侵入式容器、pacing、延迟创建 TLS、packet-owned `IoBuf` 接收零拷贝等设计均值得保留。下一阶段的主要收益不在零散微优化，而在以下四个结构性方向：

1. 降低 `QuicConnection` 中 crypto state 的常驻体积；
2. 修复 packet slice 零拷贝带来的物理内存计账缺口；
3. 实现真正的 UDP 批量收发和 Linux UDP GSO；
4. 收紧 frame、key epoch、stream terminal state 和 endpoint lifecycle 的资源及状态边界。

## 2. 当前对象布局

以下数据由当前构建环境的 Clang 22、C++23 ABI 实测；它们用于比较当前相对占比，不应视为跨编译器 ABI 常量。

| 对象 | 大小 | 观察 |
|---|---:|---|
| `QuicConnection` | 16,864 B | 1024 个空连接约 16.47 MiB，不含 TLS/stream/frame/packet heap |
| `QuicCryptoState` | 10,840 B | 占 `QuicConnection` 64.3% |
| `QuicPacketProtectionKeys` | 984 B | 每份内嵌 AEAD 和 header-protection context |
| 3 个 `QuicPacketNumberSpace` | 2,952 B | 占 `QuicConnection` 17.5% |
| `QuicPathManager` | 800 B | 内嵌 3 个 `QuicPath` |
| `QuicPath` | 240 B | 固定容量、无动态分配 |
| `QuicStream` | 320 B | 当前体积可接受 |
| `QuicOutputFrame` | 168 B | 活动数量目前没有硬上限 |

默认 `max_connections=1024` 时，空连接本身约占 16.47 MiB，其中 crypto state 约占 10.59 MiB。这个数字尚未包含 `SSL`、stream table、收发缓冲、frame 和应用层对象，因此 crypto state 是最明确的常驻内存优化入口。

## 3. P0：优先处理

### 3.1 ✅ 已修复：重构 crypto/key epoch

#### 现状

`src/quic/QuicConnection.h:157-207` 中，`QuicCryptoState` 内嵌 11 份 `QuicPacketProtectionKeys`：

- Initial read/write；
- Early read/write；
- Handshake read/write；
- Application read/write；
- Next application read/write；
- Previous application read。

每份 keys 都直接内嵌：

- secret/key/IV/HP key material；
- `EVP_AEAD_CTX`；
- `AES_KEY`；
- 多个长度和 ready 标志。

大量 context 在连接生命周期的大部分阶段尚未初始化或已经不再使用，但仍永久占用连接对象空间。

Nginx 1.31.3 的 `ngx_quic_secret_t` 将 crypto context 保存为指针，并在密钥初始化时创建：

- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_protection.h:48-67`
- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_protection.c:392-430`

#### 建议

- 将 key material 与 AEAD/HP runtime context 分离；
- Initial、Handshake、Application/KeyUpdate 分阶段按需创建 context；
- 使用 event-loop pool 或一次性 staged crypto block，避免退化成大量独立小堆分配；
- Initial/Handshake keys 丢弃后立即释放相应 context；
- 增加对象大小回归测试，首阶段目标可设为 `sizeof(QuicConnection) < 10 KiB`。

不建议机械照搬 Nginx 的“每个 context 独立堆分配”。本项目更适合用固定分阶段 block 或 pool，在降低常驻体积的同时控制 allocation churn。

#### key-update 状态仍有正确性问题

当前代码已经增加 previous application read keys，但选择逻辑仍无法正确处理上一代重排序包：

1. `src/quic/QuicPacketProcessor.cpp:93-128` 在收到新 phase 后，将当前 read key 保存为 previous，提升 next key，并翻转 `key_phase_`；
2. 后续到达的上一代 packet，其 wire phase 与当前 phase 不同；
3. `src/quic/QuicPacketCodec.cpp:369-380` 会因此选择 `next_application_read`；
4. previous key 只在 `!key_update` 分支中尝试（`:386-397`），正常上一代 packet 无法进入该分支。

RFC 9001 §6.5 明确指出，previous 和 next 可能使用相同的 Key Phase 值，需要结合 packet number 判断：

- <https://www.rfc-editor.org/rfc/rfc9001.html#section-6.5>

建议引入明确的 `QuicKeyEpochState`，至少记录：

- generation/phase；
- 当前 receive epoch 的最低 packet number；
- 本 generation 已加密 packet 数；
- connection lifetime 内认证失败 packet 数；
- 当前 generation 首个发送 packet number；
- 是否已收到当前 generation packet 的 ACK；
- 是否允许下一次主动 key update；
- previous key discard deadline。

同时实现 RFC 9001 §6.6 的 cipher-dependent AEAD 使用计数、主动 key update 和 `AEAD_LIMIT_REACHED`：

- <https://www.rfc-editor.org/rfc/rfc9001.html#section-6.6>

这比继续增加 `key_phase_`、`*_ready` 等独立布尔值更容易建立和断言状态不变量。

### 3.2 ✅ 已修复 补齐零拷贝接收的物理内存计账

#### 现状

当前接收路径在 `src/quic/QuicPacketCodec.cpp:343-425` 中为每个成功进入解密流程的 packet 创建 owning `IoBuf`，随后 STREAM/CRYPTO frame 通过 `IoBuf::retain_slice()` 保存切片。

`retain_slice()` 共享原始 `ControlBlock`：

- `src/common/mem/IoBuf.cpp:216-229`

但 reassembler 只按切片逻辑字节数增加 `buffered_bytes_`：

- `src/quic/QuicDataReassembler.cpp:154-165`

因此一个很小的乱序 STREAM slice 可以持有完整 packet backing storage。极端情况下，每流允许 4096 个 active extents；若每个 extent 来自一个接近 64 KiB 的 packet，即使逻辑上只保留约 4 KiB，也可能钉住接近 256 MiB 的物理存储。

#### 建议

按以下顺序实施：

1. 为 packet storage 增加唯一 identity/owner；
2. 第一次进入 reassembly retention 时按 backing capacity 计费，同一 packet storage 只计一次；
3. 最后一份 queued slice 释放时归还物理预算；
4. 设置 connection 和 endpoint 两级 physical retained budget；
5. 对“小 slice / 大 backing”实施 copy-compaction；
6. 最后再引入 packet buffer size-class pool。

可考虑 1280、2K、4K、9K 等常用 size class，超大 UDP packet 使用 fallback allocation。copy-compaction 可按保留比例、backing size 或乱序存活时间触发。

只增加 buffer pool 不能解决活动内存放大；它只能减少 malloc/free churn，不能限制被 slice 持有的 active backing storage。

### 3.3 ✅ 已修复：实现 UDP batching 和 GSO

#### 现状

- `src/quic/QuicUdpEndpoint.cpp:587-607` 每次 `try_recv_packet` 只接收一个 datagram；
- `src/quic/QuicSendScheduler.cpp:143-207` 每构造一个 datagram 调用一次 `try_send_packet`；
- `src/net/UdpSocket.cpp:57-76` 的 `try_send_packets` 只是逐个调用 `try_send_packet`，并非 `sendmmsg`；
- endpoint 只有一份 `read_buffer_`、`send_plaintext_buffer_` 和 `send_buffer_`，无法同时保存一批待处理 datagram。

Nginx 1.31.3 在 validated application path 上使用 UDP GSO，并最多构造 64 个 segment：

- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_output.c:13-14`
- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_output.c:278-331`
- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_output.c:335-415`
- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_output.c:419-472`

#### 建议

1. 先在 `DatagramFd/UdpSocket` 增加固定数组版本的 `recvmmsg/sendmmsg`；
2. endpoint 使用 receive/send buffer ring；
3. `QuicDatagramBuilder` 一次构造多个 datagram transaction；
4. syscall 成功后只 commit 已发送前缀，未发送后缀逆序 rollback；
5. 保持现有 pacing、公平轮转、anti-amplification、ECN 和 PMTU probe 语义；
6. Linux 上增加 UDP GSO，并限制为：
   - validated path；
   - application/1-RTT；
   - 相同 path、local/peer address、ECN 和 segment size；
   - 无 Initial/Handshake 待发送；
   - 有足够数据覆盖多包收益。

建议先落地通用 `sendmmsg/recvmmsg`，再增加 GSO/GRO；这样 net 层其他 UDP 应用也可复用。

### 3.4 为活动 frame 设置硬预算

#### 现状

`src/quic/QuicFrame.cpp:122-148` 的 `QuicOutputFramePool`：

- free list 非空时复用；
- free list 为空时无条件 `new`；
- `kQuicOutputFramePoolMaxCached=1024` 只限制已释放缓存；
- 不限制 active/outstanding frame 数量。

生产连接还共享 endpoint 级 `output_frame_pool_`，因此一条连接可以消耗大量 frame，并影响同 endpoint 的其他连接。

Nginx 按连接维护 `nframes/max_frames`，超过上限即作为 flood 拒绝：

- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic_frames.c:196-237`
- `temp/nginx-1.31.3/src/event/quic/ngx_event_quic.c:322-324`

#### 建议

分别维护：

- pool cached count；
- endpoint allocated/outstanding count；
- per-connection outstanding frame budget；
- outstanding frame/data bytes high-water mark。

超限时应进入明确、可观测的 resource/flood 错误路径，而不是单纯返回 `NoMem`。cache 上限和 active 上限是两个不同概念，不应共用一个计数。

## 4. P1：高收益重构

### 4.1 将 STREAM ACK/loss 从 O(N) 扫描改为 O(1) ticket

`src/quic/QuicStreamSendQueue.cpp:338-423` 中：

- `mark_acked()` 每次从 `head_` 扫描整个 extent list；
- `mark_failed()` 同样从头扫描；
- ACK 和 loss 分别由 `QuicAckHandler.cpp:232-239`、`QuicLossRecovery.cpp:173-180` 按 frame 调用。

小 extent、多包在飞、乱序 ACK/loss 场景下，复杂度可放大为 O(frame × extent)。

建议：

- `EncodedFrameResult` 返回对应 `IoBufNode` 的稳定 ticket；
- ticket 存入 `QuicOutputStreamFrame`；
- ACK/loss 直接定位 extent；
- 如不能严格保证 node 生命周期，增加 generation；
- reset/stream detach 通过 terminal state 或 generation 使旧 ticket 失效；
- 必要时将 send extent 链表改为双向，以支持 O(1) unlink。

当前 encode 本来就直接操作准确的 `cur` extent，因此建立 ticket 不需要额外查找。

### 4.2 收紧 stream terminal state

#### send 侧

`src/quic/QuicStreamSendQueue.h:99-102` 使用四个布尔值表达终态：

- `fin_appended_`
- `fin_inflight_`
- `fin_acked_`
- `reset_sent_`

这些字段可以形成不合法组合，而且已经导致具体行为问题：

- `src/quic/QuicStreamSendQueue.cpp:426-440` 在任何 `fin_appended_` 状态下拒绝 reset；
- `src/quic/QuicStream.cpp:665-671` 在 detach 时忽略 reset 失败；
- 若应用仍持有 stream lease，未确认 send extents 可能继续保留到 stream 析构。

建议使用：

```cpp
enum class QuicSendTerminalState : std::uint8_t {
    Open,
    FinQueued,
    FinInFlight,
    FinAcked,
    ResetSent,
};
```

detach 增加明确的 `force_clear()`，与是否允许向 wire 发送 RESET_STREAM 分离。

另外，`QuicStream::write(IoBuf, fin=true)` 在 flow-control 短写时会返回成功的部分字节数，但 FIN 没有提交。应明确选择：

- write-all coroutine 语义，内部 consume 后继续等待；或
- 返回包含 `bytes_written` 和 `fin_committed` 的结果。

不要让调用方仅凭一个成功的 `size_t` 猜测 FIN 是否发送。

#### receive 侧

`src/quic/QuicStreamRecvQueue.h:93-96` 使用：

- `has_final_size_`
- `fin_received_`
- `reset_received_`
- `stop_sending_`

建议将 FIN/RESET 合并为 receive terminal enum，将 `STOP_SENDING`/本地停止读取保持为正交状态。`has_final_size` 应由 terminal state 派生，减少重复事实来源。

### 4.3 拆分构造配置、协商状态和活动路径

`QuicConnection::Options` 当前大小 416 B，并混合：

- 连接构造输入；
- endpoint 级共享策略；
- 本地 transport params；
- peer 协商后的限制；
- active path 地址和 remote CID；
- 应用回调和 owner；
- TLS context、loop、destroy callback。

同时它在连接生命周期中持续被修改：

- 构造阶段重写 transport/stream limit：`src/quic/QuicConnection.cpp:285-333`；
- peer transport params 应用后重写 max local streams 和 idle timeout：`:1804-1834`；
- path 切换时重写 address/CID：`src/quic/QuicPathManager.cpp:186-215`；
- `set_app_ops()` 修改 `owner/ops`：`src/quic/QuicConnection.cpp:1716-1722`。

建议拆为：

- `QuicConnectionInit`：只在构造时使用；
- `const QuicEndpointPolicy*`：endpoint 共享且只读；
- `LocalTransportParams`：实际对端可见、本地广告参数；
- `PeerTransportParams`：对端提供的参数；
- `NegotiatedTransportState`：effective idle timeout、local stream limit 等运行时结果；
- `QuicApplicationBinding`：owner 和 ops；
- active address/CID：直接读取 `QuicPathManager::active()`。

这既减少连接对象复制，也避免 `local_transport()` 返回已经被 peer 参数改写的“本地配置”。

### 4.4 收敛 class 职责

#### `QuicConnection`

当前规模：

- `QuicConnection.cpp`：2798 行；
- `QuicConnection.h`：776 行。

它同时负责：

- connection lifecycle/close/draining；
- stream table、隐式建流和 waiters；
- connection/stream flow control；
- packet number spaces、loss、RTT、congestion、PTO、pacing；
- TLS/crypto/key update；
- local/remote CID 池；
- path/migration 协作。

建议抽成无虚函数、值语义或窄引用依赖的子对象：

- `QuicCryptoEpochManager`
- `QuicRecoveryController`
- `QuicStreamManager`
- `QuicConnectionIdManager`
- 可选 `QuicCloseController`

`QuicConnection` 保留对外 façade、总生命周期和各子系统的协调。不建议通过虚接口、`std::function` 或大量独立 heap object 完成拆分。

#### `QuicUdpEndpoint`

当前规模：

- `QuicUdpEndpoint.cpp`：2137 行；
- `QuicUdpEndpoint.h`：289 行。

它同时负责：

- socket lifecycle/readiness pump；
- DCID registry；
- Initial admission 和 connection factory；
- Retry/VN/stateless reset/token close；
- rate limiting；
- receive demux；
- packet/datagram build、commit、rollback；
- send scratch buffer 和 scheduler 协作。

建议优先抽：

- `QuicInitialAcceptor`
- `QuicStatelessResponder`
- `QuicConnectionRegistry`
- `QuicDatagramBuilder`
- 可选 `QuicEndpointIo`

其中 `QuicDatagramBuilder` 是 UDP batching/GSO 的前置重构，优先级最高。

## 5. P2：顺手清理和小热点

### 5.1 删除无效或重复状态

- `QuicStream::recv_state_` 只由 `sync_recv_state_from_queue()` 写入，但 public `recv_state()` 直接从 `recv_queue_` 推导；删除字段、同步函数和调用点：`src/quic/QuicStream.h:103-114,175,189`。
- `QuicPath::used` 只在 `record_received()` 中置 `true`，全模块无读取；删除或赋予明确指标语义：`src/quic/QuicPath.h:64`、`src/quic/QuicPathManager.cpp:218-223`。
- `attached_to_endpoint_` 和 `detached_from_endpoint_` 用两个 bool 表达三态，存在非法组合；改为 `NeverAttached/Attached/Detached` enum。
- `QuicPath::tag` 与 `QuicPathManager::active_` 同时表达 active identity；应只保留一个事实来源，或在所有 transition 上断言一致。

### 5.2 重构 endpoint 布尔状态

`src/quic/QuicUdpEndpoint.h:274-284` 有 11 个相关 bool。它们并非全部属于同一状态机，但可以分组：

- `EndpointLifecycle { Empty, Initialized, Running, Closing }`
- I/O interest bitmask
- I/O readiness bitmask
- `PumpState { Idle, Running, RerunRequested }`
- `write_blocked`、`prefer_write` 保持正交

目标不是节省几个字节，而是禁止 `started && !initialized`、`closing && running` 等非法组合。

### 5.3 减少 STREAM 收包重复查表

`src/quic/QuicConnection.cpp:1494-1521` 对已存在 stream 会执行：

1. `find_stream()` 检查 gone stream；
2. 再次 `find_stream()` 检查 advertised limit；
3. `get_or_create_peer_stream()` 内第三次查表。

`QuicStreamTable` 已是低负载 open-addressing hash table，单次查找是预期 O(1)，但 STREAM frame 是热路径，三次相同 probe 没有必要。顶部查一次并复用结果即可。

### 5.4 合并 frame owned-data 分配

`src/quic/QuicFrame.cpp:161-237` 中：

- ACK/NewToken/Close 先分配 `QuicOutputFrameDataBlock`；
- 再单独分配 `uint8_t[]`；
- Crypto 分配一个 heap `IoBuf` handle，再由 `IoBuf` 分配 backing storage。

ACK frame 生成频繁，建议：

- block header 和 payload 使用单块 flexible allocation；
- 或使用 endpoint/frame-data size-class pool；
- ACK ranges 可考虑 packet-number-space-owned snapshot slots；
- 不建议为了少量 ACK ranges 把大数组直接内嵌到每个 168 B frame。

### 5.5 监控 reassembler 的 adversarial 扫描成本

`QuicDataReassembler::insert()` 先通过 `insert_cost()` 扫描，再进行实际插入扫描。`last_insert_` 对顺序和相近 offset 有帮助，但大量交错乱序 extent 仍可能造成较高 CPU 放大。

建议先增加：

- 每次 insert 的 scanned extent 数；
- active extent high-water mark；
- logical retained bytes；
- physical retained capacity；
- copy-compaction 次数和字节数。

只有 profiling 或 adversarial benchmark 表明确实成为热点后，再考虑 intrusive tree、skip index 或 extent 数超过阈值后的 hybrid index。小 extent 数下链表仍有更好的缓存局部性。

### 5.6 其他低成本清理

- await-ready 路径中的 `std::chrono::steady_clock::now()` 与 event-loop cached time 不一致；确认 loop affinity 后统一使用 `EventLoop::current().now()`。
- `hkdf_expand_label()` 的 stack buffer 只给 label/context 各预留 32 字节，但 validation 允许更长输入；当前调用均为短常量和空 context，仍应让边界检查直接由 buffer capacity 推导。
- `mark_closed()` 当前 public，但正确生产调用依赖 endpoint 随后取消 timer 和 detach；可改为 private endpoint primitive，避免未来脱离完整关闭事务单独使用。
- `QuicOutputFrame` 将 packet metadata 重复存到每个 frame，并通过首 frame 的 `packet_len!=0` 隐式承担 packet bytes-in-flight；若后续 frame 数量和内存成为热点，可评估 pooled `SentPacket` header + frame list，但优先级低于 frame budget。

## 6. 建议保留的设计

以下设计当前是合理的，不建议为了统一风格而重写：

- ACK range 固定容量，避免网络输入驱动动态扩容；
- path/CID 固定小数组，提供清晰的内存上界；
- intrusive queue/tree 和自定义 pool；
- 无 C++ exception 的错误传播；
- pacing 与 scheduler 结合；
- TLS `SSL_new` 延迟到首个 Initial AEAD 成功之后；
- packet-owned `IoBuf` + reassembler owning slice 的零拷贝方向；
- connection/path/stream 的 event-loop affinity。

其中固定 3 个 path 使 `QuicPathManager` 占 800 B，但 migration 是低频路径，当前固定容量换取无分配和强上界是合理取舍。应先完成 crypto state 优化，再根据高连接数场景的实际 profile 决定是否改为“1 个 inline active path + pooled extra paths”。

## 7. 实施顺序

建议按以下阶段实施，每个阶段可独立验证和回滚：

### 阶段 A：正确性和资源边界

1. `QuicKeyEpochState`、previous/next PN 判定；
2. AEAD sent/failed-auth counters 和主动 key update；
3. physical retained capacity accounting；
4. per-connection/endpoint frame budget；
5. stream detach `force_clear()` 和 terminal enum。

### 阶段 B：常驻及动态内存

1. crypto context 延迟分配/分阶段释放；
2. packet buffer size-class pool；
3. small-slice compaction；
4. frame owned-data 单块分配；
5. 对象大小和 allocation-count regression test。

### 阶段 C：吞吐

1. `recvmmsg/sendmmsg` net API；
2. endpoint receive/send ring；
3. batch transaction commit/rollback；
4. validated 1-RTT UDP GSO；
5. send extent ticket O(1) ACK/loss；
6. STREAM lookup 和其他热路径小优化。

### 阶段 D：职责收敛

1. 抽 `QuicDatagramBuilder`；
2. 抽 stateless responder/initial acceptor；
3. 抽 stream/CID/crypto/recovery 子对象；
4. 收紧 friend 和 private API；
5. 删除重复、无效状态字段。

## 8. 建议的观测指标和验收条件

### 内存

- `sizeof(QuicConnection)` 和各主要子对象；
- 空连接/handshaking/established 三阶段常驻内存；
- logical buffered bytes 与 physical retained capacity；
- packet buffer pool active/cached/high-water；
- per-connection 和 endpoint outstanding frame 数；
- 每成功解密 packet 的 heap allocation 数。

### 性能

- recv/send datagrams per syscall；
- sendmmsg batch size、GSO segment count 和命中率；
- packets/s、Gbps、单核 CPU；
- STREAM ACK/loss 平均及最大 extent scan steps；
- reassembler insert scan steps；
- packet build rollback 次数；
- pacing timer 唤醒和 scheduler rotation 次数。

### 回归测试

- previous/current/next key 的 packet-number 边界和重排序；
- 主动 key update 前后的 ACK 确认约束；
- AEAD confidentiality/integrity limit；
- tiny slice pinning jumbo backing 的物理预算；
- frame flood 不影响其他连接；
- partial `sendmmsg` commit/rollback；
- GSO 路径的 pacing、ECN、PMTU、anti-amplification；
- FIN-inflight stream detach 和缓冲释放；
- extent ticket 在 reset、loss、ACK 和 node reuse 下的有效性。

## 9. 本次验证

执行：

```bash
cmake --build build -j2
ctest --test-dir build -R 'Quic|Http3' --output-on-failure -j2
```

结果：

- 当前分支构建成功；
- 379/379 个 QUIC/HTTP3 相关测试通过；
- Nginx 1.31.3 通过 `scripts/build_nginx.sh` 构建；
- 固定版本 Nginx 配置检查通过；
- 本次评估未修改 `src/quic/` 生产代码。
