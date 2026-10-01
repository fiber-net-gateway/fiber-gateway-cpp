# TLS 自研实现 · 07 服务端握手引擎

日期：2026-09-22
分支：`tls`
状态：**已批准，实施中**（§10 九项已定案，2026-09-22；实施记录随 P0–P5 追加于 §11）
前置：02/02b/02c（crypto 与签名证书）、03/04/05（record 与 codec）、06（客户端握手引擎）已实现。

## 1. 定位与边界

`TlsServerHandshakeEngine` 是服务端完整握手 FSM：吃客户端字节，吐服务端字节，
成功时产出 `TlsConnectedState`。与 06 客户端引擎同形（01 §3.3 三相位引擎的第二相位）：
同步、无 fd、无协程、无时钟；引擎头在 src 侧（库内胶水使用），内部状态按值持有。

**做什么**：

| 能力 | 说明 |
|---|---|
| 1.3 全握手 | SH/EE/Cert/CV/Fin 五消息服务端 flight + 客户端 flight 验证 |
| 1.3 HRR | 服务端偏好组客户端无 share 时发 HRR，验 CH2 |
| 1.3 PSK 恢复 | ticket 查找钩子 + binder 验证 + 年龄差检查 + resumption |
| 1.3 0-RTT | 接受级联判定、early data 读取（14336 预算）、EOED、拒绝时 skip |
| 1.3 mTLS | CertReq（context=0）+ 客户端 Cert/CV + tls_verify_chain(SslClient) |
| 1.3 NST | 经 ticket 铸造钩子签发（无钩子则不发） |
| 1.2 全握手 | ECDHE SKE/CKE、EMS、renegotiation_info、ALPN-in-SH、CCS 时序、mTLS、5077 NST |
| 版本判定 | CH 读点：supported_versions 含 0x0304 → 1.3；否则 legacy ≥0x0303 → 1.2 |
| DOWNGRD sentinel | 协商 1.2 时 random[24..32] 写 "DOWNGRD\x01"（我方恒支持 1.3，见 §2.2） |

**不做什么**（边界）：

| 不做 | 归属 |
|---|---|
| ticket 加密/解密、session 缓存、anti-replay 数据库 | 08（本篇只定义借用钩子，§3.2） |
| 1.2 session-id 恢复与 5077 ticket 解密恢复 | 08（07 对 1.2 恒走全握手；NST 照发） |
| fd/事件循环/超时/时钟 | 09（net 胶水取 EventLoop::now） |
| KeyUpdate、post-handshake 消息、close_notify | ConnectedEngine |
| Cookie（HRR 不带） | 非目标（本版 BoringSSL 也不发，§2.3） |
| half-RTT ticket | 不做（§2.8，client Fin 后发 NST，协议合法） |
| ALPS/ECH/ChannelID/NPN/OCSP Stapling | 非目标 |
| SNI 虚拟主机选择（多证书） | 09/net（07 单证书，SNI 只用于回显，§10.3） |
| 静态 RSA / CBC / FFDHE / DSA | 01 既定非目标 |

## 2. 事实依据（BoringSSL 已核实，temp/_deps_asan/boringssl-src）

### 2.1 ServerHello（1.3）编码与密钥时点

`tls13_server.cc:906` do_send_server_hello：legacy_version 恒 0x0303、32B 随机、
session_id 回显 CH 的、cipher id、compression=0。扩展只含三选：
key_share（服务端 share）、supported_versions(0x0304)、pre_shared_key（PSK 命中时，
RFC 要求必须最后一个）。扩展经 `ssl_add_serverhello_tlsext`（extensions.cc:4024）
按 kExtensions 表序输出（key_share 在 supported_versions 前，pre_shared_key 表尾）。

SH emit 后立刻 `tls13_derive_handshake_secrets` + 装 server_hs **write** cipher——
EE 起全部密封。cert_request = verify_mode 含 PEER 且**非 session_reused**
（tls13_server.cc:1042 邻域）。CertReq 1.3 = 1 字节 context=0 + 扩展只含
signature_algorithms（tls12_add_verify_sigalgs）。

**compat CCS 位置规则**：SH 之后、EE 之前，且仅当 `!used_hello_retry_request &&
add_change_cipher_spec`——HRR 已经发过一次 CCS，真 SH 不再补。

### 2.2 版本协商与 DOWNGRD sentinel

- `ssl_versions.cc` ssl_negotiate_version：服务端偏好序（高到低）×对端
  supported_versions 列表取最高公共；无 supported_versions 走 legacy 路径
  （client_version ≥ 0x0303 → 1.2）。
- **sentinel 存在**（纠正本会话早期结论——grep "DOWNGRD" 找不到是因为常量是
  hex 数组）：`tls13_both.cc:47-52` 定义
  `kTLS12DowngradeRandom = "DOWNGRD\x00"`、`kTLS13DowngradeRandom = "DOWNGRD\x01"`；
  `handshake_server.cc:923-935`：max_version ≥ 1.3 的服务端协商出 1.2 时，
  copy_suffix(server_random, kTLS13DowngradeRandom)——写 random[24..32]。
  我们恒 max=1.3 → 协商 1.2 时必写 `\x01`。06 客户端已检查该 sentinel，两端自洽。
- 1.2 server_random 其余 24 字节 = BE32(gmt_unix_time) || RAND(28)（同式前 4 字节
  取当前时间）；1.3 SH random = 全随机 32B。

### 2.3 HRR

触发（do_select_session / tls1_get_shared_group）：按**服务端偏好**在客户端
supported_groups 里选组；客户端 key_share 无该组 share → 发 HRR（BoringSSL 宁可
HRR 也不用客户端次选组的 share）；无任何共享组 → handshake_failure。

形态（do_send_hello_retry_request）：SH 外壳 + kHelloRetryRequest 哨兵 random
（tls13_both.cc:40，cf21ad74…339c，与 04 codec 常量一致）+ session_id 回显 +
CH1 选定的 cipher；扩展仅 supported_versions + key_share(selected_group 空载荷)；
**无 cookie**（本版从不发）。HRR 后跟 CCS；used_hello_retry_request=true。

CH2（do_read_second_client_hello）：协商参数（版本/suite）以 CH1 为准不变；
session_reused 时 PSK 必须重发（缺失 → illegal_parameter）且第二个 binder 重算
验证（不符 → decrypt_error）；resolve_ecdhe_secret 用 CH2 的 share；CH2 后同
flight 多余数据 → unexpected_message。

### 2.4 PSK 选择与 0-RTT 接受级联（do_select_session，顺序）

