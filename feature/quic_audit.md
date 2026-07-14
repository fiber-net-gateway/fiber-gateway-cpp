# `src/quic/` 模块审计

> 审计范围：`src/quic/` 全部 46 个文件（约 17,200 行），按连接核心（`QuicConnection`/`QuicPacketProcessor`/`QuicPathManager`）、packet/frame codec（`QuicPacketCodec`/`QuicFrame`/`QuicTransportCodec`/`QuicTransportParamsCodec`）、stream 层（`QuicStream`/`QuicStreamTable`/`QuicStreamSendQueue`/`QuicStreamRecvQueue`/`QuicDataReassembler`）、可靠性与拥塞（`QuicAckHandler`/`QuicCongestion`/`QuicLossRecovery`/`QuicPacketNumberSpace`/`QuicSendScheduler`）、crypto/token（`QuicCrypto`/`QuicTlsSession`/`QuicToken`/`QuicConnectionId`）、UDP endpoint（`QuicUdpEndpoint`）六个域并行评审后整合。所有结论均经直接读码 + 交叉验证。
>
> 核验事实：Initial key 派生（salt `0x38762cf7…` + DCID）、AEAD nonce（IV XOR PN）、header protection（AES-ECB/ChaCha20 双路径）、Retry integrity tag、stateless reset（HMAC-SHA256 + 常时比较）、CID 用 `RAND_bytes` CSPRNG + 重试去重均符合 RFC 9001；anti-amplification（3×）按 path `received*3` 正确封顶（`QuicPathManager.cpp:244-260`）；ECN `IP_RECVTOS`/`IP_TOS`/`IPV6_TCLASS` 设置与解析齐全；稳态 recv 零堆分配（`read_buffer_`/`plaintext_buffer_` 复用）；endpoint 单线程 per-socket，`dcid_tree_` 无锁；全 `src/quic/` `std::string/vector/function/shared_ptr` 仅 4 处（全在 `QuicUdpEndpoint.cpp`），性能纪律真执行。
>
> 测试覆盖：约 9,700 行测试，但可靠性核心（`QuicAckHandler`/`QuicLossRecovery`/`QuicSendScheduler`/`QuicPathManager`）缺专门单测；全 `src/quic/` 0 个 `TODO/FIXME` 标记。
>
> 状态：部分动工。#2 PTO 忙循环已修复（2026-07-14，见下）。待修 P0：#4/#5 DoS 向量、#1 key-update、#3 re-entrancy、#7 UAF、#9 GSO、#8 接收零拷贝，其余排期。

## 总体评价

codec 边界检查严密（`QuicCursor` 每读必 `remaining()`）、零拷贝视图（`QuicSlice`）、`write_or_count_*` 模式、crypto 核心、anti-amplification、ECN、稳态零分配均正确。存在 **9 个高危**（含 key 用量越界、PTO 忙循环、re-entrancy、DoS 向量、UAF、缺 GSO/接收零拷贝）、若干中危的可靠性与性能问题。可靠性与拥塞控制域问题最集中且无单测，风险最高。按严重度排列如下，均给出 `file:line` 与触发场景。

---

## 🔴 HIGH

### 1. 无 key-update 主动发起 + 无 AEAD 用量计数
`QuicPacketProcessor.cpp:75,519-524`（`flip_key_phase` 仅在 `apply_key_update` 内，只由对端发起触发）

`flip_key_phase()` 只在收到对端 key update（`decoded.key_update`）时经 `apply_key_update` 触发，没有任何 per-generation 包计数。RFC 9001 §6.6 的机密性上限 2²³≈840 万包在持续 H3 连接下几分钟可达；服务端从不主动 update，若客户端也不 update，长连接会越过 MUST-NOT-exceed 上限。

**修复**：按 key generation 计收发/收包数，在接近机密性上限时主动 `apply_key_update`。

### 2. PTO 定时器/probe 判定不一致 -> 忙循环 + 漏探针
`QuicLossRecovery.cpp:237-238`（timer arm）vs `:270-273`（probe skip）

`quic_loss_detection_timer` 对任何非空 `sent_frames` 都 arm PTO，但 `quic_queue_pto_probe_frames` 跳过 `back->packet_number <= largest_acked_packet_number` 的 level。当 `largest_acked` 之下还有未确认包（带 gap 的部分 ACK 常见）时：PTO 以 delay 0 触发 -> 不排探针 -> `pto_count` 不增 -> 重新 arm 同样 delay -> **忙循环**。且跳过条件本身逻辑反了：`sent_frames` 里的包定义上就是未确认的（已确认的会被移除），`back->PN <= largest_acked` 说明**确有**未确认包需要探针，而非反之。可致虚假忙循环与丢包后无探针的死锁。

**修复**：去掉 `quic_queue_pto_probe_frames` 的 skip 条件（或换 `in_flight==0` 守卫），并/或在 `quic_loss_detection_timer` 加同样守卫，使二者一致。

