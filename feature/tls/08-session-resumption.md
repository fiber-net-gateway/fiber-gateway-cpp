# TLS 自研实现 · 08 会话恢复(slice 1:无状态 NST 下发;slice 2:lookup 恢复接线;slice 3:1.2 abbreviated 服务端)

## 1. 范围与定谳

08 原案(会话恢复全案)在 slice 1 收窄为**服务端 NewSessionTicket 下发**,TLS 1.3
与 TLS 1.2 双版本,并按用户定谳做成**无状态**:服务端不存储任何已下发票据;票据本体
即全部恢复状态,以 ticket protection key(TPK)做 AEAD 加密认证;恢复时服务端只验证
票据合法性,不存在"是否下发过"的记忆判断。**slice 2 补齐 lookup 半边与恢复接线**
(§7):`TlsResumptionLookup` 真实现接进 07 引擎,1.3 票据全链路恢复——零格式改动。
**slice 3 补齐 1.2 abbreviated 服务端**(§8):同一套 TPK 物料与容器,1.2 票按
RFC 5077 恢复为无证书、无密钥交换的缩短握手。

后移到后续 slice 的内容:

- 09 net 集成换芯时的服务装配(每 loop 一个 service 或跨 loop 共享,§5)。

客户端取票入会话缓存经用户定谳**不做**(无需求,2026-09-23):06 客户端不缓存
票据、不主动发起恢复;恢复场景 = 服务端 lookup 半边 + 对端客户端(e2e 以
BoringSSL 客户端或手工 `TlsSessionOffer` 驱动,§7/§9 已是此形态)。1.2 侧经同批
定谳收窄为**仅服务端 stateless ticket abbreviated**(2026-09-23):服务端 session-id
有状态缓存不做(与无状态契约冲突且无需求),客户端票缓存不做;1.2+(EC)DHE 恢复
(非标准组合)不涉及。

## 2. 无状态契约(三条,设计即钉死)

1. **零存储 ⇒ 无 0-RTT 防重放**。重放的票据每次都能解密通过。`enable_early_data`
   维持 false;开启即部署显式接受重放(RFC 8446 §8)。本 slice 的 1.3 票据
   `max_early_data` 字段恒写 0(字段位保留,便于将来切换)。
2. **TPK 是 PFS 边界**。TPK 泄露可伪造/解密票据。缓解:1.3 票据存的是**预派生
   PSK**而非 resumption master(§3);恢复握手仍协商 (EC)DHE,泄露 TPK 不能解密
   历史录制流量;TPK 定期轮换(§5)是必选项。
3. **过期是时钟判定**。`issued_ms` 密封在载荷内,过期 = `now - issued > timeout_s`。
   多实例时钟漂移按漂移量平移接受窗口(前向漂移容忍,§4 open 语义);重启、多
   worker、横向扩容行为一致——只要持有同一 TPK 物料。

## 3. 容器格式 v1(四长寿要素冻结)

```
票据   = ver(1)=1 ‖ key_id(4 BE) ‖ nonce(12) ‖ ciphertext ‖ tag(16)
AAD    = ver ‖ key_id ‖ be16(name.len) ‖ name        // name = CH 的 SNI
载荷   = kind(1) 分叉:
  1.3  = kind=0x13 psk_len psk suite(2) alpn_len alpn
         age_add(4) max_early_data(4) issued_ms(8) timeout_s(4)
  1.2  = kind=0x12 master_len=48 master(48) suite(2) alpn_len alpn
         issued_ms(8) timeout_s(4)
```

- AEAD = AES-128-GCM(EVP_AEAD 原语,与记录密码同层);16B 随机 key、12B 随机
  nonce(每票据新鲜)、16B tag。
- **name 绑 AAD**:SNI 在密文之外,跨 vhost 重放先于解密即失败。空名==空名是唯一
  允许的空匹配(无 SNI 的客户端群)。
- **key_id**:开票按 id 找 key,retention 窗口外的 key 拒收。
- **psk-not-master**:1.3 的 psk 在 mint 时由
  `tls13_resumption_psk(resumption_master, {nonce=0})` 预派生——与客户端收到 NST
  后的推导对称,双端一次 Expand-Label 直通;master 不离开 mint 时刻。
- 四要素(ver 字节、name-in-AAD、key_id、psk-not-master)冻结:将来 lookup slice
  打开的必须是今天下发的同一格式。

