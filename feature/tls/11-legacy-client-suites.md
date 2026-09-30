# TLS 自研实现 · 11 客户端兼容老服务器：1.2 CBC 与静态 RSA 套件

日期：2026-09-27
分支：`feat/tls-legacy-client-suites`
状态：**已实现（2026-09-27；§13 的 #1–#7，#8 可选项未做。实现记录见 §16）**

## 1. 目标与边界

**目标**：我们作为**客户端**调用只支持 TLS 1.2、且不支持现代套件（无 AEAD，
或无 ECDHE）的老上游时，能完成握手并正常收发。

已定决策：

- **只动客户端**。服务端的套件选择不变——永远不选本文新增的套件
  （`scripts/interop/openssl_matrix.sh:488` 的 `s/1.2 CBC only offered` 继续
  预期失败，作为回归钉子）。
- **不加配置项**。客户端 ClientHello 在现有 AEAD 顺序**之后**固定追加 legacy
  尾巴；该顺序仅表达客户端偏好。遵循客户端顺序的服务器会优先选择现代套件，
  按自身偏好选择的服务器仍可选择 CBC 或静态 RSA，即使双方支持 ECDHE+AEAD。
  Finished 保护 CH/SH 转录不被篡改，但不保证套件优先级或协商结果与追加前相同。
  静态 RSA 不提供前向保密。

非目标：

| 不做 | 原因 |
|------|------|
| 服务端选择 CBC / 静态 RSA | 本次目标只在客户端；服务端 RSA 解密需 Bleichenbacher/ROBOT 常数时间防护，风险与收益不匹配 |
| TLS 1.0 / 1.1 | 目标服务器支持 1.2；1.0 隐式 IV、MD5+SHA1 PRF 是另一量级的工作 |
| 0xC023 / 0xC024 / 0xC028 / 0x003D | BoringSSL 无 SHA-384 CBC、无 AES-256-CBC-SHA256 AEAD，常数时间 digest 只支持 SHA1/SHA256（`tls_cbc.cc` `EVP_tls_cbc_record_digest_supported`）；0xC023 可行但 ECDSA 证书且无 GCM 的服务器极少，需要时加一行注册表 |
| DHE、3DES、Encrypt-then-MAC（RFC 7366） | 不在目标服务器画像内；3DES 块长 8，本文尺寸公式按 AES 的 16 写 |

本文修订 01 §1（"1.2 CBC 与静态 RSA KX 不做"）、05 §1 非目标表、06 §1 非目标表；
07 不变。

## 2. 事实依据

### 2.1 BoringSSL 把 CBC 当 AEAD 用

`include/openssl/aead.h` 的 "TLS-specific AEAD algorithms"，实现在
`crypto/cipher/e_tls.cc`：

| EVP_AEAD | key（MAC‖ENC） | nonce | MAC |
|---|---|---|---|
| `EVP_aead_aes_128_cbc_sha1_tls` | 20+16 = 36 | 16（CBC IV） | 20 |
| `EVP_aead_aes_256_cbc_sha1_tls` | 20+32 = 52 | 16 | 20 |
| `EVP_aead_aes_128_cbc_sha256_tls` | 32+16 = 48 | 16 | 32 |

libssl 自己就走这套接口（`ssl/ssl_aead_ctx.cc:95-113`）：CBC 与 AEAD 的差别只有
MAC key‖ENC key 拼成 AEAD key、方向绑定的 init，以及三个开关
`variable_nonce_included_in_record_` / `random_variable_nonce_` /
`omit_length_in_ad_`。open 同样是 `EVP_AEAD_CTX_open` 原地解密。
**MAC-then-encrypt、padding 校验与 Lucky13 常数时间处理全在 `aead_tls_open` 内**，
本层不写任何常数时间代码。

与现有三个 AEAD 的差异（本设计逐条消化）：

1. **方向绑定**：必须 `EVP_AEAD_CTX_init_with_direction(..., evp_aead_seal|open)`；
   对普通 AEAD 该函数等价于 `EVP_AEAD_CTX_init`（`aead.cc.inc:77`），可统一调用。
2. **nonce = 16 字节 CBC IV**，每条记录随机生成、明文放在记录头部（显式）。
3. **AD 为 11 字节**（seq‖type‖version，不含长度）。
4. **tag = 加密后的 MAC‖padding，长度随明文变化**：`seal_output_size` 仍可精确
   计算；open 的明文长度解密后才知道。
5. **`open_gather == nullptr`**：不能 body/tag 分离输入，只能连续 open。
6. **init 有堆分配**：`HMAC_CTX_new` + `EVP_CIPHER_CTX` 的 `cipher_data`（每方向
   两次，握手期一次性，记录路径零分配）。
7. **可按字节 move**：`AEAD_TLS_CTX` = `EVP_CIPHER_CTX`（值）+ `HMAC_CTX*` +
   mac_key，均无自指指针（`cipher_data` 在堆上），现有 memcpy+清零源的 move 语义
   成立。由 §12 的 ASan 用例兜底。

EVP 缓冲契约（`aead.cc.inc`）：seal 要求 `max_out_len - in_len >= tag_len`（精确
值）；open 要求 `max_out_len >= in_len`（MAC 与 padding 也要空间）；`out == in` 或
完全不重叠；失败时库把输出区清零。

