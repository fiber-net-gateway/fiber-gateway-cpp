# TLS 自研实现 · 09 net 集成换芯(TCP TLS 换自研引擎;QUIC 不动)

## 1. 范围与定谳(2026-09-23,六项全决)

范围 = TCP 面 TLS(`TlsTcpStream`/`TlsStreamFd`/http TLS 装配)从 BoringSSL 换到
06/07/08 自研引擎;**QUIC(`QuicTlsSession`)维持 BoringSSL 不动**——数据面加密本就
自研,BoringSSL 只出握手,换芯收益小代价高(传输模式解耦+QUIC 密钥派生+客户端
0-RTT 翻案)。X509/EVP 密码学原语层(证书解析、路径验证、签名、AEAD)不变——
那是 02b/05 的底座,本就在"允许的 BoringSSL 面"内。

定谳六项:

1. **TPK 物料 = 配置注入;未配置不发 NST**。无 per-boot 随机兜底:未配置 = minter
   不接线 = 引擎不发 NST(1.2 连 SH ticket ext 都不回显)。跨重启恢复仅当配置
   显式注入物料。
2. **KeyUpdate 永不主动发送**(与 nginx 同)。收方处理是必做项:收到 KeyUpdate
   立即换读钥;收到 update_requested 按 RFC 8446 §4.6.1 的 MUST 语义**被动**回一
   条 own KeyUpdate(下一次写之前)——这是响应不是主动;若连此也不回,几行可删
   (§3 注记)。
3. **1.2 收 HelloRequest(重协商请求)直接 fatal**(unexpected_message)。
4. **net 公共 API 不变**(`TlsTcpStream`/`TlsParams`/`TlsCredential`/`TrustStore`/
   `TlsServerHandshakeConfig` 签名不动);唯一增量 = `TlsServerParam` 加 ticket
   service 指针字段(§6,装配所需,纯增量)。
5. **客户端 min_version 强制做**(min=1.3 时 CH 收窄 + SH 校验);版本域仅 {1.2,1.3}。
6. **一刀切换芯**,无双芯开关、无编译期开关。现网 2387 测试即验收面。

## 2. 换芯边界

**不变(对外零感知)**:http 层(TlsAlpn 装配、`HttpServerTlsOptions`、Http1/2
Endpoint、grpc、lite_nginx)、QUIC 全部、net 公共头五件签名。

**替换(net detail 五文件)**:

| 文件 | 现状 | 换后 |
|---|---|---|
| `TlsStreamFd.cpp`(697L) | SSL\*+自定义 fd BIO(SIGPIPE 补丁)+`SSL_do_handshake/read/write/shutdown` 驱动环 + ex_data 桥接 | 引擎(握手)+`tls::TlsConnection`(后握手);协程 poll/timeout/busy 骨架保留;BIO 与 SIGPIPE 补丁**退役**(自研路径直走 StreamFd 的 send,天然 MSG_NOSIGNAL) |
| `TlsSslFactory.*` | SSL_CTX/SSL 组装 | **消失**(选项装配合并进 glue) |
| `TlsRuntime.*` | select_certificate_callback + ex_data 索引 | **消失**(被引擎 CH selector 替代,§4.1) |
| `TlsCredential.{h,cpp}` | 持 SSL_CREDENTIAL | 持 `tls::TlsCertificateChain`+`tls::TlsPrivateKey`(02b parse_pem 全齐);`session_identity_` 删(stateless 下无意义) |
| `TrustStore.cpp` | 包 X509_STORE | 包 `tls::TlsTrustStore`;system_default → tls 层同源(§4.4) |

## 3. TlsConnection(新件,slice 1 主体)

位置 `include/fiber/tls/TlsConnection.h` + `src/tls/connection/`。从
`TlsConnectedState`(move)构造;`Role{Client,Server}` 决定 KeyUpdate 的 own/peer
secret 取向(cipher 本身已按端点取向——engine 的 move 语义保证 seq 连续,§05)。

- **读**:feed(IoBuf)→ `pump(out)`:TlsRecordReader 拆录 → cipher open(长度界
  先行,AuthFail → fatal bad_record_mac)→ 1.3 按 inner type 分发:AppData →
  明文链;Handshake → 重组器(消息界 64KiB)→ **NST 吞**(定谳:客户端不缓存)
  / **KeyUpdate 换读钥**(peer secret 走 `tls13_key_update` + 新 cipher@seq0)/
  其他(CertificateRequest 等)→ fatal unsupported;Alert → close_notify=
  PeerClosed / 其他 fatal;CCS → fatal(握手后非法)。1.2 按 outer type 直接分发:
  Handshake(HelloRequest)→ fatal(定谳 3)、Alert 同上、CCS fatal。