1.2 免存 session_id:RFC 5077 恢复时 session_id 由客户端生成随机值、服务端回显,
票据载荷不需要它(07 §2.8 已核实)。1.2 无 0-RTT、无 anti-replay 需求。

## 4. TlsTicketService(`include/fiber/tls/TlsTicketService.h`)

```cpp
struct TlsTicketKeyMaterial { u32 id; array<u8,16> bytes; i64 created_ms; };
TlsTicketService(span<const TlsTicketKeyMaterial> keys, const TlsTicketKeyPolicy &) noexcept;
//  空 keys / 重复 id / init 失败 → valid()==false;minter() 产生的钩子对每次
//  mint 返回 0(钩子契约:0 = 本连接不发 NST,安全路径)。
static bool random_key(u32 id, i64 created_ms, TlsTicketKeyMaterial &out) noexcept;
TlsTicketMinter minter() noexcept;              // 07 引擎直接消费
static std::size_t mint_thunk(void *, const TlsTicketRequest &, span<u8> out);
OpenStatus open(span<const u8> ticket, string_view name, i64 now,
                TlsTicketContents &out) const;  // Ok / Expired / Rejected
```

- `TlsTicketContents`:version、`TlsSecret secret`(1.3=派生 psk / 1.2=master)、
  suite、alpn(256B 内联)、age_add、max_early_data、issued_ms、timeout_s。
- `open` 三值:Ok = 认证+解密+新鲜度全过(内容 move 进 out,仅 Ok 时写);
  Expired = key 合法但会话超时(回退全握手,不是攻击信号);Rejected = 格式/
  认证失败/未知或已退役 key/名字不符(不区分,防 oracle)。解密载荷解析严格:
  kind、secret 长度(1.3 ∈ [1,48] 且随哈希、1.2 恒 48)、尾部必须无剩余字节,
  违者 Rejected。前向时钟漂移(elapsed < 0)容忍为 Ok。
- mint 侧边界:name ≤ 255(SNI 线上界)、alpn ≤ 255、1.3 master ∈ {32,48}、
  1.2 master 恒 48、version 仅 1.2/1.3、out 容量不足、**无 mint 窗口内 key**——
  一律 mint 0,不发 NST。
- 卫生:payload/secret 栈缓冲在每条失败路径与成功路径一律 `tls_secure_wipe`;
  key 物料 `EVP_AEAD_CTX_cleanup`(零化)在 init 失败与析构时执行。

## 5. 密钥管理 —— 注入式 + 不可变 + 免锁(2026-09-23 重构定稿)

**密钥集合构造时一次注入,之后永不变化**:mint/open 是对不可变数据的纯读,
**无任何内部锁**。并发调用 race-free 的依据:`EVP_AEAD_CTX_seal/open` 以 const
指针取 ctx 且 GCM 上下文无每次调用的可变态。

- 窗口按物料算:mint 窗 = [created, created+lifetime)、开票窗 = [created,
  created+lifetime+retention)(lifetime 默认 86400s、retention 默认 604800s;
  retention 应 ≥ 票据 timeout,否则票据随 key 提前死——安全降级,非错误)。
- **mint 选 key = 时钟的纯函数**:最新的"已出生且未退役" key(环 oldest-first,
  从尾扫);未出生的 key 不用——注入 [current, successor] 时恰在 successor 出生
  时刻交接。全窗口过 → mint 0,ST 停发直到装配换入新物料(无票据、无恢复,
  绝不出错 key 票据)。
- 构造期一次 init 所有 AEAD ctx;>kMaxKeys=8 把时按 created 取最新 8(等值
  保留既有,稳定);重复 id 拒绝(valid false,open 二义)。
- **轮换是外部的**:装配层注入 [current, retained…] 物料(配置文件/持久化
  seed),换 service 实例(或重启)即轮换。同一物料喂多个 service(每 loop
  一个)完全等价——loop 间无需共享实例。
- **重启不再废票**:key 来自外部,同物料重启后历史票据全部仍可开(对比首版
  内存随机生成:重启即全灭)。`random_key` 是零配置装配的熵帮手。

## 6. 引擎接线(07 两个引擎的 mint 点,零格式耦合)

`TlsTicketRequest`(TlsConfig.h)增两字段:`version`(选载荷字段集)、
`name`(CH 的 SNI,绑入 AAD)。