### 2.2 现有代码里不受影响的部分

- 写路径：`TlsConnection::emit_sealed`（`TlsConnection.cpp:522`）与
  `TlsHandshakeContext::emit`（`TlsHandshakeContext.cpp:525`）都是
  `seal_output_size()` + 连续 `seal()`，尺寸全从 cipher 取。
- 读路径：两处都是 `min/max_ciphertext_size()` 前置检查 +
  `tls_record_open_dst_size()` + `tls_record_open_in_place()`；scratch 按
  `kTlsMaxCiphertextRecordSize`（2^14+2048）分配，CBC 最大记录
  16 + roundup(16384+32+1, 16) = 16448，够用。
- 失败映射：`AuthFail` 与解密前 `Malformed` 在两处都收敛为 `bad_record_mac`，
  `Overflow` 只在认证后报告——alert 层面不构成 padding oracle。
- `TlsStreamFd`、HTTP 层无记录开销常量；QUIC 只跑 1.3；1.2 客户端不做会话恢复
  （NST 只校验后丢弃），无 session 缓存需要兼容。
- 客户端引擎测试已经用真实 BoringSSL 服务端做对端（`TlsClientHandshakeEngineTest.cpp`
  的 `BoringServer`，`tls12_cipher` 选项），BoringSSL 服务端支持本文 10 个套件中的
  9 个（不含 0x003C）。

## 3. 新增套件与 offer 顺序

客户端 CH `cipher_suites` = `tls_effective_suite_order()`（现有 9 个，不变）+
`kTlsClientLegacySuites`（下表，**仅当 1.2 在版本窗口内**；QUIC 固定 1.3，自动排除）。
尾巴内顺序对齐 BoringSSL 客户端列表：前向保密的 CBC 在前，静态 RSA GCM 次之，
静态 RSA CBC 最后。

| 序 | id | OpenSSL 名 | kx | auth | 记录算法 | PRF |
|---|---|---|---|---|---|---|
| 1 | 0xC009 | ECDHE-ECDSA-AES128-SHA | ECDHE | ECDSA | AES-128-CBC + HMAC-SHA1 | SHA-256 |
| 2 | 0xC013 | ECDHE-RSA-AES128-SHA | ECDHE | RSA | AES-128-CBC + HMAC-SHA1 | SHA-256 |
| 3 | 0xC00A | ECDHE-ECDSA-AES256-SHA | ECDHE | ECDSA | AES-256-CBC + HMAC-SHA1 | SHA-256 |
| 4 | 0xC014 | ECDHE-RSA-AES256-SHA | ECDHE | RSA | AES-256-CBC + HMAC-SHA1 | SHA-256 |
| 5 | 0xC027 | ECDHE-RSA-AES128-SHA256 | ECDHE | RSA | AES-128-CBC + HMAC-SHA256 | SHA-256 |
| 6 | 0x009C | AES128-GCM-SHA256 | RSA | RSA | AES-128-GCM | SHA-256 |
| 7 | 0x009D | AES256-GCM-SHA384 | RSA | RSA | AES-256-GCM | SHA-384 |
| 8 | 0x002F | AES128-SHA | RSA | RSA | AES-128-CBC + HMAC-SHA1 | SHA-256 |
| 9 | 0x0035 | AES256-SHA | RSA | RSA | AES-256-CBC + HMAC-SHA1 | SHA-256 |
| 10 | 0x003C | AES128-SHA256 | RSA | RSA | AES-128-CBC + HMAC-SHA256 | SHA-256 |

说明：

- 0xC027 / 0x003C 针对"老但被加固过"的服务器（例如较老的 Windows Server，其
  Schannel 在 ECDHE_RSA 下只有 CBC 组合，合规加固又常删掉 SHA-1 MAC 套件），实际常见。
- 0xC009/0xC00A 与 0x009C/0x009D 的边际成本为零（记录层与握手路径复用）。
- RFC 5246 定义的 CBC 套件 PRF 一律 SHA-256；**记录 MAC 哈希与 PRF 哈希是两回事**，
  注册表分开表达（§4）。

## 4. 套件注册表（`include/fiber/tls/handshake/TlsCipherSuites.h`）

