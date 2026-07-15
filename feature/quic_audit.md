# `src/quic/` 模块审计

> 审计范围：`src/quic/` 全部 49 个文件（约 17,900 行），按连接核心（`QuicConnection`/`QuicPacketProcessor`/`QuicPathManager`）、packet/frame codec（`QuicPacketCodec`/`QuicFrame`/`QuicTransportCodec`/`QuicTransportParamsCodec`）、stream 层（`QuicStream`/`QuicStreamTable`/`QuicStreamSendQueue`/`QuicStreamRecvQueue`/`QuicDataReassembler`）、可靠性与拥塞（`QuicAckHandler`/`QuicCongestion`/`QuicLossRecovery`/`QuicPacketNumberSpace`/`QuicSendScheduler`/`QuicPacer`）、crypto/token（`QuicCrypto`/`QuicTlsSession`/`QuicToken`/`QuicConnectionId`）、UDP endpoint（`QuicUdpEndpoint`）六个域并行评审后整合。所有结论均经直接读码 + 交叉验证。
>
> 核验事实：Initial key 派生（salt `0x38762cf7…` + DCID）、AEAD nonce（IV XOR PN）、header protection（AES-ECB/ChaCha20 双路径）、Retry integrity tag、stateless reset（HMAC-SHA256 + 常时比较）、CID 用 `RAND_bytes` CSPRNG + 重试去重均符合 RFC 9001；anti-amplification（3×）按 path `received*3` 正确封顶（`QuicPathManager.cpp:244-260`）；ECN `IP_RECVTOS`/`IP_TOS`/`IPV6_TCLASS` 设置与解析齐全；稳态 recv 零堆分配（`read_buffer_`/`plaintext_buffer_` 复用）；endpoint 单线程 per-socket，`dcid_tree_` 无锁；全 `src/quic/` `std::string/vector/function/shared_ptr` 仅 4 处（全在 `QuicUdpEndpoint.cpp`），性能纪律真执行。
>
> 测试覆盖：约 10,600 行 QUIC 测试；`QuicPacer` 已有确定性算法单测及 scheduler/endpoint 集成回归，`QuicAckHandler`/`QuicLossRecovery`/`QuicPathManager` 的专项覆盖仍然薄弱；全 `src/quic/` 0 个 `TODO/FIXME` 标记。
>
> 状态：部分动工。#2 PTO 忙循环、#3 re-entrancy、#4 create-before-auth（SSL_new 延后，方案 A）、#5 无状态响应限速、#6 owner EventLoop 计时器已修复（2026-07-14，见下）；#7 UAF 于 2026-07-15 复核为正常生命周期不可达，并已加入 detach-before-destroy 不变量断言与回归测试，不再列为 P0；#8 接收复制于 2026-07-15 复核为当前 endpoint 复用明文缓冲架构下保证数据生命周期所必需，降为后续架构性能优化；#11 于 2026-07-15 复核为 RFC 9438 允许的 `flight_size` 减窗行为，不构成协议错误，保留为 application-limited 场景的后续性能优化；#12 pacing、#13 ACK range 扫描、#15 ACK-of-ACK 阈值已于 2026-07-15 修复；#14 于 2026-07-15 复核为方向误判不存在，并已补齐三类区间回归测试；#16 于 2026-07-15 复核为 RFC 要求的 clamp 行为，并修复相邻的 RTT 扣减等号边界；#17 于 2026-07-15 复核为调用链误判，idle ACK 在进入 CUBIC window 前已返回，路径重置也已显式重置拥塞状态；#18 强保护包双解析已于 2026-07-15 消除，Initial 为无副作用静默丢弃而有意保留预校验。待修 P0：#1 key-update、#9 GSO，其余排期。

## 总体评价

codec 边界检查严密（`QuicCursor` 每读必 `remaining()`）、零拷贝视图（`QuicSlice`）、`write_or_count_*` 模式、crypto 核心、anti-amplification、ECN、稳态零分配均正确。原审计列出 **9 个高危**；其中 #7 经生命周期调用链复核，不构成正常路径可达的独立 UAF，改列为不变量防御项；#8 经缓冲区所有权与异步消费路径复核，确认是当前架构下必要的生命周期复制，降为后续架构性能优化，不再列为 P0/HIGH。其余问题包括 key 用量越界、PTO 忙循环、re-entrancy、DoS 向量、缺 GSO，以及若干中危的可靠性与性能问题。可靠性与拥塞控制域问题最集中且专项覆盖仍然薄弱，风险最高。按严重度排列如下，均给出 `file:line` 与触发场景。

---

## 🔴 HIGH

### 1. 无 key-update 主动发起 + 无 AEAD 用量计数
`QuicPacketProcessor.cpp:75,519-524`（`flip_key_phase` 仅在 `apply_key_update` 内，只由对端发起触发）

`flip_key_phase()` 只在收到对端 key update（`decoded.key_update`）时经 `apply_key_update` 触发，没有任何 per-generation 包计数。RFC 9001 §6.6 的机密性上限 2²³≈840 万包在持续 H3 连接下几分钟可达；服务端从不主动 update，若客户端也不 update，长连接会越过 MUST-NOT-exceed 上限。

**修复**：按 key generation 计收发/收包数，在接近机密性上限时主动 `apply_key_update`。

> 📌 **nginx 对照（2026-07-14）**：本问题在 nginx 中**同样存在**，修复时应尽可能参考 nginx 的 key-update 框架，仅在其上补计数+主动发起。逐行对照结论：
> - 翻转同样只由对端发起触发：`ngx_event_quic.c:1080` `qc->key_phase ^= 1` 仅当 `pkt->key_update`，而 `pkt->key_update` 由 `ngx_event_quic_protection.c:1169-1174` 检测对端 KPHASE 变化置位（`key_phase = (pkt->flags & NGX_QUIC_PKT_KPHASE) != 0`）——与本项目 `flip_key_phase` 经 `apply_key_update`/`decoded.key_update` 触发**逐字节同构**。
> - nginx **无 per-generation 包计数**：`ngx_event_quic_connection.h:301` key 相关字段只有 `unsigned key_phase:1` 一个 1-bit 标志，全模块 grep `count/usage/generation/rotate/proactive/confidential/limit` 在 key phase 上下文零命中。发送侧 `ngx_event_quic_output.c:698` 只读 `qc->key_phase` 贴到出包，从不主动翻转。
> - nginx 唯一额外做的事是 `ngx_event_quic_ssl.c:768` 在 TLS 握手完成后 `ngx_post_event(&qc->key_update, …)`（注释引用 RFC 9001 §9.5）——这是**预派生 next key 消除 timing side channel**，使收到对端 key update 时能常数时间解密，**不是**主动发起一次 key update。
> - **方针**：key-update 子系统（HKDF `tls13 quic ku/key/iv` 派生、`keys_switch` 翻转、next_key 预派生、§9.5 timing 防护）一律对齐 nginx `ngx_event_quic_protection.c:769-862`/`ngx_event_quic_ssl.c:762-768`/`ngx_event_quic.c:288-290,1074-1090`；本项目在此基础上**新增** per-generation 收发/收包计数 + 接近 2²³ 上限时主动 `apply_key_update`（nginx 与本项目都缺的部分）。

