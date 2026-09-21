# TLS 自研实现 · 02b 签名与证书（06/07 前置深设计）

日期：2026-09-21
分支：`tls`
状态：**已实现（2026-09-21，实现记录见 §12）**
前置：02（crypto 适配层已完成，`TlsCryptoPrimitives` 是既有 openssl 触点）；本文是
02 §7.4/§7.5 的深设计展开，消费方是 06/07 握手引擎（CertificateVerify 签验、
证书链出示/验证）与 09 net 集成（凭据装载）。

深度分级：签名原语与 scheme 匹配表（§3）、证书装载与链验证（§4）为**深设计**；
对端链呈现、`TlsConnectedState` 的证书视图字段为**接口级**（06/07 定稿）。

## 1. 定位与边界

**本层 = "凭据与信任的密码学编排"**：私钥/证书/信任锚的装载、解析、匹配，
CertificateVerify 与 1.2 ServerKeyExchange 的**签名原语**，证书链的**路径验证**。

性质判断（回答"是不是纯封装 BoringSSL"）：X509/EVP 都属 01 允许的密码学原语
（x509.h/evp.h 在白名单），**主体确实是适配层封装**——本层不实现任何路径验证
算法（name constraints、EKU、basic constraints 全由 `X509_verify_cert` 执行）。
真正自研的只有三块协议语义：scheme↔密钥匹配表（§3.2）、签名内容构造规格
（§3.4，实现归 06/07，本层只钉契约）、验证结果→alert 映射（§4.5）。

**不做的事**：

| 不做 | 归属/理由 |
|---|---|
| certificate/certificate_request 消息编解码（1.3 cert entry/extensions、request context、OCSP status、SCT、RFC 8879 压缩） | 04 codec |
| 签名内容构造（context string、CR‖SR‖params 拼接，§3.4 规格已含 mTLS 方向）与 scheme 选择循环 | 06/07（本层出 supports/偏好表原语） |
| mTLS 模式语义（None/Optional/Required 三态判定、无证书 vs 无效证书的处理） | 06 引擎（§4.6 对齐既有 `TlsClientCertificateMode`） |
| 吊销（CRL/OCSP）、CT 日志验证 | 非目标（02 已定；X509_verify_cert 默认不查吊销） |
| 加密私钥（passphrase/加密 PKCS#8） | 非目标：服务端配置用明文私钥+文件权限；`openssl pkcs8 -topk8` 一行可转 |
| `rsa_pss_pss_*`（0x0809-0x080b，PSS 证书专用）、`rsa_pkcs1_sha1`、md5sha1 组合 digest | 非目标：生态罕见/版本外（1.2+ only），枚举留空隙可后补。md5sha1 不做**不挡 mTLS**——1.2 客户端 CertificateVerify 的 scheme 从服务器 certificate_request 的 signature_algorithms 列表选取，BoringSSL 默认列表即以 sha256 起步，现代客户端证书（RSA/ECDSA/Ed25519）全程无 sha1 通路 |

**范围结论**：`TlsSignatureScheme` 现有 9 值 + 补 `EcdsaSecp521r1Sha512`(0x0603)
= 10 个 scheme 全实现签验与匹配。

## 2. 事实依据（BoringSSL probe，2026-09-21 头文件/源码定谳）

1. **签名路径与 BoringSSL SSL 栈同构**（`ssl_privkey.cc` `setup_ctx`）：
   `EVP_DigestSignInit(ctx,&pctx,md,pkey)`（Ed25519 时 md=NULL）→ PSS 加
   `EVP_PKEY_CTX_set_rsa_padding(RSA_PKCS1_PSS_PADDING)` +
   `EVP_PKEY_CTX_set_rsa_pss_saltlen(RSA_PSS_SALTLEN_DIGEST)`（salt=hash 长）→
   单发 `EVP_DigestSign(ctx,out,&len,data,n)`。验签对称（VerifyInit/Verify）。
   我们复刻同一路线，无需发明；
2. **ECDSA 签名值就是 DER**（BoringSSL EVP 输出 DER；RFC 8446 §4.2.3 wire 也
   是 DER）——无裸 r‖s 转换层；RSA-PSS/PKCS1 输出裸大数。最大签名长度 = 
   `EVP_PKEY_size(pkey)`（RSA 模长；EC/Ed25519 ≤ 139 DER）；
3. **scheme↔密钥匹配权威表在 `ssl_privkey.cc` L42-100 + `ssl_pkey_supports_
   algorithm`(L124-177)**：匹配 = `EVP_PKEY_id(pkey) == alg->pkey_type` + RSA-PSS
   模长检查（`EVP_PKEY_size >= 2*hash_len+2`，1024 位 RSA 不够 SHA-512）+ 1.3 EC
   曲线 NID 检查（`EVP_PKEY_get_ec_curve_nid`）+ `tls12_ok/tls13_ok` 版本门
   （`rsa_pkcs1_*` 1.3 握手签名不可用但证书签名仍可——我们不签证书，无须区分）；