```cpp
enum class TlsCipherSuiteId : std::uint16_t {
    // ... 现有 9 个 ...
    // 1.2 legacy set — client offer only (feature/tls/11); the server never selects these.
    EcdheEcdsaAes128CbcSha = 0xC009,
    EcdheEcdsaAes256CbcSha = 0xC00A,
    EcdheRsaAes128CbcSha = 0xC013,
    EcdheRsaAes256CbcSha = 0xC014,
    EcdheRsaAes128CbcSha256 = 0xC027,
    RsaAes128GcmSha256 = 0x009C,
    RsaAes256GcmSha384 = 0x009D,
    RsaAes128CbcSha = 0x002F,
    RsaAes256CbcSha = 0x0035,
    RsaAes128CbcSha256 = 0x003C,
};

// 记录保护算法。CBC 三项是 BoringSSL 的 TLS 专用 CBC AEAD（MAC-then-encrypt）。
enum class TlsAeadAlgorithm : std::uint8_t {
    Aes128Gcm, Aes256Gcm, Chacha20Poly1305,
    Aes128CbcSha1, Aes256CbcSha1, Aes128CbcSha256,
};

// 1.2 套件名的 kx / auth 两半；1.3 为 None。
enum class TlsSuiteKx : std::uint8_t { None, Ecdhe, Rsa };
enum class TlsSuiteAuth : std::uint8_t { None, Rsa, Ecdsa };

// CBC 记录的 HMAC 长度（= MAC key 长度）；AEAD 为 0。
[[nodiscard]] constexpr std::uint8_t tls_record_mac_len(TlsAeadAlgorithm a) noexcept;

struct TlsSuiteInfo {
    TlsCipherSuiteId suite;
    TlsAeadAlgorithm aead;
    TlsHashAlgorithm hash; // 握手转录 / PRF 哈希，与 CBC 记录 MAC 无关
    std::uint8_t key_len;  // 加密 key：16 | 32（CBC 的 MAC key 另由 tls_record_mac_len 给出）
    bool is_tls13;
    TlsSuiteKx kx;
    TlsSuiteAuth auth;
};

inline constexpr std::array<TlsSuiteInfo, 19> kTlsSuiteRegistry{ /* 现有 9 项补 kx/auth + 表 3 的 10 项 */ };
```

- 现有 9 项补字段：1.3 三项 `None/None`；ECDHE-RSA 三项 `Ecdhe/Rsa`；
  ECDHE-ECDSA 三项 `Ecdhe/Ecdsa`。
- 服务端 `TlsServerHandshakeShared.cpp:43` 的 `suite_auth()` switch 改读
  `info->auth`（行为不变，消除第二份事实来源）。
- `TlsSuitePreference.h` 里的 `kTlsSuitePreference` / `tls_suites_chacha_first` /
  `tls_effective_suite_order` 与服务端遍历**不变**（legacy 不进该表，所以服务端
  天然不会选；ChaCha 重排也无需三档）。

## 5. 密钥调度

### 5.1 `TlsTrafficKeys`（`include/fiber/tls/crypto/TlsSecret.h:56`）

```cpp
struct TlsTrafficKeys {
    // 记录 cipher 的 key：AEAD 为加密 key；1.2 CBC 为 MAC key ‖ 加密 key 的拼接
    // （BoringSSL EVP_aead_*_cbc_*_tls 的 key 布局，ssl_aead_ctx.cc:97-104）。
    std::array<std::uint8_t, 64> key{}; // 32 → 64：最大 SHA-1 + AES-256 = 52
    std::array<std::uint8_t, 12> iv{};
    std::uint8_t key_len = 0; // 16|32 AEAD；36|52|48 CBC
    std::uint8_t iv_len = 0;  // 12（1.3）/ 4|12（1.2 GCM|ChaCha）/ 0（1.2 CBC：IV 逐记录显式）
};
```

1.3 与 QUIC 侧只按 `key_len` 读、按 `key.size()` 擦除，扩容无行为影响。

### 5.2 `tls12_key_block`（`src/tls/crypto/Tls12KeySchedule.cpp:128`）

RFC 5246 §6.3 的完整布局 `c_mac ‖ s_mac ‖ c_key ‖ s_key ‖ c_iv ‖ s_iv`：AEAD 的
mac 段长 0（现状），CBC 的 iv 段长 0（TLS 1.1+ 的 IV 是显式的，不从 key_block 派生）。

```cpp
const std::size_t mac_len = tls_record_mac_len(info.aead);
const std::size_t iv_len = mac_len != 0 ? 0 : info.aead == TlsAeadAlgorithm::Chacha20Poly1305 ? 12 : 4;
const std::size_t block_len = 2 * mac_len + 2 * info.key_len + 2 * iv_len;
std::array<std::uint8_t, 128> block{}; // 最大：AES256-SHA 2*20 + 2*32 = 104（prf 上限 128）
// prf(...) 不变
// 切片：client.key = c_mac ‖ c_key，server.key = s_mac ‖ s_key，
//       key_len = mac_len + info.key_len；iv 同现状（CBC 下 iv_len = 0）。
```

`tls12_master_secret` / `tls12_extended_master_secret` 的 `z.size() == 32 || 48`
断言对静态 RSA 的 48 字节 premaster 天然成立，只更新注释。

## 6. 记录层 `TlsRecordCipher`

### 6.1 接口变化

```cpp
// 每个实例只服务一个方向（本类既有契约）；CBC 的 EVP 在 init 时即绑定方向。
enum class TlsRecordDirection : std::uint8_t { Seal, Open };

[[nodiscard]] common::IoResult<void> init(TlsCipherSuiteId suite, TlsRecordProtectionKind kind,
                                          TlsRecordDirection direction,
                                          std::span<const std::uint8_t> key,
                                          std::span<const std::uint8_t> iv) noexcept;

// 1.2 CBC 套件（MAC-then-encrypt）。
[[nodiscard]] bool is_cbc() const noexcept { return mac_len_ != 0; }
// 链适配拆出的独立 tag 长度：AEAD 16；CBC 0（MAC 与 padding 在 CBC 密文体内部）。
[[nodiscard]] std::size_t detached_tag_len() const noexcept { return is_cbc() ? 0 : 16; }
```

`explicit_nonce_len()` 语义扩展：1.2 GCM 8 / ChaCha 0 / **CBC 16（随机 IV）**。

