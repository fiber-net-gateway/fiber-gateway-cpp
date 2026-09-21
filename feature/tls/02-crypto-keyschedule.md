# TLS 自研实现 · 02 crypto 适配层与密钥调度

日期：2026-09-21
分支：`tls`
状态：**已实现**（§10 清单 1-8 全部完成；§7.4/7.5 签名证书随 06/07 排期不变；
实现记录与偏差清单见 §12）

深度分级：原语适配层（§6）、1.3 密钥调度（§3）、1.2 PRF/密钥材料（§4）、密钥交换
（§5）为**深设计**（可直接实施）；签名（§7.4）与证书（§7.5）为**接口级设计**，
实施细化在 06/07 号（握手引擎）动工前补——它们的消费点在握手引擎，不在调度链上。

## 1. 定位与边界

`crypto/` 是**密码学编排层**：协议规定"何时、用什么输入、推导哪个密钥材料"（自己写），
密码学原语（HKDF/HMAC/哈希/KX/签名/X509）全部调 BoringSSL（01 号硬性规则：只准
密码学原语头，禁 `SSL`/`SSL_CTX`/BIO）。

**输出物只有两类**：

1. `TlsRecordCipher::init(suite, kind, key, iv)` 的 key/iv——调度树的终点；
2. 握手引擎直接消费的密钥材料——binder_key、verify_data、resumption_master、
   app traffic secret（KeyUpdate 输入）。

**不做的事**：

| 不做 | 归属 |
|---|---|
| transcript 维护（跑动哈希、HRR 重启、快照） | `handshake/TlsTranscript`（经 §6 适配层拿哈希原语） |
| 何时调用哪个阶段的决策 | 握手引擎 + HandshakeContext |
| nonce/seq 管理 | TlsRecordCipher（已完成） |
| exporter secret（exporter_master_secret） | 非目标（无消费方；树中位置留注释桩，一行加回） |
| OCSP/CRL/吊销 | 非目标 |
| 时间源 | 调用方注入 `now`（引擎无时钟；net 胶水取 EventLoop::now） |

依赖：`common`（IoResult、Base64、Assert）+ BoringSSL 原语头。对外头零 openssl include
（01 §3.1），BoringSSL 句柄一律 pimpl 或 opaque 定长存储。

## 2. 事实依据（代码现状）

- **suite 注册表尚不存在**：`TlsCipherSuites.h` 只有三个枚举（suite/group/sigalg），无
  suite→(AEAD, key_len, hash, PRF hash) 映射——本篇补 `TlsSuiteInfo` 注册表（§7.1），
  04/06/07 的协商逻辑与 05 cipher 的 `suite_spec`（现硬编码 switch）都迁到它上面。
- **TlsTypes.h 尚无 tls 专用错误码**（只有 alert 枚举）——本篇定错误模型（§6.2）。
- **cipher 已固化形状**：`init(suite, kind, key, iv)`，1.3 iv=12B 静态、1.2 fixed_iv=4B
  + explicit nonce=BE64(seq)；实例不可变 + 自持 seq，换钥=换实例。调度层只需产出
  (key, iv) 二元组。
- **BoringSSL API 探明**（头文件 grep 定谳）：
  - `hkdf.h`：`HKDF_extract(out_key, &out_len, digest, secret, secret_len, salt, salt_len)`
    ——输出 PRK 长度=hash 长度，out 需 `EVP_MAX_MD_SIZE` 容量；`HKDF_expand(out_key,
    out_len, digest, prk, prk_len, info, info_len)`——定长输出。
  - `curve25519.h`：`X25519_keypair(out_pub[32], out_priv[32])`、`X25519(out_shared[32],
    priv[32], peer_pub[32])`——raw 标量，**无需 pimpl**。
  - `mem.h`：`OPENSSL_cleanse`、`CRYPTO_memcmp`（常数时间比较）。
  - `pool.h`：`CRYPTO_BUFFER_new`——X509 无 BIO 路线的支点（probe 项 §9.1）。