> ✅ **已修复 2026-07-14**。复核结论：上述"忙循环"分析对触发场景的判定有误--`back->PN <= largest_acked` 意味着 `front->PN <= largest_acked`，该 level 命中 **Lost 模式**而非 Pto 模式（`quic_loss_detection_timer` 中 Lost 优先且 `front->PN <= largest_acked` 才算 has_lost），故 skip 在保序不变量下是死代码，正常路径不发病。与 nginx `ngx_event_quic_ack.c` 逐行对照，skip 条件两端**逐字节相同**（timer arm 用 `back` 无守卫、probe skip 用 `back->PN <= largest_ack`），nginx 同样如此。真正的本质差异与隐患在于：**nginx 的 `pto_count++` 在 PTO handler 末尾无条件执行（`:1164`，for 循环之后），本项目原为条件执行**（仅 `*queued` 为真时）。一旦保序不变量被破坏（失序/漏 re-arm 等），条件版会进入 `delay 0 -> 全 skip -> pto_count 不增 -> re-arm delay 0` 的忙循环，而无条件版靠 backoff 翻倍使 delay 转正、自终止。修复方式：`QuicConnection::on_loss_detection_timer` 中将 `++pto_count_` 提为无条件（与 `schedule_send` 解耦），对齐 RFC 9002 Appendix A.7 与 nginx。skip 条件本身未动（保序下为死代码，留作无害；若后续想彻底对齐 RFC "ack-eliciting in flight 才 arm PTO" 可另案处理，见 #10）。

### 3. `enter_closing` 在帧循环中途触发 -> re-entrancy
`QuicConnection.cpp:1404`（`maybe_finish_graceful_close`）/ `:2252`；`QuicPacketProcessor.cpp:289,301`（守卫仅包入口）

`recv_stream_frame -> try_release_stream -> retire_stream -> maybe_finish_graceful_close -> enter_closing` 在 `process_decoded_packet` 的帧循环内触发。`enter_closing` 调 `close_all_streams()` + `clear_pending_frames_all_levels()`，改动全局连接状态，而同包内后续帧仍在迭代。Closing 状态守卫只在包入口（`QuicPacketProcessor.cpp:289`）查，帧间不查。后果：同包内先前入队的帧（ACK/流控）被静默丢弃，后续帧在 Closing 连接上处理（违反 RFC：Closing 只应发 CC 帧）。

**修复**：`try_release_stream` 后检查 `closing()` 跳出帧循环；或把 `enter_closing` 过渡用 `loop_->post()` 延迟到包处理结束后。

### 4. 连接对象在 Initial 认证前分配 -> DoS 向量
`QuicUdpEndpoint.cpp:1168`（`create_connection`）/ `:1177`（`quic_process_datagram` 内 AEAD 认证）

`create_connection`（含 `tls().init_server(*tls_context,…)` + CID 注册）发生在 `quic_process_datagram`（AEAD 认证）**之前**。`retry=false`（默认）下，伪造源地址 + 语法合法 header + 垃圾 payload 的 Initial 每包都触发完整连接分配 + TLS ctx 初始化 + CID 注册，无一校验。这是经典 QUIC DoS 向量。`retry=true` 缓解，但 create-before-auth 顺序独立于 Retry。

**修复**：先解 header protection + AEAD 认证再分配；或默认 `retry=true`；或加 per-source 建连速率限制。

### 5. Retry / VN / invalid-token-close 无速率限制
`QuicUdpEndpoint.cpp:1087`（VN）/ `:1143`（Retry）/ `:1160`（invalid-token close）/ `:715`（仅 stateless_reset 有 token bucket）

只有 `send_stateless_reset` 受 `allow_stateless_reset` token bucket（8/s）约束。其余三个 stateless 路径 1:1 无上限放大。`encode_invalid_token_close_packet`（`:67-97`）每包还新建 `QuicConnection temp` + `init_initial_crypto` HKDF——每伪造包一份 crypto 工作。伪造源洪泛可驱动等量无限制出口流量。

**修复**：token bucket 覆盖全部 stateless 响应（最好 per-peer，nginx 风格）；预派生/缓存 Initial keys。

### 6. 计时器操作系统性用 `EventLoop::current_or_null()` 而非 `loop_`
`QuicConnection.cpp` 约 20 处（356,460,615,632,639,652,667,679,689,721,958,1009,1032…）

每个计时器 arm/cancel 与状态过渡路径用 `event::EventLoop::current_or_null()` 而非成员 `loop_`。单 loop 不变量只在 `LocalStreamAttachAwaiter`/`attach_local_stream`（`:156,1114,1167`）assert，核心方法（`enter_closing`/`arm_idle_timer`/`on_idle_timer`）不 assert。若 `Lease` 释放或 `close()` 从别的 loop 调用（如 H3 在另一 worker），计时器被 arm/cancel 到错误的堆，`~QuicConnection`（`:356`）经 `current_or_null()` 取到的可能是错的 loop，留下 armed timer 在已释放内存上触发。