### 2. PTO 定时器/probe 判定不一致 -> 忙循环 + 漏探针
`QuicLossRecovery.cpp:237-238`（timer arm）vs `:270-273`（probe skip）

`quic_loss_detection_timer` 对任何非空 `sent_frames` 都 arm PTO，但 `quic_queue_pto_probe_frames` 跳过 `back->packet_number <= largest_acked_packet_number` 的 level。当 `largest_acked` 之下还有未确认包（带 gap 的部分 ACK 常见）时：PTO 以 delay 0 触发 -> 不排探针 -> `pto_count` 不增 -> 重新 arm 同样 delay -> **忙循环**。且跳过条件本身逻辑反了：`sent_frames` 里的包定义上就是未确认的（已确认的会被移除），`back->PN <= largest_acked` 说明**确有**未确认包需要探针，而非反之。可致虚假忙循环与丢包后无探针的死锁。

**修复**：去掉 `quic_queue_pto_probe_frames` 的 skip 条件（或换 `in_flight==0` 守卫），并/或在 `quic_loss_detection_timer` 加同样守卫，使二者一致。

> ✅ **已修复 2026-07-14**。复核结论：上述"忙循环"分析对触发场景的判定有误--`back->PN <= largest_acked` 意味着 `front->PN <= largest_acked`，该 level 命中 **Lost 模式**而非 Pto 模式（`quic_loss_detection_timer` 中 Lost 优先且 `front->PN <= largest_acked` 才算 has_lost），故 skip 在保序不变量下是死代码，正常路径不发病。与 nginx `ngx_event_quic_ack.c` 逐行对照，skip 条件两端**逐字节相同**（timer arm 用 `back` 无守卫、probe skip 用 `back->PN <= largest_ack`），nginx 同样如此。真正的本质差异与隐患在于：**nginx 的 `pto_count++` 在 PTO handler 末尾无条件执行（`:1164`，for 循环之后），本项目原为条件执行**（仅 `*queued` 为真时）。一旦保序不变量被破坏（失序/漏 re-arm 等），条件版会进入 `delay 0 -> 全 skip -> pto_count 不增 -> re-arm delay 0` 的忙循环，而无条件版靠 backoff 翻倍使 delay 转正、自终止。修复方式：`QuicConnection::on_loss_detection_timer` 中将 `++pto_count_` 提为无条件（与 `schedule_send` 解耦），对齐 RFC 9002 Appendix A.7 与 nginx。skip 条件本身未动（保序下为死代码，留作无害；若后续想彻底对齐 RFC "ack-eliciting in flight 才 arm PTO" 可另案处理，见 #10）。

### 3. `enter_closing` 在帧循环中途触发 -> re-entrancy
`QuicConnection.cpp:1404`（`maybe_finish_graceful_close`）/ `:2252`；`QuicPacketProcessor.cpp:289,301`（守卫仅包入口）

`recv_stream_frame -> try_release_stream -> retire_stream -> maybe_finish_graceful_close -> enter_closing` 在 `process_decoded_packet` 的帧循环内触发。`enter_closing` 调 `close_all_streams()` + `clear_pending_frames_all_levels()`，改动全局连接状态，而同包内后续帧仍在迭代。Closing 状态守卫只在包入口（`QuicPacketProcessor.cpp:289`）查，帧间不查。后果：同包内先前入队的帧（ACK/流控）被静默丢弃，后续帧在 Closing 连接上处理（违反 RFC：Closing 只应发 CC 帧）。

**修复**：`try_release_stream` 后检查 `closing()` 跳出帧循环；或把 `enter_closing` 过渡用 `loop_->post()` 延迟到包处理结束后。

> ✅ **已修复 2026-07-14**（采用上述第二方案）。`QuicConnection::maybe_finish_graceful_close` 不再同步调 `enter_closing(close_info_)`，改为：当 `EventLoop::current_or_null()` 非空（即处于连接自身事件循环、帧循环进行中）时调 `arm_close_timer_immediate(*loop)`，把 close timer arm 到 `loop.now()`，由下一个 tick 的 `on_close_timer` 执行 `enter_closing(close_info_)`（`on_close_timer` 的 GracefulClosing 分支本就走这条路径）；`current_or_null()` 为空（同步直调、无循环）时仍 inline 完成（无帧循环可重入）。逐行对照 nginx：`ngx_quic_finalize_connection` 一律 `ngx_post_event(&qc->close, &ngx_posted_events)`，`qc->closing` 只在延后的 `ngx_quic_close_connection` 内置位，帧循环内绝不中途进入 closing——本修复与之同构。安全性要点：(1) 复用既有 `close_timer_entry_`（TimerEntry），其 cancel 不 `assert(in_loop())`，`~QuicConnection::cancel_close_timer` 已覆盖生命周期（区别于不可从 MpscQueue 安全摘除的 NotifyEntry，故未用 `post`）；(2) GracefulClosing 下 `accepting_new_streams()` 为假，`active_stream_count()` 一旦归零不会再升，故 fire 时无需重检；(3) 过渡延后期间状态仍为 GracefulClosing，同包后续帧按 GracefulClosing 正常处理（仅不再被 inline `enter_closing` 丢弃已入队 ACK/流控帧），待 `enter_closing` 在包处理结束后清空并改发 CC，符合 RFC 9000 §10.2。回归测试：`QuicConnectionShutdownTest.GracefulCloseCompletionDeferredOnRunningLoop`（运行循环上 retire 末流后断言 state 仍为 GracefulClosing 且 close_timer 已 arm，pump 后才转 Closing；回退到同步实现时该测试 FAIL）。全 1177 ctest + 56 lite_nginx 测试绿。