4. **证书解析无 BIO 路线打通**：`X509_parse_from_buffer(CRYPTO_BUFFER*)`（x509.h
   L114）存在——02 §9.1 probe 项 3 就此关闭。私钥 `d2i_AutoPrivateKey`（evp.h
   L1104，自动探测类型）存在；**遗留疑点**：PKCS#1/sec1 传统 DER 的探测行为
   BoringSSL 侧未见文档保证（OpenSSL 语义是先 PKCS#8 后传统），实施首日 probe；
5. **链验证**：`X509_STORE_new/add_cert`（信任锚）+ `X509_STORE_CTX_init(ctx,
   store, leaf, untrusted_chain)` + `X509_verify_cert`；错误码
   `X509_STORE_CTX_get_error`（不走 ERR 队列，x509.h L2940 注释）；验证后链
   `X509_STORE_CTX_get0_chain`；**时间注入** `X509_VERIFY_PARAM_set_time`（从
   `X509_STORE_CTX_get0_param` 取）——01 的"时间源调用方注入"落点；
6. **hostname/IP 匹配**：`X509_check_host`（x509.h L4568，SAN dNSName+通配符，
   flags 控制是否回落 CN）与 `X509_check_ip` 存在；flags 取值实施时对齐
   BoringSSL SSL 栈行为（SAN-only，无 CN 回落）；
7. **verify 错误→alert 映射表**：`SSL_alert_from_verify_result`（`ssl_x509.cc`
   L1106-1129+）——多数结构/签名错→bad_certificate，CA/深度/自签→unknown_ca，
   过期→certificate_expired，hostname mismatch→bad_certificate。照搬；
8. **私钥-证书匹配**：`EVP_PKEY_cmp`（evp.h L69，比较含参数与公钥成分）——
   语义正好是"证书属于这把私钥"；
9. **线程安全前提成立**：`EVP_PKEY`/`X509_STORE` 构建后只读并发安全（evp.h
   L494 注释明确；x509.h L2803 对 `X509_verify_cert` 并发的说明）。凭据对象
   immutable 共享 = EventLoopGroup 多 worker 共用一份 server 配置的必要性质；
10. `TlsProtocolVersion` 已存在（`include/fiber/tls/TlsVersion.h`）。

## 3. 签名（`include/fiber/tls/crypto/TlsSignature.h`）

### 3.1 类型与 API

```cpp
// 公钥视图：非 owning，生命周期由来源 TlsCertificate 背书（契约注释钉死）。
class TlsPublicKeyView {
public:
    [[nodiscard]] TlsKeyKind key_kind() const noexcept;
    // ... 指针 impl_ = X509_get0_pubkey 的内部指针（零增引用）
};

// 私钥：owning pimpl（EVP_PKEY*，析构在 .cpp 侧 free）。Move-only。
class TlsPrivateKey {
public:
    // DER：PKCS#8（oneAsymmetricKey）。传统 PKCS#1/sec1 DER 探测性支持（§2.4 疑点）。
    [[nodiscard]] static common::IoResult<TlsPrivateKey> parse_der(std::span<const std::uint8_t>);
    // PEM：剥 armor（"PRIVATE KEY"块，common/util/Base64）→ DER → parse_der。仅未加密。
    [[nodiscard]] static common::IoResult<TlsPrivateKey> parse_pem(std::span<const char>);

    [[nodiscard]] TlsKeyKind key_kind() const noexcept;
    // 协商匹配（§3.2 表的运行时形态）；version 参与 tls12_ok/tls13_ok 门。
    [[nodiscard]] bool supports(TlsSignatureScheme scheme, TlsProtocolVersion v) const noexcept;
    [[nodiscard]] std::size_t max_signature_len() const noexcept; // EVP_PKEY_size

    // content = 引擎已构造好的待签串（§3.4 契约）。out 容量须 >= max_signature_len。
    [[nodiscard]] common::IoResult<std::size_t> sign(TlsSignatureScheme scheme,
                                                     TlsProtocolVersion v,
                                                     std::span<const std::uint8_t> content,
                                                     std::span<std::uint8_t> out) const noexcept;
private:
    void *impl_ = nullptr; // EVP_PKEY*
};

// 验签（公钥来自对端证书）：bool=签名有效；错误（scheme 不匹配公钥等）走 IoErr，
// 与"验签失败"（返回 false）区分——后者是协议事件（发 decrypt_error alert）。
[[nodiscard]] common::IoResult<bool> tls_verify(TlsSignatureScheme scheme, TlsProtocolVersion v,
                                                const TlsPublicKeyView &key,
                                                std::span<const std::uint8_t> content,
                                                std::span<const std::uint8_t> signature) noexcept;

enum class TlsKeyKind : std::uint8_t { Rsa, EcP256, EcP384, EcP521, Ed25519 };

// 协商偏好序（06 的选择循环遍历它，取第一个 peer offered 且本地 supports 的）。
// 对齐 BoringSSL 默认签名算法偏好：PSS 优先于 PKCS1，SHA-256 > 384 > 512。
inline constexpr std::array<TlsSignatureScheme, 7> kTls13SignaturePreference{ /* 0x0804,0x0805,0x0806,0x0807,0x0403,0x0503,0x0603 */ };
inline constexpr std::array<TlsSignatureScheme, 10> kTls12SignaturePreference{ /* 同序 + rsa_pkcs1_* 后备 */ };
```

