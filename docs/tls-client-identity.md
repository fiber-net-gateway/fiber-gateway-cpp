# TLS 凭据、信任根与握手参数

TLS 材料与连接策略相互独立：

- `TlsCredential` 是不可变的证书链和私钥，内部持有 BoringSSL `SSL_CREDENTIAL *`。
- `TrustStore` 是不可变的信任根集合，内部持有 `X509_STORE *`。
- `TlsClientParam` 和 `TlsServerParam` 只描述一次连接的握手策略，借用上述材料。

`TlsCredential::create()` 会同步读取 PEM、构造证书链并校验证书与私钥是否匹配；
`TrustStore::create()` 支持 PEM 文件、PEM 内容和系统信任根。失败时不会发布半初始化对象，普通日志和
API 响应也不应输出私钥内容、secret reference 或解析后的私钥路径。

## 客户端

没有客户端证书时，`credential` 保持为空。`verify_peer=true` 时必须提供 `trust_store`；`sni_name`
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
tls.enable_tls = true;
tls.credential = credential->get();
tls.trust_store = trust_store->get();
tls.verify_peer = true;
tls.sni_name = "route.example.com";
tls.verify_name = "certificate.example.com";
tls.alpn = {"h2", "http/1.1"};
```

## 服务端 ClientHello 配置回调

`TlsServerParam` 没有默认凭据字段。服务端必须设置同步 `configure_callback`；回调接收
`TlsClientHelloView` 和仅在回调期间有效的 `TlsServerHandshakeConfig`，并至少调用一次
`add_credential()`。它还可以添加多个候选凭据、替换 trust store、设置客户端证书模式、session ID
context、TLS 版本、early data，或指定本次连接的 ALPN。

静态单证书服务也走同一条路径：

```cpp
fiber::net::TlsServerParam tls{};
tls.configure_callback = &fiber::net::configure_tls_with_credential;
tls.configure_ctx = credential.get();
```

动态 SNI 配置示例：

```cpp
fiber::common::IoErr configure_tls(
        void *ctx,
        fiber::net::TlsServerHandshakeConfig &config,
        const fiber::net::TlsClientHelloView &hello) noexcept {
    auto &identities = *static_cast<IdentityTable *>(ctx);
    const fiber::net::TlsCredential *credential = identities.find(hello.server_name);
    if (!credential) {
        credential = identities.fallback();
    }
    return credential ? config.add_credential(*credential)
                      : fiber::common::IoErr::NotFound;
}
```

内部只创建一份进程级 client `SSL_CTX` 和一份 server `SSL_CTX`。server context 通过
`SSL_CTX_set_select_certificate_cb` 安装固定 trampoline；每条连接的回调状态放在 `SSL` ex-data 中，
trampoline 再从 `client_hello->ssl` 取回。因此业务回调修改的是当前连接的 `SSL`，不会切换或修改共享
`SSL_CTX`，也不会暴露原始 `SSL *`。

回调当前只支持同步成功或失败，不返回 `ssl_select_cert_retry`。`TlsServerParam` 必须活到握手任务结束；
callback state 及其选择的 `TlsCredential`、`TrustStore` 必须活到同步回调完成。成功调用
`SSL_add1_credential()` 或 `SSL_set1_verify_cert_store()` 后，当前 `SSL` 会持有自己的引用，材料对象无需继续
等待握手结束。

## 连接池与轮换

客户端身份、信任根、peer verification、SNI、`verify_name` 和 ALPN 都属于有效 TLS profile。
不同 profile 不得共用 HTTP/1 keep-alive 连接，应映射到不同的
`Http1ConnectionPoolAffinity`。轮换时先完整创建新 `TlsCredential`/`TrustStore`，再与新的 profile
generation 一起发布；旧连接退役前不要复用旧 generation。不要从证书路径、私钥或 secret 内容推导
affinity。
