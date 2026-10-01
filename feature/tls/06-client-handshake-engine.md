# TLS 自研实现 · 06 客户端握手引擎（TlsClientHandshakeEngine）

日期：2026-09-21
分支：`tls`
状态：**已实现（实现记录见 §10 末）**

上游输入：01 修订 2（三相位引擎）· 02/02b（crypto/密钥调度/签名证书）· 03（分帧 Reader/Writer）·
04（codec，decode 侧仅 ClientHello）· 05（记录保护 + 链适配层）。

## 1. 定位与边界

`handshake/TlsClientHandshakeEngine`：客户端握手相位的完整 FSM。构造即产出第一飞
（ClientHello ± PSK binder ± middlebox CCS）；feed 对端字节推进；成功以
`Event::HandshakeDone` 交付 `TlsConnectedState`，失败以 `Event::Failed` 终态
（alert 已编码进出向缓冲）。同步、无 fd、无协程、无时钟——全部状态推进由 feed 驱动。

### 1.1 引擎做什么 / 不做什么

| 做 | 不做 |
|---|---|
| CH 编码（1.3 offered_versions 含 1.2；版本无起点，SH 读点才定版本） | 任何 socket / fd / event loop / 定时器（09 的胶水层驱动） |
| 1.3：SH→[HRR 循环]→EE→[Cert→CV]→Fin；client Fin [→EOED] 的消费与产出 | 1.2 重协商（HelloRequest 到达 → `no_renegotiation` warning，归 ConnectedEngine 钩子） |
| 1.2：SH→[Cert→SKE→[CertReq]→SHD]→client flight [Cert]/CKE/[CV]/CCS/Fin→server CCS+Fin 验证 | post-handshake 消息（NST/KeyUpdate/post CertReq——ConnectedEngine 与 08） |
| 0-RTT **写路径**：early data seal/发送、接受/拒绝判定、拒绝回滚（01 修订 2：封引擎内） | 会话存储与 PSK 推导（ticket 解析、resumption PSK 计算、obfuscated_ticket_age——08；06 只消费 `TlsSessionOffer` 输入） |
| 证书链验证（02b `tls_verify_chain`，SNI/IP 名字校验）、CV 验签（02b `tls_verify` + 偏好表） | 1.2 session ticket 的**消费与存储**（收到 NST 只校验结构+转录后忽略；08 接管） |
| alert 的收（路由/终止判定）与发（含密文 alert） | OCSP stapling / compress_certificate / 1.2 CBC / 静态 RSA KX（01 §1 既定不做） |
| 密钥时点编排：1.3 三次换 cipher 实例；1.2 key_block 切片 + 双向 CCS 时点 | record_size_limit / max_early_data_bytes 之外的策略协商（后续按需加扩展） |

### 1.2 与后续编号的切分

- **07 服务端引擎**：镜像 FSM，共享本文 §5 的 `detail/TlsHandshakeContext` 与
  `TlsTranscript`（06 以首个消费者身份定形，07 复核共享面）。
- **08 恢复**：`TlsSessionState`（会话缓存条目）、NST 消费（1.3 PSK 推导 / 1.2 ticket 存储）、
  anti-replay。06 的 `TlsSessionOffer` 是 08 未来 `TlsSessionState` 投影出的**借用视图**。
- **09 net 集成**：`TlsEngineStream` 胶水（read→feed、drain→write、HandshakeDone 处直换
  ConnectedEngine）。06 不假设其存在，但 API 按"构造→drain→feed 循环→take_state"可直驱。

## 2. 事实依据（RFC 条款 + 既定谳）

消费 02/02b/03/04/05 已定谳的事实，不重复 probe；本文新增以下协议事实为设计依据。

### 2.1 版本判定（RFC 8446 §4.1.2/§4.1.3）

客户端 offered `supported_versions=[0x0304, 0x0303]`，读 SH：

| SH 形态 | 判定 | 违例 alert |
|---|---|---|
| 带 `supported_versions` 扩展 | 值必须 0x0304 → **1.3**；其他值 | illegal_parameter |
| 无 `supported_versions`，legacy_version=0x0303 | → **1.2**；且因我们 offered 1.3，须检 `random[24..31] ≠ "DOWNGRD\x01"/"DOWNGRD\x00"`（sentinel 命中 = 服务器实支持更高版本却降级）| illegal_parameter |
| 无 `supported_versions`，legacy_version=0x0302 及以下 | （SSL3/1.0/1.1 不支持） | protocol_version |

sentinel 在 `random` 末 8 字节（§4.1.3），不在 legacy_version——legacy_version 只表达
"协商到几"，sentinel 才表达"服务器是否在撒谎"。

SH cipher_suite 不在 offered 列表 → illegal_parameter；compression ≠ 0 → illegal_parameter。

### 2.2 HRR（RFC 8446 §4.1.4）

- HRR 即 `random == CF21AD74…339C` 的 ServerHello（类型 2 外壳，语义为类型 6）。
- 收到第二个 HRR → unexpected_message（协议 MUST）。
- HRR 校验：cipher_suite 必须等于 CH1 的选择；session_id 必须等于 CH1 的；
  selected_group 必须在 CH1 的 `supported_groups` 内——违例均 illegal_parameter。
