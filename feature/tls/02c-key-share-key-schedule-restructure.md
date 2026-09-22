# 02c：KeyExchange 类层次化 + KeySchedule 按版本拆分（结构重构设计）

> 状态：**已实施**（Phase 1 + Phase 2，2026-09-22；实现记录见 §10）
> 前置：02（密钥调度）已实现并提交（362a8be）；引擎已按版本拆分（977ffc2）。
> 本文档是 02 层的组织结构重构：**语义零变化**（Phase 1）/ 仅 KX API 换形态（Phase 2），
> KAT 与互驱测试是安全网。

## 1. 定位与目标

**目标**：KeyExchange 与 KeySchedule 的结构和流程对齐 BoringSSL ssl 层——
我们调用 BoringSSL 的 crypto 原语，SSL 层逻辑自研。

1. KeySchedule 按 TLS 1.2 / 1.3 拆分为独立文件（对应 `ssl/t1_enc.cc` / `ssl/tls13_enc.cc`），
   与引擎的 `Tls13/Tls12ClientHandshake` 版本子流同缝对称；
2. KeyExchange 从"单类+组分支"改为**抽象基类 + 每组子类 + 工厂**（对应 `ssl/ssl_key_share.cc`
   的 `SSLKeyShare` 层次），API 采用 BoringSSL 的 KEM 统一形态 Generate/Encap/Decap。

**非目标**：

- 不改任何派生语义（KAT 必须零改动全绿）；
- 不实现 PQ / hybrid 组（X25519MLKem768 等）——只留缝（§6）；
- 不引入 `SSL_HANDSHAKE` 式 god-object；不改 staged machine 的 call-once 不变量；
- 不融合"派生+装 cipher"（保留 schedule → secret → traffic_keys → cipher.init 显式链，
  05 的 scatter 链形态需要引擎显式控制）；
- `TlsCryptoPrimitives.{h,cpp}`（唯一 openssl 触点）不动。

## 2. 事实依据（已核实）

### 2.1 BoringSSL 参照（temp/_deps_asan/boringssl-src）

| 对象 | 位置 | 形态 |
|---|---|---|
| `SSLKeyShare` | ssl/internal.h:910 | 抽象基类：`GroupID/Generate/Encap/Decap` 虚函数 + 静态工厂 `Create(group_id)` |
| 子类 | ssl/ssl_key_share.cc:43-379 | `ECKeyShare`/`X25519KeyShare`/`X25519Kyber768`/`X25519MLKEM768`/`MLKEM1024` |
| 1.2 schedule | ssl/t1_enc.cc | `tls1_prf`/`generate_key_block`/`tls1_generate_master_secret`/`tls1_change_cipher_state` |
| 1.3 schedule | ssl/tls13_enc.cc | `tls13_init/advance_key_schedule`/`derive_secret`/`tls13_set_traffic_key`/`tls13_derive_early|handshake|application_secrets` + label 常量 |
| transcript | ssl/ssl_transcript.cc | 我们已有对应物 `TlsTranscript`（handshake/），不动 |

KEM 形态要点：经典 DH 也映射到 Encap/Decap 上（server `Encap` = 己方 keygen + scalar-mult
一步完成；client `Generate` 后 `Decap` = scalar-mult）。X25519 的 Decap 即标量乘。
这是"流程参考 BoringSSL"最核心的一条，也是 PQ 组（真 Encap/Decap）的前置。

### 2.2 我们的现状

- `src/tls/crypto/TlsKeyExchange.cpp`（91 行）：单类；X25519 标量内置，P-256 藏在
  `void *p256_`（EVP_PKEY*）——类层次化后变为子类的类型化成员；
- `src/tls/crypto/TlsKeySchedule.cpp`（533 行）：1.3 staged machine（`TlsKeySchedule13`）
  + 1.3 自由函数 + 1.2 自由函数，混在一个文件；
- 消费方（Phase 2 适配面，file:line 为当前行号）：
  - `TlsClientHandshakeShared.h:88`：`std::optional<TlsKeyExchange> kx`
  - `TlsClientHandshakeShared.cpp:23-26,37`：`reset()/emplace(group)/generate()/public_value()`
  - `Tls13ClientHandshake.cpp:113,196`：`shared_secret(sh.key_share)`、HRR `reset()`
  - `Tls12ClientHandshake.cpp:269-277,371`：SKE 组切换 `reset/emplace/generate`、
    `shared_secret(ske.public_key)`、CKE 编码 `public_value()`
  - `tests/TlsKeyExchangeTest.cpp` / `tests/TlsKeyScheduleTest.cpp`