- **1.3**(`Tls13ServerHandshake::finish_1_3`):client Fin 后、以 server_app
  cipher 密封 NST 时 mint;门 = psk_key_exchange_modes 扩展 + minter 非空,恒
  1 张、nonce 0。请求由位置式初始化改为字段赋值,补
  `version=Tls13`、`name=hello_.view.server_name`(借 16KiB CH 保留数组,
  稳定;mint 同步调用,借用安全)。
- **1.2**(`Tls12ServerHandshake::send_final_flight_12`):Fin 之前、明文 NST;
  门 = CH offered 5077(`ticket_wanted_`)AND minter 非空。追加同两字段。

## 7. lookup 半边(2026-09-23 slice 2 实施;验证清单原样兑现,零格式改动)

`TlsResumptionLookup` 签名扩两参:`(ctx, identity, name, now_unix_ms, out)`。
`name` = CH 的 SNI(stateless open 据此核 AAD 名绑定),由引擎传
`hello_.view.server_name`(借 16KiB CH 保留数组,HRR 后 CH2 re-decode 同栈 view
仍有效);`now_unix_ms` = `cfg_.now_unix_ms`(与 age 门同一时钟快照,过载判定
与 age 门永不冲突)。out spans 借 hook 存储、**调用返回后被引擎读取**——契约注释
由错误的"返回前消费"修正。

真实现 = `TlsTicketService::lookup()` 适配对 + `lookup_thunk`:

- `open(identity, name, now, contents)` — 非 Ok(错名/篡改/过期)全部 miss →
  全握手回退。清单 1/2/3 由此兑现。slice 3 起钩子**version-blind**(原 slice 2
  在 thunk 内置"仅 1.3 票"版本门,现已上提到各引擎:1.3 引擎 lookup 后校验
  `resumed.version == Tls13` 否则 `PskOutcome::Reject`;1.2 引擎在 §8 接受级联的
  version 门里校验 `== Tls12`——错版票对两引擎都是普通 miss)。
- 命中时 `TlsResumedSession` 全字段映射:psk=`contents.secret.bytes()`(引擎立即
  拷进 `TlsSecret`;双语义——1.3:预派生 PSK;1.2:48B master)、**version**
  (slice 3 新增,引擎门输入)、suite、alpn、age_add、max_early_data、issued_ms;
  随后引擎既有级联接管:清单 4(suite 门)、5(binder 恒校验,mismatch=fatal)、
  6(恒 0 ⇒ 无 early_data 扩展)。
- **借用安全**:thunk 内开的票停 `thread_local TlsTicketContents t_staged_resumption`
  暂存格——栈变量随 thunk 返回即悬空,而引擎在 hook 返回之后才消费;引擎在同一
  步内读完,同线程下一次 lookup 覆盖,单飞不并发。

## 8. 1.2 abbreviated 服务端(slice 3,2026-09-23;RFC 5246 §7.3 + RFC 5077)

范围 = §1 定谳:仅服务端 stateless ticket 恢复。CH 出示票据 → lookup 接受 →
SH(sid 回显、票 suite/ALPN、EMS)[NST 轮换] CCS server-Fin,客户端回 CCS + Fin;
无 Cert/SKE/SHD/CKE,master 复用,key_block =
`tls12_key_block(suite, master, client_random, server_random)`。任一门不过 →
全握手回退,**永不因票据问题失败连接**。

**EMS 走 mint 门(容器格式零改动)**:1.2 发票门 = `minter && has_session_ticket &&
ems_negotiated_`——只有 EMS 会话发票 ⇒ RFC 7627 §5.3 禁止的跨 EMS 恢复在本体系
不存在;恢复侧 SH 恒回显 EMS,且 CH 须 offer EMS 否则 miss。容器无需 EMS 位。

**接受级联 `try_resume_12`**(短路序):结构前置(lookup 非空、非 mTLS——
`client_trust != nullptr` 恒全握手、票非空、sid 非空)→ open(AAD 名绑定/篡改/过期
在容器内)→ version 门(`== Tls12`)→ suite 门(`tls_suite_info` 非空、非 1.3
专属、CH offered;credential 无关——恢复连接不签名)→ EMS 门(CH 须 offer)→
ALPN 门(票有 alpn ⇒ CH 须仍 offer,借共享 `tls_ch_offers_alpn`;票无 ⇒ 无
alpn,绑定语义)。接受即:`master12_` = 票内 48B、kb12_ 派生、suite/alpn 继承、
`ticket_wanted_ = minter`(出示票据本身就是 5077 offer ⇒ 恢复恒轮换)。无 age 门
(1.2 无 obfuscated_ticket_age;过期已在 open 内)。

