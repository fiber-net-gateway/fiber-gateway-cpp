# TLS 自研实现 · 05 记录保护层：TlsRecordCipher（原始能力）

日期：2026-09-20
分支：`tls`
状态：**已实现（2026-09-20；38 项 cipher 单测 + span scatter 原语；链形态四函数适配层
见 §13，另 13 项链单测。实现期修正记录见 §12/§13.6）**

修订记录：修订 1 为"cipher 持池、内部完成 gather/建链"；修订 2 按评审意见改为
**纯 span 变换**——cipher 内部不做任何内存分配，目标缓冲由外部传入，并提供
尺寸计算函数。缓冲编排（gather、视图收缩、建链）上移引擎层；修订 3（§13）在
record/ 内补一层**链适配**（IoBufChain 输入的四个形态 + span scatter 原语），
链编排不再要求引擎逐节点手写。

## 1. 定位与边界

TlsRecordCipher 是记录保护的**原语层**：输入成品密钥材料与字节区间，输出
受保护/还原的字节区间。只做三件事——AEAD seal、AEAD open、自持序列号。
**不持有 IoBufNodePool，不碰 IoBufChain，内部零分配**（唯一内部状态构造
`EVP_AEAD_CTX_init` 对本层三种 AEAD 均为结构内定长密钥编排，无堆分配）。

**不关注 epoch**（核心约束）。TLS 里所有"换钥时刻"——1.2 的 ChangeCipherSpec
半双工切换、1.3 的 early-data/handshake/application 三套流量密钥、KeyUpdate
双向换钥——在原语层统一塌缩为一个动作：**引擎换一个 cipher 实例**。
每个实例构造时固化 (算法, key, iv)，自带从 0 起算的 seq；换钥 = 构造新实例、
替换指针、销毁旧实例。好处：

- cipher 无状态机、无阶段判断、无"当前密钥"指针管理——不可变对象 + 一个计数器；
- 密钥材料的生命周期与 02 号密钥调度解耦：`init` 后 key 只存在于
  `EVP_AEAD_CTX` 内部，调用方可立即丢弃源缓冲；
- 1.3 key phase 检测（KeyUpdate 触发的接收侧换钥）天然正确：新实例 seq 从 0
  起，与对端新密钥的 seq 对齐。

其余不做的事（归别处）：

| 不做 | 归属 |
|------|------|
| 密钥调度（HKDF-Expand-Label、traffic secret 推导） | 02 号 crypto 适配层 |
| 内存编排（gather、原地视图收缩/扩展、建链） | 引擎层 TlsEngine（§5.3） |
| 明文/密文记录路由（CCS 兼容记录、握手初期明文） | 引擎层 TlsEngine |
| 换钥时机、KeyUpdate 消息本身、seq 上限策略 | 引擎层（cipher 暴露 `sequence()`） |
| 记录分帧/头部编解码 | TlsRecordReader/Writer（已完成） |
| alert 的选择与发送 | 引擎层（cipher 只报 Status） |
| 1.3 记录 padding、CBC 套件、EtM | 非目标（01 号范围为 AEAD-only） |

## 2. 事实依据（代码现状）

- **9 个 suite 收敛为 3 个 AEAD**（`TlsCipherSuites.h` 全表）：1.3 三个
  (0x1301/0x1302/0x1303) + 1.2 六个 ECDHE 套件，密码学原语全部落在
  `EVP_aead_aes_128_gcm` / `EVP_aead_aes_256_gcm` / `EVP_aead_chacha20_poly1305`。
  先例：`src/quic/QuicCrypto.cpp` 已用同一族 EVP_AEAD API（init/seal/open，
  **调用方缓冲**的形态）。
- **EVP_AEAD 的缓冲契约**：`out == in` 允许（原地）；`out != in` 要求两区域
  **不重叠**（库内完成搬运）。`EVP_AEAD_CTX_seal/open` 均为单输入区/单输出区。