**修复**：连接方法内一律用 `loop_`；公开方法入口 assert `EventLoop::current()==loop_`。

### 7. `~QuicConnection` 不通知 peer-data waiter -> 潜在 UAF
`QuicConnection.cpp:354-363`（析构）/ `:2147`（`detach_from_endpoint` 两边都通知）

析构只 `notify_all_local_stream_attach_waiters(Canceled)`，**未** `notify_peer_data_waiters(Canceled)`。`detach_from_endpoint`（`:2147`）两边都通知，正常路径安全；但任何不经 `detach_from_endpoint` 就到析构的生命周期 bug，会留下挂起的 `WriteAwaiter` 持有指向已释放连接的悬空 `peer_data_wait_link_`。

**修复**：析构补 `notify_peer_data_waiters(common::IoErr::Canceled);`。

### 8. 接收路径无零拷贝：STREAM 数据 memcpy 进 16KB 堆块
`QuicStreamRecvQueue.cpp:648-661`（`create_extent`）/ `QuicStream.cpp:519-522`（`on_stream_data_recv`）/ `QuicFrame.h:65-70`（`QuicSlice`）

`QuicSlice` 是裸 `{const uint8_t*, size_t}` 指向包 payload（`QuicTransportCodec.cpp:725-731` 经 `payload.read_slice` 设置），没 retain refcounted IoBuf。`create_extent` 只能 `IoBuf::allocate(kRecvBlockSize)`（16KB）+ `std::memcpy`。发送侧是零拷贝（`QuicStreamSendQueue.cpp:55,270` 用 `retain_slice`），接收侧不是——明显不对称。

**修复**：frame 层穿透 `IoBuf` view 而非裸指针，in-order 数据 `retain_slice()` 跳过拷贝。

### 9. 无 GSO / sendmmsg / recvmmsg / GRO
`src/net/detail/DatagramFd.cpp:440,499`；`QuicSendScheduler.cpp:248`（`flush_connection` 逐包 `try_send_packet`）

每数据报一次 `recvmsg`/`sendmsg` 系统调用。nginx/quiche/cloudflare 都用 `sendmmsg`+GSO（`UDP_SEGMENT`）批量数十到数百 coalesced 数据报/系统调用，recv 用 `recvmmsg`/GRO。这是 QUIC 服务端最大的性能缺口。

**修复**：加 GSO 发送路径批量多连接数据报到一次 `sendmmsg`/GSO；recv 用 `recvmmsg`/GRO。

---

## 🟠 MEDIUM — 可靠性与拥塞控制（无单测，风险最高）

### 10. PTO 基于 non-ack-eliciting 包，违反 RFC 9002
`QuicLossRecovery.cpp:213-238`

`quic_loss_detection_timer` 只查 `sent_frames.empty()`，不查帧是否 ack-eliciting（`packet_len!=0`）。ACK-only 包（non-ack-eliciting，`packet_len=0`）也记入 `sent_frames`，PTO 会基于其 `send_time` arm。RFC 9002 §6.2.1 要求仅当有 ack-eliciting 包在飞时 arm PTO，且基于最近 ack-eliciting 包。后果：仅发了 ACK 也发多余 PING 探针。

**修复**：arm 前查 `in_flight>0`（或反向扫到首个 `packet_len!=0` 的帧）。

### 11. loss 减窗基准用 `in_flight`（减后）而非 `window`
`QuicCongestion.cpp:166-172`（`quic_congestion_on_loss` 先 `subtract_in_flight` 再传 `cg.in_flight`）/ 对比 `:184`（ECN CE 路径正确用 `cg.window`）

`quic_congestion_on_loss` 先 `subtract_in_flight(cg, sample.packet_len)`，再把已减小的 `cg.in_flight` 作 `reduction_basis` 传 `enter_recovery`。app-limited（`in_flight<window`）时 `ssthresh=(in_flight-pkt_len)*0.7` 而非 `window*0.7`，过度减窗，拖慢恢复。ECN CE 路径正确。RFC 9438 §4.1 与 Linux `tcp_cubic.c` 均按 `cwnd` 减。**一行修**。

**修复**：`quic_congestion_on_loss` 传 `cg.window` 作 `reduction_basis`。

### 12. 无 pacing
`QuicSendScheduler.cpp:226-286`（`flush_connection` 紧 for 循环）

`flush_connection` 紧 for 循环到 `max_packets_per_connection`（默认 64）包或 socket 阻塞。无 inter-packet delay、无 token bucket、无 pacing rate 计算（全 `src/quic/` 搜 `pacing/pacer/PacingRate` 无结果）。单连接一次唤醒可突发 64 包，在共享瓶颈上致微突发、提高丢包概率。RFC 9002 §7.8 建议 pacing。跨连接有 round-robin 公平（`rotate_front_to_back`），但连接内无 pacing。