调用点（机械修改）：生产 7 处——`Tls12ClientHandshake.h:133`、
`Tls12ServerHandshake.h:148`（`swap_cipher_12` 增加 direction 参数，两处调用方按
`write_cipher()` / `read_cipher()` 传 Seal / Open），`Tls13ClientHandshake.h:167`、
`Tls13ServerHandshake.h:168`、`TlsClientHandshakeShared.cpp:149`、
`TlsConnection.cpp:180,210`；测试约 20 处，`fuzz/tls_connection_fuzzer.cpp` 1 处。

### 6.2 成员

```cpp
EVP_AEAD_CTX aead_ctx_{};
TlsCipherSuiteId suite_;
TlsRecordProtectionKind kind_;
TlsRecordDirection direction_ = TlsRecordDirection::Seal; // 新增
std::array<std::uint8_t, 12> iv_{};    // CBC 不用
std::uint8_t explicit_nonce_len_ = 0;  // GCM 8 / ChaCha 0 / CBC 16
std::uint8_t mac_len_ = 0;             // 新增：CBC 20|32；0 = AEAD 套件（兼作 CBC 判别）
std::uint64_t seq_ = 0;
bool initialized_ = false;
```

`move_from` 同步搬 `direction_` / `mac_len_`。

### 6.3 `suite_spec` 与 init

`suite_spec` 增加三个 CBC 映射（`EVP_aead_aes_128_cbc_sha1_tls` /
`EVP_aead_aes_256_cbc_sha1_tls` / `EVP_aead_aes_128_cbc_sha256_tls`），CBC 的
`key_len = mac_len + enc_key_len`、`iv_len = 0`、`explicit_nonce_len = 16`、
`mac_len = 20|32`。init 统一调用：

```cpp
EVP_AEAD_CTX_init_with_direction(&aead_ctx_, spec.aead, key.data(), spec.key_len,
                                 EVP_AEAD_DEFAULT_TAG_LENGTH,
                                 direction == TlsRecordDirection::Seal ? evp_aead_seal : evp_aead_open);
```

CBC 套件配 `TlsRecordProtectionKind::Tls13` 仍由注册表 `is_tls13` 检查拒绝（Invalid）。

### 6.4 尺寸函数（CBC 分支）

```
cbc_body(n)             = roundup(n + mac_len + 1, 16)           // IV 之后的密文
seal_output_size(n)     = 16 + cbc_body(n)                        // 精确
open_output_size(len)   = len - 16                                // 容量（EVP 要求 >= in_len）
min_ciphertext_size()   = 16 + roundup(mac_len + 1, 16)           // SHA-1 48 / SHA-256 64
max_ciphertext_size()   = 2^14 + 2048                             // 不变
```

AEAD 分支全部不变。

### 6.5 seal / seal_scatter（CBC）

```cpp
// seal：原地契约 dst.data() + 16 == plaintext.data()（与 GCM 的 +8 同形）。
FIBER_ASSERT(direction_ == TlsRecordDirection::Seal);
std::array<std::uint8_t, 16> iv{};
if (!tls_random_bytes(iv)) { return {Status::AuthFail, 0}; } // seal 失败沿用 AuthFail，调用方映射为写失败
std::memcpy(dst.data(), iv.data(), 16);
// ad[11] = BE64(seq_) ‖ inner_type ‖ 0x03 0x03
EVP_AEAD_CTX_seal(&aead_ctx_, dst.data() + 16, &written, dst.size() - 16, iv.data(), 16,
                  plaintext.data(), plaintext.size(), ad.data(), 11);
FIBER_ASSERT(written == out_len - 16);
++seq_;
```

`seal_scatter`：dst_prefix ≥ 16 放随机 IV；dst_ct 接收前 n 字节密文（原地允许）；
dst_tag 接收其余 `cbc_body(n) - n` 字节（MAC 尾部 ‖ padding，21..48 字节，
**断言按该精确值**而不是 16）；`extra_in` 不支持（1.2 本就不用）。
wire = prefix ‖ ct ‖ tag，与 GCM 同形（CBC 链式加密使最后一个不满块跨越 ct/tag，
由 `aead_tls_seal_scatter` 处理）。

`kTlsRecordVersionTls12` 固定写入 AD 与现状一致。

### 6.6 open / open_scatter（CBC）

`open()` 在 CBC 下直接委托 `open_scatter`，只保留一份实现：

```cpp
// open()：
if (is_cbc()) {
    if (length < min_ciphertext_size() || length > max_ciphertext_size()) { return malformed; }
    FIBER_ASSERT(ciphertext.size() >= length);
    return open_scatter(outer_type, legacy_version, length, ciphertext.first(16),
                        ciphertext.subspan(16, length - 16), {}, dst);
}

// open_scatter() CBC 分支：
if (length < min || length > max || (length - 16) % 16 != 0) {
    return malformed; // 公开信息，解密前拒绝（与 BoringSSL "publicly invalid" 一致）
}
FIBER_ASSERT(direction_ == TlsRecordDirection::Open);
FIBER_ASSERT(explicit_nonce.size() >= 16 && tag.empty());
FIBER_ASSERT(body.size() >= length - 16 && dst.size() >= length - 16);
// 重叠时 dst.data() == body.data()
std::array<std::uint8_t, 16> iv; std::memcpy(iv.data(), explicit_nonce.data(), 16);
// ad[11] = BE64(seq_) ‖ outer_type ‖ legacy_version（收到的头部原样）
if (EVP_AEAD_CTX_open(&aead_ctx_, dst.data(), &written, length - 16, iv.data(), 16,
                      body.data(), length - 16, ad.data(), 11) != 1) {
    return {Status::AuthFail, ...}; // padding 错与 MAC 错不可区分（库内常数时间）
}
if (written > kTlsMaxPlaintextSize) { return {Status::Overflow, ...}; } // 认证后
++seq_;
return {Status::Ok, outer_type, written};
```