- **03 号文档既定约束**：`contiguous_payload()` 单 span 原地快路径、每条记录
  至多一次拷贝——由引擎按 §5.3 编排兑现，cipher 只提供使能该编排的纯变换。

## 3. wire 格式与长度策略

### 3.1 TLS 1.3（RFC 8446 §5.2）

```
外层记录:  type=23(app_data) | 0x0303 | len
payload:   AEAD(plaintext || inner_type || zero_padding...) || tag(16)
nonce:     static_iv(12) 中低 64 位 XOR BE64(seq)
AAD:       tls_encode_record_header(23, 0x0303, len)   // len 含 tag
```

- seal 侧 AAD 由 cipher 内部生成（out_len 先验可知 = plain + 1 + 16）。
- open 侧 AAD 用**收到的头部原样**（`legacy_version` 照抄，兼容对端发
  0x0301 等历史值）——直接复用 `tls_encode_record_header`。
- 解密后自尾部扫描零填充，最后一个非零字节即 inner_type；合法值 ∈
  {20,21,22,23}，否则 Malformed（引擎映射 unexpected_message）。padding
  扫描上界 = 记录长，有界。
- 长度检查（open，在解密**之前**，DoS 守卫）：
  `17 ≤ len ≤ 2^14+256`。下限 = 1(type)+16(tag)；上限是 1.3 精确帽
  （`TlsRecord.h` 注释承诺归本层），超限 Malformed（引擎映射
  record_overflow）。明文 = len−17 ≤ 2^14 自动满足。
- 空明文合法：len=17，还原出 0 字节 application data。

### 3.2 TLS 1.2 AEAD（RFC 5288 GCM / RFC 7905 ChaCha，同构）

```
外层记录:  type=真实类型(21/22/23) | 0x0303 | len
payload:   explicit_nonce(8)=BE64(seq) || AEAD(plaintext) || tag(16)
AEAD nonce: fixed_iv(4) || explicit_nonce(8)
AAD:       BE64(seq) || type || version || BE16(plain_len)
```

- ChaCha 1.2 套件与 GCM 完全同构（RFC 7905 §2 明文复用 5288 构造），
  同一代码路径。
- open 的 AAD 用**本实例 seq** + 收到的 `type/legacy_version`；
  plain_len = len − 24。explicit nonce 取线上字节构造 nonce（宽松策略，
  与 BoringSSL 一致，不强制 == seq）。
- 长度检查（open 之前）：`24 ≤ len ≤ 2^14+2048`（上限与 Reader 通用帽一致）。

| | wire 最小 | wire 最大 | 开销 |
|---|---|---|---|
| 1.3 | 17 | 2^14+256 | +17 / −17 |
| 1.2 | 24 | 2^14+2048 | +24 / −24 |
| seal 明文上界 | — | 2^14（Writer 分片已保证） | — |

## 4. 对象模型

单一具体类 `TlsRecordCipher`，**每方向一个实例**（连接持 read/open 实例与
write/seal 实例各一）：

- 状态：`EVP_AEAD_CTX`（按值，结构内定长，无堆分配）+ `kind`(Tls13/Tls12)
  + `suite` + `fixed_iv/static_iv`（≤12B）+ `seq`(u64)。
  除 `seq` 外全部不可变——密钥、IV、算法在 init 时固化。
- `seal()` 与 `open()` 同在一类（`EVP_AEAD_CTX` 天然双向）；引擎按方向使用
  自己持有的实例。不做 Sealer/Opener 拆分（多两个类型无收益）。
- 无虚函数：`kind` 二值 switch 分派 1.2/1.3 构造差异（AAD/nonce/前后缀），
  AEAD 差异已被 `EVP_AEAD_CTX` 吸收。
- 构造失败面：`EVP_AEAD_CTX_init` 可能失败（key/iv 长度不符）→ 采用
  Writer/Reader 同款模式：默认构造 + `IoResult<void> init(...)`，
  `seal/open` 入口 `FIBER_ASSERT(initialized)`（空值检查推到边界）。
