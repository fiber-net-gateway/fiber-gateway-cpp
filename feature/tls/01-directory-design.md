# TLS 自研实现 · 01 代码存放目录设计

日期：2026-09-20
分支：`tls`
状态：设计定稿（实现未开始）

## 1. 目标与范围

自研 TLS 协议栈，支持 **TLS 1.2 与 TLS 1.3**，服务端与客户端两种角色。**crypto（密码学原语）全部调用 BoringSSL**，协议部分（记录层、握手状态机、密钥调度编排、扩展编解码）自己实现。

- 传输：仅 TCP 上的 TLS（不做 DTLS）
- 套件：1.3 = TLS_AES_128_GCM_SHA256 / TLS_AES_256_GCM_SHA384 / TLS_CHACHA20_POLY1305_SHA256；
  1.2 = ECDHE 套件（ECDHE-RSA / ECDHE-ECDSA，X25519 + P-256，GCM + ChaCha20）。1.2 CBC 与静态 RSA KX 不做
- 恢复：1.3 session ticket + PSK resumption + 0-RTT；1.2 session ticket（最小实现）
- 1.2 重协商：收到即拒绝（renegotiation_info 扩展仍要正确编解码，现代实践）
- 密钥更新：1.3 KeyUpdate（对齐 QUIC key-update 已有经验）

**BoringSSL 边界（硬性规则）**：新模块只允许使用密码学原语头
（`openssl/aead.h`、`hkdf.h`、`hmac.h`、`sha.h`、`evp.h`、`rand.h`、`x509.h`、`pem.h`、`err.h` 等），
**禁止使用 `SSL`/`SSL_CTX`/BIO 等 TLS 协议实现对象**。BoringSSL 是 crypto 库，不是 TLS 栈。

## 2. 模块定位：新的顶层模块 `fiber::tls`

新建顶层模块 `src/tls/` + `include/fiber/tls/`，命名空间 `fiber::tls`。不放在 `net` 之下，理由：

1. **net 是消费者不是宿主**：TLS 引擎是纯内存状态机（字节进/字节出），不持有 fd/socket/event loop。fd 驱动的胶水层留在 `net`（同现有 `TlsStreamFd` 的位置）
2. **依赖单向**：`tls` 只依赖 `common`（`IoResult`/`IoBuf`/`Assert`）+ BoringSSL crypto。禁止依赖 `net`/`event`/`async`——这保证 record/handshake 层可以脱离 socket 用内存回环做单测（对齐 QUIC codec 与 endpoint 分离的结构）
3. **未来可被 QUIC 复用**：QUIC 目前用 BoringSSL `SSL` QUIC-method 做握手（`QuicTlsSession`）；自研握手成熟后可替换，`tls` 必须独立于 `net` 才能同时服务两者

依赖方向：

```
common ← tls ← net（fd 胶水层） ← http
            ← quic（远期：替换 QuicTlsSession 的 SSL 握手）
```

## 3. 目录结构

### 3.1 头文件归置规则

- `include/fiber/tls/` —— 对外 API。**零 `openssl/*.h` include**；类型全是 POD/定长数组/`string_view`（BoringSSL 句柄一律 pimpl 或前向声明，存储放 .cpp 侧或 opaque buffer）
- `src/tls/` —— 与 include 同构镜像的实现（每头一 `.cpp`）；仅实现内部使用、且必须触碰 openssl 类型的适配头放这里
- 大模块按关注点分子目录（对齐 `script` 的 parse/ir/run、`common` 的 mem/json 模式）

### 3.2 布局总览