**修复**：实现简单 token-bucket pacer，速率键 `window/smoothed_rtt`。

### 13. `handle_ack_range` O(R×N)
`QuicAckHandler.cpp:186-242`

每个 ACK range 从 `sent_frames.front()` 重扫。R 个 range 按 PN 降序处理，`sent_frames` 升序，front 的小 PN 包被越过 R 次。最坏 32 ranges（`kQuicMaxAckRanges`）× 数千 in-flight 包 = O(32×N)。

**修复**：range 按 PN 降序、list 升序，从上一 range 停止处续扫；或用有序/索引结构 O(log n)。

### 14. persistent congestion 误判
`QuicLossRecovery.cpp:193-199`

条件 `(stat->newest < oldest_lost || stat->oldest > newest_lost)` 把"所有 acked 包发在所有 lost 包之前"（`stat->newest < oldest_lost`）当作 PC 触发。但此时 lost 包发在 largest newly acked 包**之后**。RFC 9002 §7.6.2 只计发在 largest newly acked 包**之前**的 lost 包。`stat->oldest > newest_lost` 分支正确，`stat->newest < oldest_lost` 分支错。

**修复**：去掉 `stat->newest < oldest_lost` 分支，或整条件换 `stat->oldest > oldest_lost`。

### 15. `drop_ack_ranges` 用发送 PN 阈值接收 PN range
`QuicAckHandler.cpp:221-223`

收到的 ACK 帧被 ack 时，`drop_ack_ranges(frame->packet_number)` 用发送包 PN 调，但 `drop_ack_ranges` 阈值 `largest_range`/`ack_ranges` 跟踪的是**接收** PN。发送/接收 PN 是独立序列，比较无意义。发送 PN 超前接收 PN（数据密集服务端常见）时，所有接收 ACK range 被过早丢弃，致冗余重 ACK。非正确性问题（重复 ACK 无害），但浪费带宽。

**修复**：存并传该 ACK 帧覆盖的最大接收 PN（`frame->u.ack.largest`）而非 `frame->packet_number`。

### 16. `ack_delay` 被 clamp 而非 skip
`QuicCongestion.cpp:77-79`

```cpp
if (handshake_confirmed) { ack_delay = std::min(ack_delay, max_ack_delay); }
```
RFC 9002 §A.1：若 `ack_delay > max_ack_delay`，**不**减（`adjusted_rtt = latest_rtt`）。代码 clamp 后减 `max_ack_delay`，轻微低估 `smoothed_rtt`。下游 `min_rtt + ack_delay < latest` 守卫限制损害。

**修复**：`if (ack_delay <= max_ack_delay) adjusted -= ack_delay;`。

### 17. CUBIC `k` 在 ACK 处理中误偏移；recovery_start future-time fixup 是绷带
`QuicCongestion.cpp:206`（`quic_congestion_cubic_window` 每 ACK 调 `quic_congestion_on_idle(cg, cg.idle, now)`）/ `:124-126`（`now < recovery_start` fixup）

若 `cg.idle` 被 `QuicUdpEndpoint:1744` 置 true 但 ACK 到达时未清，CUBIC `k` epoch 起点 spurious 前移 `now - idle_start`。影响可忽略（亚毫秒 `k` 偏移）但逻辑错。`recovery_start` future-time fixup 在单调时钟下不应发生，掩盖路径重置问题。

**修复**：ACK 处理前清 `cg.idle`，或仅外部 `quic_congestion_on_idle` 调用路径应用 idle 偏移；路径重置显式处理 `recovery_start`。

---

## 🟠 MEDIUM — Codec

### 18. 每包双解析（死字段）
`QuicPacketCodec.cpp:424-433` + `QuicPacketProcessor.cpp:300-305`

`quic_decode_packet` 走完整 decrypted payload 调 `quic_parse_frame_for_receiver` 纯为填 `result.frame_count`/`result.ack_eliciting`，然后 `process_decoded_packet` 重新解析同字节。grep 确认无调用方读 `decoded.frame_count`/`decoded.ack_eliciting`（**死字段**），只读 `key_update`/`header`/`payload`。每包帧解析 CPU 翻倍。

**修复**：删死字段，或从 decode 返回已解析帧数组，或把校验循环移入 processor 带 rollback-on-failure。

### 19. 重复 transport param 静默接受
`QuicTransportParamsCodec.cpp:113-277`

`quic_parse_transport_params` 循环无 seen-set，重复 `initial_max_data` 后值胜出。RFC 9000 §18 要求 PROTOCOL_VIOLATION。

**修复**：维护小 seen-set（如 17 个已知 param ID 的 bitset）拒重复。

### 20. PADDING 逐字节 skip
`QuicTransportCodec.cpp:667-675`