- **纯内存、零分配**：无 fd、无协程、无锁、无池。span 进 span 出，
  天然可重入测试（RFC 8448 向量直接字节级断言）。

## 5. 缓冲契约（本次修订的核心）

### 5.1 尺寸计算（纯函数，调用方据此备内存）

```
seal_output_size(plain_len)      // 调用方须给 dst 的容量：plain+17 (1.3) / plain+24 (1.2)
open_output_size(cipher_len)     // 调用方须给 dst 的容量：cipher_len−16 (1.3) / cipher_len−24 (1.2)
                                 //   （§12 修正：1.3 的 EVP 输出工作区含 inner_type，
                                 //   若按 −17 备 dst，合法记录会被 EVP 拒掉；
                                 //   实际明文以 OpenResult::plain_len 为准——本实现
                                 //   seal 从不加 padding，恒 = open_output_size −1）
min_ciphertext_size()            // 17 / 24（open 前置下限）
max_ciphertext_size()            // 2^14+256 / 2^14+2048（open 前置上限）
```

`dst.size() < 所需容量` 是编程错误 → `FIBER_ASSERT`（同 Writer 的
"未绑定即断言"分层：契约违例断言、运行时失败只留协议性 Status）。

### 5.2 别名（原地）规则

EVP 要求 `out == in` 或两区域不重叠。cipher 据此定义两种合法形态，
**其余部分重叠一律 FIBER_ASSERT**（debug 捕获调用方错误）：

| 操作 | 原地形态 | 说明 |
|------|----------|------|
| seal 1.3 | `dst.data() == plaintext.data()` | cipher 在明文末尾写 inner_type 字节（该字节可能落在 plaintext span 之外但必在 dst 容量内——调用方即节点 tailroom，须可写）；tag 落其后 |
| seal 1.2 | `dst.data() + 8 == plaintext.data()` | headroom ≥ 8：nonce 写 dst 头 8 字节，AEAD 原地区从 plaintext 起 |
| open 1.3 | `dst.data() == ciphertext.data()` | AEAD 原地，产出含 padding 工作区后剥 |
| open 1.2 | `dst.data() == ciphertext.data() + 8` | **明文总落在 dst.data()**（§12 修正：ciphertext 须传含 8 字节 explicit nonce 的完整 payload，EVP in/out 同为 `ciphertext+8` 起的子区） |

非原地（dst 与 src 完全不相交）时的搬运次数：

| 操作 | 原地 | 独立 dst |
|------|------|----------|
| seal 1.3 | **0** | 1 次 memcpy（EVP 单输入区，type 字节须先与明文拼接，拼完原地 EVP） |
| seal 1.2 | **0** | 0 次（EVP `out≠in` 原生搬运；nonce 前置直写） |
| open 1.3 | **0** | 0 次（EVP 原生） |
| open 1.2 | **0** | 0 次（EVP 原生） |

即：**唯一需要拷贝的形态是"独立 dst 的 1.3 seal"**，且该拷贝在 cipher 内
完成（memcpy 到 dst 后原地 EVP）——调用方选原地即免。

### 5.3 链编排（引擎侧备忘，不属 cipher；**已被 §13 链适配层取代，仅存档**）

> 修订 3 起引擎不再手写本节编排：`record/TlsRecordCipherChain`（§13）的四个
> IoBufChain 形态已覆盖。本节的 unique()/tailroom 要求源于"tag 写进节点
> tailroom"的旧路线——seal_scatter 把 tag/type/nonce 移到外部缓冲后，原地
> seal/open 均**无 unique() 要求**（测试 InPlaceSharedStorageNeedsNoUnique）。

引擎持池，按下表把 chain 适配为 span（这正是 03 号"每记录至多一次拷贝"
的兑现点；快路径全零拷贝）：