```
include/fiber/tls/
├── TlsTypes.h                 # ContentType、Alert(desc/severity)、IoErr 之外的 TLS 专用错误码、公共常量
├── TlsVersion.h               # 协议版本枚举 + 版本协商结果
├── TlsEngine.h                # 核心：字节进/字节出的同步协议引擎（组合 record + handshake FSM）
├── TlsConfig.h                # 不可变成参：角色、ALPN、SNI、groups/suites 偏好、超时无关的策略项
├── TlsCredentials.h           # 证书/私钥材料（PEM 原文或已解析视图），net 旧栈 TlsCredential 的无 SSL 版
├── TlsTrustAnchors.h          # 信任锚材料，net 旧栈 TrustStore 的无 SSL 版
├── TlsSessionState.h          # 对外会话类型：可恢复参数（1.3 ticket/PSK、1.2 session）
├── crypto/                    # —— 密码学层（纯算法编排；openssl 适配在 src 侧）——
│   ├── TlsKeySchedule.h       # 1.3 key schedule（early/handshake/master/traffic/resumption）+ 1.2 PRF/master secret/key block
│   ├── TlsKeyExchange.h       # X25519/P-256 keypair + shared secret
│   ├── TlsSignature.h         # 签名/验签：rsa_pss_rsae_*、ecdsa_secp256r1、ed25519；1.2 rsa_pkcs1
│   └── TlsCertificate.h       # X509 解析、链构建验证、hostname 验证
├── record/                    # —— 记录层 ——
│   ├── TlsRecord.h            # record 头视图 + 定长常量（2^14 等）
│   ├── TlsRecordReader.h      # 流式分帧：输入字节流 → record 视图（跨 feed 重组、长度校验、空记录规则）
│   ├── TlsRecordWriter.h      # 出向分片与编码（含加密开销预算）
│   └── TlsRecordCipher.h      # 方向级保护状态：1.3 AEAD(静态 IV⊕seq) / 1.2 GCM(explicit nonce)；序列号管理
├── handshake/                 # —— 握手层 ——
│   ├── TlsHandshakeMessage.h  # 消息类型枚举 + 各 body POD（含 1.3 专属：EE/CertificateRequest/NewSessionTicket/KeyUpdate）
│   ├── TlsHandshakeCodec.h    # 上述消息的 encode/decode
│   ├── TlsExtensionCodec.h    # 扩展编解码：supported_versions/key_share/pre_shared_key/psk_key_exchange_modes/
│   │                          #   alpn/server_name/signature_algorithms/supported_groups/renegotiation_info/…
│   ├── TlsCipherSuites.h      # suite/group/sigalg 注册表 + 双版本协商选择逻辑
│   ├── TlsTranscript.h        # 跑动哈希：1.3 按 hash 算法持有（cert_req 分叉）、1.2 handshake_messages 缓冲
│   ├── TlsClientHandshake.h   # 客户端 FSM（1.2/1.3 双分支）
│   ├── TlsServerHandshake.h   # 服务端 FSM（1.2/1.3 双分支）
│   └── TlsPsk.h               # 1.3 PSK/票据/0-RTT 参数推导 + binder 计算；1.2 ticket 封装
└── detail/                    # 内部跨编译单元共享头（保持无 openssl）

src/tls/                       # 与 include 同构；另外：
└── crypto/ 中允许内部适配头（如 TlsCryptoPrimitives.h：EVP_AEAD/HKDF/HMAC/SHA/RAND 的薄封装，
    全模块唯一触碰 openssl 头的文件），对外类型不泄漏任何 openssl 符号

tests/
└── Tls*Test.cpp               # 自动收集进 fiber_tests：
                               #   TlsKeyScheduleTest（RFC 8446/8448 向量）
                               #   TlsRecordReaderWriterTest、TlsHandshakeCodecTest、TlsExtensionCodecTest
                               #   TlsCertificateTest、TlsClientServerLoopbackTest（内存回环，双版本/双角色）
                               #   TlsInteropTest（对旧栈/BoringSSL 客户端服务端互通）

feature/tls/                   # 实现过程文档（本系列，见 §7）
```

### 3.3 引擎 API 形状（示意，非定稿）

```cpp
namespace fiber::tls {

// 同步、无 fd、无 coroutine —— 由 net 层的 fd 胶水驱动。
// feed 对端字节 / emit 我方字节 / 取出应用明文，三段式事件输出。
class TlsEngine {
public:
    enum class Event : std::uint8_t { /* HandshakeDone, AppData, Outbound, Alert, Closed, … */ };
    // feed()/drain_outbound()/take_plaintext() … 详细签名在实现 02/03 号文档时定稿
};

} // namespace fiber::tls
```