设计要点：

- `sign/verify` 是**纯原语**：吃 content（已含 transcript hash 的完整待签串），
  出 wire 格式签名值（ECDSA=DER，无转换）。digest 选择、PSS 参数、Ed25519 的
  NULL md 全部按 scheme 内部分发（复刻 `setup_ctx`），调用方零密码学知识；
- scheme 不在实现集 / 不匹配该私钥类型：`supports` 查询式预检（协商用）+
  `sign` 内 FIBER_ASSERT（引擎选过的 scheme 再进 sign，违约=引擎 bug）；验签
  侧不 assert——对端选的 scheme 可能不配其证书，返回 IoErr::Invalid（引擎发
  illegal_parameter）；
- 签名缓冲上限：`max_signature_len()` = EVP_PKEY_size（RSA-4096 = 512）。引擎
  栈缓冲 512 即可，无堆分配。

### 3.2 scheme↔密钥匹配表（照搬 ssl_privkey.cc，自建为常量）

| scheme | 值 | 密钥 | 1.2 | 1.3 | 附加约束 |
|---|---|---|---|---|---|
| RsaPssRsaeSha256 | 0x0804 | RSA | ✓ | ✓ | mod_len ≥ 2·32+2 |
| RsaPssRsaeSha384 | 0x0805 | RSA | ✓ | ✓ | mod_len ≥ 2·48+2 |
| RsaPssRsaeSha512 | 0x0806 | RSA | ✓ | ✓ | mod_len ≥ 2·64+2 |
| Ed25519 | 0x0807 | Ed25519 | ✓ | ✓ | md=NULL |
| EcdsaSecp256r1Sha256 | 0x0403 | EC | ✓ | ✓ | 1.3：曲线=NID_X9_62_prime256v1 |
| EcdsaSecp384r1Sha384 | 0x0503 | EC | ✓ | ✓ | 1.3：曲线=NID_secp384r1 |
| EcdsaSecp521r1Sha512 | 0x0603 | EC | ✓ | ✓ | 1.3：曲线=NID_secp521r1 |
| RsaPkcs1Sha256/384/512 | 0x04/05/0601 | RSA | ✓ | ✗（tls13_ok=false） | — |

1.2 的 EC 匹配不查曲线 NID（`ssl_pkey_supports_algorithm` 只在 ≥1.3 查——
1.2 的 scheme 本身不带曲线语义，任意 EC 密钥签 ecdsa_secp256r1_sha256 也由
对端担责；我们照搬该行为，注释注明）。

### 3.3 签名值 wire 形态

| scheme | EVP 输出 | TLS wire | 转换 |
|---|---|---|---|
| rsa_pss_rsae_* / rsa_pkcs1_* | 裸模长字节 | 同 | 无 |
| ecdsa_* | DER | DER | 无 |
| ed25519 | 64B r‖s | 64B | 无 |

（此表是 06 编码 CertificateVerify 的依据：签名值直接定长/变长编码进消息。）

### 3.4 签名内容构造契约（归 06/07，此处钉死规格）

- **1.3 CertificateVerify，双方向**（RFC 8446 §4.4.3）：
  `0x20 ×64 ‖ "TLS 1.3, server CertificateVerify"|"TLS 1.3, client CertificateVerify" ‖ 0x00 ‖ Transcript-Hash`
  （≤ 64+33+1+48 = 146 字节，栈缓冲 200 即可）；
- **1.2 ServerKeyExchange 签名**（RFC 5246 §7.4.3）：
  `client_random(32) ‖ server_random(32) ‖ ServerKeyExchange.params`；