PADDING case 对每零字节 `payload.skip(1)` 循环。Initial 包 pad 到 1200 字节（~1100 padding），循环跑两遍/包（decode 校验 + process），每次 `skip(1)` 冗余重查边界。

**修复**：`memchr` 找下一非零字节批量 skip。

### 21. varint 接受非规范编码；NewConnectionId encode 不校验 cid_len；TP 范围校验推迟
`QuicTransportCodec.cpp:307-327`（非规范 varint）/ `:1224-1229`（encode 不校验 `cid_len≤kMaxConnectionIdLength`，decode 校验）/ `QuicTransportParamsCodec.cpp:148-231`（`ack_delay_exponent`/`max_ack_delay`/`active_connection_id_limit`/`max_udp_payload_size` 不在 codec 校验，推迟到 `QuicConnection.cpp:1615-1616`）

非规范 varint（如 `\x40\x00` 解出 0）允许指纹并掩盖协议 bug（RFC 9000 §16 MAY 拒）。encode 路径 `cid_len>20` 会读越 `cid[20]` 数组入 `stateless_reset_token` 及以远（现调用方设自校验过的 `QuicConnectionId`，潜在）。TP 范围消费方（如 `QuicPacketNumberSpace.cpp:185` 的 `ack_delay_us >>= ack_delay_exponent`）仅因连接层先校验才安全。

**修复**：varint 读后验最小编码；encode 前校验 `cid_len`；TP 在 codec 层拒越界范围（defense-in-depth）。

---

## 🟠 MEDIUM — Stream 层

### 22. `reset()` 拒绝 FIN'd 流，清卸时留 send buffer
`QuicStreamSendQueue.cpp:430-432`；`QuicStream.cpp:508-511`（`close`）/ `:668-670`（`detach_from_connection`）

`QuicStreamSendQueue::reset()` 在 `fin_appended_` 为真时返回 `Invalid`。`close()` 与 `detach_from_connection()` 的 `reset(0)` 清卸逃生口对 FIN'd 流静默失败（`(void)`），extents 不清，`ready_for_connection_release()` 永假（`send_queue_.empty()` 为假），流滞留表里直到连接对象完全析构。RFC 9000 允许 RESET_STREAM 覆盖未确认 FIN。

**修复**：`fin_appended_ && !fin_acked_` 时允许 reset（仅 `fin_acked_` 时拒），或为 detach 路径加 `force_clear_extents()`。

### 23. `write()` 短写静默丢 FIN
`QuicStream.cpp:406-426`（`write(IoBuf)`）/ `:306-334`（`try_write`）

`try_write` append `min(bytes, write_available())` 并以该数为成功返回（非 `WouldBlock`）。`write()` 循环任意成功即 `co_return written`——1000 字节 `write(buf, fin=true)` 只能 append 100 字节时返回 100，`append_fin=false`（`:326` `fin && append_bytes==bytes`）。调用方不知 FIN 丢了，须知道再带剩余 buffer 调。循环不能简单重试（`buf` 未 consume，会从 offset 0 重 append 致重复）。

**修复**：`buf.consume(*written)` + 短写时清 fin 后 `continue` 循环。

### 24. `mark_acked`/`mark_failed` 每 ack range O(n) 全表扫无游标
`QuicStreamSendQueue.cpp:338-385`（`mark_acked`）/ `:387-424`（`mark_failed`）；对比 `QuicStreamRecvQueue.cpp:412-415`（recv 有 `last_insert_` hint）

每 ACK range 从 `head_` `while(cur!=nullptr)`。小 extent 多 ack 的高吞吐流 O(n)/ack、O(n·m) 聚合。recv queue 有 `last_insert_` 游标 hint，send queue 无。`mark_send_failed`（`:396-412`）只前向 merge 不后向，loss/retrans 周期下碎片化 extent list。

**修复**：从 `ready_head_` 起扫或维护 `inflight_head_` 游标；后向 merge `prev`。

### 25. reassembly 逻辑重复 ~200 行
`QuicStreamRecvQueue.cpp:392-456`/`458-547`/`623-733` vs `QuicDataReassembler.cpp:46-162`/`188-253`/`289-393`

`insert_cost`/`insert_reassembled`|`insert`/`create_extent`/`try_merge_with_next`/`insert_after`/`unlink_after`/`has_same_block_neighbor`/`block_of`|`block_offset`|`block_end` 近乎拷贝，仅 block size（16KB vs 4KB）与 cost struct 字段略异。任一 bug 须修两遍。`QuicStreamRecvQueue.cpp` 735 行 largely 因携此重复引擎 + 完整 `ReadAwaiter`。

**修复**：抽共享 `QuicExtentReassembler` 模板/基类。

### 26. 每 reassembly block 16KB `::operator new`，无回收
`QuicStreamRecvQueue.cpp:653`（`IoBuf::allocate(kRecvBlockSize)`）/ `QuicDataReassembler.cpp:313`