- HRR 处理义务：key_share 重建（丢弃旧 `TlsKeyExchange` 整个对象，02 契约：不可复用），
  只带 selected_group 的单 share；cookie 回带；`early_data` 从 CH2 移除（0-RTT 死亡，
  PSK 保留、binder 重算）；transcript 重启为 message_hash：
  `254 || 0x00 0x00 <len> || Transcript-Hash(CH1..HRR)` 成为新 transcript 的唯一前缀。
- 客户端**不重发** early data；HRR 前已写的 early data 记录由服务器丢弃，引擎只清写状态。

### 2.3 CCS 与记录版本（RFC 8446 §5）

- middlebox compat：客户端在 CH 后**立即**发一条 CCS(0x01)（在任何 early data 前）；
  HRR 后 CH2 之后**必须**再发一条。1.3 客户端收到对端 CCS 一律忽略（含服务器 SH 后的
  compat CCS），不推进任何状态。
- 记录 legacy_version：首飞 0x0301（`TlsRecordWriter::set_legacy_version`）；此后恒
  0x0303（05 §4 已硬编码密文记录 AAD/outer 版本，引擎只需在 SH 处理后改 writer 设置）。
- 1.3 中除 close_notify / user_canceled 外，**任何级别的 alert 都按 fatal 处理**
  （§6）；warning 级字段被忽略。

### 2.4 1.2 时序（RFC 5246 §7.4 + RFC 5746 + RFC 7627 + RFC 5077）

- server flight：SH → [Cert] → SKE → [CertReq] → SHD；client flight：[Cert] → CKE →
  [CV] → CCS → Fin；随后 server：[NST] → CCS → Fin。
- **NST 在 server CCS 之前，明文**（RFC 5077 §3.3）。客户端视角：我们发完 CCS/Fin 后
  可能先收到明文 NST，再收 server CCS（切 read cipher）与密文 server Fin。
- **HandshakeDone 时机 = server Finished 验证通过**（对齐 BoringSSL `SSL_connect`
  完成语义；RFC 5246 §7.4.9 允许客户端发完自己的 Fin 即发 app data，但 client MUST
  verify server Finished——本栈选择不把"待验证"状态泄漏进 ConnectedEngine）。
  1.3 对称点：server Fin 验证通过、client Fin 编码完成即 Done（1.3 客户端发完 Fin 即成）。
- EMS（RFC 7627）：我们 offer `extended_master_secret`；SH 回显则 master secret 用
  `PRF(z, "extended master secret", session_hash)`（session_hash = 到 SKE 为止的
  handshake_messages 快照，**不含** client flight）。不回显则回退经典 randoms 公式
  （互通优先，BoringSSL parity；RFC 9156 的 require 立场不做）。**02 层缺口**：
  现有 `tls12_master_secret` 只有 randoms 版，需补 `tls12_extended_master_secret`。
- 安全重协商（RFC 5746）：CH 带**空** `renegotiation_info`；1.2 SH 未回显 →
  handshake_failure（BoringSSL 默认立场：拒绝不安全对端）；回了但非空 → illegal_parameter。
  1.3 SH 中出现 `renegotiation_info` 等已废弃扩展 → unsupported_extension。
- 1.2 ALPN 选择在 **SH 扩展**里（RFC 7301），不是 EE（类型 8 在 1.2 不存在——
  `TlsHandshakeType::EncryptedExtensions` 的枚举注释仅指 1.3 语义）。
- CKE：仅 ECDHE（named_curve 编码，X25519/P-256）；不实现静态 RSA——server 若选了
  RSA KX 套件必然不在我们 offered 列表（§2.1 已拦），无此路径。
- SKE 验签：sigalg 必须在 offered `signature_algorithms` 内（否则 illegal_parameter）；
  内容 = handshake_messages（不含 SKE 自身）的 suite-hash 终值 digest；Ed25519 例外
  签原文（02b 已按版本实现，引擎只管内容构造）。

### 2.5 1.3 密钥时点（对齐 02 的 TlsKeySchedule13 阶段机）

| 时点 | 动作 |
|---|---|
| CH 编码前（PSK offer） | `TlsKeySchedule13(psk.suite)` + `set_psk`；`binder_key(Resumption)` → 截断 CH hash（`psk_binder_block_offset`，04 已给锚点）→ `tls13_psk_binder_mac` |
| early data 写（offer 且未拒） | `client_early_traffic_secret(Hash(CH))` → init early write cipher（单纪元，seq 从 0） |
| SH 处理完（z 已算出） | `handshake_secrets(z, Hash(CH..SH))` → init **read** cipher(s hs traffic)；client Fin 前不变（写侧在 early/明文之间按状态） |
| client Fin 编码时 | init write cipher(c hs traffic)，Fin 用 hs 密钥；EOED 同 |
| server Fin 验证后 | `application_secrets(Hash(CH..server Fin))` → client_app0/server_app0 → init app 双向 cipher（写侧从 client Fin 之后生效、读侧从 server Fin 之后生效——两个实例各自独立 seq，天然表达错峰） |
| client Fin 进 transcript 后 | `resumption_master_secret(Hash(CH..client Fin))` → 存入 ConnectedState（08 消费） |