**飞行 `send_abbreviated_flight_12`**:SH(sid 回显、票 suite、EMS、空 RI、alpn、
ticket ext 空回显)→ NST 轮换(明文、CCS 前,`resumption_master` = master 本体)
→ CCS + write 侧换 `kb12_.server` → **server Finished 先发**(1.3 式顺序;MAC 覆盖
CH‖SH‖NST‖serverFin)→ `ExpectClientCcs12`。client Fin 验证通过且 `resumed12_`
→ 直达完成(不再发 final flight);`state_.session_resumed = true`。

**sid 回显语义(slice 3 的 wire 修复)**:SH 回显非空 sid 是客户端判定恢复的
唯一信号(BoringSSL:SH sid 匹配 offered session 的 sid ⇒ session_reused,期待
abbreviated flight)。全握手回退若照抄回显,持票客户端收到 Certificate 即
UNEXPECTED_MESSAGE("got type 11, wanted type 4")。修:1.2 全握手恒发**空 sid**
(无 id 缓存的无状态服务器,RFC 5246 §7.4.1.3 空 sid = 不可按 id 恢复);abbreviated
SH 是唯一合法回显。1.3 不受影响(legacy_session_id 回显是 RFC 8446 强制)。06 自研
1.2 客户端本就不校验 SH sid("server's own choice"),零影响。

**安全权衡(定谳在案)**:恢复连接无 PFS(master 复用,RFC 5077 固有,OpenSSL
stateless 同);TPK 泄露对 1.2 = master 直接暴露(对比 1.3 票存预派生 PSK、恢复
仍过 (EC)DHE);票重放可再建恢复握手,但 1.2 无 0-RTT、无数据注入面(§2 契约
第 1 条在 1.2 侧无对应物)。

## 9. 测试(2387 全量绿;票据面 17 单测 + 无状态恢复 e2e 5 + EE 重钉)

`tests/TlsTicketServiceTest.cpp`(17 个单测,slice 1 的 14 个 + lookup 3 个):
EmptyKeySetIsInvalid(valid false + mint 恒 0)、DuplicateIdsAreInvalid、
OverflowKeepsNewestEight(10 把注入 → 保留最新 8,mint 选最新)、
RandomKeyMintsFreshMaterial(熵新鲜 + 可构造可开票)、
Mint13RoundtripCarriesDerivedPsk(psk == `tls13_resumption_psk(master,{0})` 且
≠ master;注入 key id 落进容器 key_id 字段)、Mint12RoundtripCarriesMaster
(48B 回读)、DistinctNoncesDiffer、NameIsBoundIntoAad(错名/空名/空名票据)、
TamperedTicketRejected(ver/key_id/nonce/ct/tag 各翻一位)、
TruncatedTicketRejected(全前缀)、RotationHandsOverAtBirthAndRetainsThenDrops
([current,successor] 前置注入:mint@出生前用旧 key、@出生时刻交接、retention
窗内 Ok、窗外 Rejected、新 key 恒 Ok)、MintStopsWhenAllKeysPastWindow(全窗口
过 → mint 0)、TimeoutExpiryIsExpiredNotRejected(4'999 Ok / 5'001 Expired /
前向漂移容忍)、BadInputsMintNothing(1.2 47B master、1.3 非 hash 长、name 256、
alpn 256、version=Tls11、out 过小)。

`tests/TlsServerHandshakeEngineTest.cpp` 增 2 条 e2e(CapturingServiceMinter 包装
钩子捕获 mint 出的 blob,避免线上字节提取;ClientOptions 增 `send_sni` 经
`SSL_set_tlsext_host_name` 上线 SNI):
- **Mint13ThroughEngineBindsSniAndCarriesPsk**:DualMaterial(我方 06 client ↔
  07 server)pump 后开票 == Ok,psk 与服务端 resumption_master 推导相等,
  alpn=="h2"、issued==kRefNowMs、timeout==7200;错名 Rejected。
- **Mint12ThroughEngineCarriesMaster**:BoringClient(tls12_only + send_sni)全
  握手后开票 == Ok,version==Tls12、secret==state.tls12_master(48B);空名
  Rejected(SNI 已上线)。

slice 2 新增:

