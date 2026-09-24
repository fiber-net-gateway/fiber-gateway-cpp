# TLS 自研实现 · 10 QUIC-TLS 换芯(QUIC 握手接自研引擎;去 BoringSSL ssl 库依赖)

## 1. 范围与定谳(2026-09-24)

范围 = `src/quic/QuicTlsSession.cpp`(508 行,QUIC 里唯一的 BoringSSL **ssl 库**
消费者)从 `SSL_QUIC_METHOD` 驱动换到 06/07/08 引擎的 **QUIC 模式**;`fiber_lib`
链接面从 `ssl+crypto` 收窄为**仅 crypto**(CMakeLists.txt:54)。09 §1 "QUIC 不动"
至此翻案——当时列的三笔账(传输模式解耦、QUIC 密钥派生、客户端 0-RTT)本篇全数
偿还。

不动:`src/quic/QuicCrypto.*`(643 行)——纯 crypto 原语,本就引擎无关:
secret→`quic key/iv/hp` 派生、key phase(`"quic ku"`,QuicCrypto.cpp:29)、retry
integrity tag、Initial secrets 全在本地。key update 的 QUIC 侧机制
(`apply_peer_key_update` + `quic_derive_next_key_pair` + key_phase bit 检测)
不动。X509/EVP 密码学原语层(02b/05 底座)不动。

定谳(设计内已决):

1. **QUIC 模式形态 = 传输回调集**。`TlsQuicCallbacks`(裸函数指针,无
   std::function/虚函数),挂 `TlsClientConfig`/`TlsServerConfig` 新增指针字段;
   null = 现 TCP 行为(引擎全路径零分支成本不变)。与 BoringSSL
   `SSL_QUIC_METHOD` 同构(set_secret/add_handshake_data/send_alert),
   `QuicTlsSession` 移植面最小。`flush_flight` 不要——引擎是 take_output 模型,
   flight 边界天然由一次 drive 界定。
2. **QUIC 模式不变量**:握手记录恒明文(不装记录 cipher、不发 compat CCS、
   fatal alert 不编码进出站——alert 走 `failure_alert()` → QUIC 层
   `close_crypto_error` = 0x0100|alert 的 CONNECTION_CLOSE,现映射不变);握手
   完成即散(**不构 `TlsConnectedState`**,QUIC 层接管 app 数据;`take_state()`
   在 QUIC 模式断言不可达)。
3. **密钥导出点 = 引擎 FSM 里"构造记录 cipher"的位置改调回调**(§4)。early/
   hs_c/hs_s/ap_c/ap_s 五个点;EarlyData 仅 client+0-RTT。接缝正好对上现有
   `quic_set_encryption_secret(state, level, write, suite, secret, secret_len)`
   (QuicTlsSession.cpp:78 的 set_secret 原样平移)。
4. **KeyUpdate 消息在 QUIC 被禁**(2026-09-24 实证定谳):RFC 9001 的 key
   update 只走包层 key_phase bit + `"quic ku"` 派生,不用 TLS KeyUpdate 消息。
   BoringSSL 双向实证:发起侧 `SSL_key_update` 在 QUIC 模式直接报错拒绝
   (ssl_lib.cc:997 `SSL_is_quic` → `ERR_R_SHOULD_NOT_HAVE_BEEN_CALLED`);接收侧
   分发层对 QUIC 连线上的 KeyUpdate 一律 fatal unexpected_message
   (tls13_both.cc:714-719)。新实现同语义:QUIC 模式 post-handshake 消费器对
   KeyUpdate → close 0x0100|unexpected_message。QUIC 层现有
   `apply_peer_key_update`(key_phase bit 触发)不动——那就是"对端发起 key
   update"的全部语义,无需 TLS 层转译;主动发起(临界限前翻 bit)同样纯包层,
   RFC 合规(见 §12.1 撤案记录)。
5. **0-RTT 客户端不走引擎 `write_early_data`**:QUIC 的 early data 是 STREAM 帧,
   不是 TLS 记录;只要 `client_early_traffic_secret` 导出。~~EndOfEarlyData 仍由
   引擎在 Handshake 级 CRYPTO 发出~~(§13 勘误:RFC 9001 §8.3 整个移除该消息,
   发出端不发、接收端 fatal、transcript 亦不含——见 §13 实施记录)。BoringSSL 的
   `SSL_ERROR_EARLY_DATA_REJECTED`+`SSL_reset_early_data_reject` 自动重驱分支
   (QuicTlsSession.cpp:440-447)整体消失——引擎本就同时发 CH+0-RTT,SH 拒了
   自然走 1-RTT 路径,QUIC 层只收 `on_early_data_rejected` 通知丢早数据。与
   "拒绝自动回滚"的既有定谳(两段式 connect)更贴合。
