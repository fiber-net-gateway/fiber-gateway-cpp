# TLS 客户端证书身份

`TlsContext` 是一组不可变的证书身份与信任根，既可派生客户端连接，也可派生服务端连接。客户端使用带证书身份的 context 时，会在握手中发送证书链，可用于 mTLS。

## 文件和初始化语义

- `certificate_chain` 和 `private_key` 分别接受 PEM 文件路径或 PEM 内容；证书内容的第一个证书是 leaf，后面可依次包含 intermediate chain。
- 两者必须同时为空或同时设置；同时为空表示 context 没有本端证书身份。
- `trust_store` 接受根证书文件、根证书内容或显式的系统信任根。
- `TlsContext::create()` 同步加载材料并校验证书和私钥是否匹配。任何一步失败都返回错误，不会发布半初始化的 context。
- 初始化错误不会包含证书路径、私钥信息或 OpenSSL 错误队列详情。调用方也不应在普通日志或 API 响应中输出 secret reference 或解析后的文件路径。

客户端身份与服务端认证互相独立：

- `TlsClientConnectionOptions::verify_peer` 决定是否验证服务端证书；启用时 context 必须包含 trust store。
- `sni_name` 是发送给服务端的 SNI；IP 地址不会作为 SNI 发送。
- `verify_name` 是独立的证书校验名；为空时回退到 DNS 类型的 `sni_name`。
- ALPN、握手超时、取消和关闭仍沿用原有 TLS transport 行为。

一个典型配置如下：

```cpp
fiber::net::TlsOptions material{};
material.certificate_chain = fiber::net::TlsPemSource::from_file(resolved_client_chain_path);
material.private_key = fiber::net::TlsPemSource::from_file(resolved_client_key_path);
material.trust_store = fiber::net::TlsTrustStoreSource::from_file(resolved_ca_path);

auto context = fiber::net::TlsContext::create(material);
if (!context) {
    return std::unexpected(context.error());
}

fiber::net::TlsClientConnectionOptions tls{};
tls.context = context->get();
tls.verify_peer = true;
tls.sni_name = "route.example.com";
tls.verify_name = "certificate.example.com";
tls.alpn = {"h2", "http/1.1"};
```

证书文件只在 `create()` 时读入。初始化成功后不得通过 `raw()` 修改 `SSL_CTX`，并须保证 context 比所有由其创建的 TLS 连接活得更久。

## 连接池与轮换

TLS 连接建立后，客户端身份、信任与验证结果都属于该连接。不同身份不能共用 HTTP/1 keep-alive 连接，因此使用连接池时必须把有效 TLS profile 映射到 `Http1ConnectionPoolAffinity`：

```cpp
auto key = fiber::http::Http1ConnectionGroupKey::from_name(
        upstream_host,
        upstream_port,
        fiber::http::Http1ConnectionGroupKey::Scheme::Https,
        fiber::http::Http1ConnectionPoolAffinity{tls_profile_generation});

fiber::http::Http1ClientConnectionOptions connection_options;
connection_options.pool_affinity = key->pool_affinity();
// Fill peer_addr and tls from the same immutable profile before emplacing.
```

`0` 是 key 与 connection options 兼容旧调用方的默认 affinity。`Lease::emplace_connection()` 会拒绝两者 affinity 不一致的连接，但 affinity 与具体 TLS 内容的映射仍由配置控制层负责。存在多个身份时，应分配互不冲突的非零 generation；不要使用证书路径或私钥内容作为 affinity。轮换时先完整初始化新上下文，再原子发布新上下文和新 generation，并在旧连接全部退役前避免复用旧 generation。这样新请求不会命中旧身份的 idle 连接；旧连接可自然退役，也可以由应用显式清池。

普通路由内容、管理 API 和常规日志只应携带不透明的 secret reference 或 profile generation，不应携带私钥内容或解析后的私钥路径。