- **写**:payload 每 ≤16KiB 一 record:seal(disjoint dst,每 record 一次
  memcpy;05 的 scatter 原语在引擎侧已就位,连接侧 v1 走简路径,benchmark 后再
  优化)+ 5B header,append 到 out。空 payload 出零长 record(与 writer 语义同)。
- **close_notify(out)**:若 rekey_pending_ 先回 KeyUpdate,再 seal close_notify。
- **fatal**:pump 把 fatal alert 记录写进 out(net 尽力冲刷后关连接)。
- **update_requested 被动响应**:置 rekey_pending_,下次 write/close_notify 前先
  发 own KeyUpdate(update_not_requested)再换写钥。注记:RFC MUST 的响应项与
  定谳 2 的"主动"不冲突;不要响应则删该分支即可。
- 明文读接口:`read(buf,len,out)` 状态机 {Ok,NeedMore,PeerClosed,Fatal} +
  `pending_plaintext()`。

**引擎小补件**:`take_inbound_leftover()`(两引擎,转发 ctx pending)——握手完成
后 ctx 里未消费的入站字节(1.3 客户端侧的 NST、TCP piggyback 的 app data)移交
连接对象。不做这条,客户端连接在真实服务器补发 NST 时丢字节。1.3 NST 首张在握手
尾由引擎发(08),"对端再发的"归连接对象吞。

## 4. 引擎补件(net 面,slice 2 前半)

1. **server CH selector**:`TlsServerHandshakeEngine` 增构造形态
   `TlsServerConfigSource{const TlsServerConfig *(*select)(void*, const TlsClientHello &); void *ctx}`
   ——fork 解码 CH 后调用;null 返回 → HandshakeFailure(未识别 SNI)。
   staged config 借用契约与今 `TlsServerParam` 同型(握手期间存活,glue 每连接
   staging)。无 selector 的单 config 构造保留(测试/未来 QUIC)。net 的
   configure_callback 在 selector 里跑,经 shim `TlsServerHandshakeConfig` 填
   staged config。
2. **版本界**:两 config 加 `min_version`(默认 1.2;max 恒 1.3 = 实现上限)。
   server fork 按 [min,1.3] 门控(全不满足 → protocol_version);client CH
   supported_versions 按 min 收窄 + SH 所选 < min → fatal。net param int 映射,
   域外/倒置 → Invalid(start_client/start_server 报,与现行为同型)。
3. **`TlsClientConfig.check_host`**(空 = sni_host):承接 net 的
   server_name(SNI)/verify_name(校验名)分离;verify_name 是 IP 字面量时
   glue 转 verify_ip(4/16B)。
4. **`tls::TlsTrustStore::system_default()`**:X509 默认路径,进程级缓存
   (immutable + 免锁,同 08 §5 之理)。

## 5. net 换芯映射(slice 2 主体)

- `TlsCredential::create` → `parse_pem_bundle`/`parse_pem` → 持链+钥;
  `add_credential` → staged config 取材。
- `TlsServerHandshakeConfig` 变 builder:填充 glue 的 staged `TlsServerConfig`
  (alpn 上界内拷贝);`set_session_id_context` = 接受 + no-op(stateless 票据下
  无意义,注释在案);`set_protocol_versions` → staged bounds;
  `set_early_data_enabled` → staged(维持默认 off,反重放缺位 08 §2)。
- `TlsStreamFd`:握手协程 = engine 构造(client 用 `TlsClientConfig` 直构;server
  用 selector 包 configure_callback)→ take_output→fd write→fd read→feed→…
  →Done:冲刷输出 → take_state → 建 TlsConnection;读/写协程 = connection
  pump/read/write;shutdown = close_notify + 限时收对端 close_notify(RST/超时
  降级关,与 SSL_shutdown 双向语义对齐)。`selected_alpn()` = ConnectedState.alpn。
- **每连接私有 `IoBufNodePool`**(pool 是非线程安全 freelist,且 TlsStreamFd 支持
  detach_for_handover——stealable 池跨 loop 偷连接,H3 有 node_pool 前科):
  engine 与 connection 共用同一私有池,连接销毁时释放——跨 loop 寿命问题根除,
  handover 零特判。
- `now_unix_ms` = `EventLoop::current().now()`(ms;请求路径统一时钟源)。
- 客户端映射:security.credential → client_chain/key;trust_store/verify_peer →
  同名;server_name → sni_host;verify_name → check_host 或 IP 字面量 → verify_ip;
  alpn span 直传。