1. pre_shared_key 无 psk_key_exchange_modes → missing_extension；
2. 客户端无 psk_dhe_ke → 忽略 ticket 走全握手；
3. ticket 查表（08 钩子）miss → 全握手；
4. **binder 验证**（对截断到 binder 块前的 CH 重算）不符 → decrypt_error；
5. age 折算：client_age = (obfuscated_age − ticket_age_add) / 1000；
   skew = client_age − server_age（server_age = now − 签发时刻）；
   |skew| > **60 秒**（kMaxTicketAgeSkewSeconds，tls13_server.cc:44）→ 拒绝恢复；
6. 0-RTT 接受还要求（全部满足）：enable_early_data 开、对端 offer 了 early_data、
   恢复成功、ticket 的 max_early_data > 0、**ALPN == ticket 内 early_alpn**、
   无需 HRR；
7. 0-RTT 拒绝但对端发了 early data → skip_early_data：丢弃（不解密）直到客户端
   第一飞（EOED 在被丢弃的数据里）。

调度初始化序：tls13_init_key_schedule(psk 或 zeros) → 哈希 CH（binder 已先行
单独验证）；接受 0-RTT 才 derive early secret。

### 2.5 密钥与 flight 时点（1.3 服务端）

| 时点 | 动作 |
|---|---|
| CH 验完（含 binder） | set_psk(psk 或 zeros)；transcript 从 CH 起跑 |
| 选组后 | `TlsKeyExchange::create(group)` + **`encap(客户端 share)`**（02c 落的接口，首次生产消费方）→ z + 服务端 share |
| SH emit 后 | handshake_secrets(z, Hash(CH..SH)) → server_hs 做 write（EE/Cert/CV/Fin 密封）、client_hs 留 read |
| 接受 0-RTT、SH 发出后 | client_early_traffic_secret(Hash(CH)) → early read cipher，in_early_data=true |
| server Fin emit 后 | application_secrets(Hash(CH..server Fin)) → server_app0 write swap |
| EOED（空体，非空 → decode_error） | read 切回 client_hs |
| client Fin 验证过 | client_app0 read swap + resumption_master(Hash(CH..client Fin)) |

客户端 flight 读取（do_read_client_encrypted_extensions:1200 起）：我们不协商
ALPS → 客户端 EE（若发）按 unexpected_message 拒；client Cert：cert_request 时
必读（allow_anonymous = 非 FAIL_IF_NO_PEER_CERT）；随后 tls_verify_chain（无名字
参数，SslClient purpose）；client CV 仅当客户端给了证书（tls13_process_certificate_verify
约束 sigalg 在请求列表内）。

### 2.6 NST（1.3，do_send_new_session_ticket + add_new_session_tickets:151）

- 触发：client Fin 验证后（do_read_client_finished:1347：early data accepted 时
  half-RTT 已发过则跳过——我们没有 half-RTT，恒在此发）。
- 不发条件：客户端未 offer psk_dhe_ke、或无铸造钩子。
- 默认 num_tickets = 2（internal.h:3757），上限 16；每张随机 ticket_age_add。
- 消息体：u32 lifetime（timeout，默认 7200s）+ u32 age_add + u8 前缀 nonce（1 字节
  = 序号 i，每张不同 → 不同 PSK）+ u16 前缀 ticket（opaque，加密产物）+
  extensions{ early_data: u32 max_early_data（enable_early_data 时，值 =
  kMaxEarlyDataAccepted=14336，internal.h:3411）}。
- TLS-over-TCP 中 ticket 推迟到服务端下次 write 才 flush（我们 take_output 拉取
  无此问题，直接随 Done 输出）。

### 2.7 half-RTT ticket（BoringSSL 优化，**不跟随**）

do_send_half_rtt_ticket：仅 early_data_accepted 时预喂 EOED+Fin 进 transcript、
提前 derive resumption secret、握手中途发 ticket。动机是避免 SSL_read 驱动的
write-on-read 死锁。我们是纯拉取（take_output），无此问题——**v1 在 client Fin
验证后发 NST**，协议合法（RFC 8446 §4.6.1：ticket 可在握手完成后任意时点发）。

### 2.8 1.2 服务端流程（handshake_server.cc）

**flight 结构**：
`SH → [Cert] → [CertReq] → SKE → SHD` ←客户端 ` [Cert] → [CV] → CKE → CCS → Fin`
←服务端 ` [NST(明文)] → CCS → Fin(加密)`。

- **SH**（do_send_server_hello:883）：random = gmt||rand 再叠 DOWNGRD\x01（§2.2）；
  session_id：恢复命中时回显 CH 的（08 前 1.2 恒 miss）→ v1 恒生成新随机 id。
  扩展：ems（客户端 offer 过则回显）、renegotiation_info（恒发空 renegotiated，
  extensions.cc ext_ri_add_serverhello：1.2 恒 1 字节 0x00，无论客户端是否
  offer——SCSV/伪造机制兜底）、alpn（1.2 放 SH 非 EE）、session_ticket（5077，
  仅 minter 存在且客户端 offer 时）。
- **CertReq 1.2**（do_send_server_hello_done:1155）：cert_types = {rsa_sign(1),
  ecdsa_sign(64)} + u16 前缀 sigalgs（tls12_add_verify_sigalgs）+
  certificate_authorities 列表（我们无 CA 配置 → 空列表 = 任意 CA 可接受）。
- **SKE**（do_send_server_key_exchange:1085）：ServerECDHParams =
  curve_type(3=named) + u16 group + u8 前缀服务端 point；其后 u16 sigalg + u16
  前缀 signature，签名内容 = client_random ‖ server_random ‖ ServerECDHParams。
  sigalg 由证书 key 类型 ∩ 客户端 signature_algorithms ∩ 我方偏好
  （kTls12SignaturePreference）选取。**服务端 1.2 KX = `generate()`**（此刻没有
  客户端 point）；客户端 CKE 到达才 `decap()`。
- **suite 选择**（choose_cipher:156）：prio × allow 交集；我方取**服务端偏好序**
  （§10.2 待拍板）。可用集先按证书 key 类型过滤（RSA 链 → C02F/C030/CCA8；
  ECDSA 链 → C02B/C02C/CCA9），且**无共享组时 1.2 ECDHE 全灭** → handshake_failure
  （无静态 RSA 兜底）。
- **CKE**（do_read_client_key_exchange:1274）：u8 前缀客户端 point，解出后
  decap → z；随即 master_secret（EMS iff 协商了 ems）派生。
- **客户端 CCS**（do_process_change_cipher_spec:1577）：装 client 侧 read cipher。
- **Fin**（do_send_server_finished:1670）：先 NST（若 ticket_expected：u32
  lifetime + u16 ticket，**明文**——在服务端 CCS 之前）→ 服务端 CCS → 装 server
  侧 write cipher → 加密 Fin（verify_data = tls12_verify_data(server)）。
- 客户端 Fin 验证后握手完成。

### 2.9 防护上限（继承 06 的边界）