- `TlsTicketServiceTest` lookup 面:**LookupThunkResumes13Ticket**(lookup() 命中:
  psk == `tls13_resumption_psk(master,{0})`、suite/alpn=="h2"/age_add/issued 全
  字段回读)、**LookupThunkMissesOnWrongNameTamperExpiryOr12**(错名 / ct 翻位 /
  4s 票 5s 后过期 / 1.2 票 / 空 key service 全 false;slice 3 改名
  LookupThunkMissesOnWrongNameTamperOrExpiry 并去 1.2 用例——1.2 票不再是 miss)。
- `TlsServerHandshakeEngineTest` 无状态恢复 e2e 3 条(RecordingServiceLookup 包
  装 thunk,记录 called/seen_name/seen_now_ms):**StatelessResume13WithBoringClient**
  ——hop1 BoringClient(collect_tickets+send_sni)mint+NST 消化,hop2 set_session
  重连:session_resumed、SSL_session_reused==1、lookup 收 name=="example.com" 与
  now==kRefNowMs、二跳再 mint、app 数据双向 round-trip(证明 psk-not-master 派生
  与客户端 RFC 推导逐字节一致);**StatelessResumeOurPair**——DualMaterial hop1
  pump 抓票 → open 回读 → 手构 TlsSessionOffer(身份/age_add/suite/psk)→ hop2
  `service.lookup()` 接线 pump+tail:双侧 resumed、app secrets 相等、再 mint 新票
  且可开;**CrossVhostTicketFallsBackToFull**——hop1 票在 hop2 换 SNI
  ("other.example")出示(BoringSSL 会话使用不按名字设门,跨 SNI 仍 offer——已核
  其源码),AAD 名绑定拒 → 全握手成功、!resumed、reused==0、recorder 见新名
  (miss 归因服务端 AAD 拒,非客户端未带)。
- `TlsHandshakeCodecTest` EE server_name ack 重钉为零长度扩展体 + 负测(见 §11
  wire bug 修复)。

slice 3 新增(1.2 abbreviated):

- `TlsTicketServiceTest`:**LookupThunkResumes12TicketWithMaster**——1.2 票经
  lookup() 命中:version==Tls12、psk==48B master 原文、suite/alpn/issued 回读
  (钩子 version-blind 的直接验证);**LookupThunkResumes13Ticket** 补 version 断言。
- `TlsServerHandshakeEngineTest` e2e 2 条:**StatelessResume12WithBoringClient**
  ——hop1 BoringClient(tls12_only + collect_tickets + send_sni)全握手 mint 出票,
  SSL_read 消化 NST;hop2 set_session 重连:version==Tls12、session_resumed、
  SSL_session_reused==1、peer_chain 空、recorder 收 name=="example.com"/
  now==kRefNowMs、轮换 NST 已消化(g_new_sessions==2)、app ping/pong 双向
  round-trip(证明 master 复用 + key_block 派生与 BoringSSL 客户端逐字节一致);
  **CrossVhostTicket12FallsBackToFull**——hop2 换 SNI("other.example")出示同一票:
  AAD 名绑定拒 → 全握手成功、!session_resumed、reused==0、recorder 见新名
  (兼作 §8 空sid 修复的行为验证:回退客户端不再误判恢复)。

## 10. 待拍板 / 遗留

- 装配形态(09):每 loop 一个 service(免跨线程,物料同种子)vs 全局共享
  ——免锁重构后两者代码路径完全一致,纯归属选择;倾向每 loop(与 EventLoop
  归属一致)。TPK 物料来源(配置文件/seed)与分发是 09 装配题。
- 08 会话恢复至此收口:1.3 恢复 + 1.2 abbreviated 均为服务端 stateless,客户端
  票缓存与 session-id 缓存均定谳不做(§1)。

## 11. 实施记录

- **2026-09-23 slice 1**:TlsConfig.h(TlsTicketRequest +version/name)、新增
  include/fiber/tls/TlsTicketService.h + src/tls/session/TlsTicketService.cpp
  (容器 codec + 密钥环 + mint/open,EVP_AEAD 仅入此 TU)、两引擎 mint 点接线、
  单测 + e2e。自查修复:ctor/dtor 持锁 delete impl_ 的 UB(去锁+注释)、mint
  失败路径补 wipe、open 长度上界收严为 kMaxTicketLen。