HRR 循环：schedule 对象**销毁重建**（02 契约）；KX 同；early cipher 状态清零。
PSK 恢复（selected_identity 命中）：跳过 Cert/CV；SH 的 psk index 越界 → illegal_parameter。

### 2.6 防护上限

- 握手消息重组器（§5.1）缓冲上限 **4 MiB**（kMaxCerts(4)×kMaxDerLen(1 MiB)+裕量；
  协议上限 2^24 之前先拦）→ decode_error。
- 1.3 中 SH 之后到达的**明文** handshake/app_data 记录 = unexpected_message
  （全记录必须已加密；pre-SH 明文 handshake 记录数 ≤ 4）。
- 1.2 明文记录无此限（协议本性），靠重组器上限 + 消息级校验。
- 握手期 app_data 记录（type=23）一律 unexpected_message（客户端在 Done 前不收 app data；
  1.3 对端也不会发）。

## 3. 引擎 API（`src/tls/handshake/TlsClientHandshakeEngine.h`）

```cpp
namespace fiber::tls {

// 输入配置：全借用视图，须活过引擎生命周期（net 胶水持有 config）。不可变。
struct TlsClientConfig {                     // include/fiber/tls/TlsConfig.h
    std::string_view sni_host;               // SNI 发送名 + 证书校验名（SAN-only，02b 语义）
    std::span<const std::uint8_t> verify_ip; // 可选：IP 校验替代 host（非空时不发 SNI）
    std::span<const std::string_view> alpn;  // offered，序=偏好；空=不发 ALPN
    const TlsCertificateChain *client_chain; // mTLS，可空
    const TlsPrivateKey *client_key;         // mTLS，与 chain 配对，可空
    const TlsTrustStore *trust;              // 信任锚（02b 类型，必传）
    bool verify_peer = true;                 // false 仅测试回环用（lite_nginx parity）
    std::int64_t now_unix_ms = 0;            // 证书有效期校验快照（引擎无时钟，构造方注入）
};

// 恢复尝试（08 的 TlsSessionState 投影；06 定义借用形状，无所有权）
struct TlsSessionOffer {
    std::span<const std::uint8_t> identity;  // ticket 原文
    std::uint32_t obfuscated_ticket_age;     // 08 计算（age_add 折算），引擎只透传
    TlsCipherSuiteId suite;                  // PSK 绑定套件（binder/transcript hash 由它定）
    std::span<const std::uint8_t> psk;       // resumption PSK 字节（08 从 NST 推导）
    std::size_t max_early_data = 0;          // 0 = 不发 early_data 扩展
};

class TlsClientHandshakeEngine final : public common::NonCopyable, public common::NonMovable {
public:
    enum class Event : std::uint8_t {
        None,          // 消化了部分字节 / 需要更多输入；继续 feed
        HandshakeDone, // 终态（成功）：take_state() 就绪；out 可能仍有尾字节待 drain
        Failed,        // 终态（失败）：alert（若有）已在 out；failure() 可查因
    };

    // 构造即完成第一飞编码（CH ± binder ± compat CCS 已进 out_）。
    TlsClientHandshakeEngine(const TlsClientConfig &config, const TlsSessionOffer *session,
                             mem::IoBufNodePool &pool) noexcept;

    // 对端字节（任意分块）。内部循环消化到 NeedMore/终态。IoErr 仅 NoMem（连接级失败）。
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBuf &&bytes) noexcept;
    [[nodiscard]] common::IoResult<Event> feed(mem::IoBufChain &&bytes) noexcept;

    // 0-RTT 写：仅 offer.max_early_data>0 且拒绝判定前可写；累计超限返回 MessageTooLarge
    // （已写部分保留）。拒绝（SH 无 selected_identity / EE 无 early_data）后调用返回 Invalid。
    [[nodiscard]] common::IoResult<void> write_early_data(std::span<const std::uint8_t> data) noexcept;

    // 出向字节整体移交（密文记录序列，含 alert）。drain 后再取为空。
    [[nodiscard]] mem::IoBufChain take_output() noexcept;

    [[nodiscard]] bool done() const noexcept;          // HandshakeDone 或 Failed
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] TlsConnectedState take_state() noexcept;      // 断言 done && !failed
    [[nodiscard]] TlsAlertDesc failure_alert() const noexcept;  // Failed 时
};

} // namespace fiber::tls
```

要点：

- **构造即首飞**：无 start()；net 胶水"构造→take_output→写 socket→进 read 循环"。
- `feed` 可一次喂整个 flight（内部 Reader 缓冲 + 消息重组器逐条推进）；
  也可一字节一字节喂（分块边界无要求）。
- `Event` 单枚举三值：EarlyDataRejected **不设独立事件**——拒绝是引擎内部回滚，
  终态由 `ConnectedState.early_data_accepted` 表达；调用方对"何时停写 early data"的感知
  是 `write_early_data` 转为返回 Invalid。