- ctx 既有：握手重组 4 记录 / 4 MiB 上限、空记录规则；
- early data 接受预算 **14336 B**（kMaxEarlyDataAccepted，s3_pkt.cc:275：单记录
  超剩余预算 → fatal unexpected_message）；
- ticket identity 长度上限：RFC 无硬性规定，取 4096 B（我方铸造的 ticket 远小；
  超限 → illegal_parameter 拒绝该 ticket 走全握手）；
- session_id ≤ 32（codec 既有）、compression 必须 [0]（1.3/1.2 同）。

## 3. 引擎 API

### 3.1 TlsServerHandshakeEngine（include/fiber/tls/handshake/，与 06 头同构）

```cpp
class TlsServerHandshakeEngine final : public common::NonCopyable, public common::NonMovable {
public:
    enum class Event : std::uint8_t { None, HandshakeDone, Failed }; // 同 06 三值

    // config/resumption/minter 全部借用，须活得比引擎久（net 胶水持有）。
    // resumption/minter == nullptr：不查恢复、不发 NST（仍可被客户端 PSK 攻击
    // 路径安全降级为全握手）。pool 语义同 06。
    TlsServerHandshakeEngine(const TlsServerConfig &config,
                             const TlsResumptionLookup *resumption,
                             const TlsTicketMinter *minter,
                             mem::IoBufNodePool &pool) noexcept;
    ~TlsServerHandshakeEngine();

    // 客户端字节进（任意分块）。NoMem = 连接级失败；喂终态引擎 = FIBER_ASSERT。
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBufChain &&bytes) noexcept;

    // 0-RTT 读路径：已解密的 early data 明文（仅接受时非空；上限 14336）。
    // glue 在 HandshakeDone 前后都 drain；第二次 take 得空链。
    [[nodiscard]] mem::IoBufChain take_early_data() noexcept;

    [[nodiscard]] mem::IoBufChain take_output() noexcept; // 同 06
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] TlsAlertDesc failure_alert() const noexcept;
    [[nodiscard]] TlsConnectedState take_state() noexcept;
};
```

与 06 的镜像差异（其余逐字段同形）：

| 差异 | 原因 |
|---|---|
| **构造不产首飞** | 服务端首飞由 CH 驱动；构造后 out 为空，第一次 feed(CH) 才出 SH |
| `take_early_data()` 替代 `write_early_data()` | 0-RTT 方向翻转：客户端写、服务端读 |
| 恢复/铸造钩子替代 `TlsSessionOffer` | 客户端带恢复参数进来；服务端要**查**（恢复）与**铸**（NST） |

### 3.2 TlsServerConfig + 08 边界钩子（TlsConfig.h 增补，接口级草案，08 定稿）

```cpp
struct TlsServerConfig {
    const TlsCertificateChain *chain = nullptr; // 必需（无 PSK-only 模式）
    const TlsPrivateKey *key = nullptr;         // 与 chain 配对（02b）
    std::span<const std::string_view> alpn;     // 空 = 忽略客户端 ALPN
    // mTLS：trust == null 不请求客户端证书；require 决定空证书是否致命
    const TlsTrustStore *client_trust = nullptr;
    bool require_client_cert = false;
    std::int64_t now_unix_ms = 0;               // 证书有效期 + ticket 年龄快照
    bool enable_early_data = false;             // 0-RTT 总开关（anti-replay 未就绪前默认关）
    std::uint32_t session_timeout_s = 7200;     // NST lifetime
};

// 恢复查询钩子：identity → 已解密的恢复参数投影（08 的真实现含 ticket 解密
// 与 anti-replay；测试侧用内存表）。返回 false = miss。
struct TlsResumedSession {                    // 全部借用视图，lookup 调用方持有
    std::span<const std::uint8_t> psk;        // resumption PSK（08 从 NST 推导）
    TlsCipherSuiteId suite;                   // 原 suite（绑定 binder 与 transcript hash）
    std::string_view alpn;                    // ticket 记录的 early_alpn（空 = 无）
    std::uint32_t ticket_age_add = 0;
    std::uint32_t max_early_data = 0;         // 0 = 该 ticket 不允许 0-RTT
    std::int64_t ticket_issued_ms = 0;        // 算年龄差用
};
struct TlsResumptionLookup {
    bool (*lookup)(void *ctx, std::span<const std::uint8_t> identity,
                   TlsResumedSession &out) noexcept;
    void *ctx = nullptr;
};

// ticket 铸造钩子：一张 opaque ticket 写入 out，返回长度；0 = 本次不发。
// 08 的真实现做 AEAD 加密；测试侧把 TlsTicketRequest 序列化进 ticket 即可
// （ticket 对客户端是不透明 blob，互通不要求格式）。
struct TlsTicketRequest {
    std::span<const std::uint8_t> resumption_master; // 本连接 resumption secret
    std::uint8_t ticket_nonce;                        // = 序号 i（1.3）
    TlsCipherSuiteId suite;
    std::string_view alpn;                            // 协商结果（写进 early_alpn 语义）
    std::uint32_t max_early_data;                     // enable_early_data ? 14336 : 0
    std::uint32_t timeout_s;
    std::int64_t now_unix_ms;
};
struct TlsTicketMinter {
    std::size_t (*mint)(void *ctx, const TlsTicketRequest &,
                        std::span<std::uint8_t> out) noexcept;
    void *ctx = nullptr;
};
```

## 4. FSM

### 4.1 外壳（TlsServerHandshakeEngine.cpp，镜像 06 外壳）

```
Construct ──► WaitClientHello ──feed──► CH 完整重组/decode（ctx Plaintext13 起步）
                 │
                 ├─ supported_versions 含 0x0304 ──► mount Tls13ServerHandshake.start(ch)
                 ├─ 无 supported_versions 且 legacy ≥0x0303 ──► mount Tls12ServerHandshake.start(ch)
                 └─ 其他（版本不可谈 / 记录版本 <0x0300 / compression ≠ 0）──► protocol_version 等 alert
此后 on_message/on_ccs 路由进子流程；out_ 累积 SH flight；Done/Failed 经共享 outcome。
```

外壳持有（06 同位）：ctx、variant 子流程、`TlsServerHelloState`（CH 视图 + 协商
中间态 + `unique_ptr<TlsKeyExchange>` kx + 双 random）、outcome、服务端偏好表。
版本判定后 `ctx.set_legacy_version(0x0303)`。

**inbound 记录版本复核**（实施项）：CH 常以 record version 0x0301 分帧（旧客户端），
negotiation 前 inbound 需接受 0x0300–0x0303，SH 发出后锁定 0x0303——核对
ctx/Reader 现行行为，紧了就放宽（只在 Plaintext 模式）。

### 4.2 1.3 子流程（Tls13ServerHandshake，状态表）