- **库内先例**：`src/quic/QuicCrypto.cpp` 已用 `HKDF_extract/expand`（EVP_sha256 形态）
  且自带 `kTls13LabelPrefix = "tls13 "`——HKDF-Expand-Label 的编码骨架可对齐。
- **PEM→DER 无 BIO**：`common/util/Base64` 已存在，PEM armor（`-----BEGIN-----` 定界 +
  base64 体）自己剥，DER 进 `CRYPTO_BUFFER`。
- `IoErr` 已有 Invalid/NoMem/NotSupported 等通用值（映射见 §6.2）。

## 3. TLS 1.3 密钥调度（RFC 8446 §7.1）

### 3.1 推导树（本层实现的全部）

```
             0
             |
             v
   PSK(可空) -> HKDF-Extract = Early Secret ──(阶段 0，构造/set_psk 即推导)
             |
             +-> Derive-Secret(., "ext binder"|"res binder", "")    = binder_key        [08 号消费]
             +-> Derive-Secret(., "c e traffic", Hash(CH))          = client_early_traffic_secret
             |                                                       （0-RTT 写读共用，双方都推）
             v
       Derive-Secret(., "derived", "")
             |
             v
   (EC)DHE Z -> HKDF-Extract = Handshake Secret ──────────────────(阶段 1)
             |
             +-> Derive-Secret(., "c hs traffic", Hash(CH..SH))     = client_hs_traffic_secret
             +-> Derive-Secret(., "s hs traffic", Hash(CH..SH))     = server_hs_traffic_secret
             v
       Derive-Secret(., "derived", "")
             |
             v
   0 -> HKDF-Extract = Master Secret ─────────────────────────────(阶段 2)
             |
             +-> Derive-Secret(., "c ap traffic", Hash(CH..server Fin)) = client_app_traffic_secret_0
             +-> Derive-Secret(., "s ap traffic", Hash(CH..server Fin)) = server_app_traffic_secret_0
             +-> Derive-Secret(., "res master", Hash(CH..client Fin))   = resumption_master_secret [08 号]
             （"exp master" 同位置，非目标，桩注释）
```

- `Derive-Secret(secret, label, transcripts) = HKDF-Expand-Label(secret, label,
  Hash(transcripts), Hash.length)`；`Hash(transcripts)` 由调用方（HandshakeContext 持
  TlsTranscript）在正确时机取当前跑动哈希传入——**本层不持 transcript**。
- 每个 traffic secret 出 cipher 材料：`key = Expand-Label(s, "key", "", key_len)`、
  `iv = Expand-Label(s, "iv", "", 12)`（§7.2）。
- KeyUpdate（§3.5）：`secret_{N+1} = Expand-Label(secret_N, "traffic upd", "",
  Hash.length)`，再走同一 traffic_keys。

### 3.2 调用点与角色对称性

| 阶段方法 | transcript 输入 | 客户端时机 | 服务端时机 |
|---|---|---|---|
| `set_psk(psk)` | — | 构造后、组 CH 前 | 收含 PSK 的 CH 后、回 SH 前 |
| `binder_key(kind)` | "" | 组 CH binder 前 | 验 binder 前 |
| `client_early_traffic_secret(h)` | Hash(CH) | 发 CH 后（0-RTT 写） | 收 CH 后（0-RTT 读） |
| `handshake_secrets(Z, h)` | Hash(CH..SH) | 收 SH 后 | 组 SH 时（先推后写 EE..Fin） |
| `application_secrets(h)` | Hash(CH..server Fin) | 验完 server Fin 后 | 发完 server Fin 后 |
| `resumption_master_secret(h)` | Hash(CH..client Fin) | 发完 client Fin 后 | 验完 client Fin 后 |

**关键性质：调用序列双端完全一致**（上表逐行时机对齐），只有"哪个 secret 映射到写向
cipher"随角色翻转（客户端写=c_*、服务端写=s_*）。因此调度对象**角色无关**——一个实现
零 `is_client_` 分支，与 05 cipher 同构；方向选择留在引擎取 secret 时表达。

