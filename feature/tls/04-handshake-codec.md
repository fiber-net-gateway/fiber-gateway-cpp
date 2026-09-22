# TLS 自研实现 · 04 握手层（一）：ClientHello 解码

日期：2026-09-20
分支：`tls`
状态：**ClientHello 解码已完成**（ServerHello 及其余消息编解码已随 06 交付，见 06 §5.5）

## 1. 交付物

| 文件 | 内容 |
|---|---|
| `include/fiber/tls/detail/TlsCursor.h` | `TlsReadCursor`：单连续缓冲上的边界检查大端读取（u8/be16/be24/be32/slice/skip），越界返回 `IoErr::Invalid`。全 inline，镜像 `fiber::quic` 的 QuicReadCursor 惯例 |
| `include/fiber/tls/TlsVersion.h` + `src/tls/TlsVersion.cpp` | `TlsProtocolVersion` 枚举、`tls_is_known_version`、`tls_version_list_contains`（supported_versions 列表成员测试） |
| `include/fiber/tls/handshake/TlsCipherSuites.h` | `TlsCipherSuiteId`（1.3 AEAD + 1.2 ECDHE 套件）、`TlsNamedGroup`、`TlsSignatureScheme`，header-only |
| `include/fiber/tls/handshake/TlsExtensionCodec.h` + cpp | `TlsExtensionType` 枚举、扩展块游标 `TlsExtensionCursor`、ALPN 游标 `TlsAlpnCursor`、`tls_find_client_key_share`、`tls_psk_modes_contains` |
| `include/fiber/tls/handshake/TlsHandshakeMessage.h` | `TlsHandshakeType`、`TlsHandshakeHeader` + `tls_decode_handshake_header`、解码结果 POD `TlsClientHello` |

`TlsHandshakeType` 覆盖 TLS 1.2（RFC 5246 §7.4）+ 1.3（RFC 8446 §4）全集：
0 HelloRequest（1.2 重协商触发，回答 NO_RENEGOTIATION）、3 HelloVerifyRequest
（仅 DTLS，登记不收发）、6 HelloRetryRequest（1.3，转录以 MessageHash 254 替换）、
12 ServerKeyExchange / 14 ServerHelloDone / 16 ClientKeyExchange（1.2 ECDHE 全程）、
25 CompressedCertificate（RFC 8879）、67 NextProtocol（NPN，已废弃，仅登记）。
未分配的线上值（7/9/10/17..19…）经 static_cast 照常落入枚举，dispatch 不命中
已知类型即按 unexpected_message 处理。
| `include/fiber/tls/handshake/TlsHandshakeCodec.h` + `src/tls/handshake/TlsHandshakeCodec.cpp` | `tls_decode_client_hello(body, len, out)` |
| `tests/TlsClientHelloDecodeTest.cpp` | 25 用例 |

CMake 零改动。

## 2. API 形状

```cpp
// 输入 = 4 字节握手头之后的 body（长度 == header.length），可来自
// TlsRecord::contiguous_payload() 或拷贝材料化；解码器零分配。
common::IoResult<void> tls_decode_client_hello(const std::uint8_t *body, std::size_t len,
                                               TlsClientHello &out) noexcept;
```

`TlsClientHello` 是纯 POD：顶层字段 + 每个提取的扩展一对 `has_*` / borrow span。
所有 span 借自 body 缓冲，零拷贝、零分配；`out` 仅在成功时改写（失败时原值不动，
契约与 QuicTransportParamsCodec 一致）。连续性由调用方保证——body 必须是单段
连续内存（从记录层拿到的多节点链需先材料化，符合 03 号文档“拆分管原始、
解码管拷贝”的分工）。

### 结构校验清单（解码器职责，全部返回 Invalid）

- random 恰 32 字节；session_id ≤ 32；cipher_suites ≥ 2 字节且偶数；compression ≥ 1 字节
- 扩展块（1.2 可选）必须精确走完 body，块外不得有尾随字节
- 扩展不得重复；单条 CH 扩展数上限 64（防去重表攻击面，超出按畸形拒绝）
- supported_versions / supported_groups / signature_algorithms[_cert]：长度前缀
  必须精确覆盖 payload，列表非空且偶数