| 状态 | 期望/动作 | 违例 → alert |
|---|---|---|
| SelectParams | 组选择（服务端偏好 × CH supported_groups）；PSK 查表+binder+age；CH1 无该组 share → SendHrr；无共享组 → handshake_failure | §2.4 各条 |
| SendHrr | HRR 外壳（哨兵 random）+ CCS；→ WaitClientHello2 | — |
| WaitClientHello2 | CH2（版本/suite 与 CH1 一致性、PSK 重发、第二 binder、share）；→ SendServerHello | illegal_parameter / decrypt_error / unexpected_message |
| SendServerHello | set_psk（miss 时 zeros）→ 哈希 CH（HRR 时 transcript=message_hash 重启）→ encap → z → SH（session_id 回显；非 HRR 路径补 compat CCS）→ handshake_secrets → server_hs write → SendFlight | — |
| SendFlight | EE（server_name 空回显 iff SNI、alpn、early_data 空ext iff 接受）→ [CertReq] → Cert（der 零拷贝重发）→ CV（tls_sign over "TLS 1.3, server CertificateVerify"）→ Fin；server_app0 write swap → WaitClientFlight | — |
| WaitClientFlight | 接受 0-RTT：early read cipher、in_early_data、明文进 early_ 缓冲（预算 14336）；拒绝但 offer 过：skip（不解密丢弃 application_data 外层记录）；EOED → client_hs read | 超 → unexpected_message；EOED 非空体 → decode_error |
| [WaitClientCert] | mTLS：Cert（context 必须 == 请求的）；空证书：require → certificate_required，否则跳过 CV | unexpected_message / certificate_required |
| [VerifyClientCert] | tls_verify_chain(SslClient, 无名) 不信 → 其 alert（bad_certificate 等） | §2.5 |
| [WaitClientCv] | CV：sigalg ∈ 请求列表；验签过 | illegal_parameter / decrypt_error |
| WaitClientFin | 常数时间验 verify_data（0-RTT 接受时 transcript 已预含 EOED）；client_app0 read + resumption_master → SendNst → Done | decrypt_error |

### 4.3 1.2 子流程（Tls12ServerHandshake，状态表）

| 状态 | 动作 / 违例 alert |
|---|---|
| SendServerHello | random（gmt+rand+DOWNGRD\x01）、suite（服务端偏好 × 证书 key 类型过滤）、扩展 ems/ri/alpn/session_ticket；→ SendFlight |
| SendFlight | [Cert] → [CertReq] → SKE（kx `generate()`；签名 CR‖SR‖params）→ SHD；→ WaitClientFlight |
| WaitClientFlight | [Cert（空：require → certificate_required）] → [CV] → CKE（u8 point → `decap` → z → master+EMS）→ CCS（装 client read）→ client Fin（常数时间验）→ SendFinished |
| SendFinished | [NST 明文（minter 且 5077 offer 时）] → CCS → 装 server write → Fin；→ Done |
| 违例映射 | CKE 坏 point → illegal_parameter/decode_error；CV 验签失败 → decrypt_error；Fin 不符 → decrypt_error；中途 CCS 序错 → unexpected_message |

1.2 的握手 transcript 用 `TlsTranscript12`（handshake_messages 缓冲，客户端
CV/Fin 的 PRF 输入），1.3 用 `TlsTranscript13`——两者 06 已备好，直接复用。

### 4.4 期望表（两子流程共用的"消息序违例"规则）

- 任一状态收到不期望的握手类型 → unexpected_message；
- 密文 flight 阶段收到明文握手 → unexpected_message（ctx 模式机已拦）；
- 1.3 客户端 EE → unexpected_message（我们不协商 ALPS）；
- 任一 fatal alert 进站 → Failed（alert 记录进 failure_alert）；
- close_notify 中途 → Failed（同 06 语义）。

## 5. 密钥时点与 KX 消费点（汇总表）

| 版本 | 时点 | KX 调用 | schedule 调用 | cipher swap |
|---|---|---|---|---|
| 1.3 | CH 验完 | — | set_psk | — |
| 1.3 | SH 构建 | **encap**（首个生产消费方，02c §5.1 预留） | handshake_secrets | server_hs → write |
| 1.3 | 0-RTT 接受 | — | client_early_traffic_secret | early → read |
| 1.3 | server Fin 后 | — | application_secrets | server_app0 → write |
| 1.3 | client Fin 后 | — | resumption_master | client_app0 → read |
| 1.2 | SKE 构建 | **generate** | — | — |
| 1.2 | CKE | **decap** | tls12_master_secret（EMS iff 协商） | （key_block 派生备好） |
| 1.2 | 客户端 CCS | — | — | client keys → read |
| 1.2 | 服务端 CCS 前 | — | — | server keys → write |

swap 全部走 06 的 swap_cipher 形态（fresh 实例 + 派生后 wipe keys）。

## 6. 共享件与 codec 补齐

### 6.1 TlsHandshakeContext 复用复核（不改契约，最多两处放宽）

| 复核点 | 预期 |
|---|---|
| bind/feed/step/emit/send_ccs/set_inbound_mode/take_output | 服务端原样用 |
| Plaintext13 首记录版本 | 需接受 0x0300–0x0303（§4.1）；若现行锁死则放宽且仅 Plaintext 模式 |
| Sealed13/Sealed12 模式切换 | 06 已走通，服务端镜像 |

新文件（镜像 client 侧组织）：

```
src/tls/handshake/TlsServerHandshakeEngine.h             # §3.1
src/tls/handshake/TlsServerHandshakeEngine.cpp           # 外壳 + 版本判定
src/tls/handshake/Tls13ServerHandshake.{h,cpp}           # 1.3 子流程（Mount 形态同 06）
src/tls/handshake/Tls12ServerHandshake.{h,cpp}           # 1.2 子流程
src/tls/handshake/TlsServerHandshakeShared.{h,cpp}       # 偏好表/CH 状态/outcome/共享 helper
```

`TlsServerHandshakeShared`：`kServerSuites`（同 9 suite，语义 = 服务端偏好序）、
`kServerGroups`{X25519, P-256}、`TlsServerHelloState`（CH POD 借视图 + random×2 +
`unique_ptr<TlsKeyExchange>`）、`TlsServerHandshakeOutcome`（同 client outcome）+
helper（组选择、suite×证书 key 类型过滤、sigalg 选择、SH/EE 扩展编码）。

### 6.2 codec 补齐（TlsHandshakeCodec，P0）