每新 16KB block 触发 `IoBuf::allocate` -> `::operator new(sizeof(ControlBlock)+16384)`。block 消费时（refcount->0）释放，从不回收。持续吞吐下 recv 热路径不停 16KB malloc/free churn，违反"minimize alloc churn"。`IoBufNodePool` 回收 list node 但不回收其指向的 IoBuf 存储。

**修复**：block-size-aware free-list，或 `BufPool` arena 给 reassembly 存储。

---

## 🟠 MEDIUM — Crypto

### 27. HKDF `info` 栈缓冲可溢出
`QuicCrypto.cpp:71,75`（buffer `info[2+1+prefix_len+32+1+32]` 留 32 字节给 label，守卫只拒 `prefix_len+label_len>0xff`）

未来调用方传 33–249 字节 label 溢出栈。key-derivation 路径潜在栈溢出。

**修复**：收紧到 `label_len>32`，或 buffer 扩到 `2+1+prefix_len+255+1+255`。

### 28. BoringSSL 回调内 re-entrant `close()`
`QuicTlsSession.cpp:113,121,127,143,153`（`add_handshake_data` 直接 `connection->close(...)`）

`add_handshake_data` 在 `SSL_do_handshake` 内执行时直接 `connection->close()`，中途改帧队列/流/close 状态。与 `send_alert`（`:169-174`）显式文档化的"连接 close 状态从不从 TLS 栈内 re-entrant 改"矛盾，若 `enter_closing` 幂等守卫变化可致 re-entrancy bug。

**修复**：如 `send_alert` 暂存错误，让 `drive_handshake` 在 `SSL_do_handshake` 返回后发 close。

### 29. peer key update 后前代包被丢
`QuicPacketCodec.cpp:385`（`wire_key_phase != connection.key_phase()` 时 `key_update=true`，只试 `next_application_read`，失败后 `!key_update &&` 守卫挡前代 trial-decryption）

重排序包用保留的 `previous_application_read` 加密时被丢而非解出。每次 key update 后虚假丢包 + 重传。

**修复**：next-key 解密失败时，若前代 key 在 grace window 内，也试 `previous_application_read`，不论 `key_update`。

### 30. Retry token 无 replay 保护
`QuicToken.cpp:110-146,148-211`（token 仅 expiry 时间戳，无 nonce/序列号/缓存）

捕获的 Retry token 在有效期内可从同地址重放，绕过地址验证发放大流量。RFC 9000 §8.1.2 建议单次。

**修复**：嵌随机 nonce + 短期 consumed-nonce 缓存，或绑 token 到客户端 Initial SCID。

---

## 🟠 MEDIUM — UDP endpoint

### 31. 1754 行 god file，混 6 职责
`src/quic/QuicUdpEndpoint.cpp`

应拆独立 TU：(a) CID 路由表 `:465-685`；(b) stateless responder（retry/VN/reset/token-close）`:715-941`；(c) 连接创建 `:943-1027`；(d) recv demux `:1029-1194`；(e) send/coalescing engine `:1269-1676`（`build_send_datagram` 单函数 ~280 行）；(f) ECN `:267-332`；(g) ACK 帧生成 `:348-379`。

**修复**：先抽 `QuicPacketBuilder`（send 路径自包含）。

### 32. 单一共享 `send_buffer_` 串行化所有连接发送
`QuicUdpEndpoint.h:188`；`QuicSendScheduler.cpp:233-235`

`flush_connection` 复用 endpoint 级 `send_buffer_`，build 与 send 不能重叠，即便加 `sendmmsg` 也被一次建一个 datagram 限制。

**修复**：per-connection scratch buffer 或小 buffer ring，使 build 可与在飞 send 重叠。

### 33. `recv_loop` 从未被 `init()` spawn
`QuicUdpEndpoint.cpp:437`（只 spawn `send_scheduler_.run()`）/ `:518`（`recv_loop` 无生产调用方，仅测试调 `recv_once`）

app 调 `init()` 等待将永远收不到包。潜在 silent-failure。

**修复**：`init` 里 spawn `recv_loop`，或显式文档化调用方须驱动 `recv_once`/`recv_loop`。

### 34. `recv_loop` 吞错无背压
`QuicUdpEndpoint.cpp:518-529`

仅 `Canceled`/`BadFd` break，`WouldBlock`/`Invalid`/`MessageTooLarge`/`Already` 都 `continue`。持续丢包（内核 rx buffer 压力、坏包）下循环重 arm `recv_packet` 无 yield、无 `SO_RCVBUF` 增长、`dropped_datagram_count_` 增长无告警。

**修复**：重复 WouldBlock 时 yield；drop-rate 飙升时 metric/log。

---

## 🟢 LOW / 设计 / 结构

### 35. QuicConnection.cpp 2601 行 god object，混 6+ 职责
`QuicConnection.cpp` / `QuicConnection.h:214-746`