## 6. TPK 装配(config 注入,slice 3)

- `HttpServerTlsOptions` 增可选物料:`[]{id, key(hex), created_ms}`;配置了 →
  server 构建一个 `TlsTicketService`(注入式免锁,跨 worker 共享安全——mint 选
  key 是时钟纯函数,lookup 暂存格 thread_local)→ 经 `TlsServerParam` 新增指针
  字段送达 glue → minter+lookup 接线。
- 未配置 → 字段空 → minter/lookup null → 不发 NST、不恢复(全握手)。定谳 1。
- 物料校验:hex 长度 = TPK 界、id 唯一;错 → server 启动 Invalid。

## 7. slice 划分与测试

| slice | 内容 | 验收 |
|---|---|---|
| 1 | `tls::TlsConnection` + 引擎 `take_inbound_leftover` | TlsConnectionTest:自驱双连接(引擎对打到两连接)app 双向、close_notify 双向、KU 收方换钥 + update_requested 被动回、NST 吞、fatal alert、1.2 HelloRequest fatal、BoringSSL 内存对当 |
| 2 | 引擎 selector+版本界+check_host+system_default;net 五文件换芯(API 不变) | 现网 2387 全绿即验收(TlsStreamFdTest/动态证书 SNI/ALPN/http TLS e2e/grpc);版本下限新测 |
| 3 | TPK 装配+http 透出;无配置→无 NST;BoringSSL 客户端跨重启恢复 e2e;benchmark(对照 BoringSSL 基线,all_benchmark 基建);删 BIO;文档收尾 | 08 §10 装配遗留清零 |

## 8. 实施记录

- **2026-09-23 slice 1**:`tls::TlsConnection` + 引擎 `take_inbound_leftover`,2402 全量绿
  (2387 基线 + 15 新)。
  - 新件 `include/fiber/tls/TlsConnection.h`(公共头,pimpl 零 openssl)+
    `src/tls/connection/TlsConnection.cpp`。**复用而非重写** `TlsHandshakeContext`:
    `arm_app_data_sink` 泛化 0-RTT sink 为连接态 app-data 投递(两版本同路——1.2
    open 后 inner_type==outer type 命中同一 sink 分支);写 = `ctx.emit`(≤16KiB 分
    record、1.3 outer AppData/1.2 保型、0x0303 皆内建);fail/alert 编码全继承。
  - KeyUpdate:收方即换读钥(`tls13_key_update`+新 cipher@seq0,stage-then-swap);
    update_requested 置 `rekey_pending_`,下次 write/close_notify 前先发 own
    KU(5 字节消息,**旧写钥** seal)再轮换写侧——先派生后发送,失败路径零状态变
    更。NST 吞;其他 1.3 后握手消息/1.2 一切后握手 Handshake(含 HelloRequest)/
    后握手 CCS 全 fatal unexpected_message;KU 畸形体 decode_error(与 BoringSSL
    同值)。对端 fatal alert:置 failed 不回敬;对端 close_notify:置
    peer_closed,同 feed 已投递明文仍可读尽。
  - `TlsRecordReader::take_pending()` / ctx `take_inbound_leftover()` / 两引擎
    `take_inbound_leftover()`(转发,done&&!failed 契约)——引擎终态后未消费字节
    移交连接对象,reader 移出后原地重绑同池继续拆录。
  - **顺带修复(ctx 既有缺口)**:1.2 Sealed12 模式下入站 Alert 记录此前按明文
    2 字节直解(密文当 alert)——现先 open 再走 inner-alert 分支;此前不可达
    (引擎测试的 1.2 对端从不发密封 alert),连接态 1.2 close_notify 必经此路。
  - 测试 `tests/TlsConnectionTest.cpp` 15 条:自驱双连接 ping/pong+ALPN、40KB
    分 record、close_notify 双向(同 feed 明文先读尽)、空写=单零长 record、
    **KU 全闭环**(crafted KU+发端写侧轮换 → 收方换读钥 → 被动响应[旧钥 KU+
    新钥 app]→ 对端换读钥 → 后轮换写回读通,零手工解密)、KU(0) 无响应且对端
    读钥不换、NST 吞后续流活、对端 fatal alert 悬置、篡改=BadRecordMac+自警
    报出、CCS fatal、**leftover 移交**(Fin 飞行带 3 字节半 record 头 → 引擎
    leftover → 连接续流拼回)、合成 1.2 情报对(tls12_key_block 直构)HelloRequest
    fatal + app 双向 + 密封 close_notify 双向、BoringSSL 1.3(app 双向+
    SSL_key_update(REQUESTED) 真 peer 验证响应时序+shutdown)、BoringSSL 1.2
    (app 双向+双向密封 close_notify,即 ctx 修复的真机验收)。