### 4. ✅ 连接对象在 Initial 认证前分配 -> DoS 向量（SSL_new 延后已修，方案 A）
`QuicUdpEndpoint.cpp:1168`（`create_connection`）/ `:1177`（`quic_process_datagram` 内 AEAD 认证）

`create_connection`（含 `tls().init_server(*tls_context,…)` + CID 注册）发生在 `quic_process_datagram`（AEAD 认证）**之前**。`retry=false`（默认）下，伪造源地址 + 语法合法 header + 垃圾 payload 的 Initial 每包都触发完整连接分配 + TLS ctx 初始化 + CID 注册，无一校验。这是经典 QUIC DoS 向量。`retry=true` 缓解，但 create-before-auth 顺序独立于 Retry。

**修复**：先解 header protection + AEAD 认证再分配；或默认 `retry=true`；或加 per-source 建连速率限制。

> 📌 **nginx 对照（2026-07-14）**：create-before-auth 的**顺序在 nginx 中同样存在**，默认 `retry` 配置也一致，但 nginx 已把最贵的 `SSL_new` 延后到认证之后，故每伪造包成本显著低于本项目。逐行对照结论：
> - 首包入口 `ngx_quic_run`（`ngx_event_quic.c:200`，由 `ngx_http_v3_request.c:75` 调用）-> `ngx_quic_handle_datagram` -> `ngx_quic_handle_packet` -> **`ngx_quic_new_connection()`（`:948`，分配 `ngx_quic_connection_t`+`ngx_quic_keys_t`+经 `ngx_quic_keys_set_initial_secret` 做 HKDF Initial 密钥派生）发生在 `ngx_quic_decrypt()`（`:996`，AEAD 认证）之前**--与本项目 `create_connection` -> `quic_process_datagram`（AEAD）顺序逐字节同构。
> - 默认 **`retry=0`（off）**：`ngx_http_v3_module.c:244` `ngx_conf_merge_value(conf->quic.retry, prev->quic.retry, 0)`，与本项目 `retry=false` 默认一致。故默认配置下伪造源 + 合法 header + 垃圾 payload 的 Initial 同样在 AEAD 拒绝前到达 `ngx_quic_new_connection`。
> - **关键差异（nginx 已做的便宜那一半）**：`ngx_quic_new_connection` **不**创建 SSL 对象；`SSL_new`+`SSL_set_quic_method`+transport params 编码全在 `ngx_quic_init_connection()`（`:1015`），而它**只在 `ngx_quic_decrypt` 成功后**（`:1014 if (c->ssl == NULL)`）才执行。本项目 `create_connection` 在认证前就 `tls().init_server()` -> `SSL_new()`（`QuicTlsSession.cpp:238`，含 `SSL_set_quic_method`/TP 编码/early-data 设置，`QuicUdpEndpoint.cpp:1015`）--这是本项目每伪造包多付的最大一块成本。
> - 无连接堆积：decrypt 失败返 `NGX_DECLINED`/`NGX_DONE` -> `ngx_quic_run:209-211` 立即 `ngx_quic_close_connection()`，伪造包不累积活连接，仅每包 CPU（分配+HKDF+失败解密+释放）。本项目失败时 `force_detach_connection`，同样不堆积--差异仅在每包成本量级。
> - `ssl_retry on` 时，`ngx_quic_validate_token`（`:904`）/`ngx_quic_send_retry`（`:931`）在 `ngx_quic_new_connection` **之前**，未验证源永不分配连接。本项目有同等 Retry 机制但默认关。
> - **方针**：本项目修复**优先对齐 nginx 已做的便宜那一半**--把 `init_server`（`SSL_new`+QUIC method+TP 编码+early-data）从 `create_connection` 延后到 `quic_process_datagram` AEAD 认证成功之后（对齐 nginx `ngx_quic_init_connection` 的位置与触发条件）；更进一步再做 nginx 未做的贵那一半（先解 header protection + AEAD 再分配连接结构体，需更大重构，须重排 `create_connection` 与认证的先后）；并/或默认 `retry=true`、加 per-source 建连限速。
>
> **修改方案（✅ 已实施 2026-07-14，方案 A）**：把 `init_server` 从 `create_connection` 延后到 `quic_process_datagram` 的 AEAD 认证成功（`quic_decode_packet`，`QuicPacketProcessor.cpp:706`）之后、`process_decoded_packet`（`:738`，处理 CRYPTO 帧）之前，对齐 nginx `ngx_quic_init_connection`（`ngx_event_quic.c:1015`，decrypt 成功后 `if (c->ssl == NULL)` 触发）。
>
> 核心收益：伪造源 + 合法 header + 垃圾 payload 的 Initial 在 `quic_decode_packet`（AEAD）处即被拒，**不再付出 `SSL_new`**（当前每伪造包一份，`QuicTlsSession.cpp:238`）。认证失败 -> `recv_once` 既有 `force_detach_connection`（`QuicUdpEndpoint.cpp:1183`）清理，路径不变。
>
> 改动清单：
> 1. `QuicConnection.h`：`Options`（`:277`）加 `net::TlsServerContext *tls_context = nullptr;`（前向声明 `net::TlsServerContext`，与 `QuicUdpEndpoint::Options:55` 同型）；新增 `[[nodiscard]] common::IoResult<void> ensure_server_tls() noexcept;`
> 2. `QuicConnection.cpp`：实现 `ensure_server_tls()` = `if (tls_.initialized()) return {}; if (options_.tls_context == nullptr) return {}; return tls_.init_server(*options_.tls_context, *this);`（双重幂等：`QuicTlsSession::init_server` 开头 `if (ssl_!=nullptr) return Already`，`QuicTlsSession.cpp:241`）
> 3. `QuicUdpEndpoint.cpp::create_connection`（`:1010-1021`）：加 `conn_options.tls_context = options_.tls_context;` 填指针；**删除** `if (options_.tls_context != nullptr) { tls().init_server(...) }` 整块（其 cleanup 分支随之删除）；`attach_to_endpoint`/push_back/`++active_connection_count_` 不变
> 4. `QuicPacketProcessor.cpp::quic_process_datagram`：在 `quic_decode_packet` 成功（`:735`）后、`process_decoded_packet`（`:738`）前插 `if (conn.role()==Server && !conn.tls().initialized()) { auto ok=conn.ensure_server_tls(); if (!ok) { if (has_good_packet) return aggregate; return std::unexpected(ok.error()); } }`；`quic_process_initial_datagram`（test-only，`:599` 帧循环前）同步加
>
> 不变量验证（已逐项读码确认）：
> - `init_server` 全仓唯一调用点即 `create_connection:1015`，延后不影响他处。
> - `~QuicTlsSession`（`QuicTlsSession.cpp:231-233`）`if (ssl_!=nullptr) SSL_free`，未初始化时安全 -> 认证失败 force_detach 析构无 UAF。
> - `attach_to_endpoint`（`QuicConnection.cpp:2162`）只设 `endpoint_`+标志，不碰 TLS -> create_connection 内 init_server 移除后 attach 安全。
> - `detach_from_endpoint`/`force_detach_connection` 不碰 TLS -> 认证失败清理路径不变。
> - 首包 CRYPTO 帧不丢：`init_server` 在 `process_decoded_packet` 前完成；`provide_crypto_data`（`:170`）/`handle_crypto_frame`（`:208`）的 `!tls().initialized()` 守卫不再命中（生产）；测试（tls_context==nullptr）仍命中跳过，行为与现状一致。
> - coalesced Initial+0-RTT：while 循环按 offset 顺序，Initial 先认证触发 init_server，0-RTT 后处理时已初始化（early_data key 依赖 SSL 对象），时机与原"create_connection 内 init"等价。
> - client role 不受影响（全仓无 `init_client`，client 连接不经此路径）。
>
> 测试兼容性：`QuicUdpEndpointTest`/`QuicPacketProcessorTest` 均不设 `tls_context`（`make_endpoint_options` 默认 nullptr），`ensure_server_tls` 直接 `return {}` 跳过，行为不变。**回归已验证全绿：1167 ctest 通过，QUIC+H3 子集 196 全过。** 建议补一条计量测试（mock `TlsServerContext` 计数 `init_server` 调用，断言认证失败的伪造 Initial 不触发 `SSL_new`）。
>
> 备选/远期：
> - **方案 B（贵那一半，nginx 未做）**：endpoint 层先派生临时 Initial key + `quic_decode_packet` 认证首包，认证成功才 `create_connection` + 注册 CID。需在 `QuicUdpEndpoint` 持临时 crypto 状态、处理 DCID 路由前认证，重构大、风险高，列远期。
> - **方案 C（廉价叠加，与 A 正交）**：默认 `retry=true`（未验证源发 Retry，永不触达 create_connection）+ per-source 建连 token bucket（与 #5 的 stateless 限速同框架）。可同做。