| 方向 | 函数 | 说明 |
|---|---|---|
| encode | tls_encode_server_hello | 3 形态：1.3-SH / HRR / 1.2-SH（扩展序：1.3 = key_share, supported_versions, [pre_shared_key 表尾]；1.2 = [ems][renegotiate][alpn][session_ticket]） |
| encode | tls_encode_encrypted_extensions | server_name 空回显 / alpn / early_data 空 ext |
| encode | tls_encode_certificate_request_13 | context=0 + signature_algorithms |
| encode | tls_encode_certificate_request_12 | types{1,64} + sigalgs + CA 列表（空） |
| encode | tls_encode_server_key_exchange | ServerECDHParams + sigalg + signature |
| encode | tls_encode_new_session_ticket | 1.3（lifetime/age_add/nonce/ticket/ext-early_data）与 1.2（lifetime/ticket）两形态 |
| decode | tls_decode_client_key_exchange | u8 前缀 point |
| helper | tls_key_share_find / tls_psk_identity_at | 遍历 CH 的 key_share_entries / psk_identities 原始 span（现 POD 只存块） |

复用不动：tls_decode_client_hello（04 已备好全部服务端输入字段，含
psk_binder_block_offset/has_early_data）、tls_encode_certificate_13/12（P0 复核
context 参数化：服务端发送 context=""、客户端 mTLS 回显——若签名已带 context
参数则零改动）、tls_encode_certificate_verify、tls_encode_finished、
tls_encode_client_key_exchange（1.2 客户端侧已有，decode 是新面）。

## 7. 安全要点

1. **binder / verify_data / client Fin 比较一律常数时间**（tls_constant_time_equal，
   02 层既有）；
2. 0-RTT 接受 = 重放暴露：**enable_early_data 默认 false**，08 的 anti-replay
   钩子就绪前生产开启属配置责任（文档 + 09 配置面警示）；
3. age skew 60s + ALPN 一致 + 无 HRR 才接受 0-RTT（§2.4 级联全表落实）；
4. early data 预算 14336 硬顶，超限 fatal——不允许无界缓冲；
5. 拒绝路径的 skip-early-data 不解密不缓冲（无密钥也不存在时序侧信道）；
6. resumption_master 在 NST 铸完后 wipe（ ConnectedState 不携带它，1.3 客户端
   才需要）；
7. 服务端 CV 签名走 02b `TlsPrivateKey::sign`（FIBER_ASSERT supports 先查）；
8. CH 重组上限沿用 ctx（4 记录/4 MiB）——DoS 面 06 已定，服务端不放宽。

## 8. 测试计划（tests/TlsServerHandshakeEngineTest.cpp 等）

### 8.1 单元（codec P0 随行）

- SH×3/EE/CertReq×2/SKE/NST encode 向量（手工构造 + 我方 decode 回读 round-trip）；
  CKE decode；key_share/psk_identity helper 遍历（含畸形截断）。

### 8.2 引擎对驱（内存回环，我方 06 客户端 ↔ 07 服务端）

1.3：全握手（X25519）、P-256-only 客户端触发 HRR、mTLS 三态（无请求/可选空证书/
必要求证书）、ALPN 命中与 no_application_protocol、PSK 恢复（内存 lookup 表）、
binder 错误、age 超限拒绝恢复、0-RTT 接受（early data 到达+EOED）、0-RTT 拒绝
（skip 路径、early data 被弃）、early data 超预算、客户端 Fin 错误、
无 psk_dhe_ke 客户端（旧栈形态）不发 NST。
1.2：全握手（RSA 链与 ECDSA 链各一，sigalg 路径分开覆盖）、EMS 协商、
renegotiation_info 回显、mTLS、NST(5077)、客户端 Fin 错误、坏 CKE point。
双向死亡/契约：喂终态引擎等（少量，06 同位）。

### 8.3 BoringSSL 互驱（s_client 形态：SSL 对象 + BIO_s_mem，BoringSSL 当客户端）

06 的互驱 harness 反转角色即可：1.3 全握手+ALPN、1.2 全握手、HRR（用
SSL_set1_groups 限制客户端组）、mTLS（SSL_use_certificate）、
**PSK 恢复**（收我方 NST → SSL_SESSION 复用第二次连接——ticket 是不透明 blob，
测试 minter 序列化即互通）、0-RTT（BoringSSL early data 写）、坏 binder /
坏 Fin 被正确 alert。
另跑旧栈 TlsStreamFd（BoringSSL 服务端）↔ 新客户端 06 的既有互驱确认无回归。

### 8.4 验收门

每一 P 步全量 ctest 绿；不新增手抄 RFC hex 常量（RFC 8448 §5-§7 的 1.3 服务端
向量经既有 gen8448 流水线扩展生成，方法照 02 §12.2）。

## 9. 实施清单（P0–P5，每步全量绿）

1. **P0** codec 补齐（§6.2 表）+ 单测 + ctx 首记录版本复核；
2. **P1** 1.3 全握手（无 PSK/0-RTT/mTLS）：SH/flight/client Fin → ConnectedState；
   我方 06 客户端对驱 + BoringSSL s_client 互驱；
3. **P2** HRR + CH2 验证 + 防护规则；
4. **P3** mTLS（1.3 CertReq/客户端 Cert/CV + verify_chain）；
5. **P4** 1.2 全握手（SKE 签名、EMS、RI、ALPN、CCS 时序、5077 NST）+ BoringSSL
   1.2 互驱；
6. **P5** 1.3 PSK 恢复 + NST（钩子）+ 0-RTT（接受级联/预算/skip）+ BoringSSL
   恢复与 0-RTT 互驱 + RFC 8448 服务端向量 + 文档收尾。

每步全量 ctest 绿；`./format_code.sh` 收尾一次；不主动提交（用户另行指令）。

## 10. 待拍板问题

1. **mTLS 模式表达**：`client_trust` 指针 + `require_client_cert` bool（推荐——
   对齐 06 的 `verify_peer` bool 风格，null=不请求天然成三态）vs
   `enum class ClientAuth { None, Optional, Required }`（更显式但多一个类型）；
2. **suite 选择序**：服务端偏好（推荐，nginx 默认行为）vs 客户端偏好
   （BoringSSL 默认）。engine-fixed 注册表序（06 同款"不进 config"）；
3. **SNI 不匹配策略**：v1 忽略（单证书服务，SNI 只回显不校验；推荐）vs
   unrecognized_name fatal；多证书虚拟主机留 09；
4. **age skew 阈值**：60s（对齐本版 BoringSSL kMaxTicketAgeSkewSeconds，推荐）vs
   10s（旧版值，更保守）；
5. **early data 预算**：固定 14336（推荐，对齐 BoringSSL）vs 进 config；
6. **anti-replay**：v1 无（enable_early_data 默认 false 承担风险面，08 加钩子）
   ——确认此边界；
7. **half-RTT ticket**：不做（§2.7，client Fin 后发；协议合法且我们无
   write-on-read 问题）——确认此简化；