6. **一刀切,无双栈开关**(同 09 定谳 6)。对驱靠 tests 里的 BoringSSL peer,
   不靠生产双栈。

待拍板(§12 详述,各带推荐):

- **A. ssl 库是否连 tests 都删**:推荐**保留 test-only**(`fiber_lib`/apps 不链,
  仅 `fiber_tests`)——那是目前最有价值的 oracle,ngtcp2/curl 只覆盖黑盒外部路径。
- **B. 服务端 0-RTT anti-replay 策略**:07 §10.6 "anti-replay 不存在就不开
  early data" 在 QUIC 绕不过去(0-RTT 是 QUIC 主打能力)。推荐维持 08 现状:
  stateless ticket 无全局防重放,开门责任在配置方,文档明示。
- ~~**C. key update 发起是否补发 TLS KeyUpdate 消息**~~:**作废**。RFC 9001 禁
  该消息于 QUIC,BoringSSL 发起拒调/接收 fatal 双向封死(定谳 4 的实证);不发
  即合规,补发反而违规。

## 2. 接缝盘点(2026-09-24 实查)

| 层 | 事实 | 本篇动作 |
|---|---|---|
| `src/quic/QuicCrypto.*` | 引擎无关;`quic_set_encryption_secret` 是 secret 入口;current/next/previous 槽 + promote | 零改动 |
| `QuicTlsSession.cpp` | ssl 唯一 src/quic 消费者;对 `QuicConnection` 契约面:init_server/init_client/provide_crypto_data/drive_handshake/process_post_handshake/handshake_done/session_reused/selected_alpn/peer_verify_result/alert stash | 重写(§8) |
| key update 现状 | QUIC 层本地管(key_phase bit + `quic ku`);`quic_packet_keys(Application)` 直写 **current** 槽(QuicCrypto.cpp) | 语义不变,零改动(定谳 4) |
| 公共头 SSL_SESSION 泄漏 | QuicConnection.h:34/186/494/875、QuicClientConnect.h:12/37/48、QuicTlsSession.h:19/34 | 去 SSL_SESSION 化(§9) |
| QUIC 服务端装配 | `QuicUdpEndpoint.h:99/120` 持 `const net::TlsServerParam *tls` → QuicConnection.cpp:2519 `init_server` | 换引擎侧装配(§8) |
| net 残留 ssl | `TlsSslFactory.*`/`TlsRuntime.*`(09 后仅 QUIC+tests 消费)、`TlsCredential.cpp`(X509 属 crypto,ssl.h include 可降) | 删/降级(§10) |
| 套件域 | 引擎 registry 含 0x1301/0x1302/0x1303(TlsCipherSuites.h:13-15),QUIC 三件套全覆盖 | 映射表即可 |

## 3. 引擎 QUIC 模式(核心新件,slice 1 主体)

### 3.1 回调集

```cpp
// include/fiber/tls/TlsConfig.h 增(两 config 各挂一个指针,null = TCP 模式)
struct TlsQuicCallbacks {
    // 密钥导出:引擎在各派生点调用;secret span 短暂有效(派生 scratch),
    // 调用方即拷即用。返回 false = 安装失败(引擎转 Fatal)。
    bool (*set_secret)(void *ctx, TlsQuicSecretLevel level, bool write_secret,
                       TlsCipherSuiteId suite, std::span<const std::uint8_t> secret) noexcept;
    // 出站握手字节按级别分发(引擎在 emit 前设定级别;QUIC 层组 CRYPTO 帧)。
    bool (*add_handshake_data)(void *ctx, TlsQuicSecretLevel level,
                               std::span<const std::uint8_t> data) noexcept;
    // 对端 0x39(quic_transport_parameters):CH(server 侧)/EE(client 侧)解出即调,一次。
    void (*on_peer_transport_params)(void *ctx, std::span<const std::uint8_t> params) noexcept;
    // fatal alert 报告(不出站记录;QUIC 层转 CONNECTION_CLOSE 0x0100|desc)。
    void (*send_alert)(void *ctx, TlsAlertDesc desc) noexcept;
    void *ctx = nullptr;
};
// TlsQuicSecretLevel ∈ {EarlyData, Handshake, Application}(Initial 不经 TLS)
```

### 3.2 出站

- 引擎 emit 路径(TlsHandshakeContext::emit,TlsHandshakeContext.h:108)在 QUIC
  模式改走 `add_handshake_data(level, …)` sink,不落 out_。
- flight→level 映射(引擎 FSM 已知消息类型,emit 时定级):
  client:CH→Handshake 级 CRYPTO 上用 **Initial 包保护**发送——注意:TLS 层级别
  只有三档,QUIC 的"CH 在 Initial 包里"是**包层**概念;映射为 client
  {CH→Initial, Finished→Handshake}(EoED 不存在,§13),server {**SH→Initial**,
  EE..Finished→Handshake, NST→Application}(SH 走 Initial——§13 定谳 7)。
  回调用 QuicEncryptionLevel 语义(四档含 Initial),CH 期
  导出级别即 Initial——与 BoringSSL `ssl_encryption_initial` 对应关系原样平移
  (quic_level_from_ssl,QuicTlsSession.cpp:29)。