`max_out_len` 传精确的 `length - 16`，避免 dst 超出部分与输入发生部分重叠而被
`check_alias` 拒绝。

### 6.7 防 oracle 规则（实现与评审要点）

- 解密前只检查公开长度（上下界、块对齐）；其余一切失败统一 `AuthFail`。
- 不在 cipher 外"预读"明文或 padding 字节；`Overflow` 只在认证通过后报。
- 两个调用方已把 `AuthFail` 与 `Malformed` 映射为同一个 `bad_record_mac`（§2.2），
  无需改动。

### 6.8 AD 构造去重

现有 1.2 路径里 13 字节 AD 的拼装写了四遍，CBC 再加两处。抽一个局部 helper
`write_ad12(aad, seq, type, version)` 返回前 11 字节，AEAD 调用方再追加 2 字节长度。

## 7. 链适配 `TlsRecordCipherChain`

把写死的 `16`、按 kind/nonce 分支的收缩逻辑，改为由 cipher 提供的两个量驱动，
三种 1.2 构造与 1.3 共用一套代码：

```cpp
const std::size_t expl = cipher.explicit_nonce_len();   // 0 | 8 | 16（1.3 恒 0）
const std::size_t tag = cipher.detached_tag_len();      // 16 | 0（CBC）
const std::size_t body_len = length - tag - expl;
std::array<std::uint8_t, 16> nonce_prefix{};            // 8 → 16
// chain_contiguous(payload, body_off + body_len, tag, ...)：tag = 0 时恒成功，传空 span
// 原地收缩统一为：
if (expl > 0) { payload.consume(expl); }
payload.trim_end(length - expl - result.plain_len);     // GCM 16、ChaCha 16、1.3 tag+type+pad、CBC MAC+pad
```

- `tls_record_open_dst_size` 已按 `explicit_nonce_len()` 计算，CBC 为 `length - 16`，
  恰为 EVP 要求的容量，不改。
- 跨节点记录：现有"先把 body+tag 聚拢到 dst、再原地解密"的退化路径对 CBC 原样适用
  （CBC 必须连续输入，这条路径正好满足）。
- seal 两个链函数只需把 `off` 与 tag 容量改为通用写法；`seal_in_place` 的
  `dst_tailer` 容量说明改为"AEAD 16/17，CBC 最多 48"（目前只有测试调用）。

## 8. 客户端套件 offer（`TlsSuitePreference.h`、`TlsClientHandshakeShared.*`）

```cpp
// TlsSuitePreference.h
// 仅客户端使用的尾巴（feature/tls/11）：追加在 AEAD 顺序之后，服务端从不遍历。
inline constexpr std::array<std::uint16_t, 10> kTlsClientLegacySuites{
        0xC009, 0xC013, 0xC00A, 0xC014, 0xC027, 0x009C, 0x009D, 0x002F, 0x0035, 0x003C,
};
// CH 的 cipher_suites：有效 AEAD 顺序 + （offer_tls12 时）legacy 尾巴。静态存储，一次计算。
[[nodiscard]] std::span<const std::uint16_t> tls_client_offer_suites(bool offer_tls12) noexcept;
```

- `TlsClientHandshakeShared.cpp:49`：
  `in.cipher_suites = tls_client_offer_suites(cfg.min_version <= kTlsVersionTls12 && cfg.max_version >= kTlsVersionTls12);`
- `tls_client_suite_offer_index()`（返回下标、用 `size()` 作哨兵）改为
  `tls_client_suite_offered(raw)`（扫描 `kTlsSuitePreference` 与 legacy 尾巴）；
  三个调用点 `Tls12ClientHandshake.cpp:72`、`Tls13ClientHandshake.cpp:72,188` 同步。
  1.3 SH 选中 legacy 套件仍被 `is_tls13` 检查拒绝；窗口外的 1.2 SH 在 fork 处已被
  版本下限拒绝，所以成员检查无需感知窗口。

## 9. 客户端 1.2 握手（`Tls12ClientHandshake.*`）

### 9.1 Certificate：leaf 密钥类型必须匹配套件 auth

对齐 BoringSSL `ssl_check_leaf_certificate`（在验链之前）：

```cpp
const auto leaf_key = peer_chain_.leaf().public_key();
// ... has_value 检查 → UnsupportedCertificate
const bool fits = suite_info()->auth == TlsSuiteAuth::Rsa
                          ? leaf_key->key_kind() == TlsKeyKind::Rsa
                          : leaf_key->key_kind() != TlsKeyKind::Rsa; // ECDSA 套件：EC 或 Ed25519（RFC 8422）
if (!fits) { fail(TlsAlertDesc::IllegalParameter); return; }
```

