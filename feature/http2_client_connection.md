# Http2ClientConnection Usage

## Purpose
- `Http2ClientConnection` is a thin client-side connection owner.
- It creates and starts one client HTTP/2 connection on a single `EventLoop`.
- It owns:
  - one `TlsContext`
  - one `Http2Connection`
- It does not create `ClientHttp2Exchange` and does not manage request pooling.

## Scope
- Single thread only.
- One `EventLoop` owns the whole connection lifecycle.
- `connect(timeout)` establishes transport and starts the HTTP/2 session.
- `run()` drives the read loop.
- Request stream creation is done through `http2()`.

## Required Options
- `Options::peer_addr` must be set.
- `Options::h2.outbound_hpack_catalog` must be set.
- `Options::h2.role` is forced to `Client` internally.
- If `Options::tls.enabled == true`:
  - ALPN is forced to `h2`
  - `TlsContext::init()` is run before handshake

## Lifecycle
1. Construct `Http2ClientConnection` on the target loop.
2. `co_await connect(timeout)`.
3. Start `run()` on the same loop.
4. Use `http2()` to create local streams.
5. Shutdown through `shutdown()` or `graceful_shutdown()`.

## Important Semantics
- `connect(timeout)` is a member function, not a static factory.
- `connect(timeout)` may only be called once.
- `timeout` only bounds the TCP connect phase; TLS uses `TlsOptions::handshake_timeout`.
- `std::chrono::milliseconds::max()` means an unlimited TCP connect wait.
- After `connect(timeout)` succeeds, `Http2Connection::start(transport)` has already run.
- For client role, initial preface + SETTINGS are already queued during `start()`.
- Because of that, local streams may be created after `connect(timeout)` succeeds, even before `run()` begins polling reads.

## Transport Selection
- If `tls.enabled == false`, `connect()` creates a `TcpTransport`.
- If `tls.enabled == true`, `connect()` creates a `TlsTransport`, performs handshake, and verifies `negotiated_alpn() == "h2"`.

## Minimal Example
```cpp
fiber::http::Http2ClientConnection::Options options;
options.peer_addr = fiber::net::SocketAddress(peer_ip, 443);
options.tls.enabled = true;
options.tls.server_name = "example.com";
options.h2.outbound_hpack_catalog = &catalog;

fiber::http::Http2ClientConnection conn(loop, std::move(options));

auto connect_result = co_await conn.connect(std::chrono::seconds(5));
if (!connect_result) {
    co_return std::unexpected(connect_result.error());
}

fiber::async::spawn(loop, [&conn]() -> fiber::async::Task<void> {
    (void)co_await conn.run();
});

fiber::http::Http2Stream *stream = conn.http2().create_local_stream(1);
if (!stream) {
    co_return std::unexpected(fiber::common::IoErr::Busy);
}
```

## Non-Goals
- No connection cache.
- No thread-safe access.
- No request API on `Http2ClientConnection` itself.
- No automatic `ClientHttp2Exchange` creation.

## Current Boundary
- `Http2ClientConnection` only solves connect/start/run ownership.
- Request send/receive behavior still belongs to `ClientHttp2Request` and higher layers.