8. **1.2 恢复**：07 恒全握手，session-id/5077 解密恢复归 08——确认此边界；
9. **NST 数量**：1 张（推荐——我们 take_output 直发，无 BoringSSL 的 flush
  顾虑；两张是 BoringSSL 为丢失冗余的默认）vs 2 张（对齐 BoringSSL）。

九项均按建议定案（用户 2026-09-22 "直接按照建议实现"）：
mTLS = `client_trust` 指针 + `require_client_cert`；suite = 服务端偏好序
（engine-fixed 注册表，不进 config）；SNI = v1 忽略只回显；skew = 60s；
early data 预算 = 固定 14336；anti-replay = v1 无（`enable_early_data`
默认 false 承担风险面，08 加钩子）；half-RTT ticket 不做（client Fin 后发
NST）；1.2 恒全握手（恢复归 08）；NST 恒 1 张（nonce=0）。

## 11. 实施记录（随 P0–P5 追加）

### P0 — codec 补齐 + ctx 复核（2026-09-22，2332 全绿）

- §6.2 表全部落地：`tls_encode_server_hello`（3 形态）/ `_encrypted_extensions`
  / `_certificate_request_13|12` / `_server_key_exchange` / `_new_session_ticket_13|12`、
  `tls_decode_client_key_exchange`、`tls_psk_identity_at`/`tls_psk_binder_at`
  （06 的 `tls_find_client_key_share` 即设计稿的 `tls_key_share_find`，沿用现名）。
  新增 11 个单测（含 1.3 SH 扩展序 {51,43,41}、HRR 哨兵形态、NST13 逐字节）。
- **1.2 CertificateRequest authorities 定谳**：u16 前缀（RFC 4346/5246
  `<0..2^16-1>`）；u24 是 RFC 2246（TLS 1.0）专属。实施中曾误按 u24 写 encoder
  并"顺手修"既有 decoder，被 BoringSSL 互驱测试（真 1.2 mTLS 服务端）当场打回
  ——既有 decoder 本就正确。教训与 02 §12.3.11 同源：wire 格式必须查原文。
- **ctx 首记录版本复核结论**：`TlsRecordReader` inbound 对
  legacy_record_version 恒不拒，"接受 0x0300–0x0303"天然满足，零改动。

### P1 — 1.3 全握手（2026-09-22，2339 全绿）

- 新文件按 §6.1 组织：`TlsServerHandshakeEngine.{h,cpp}`（外壳：无首飞构造、
  pump、CH fork）、`Tls13ServerHandshake.{h,cpp}`（SH→CCS→EE→Cert→CV→Fin +
  WaitClientFin）、`TlsServerHandshakeShared.{h,cpp}`（kServerSuites/kServerGroups/
  TlsServerHelloState{kCap=16384}/outcome + suite/group/alpn/CV-scheme 选择
  helper，全部服务端偏好序）。`TlsConfig.h` 增补 §3.2 全部结构。
- 密钥时点按 §5 表执行：CH 验完 encap（02c 首个生产消费方）→ SH 后
  handshake_secrets（server_hs→write、client_hs→read、Sealed13）→ server Fin 后
  application_secrets（server_app0→write）→ client Fin 验过后
  resumption_master（client_app0→read）。
- fork 版本判定：supported_versions 含 0x0304→1.3；否则 supported_versions 含
  0x0303 或无该扩展且 legacy≥0x0303→1.2（P4 前恒 protocol_version 拒绝）；
  其余 protocol_version。compression≠null 按版本分 alert（1.3
  illegal_parameter / 1.2 handshake_failure）。
- 测试（tests/TlsServerHandshakeEngineTest.cpp，7 个）：BoringClient 内存 BIO
  互驱全握手 + ALPN(h2) + 双向 app-data（SSL_write→read_cipher.open /
  write_cipher.seal→SSL_read）；1 字节切片喂入；ECDSA P-256 凭据（CV scheme
  选 ecdsa_secp256r1_sha256）；我方 06 客户端对驱（双向 FSM + app secret 相等
  + 链方向断言）；1.2-only 拒绝（protocol_version）；非 CH 首消息拒绝
  （unexpected_message）；空凭据构造失败（internal_error）。
- 偏离：无（P1 范围内 HRR/PSK/mTLS/0-RTT 分支留位不实现——share 缺失暂以
  handshake_failure 应答，P2 改 HRR）。

### P2 — HRR + CH2 防护（2026-09-22，2342 全绿）

- 分支结构：start() 在 transcript init+喂 CH1 之后判 share；支持组但无 share
  → `send_hello_retry()`（哨兵 random + supported_versions/key_share(selected_group
  空载荷) 两扩展、无 cookie）+ `restart_message_hash()` + compat CCS（RFC 8446
  D.4：整握手恰一张 CCS——HRR 路径已花费，真 SH 后不再补）→ WaitClientHello2。
- CH2 防护规则（§4.2 表 + RFC 8446 §4.1.2 全集）：echo 字段五元组（legacy_version/
  random/session_id/cipher_suites 原始字节/compression）任一不等 →
  illegal_parameter；supported_versions 仍含 0x0304、无 early_data、无 cookie
  （原始扩展块扫描 type 44——decoder 不暴露该字段）、CH1 offered PSK 则 CH2 必
  重发、key_share 必在 → 否则 illegal_parameter；请求组的 share 仍缺 →
  unexpected_message（无第二次 HRR）。
- CH2 不做保留：decode 借用 ctx body、echo 比对借 hello_.view（CH1 仍在位），
  全部续接（encap→SH flight）在同一 handler 内完成。
- 修复（实现期发现）：HRR 路径 send_server_flight 尾部未把 st_ 推回
  WaitClientFin（直连路径靠成员默认值侥幸成立）——BoringSSL 客户端互驱当场
  捕获（客户端已完成、服务端 unexpected_message）。
- 测试（+3）：BoringClient 组序 "P-256:X25519"（首 share P256、支持 X25519）→
  HRR→CH2(X25519 share)→完成（含哨兵 random 落线断言）；手造 CH 对（codec
  直造）：CH2 仍缺 share → unexpected_message；echo random 翻转 →
  illegal_parameter。

### P3 — 1.3 mTLS（2026-09-22，2346 全绿）

- SendFlight 在 EE 后插入 CertificateRequest（`client_trust != nullptr` 即请求；
  sigalgs = kServerCrSigalgs = 02b 1.3 偏好 7 项，shared 新常量）；st 尾随
  client_trust → WaitClientCert。
- WaitClientCert：context 必须空（我们以空 context 请求）否则 illegal_parameter；
  空链：require → certificate_required(116)，否则跳过 CV 直达 WaitClientFin；
  非空链 from_der_list → tls_verify_chain(SslClient, 空 host/ip, cfg 时钟)，
  NotTrusted → 验证器自带 alert（02b 映射：unrooted=unknown_ca 等）。
