# TLS 客户端证书身份

`TlsOptions::cert_file` 和 `TlsOptions::key_file` 同时适用于客户端与服务端 `TlsContext`。客户端配置这两个字段后，会在握手时发送证书身份，可用于 mTLS。

## 文件和初始化语义

- `cert_file` 使用 PEM 格式：第一个证书是 leaf，后面可依次包含 intermediate chain。
- `key_file` 是与 leaf 匹配的 PEM 私钥。
- 两个字段必须同时为空或同时设置；客户端同时为空保持原有匿名 TLS 行为。
- `TlsContext::init()` 会加载完整证书链、加载私钥并校验匹配关系。任何一步失败都返回 `IoErr::Invalid`，且不会通过 `raw()` 发布半初始化的 `SSL_CTX`。
- 初始化错误不会包含证书路径、私钥信息或 OpenSSL 错误队列详情。调用方也不应在普通日志或 API 响应中输出 secret reference 或解析后的文件路径。

客户端身份与服务端认证互相独立：

- `verify_peer` 决定是否验证服务端证书。
- `ca_file` 在 `verify_peer=true` 时提供客户端信任根；为空时使用系统信任根。
- `server_name` 是发送给服务端的 SNI。
- `verify_name` 是独立的证书校验名；为空时回退到 `server_name`。
- ALPN、握手超时、取消和关闭仍沿用原有 TLS transport 行为。

一个典型配置如下：

```cpp
fiber::net::TlsOptions tls{};
tls.cert_file = resolved_client_chain_path;
tls.key_file = resolved_client_key_path;
tls.verify_peer = true;
tls.ca_file = resolved_ca_path;
tls.server_name = "route.example.com";
tls.verify_name = "certificate.example.com";
tls.alpn = {"h2", "http/1.1"};

fiber::net::TlsContext context(std::move(tls), false);
auto result = context.init();
```

证书文件只在 `init()` 时读入。初始化成功后，应把 `TlsContext` 当作不可变对象，并保证它比所有由其创建的 TLS 连接活得更久。

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