- compat CCS 抑制(QUIC 禁止;client 首飞与 HRR 重启两处)。alert 不编码出站。

### 3.3 入站

- `provide(level, span)`:语义同 `SSL_provide_quic_data`——glue 按
  QuicHandshakeGate 的流门控喂当前级字节,越级字节由 QUIC 层缓冲(现契约,
  引擎按序消费)。
- 明文常驻:引擎停在 `TlsInboundMode::Plaintext13` 不换 Sealed
  (TlsHandshakeContext.h:50);QUIC 模式下 cipher 安装点全部改调 set_secret。
- DOs 界放宽:`kMaxPlaintextHandshakeRecords13 = 4`(TlsHandshakeContext.h:73)
  是 TCP 飞行界;QUIC CRYPTO 流是流式重组,CH 可任意分片——QUIC 模式改总字节
  界(4 MiB 消息界保留)。

### 3.4 版本域

QUIC 恒 TLS 1.3:glue 设 `min_version = max_version = 1.3`(09 §4.2 的版本界
复用);1.2 子流在 QUIC 模式不可达(断言)。

## 4. 密钥导出点(引擎 FSM 位点)

| 秘密 | client 引擎 | server 引擎 | 现状对应 |
|---|---|---|---|
| client_early_traffic_secret | 0-RTT offer 发出后(SH 前即可导给 QUIC 发 Initial 包后的 0-RTT) | ~~不导(收方用 skip 窗)~~(§13 勘误:**accept 即导出读侧**——QUIC 没有 skip 窗,0-RTT 包由 QUIC 层解;拒绝则不导) | `ssl_encryption_early_data` |
| client_hs_traffic_secret | SH 解出后 | SH 构造后、EE 前(SH 本体走 Initial CRYPTO——§13 定谳 7) | set_secret(write=client 侧) |
| server_hs_traffic_secret | SH 解出后 | SH 构造后、EE 前(同上) | 同上 |
| client_ap_traffic_secret | 服务器 Finished 验证后 | client Finished 验证后 | `ssl_encryption_application` |
| server_ap_traffic_secret | 同上 | 同上 | 同上 |

实施口径:两引擎在"由 secret 构造 `TlsRecordCipher`"的既有位点分叉——TCP 模式
原样,QUIC 模式调回调后**跳过 cipher 构造**(不持有记录密钥)。QUIC 层拿到
(level, write, suite, secret) 后走现成 `quic_set_encryption_secret` →
`quic key/iv/hp` 派生。握手完成后 QUIC 模式**没有后续 set_secret 调用**
(KeyUpdate 归 §7 的 post-handshake 消费器,定谳 4)。

App 秘钥即丢弃时限:QUIC 层现有 `discard_level(EarlyData)` 时机
(QuicTlsSession.cpp:96-100,app 双向 ready 且 client)原样平移;握手级槽丢弃
现有机制不动。

## 5. 0x39(quic_transport_parameters)扩展

- **注入**:`TlsClientConfig`/`TlsServerConfig` 各加
  `std::span<const std::uint8_t> quic_transport_params`(借用,glue staging 存活);
  引擎编码 CH/EE 时携带(TlsExtensionCodec 增 0x39 编码,纯字节透传)。
- **提取**:CH 侧——`TlsClientHello.extensions_block`
  (TlsHandshakeMessage.h:64)本就可重扫,但取整洁:解码 CH 时顺手解 0x39 →
  `on_peer_transport_params`;EE 侧——client 引擎解 EE 时同样回调。QUIC 层存下,
  `drive_handshake` 循环里应用(保持现契约:握手完成时必须已收到并应用,否则
  close TransportParameterError,QuicTlsSession.cpp:401-414 的时机语义)。
- 不做通用扩展 registry(QUIC 是唯一消费者,YAGNI;要加再说)。

## 6. 0-RTT 与恢复

### 6.1 服务端

- **early-data veto**:`TlsResumptionLookup::lookup` 签名增
  `std::span<const std::uint8_t> quic_transport_params`(空 = 非 QUIC/TCP 面,
  现有调用方传空即可;08 的测试表小改)。QUIC 层的 lookup 实现者在此比对
  **remembered transport params**(存票时已随 `QuicClientCacheOps` 带上)与 CH
  的 0x39——BoringSSL `SSL_set_quic_early_data_context`(QuicTlsSession.cpp:325)
  的参数一致性门由 QUIC 层自办,引擎只供货。ALPN 一致性已有
  (`TlsResumedSession.alpn`)。