### 5. ✅ Retry / VN / invalid-token-close 无速率限制
`QuicUdpEndpoint.cpp:1087`（VN）/ `:1143`（Retry）/ `:1160`（invalid-token close）/ `:715`（仅 stateless_reset 有 token bucket）

只有 `send_stateless_reset` 受 `allow_stateless_reset` token bucket（8/s）约束。其余三个 stateless 路径 1:1 无上限放大。`encode_invalid_token_close_packet`（`:67-97`）每包还新建 `QuicConnection temp` + `init_initial_crypto` HKDF——每伪造包一份 crypto 工作。伪造源洪泛可驱动等量无限制出口流量。

**修复**：token bucket 覆盖全部 stateless 响应（最好 per-peer，nginx 风格）；预派生/缓存 Initial keys。

> ✅ **已修复 2026-07-14**。`QuicUdpEndpoint` 现在用固定内存、按响应类型独立计数的 endpoint + peer 双层限速覆盖 Stateless Reset、Version Negotiation、Retry 和 INVALID_TOKEN close；IPv4 按地址、IPv6 按 /64 聚合，peer 表固定 256 桶，不允许网络流量驱动动态分配。配置通过 `Options::stateless_response_limits` 暴露，`0` 表示关闭相应维度的上限，并增加统一的限速丢弃计数。VN 另补 RFC 9000 §5.2.2 的 1200 字节门槛，避免对不足以承载 v1 Initial 的未知版本小包响应；无法认证、无法识别用途的 garbage token 改为未验证地址（启用 Retry 时重新发 Retry），只对已识别为 Retry 且失效/过期的 token 发送 INVALID_TOKEN。Initial key 预派生/缓存尚未实施，可作为后续纯 CPU 优化独立处理。

### 6. ✅ 计时器操作系统性用 `EventLoop::current_or_null()` 而非 `loop_`
`QuicConnection.cpp` 约 20 处（356,460,615,632,639,652,667,679,689,721,958,1009,1032…）

每个计时器 arm/cancel 与状态过渡路径用 `event::EventLoop::current_or_null()` 而非成员 `loop_`。单 loop 不变量只在 `LocalStreamAttachAwaiter`/`attach_local_stream`（`:156,1114,1167`）assert，核心方法（`enter_closing`/`arm_idle_timer`/`on_idle_timer`）不 assert。若 `Lease` 释放或 `close()` 从别的 loop 调用（如 H3 在另一 worker），计时器被 arm/cancel 到错误的堆，`~QuicConnection`（`:356`）经 `current_or_null()` 取到的可能是错的 loop，留下 armed timer 在已释放内存上触发。

**修复**：连接方法内一律用 `loop_`；公开方法入口 assert `EventLoop::current()==loop_`。

> ✅ **已修复 2026-07-14**。所有 connection/path timer API 删除外部 `EventLoop&` 参数，arm/cancel、回调取时与重排统一使用 `QuicConnection::loop_`；状态迁移入口增加 loop-affinity 断言，回调同时断言正在 owner loop 上运行。析构时仅在 owner loop 内正常 cancel；若所属 `EventLoopGroup` 已 stop+join，则走 `cancel_quiesced` 从 owner loop 的 intrusive heap 清理，运行中的其他线程绝不跨 loop 修改 timer heap。`QuicUdpEndpoint::detach_connection` 同样断言 endpoint loop 与 connection owner loop 一致后统一取消全部 connection/path timers。

### 7. ⚪ 复核排除：析构少通知是生命周期不变量防御缺口，非独立 UAF
`QuicConnection.cpp:376-393`（析构）/ `:2268-2299`（detach、release 与销毁门槛）/ `QuicUdpEndpoint.cpp:630-655`（先 detach、后释放 endpoint lease）

析构确实只调用 `notify_all_local_stream_attach_waiters(Canceled)`，没有调用 `notify_peer_data_waiters(Canceled)`；但原结论把这个不对称直接推导成可达 UAF，并不成立。