## 4. 集成层归属（第二阶段）

fd 驱动的异步胶水（record 读写挂到 `RWFd`、握手 awaiter、close_notify 生命周期）**留在 `src/net/detail/`**，与现有 `TlsStreamFd` 同位：

- 新文件暂名 `src/net/detail/TlsEngineStream.{h,cpp}`（+ 必要的 awaiter），只做“搬运 + 挂等待”，不含协议逻辑
- 目标终态：**`include/fiber/net/` 公共 API（`TlsTcpStream`/`TlsParams` 等）签名不变**，detail 换芯，`http` 层零感知
- 凭据类型的迁移（net 旧 `TlsCredential`/`TrustStore` ↔ 新 `tls::TlsCredentials`/`TlsTrustAnchors`）在集成文档里定：倾向将旧类型降级为新类型的别名/适配器，`http::HttpServerTlsOptions` 冻结层不动

## 5. 与旧栈（BoringSSL SSL 对象驱动）的关系

旧栈文件（保持可用，直到新栈对等后替换）：

| 旧文件 | 终态 |
|---|---|
| `src/net/detail/TlsStreamFd.cpp` | 被新胶水层替换（保留文件名或换名，集成期定） |
| `src/net/detail/TlsSslFactory.{h,cpp}` | 删除（新栈无 SSL 对象） |
| `src/net/detail/TlsRuntime.{h,cpp}` | 删除或缩为 ERR 队列工具 |
| `src/net/TlsTcpStream.cpp`、`include/fiber/net/TlsTcpStream.h` | 公共 API 保留，实现换芯 |
| `src/net/TlsCredential.cpp`、`TrustStore.cpp`、`TlsServerHandshakeConfig.cpp` | 迁移为 `fiber::tls` 类型的适配层或删除 |

替换判据（对等 = 全部满足才删旧栈）：双版本双角色互通（对 BoringSSL `bssl s_client/s_server` 与系统 openssl）、lite_nginx HTTPS 全量测试绿、1.3 resumption/0-RTT 互通、ASan 全量绿。旧栈在切换前**一行不改**，作为开发期的互通对照实现。

## 6. 构建与命名

- **CMake 零改动**：`GLOB_RECURSE src/*.cpp` 自动纳入；`fiber_lib` 已 `PUBLIC boringssl::ssl boringssl::crypto`；测试自动收集
- 命名空间 `fiber::tls`（子层不加子命名空间，与 quic/script 一致）
- 类型前缀 `Tls`；头文件守卫 `FIBER_TLS_<子目录大写>_<名称>_H`（顶层 `FIBER_TLS_<名称>_H`）
- include 路径：库内相对（`#include "../common/IoError.h"` 风格按现有惯例），对外 `<fiber/tls/...>`
- 错误处理：全 `IoResult<T>`，无异常；openssl 调用失败统一翻译成 `tls` 错误码（`TlsTypes.h`），并 drain ERR 队列
- 性能路径沿用全库规则：热路径禁 `std::string`/`std::vector` 持有，用 `IoBuf`/定长数组/`string_view`；record 加解密零拷贝视图进出

## 7. 本文档系列规划（feature/tls/）

| 编号 | 主题 | 对应目录 |
|---|---|---|
| 01 | 目录设计（本文） | — |
| 02 | crypto 适配层 + 密钥调度 | `crypto/` |
| 03 | 记录层 | `record/` |
| 04 | 消息/扩展编解码 + suite 注册表 | `handshake/`(codec 部分) |
| 05 | 客户端握手 FSM | `handshake/TlsClientHandshake` |
| 06 | 服务端握手 FSM | `handshake/TlsServerHandshake` |
| 07 | 恢复：ticket/PSK/0-RTT | `handshake/TlsPsk`、`TlsSessionState` |
| 08 | net 集成与旧栈替换 | `src/net/detail` 胶水层 |
| 09 | 互通与回归测试报告 | `tests/Tls*` |

实现顺序即编号顺序（02/03/04 可交错，codec 类先行——它们是 FSM 的输入）。