- **2026-09-23 slice 2**:引擎补件 + net 五文件换芯,2412 全量绿(2402 + 10 新)。
  - **引擎补件(§4)**:server `select_server_config`(SNI/ALPN/version 三元回调,
    staging cfg 由 param 的 configure callback 每 CH 重派;null 答案 =
    handshake_failure);两引擎版本域收紧 `{1.2, 1.3}`(client SH 版本低于下限在
    1.2 分叉点即拒;server CH 低于下限 protocol_version);client `check_host`
    (独立于 SNI 的校验名,wrong = bad_certificate);`TrustStore::system_default()`
    进程级缓存(首用加载后冻结);`evp_pkey_handle`/`x509_store_handle` void*
    桥(net 物料 → 引擎 crypto 层,对外头零 openssl include)。server 引擎
    ctor 改 selector-模板模式:config 持有期断言 + 全量 invariants 在构造边界
    建立(nullability-at-edges)。
  - **net 换芯(§5)**:`TlsStreamFd` 重写为引擎宿主——握手期 `Handshake*`
    staging(param 借用契约延至 handshake co_returns;callback 错误 latch 后
    随 fatal alert 一起上报),成功路径换 `tls::TlsConnection`;每连接私有
    `IoBufNodePool`(loop 无关,支持 handover/CrossLoop);`flush_output` 16-iov
    writev + `(ptr,len)` 同指针重试契约(不同缓冲重入 = Busy);`feed_engine`
    单次 32KiB 读喂引擎;`early_data_`(server 0-RTT 明文先行交付)。公共 API
    零变更——现网 2387 基线测试即验收面(定谳⑥)。新增 10 测:server selector
    ×2、版本下限(client ×4/server ×1)、check_host ×2、system_default 缓存 ×1、
    engine 对打 transport(见下)×…(实为 10 条:TlsTransport wait_readable/
    writev 合并/WouldBlock 组保持/abandon 等 transport 面由既有 TlsStreamFdTest
    覆盖)。
  - **偏离 §2 计划**:①QUIC 共用文件(TlsSslFactory/TlsRuntime/TlsSessionOps/
    TlsHandshakeState)保留为 QUIC-only BoringSSL glue,不删——§2 原写"消失",
    实际 QUIC 面仍依赖(QUIC 不在本 09 范围);②证书有效期快照用
    `system_clock` 墙钟而非 `EventLoop::now()`(steady 单调源——有效期是真实
    世界时间输入,文档 §4 原写 EventLoop::now 是错的);③shutdown =
    close_notify→flush 后即返,不等对端回告(不产生额外往返等待);
    ④`TlsCredential::create` 显式 cert/key pair 检查(matches_private_key,
    复刻 BoringSSL set1_private_key 行为);⑤TrustStore File/Content 路径统一走
    `from_pem_bundle`;⑥engine-mode `set_trust_store` 非空即请求客户端证书
    (mTLS 语义 = trust 非空);⑦`set_protocol_versions` 域收紧同样覆盖 SSL 模式。
  - **四项调试修复**(均现网/net 测试暴露,前三 glue 层、第四引擎层):
    1. **成功路径 drain 引擎输出**——handshake 成功换芯时先
       `out_pending_.append_chain(engine->take_output())` 再 take_state/delete:
       1.3 client 的 Finished 记录在最后一次 feed 后才 seal,留在引擎输出链里,
       引擎一死记录蒸发,对端死等 Fin。
    2. **zombie 节点**——`IoBufChain::empty()` 数节点而 `consume()` 不摘链:
       flush 写空后 out_pending_ 留零可读 zombie 节点,`empty()` 为真值但
       readable_bytes()==0,write_once 走"仍在 flush"分支 → 指针不匹配 → Busy
       假错。三处 `consume`→`consume_and_compact`(out_pending_/early_data_/
       TlsConnection::read 的 plaintext_),与全库读取消费惯例对齐。
    3. **终态计入 has_pending_read**——对端 close_notify 已被消费进 conn_
       (peer_closed latched)而无 pending 明文时,socket 上永远不会再有字节
       边沿;`has_pending_read` 纳入 `peer_closed()/failed()`,否则
       TlsTransport::wait_readable 死等一个已 fired 的边沿(TimedOut 假超时)。
    4. **降级哨兵双向门控(引擎层,RFC 8446 §4.1.3 真缺陷)**——我方 1.2 server
       无条件写 DOWNGRD 哨兵 + 我方 1.2 client 无条件检查:pinned-1.2 双自研
       引擎对驱(双侧引擎测试各自只与 BoringSSL 对驱,该组合不可达)首次暴露,
       双侧 IllegalParameter(47)。修法=server 仅 CH 不含 1.3 时写(§4.1.3 原
       文条件);client 仅 `max_version >= 1.3` 时检查(1.3-capable client 才有
       资格认定"被降级";pinned-1.2 client 的 CH 不含 1.3 是真实意愿非被剥)。