> ⚪ **复核 2026-07-15**。endpoint 管理的连接仅在 `!attached_to_endpoint_ && ref_count_ == 0` 时经 `on_destroy_` 销毁。`QuicUdpEndpoint::detach_connection` 在释放 endpoint 持有的最后一份 lease 之前，必先调用 `QuicConnection::detach_from_endpoint`；后者先通知 local-stream 与 peer-data 两类 waiter，再关闭/清空 stream。endpoint 关闭、主动移除连接和新建连接后的收包失败均走这条 detach 路径。attach 之前的建连失败可以直接析构，但新对象此时尚无 stream，也不可能已有 peer-data waiter。因此在仓库当前所有生产销毁入口中，析构开始时 peer-data waiter 链表已经为空。
>
> `WriteAwaiter` 也不直接保存 `QuicConnection *`；它通过 `stream_->conn_` 回到连接，`peer_data_wait_link_` 本身是 awaiter 内的 intrusive hook，并不是指向连接的指针。`QuicStreamTable::clear` 会先执行 `stream.detach_from_connection()` 把 `conn_` 置空，再释放 table 持有的 stream lease。因此在 stream 另有合法 lease 保活时，残留 hook 本身不会使 awaiter 解引用已析构的 connection。
>
> 只有调用者绕过 intrusive ownership，直接销毁一个仍有挂起异步操作的栈/堆连接，或先引入另一个破坏 detach-before-destroy 不变量的生命周期 bug，才能进入原条目假设的状态。这不应作为一个独立 HIGH 问题计算。若 stream table 又是 stream 的唯一所有者，还可能在异步 resume 前释放 stream；那是 awaiter/stream 自身的保活问题，且仅在析构中补一行通知并不能解决。

**代码修改方案**：

1. **推荐的当前契约方案（小改）**：保持 `detach_from_endpoint()` 为唯一运行时清理边界；在 `~QuicConnection` 增加 `FIBER_ASSERT(peer_data_wait_head_ == nullptr)` 与 `FIBER_ASSERT(peer_data_wait_tail_ == nullptr)`，把 detach-before-destroy 明确为可执行不变量。不要在析构中无条件补 `notify_peer_data_waiters()`：`complete()` 会取消 awaiter timer 并异步 post resume，若析构发生在已 stop/join 的 loop 外线程，普通 `EventLoop::cancel()` 违反 loop-affinity；若 stream 随后的 table clear 是最后一份 lease，异步恢复前仍可能先释放 stream。
2. **若产品要求支持“连接被动销毁时仍有挂起 awaiter”（中改）**：让 `QuicStream::WriteAwaiter` 在成功 suspend 后持有一份 `QuicStream::Lease`，在 `await_resume`/析构完成 unlink、清 `write_waiter_` 后最后释放，确保 `notify -> post resume -> await_resume` 全程 stream 存活；`LocalStreamAttachAwaiter` 同理持有 `QuicConnection::Lease`。连接销毁仍限定在 owner loop，owner-loop 上可在 stream table 清理前通知 waiter；quiesced 的 off-loop 析构只允许 waiter 链表为空，不跨线程操作 timer heap。
3. **回归测试**：新增 endpoint 生命周期测试，建立“stream credit 有余、connection MAX_DATA 为 0”的写阻塞，使 `WriteAwaiter` 确实挂入 peer-data 链表；随后在 owner loop 调 `remove_connection()`/`close()`，断言写协程以 `Canceled` 恢复、destroy callback 只在最后一份 lease 释放后执行。另加 debug death/assert 测试覆盖绕过 detach 的非法析构；保活方案若实施，应在 ASan 下覆盖“detach 后立即释放 table lease、下一 tick 才 resume”。

> ✅ **不变量加固已实施 2026-07-15**。`~QuicConnection` 现在断言 peer-data waiter 的 head/tail 均为空；`notify_peer_data_waiters` 增加链表入口一致性与清空后置断言。新增 `QuicConnectionTest.AsyncWriteCanceledWhileBlockedByConnectionWindow` 和 `QuicUdpEndpointTest.DetachCancelsPeerDataWaiterBeforeDestroy`：前者验证 connection-level flow-control waiter 在连接关闭时以 `Canceled` 恢复，后者验证 endpoint detach 先取消 waiter、最后一份 connection lease 释放后才执行 destroy callback。定向 2 测试通过，完整 CTest **1179/1179** 通过。上述第 2 项 awaiter self-retain 属可选的 API 生命周期扩展，本次未实施。

## ⏸ DEFERRED — 架构性能优化

### 8. ⏸ 接收路径未实现零拷贝（当前架构必要复制，后续优化）
`QuicStreamRecvQueue.cpp:648-661`（`create_extent`）/ `QuicStream.cpp:519-522`（`on_stream_data_recv`）/ `QuicFrame.h:65-70`（`QuicSlice`）

`QuicSlice` 是裸 `{const uint8_t*, size_t}` 指向包 payload（`QuicTransportCodec.cpp:725-731` 经 `payload.read_slice` 设置），没 retain refcounted IoBuf。`create_extent` 只能 `IoBuf::allocate(kRecvBlockSize)`（16KB）+ `std::memcpy`。发送队列可以对调用方传入的持久 `IoBuf` 使用 `retain_slice`（`QuicStreamSendQueue.cpp:55,270`）；接收侧的来源是临时复用明文缓冲区，所有权模型不同。

> ⏸ **复核 2026-07-15**。上述复制确实存在，但在当前架构下是 correctness 所必需，并非独立缺陷：`QuicUdpEndpoint::plaintext_buffer_` 是 endpoint 级复用的 `unique_ptr<uint8_t[]>`，AEAD 解密结果和 `QuicSlice` 都只临时指向它；后续 coalesced packet 或下一 datagram 会覆盖该缓冲区，而 STREAM 数据可能长期留在乱序重组队列并由应用异步读取。即使数据 in-order，读取协程也不是在帧处理栈内同步消费，不能安全保留裸 view。因此本项不再作为 P0/HIGH correctness 问题，保留为后续架构级性能优化。
>
> “每个 STREAM 帧分配 16KB”也不是准确描述：首次触及一个逻辑 16KB stream block 时分配 16KB，同一活动 block 内的后续 extent 会复用该 storage；不过所有首次接收且非重复的字节仍会执行一次 `memcpy`。