- anti-replay:见待拍板 B(推荐维持 08 现状,配置方开门)。
- 票据服务:`TlsTicketService`(09 slice3 装配链)直用;`TlsTicketRequest`
  的 resumption_master 在 QUIC 模式仍由引擎派生(握手尾),minter 接线同 TCP。

### 6.2 客户端

- **NST 收据**(§7 消费器产出)→ 新 owning 状态:

```cpp
// include/fiber/tls/TlsConfig.h 增(owning;TlsSessionOffer 是它的借用投影)
struct TlsSessionState {
    std::vector<std::uint8_t> identity;  // 票据
    TlsSecret psk;                        // resumption_master+nonce 派生(引擎出)
    TlsCipherSuiteId suite;               // 票据套件
    std::uint32_t ticket_age_add = 0;
    std::uint32_t max_early_data = 0;
    std::array<std::uint8_t, 256> alpn{}; std::uint16_t alpn_len = 0;
    std::int64_t issued_ms = 0;
    std::uint32_t obfuscated_ticket_age() const noexcept; // (now-issued)+age_add mod 2^32
};
```

vector 一处破例(低频路径,票据收据非热路径;`IoBuf` 不适合长生命周期缓存值)。
09 §1 "客户端不缓存 NST" 的定谳就此收窄为"**引擎不缓存**,QUIC 层缓存"——
`TlsConnection` 的吞 NST 行为不变(TCP 面无恢复需求,维持)。
- offer 装配:glue 由 `TlsSessionState` 现场拼 `TlsSessionOffer`(borrowed),
  `obfuscated_ticket_age` 用 helper(08 注释说"08 算",08 没做客户端——本篇补)。
- 拒绝语义:见定谳 5;`on_early_data_rejected`/两段式 connect 契约不动。

## 7. QUIC 模式 post-handshake 消费器(归属 QuicTlsSession)

握手完成后 app 级 CRYPTO 流仍可能到 TLS 消息(NST、KeyUpdate、对端
CertificateRequest 等)。BoringSSL 用 `SSL_process_quic_post_handshake`;新实现为
`QuicTlsSession` 内的小件:明文记录读取(TlsRecordReader)+ 重组 + 分发:

- **NST**(client):解 `TlsTicketRequest` 同源字段 + `resumption_master` 引擎
  派生 PSK → `TlsSessionState` → QUIC 层 store_session(替换
  `store_new_client_session`/`SSL_SESSION` 转移语义,QuicTlsSession.cpp:24)。
  注:resumption_master 必须在**握手完成时**由引擎交出(QUIC 模式握手完即散,
  定谳 2——修正:QUIC 模式的握手尾**交付** resumption_master 给 glue,不构
  ConnectedState、不构 cipher;一个 span/值交付点)。
- **KeyUpdate**:fatal unexpected_message(RFC 9001 禁该消息于 QUIC;BoringSSL
  tls13_both.cc:714 同拒)。对端发起的 key update 只从包层 key_phase bit 进来,
  走 QUIC 层 `apply_peer_key_update`,与 TLS 层无关(update_requested 被动响应是
  TCP `TlsConnection` 的机制,QUIC 无此路径)。
- 其他 → fatal unexpected_message → close 0x0100|alert。

## 8. QuicTlsSession 重写与装配迁移(slice 2 主体)

契约面(init/provide/drive/post_handshake/alpn/alert stash)不变,实现映射:

| 现 BoringSSL 调用 | 新引擎 API |
|---|---|
| `SSL_set_quic_method` | `TlsQuicCallbacks` 装配(config 字段) |
| `SSL_provide_quic_data` | engine.feed(QUIC 层 provide 语义包装) |
| `SSL_do_handshake` + WANT_READ/WRITE | 构造(client 首飞)/feed → `Event{None,Done,Failed}`;None=WouldBlock |
| `SSL_is_init_finished` | `engine.done()` |
| `SSL_get0_alpn_selected` | ConnectedState.alpn 同源(QUIC 模式:引擎 alpn 查询——握手完成时从引擎取,交付点同 resumption_master) |
| `SSL_get_verify_result` | 引擎 verify 失败即 Failed(Permission),无 long 码 |
| `SSL_session_reused` | `TlsConnectedState.session_resumed` 同源(交付点) |
| `SSL_early_data_accepted` | 同上(early_data_accepted) |
| `SSL_set_session` | `TlsSessionOffer` 装配(§6.2) |
| alert stash + `close_crypto_error` | `failure_alert()` → 现映射不变 |

**握手完成交付点**:QUIC 模式握手尾一次性交出
{alpn, session_resumed, early_data_accepted, verify 结果(失败早退),
resumption_master}——一个 `TlsQuicHandshakeResult` 结构,不构 ConnectedState。