- server_name：ServerNameList 精确走完，提取首个 host_name（未知 name_type 跳过）
- key_share：client_shares 精确走完，每条 key_exchange 非空
- alpn：ProtocolNameList 精确走完
- psk_key_exchange_modes：长度字节必须等于剩余字节数
- record_size_limit：payload 恰 2 字节
- early_data / encrypt_then_mac / extended_master_secret：强制空 payload
- pre_shared_key：必须是**最后一个**扩展；identities/binders 向量精确走完扩展；
  identity 数 == binder 数；binders 必须恰好收尾整条消息

### 语义检查（明确不在解码器，留给 server FSM）

版本协商（supported_versions 是否含 1.3）、套件/群组/签名算法交集、ALPN 选择、
PSK 会话查找与 binder 校验、SNI 路由。解码器只回答“这条消息结构合法吗”。

## 3. psk_binder_block_offset：转录截断锚点

PSK binder 校验需要把 ClientHello 在 binders 向量的**长度前缀处截断**后做
transcript hash（RFC 8446 §4.2.11.2）。解码器直接给出锚点：

```
psk_binder_block_offset = ext_data + 2 + identities.size()   （相对 body 起点）
消息内偏移 = psk_binder_block_offset + kTlsHandshakeHeaderSize
截断转录 = header || body[0 .. psk_binder_block_offset)
```

测试用确定性布局（无 session_id、单套件、单一 psk 扩展）验证锚点字节数值
恰为 binders 长度前缀（0x00,0x21 = 1+32）。

## 4. 已知边界

- 扩展去重表为固定 64 槽线性扫描——合法 CH 不会超过（各实现上限普遍 ≤ 20），
  攻击者超限即拒绝，无动态分配。
- `tls_find_client_key_share` 在完整 `key_share_entries` 上按需重扫（O(n) 群组
  数，n ≤ 支持的群组数），不做解码期索引——保持解码器单遍零分配。
- legacy_version 只透传不解释（1.3 下它必须仍是 0x0303，属语义层）。
- Cookie（HelloRetryRequest 用）、status_request 等 1.3 服务端侧扩展仅解析
  存在性或跳过，字段提取在用到时再加。

## 5. 测试覆盖（25 用例）

- FullParseExtractsEveryField / SpansBorrowTheBodyBuffer：全扩展 CH 逐字段 +
  span 指针同一性（random/session_id/cipher_suites/extensions_block/server_name
  均指回 body 缓冲内偏移）
- MinimalTls12HelloWithoutExtensions：1.2 最小 CH，无扩展块合法
- EveryTruncationIsRejected：对全量 body 的每个前缀长度断言失败，唯一例外是
  恰好截到“无扩展”边界（1.2 语义）
- 各畸形分支：session_id 33 字节、奇数/空 cipher_suites、空 compression、
  重复扩展、65 扩展、空 payload 扩展携带 payload、record_size_limit 1/3 字节、
  SNI 列表长度不匹配、supported_versions 长度错、u16 列表奇数、key_share 空
  key_exchange、ALPN 名字前缀越界、psk 模式长度错
- PSK 专项：非最后扩展拒绝、identity/binder 数不匹配、binders 不收尾消息、
  binder_block_offset 锚点
- 游标助手：TlsAlpnCursor 遍历、tls_find_client_key_share 命中/未命中
- OutIsUntouchedOnFailure：失败不改写 out
- 握手头解码：type/be24 length；截断拒绝

## 6. 下一步

1. ServerHello / EncryptedExtensions / Certificate / CertificateVerify / Finished
   编解码（客户端视角的解码 + 服务端的编码复用同一游标层）
2. 02 号：crypto 适配层 + TLS 1.3 密钥调度（HKDF 树、transcript hash）
3. 05/06 号：server / client FSM