- **open**：`contiguous_payload() != nullptr` → 原地 open（dst=src=该 span）
  → `trim_end(length − plain_len)` 收缩视图；跨节点 → gather 进引擎自备
  缓冲（这一次拷贝不可避免）→ open → 建链。
- **seal 1.3**：尾节点 `unique()` 且 tailroom ≥ 17 → 原地（dst=明文 span，
  容量由 tailroom 保证）→ `commit_tailroom(17)` 扩展 readable → 交
  Writer；否则新节点（plain+17）独立 dst → cipher 内一次 memcpy。
- **seal 1.2**：首节点 headroom ≥ 8 且明文连续 → 原地（dst=src−8）→ 前部
  视图扩展 + `commit_tailroom`（**前部扩展 API 未随本次交付**，引擎实施时补；
  也可每次新节点独立 dst——EVP 原生搬运 0 额外拷贝，仅多一次节点分配）。
- 产出交 `Writer::write(outer_type, protected_chain, out)`：1.3 传
  ApplicationData（类型改写由调用点表达），1.2 传真实类型；长度域自动含
  tag。**Writer 零改动**。一致性要求：加密开始后 Writer 的
  `legacy_version_` 必须为 0x0303（默认即是；客户端首飞设过 0x0301 的须复位）。

### 5.4 mem 层小补充（3 个，归属引擎侧，对称于既有 commit/consume 家族）

1. `IoBuf::uncommit(n)` — `last_ -= n`（尾部收缩，assert n ≤ readable）；
2. `IoBufChain::trim_end(n)` — 尾节点 uncommit + `readable_bytes_ -= n`，
   部分裁剪归还的 tailroom 加回 `writable_bytes_`；整节点吞没时释放节点
   并扣其 writable 账目（原地 open 后剥 tag/type/padding）；
3. `IoBufChain::commit_tailroom(n)` — 尾节点物理 tailroom 提交 n 字节为
   readable（原地 seal 后把 tag 纳入 readable）。

**账目修正（§12.3）**：设计时认为"传输读入的已提交节点其 tailroom 不在
`writable_bytes_` 账目内"——实现时核实不成立：`append_node` 起链上
`∑ node.writable() == writable_bytes_` 全程保持（take_prefix 等均转移账目）。
因此 `commit_tailroom` 的账目行为与 `commit_back` 一致（扣 `writable_bytes_`），
`trim_end` 部分裁剪须加回账目；两者语义差异只在命名表达调用方意图
（字节由加密层写入 tailroom，而非传输读入 writable 区）。

备选（不动 mem 层）：`retain_slice`/`unsafe_retain_slice` 重铸视图再建链，
每记录 1 次池分配——热路径不可取。

## 6. API 草案（签名级，无实现）

```
enum class TlsRecordProtectionKind : std::uint8_t { Tls13, Tls12 };

class TlsRecordCipher : public common::NonCopyable, public common::NonMovable {
public:
    TlsRecordCipher() noexcept = default;

    // key/iv 拷入 EVP_AEAD_CTX 后不再持有调用方内存；长度与 suite 不符
    // → IoErr::Invalid。无堆分配。
    [[nodiscard]] common::IoResult<void> init(TlsCipherSuiteId suite,
                                              TlsRecordProtectionKind kind,
                                              std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> iv) noexcept;

    // ---- 尺寸计算（§5.1）----
    [[nodiscard]] std::size_t seal_output_size(std::size_t plaintext_len) const noexcept;
    [[nodiscard]] std::size_t open_output_size(std::size_t ciphertext_len) const noexcept;
    [[nodiscard]] std::size_t min_ciphertext_size() const noexcept;
    [[nodiscard]] std::size_t max_ciphertext_size() const noexcept;

    enum class Status : std::uint8_t { Ok, AuthFail, Malformed };

    // 明文进、受保护字节出。成功时 out_len == seal_output_size(plaintext.size())。
    // 成功后 seq++。1.3 的 inner_type 加密进 payload；1.2 原样用于 AAD。
    struct SealResult { Status status; std::size_t out_len; };
    [[nodiscard]] SealResult seal(TlsContentType inner_type,
                                  std::span<const std::uint8_t> plaintext,
                                  std::span<std::uint8_t> dst) noexcept;

    // 密文进（含 tag；1.2 含 8 字节 explicit nonce 前缀）、明文出。
    // outer_type/legacy_version/length 取自收到的记录头（1.3 AAD 原样、
    // 1.2 AAD 用本实例 seq + 收到字段）。plain_len 为实际明文长（1.3 剥
    // padding 后）。成功后 seq++。
    struct OpenResult { Status status; TlsContentType inner_type; std::size_t plain_len; };
    [[nodiscard]] OpenResult open(TlsContentType outer_type, std::uint16_t legacy_version,
                                  std::uint16_t length,
                                  std::span<const std::uint8_t> ciphertext,
                                  std::span<std::uint8_t> dst) noexcept;

    [[nodiscard]] std::uint64_t sequence() const noexcept;  // 引擎换钥策略用
    // suite()/kind() 查询、initialized() 断言辅助
};
```