### 3.3 HKDF-Expand-Label 编码

```
HkdfLabel = BE16(length) || uint8(label_len) || "tls13 " + Label || uint8(ctx_len) || context
```

- label 全集是**常量表**（"key"/"iv"/"derived"/"c e traffic"/"c hs traffic"/"s hs
  traffic"/"c ap traffic"/"s ap traffic"/"ext binder"/"res binder"/"res master"/
  "traffic upd"，加桩 "exp master"）——编译期数组，无动态分配。
- 单一编码函数（cpp 内部）：`expand_label(out, out_len, secret, label, context)`，
  上限 255B info，断言不溢出。
- 与 QUIC `kTls13LabelPrefix` 同源；**1.2 不用** Expand-Label（走 §4 PRF）。

### 3.4 对象模型：阶段化（staged）

`TlsKeySchedule13` 是**一次性阶段机**：

- 构造 `(suite)`（断言 1.3 suite）即完成阶段 0（Early Secret，PSK 缺省为空）；
- `set_psk` 必须在构造后、任何 secret 推导前调用（PSK 影响 Early 及其后整棵树；
  顺序断言）；
- 每个阶段方法恰调用一次，阶段序号单调递增，`FIBER_ASSERT` 顺序——契约违例是引擎
  bug，不是协议失败；
- 中间 secret（Early/Handshake PRK）是对象私有状态，不外泄；
- **零分配**：全部 secret 是 §7.2 的 48B 定长 POD；对象本身无堆成员；
- HRR：**作废重建**（值语义，便宜）——transcript 已重启（message_hash 替换 CH1），
  旧树上无任何需要保留的状态。

### 3.5 与特殊路径的交互

- **0-RTT**：client_early_traffic_secret 双端推导；early exporter 桩。0-RTT 写路径
  归 ClientHandshakeEngine（01 §3.3），本层只供 secret。
- **HRR**：如上，重建调度对象 + 重启 transcript；`set_psk` 重新调（PSK 扩展会变）。
- **KeyUpdate**：调度对象已随握手引擎消亡——**app traffic secret_0 必须进
  `TlsConnectedState`**（01 §3.3 DTO 字段表的增补），KeyUpdate 的 N+1 推导做成自由
  函数 `tls13_key_update(secret)`，ConnectedEngine 自持当前代际。
- **1.3 无 CCS 换钥**：cipher 实例切换即全部（engine 换指针）。

## 4. TLS 1.2 PRF 与密钥材料（RFC 5246 §5/§6.3 + RFC 5288）

### 4.1 PRF

`PRF(secret, label, seed) = P_hash(secret, label || seed)`，`P_hash` = HMAC 链
（A(1)=HMAC(secret,A(0)‖..) 经典构造）。**hash 由 suite 决定**：SHA-384 套件
（0xC02C/0x0030）用 SHA-384 PRF，其余 SHA-256（RFC 5246 §6.1 强制）。输出按需取
N 字节，实现为内部 `prf_expand(hash, secret, label, seed, out)`。

### 4.2 材料推导与顺序坑（对照表）

| 项 | 公式 | 顺序坑 |
|---|---|---|
| pre_master | ECDHE 共享 Z（**定长不剥前导零**：X25519 32B、P-256 32B x 坐标） | Z 必须按曲线定长左垫零 |
| master_secret | `PRF(Z, "master secret", CR‖SR)[0..47]` | **CR 在前** |
| key_block | `PRF(master, "key expansion", SR‖CR)` 按需伸展 | **SR 在前（与 master 反向）** |
| verify_data | `PRF(master, "client finished"‖"server finished", Hash(handshake_messages))[0..11]` | hash = 跑动握手哈希 |

### 4.3 key_block 切片（AEAD 套件，MAC key 长度恒 0）

```
key_block = client_write_MAC(0) || server_write_MAC(0)
         || client_write_key(key_len) || server_write_key(key_len)
         || client_write_IV(4) || server_write_IV(4)
```

