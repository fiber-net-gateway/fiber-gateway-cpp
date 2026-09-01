# Http2ClientConnection Usage

## Purpose

- `Http2ClientConnection` is a thin client-side connection owner.
- It creates and starts one client HTTP/2 connection on a single `EventLoop`.
- It owns one `TlsContext` and one `Http2Connection`.
- It creates non-owning `ClientHttp2Exchange` handles through `open_exchange(pool)`; the caller owns each exchange
  and its buffer pool.

## Scope

- Single thread only.
- One `EventLoop` owns the whole connection lifecycle.
- `connect(timeout)` establishes transport and starts the HTTP/2 session.
- `Http2Connection::start()` drives connection I/O; no separate `run()` coroutine is needed.
- Normal requests are opened through `open_exchange(pool)`.

## Required Options

- `Options::peer_addr` must be set.
- `Options::h2.role` is forced to `Client` internally.
- If `Options::tls.enabled == true`:
  - ALPN is forced to `h2`.
  - `TlsContext::init()` is run before handshake.

## Lifecycle

1. Construct `Http2ClientConnection` on the target loop.
2. `co_await connect(timeout)`.
3. Create an exchange with `open_exchange(pool)` and send its request.
4. Shutdown through `shutdown()` or `graceful_shutdown()`.

## Important Semantics

- `connect(timeout)` is a member function, not a static factory.
- `connect(timeout)` may only be called once.
- `timeout` only bounds the TCP connect phase; TLS uses `TlsOptions::handshake_timeout`.
- `std::chrono::milliseconds::max()` means an unlimited TCP connect wait.
- After `connect(timeout)` succeeds, `Http2Connection::start(transport)` has already run.
- For client role, initial preface and SETTINGS are already queued during `start()`.
- Local streams may therefore be created immediately after `connect(timeout)` succeeds.

## Local Stream Capacity

- `ClientHttp2Exchange::send_request_header()` lazily creates its request stream.
- If the peer's `SETTINGS_MAX_CONCURRENT_STREAMS` budget or the connection's fixed stream table is full, the
  coroutine suspends without blocking the `EventLoop` thread.
- Waiters are granted capacity in FIFO order. Only the number of available slots is resumed, so newly arriving
  requests cannot steal capacity already granted to queued requests.
- Closing an active local stream or receiving a larger `SETTINGS_MAX_CONCURRENT_STREAMS` value can resume waiters.
- Timeout, GOAWAY, graceful drain, shutdown, connection failure, and local stream-ID exhaustion end the wait.
  Timeout returns `TimedOut`; terminal connection states return `Canceled` or the connection failure.
- A waiter does not consume a stream ID until attachment succeeds.
- The timeout passed to `send_request_header()` is one total budget shared by stream-capacity waiting and header
  sending.
- Low-level callers that cannot suspend can use `try_attach_local_stream()`, which returns `Busy` while capacity is
  unavailable. The coroutine form is `attach_local_stream(stream, timeout)`.

## Transport Selection

- If `tls.context == nullptr`, `connect()` creates a `TcpTransport`.
- If `tls.context != nullptr`, `connect()` creates a `TlsTransport`, performs handshake, and verifies
  `negotiated_alpn() == "h2"`.

## Minimal Example

```cpp
fiber::http::Http2ClientConnection::Options options;
options.peer_addr = fiber::net::SocketAddress(peer_ip, 443);
options.tls.context = tls_context.get();
options.tls.sni_name = "example.com";

fiber::http::Http2ClientConnection conn(loop, std::move(options));

auto connect_result = co_await conn.connect(std::chrono::seconds(5));
if (!connect_result) {
    co_return std::unexpected(connect_result.error());
}

fiber::mem::BufPool pool;
auto exchange = conn.open_exchange(pool);
auto send_result = co_await exchange.send_request_header(
        {
                .method = fiber::http::HttpMethod::Get,
                .scheme = "https",
                .authority = "example.com",
                .path = "/",
        },
        true, std::chrono::seconds(5));
if (!send_result) {
    co_return std::unexpected(send_result.error());
}
```

## Non-Goals

- No connection cache.
- No thread-safe access.
- No automatic `ClientHttp2Exchange` creation.

## Current Boundary

- `Http2ClientConnection` solves connect/start/close ownership and creates exchange handles.
- Request send/receive behavior still belongs to `ClientHttp2Request` and higher layers.