静态 RSA 需要 RSA 公钥来加密，这个检查是它的前提；它同时收紧了现有 ECDHE 套件：
今天 ECDHE-RSA 套件配 EC 证书且 SKE 用 ECDSA 签名能通过，改后在 Certificate 处
以 illegal_parameter 拒绝。不支持的公钥类型由 `TlsCertificate::public_key()`
返回 Invalid，握手发送 unsupported_certificate，不能进入 `key_kind()` 的 panic 分支。

证书用途与套件绑定检查：`TlsCertificate::allows_key_usage(TlsCertificateKeyUsage)`
封装 BoringSSL `X509_get_key_usage()`，静态 RSA 要求 KeyEncipherment，ECDHE 要求
DigitalSignature。不声明 KeyUsage 时允许；畸形或重复扩展拒绝。此检查独立于
`verify_peer`，在 Certificate 处以 bad_certificate 拒绝，尚未发送 ClientKeyExchange。

### 9.2 状态分支

```cpp
st_ = suite_info()->kx == TlsSuiteKx::Rsa ? St::ExpectCrShd12 : St::ExpectSke12;
```

静态 RSA 下收到 SKE 会落到 `ExpectCrShd12` 的 default 分支 → unexpected_message，
无需新代码。反之 ECDHE 套件缺 SKE 同理。

### 9.3 `send_client_flight_12`：静态 RSA 的 ClientKeyExchange

```cpp
if (suite_info()->kx == TlsSuiteKx::Rsa) {
    // RFC 5246 §7.4.7.1：premaster = client_version（CH 的 0x0303）‖ 46 随机字节，
    // RSAES-PKCS1-v1_5 加密到 leaf 公钥，u16 长度前缀。
    z12_ = TlsKxShared{};
    z12_.len = 48;
    z12_.z[0] = 0x03;
    z12_.z[1] = 0x03;
    if (!tls_random_bytes({z12_.z.data() + 2, 46})) { fail(InternalError); return false; }
    std::array<std::uint8_t, kClientMaxRsaCiphertext> ct{}; // 1024：RSA-8192 上限
    const auto ct_len = leaf_key->rsa_encrypt_pkcs1(z12_.bytes(), ct);
    // ... 失败 → InternalError
    cke_len = tls_encode_client_key_exchange_rsa({ct.data(), *ct_len}, scratch_);
} else {
    // 现有 ECDHE：u8 长度前缀的点
}
// 之后的 EMS / master / key_block / CV / CCS / Fin 全部不变（z12_ 统一承载 premaster）
```

`hello_.kx`（CH 为 1.3 预生成的 key share）在静态 RSA 下不使用，析构照常擦除。

### 9.4 （可选，单独提交）容忍缺失 renegotiation_info

`Tls12ClientHandshake.cpp:64-71` 目前要求 SH 必须回显空 RI，早于 RFC 5746 的服务器
会以 handshake_failure 失败。BoringSSL 客户端在初次握手允许缺失
（`ssl/extensions.cc:713-721`），而我们本来就拒绝重协商，放宽是安全的：

```cpp
if (sh.has_renegotiation_info && (sh.renegotiation_info.size() != 1 || sh.renegotiation_info[0] != 0)) {
    fail(TlsAlertDesc::HandshakeFailure);
    return;
}
```

目标服务器支持 TLS 1.2，基本都实现了 RFC 5746，所以这一项只在遇到时才需要；
注意 OpenSSL 3 客户端默认是严格的，BoringSSL 是宽松的。

## 10. 新增的密码学 / 编解码 API

`include/fiber/tls/crypto/TlsSignature.h`，`TlsPublicKeyView`（`TlsSignature.cpp`
仍是唯一接触 `EVP_PKEY` 的地方）：

```cpp
// RSAES-PKCS1-v1_5 加密到本公钥（1.2 静态 RSA 的 ClientKeyExchange）。非 RSA 公钥或
// out 小于模长 → Invalid；返回密文长度（= 模长字节数）。
[[nodiscard]] common::IoResult<std::size_t> rsa_encrypt_pkcs1(std::span<const std::uint8_t> in,
                                                              std::span<std::uint8_t> out) const noexcept;
```

实现：`EVP_PKEY_get0_RSA` + `RSA_encrypt(..., RSA_PKCS1_PADDING)`（BoringSSL 公开 API）。

`include/fiber/tls/handshake/TlsHandshakeCodec.h`：

```cpp
// 1.2 静态 RSA ClientKeyExchange（RFC 5246 §7.4.7.1）：带 u16 长度前缀的 EncryptedPreMasterSecret。
[[nodiscard]] common::IoResult<std::size_t>
tls_encode_client_key_exchange_rsa(std::span<const std::uint8_t> encrypted, std::span<std::uint8_t> scratch) noexcept;
```

不加 decode（服务端不选 kRSA，codec fuzzer 不受影响）。

## 11. 刻意不改的部分