| suite | key_len | PRF hash | cipher fixed_iv |
|---|---|---|---|
| 0xC02B EcdheEcdsaAes128Gcm | 16 | SHA-256 | 4 |
| 0xC02C EcdheEcdsaAes256Gcm | 32 | SHA-384 | 4 |
| 0xC02F EcdheRsaAes128Gcm | 16 | SHA-256 | 4 |
| 0x0030 EcdheRsaAes256Gcm | 32 | SHA-384 | 4 |
| 0xCCA9/0xCCA8 Ecdhe*Chacha | 32 | SHA-256 | 4 |

产物直接喂两个 `TlsRecordCipher::init(suite, Tls12, key, iv)`（客户端写=client_write_*）。

**1.2 无 KeyUpdate**：重协商被拒（01 范围）→ 写密钥终身有效，cipher 实例不换；
seq 为 u64 无回绕之虞。故 1.2 不进 ConnectedState 的 secret 字段（只有 1.3 需要）。

## 5. 密钥交换（TlsKeyExchange）

- **X25519（首选组）**：raw 32B 标量，`curve25519.h` 三函数直调——**私钥可直接放
  POD**，不 pimpl。
- **P-256（次选组）**：BoringSSL 路线 = EVP_PKEY（生成/导出/derive），句柄必须
  pimpl：对象内放 opaque 定长缓冲（`alignas(16) uint8_t storage[64]`，尺寸 probe 定），
  只在 .cpp 侧还原。私钥导出为 SEC1/raw 可选（会话内不落盘，不需要）。
- **公开值变长**：X25519=32B；P-256 未压缩点=65B（0x04‖X‖Y）——`TlsKeySharePub`
  定长 65B 数组 + len。
- **Z 语义**：定长输出（32B），失败（无效对端公钥/点不在曲线上）= 协议失败
  （引擎映射 illegal_parameter / decode_error），不是断言。
- FFDHE2048 在枚举里但 01 范围外——注册表标记 NotSupported，协商跳过。
- 服务器长期证书私钥（RSA/ECDSA 签名用）**不在本类型**——归 §7.4 TlsSignature 的
  私钥材料；ECDHE 临时对每握手一份。

## 6. 原语适配层：`src/tls/crypto/TlsCryptoPrimitives.h`（内部头）

**全模块唯一触碰 openssl 头的文件**（01 §3.2 既定）。crypto/ 其余 .cpp 只 include 它；
签名/证书的 EVP_PKEY/X509 类型经 pimpl 不穿透到此头之上。

### 6.1 函数清单