- **服务端装配**:`init_server(net::TlsServerParam &)` → staged
  `tls::TlsServerConfig` + selector(09 §4.1 的 `TlsServerConfigSource` 形态,
  net 的 configure_callback 经 shim 填 staged config,同 TlsStreamFd.cpp:312 的
  现成模式)+ `TlsTicketService`(09 slice3 装配链复用,`TlsServerParam` 的
  ticket service 指针字段已是 09 增量)。`QuicUdpEndpoint.h:99/120` 的
  `const net::TlsServerParam *tls` 类型不动(net 公共 API 09 定谳 4 仍守)——
  QUIC 是 net 参数的**内部消费者**,装配翻译在 QuicTlsSession 内完成。
- **客户端装配**:`init_client(net::TlsClientParam &, allow_insecure,
  SSL_SESSION *)` → `tls::TlsClientConfig`(09 §5 的映射直用:
  security/credential/trust/server_name/verify_name/alpn + now_unix_ms =
  `EventLoop::current().now()`)+ `TlsSessionState *`(可为 null,全握手)。
- 传输参数:create_*_transport_params 两函数(QuicTlsSession.cpp:180/237)不动,
  产物从 `SSL_set_quic_transport_params` 改填 config 的 0x39 span。
- `maybe_apply_peer_transport_params` 的"完成时必须已应用"校验逻辑保留,数据源
  改 `on_peer_transport_params` 存下的副本。

## 9. 公共 API 去 SSL_SESSION 化(slice 2 内)

- `QuicConnection.h:34/186`(resumption_session)/`:494`(on_new_tls_session 回调
  签名)/`:875`、`QuicClientConnect.h:12/37/48`、`QuicTlsSession.h:19/34`:
  `SSL_SESSION *` 全部换 `tls::TlsSessionState`(`QuicClientCachedState.session`
  变 owning 值;store 回调的"引用转移"注释改"move 转移")。
- 消费者:lite_nginx H3 client(apps)、tests/QuicClientTest.cpp(ssl.h include
  随之消失)。`on_new_tls_session` 的连接侧缓存逻辑
  (QuicConnection.cpp:2320)改存 TlsSessionState。
- 注意:h3-client-teardown-uaf 既有缺陷(h3-client-teardown-uaf-pending)不在
  本篇修,session 缓存 API 改形时保持其生命周期形态不变,避免混淆归因。

## 10. 构建与依赖收窄(slice 4)

- CMakeLists.txt:54 → `target_link_libraries(fiber_lib PUBLIC boringssl::crypto)`;
  `fiber_tests` 增链 `boringssl::ssl`(待拍板 A 通过则不链,并删除对驱用例,
  改依赖外部矩阵)。
- **防回归**:`scripts/` 或 CI 一条 grep——`src/`、`include/` 出现
  `#include <openssl/ssl.h>` 即 fail(tests/apps 豁免或按 A 结论)。
- `src/net/detail/TlsSslFactory.*`、`TlsRuntime.*` 删除(消费者仅剩 QUIC+tests);
  `TlsCredential.cpp` 的 `openssl/ssl.h` include 降为 `openssl/x509.h`(crypto)。
- Deps.cmake 不动(boringssl 整体照 fetch,ssl 目标照 build——链接面收窄而已,
  测试 peer 仍需)。

## 11. slice 划分与测试

| slice | 内容 | 验收 |
|---|---|---|
| 1 引擎 QUIC 模式 | TlsQuicCallbacks + 明文常驻 + 导出点五处 + 0x39 注入/提取 + CCS 抑制 + DOs 放宽 + 握手尾交付结构 | 引擎级:RFC 9001 附录 A 密钥推导 KAT(secret 级向量);BoringSSL 内存对驱(见下) |
| 2 QUIC 侧换芯 | QuicTlsSession 重写 + 服务端/客户端装配 + post-handshake 消费器(先 NST 通路留 slice 3)+ §9 API 改形 | lite_nginx H3 全量;curl(ngtcp2,127.0.0.1 坑在册)+ nginx echo 9001/9002 外部回归;retry/address validation;24xx 全量绿 |
| 3 0-RTT/恢复 | TlsSessionState + obfuscated_ticket_age + lookup veto(remembered params 比对)+ NST 收据 + 拒绝语义复验 | 0-RTT 接受/拒绝/重连矩阵;remembered params 不一致 → 拒 0-RTT 降 1-RTT;session 跨连接复用 |
| 4 ssl 收窄 | CMake 改链 + 工厂/Runtime 删除 + 防回归 grep + 全量 | 全量(24xx)+ ASan(独立 FIBER_DEPS_DIR);fiber_lib 无 ssl 符号(nm 验证) |