返回 `{Status, ...}` 结构体而非 `IoResult<T>`：与 `TlsRecordReader::Result`
同款；Status 需区分两类协议性失败（见 §7）。**无 NoMem**——零分配的直接
收益：运行时失败只剩协议性的 AuthFail/Malformed。

## 7. 错误模型

| Status | 含义 | 引擎映射（alert） |
|--------|------|-------------------|
| Ok | 成功 | — |
| AuthFail | AEAD 认证失败 | bad_record_mac（必须致命） |
| Malformed | 长度越界、1.3 outer≠23、inner_type 非法、padding/type 缺失 | record_overflow / decode_error / unexpected_message |

dst 容量不足、部分重叠、未 init = 契约违例 → `FIBER_ASSERT`（§5.1）。

## 8. 安全要点

- 常量时间 tag 比较、解密实现在 BoringSSL（EVP_AEAD）；本层不产生任何
  依赖失败细节的分支——AuthFail 不区分 tag 位/padding 位置。
- nonce 不复用由结构保证：每实例 seq 单调 + 密钥唯一，无外部注入 seq 的口子。
- seq 回绕：RFC 8446 §5.2 要求不得回绕。cipher 仅暴露 `sequence()`，
  主动 KeyUpdate（如 ≥2^24 记录）策略归引擎；构造性回绕在 u64 下不可达。
- 密钥不可读出：init 后无任何取回 key/iv 的接口。
- open 前置长度检查在解密之前执行（不做解密 oracle）。

## 9. KeyUpdate / 换钥边界（引擎侧备忘）

- 收到 KeyUpdate(requested)：引擎完成在途写出 → 02 号推新 write key →
  新实例替换 seal 侧 → 需要时回 KeyUpdate。
- 发起 KeyUpdate：新 write key 替换 → 发 KeyUpdate → **收到对端 KeyUpdate
  后**才替换 read 侧（对端用旧 key 发完在途记录）。
- 1.2 CCS：两侧各自收到 CCS 后由 Finished 消费时点替换对应方向实例
  （半双工不一致窗口是协议本性，实例模型天然表达）。
- 以上全部不涉及 cipher 内部状态迁移——只有实例指针替换。

## 10. 测试计划（tests/TlsRecordCipherTest.cpp，已实现，34 绿）

1. **KAT（1.3/1.2 各 3 suite 参数化）**：实现降级为**独立构造交叉验证**——
   实施环境无网络，RFC 8448 向量抓取不到（curl/WebFetch 均不可达）；测试内
   用裸 `EVP_AEAD_CTX` 按 RFC 8446 §5.2 / RFC 5288 §3 **手工拼 nonce/AAD/
   payload 布局**生成期望密文（与被测代码零共享路径）逐字节比对，AAD/nonce/
   type 布局逻辑仍被钉死。TODO：联网后补 RFC 8448 §3 原文向量
   （server_hs key `3fce5160…` 首条 657B 明文→674B 密文等，长度关系已核对）。