- **1.2 客户端 CertificateVerify**（RFC 5246 §7.4.8，mTLS 方向）：待签 content =
  Hash(handshake_messages)——跑动哈希（client_hello 起至本消息前，含
  CertificateRequest 与客户端 Certificate）按 scheme 的 hash 取**终值**，签这个
  digest 本身（RSA=DigestInfo(digest)、ECDSA=sign(digest)）；**Ed25519 特例**
  （RFC 8422 §5.5）：不预哈希，content = 握手消息**原文**（transcript 全量拼接，
  长度可达数十 KiB——06 需要非定长 content 通路，`sign/verify` 的 span 形态已
  支持，无接口改动）。scheme 从服务器 certificate_request 的
  signature_algorithms 列表 ∩ `kTls12SignaturePreference` ∩ 客户端密钥
  `supports()` 选取（06 的选择循环）；md5sha1 组合 digest 非目标（§1）；
- 1.0/1.1 的 MD5‖SHA1 **无 scheme 协商**拼接（CertificateVerify 无算法字段时代
  的构造）：版本外，不做。

### 3.5 适配层增补（`TlsCryptoPrimitives` vs 独立 cpp）

签名不走 `TlsCryptoPrimitives`（那是无状态原语集）；EVP_MD_CTX/EVP_PKEY 的
有状态封装放 `src/tls/crypto/TlsSignature.cpp` 直接持有，**openssl include 仍
只出现在 src/tls/ 侧**——但注意 `TlsSignature.h` 位于 include/，其 pimpl 保证
零 openssl 穿透（对齐 01 §42 规则）。ctx 为栈上临时（每签名一个，无复用），
签名路径每握手 ≤2 次，无热路径顾虑。

## 4. 证书（`include/fiber/tls/crypto/TlsCertificate.h`）

### 4.1 TlsCertificate：单证书（owning pimpl = X509* + CRYPTO_BUFFER*）

```cpp
class TlsCertificate {  // Move-only
public:
    [[nodiscard]] static common::IoResult<TlsCertificate> parse_der(std::span<const std::uint8_t>);
    // wire 出证（06 收到 certificate 消息后逐条 parse）与配置装载共用。

    [[nodiscard]] std::span<const std::uint8_t> der() const noexcept; // 原文视图：wire 发送免重序列化
    [[nodiscard]] common::IoResult<TlsPublicKeyView> public_key() const noexcept; // 借用视图（§3.1）
    struct Validity { std::int64_t not_before_ms; std::int64_t not_after_ms; };
    [[nodiscard]] common::IoResult<Validity> validity() const noexcept; // 展示/启动自检（验证时刻仍由 X509_verify_cert 把关）
    [[nodiscard]] bool matches_host(std::string_view dns_name) const noexcept; // X509_check_host, SAN-only
    [[nodiscard]] bool matches_ip(std::span<const std::uint8_t> ip4or16) const noexcept;
    [[nodiscard]] common::IoResult<bool> matches_private_key(const TlsPrivateKey &) const noexcept; // EVP_PKEY_cmp
    // subject/issuer 一行摘要（日志/测试名用）：X509_NAME 文本化，定长缓冲填充式。
    [[nodiscard]] common::IoResult<std::size_t> subject_line(std::span<char> out) const noexcept;
    [[nodiscard]] common::IoResult<std::size_t> issuer_line(std::span<char> out) const noexcept;
private:
    void *x509_ = nullptr;        // X509*（parse_from_buffer 的产物，owning）
    void *der_buf_ = nullptr;     // CRYPTO_BUFFER*（持有 der 内存，owning）
    std::uint32_t der_len_ = 0;
};
```

要点：**X509 与 DER 原文同寿**（CRYPTO_BUFFER 被 X509 引用着）——两个指针一个
析构函数统一释放（X509_free 后 CRYPTO_BUFFER_free）。`matches_host` 用
`X509_check_host`；返回 bool 而非 IoResult（不匹配是正常结果，库失败即 false+
注释；BoringSSL 语义）。

### 4.2 TlsCertificateChain：leaf-first 定长容器

```cpp
class TlsCertificateChain {
public:
    // PEM bundle（"-----BEGIN CERTIFICATE-----" 多块）→ 逐块剥 armor → parse_der。
    [[nodiscard]] static common::IoResult<TlsCertificateChain> parse_pem_bundle(std::span<const char>);
    // 1.3 wire：每条 DER 已由 04 解出长度，逐条 parse。
    [[nodiscard]] static common::IoResult<TlsCertificateChain> from_der_list(
        std::span<const std::span<const std::uint8_t>> ders);

    [[nodiscard]] const TlsCertificate &leaf() const noexcept;        // [0]，断言非空
    [[nodiscard]] std::span<const TlsCertificate> intermediates() const noexcept; // [1..n)
private:
    std::array<TlsCertificate, 4> certs_{};  // 4 = leaf + 3 中间（现实上限；溢出=配置错）
    std::uint8_t count_ = 0;
};
```