- WaitClientCv：scheme ∈ CR 列表（kTls13SignaturePreference）否则
  illegal_parameter；content = 64×0x20 + "TLS 1.3, client CertificateVerify" +
  0x00 + Hash(through client Cert)；tls_verify IoErr→illegal_parameter（scheme
  不配 key）、false→decrypt_error。
- finish 把 peer_chain_ 移入 state_。
- 测试（+4）：BoringClient 带 kClientRsaPem 凭据（required，链 2 张验证通过）；
  required 无凭据 → certificate_required；optional 无凭据 → 成功且空链；
  凭据不锚定（trust=kRootUnrelatedPem）→ unknown_ca（02b 映射，非 bad_certificate）。

### P4 — 1.2 全握手（2026-09-22，2351 全绿）

- 新文件 `Tls12ServerHandshake.{h,cpp}`（Mount 全借用、无 resumption——§10.8）：
  状态机 ExpectClientCert12→ExpectCke12→[ExpectClientCv12]→ExpectClientCcs12→
  ExpectClientFin12→Done；`TlsServerHandshakeShared` 增 `tls_server_suite_select_12`
  （偏好序×offer×is_tls13 过滤×suite_auth 匹配：RSA key→ECDHE-RSA 套、EC key→
  ECDHE-ECDSA 套、Ed25519→false）与 `kServerCr12Sigalgs`（1.2 偏好 10 项）。
  外壳 fork offers12 分支：compression≠0 → handshake_failure（1.3
  illegal_parameter）。
- SH random = gmt(4)+random(20)+DOWNGRD\x01 无条件填（BoringSSL
  ssl_fill_hello_random 语义）；EMS/ticket_wanted 协商后 ALPN 选择入 SH；
  inbound 停 Plaintext12，t12_ 喂 CH。
- **飞行顺序定谳（实现期纠错）**：SH → Cert → SKE → **CR** → SHD。初稿把 CR
  放在 Cert 前，BoringSSL 客户端互驱当场打回（"got type 13, wanted type 11"
  ——收到 CR 时在等 Cert）；RFC 5246 §7.3 的 message 列表 CR 明确在 SKE 之后、
  SHD 之前。transcript 喂入序同步（t12_ 顺序=wire 顺序）。
- SKE 签名内容 = CR‖SR‖curve_type(0x03)+be16(group)+u8(len)+point（非
  transcript digest，与 06 客户端验证字节完全对称）；scheme 走
  tls_server_cv_scheme_select(Tls12)。
- 密钥时点：CKE 接收点 decap→喂→EMS session_hash=活 transcript（含 CKE 不含
  CV，双端对称）或 classic master→key_block→st 推进；CV 验签 t12_.buffer() RAW
  字节、scheme∈CR 列表否则 illegal_parameter、false→decrypt_error；client CCS
  装 read=kb12.client+Sealed12；Fin verify_data 常数时间比对；NST→CCS→write
  swap→server Fin（服务端 Fin MAC 覆盖 NST——1.2 的 NST 在 Fin 之前喂
  transcript）。
- RFC 5077：SH 回显空载荷 session_ticket 扩展（codec 字段 span→bool，
  encoder/decoder/既有 round-trip 测试同步）；ticket_wanted_ = minter≠null &&
  ch.has_session_ticket；TlsTicketRequest.resumption_master 传 master（1.2 的
  恢复密钥就是 master）；mint len∈(0,cap] 才发 NST，len=0 容忍（SH 已回显，
  RFC 5077 允许不发）。
- **既有层缺陷修复（1.2 ChaCha，双向）**：
  1. 05 record 层违反 RFC 7905 §2——1.2 ChaCha 误用 GCM 的 4+8 显式 nonce 形式
     （overhead 24），应为 12 字节隐式 IV XOR seq、wire 无 nonce、overhead 16。
     06 客户端 kOfferedSuites 含 0xCCA8/0xCCA9 → 双向 bug。修复：
     `TlsRecordCipher` 新增 `explicit_nonce_len_`（1.2 GCM=8，1.3 与 1.2
     ChaCha=0）贯穿 init/move/size 四函数/seal/open 两分支/scatter 两分支；
     chain 层（transcribe/in_place/seal）全部经 `explicit_nonce_len()` 取代
     硬编码 8，in_place 收缩三支（GCM consume(8)+trim(16) / 1.2-ChaCha
     trim(16) / 1.3 trim(length-plain_len)）。
  2. 02 key schedule 层 `tls12_key_block` 硬编码 fixed_iv=4——RFC 7905 §2 的
     write_IV 是完整 12 字节隐式 IV。修复：iv_len 按 suite AEAD 分叉（4
     GCM/12 ChaCha），block 上限 72→88（2×32+2×12），client/server 两侧切片
     与 iv_len 同步；TlsSecret.h/Tls12KeySchedule.h 契约注释更新。客户端
     （06）同函数消费，双向一并修复。
  - 测试同步：TlsRecordCipherTest（expected_tls12_payload 加 ChaCha 隐式
    nonce 分支、ChaCha 向量 iv 改 12 字节、Sizes/RoundTrip/Open/scatter 三测
    经 wire_nonce_len() 参数化、1.2 端到端编排显式 offset 化）、
    TlsRecordCipherChainTest（新增 tls12_chacha_vec 进全部 8 个 per-kind
    循环、offset 全经 explicit_nonce_len()）、TlsKeyScheduleTest（key_block
    表加 {Chacha, key 32, iv 12} 用例 + iv_len 断言）。
- 测试（tests/TlsServerHandshakeEngineTest.cpp §8.5，+6）：全握手+双向 app
  data（版本/套件/EMS(SSL_get_extms_support)/ALPN/DOWNGRD 哨兵落线断言）；
  1 字节切片；EC 凭据（0xC02B）；钉选 0xC030 与 0xCCA8（后者即 ChaCha 回归）；
  mTLS required 客户端链验证；CapturingMinter NST（resumption_master 48 字节
  == state.tls12_master、wire 含 ticket blob）。

### P5 — 1.3 PSK 恢复 + NST + 0-RTT + RFC 8448 §5 向量（2026-09-22，全量绿）

- 服务端接线：`TlsResumptionLookup`/`TlsResumedSession`（identity →
  psk/suite/alpn/age_add/max_early_data/issued_ms；lookup 返回借用，引擎不拷贝）。
  start() 在 CH 解析后 `try_accept_psk`（binder 常数时间比对，截断点 =
  pre_shared_key 扩展前；binder_key 每实例一次，故 fresh schedule）；age 检查
  超过 60s skew → 拒 ticket 走全握手（非 fatal）；accepted → SH 回
  selected_identity、`set_psk`、握手密钥双源（psk + 新 z）。