| 位置 | 理由 |
|---|---|
| 服务端套件选择、`kTlsSuitePreference`、ChaCha 重排 | legacy 不进服务端表 |
| `TlsConnection`、`TlsHandshakeContext` 的 seal/open 编排与 alert 映射 | 尺寸全从 cipher 取；失败已统一为 `bad_record_mac`（§2.2） |
| `TlsRecordReader/Writer`、`TlsStreamFd`、HTTP 层 | 无记录开销常量 |
| QUIC | 仅 1.3；legacy 尾巴只在 1.2 进入版本窗口时 offer |
| HTTP/2 over CBC | RFC 9113 §9.2.2 对端"MAY"视为 INADEQUATE_SECURITY；我们作为客户端从宽（与 BoringSSL 一致），不拦截 |
| ticket 服务 | 服务端从不以 legacy 套件铸造 ticket；1.2 客户端不做恢复 |

## 12. 测试计划

**TlsRecordCipherTest**（CBC 的参考实现必须**独立于** `EVP_aead_*_tls`：测试里用
BoringSSL 的 `HMAC` + `EVP_aes_*_cbc` 手工构造/拆解记录，否则就是自测自）：

- 三种 CBC 算法 × open 已知答案：手工构造 IV‖CBC(pt‖MAC‖pad) → `open` 还原；
- seal → 独立解密：校验 IV 前缀、padding 字节全为 pad_len-1、MAC 覆盖
  seq‖type‖0x0303‖len‖pt；n ∈ {0, 1, 15, 16, 17, 31, 32, 16384}；
- `seal_output_size` 在 n = 0..64 与 16384 下与实际输出逐一相等；两次 seal 同一明文
  IV 不同；
- 篡改矩阵：改 IV / 密文体 / MAC / padding 值 / padding 长度超界 → `AuthFail`；
  长度非块对齐、小于下限、超上限 → `Malformed` 且 seq 不前进；
- 独立构造的 16385 字节明文且认证通过 → `Overflow`；
- seq 绑定：重放第一条记录到第二个位置 → `AuthFail`；
- 方向契约：Seal 实例调 open → `EXPECT_DEATH`（现有测试已在用 death test）；
- move：中途 move 构造/赋值后继续收发；在 ASan 构建（fuzz 构建树）下跑，确认
  `HMAC_CTX`/`cipher_data` 无泄漏、无 double free；
- init 拒绝：CBC 套件配 Tls13 kind、key 长度 ≠ mac+enc、iv 非空 → Invalid；
- seal_scatter 与 seal 的产物都能被 open 还原；CBC 的 open_scatter 要求 tag 为空。

**TlsRecordCipherChainTest**：CBC 记录在每个字节偏移处切成两个节点（跨 IV、跨体）→
原地/转录两条路径的明文一致、视图收缩后可读字节恰为明文；seal_in_place / transcribe
的 CBC 形态。

**TlsKeyScheduleTest**：CBC key_block 切片与测试内独立的 P_SHA256 参考实现
（BoringSSL HMAC）逐字节比对，覆盖 SHA-1/SHA-256 MAC、AES-128/256；AEAD 布局回归不变。

**TlsSuitePreferenceTest**：服务端顺序不变；`tls_client_offer_suites(true)` = 有效顺序
+ legacy 尾巴，`(false)` 不含尾巴；尾巴全部在注册表内且 `!is_tls13`，kx/auth 与名字相符。

**TlsClientHandshakeEngineTest**（`BoringServer` 对端，`tls12_cipher` 逐个选择；ECDSA
套件用 P-256 证书）：

- 9 个 BoringSSL 支持的套件（0x003C 除外）：握手完成、`state.suite` 与
  `SSL_get_current_cipher` 一致、双向应用数据（长度 1/15/16/17/16384，覆盖 padding
  边界）；
- 负例（沿用 `expect_reject_after_sh_mutation`）：
  - ECDSA 证书服务端，把 SH 套件改成 0x002F → Certificate 处 illegal_parameter（§9.1）；
  - ECDHE-RSA-AES128-SHA 服务端，把 SH 套件改成 0x002F → 收到 SKE → unexpected_message；
  - AES128-SHA 服务端，把 SH 套件改成 0xC013 → 缺 SKE → unexpected_message；
  - CBC 下篡改服务端 Finished 记录 → bad_record_mac；
- **修改既有用例** `TlsClientHandshake12Reject.SuiteNotOfferedAborts`：它用 0x002F 当
  "从未 offer 的套件"，改为 0x000A（3DES）。

**TlsConnectionTest**：`BoringSsl12AppAndSealedCloseNotify` 参数化一个 CBC 套件与一个
静态 RSA 套件。

**interop**（`scripts/interop/openssl_matrix.sh`）：

- 新增客户端用例：表 3 的 10 个套件逐个 `openssl s_server -tls1_2 -cipher X`
  （ECDSA 两项用 P-256 证书）——0x003C 只能在这里验证；
- 新增 `c/1.2 1 MiB, 512-byte records` 的 AES128-SHA 与 ECDHE-RSA-AES128-SHA256 变体；
- 翻转 `:380`（non-ECDHE only）与 `:381`（CBC only）为 ok，并断言协商到的套件；
- `:488` `s/1.2 CBC only offered` 保持 fail（钉住服务端不选 legacy）。

**fuzz**：`tls_fuzz_seedgen` 增加 AES128-SHA / ECDHE-RSA-AES128-SHA 的客户端种子；
`tls_connection_fuzzer` 增加 CBC cipher 状态选择。`-runs=0` 回放语料加短时运行。