**不验证链结构**（leaf 签发者是否匹配 intermediate 的 subject 等）——那是
`tls_verify_chain` 的事；装载层只做格式解析。上限 4：`X509_STORE_CTX_init` 的
untrusted 链照传。

### 4.3 TlsTrustStore：信任锚（owning pimpl = X509_STORE*）

```cpp
class TlsTrustStore {  // Move-only；构建后 immutable → 多线程共享（§2.9）
public:
    [[nodiscard]] static common::IoResult<TlsTrustStore> from_pem_bundle(std::span<const char>);
    [[nodiscard]] static common::IoResult<TlsTrustStore> from_der_roots(
        std::span<const std::span<const std::uint8_t>> ders);
private:
    void *store_ = nullptr;  // X509_STORE*
};
```

系统根（/etc/ssl/certs/ca-bundle）的装载：09 net 集成读文件后走同一 from_*
（文件 I/O 不进 tls 层，01 依赖单向规则）。

### 4.4 tls_verify_chain：路径验证（每握手 ≤1 次，无热路径顾虑）

```cpp
struct TlsCertVerification {
    enum class Status : std::uint8_t { Trusted, NotTrusted };
    Status status = Status::NotTrusted;
    TlsAlert alert = TlsAlert::BadCertificate;     // 失败时引擎直发（§4.5 映射）
    int verify_error = 0;                          // X509_V_ERR_* 原码（日志/测试断言）
};
// purpose 选择 EKU 检查方向（SslServer=验服务器链 / SslClient=mTLS 验客户端链），
// 对应 BoringSSL X509_STORE_CTX_set_default("ssl_server"|"ssl_client") 继承的
// purpose+trust 两项。host/ip 二选一可空（服务端验证客户端证书时都不传；客户端
// 验证服务器证书至少传一）。now_unix_ms：调用方时间源（net 胶水取 EventLoop::now），
// 秒精度足够（set_time 取秒）。
[[nodiscard]] common::IoResult<TlsCertVerification> tls_verify_chain(
    const TlsCertificateChain &chain, const TlsTrustStore &anchors,
    TlsCertPurpose purpose, std::string_view host, std::span<const std::uint8_t> ip,
    std::int64_t now_unix_ms) noexcept;
```

内部：`X509_STORE_CTX_init(store, leaf, intermediates)` →
`X509_VERIFY_PARAM_set_time(get0_param, now_s)` + set_purpose + set_trust →
hostname/IP 匹配（先验，RFC 6125 与 BoringSSL 行为对齐——在链验证**前**查，
无效直接 HOSTNAME_MISMATCH，免得白跑链）→ `X509_verify_cert` → 结果映射。

**partial chain**：不设 `X509_V_FLAG_PARTIAL_CHAIN`（对齐 BoringSSL 默认：链必
须走到信任锚）。

### 4.5 verify 错误 → alert 映射（照搬 `SSL_alert_from_verify_result`）

| X509_V_ERR 类 | alert |
|---|---|
| CERT_HAS_EXPIRED / CERT_NOT_YET_VALID / ERROR_IN_*_FIELD | certificate_expired |
| UNABLE_TO_GET_ISSUER_CERT(_LOCALLY) / DEPTH_ZERO_SELF_SIGNED / SELF_SIGNED_IN_CHAIN / INVALID_CA / CHAIN_TOO_LONG / PATH_LENGTH_EXCEEDED / UNABLE_TO_GET_CRL* | unknown_ca |
| UNABLE_TO_DECRYPT_CERT_SIGNATURE / DECODE_ISSUER_PUBLIC_KEY / CERT_UNTRUSTED / CERT_REJECTED / HOSTNAME_MISMATCH / 其余 | bad_certificate |

表作为 .cpp 常量 switch 实现；alert 枚举已在 `TlsTypes.h`。06 引擎拿到
NotTrusted 后：发 alert + 关连接（无证书协商失败路径），不做"验证失败但继续"
选项（对照现有 net 栈 `TlsParams` 的 verify 行为，09 接线时再对齐开关面）。

### 4.6 mTLS 验证语义（对齐既有 `TlsClientCertificateMode`）

既有 net 栈 `include/fiber/net/TlsParams.h:71` 已定义三态
（`None/Optional/Required`）+ 服务端 `trust_store`（客户端 CA）+ 客户端
`TlsClientSecurity::credential`（出示证书+私钥）——自研栈**沿用同一语义面**，
三态判定归 06 引擎，本层提供全部原语：

