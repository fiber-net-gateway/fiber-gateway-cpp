# TLS 凭据、信任根与握手参数

TLS 材料与连接策略相互独立：

- `TlsCredential` 是不可变证书链和私钥的引用计数句柄：整份材料只有一个原子引用计数（与 BoringSSL
  `SSL_CREDENTIAL` 相同），拷贝句柄只做一次计数加一，移动不改计数；材料本身不随句柄移动，握手借用的指针
  始终有效。
- `TrustStore` 是不可变的信任根集合，内部持有 `X509_STORE *`。
- `TlsClientSecurity` 组合一次客户端连接使用的身份、信任根和 peer verification 策略。
- `TlsClientParam` 只描述创建一个原始 client SSL 时实际应用的安全、版本、SNI、校验名和 ALPN。
- HTTP 和 QUIC 使用各自的连接参数管理 TLS 开关、超时、协议 ALPN、session cache 和 0-RTT。

`TlsCredential::create()` 会同步读取 PEM、构造证书链并校验证书与私钥是否匹配，成功时返回
`IoResult<TlsCredential>` 句柄；
`TrustStore::create()` 支持 PEM 文件、PEM 内容和系统信任根。失败时不会发布半初始化对象，普通日志和
API 响应也不应输出私钥内容、secret reference 或解析后的私钥路径。

## 客户端

没有客户端证书时，`credential` 保持为空。`verify_peer=true` 时必须提供 `trust_store`；`server_name`
用于 ClientHello SNI，IP 字面量不会作为 SNI 发送，`verify_name` 则独立控制证书 DNS/IP SAN 校验目标。

```cpp
fiber::net::TlsCredentialOptions credential_options{};
credential_options.certificate_chain =
        fiber::net::TlsPemSource::from_file(resolved_client_chain_path);
credential_options.private_key =
        fiber::net::TlsPemSource::from_file(resolved_client_key_path);
auto credential = fiber::net::TlsCredential::create(credential_options);

auto trust_store = fiber::net::TrustStore::create(
        fiber::net::TrustStoreOptions::from_file(resolved_ca_path));
if (!credential || !trust_store) {
    return std::unexpected(credential ? trust_store.error() : credential.error());
}

fiber::net::TlsClientParam tls{};
tls.security.credential = &*credential;
tls.security.trust_store = trust_store->get();
tls.security.verify_peer = true;
tls.server_name = "route.example.com";
tls.verify_name = "certificate.example.com";
tls.alpn = {"h2", "http/1.1"};
```

客户端 `credential` 是借用指针：句柄（或同一材料的其它句柄）必须活到握手结束；指向空句柄会在 staging 时
返回 `Invalid`。

`TlsClientParam` 不包含 enable bit 或 handshake timeout：调用 `TlsTcpStream::handshake()` 本身就表示启用
TLS，超时由该操作的参数控制。它也不提供 session 保存 callback；TCP 当前没有配套的 session 恢复 API。
QUIC resumption/0-RTT 使用 `QuicClientCacheOps`，不经过通用 TLS 参数。

## 服务端 ClientHello 配置回调

`TlsServerParam` 没有默认凭据字段。服务端必须设置同步 `configure_callback`；回调接收
`TlsClientHelloView` 和仅在回调期间有效的 `TlsServerHandshakeConfig`，并至少添加一份凭据：

- `add_credential(TlsCredential)`：握手持有自己的引用，直到该握手结束（成功、失败、取消或 engine 销毁；
  QUIC 在 done-transition 释放）。调用方可以立即丢弃自己的句柄，适合可能在握手途中被替换的动态凭据。
  传左值会拷贝（一次原子加一），传右值则直接移交引用。
- `add_borrowed_credential(const TlsCredential &)`：只借用，不碰引用计数；调用方必须保证该材料活到握手
  结束（例如与 server options 同生命周期的静态凭据）。

同一回调里后添加的凭据会替换并释放先前的凭据。它还可以添加多个候选凭据、替换 trust store、设置客户端证书模式、session ID
context、TLS 版本、early data，或指定本次连接的 ALPN。

静态单证书服务也走同一条路径，`configure_tls_with_credential` 以借用方式添加 `configure_ctx` 指向的句柄，
因此该句柄必须位于稳定地址并活到握手结束：

```cpp
fiber::net::TlsServerParam tls{};
tls.configure_callback = &fiber::net::configure_tls_with_credential;
tls.configure_ctx = &*credential;
```

动态 SNI 配置示例：

```cpp
fiber::common::IoErr configure_tls(
        void *ctx,
        fiber::net::TlsServerHandshakeConfig &config,
        const fiber::net::TlsClientHelloView &hello) noexcept {
    auto &identities = *static_cast<IdentityTable *>(ctx);
    // 拷贝出当前发布的句柄：表随后被替换也不影响本次握手。
    fiber::net::TlsCredential credential = identities.find(hello.server_name);
    if (credential.empty()) {
        credential = identities.fallback();
    }
    return credential.empty() ? fiber::common::IoErr::NotFound
                              : config.add_credential(std::move(credential));
}
```

内部只创建一份进程级 client `SSL_CTX` 和一份 server `SSL_CTX`。server context 通过
`SSL_CTX_set_select_certificate_cb` 安装固定 trampoline；每条连接的回调状态放在 `SSL` ex-data 中，
trampoline 再从 `client_hello->ssl` 取回。因此业务回调修改的是当前连接的 `SSL`，不会切换或修改共享
`SSL_CTX`，也不会暴露原始 `SSL *`。

回调当前只支持同步成功或失败。`TlsServerParam` 必须活到握手任务结束；engine 在回调返回后仍会读取凭据
（Certificate/CertificateVerify，HelloRetryRequest 时要再晚一个往返），所以 `add_borrowed_credential`
借用的凭据和 `set_trust_store` 借用的 `TrustStore` 都必须活到握手结束；经 `add_credential` 添加的凭据由
握手自己持有。

## 连接池与轮换

客户端身份、信任根、peer verification、SNI、`verify_name` 和 ALPN 都属于有效 TLS profile。
不同 profile 不得共用 HTTP/1 keep-alive 连接。`HttpConnectionGroupKey` 只按 host、端口、scheme
和拨号地址分组，SNI 与 `verify_name` 直接来自 `host()`；连接池不区分其它 profile 维度，需要按
profile 隔离的调用方必须自行保证不同 profile 不会命中同一个 key。轮换时先完整创建新
`TlsCredential`/`TrustStore`，再切换到新 profile；旧连接退役前不要销毁它们借用的材料。