**后续优化方向**：不能只把 frame 改为 `IoBuf` view 或直接调用 `retain_slice()`；必须先把 AEAD 明文目标改成可转移所有权、引用计数且可池化的 packet buffer，再让 decode result、frame 和 recv queue 贯穿持有其 slice，所有引用释放后整包缓冲区才可回池。设计时还需权衡小 STREAM frame 长期钉住整个 packet buffer、乱序 extent 数量以及缓冲池容量压力。

## 🔴 HIGH（续）

### 9. 无 GSO / sendmmsg / recvmmsg / GRO
`src/net/detail/DatagramFd.cpp:440,499`；`QuicSendScheduler.cpp:248`（`flush_connection` 逐包 `try_send_packet`）

每数据报一次 `recvmsg`/`sendmsg` 系统调用。nginx/quiche/cloudflare 都用 `sendmmsg`+GSO（`UDP_SEGMENT`）批量数十到数百 coalesced 数据报/系统调用，recv 用 `recvmmsg`/GRO。这是 QUIC 服务端最大的性能缺口。

**修复**：加 GSO 发送路径批量多连接数据报到一次 `sendmmsg`/GSO；recv 用 `recvmmsg`/GRO。

---

## 🟠 MEDIUM — 可靠性与拥塞控制（专项覆盖仍薄弱）

### 10. ✅ PTO 基于 non-ack-eliciting 包（复核为误报）
`QuicLossRecovery.cpp:213-238`

RFC 9002 §6.2.1 确实要求 PTO 基于在飞的 ack-eliciting 包，但原分析把
`packet_len == 0` 误当成了 non-ack-eliciting 标志。实际上，编码后同一包的所有帧都会复制
`encoded->ack_eliciting` 到 `packet_ack_eliciting`；`packet_len` 只记在该包的第一个帧上，用于避免
bytes-in-flight 重复计账，同一 ack-eliciting 包内的其余帧也会是 `packet_len == 0`。

> ✅ **复核 2026-07-15**。`QuicUdpEndpoint::commit_send_datagram` 只在
> `frame->packet_ack_eliciting` 为真时把帧放入 `sent_frames`；ACK-only 包的 ACK 帧走
> `release_frame` 分支。全部生产代码中也只有这一处向 `sent_frames` 插入帧，因此
> `sent_frames` 非空已隐含“该 packet number space 存在未确认的 ack-eliciting 包”；队尾
> `send_time` 也就是最近 ack-eliciting 包的发送时间。当前 PTO 计算符合 RFC 9002，
> 无需加 `in_flight > 0` 或扫描 `packet_len`。回归测试现直接验证 ACK-only 发送后
> `sent_frames.empty()` 且 `quic_loss_detection_timer` 返回 `None`。

### 11. ✅ 复核：loss 减窗基准用 `in_flight`（减后）而非 `window`
`QuicCongestion.cpp:166-172`（`quic_congestion_on_loss` 先 `subtract_in_flight` 再传 `cg.in_flight`）/ 对比 `:184`（ECN CE 路径正确用 `cg.window`）

`quic_congestion_on_loss` 先 `subtract_in_flight(cg, sample.packet_len)`，再把已减小的 `cg.in_flight` 作 `reduction_basis` 传 `enter_recovery`。因此 app-limited（`in_flight<window`）时会按较小的 flight size 减窗，可能比按 `window` 减窗更保守、拖慢恢复。ECN CE 路径则使用 `cg.window`。

> ✅ **复核 2026-07-15**。上述代码行为存在，但不构成协议错误。RFC 9438 §4.6 Figure 5 的主公式正是 `ssthresh = flight_size * beta_cubic`，并明确说明 rate/application-limited 时该公式可能把 `cwnd` 降得过低；规范建议可用 RFC 7661 的 congestion-window validation 改善。RFC 9438 也允许改用 `cwnd`，但要求同时采取措施，防止 bytes-in-flight 小于 `cwnd` 时窗口继续虚增。RFC 9002 中按 `congestion_window` 减窗的是其 NewReno 控制器示例，不能据此判定 CUBIC 的 flight-size 方案错误；nginx 当前 QUIC CUBIC 实现也同样先扣 `in_flight`，再按剩余值乘 `beta`。
>
> **结论**：不再作为确定缺陷或“一行修”追踪。保留为后续性能优化项：若要改善 application-limited 场景，应实现 RFC 7661 风格的窗口验证及专项测试，不能只把 `cg.in_flight` 改成 `cg.window`。

### 12. ✅ 已实现 pacing
`QuicPacer.cpp` / `QuicSendScheduler.cpp:230-314` / `QuicConnection.cpp:960-982,1106-1118`

原问题确认存在：旧 `flush_connection` 会紧循环到 `max_packets_per_connection`（默认 64）包或 socket 阻塞，连接内没有 pacing。RFC 9002 §7.7 建议发送方根据拥塞窗口和 RTT 对拥塞控制包进行 pacing。

> ✅ **修复 2026-07-15**。新增无动态分配的 byte token-bucket `QuicPacer`，默认速率为
> `5/4 * congestion_window / smoothed_rtt`，burst credit 默认上限为 10 个 path MTU，并按成功发送的
> UDP datagram 实际长度扣减预算；idle credit 有上限，亚字节 refill 余量跨检查保留。
>
> scheduler 在预算不足时只尝试发送 ACK/ACK_ECN/CONNECTION_CLOSE；若没有这类豁免帧，立即把连接
> 从 ready list 移除并在连接 owner EventLoop 上设置 pacing timer。定时器到期且仍有待发工作时重新
> `schedule_send`。ACK 或 CONNECTION_CLOSE 提前唤醒连接时会先取消旧 timer、重新核算预算，避免重复入队
> 与悬挂回调。
> ACK-only/close 不扣 pacing 预算，混有 ack-eliciting 数据或 PADDING 的 datagram 仍受 pacing 控制。
> pacing timer 已 armed 时，新的普通数据 frame 只进入 pending queue，不再重复唤醒 scheduler 或
> cancel/rearm timer；只有 ACK 或 CONNECTION_CLOSE 可以提前重新提交连接。
>
> 连接 close、detach、endpoint close、析构和 path congestion reset 均清理或重置 pacing 状态；同时把
> `max_packets_per_wakeup` 的计数从“每次 flush 调用 +1”修正为真实发送 packet 数。新增 7 个确定性 pacer
> 单测和 3 个 endpoint 集成测试，覆盖速率/容量/粒度/高频 refill、scheduler 移除与 timer 重入队、ACK/
> CONNECTION_CLOSE bypass、detach 前取消 timer。

### 13. ✅ `handle_ack_range` O(R×N)
`QuicAckHandler.cpp:180-259`