- 失败路径统一：任一致命输入 → 编码 alert 进 out_ → `state_=Failed` → 返回 Failed。
  Failed 后再 feed = 契约违例（FIBER_ASSERT，胶水必换引擎）。

## 4. FSM

### 4.1 状态表

```
Start ──ctor: 编码 CH(±binder) + compat CCS──▶ WaitServerHello
WaitServerHello:
    SH(1.3) ────────────────────────────────▶ WaitServerFlight13   [切 read cipher]
    SH(HRR) ──CH2+CCS──▶ WaitServerHello     [HRR 计数=1；再收 HRR→unexpected_message]
    SH(1.2) ────────────────────────────────▶ WaitServerFlight12
    fatal alert / 认证失败 ─────────────────▶ Failed
WaitServerFlight13:   (EE→[Cert→CV]→Fin，见期望表；PSK 时无 Cert/CV)
    server Fin 验证通过 + client Fin[+EOED] 编码 ▶ Done(HandshakeDone)
WaitServerFlight12:   (Cert→SKE→[CertReq]→SHD)
    SHD → 编码 client flight([Cert]/CKE/[CV]/CCS/Fin) ─▶ WaitServerFinished12
    WaitServerFinished12: [NST(明文,转录后忽略)] → CCS(切 read) → Fin 验证 ▶ Done
Done / Failed：终态
```

版本无起点的实现形态：`WaitServerHello` 前引擎不区分版本；SH 读点一次判版本+套件，
随后建 1.3/1.2 子流程对象（schedule 类型、transcript 形态、期望表）——版本是**子流程选择**
而非引擎状态维（01 修订 2 §3.3 边界决策）。

### 4.2 1.3 server flight 期望表（WaitServerFlight13 内）

server flight：EE → [CertReq] → [Cert → CV] → Fin（PSK 恢复时跳过 Cert/CV，CertReq 仍可出现）。

| 序 | 期望 | 违例 → alert |
|---|---|---|
| 1 | EE | 其他 → unexpected_message；EE 内含 SH 专属扩展（key_share/cookie/…）→ unsupported_extension；alpn 值不在 offered → illegal_parameter |
| 1.5 | [CertReq]（可选，仅 EE 后）| 结构错 → decode_error；有配置证书则进 client flight，无则 1.3 发空链（继续） |
| 2a | Cert（非 PSK）| 空链/结构错 → decode_error；证书验证失败 → 02b `TlsCertVerification.alert` |
| 2b | Fin（PSK 恢复，跳过 2a/3）| — |
| 3 | CV（仅 2a 后）| scheme 未 offer → illegal_parameter；验签 false → decrypt_error |
| 4 | Fin | verify_data 不符 → decrypt_error |

server Fin 验证通过后引擎产出 client flight：[Cert（被请求）→ CV] → [EOED] → Fin
（CV 内容 = 64×0x20 \|\| "TLS 1.3, client CertificateVerify" \|\| 0x00 \|\| transcript hash）。

SH 处理序：decode → 版本判定（§2.1）→ HRR 识别（§2.2）→ suite∈offered →
`supported_identity` 越界检查 → KX.shared(server key_share)（组不在 CH share 内 →
illegal_parameter）→ schedule 重建/推进（§2.5）→ read cipher init。

### 4.3 1.2 期望表（WaitServerFlight12 / WaitServerFinished12）

| 序 | 期望 | 违例 → alert |
|---|---|---|
| 1 | [Cert]（ECDHE 套件必有）| 验证同 1.3（02b 映射） |
| 2 | SKE | 组未实现/参数非法 → illegal_parameter；sigalg 未 offer → illegal_parameter；验签 false → decrypt_error |
| 3 | [CertReq] | 记录"被请求"标志；签名算法/CA 名不匹配不致命（RFC 5246：无合适证书可发空 Cert；本栈：有配置证书就发，没有发空链 + 警告） |
| 4 | SHD | — |
| → | client flight：[Cert(仅被请求)] CKE [CV(仅发了 Cert)] CCS Fin | CCS 前消息明文；CV 内容 = handshake_messages 至 CKE 为止的 digest（Ed25519 签原文，02b 版本分派）；Fin 用 key_block client write key |
| 5 | [NST（明文，结构校验+转录，内容忽略）] | 结构非法 → decode_error |
| 6 | CCS（server）| 非 1 字节 0x01 → unexpected_message；切 read cipher |
| 7 | Fin（密文）| verify_data ≠ `tls12_verify_data` → decrypt_error |

## 5. 共享件与前置补丁

### 5.1 `detail/TlsHandshakeContext`（共享组合根，01 §3.2 防重复约束）

两个握手引擎的公共管道收敛于此（06 定形，07 复核）。**内部头**（`src/tls/detail/`，
无 openssl include；引擎保持薄 FSM）。职责与关键成员：