| 原语 | 薄封装 | 备注 |
|---|---|---|
| HKDF | `hkdf_extract(prk_out, digest, secret, salt)` / `hkdf_expand(out, digest, prk, info)` | extract 出长=hash 长；PRK 缓冲 `EVP_MAX_MD_SIZE` |
| 哈希 | `Sha256Ctx/Sha384Ctx` 值类型：init/update/final/**copy**（快照） | transcript 的依赖点；ctx 可拷贝 |
| HMAC | `hmac(hash, key, data, out)` 单发 + `HmacCtx` 增量 | 1.2 PRF 与 binder |
| 随机 | `random_bytes(span)` | client_random/server_random/临时标量 |
| 清零 | `secure_wipe(void*, len)` = `OPENSSL_cleanse` | 编译器不可优化掉 |
| 常数时间比较 | `constant_time_equal(a, b, len)` = `CRYPTO_memcmp` | binder / verify_data |
| X25519 | 直调三函数 | 见 §5 |
| P-256 | EVP_PKEY 生成/derive（pimpl 尺寸 probe） | 见 §5 |
| 签名/验签 | EVP_DigestSign/Verify + PSS padding 设置 | §7.4 细化 |
| X509 | `CRYPTO_BUFFER_new` + X509 转换（probe 无 BIO 路线） | §7.5 细化 |

### 6.2 错误转换

- 原语失败（openssl 返回 0/NULL）统一：drain ERR 队列 → 返回失败 → 上层映射
  `IoErr`（Internal/NoMem/NotSupported 按语义），**不抛异常**（全库规则）；
- "对端数据坏"（无效公钥、坏签名、坏证书）是**协议失败**不是内部错误——KX/verify
  的返回结构区分 `PrimitiveFail`（我方内部）与 `BadPeerData`（引擎映射 alert），
  形态对齐 05 cipher 的 `{Status, ...}` 结构体先例（比塞进 IoErr 更能携带协议语义）。

## 7. API 草案（签名级，无实现）

### 7.1 suite 注册表（`TlsCipherSuites.h` 增补）

```cpp
struct TlsSuiteInfo {                    // 九 suite 常量表，constexpr 数组
    TlsCipherSuiteId suite;
    const EVP_无关的 hash 枚举;           // Sha256 | Sha384（自定义枚举，不漏 openssl）
    std::uint8_t key_len;                 // 16 | 32
    std::uint8_t tls13_iv_len;            // 12
    std::uint8_t tls12_fixed_iv_len;      // 4
    bool tls12_prf_sha384;                // PRF hash 选择
};
[[nodiscard]] const TlsSuiteInfo *tls_suite_info(TlsCipherSuiteId) noexcept; // null = 不支持
```

05 cipher 的 `suite_spec` switch 与未来协商逻辑共用此表（迁移属实施清单）。

### 7.2 密钥材料类型（`TlsKeySchedule.h`）

```cpp
struct TlsSecret {                       // POD，值初始化清零；上限 = SHA-384
    std::array<std::uint8_t, 48> buf{};
    std::uint8_t len = 0;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    void wipe() noexcept;                // 显式；无魔法析构（§8.1）
};
struct TlsTrafficKeys {
    std::array<std::uint8_t, 32> key{};  // key_len 生效前缀
    std::uint8_t key_len = 0;
    std::array<std::uint8_t, 12> iv{};   // 1.3 静态 iv / 1.2 fixed iv 不在此（1.2 走 key_block）
};
```

### 7.3 1.3 调度 + 1.2 材料（`TlsKeySchedule.h`）

```cpp
enum class TlsPskBinderKind : std::uint8_t { External, Resumption };

class TlsKeySchedule13 {                 // 阶段机，见 §3.4；零分配、角色无关
public:
    explicit TlsKeySchedule13(TlsCipherSuiteId suite) noexcept;
    common::IoResult<void> set_psk(std::span<const std::uint8_t> psk) noexcept;
    common::IoResult<TlsSecret> binder_key(TlsPskBinderKind) noexcept;          // ctx=""
    common::IoResult<TlsSecret> client_early_traffic_secret(std::span<const std::uint8_t> hash_ch) noexcept;
    common::IoResult<void> handshake_secrets(std::span<const std::uint8_t> z,
                                             std::span<const std::uint8_t> hash_ch_sh,
                                             TlsSecret &client_hs, TlsSecret &server_hs) noexcept;
    common::IoResult<void> application_secrets(std::span<const std::uint8_t> hash_ch_server_fin,
                                               TlsSecret &client_app0, TlsSecret &server_app0) noexcept;
    common::IoResult<TlsSecret> resumption_master_secret(std::span<const std::uint8_t> hash_ch_client_fin) noexcept;
};

[[nodiscard]] common::IoResult<TlsSecret> tls13_key_update(const TlsSecret &current) noexcept;
[[nodiscard]] common::IoResult<TlsTrafficKeys> tls13_traffic_keys(const TlsSecret &, TlsCipherSuiteId) noexcept;

// 1.2：自由函数（无阶段机——顺序由握手流程天然保证）
[[nodiscard]] common::IoResult<TlsSecret> tls12_master_secret(TlsCipherSuiteId, std::span<const std::uint8_t> z,
                                                              std::span<const std::uint8_t> client_random,
                                                              std::span<const std::uint8_t> server_random) noexcept;
struct Tls12WriteKeys { TlsTrafficKeys client; TlsTrafficKeys server; };
[[nodiscard]] common::IoResult<Tls12WriteKeys> tls12_key_block(TlsCipherSuiteId, const TlsSecret &master,
                                                               client_random, server_random) noexcept;
[[nodiscard]] common::IoResult<std::array<std::uint8_t, 12>> tls12_verify_data(TlsCipherSuiteId, const TlsSecret &master,
                                                                              bool client,  // "client finished" | "server finished"
                                                                              std::span<const std::uint8_t> handshake_hash) noexcept;
```

### 7.4 签名（`TlsSignature.h`，接口级）

```cpp
// CertificateVerify：1.3 内容 = "TLS 1.3, server/client CertificateVerify" || 0x00 || Hash(transcript)
//                    1.2 = 对握手消息按 sigalg 的摘要组合
[[nodiscard]] ... tls_sign(TlsSignatureScheme, 私钥材料, span content, out sig);   // 服务端
[[nodiscard]] ... tls_verify(TlsSignatureScheme, 对端公钥(证书内), span content, span sig); // 客户端
// 私钥材料：PEM 解析（Base64 + CRYPTO_BUFFER）→ EVP_PKEY pimpl
```

### 7.5 证书（`TlsCertificate.h`，接口级）

```cpp
// 解析（PEM/DER，无 BIO）+ 链构建验证（X509_STORE 填信任锚）+ RFC 6125 hostname
[[nodiscard]] ... tls_parse_chain(std::span<const char> pem) -> TlsCertificateChain;   // pimpl 句柄
[[nodiscard]] ... tls_verify_chain(const TlsCertificateChain &, const TlsTrustAnchors &,
                                   std::string_view hostname, std::int64_t now_ms) -> 验证结果枚举;
```

## 8. 安全要点

1. **秘密清零：显式 wipe、无魔法析构**。理由：cipher `init` 后 key 只活在
   `EVP_AEAD_CTX` 内（05 既定），调度产物在两个点位被消费完即 wipe——
   (a) traffic secret 交给 ConnectedState/cipher 后，调度对象内中间 secret 随对象
   消亡前统一 wipe（析构里调 secure_wipe 不算"魔法"——成员清零是确定性动作，无
   控制流副作用；但 TlsSecret 本体保持平凡类型，wipe 只在调度对象析构与引擎
   handoff 点显式调）。陷阱：堆上的 TlsSecret 移动语义——实现时 delete 拷贝、
   只留移动，移动后源 wipe。
2. **常数时间比较**：binder 验证、1.2 verify_data 比较一律 `CRYPTO_memcmp`——
   逐字节 `==` 会泄漏匹配前缀（FF-oracle 类）。
3. **Z 定长**：前导零不剥（GCM nonce/PRF 输入语义），X25519/P-256 均 32B。
4. **随机数**：client_random/server_random/临时标量全 `RAND_bytes`；失败 = 内部
   错误（握手 fatal internal_error），绝不静默降级。
5. **PSK 长度**：PSK 为空与"无 PSK"在树中同一形态（ikm=0 长度）——不因 PSK 有无
   改变调用面；resumption PSK 长度不泄漏 ticket 内容。
6. nonce、重放窗口、anti-replay 均不在本层（cipher/引擎/08 号）。

## 9. 测试计划（`tests/TlsKeyScheduleTest.cpp` 等）

### 9.1 probe（先行，方法同 05）

1. `HKDF_extract` 输出长度语义（out_len 返回值、PRK 缓冲上限）；
2. P-256 EVP_PKEY 生成/derive 的 pimpl 尺寸与 Z 定长性；
3. X509↔`CRYPTO_BUFFER` 无 BIO 路线（`X509_parse_from_buffer` 在本构建的存在性，
   缺则降级 d2i_X509）；
4. `EVP_MAX_MD_SIZE` 与 SHA-384 ctx 可拷贝性（transcript 快照的前提）。

### 9.2 KAT 与交叉验证

- **RFC 8448 §3（simple 1-RTT）**：每个中间 secret 与终态 key/iv 都有字面向量——
  调度层最权威的测试（网络恢复后取原文，同 05 的 TODO 同一批）；
- 1.2 PRF：RFC 5246 附录向量 + 测试侧手工 HMAC 链交叉（与被测零共享）；
- **端到端一致性回环**：schedule → traffic_keys → `TlsRecordCipher` seal/open
  互通（1.3 与 1.2 各自；1.2 另验 key_block 切片顺序坑）；
- KeyUpdate 链：secret_0→_1→_2 迭代向量（手工 HKDF 展开）；
- HRR 重建、PSK 分支（binder_key 两 kind）、错误路径（坏公钥→BadPeerData 非
  内部错）。

## 10. 实施清单（顺序）

1. probe 三~四项（§9.1）；
2. `TlsCryptoPrimitives.h/.cpp`（适配层全部原语）；
3. `TlsSuiteInfo` 注册表 + 05 cipher `suite_spec` 迁移到注册表（行为不变）；
4. `TlsKeySchedule.h/.cpp`：1.3 阶段机 + traffic_keys + key_update；
5. 1.2 PRF/master/key_block/verify_data；
6. `TlsKeyExchange`（X25519 先行，P-256 随后）；
7. 测试：KAT/回环/错误路径（§9.2）；
8. 文档收尾（状态行 + 实现记录）。
   （§7.4/7.5 签名证书的实施随 06/07 前置单独排期。）

## 11. 待拍板问题

1. **TlsSecret 拷贝语义**：禁拷贝只移动（推荐，wipe 点位明确）vs 平凡可拷贝
   （更省心但可能被隐式复制后遗忘）；
2. **suite 注册表的 hash 枚举**：自定义两值枚举（推荐，保 openssl 不穿透）vs 直接
   `const EVP_MD *`（穿透，违反 01 §3.1）——后者已否，仅记录；
3. **签名/证书细化时点**：06/07 动工前单独出补充节（推荐）vs 现在展开（会拉长本篇
   一倍且消费点未定）。

## 12. 实现记录（2026-09-21）

### 12.1 交付物

- `src/tls/crypto/TlsCryptoPrimitives.h/.cpp`：适配层全量原语（HKDF/HMAC/digest/
  random/wipe/ct-equal/X25519/P-256），crypto/ 唯一触碰 openssl 头的文件；
- `include/fiber/tls/crypto/TlsKeySchedule.h` + `src/tls/crypto/TlsKeySchedule.cpp`：
  1.3 阶段机（Early→Handshake→Application，binder/early-traffic 阶段 0 输出）、
  traffic_keys/key_update/finished_mac/psk_binder_mac、1.2 PRF
  master/key_block/verify_data；
- `include/fiber/tls/crypto/TlsKeyExchange.h` + `src/tls/crypto/TlsKeyExchange.cpp`：
  X25519 + P-256（TlsKxStatus::Ok/BadPeerData/Internal）；
- `include/fiber/tls/handshake/TlsCipherSuites.h`：`TlsSuiteInfo` 注册表（9 套件）；
- 测试：`tests/TlsKeyExchangeTest.cpp`（8 用例）、`tests/TlsKeyScheduleTest.cpp`
  （20 用例 + 死亡 1）、`tests/TlsRfc8448Constants.h`（机器生成 KAT 常量）。

### 12.2 KAT 流水线（§9.2 的执行强化）

RFC 8448 原文落盘 `temp/rfc8448.txt` → `temp/parse8448.py` 解析全部派生块并以
hashlib 独立重算自校验（272/272 通过，含跨页 dump 容错与 "(empty)" 特判）→
`temp/gen8448.py` 以独立参考实现重推整棵树（transcript 从明文消息拼接重构、
Finished MAC 双向、binder 全链、write-key/iv 全套，43/43 checks 通过）→ 生成
`tests/TlsRfc8448Constants.h`（40 个常量）。**全程零人工转录**——任何 hex 常量
都不经手抄，这本身是本轮最大的方法论收获（见 ⑪）。

### 12.3 偏差与修正清单（相对 §3/§7 设计稿）

1. **no-PSK Early Secret 的 IKM 是 zeros(hash_len)，不是空串**（RFC 8446 图中
   的 `0/""` 指 zeros；RFC 8448 §3 line 212 IKM=32 零字节、secret=33ad0a1c… 三重
   实证，BoringSSL `tls13_init_key_schedule` 同样传 `kZeroes`）。构造函数与
   master extract 两处均按此修正；
2. **Master Secret extract 的 IKM 同为 zeros(hash_len)**（§3 line 327 IKM=32 零
   字节），非空 span；
3. **"derived" 与 binder_key 的 Derive-Secret "" 语境 = Transcript-Hash("") =
   Hash(空输入)，是满长度 digest（SHA-256 e3b0c442…），不是 0 字节 context**；
   而 "finished"/"key"/"iv"/"traffic upd" 的空 context 是字面 0 字节 span。两者
   在 RFC 8446 图里长得一样，语义不同——以 trace §3/§4 与 BoringSSL
   `tls13_advance_key_schedule`/`tls13_psk_binder`（`EVP_Digest(nullptr,0,…)`）
   双重定证。生产侧由 `derive_secret_empty_transcript` helper 统一表达；
4. **binder 构造与 Finished 同构**（先 Expand-Label "finished" 得 fin_key 再
   HMAC），trace §4 注记明示，`tls13_psk_binder_mac` 按此实现并被 69fe131a/
   5588673e/3add4fb2 三级向量钉死；
5. **set_psk 空串归一化为 zeros(hash_len)**：EmptyPsk==NoPsk==ZeroPsk 三路一致
   （测试钉死）；
6. 适配层新增 `tls_digest_empty`（`EVP_Digest(nullptr,0,…)`，形态对齐
   BoringSSL）支撑 3 的语义，公开头零 openssl 穿透不变；
7. **X25519 全零共享拒绝放在适配层**（RFC 7748 §6.1）：BoringSSL X25519 对
   small-order 点返回成功，拒绝以常数时间比较实现，映射为 BadPeerData；
8. **1.2 master 恒 48 字节**断言（RFC 5246 §6.3 两种 PRF 下均如此）落在
   `tls12_key_block`/`tls12_verify_data`；本轮另修 prf 标签长度断言的基准错误
   （"client finished"/"server finished" 是 15 字符，原以 13 字符的
   "key expansion" 为上限——既有 bug，verify_data 路径此前必炸）；
9. `tls_hkdf_extract` 的 prk_out 容量契约是 64（EVP_MAX_MD_SIZE）而非 hash_len，
   TlsKeySchedule 内部一律经 64 字节 scratch 中转；
10. HkdfLabel 的 info 用栈上 255 字节组装，label/context 长度全断言（§3.3 无偏差）；
11. **KAT 转录教训（方法论）**：手抄/目测复制 hex 常量在本轮早期引入两类错误
    （binder info 标签长度笔误、对 derived 块 hash 字段的错误记忆），全部被
    "原文机械提取 + hashlib 重算 + 生成器直出 C++ 常量" 的流水线根治。后续
    06/07/08 号所有 KAT 一律走该路线，测试文件不再出现手抄 RFC 常量。

### 12.4 测试结果

- TLS 定向：149/149 绿（KeySchedule 20、KeyExchange 8、RecordCipher KAT 等）；
- 全量 `ctest`：2225/2225 绿（4 个互操作用例环境性 skip）；
- `./format_code.sh` 已跑，构建复验通过。

### 12.5 遗留

- §7.4 签名/§7.5 证书：随 06/07 前置补设计（原计划不变）；
- RFC 8448 §5-§7（HRR/客户端认证/兼容模式）向量已在落盘原文中，06/07 握手
  引擎可直接复用同一流水线生成常量；
- 1.2 侧无 RFC trace（8448 只覆盖 1.3）：测试以独立参考实现（手卷 RFC 5246
  P_hash，与被测零共享、经 §3/§4 校准）为 oracle。