- 函数清单已与 BoringSSL 对齐（02 §12 实现记录），**差的是组织，不是语义**。

## 3. 目标文件布局（修订 01-directory-design.md:58-60 的树）

```
include/fiber/tls/crypto/
  TlsKeyExchange.h      # 抽象基类 + create() 工厂（子类在 src 侧，公共头零 openssl）
  TlsSecret.h           # TlsSecret + TlsTrafficKeys（共享小头，两版本都引用）
  Tls13KeySchedule.h    # TlsKeySchedule13 + tls13_* 自由函数 + TlsPskBinderKind
  Tls12KeySchedule.h    # tls12_* 自由函数 + Tls12WriteKeys
src/tls/crypto/
  TlsKeyExchange.cpp    # X25519KeyExchange / P256KeyExchange 子类 + 工厂
  Tls13KeySchedule.cpp  # ← TlsKeySchedule.cpp 拆分
  Tls12KeySchedule.cpp  # ← TlsKeySchedule.cpp 拆分
  TlsCryptoPrimitives.{h,cpp}   # 不动
```

`TlsKeySchedule.{h,cpp}` 删除（内部层，不做兼容 shim，直接改消费方 include）。

## 4. Phase 1：KeySchedule 按版本拆分（纯搬家）

### 4.1 符号归属

| 符号 | 去处 |
|---|---|
| `TlsSecret`、`TlsTrafficKeys` | `TlsSecret.h` |
| `TlsPskBinderKind`、`TlsKeySchedule13`、`tls13_traffic_keys`、`tls13_key_update`、`tls13_resumption_psk`、`tls13_finished_mac`、`tls13_psk_binder_mac` | `Tls13KeySchedule.h` |
| `tls12_master_secret`、`tls12_extended_master_secret`、`tls12_key_block`、`tls12_verify_data`、`Tls12WriteKeys` | `Tls12KeySchedule.h` |

细节：

- `TlsSecret` 的 `friend class TlsKeySchedule13`（from_bytes 私有填充）保留——
  `TlsSecret.h` 内前置声明即可；
- `.cpp` 沿 1.3/1.2 的天然缝拆两文件，**逐字搬移**，不改任何逻辑与断言；
- 消费方 include 按符号归属改（`TlsClientHandshakeShared.h`、`Tls13ClientHandshake.h/.cpp`、
  `Tls12ClientHandshake.cpp`、`TlsClientHandshakeEngine.cpp`，共 5 处 + 测试）。

### 4.2 验收门

- `tests/TlsKeyScheduleTest.cpp` **断言零改动**（仅 include 行），全量 ctest 绿；
- KAT 机械管线产物（常量头）不动。

## 5. Phase 2：KeyExchange 类层次（KEM 形态 API）

### 5.1 基类（`include/fiber/tls/crypto/TlsKeyExchange.h`）

```cpp
// (EC)DHE / KEM key exchange for the TLS handshake. 抽象基类：每组一个子类
// （src 侧），工厂 create() 按组构造。API 采用 KEM 统一形态（参考 BoringSSL
// SSLKeyShare 的 Generate/Encap/Decap）：经典 DH 组把 Encap 实现为
// "keygen + 共享秘密"复合、Decap 实现为纯共享秘密；PQ 组（未来）两者语义
// 真正分叉。alert 决策不在此层——TlsKxStatus 由引擎映射。
class TlsKeyExchange : public common::NonCopyable, common::NonMovable {
public:
    // 工厂。组不在 {X25519, Secp256r1} 是引擎 bug（FIBER_ASSERT，同现 ctor 契约）；
    // 分配失败返回 NoMem。返回的实例尚未 generate。
    [[nodiscard]] static common::IoResult<std::unique_ptr<TlsKeyExchange>>
    create(TlsNamedGroup group) noexcept;

    virtual ~TlsKeyExchange(); // wipes private material（同现契约）

    [[nodiscard]] virtual TlsNamedGroup group() const noexcept = 0;

    // 客户端 CH：生成临时密钥对；随后 public_value() 可用。
    // X25519 不能失败；P-256 仅分配可失败（同现契约）。
    [[nodiscard]] virtual common::IoResult<void> generate() noexcept = 0;
    [[nodiscard]] virtual const TlsKeySharePub &public_value() const noexcept = 0; // asserts generated

    // 服务端（07 消费，本期落接口）：从 peer 的 key_share 一步生成己方 share
    // （public_value() 随后可用）+ 共享秘密。要求未 generate。
    [[nodiscard]] virtual TlsKxShared encap(std::span<const std::uint8_t> peer_public) noexcept = 0;

    // 客户端收到 peer share（1.3 SH / 1.2 SKE）：恢复共享秘密。要求已 generate。
    [[nodiscard]] virtual TlsKxShared decap(std::span<const std::uint8_t> peer_public) noexcept = 0;
};
```