对驱基建(复制 06-09 的 9/9 模式到 QUIC 层):tests 内以 BoringSSL
`SSL_set_quic_method` 起对端,自研 QUIC 模式引擎 ↔ BoringSSL 双向互驱(client↔
server 各向);key update 对驱在**包层**驱动(对端翻 key_phase bit 发包,断言双方
promote/试解/再派生正确;另加"对端发 TLS KeyUpdate 消息 → 关闭"用例锁定拒收
语义)——TLS 层 KeyUpdate 消息被禁,`SSL_key_update` 在 BoringSSL QUIC 模式
不可用(定谳 4)。

## 12. 风险与开放问题

1. **KeyUpdate 暗礁——证伪撤案(2026-09-24)**:此前推测"对端发 TLS KeyUpdate
   会经 `SSL_process_quic_post_handshake` 触发 set_secret(Application) 直写
   current 槽,与 QUIC 层 epoch 记账(generation/phase/current_read_lowest_pn)
   失同步"。实证 BoringSSL 分发层(tls13_both.cc:714-719)对 QUIC 连线上的
   KeyUpdate 一律 fatal unexpected_message,根本到不了 set_secret——暗礁不存在,
   无需修。保留两条:(a) 新实现的 post-handshake 消费器必须同样拒收(定谳 4);
   (b) key update 语义全在包层(key_phase bit),主动发起的动机与 RFC 依据 =
   RFC 9001 §6.6"An endpoint MUST initiate a key update prior to exceeding any
   limit set for the AEAD"(AES-GCM 机密性上限 2^23 包,Appendix B 推导)。
2. **待拍板 A/B**(§1):A 影响 slice 4 的 tests 链接与对驱去留;B 是产品
   决策(0-RTT 服务端默认开不开)。
3. **flight 跨级别**:~~一次 drive 内出站级别理论恒定(CH 期 Initial;EoED+Fin 同
   Handshake)~~(§13 勘误:服务端单次 drive 实际跨两档——SH@Initial 先出、随后
   EE/Cert/CV/Fin@Handshake;client 侧 Fin 亦 Handshake;EoED 不存在);但 NST
   (HandshakeDone 后)与 KeyUpdate 响应发生在 drive 之间——sink 按次带级别,
   引擎不依赖"一次一档"假设,实现零特判。
4. **CH 分片界**:DOs 4-record 界放宽后,超界 CH 的 fuzz 面(CRYPTO offset
   重组)已有 QuicDataReassembler 承担,引擎只管消息界;回归用例:CH 跨 5+ 记录。
5. **性能**:QUIC 模式不装记录 cipher、不构 ConnectedState,握手路径比 TCP 更
   短;secret 导出为 span 即拷,零额外分配。all_benchmark #5 的 H3 档位基线在册,
   slice 2 后复测防回退。

## 13. 实施记录

### slice 1(2026-09-24,引擎 QUIC 模式)— 完成

**状态**:完成;全量 ctest **2428/2428 绿**(新增 `tests/TlsQuicHandshakeTest.cpp`
12 用例,4 例环境门控跳过与本次无关)。

**落地形态**(与 §3 的偏离就地标注):

- `TlsQuicCallbacks`(TlsConfig.h):四函数指针 + ctx,与 §3.1 同构;两 config
  各挂 `const TlsQuicCallbacks *quic`,null = TCP 行为不变(引擎全路径零分支
  成本的定谳 1 保持)。
- **级别枚举实现为四档** `TlsQuicLevel{Initial, EarlyData, Handshake, Application}`
  ——偏离 §3.1 的三档 `TlsQuicSecretLevel`:出站 CRYPTO 必须能标 Initial
  (CH/HRR/SH 都是 Initial 包级,见定谳 7),而 `set_secret` 恒不触发 Initial
  (该档注释即此约束)。`static_cast<int>` 与 BoringSSL `ssl_encryption_*` 一一
  数值对齐,测试 static_assert 锁定。
- 引擎 API(client/server 两个 engine 对称):`feed_quic(level, span)`(内部
  reassembly:消息界 4 MiB 超界 DecodeError、空 span = 合法 level ping、级别
  单调断言)、`take_quic_result()`(握手尾一次性交付 `TlsQuicHandshakeResult`
  {resumption_master, alpn, peer_chain, session_resumed, early_data_accepted},
  不构 ConnectedState——`take_state()`/`take_quic_result()` 互相断言对方模式
  不可达,定谳 2 双向守卫)、`take_inbound_leftover()`(尾部字节交付,slice 2
  装配 post-handshake 消费器的数据源)。
- 明文常驻:握手记录不装 cipher、compat CCS 抑制(client 首飞 + HRR 重启两处)、
  fatal alert 不编码进出站(`failure_alert()` → QUIC 层 0x0100|desc 映射不动)。