```cpp
class TlsHandshakeContext {
    // ---- 入站管道（engine::feed 调用）----
    // reader_ 分帧 → 记录路由：alert(解码/终止判定) | ccs(版本×角色分派) |
    //   handshake/app_data(密文先过 read cipher，失败退转录) → 重组器 → 逐消息回调引擎
    // ---- 握手消息重组器 ----
    // 累积 type=22 明文字节，按 4B 头+be24 长度切完整消息（跨记录分片/单记录多消息均正确）；
    // 上限 4 MiB；完整消息一处拷贝材料化（codec 需连续 body，03 §3.1 分工）
    // ---- 出站管道 ----
    // send_handshake(type, body)：encode 进 scratch → transcript 喂入 → writer 分帧 →
    //   write cipher 活跃则 seal（记录类型改写 application_data）
    // send_alert(level, desc)；send_ccs()；out_ 积累（take_output 移交）
    // ---- 协商落地 ----
    // version_/suite_/alpn_/peer_chain_/client_random_/server_random_/hrr_count_ …
    // ---- transcript 持有 ----
};
```

flight 的消息语义解释（HRR 判定、CV 内容构造、验证时点）留在引擎；context 只做
搬运、分帧、转录调用、alert 编码与协商结果存取。

### 5.2 `handshake/TlsTranscript`（+ 02 层增量 hash 补丁）

```cpp
// 1.3：增量跑动哈希；1.2：handshake_messages 字节缓冲（同样 4 MiB 上限）
class TlsTranscript13 {   // TlsHash 按构造给定 suite hash；不可变更 hash
    void update(span header_and_body);                      // 完整消息粒度喂入
    void snapshot_digest(out hash_len);                     // 终值快照
    TlsTranscript13 fork() const;                           // hash 状态拷贝（分叉预留：
};                          //  客户端握手期不用；post-handshake CertReq 备，07/09 消费）
    void restart_message_hash(ch1, hrr, suite_hash);        // §2.2 HRR 重启
class TlsTranscript12 {
    void update(span header_and_body);
    void snapshot_digest(out suite_hash_len);               // suite hash 终值
};
```

- **02 补丁**：`TlsCryptoPrimitives` 增加增量 `TlsHash`（SHA-256/384；`SHA256_CTX`/
  `SHA512_CTX` 直嵌，POD 可拷贝——fork 即 memcpy）。现仅有一次性 `tls_digest_empty`。
- 版本未定阶段（CH 编码）**不存在 transcript 对象**：客户端在 SH 读点用保留的
  CH1/CH2 编码字节从头喂入（自有字节在手，无需增量累积）；PSK binder 用独立单次
  hash（psk.suite 定 hash），不走 transcript 对象。
- 1.2 的 session_hash（EMS）与 verify_data 输入同源：`TlsTranscript12` 快照。

### 5.3 `TlsConnectedState`（+ 05 层 cipher move 补丁）

```cpp
struct TlsConnectedState {          // include/fiber/tls/TlsConnectedState.h（DTO，movable）
    TlsProtocolVersion version;
    TlsCipherSuiteId suite;
    TlsRecordCipher read_cipher;    // 1.3: server_app0 / 1.2: key_block server write keys
    TlsRecordCipher write_cipher;   // 1.3: client_app0 / 1.2: key_block client write keys
    TlsSecret client_app_secret;    // 1.3 KeyUpdate 写侧基（server 侧叫 server_*；1.2 空）
    TlsSecret server_app_secret;    // 1.3 KeyUpdate 读侧基
    TlsSecret resumption_master;    // 1.3：NST→PSK 推导（08）；1.2 空
    TlsSecret tls12_master;         // 1.2：08 session cache / verify 校验源；1.3 空
    std::array<std::uint8_t, 256> alpn; std::uint16_t alpn_len;   // 空表 alpn_len=0
    TlsCertificateChain peer_chain; // PSK 恢复时为空（move 拥有）
    bool session_resumed;
    bool early_data_accepted;
};
```

- **05 补丁**：`TlsRecordCipher` 目前 NonMovable，ConnectedState 无法携带。补 move
  ctor/assign（字节窃取+源置零——05 已立"EVP_AEAD_CTX 零值==未初始化、cleanup 安全"
  不变量，move 是该不变量的直接推论，~10 行）。seq 连续性随实例移交（client Fin 用
  hs cipher 到 seq=N，app 记录必须从新 cipher seq=0 起——1.3 换纪元天然如此；1.2 同一
  key_block 实例直传，seq 接续）。
- 敏感材料（app secrets/master）保留在 state 内供 KeyUpdate/08——密钥不可读出 cipher
  的契约不破（cipher 只收不吐；secret 副本来自 schedule）。
- 1.3 cipher 实例时点校准：`write_cipher`=client_app0 实例（client Fin 走 hs 实例，
  编码后即弃——Done 前最后一步 init app 实例）；`read_cipher`=server_app0 实例
  （server Fin 验证用的 hs read 实例即弃）。

### 5.4 `TlsConfig.h` 与凭据类型收敛（01 布局修订）

- 01 §3.2 规划的 `TlsCredentials.h` / `TlsTrustAnchors.h` **不再新建**：02b 落地的
  `TlsCertificateChain` / `TlsPrivateKey` / `TlsTrustStore` 即是"无 SSL 版凭据"，
  引擎与 config 直接消费。net 旧栈迁移（09）走别名/适配器（01 §5 既定方向不变）。
  本条作为对 01 §3.2 的修订记录，01 文档加一行注记。