2. **尺寸函数**：两 kind 的边界表；`open_output_size` 与 `plain_len`
   关系（本实现无 padding，1.3 恒 = capacity−1；1.2 恒等）。
3. **roundtrip 属性**：3 AEAD × {空明文, 1B, 2^14} × 连续 seal 递增
   （1.2 断言 explicit nonce = BE64(seq)；两次 seal 密文必异；
   AuthFail 不推进 seq）+ 1.3 明文尾零存活语义（§12.2）。
4. **别名形态**：原地 seal 1.3（type 字节恰落 tailroom）/seal 1.2（nonce
   落 headroom）/open 守卫字节不变；独立 dst 与原地产出逐字节相同；
   独立 dst 不触碰源。
5. **失败路径**：bit 翻转（首/中/尾）→ AuthFail；len<17/<24 与 1.3
   2^14+257 → Malformed（先于任何缓冲断言检查，§12 修正）；1.3 outer≠23 →
   Malformed；inner_type 非法（手工构造 0x01）与全零 inner（无 type 字节）
   → Malformed；AAD 敏感性（篡改 legacy_version/length → AuthFail）；
   1.2 同记录重放 → AuthFail。
6. **Reader→(cipher)→Writer 端到端**：1.3 走原地快路径（尾节点 tailroom
   seal + `commit_tailroom` + 原地 open + `trim_end`）；1.2 走独立节点
   （EVP 原生搬运）+ consume(8) + trim_end——mem 层 3 个新 API 的集成验证。

## 11. 实施清单（已完成 2026-09-20）

1. ✅ mem 层：`IoBuf::uncommit` + `IoBufChain::trim_end` +
   `IoBufChain::commit_tailroom`（IoBufTest/IoBufChainTest 各增单测）；
2. ✅ `include/fiber/tls/record/TlsRecordCipher.h` +
   `src/tls/record/TlsRecordCipher.cpp`（无池、无链、零分配）；
3. ✅ `tests/TlsRecordCipherTest.cpp`（§10，34 绿，全量 ctest 2184 绿）；
4. ✅ 文档收尾：本号状态改"已实现"，03 号待做勾掉 RecordCipher 条目。

## 12. 实现记录（相对设计的修正，均已落入代码与测试）

1. **open_output_size 1.3 = cipher_len−16（容量需求语义）**：EVP open 的
   max_out 工作区含 inner_type（及对端可能发送的 padding）；按 −17 备 dst
   会把合法记录错判为缓冲不足。§5.1/头文件注释已改。
2. **1.3 明文尾零不会被剥**：设计评审时误记"明文尾零与 padding 不可分、
   会被剥"——实际 inner_type 位于明文**之后**（TLSInnerPlaintext =
   content || type || zeros），type 字节守护明文尾零；只有发送端主动加的
   全零 padding 才被剥（本实现 seal 从不加 padding）。测试以
   `Tls13PlaintextTrailingZerosSurvive` 钉死该语义。
3. **mem 账目不变量**（§5.4 修正）：`∑ node.writable() == writable_bytes_`
   全程保持，`commit_tailroom` 与 `commit_back` 账目行为一致（扣），
   `trim_end` 部分裁剪须加回；设计时"传输节点 tailroom 不在账目"的论断
   不成立。
4. **1.2 open 的 EVP 输入区含 tag**：`in = ciphertext+8, in_len = length−8`
   （密文体+tag）；AAD 里的 plain_len 才是 `length−24`。写成 `length−24`
   时 GCM 把密文尾部当 tag——短记录（plain<16）直接 AuthFail、长记录
   认证失败，由 5 字节 probe 程序对 BoringSSL 语义定谳后修正。