**定谳 7(新增):SH 本体走 Initial CRYPTO。** RFC 9001 通篇只说"CRYPTO 帧
带 TLS 握手字节",级别归属看实现生态:BoringSSL `tls_set_write_state`
(tls_method.cc:60-100)先 `tls_flush_pending_hs_data` 把**积压的 SH 用旧级别
(Initial)刷出去**、然后才 set_write_secret 推级别——故 SH@Initial、
EE/Cert/CV/Fin@Handshake;quic-go/ngtcp2 同样从 Initial crypto 流读 SH。喂入侧
对应门:`SSL_provide_quic_data` 要求 level == 当前 quic_read_level,否则
WRONG_ENCRYPTION_LEVEL_RECEIVED(ssl_lib.cc:709)。实现遵循同序(SH 先 emit,
级别再进 Handshake、hs 秘密在其后导出);HRR 场景 Initial 流上有**两个** type
0x02 记录(HRR + 真 SH),测试已锁定。曾按"SH 应走 Handshake"重排,被 BoringSSL
客户端以 WRONG_ENCRYPTION_LEVEL 当场驳回——此为互操作硬约束,非风格选择。

**导出序契约(测试锁定)**:

- 同级双秘(两侧都在的一档):write 先于 read 导出;非 Initial 级的
  add_handshake_data 恒晚于本级 write secret(该级 CRYPTO 不先于其加密能力)。
- server 接受 0-RTT:**只导 early 读秘**(client_early 的读方向),无对称写——
  合法不对称,§4 表勘误即此。
- 对驱比对方向恒翻转:engine(client) 的 write@level ↔ peer(server) 的
  read@level(同为 client_X_traffic_secret)。

**RFC 9001 对齐修正(实施中实证补齐,7 处)**:

1. legacy_session_id 恒空(§8.4);server 见非空 → illegal_parameter
   (对齐 BoringSSL UNEXPECTED_COMPATIBILITY_MODE)。
2. **回显校验比对线上形态**:client 引擎的 sid 回显检查原比 32 字节抽取熵,
   QUIC 线上恒空 → 每 QUIC 握手必 illegal_parameter。修为 QUIC 分支比空
   (start() 与 HRR 重启两处)——slice 1 唯二生产 bug 之一。
3. EoED 整体移除(§8.3):client 不发、transcript 不含;server 早接受分支在
   QUIC 直进 WaitClientFin(原 TCP 的 WaitEndOfEarlyData 等一个永不到来的
   消息 → UnexpectedMessage)——唯一二生产 bug。收到 EoED → fatal。
4. ALPN 强制(§8.1):client config 无 ALPN → 构造失败(failure_alert =
   InternalError);client 侧 EE 无 ALPN → NoApplicationProtocol(0x0178);
   server 侧 CH 无 ALPN → NoApplicationProtocol。
5. 票据 max_early_data 用 0xffffffff 哨兵(§4.6.1):QUIC 模式 minter 请求
   带 alpn 时按"无限早数据"发哨兵(存储侧见 store entry 断言)。
6. 0x39 注入/提取:CH(server)与 EE(client)解出即回调
   `on_peer_transport_params`(一次;提取用 extensions_block 重扫)。
7. 握手尾交付:resumption_master/alpn/peer_chain/session_resumed/
   early_data_accepted 由 `take_quic_result()` 一次性交出;post-handshake
   尾字节由 `take_inbound_leftover()` 交付(slice 2 消费器数据源)。

**测试矩阵(12 用例,tests/TlsQuicHandshakeTest.cpp)**:

- 自研 client ↔ BoringSSL server(全秘字节比对、HRR、垃圾首消息、
  InboundLeftover、0-RTT offer 被 BoringSSL server 拒)。
- 自研 server ↔ BoringSSL client(全秘比对、HRR 双 0x02@Initial、NST 经
  `SSL_process_quic_post_handshake` 被 BoringSSL client 消费、store entry
  max_early_data 哨兵、sid 非空拒、ALPN 缺失拒)。
- 自驱 0-RTT:hop1 出票(哨兵)→ hop2 带 obfuscated_ticket_age 恢复,
  early client-W == server-R,hs/app 翻转比对,resumed+early_accepted 双真,
  resumption_master 相等。
- 消息界/分片:0x400001 超界 DecodeError、partial 无事、空 span ping 合法。

对驱注意(后续 slice 复用):BoringSSL server 无 AES 加速主机选 ChaCha20
(0x1303)→ 断言须套件无关(Finished 尺寸 0x1302→52 否则 36);`SSL_set1_host`
fixture SAN 域须匹配;测试 sink 须留累计日志(drained 队列被 pump 消耗)且
指针在 pump 后取(vector 增长悬垂)。

**待续**:slice 2(QuicTlsSession 重写 + 装配 + post-handshake 消费器 + §9)、
slice 3(0-RTT/恢复矩阵)、slice 4(链接收窄)。待拍板 A/B 未决。