保持不变：`TlsKeySharePub`（65B cap + len）、`TlsKxStatus`、`TlsKxShared`。
基类**保名 `TlsKeyExchange`**（消费方少动；"exchange" 涵盖 KEM 语义）。

### 5.2 子类（`src/tls/crypto/TlsKeyExchange.cpp` 内部类型）

| 子类 | 状态成员 | generate / encap / decap |
|---|---|---|
| `X25519KeyExchange` | `uint8_t scalar[32]` + `TlsKeySharePub`（现 x25519_priv_/pub_ 原样） | keypair / keypair+X25519 / X25519（现 shared_secret 的 X25519 分支） |
| `P256KeyExchange` | `EVP_PKEY *`（现 void* p256_ 类型化）+ `TlsKeySharePub` | EVP keygen / keygen+derive / derive（现 P-256 分支） |

- 现单类的组分支逻辑逐字搬入对应子类；`wipe()` 进各子类析构；
- 公共头零 openssl（EVP_PKEY 只出现在 .cpp）。

### 5.3 消费方适配（before → after）

| 位置 | 现状 | 改为 |
|---|---|---|
| `TlsClientHandshakeShared.h:88` | `std::optional<TlsKeyExchange> kx` | `std::unique_ptr<TlsKeyExchange> kx`（可空语义一致） |
| `TlsClientHandshakeShared.cpp:23-26` | `kx.reset(); kx.emplace(g); kx->generate()` | `kx = tls_client_kx_offer(g)`（新共享 helper，见下） |
| `Tls12ClientHandshake.cpp:269-271` | 同上（SKE 组切换） | 同上 |
| `Tls13ClientHandshake.cpp:113` | `kx->shared_secret(sh.key_share)` | `kx->decap(sh.key_share)` |
| `Tls12ClientHandshake.cpp:277` | `kx->shared_secret(ske.public_key)` | `kx->decap(ske.public_key)` |
| `Tls13ClientHandshake.cpp:196` / 12.cpp:269 | `kx.reset()` | 不变（unique_ptr::reset） |
| 12.cpp:371 / Shared.cpp:37 | `public_value()` | 不变 |

新共享 helper（`TlsClientHandshakeShared`）：

```cpp
// create + generate 组合：CH 首飞、HRR 重建、1.2 SKE 组切换三处共用。
[[nodiscard]] common::IoResult<std::unique_ptr<TlsKeyExchange>>
tls_client_kx_offer(TlsNamedGroup group) noexcept;
```

### 5.4 验收门

- `tests/TlsKeyExchangeTest.cpp` 仅 API 适配（工厂 + decap 改名），**测试向量与断言值零改动**；
- 互驱（BoringSSL 9/9）与全量 ctest 绿。

## 6. PQ 留缝（本期不做，记录已知变更点）

`TlsKeySharePub` 的 65B 定容装不下 ML-KEM（pub ~1.2KB、ct ~1.1KB）。Phase 3 引入
hybrid 组时：`public_value()` 改返回 `std::span`（子类自持缓冲）或引入
`TlsKemCiphertext` 独立类型；hybrid 子类组合两个子 exchange（参考
`X25519MLKEM768KeyShare` 的组合形态），shared secret = 两段拼接后进 schedule。
基类三操作接口不需要再动——这正是本期落 KEM 形态的目的。

## 7. 实施顺序

1. Phase 1 拆分 → `cmake --build` + `TlsKeySchedule*` 测试 → **全量 ctest**；
2. Phase 2 类层次 → KX 测试适配 → **全量 ctest**；
3. `./format_code.sh` 一次；
4. 文档收尾：01 目录树（:58-60）修订、02 §5/§7 加"结构已由 02c 重构"指针、
   02c 追加实现记录与偏离清单；memory 更新。

每步全量绿；不主动提交（用户另行指令）。

## 8. 风险

- 02 层是 KAT + 9/9 互驱 + 2318 全绿验证过的成熟层。安全网 = **KAT 断言零改动**；
  若搬家后任何 KAT 变红即语义漂移，立即停；
- `std::optional<TlsKeyExchange>` → `unique_ptr` 多一次堆分配/握手（每次 CH 构建 1 次，
  ~32B 级），可忽略；