| mode | 06 引擎行为（两版本差异在消息形态，判定同构） |
|---|---|
| None | 不发 certificate_request；收到客户端主动塞证书（1.2 允许）不验证、不使用 |
| Optional | 发 certificate_request；1.3 无 Certificate 消息 / 1.2 空 cert 列表 → 握手继续（`peer_certificates` 空）；给了证书 → `tls_verify_chain`（host/ip 空）失败即 fatal |
| Required | 发 certificate_request；无证书（同上两形态）→ fatal（`certificate_required`，1.3 专有 alert；1.2 用 `bad_certificate`/`handshake_failure`——06 定，对齐 BoringSSL 1.2 行为）；给了 → 同 Optional 验证 |

判定输入只有三个：mode、对端是否出示（链非空）、`tls_verify_chain` 结果——
本层 API 已完整覆盖（chain 解析出的 `leaf()/intermediates()` 非空 = 出示）。
**Optional 下"无证书"与"验证失败"是不同结局**：前者握手成功且
`TlsConnectedState.peer_certificates` 为空，后者 fatal——06 不得混淆。

## 5. 与既有/后续模块的接缝

| 消费方 | 用什么 |
|---|---|
| 06 服务端引擎 | `TlsPrivateKey::parse_pem`+`Chain::parse_pem_bundle`+`matches_private_key`（启动自检）、`supports`+`kTls*SignaturePreference`（协商）、`sign`（CertificateVerify/SKE）；mTLS：certificate_request 的 sigalg 列表 = 偏好序过滤、客户端链 `tls_verify_chain`（host/ip 空）、`tls_verify`（客户端 CertificateVerify，§3.4 两种构造）、三态判定（§4.6） |
| 06 客户端引擎 | `tls_verify_chain`（host 必传）、对端链进 `TlsConnectedState.peer_certificates`（der() 视图）；mTLS：收到 certificate_request 时出示凭据（`TlsPrivateKey`+`Chain`，即 `TlsClientSecurity::credential` 的自研栈对应物）+ `sign`（客户端 CertificateVerify，§3.4） |
| 09 net 集成 | 凭据三件（key/chain/trust）装载与 immutable 共享；既有 `TlsServerParam.trust_store/client_certificate_mode` 与 `TlsClientSecurity.credential` 接到自研栈对应物（注意：既有 net 栈的 `TrustStore`/`TlsCredential` 是 BoringSSL SSL 句柄族，与本层 `TlsTrustStore`/`TlsPrivateKey` 是**不同对象**，09 做桥接或换型，接线细节 09 定） |
| 08 会话恢复 | 无直接依赖（PSK 路径无证书） |
| 04 codec | 无依赖反向；certificate/certificate_request 消息的 entry/extension 解出 DER 后调 `parse_der` |

依赖方向：`TlsCertificate.h` → `TlsSignature.h`（TlsPublicKeyView）→
`TlsTypes.h`/`TlsVersion.h`/`TlsCipherSuites.h`。零 openssl 穿透到 include/
（pimpl 全 void*，析构/操作全 .cpp）。

## 6. 安全要点

1. **私钥内存**：EVP_PKEY 由 BoringSSL 管理（其内部有 cleansing 分配器）；我们
   不额外缓存密钥位。PEM 明文配置由调用方（09 文件装载）负责权限；
2. **验签常量时间**：RSA/ECDSA 验证走 EVP，比较在库内；`tls_verify` 的 bool
   返回不泄漏错误位置细节；
3. **时间注入而非时钟**：`tls_verify_chain` 的 now 是参数——引擎可测试、可
   单测注入过期边界；拒绝在库内取系统时间（与 02/01 的无时钟原则一致）；
4. **hostname 先于链验证**：无效主机名在链构建前短路（省一次路径构建，也避免
   "链有效但名字错"的日志误导）；
5. **未验证链不出现在信任决策里**：`get0_chain` 的产物只用于日志/呈现，
   `TlsCertVerification` 的判定只看 `X509_verify_cert` 返回；
6. **线程安全**：所有对象 build 后 immutable；多 worker 共享 = const 引用。
   构建期（add_cert 等）非线程安全——装载在配置阶段单线程完成（09 契约）。

## 7. 测试计划（`tests/TlsSignatureTest.cpp` / `tests/TlsCertificateTest.cpp`）

### 7.1 fixture（`tests/tls_certs/`，openssl CLI 预生成 + `regen.sh` 入库可复现）