## 13. 提交拆分

| # | 提交 | 内容 |
|---|---|---|
| 1 | `docs(tls): design legacy CBC and RSA suites for the client` | 本文 + 01/05/06 非目标条目修订 |
| 2 | `refactor(tls): add kx/auth to the suite registry` | 注册表字段；`suite_auth()` 读注册表；`tls_client_suite_offered`；无行为变化 |
| 3 | `fix(tls): reject a server leaf that does not match the suite's auth` | §9.1 + 负例 |
| 4 | `refactor(tls): bind record ciphers to a direction at init` | §6.1 签名变更 + 全部调用点 |
| 5 | `feat(tls): TLS 1.2 CBC record protection` | §5、§6、§7 + cipher/chain/key schedule 单测（套件尚未 offer） |
| 6 | `feat(tls): offer ECDHE CBC suites from the client` | 注册 0xC009/0xC013/0xC00A/0xC014/0xC027 + offer 尾巴 + 引擎测试 + interop + 种子 |
| 7 | `feat(tls): client-side static RSA key exchange` | §9.2、§9.3、§10 + 注册 0x009C/0x009D/0x002F/0x0035/0x003C + 测试 + interop 翻转 |
| 8 | `fix(tls): accept a ServerHello without renegotiation_info`（可选） | §9.4 |

每个提交独立可编译、`ctest --test-dir build` 全绿。

## 14. 风险与取舍

- **方向传错**：CBC 下 EVP 会直接失败；加上 `FIBER_ASSERT(direction_ ...)` 和端到端
  用例，能在第一次握手时暴露。
- **性能**：AEAD 路径每条记录只多一个可预测分支。CBC 路径每条记录多一次
  `RAND_bytes(16)`，加上 HMAC 和串行的 CBC 加密，吞吐明显低于 GCM——只影响协商到
  legacy 套件的连接。
- **安全姿态**：客户端会接受无前向保密（静态 RSA）和 MAC-then-encrypt（CBC）的
  连接。服务器按自身偏好选择时，即使支持 ECDHE+AEAD，也可能选择这些旧套件；
  追加到客户端列表末尾并不能防止这种选择。CH 多 20 字节，客户端指纹（JA3）会变化。
- **记录层"零分配"措辞**：CBC 实例在 init 时有两次堆分配，05 §1 的描述要相应修订。

## 15. 工作量

| 部分 | 生产代码 | 测试 |
|---|---|---|
| 注册表 + offer + auth 检查（#2、#3） | ~90 | ~80 |
| direction 签名（#4） | ~20 | ~25（机械修改） |
| key_block + 记录层 + 链（#5） | ~180 | ~450 |
| ECDHE CBC 启用（#6） | ~15 | ~150（+ interop ~15 行） |
| 静态 RSA 客户端（#7） | ~100 | ~120（+ interop 翻转） |
| RI 放宽（#8，可选） | ~5 | ~30 |
| **合计** | **~410** | **~850** |

预计 3–4 个工作日（含 interop 与 fuzz 回放）。

## 16. 实现记录（与设计稿的差异）

- **方向参数一路传到 1.3**：除 `swap_cipher_12` 外，1.3 两侧的 `swap_cipher` 也加了
  `TlsRecordDirection` 参数，所有调用点按 `read_cipher()` / `write_cipher()` 显式传
  Open / Seal；测试里的 `init_cipher` 辅助函数默认 Seal（AEAD 忽略方向），CBC 用例显式传。
- **CBC 记录测试单独成文件** `tests/TlsRecordCipherCbcTest.cpp`：参考实现用 `AES_cbc_encrypt`
  + `HMAC` 手工拼记录，与 `EVP_aead_*_tls` 不共享代码；另含 ASan 下由 fuzz 构建覆盖的
  move 语义（`tls_connection_fuzzer` 新增 config bits 2-3 选择 CBC 记录套件）。
- **AD 去重**（§6.8）落为 `write_ad12_prefix()`，AEAD 与 CBC 共用。
- **interop 脚本读取协商结果的方式**：`s_server -www` 页面的 `New, <ver>, Cipher is <c>`
  打印的是套件的**最低**协议版本（`SSL_CIPHER_get_version`：SHA-1 CBC 套件为 TLSv1.0、
  `AES128-SHA` 为 SSLv3），不是会话版本。脚本改为优先读 `SSL_SESSION_print` 的
  `Protocol  :` / `Cipher    :` 两行，`New,` 行只作回退。结果：112/112（OpenSSL 3.0.13）。
- **§9.4（放宽 renegotiation_info）未做**：目标服务器支持 TLS 1.2，按设计遇到再加。

### PR #43 评论修复回归

- 证书用途覆盖未声明、仅 digitalSignature、仅 keyEncipherment、两者都有、畸形和重复扩展。
- 静态 RSA 与 ECDHE 均在 verify_peer 开/关下验证用途；拒绝时只输出 fatal alert，
  不发送 ClientKeyExchange、CCS 或 Finished。
- 不支持的叶证书公钥在 Certificate 阶段拒绝，不使进程退出。
- 保留遵循客户端顺序时 AEAD 优先的测试，另覆盖服务端强制自身顺序时选择静态 RSA CBC。