每个 ACK range 从 `sent_frames.front()` 重扫。R 个 range 按 PN 降序处理，`sent_frames` 升序，front 的小 PN 包被越过 R 次。原审计称最坏为 32 ranges，但 `kQuicMaxAckRanges` 只限制本端生成 ACK 时保存的 ranges；入站 `ACK Range Count` 是 varint，解析器未以 32 封顶，实际 R 仅受合法 packet 大小和包号范围约束，最坏复杂度确为 O(R×N)，且 `sent_frames` 按 frame 而非 packet 计数。

**修复**：range 按 PN 降序、list 升序，从上一 range 停止处续扫；或用有序/索引结构 O(log n)。

> ✅ **已修复 2026-07-15**。`QuicOutputFrameQueue` 增加侵入式 `prev` 链并在 `push_front`/`push_back`/`insert_after`/`erase_after`/`prepend_all` 中统一维护；`quic_handle_ack_frame` 从 `sent_frames.back()` 建立反向 scan cursor，各 ACK range 复用上一 range 的停止位置。查找 range 时向低 PN 移动，range 内仍按原来的低 PN 到高 PN 顺序处理，避免改变拥塞更新和 stream ACK 回调顺序。所有 gap frame 最多被反向越过一次，acked frame 仅多一次反向定位和一次正向处理，总复杂度降为 O(N+R)，稳态无动态分配。回归测试 `QuicOutputFrameQueueTest.MaintainsReverseLinksAcrossMutations` 覆盖双向链不变量，`QuicAckHandlerTest.HandlesDescendingAckRangesWithSingleReverseScan` 覆盖交错 singleton ranges 后保留队列的顺序与反向链。

### 14. ⚪ 复核排除：persistent congestion 区间判定无方向误判
`QuicLossRecovery.cpp:193-199`

> ⚪ **复核 2026-07-15**。RFC 9002 §7.6.2 要求两个 ack-eliciting lost 包的发送时间之间没有任何已确认包，并未要求 lost 区间必须位于 largest newly acknowledged packet 之前。当前条件实际表达的是“本次 newly-acked 时间区间与 newly-lost 时间区间完全不相交”：`stat->newest < oldest_lost` 表示 ACK 全在 loss 之前，`stat->oldest > newest_lost` 表示 ACK 全在 loss 之后，两者都没有 ACK 落在 lost 窗口内，均可合法触发 persistent congestion。
>
> 第一个分支在跨 packet-number space 检测中尤其必要：`quic_detect_lost` 遍历 Initial、Handshake 和 Application 三个空间，而 `stat` 只来自当前 ACK 所属空间；当前 ACK 所确认包的发送时间完全早于另一空间的 lost 区间是可达且合法的。nginx 上游也使用相同的两个分支。删除第一分支会造成漏报；改为 `stat->oldest > oldest_lost` 则会在 ACK 位于 lost 区间内时误报。
>
> 当前实现仍是 nginx 式近似：`QuicLossAckStat` 只保存当前 ACK 的最早/最晚发送时间，且旧 ACK 历史会随 sent frame 释放而丢失，无法严格重建跨空间的完整 ACK/loss 时间序列。这可能导致更宽泛的漏报或误报，但不能通过原建议的一行修改解决；若要严格对齐 RFC，需单独设计固定内存的跨空间状态跟踪，按 P2/Medium 后续项处理。
>
> 回归测试：`QuicLossRecoveryTest.AckedIntervalBeforeLostIntervalCanEstablishPersistentCongestion`、`AckedIntervalAfterLostIntervalCanEstablishPersistentCongestion` 和 `AckedPacketInsideLostIntervalPreventsPersistentCongestion` 分别覆盖 ACK 全在 loss 之前、全在 loss 之后和落在 loss 区间内的三种边界。

### 15. ✅ `drop_ack_ranges` 用发送 PN 阈值接收 PN range
`QuicAckHandler.cpp:225-229`

收到的 ACK 帧被 ack 时，`drop_ack_ranges(frame->packet_number)` 用发送包 PN 调，但 `drop_ack_ranges` 阈值 `largest_range`/`ack_ranges` 跟踪的是**接收** PN。发送/接收 PN 是独立序列，比较无意义。发送 PN 超前接收 PN（数据密集服务端常见）时，会过早丢弃该 ACK 发出后新收到的 range，造成漏 ACK、对端虚假判丢、无谓重传与减窗；发送 PN 落后时则删除不足，产生冗余 ACK。

**修复**：存并传该 ACK 帧覆盖的最大接收 PN（`frame->u.ack.largest`）而非 `frame->packet_number`。

> ✅ **已修复 2026-07-15**。ACK-of-ACK 处理改为将已发送 ACK/ACK_ECN 帧保存的 `u.ack.largest` 传给 `drop_ack_ranges`，并将其参数重命名为 `largest_acknowledged`、修正范围方向注释。修复不增加成员、分配或热路径分支。回归测试 `QuicAckHandlerTest.AckOfAckDropsRangesThroughSentAckLargest` 构造本地发送 PN 100 承载 `largest=10` 的 ACK、随后收到对端 PN 11，再确认本地 PN 100，验证仅丢弃到 10 且 PN 11 的 range 与 `pending_ack` 均保留。

### 16. ⚪ 复核排除：`ack_delay` clamp 符合 RFC；相邻等号边界已修复
`QuicCongestion.cpp:77-79`

> ⚪ **复核 2026-07-15**。原结论对 RFC 9002 的解读相反：§5.3 明确要求握手确认后 **MUST use the lesser of** 对端报告的 `ack_delay` 与 `max_ack_delay`，附录 A.7 的 `UpdateRtt` 伪代码也先执行 `ack_delay = min(ack_delay, max_ack_delay)`，再在调整结果不低于 `min_rtt` 时扣减。原文引用的 A.1 是 sent-packet 状态跟踪，并非 RTT 更新。因此当前 clamp 是正确行为，改成超限时完全 skip 会高估 `smoothed_rtt`、`rttvar` 和 PTO，不应实施。
>
> 复核时发现相邻但独立的精确边界问题：RFC 的条件是 `latest_rtt >= min_rtt + ack_delay`，原实现使用严格 `<`，在 `latest_rtt == min_rtt + ack_delay` 时不会扣减，虽然扣减结果恰好等于 `min_rtt`，属于规范允许的边界。现改为等价且避免加法溢出的 `ack_delay <= latest_rtt - min_rtt`。
>
> 回归测试：`QuicCongestionTest.RttSampleClampsAckDelayAfterHandshakeConfirmation` 固定超限 ACK delay 仍按 `max_ack_delay` 扣减；`QuicCongestionTest.RttSampleSubtractsAckDelayAtMinRttBoundary` 覆盖调整结果恰好等于 `min_rtt` 的等号边界。