- `TlsConfig.h` 06 只放 `TlsClientConfig` + `TlsSessionOffer`（§3）；07 增 server 形状。
  groups/suites 偏好、record_size_limit、max_early_data_bytes 策略**暂不开放**
  （引擎内置注册表序；09 提需求再升格为字段——YAGNI）。

### 5.5 codec 补齐（归属 06 交付；04 状态行同步注记）

| 函数（`TlsHandshakeCodec` / `TlsHandshakeMessage` 增量）| 方向 | 备注 |
|---|---|---|
| `tls_encode_client_hello` | encode | 布局确定序（pre_shared_key 恒最后）；binder 区先置零，引擎算后回填 |
| `tls_decode_server_hello` → `TlsServerHello` | decode | 提取 supported_version/key_share server share/cookie/selected_identity/alpn(1.2)/EMS/ri 存在性；HRR 随机值比对助手 |
| `tls_decode_encrypted_extensions` → `TlsEncryptedExtensions` | decode | alpn / early_data / SH 专属扩展侵入检查 |
| `tls_decode_certificate`（1.3 与 1.2 两形态）| decode | DER span 列表 + context(1.3)；1.3 空链=拒绝出证；上限 kMaxCerts×kMaxDerLen |
| `tls_decode_certificate_request`（两版）| decode | 只取"被请求"+ sigalgs（CA 名解析留 07/08 需要） |
| `tls_decode_certificate_verify` | decode | algorithm + signature span |
| `tls_decode_finished` | decode | verify_data span（1.2=12；1.3=hash_len） |
| `tls_decode_server_key_exchange` | decode | 1.2 ECDHE 参数 + 签名 |
| SHD / EOED | decode | 空 body 断言（EOED encode=4 字节头，inline） |
| `tls_encode_certificate` / `tls_encode_certificate_verify` / `tls_encode_finished` | encode | client mTLS flight 与两版 Finished |
| alert 编码（2B）/ CCS（1B 0x01）| 记录层 | context 直写，不进 codec |

decode 契约沿用 04：borrow span、零分配、失败不动 out、结构校验在解码器、
语义在引擎。encode 契约：写入调用方 scratch（span<uint8_t>）返回长度，分配归
context（每飞一次拷贝进 writer 链，热路径无感）。

## 6. 与既有层接缝总表

| 层 | 06 消费点 | 新增/补丁 |
|---|---|---|
| 03 Reader/Writer | feed 入站分帧；flight/CCS/alert 出站 | 无改（`set_legacy_version` 切 0x0301→0x0303） |
| 05 RecordCipher/Chain | 三时点换实例（§2.5）；读路径 open(失败退转录) 写路径 seal | **move ctor 补丁**（§5.3） |
| 04 codec | CH decode 已有（07 用）；`psk_binder_block_offset` | §5.5 全表 |
| 02 KeySchedule13/1.2 | 阶段机按 §2.5 时点推进；`tls13_*`/`tls12_*` free fns | `TlsHash` 增量、`tls12_extended_master_secret` |
| 02 KeyExchange | CH share 生成；SH share→z；HRR 后**销毁重建** | 无改 |
| 02b Signature/Certificate | `tls_verify_chain`（host 必传 config 名）；CV：偏好表选 scheme→`tls_verify`；mTLS：`sign` | 无改 |
| mem | `IoBufNodePool` 构造注入；on-loop 析构契约（03 §3.2） | 无改 |

## 7. 约束

- 生产代码零 openssl include 于 `include/fiber/tls/**`（01 §3.1）；新 cpp 只经
  `TlsCryptoPrimitives.h`（TlsHash 补丁同规）。
- 无异常：全 `IoResult`；协议失败=Event+alert，编程错误=FIBER_ASSERT。
- 热路径：握手每连接一次，允许"每飞一次拷贝"级成本；禁 `std::string`/`std::vector`/
  `std::function` 持有（config 借用 span；alpn 出参定长数组）。
- 池契约：Reader/Writer/out_ 同池；引擎与产出记录在同 loop 析构（03 §3.2）。
- `TlsKeyExchange`/schedule 私材料：引擎终态时随对象析构 wipe（02 契约）。

## 8. 测试计划（tests/TlsClientHandshakeEngineTest.cpp 等）

**peer 策略**：测试代码不受 BoringSSL 边界约束——用真 BoringSSL `SSL_CTX/SSL`
+ `BIO_s_mem` 内存对驱当服务器（07 未实现前唯一真实对端；比自产自销强一个数量级）。
驱动循环：`engine.take_output()→BIO_write(server)`；`SSL_do_handshake`；
`BIO_read(server)→engine.feed`（feed 粒度另做 1B 切片变体，打重组器/NeedMore 路径）。

1. **1.3 全握手参数化**：3 套件 × X25519/P-256；断言 HandshakeDone、version/suite、
   alpn 协商、peer_chain 非空、`session_resumed=false`、ConnectedState 双 cipher
   可直接开 app 记录（用 state.read/write_cipher 手工 seal/open 回环）。