- `TlsClientHelloState` 不再内联持有 kx（optional 105B → unique_ptr 8B），
  `kCap=8192` 的 CH 缓冲不受影响。

## 9. 待拍板

1. 基类保名 `TlsKeyExchange`（vs BoringSSL 的 `SSLKeyShare` 命名）——建议保名；
2. `shared_secret` 改名 `decap`（客户端视角语义 + PQ 对齐）——建议改；
3. `encap` 本期落接口但无调用方（07 才消费）——建议落（锁定 Phase 3/07 形态，
   两个 DH 子类实现即 encap = generate+decap 复合，成本一行级）。

三项均按建议定案（用户 2026-09-22 "直接按照建议实现"）。

## 10. 实现记录（2026-09-22）

Phase 1 + Phase 2 全部完成；全量 ctest **2321/2321 绿**（2318 基线 + 3 个新增
encap 测试），BoringSSL 互驱握手全绿，`TlsKeyScheduleTest` KAT 断言零改动。

### 10.1 Phase 1（KeySchedule 按版本拆分，纯搬家）

- `TlsKeySchedule.{h,cpp}`（533 行）→ `TlsSecret.{h,cpp}` + `Tls13KeySchedule.{h,cpp}` +
  `Tls12KeySchedule.{h,cpp}`，逐字搬移，逻辑与断言零变化；
- 消费方 include 重定向 7 处：`TlsClientHandshakeShared.h`、`Tls13ClientHandshake.h/.cpp`、
  `Tls12ClientHandshake.h/.cpp`（显式补 12 头——原经 Shared→旧头的传递依赖断供）、
  `TlsConnectedState.h`（→ `crypto/TlsSecret.h`）、两个测试文件；
- `suite_info_or_assert` 在两个 schedule .cpp 的匿名命名空间各留一份（自包含优先）。

### 10.2 Phase 2（KeyExchange 类层次）

- 基类保名 `TlsKeyExchange`：`create()` 工厂（`new (std::nothrow)`，失败 NoMem；
  组外值 FIBER_ASSERT）+ 纯虚 `group()/generate()/public_value()/encap()/decap()`；
  虚析构 = default（清理在各子类）；
- 子类全在 `TlsKeyExchange.cpp` 匿名命名空间：`X25519KeyExchange`（32B 标量内置 +
  `TlsKeySharePub`）、`P256KeyExchange`（adapter 的 `TlsP256Key` 类型化句柄——
  `tls_p256_free` 幂等置 null，故 encap 失败路径的销毁与析构双安全）；
  原单类组分支逻辑逐字落入子类；
- `encap` 失败契约：实例保持未 generate（X25519 拒绝对端时主动擦除刚生成的标量；
  P-256 释放句柄）——失败后 `public_value()` 仍按断言拦截；
- 消费方按 §5.3 表适配；`TlsClientHelloState::kx` → `unique_ptr`；新共享 helper
  `tls_client_kx_offer()`（create+generate 组合）承接 CH 首飞 / HRR 重建 / 1.2 SKE
  组切换三处。

### 10.3 偏离清单（vs 本设计稿）

1. **`src/tls/crypto/TlsSecret.cpp` 是 §3 布局外新增**：`TlsSecret::wipe()` 调用
   adapter 的 `tls_secure_wipe`，头文件内联会把 openssl 触点泄漏进公共头——
   move/wipe/from_bytes 落 .cpp 是"公共头零 openssl"约束的直接后果；
2. **未保留公开 `wipe()`**（§5.1 草案本就没有）：再生 = 换实例（消费方原本就是
   reset+重建）；两个生命周期测试按实例替换语义重写
   （`FreshInstanceGivesFreshPair` / `P256FreshInstanceFreesTheKeyHandle`），
   death 测试的 wipe 后 `public_value()` 分支换成 encap-after-generate 分支；
3. **新增 3 个 encap 测试**（`X25519EncapAgreesWithDecap` / `P256EncapAgreesWithDecap` /
   `EncapRejectsBadPeerData`）：验收门只要求既有测试 API 适配，但 encap 是新公共
   虚接口，零覆盖不可接受；既有测试向量与断言值零改动；
4. 测试经 `make_kx()` 工厂薄壳（EXPECT + null 兜底）适配，向量（`ramp`/
   `p256_bad_point`/memcmp 对比值）原样。

### 10.4 文档收尾

- 01 目录树（crypto/ 段）已修订为拆分后布局；
- 02 §5/§7 头部已加"结构已由 02c 重构"指针。