### 17. ⚪ 复核排除：idle ACK 不会进入 CUBIC window，路径重置已显式处理
`QuicCongestion.cpp:115-137,195-206` / `QuicConnection.cpp:2181-2185`

> ⚪ **复核 2026-07-15**。原结论遗漏了 `quic_congestion_on_ack` 的控制流：完成
> `in_flight` 扣减后，函数在 `sample.send_time <= recovery_start || cg.idle` 时直接返回；只有
> `cg.idle == false` 且已进入 congestion avoidance 的 ACK 才会调用
> `quic_congestion_cubic_window`。因此 ACK 路径不可能在 `cg.idle == true` 时经
> `quic_congestion_on_idle(cg, cg.idle, now)` 平移 `k`，所述 spurious epoch 偏移不可达。
>
> `QuicUdpEndpoint::commit_send_datagram` 根据是否仍有发送工作调用
> `quic_congestion_on_idle`。从 application-limited 状态恢复且仍有连续发送工作时，外部调用以
> `idle=false` 将 `k` 一次性平移实际 idle 时长；若发送后再次立即耗尽工作，状态保持 idle，随后 ACK
> 不增长拥塞窗口。这符合 RFC 9438 §4.2/§5.8：CUBIC 时间 `t` 不得包含 application-limited
> 期间，且 application-limited 流不增长 `cwnd`。在 ACK 前无条件清 `cg.idle` 反而会破坏该行为。
>
> `now < recovery_start` 分支在本项目的 signed `std::chrono::milliseconds` 单调时钟模型下确实是
> 防御性死分支，但没有证据表明它掩盖路径重置问题：普通 recovery 将 `recovery_start` 设为当前
> `now`，persistent congestion 将其设为最旧在途包时间之前，路径重置则经
> `QuicConnection::reset_congestion_for_path` 调用 `quic_congestion_reset_for_path`，明确将其设为
> reset `now - 1ms`，同时用 `reset_packet_number_` 排除重置前的旧包。该 fixup 源自 nginx 对无符号
> `ngx_msec_t` 回绕的防御，在当前类型下冗余但无害，不值得单独修改。
>
> **结论**：不实施“ACK 前清 `cg.idle`”，不单独修改 `recovery_start` 防御分支，本项不作为缺陷追踪。

---

## 🟠 MEDIUM — Codec

### 18. ✅ 强保护包双解析已消除；Initial 有意保留预校验
`QuicPacketCodec.cpp:423-431` + `QuicPacketProcessor.cpp:323-337`

原性能现象确认存在：`quic_decode_packet` 原先遍历完整 decrypted payload，随后
`process_decoded_packet` 再解析并分发相同字节；`decoded.frame_count`/`decoded.ack_eliciting`
在生产路径无人消费。不过第一次遍历并非纯死代码：它还保证在尾部坏帧时，前置帧尚未产生副作用，
并恢复解密时预先更新的 `largest_received_packet_number`。

> ✅ **修复 2026-07-15**。删除 `QuicPacketDecodeResult::frame_count`/`ack_eliciting`，Handshake、
> 0-RTT 和 1-RTT 的 decode 阶段不再解析帧，只在 `process_decoded_packet` 中解析并提交一次。
> 强保护包遇到未知/坏编码帧时以 `FRAME_ENCODING_ERROR` 关闭；已知帧出现在禁止的加密级别或
> 接收方向时以 `PROTOCOL_VIOLATION` 关闭，并在错误路径重新读取一次 frame type 填入
> CONNECTION_CLOSE，不给正常热路径增加分支成本之外的额外解析。
>
> Initial 仍在 AEAD 成功后、TLS 初始化和任何帧副作用之前完整预校验。这是有意保留：Initial
> 保护不认证发送者，RFC 9000 允许静默丢弃无效 Initial 的前提是未处理其中帧或完整回滚；保留
> 只读预校验还避免 AEAD 合法但帧格式错误的伪造 Initial 触发 `SSL_new`。因此稳态强保护流量从
> 两次帧解析降为一次，握手 Initial 的双遍历属于安全边界，不再追求用帧数组消除（最坏 64KB
> payload 可含数万单字节帧，缓存会引入大块栈内存或动态分配）。Closing/Draining 的强保护包
> 也不再在 decode 阶段扫描 payload。
>
> 回归测试覆盖：坏尾帧 Initial 在 PN/TLS/连接状态提交前被拒；1-RTT 未知帧映射
> `FRAME_ENCODING_ERROR`；方向非法的 HANDSHAKE_DONE 映射 `PROTOCOL_VIOLATION`；坏尾帧
> key update 不翻转密钥；Closing 连接跳过坏 payload 的帧分发。

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
- **测试缺口**：`QuicAckHandler` 已补交错 range 回归，但与 `QuicLossRecovery`/`QuicSendScheduler`/`QuicPathManager` 的专项覆盖仍不充分，正是可靠性 bug 最易藏处。建议继续优先补。

---

## 修复优先级

| 优先级 | 项 | 工作量 |
|---|---|---|
| P0 | #2 PTO 忙循环✅、#3 re-entrancy✅、#4 create-before-auth✅(方案A)、#5 DoS 向量、#1 key-update | 中 |
| P0 | #9 GSO/sendmmsg、#8 接收零拷贝 | 大 |
| P1 | #11 loss 减窗基准、#22 `reset()` FIN'd 流、#23 `write()` 短写丢 FIN、#19 重复 TP 拒绝、#27 HKDF 溢出、#16 ack_delay skip | 小（多为一行） |
| P1 | #13✅/#14⚪/#15✅；继续给 AckHandler/LossRecovery/SendScheduler/PathManager 补单测 | 中 |
| P2 | #7 detach-before-destroy 不变量断言✅；若要支持挂起 awaiter 下被动销毁，再做 awaiter self-retain | 小 / 中 |
| P2 | #14 可选的严格跨空间 persistent-congestion 状态跟踪 | 中 |
| P2 | #35 拆 QuicConnection god object、#31 拆 QuicUdpEndpoint、#25 去重 reassembly、#44 去重 awaiter | 大 |