5. **1.2 open 的 dst 起点即明文落点**：原地形态为
   `dst.data() == ciphertext.data()+8`（ciphertext 传含 nonce 前缀的完整
   payload），引擎在链上 consume(8) 后取 span 即满足——设计表述已更正。
6. **协议性检查先于契约断言**：open 的 min/max 长度检查置于所有
   `FIBER_ASSERT` 之前（越界 length 是运行时数据而非调用方 bug，且不应
   因断言缓冲而 abort）。

## 13. 链形态：span scatter 原语 + record/ 链适配层（2026-09-20 修订 3）

用户需求：cipher 接受 **IoBufChain**（一条完整记录，可跨节点）输入，支持四形态——
① 转录解码（chain→dst 连续）、② 原地解码（chain readable 原地改、不要求 unique）、
③ 转录编码（chain→dst）、④ 原地编码（chain 明文原地变密文 + 外部 dst_header/dst_tailer）。
实施为两层：cipher 新增 **span scatter 原语**，record/ 新增**链适配层**做拓扑路由。

### 13.1 span 原语：seal_scatter / open_scatter（probe 定谳，/tmp/evp_scatter_probe.cpp）

- `EVP_AEAD_CTX_seal_scatter(ctx, out, out_tag, &out_tag_len, max_out_tag_len, nonce, 12,
  in, in_len, extra_in, extra_in_len, ad, ad_len)`：tag 写**独立 out_tag**（不得与任何
  参数别名）；`extra_in` 的密文写在 out_tag 头部——1.3 的 inner_type 字节经此与明文
  **物理分离**（out_tag_len = 17 = ct(type)||tag），这是形态 ④ 前后缀外置的支点。
  `out==in` 原地允许。三 AEAD（aes128/256-gcm、chacha）与普通 seal 逐字节一致。
- `EVP_AEAD_CTX_open_gather(ctx, out, nonce, 12, in, in_len, in_tag, 16, ad, ad_len)`：
  tag 读**独立 in_tag**（可在别的节点——形态 ② "tag 落下一节点"仍零拷贝的支点）；
  无 out_len 参数（恰好写 in_len 字节）；out==in 允许。
- **本 BoringSSL evp.h 无任何 EVP_CIPHER 系 AEAD** → 无流式分段；AEAD 主体区必须
  连续，跨节点必有一次 gather。且 tag 认证整条消息（GHASH 多项式），**分段 seal =
  nonce 复用 = GCM 灾难**——"密文/明文等长故可逐节点加密"仅对加密部成立，对
  认证部不成立，与长度无关。
- **失败时 BoringSSL 清零输出区**（in-place 即抹链上记录字节；连接必死无碍，
  已写入头文件契约）。

### 13.2 API（`record/TlsRecordCipherChain.h`，六函数）

```
tls_record_open_dst_size(cipher, length)              // dst 容量：1.2 = length−8，1.3 = length
tls_record_open_transcribe(cipher, outer, ver, length, const chain, dst) -> OpenResult
tls_record_open_in_place (cipher, outer, ver, length, chain&, dst)       -> {OpenResult, in_chain}
tls_record_seal_transcribe(cipher, type, const chain, dst)               -> SealResult
tls_record_seal_in_place  (cipher, type, chain&, dst_header, dst_tailer, dst) -> {SealResult, in_chain}
```

- **入口断言**：open 侧 `FIBER_ASSERT(payload.readable_bytes() == length)`（调用方
  契约：一次提交一条完整记录，可跨节点）；seal 侧 `plain <= kTlsMaxPlaintextSize`
  （Writer 分片契约）。length 越界仍是 cipher 的协议性 Malformed（先于断言，§12.6）。
- **拓扑路由**：`chain_contiguous(off, len)` 判主体（1.2 body / 1.3 body=含 ct(type)
  的 length−16 区）是否单节点连续——是则 scatter 直通（0 拷贝），否则 gather 后
  原地（每条记录至多 1 拷贝）。返回结构 `in_chain` 标明落点（失败时无意义）。