- 三级链 root → intermediate → leaf（RSA-2048，SAN=example.com）；
- EC P-256/P-384 leaf 各一（ecdsa 协商用）；
- Ed25519 leaf（openssl 1.1.1+ 支持）；
- 过期 leaf（-days 0 构造）/ 未生效 leaf（not_before 未来）/ 主机名不匹配 leaf
  （SAN=other.example.com）/ 自签非锚 leaf / 断链（缺 intermediate）；
- 超小 RSA-1024 leaf（PSS-SHA512 模长不足路径）。

### 7.2 用例

- **签名 KAT**：对固定 content（= §3.4 的 1.3 服务端构造串，hash 用 RFC 8448
  §3 的 transcript 值复用）用 fixture 私钥签，向量由 python cryptography 离线
  生成落 `TlsSignatureVectors.h`（**机械生成**，沿 02 §12.2 流水线纪律，测试
  不手抄 hex）；`tls_verify` 双向闭合 + 对 1 比特篡改返回 false；mTLS 方向
  （1.3 client context string、1.2 digest-only content）同型固定 content 各
  补一组闭合——sign/verify 原语本就方向无关，此组只是把两种构造形态钉进 KAT；
- **supports 全组合**：10 scheme × {RSA-2048, RSA-1024, P-256, P-384, Ed25519}
  × {1.2, 1.3} 的矩阵断言（表驱动，钉死 §3.2）；
- **parse 往返**：PEM bundle ↔ der() ↔ parse_der 幂等；损坏 DER/armor →
  IoErr；PKCS#1 传统私钥按 probe 结果断言（支持→往返，不支持→文档化拒绝）；
- **matches_private_key**：配对 true；换钥 false；同参数不同密钥 false；
- **matches_host**：精确/大小写/通配符 `*.example.com`/尾点/SAN-only（无 SAN
  有 CN → false）/IP v4 v6；
- **verify_chain**：§7.1 各 fixture 正负例 + now 注入边界（过期前 1ms/后 1ms）
  + 错误码→alert 映射逐例断言 + **mTLS 形态**：host/ip 全空时同一链验证通过
  （服务端验证客户端证书不查主机名——负例再验：host 空不等于跳过链验证）；
- **线程共享**：两线程并发 `tls_verify_chain` 同一 store（ASan/TSan 下绿）；
- 死亡契约：sign 前不 supports 的 scheme（FIBER_ASSERT）、leaf() 于空链。

## 8. 实施清单（顺序）

1. probe 两项：传统 DER 私钥探测行为；`X509_check_host` flags 与 BoringSSL
   SSL 栈行为对齐（SAN-only？CN 回落？）——各写 10 行 probe 程序定谳；
2. `TlsSignature.h/.cpp`（TlsPublicKeyView/TlsPrivateKey/supports 表/偏好序/
   sign/tls_verify）+ `TlsSignatureScheme` 补 0x0603；
3. `TlsCertificate.h/.cpp`（Certificate/Chain/TrustStore/tls_verify_chain +
   alert 映射表）；
4. fixture 生成脚本 + 签名向量生成脚本（python cryptography）+ 两个测试文件；
5. 文档收尾（状态行 + 实现记录）；format_code.sh；全量 ctest。

（预计 1.5-2 天；大头在 fixture 与测试，生产代码 ~600 行。）

## 9. 待拍板问题

1. ~~1.2 客户端 CertificateVerify~~ **已拍板（2026-09-21）**：mTLS 全链路支持
   ——既有 `TlsServerParam::client_certificate_mode`（None/Optional/Required）
   就是需求面，1.2+1.3 双方向客户端 CertificateVerify 构造规格进 §3.4，
   三态语义对齐见 §4.6；仅 md5sha1 组合 digest 维持非目标（不挡现代 mTLS）；
2. ~~`subject_line` 文本化~~ **已定谳（2026-09-21）**：BoringSSL 有
   `X509_NAME_oneline`（oneline `/CN=...` 格式，无需 BIO）；缓冲区装不下首
   entry 时**截断为空行而非报错**（停在最后一个完整 entry），实现按此语义
   并在测试钉死；仅日志用途，Invalid 只覆盖退化输入；
3. ~~信任锚的 `X509_STORE` 默认 flags~~ **已定谳（2026-09-21）**：
   BoringSSL 无 OpenSSL 式 security level/算法策略——`kDefaultParam`
   （x509_vpm.cc）仅 `X509_V_FLAG_TRUSTED_FIRST` + depth=100，"ssl_client"/
   "ssl_server" 两参数表 flags 也全为 0，**无 SHA-1 弱化可关也无白名单可开**；
   与 BoringSSL SSL 栈行为天然一致，不设额外 flags。trust 字段
   （X509_TRUST_SSL_CLIENT/SERVER）按 set_default 语义一并设置（锚侧信任
   检查——coding-only 的锚会被拒）；