同时拥有：close/idle/drain 状态机、stream 生命周期 + 隐式建流、连接级 + per-stream 流控（`recv_data_consumed_`/`peer_data_reserved_`/`maybe_extend_recv_data_flow_control`/`check_recv_data_delta`）、本地 + 远端 CID 池管理（~300 行 `find_*_connection_id_slot`/`retire_remote_connection_id`/`recv_new_connection_id_frame`）、15+ `queue_*` 样板、5 个计时器回调。应拆 `QuicConnectionIdManager`、`QuicFlowController`，`queue_*` 模板化。

### 36. `Options` 混淆不可变配置与可变运行时状态
`QuicConnection.h:678`（`Options options_{}`）

构造时拷贝但生命周期内被改：`options_.remote_addr`/`options_.remote_connection_id` 随 migration 改（`QuicPathManager.cpp:214-216`）；`options_.max_local_*_streams` 随 `recv_max_streams_frame` 改（`:1517,1640`）；`options_.transport.max_idle_timeout` 随对端 params 改（`:1652`）。配置与运行时状态对调用方不可区分。

**修复**：拆 `const Options config_` + 独立 `RuntimeState`。

### 37. `QuicPathManager` 经 friend 直插 QuicConnection 私有
`QuicPathManager.cpp:122,155,198,214-216`；friend 声明 `QuicConnection.h:676`

直接访问 `connection_.remote_cids_`（私有 `std::array`）、`connection_.options_`、`connection_.congestion()`/`connection_.rtt()`。CID 池实现细节跨类泄漏。`rebind_paths_to_cid` 写 `connection_.options_.remote_connection_id`，耦合 path 状态与连接 config。

**修复**：暴露窄 accessor（`active_remote_cids()`/`set_remote_connection_id()`）代替 blanket friend。

### 38. `mark_closed()` public API 跳过计时器取消 + endpoint detach
`QuicConnection.cpp:749-757`

置 `state_=Closed`、关流、清流表，但**不**取消 5 个计时器、**不** detach endpoint、**不**调 `enter_closed()`。唯一调用方 `QuicUdpEndpoint::force_detach_connection`（`QuicUdpEndpoint.cpp:638`）紧接 `detach_from_endpoint()` 才掩盖。若 `mark_closed()` 单独调用，armed timer 在 Closed 连接上触发，`ready_for_destruction()` 永假（`attached_to_endpoint_` 仍真），连接泄漏。

**修复**：`mark_closed()` 委托 `enter_closed()`，或改 private 让 `force_detach_connection` 调 `enter_closed()`+`detach_from_endpoint()`。

### 39. `EVP_AEAD_CTX` 用 `memcpy` 交换，依赖 BoringSSL 内部布局
`QuicConnection.cpp:93-96`（`QuicPacketProtectionKeys::swap()`）

按 `std::memcpy` 拷 `EVP_AEAD_CTX` 结构，注释承认假设"无内部指针引用 context 自身地址"。每次 key update（`apply_key_update`，`QuicPacketProcessor.cpp:59-68`）触发。BoringSSL 布局变化引入自引用指针会静默损坏 AEAD 状态，不编译失败。

**修复**：destroy + 从交换后的 key/iv 重建 AEAD ctx，或 `EVP_AEAD_CTX_copy` 后 `EVP_AEAD_CTX_cleanup` 源。

### 40. 协议违规返回 `IoErr::Busy`
`QuicConnection.cpp:1332,1414,1471,1487,1494,392`

每条协议违规路径 `close(...)` 后返回 `std::unexpected(common::IoErr::Busy)`。`Busy` 语义是"稍后重试"，但连接已 terminal-closing。`process_decoded_packet` 传播之，`quic_process_datagram` 返回给调用方，后者可能把 `Busy` 当瞬时态重试。

**修复**：post-`close()` 返回一律用 `IoErr::Canceled`。

### 41. STREAM 帧热路径每帧 3 次 O(log n) `find_stream`
`QuicConnection.cpp:1335,1343,1348`

`recv_stream_frame` 在 `:1335`（`is_gone_peer_stream` 守卫）调 `find_stream`，`:1343`（`peer_stream_exceeds_advertised_limit` 守卫）再调，`:1348` `get_or_create_peer_stream` 内部第三次（`streams_.find` 于 `:1269`）。对已存在流每 STREAM 帧 3 次 O(log n) 查找，每次还 `stream_type()`/`is_local_stream()` 重算位掩码。

**修复**：顶部 `find_stream` 一次复用，仅 null 时 `get_or_create_peer_stream`。

### 42. 重复 equality helper 跨 3 个 TU
`connection_id_equal`：`QuicConnection.cpp:44` + `QuicPacketProcessor.cpp:31`；`socket_address_equal`+`ip_address_equal`：`QuicPacketProcessor.cpp:17-29` + `QuicPathManager.cpp:18-30`

三份独立拷贝。改一处（如加常时路径或新地址族）须手动复制。