2. **1.2 全握手**：RSA 与 ECDSA 服务器证书各一（fixtures）；EMS 回显/不回显两路
   （openssl 服务器开关）；NST 到达被忽略且握手仍成。
3. **HRR**：服务器锁 P-256 而客户端首 share X25519 → 强制 HRR；断言 CH2 单 share、
   cookie 回带、握手成；二次 HRR 由测试注入伪造记录 → unexpected_message。
4. **mTLS**：服务器 request/require 客户端证书；1.3 与 1.2 各自的 client
   Cert/CV flight；无证书配置时发空链（1.3）照常完成（服务器 optional 模式）。
5. **验证失败矩阵**：无锚信任/过期/主机名不符/SKE 或 CV 签名坏 → Failed +
   alert 映射逐条断言（02b `TlsCertVerification` 表）；喂篡改密文 → bad_record_mac。
6. **PSK 恢复 + 0-RTT**（对 BoringSSL）：第一跳握手后用 ConnectedState 手工开
   NST 密文记录（read_cipher 直用）、测试侧推导 PSK（复刻 08 未来逻辑：
   `Expand-Label(resumption_master, "resumption", nonce)`），第二跳携 `TlsSessionOffer`：
   1-RTT 恢复（无 Cert/CV）与 0-RTT 接受（EE.early_data + EOED 发送）两路；
   服务器拒绝 0-RTT（`SSL_CTX` 关 early data）→ `early_data_accepted=false`、
   `write_early_data` 转 Invalid、握手仍 Done。
7. **边界/恶意**：SH 未 offer 套件、版本 sentinel(0x0302)、EE 前到 Cert、Fin 坏
   verify_data、明文记录 post-SH(1.3)、超 4 MiB 重组、HRR 后再 HRR、CCS 非法值。
8. **单元**：`TlsTranscriptTest`（HRR message_hash 重启 KAT、fork 等价、1.2 快照）、
   SH/EE/Cert/CV/Fin/SKE decode 畸形表（沿 04 测试风格）、`tls12_extended_master_secret`
   KAT（RFC 7627 附录向量）、cipher move 语义（源置零、seq 接续）。

RFC 8448 常量仅在 schedule/cipher 层做 KAT（已有）；**引擎层不做 8448 字节回放**
——8448 要求逐字节相同的 CH（random 注入钩子），不值得为测试在生产 config 开口子；
BoringSSL 互驱是更强的等价覆盖。

## 9. 实施清单

| 步 | 内容 | 规模预估 |
|---|---|---|
| P0 | 前置补丁：`TlsHash`（02）、`tls12_extended_master_secret`（02）、`TlsRecordCipher` move（05）+ 各自单测 | 小 |
| P1 | codec 补齐 §5.5 全表 + decode 畸形单测（沿 04 风格） | 大（~1300 行含测试） |
| P2 | `TlsTranscript` + `TlsConfig.h`/`TlsSessionOffer` + `TlsConnectedState.h` + 单测 | 中 |
| P3 | `detail/TlsHandshakeContext`（入站路由/重组器/出站管道）+ 1.3 FSM + 互驱测试 §8.1-§8.5 | 大 |
| P4 | 1.2 子流程 + §8.2/§8.7 | 中 |
| P5 | PSK/0-RTT 写路径 + §8.6 | 中 |

每步全量 ctest 绿后进下一步；文档状态行随 P5 收尾改"已实现（实现记录见 §10）"。

## 10. 待拍板问题（六项均按推荐执行）

1. **codec 归属**（§5.5）：推荐随 06 交付（引擎直接输入、同批评测）；备选单独先行
   （04 续篇）。若认可，04 状态行注记"其余消息编解码随 06 交付"。
2. **凭据类型收敛**（§5.4）：砍 `TlsCredentials.h`/`TlsTrustAnchors.h`，直接用 02b
   类型。若认可，01 §3.2 同步注记。
3. **1.2 EMS 回退 vs 强制**（§2.4）：推荐 offer+fallback（BoringSSL parity）；若取
   RFC 9156 强制立场，改动点仅 SH 处理一处，测试矩阵去一列。
4. **now_unix_ms 注入**（§3）：推荐 config 快照字段；备选 feed 形参（每 feed 传，
   噪音大）或构造参独立于 config。影响：07 服务端同问题，06 定形后沿用。
5. **`TlsConfig` 字段开放度**（§5.4）：推荐首版只 §3 所列字段；groups/suites/record
   size limit 待 09 需求。若 lite_nginx 已知要 ALPN 强制校验/密码套件策略，现在加。
6. **1.2 session ticket 的 06 侧行为**（§1.1）：推荐"校验+转录+忽略"（08 接管消费）；
   备选 06 即带上交通道（函数指针回调）。影响 08 的接口形状。

### 实现记录（P0–P5，2026-09-21）

P0–P5 全部完成；每步全量 ctest 绿（最终 2318/2318）。§10 六项待拍板均按推荐执行：
codec 随 06 交付、凭据直接用 02b 类型、1.2 EMS offer+fallback、now_unix_ms 入 config、
TlsConfig 首版最小字段集、1.2 NST 校验+转录+忽略。

