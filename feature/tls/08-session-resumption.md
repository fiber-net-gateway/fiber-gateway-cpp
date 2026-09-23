# TLS 自研实现 · 08 会话恢复(slice 1:无状态双版本 NST 下发)

## 1. 范围与定谳

08 原案(会话恢复全案)在本 slice 收窄为**服务端 NewSessionTicket 下发**,TLS 1.3 与
TLS 1.2 双版本,并按用户定谳做成**无状态**:服务端不存储任何已下发票据;票据本体即
全部恢复状态,以 ticket protection key(TPK)做 AEAD 加密认证;恢复时服务端只验证
票据合法性,不存在"是否下发过"的记忆判断。

后移到后续 slice 的内容:

- lookup 半边 + 恢复接线(本文 §7 已冻结 open() 验证清单,届时**零格式改动**);
- 客户端取票入会话缓存(06 引擎侧);
- 1.2 abbreviated handshake 双侧(RFC 5077 恢复路径);
- 09 net 集成换芯时的服务装配(每 loop 一个 service 或跨 loop 共享,§5)。

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

## 7. 冻结的 lookup 验证清单(后续 slice 的验收基线,零格式改动)

将来 `TlsResumptionLookup` 的真实现 = `open(identity, CH 的 SNI, now, contents)`
+ 引擎侧既有 binder/age 门,具体:

1. open Rejected → 全握手回退(PSK identity 拒);
2. open Expired → 同上(区别仅在语义与统计);
3. contents.version ≠ 引擎协商版本 → 拒;
4. contents.suite ∉ 本连接可用 → 拒(07 已有 suite 一致性门);
5. 1.3 恢复用 contents.secret 作 PSK 走 PSK+DHE(binder 恒校验);
6. max_early_data == 0(无状态期恒 0)⇒ 不发 early_data 扩展。

## 8. 测试(2379 全绿;票据面 16)

`tests/TlsTicketServiceTest.cpp`(14 个单测):
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

## 9. 待拍板 / 遗留

- 装配形态(09):每 loop 一个 service(免跨线程,物料同种子)vs 全局共享
  ——免锁重构后两者代码路径完全一致,纯归属选择;倾向每 loop(与 EventLoop
  归属一致)。TPK 物料来源(配置文件/seed)与分发是 09 装配题。
- 1.2 abbreviated 双侧与客户端缓存接线随 lookup slice 一起做。

## 10. 实施记录

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