- **NST mint 门 = psk_key_exchange_modes 扩展，与是否 offer PSK 无关**（RFC
  8446 §4.2.9；BoringSSL 客户端恒发 modes standing extension，无 PSK offer 的
  全握手也 mint）。`psk_dhe_ke_offered_` 在 start() 从 CH 直接计算、
  handle_client_hello2 从 CH2 重算（CH2 会丢 CH1 没有的扩展，最终 offer 为准）。
  实现期纠错：初版只在 try_accept_psk 内置位——无 PSK offer 时永不 mint，
  BoringClient 全握手拿不到 ticket 当场暴露。
- 0-RTT 接受级联（§4.6）：enable_early_data ∧ psk_accepted ∧ ch.has_early_data
  ∧ resumed max_early_data>0 ∧ ¬after_hrr ∧ ALPN==resumed_alpn；EE 带
  early_data，read=client_early。**client_early_ 必须在 start() 预派生**：
  schedule 的 client_early_traffic_secret 断言 Stage::Early，而
  send_server_flight 的 handshake_secrets() 先推进阶段（实现期纠错：
  FIBER_ASSERT 当场暴露）。内容预算 14336 超限 → fatal unexpected_message。
  `TlsTicketRequest` 新增 `ticket_age_add`（NST 混淆键，bind 进 ticket 才能
  复现 age 算术）；恒 1 张 nonce=0（§10.9）。
- **half-RTT ticket 定谳**（BoringSSL tls13_server.cc do_send_half_rtt_ticket）：
  0-RTT 接受时服务端在 SH flight 后立即以 server_app keys **write seq 0** 密封
  NST 并与 SH flight 同一次 flush；引擎在 client Fin 处终止时 harness 的
  done-guard 会把 NST 记录丢进 unfed tail——不消费则后续 app 记录 seq 错位
  AuthFail。两侧 harness 统一 **record-granular feed + tail 捕获**（client 测试
  DriveLog.server_tail、server 测试 pump(…, server_tail)），tail 经 connected
  read_cipher open+discard 推进序列。
- 06 客户端一处修正：接受 0-RTT 路径的 EOED 显式走 early cipher 实例
  （`ctx_.emit(…, &early_.write)`——该路径 ctx_.write_cipher() 尚未上线），
  记录序接续 early data。
- **BoringSSL 0-RTT 客户端 API 三行为**（harness 纪录）：带 session+early
  data 首次 SSL_do_handshake 在发出 CH 后 early-return 1（与服务端镜像）；
  HRR/0-RTT 被拒后返回 -1 且 SSL_get_error==SSL_ERROR_EARLY_DATA_REJECTED(15)
  **sticky**，须 `SSL_reset_early_data_reject` 后重试（照 ssl_test.cc）；NST
  消化走 SSL_read。ticket 收集：SSL_SESS_CACHE_CLIENT|NO_INTERNAL_STORE +
  sess_set_new_cb 返回 1 取所有权。
- 测试（§8.2/8.3）：BoringSSL 互驱 8 + 双引擎 2 = 10 ——
  ResumesWithTicket / EarlyDataAccepted（手驱 early 写）/ HrrAfterPskResume
  （groups 限 X25519→P-256；EARLY_DATA_REJECTED→reset→CH2 第二 binder；resumed=1、
  early 拒）/ OurClientResumesOurServer / OurClientEarlyDataAccepted（pump+tail）/
  EarlyDataRejectedWhenDisabled（hop1 enable→hop2 disable 的 skip 路径）/
  EarlyDataOverBudgetFatal（offer 谎报 17408、写 15000 → fatal 10；alert 经 tail
  用 client read cipher 打开验证）/ UnknownTicketFallsBackToFull（miss）/
  BinderMismatchFatal（psk 破坏）/ StaleTicketFallsBackToFull（issued_ms −5min →
  server_age 300s>60s）。TestSessionStore：mint=derive resumption psk、ticket
  blob=8B 大端计数器、全字段入 map；g_new_sessions + CollectedSessionsGuard 管理
  ticket 所有权。
- **RFC 8448 §5 向量**（gen8448 扩展，02 §12.2 纪律，零手抄）：流水线新增 16 项
  §5 校验——h1 = Hash(msg_hash(CH1)‖HRR‖CH2‖**SH2**)（注意 hs traffic 语境哈希
  穿过 SH2 而非止于 CH2）、h2/h3、双向 Fin MAC、res master 全链；
  TlsRfc8448Constants.h 40→61 常量（+CH1/HRR/CH2/SH2 原文 dump）。新测 2：
  TlsTranscript13.Rfc8448Section5RestartMatchesTrace（§4.4.1 重启构造对 RFC
  端到端钉死）、TlsKeySchedule13.Rfc8448Section5HrrTree（staged 机消费重启
  transcript）。§5 无 PSK offer——RFC 8448 不含 PSK+HRR 组合，第二 binder 的
  重启 transcript 组合由 BoringClientHrrAfterPskResume 互操钉死（binder 构造
  本身 §4 常量已有）。§6（client auth）/§7（compat）不引入新派生规则，已由
  P3/P4 互操覆盖，不重复出向量。
- **测试代码三处生命周期缺陷修复**（全量顺序跑才复现的 MalformedTable
  "vector" 异常根因；ASan 定位）：①ServerHelloDecode.MalformedTable 两处
  `plain_sh_body().begin()/plain_sh_body().end()` 双临时迭代器跨界（垃圾长度 →
  length_error("vector") 或堆越读，heap-layout 依赖）；②ServerFlightEncode 两测
  `in.session_id = std::vector<…>(…)` —— TlsServerHelloInput 是借用 span 结构，
  临时 vector 语句尾析构 → encoder 内 heap-use-after-free。修：具名局部保活。
  全量顺序跑 2067 绿、ASan 全量（除既有 Http3 UAF 一例）2066 绿、ctest
  2361/2361。
- **07 遗留 wire bug（08 slice 2 修复，2026-09-23）：EE server_name ack 形态**。
  本档 §7 EE 编码器把 server_name ack 写成 2 字节空 ServerNameList（RFC 6066
  的 1.2 形态 `00 00 00 02 00 00`）；RFC 8446 §4.2.1 要求 1.3 EE 的 ack 为**零
  长度扩展体**。当时未暴露：07 的 1.3 BoringSSL 互驱从不发 SNI，自家 06 client
  decode 对 server_name 走 default 全忽略——收发两端口对称地错，互驱全绿。08
  slice 2 的 send_sni e2e 一上真 BoringSSL 客户端即 ERROR_PARSING_EXTENSION
  （extensions.cc 对 server_name 要求 `CBS_len(contents)==0`）。已修：编码器
  零长度 ack + decode 严格化（带载荷 → Invalid）+ 单测重钉/负测，详见 08 §10。