实现偏离（对正文条款的修正，均已按实现收敛）：

1. **plain-path CCS 补发点**（§2.2）：compat CCS 在 finish_1_3 第二飞前补发（非文档
   所述仅首飞后）；early/HRR 路径已在首飞后发送则跳过。
2. **transcript 持有在 engine**（§5.2）：TlsTranscript13 由引擎成员持有（文档示意
   ctx 层）；TlsTranscript12 为字节缓冲化 move-only（4 MiB 上限）。
3. **1.2 SKE 签名内容**（§2.4）：client_random‖server_random‖0x03‖be16(group)‖
   u8(pt_len)‖point（RFC 5246 实际形态），非文档笔误的"transcript digest 终值"。
4. **EMS session_hash 端点**（§2.4）：CH..SHD + [client Cert] + **CKE**（BoringSSL
   两端均在 hash 完 CKE 后才推导 master，handshake_{server,client}.cc）；不含 CV。
5. **1.3 套件参数化**（§8.1）：BoringSSL 1.3 套件不开放服务器配置，互驱改为
   "engine 协商值 == 服务器实际选择"一致性断言；1.2 套件按 §8.2 逐个 pin。
6. **RI echo 形态**（§2.4）：SH 中 echo = 恰 1 字节 0x00（空 renegotiated_connection
   的长度前缀）；空载荷判定为错。
7. **1.2 密封记录外层类型**（§5.1/05）：1.2 保留真实内容类型（client Fin 外层 =
   handshake 22），AAD 绑定之；仅 1.3 改写为 application_data。
8. **TLS 1.2 PRF A-chain 修复**（02 层生产 bug）：RFC 5246 §5 要求 A(i+1)=
   HMAC(secret, A(i))，独立于 T 输出链；02 实现与单测参考共写成 A(i+1)=T(i)，
   单块输出（verify_data）不受影响、master/key_block 第二块起全错，首次被 1.2
   互驱抓住。实现+测试参考已同步修复。
9. **CH 的 psk_key_exchange_modes 无条件 offer**（§5.5 编码表）：文档已写"engine
   固定常量"，实现初版误挂在 has_psk 门下——BoringSSL server 见 CH 无此扩展即
   完全不发 NST（!accept_psk_mode），PSK hop-1 取不到 ticket。已改为无条件发送。
10. **tls13_resumption_psk 落 02 层**（§1.1）：Expand-Label(resumption_master,
    "resumption", nonce) 作为 02 自由函数交付（08 消费）；测试复刻 08 逻辑直接调用。
11. **1.2 NST 互驱不可达**（§8.2）：1.2 server 仅在 client offer RFC 5077
    session_ticket 时发 NST，本引擎不 offer（08 范围），故"NST 到达被忽略"由
    FSM 边界校验覆盖；测试加 NOTE 说明。
12. **引擎内部分叉为版本子对象**（§4 FSM，2026-09-22 行为等价重构）：
    `TlsClientHandshakeEngine.cpp`（原 1648 行单 FSM）拆为外层薄壳 +
    `Tls13ClientHandshake` + `Tls12ClientHandshake`（`src/tls/handshake/`
    内部类型，公共 API 与头不变）。边界：外层持 ctx/保留 CH/调度/0-RTT 窗口
    与终态通道，做首飞构造（含 PSK binder）与记录级路由，在**首个握手消息
    检测点**（parse 头 → supported_versions/HRR 形态）以 `std::variant`
    原地 mount 子对象并移交**原始消息**——SH 校验规则全部归子对象。HRR
    循环（CH2 重建经共享 `TlsClientHandshakeShared` 的构造函数）归 1.3 子；
    early-data 写窗口归外层（`write_early_data` 无版本分派，1.3 子经引用
    在 SH/HRR/EOED 点关窗，1.2 分叉点直接关闭）。子对象经 Mount 引用集
    回报终态，无虚基类；子对象各自析构擦除己方密钥。一处行为收紧：HRR
    后到达 1.2 形态 SH（server 违反 RFC 8446 §4.1.4 的 1.3 承诺）原实现
    会中途切进 1.2 子流程，现按 illegal_parameter 终止。07 服务端引擎
    预期同形（收 CH → 检测 → 分叉）。

BoringSSL 互驱测试侧经验（§8 harness 约定）：

- TCP 1.3 server 把 NewSessionTicket **推迟**到下一次 write 才刷出 BIO
  （tls13_server.cc do_send_new_session_ticket 注释）；测试用零字节 SSL_write
  冲洗。跳过不消费 ticket 记录会使 read cipher 序列号失步（AEAD nonce 错位），
  须经 read_cipher 逐条 open 后再开后续 app 记录。
- PSK hop-2 server 必须共享 hop-1 的 SSL_CTX（ticket key 每 ctx 独立铸造）。
- 0-RTT 接受还需：ticket 签发时 ctx 开 early_data（ticket_max_early_data 只在
  签发点盖戳）+ 协商 ALPN 与 ticket 内 early_alpn 一致 + ticket 年龄偏差 <10s。