4. **Required 无证书时 1.2 的 alert**：BoringSSL 1.2 对"发了 certificate_request
   收到空 cert 列表"发 `handshake_failure` 还是 `certificate_required`——
   **留 06**：属引擎消息层行为，实施 06 时对照 BoringSSL `ssl_server.cc`
   定谳（1.3 规定 `certificate_required`）。

## 12. 实现记录（2026-09-21）

### 12.1 交付物

| 文件 | 内容 |
|---|---|
| `include/fiber/tls/crypto/TlsSignature.h` + `src/tls/crypto/TlsSignature.cpp` | TlsKeyKind/TlsPublicKeyView/TlsPrivateKey（parse_der/parse_pem/supports/max_signature_len/sign）/tls_verify + `kTls13SignaturePreference`（7）/`kTls12SignaturePreference`（10）；10 行 scheme 表驱动（pkey 类型/PSS 模长/1.3 曲线 NID/版本门） |
| `include/fiber/tls/crypto/TlsCertificate.h` + `src/tls/crypto/TlsCertificate.cpp` | TlsCertPurpose/TlsCertificate（der 往返/public_key/validity/matches_host|ip|private_key/subject|issuer_line）/TlsCertificateChain（≤4）/TlsTrustStore（≤512 锚）/TlsCertVerification/tls_verify_chain + §4.5 alert 映射 switch |
| `include/fiber/tls/handshake/TlsCipherSuites.h` | `TlsSignatureScheme` 补 `EcdsaSecp521r1Sha512=0x0603`（共 10 值） |
| `tests/tls_certs/gen_tls_certs.py` + `regen.sh` | fixture 生成器（python cryptography），自校验后落 .h 常量 |
| `tests/TlsCertFixtures.h` / `tests/TlsSignatureVectors.h` | 机器生成：16 证书 + 9 私钥 PEM（含传统 PKCS#1/sec1/加密形态）+ 30 条签名 KAT |
| `tests/TlsSignatureTest.cpp`（12 用例）/ `tests/TlsCertificateTest.cpp`（23 用例） | §7.2 清单落地 |

### 12.2 偏差与定谳（相对设计稿）

1. **fixture 用 python cryptography 而非 openssl CLI**（§7.1 偏差）：CLI 造不出
   可控 EKU/SAN 组合/自定义 validity/PSS salt=hash_len；树按 §7.1 扩为
   16 证书（另含 P-521、RSA-1024、clientAuth、CN-only、无关根树）；
2. **KAT content 用合成形态**而非 RFC 8448 transcript 复用（§7.2 偏差）：
   原语对 content 无关，1.3 用 146B context-string 构造串、1.2 用 48B
   digest 形态、roundtrip 用 64B 任意串；30 向量 = 全部 (key,scheme,version)
   支持组合，python 侧 sign→verify 自校验后落盘；
3. **`TlsCertPurpose` 参数新增**（§4.4 签名已同步补）：probe 发现 BoringSSL
   set_default 设的不止 EKU purpose——一并 `set_trust(X509_TRUST_SSL_*)`
   （锚侧信任检查，coding-only 锚会被拒）；
4. **now 边界测试形态改 root-as-leaf ±1s**：set_time 取秒，±1ms 不可分辨；
   且 leaf_expired 的窗口与 intermediate 效期不相交，"过期前 1ms Trusted"
   形态在该树上不成立——root（自签锚）作 leaf 隔离出纯有效期边界；
5. **probe 定谳三条**（§8-1）：d2i_AutoPrivateKey 按元素数探测传统 PKCS#1/sec1
   DER（支持，往返测试钉死）；X509_check_host 必须显式
   `X509_CHECK_FLAG_NEVER_CHECK_SUBJECT` 才 SAN-only（无 CN 回落）；无尾点
   剥离（length-exact 比较，`example.com.` 不匹配）——三条均有测试；
6. **TlsKeyKind 收敛为 Rsa/Ec/Ed25519 三值**：曲线不作为 kind，是 supports()
   内 scheme 级约束（1.3 才查）；
7. **线程共享**：两线程 ×64 并发 tls_verify_chain 同一 store 绿（ASan 下零报告）。

### 12.3 测试结果

- 新增 35 用例全绿（含 4 死亡契约）；
- 全量 ctest **2260/2260** 绿（4 skipped 为既有 interop 门控，与本次无关）；
- ASan（clang-20 + libc++ 共享库，独立 `temp/_deps_asan`，配方见
  asan-sweep 记忆）：35 用例零 UAF/泄露报告；
- 生产代码 ~1100 行（头 316 + 实现 781），测试 ~840 行 + 生成器 ~350 行
  （另有两份机器生成 .h 共 ~53KB 入库）。