### slice 2(2026-09-24,QuicTlsSession 重写 + 装配 + 消费器 + §9)— 完成

**落地形态**(src/quic/QuicTlsSession.{h,cpp} 全量重写,公共契约面不变):

- init/init_client/init_server/provide_crypto_data/drive_handshake/
  process_post_handshake/alpn/alert stash 签名与语义保持 pre-10 形状;
  内部一角色一引擎(`TlsClientHandshakeEngine`/`TlsServerHandshakeEngine`
  raw new,RAII delete,须在连接 loop 上生死——IoBufChain 节点池亲和)。
- 配置staging 全为地址稳定成员(quic_cb_/client_cfg_/server_cfg_/selector_/
  minter_/lookup_);`TlsQuicCallbacks` 四静态 thunk 转发到 on_quic_*。
- server 装配(§8):net::TlsServerParam → 模板 TlsServerConfig(ALPN
  强制非空、mTLS trust 检查、1.3 pin、now);凭据经 09 §4.1 selector shim
  (`select_server_config`,TlsServerHandshakeConfig 引擎模式构造 + alpn_list
  前两字节 wire-form 技巧)按 CH 重建,callback 错误 latch 后由
  fail_terminal 优先上报(不 close)。
- client 装配(§8):mirror TlsStreamFd::start_client(system_default trust、
  verify_name IP/host 二分、sni 非 IP 才发);恢复会话整体拷贝进
  session_state_(identity vector + psk from_bytes),offer_ span 指向该副本
  (param 借用契约:连接握手存续期)。
- done-transition(`finish_handshake`,一次性):take_quic_result →
  resumption_master/alpn/suite stash → take_inbound_leftover →0x39 完整性
  (缺失 TransportParameterError close)→ client 0-RTT 裁决(accepted →
  on_early_data_accepted[失败亦 TPE close];rejected → 定谳 5:无回滚无
  re-drive,仅 on_early_data_rejected 丢弃早状态)。时序安全:全部先于
  mark_established。
- 秘钥导出:on_quic_set_secret 映射级/套件后
  quic_set_encryption_secret;negotiated_suite_ 首次导出即存(NST 回执需);
  EarlyData 丢弃条件 = Application+Client+双 app 钥就绪。
- post-handshake 消费器(§7):done 后 app CRYPTO 入 post_buf_(64KiB 上限,
  超限 MessageTooLarge+close);pump 解 4 字节握手头:NST(client-only,
  decode→tls13_resumption_psk→TlsSessionState→on_new_tls_session;decode
  失败 DecodeError close,psk 分配失败静默跳过)、KeyUpdate/其余 → fatal
  unexpected_message(RFC 9001 §6,定谳 4)。
- 证书校验失败映射(§8 无 long 码):cert 族 alert{42,43,44,45,46,48,49} →
  verify_failed_+Permission;peer_verify_result() 回 alert 值(非零语义保持)。
- 票据零配置平权:`ticket_service == nullptr` 时进程级惰性单钥
  TlsTicketService(random_key,10 年 mint 窗,永不析构——对齐 pre-10
  TlsRuntime::server_context() 共享 CTX 默认票据钥的跨连接恢复形状;
  08 注入旋转仍是部署路径)。熵失败 = 安全降级(无 NST)。

**§9 去 SSL_SESSION 化**:QuicClientCachedState.session →
tls::TlsSessionState(持有值);store_session 签名改 move 交接;QuicConnection
Ops.on_new_tls_session 改 (void*, QuicConnection&, TlsSessionState&&);
Http3ClientConnection 缓存接线随之更新;tests/QuicClientTest 的
TestClientCache 去 SSL_SESSIONFree/early_data_capable(→
`!empty() && max_early_data != 0`)。

**两处 fixture 修正**(引擎 SAN-only 语义对齐,matches_host 恒
NEVER_CHECK_SUBJECT):tests/QuicTestTlsCertificate.h 与 lite_nginx
kSelfSignedCertPem 重发为 SAN=DNS:localhost,IP:127.0.0.1(同钥换证,
CA:TRUE 保持;openssl req -x509 默认已带 BC,勿 -addext 重复)。pre-10
BoringSSL 走 CN 回退故旧证可过;QUIC 全量测试经此修复 2428 绿。

**验收**:2428/2428 ctest 绿;curl(ngtcp2/nghttp3)对 lite_nginx H3
(19543→nginx echo 9001)GET/POST/CA 验证通,TLS1.3 x25519
TLS_AES_128_GCM_SHA256;Retry/0-RTT 矩阵由测试覆盖(绿)。
snap curl --cacert 须 $HOME(仓库路径读不了)。

**待续**:slice 3(0-RTT/恢复矩阵扩展)、slice 4(链接收窄 + TlsSslFactory/
TlsRuntime 清退)。待拍板 A/B 未决。