**修复**：提到共享 `QuicAddressUtil.h` inline 头。

### 43. CID slot / stateless reset 检测 O(n) 线性扫
`QuicConnection.cpp:1700-1728`（`detects_stateless_reset`，每不可解短头包扫 8 slot 16 字节常时比较）/ `1751,1826,1770,1845,1679`

`local_cids_`（3 slot）/`remote_cids_`（8 slot）线性扫。N 小可接受，但 `find_local_connection_id_slot` 在 endpoint DCID 路由与 `recv_retire_connection_id_frame` 每 packet 调。

**修复**：序列稠密且小，按 `sequence_number % capacity` 索引或维护 `sequence->slot*` 查找。

### 44. `ReadAwaiter`/`WriteAwaiter` 结构相同但全重复
`QuicStreamRecvQueue.cpp:17-154` vs `QuicStream.cpp:22-195`（~140 行重复）

deadline 检查、单写者 `Busy`、`post_at` 计时器、`post` resume、`completed_`/`resume_posted_` 标志、`await_*` 套件相同。`WriteAwaiter` 额外有连接窗口 wait list（`peer_data_wait_link_`）。

**修复**：抽共享基类。

### 45. 其他 LOW
- `quic_varint_len` 对 `value>kMaxVarint` 返 8 而非错误（`QuicTransportCodec.cpp:358`），长度计算已先承诺 8 字节。
- varint parse 循环逐字节 `read_u8` 冗余边界检查（`:315-325`），8 字节 varint 7 次冗余。
- `preferred_address` TP 静默丢弃（`QuicTransportParamsCodec.cpp:274` default），preferred address migration 不支持；至少可校验其定长结构。
- `address_validation_key` 生成一次从不轮换（`QuicUdpEndpoint.cpp:395`）。
- `address_hash` 用 SHA-1（`QuicToken.cpp:15-38`）。
- key material 用 `{}` 清零可能被 dead-store 消除（`QuicPacketProtectionKeys::reset`）；应用 `OPENSSL_cleanse`。
- `quic_process_initial_datagram`（test-only 入口）跳过长头保留位校验（`QuicPacketProcessor.cpp:599-615`），生产路径在 `QuicPacketCodec.cpp:413-417` 查。
- `pto_backoff` 无 max PTO count cap（`QuicLossRecovery.cpp:22-36`）。
- `ack_delay` 左移对大 raw 值可溢出 `uint64_t`（`QuicCongestion.cpp:69-74`），仅 `exponent>=63` 守卫。
- `last_insert_` 在 take 时置 nullptr 强制下次全扫（`QuicStreamRecvQueue.cpp:566-568`）。
- `try_write(IoBufChain&)` 即便短写也经 `take_prefix` 不可逆改调用方 chain（`QuicStream.cpp:375-380`）。
- `encode_stream_frame` 返回 `Blocked` 不清 `stream_send_pending_`（`QuicStream.cpp:570-572`），调度器重复尝试装不下的流。
- `generate_unique_connection_id` 每次最多 8 次 `RAND_bytes` + 8 次树查（`QuicUdpEndpoint.cpp:650-661`）；用 userspace CSPRNG 或 counter-encoded CID。
- `detach_connection` 同步在 `process_datagram` 内跑 destroy 回调（`:632-634`），潜在 reentrancy。
- CID 表用字节级 FNV-1a 哈希每入站数据报（`:536-545`），可直接按 CID 字节 key RB-tree。

---

## 杂项

- `FIBER_ENABLE_HTTP3` CMake 选项是**死选项**（`CMakeLists.txt:11` 声明后全仓无引用，QUIC 源码经 `GLOB_RECURSE` 无条件全编）。要么用起来要么删掉。
- 全 `src/quic/` **0 个 `TODO/FIXME`**——可能是真干净，也可能是 marker 被清；上述若干问题值得补标记追踪。
- **测试缺口**：`QuicAckHandler`/`QuicLossRecovery`/`QuicSendScheduler`/`QuicPathManager` 无专门单测，正是可靠性 bug 最易藏处。建议优先补。

---

## 修复优先级

| 优先级 | 项 | 工作量 |
|---|---|---|
| P0 | #2 PTO 忙循环、#4/#5 DoS 向量、#1 key-update、#3 re-entrancy、#7 UAF | 中 |
| P0 | #9 GSO/sendmmsg、#8 接收零拷贝 | 大 |
| P1 | #11 loss 减窗基准、#22 `reset()` FIN'd 流、#23 `write()` 短写丢 FIN、#19 重复 TP 拒绝、#27 HKDF 溢出、#16 ack_delay skip | 小（多为一行） |
| P1 | 给 #13/#14/#15 + AckHandler/LossRecovery/SendScheduler/PathManager 补单测 | 中 |
| P2 | #35 拆 QuicConnection god object、#31 拆 QuicUdpEndpoint、#25 去重 reassembly、#44 去重 awaiter | 大 |