- **原地解码后视图手术**：1.2 `consume(8)+trim_end(16)`；1.3 `trim_end(length−plain_len)`
  （剥 ct(type)+padding）。原地编码链 readable 长度不变（ct 等长覆盖明文，
  tag/nonce 落 dst_tailer/dst_header）。
- **unique 非对称消解**：open 原地只写记录自身 readable 字节；seal_scatter 原地不碰
  headroom/tailroom——两方向均无需 unique（`InPlaceSharedStorageNeedsNoUnique` 以
  retain_slice 共享存储 sibling 钉死）。
- **1.2 nonce 恒 8 字节拷栈**（免连续性要求，cheap）；零长明文时 chain_contiguous
  返回 null 指针 → 换非空哑指针（never dereferenced）。
- mem 层新增 `IoBufChain::front_node()` 只读访问器（front() 返回 IoBuf* 不足以游走
  next 指针）。

### 13.3 拷贝次数矩阵

| 形态 | 主体单节点连续 | 跨节点 |
|---|---|---|
| ① 转录解码 | 0（EVP open_gather 直写 dst） | 1（gather 后原地） |
| ② 原地解码 | 0（tag 在下一节点亦可） | 退化为 ①（in_chain=false） |
| ③ 转录编码 | 0（EVP seal_scatter 即转录） | 1（gather 即转录） |
| ④ 原地编码 | 0（ct 覆盖明文，前后缀外置） | 退化为 ③（in_chain=false） |

### 13.4 测试（cipher +4 = 38 绿；链层 13 绿；mem +1；全量 ctest 2202 绿）

- cipher：`SealScatterMatchesSeal`（双布局重组比对 wire）、`OpenScatterRestoresPlaintext`
  （disjoint + staged 原地）、`EmptyPlaintextCrossFormRoundTrip`（跨形态互逆）、
  `AuthFailAndMalformed`（翻转→AuthFail 且 seq 不增；越界先于断言）。
- 链：dst 尺寸；转录不动链（跨 4 种 split 拓扑）；原地视图收缩；tag 在下一节点
  仍 in_chain；跨节点退化（链不动/前后缀保持零）；共享存储免 unique；空明文两形态；
  端到端 `InPlaceSealFeedsInPlaceOpen`（④ 产出 → 手工拼 framed → Reader → ② 消费）。
- mem：`FrontNodeWalksReadableSpansInOrder`。

### 13.5 引擎侧收益

引擎不再手写链编排：读路径 Reader 取完整记录链 → ②（失败退 ①）；写路径 Writer
分片后 → ④（前缀 5 字节头 + 1.2 的 8 字节 nonce；尾 17/16 字节）或 ③。KeyUpdate
换 cipher 实例即换适配层入参，无新增状态。

### 13.6 实现记录（本期修正）

1. **seal_scatter 的 EVP nonce_len 恒 12**（1.3 static iv 12；1.2 fixed 4||explicit 8），
   误传 iv_len（1.2=4）则 1.2 全 suite AuthFail——与既有 seal() 两分支硬编码 12 同源。
2. **regions_overlap 零长修正**：注释承诺"零长区域不与任何区域重叠"但公式只对
   零长**第一**参成立；零长第二参落于第一参区间内仍报重叠（空明文 1.2 原地路径
   误触发别名断言）。改为 `a_len != 0 && b_len != 0 && ...`。
3. **1.3 scatter 的 body region = length−16 含 ct(type) 字节**（scatter 视角 tag 才
   16 字节）——分界/容量计算按 length−16 而非明文长（§12.1 同源教训）。
4. dst 容量分层：cipher open_scatter 断言用 `open_output_size(length)`（1.3 = −16
   含 type 工作区）；链层 open 侧 dst 需 `tls_record_open_dst_size`（1.3 = length，
   gather 需容整条记录；1.2 = length−8，nonce 走栈）。