- **2026-09-23 slice 3**:TPK 装配 + http 透出 + 跨重启恢复 e2e + benchmark,2416
  全量绿(2412 + 4 新);08 §10 装配遗留清零(归属定谳 = 每 endpoint 一个共享
  service,理由见 08 §10)。
  - **装配链(§6)**:`HttpServerTlsOptions::ticket_keys`(`{id, key_hex, created_ms}`
    owning;空 = tickets off)→ `TcpEndpointBase::build_ticket_service()`(on_start
    在 bind 之前校验:hex 恰 32 字符、hex 合法、id 唯一、≤kMaxKeys=8;失败 →
    server 启动 Invalid,唯一失败点)→ 每 endpoint 一个共享 `TlsTicketService`
    (免锁:mint 选 key = 时钟纯函数、lookup 暂存格 thread_local,跨 worker 共享
    安全)→ H1/H2 endpoint 的 `make_tls_param()` 盖章 `param.ticket_service`
    (定谳④唯一 net API 增量)→ `TlsStreamFd` Handshake staging 持 minter/lookup
    适配器 → 引擎 ctor(null = 无 NST 无恢复,引擎原生)。H3 endpoint 不消费该
    字段(QUIC 不在 09 范围);window policy 保持库默认 24h/7d(YAGNI)。
  - `TlsTicketService::minter()/lookup()` 加 const(hook ABI 是 void\* ctx,const
    仅在 thunk 内补回)——glue 持 param 的 const 指针。
  - **测试 4 条**:net 级 BoringSSL socket 哨兵(blocking socketpair + 双
    `BIO_new_socket(BIO_NOCLOSE)` + 5s `SO_RCVTIMEO` 界定挂起;1.3 NST 尾随
    flight,需 poll+read slurp;1.2 ticket 在 flight 内被握手消化):
    `UnconfiguredTicketServiceMintsNoSessionTicket`(无配置 → 客户端 stash 空,
    定谳①)、`BoringsslClientResumesAcrossTicketServiceRebuild`(双版本:service_a
    握手取票 → 同物料 service_b 重建 + `SSL_set_session` → reused)。http 级全栈
    e2e(物料 → 校验 → service → param → glue → 引擎):
    `RejectsBadTicketKeyMaterialBeforeListening`(31 字符 hex / 'z' / dup id /
    9 把 → start Invalid;正例 good 集起停)、`ServesResumableTicketsAcrossServerRestart`
    (server A 取票 + GET 200 → stop_and_join → server B 同物料重启 → resumer
    reused + GET 200)。**坑**:`std::array<kMaxKeys>` staging 整体转 span 会把零填
    尾部当 duplicate id-0 key → service invalid → 启动 Invalid(http 级首跑抓住);
    修为 `span(material.data(), ticket_keys.size())` 只传前 N 条。
  - **删 BIO(§7 项)确认**:slice 2 已退役 net 层 fd-BIO(`TlsStreamFd` 直走
    StreamFd send,天然 MSG_NOSIGNAL);生产代码 grep 证实 BIO 仅存 crypto 原语层
    (TlsSignature/TlsCertificate,允许面)。测试里的 `BIO_new_socket` 是 BoringSSL
    哨兵客户端自身,非生产路径。
  - **benchmark(#5 同机同法;lite 端口 18080→18081 避无关进程冲突)**:对照
    openresty/nginx H2 四场景与 #5 偏差 ≤1% = 环境等价;lite H1(明文回归面)
    +4~12%(环境略快,代码路径未动,无回归)。**lite H2(TLS 换芯信号)**:
    GET 1K 158.8k(-11%)、GET 64K 10.2k(-54%)、GET 1M 704(-60%)、POST 343
    (-60%);全部 0 错误 / 0 非 2xx。bulk 三场景绝对值与 #3(≈ a58cf18 之前)
    几乎一致而对照零变化 → 回退主因假设 = 05 适配层 seal_scatter 跨 IoBufChain
    节点退化转录,吃掉了 a58cf18 的跨节点零拷贝增益。§3 原计划"连接侧 v1 简路径,
    benchmark 后再优化"如约到期:**恢复跨节点零拷贝 seal 是换芯后的首个优化项**。