- **2026-09-23 重构(用户定谳)**:密钥管理从"内部随机生成 + mint 惰性轮换 +
  std::mutex"改为**注入式 + 不可变 + 免锁**——`TlsTicketKeyMaterial{id,bytes,
  created_ms}` 构造期注入,窗口按 created+policy 推导,mint 选 key = 时钟纯函数
  (最新已出生未退役;未出生不用,前置注入 [current,successor] 于出生时刻交接),
  全窗口过 → mint 0 停发(安全降级);删 rotate()/current_key_id()/mutex,增
  random_key() 与 kMaxKeys 溢出取最新 8。动机:stateless 的"只读"直觉对会话态
  成立,对密钥环不成立——注入式让密钥环也真只读,顺带解决重启废票(同物料
  重启票据全活)。重构中修 capped insertion "最新且已满"分支错丢的 bug
  (OverflowKeepsNewestEight 红转绿)。2379 全量绿。
- **2026-09-23 二次重构(用户定谳):删 Impl(pimpl)**。免锁化后 Impl 只剩
  `array<Key,8> + count` 纯数据,唯一作用是挡 openssl/aead.h 出公共头——而该
  约束已有 TlsRecordCipher.h 例外(同样需要完整类型 + 零值=未初始化契约),
  且 TlsConnectedState.h → TlsRecordCipher.h 的传递包含早已把 aead.h 带遍所有
  引擎 TU,再引一次不扩大编译面。现为成员直排:零堆分配、少一层间接、
  valid()==(key_count_!=0)、Key::live 标志删除;失败路径无 delete,只 cleanup
  已 init 的 ctx 并归零 count。API 与测试零改动,2379 全量绿。
- **2026-09-23 slice 2:lookup 半边 + 恢复接线**。TlsConfig.h(TlsResumptionLookup
  +name/+now 两参,借用契约修正为"返回后消费")、TlsTicketService::lookup()/
  lookup_thunk(thread_local 暂存,见 §7)、Tls13ServerHandshake try_accept_psk 接
  线(identity[0] miss → PskOutcome::Reject 全握手回退)、单测 2 + e2e 3 + EE 重钉
  (见 §8)。自查修复:thunk 首版栈上 TlsTicketContents 返回即悬空(引擎在 hook
  返回后才读 resumed,测试红:psk 读到栈残骸、binder over garbage)→ 改暂存格。
  **07 遗留 wire bug 一并修复(EE server_name ack)**:07 的 EE 编码器写 2 字节空
  ServerNameList(RFC 6066 1.2 形态 `00 00 00 02 00 00`),而 RFC 8446 §4.2.1
  要求 1.3 EE 的 server_name ack 为**零长度扩展体**(无 ServerNameList)。此前未
  暴露:07 的 1.3 BoringSSL 互驱从不发 SNI(send_sni 只用于 1.2/双引擎测试),自家
  06 client decode 对 server_name 走 default 全忽略——两端口对称地错。slice 2 的
  send_sni e2e 一上 BoringSSL 即报 ERROR_PARSING_EXTENSION(ext 0;extensions.cc
  `CBS_len(contents)==0` 严格校验)。修四处:编码器 ack 分支(零长度)、头注释、
  decode 严格化(带载荷 → Invalid)、单测重钉 + 负测。2384 全量绿。
- **2026-09-23 定谳:客户端取票缓存不做**(无需求)——06 客户端不缓存票据、
  不发 PSK offer;后续仅剩 1.2 abbreviated 服务端与 09 装配。
- **2026-09-23 slice 3:1.2 abbreviated 服务端**。TlsConfig.h(TlsResumedSession
  +version,psk 双语义注)、lookup thunk version-blind 化(版本门上提两引擎,
  §7)、Tls12ServerHandshake 双形态(Mount +resumption、try_resume_12 接受级联、
  send_abbreviated_flight_12、resumed12_ 客户端 Fin 直达完成、恢复恒轮换)、
  共享 `tls_ch_offers_alpn`、1.2 发票门收紧为 `minter && has_session_ticket &&
  ems_negotiated_`(EMS 走 mint 门,格式零改动)、单测 +1 + e2e 2(§9)。
  自查修复 **sid 回显 wire bug**:全握手回退曾照抄回显 CH 的非空 sid——持票
  客户端以 sid 匹配判"已恢复"、期待 abbreviated flight,收到 Certificate 即
  UNEXPECTED_MESSAGE(handshake.cc:117 "got type 11, wanted type 4";
  CrossVhostTicket12FallsBackToFull 红)。修为全握手恒发空 sid(§8 语义)。
  2387 全量绿(两轮)。
